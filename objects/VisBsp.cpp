#include "VisBsp.h"
#include <cstring>

// ---- lectura LE sin alineacion ----
static unsigned LeeU32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static float LeeF32(const unsigned char* p) { unsigned u = LeeU32(p); float f; memcpy(&f, &u, 4); return f; }

void VisBsp::Limpiar() {
    nodos.clear(); hojaCluster.clear(); pvsOff.clear(); pvs.clear(); hijos.clear();
    nClusters = 0; bytesFila = 0; rle = false;
}

bool VisBsp::Cargar(const unsigned char* d, size_t n, std::string* err) {
    Limpiar();
    if (n < 24 || memcmp(d, "W3BS", 4) != 0) { if (err) *err = "no es un .w3dbsp"; return false; }
    const unsigned version = (unsigned)d[4] | ((unsigned)d[5] << 8);
    const unsigned flags   = (unsigned)d[6] | ((unsigned)d[7] << 8);
    if (version != 1) { if (err) *err = "version de .w3dbsp desconocida"; return false; }
    const unsigned nNodos = LeeU32(d + 8), nHojas = LeeU32(d + 12), nCl = LeeU32(d + 16), nHijos = LeeU32(d + 20);
    if (nNodos > 4000000u || nHojas > 4000000u || nCl > 1000000u || nHijos > 1000000u) { if (err) *err = "cabecera absurda"; return false; }
    size_t p = 24;
    if (n < p + (size_t)nNodos * 24 + (size_t)nHojas * 4) { if (err) *err = "cortado en nodos/hojas"; return false; }
    nodos.resize(nNodos);
    for (unsigned i = 0; i < nNodos; i++, p += 24) {
        Nodo& nd = nodos[i];
        nd.a = LeeF32(d + p); nd.b = LeeF32(d + p + 4); nd.c = LeeF32(d + p + 8); nd.d = LeeF32(d + p + 12);
        nd.hijo0 = (int)LeeU32(d + p + 16); nd.hijo1 = (int)LeeU32(d + p + 20);
        // un hijo fuera de rango = archivo roto (sino la bajada por el arbol se sale de los arrays)
        if ((nd.hijo0 >= 0 && (unsigned)nd.hijo0 >= nNodos) || (nd.hijo0 < 0 && (unsigned)(-nd.hijo0 - 1) >= nHojas) ||
            (nd.hijo1 >= 0 && (unsigned)nd.hijo1 >= nNodos) || (nd.hijo1 < 0 && (unsigned)(-nd.hijo1 - 1) >= nHojas)) {
            if (err) *err = "nodo con hijo fuera de rango"; Limpiar(); return false;
        }
    }
    hojaCluster.resize(nHojas);
    for (unsigned i = 0; i < nHojas; i++, p += 4) hojaCluster[i] = (int)LeeU32(d + p);
    nClusters = (int)nCl;
    bytesFila = (nClusters + 7) / 8;
    rle = (flags & 1u) != 0;
    if (n < p + 4) { if (err) *err = "cortado en el PVS"; Limpiar(); return false; }
    const unsigned tamStream = LeeU32(d + p); p += 4;
    if (n < p + (size_t)nCl * 4 + tamStream) { if (err) *err = "cortado en el PVS"; Limpiar(); return false; }
    pvsOff.resize(nCl);
    for (unsigned i = 0; i < nCl; i++, p += 4) {
        pvsOff[i] = LeeU32(d + p);
        if (pvsOff[i] > tamStream) { if (err) *err = "offset del PVS fuera del stream"; Limpiar(); return false; }
    }
    pvs.assign(d + p, d + p + tamStream); p += tamStream;
    hijos.resize(nHijos);
    for (unsigned i = 0; i < nHijos; i++) {
        if (n < p + 4) { if (err) *err = "cortado en los hijos"; Limpiar(); return false; }
        const unsigned largo = LeeU32(d + p); p += 4;
        if (n < p + largo + 4) { if (err) *err = "cortado en los hijos"; Limpiar(); return false; }
        hijos[i].nombre.assign((const char*)d + p, largo); p += largo;
        const unsigned nb = LeeU32(d + p); p += 4;
        if (n < p + nb) { if (err) *err = "cortado en los hijos"; Limpiar(); return false; }
        hijos[i].bits.assign(d + p, d + p + nb); p += nb;
        hijos[i].bits.resize((size_t)bytesFila, 0);   // mismo largo que una fila (AND directo)
    }
    return true;
}

int VisBsp::HojaEn(float x, float y, float z) const {
    if (nodos.empty()) return -1;
    int i = 0;
    for (int paso = 0; paso < 100000 && i >= 0; paso++) {   // (el tope es solo por si el archivo trae un ciclo)
        const Nodo& nd = nodos[(size_t)i];
        const float s = nd.a * x + nd.b * y + nd.c * z - nd.d;
        i = (s >= 0.0f) ? nd.hijo0 : nd.hijo1;
    }
    return (i < 0) ? (-i - 1) : -1;
}

int VisBsp::ClusterEn(float x, float y, float z) const {
    const int h = HojaEn(x, y, z);
    if (h < 0 || h >= (int)hojaCluster.size()) return -1;
    const int c = hojaCluster[(size_t)h];
    return (c >= 0 && c < nClusters) ? c : -1;
}

bool VisBsp::Fila(int cluster, std::vector<unsigned char>& out) const {
    if (cluster < 0 || cluster >= nClusters) return false;
    out.assign((size_t)bytesFila, 0);
    size_t p = pvsOff[(size_t)cluster];
    if (!rle) {
        if (p + (size_t)bytesFila > pvs.size()) return false;
        memcpy(&out[0], &pvs[p], (size_t)bytesFila);
        return true;
    }
    size_t o = 0;
    while (o < (size_t)bytesFila && p < pvs.size()) {
        const unsigned char b = pvs[p++];
        if (b) { out[o++] = b; continue; }
        if (p >= pvs.size()) return false;
        unsigned cuantos = pvs[p++];
        while (cuantos-- && o < (size_t)bytesFila) out[o++] = 0;
    }
    return o == (size_t)bytesFila;
}

bool VisBsp::Interseca(const std::vector<unsigned char>& fila, const std::vector<unsigned char>& bits) {
    const size_t n = fila.size() < bits.size() ? fila.size() : bits.size();
    for (size_t i = 0; i < n; i++) if (fila[i] & bits[i]) return true;
    return false;
}

int VisBsp::IndiceHijo(const std::string& nombre) const {
    for (size_t i = 0; i < hijos.size(); i++) if (hijos[i].nombre == nombre) return (int)i;
    return -1;
}
