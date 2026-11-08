#include "W3dNav.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

W3dNav::W3dNav()
    : x0(0), y0(0), celda(16), paso(18), radio(0), nx(0), ny(0), capas(0), z(0), bloqueo(0), nBloqueos(0), maxNodos(40000),
      g(0), padre(0), sello(0), selloActual(0), monton(0), montonF(0), capMonton(0) {
    memset(prendido, 0, sizeof(prendido));
}

namespace {
inline int nodo(const W3dNav& n, int x, int y, int c) { return (y * n.nx + x) * n.capas + c; }

inline bool pisable(const W3dNav& n, int id) {
    if (n.z[id] == W3DNAV_NADA) return false;
    const uint8_t b = n.bloqueo[id];
    return !b || !n.prendido[b];
}

// la capa de la celda (x, y) con el piso a menos de 'tope' de z (la mas cerca); -1 si no hay
int capa_cerca(const W3dNav& n, int x, int y, float z, float arriba, float abajo) {
    if (x < 0 || y < 0 || x >= n.nx || y >= n.ny) return -1;
    int mejor = -1;
    float md = 1e30f;
    for (int c = 0; c < n.capas; c++) {
        const int id = nodo(n, x, y, c);
        if (!pisable(n, id)) continue;
        const float d = (float)n.z[id] - z;
        if (d > arriba || d < -abajo) continue;
        const float a = d < 0 ? -d : d;
        if (a < md) { md = a; mejor = c; }
    }
    return mejor;
}

void celda_de(const W3dNav& n, const float* p, int& x, int& y) {
    x = (int)floorf((p[0] - n.x0) / n.celda);
    y = (int)floorf((p[1] - n.y0) / n.celda);
}

// el monticulo (los abiertos del A*, por f)
void subir(W3dNav& n, int i) {
    while (i > 0) {
        const int p = (i - 1) / 2;
        if (n.montonF[p] <= n.montonF[i]) break;
        int t = n.monton[p]; n.monton[p] = n.monton[i]; n.monton[i] = t;
        float f = n.montonF[p]; n.montonF[p] = n.montonF[i]; n.montonF[i] = f;
        i = p;
    }
}

int sacar(W3dNav& n, int& tam) {
    const int r = n.monton[0];
    tam--;
    n.monton[0] = n.monton[tam];
    n.montonF[0] = n.montonF[tam];
    int i = 0;
    for (;;) {
        const int a = i * 2 + 1, b = a + 1;
        int m = i;
        if (a < tam && n.montonF[a] < n.montonF[m]) m = a;
        if (b < tam && n.montonF[b] < n.montonF[m]) m = b;
        if (m == i) break;
        int t = n.monton[m]; n.monton[m] = n.monton[i]; n.monton[i] = t;
        float f = n.montonF[m]; n.montonF[m] = n.montonF[i]; n.montonF[i] = f;
        i = m;
    }
    return r;
}

void centro(const W3dNav& n, int id, float* p) {
    const int c = id % n.capas, xy = id / n.capas;
    p[0] = n.x0 + ((xy % n.nx) + 0.5f) * n.celda;
    p[1] = n.y0 + ((xy / n.nx) + 0.5f) * n.celda;
    p[2] = (float)n.z[nodo(n, xy % n.nx, xy / n.nx, c)];
}
}

bool W3dNavArmar(W3dNav& n, const void* datos, uint32_t largo) {
    W3dNavSoltar(n);
    const unsigned char* d = (const unsigned char*)datos;
    if (!d || largo < 32 || memcmp(d, "NAV1", 4) != 0) return false;
    float f[4];
    int32_t e[3];
    memcpy(f, d + 4, 16);
    memcpy(e, d + 20, 12);
    if (e[0] <= 0 || e[1] <= 0 || e[2] <= 0 || e[2] > 4) return false;
    const uint32_t total = (uint32_t)e[0] * (uint32_t)e[1] * (uint32_t)e[2];
    if (largo < 32 + total * 2) return false;
    n.x0 = f[0]; n.y0 = f[1]; n.celda = f[2]; n.paso = f[3];
    n.nx = e[0]; n.ny = e[1]; n.capas = e[2];
    n.z = (int16_t*)malloc(total * sizeof(int16_t));
    n.bloqueo = (uint8_t*)calloc(total, 1);
    n.g = (float*)malloc(total * sizeof(float));
    n.padre = (int*)malloc(total * sizeof(int));
    n.sello = (uint32_t*)calloc(total, sizeof(uint32_t));
    n.capMonton = (int)(total < 65536 ? total : 65536);
    n.monton = (int*)malloc(n.capMonton * sizeof(int));
    n.montonF = (float*)malloc(n.capMonton * sizeof(float));
    if (!n.z || !n.bloqueo || !n.g || !n.padre || !n.sello || !n.monton || !n.montonF) { W3dNavSoltar(n); return false; }
    memcpy(n.z, d + 32, total * 2);
    return true;
}

