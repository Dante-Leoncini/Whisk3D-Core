#include "objects/W3dCables.h"
#include "w3dGraphics.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace gfx = w3dEngine;

namespace {
enum { MAX_VERTS = 65535, MAX_CINTAS = 16383 };   // (indices de 16 bits: 4 vertices por cinta)
const int UNO = 1024;                             // el punto fijo de los ejes de la camara

int32_t centimetros(float m) {
    const float c = m * 100.0f;
    return (int32_t)(c >= 0.0f ? c + 0.5f : c - 0.5f);
}

// agranda un arreglo (realloc) a 'cap' elementos de 'tam' bytes; false si no hay memoria
template <class T> bool crecer(T*& p, int cap, int tam) {
    void* q = realloc(p, (size_t)cap * (size_t)tam);
    if (!q) return false;
    p = (T*)q;
    return true;
}

bool reservar_vertices(W3dCables& l, int n) {
    if (n <= l.capVerts) return true;
    int cap = l.capVerts ? l.capVerts : 256;
    while (cap < n) cap *= 2;
    if (cap > MAX_VERTS) cap = MAX_VERTS;
    if (!crecer(l.pos, cap, 3 * sizeof(float)) || !crecer(l.cm, cap, 3 * sizeof(int32_t)) ||
        !crecer(l.color, cap, 4) || !crecer(l.px, cap, sizeof(int32_t)) || !crecer(l.fuera, cap, 1) ||
        !crecer(l.idxLineas, cap, 2 * sizeof(unsigned short)))
        return false;
    // las cintas: una por tramo como mucho (y no mas de las que entran con indices de 16 bits)
    const int capC = cap < MAX_CINTAS ? cap : MAX_CINTAS;
    if (capC > l.capCintas) {
        if (!crecer(l.cinta, capC, 12 * sizeof(float)) || !crecer(l.colCinta, capC, 16) ||
            !crecer(l.idxCinta, capC, 6 * sizeof(unsigned short)))
            return false;
        // (los indices de las cintas son siempre los mismos: dos triangulos por cada 4 vertices)
        for (int k = l.capCintas; k < capC; k++) {
            unsigned short* q = l.idxCinta + k * 6;
            const unsigned short b = (unsigned short)(k * 4);
            q[0] = b; q[1] = (unsigned short)(b + 1); q[2] = (unsigned short)(b + 2);
            q[3] = b; q[4] = (unsigned short)(b + 2); q[5] = (unsigned short)(b + 3);
        }
        l.capCintas = capC;
    }
    l.capVerts = cap;
    return true;
}

bool reservar_cables(W3dCables& l, int n) {
    if (n <= l.capCables) return true;
    int cap = l.capCables ? l.capCables * 2 : 32;
    while (cap < n) cap *= 2;
    if (!crecer(l.cables, cap, sizeof(W3dCable)) || !crecer(l.oculto, cap, 1)) return false;
    l.capCables = cap;
    return true;
}

// el lado de la cinta en el vertice j: perpendicular al cable (su tangente en j) y a la linea de la camara, con la
// mitad del grosor
void lado(const W3dCables& l, const W3dCable& c, int j, const float* cam, float* s) {
    const int a = j > c.primero ? j - 1 : j;
    const int b = j < c.primero + c.cuantos - 1 ? j + 1 : j;
    const float* pa = l.pos + a * 3;
    const float* pb = l.pos + b * 3;
    const float* p = l.pos + j * 3;
    const float t[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
    const float v[3] = { p[0] - cam[0], p[1] - cam[1], p[2] - cam[2] };
    s[0] = t[1] * v[2] - t[2] * v[1];
    s[1] = t[2] * v[0] - t[0] * v[2];
    s[2] = t[0] * v[1] - t[1] * v[0];
    const float m = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
    const float k = m > 1e-12f ? c.grosor * 0.5f / sqrtf(m) : 0.0f;
    s[0] *= k; s[1] *= k; s[2] *= k;
}
}

W3dCables::W3dCables()
    : pos(0), cm(0), nVerts(0), capVerts(0), cables(0), oculto(0), nCables(0), capCables(0), color(0), px(0), fuera(0),
      idxLineas(0), cinta(0), colCinta(0), idxCinta(0), capCintas(0), tope(120.0f), piso(0.2f), umbralCinta(2.0f),
      realce(0.5f), tablaRealce(-1.0f), distMitad(30.0f) {
    memset(&ultimo, 0, sizeof(ultimo));
    memset(tablaAlfa, 0, sizeof(tablaAlfa));
}

int W3dCablesAgregar(W3dCables& l, const float* puntos, int n, float grosor, const unsigned char* rgba) {
    if (!puntos || n < 2 || l.nVerts + n > MAX_VERTS) return -1;
    if (!reservar_vertices(l, l.nVerts + n) || !reservar_cables(l, l.nCables + 1)) return -1;
    W3dCable& c = l.cables[l.nCables];
    c.primero = l.nVerts;
    c.cuantos = n;
    c.grosor = grosor > 0.0f ? grosor : 0.0f;
    for (int e = 0; e < 4; e++) c.rgba[e] = rgba ? rgba[e] : (unsigned char)(e < 3 ? 0 : 255);
    for (int e = 0; e < 3; e++) c.mn[e] = c.mx[e] = puntos[e];
    for (int i = 0; i < n; i++) {
        const float* p = puntos + i * 3;
        for (int e = 0; e < 3; e++) {
            l.pos[(l.nVerts + i) * 3 + e] = p[e];
            l.cm[(l.nVerts + i) * 3 + e] = centimetros(p[e]);
            if (p[e] < c.mn[e]) c.mn[e] = p[e];
            if (p[e] > c.mx[e]) c.mx[e] = p[e];
        }
    }
    l.oculto[l.nCables] = 0;
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
void W3dCablesComba(const float* a, const float* b, float largo, int tramos, const float* abajo, float* out) {
    if (tramos < 1) tramos = 1;
    double g[3] = { 0.0, -1.0, 0.0 };
    if (abajo) {
        const double m = sqrt((double)abajo[0] * abajo[0] + (double)abajo[1] * abajo[1] + (double)abajo[2] * abajo[2]);
        if (m > 1e-9) for (int e = 0; e < 3; e++) g[e] = abajo[e] / m;
    }
    const double d[3] = { (double)b[0] - a[0], (double)b[1] - a[1], (double)b[2] - a[2] };
    const double dg = d[0] * g[0] + d[1] * g[1] + d[2] * g[2];
    double hd[3] = { d[0] - dg * g[0], d[1] - dg * g[1], d[2] - dg * g[2] };
    const double h = sqrt(hd[0] * hd[0] + hd[1] * hd[1] + hd[2] * hd[2]);
    const double v = -dg;   // (lo que sube b sobre a)
    const double dist = sqrt(h * h + v * v);
    const double L = largo;
    // derecho: sin largo de mas o casi vertical (una catenaria necesita distancia horizontal)
    if (L <= dist * 1.0005 || h < 1e-3 || h < L * 1e-3) {
        for (int k = 0; k <= tramos; k++) {
            const float t = (float)k / (float)tramos;
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
    for (int k = 0; k <= tramos; k++) {
        if (k == tramos) {
            for (int e = 0; e < 3; e++) out[k * 3 + e] = b[e];
            break;
        }
        const double s = h * k / tramos;
        const double y = p * cosh((s - s0) / p) + C;
        for (int e = 0; e < 3; e++) out[k * 3 + e] = (float)(a[e] + hd[e] * s - g[e] * y);
    }
}

int W3dCablesAgregarComba(W3dCables& l, const float* a, const float* b, float largo, int tramos, float grosor,
                          const unsigned char* rgba, const float* abajo) {
    if (tramos < 1) tramos = 1;
    if (tramos > 4096) tramos = 4096;
    float* p = (float*)malloc((size_t)(tramos + 1) * 3 * sizeof(float));
    if (!p) return -1;
    W3dCablesComba(a, b, largo, tramos, abajo, p);
    const int r = W3dCablesAgregar(l, p, tramos + 1, grosor, rgba);
    free(p);
    return r;
}

int W3dCablesDibujar(W3dCables& l, const W3dCablesVista& v) {
    memset(&l.ultimo, 0, sizeof(l.ultimo));
    if (!l.nCables || v.altoPx <= 0) return 0;
    // la lente
    const float tanY = tanf(v.fov * 0.5f * 3.14159265f / 180.0f);
    const float focal = (float)v.altoPx / (2.0f * tanY);
    const float tanX = tanY * (float)(v.anchoPx > 0 ? v.anchoPx : v.altoPx) / (float)v.altoPx;
    // (un poco de margen: lo que asoma por el borde lo recorta GL)
    const int32_t tx = (int32_t)(tanX * 1.05f * UNO) + 1;
    const int32_t ty = (int32_t)(tanY * 1.05f * UNO) + 1;
    // la camara en centimetros y sus ejes en punto fijo
    const int32_t cx = centimetros(v.pos[0]), cy = centimetros(v.pos[1]), cz = centimetros(v.pos[2]);
    int32_t ed[3], ea[3], ez[3];
    for (int e = 0; e < 3; e++) {
        ed[e] = (int32_t)(v.der[e] * UNO);
        ea[e] = (int32_t)(v.arr[e] * UNO);
        ez[e] = (int32_t)(-v.atras[e] * UNO);    // (hacia adelante: z positiva delante de la camara)
    }
    const float tope = l.tope > 1.0f ? l.tope : 1.0f;
    const float tope2 = tope * tope;
    const int32_t topeCm = (int32_t)(tope * 100.0f);
    const int32_t fundidoCm = topeCm - topeCm / 4;             // desde aca el alfa baja a cero en el tope
    const int32_t invFundido = (256 << 16) / (topeCm - fundidoCm);
    const int32_t cercaCm = v.cerca > 0.01f ? (int32_t)(v.cerca * 100.0f) : 1;
    const int32_t pisoA = (int32_t)((l.piso < 0.0f ? 0.0f : l.piso > 1.0f ? 1.0f : l.piso) * 255.0f);
    const int32_t umbral = l.umbralCinta > 0.0f ? (int32_t)(l.umbralCinta * 256.0f) : 0x7fffffff;
    // la curva de la cobertura (se rearma si cambia el realce)
    if (l.tablaRealce != l.realce) {
        const float e = l.realce > 0.05f ? l.realce : 0.05f;
        for (int i = 0; i < 256; i++) l.tablaAlfa[i] = (unsigned char)(powf(i / 255.0f, e) * 255.0f + 0.5f);
        l.tablaRealce = l.realce;
    }
    int nLin = 0, nCin = 0;

    for (int ci = 0; ci < l.nCables; ci++) {
        if (l.oculto[ci]) continue;
        const W3dCable& c = l.cables[ci];
        // EL CABLE ENTERO: la caja mas lejos que el tope o afuera de algun plano de la piramide
        float d2 = 0.0f;
        for (int e = 0; e < 3; e++) {
            const float q = v.pos[e] < c.mn[e] ? c.mn[e] - v.pos[e] : v.pos[e] > c.mx[e] ? v.pos[e] - c.mx[e] : 0.0f;
            d2 += q * q;
        }
        if (d2 > tope2) continue;
        if (v.planos) {
            bool afuera = false;
            for (int k = 0; k < 6 && !afuera; k++) {
                const float* pl = v.planos + k * 4;
                // (el vertice de la caja mas adentro del plano)
                const float s = pl[0] * (pl[0] >= 0 ? c.mx[0] : c.mn[0]) + pl[1] * (pl[1] >= 0 ? c.mx[1] : c.mn[1]) +
                                pl[2] * (pl[2] >= 0 ? c.mx[2] : c.mn[2]) + pl[3];
                if (s < 0.0f) afuera = true;
            }
            if (afuera) continue;
        }
        l.ultimo.cables++;
        // K / z = el grosor en pantalla (256 = 1 pixel) con z en centimetros
        const int32_t K = (int32_t)(c.grosor * 100.0f * focal * 256.0f);
        const int fin = c.primero + c.cuantos;
        // (lejos, un vertice de cada dos: el cable ya es una raya fina)
        const int paso = (l.distMitad > 0.0f && d2 > l.distMitad * l.distMitad && c.cuantos > 4) ? 2 : 1;
        // POR VERTICE: la vista en enteros, los bits de afuera y el alfa
        for (int i = c.primero;; i += paso) {
            if (i > fin - 1) i = fin - 1;
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
            if (z < cercaCm) f |= 16;
            if (z > topeCm) f |= 32;
            l.fuera[i] = f;
            const int32_t g = z > 0 ? K / z : 0x7fffffff;
            l.px[i] = g;
            int32_t a = l.tablaAlfa[g >= 255 ? 255 : g];
            if (a < pisoA) a = pisoA;
            if (z > fundidoCm) {
                int32_t k = ((topeCm - z) * invFundido) >> 16;
                if (k < 0) k = 0;
                a = (a * k) >> 8;
            }
            unsigned char* col = l.color + i * 4;
            col[0] = c.rgba[0];
            col[1] = c.rgba[1];
            col[2] = c.rgba[2];
            col[3] = (unsigned char)((a * (c.rgba[3] + 1)) >> 8);
            l.ultimo.vertices++;
            if (i == fin - 1) break;
        }
        // POR TRAMO: la linea (salvo con los dos extremos del mismo lado afuera) y, de cerca, la cinta
        for (int i = c.primero; i < fin - 1;) {
            const int j = i + paso < fin ? i + paso : fin - 1;
            if (l.fuera[i] & l.fuera[j]) { i = j; continue; }
            l.idxLineas[nLin * 2] = (unsigned short)i;
            l.idxLineas[nLin * 2 + 1] = (unsigned short)j;
            nLin++;
            if ((l.px[i] >= umbral || l.px[j] >= umbral) && nCin < l.capCintas) {
                float s0[3], s1[3];
                lado(l, c, i, v.pos, s0);
                lado(l, c, j, v.pos, s1);
                const float* p0 = l.pos + i * 3;
                const float* p1 = l.pos + j * 3;
                float* q = l.cinta + nCin * 12;
                for (int e = 0; e < 3; e++) {
                    q[e] = p0[e] - s0[e];
                    q[3 + e] = p0[e] + s0[e];
                    q[6 + e] = p1[e] + s1[e];
                    q[9 + e] = p1[e] - s1[e];
                }
                unsigned char* k = l.colCinta + nCin * 16;
                memcpy(k, l.color + i * 4, 4);
                memcpy(k + 4, l.color + i * 4, 4);
                memcpy(k + 8, l.color + j * 4, 4);
                memcpy(k + 12, l.color + j * 4, 4);
                nCin++;
            }
            i = j;
        }
    }
    l.ultimo.lineas = nLin;
    l.ultimo.cintas = nCin;
    if (!nLin) return 0;

    // EL DIBUJO: sin textura ni luz, color por vertice, mezcla alfa, z sin escribir
    gfx::UnbindVBOs();
    gfx::MezclaTexturasOff();
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
    gfx::SetMezcla(gfx::MezclaAlpha);
    gfx::VertexPointer3f(0, l.pos);
    gfx::ColorPointer4ub(l.color);
    gfx::DrawLinesIndexed(nLin * 2, l.idxLineas);
    if (nCin) {
        gfx::VertexPointer3f(0, l.cinta);
        gfx::ColorPointer4ub(l.colCinta);
        gfx::DrawElements16(gfx::PrimTriangulos, nCin * 6, l.idxCinta);
    }
    // (lo que espera lo que sigue: como lo deja MundoBsp)
    gfx::Disable(gfx::Blend);
    gfx::DepthFunc(gfx::DepthLess);
    gfx::DepthMask(true);
    gfx::Enable(gfx::CullFace);
    gfx::EnableArray(gfx::TexCoordArray);
    return nLin * 2 + nCin * 4;
}

void W3dCablesSoltar(W3dCables& l) {
    free(l.pos); free(l.cm); free(l.cables); free(l.oculto); free(l.color); free(l.px); free(l.fuera);
    free(l.idxLineas); free(l.cinta); free(l.colCinta); free(l.idxCinta);
    const float tope = l.tope, piso = l.piso, umbral = l.umbralCinta, realce = l.realce, mitad = l.distMitad;
    l = W3dCables();
    l.tope = tope; l.piso = piso; l.umbralCinta = umbral; l.realce = realce; l.distMitad = mitad;
}
