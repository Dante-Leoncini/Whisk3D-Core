#include "MeshFlows.h"
#include "MeshResource.h"
#include "w3dGraphics.h"
#include "w3dlog.h"
#include <algorithm>
#include <string.h>

namespace gfx = w3dEngine;

bool W3dPrimSimulated(int prim) { return prim == RPrimQuads || prim == RPrimStripQuads || prim == RPrimPolygon; }

// ---------------------------------------------------------------------------
//  TIRAS: greedy por adyacencia. Cada tira arranca en un triangulo libre (probando sus 3 rotaciones: se queda
//  con la que mas se estira) y avanza por la arista que forman sus 2 ultimos vertices. El triangulo siguiente
//  solo entra si, tal como lo va a dibujar GL (los impares dados vuelta), queda con el MISMO sentido que tiene en
//  la malla: el culling sigue funcionando igual. Las tiras se cosen con degenerados (y uno mas si la siguiente
//  arrancaria en posicion impar, para no invertirla).
// ---------------------------------------------------------------------------
struct EdgeTri { unsigned long long key; int tri; };
static bool LowerEdge(const EdgeTri& a, const EdgeTri& b) {
    return a.key < b.key || (a.key == b.key && a.tri < b.tri);
}
static unsigned long long KeyEdge(unsigned a, unsigned b) {
    if (a > b) { unsigned t = a; a = b; b = t; }
    return ((unsigned long long)a << 32) | (unsigned long long)b;
}
// (u,v,w) es una rotacion ciclica de (a,b,c)
static bool SameDirection(unsigned a, unsigned b, unsigned c, unsigned u, unsigned v, unsigned w) {
    return (a == u && b == v && c == w) || (a == v && b == w && c == u) || (a == w && b == u && c == v);
}

struct Puller {
    const MeshIndex* t;
    int n;                                 // triangulos
    std::vector<EdgeTri> ar;               // (arista sin sentido, triangulo), ordenadas
    std::vector<int> used;                 // 0 libre, >0 la marca de la prueba que lo tomo, -1 definitivo
    int mark;
    std::vector<int> marked;               // los que tomo la prueba en curso (para desmarcarlos sin recorrer todo)

    // el triangulo libre (ni definitivo ni de esta prueba) que comparte la arista (p,q) y que dibujado en esa
    // posicion de la tira queda con su sentido. -1 = ninguno. 'x' = su tercer vertice
    int Neighbor(unsigned p, unsigned q, bool odd, unsigned& x) const {
        EdgeTri k; k.key = KeyEdge(p, q); k.tri = -1;
        std::vector<EdgeTri>::const_iterator it = std::lower_bound(ar.begin(), ar.end(), k, LowerEdge);
        for (; it != ar.end() && it->key == k.key; ++it) {
            const int c = it->tri;
            if (used[c] != 0) continue;
            const unsigned a = t[c * 3], b = t[c * 3 + 1], d = t[c * 3 + 2];
            unsigned third   = (a != p && a != q) ? a : ((b != p && b != q) ? b : d);
            // como lo dibuja GL en esa posicion: par (p,q,x), impar (q,p,x)
            const bool ok = odd ? SameDirection(q, p, third, a, b, d) : SameDirection(p, q, third, a, b, d);
            if (ok) { x = third; return c; }
        }
        return -1;
    }
    // arma la tira desde el triangulo 'c' en la rotacion 'rot'; con 'prueba' solo marca con la marca de la prueba
    int Build(int c, int rot, std::vector<MeshIndex>* strip, int m) {
        unsigned v[3] = { t[c * 3], t[c * 3 + 1], t[c * 3 + 2] };
        unsigned a = v[rot % 3], b = v[(rot + 1) % 3], d = v[(rot + 2) % 3];
        if (strip) { strip->clear(); strip->push_back((MeshIndex)a); strip->push_back((MeshIndex)b); strip->push_back((MeshIndex)d); }
        used[c] = m;
        if (m > 0) marked.push_back(c);
        int length = 1;
        unsigned p = b, q = d;
        for (;;) {
            unsigned x = 0;
            const int next = Neighbor(p, q, (length & 1) != 0, x);
            if (next < 0) break;
            used[next] = m;
            if (m > 0) marked.push_back(next);
            if (strip) strip->push_back((MeshIndex)x);
            p = q; q = x;
            length++;
        }
        return length;
    }
    void Unmark() { for (size_t i = 0; i < marked.size(); i++) used[marked[i]] = 0; marked.clear(); }
};

