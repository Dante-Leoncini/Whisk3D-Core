#include "objects/W3dCables.h"
#include "w3dGraphics.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace gfx = w3dEngine;

namespace {
enum { MAX_VERTS = 65535, MAX_RIBBONS = 16383 };   // (indices de 16 bits: 4 vertices por cinta)
const int UNO = 1024;                              // el punto fijo de los ejes de la camara

int32_t centimeters(float m) {
    const float c = m * 100.0f;
    return (int32_t)(c >= 0.0f ? c + 0.5f : c - 0.5f);
}

// agranda un arreglo (realloc) a 'cap' elementos de 'tam' bytes; false si no hay memoria
template <class T> bool grow(T*& p, int cap, int size) {
    void* q = realloc(p, (size_t)cap * (size_t)size);
    if (!q) return false;
    p = (T*)q;
    return true;
}

bool reserve_vertices(W3dCables& l, int n) {
    if (n <= l.capVerts) return true;
    int cap = l.capVerts ? l.capVerts : 256;
    while (cap < n) cap *= 2;
    if (cap > MAX_VERTS) cap = MAX_VERTS;
    if (!grow(l.pos, cap, 3 * sizeof(float)) || !grow(l.cm, cap, 3 * sizeof(int32_t)) ||
        !grow(l.color, cap, 4) || !grow(l.px, cap, sizeof(int32_t)) || !grow(l.outside, cap, 1) ||
        !grow(l.idxLines, cap, 2 * sizeof(unsigned short)))
        return false;
    // las cintas: una por tramo como mucho (y no mas de las que entran con indices de 16 bits)
    const int capC = cap < MAX_RIBBONS ? cap : MAX_RIBBONS;
    if (capC > l.capRibbons) {
        if (!grow(l.ribbon, capC, 12 * sizeof(float)) || !grow(l.colRibbon, capC, 16) ||
            !grow(l.idxRibbon, capC, 6 * sizeof(unsigned short)))
            return false;
        // (los indices de las cintas son siempre los mismos: dos triangulos por cada 4 vertices)
        for (int k = l.capRibbons; k < capC; k++) {
            unsigned short* q = l.idxRibbon + k * 6;
            const unsigned short b = (unsigned short)(k * 4);
            q[0] = b; q[1] = (unsigned short)(b + 1); q[2] = (unsigned short)(b + 2);
            q[3] = b; q[4] = (unsigned short)(b + 2); q[5] = (unsigned short)(b + 3);
        }
        l.capRibbons = capC;
    }
    l.capVerts = cap;
    return true;
}

bool reserve_cables(W3dCables& l, int n) {
    if (n <= l.capCables) return true;
    int cap = l.capCables ? l.capCables * 2 : 32;
    while (cap < n) cap *= 2;
    if (!grow(l.cables, cap, sizeof(W3dCable)) || !grow(l.hidden, cap, 1)) return false;
    l.capCables = cap;
    return true;
}

// el lado de la cinta en el vertice j: perpendicular al cable (su tangente en j) y a la linea de la camara, con la
// mitad del grosor
void side(const W3dCables& l, const W3dCable& c, int j, const float* cam, float* s) {
    const int a = j > c.first ? j - 1 : j;
    const int b = j < c.first + c.count - 1 ? j + 1 : j;
    const float* pa = l.pos + a * 3;
    const float* pb = l.pos + b * 3;
    const float* p = l.pos + j * 3;
    const float t[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
    const float v[3] = { p[0] - cam[0], p[1] - cam[1], p[2] - cam[2] };
    s[0] = t[1] * v[2] - t[2] * v[1];
    s[1] = t[2] * v[0] - t[0] * v[2];
    s[2] = t[0] * v[1] - t[1] * v[0];
    const float m = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
    const float k = m > 1e-12f ? c.thickness * 0.5f / sqrtf(m) : 0.0f;
    s[0] *= k; s[1] *= k; s[2] *= k;
}
}

W3dCables::W3dCables()
    : pos(0), cm(0), nVerts(0), capVerts(0), cables(0), hidden(0), nCables(0), capCables(0), color(0), px(0), outside(0),
      idxLines(0), ribbon(0), colRibbon(0), idxRibbon(0), capRibbons(0), limit(120.0f), floor(0.2f), thresholdRibbon(2.0f),
      highlight(0.5f), highlightTable(-1.0f), distHalf(30.0f) {
    memset(&last, 0, sizeof(last));
    memset(alphaTable, 0, sizeof(alphaTable));
}

int W3dCablesAdd(W3dCables& l, const float* points, int n, float thickness, const unsigned char* rgba) {
    if (!points || n < 2 || l.nVerts + n > MAX_VERTS) return -1;
    if (!reserve_vertices(l, l.nVerts + n) || !reserve_cables(l, l.nCables + 1)) return -1;
    W3dCable& c = l.cables[l.nCables];
    c.first = l.nVerts;
    c.count = n;
    c.thickness = thickness > 0.0f ? thickness : 0.0f;
    for (int e = 0; e < 4; e++) c.rgba[e] = rgba ? rgba[e] : (unsigned char)(e < 3 ? 0 : 255);
    for (int e = 0; e < 3; e++) c.mn[e] = c.mx[e] = points[e];
    for (int i = 0; i < n; i++) {
        const float* p = points + i * 3;
        for (int e = 0; e < 3; e++) {
            l.pos[(l.nVerts + i) * 3 + e] = p[e];
            l.cm[(l.nVerts + i) * 3 + e] = centimeters(p[e]);
            if (p[e] < c.mn[e]) c.mn[e] = p[e];
            if (p[e] > c.mx[e]) c.mx[e] = p[e];
        }
    }
    l.hidden[l.nCables] = 0;
    l.nVerts += n;
    return l.nCables++;
}

// LA CATENARIA: con el eje 'arriba' = -abajo, los extremos quedan a una distancia horizontal h y una diferencia de
// altura v (b respecto de a). La curva y(s) = p cosh((s - s0) / p) + C mide de arco L = 2 p cosh((h - 2 s0) / 2p)
// sinh(h / 2p) y sube v = 2 p sinh((h - 2 s0) / 2p) sinh(h / 2p). De ahi:
//   sqrt(L^2 - v^2) = 2 p sinh(h / 2p)   ->   con x = h / 2p:  sinh(x) / x = sqrt(L^2 - v^2) / h = r
//   tanh((h - 2 s0) / 2p) = v / L          ->   s0 = h / 2 - p atanh(v / L)
// sinh(x) / x = r se resuelve con Newton desde arriba: como sinh(x) / x >= 1 + x^2 / 6, la raiz es menor que
// sqrt(6 (r - 1)) y ahi la funcion ya sube (es convexa): baja derecho a la raiz sin pasarse.
void W3dCablesSag(const float* a, const float* b, float length, int segments, const float* down, float* out) {
    if (segments < 1) segments = 1;
    double g[3] = { 0.0, -1.0, 0.0 };
    if (down) {
        const double m = sqrt((double)down[0] * down[0] + (double)down[1] * down[1] + (double)down[2] * down[2]);
        if (m > 1e-9) for (int e = 0; e < 3; e++) g[e] = down[e] / m;
    }
    const double d[3] = { (double)b[0] - a[0], (double)b[1] - a[1], (double)b[2] - a[2] };
    const double dg = d[0] * g[0] + d[1] * g[1] + d[2] * g[2];
    double hd[3] = { d[0] - dg * g[0], d[1] - dg * g[1], d[2] - dg * g[2] };
    const double h = sqrt(hd[0] * hd[0] + hd[1] * hd[1] + hd[2] * hd[2]);
    const double v = -dg;   // (lo que sube b sobre a)
    const double dist = sqrt(h * h + v * v);
    const double L = length;
    // derecho: sin largo de mas o casi vertical (una catenaria necesita distancia horizontal)
    if (L <= dist * 1.0005 || h < 1e-3 || h < L * 1e-3) {
        for (int k = 0; k <= segments; k++) {
            const float t = (float)k / (float)segments;
            for (int e = 0; e < 3; e++) out[k * 3 + e] = a[e] + (b[e] - a[e]) * t;
        }
        return;
    }
    for (int e = 0; e < 3; e++) hd[e] /= h;
    const double r = sqrt(L * L - v * v) / h;
    double x = sqrt(6.0 * (r - 1.0));
    for (int it = 0; it < 100; it++) {
        const double f = sinh(x) - r * x;
        const double df = cosh(x) - r;
        if (df <= 0.0) break;
        const double nx = x - f / df;
        if (nx <= 0.0 || fabs(nx - x) < 1e-10 * x) { x = nx > 0.0 ? nx : x; break; }
        x = nx;
    }
    const double p = h / (2.0 * x);
    const double q = v / L;
    const double s0 = h * 0.5 - p * 0.5 * log((1.0 + q) / (1.0 - q));   // (atanh)
    const double C = -p * cosh(s0 / p);
    for (int k = 0; k <= segments; k++) {
        if (k == segments) {
            for (int e = 0; e < 3; e++) out[k * 3 + e] = b[e];
            break;
        }
        const double s = h * k / segments;
        const double y = p * cosh((s - s0) / p) + C;
        for (int e = 0; e < 3; e++) out[k * 3 + e] = (float)(a[e] + hd[e] * s - g[e] * y);
    }
}

int W3dCablesAddSag(W3dCables& l, const float* a, const float* b, float length, int segments, float thickness,
                          const unsigned char* rgba, const float* down) {
    if (segments < 1) segments = 1;
    if (segments > 4096) segments = 4096;
    float* p = (float*)malloc((size_t)(segments + 1) * 3 * sizeof(float));
    if (!p) return -1;
    W3dCablesSag(a, b, length, segments, down, p);
    const int r = W3dCablesAdd(l, p, segments + 1, thickness, rgba);
    free(p);
    return r;
}

int W3dCablesDraw(W3dCables& l, const W3dCablesView& v) {
    memset(&l.last, 0, sizeof(l.last));
    if (!l.nCables || v.heightPx <= 0) return 0;
    // la lente
    const float tanY = tanf(v.fov * 0.5f * 3.14159265f / 180.0f);
    const float focal = (float)v.heightPx / (2.0f * tanY);
    const float tanX = tanY * (float)(v.widthPx > 0 ? v.widthPx : v.heightPx) / (float)v.heightPx;
    // (un poco de margen: lo que asoma por el borde lo recorta GL)
    const int32_t tx = (int32_t)(tanX * 1.05f * UNO) + 1;
    const int32_t ty = (int32_t)(tanY * 1.05f * UNO) + 1;
    // la camara en centimetros y sus ejes en punto fijo
    const int32_t cx = centimeters(v.pos[0]), cy = centimeters(v.pos[1]), cz = centimeters(v.pos[2]);
    int32_t ed[3], ea[3], ez[3];
    for (int e = 0; e < 3; e++) {
        ed[e] = (int32_t)(v.right[e] * UNO);
        ea[e] = (int32_t)(v.arr[e] * UNO);
        ez[e] = (int32_t)(-v.back[e] * UNO);    // (hacia adelante: z positiva delante de la camara)
    }
    const float limit = l.limit > 1.0f ? l.limit : 1.0f;
    const float limit2 = limit * limit;
    const int32_t limitCm = (int32_t)(limit * 100.0f);
    const int32_t fadeCm = limitCm - limitCm / 4;             // desde aca el alfa baja a cero en el tope
    const int32_t invFade = (256 << 16) / (limitCm - fadeCm);
    const int32_t nearCm = v.near > 0.01f ? (int32_t)(v.near * 100.0f) : 1;
    const int32_t floorA = (int32_t)((l.floor < 0.0f ? 0.0f : l.floor > 1.0f ? 1.0f : l.floor) * 255.0f);
    const int32_t threshold = l.thresholdRibbon > 0.0f ? (int32_t)(l.thresholdRibbon * 256.0f) : 0x7fffffff;
    // la curva de la cobertura (se rearma si cambia el realce)
    if (l.highlightTable != l.highlight) {
        const float e = l.highlight > 0.05f ? l.highlight : 0.05f;
        for (int i = 0; i < 256; i++) l.alphaTable[i] = (unsigned char)(powf(i / 255.0f, e) * 255.0f + 0.5f);
        l.highlightTable = l.highlight;
    }
    int nLin = 0, nCin = 0;

    for (int ci = 0; ci < l.nCables; ci++) {
        if (l.hidden[ci]) continue;
        const W3dCable& c = l.cables[ci];
        // EL CABLE ENTERO: la caja mas lejos que el tope o afuera de algun plano de la piramide
        float d2 = 0.0f;
        for (int e = 0; e < 3; e++) {
            const float q = v.pos[e] < c.mn[e] ? c.mn[e] - v.pos[e] : v.pos[e] > c.mx[e] ? v.pos[e] - c.mx[e] : 0.0f;
            d2 += q * q;
        }
        if (d2 > limit2) continue;
        if (v.planes) {
            bool outside = false;
            for (int k = 0; k < 6 && !outside; k++) {
                const float* pl = v.planes + k * 4;
                // (el vertice de la caja mas adentro del plano)
                const float s = pl[0] * (pl[0] >= 0 ? c.mx[0] : c.mn[0]) + pl[1] * (pl[1] >= 0 ? c.mx[1] : c.mn[1]) +
                                pl[2] * (pl[2] >= 0 ? c.mx[2] : c.mn[2]) + pl[3];
                if (s < 0.0f) outside = true;
            }
            if (outside) continue;
        }
        l.last.cables++;
        // K / z = el grosor en pantalla (256 = 1 pixel) con z en centimetros
        const int32_t K = (int32_t)(c.thickness * 100.0f * focal * 256.0f);
        const int end = c.first + c.count;
        // (lejos, un vertice de cada dos: el cable ya es una raya fina)
        const int step = (l.distHalf > 0.0f && d2 > l.distHalf * l.distHalf && c.count > 4) ? 2 : 1;
        // POR VERTICE: la vista en enteros, los bits de afuera y el alfa
        for (int i = c.first;; i += step) {
            if (i > end - 1) i = end - 1;
            const int32_t* p = l.cm + i * 3;
            const int32_t dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
            const int32_t z = (dx * ez[0] + dy * ez[1] + dz * ez[2]) / UNO;
            const int32_t x = (dx * ed[0] + dy * ed[1] + dz * ed[2]) / UNO;
            const int32_t y = (dx * ea[0] + dy * ea[1] + dz * ea[2]) / UNO;
            const int32_t zx = (z * tx) / UNO, zy = (z * ty) / UNO;
            uint8_t f = 0;
            if (x < -zx) f |= 1;
            if (x > zx) f |= 2;
            if (y < -zy) f |= 4;
            if (y > zy) f |= 8;
            if (z < nearCm) f |= 16;
            if (z > limitCm) f |= 32;
            l.outside[i] = f;
            const int32_t g = z > 0 ? K / z : 0x7fffffff;
            l.px[i] = g;
            int32_t a = l.alphaTable[g >= 255 ? 255 : g];
            if (a < floorA) a = floorA;
            if (z > fadeCm) {
                int32_t k = ((limitCm - z) * invFade) >> 16;
                if (k < 0) k = 0;
                a = (a * k) >> 8;
            }
            unsigned char* col = l.color + i * 4;
            col[0] = c.rgba[0];
            col[1] = c.rgba[1];
            col[2] = c.rgba[2];
            col[3] = (unsigned char)((a * (c.rgba[3] + 1)) >> 8);
            l.last.vertices++;
            if (i == end - 1) break;
        }
        // POR TRAMO: la linea (salvo con los dos extremos del mismo lado afuera) y, de cerca, la cinta
        for (int i = c.first; i < end - 1;) {
            const int j = i + step < end ? i + step : end - 1;
            if (l.outside[i] & l.outside[j]) { i = j; continue; }
            l.idxLines[nLin * 2] = (unsigned short)i;
            l.idxLines[nLin * 2 + 1] = (unsigned short)j;
            nLin++;
            if ((l.px[i] >= threshold || l.px[j] >= threshold) && nCin < l.capRibbons) {
                float s0[3], s1[3];
                side(l, c, i, v.pos, s0);
                side(l, c, j, v.pos, s1);
                const float* p0 = l.pos + i * 3;
                const float* p1 = l.pos + j * 3;
                float* q = l.ribbon + nCin * 12;
                for (int e = 0; e < 3; e++) {
                    q[e] = p0[e] - s0[e];
                    q[3 + e] = p0[e] + s0[e];
                    q[6 + e] = p1[e] + s1[e];
                    q[9 + e] = p1[e] - s1[e];
                }
                unsigned char* k = l.colRibbon + nCin * 16;
                memcpy(k, l.color + i * 4, 4);
                memcpy(k + 4, l.color + i * 4, 4);
                memcpy(k + 8, l.color + j * 4, 4);
                memcpy(k + 12, l.color + j * 4, 4);
                nCin++;
            }
            i = j;
        }
    }
    l.last.lines   = nLin;
    l.last.ribbons = nCin;
    if (!nLin) return 0;

    // EL DIBUJO: sin textura ni luz, color por vertice, mezcla alfa, z sin escribir
    gfx::UnbindVBOs();
    gfx::MixTexturesOff();
    gfx::Disable(gfx::Texture2D);
    gfx::Disable(gfx::Lighting);
    gfx::Disable(gfx::ColorMaterial);
    gfx::EnableArray(gfx::VertexArray);
    gfx::EnableArray(gfx::ColorArray);
    gfx::DisableArray(gfx::TexCoordArray);
    gfx::DisableArray(gfx::NormalArray);
    gfx::Color4f(1.0f, 1.0f, 1.0f, 1.0f);
    // (el test de profundidad SIEMPRE, sea cual sea el estado en que llega: lo tapan paredes y techos)
    gfx::Enable(gfx::DepthTest);
    gfx::DepthFunc(gfx::DepthLEqual);
    gfx::DepthRange(0.0f, 1.0f);
    gfx::DepthMask(false);
    gfx::Disable(gfx::CullFace);
    gfx::AlphaTest(0.0f);
    gfx::Enable(gfx::Blend);
    gfx::SetMix(gfx::MixAlpha);
    gfx::VertexPointer3f(0, l.pos);
    gfx::ColorPointer4ub(l.color);
    gfx::DrawLinesIndexed(nLin * 2, l.idxLines);
    if (nCin) {
        gfx::VertexPointer3f(0, l.ribbon);
        gfx::ColorPointer4ub(l.colRibbon);
        gfx::DrawElements16(gfx::PrimTriangles, nCin * 6, l.idxRibbon);
    }
    // (lo que espera lo que sigue: como lo deja WorldBsp)
    gfx::Disable(gfx::Blend);
    gfx::DepthFunc(gfx::DepthLess);
    gfx::DepthMask(true);
    gfx::Enable(gfx::CullFace);
    gfx::EnableArray(gfx::TexCoordArray);
    return nLin * 2 + nCin * 4;
}

void W3dCablesRelease(W3dCables& l) {
    free(l.pos); free(l.cm); free(l.cables); free(l.hidden); free(l.color); free(l.px); free(l.outside);
    free(l.idxLines); free(l.ribbon); free(l.colRibbon); free(l.idxRibbon);
    const float limit = l.limit, floor = l.floor, threshold = l.thresholdRibbon, highlight = l.highlight, half = l.distHalf;
    l = W3dCables();
    l.limit = limit; l.floor = floor; l.thresholdRibbon = threshold; l.highlight = highlight; l.distHalf = half;
}
