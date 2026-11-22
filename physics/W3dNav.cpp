#include "W3dNav.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

W3dNav::W3dNav()
    : x0(0), y0(0), cell(16), step(18), radius(0), nx(0), ny(0), layers(0), z(0), lock(0), nLocks(0), maxNodes(40000),
      g(0), parent(0), stamp(0), currentStamp(0), heap(0), heapF(0), capHeap(0) {
    memset(on, 0, sizeof(on));
}

namespace {
inline int node(const W3dNav& n, int x, int y, int c) { return (y * n.nx + x) * n.layers + c; }

inline bool pisable(const W3dNav& n, int id) {
    if (n.z[id] == W3DNAV_NOTHING) return false;
    const uint8_t b = n.lock[id];
    return !b || !n.on[b];
}

// la capa de la celda (x, y) con el piso a menos de 'tope' de z (la mas cerca); -1 si no hay
int layer_near(const W3dNav& n, int x, int y, float z, float up, float down) {
    if (x < 0 || y < 0 || x >= n.nx || y >= n.ny) return -1;
    int best = -1;
    float md = 1e30f;
    for (int c = 0; c < n.layers; c++) {
        const int id = node(n, x, y, c);
        if (!pisable(n, id)) continue;
        const float d = (float)n.z[id] - z;
        if (d > up || d < -down) continue;
        const float a = d < 0 ? -d : d;
        if (a < md) { md = a; best = c; }
    }
    return best;
}

void cell_of(const W3dNav& n, const float* p, int& x, int& y) {
    x = (int)floorf((p[0] - n.x0) / n.cell);
    y = (int)floorf((p[1] - n.y0) / n.cell);
}

// el monticulo (los abiertos del A*, por f)
void upload(W3dNav& n, int i) {
    while (i > 0) {
        const int p = (i - 1) / 2;
        if (n.heapF[p] <= n.heapF[i]) break;
        int t = n.heap[p]; n.heap[p] = n.heap[i]; n.heap[i] = t;
        float f = n.heapF[p]; n.heapF[p] = n.heapF[i]; n.heapF[i] = f;
        i = p;
    }
}

int remove(W3dNav& n, int& size) {
    const int r = n.heap[0];
    size--;
    n.heap[0] = n.heap[size];
    n.heapF[0] = n.heapF[size];
    int i = 0;
    for (;;) {
        const int a = i * 2 + 1, b = a + 1;
        int m = i;
        if (a < size && n.heapF[a] < n.heapF[m]) m = a;
        if (b < size && n.heapF[b] < n.heapF[m]) m = b;
        if (m == i) break;
        int t = n.heap[m]; n.heap[m] = n.heap[i]; n.heap[i] = t;
        float f = n.heapF[m]; n.heapF[m] = n.heapF[i]; n.heapF[i] = f;
        i = m;
    }
    return r;
}

void center(const W3dNav& n, int id, float* p) {
    const int c = id % n.layers, xy = id / n.layers;
    p[0] = n.x0 + ((xy % n.nx) + 0.5f) * n.cell;
    p[1] = n.y0 + ((xy / n.nx) + 0.5f) * n.cell;
    p[2] = (float)n.z[node(n, xy % n.nx, xy / n.nx, c)];
}
}

bool W3dNavBuild(W3dNav& n, const void* data, uint32_t length) {
    W3dNavRelease(n);
    const unsigned char* d = (const unsigned char*)data;
    if (!d || length < 32 || memcmp(d, "NAV1", 4) != 0) return false;
    float f[4];
    int32_t e[3];
    memcpy(f, d + 4, 16);
    memcpy(e, d + 20, 12);
    if (e[0] <= 0 || e[1] <= 0 || e[2] <= 0 || e[2] > 4) return false;
    const uint32_t total = (uint32_t)e[0] * (uint32_t)e[1] * (uint32_t)e[2];
    if (length < 32 + total * 2) return false;
    n.x0 = f[0]; n.y0 = f[1]; n.cell = f[2]; n.step = f[3];
    n.nx = e[0]; n.ny = e[1]; n.layers = e[2];
    n.z = (int16_t*)malloc(total * sizeof(int16_t));
    n.lock = (uint8_t*)calloc(total, 1);
    n.g = (float*)malloc(total * sizeof(float));
    n.parent = (int*)malloc(total * sizeof(int));
    n.stamp  = (uint32_t*)calloc(total, sizeof(uint32_t));
    n.capHeap = (int)(total < 65536 ? total : 65536);
    n.heap = (int*)malloc(n.capHeap * sizeof(int));
    n.heapF = (float*)malloc(n.capHeap * sizeof(float));
    if (!n.z || !n.lock || !n.g || !n.parent || !n.stamp || !n.heap || !n.heapF) { W3dNavRelease(n); return false; }
    memcpy(n.z, d + 32, total * 2);
    return true;
}