void W3dNavSoltar(W3dNav& n) {
    free(n.z); free(n.bloqueo); free(n.g); free(n.padre); free(n.sello); free(n.monton); free(n.montonF);
    n.z = 0; n.bloqueo = 0; n.g = 0; n.padre = 0; n.sello = 0; n.monton = 0; n.montonF = 0;
    n.nx = n.ny = n.capas = 0;
    n.nBloqueos = 0;
    memset(n.prendido, 0, sizeof(n.prendido));
}

bool W3dNavSuelo(const W3dNav& n, const float* p, float* z) {
    if (!n.z) return false;
    int x, y;
    celda_de(n, p, x, y);
    const int c = capa_cerca(n, x, y, p[2], n.paso, n.paso * 4);
    if (c < 0) return false;
    *z = (float)n.z[nodo(n, x, y, c)];
    return true;
}

bool W3dNavMasCerca(const W3dNav& n, const float* p, float radio, float* out) {
    if (!n.z) return false;
    int x, y;
    celda_de(n, p, x, y);
    const int r = (int)(radio / n.celda) + 1;
    float md = 1e30f;
    bool hay = false;
    for (int j = y - r; j <= y + r; j++)
        for (int i = x - r; i <= x + r; i++) {
            const int c = capa_cerca(n, i, j, p[2], n.paso * 2, n.paso * 4);
            if (c < 0) continue;
            float q[3];
            centro(n, nodo(n, i, j, c), q);
            const float dx = q[0] - p[0], dy = q[1] - p[1], dz = (q[2] - p[2]) * 0.5f;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < md && d <= radio * radio) { md = d; hay = true; out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; }
        }
    return hay;
}

bool W3dNavDirecto(const W3dNav& n, const float* a, const float* b) {
    if (!n.z) return false;
    const float dx = b[0] - a[0], dy = b[1] - a[1];
    const float l = sqrtf(dx * dx + dy * dy);
    const int pasos = (int)(l / (n.celda * 0.35f)) + 1;
    // (con 'radio': los dos costados de la linea a esa distancia tambien se pisan; el que camina no roza las esquinas)
    const float px = l > 1e-6f ? -dy / l * n.radio : 0, py = l > 1e-6f ? dx / l * n.radio : 0;
    float z = a[2];
    int ux = -1, uy = -1;
    for (int k = 0; k <= pasos; k++) {
        const float t = (float)k / (float)pasos;
        const float p[3] = { a[0] + dx * t, a[1] + dy * t, z };
        int x, y;
        celda_de(n, p, x, y);
        if (x == ux && y == uy) continue;
        // (cada celda nueva tiene que tener un piso a un escalon del anterior; y el ancho de la persona: las de los
        // costados tambien, asi la linea no roza una pared)
        const int c = capa_cerca(n, x, y, z, n.paso, n.paso);
        if (c < 0) return false;
        z = (float)n.z[nodo(n, x, y, c)];
        if (n.radio > 0) {
            for (int lado = -1; lado <= 1; lado += 2) {
                const float q[3] = { p[0] + px * lado, p[1] + py * lado, z };
                int qx, qy;
                celda_de(n, q, qx, qy);
                if ((qx != x || qy != y) && capa_cerca(n, qx, qy, z, n.paso, n.paso) < 0) return false;
            }
        }
        if (ux >= 0 && x != ux && y != uy) {   // (en diagonal: las dos de los costados)
            if (capa_cerca(n, ux, y, z, n.paso, n.paso) < 0 || capa_cerca(n, x, uy, z, n.paso, n.paso) < 0) return false;
        }
        ux = x;
        uy = y;
    }
    return true;
}

