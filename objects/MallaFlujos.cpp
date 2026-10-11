#include "MallaFlujos.h"
#include "MallaRecurso.h"
#include "w3dGraphics.h"
#include "w3dlog.h"
#include <algorithm>
#include <string.h>

namespace gfx = w3dEngine;

bool W3dPrimSimulada(int prim) { return prim == RPrimQuads || prim == RPrimTiraQuads || prim == RPrimPoligono; }

// ---------------------------------------------------------------------------
//  TIRAS: greedy por adyacencia. Cada tira arranca en un triangulo libre (probando sus 3 rotaciones: se queda
//  con la que mas se estira) y avanza por la arista que forman sus 2 ultimos vertices. El triangulo siguiente
//  solo entra si, tal como lo va a dibujar GL (los impares dados vuelta), queda con el MISMO sentido que tiene en
//  la malla: el culling sigue funcionando igual. Las tiras se cosen con degenerados (y uno mas si la siguiente
//  arrancaria en posicion impar, para no invertirla).
// ---------------------------------------------------------------------------
struct AristaTri { unsigned long long clave; int tri; };
static bool MenorArista(const AristaTri& a, const AristaTri& b) {
    return a.clave < b.clave || (a.clave == b.clave && a.tri < b.tri);
}
static unsigned long long ClaveArista(unsigned a, unsigned b) {
    if (a > b) { unsigned t = a; a = b; b = t; }
    return ((unsigned long long)a << 32) | (unsigned long long)b;
}
// (u,v,w) es una rotacion ciclica de (a,b,c)
static bool MismoSentido(unsigned a, unsigned b, unsigned c, unsigned u, unsigned v, unsigned w) {
    return (a == u && b == v && c == w) || (a == v && b == w && c == u) || (a == w && b == u && c == v);
}

struct Tirador {
    const MeshIndex* t;
    int n;                                   // triangulos
    std::vector<AristaTri> ar;               // (arista sin sentido, triangulo), ordenadas
    std::vector<int> usado;                  // 0 libre, >0 la marca de la prueba que lo tomo, -1 definitivo
    int marca;
    std::vector<int> marcados;               // los que tomo la prueba en curso (para desmarcarlos sin recorrer todo)

    // el triangulo libre (ni definitivo ni de esta prueba) que comparte la arista (p,q) y que dibujado en esa
    // posicion de la tira queda con su sentido. -1 = ninguno. 'x' = su tercer vertice
    int Vecino(unsigned p, unsigned q, bool impar, unsigned& x) const {
        AristaTri k; k.clave = ClaveArista(p, q); k.tri = -1;
        std::vector<AristaTri>::const_iterator it = std::lower_bound(ar.begin(), ar.end(), k, MenorArista);
        for (; it != ar.end() && it->clave == k.clave; ++it) {
            const int c = it->tri;
            if (usado[c] != 0) continue;
            const unsigned a = t[c * 3], b = t[c * 3 + 1], d = t[c * 3 + 2];
            unsigned tercero = (a != p && a != q) ? a : ((b != p && b != q) ? b : d);
            // como lo dibuja GL en esa posicion: par (p,q,x), impar (q,p,x)
            const bool ok = impar ? MismoSentido(q, p, tercero, a, b, d) : MismoSentido(p, q, tercero, a, b, d);
            if (ok) { x = tercero; return c; }
        }
        return -1;
    }
    // arma la tira desde el triangulo 'c' en la rotacion 'rot'; con 'prueba' solo marca con la marca de la prueba
    int Armar(int c, int rot, std::vector<MeshIndex>* tira, int m) {
        unsigned v[3] = { t[c * 3], t[c * 3 + 1], t[c * 3 + 2] };
        unsigned a = v[rot % 3], b = v[(rot + 1) % 3], d = v[(rot + 2) % 3];
        if (tira) { tira->clear(); tira->push_back((MeshIndex)a); tira->push_back((MeshIndex)b); tira->push_back((MeshIndex)d); }
        usado[c] = m;
        if (m > 0) marcados.push_back(c);
        int largo = 1;
        unsigned p = b, q = d;
        for (;;) {
            unsigned x = 0;
            const int sig = Vecino(p, q, (largo & 1) != 0, x);
            if (sig < 0) break;
            usado[sig] = m;
            if (m > 0) marcados.push_back(sig);
            if (tira) tira->push_back((MeshIndex)x);
            p = q; q = x;
            largo++;
        }
        return largo;
    }
    void Desmarcar() { for (size_t i = 0; i < marcados.size(); i++) usado[marcados[i]] = 0; marcados.clear(); }
};