void W3dTriangleStrips(const MeshIndex* tri, int nIndices, std::vector<MeshIndex>& out) {
    out.clear();
    const int n = nIndices / 3;
    if (n <= 0) return;
    Puller T;
    T.t = tri; T.n = n; T.mark = 0;
    T.ar.resize((size_t)n * 3);
    for (int c = 0; c < n; c++)
        for (int e = 0; e < 3; e++) {
            EdgeTri& a = T.ar[(size_t)c * 3 + e];
            a.key = KeyEdge(tri[c * 3 + e], tri[c * 3 + (e + 1) % 3]);
            a.tri = c;
        }
    std::sort(T.ar.begin(), T.ar.end(), LowerEdge);
    T.used.assign((size_t)n, 0);
    std::vector<MeshIndex> strip;
    for (int c = 0; c < n; c++) {
        if (T.used[c] != 0) continue;
        // la rotacion que mas se estira (pruebas con marcas positivas, que despues se borran)
        int best = 0, bestLength = 0;
        for (int rot = 0; rot < 3; rot++) {
            const int m = ++T.mark;
            const int l = T.Build(c, rot, NULL, m);
            T.Unmark();
            if (l > bestLength) { bestLength = l; best = rot; }
        }
        T.Build(c, best, &strip, -1);
        // coser: degenerados (el ultimo de lo anterior y el primero de esta) y uno mas si arrancaria impar
        if (!out.empty()) {
            out.push_back(out.back());
            out.push_back(strip[0]);
            if (out.size() & 1) out.push_back(strip[0]);
        }
        out.insert(out.end(), strip.begin(), strip.end());
    }
}

// ---------------------------------------------------------------------------
//  EL CACHE de flujos derivados de un recurso (se arma a pedido; se tira si la malla cambia)
// ---------------------------------------------------------------------------
enum { CStrip = 0, CEdges, CVertices, CQuads, CStripQuads, CPolygon, kClasses };
struct DerivedFlow {
    std::vector<MeshIndex> idx;
    unsigned int ibo;
    bool ready;
    bool iboLost;                   // tenia IBO y el contexto GL se perdio: se vuelve a subir de 'idx' al usarlo
    DerivedFlow() : ibo(0), ready(false), iboLost(false) {}
};
struct CacheFlows {
    unsigned version;
    const MeshIndex* source;
    int nIndices;
    size_t nParts;
    std::vector<DerivedFlow> f;      // (nParts + 1) * kClasses: la ultima "parte" es TODAS
    std::vector<float> boxes;        // (nParts + 1) * 6
    std::vector<int> maxIdx;         // (nParts + 1), -2 = sin calcular
    std::vector<char> boxList;
    bool noticeNoRAM;                  // (onlyGPU) ya se aviso que un flujo derivado no se puede armar sin la RAM
    CacheFlows() : version(0), source(0), nIndices(0), nParts(0), noticeNoRAM(false) {}
};

static void DeleteCache(CacheFlows* c) {
    if (!c) return;
    for (size_t i = 0; i < c->f.size(); i++) if (c->f[i].ibo) gfx::DeleteBuffer(c->f[i].ibo);
    delete c;
}
void W3dMeshFlowsDelete(MeshResource* r) {
    if (!r || !r->flows) return;
    DeleteCache((CacheFlows*)r->flows);
    r->flows = 0;
}
static CacheFlows* Cache(MeshResource* r) {
    CacheFlows* c = (CacheFlows*)r->flows;
    if (c && c->version == r->version && c->source == r->faces && c->nIndices == r->facesSize &&
        c->nParts == r->parts.size()) return c;
    DeleteCache(c);
    c = new CacheFlows();
    c->version = r->version; c->source = r->faces; c->nIndices = r->facesSize; c->nParts = r->parts.size();
    c->f.resize((c->nParts + 1) * kClasses);
    c->boxes.assign((c->nParts + 1) * 6, 0.0f);
    c->boxList.assign(c->nParts + 1, 0);
    c->maxIdx.assign(c->nParts + 1, -2);
    r->flows = c;
    return c;
}
// el rango de la parte en el index buffer (parte < 0 o sin partes = todo)
static void Range(const MeshResource* r, int part, int& start, int& count, size_t& slot) {
    if (part < 0 || part >= (int)r->parts.size()) { start = 0; count = r->facesSize; slot = r->parts.size(); return; }
    start = r->parts[(size_t)part].start; count = r->parts[(size_t)part].count; slot = (size_t)part;
}

