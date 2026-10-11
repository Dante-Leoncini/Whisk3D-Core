// ============================================================================
//  MundoBsp.cpp — ver MundoBsp.h. El mundo de un mapa BSP dibujado por el motor:
//  PVS + frustum jerarquico por el arbol, caras juntadas por material, un draw
//  por material desde VBOs. C++03 / Symbian-safe.
// ============================================================================
#include "objects/MundoBsp.h"
#include "objects/VisBsp.h"
#include "w3dGraphics.h"
#include <string.h>
#include <algorithm>

namespace gfx = w3dEngine;

// ---- lectura LE sin alineacion (como VisBsp.cpp) ----
static unsigned LeeU32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned LeeU16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static float LeeF32(const unsigned char* p) { unsigned u = LeeU32(p); float f; memcpy(&f, &u, 4); return f; }

MundoBsp::MundoBsp() : clusterMarcado(-2), sello(0) {}
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
    cajaNodo.clear(); cajaHoja.clear(); cajaCara.clear();
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
bool MundoBsp::Cargar(const unsigned char* d, size_t n, size_t p, const VisBsp& bsp, std::string* err) {
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
    }
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

void MundoBsp::SubirGrupo(Grupo& g) {
    g.subido = true;
    if (!gfx::VBOSoportado()) return;   // (se dibuja desde RAM)
    g.vboPos = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboPos, &g.pos[0], (int)(g.pos.size() * sizeof(float)));
    g.vboUV  = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboUV,  &g.uv[0],  (int)(g.uv.size() * sizeof(float)));
    g.vboCol = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboCol, &g.col[0], (int)g.col.size());
    if (!g.lm.empty()) { g.vboLm = gfx::GenBuffer(); gfx::ArrayBufferData(g.vboLm, &g.lm[0], (int)(g.lm.size() * sizeof(float))); }
    if (g.vboPos && g.vboUV && g.vboCol) {
        // la RAM de los atributos ya no hace falta (el telefono tiene poca): quedan en la GPU. Los indices si (viajan)
        std::vector<float>().swap(g.pos); std::vector<float>().swap(g.uv);
        if (g.vboLm) std::vector<float>().swap(g.lm);
        std::vector<unsigned char>().swap(g.col);
    }
}

void MundoBsp::PunterosGrupo(Grupo& g) {
    if (!g.subido) SubirGrupo(g);
    if (g.vboPos) { gfx::VertexVBO(g.vboPos); gfx::TexCoordVBO(g.vboUV); gfx::ColorVBO(g.vboCol); }
    else {
        gfx::UnbindVBOs();
        gfx::VertexPointer3f(0, &g.pos[0]); gfx::TexCoordPointer2f(0, &g.uv[0]); gfx::ColorPointer4ub(&g.col[0]);
    }
}

void MundoBsp::EstadoMaterial(const Material& m, const Grupo& g) {
    if (m.idTex && !(m.flags & MatSinTextura)) {
        gfx::Enable(gfx::Texture2D);
        gfx::BindTexture(m.idTex);
        gfx::TexFilter(!(m.flags & MatSinFiltro));
        gfx::TexWrap(!(m.flags & MatSinRepetir));
    } else gfx::Disable(gfx::Texture2D);
    // el lightmap: la unidad 1 con la pagina en Multiply 2x y SUS UV (las del grupo)
    if (m.idLm && (g.vboLm || !g.lm.empty())) {
        gfx::MezclaTexturas(m.idLm, 1.0f, true, false, 2);
        if (g.vboLm) gfx::MezclaTexturasUVVBO(g.vboLm); else gfx::MezclaTexturasUV(&g.lm[0]);
    } else gfx::MezclaTexturasOff();
    gfx::AlphaTest((m.flags & MatAlfaTest) ? m.alfaRef : 0.0f);
    if (m.flags & (MatMezcla | MatAditivo)) { gfx::Enable(gfx::Blend); gfx::SetMezcla((m.flags & MatAditivo) ? gfx::MezclaAdd : gfx::MezclaAlpha); }
    else gfx::Disable(gfx::Blend);
    if (m.flags & MatDosCaras) gfx::Disable(gfx::CullFace); else gfx::Enable(gfx::CullFace);
    gfx::DepthMask(!(m.flags & MatSinZ));
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
            const Cara& c = caras[f];
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
        EstadoMaterial(materiales[mi], g);
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
    gfx::MezclaTexturasOff();
    gfx::AlphaTest(0.0f);
    gfx::Disable(gfx::Blend);
    gfx::DepthMask(true);
    gfx::Enable(gfx::CullFace);
}