void W3dTirasDeTriangulos(const MeshIndex* tri, int nIndices, std::vector<MeshIndex>& out) {
    out.clear();
    const int n = nIndices / 3;
    if (n <= 0) return;
    Tirador T;
    T.t = tri; T.n = n; T.marca = 0;
    T.ar.resize((size_t)n * 3);
    for (int c = 0; c < n; c++)
        for (int e = 0; e < 3; e++) {
            AristaTri& a = T.ar[(size_t)c * 3 + e];
            a.clave = ClaveArista(tri[c * 3 + e], tri[c * 3 + (e + 1) % 3]);
            a.tri = c;
        }
    std::sort(T.ar.begin(), T.ar.end(), MenorArista);
    T.usado.assign((size_t)n, 0);
    std::vector<MeshIndex> tira;
    for (int c = 0; c < n; c++) {
        if (T.usado[c] != 0) continue;
        // la rotacion que mas se estira (pruebas con marcas positivas, que despues se borran)
        int mejor = 0, mejorLargo = 0;
        for (int rot = 0; rot < 3; rot++) {
            const int m = ++T.marca;
            const int l = T.Armar(c, rot, NULL, m);
            T.Desmarcar();
            if (l > mejorLargo) { mejorLargo = l; mejor = rot; }
        }
        T.Armar(c, mejor, &tira, -1);
        // coser: degenerados (el ultimo de lo anterior y el primero de esta) y uno mas si arrancaria impar
        if (!out.empty()) {
            out.push_back(out.back());
            out.push_back(tira[0]);
            if (out.size() & 1) out.push_back(tira[0]);
        }
        out.insert(out.end(), tira.begin(), tira.end());
    }
}

// ---------------------------------------------------------------------------
//  EL CACHE de flujos derivados de un recurso (se arma a pedido; se tira si la malla cambia)
// ---------------------------------------------------------------------------
enum { CTira = 0, CAristas, CVertices, CQuads, CTiraQuads, CPoligono, kClases };
struct FlujoDerivado {
    std::vector<MeshIndex> idx;
    unsigned int ibo;
    bool listo;
    bool iboPerdido;                   // tenia IBO y el contexto GL se perdio: se vuelve a subir de 'idx' al usarlo
    FlujoDerivado() : ibo(0), listo(false), iboPerdido(false) {}
};
struct CacheFlujos {
    unsigned version;
    const MeshIndex* fuente;
    int nIndices;
    size_t nPartes;
    std::vector<FlujoDerivado> f;      // (nPartes + 1) * kClases: la ultima "parte" es TODAS
    std::vector<float> cajas;          // (nPartes + 1) * 6
    std::vector<int> maxIdx;           // (nPartes + 1), -2 = sin calcular
    std::vector<char> cajaLista;
    bool avisoSinRAM;                  // (soloGPU) ya se aviso que un flujo derivado no se puede armar sin la RAM
    CacheFlujos() : version(0), fuente(0), nIndices(0), nPartes(0), avisoSinRAM(false) {}
};

static void BorrarCache(CacheFlujos* c) {
    if (!c) return;
    for (size_t i = 0; i < c->f.size(); i++) if (c->f[i].ibo) gfx::DeleteBuffer(c->f[i].ibo);
    delete c;
}
void W3dMallaFlujosBorrar(MallaRecurso* r) {
    if (!r || !r->flujos) return;
    BorrarCache((CacheFlujos*)r->flujos);
    r->flujos = 0;
}
static CacheFlujos* Cache(MallaRecurso* r) {
    CacheFlujos* c = (CacheFlujos*)r->flujos;
    if (c && c->version == r->version && c->fuente == r->faces && c->nIndices == r->facesSize &&
        c->nPartes == r->partes.size()) return c;
    BorrarCache(c);
    c = new CacheFlujos();
    c->version = r->version; c->fuente = r->faces; c->nIndices = r->facesSize; c->nPartes = r->partes.size();
    c->f.resize((c->nPartes + 1) * kClases);
    c->cajas.assign((c->nPartes + 1) * 6, 0.0f);
    c->cajaLista.assign(c->nPartes + 1, 0);
    c->maxIdx.assign(c->nPartes + 1, -2);
    r->flujos = c;
    return c;
}
// el rango de la parte en el index buffer (parte < 0 o sin partes = todo)
static void Rango(const MallaRecurso* r, int parte, int& ini, int& cant, size_t& slot) {
    if (parte < 0 || parte >= (int)r->partes.size()) { ini = 0; cant = r->facesSize; slot = r->partes.size(); return; }
    ini = r->partes[(size_t)parte].inicio; cant = r->partes[(size_t)parte].cantidad; slot = (size_t)parte;
}

