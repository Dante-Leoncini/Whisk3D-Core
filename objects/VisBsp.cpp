#include "VisBsp.h"
#include <cstring>
#include <stdio.h>   // snprintf (el aviso de version)

// ---- lectura LE sin alineacion ----
static unsigned LeeU32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static float LeeF32(const unsigned char* p) { unsigned u = LeeU32(p); float f; memcpy(&f, &u, 4); return f; }

void VisBsp::Clear() {
    nodes.clear(); leafCluster.clear(); pvsOff.clear(); pvs.clear(); children.clear();
    nClusters = 0; bytesRow = 0; rle = false; version = 0; geomOff = 0;
}

bool VisBsp::Load(const unsigned char* d, size_t n, std::string* err) {
    Clear();
    if (n < 24 || memcmp(d, "W3BS", 4) != 0) { if (err) *err = "no es un .w3dbsp"; return false; }
    const unsigned version = (unsigned)d[4] | ((unsigned)d[5] << 8);
    const unsigned flags   = (unsigned)d[6] | ((unsigned)d[7] << 8);
    if (version != 1 && version != 2) {
        // (con el numero: un binario viejo que no lee la 2 lo dice en el log y se sabe que hay que recompilar)
        if (err) { char b[96]; snprintf(b, sizeof b, "version de .w3dbsp desconocida: %u (este motor lee 1 y 2)", version); *err = b; }
        return false;
    }
    this->version = version;
    const unsigned nNodes = LeeU32(d + 8), nLeaves = LeeU32(d + 12), nCl = LeeU32(d + 16), nChildren = LeeU32(d + 20);
    if (nNodes > 4000000u || nLeaves > 4000000u || nCl > 1000000u || nChildren > 1000000u) { if (err) *err = "cabecera absurda"; return false; }
    size_t p = 24;
    if (n < p + (size_t)nNodes * 24 + (size_t)nLeaves * 4) { if (err) *err = "cortado en nodos/hojas"; return false; }
    nodes.resize(nNodes);
    for (unsigned i = 0; i < nNodes; i++, p += 24) {
        Node& nd = nodes[i];
        nd.a = LeeF32(d + p); nd.b = LeeF32(d + p + 4); nd.c = LeeF32(d + p + 8); nd.d = LeeF32(d + p + 12);
        nd.child0 = (int)LeeU32(d + p + 16); nd.child1 = (int)LeeU32(d + p + 20);
        // un hijo fuera de rango = archivo roto (sino la bajada por el arbol se sale de los arrays)
        if ((nd.child0 >= 0 && (unsigned)nd.child0 >= nNodes) || (nd.child0 < 0 && (unsigned)(-nd.child0 - 1) >= nLeaves) ||
            (nd.child1 >= 0 && (unsigned)nd.child1 >= nNodes) || (nd.child1 < 0 && (unsigned)(-nd.child1 - 1) >= nLeaves)) {
            if (err) *err = "nodo con hijo fuera de rango"; Clear(); return false;
        }
    }
    leafCluster.resize(nLeaves);
    for (unsigned i = 0; i < nLeaves; i++, p += 4) leafCluster[i] = (int)LeeU32(d + p);
    nClusters = (int)nCl;
    bytesRow  = (nClusters + 7) / 8;
    rle = (flags & 1u) != 0;
    if (n < p + 4) { if (err) *err = "cortado en el PVS"; Clear(); return false; }
    const unsigned streamSize = LeeU32(d + p); p += 4;
    if (n < p + (size_t)nCl * 4 + streamSize) { if (err) *err = "cortado en el PVS"; Clear(); return false; }
    pvsOff.resize(nCl);
    for (unsigned i = 0; i < nCl; i++, p += 4) {
        pvsOff[i] = LeeU32(d + p);
        if (pvsOff[i] > streamSize) { if (err) *err = "offset del PVS fuera del stream"; Clear(); return false; }
    }
    pvs.assign(d + p, d + p + streamSize); p += streamSize;
    children.resize(nChildren);
    for (unsigned i = 0; i < nChildren; i++) {
        if (n < p + 4) { if (err) *err = "cortado en los hijos"; Clear(); return false; }
        const unsigned length = LeeU32(d + p); p += 4;
        if (n < p + length + 4) { if (err) *err = "cortado en los hijos"; Clear(); return false; }
        children[i].name.assign((const char*)d + p, length); p += length;
        const unsigned nb = LeeU32(d + p); p += 4;
        if (n < p + nb) { if (err) *err = "cortado en los hijos"; Clear(); return false; }
        children[i].bits.assign(d + p, d + p + nb); p += nb;
        children[i].bits.resize((size_t)bytesRow, 0);   // mismo largo que una fila (AND directo)
    }
    // version 2: lo que sigue es la geometria del mundo (WorldBsp la lee desde aca)
    if (version == 2 && p < n) geomOff = p;
    return true;
}

