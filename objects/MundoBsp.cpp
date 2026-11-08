// ============================================================================
//  MundoBsp.cpp — ver MundoBsp.h. El mundo de un mapa BSP dibujado por el motor:
//  PVS + frustum jerarquico por el arbol, caras juntadas por material, un draw
//  por material desde VBOs. C++03 / Symbian-safe.
// ============================================================================
#include "objects/MundoBsp.h"
#include "objects/VisBsp.h"
#include "w3dGraphics.h"
#include "w3dlog.h"       // que paso con cada grupo (el N95 no tiene consola)
#include <string.h>
#include <stdlib.h>   // getenv: W3D_MUNDOBSP_RAM=1 fuerza el camino sin VBO (probarlo en PC)
#include <algorithm>
#include <math.h>

namespace gfx = w3dEngine;

// ---- lectura LE sin alineacion (como VisBsp.cpp) ----
static unsigned LeeU32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned LeeU16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static float LeeF32(const unsigned char* p) { unsigned u = LeeU32(p); float f; memcpy(&f, &u, 4); return f; }

MundoBsp::MundoBsp() : corteProp(0.0f), corteCalco(0.0f), escalaRango(1.0f), tiempoAnim(0.0f), animPuesta(false), clusterMarcado(-2), sello(0) { corteOjo[0] = corteOjo[1] = corteOjo[2] = 0.0f; }

void MundoBsp::CorteTamano(const float* ojo, float proporcion) {
    corteOjo[0] = ojo[0]; corteOjo[1] = ojo[1]; corteOjo[2] = ojo[2];
    corteProp = proporcion > 0.0f ? proporcion : 0.0f;
}

void MundoBsp::CorteCalcomanias(float proporcion) { corteCalco = proporcion > 0.0f ? proporcion : 0.0f; }

void MundoBsp::EscalaRangos(float escala) { escalaRango = escala > 0.01f ? escala : 0.01f; }
MundoBsp::~MundoBsp() { Limpiar(); }

void MundoBsp::Limpiar() {
    for (size_t i = 0; i < grupos.size(); i++) {
        Grupo& g = grupos[i];
        if (g.vboPos) gfx::DeleteBuffer(g.vboPos);
        if (g.vboUV)  gfx::DeleteBuffer(g.vboUV);
        if (g.vboLm)  gfx::DeleteBuffer(g.vboLm);
        if (g.vboCol) gfx::DeleteBuffer(g.vboCol);
    }
    materiales.clear(); grupos.clear(); caras.clear(); hojaCaraOff.clear(); hojaCaras.clear();
    cajaNodo.clear(); cajaHoja.clear(); cajaCara.clear(); rangoCara.clear(); centroRango.clear();
    anims.clear(); animDe.clear();
    hojaVis.clear(); nodoVis.clear(); selloCara.clear(); cubos.clear(); cuboTocado.clear(); tocados.clear();
    clusterMarcado = -2; sello = 0;
    ultimo = Estad();
}

size_t MundoBsp::Bytes() const {
    size_t b = caras.size() * sizeof(Cara) + (hojaCaraOff.size() + hojaCaras.size()) * 4 + (cajaNodo.size() + cajaHoja.size() + cajaCara.size()) * 4;
    for (size_t i = 0; i < grupos.size(); i++) {
        const Grupo& g = grupos[i];
        b += (g.pos.size() + g.uv.size() + g.lm.size()) * 4 + g.col.size() + g.idx.size() * 2;
    }
    return b;
}