static void BuildDerived(const MeshResource* r, int start, int count, int cls, std::vector<MeshIndex>& o) {
    const MeshIndex* s = r->faces + start;
    o.clear();
    switch (cls) {
    case CStrip: W3dTriangleStrips(s, count, o); break;
    case CEdges: {
        std::vector<unsigned long long> k;
        k.reserve((size_t)count);
        for (int c = 0; c + 2 < count; c += 3)
            for (int e = 0; e < 3; e++) k.push_back(KeyEdge(s[c + e], s[c + (e + 1) % 3]));
        std::sort(k.begin(), k.end());
        k.erase(std::unique(k.begin(), k.end()), k.end());
        for (size_t i = 0; i < k.size(); i++) { o.push_back((MeshIndex)(k[i] >> 32)); o.push_back((MeshIndex)(k[i] & 0xffffffffull)); }
        break;
    }
    case CVertices:
        o.assign(s, s + count);
        std::sort(o.begin(), o.end());
        o.erase(std::unique(o.begin(), o.end()), o.end());
        break;
    case CQuads:        // (v0 v1 v2 v3) -> (v0 v1 v2) (v0 v2 v3)
        for (int q = 0; q + 3 < count; q += 4) {
            const MeshIndex a = s[q], b = s[q + 1], c = s[q + 2], d = s[q + 3];
            o.push_back(a); o.push_back(b); o.push_back(c); o.push_back(a); o.push_back(c); o.push_back(d);
        }
        break;
    case CStripQuads:    // quad i = (2i, 2i+1, 2i+3, 2i+2)
        for (int q = 0; q + 3 < count; q += 2) {
            const MeshIndex a = s[q], b = s[q + 1], c = s[q + 3], d = s[q + 2];
            o.push_back(a); o.push_back(b); o.push_back(c); o.push_back(a); o.push_back(c); o.push_back(d);
        }
        break;
    case CPolygon:     // abanico desde el primero
        for (int i = 1; i + 1 < count; i++) { o.push_back(s[0]); o.push_back(s[i]); o.push_back(s[i + 1]); }
        break;
    }
}
static int ClassOf(int prim) {
    switch (prim) {
    case RPrimStripTriangles: return CStrip;
    case RPrimLines:         return CEdges;
    case RPrimPoints:         return CVertices;
    case RPrimQuads:          return CQuads;
    case RPrimStripQuads:      return CStripQuads;
    case RPrimPolygon:       return CPolygon;
    default:                  return -1;   // el index buffer tal cual
    }
}
static int PrimGL(int prim) {
    switch (prim) {
    case RPrimPoints:         return gfx::PrimPoints;
    case RPrimLines:         return gfx::PrimLines;
    case RPrimStripLines:     return gfx::PrimStripLines;
    case RPrimLoopLines:     return gfx::PrimLoopLines;
    case RPrimStripTriangles: return gfx::PrimStripTriangles;
    case RPrimFan:        return gfx::PrimFan;
    default:                  return gfx::PrimTriangles;   // triangulos y las simuladas
    }
}

// el IBO de un flujo derivado. Un glBufferData que no entra (el MBX del N95 sin memoria de VBO) falla en silencio: sin
// IBO el flujo se manda de su copia en RAM (d.idx) en vez de un IBO vacio
static void UploadIBO(DerivedFlow& d) {
    gfx::ErrorGL();
    d.ibo = gfx::GenBuffer();
    if (d.ibo) gfx::IndexBufferData(d.ibo, &d.idx[0], (int)(d.idx.size() * sizeof(MeshIndex)));
    if (d.ibo && gfx::ErrorGL() != 0) { gfx::DeleteBuffer(d.ibo); d.ibo = 0; }
}

bool W3dMeshFlow(MeshResource* r, int part, int prim, W3dFlow& f) {
    // (onlyGPU: sin r->faces, los indices estan en el IBO de la malla. Sin IBO -un contexto GL perdido que todavia
    // no se volvio a subir- el flujo igual dice cuantos indices son: f.ibo 0 y f.idx NULL = no hay que mandar)
    if (!r || r->facesSize <= 0 || (!r->faces && !r->onlyGPU)) return false;
    int start, count; size_t slot;
    Range(r, part, start, count, slot);
    f.primGL = PrimGL(prim);
    const int cls = ClassOf(prim);
    if (cls < 0) {
        f.idx = r->faces ? r->faces + start : 0; f.count = count; f.ibo = r->vboIdx; f.first = start;
        return true;
    }
    CacheFlows* c = Cache(r);
    DerivedFlow& d = c->f[slot * kClasses + (size_t)cls];
    if (d.ready && d.iboLost) {
        // (el contexto GL se perdio: el IBO del flujo se vuelve a subir de su copia, una vez)
        d.iboLost = false;
        if (r->vboIdx && !d.idx.empty()) {
            UploadIBO(d);
        }
    }
    if (!d.ready && !r->faces) {
        // la malla solto su RAM (onlyGPU) y este flujo nunca se armo: no se puede armar sin releerla
        if (!c->noticeNoRAM) {
            c->noticeNoRAM = true;
            w3dLogfW("[mallas] '%s' esta solo en la GPU: el flujo de la primitiva %d de la parte %d no estaba armado "
                     "(no se dibuja; el editor o la precarga de la rutina lo arman releyendo la malla)",
                     r->name.c_str(), prim, part);
        }
        return false;
    }
    if (!d.ready) {
        BuildDerived(r, start, count, cls, d.idx);
        d.ready = true;
        // al IBO si la malla dibuja de VBO (si no, de RAM)
        if (r->vboIdx && !d.idx.empty()) {
            UploadIBO(d);
        }
    }
    f.idx = d.idx.empty() ? 0 : &d.idx[0]; f.count = (int)d.idx.size(); f.ibo = d.ibo; f.first = 0;
    return true;
}