static void ArmarDerivado(const MallaRecurso* r, int ini, int cant, int clase, std::vector<MeshIndex>& o) {
    const MeshIndex* s = r->faces + ini;
    o.clear();
    switch (clase) {
    case CTira: W3dTirasDeTriangulos(s, cant, o); break;
    case CAristas: {
        std::vector<unsigned long long> k;
        k.reserve((size_t)cant);
        for (int c = 0; c + 2 < cant; c += 3)
            for (int e = 0; e < 3; e++) k.push_back(ClaveArista(s[c + e], s[c + (e + 1) % 3]));
        std::sort(k.begin(), k.end());
        k.erase(std::unique(k.begin(), k.end()), k.end());
        for (size_t i = 0; i < k.size(); i++) { o.push_back((MeshIndex)(k[i] >> 32)); o.push_back((MeshIndex)(k[i] & 0xffffffffull)); }
        break;
    }
    case CVertices:
        o.assign(s, s + cant);
        std::sort(o.begin(), o.end());
        o.erase(std::unique(o.begin(), o.end()), o.end());
        break;
    case CQuads:        // (v0 v1 v2 v3) -> (v0 v1 v2) (v0 v2 v3)
        for (int q = 0; q + 3 < cant; q += 4) {
            const MeshIndex a = s[q], b = s[q + 1], c = s[q + 2], d = s[q + 3];
            o.push_back(a); o.push_back(b); o.push_back(c); o.push_back(a); o.push_back(c); o.push_back(d);
        }
        break;
    case CTiraQuads:    // quad i = (2i, 2i+1, 2i+3, 2i+2)
        for (int q = 0; q + 3 < cant; q += 2) {
            const MeshIndex a = s[q], b = s[q + 1], c = s[q + 3], d = s[q + 2];
            o.push_back(a); o.push_back(b); o.push_back(c); o.push_back(a); o.push_back(c); o.push_back(d);
        }
        break;
    case CPoligono:     // abanico desde el primero
        for (int i = 1; i + 1 < cant; i++) { o.push_back(s[0]); o.push_back(s[i]); o.push_back(s[i + 1]); }
        break;
    }
}
static int ClaseDe(int prim) {
    switch (prim) {
    case RPrimTiraTriangulos: return CTira;
    case RPrimLineas:         return CAristas;
    case RPrimPuntos:         return CVertices;
    case RPrimQuads:          return CQuads;
    case RPrimTiraQuads:      return CTiraQuads;
    case RPrimPoligono:       return CPoligono;
    default:                  return -1;   // el index buffer tal cual
    }
}
static int PrimGL(int prim) {
    switch (prim) {
    case RPrimPuntos:         return gfx::PrimPuntos;
    case RPrimLineas:         return gfx::PrimLineas;
    case RPrimTiraLineas:     return gfx::PrimTiraLineas;
    case RPrimLazoLineas:     return gfx::PrimLazoLineas;
    case RPrimTiraTriangulos: return gfx::PrimTiraTriangulos;
    case RPrimAbanico:        return gfx::PrimAbanico;
    default:                  return gfx::PrimTriangulos;   // triangulos y las simuladas
    }
}

bool W3dMallaFlujo(MallaRecurso* r, int parte, int prim, W3dFlujo& f) {
    // (soloGPU: sin r->faces, los indices estan en el IBO de la malla. Sin IBO -un contexto GL perdido que todavia
    // no se volvio a subir- el flujo igual dice cuantos indices son: f.ibo 0 y f.idx NULL = no hay que mandar)
    if (!r || r->facesSize <= 0 || (!r->faces && !r->soloGPU)) return false;
    int ini, cant; size_t slot;
    Rango(r, parte, ini, cant, slot);
    f.primGL = PrimGL(prim);
    const int clase = ClaseDe(prim);
    if (clase < 0) {
        f.idx = r->faces ? r->faces + ini : 0; f.cantidad = cant; f.ibo = r->vboIdx; f.primero = ini;
        return true;
    }
    CacheFlujos* c = Cache(r);
    FlujoDerivado& d = c->f[slot * kClases + (size_t)clase];
    if (d.listo && d.iboPerdido) {
        // (el contexto GL se perdio: el IBO del flujo se vuelve a subir de su copia, una vez)
        d.iboPerdido = false;
        if (r->vboIdx && !d.idx.empty()) {
            d.ibo = gfx::GenBuffer();
            if (d.ibo) gfx::IndexBufferData(d.ibo, &d.idx[0], (int)(d.idx.size() * sizeof(MeshIndex)));
        }
    }
    if (!d.listo && !r->faces) {
        // la malla solto su RAM (soloGPU) y este flujo nunca se armo: no se puede armar sin releerla
        if (!c->avisoSinRAM) {
            c->avisoSinRAM = true;
            w3dLogfW("[mallas] '%s' esta solo en la GPU: el flujo de la primitiva %d de la parte %d no estaba armado "
                     "(no se dibuja; el editor o la precarga de la rutina lo arman releyendo la malla)",
                     r->nombre.c_str(), prim, parte);
        }
        return false;
    }
    if (!d.listo) {
        ArmarDerivado(r, ini, cant, clase, d.idx);
        d.listo = true;
        // al IBO si la malla dibuja de VBO (si no, de RAM)
        if (r->vboIdx && !d.idx.empty()) {
            d.ibo = gfx::GenBuffer();
            if (d.ibo) gfx::IndexBufferData(d.ibo, &d.idx[0], (int)(d.idx.size() * sizeof(MeshIndex)));
        }
    }
    f.idx = d.idx.empty() ? 0 : &d.idx[0]; f.cantidad = (int)d.idx.size(); f.ibo = d.ibo; f.primero = 0;
    return true;
}

