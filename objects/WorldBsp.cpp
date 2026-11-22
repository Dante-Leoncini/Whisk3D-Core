// ============================================================================
//  WorldBsp.cpp — ver WorldBsp.h. El mundo de un mapa BSP dibujado por el motor:
//  PVS + frustum jerarquico por el arbol, caras juntadas por material, un draw
//  por material desde VBOs. C++03 / Symbian-safe.
// ============================================================================
#include "objects/WorldBsp.h"
#include "objects/VisBsp.h"
#include "w3dGraphics.h"
#include "w3dlog.h"       // que paso con cada grupo (el N95 no tiene consola)
#include <string.h>
#include <stdlib.h>   // getenv: W3D_BSPWORLD_RAM=1 fuerza el camino sin VBO (probarlo en PC)
#include <algorithm>
#include <math.h>

namespace gfx = w3dEngine;

// ---- lectura LE sin alineacion (como VisBsp.cpp) ----
static unsigned LeeU32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned LeeU16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static float LeeF32(const unsigned char* p) { unsigned u = LeeU32(p); float f; memcpy(&f, &u, 4); return f; }

WorldBsp::WorldBsp() : cutProp(0.0f), cutTrace(0.0f), rangeScale(1.0f), timeAnim(0.0f), animPlaced(false), markedCluster(-2), stamp(0) { cutEye[0] = cutEye[1] = cutEye[2] = 0.0f; }

void WorldBsp::CutSize(const float* eye, float ratio) {
    cutEye[0] = eye[0]; cutEye[1] = eye[1]; cutEye[2] = eye[2];
    cutProp = ratio > 0.0f ? ratio : 0.0f;
}

void WorldBsp::CutDecals(float ratio) { cutTrace = ratio > 0.0f ? ratio : 0.0f; }

void WorldBsp::RangeScale(float scale) { rangeScale = scale > 0.01f ? scale : 0.01f; }
WorldBsp::~WorldBsp() { Clear(); }

void WorldBsp::Clear() {
    for (size_t i = 0; i < groups.size(); i++) {
        Group& g = groups[i];
        if (g.vboPos) gfx::DeleteBuffer(g.vboPos);
        if (g.vboUV)  gfx::DeleteBuffer(g.vboUV);
        if (g.vboLm)  gfx::DeleteBuffer(g.vboLm);
        if (g.vboCol) gfx::DeleteBuffer(g.vboCol);
    }
    materials.clear(); groups.clear(); faces.clear(); leafFaceOff.clear(); leafFaces.clear();
    boxNode.clear(); boxLeaf.clear(); boxFace.clear(); faceRange.clear(); rangeCenter.clear();
    anims.clear(); animOf.clear();
    leafVis.clear(); nodeVis.clear(); stampFace.clear(); cubes.clear(); touchedCube.clear(); touched.clear();
    markedCluster = -2; stamp = 0;
    last = Stats();
}

size_t WorldBsp::Bytes() const {
    size_t b = faces.size() * sizeof(Face) + (leafFaceOff.size() + leafFaces.size()) * 4 + (boxNode.size() + boxLeaf.size() + boxFace.size()) * 4;
    for (size_t i = 0; i < groups.size(); i++) {
        const Group& g = groups[i];
        b += (g.pos.size() + g.uv.size() + g.lm.size()) * 4 + g.col.size() + g.idx.size() * 2;
    }
    return b;
}