int W3dMeshFlowIndices(MeshResource* r, int part, int prim) {
    W3dFlow f;
    return W3dMeshFlow(r, part, prim, f) ? f.count : 0;
}

bool W3dMeshPartBox(MeshResource* r, int part, float mn[3], float mx[3]) {
    if (!r || r->facesSize <= 0 || ((!r->faces || !r->vertex) && !r->onlyGPU)) return false;
    int start, count; size_t slot;
    Range(r, part, start, count, slot);
    CacheFlows* c = Cache(r);
    float* k = &c->boxes[slot * 6];
    if (!c->boxList[slot]) {
        if (!r->faces || !r->vertex) return false;   // (onlyGPU sin la caja cacheada: W3dMeshFlowsPrecompute la deja)
        for (int e = 0; e < 3; e++) { k[e] = 1e30f; k[3 + e] = -1e30f; }
        for (int i = start; i < start + count; i++) {
            const float* v = &r->vertex[(size_t)r->faces[i] * 3];
            for (int e = 0; e < 3; e++) { if (v[e] < k[e]) k[e] = v[e]; if (v[e] > k[3 + e]) k[3 + e] = v[e]; }
        }
        c->boxList[slot] = 1;
    }
    for (int e = 0; e < 3; e++) { mn[e] = k[e]; mx[e] = k[3 + e]; }
    return true;
}

int W3dMeshPartMaxIndex(MeshResource* r, int part) {
    if (!r || r->facesSize <= 0 || (!r->faces && !r->onlyGPU)) return -1;
    int start, count; size_t slot;
    Range(r, part, start, count, slot);
    CacheFlows* c = Cache(r);
    if (c->maxIdx[slot] == -2) {
        if (!r->faces) return -1;   // (onlyGPU sin el indice cacheado: W3dMeshFlowsPrecompute lo deja)
        int m = -1;
        for (int i = start; i < start + count; i++) if ((int)r->faces[i] > m) m = (int)r->faces[i];
        c->maxIdx[slot] = m;
    }
    return c->maxIdx[slot];
}

bool W3dMeshFlowReady(MeshResource* r, int part, int prim) {
    if (!r || r->facesSize <= 0) return false;
    const int cls = ClassOf(prim);
    if (cls < 0) return r->faces || r->vboIdx;   // el index buffer tal cual
    CacheFlows* c = (CacheFlows*)r->flows;
    if (!c || c->version != r->version || c->source != r->faces || c->nIndices != r->facesSize ||
        c->nParts != r->parts.size()) return false;
    int start, count; size_t slot;
    Range(r, part, start, count, slot);
    return c->f[slot * kClasses + (size_t)cls].ready;
}

void W3dMeshFlowsPrecompute(MeshResource* r) {
    if (!r || !r->faces || !r->vertex || r->facesSize <= 0) return;
    float mn[3], mx[3];
    for (int k = -1; k < (int)r->parts.size(); k++) {
        W3dMeshPartBox(r, k, mn, mx);
        W3dMeshPartMaxIndex(r, k);
    }
}

void W3dMeshFlowsRepoint(MeshResource* r) {
    CacheFlows* c = r ? (CacheFlows*)r->flows : 0;
    // (solo si es el cache de ESTA version: uno viejo lo tira Cache() en el proximo uso)
    if (c && c->version == r->version && c->nIndices == r->facesSize && c->nParts == r->parts.size()) c->source = r->faces;
}

void W3dMeshFlowsGLLost(MeshResource* r) {
    CacheFlows* c = r ? (CacheFlows*)r->flows : 0;
    if (!c) return;
    for (size_t i = 0; i < c->f.size(); i++)
        if (c->f[i].ibo) { c->f[i].ibo = 0; c->f[i].iboLost = true; }
}