int W3dMallaFlujoIndices(MallaRecurso* r, int parte, int prim) {
    W3dFlujo f;
    return W3dMallaFlujo(r, parte, prim, f) ? f.cantidad : 0;
}

bool W3dMallaParteCaja(MallaRecurso* r, int parte, float mn[3], float mx[3]) {
    if (!r || r->facesSize <= 0 || ((!r->faces || !r->vertex) && !r->soloGPU)) return false;
    int ini, cant; size_t slot;
    Rango(r, parte, ini, cant, slot);
    CacheFlujos* c = Cache(r);
    float* k = &c->cajas[slot * 6];
    if (!c->cajaLista[slot]) {
        if (!r->faces || !r->vertex) return false;   // (soloGPU sin la caja cacheada: W3dMallaFlujosPrecalcular la deja)
        for (int e = 0; e < 3; e++) { k[e] = 1e30f; k[3 + e] = -1e30f; }
        for (int i = ini; i < ini + cant; i++) {
            const float* v = &r->vertex[(size_t)r->faces[i] * 3];
            for (int e = 0; e < 3; e++) { if (v[e] < k[e]) k[e] = v[e]; if (v[e] > k[3 + e]) k[3 + e] = v[e]; }
        }
        c->cajaLista[slot] = 1;
    }
    for (int e = 0; e < 3; e++) { mn[e] = k[e]; mx[e] = k[3 + e]; }
    return true;
}

int W3dMallaParteMaxIndice(MallaRecurso* r, int parte) {
    if (!r || r->facesSize <= 0 || (!r->faces && !r->soloGPU)) return -1;
    int ini, cant; size_t slot;
    Rango(r, parte, ini, cant, slot);
    CacheFlujos* c = Cache(r);
    if (c->maxIdx[slot] == -2) {
        if (!r->faces) return -1;   // (soloGPU sin el indice cacheado: W3dMallaFlujosPrecalcular lo deja)
        int m = -1;
        for (int i = ini; i < ini + cant; i++) if ((int)r->faces[i] > m) m = (int)r->faces[i];
        c->maxIdx[slot] = m;
    }
    return c->maxIdx[slot];
}

bool W3dMallaFlujoListo(MallaRecurso* r, int parte, int prim) {
    if (!r || r->facesSize <= 0) return false;
    const int clase = ClaseDe(prim);
    if (clase < 0) return r->faces || r->vboIdx;   // el index buffer tal cual
    CacheFlujos* c = (CacheFlujos*)r->flujos;
    if (!c || c->version != r->version || c->fuente != r->faces || c->nIndices != r->facesSize ||
        c->nPartes != r->partes.size()) return false;
    int ini, cant; size_t slot;
    Rango(r, parte, ini, cant, slot);
    return c->f[slot * kClases + (size_t)clase].listo;
}

void W3dMallaFlujosPrecalcular(MallaRecurso* r) {
    if (!r || !r->faces || !r->vertex || r->facesSize <= 0) return;
    float mn[3], mx[3];
    for (int k = -1; k < (int)r->partes.size(); k++) {
        W3dMallaParteCaja(r, k, mn, mx);
        W3dMallaParteMaxIndice(r, k);
    }
}

void W3dMallaFlujosReapuntar(MallaRecurso* r) {
    CacheFlujos* c = r ? (CacheFlujos*)r->flujos : 0;
    // (solo si es el cache de ESTA version: uno viejo lo tira Cache() en el proximo uso)
    if (c && c->version == r->version && c->nIndices == r->facesSize && c->nPartes == r->partes.size()) c->fuente = r->faces;
}

void W3dMallaFlujosGLPerdido(MallaRecurso* r) {
    CacheFlujos* c = r ? (CacheFlujos*)r->flujos : 0;
    if (!c) return;
    for (size_t i = 0; i < c->f.size(); i++)
        if (c->f[i].ibo) { c->f[i].ibo = 0; c->f[i].iboPerdido = true; }
}