// la seccion de geometria del .w3dbsp v2 (formato/w3dbsp.md):
//   "GEOM"
//   u32 nMateriales x { str textura; str lightmap; u32 flags; f32 alphaRef }        (str = u32 largo + utf-8)
//   u32 nGroups x { u32 nVerts; u8 withLm; pos f32 x3; uv f32 x2; [lm f32 x2]; col u8 x4; u32 nIdx; u16 x nIdx }
//   u32 nFaces x { u16 grupo; u16 material; u32 primero; u32 cantidad }
//   u32 nLeaves; (nLeaves + 1) x u32 offsets; u32 nHojaCaras; u32 x nHojaCaras
//   cajas: nNodes x 6 f32; nLeaves x 6 f32; [u8 1; nFaces x 6 f32]  (las de las caras: opcionales)
//   (opcional, al final, solo con las cajas de las caras) "MANI" u32 n x { u16 material; u16 0; Anim }: las animaciones
//   de los materiales MatAnimated. Un Anim: u16 nFrames; u16 layerMode; f32 scroll[4]; f32 color[3]; f32 brillo, pulso,
//   periodo, ruido; f32 fps; f32 distFrames[2]; f32 distTurnOff[3]; nFrames x str (las texturas de los cuadros)
bool WorldBsp::Load(const unsigned char* d, size_t n, size_t p, const VisBsp& bsp, std::string* err, bool uploadOnLoad) {
    Clear();
    const size_t nNodes = bsp.nodes.size(), nLeaves = bsp.leafCluster.size();
    if (p + 8 > n || memcmp(d + p, "GEOM", 4) != 0) { if (err) *err = "sin seccion de geometria"; return false; }
    p += 4;
    const unsigned nMat = LeeU32(d + p); p += 4;
    if (nMat > 65535u) { if (err) *err = "cantidad de materiales absurda"; return false; }
    materials.resize(nMat);
    for (unsigned i = 0; i < nMat; i++) {
        Material& m = materials[i];
        for (int k = 0; k < 2; k++) {
            if (p + 4 > n) { if (err) *err = "cortado en los materiales"; Clear(); return false; }
            const unsigned length = LeeU32(d + p); p += 4;
            if (length > 4096u || p + length > n) { if (err) *err = "cortado en los materiales"; Clear(); return false; }
            (k == 0 ? m.texture : m.lightmap).assign((const char*)d + p, length); p += length;
        }
        if (p + 8 > n) { if (err) *err = "cortado en los materiales"; Clear(); return false; }
        m.flags = LeeU32(d + p); m.alphaRef = LeeF32(d + p + 4); p += 8;
    }
    if (p + 4 > n) { if (err) *err = "cortado en los grupos"; Clear(); return false; }
    const unsigned nGroups = LeeU32(d + p); p += 4;
    if (nGroups > 65535u) { if (err) *err = "cantidad de grupos absurda"; Clear(); return false; }
    groups.resize(nGroups);
    int inVbo = 0, inRam = 0;
    for (unsigned gi = 0; gi < nGroups; gi++) {
        Group& g = groups[gi];
        if (p + 5 > n) { if (err) *err = "cortado en los grupos"; Clear(); return false; }
        g.nVerts = LeeU32(d + p); p += 4;
        const bool withLm = d[p] != 0; p += 1;
        if (g.nVerts == 0 || g.nVerts > 65536u) { if (err) *err = "grupo con mas de 65536 vertices"; Clear(); return false; }
        const size_t bytesV = (size_t)g.nVerts * (12 + 8 + (withLm ? 8 : 0) + 4);
        if (p + bytesV + 4 > n) { if (err) *err = "cortado en los vertices"; Clear(); return false; }
        g.pos.resize((size_t)g.nVerts * 3); g.uv.resize((size_t)g.nVerts * 2); g.col.resize((size_t)g.nVerts * 4);
        if (withLm) g.lm.resize((size_t)g.nVerts * 2);
        for (size_t k = 0; k < g.pos.size(); k++, p += 4) g.pos[k] = LeeF32(d + p);
        for (size_t k = 0; k < g.uv.size(); k++, p += 4)  g.uv[k]  = LeeF32(d + p);
        for (size_t k = 0; k < g.lm.size(); k++, p += 4)  g.lm[k]  = LeeF32(d + p);
        memcpy(&g.col[0], d + p, g.col.size()); p += g.col.size();
        const unsigned nIdx = LeeU32(d + p); p += 4;
        if (nIdx > 50000000u || p + (size_t)nIdx * 2 > n) { if (err) *err = "cortado en los indices"; Clear(); return false; }
        g.idx.resize(nIdx);
        for (unsigned k = 0; k < nIdx; k++, p += 2) {
            const unsigned v = LeeU16(d + p);
            if (v >= g.nVerts) { if (err) *err = "indice fuera del grupo"; Clear(); return false; }
            g.idx[k] = (unsigned short)v;
        }
        // a la GPU ya (el contexto esta: se carga desde el render): la RAM del grupo se suelta antes de parsear el que
        // sigue, asi el pico de memoria es el archivo + UN grupo, no el archivo + todo el mundo
        if (uploadOnLoad) { if (UploadGroup(g, (int)gi)) inVbo++; else inRam++; }
    }
    if (uploadOnLoad) w3dLogf("[MundoBsp] %u grupos: %d en VBO, %d en RAM", nGroups, inVbo, inRam);
    if (p + 4 > n) { if (err) *err = "cortado en las caras"; Clear(); return false; }
    const unsigned nFaces = LeeU32(d + p); p += 4;
    if (nFaces > 50000000u || p + (size_t)nFaces * 12 > n) { if (err) *err = "cortado en las caras"; Clear(); return false; }
    faces.resize(nFaces);
    for (unsigned i = 0; i < nFaces; i++, p += 12) {
        Face& c = faces[i];
        c.group = (unsigned short)LeeU16(d + p); c.material = (unsigned short)LeeU16(d + p + 2);
        c.first = LeeU32(d + p + 4); c.count = LeeU32(d + p + 8);
        if (c.group >= nGroups || c.material >= nMat || c.count % 3 != 0 ||
            (size_t)c.first + c.count > groups[c.group].idx.size()) {
            if (err) *err = "cara fuera de rango"; Clear(); return false;
        }
    }
    if (p + 4 > n) { if (err) *err = "cortado en las hojas"; Clear(); return false; }
    const unsigned nH = LeeU32(d + p); p += 4;
    if (nH != nLeaves) { if (err) *err = "la geometria no tiene las hojas del arbol"; Clear(); return false; }
    if (p + (size_t)(nH + 1) * 4 + 4 > n) { if (err) *err = "cortado en las hojas"; Clear(); return false; }
    leafFaceOff.resize(nH + 1);
    for (unsigned i = 0; i <= nH; i++, p += 4) leafFaceOff[i] = LeeU32(d + p);
    const unsigned nHC = LeeU32(d + p); p += 4;
    if (nHC > 50000000u || p + (size_t)nHC * 4 > n || leafFaceOff[nH] != nHC) { if (err) *err = "cortado en las caras por hoja"; Clear(); return false; }
    leafFaces.resize(nHC);
    for (unsigned i = 0; i < nHC; i++, p += 4) {
        leafFaces[i] = LeeU32(d + p);
        if (leafFaces[i] >= nFaces) { if (err) *err = "cara de hoja fuera de rango"; Clear(); return false; }
    }
    for (unsigned i = 0; i < nH; i++)
        if (leafFaceOff[i] > leafFaceOff[i + 1]) { if (err) *err = "offsets de hojas desordenados"; Clear(); return false; }
    const size_t nBox = (nNodes + nLeaves) * 6;
    if (p + nBox * 4 > n) { if (err) *err = "cortado en las cajas"; Clear(); return false; }
    boxNode.resize(nNodes * 6); boxLeaf.resize(nLeaves * 6);
    for (size_t k = 0; k < boxNode.size(); k++, p += 4) boxNode[k] = LeeF32(d + p);
    for (size_t k = 0; k < boxLeaf.size(); k++, p += 4) boxLeaf[k] = LeeF32(d + p);
    // opcional: la caja de cada cara (un byte de presencia + nFaces x 6 f32)
    if (p < n && d[p] != 0) {
        p += 1;
        if (p + (size_t)nFaces * 24 > n) { if (err) *err = "cortado en las cajas de las caras"; Clear(); return false; }
        boxFace.resize((size_t)nFaces * 6);
        for (size_t k = 0; k < boxFace.size(); k++, p += 4) boxFace[k] = LeeF32(d + p);
        // opcional: el rango de distancia de cada cara (un byte 2 + nFaces x 2 f32): los niveles de detalle
        if (p < n && d[p] == 2) {
            p += 1;
            if (p + (size_t)nFaces * 8 > n) { if (err) *err = "cortado en los rangos de las caras"; Clear(); return false; }
            faceRange.resize((size_t)nFaces * 2);
            for (size_t k = 0; k < faceRange.size(); k++, p += 4) faceRange[k] = LeeF32(d + p);
            // opcional: el punto desde el que se mide el rango de cada cara (un byte 3 + nFaces x 3 f32)
            if (p < n && d[p] == 3) {
                p += 1;
                if (p + (size_t)nFaces * 12 > n) { if (err) *err = "cortado en los centros de los rangos"; Clear(); return false; }
                rangeCenter.resize((size_t)nFaces * 3);
                for (size_t k = 0; k < rangeCenter.size(); k++, p += 4) rangeCenter[k] = LeeF32(d + p);
            }
        }
    }
    // opcional: las animaciones de los materiales
    animOf.assign(nMat, -1);
    if (p + 8 <= n && memcmp(d + p, "MANI", 4) == 0) {
        p += 4;
        const unsigned nA = LeeU32(d + p); p += 4;
        if (nA > nMat) { if (err) *err = "mas animaciones que materiales"; Clear(); return false; }
        anims.resize(nA);
        for (unsigned i = 0; i < nA; i++) {
            if (p + 4 > n) { if (err) *err = "cortado en las animaciones"; Clear(); return false; }
            const unsigned mi = LeeU16(d + p); p += 4;
            p = ReadAnim(d, n, p, anims[i]);
            if (!p || mi >= nMat) { if (err) *err = "animacion cortada o de un material que no esta"; Clear(); return false; }
            animOf[mi] = (int)i;
        }
    }
    stampFace.assign(nFaces, 0);
    cubes.resize((size_t)nGroups * nMat);
    touchedCube.assign(cubes.size(), 0);
    return true;
}