int VisBsp::LeafIn(float x, float y, float z) const {
    if (nodes.empty()) return -1;
    int i = 0;
    for (int step = 0; step < 100000 && i >= 0; step++) {   // (el tope es solo por si el archivo trae un ciclo)
        const Node& nd = nodes[(size_t)i];
        const float s = nd.a * x + nd.b * y + nd.c * z - nd.d;
        i = (s >= 0.0f) ? nd.child0 : nd.child1;
    }
    return (i < 0) ? (-i - 1) : -1;
}

int VisBsp::ClusterIn(float x, float y, float z) const {
    const int h = LeafIn(x, y, z);
    if (h < 0 || h >= (int)leafCluster.size()) return -1;
    const int c = leafCluster[(size_t)h];
    return (c >= 0 && c < nClusters) ? c : -1;
}

bool VisBsp::Row(int cluster, std::vector<unsigned char>& out) const {
    if (cluster < 0 || cluster >= nClusters) return false;
    out.assign((size_t)bytesRow, 0);
    size_t p = pvsOff[(size_t)cluster];
    if (!rle) {
        if (p + (size_t)bytesRow > pvs.size()) return false;
        memcpy(&out[0], &pvs[p], (size_t)bytesRow);
        return true;
    }
    size_t o = 0;
    while (o < (size_t)bytesRow && p < pvs.size()) {
        const unsigned char b = pvs[p++];
        if (b) { out[o++] = b; continue; }
        if (p >= pvs.size()) return false;
        unsigned count = pvs[p++];
        while (count-- && o < (size_t)bytesRow) out[o++] = 0;
    }
    return o == (size_t)bytesRow;
}

bool VisBsp::Intersects(const std::vector<unsigned char>& row, const std::vector<unsigned char>& bits) {
    const size_t n = row.size() < bits.size() ? row.size() : bits.size();
    for (size_t i = 0; i < n; i++) if (row[i] & bits[i]) return true;
    return false;
}

int VisBsp::ChildIndex(const std::string& name) const {
    for (size_t i = 0; i < children.size(); i++) if (children[i].name == name) return (int)i;
    return -1;
}

bool VisBsp::BoxClusters(const float* mn, const float* mx, std::vector<unsigned char>& bits) const {
    bits.assign((size_t)bytesRow, 0);
    if (nodes.empty() || nClusters <= 0) return false;
    // bajada con pila: en cada plano va por el lado del vertice mas positivo y, si la caja lo cruza, por los dos
    std::vector<int> stack;
    stack.reserve(64);
    stack.push_back(0);
    while (!stack.empty()) {
        const int i = stack.back(); stack.pop_back();
        if (i < 0) {
            const int h = -i - 1;
            if (h < (int)leafCluster.size()) {
                const int c = leafCluster[(size_t)h];
                if (c >= 0 && c < nClusters) bits[(size_t)c >> 3] |= (unsigned char)(1u << (c & 7));
            }
            continue;
        }
        const Node& nd = nodes[(size_t)i];
        const float sMax = nd.a * (nd.a >= 0 ? mx[0] : mn[0]) + nd.b * (nd.b >= 0 ? mx[1] : mn[1]) + nd.c * (nd.c >= 0 ? mx[2] : mn[2]) - nd.d;
        const float sMin = nd.a * (nd.a >= 0 ? mn[0] : mx[0]) + nd.b * (nd.b >= 0 ? mn[1] : mx[1]) + nd.c * (nd.c >= 0 ? mn[2] : mx[2]) - nd.d;
        if (sMax >= 0.0f) stack.push_back(nd.child0);
        if (sMin < 0.0f)  stack.push_back(nd.child1);
        if (stack.size() > 100000) break;   // (un archivo con ciclos)
    }
    return true;
}