int W3dNavCamino(W3dNav& n, const float* desde, const float* hasta, float* out, int max, bool* llega) {
    if (llega) *llega = false;
    if (!n.z || max < 1) return 0;
    float a[3], b[3];
    if (!W3dNavMasCerca(n, desde, n.celda * 4, a)) return 0;
    const bool hayB = W3dNavMasCerca(n, hasta, n.celda * 4, b);
    if (!hayB) { b[0] = hasta[0]; b[1] = hasta[1]; b[2] = hasta[2]; }
    int ax, ay, bx, by;
    celda_de(n, a, ax, ay);
    celda_de(n, b, bx, by);
    const int inicio = nodo(n, ax, ay, capa_cerca(n, ax, ay, a[2], 1, 1));
    const int fin = hayB ? nodo(n, bx, by, capa_cerca(n, bx, by, b[2], 1, 1)) : -1;
    // A*
    if (++n.selloActual == 0) { memset(n.sello, 0, sizeof(uint32_t) * n.nx * n.ny * n.capas); n.selloActual = 1; }
    const uint32_t S = n.selloActual;
    int tam = 0;
    n.g[inicio] = 0;
    n.padre[inicio] = -1;
    n.sello[inicio] = S;
    n.monton[tam] = inicio;
    n.montonF[tam] = 0;
    tam++;
    int mejor = inicio;
    float mejorH = 1e30f;
    int revisados = 0;
    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    const float RAIZ2 = 1.41421356f;
    while (tam > 0 && revisados < n.maxNodos) {
        const int c = sacar(n, tam);
        revisados++;
        if (c == fin) { mejor = c; mejorH = 0; break; }
        const int capa = c % n.capas, xy = c / n.capas, x = xy % n.nx, y = xy / n.nx;
        const float zc = (float)n.z[c];
        {   // (la revisada mas cerca del destino: si no se llega, hasta ahi)
            const float hx = (float)(x - bx), hy = (float)(y - by);
            const float h = hx * hx + hy * hy;
            if (h < mejorH) { mejorH = h; mejor = c; }
        }
        (void)capa;
        for (int k = 0; k < 8; k++) {
            const int x2 = x + DX[k], y2 = y + DY[k];
            const int c2 = capa_cerca(n, x2, y2, zc, n.paso, n.paso * 2);
            if (c2 < 0) continue;
            if (k >= 4 && (capa_cerca(n, x2, y, zc, n.paso, n.paso * 2) < 0 || capa_cerca(n, x, y2, zc, n.paso, n.paso * 2) < 0))
                continue;
            const int v = nodo(n, x2, y2, c2);
            const float ng = n.g[c] + (k >= 4 ? RAIZ2 : 1.0f) * n.celda;
            if (n.sello[v] == S && n.g[v] <= ng) continue;
            n.sello[v] = S;
            n.g[v] = ng;
            n.padre[v] = c;
            const float hx = fabsf((float)(x2 - bx)), hy = fabsf((float)(y2 - by));
            const float h = ((hx > hy ? hx - hy : hy - hx) + RAIZ2 * (hx < hy ? hx : hy)) * n.celda;
            if (tam < n.capMonton) {
                n.monton[tam] = v;
                n.montonF[tam] = ng + h;
                subir(n, tam);
                tam++;
            }
        }
    }
    if (llega) *llega = mejor == fin;
    // el camino de celdas (al reves) y el tensado: desde cada punto, el mas lejano que se ve derecho
    int nCel = 0;
    for (int c = mejor; c >= 0; c = n.padre[c]) nCel++;
    int* cel = (int*)malloc(sizeof(int) * (nCel > 0 ? nCel : 1));
    if (!cel) return 0;
    {
        int k = nCel;
        for (int c = mejor; c >= 0; c = n.padre[c]) cel[--k] = c;
    }
    float pt[3], q[3];
    memcpy(pt, desde, sizeof(pt));
    pt[2] = a[2];
    int nOut = 0, i = 0;
    bool completo = nCel <= 1;
    while (i < nCel - 1 && nOut < max - 1) {
        int j = nCel - 1;
        for (; j > i + 1; j--) {
            centro(n, cel[j], q);
            if (W3dNavDirecto(n, pt, q)) break;
        }
        centro(n, cel[j], q);
        if (j == nCel - 1) { completo = true; break; }
        out[nOut * 3] = q[0]; out[nOut * 3 + 1] = q[1]; out[nOut * 3 + 2] = q[2];
        nOut++;
        memcpy(pt, q, sizeof(pt));
        i = j;
    }
    if (!completo) {
        // (no entraron todas las esquinas en 'max': el ultimo punto es la que sigue, no el destino -derecho hasta alla
        // se cruzaria lo que haya en el medio-; quien llama vuelve a pedir el camino desde ahi)
        int j = nCel - 1;
        for (; j > i + 1; j--) {
            centro(n, cel[j], q);
            if (W3dNavDirecto(n, pt, q)) break;
        }
        centro(n, cel[j], out + nOut * 3);
        free(cel);
        if (llega) *llega = false;
        return nOut + 1;
    }
    free(cel);
    // el final: el destino si se llego, si no el centro de la celda mas cerca
    if (mejor == fin && hayB) { out[nOut * 3] = hasta[0]; out[nOut * 3 + 1] = hasta[1]; out[nOut * 3 + 2] = b[2]; }
    else centro(n, mejor, out + nOut * 3);
    return nOut + 1;
}

int W3dNavBloqueoCaja(W3dNav& n, const float* mn, const float* mx) {
    if (!n.z || n.nBloqueos >= 255) return 0;
    const int id = ++n.nBloqueos;
    int x0, y0, x1, y1;
    celda_de(n, mn, x0, y0);
    celda_de(n, mx, x1, y1);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < n.ny; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < n.nx; x++)
            for (int c = 0; c < n.capas; c++) {
                const int i = nodo(n, x, y, c);
                if (n.z[i] != W3DNAV_NADA && n.z[i] >= mn[2] - n.paso && n.z[i] <= mx[2]) n.bloqueo[i] = (uint8_t)id;
            }
    return id;
}

void W3dNavBloquear(W3dNav& n, int bloqueo, bool si) {
    if (bloqueo > 0 && bloqueo < 256) n.prendido[bloqueo] = si;
}