// ---- las hojas visibles desde el cluster y los nodos que tienen alguna (una vez por cambio de cluster) ----
void WorldBsp::Mark(const VisBsp& bsp, int cluster, const std::vector<unsigned char>& row) {
    const size_t nH = bsp.leafCluster.size(), nN = bsp.nodes.size();
    leafVis.assign(nH, 1);
    nodeVis.assign(nN, 1);
    markedCluster = cluster;
    if (cluster < 0 || row.size() < (size_t)bsp.bytesRow) return;   // sin PVS: todo pasa
    for (size_t h = 0; h < nH; h++) {
        const int c = bsp.leafCluster[h];
        leafVis[h] = (c >= 0 && c < bsp.nClusters && (row[(size_t)c >> 3] & (1u << (c & 7)))) ? 1 : 0;
    }
    // los nodos, de abajo hacia arriba: post-orden con pila explicita (el arbol puede ser hondo; la pila de Symbian no)
    if (nN == 0) return;
    nodeVis.assign(nN, 0);
    std::vector<unsigned char> done(nN, 0);
    stack.clear(); stack.push_back(0);
    while (!stack.empty()) {
        const int i = stack.back();
        const VisBsp::Node& nd = bsp.nodes[(size_t)i];
        if (!done[(size_t)i]) {
            done[(size_t)i] = 1;   // (primera visita: bajar a los hijos que son nodos)
            if (nd.child0 >= 0) stack.push_back(nd.child0);
            if (nd.child1 >= 0) stack.push_back(nd.child1);
            continue;
        }
        stack.pop_back();
        const bool v0 = nd.child0 >= 0 ? nodeVis[(size_t)nd.child0] != 0 : leafVis[(size_t)(-nd.child0 - 1)] != 0;
        const bool v1 = nd.child1 >= 0 ? nodeVis[(size_t)nd.child1] != 0 : leafVis[(size_t)(-nd.child1 - 1)] != 0;
        nodeVis[(size_t)i] = (v0 || v1) ? 1 : 0;
    }
}