void W3dNavRelease(W3dNav& n) {
    free(n.z); free(n.lock); free(n.g); free(n.parent); free(n.stamp); free(n.heap); free(n.heapF);
    n.z = 0; n.lock = 0; n.g = 0; n.parent = 0; n.stamp = 0; n.heap = 0; n.heapF = 0;
    n.nx = n.ny = n.layers = 0;
    n.nLocks = 0;
    memset(n.on, 0, sizeof(n.on));
}

bool W3dNavGround(const W3dNav& n, const float* p, float* z) {
    if (!n.z) return false;
    int x, y;
    cell_of(n, p, x, y);
    const int c = layer_near(n, x, y, p[2], n.step, n.step * 4);
    if (c < 0) return false;
    *z = (float)n.z[node(n, x, y, c)];
    return true;
}

bool W3dNavNearest(const W3dNav& n, const float* p, float radius, float* out) {
    if (!n.z) return false;
    int x, y;
    cell_of(n, p, x, y);
    const int r = (int)(radius / n.cell) + 1;
    float md = 1e30f;
    bool has = false;
    for (int j = y - r; j <= y + r; j++)
        for (int i = x - r; i <= x + r; i++) {
            const int c = layer_near(n, i, j, p[2], n.step * 2, n.step * 4);
            if (c < 0) continue;
            float q[3];
            center(n, node(n, i, j, c), q);
            const float dx = q[0] - p[0], dy = q[1] - p[1], dz = (q[2] - p[2]) * 0.5f;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < md && d <= radius * radius) { md = d; has = true; out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; }
        }
    return has;
}

bool W3dNavDirect(const W3dNav& n, const float* a, const float* b) {
    if (!n.z) return false;
    const float dx = b[0] - a[0], dy = b[1] - a[1];
    const float l = sqrtf(dx * dx + dy * dy);
    const int steps = (int)(l / (n.cell * 0.35f)) + 1;
    // (con 'radio': los dos costados de la linea a esa distancia tambien se pisan; el que camina no roza las esquinas)
    const float px = l > 1e-6f ? -dy / l * n.radius : 0, py = l > 1e-6f ? dx / l * n.radius : 0;
    float z = a[2];
    int ux = -1, uy = -1;
    for (int k = 0; k <= steps; k++) {
        const float t = (float)k / (float)steps;
        const float p[3] = { a[0] + dx * t, a[1] + dy * t, z };
        int x, y;
        cell_of(n, p, x, y);
        if (x == ux && y == uy) continue;
        // (cada celda nueva tiene que tener un piso a un escalon del anterior; y el ancho de la persona: las de los
        // costados tambien, asi la linea no roza una pared)
        const int c = layer_near(n, x, y, z, n.step, n.step);
        if (c < 0) return false;
        z = (float)n.z[node(n, x, y, c)];
        if (n.radius > 0) {
            for (int side = -1; side <= 1; side += 2) {
                const float q[3] = { p[0] + px * side, p[1] + py * side, z };
                int qx, qy;
                cell_of(n, q, qx, qy);
                if ((qx != x || qy != y) && layer_near(n, qx, qy, z, n.step, n.step) < 0) return false;
            }
        }
        if (ux >= 0 && x != ux && y != uy) {   // (en diagonal: las dos de los costados)
            if (layer_near(n, ux, y, z, n.step, n.step) < 0 || layer_near(n, x, uy, z, n.step, n.step) < 0) return false;
        }
        ux = x;
        uy = y;
    }
    return true;
}