// la seccion de geometria del .w3dbsp v2 (formato/w3dbsp.md):
//   "GEOM"
//   u32 nMateriales x { str textura; str lightmap; u32 flags; f32 alfaRef }        (str = u32 largo + utf-8)
//   u32 nGrupos x { u32 nVerts; u8 conLm; pos f32 x3; uv f32 x2; [lm f32 x2]; col u8 x4; u32 nIdx; u16 x nIdx }
//   u32 nCaras x { u16 grupo; u16 material; u32 primero; u32 cantidad }
//   u32 nHojas; (nHojas + 1) x u32 offsets; u32 nHojaCaras; u32 x nHojaCaras
//   cajas: nNodos x 6 f32; nHojas x 6 f32; [u8 1; nCaras x 6 f32]  (las de las caras: opcionales)
//   (opcional, al final, solo con las cajas de las caras) "MANI" u32 n x { u16 material; u16 0; Anim }: las animaciones
//   de los materiales MatAnimada. Un Anim: u16 nCuadros; u16 modoCapa; f32 scroll[4]; f32 color[3]; f32 brillo, pulso,
//   periodo, ruido; f32 fps; f32 distCuadros[2]; f32 distApagar[3]; nCuadros x str (las texturas de los cuadros)
bool MundoBsp::Cargar(const unsigned char* d, size_t n, size_t p, const VisBsp& bsp, std::string* err, bool subirAlCargar) {
    Limpiar();
    const size_t nNodos = bsp.nodos.size(), nHojas = bsp.hojaCluster.size();
    if (p + 8 > n || memcmp(d + p, "GEOM", 4) != 0) { if (err) *err = "sin seccion de geometria"; return false; }
    p += 4;
    const unsigned nMat = LeeU32(d + p); p += 4;
    if (nMat > 65535u) { if (err) *err = "cantidad de materiales absurda"; return false; }
    materiales.resize(nMat);
    for (unsigned i = 0; i < nMat; i++) {
        Material& m = materiales[i];
        for (int k = 0; k < 2; k++) {
            if (p + 4 > n) { if (err) *err = "cortado en los materiales"; Limpiar(); return false; }
            const unsigned largo = LeeU32(d + p); p += 4;
            if (largo > 4096u || p + largo > n) { if (err) *err = "cortado en los materiales"; Limpiar(); return false; }
            (k == 0 ? m.textura : m.lightmap).assign((const char*)d + p, largo); p += largo;
        }
        if (p + 8 > n) { if (err) *err = "cortado en los materiales"; Limpiar(); return false; }
        m.flags = LeeU32(d + p); m.alfaRef = LeeF32(d + p + 4); p += 8;
    }
    if (p + 4 > n) { if (err) *err = "cortado en los grupos"; Limpiar(); return false; }
    const unsigned nGrupos = LeeU32(d + p); p += 4;
    if (nGrupos > 65535u) { if (err) *err = "cantidad de grupos absurda"; Limpiar(); return false; }
    grupos.resize(nGrupos);
    int enVbo = 0, enRam = 0;
    for (unsigned gi = 0; gi < nGrupos; gi++) {
        Grupo& g = grupos[gi];
        if (p + 5 > n) { if (err) *err = "cortado en los grupos"; Limpiar(); return false; }
        g.nVerts = LeeU32(d + p); p += 4;
        const bool conLm = d[p] != 0; p += 1;
        if (g.nVerts == 0 || g.nVerts > 65536u) { if (err) *err = "grupo con mas de 65536 vertices"; Limpiar(); return false; }
        const size_t bytesV = (size_t)g.nVerts * (12 + 8 + (conLm ? 8 : 0) + 4);
        if (p + bytesV + 4 > n) { if (err) *err = "cortado en los vertices"; Limpiar(); return false; }
        g.pos.resize((size_t)g.nVerts * 3); g.uv.resize((size_t)g.nVerts * 2); g.col.resize((size_t)g.nVerts * 4);
        if (conLm) g.lm.resize((size_t)g.nVerts * 2);
        for (size_t k = 0; k < g.pos.size(); k++, p += 4) g.pos[k] = LeeF32(d + p);
        for (size_t k = 0; k < g.uv.size(); k++, p += 4)  g.uv[k]  = LeeF32(d + p);
        for (size_t k = 0; k < g.lm.size(); k++, p += 4)  g.lm[k]  = LeeF32(d + p);
        memcpy(&g.col[0], d + p, g.col.size()); p += g.col.size();
        const unsigned nIdx = LeeU32(d + p); p += 4;
        if (nIdx > 50000000u || p + (size_t)nIdx * 2 > n) { if (err) *err = "cortado en los indices"; Limpiar(); return false; }
        g.idx.resize(nIdx);
        for (unsigned k = 0; k < nIdx; k++, p += 2) {
            const unsigned v = LeeU16(d + p);
            if (v >= g.nVerts) { if (err) *err = "indice fuera del grupo"; Limpiar(); return false; }
            g.idx[k] = (unsigned short)v;
        }
        // a la GPU ya (el contexto esta: se carga desde el render): la RAM del grupo se suelta antes de parsear el que
        // sigue, asi el pico de memoria es el archivo + UN grupo, no el archivo + todo el mundo
        if (subirAlCargar) { if (SubirGrupo(g, (int)gi)) enVbo++; else enRam++; }
    }
    if (subirAlCargar) w3dLogf("[MundoBsp] %u grupos: %d en VBO, %d en RAM", nGrupos, enVbo, enRam);
    if (p + 4 > n) { if (err) *err = "cortado en las caras"; Limpiar(); return false; }
    const unsigned nCaras = LeeU32(d + p); p += 4;
    if (nCaras > 50000000u || p + (size_t)nCaras * 12 > n) { if (err) *err = "cortado en las caras"; Limpiar(); return false; }
    caras.resize(nCaras);
    for (unsigned i = 0; i < nCaras; i++, p += 12) {
        Cara& c = caras[i];
        c.grupo = (unsigned short)LeeU16(d + p); c.material = (unsigned short)LeeU16(d + p + 2);
        c.primero = LeeU32(d + p + 4); c.cantidad = LeeU32(d + p + 8);
        if (c.grupo >= nGrupos || c.material >= nMat || c.cantidad % 3 != 0 ||
            (size_t)c.primero + c.cantidad > grupos[c.grupo].idx.size()) {
            if (err) *err = "cara fuera de rango"; Limpiar(); return false;
        }
    }
    if (p + 4 > n) { if (err) *err = "cortado en las hojas"; Limpiar(); return false; }
    const unsigned nH = LeeU32(d + p); p += 4;
    if (nH != nHojas) { if (err) *err = "la geometria no tiene las hojas del arbol"; Limpiar(); return false; }
    if (p + (size_t)(nH + 1) * 4 + 4 > n) { if (err) *err = "cortado en las hojas"; Limpiar(); return false; }
    hojaCaraOff.resize(nH + 1);
    for (unsigned i = 0; i <= nH; i++, p += 4) hojaCaraOff[i] = LeeU32(d + p);
    const unsigned nHC = LeeU32(d + p); p += 4;
    if (nHC > 50000000u || p + (size_t)nHC * 4 > n || hojaCaraOff[nH] != nHC) { if (err) *err = "cortado en las caras por hoja"; Limpiar(); return false; }
    hojaCaras.resize(nHC);
    for (unsigned i = 0; i < nHC; i++, p += 4) {
        hojaCaras[i] = LeeU32(d + p);
        if (hojaCaras[i] >= nCaras) { if (err) *err = "cara de hoja fuera de rango"; Limpiar(); return false; }
    }
    for (unsigned i = 0; i < nH; i++)
        if (hojaCaraOff[i] > hojaCaraOff[i + 1]) { if (err) *err = "offsets de hojas desordenados"; Limpiar(); return false; }
    const size_t nCaja = (nNodos + nHojas) * 6;
    if (p + nCaja * 4 > n) { if (err) *err = "cortado en las cajas"; Limpiar(); return false; }
    cajaNodo.resize(nNodos * 6); cajaHoja.resize(nHojas * 6);
    for (size_t k = 0; k < cajaNodo.size(); k++, p += 4) cajaNodo[k] = LeeF32(d + p);
    for (size_t k = 0; k < cajaHoja.size(); k++, p += 4) cajaHoja[k] = LeeF32(d + p);
    // opcional: la caja de cada cara (un byte de presencia + nCaras x 6 f32)
    if (p < n && d[p] != 0) {
        p += 1;
        if (p + (size_t)nCaras * 24 > n) { if (err) *err = "cortado en las cajas de las caras"; Limpiar(); return false; }
        cajaCara.resize((size_t)nCaras * 6);
        for (size_t k = 0; k < cajaCara.size(); k++, p += 4) cajaCara[k] = LeeF32(d + p);
        // opcional: el rango de distancia de cada cara (un byte 2 + nCaras x 2 f32): los niveles de detalle
        if (p < n && d[p] == 2) {
            p += 1;
            if (p + (size_t)nCaras * 8 > n) { if (err) *err = "cortado en los rangos de las caras"; Limpiar(); return false; }
            rangoCara.resize((size_t)nCaras * 2);
            for (size_t k = 0; k < rangoCara.size(); k++, p += 4) rangoCara[k] = LeeF32(d + p);
            // opcional: el punto desde el que se mide el rango de cada cara (un byte 3 + nCaras x 3 f32)
            if (p < n && d[p] == 3) {
                p += 1;
                if (p + (size_t)nCaras * 12 > n) { if (err) *err = "cortado en los centros de los rangos"; Limpiar(); return false; }
                centroRango.resize((size_t)nCaras * 3);
                for (size_t k = 0; k < centroRango.size(); k++, p += 4) centroRango[k] = LeeF32(d + p);
            }
        }
    }
    // opcional: las animaciones de los materiales
    animDe.assign(nMat, -1);
    if (p + 8 <= n && memcmp(d + p, "MANI", 4) == 0) {
        p += 4;
        const unsigned nA = LeeU32(d + p); p += 4;
        if (nA > nMat) { if (err) *err = "mas animaciones que materiales"; Limpiar(); return false; }
        anims.resize(nA);
        for (unsigned i = 0; i < nA; i++) {
            if (p + 4 > n) { if (err) *err = "cortado en las animaciones"; Limpiar(); return false; }
            const unsigned mi = LeeU16(d + p); p += 4;
            p = LeerAnim(d, n, p, anims[i]);
            if (!p || mi >= nMat) { if (err) *err = "animacion cortada o de un material que no esta"; Limpiar(); return false; }
            animDe[mi] = (int)i;
        }
    }
    selloCara.assign(nCaras, 0);
    cubos.resize((size_t)nGrupos * nMat);
    cuboTocado.assign(cubos.size(), 0);
    return true;
}