static bool BoxInPlanes(const float* p, const float* box) {
    for (int i = 0; i < 6; i++) {
        const float a = p[i * 4], b = p[i * 4 + 1], c = p[i * 4 + 2], d = p[i * 4 + 3];
        const float px = (a >= 0.0f) ? box[3] : box[0];
        const float py = (b >= 0.0f) ? box[4] : box[1];
        const float pz = (c >= 0.0f) ? box[5] : box[2];
        if (a * px + b * py + c * pz + d < 0.0f) return false;
    }
    return true;
}

// sube los atributos de un grupo a VBOs y, si el driver los acepto (un glBufferData que no entra -el MBX del N95- falla
// en silencio: se pregunta ErrorGL despues), suelta su RAM. Si no entro, se borran los buffers y el grupo se dibuja
// desde RAM (mas lento, pero se ve). Devuelve si quedo en VBO
bool WorldBsp::UploadGroup(Group& g, int index) {
    g.uploaded = true;
    static const bool forceRam = getenv("W3D_BSPWORLD_RAM") != NULL && getenv("W3D_BSPWORLD_RAM")[0] != 0;
    if (!gfx::VBOSupported() || forceRam) { w3dLogfW("[MundoBsp] grupo %d: %s -> RAM", index, forceRam ? "W3D_BSPWORLD_RAM" : "sin VBO en este driver"); return false; }
    gfx::ErrorGL();   // (vaciar lo que hubiera)
    g.vboPos = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboPos, &g.pos[0], (int)(g.pos.size() * sizeof(float)));
    g.vboUV  = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboUV,  &g.uv[0],  (int)(g.uv.size() * sizeof(float)));
    g.vboCol = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboCol, &g.col[0], (int)g.col.size());
    if (!g.lm.empty()) { g.vboLm = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboLm, &g.lm[0], (int)(g.lm.size() * sizeof(float))); }
    const unsigned err = gfx::ErrorGL();
    const bool ok = err == 0 && g.vboPos && g.vboUV && g.vboCol && (g.lm.empty() || g.vboLm);
    if (!ok) {
        w3dLogfW("[MundoBsp] grupo %d (%u vertices, %u KB): el driver no lo acepto (glError 0x%x) -> se dibuja desde RAM",
                 index, g.nVerts, (unsigned)((g.pos.size() + g.uv.size() + g.lm.size()) * 4 + g.col.size()) / 1024u, err);
        if (g.vboPos) gfx::DeleteBuffer(g.vboPos);
        if (g.vboUV)  gfx::DeleteBuffer(g.vboUV);
        if (g.vboCol) gfx::DeleteBuffer(g.vboCol);
        if (g.vboLm)  gfx::DeleteBuffer(g.vboLm);
        g.vboPos = g.vboUV = g.vboCol = g.vboLm = 0;
        return false;
    }
    // la RAM de los atributos ya no hace falta (el telefono tiene poca): quedan en la GPU. Los indices si (viajan)
    std::vector<float>().swap(g.pos); std::vector<float>().swap(g.uv);
    if (g.vboLm) std::vector<float>().swap(g.lm);
    std::vector<unsigned char>().swap(g.col);
    return true;
}