int W3dNavPath(W3dNav& n, const float* from, const float* until, float* out, int max, bool* arrives) {
    if (arrives) *arrives = false;
    if (!n.z || max < 1) return 0;
    float a[3], b[3];
    if (!W3dNavNearest(n, from, n.cell * 4, a)) return 0;
    const bool hasB = W3dNavNearest(n, until, n.cell * 4, b);
    if (!hasB) { b[0] = until[0]; b[1] = until[1]; b[2] = until[2]; }
    int ax, ay, bx, by;
    cell_of(n, a, ax, ay);
    cell_of(n, b, bx, by);
    const int start = node(n, ax, ay, layer_near(n, ax, ay, a[2], 1, 1));
    const int end = hasB ? node(n, bx, by, layer_near(n, bx, by, b[2], 1, 1)) : -1;
    // A*
    if (++n.currentStamp == 0) { memset(n.stamp, 0, sizeof(uint32_t) * n.nx * n.ny * n.layers); n.currentStamp = 1; }
    const uint32_t S = n.currentStamp;
    int size = 0;
    n.g[start] = 0;
    n.parent[start] = -1;
    n.stamp[start]  = S;
    n.heap[size] = start;
    n.heapF[size] = 0;
    size++;
    int best = start;
    float bestH = 1e30f;
    int checked = 0;
    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    const float ROOT2 = 1.41421356f;
    while (size > 0 && checked < n.maxNodes) {
        const int c = remove(n, size);
        checked++;
        if (c == end) { best = c; bestH = 0; break; }
        const int layer = c % n.layers, xy = c / n.layers, x = xy % n.nx, y = xy / n.nx;
        const float zc  = (float)n.z[c];
        {   // (la revisada mas cerca del destino: si no se llega, hasta ahi)
            const float hx = (float)(x - bx), hy = (float)(y - by);
            const float h = hx * hx + hy * hy;
            if (h < bestH) { bestH = h; best = c; }
        }
        (void)layer;
        for (int k = 0; k < 8; k++) {
            const int x2 = x + DX[k], y2 = y + DY[k];
            const int c2 = layer_near(n, x2, y2, zc, n.step, n.step * 2);
            if (c2 < 0) continue;
            if (k >= 4 && (layer_near(n, x2, y, zc, n.step, n.step * 2) < 0 || layer_near(n, x, y2, zc, n.step, n.step * 2) < 0))
                continue;
            const int v = node(n, x2, y2, c2);
            const float ng = n.g[c] + (k >= 4 ? ROOT2 : 1.0f) * n.cell;
            if (n.stamp[v] == S && n.g[v] <= ng) continue;
            n.stamp[v] = S;
            n.g[v] = ng;
            n.parent[v] = c;
            const float hx = fabsf((float)(x2 - bx)), hy = fabsf((float)(y2 - by));
            const float h = ((hx > hy ? hx - hy : hy - hx) + ROOT2 * (hx < hy ? hx : hy)) * n.cell;
            if (size < n.capHeap) {
                n.heap[size] = v;
                n.heapF[size] = ng + h;
                upload(n, size);
                size++;
            }
        }
    }
    if (arrives) *arrives = best == end;
    // el camino de celdas (al reves) y el tensado: desde cada punto, el mas lejano que se ve derecho
    int nCel = 0;
    for (int c = best; c >= 0; c = n.parent[c]) nCel++;
    int* cel = (int*)malloc(sizeof(int) * (nCel > 0 ? nCel : 1));
    if (!cel) return 0;
    {
        int k = nCel;
        for (int c = best; c >= 0; c = n.parent[c]) cel[--k] = c;
    }
    float pt[3], q[3];
    memcpy(pt, from, sizeof(pt));
    pt[2] = a[2];
    int nOut = 0, i = 0;
    bool complete = nCel <= 1;
    while (i < nCel - 1 && nOut < max - 1) {
        int j = nCel - 1;
        for (; j > i + 1; j--) {
            center(n, cel[j], q);
            if (W3dNavDirect(n, pt, q)) break;
        }
        center(n, cel[j], q);
        if (j == nCel - 1) { complete = true; break; }
        out[nOut * 3] = q[0]; out[nOut * 3 + 1] = q[1]; out[nOut * 3 + 2] = q[2];
        nOut++;
        memcpy(pt, q, sizeof(pt));
        i = j;
    }
    if (!complete) {
        // (no entraron todas las esquinas en 'max': el ultimo punto es la que sigue, no el destino -derecho hasta alla
        // se cruzaria lo que haya en el medio-; quien llama vuelve a pedir el camino desde ahi)
        int j = nCel - 1;
        for (; j > i + 1; j--) {
            center(n, cel[j], q);
            if (W3dNavDirect(n, pt, q)) break;
        }
        center(n, cel[j], out + nOut * 3);
        free(cel);
        if (arrives) *arrives = false;
        return nOut + 1;
    }
    free(cel);
    // el final: el destino si se llego, si no el centro de la celda mas cerca
    if (best == end && hasB) { out[nOut * 3] = until[0]; out[nOut * 3 + 1] = until[1]; out[nOut * 3 + 2] = b[2]; }
    else center(n, best, out + nOut * 3);
    return nOut + 1;
}

int W3dNavLockBox(W3dNav& n, const float* mn, const float* mx) {
    if (!n.z || n.nLocks >= 255) return 0;
    const int id = ++n.nLocks;
    int x0, y0, x1, y1;
    cell_of(n, mn, x0, y0);
    cell_of(n, mx, x1, y1);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < n.ny; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < n.nx; x++)
            for (int c = 0; c < n.layers; c++) {
                const int i = node(n, x, y, c);
                if (n.z[i] != W3DNAV_NOTHING && n.z[i] >= mn[2] - n.step && n.z[i] <= mx[2]) n.lock[i] = (uint8_t)id;
            }
    return id;
}

void W3dNavLock(W3dNav& n, int lock, bool yes) {
    if (lock > 0 && lock < 256) n.on[lock] = yes;
}
