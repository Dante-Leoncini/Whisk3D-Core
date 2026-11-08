#include "animation/W3dLocomocion.h"
#include "physics/W3dRagdoll.h"
#include <math.h>
#include <string.h>

// ------------------------------------------------------------------------------------------------ cuentas
namespace {
const float GRAD = 0.017453293f;
enum { MAX_H = 160 };

inline float topar(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
inline float punto(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void q_mul(const float* a, const float* b, float* o) {
    float r[4];
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    memcpy(o, r, sizeof(r));
}
inline void q_inv(const float* a, float* o) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
void q_eje(const float* eje, float ang, float* q) {
    const float s = sinf(ang * 0.5f);
    q[0] = eje[0] * s; q[1] = eje[1] * s; q[2] = eje[2] * s; q[3] = cosf(ang * 0.5f);
}
void q_rotar(const float* q, const float* v, float* o) {
    // v + 2w (u x v) + 2 u x (u x v)
    const float* u = q;
    float c[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    float d[3] = { u[1] * c[2] - u[2] * c[1], u[2] * c[0] - u[0] * c[2], u[0] * c[1] - u[1] * c[0] };
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = v[k] + 2 * (q[3] * c[k] + d[k]);
    memcpy(o, r, sizeof(r));
}

// las transformaciones en el modelo (giro y posicion de cada hueso)
struct Modelo {
    float q[MAX_H][4], p[MAX_H][3];
};

void fk(const W3dLocoEsqueleto& e, float (*q)[4], float (*t)[3], Modelo& m) {
    for (int k = 0; k < e.n && k < MAX_H; k++) {
        const int i = e.orden ? e.orden[k] : k;
        const int pa = e.padre[i];
        if (pa < 0) {
            memcpy(m.q[i], q[i], sizeof(float) * 4);
            memcpy(m.p[i], t[i], sizeof(float) * 3);
        } else {
            q_mul(m.q[pa], q[i], m.q[i]);
            float d[3];
            q_rotar(m.q[pa], t[i], d);
            for (int j = 0; j < 3; j++) m.p[i][j] = m.p[pa][j] + d[j];
        }
    }
}

// gira el hueso h por G (en el modelo, alrededor de su origen): local' = P^-1 G P local
void girar(const W3dLocoEsqueleto& e, float (*q)[4], const Modelo& m, int h, const float* G) {
    float P[4] = { 0, 0, 0, 1 }, Pi[4], a[4], b[4];
    if (e.padre[h] >= 0) memcpy(P, m.q[e.padre[h]], sizeof(P));
    q_inv(P, Pi);
    q_mul(G, P, a);
    q_mul(Pi, a, b);
    q_mul(b, q[h], a);
    memcpy(q[h], a, sizeof(a));
}

// la afin 3x4 por columnas: punto y direccion al mundo, y de vuelta
void al_mundo(const float* M, const float* p, float* o) {
    for (int k = 0; k < 3; k++) o[k] = M[k] * p[0] + M[3 + k] * p[1] + M[6 + k] * p[2] + M[9 + k];
}
bool invertir(const float* M, float* I) {
    const float a = M[0], b = M[3], c = M[6], d = M[1], e = M[4], f = M[7], g = M[2], h = M[5], i = M[8];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-12f) return false;
    const float k = 1.0f / det;
    // (filas de la inversa en I por columnas)
    I[0] = A * k;             I[3] = -(b * i - c * h) * k; I[6] = (b * f - c * e) * k;
    I[1] = B * k;             I[4] = (a * i - c * g) * k;  I[7] = -(a * f - c * d) * k;
    I[2] = C * k;             I[5] = -(a * h - b * g) * k; I[8] = (a * e - b * d) * k;
    for (int r = 0; r < 3; r++) I[9 + r] = -(I[r] * M[9] + I[3 + r] * M[10] + I[6 + r] * M[11]);
    return true;
}
void dir_a(const float* I, const float* v, float* o) {
    for (int k = 0; k < 3; k++) o[k] = I[k] * v[0] + I[3 + k] * v[1] + I[6 + k] * v[2];
}
}

// ------------------------------------------------------------------------------------------------ la marcha
W3dLocoMarcha::W3dLocoMarcha() : rapidez(0), acelera(150), frena(220), paso(0) {}

void W3dLocoAvanzar(W3dLocoMarcha& m, float quiere, float velCaminar, float velCorrer, float dt) {
    const float d = quiere - m.rapidez;
    const float tope = (d > 0 ? m.acelera : m.frena) * dt;
    m.rapidez += topar(d, -tope, tope);
    if (m.rapidez < 0) m.rapidez = 0;
    // (con histeresis: no cambia de clip en el borde)
    const float corre = (velCaminar + velCorrer) * 0.5f;
    if (m.rapidez < velCaminar * 0.08f) m.paso = 0;
    else if (m.paso == 2 && m.rapidez < corre * 0.9f) m.paso = 1;
    else if (m.paso != 2 && m.rapidez > corre * 1.1f && velCorrer > velCaminar) m.paso = 2;
    else if (m.paso == 0 && m.rapidez > velCaminar * 0.15f) m.paso = 1;
}

float W3dLocoRitmo(float rapidez, float velClip, float min, float max) {
    if (velClip <= 1e-4f) return 1;
    return topar(rapidez / velClip, min, max);
}

// ------------------------------------------------------------------------------------------------ el giro
W3dLocoGiroDef::W3dLocoGiroDef() : umbral(55), velPaso(160), velCaminando(240), torsionMax(45) {}
W3dLocoGiro::W3dLocoGiro() : yaw(0), torsion(0), pisando(false) {}

void W3dLocoGirar(W3dLocoGiro& g, float quiere, bool caminando, const W3dLocoGiroDef& d, float dt) {
    float e = quiere - g.yaw;
    while (e > 180) e -= 360;
    while (e < -180) e += 360;
    if (caminando) {
        g.pisando = false;
        const float t = d.velCaminando * dt;
        g.yaw += topar(e, -t, t);
        g.torsion += (0 - g.torsion) * topar(dt * 6, 0, 1);
        return;
    }
    if (!g.pisando && fabsf(e) > d.umbral) g.pisando = true;
    if (g.pisando) {
        const float t = d.velPaso * dt;
        g.yaw += topar(e, -t, t);
        if (fabsf(e) < 4) g.pisando = false;
    }
    // (el torso hacia lo que falta, hasta su tope)
    float resta = quiere - g.yaw;
    while (resta > 180) resta -= 360;
    while (resta < -180) resta += 360;
    g.torsion += (topar(resta, -d.torsionMax, d.torsionMax) - g.torsion) * topar(dt * 5, 0, 1);
}

// ------------------------------------------------------------------------------------------------ la mirada
void W3dLocoMirar(const W3dLocoEsqueleto& e, float (*q)[4], float (*t)[3], const W3dLocoCadena& c, float yaw,
                  float pitch) {
    if (e.n > MAX_H || c.n <= 0 || (fabsf(yaw) < 0.05f && fabsf(pitch) < 0.05f)) return;
    static Modelo m;
    fk(e, q, t, m);
    float fijos[W3DLOCO_FIJOS][4];
    for (int k = 0; k < c.nFijos; k++) memcpy(fijos[k], m.q[c.fijo[k]], sizeof(fijos[k]));
    for (int k = 0; k < c.n; k++) {
        const float y = topar(yaw * c.peso[k], -c.topeYaw[k], c.topeYaw[k]) * GRAD;
        const float p = topar(pitch * c.peso[k], -c.topePitch[k], c.topePitch[k]) * GRAD;
        float qy[4], qp[4], G[4];
        q_eje(c.arriba, y, qy);
        q_eje(c.derecha, p, qp);
        q_mul(qy, qp, G);
        girar(e, q, m, c.hueso[k], G);
        fk(e, q, t, m);
    }
    // (los fijos -las claviculas- vuelven a su giro del modelo: los brazos no siguen a la columna)
    for (int k = 0; k < c.nFijos; k++) {
        const int h = c.fijo[k], pa = e.padre[h];
        if (pa < 0) { memcpy(q[h], fijos[k], sizeof(fijos[k])); continue; }
        float pi[4];
        q_inv(m.q[pa], pi);
        q_mul(pi, fijos[k], q[h]);
    }
}

// ------------------------------------------------------------------------------------------------ los pies
W3dLocoPiesDef::W3dLocoPiesDef()
    : pelvis(-1), maxBaja(0.4f), maxPiso(0.5f), alturaPisa(0.04f), maxDesliz(0.3f), maxGiro(25), margenMedio(0.03f),
      durPaso(0.2f), altoPaso(0.06f), asentar(0.03f), entrePasos(0), velPisa(0), velSuelta(0) {
    frente[0] = frente[1] = frente[2] = 0;
    pie[0].cadera = pie[0].rodilla = pie[0].tobillo = -1;
    pie[1] = pie[0];
    arriba[0] = 0; arriba[1] = 1; arriba[2] = 0;
}

W3dLocoPiesEstado::W3dLocoPiesEstado() : pelvis(0), listo(false) {
    clavado[0] = clavado[1] = false;
    peso[0] = peso[1] = 0;
    minTobillo[0] = minTobillo[1] = 0;
    pasoT[0] = pasoT[1] = -1;
    hayPolo[0] = hayPolo[1] = false;
    desdePaso = 1e3f;
    vel[0] = vel[1] = 0;
    memset(antes, 0, sizeof(antes));
    hayFrentePie[0] = hayFrentePie[1] = false;
    memset(polo, 0, sizeof(polo));
    memset(ancla, 0, sizeof(ancla));
    memset(pasoDe, 0, sizeof(pasoDe));
    memset(frente, 0, sizeof(frente));
}

void W3dLocoPies(const W3dLocoEsqueleto& e, float (*q)[4], float (*t)[3], const float* modelo,
                 const W3dLocoPiesDef& d, W3dLocoPiesEstado& s, W3dLocoSueloFn suelo, void* usuario, bool clavar,
                 float dt) {
    if (e.n > MAX_H || d.pelvis < 0 || !suelo) return;
    for (int f = 0; f < 2; f++)
        if (d.pie[f].cadera < 0 || d.pie[f].rodilla < 0 || d.pie[f].tobillo < 0) return;
    float I[12];
    if (!invertir(modelo, I)) return;
    static Modelo m;
    fk(e, q, t, m);
    const float* U = d.arriba;
    const float O[3] = { modelo[9], modelo[10], modelo[11] };
    // 1. cada pie: donde lo pone la animacion, su piso y cuanto se corre
    float A0[2][3], off[2];
    bool pisa[2], suelta[2];
    for (int f = 0; f < 2; f++) {
        al_mundo(modelo, m.p[d.pie[f].tobillo], A0[f]);
        const float rel[3] = { A0[f][0] - O[0], A0[f][1] - O[1], A0[f][2] - O[2] };
        const float h = punto(rel, U);
        if (!s.listo || h < s.minTobillo[f]) s.minTobillo[f] = h;
        else s.minTobillo[f] += (h - s.minTobillo[f]) * topar(dt * 0.05f, 0, 1);
        pisa[f] = h - s.minTobillo[f] < d.alturaPisa;
        suelta[f] = !pisa[f];
        if (d.velPisa > 0) {
            // (la velocidad sobre el piso del tobillo de la animacion: el que vuela no apoya aunque pase bajo)
            if (dt > 1e-5f) {
                float dv[3] = { A0[f][0] - s.antes[f][0], A0[f][1] - s.antes[f][1], A0[f][2] - s.antes[f][2] };
                const float u = punto(dv, U);
                for (int k = 0; k < 3; k++) dv[k] -= U[k] * u;
                s.vel[f] = s.listo ? sqrtf(punto(dv, dv)) / dt : 0;
                memcpy(s.antes[f], A0[f], sizeof(A0[f]));
            }
            pisa[f] = pisa[f] && s.vel[f] < d.velPisa;
            suelta[f] = s.vel[f] > d.velSuelta || h - s.minTobillo[f] > d.alturaPisa * 5;
        }
        // (el punto del plano de los pies del modelo bajo el tobillo, y el piso de verdad ahi)
        const float S[3] = { A0[f][0] - U[0] * h, A0[f][1] - U[1] * h, A0[f][2] - U[2] * h };
        float G[3];
        off[f] = 0;
        if (suelo(usuario, S, G)) {
            const float dg[3] = { G[0] - S[0], G[1] - S[1], G[2] - S[2] };
            const float o = punto(dg, U);
            if (fabsf(o) <= d.maxPiso) off[f] = o;
        }
    }
    s.listo = true;
    // 2. la pelvis baja lo que pida el pie mas bajo
    float baja = off[0] < off[1] ? off[0] : off[1];
    baja = topar(baja, -d.maxBaja, 0);
    s.pelvis += (baja - s.pelvis) * topar(dt * 10, 0, 1);
    if (fabsf(s.pelvis) > 1e-5f) {
        const float vu[3] = { U[0] * s.pelvis, U[1] * s.pelvis, U[2] * s.pelvis };
        float vm[3];
        dir_a(I, vu, vm);
        const int pa = e.padre[d.pelvis];
        if (pa >= 0) {
            float pi[4];
            q_inv(m.q[pa], pi);
            q_rotar(pi, vm, vm);
        }
        for (int k = 0; k < 3; k++) t[d.pelvis][k] += vm[k];
        fk(e, q, t, m);
    }
    // 3. cada pierna: el objetivo del tobillo (corrido a su piso, clavado donde apoyo o en el aire de un pasito) y la
    //    IK de dos huesos. El marco de la cadera: su punto medio y el eje de una cadera a la otra (sin el arriba), y un
    //    eje horizontal del cuerpo para medir cuanto giro desde que apoyo cada pie
    float Hw[2][3], medio[3], lat[3], fr[3];
    al_mundo(modelo, m.p[d.pie[0].cadera], Hw[0]);
    al_mundo(modelo, m.p[d.pie[1].cadera], Hw[1]);
    for (int k = 0; k < 3; k++) { medio[k] = (Hw[0][k] + Hw[1][k]) * 0.5f; lat[k] = Hw[0][k] - Hw[1][k]; }
    {
        const float u = punto(lat, U);
        for (int k = 0; k < 3; k++) lat[k] -= U[k] * u;
        const float l = sqrtf(punto(lat, lat));
        if (l > 1e-6f) for (int k = 0; k < 3; k++) lat[k] /= l;
        // (la columna del modelo mas horizontal)
        int c = 0;
        float mejor = 2;
        for (int j = 0; j < 3; j++) {
            const float n = sqrtf(punto(modelo + j * 3, modelo + j * 3));
            const float a = n > 1e-9f ? fabsf(punto(modelo + j * 3, U)) / n : 2;
            if (a < mejor) { mejor = a; c = j; }
        }
        const float v = punto(modelo + c * 3, U);
        for (int k = 0; k < 3; k++) fr[k] = modelo[c * 3 + k] - U[k] * v;
        const float lf = sqrtf(punto(fr, fr));
        if (lf > 1e-9f) for (int k = 0; k < 3; k++) fr[k] /= lf;
    }
    const float cosGiro = cosf(d.maxGiro * GRAD);
    s.desdePaso += dt;
    for (int f = 0; f < 2; f++) {
        const W3dLocoPie& P = d.pie[f];
        const float lado_f = f == 0 ? 1.0f : -1.0f;
        float T[3] = { A0[f][0] + U[0] * off[f], A0[f][1] + U[1] * off[f], A0[f][2] + U[2] * off[f] };
        float obj[3];
        if (s.pasoT[f] >= 0) {
            // (en el aire: de donde salio a donde lo pone la animacion ahora, en arco)
            s.pasoT[f] += d.durPaso > 1e-4f ? dt / d.durPaso : 1;
            const float u = topar(s.pasoT[f], 0, 1), uu = u * u * (3 - 2 * u);
            const float alto = d.altoPaso * sinf(u * 3.1415927f);
            for (int k = 0; k < 3; k++) obj[k] = s.pasoDe[f][k] + (T[k] - s.pasoDe[f][k]) * uu + U[k] * alto;
            if (u >= 1) {
                s.pasoT[f] = -1;
                memcpy(s.ancla[f], T, sizeof(T));
                memcpy(s.frente[f], fr, sizeof(fr)); s.hayFrentePie[f] = true;
                s.clavado[f] = clavar && pisa[f];
                s.peso[f] = s.clavado[f] ? 1.0f : 0.0f;
            }
        } else {
            if (s.clavado[f]) {
                float dx[3] = { T[0] - s.ancla[f][0], T[1] - s.ancla[f][1], T[2] - s.ancla[f][2] };
                const float lado = sqrtf(topar(punto(dx, dx) - punto(dx, U) * punto(dx, U), 0, 1e9f));
                const float ea[3] = { A0[f][0] - medio[0], A0[f][1] - medio[1], A0[f][2] - medio[2] };
                const float ec[3] = { s.ancla[f][0] - medio[0], s.ancla[f][1] - medio[1], s.ancla[f][2] - medio[2] };
                const float sAnim = punto(ea, lat) * lado_f, sAncla = punto(ec, lat) * lado_f;
                // (cruzado: del otro lado de la linea media, o pasando, cuando la animacion lo tiene de su lado)
                const bool cruzado = sAncla < d.margenMedio && sAncla < sAnim - d.margenMedio;
                const bool girado = punto(s.frente[f], fr) < cosGiro;
                const bool desacomodado = !clavar && (lado > d.asentar || punto(s.frente[f], fr) < 0.985f);
                // (el otro ya casi apoya: este sale; y no mas seguido que 'entrePasos')
                const bool otro = (s.pasoT[1 - f] >= 0 && s.pasoT[1 - f] < 0.6f) || s.desdePaso < d.entrePasos;
                if (lado > d.maxDesliz * 4) {
                    // (lo pusieron en otro lado: apoya donde esta, sin pasito)
                    memcpy(s.ancla[f], T, sizeof(T));
                    memcpy(s.frente[f], fr, sizeof(fr)); s.hayFrentePie[f] = true;
                    s.clavado[f] = clavar && pisa[f];
                } else if (clavar && suelta[f]) {
                    s.clavado[f] = false;     // (la animacion lo levanta: el paso del clip)
                } else if ((cruzado || girado || lado > d.maxDesliz || desacomodado) && !otro) {
                    for (int k = 0; k < 3; k++) s.pasoDe[f][k] = T[k] + (s.ancla[f][k] - T[k]) * s.peso[f];
                    s.pasoT[f] = 0;
                    s.desdePaso = 0;
                    s.clavado[f] = false;
                } else if (!clavar && !desacomodado && !cruzado) {
                    s.clavado[f] = false;     // (casi donde lo pone la animacion: se suelta sin que se note)
                }
            } else if (clavar && pisa[f]) {
                if (d.velPisa > 0) {
                    // (apoya donde esta ahora, con lo que le quedaba de la vuelta del apoyo anterior: no salta)
                    float da[3] = { s.ancla[f][0] - T[0], s.ancla[f][1] - T[1], s.ancla[f][2] - T[2] };
                    const float u = punto(da, U);
                    for (int k = 0; k < 3; k++) s.ancla[f][k] = T[k] + (da[k] - U[k] * u) * s.peso[f];
                    s.peso[f] = 1;
                } else {
                    memcpy(s.ancla[f], T, sizeof(T));
                }
                memcpy(s.frente[f], fr, sizeof(fr)); s.hayFrentePie[f] = true;
                s.clavado[f] = true;
            }
            if (s.pasoT[f] >= 0) {
                memcpy(obj, s.pasoDe[f], sizeof(obj));
                s.peso[f] = 0;
            } else {
                if (s.clavado[f]) s.peso[f] += (1 - s.peso[f]) * topar(dt * 12, 0, 1);
                else {
                    s.peso[f] += (0 - s.peso[f]) * topar(dt * 8, 0, 1);
                    if (s.peso[f] < 0.02f) memcpy(s.ancla[f], T, sizeof(T));
                }
                float da[3] = { s.ancla[f][0] - T[0], s.ancla[f][1] - T[1], s.ancla[f][2] - T[2] };
                if (d.velPisa > 0) {
                    // (fijo sobre el piso; la altura, la de la animacion)
                    const float u = punto(da, U);
                    for (int k = 0; k < 3; k++) da[k] -= U[k] * u;
                }
                for (int k = 0; k < 3; k++) obj[k] = T[k] + da[k] * s.peso[f];
            }
        }
        memcpy(T, obj, sizeof(T));
        // (el giro del pie: clavado, el pie queda como apoyo en el mundo; en el aire vuelve de a poco al del clip)
        float pesoGiro = 0;
        if (s.pasoT[f] >= 0) {
            const float u = topar(s.pasoT[f], 0, 1);
            pesoGiro = 1 - u * u * (3 - 2 * u);
        } else {
            pesoGiro = s.peso[f];
        }
        float H[3], K[3], A[3];
        al_mundo(modelo, m.p[P.cadera], H);
        al_mundo(modelo, m.p[P.rodilla], K);
        al_mundo(modelo, m.p[P.tobillo], A);
        const float dT[3] = { T[0] - A[0], T[1] - A[1], T[2] - A[2] };
        if (punto(dT, dT) < 1e-8f && pesoGiro < 1e-3f) continue;
        // (la rodilla hacia donde la dobla la animacion si esta bien doblada; con la pierna casi derecha -el idle- esa
        // direccion es ruido y daria vuelta la rodilla: hacia el frente del modelo, o la ultima buena)
        float pol[3];
        {
            const float b[3] = { K[0] - (H[0] + A[0]) * 0.5f, K[1] - (H[1] + A[1]) * 0.5f, K[2] - (H[2] + A[2]) * 0.5f };
            float bm[3];
            dir_a(I, b, bm);
            const float l = sqrtf(punto(bm, bm));
            if (l > 0.04f) {
                for (int k = 0; k < 3; k++) s.polo[f][k] = bm[k] / l;
                s.hayPolo[f] = true;
            } else if (punto(d.frente, d.frente) > 1e-6f) {
                memcpy(s.polo[f], d.frente, sizeof(d.frente));
                s.hayPolo[f] = true;
            }
            if (s.hayPolo[f]) {
                for (int k = 0; k < 3; k++)
                    pol[k] = K[k] + modelo[k] * s.polo[f][0] + modelo[3 + k] * s.polo[f][1] + modelo[6 + k] * s.polo[f][2];
            } else {
                for (int k = 0; k < 3; k++) pol[k] = K[k] + b[k];
            }
        }
        // (el objetivo nunca mas lejos que la pierna estirada: la rodilla no se traba derecha ni salta)
        {
            const float l1 = sqrtf((K[0] - H[0]) * (K[0] - H[0]) + (K[1] - H[1]) * (K[1] - H[1]) + (K[2] - H[2]) * (K[2] - H[2]));
            const float l2 = sqrtf((A[0] - K[0]) * (A[0] - K[0]) + (A[1] - K[1]) * (A[1] - K[1]) + (A[2] - K[2]) * (A[2] - K[2]));
            const float dh[3] = { T[0] - H[0], T[1] - H[1], T[2] - H[2] };
            const float dl = sqrtf(punto(dh, dh)), tope = (l1 + l2) * 0.999f;
            if (dl > tope && dl > 1e-6f) for (int k = 0; k < 3; k++) T[k] = H[k] + dh[k] * (tope / dl);
        }
        float K2[3], A2[3];
        W3dIkDosHuesos(H, K, A, T, pol, K2, A2);
        // del mundo al modelo y a giros de hueso: el muslo, la pantorrilla; el pie conserva su giro del modelo
        float Km[3], Am[3];
        al_mundo(I, K2, Km);
        al_mundo(I, A2, Am);
        float pieQ[4];
        memcpy(pieQ, m.q[P.tobillo], sizeof(pieQ));
        if (pesoGiro > 1e-3f && s.hayFrentePie[f]) {
            // (lo que giro el cuerpo desde que apoyo, alrededor del arriba, de vuelta en el pie)
            float cr[3] = { fr[1] * s.frente[f][2] - fr[2] * s.frente[f][1], fr[2] * s.frente[f][0] - fr[0] * s.frente[f][2],
                            fr[0] * s.frente[f][1] - fr[1] * s.frente[f][0] };
            const float ang = atan2f(punto(cr, U), punto(fr, s.frente[f])) * pesoGiro;
            float um[3];
            dir_a(I, U, um);
            const float lu = sqrtf(punto(um, um));
            if (lu > 1e-6f && fabsf(ang) > 1e-4f) {
                for (int k = 0; k < 3; k++) um[k] /= lu;
                float g[4], r[4];
                q_eje(um, ang, g);
                q_mul(g, pieQ, r);
                memcpy(pieQ, r, sizeof(r));
            }
        }
        float a[3], b[3], G[4];
        for (int k = 0; k < 3; k++) { a[k] = m.p[P.rodilla][k] - m.p[P.cadera][k]; b[k] = Km[k] - m.p[P.cadera][k]; }
        W3dIkGiroEntre(a, b, G);
        girar(e, q, m, P.cadera, G);
        fk(e, q, t, m);
        for (int k = 0; k < 3; k++) { a[k] = m.p[P.tobillo][k] - m.p[P.rodilla][k]; b[k] = Am[k] - m.p[P.rodilla][k]; }
        W3dIkGiroEntre(a, b, G);
        girar(e, q, m, P.rodilla, G);
        fk(e, q, t, m);
        float pi[4];
        q_inv(m.q[P.rodilla], pi);
        if (e.padre[P.tobillo] == P.rodilla) q_mul(pi, pieQ, q[P.tobillo]);
    }
}