void WorldBsp::PointersGroup(Group& g) {
    if (!g.uploaded) UploadGroup(g, (int)(&g - &groups[0]));
    if (g.vboPos) { gfx::VertexVBO(g.vboPos); gfx::TexCoordVBO(g.vboUV); gfx::ColorVBO(g.vboCol); }
    else {
        gfx::UnbindVBOs();
        gfx::VertexPointer3f(0, &g.pos[0]); gfx::TexCoordPointer2f(0, &g.uv[0]); gfx::ColorPointer4ub(&g.col[0]);
    }
}

WorldBsp::Anim::Anim() : layerMode(5), brightness(1.0f), pulse(0.0f), period(1.0f), noise(0.0f), fps(0.0f) {
    scroll[0] = scroll[1] = scroll[2] = scroll[3] = 0.0f;
    color[0] = color[1] = color[2] = 1.0f;
    distFrames[0] = distFrames[1] = 0.0f;
    distTurnOff[0] = distTurnOff[1] = 0.0f; distTurnOff[2] = 1.0f;
}

size_t WorldBsp::ReadAnim(const unsigned char* d, size_t n, size_t p, Anim& a) {
    if (p + 72 > n) return 0;
    const unsigned nc = LeeU16(d + p);
    a.layerMode = (int)LeeU16(d + p + 2); p += 4;
    float f[17];
    for (int k = 0; k < 17; k++, p += 4) f[k] = LeeF32(d + p);
    for (int k = 0; k < 4; k++) a.scroll[k] = f[k];
    for (int k = 0; k < 3; k++) a.color[k] = f[4 + k];
    a.brightness = f[7]; a.pulse = f[8]; a.period = f[9] > 0.001f ? f[9] : 1.0f; a.noise = f[10]; a.fps = f[11];
    a.distFrames[0] = f[12]; a.distFrames[1] = f[13];
    a.distTurnOff[0] = f[14]; a.distTurnOff[1] = f[15]; a.distTurnOff[2] = f[16];
    if (nc > 256) return 0;
    a.frames.resize(nc);
    a.idFrames.assign(nc, 0u);
    for (unsigned k = 0; k < nc; k++) {
        if (p + 4 > n) return 0;
        const unsigned length = LeeU32(d + p); p += 4;
        if (length > 4096u || p + length > n) return 0;
        a.frames[k].assign((const char*)d + p, length); p += length;
    }
    return p;
}