// ---- las hojas visibles desde el cluster y los nodos que tienen alguna (una vez por cambio de cluster) ----
void MundoBsp::Marcar(const VisBsp& bsp, int cluster, const std::vector<unsigned char>& fila) {
    const size_t nH = bsp.hojaCluster.size(), nN = bsp.nodos.size();
    hojaVis.assign(nH, 1);
    nodoVis.assign(nN, 1);
    clusterMarcado = cluster;
    if (cluster < 0 || fila.size() < (size_t)bsp.bytesFila) return;   // sin PVS: todo pasa
    for (size_t h = 0; h < nH; h++) {
        const int c = bsp.hojaCluster[h];
        hojaVis[h] = (c >= 0 && c < bsp.nClusters && (fila[(size_t)c >> 3] & (1u << (c & 7)))) ? 1 : 0;
    }
    // los nodos, de abajo hacia arriba: post-orden con pila explicita (el arbol puede ser hondo; la pila de Symbian no)
    if (nN == 0) return;
    nodoVis.assign(nN, 0);
    std::vector<unsigned char> hecho(nN, 0);
    pila.clear(); pila.push_back(0);
    while (!pila.empty()) {
        const int i = pila.back();
        const VisBsp::Nodo& nd = bsp.nodos[(size_t)i];
        if (!hecho[(size_t)i]) {
            hecho[(size_t)i] = 1;   // (primera visita: bajar a los hijos que son nodos)
            if (nd.hijo0 >= 0) pila.push_back(nd.hijo0);
            if (nd.hijo1 >= 0) pila.push_back(nd.hijo1);
            continue;
        }
        pila.pop_back();
        const bool v0 = nd.hijo0 >= 0 ? nodoVis[(size_t)nd.hijo0] != 0 : hojaVis[(size_t)(-nd.hijo0 - 1)] != 0;
        const bool v1 = nd.hijo1 >= 0 ? nodoVis[(size_t)nd.hijo1] != 0 : hojaVis[(size_t)(-nd.hijo1 - 1)] != 0;
        nodoVis[(size_t)i] = (v0 || v1) ? 1 : 0;
    }
}