unsigned WorldBsp::FrameAnim(const Anim& a, float t, float distance, unsigned idTex) {
    const int nc = (int)a.idFrames.size();
    if (nc == 0) return idTex;
    int k = 0;
    if (a.fps > 0.0f) k = (int)(t * a.fps) % nc;
    else if (distance >= 0.0f && a.distFrames[1] > a.distFrames[0]) {
        const float f = (distance - a.distFrames[0]) / (a.distFrames[1] - a.distFrames[0]);
        k = (int)(f * (float)(nc - 1) + 0.5f);
    }
    if (k < 0) k = 0;
    if (k >= nc) k = nc - 1;
    return a.idFrames[(size_t)k] ? a.idFrames[(size_t)k] : idTex;
}

// (el corrimiento de las UV dentro de una vuelta: con el reloj grande el float pierde los decimales)
static float Turn(float x) { return x - floorf(x); }

void WorldBsp::ApplyAnim(const Anim& a, float t, float distance, unsigned idLayer, unsigned seed, bool mixed) {
    gfx::TexMatrixUV(Turn(a.scroll[0] * t), Turn(a.scroll[1] * t), 1.0f, 1.0f);
    if (idLayer) {
        gfx::MixTextures(idLayer, 1.0f, true, true, a.layerMode);
        gfx::MixTexturesUV(0);
        gfx::MixTexturesMatrixUV(Turn(a.scroll[2] * t), Turn(a.scroll[3] * t), 1.0f, 1.0f);
    } else gfx::MixTexturesOff();
    // el parpadeo: un azar nuevo 15 veces por segundo (un hash del tramo, distinto por material)
    unsigned h = (unsigned)(t * 15.0f) * 2654435761u + seed * 40503u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    const float random = (float)(h & 0xffffu) / 65535.0f;
    float k = a.brightness + a.pulse * sinf(t * 6.2831853f / a.period) - a.noise * random;
    if (distance >= 0.0f && a.distTurnOff[1] > a.distTurnOff[0]) {
        float f = (distance - a.distTurnOff[0]) / (a.distTurnOff[1] - a.distTurnOff[0]);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        k *= 1.0f + (a.distTurnOff[2] - 1.0f) * f;
    }
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    gfx::DisableArray(gfx::ColorArray);
    if (mixed) gfx::Color4f(a.color[0], a.color[1], a.color[2], k);
    else gfx::Color4f(a.color[0] * k, a.color[1] * k, a.color[2] * k, 1.0f);
}

void WorldBsp::RemoveAnim() {
    gfx::TexMatrixUV(0.0f, 0.0f, 1.0f, 1.0f);
    gfx::MixTexturesMatrixUV(0.0f, 0.0f, 1.0f, 1.0f);
    gfx::EnableArray(gfx::ColorArray);
    gfx::Color4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void WorldBsp::MaterialState(const Material& m, const Group& g, unsigned index) {
    const Anim* an = ((m.flags & MatAnimated) && index < animOf.size() && animOf[index] >= 0) ? &anims[(size_t)animOf[index]] : 0;
    if (!an && animPlaced) { RemoveAnim(); animPlaced = false; }
    const unsigned idTex = an ? FrameAnim(*an, timeAnim, -1.0f, m.idTex) : m.idTex;
    if (idTex && !(m.flags & MatNoTexture)) {
        gfx::Enable(gfx::Texture2D);
        gfx::BindTexture(idTex);
        gfx::TexFilter(!(m.flags & MatNoFilter));
        gfx::TexWrap(!(m.flags & MatNoRepeat));
    } else gfx::Disable(gfx::Texture2D);
    // el lightmap: la unidad 1 con la pagina en Multiply 2x y SUS UV (las del grupo)
    // (MatLayerSum: la capa se SUMA con las mismas UV de la textura base: el grupo no necesita UV propias)
    // (MatAnimated: la 2da capa con su modo, sus UV corriendo, y el color del pulso)
    if (an) {
        ApplyAnim(*an, timeAnim, -1.0f, m.idLm, index, (m.flags & (MatMix | MatAdditive)) != 0);
        animPlaced = true;
    } else if (m.idLm && (m.flags & MatLayerSum)) {
        gfx::MixTextures(m.idLm, 1.0f, true, true, 5);
        gfx::MixTexturesUV(0);
    } else if (m.idLm && (g.vboLm || !g.lm.empty())) {
        gfx::MixTextures(m.idLm, 1.0f, true, false, 2);
        if (g.vboLm) gfx::MixTexturesUVVBO(g.vboLm); else gfx::MixTexturesUV(&g.lm[0]);
    } else gfx::MixTexturesOff();
    gfx::AlphaTest((m.flags & MatAlphaTest) ? m.alphaRef : 0.0f);
    // (lo aditivo animado se suma por su alfa: el pulso lo apaga y lo prende entero, las dos capas)
    if (m.flags & (MatMix | MatAdditive)) { gfx::Enable(gfx::Blend); gfx::SetMix((m.flags & MatAdditive) ? (an ? gfx::MixAddAlpha : gfx::MixAdd) : gfx::MixAlpha); }
    else gfx::Disable(gfx::Blend);
    if (m.flags & MatTwoFaces) gfx::Disable(gfx::CullFace); else gfx::Enable(gfx::CullFace);
    gfx::DepthMask(!(m.flags & MatNoZ));
    // las calcomanias, hacia la camara en unidades del z (crece con la distancia, como el error del z de 16 bits) y
    // segun la pendiente (el piso visto de costado)
    if (m.flags & MatDecal) { gfx::Enable(gfx::PolygonOffsetFill); gfx::PolygonOffset(-1.0f, -4.0f); }
    else gfx::Disable(gfx::PolygonOffsetFill);
}

void WorldBsp::Draw(const VisBsp& bsp, int cluster, const std::vector<unsigned char>& row, const float* planes, int phase) {
    if (!Valid()) { last = Stats(); return; }
    const unsigned nMat = (unsigned)materials.size();
    if (phase == 2) {   // lo mezclado, con lo que junto la fase 1
        if (touched.empty()) return;
        DrawTouched(2);
        return;
    }
    last = Stats();
    const size_t nH = bsp.leafCluster.size();
    if (cluster != markedCluster || leafVis.size() != nH) Mark(bsp, cluster, row);
    // ---- 1) juntar las caras de las hojas visibles por (grupo, material) ----
    if (++stamp == 0) { stamp = 1; std::fill(stampFace.begin(), stampFace.end(), 0u); }
    for (size_t t = 0; t < touched.size(); t++) { cubes[touched[t]].clear(); touchedCube[touched[t]] = 0; }
    touched.clear();
    stack.clear();
    if (bsp.nodes.empty()) { for (size_t h = 0; h < nH; h++) stack.push_back(-(int)h - 1); }
    else stack.push_back(0);
    while (!stack.empty()) {
        const int i = stack.back(); stack.pop_back();
        if (i >= 0) {
            if (!nodeVis[(size_t)i]) continue;
            if (planes && !BoxInPlanes(planes, &boxNode[(size_t)i * 6])) continue;
            const VisBsp::Node& nd = bsp.nodes[(size_t)i];
            stack.push_back(nd.child1); stack.push_back(nd.child0);
            continue;
        }
        const size_t h = (size_t)(-i - 1);
        if (!leafVis[h]) continue;
        if (planes && !BoxInPlanes(planes, &boxLeaf[h * 6])) continue;
        last.leaves++;
        for (unsigned k = leafFaceOff[h]; k < leafFaceOff[h + 1]; k++) {
            const unsigned f = leafFaces[k];
            if (stampFace[f] == stamp) continue;   // (ya la junto otra hoja)
            stampFace[f] = stamp;
            if (planes && !boxFace.empty() && !BoxInPlanes(planes, &boxFace[(size_t)f * 6])) continue;
            if (!faceRange.empty()) {
                // el nivel de detalle: la distancia a su centro (el del dato, o el de la caja) contra el rango de la cara
                const float* cc = &boxFace[(size_t)f * 6];
                float cx = (cc[0] + cc[3]) * 0.5f, cy = (cc[1] + cc[4]) * 0.5f, cz = (cc[2] + cc[5]) * 0.5f;
                if (!rangeCenter.empty()) {
                    const float* c3 = &rangeCenter[(size_t)f * 3];
                    cx = c3[0]; cy = c3[1]; cz = c3[2];
                }
                const float dx = cx - cutEye[0], dy = cy - cutEye[1], dz = cz - cutEye[2];
                const float d2 = (dx * dx + dy * dy + dz * dz) * rangeScale * rangeScale;
                const float r0 = faceRange[(size_t)f * 2], r1 = faceRange[(size_t)f * 2 + 1];
                if (d2 < r0 * r0 || d2 >= r1 * r1) continue;
            }
            const Face& c = faces[f];
            const float cut = (cutTrace > cutProp && (materials[c.material].flags & MatDecal)) ? cutTrace : cutProp;
            if (cut > 0.0f && !boxFace.empty()) {
                const float* cc = &boxFace[(size_t)f * 6];
                const float hx = (cc[3] - cc[0]) * 0.5f, hy = (cc[4] - cc[1]) * 0.5f, hz = (cc[5] - cc[2]) * 0.5f;
                const float dx = cc[0] + hx - cutEye[0], dy = cc[1] + hy - cutEye[1], dz = cc[2] + hz - cutEye[2];
                if (hx * hx + hy * hy + hz * hz < (dx * dx + dy * dy + dz * dz) * cut * cut) continue;
            }
            const unsigned b = (unsigned)c.group * nMat + c.material;
            std::vector<unsigned>& v = cubes[b];
            if (!touchedCube[b]) { touchedCube[b] = 1; touched.push_back(b); }
            if (!v.empty() && v[v.size() - 2] + v[v.size() - 1] == c.first) v[v.size() - 1] += c.count;   // seguidas: un tramo
            else { v.push_back(c.first); v.push_back(c.count); }
            last.faces++;
        }
    }
    if (touched.empty()) return;
    std::sort(touched.begin(), touched.end());
    DrawTouched(phase);
}

// ---- 2) dibujar lo juntado: por grupo (sus punteros una vez) y por material en el orden del archivo (opacos primero).
//      'fase' 1 = solo opacos, 2 = solo mezclados, 0 = todos ----
void WorldBsp::DrawTouched(int phase) {
    const unsigned nMat = (unsigned)materials.size();
    gfx::Disable(gfx::Lighting);
    gfx::Disable(gfx::ColorMaterial);
    gfx::EnableArray(gfx::VertexArray); gfx::EnableArray(gfx::TexCoordArray); gfx::EnableArray(gfx::ColorArray);
    gfx::DisableArray(gfx::NormalArray);
    gfx::TexEnvReplace(false);
    gfx::Color4f(1.0f, 1.0f, 1.0f, 1.0f);
    gfx::Enable(gfx::DepthTest);
    gfx::DepthFunc(gfx::DepthLess);
    gfx::SmoothShading(true);
    unsigned currentGroup = 0xFFFFFFFFu;
    for (size_t t = 0; t < touched.size(); t++) {
        const unsigned b = touched[t];
        const unsigned gi = b / nMat, mi = b % nMat;
        const bool mixed = (materials[mi].flags & (MatMix | MatAdditive)) != 0;
        if ((phase == 1 && mixed) || (phase == 2 && !mixed)) continue;
        Group& g = groups[gi];
        if (gi != currentGroup) { PointersGroup(g); currentGroup = gi; }
        MaterialState(materials[mi], g, mi);
        const std::vector<unsigned>& v = cubes[b];
        indices.clear();
        for (size_t k = 0; k + 1 < v.size(); k += 2) {
            const unsigned short* src = &g.idx[v[k]];
            const unsigned cnt = v[k + 1];
            const size_t base = indices.size();
            indices.resize(base + cnt);
            if (sizeof(MeshIndex) == 2) memcpy(&indices[base], src, cnt * 2);
            else for (unsigned q = 0; q < cnt; q++) indices[base + q] = (MeshIndex)src[q];
        }
        if (indices.empty()) continue;
        gfx::DrawTrianglesClientIdx((int)indices.size(), &indices[0]);
        last.draws++;
        last.tris += (int)(indices.size() / 3);
    }
    if (animPlaced) { RemoveAnim(); animPlaced = false; }
    gfx::MixTexturesOff();
    gfx::AlphaTest(0.0f);
    gfx::Disable(gfx::Blend);
    gfx::Disable(gfx::PolygonOffsetFill);
    gfx::DepthMask(true);
    gfx::Enable(gfx::CullFace);
}