static bool CajaEnPlanos(const float* p, const float* caja) {
    for (int i = 0; i < 6; i++) {
        const float a = p[i * 4], b = p[i * 4 + 1], c = p[i * 4 + 2], d = p[i * 4 + 3];
        const float px = (a >= 0.0f) ? caja[3] : caja[0];
        const float py = (b >= 0.0f) ? caja[4] : caja[1];
        const float pz = (c >= 0.0f) ? caja[5] : caja[2];
        if (a * px + b * py + c * pz + d < 0.0f) return false;
    }
    return true;
}

// sube los atributos de un grupo a VBOs y, si el driver los acepto (un glBufferData que no entra -el MBX del N95- falla
// en silencio: se pregunta ErrorGL despues), suelta su RAM. Si no entro, se borran los buffers y el grupo se dibuja
// desde RAM (mas lento, pero se ve). Devuelve si quedo en VBO
bool MundoBsp::SubirGrupo(Grupo& g, int indice) {
    g.subido = true;
    static const bool forzarRam = getenv("W3D_MUNDOBSP_RAM") != NULL && getenv("W3D_MUNDOBSP_RAM")[0] != 0;
    if (!gfx::VBOSoportado() || forzarRam) { w3dLogfW("[MundoBsp] grupo %d: %s -> RAM", indice, forzarRam ? "W3D_MUNDOBSP_RAM" : "sin VBO en este driver"); return false; }
    gfx::ErrorGL();   // (vaciar lo que hubiera)
    g.vboPos = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboPos, &g.pos[0], (int)(g.pos.size() * sizeof(float)));
    g.vboUV  = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboUV,  &g.uv[0],  (int)(g.uv.size() * sizeof(float)));
    g.vboCol = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboCol, &g.col[0], (int)g.col.size());
    if (!g.lm.empty()) { g.vboLm = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboLm, &g.lm[0], (int)(g.lm.size() * sizeof(float))); }
    const unsigned err = gfx::ErrorGL();
    const bool ok = err == 0 && g.vboPos && g.vboUV && g.vboCol && (g.lm.empty() || g.vboLm);
    if (!ok) {
        w3dLogfW("[MundoBsp] grupo %d (%u vertices, %u KB): el driver no lo acepto (glError 0x%x) -> se dibuja desde RAM",
                 indice, g.nVerts, (unsigned)((g.pos.size() + g.uv.size() + g.lm.size()) * 4 + g.col.size()) / 1024u, err);
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

void MundoBsp::PunterosGrupo(Grupo& g) {
    if (!g.subido) SubirGrupo(g, (int)(&g - &grupos[0]));
    if (g.vboPos) { gfx::VertexVBO(g.vboPos); gfx::TexCoordVBO(g.vboUV); gfx::ColorVBO(g.vboCol); }
    else {
        gfx::UnbindVBOs();
        gfx::VertexPointer3f(0, &g.pos[0]); gfx::TexCoordPointer2f(0, &g.uv[0]); gfx::ColorPointer4ub(&g.col[0]);
    }
}

MundoBsp::Anim::Anim() : modoCapa(5), brillo(1.0f), pulso(0.0f), periodo(1.0f), ruido(0.0f), fps(0.0f) {
    scroll[0] = scroll[1] = scroll[2] = scroll[3] = 0.0f;
    color[0] = color[1] = color[2] = 1.0f;
    distCuadros[0] = distCuadros[1] = 0.0f;
    distApagar[0] = distApagar[1] = 0.0f; distApagar[2] = 1.0f;
}

size_t MundoBsp::LeerAnim(const unsigned char* d, size_t n, size_t p, Anim& a) {
    if (p + 72 > n) return 0;
    const unsigned nc = LeeU16(d + p);
    a.modoCapa = (int)LeeU16(d + p + 2); p += 4;
    float f[17];
    for (int k = 0; k < 17; k++, p += 4) f[k] = LeeF32(d + p);
    for (int k = 0; k < 4; k++) a.scroll[k] = f[k];
    for (int k = 0; k < 3; k++) a.color[k] = f[4 + k];
    a.brillo = f[7]; a.pulso = f[8]; a.periodo = f[9] > 0.001f ? f[9] : 1.0f; a.ruido = f[10]; a.fps = f[11];
    a.distCuadros[0] = f[12]; a.distCuadros[1] = f[13];
    a.distApagar[0] = f[14]; a.distApagar[1] = f[15]; a.distApagar[2] = f[16];
    if (nc > 256) return 0;
    a.cuadros.resize(nc);
    a.idCuadros.assign(nc, 0u);
    for (unsigned k = 0; k < nc; k++) {
        if (p + 4 > n) return 0;
        const unsigned largo = LeeU32(d + p); p += 4;
        if (largo > 4096u || p + largo > n) return 0;
        a.cuadros[k].assign((const char*)d + p, largo); p += largo;
    }
    return p;
}

unsigned MundoBsp::CuadroAnim(const Anim& a, float t, float distancia, unsigned idTex) {
    const int nc = (int)a.idCuadros.size();
    if (nc == 0) return idTex;
    int k = 0;
    if (a.fps > 0.0f) k = (int)(t * a.fps) % nc;
    else if (distancia >= 0.0f && a.distCuadros[1] > a.distCuadros[0]) {
        const float f = (distancia - a.distCuadros[0]) / (a.distCuadros[1] - a.distCuadros[0]);
        k = (int)(f * (float)(nc - 1) + 0.5f);
    }
    if (k < 0) k = 0;
    if (k >= nc) k = nc - 1;
    return a.idCuadros[(size_t)k] ? a.idCuadros[(size_t)k] : idTex;
}

// (el corrimiento de las UV dentro de una vuelta: con el reloj grande el float pierde los decimales)
static float Vuelta(float x) { return x - floorf(x); }

void MundoBsp::AplicarAnim(const Anim& a, float t, float distancia, unsigned idCapa, unsigned semilla, bool mezclado) {
    gfx::TexMatrixUV(Vuelta(a.scroll[0] * t), Vuelta(a.scroll[1] * t), 1.0f, 1.0f);
    if (idCapa) {
        gfx::MezclaTexturas(idCapa, 1.0f, true, true, a.modoCapa);
        gfx::MezclaTexturasUV(0);
        gfx::MezclaTexturasMatrizUV(Vuelta(a.scroll[2] * t), Vuelta(a.scroll[3] * t), 1.0f, 1.0f);
    } else gfx::MezclaTexturasOff();
    // el parpadeo: un azar nuevo 15 veces por segundo (un hash del tramo, distinto por material)
    unsigned h = (unsigned)(t * 15.0f) * 2654435761u + semilla * 40503u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    const float azar = (float)(h & 0xffffu) / 65535.0f;
    float k = a.brillo + a.pulso * sinf(t * 6.2831853f / a.periodo) - a.ruido * azar;
    if (distancia >= 0.0f && a.distApagar[1] > a.distApagar[0]) {
        float f = (distancia - a.distApagar[0]) / (a.distApagar[1] - a.distApagar[0]);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        k *= 1.0f + (a.distApagar[2] - 1.0f) * f;
    }
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    gfx::DisableArray(gfx::ColorArray);
    if (mezclado) gfx::Color4f(a.color[0], a.color[1], a.color[2], k);
    else gfx::Color4f(a.color[0] * k, a.color[1] * k, a.color[2] * k, 1.0f);
}

void MundoBsp::QuitarAnim() {
    gfx::TexMatrixUV(0.0f, 0.0f, 1.0f, 1.0f);
    gfx::MezclaTexturasMatrizUV(0.0f, 0.0f, 1.0f, 1.0f);
    gfx::EnableArray(gfx::ColorArray);
    gfx::Color4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void MundoBsp::EstadoMaterial(const Material& m, const Grupo& g, unsigned indice) {
    const Anim* an = ((m.flags & MatAnimada) && indice < animDe.size() && animDe[indice] >= 0) ? &anims[(size_t)animDe[indice]] : 0;
    if (!an && animPuesta) { QuitarAnim(); animPuesta = false; }
    const unsigned idTex = an ? CuadroAnim(*an, tiempoAnim, -1.0f, m.idTex) : m.idTex;
    if (idTex && !(m.flags & MatSinTextura)) {
        gfx::Enable(gfx::Texture2D);
        gfx::BindTexture(idTex);
        gfx::TexFilter(!(m.flags & MatSinFiltro));
        gfx::TexWrap(!(m.flags & MatSinRepetir));
    } else gfx::Disable(gfx::Texture2D);
    // el lightmap: la unidad 1 con la pagina en Multiply 2x y SUS UV (las del grupo)
    // (MatCapaSuma: la capa se SUMA con las mismas UV de la textura base: el grupo no necesita UV propias)
    // (MatAnimada: la 2da capa con su modo, sus UV corriendo, y el color del pulso)
    if (an) {
        AplicarAnim(*an, tiempoAnim, -1.0f, m.idLm, indice, (m.flags & (MatMezcla | MatAditivo)) != 0);
        animPuesta = true;
    } else if (m.idLm && (m.flags & MatCapaSuma)) {
        gfx::MezclaTexturas(m.idLm, 1.0f, true, true, 5);
        gfx::MezclaTexturasUV(0);
    } else if (m.idLm && (g.vboLm || !g.lm.empty())) {
        gfx::MezclaTexturas(m.idLm, 1.0f, true, false, 2);
        if (g.vboLm) gfx::MezclaTexturasUVVBO(g.vboLm); else gfx::MezclaTexturasUV(&g.lm[0]);
    } else gfx::MezclaTexturasOff();
    gfx::AlphaTest((m.flags & MatAlfaTest) ? m.alfaRef : 0.0f);
    // (lo aditivo animado se suma por su alfa: el pulso lo apaga y lo prende entero, las dos capas)
    if (m.flags & (MatMezcla | MatAditivo)) { gfx::Enable(gfx::Blend); gfx::SetMezcla((m.flags & MatAditivo) ? (an ? gfx::MezclaAddAlpha : gfx::MezclaAdd) : gfx::MezclaAlpha); }
    else gfx::Disable(gfx::Blend);
    if (m.flags & MatDosCaras) gfx::Disable(gfx::CullFace); else gfx::Enable(gfx::CullFace);
    gfx::DepthMask(!(m.flags & MatSinZ));
    // las calcomanias, hacia la camara en unidades del z (crece con la distancia, como el error del z de 16 bits) y
    // segun la pendiente (el piso visto de costado)
    if (m.flags & MatCalcomania) { gfx::Enable(gfx::PolygonOffsetFill); gfx::PolygonOffset(-1.0f, -4.0f); }
    else gfx::Disable(gfx::PolygonOffsetFill);
}

void MundoBsp::Dibujar(const VisBsp& bsp, int cluster, const std::vector<unsigned char>& fila, const float* planos, int fase) {
    if (!Valido()) { ultimo = Estad(); return; }
    const unsigned nMat = (unsigned)materiales.size();
    if (fase == 2) {   // lo mezclado, con lo que junto la fase 1
        if (tocados.empty()) return;
        DibujarTocados(2);
        return;
    }
    ultimo = Estad();
    const size_t nH = bsp.hojaCluster.size();
    if (cluster != clusterMarcado || hojaVis.size() != nH) Marcar(bsp, cluster, fila);
    // ---- 1) juntar las caras de las hojas visibles por (grupo, material) ----
    if (++sello == 0) { sello = 1; std::fill(selloCara.begin(), selloCara.end(), 0u); }
    for (size_t t = 0; t < tocados.size(); t++) { cubos[tocados[t]].clear(); cuboTocado[tocados[t]] = 0; }
    tocados.clear();
    pila.clear();
    if (bsp.nodos.empty()) { for (size_t h = 0; h < nH; h++) pila.push_back(-(int)h - 1); }
    else pila.push_back(0);
    while (!pila.empty()) {
        const int i = pila.back(); pila.pop_back();
        if (i >= 0) {
            if (!nodoVis[(size_t)i]) continue;
            if (planos && !CajaEnPlanos(planos, &cajaNodo[(size_t)i * 6])) continue;
            const VisBsp::Nodo& nd = bsp.nodos[(size_t)i];
            pila.push_back(nd.hijo1); pila.push_back(nd.hijo0);
            continue;
        }
        const size_t h = (size_t)(-i - 1);
        if (!hojaVis[h]) continue;
        if (planos && !CajaEnPlanos(planos, &cajaHoja[h * 6])) continue;
        ultimo.hojas++;
        for (unsigned k = hojaCaraOff[h]; k < hojaCaraOff[h + 1]; k++) {
            const unsigned f = hojaCaras[k];
            if (selloCara[f] == sello) continue;   // (ya la junto otra hoja)
            selloCara[f] = sello;
            if (planos && !cajaCara.empty() && !CajaEnPlanos(planos, &cajaCara[(size_t)f * 6])) continue;
            if (!rangoCara.empty()) {
                // el nivel de detalle: la distancia a su centro (el del dato, o el de la caja) contra el rango de la cara
                const float* cc = &cajaCara[(size_t)f * 6];
                float cx = (cc[0] + cc[3]) * 0.5f, cy = (cc[1] + cc[4]) * 0.5f, cz = (cc[2] + cc[5]) * 0.5f;
                if (!centroRango.empty()) {
                    const float* c3 = &centroRango[(size_t)f * 3];
                    cx = c3[0]; cy = c3[1]; cz = c3[2];
                }
                const float dx = cx - corteOjo[0], dy = cy - corteOjo[1], dz = cz - corteOjo[2];
                const float d2 = (dx * dx + dy * dy + dz * dz) * escalaRango * escalaRango;
                const float r0 = rangoCara[(size_t)f * 2], r1 = rangoCara[(size_t)f * 2 + 1];
                if (d2 < r0 * r0 || d2 >= r1 * r1) continue;
            }
            const Cara& c = caras[f];
            const float corte = (corteCalco > corteProp && (materiales[c.material].flags & MatCalcomania)) ? corteCalco : corteProp;
            if (corte > 0.0f && !cajaCara.empty()) {
                const float* cc = &cajaCara[(size_t)f * 6];
                const float hx = (cc[3] - cc[0]) * 0.5f, hy = (cc[4] - cc[1]) * 0.5f, hz = (cc[5] - cc[2]) * 0.5f;
                const float dx = cc[0] + hx - corteOjo[0], dy = cc[1] + hy - corteOjo[1], dz = cc[2] + hz - corteOjo[2];
                if (hx * hx + hy * hy + hz * hz < (dx * dx + dy * dy + dz * dz) * corte * corte) continue;
            }
            const unsigned b = (unsigned)c.grupo * nMat + c.material;
            std::vector<unsigned>& v = cubos[b];
            if (!cuboTocado[b]) { cuboTocado[b] = 1; tocados.push_back(b); }
            if (!v.empty() && v[v.size() - 2] + v[v.size() - 1] == c.primero) v[v.size() - 1] += c.cantidad;   // seguidas: un tramo
            else { v.push_back(c.primero); v.push_back(c.cantidad); }
            ultimo.caras++;
        }
    }
    if (tocados.empty()) return;
    std::sort(tocados.begin(), tocados.end());
    DibujarTocados(fase);
}

// ---- 2) dibujar lo juntado: por grupo (sus punteros una vez) y por material en el orden del archivo (opacos primero).
//      'fase' 1 = solo opacos, 2 = solo mezclados, 0 = todos ----
void MundoBsp::DibujarTocados(int fase) {
    const unsigned nMat = (unsigned)materiales.size();
    gfx::Disable(gfx::Lighting);
    gfx::Disable(gfx::ColorMaterial);
    gfx::EnableArray(gfx::VertexArray); gfx::EnableArray(gfx::TexCoordArray); gfx::EnableArray(gfx::ColorArray);
    gfx::DisableArray(gfx::NormalArray);
    gfx::TexEnvReplace(false);
    gfx::Color4f(1.0f, 1.0f, 1.0f, 1.0f);
    gfx::Enable(gfx::DepthTest);
    gfx::DepthFunc(gfx::DepthLess);
    gfx::SmoothShading(true);
    unsigned grupoActual = 0xFFFFFFFFu;
    for (size_t t = 0; t < tocados.size(); t++) {
        const unsigned b = tocados[t];
        const unsigned gi = b / nMat, mi = b % nMat;
        const bool mezclado = (materiales[mi].flags & (MatMezcla | MatAditivo)) != 0;
        if ((fase == 1 && mezclado) || (fase == 2 && !mezclado)) continue;
        Grupo& g = grupos[gi];
        if (gi != grupoActual) { PunterosGrupo(g); grupoActual = gi; }
        EstadoMaterial(materiales[mi], g, mi);
        const std::vector<unsigned>& v = cubos[b];
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
        ultimo.draws++;
        ultimo.tris += (int)(indices.size() / 3);
    }
    if (animPuesta) { QuitarAnim(); animPuesta = false; }
    gfx::MezclaTexturasOff();
    gfx::AlphaTest(0.0f);
    gfx::Disable(gfx::Blend);
    gfx::Disable(gfx::PolygonOffsetFill);
    gfx::DepthMask(true);
    gfx::Enable(gfx::CullFace);
}
