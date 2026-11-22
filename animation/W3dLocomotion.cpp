#include "animation/W3dLocomotion.h"
#include "physics/W3dRagdoll.h"
#include <math.h>
#include <string.h>

// ------------------------------------------------------------------------------------------------ cuentas
namespace {
const float GRAD = 0.017453293f;
enum { MAX_H = 160 };

inline float hit(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
inline float point(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void q_mul(const float* a, const float* b, float* o) {
    float r[4];
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    memcpy(o, r, sizeof(r));
}
inline void q_inv(const float* a, float* o) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
void q_axis(const float* axis, float ang, float* q) {
    const float s = sinf(ang * 0.5f);
    q[0] = axis[0] * s; q[1] = axis[1] * s; q[2] = axis[2] * s; q[3] = cosf(ang * 0.5f);
}
void q_rotate(const float* q, const float* v, float* o) {
    // v + 2w (u x v) + 2 u x (u x v)
    const float* u = q;
    float c[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    float d[3] = { u[1] * c[2] - u[2] * c[1], u[2] * c[0] - u[0] * c[2], u[0] * c[1] - u[1] * c[0] };
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = v[k] + 2 * (q[3] * c[k] + d[k]);
    memcpy(o, r, sizeof(r));
}

// las transformaciones en el modelo (giro y posicion de cada hueso)
struct Model {
    float q[MAX_H][4], p[MAX_H][3];
};

void fk(const W3dLocoSkeleton& e, float (*q)[4], float (*t)[3], Model& m) {
    for (int k = 0; k < e.n && k < MAX_H; k++) {
        const int i = e.order ? e.order[k] : k;
        const int pa = e.parent[i];
        if (pa < 0) {
            memcpy(m.q[i], q[i], sizeof(float) * 4);
            memcpy(m.p[i], t[i], sizeof(float) * 3);
        } else {
            q_mul(m.q[pa], q[i], m.q[i]);
            float d[3];
            q_rotate(m.q[pa], t[i], d);
            for (int j = 0; j < 3; j++) m.p[i][j] = m.p[pa][j] + d[j];
        }
    }
}

// gira el hueso h por G (en el modelo, alrededor de su origen): local' = P^-1 G P local
void rotate(const W3dLocoSkeleton& e, float (*q)[4], const Model& m, int h, const float* G) {
    float P[4] = { 0, 0, 0, 1 }, Pi[4], a[4], b[4];
    if (e.parent[h] >= 0) memcpy(P, m.q[e.parent[h]], sizeof(P));
    q_inv(P, Pi);
    q_mul(G, P, a);
    q_mul(Pi, a, b);
    q_mul(b, q[h], a);
    memcpy(q[h], a, sizeof(a));
}

// la afin 3x4 por columnas: punto y direccion al mundo, y de vuelta
void to_world(const float* M, const float* p, float* o) {
    for (int k = 0; k < 3; k++) o[k] = M[k] * p[0] + M[3 + k] * p[1] + M[6 + k] * p[2] + M[9 + k];
}
bool invert(const float* M, float* I) {
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
W3dLocoGait::W3dLocoGait() : speed(0), accelerates(150), frena(220), step(0) {}

void W3dLocoAdvance(W3dLocoGait& m, float wants, float velWalk, float velRun, float dt) {
    const float d = wants - m.speed;
    const float limit = (d > 0 ? m.accelerates : m.frena) * dt;
    m.speed += hit(d, -limit, limit);
    if (m.speed < 0) m.speed = 0;
    // (con histeresis: no cambia de clip en el borde)
    const float runs = (velWalk + velRun) * 0.5f;
    if (m.speed < velWalk * 0.08f) m.step = 0;
    else if (m.step == 2 && m.speed < runs * 0.9f) m.step = 1;
    else if (m.step != 2 && m.speed > runs * 1.1f && velRun > velWalk) m.step = 2;
    else if (m.step == 0 && m.speed > velWalk * 0.15f) m.step = 1;
}

float W3dLocoRhythm(float speed, float velClip, float min, float max) {
    if (velClip <= 1e-4f) return 1;
    return hit(speed / velClip, min, max);
}

// ------------------------------------------------------------------------------------------------ el giro
W3dLocoTurnDef::W3dLocoTurnDef() : threshold(55), velStep(160), velWalking(240), torsionMax(45) {}
W3dLocoTurn::W3dLocoTurn() : yaw(0), torsion(0), overwriting(false) {}

void W3dLocoRotate(W3dLocoTurn& g, float wants, bool walking, const W3dLocoTurnDef& d, float dt) {
    float e = wants - g.yaw;
    while (e > 180) e -= 360;
    while (e < -180) e += 360;
    if (walking) {
        g.overwriting = false;
        const float t = d.velWalking * dt;
        g.yaw += hit(e, -t, t);
        g.torsion += (0 - g.torsion) * hit(dt * 6, 0, 1);
        return;
    }
    if (!g.overwriting && fabsf(e) > d.threshold) g.overwriting = true;
    if (g.overwriting) {
        const float t = d.velStep * dt;
        g.yaw += hit(e, -t, t);
        if (fabsf(e) < 4) g.overwriting = false;
    }
    // (el torso hacia lo que falta, hasta su tope)
    float subtract = wants - g.yaw;
    while (subtract > 180) subtract -= 360;
    while (subtract < -180) subtract += 360;
    g.torsion += (hit(subtract, -d.torsionMax, d.torsionMax) - g.torsion) * hit(dt * 5, 0, 1);
}

// ------------------------------------------------------------------------------------------------ la mirada
void W3dLocoLook(const W3dLocoSkeleton& e, float (*q)[4], float (*t)[3], const W3dLocoChain& c, float yaw,
                  float pitch) {
    if (e.n > MAX_H || c.n <= 0 || (fabsf(yaw) < 0.05f && fabsf(pitch) < 0.05f)) return;
    static Model m;
    fk(e, q, t, m);
    float fixed[W3DLOCO_FIXED][4];
    for (int k = 0; k < c.nFixed; k++) memcpy(fixed[k], m.q[c.fixed[k]], sizeof(fixed[k]));
    for (int k = 0; k < c.n; k++) {
        const float y = hit(yaw * c.weight[k], -c.limitYaw[k], c.limitYaw[k]) * GRAD;
        const float p = hit(pitch * c.weight[k], -c.limitPitch[k], c.limitPitch[k]) * GRAD;
        float qy[4], qp[4], G[4];
        q_axis(c.up, y, qy);
        q_axis(c.right, p, qp);
        q_mul(qy, qp, G);
        rotate(e, q, m, c.bone[k], G);
        fk(e, q, t, m);
    }
    // (los fijos -las claviculas- vuelven a su giro del modelo: los brazos no siguen a la columna)
    for (int k = 0; k < c.nFixed; k++) {
        const int h = c.fixed[k], pa = e.parent[h];
        if (pa < 0) { memcpy(q[h], fixed[k], sizeof(fixed[k])); continue; }
        float pi[4];
        q_inv(m.q[pa], pi);
        q_mul(pi, fixed[k], q[h]);
    }
}

// ------------------------------------------------------------------------------------------------ los pies
W3dLocoFeetDef::W3dLocoFeetDef()
    : pelvis(-1), lowMax(0.4f), maxFloor(0.5f), stepHeight(0.04f), maxSlip(0.3f), maxTurn(25), marginMiddle(0.03f),
      durStep(0.2f), heightStep(0.06f), settle(0.03f), betweenSteps(0), stepVel(0), looseVel(0) {
    front[0] = front[1] = front[2] = 0;
    foot[0].hip = foot[0].knee = foot[0].ankle = -1;
    foot[1] = foot[0];
    up[0] = 0; up[1] = 1; up[2] = 0;
}

W3dLocoFeetState::W3dLocoFeetState() : pelvis(0), ready(false) {
    pinned[0] = pinned[1] = false;
    weight[0] = weight[1] = 0;
    minAnkle[0] = minAnkle[1] = 0;
    stepT[0] = stepT[1] = -1;
    hasPole[0] = hasPole[1] = false;
    fromStep = 1e3f;
    vel[0] = vel[1] = 0;
    memset(prev, 0, sizeof(prev));
    hasFrontFoot[0] = hasFrontFoot[1] = false;
    memset(pole, 0, sizeof(pole));
    memset(anchor, 0, sizeof(anchor));
    memset(stepOf, 0, sizeof(stepOf));
    memset(front, 0, sizeof(front));
}

void W3dLocoFeet(const W3dLocoSkeleton& e, float (*q)[4], float (*t)[3], const float* model,
                 const W3dLocoFeetDef& d, W3dLocoFeetState& s, W3dLocoGroundFn ground, void* user, bool pin,
                 float dt) {
    if (e.n > MAX_H || d.pelvis < 0 || !ground) return;
    for (int f = 0; f < 2; f++)
        if (d.foot[f].hip < 0 || d.foot[f].knee < 0 || d.foot[f].ankle < 0) return;
    float I[12];
    if (!invert(model, I)) return;
    static Model m;
    fk(e, q, t, m);
    const float* U = d.up;
    const float O[3] = { model[9], model[10], model[11] };
    // 1. cada pie: donde lo pone la animacion, su piso y cuanto se corre
    float A0[2][3], off[2];
    bool overwrites[2], loose[2];
    for (int f = 0; f < 2; f++) {
        to_world(model, m.p[d.foot[f].ankle], A0[f]);
        const float rel[3] = { A0[f][0] - O[0], A0[f][1] - O[1], A0[f][2] - O[2] };
        const float h = point(rel, U);
        if (!s.ready || h < s.minAnkle[f]) s.minAnkle[f] = h;
        else s.minAnkle[f] += (h - s.minAnkle[f]) * hit(dt * 0.05f, 0, 1);
        overwrites[f] = h - s.minAnkle[f] < d.stepHeight;
        loose[f] = !overwrites[f];
        if (d.stepVel > 0) {
            // (la velocidad sobre el piso del tobillo de la animacion: el que vuela no apoya aunque pase bajo)
            if (dt > 1e-5f) {
                float dv[3] = { A0[f][0] - s.prev[f][0], A0[f][1] - s.prev[f][1], A0[f][2] - s.prev[f][2] };
                const float u = point(dv, U);
                for (int k = 0; k < 3; k++) dv[k] -= U[k] * u;
                s.vel[f] = s.ready ? sqrtf(point(dv, dv)) / dt : 0;
                memcpy(s.prev[f], A0[f], sizeof(A0[f]));
            }
            overwrites[f] = overwrites[f] && s.vel[f] < d.stepVel;
            loose[f] = s.vel[f] > d.looseVel || h - s.minAnkle[f] > d.stepHeight * 5;
        }
        // (el punto del plano de los pies del modelo bajo el tobillo, y el piso de verdad ahi)
        const float S[3] = { A0[f][0] - U[0] * h, A0[f][1] - U[1] * h, A0[f][2] - U[2] * h };
        float G[3];
        off[f] = 0;
        if (ground(user, S, G)) {
            const float dg[3] = { G[0] - S[0], G[1] - S[1], G[2] - S[2] };
            const float o = point(dg, U);
            if (fabsf(o) <= d.maxFloor) off[f] = o;
        }
    }
    s.ready = true;
    // 2. la pelvis baja lo que pida el pie mas bajo
    float low = off[0] < off[1] ? off[0] : off[1];
    low = hit(low, -d.lowMax, 0);
    s.pelvis += (low - s.pelvis) * hit(dt * 10, 0, 1);
    if (fabsf(s.pelvis) > 1e-5f) {
        const float vu[3] = { U[0] * s.pelvis, U[1] * s.pelvis, U[2] * s.pelvis };
        float vm[3];
        dir_a(I, vu, vm);
        const int pa = e.parent[d.pelvis];
        if (pa >= 0) {
            float pi[4];
            q_inv(m.q[pa], pi);
            q_rotate(pi, vm, vm);
        }
        for (int k = 0; k < 3; k++) t[d.pelvis][k] += vm[k];
        fk(e, q, t, m);
    }
    // 3. cada pierna: el objetivo del tobillo (corrido a su piso, clavado donde apoyo o en el aire de un pasito) y la
    //    IK de dos huesos. El marco de la cadera: su punto medio y el eje de una cadera a la otra (sin el arriba), y un
    //    eje horizontal del cuerpo para medir cuanto giro desde que apoyo cada pie
    float Hw[2][3], middle[3], lat[3], fr[3];
    to_world(model, m.p[d.foot[0].hip], Hw[0]);
    to_world(model, m.p[d.foot[1].hip], Hw[1]);
    for (int k = 0; k < 3; k++) { middle[k] = (Hw[0][k] + Hw[1][k]) * 0.5f; lat[k] = Hw[0][k] - Hw[1][k]; }
    {
        const float u = point(lat, U);
        for (int k = 0; k < 3; k++) lat[k] -= U[k] * u;
        const float l = sqrtf(point(lat, lat));
        if (l > 1e-6f) for (int k = 0; k < 3; k++) lat[k] /= l;
        // (la columna del modelo mas horizontal)
        int c = 0;
        float best = 2;
        for (int j = 0; j < 3; j++) {
            const float n = sqrtf(point(model + j * 3, model + j * 3));
            const float a = n > 1e-9f ? fabsf(point(model + j * 3, U)) / n : 2;
            if (a < best) { best = a; c = j; }
        }
        const float v = point(model + c * 3, U);
        for (int k = 0; k < 3; k++) fr[k] = model[c * 3 + k] - U[k] * v;
        const float lf = sqrtf(point(fr, fr));
        if (lf > 1e-9f) for (int k = 0; k < 3; k++) fr[k] /= lf;
    }
    const float cosTurn = cosf(d.maxTurn * GRAD);
    s.fromStep += dt;
    for (int f = 0; f < 2; f++) {
        const W3dLocoFoot& P = d.foot[f];
        const float side_f = f == 0 ? 1.0f : -1.0f;
        float T[3] = { A0[f][0] + U[0] * off[f], A0[f][1] + U[1] * off[f], A0[f][2] + U[2] * off[f] };
        float obj[3];
        if (s.stepT[f] >= 0) {
            // (en el aire: de donde salio a donde lo pone la animacion ahora, en arco)
            s.stepT[f] += d.durStep > 1e-4f ? dt / d.durStep : 1;
            const float u = hit(s.stepT[f], 0, 1), uu = u * u * (3 - 2 * u);
            const float height = d.heightStep * sinf(u * 3.1415927f);
            for (int k = 0; k < 3; k++) obj[k] = s.stepOf[f][k] + (T[k] - s.stepOf[f][k]) * uu + U[k] * height;
            if (u >= 1) {
                s.stepT[f] = -1;
                memcpy(s.anchor[f], T, sizeof(T));
                memcpy(s.front[f], fr, sizeof(fr)); s.hasFrontFoot[f] = true;
                s.pinned[f] = pin && overwrites[f];
                s.weight[f] = s.pinned[f] ? 1.0f : 0.0f;
            }
        } else {
            if (s.pinned[f]) {
                float dx[3] = { T[0] - s.anchor[f][0], T[1] - s.anchor[f][1], T[2] - s.anchor[f][2] };
                const float side = sqrtf(hit(point(dx, dx) - point(dx, U) * point(dx, U), 0, 1e9f));
                const float ea[3] = { A0[f][0] - middle[0], A0[f][1] - middle[1], A0[f][2] - middle[2] };
                const float ec[3] = { s.anchor[f][0] - middle[0], s.anchor[f][1] - middle[1], s.anchor[f][2] - middle[2] };
                const float sAnim = point(ea, lat) * side_f, sAnchor = point(ec, lat) * side_f;
                // (cruzado: del otro lado de la linea media, o pasando, cuando la animacion lo tiene de su lado)
                const bool crossed = sAnchor < d.marginMiddle && sAnchor < sAnim - d.marginMiddle;
                const bool rotated = point(s.front[f], fr) < cosTurn;
                const bool misplaced = !pin && (side > d.settle || point(s.front[f], fr) < 0.985f);
                // (el otro ya casi apoya: este sale; y no mas seguido que 'betweenSteps')
                const bool other = (s.stepT[1 - f] >= 0 && s.stepT[1 - f] < 0.6f) || s.fromStep < d.betweenSteps;
                if (side > d.maxSlip * 4) {
                    // (lo pusieron en otro lado: apoya donde esta, sin pasito)
                    memcpy(s.anchor[f], T, sizeof(T));
                    memcpy(s.front[f], fr, sizeof(fr)); s.hasFrontFoot[f] = true;
                    s.pinned[f] = pin && overwrites[f];
                } else if (pin && loose[f]) {
                    s.pinned[f] = false;     // (la animacion lo levanta: el paso del clip)
                } else if ((crossed || rotated || side > d.maxSlip || misplaced) && !other) {
                    for (int k = 0; k < 3; k++) s.stepOf[f][k] = T[k] + (s.anchor[f][k] - T[k]) * s.weight[f];
                    s.stepT[f] = 0;
                    s.fromStep = 0;
                    s.pinned[f] = false;
                } else if (!pin && !misplaced && !crossed) {
                    s.pinned[f] = false;     // (casi donde lo pone la animacion: se suelta sin que se note)
                }
            } else if (pin && overwrites[f]) {
                if (d.stepVel > 0) {
                    // (apoya donde esta ahora, con lo que le quedaba de la vuelta del apoyo anterior: no salta)
                    float da[3] = { s.anchor[f][0] - T[0], s.anchor[f][1] - T[1], s.anchor[f][2] - T[2] };
                    const float u = point(da, U);
                    for (int k = 0; k < 3; k++) s.anchor[f][k] = T[k] + (da[k] - U[k] * u) * s.weight[f];
                    s.weight[f] = 1;
                } else {
                    memcpy(s.anchor[f], T, sizeof(T));
                }
                memcpy(s.front[f], fr, sizeof(fr)); s.hasFrontFoot[f] = true;
                s.pinned[f] = true;
            }
            if (s.stepT[f] >= 0) {
                memcpy(obj, s.stepOf[f], sizeof(obj));
                s.weight[f] = 0;
            } else {
                if (s.pinned[f]) s.weight[f] += (1 - s.weight[f]) * hit(dt * 12, 0, 1);
                else {
                    s.weight[f] += (0 - s.weight[f]) * hit(dt * 8, 0, 1);
                    if (s.weight[f] < 0.02f) memcpy(s.anchor[f], T, sizeof(T));
                }
                float da[3] = { s.anchor[f][0] - T[0], s.anchor[f][1] - T[1], s.anchor[f][2] - T[2] };
                if (d.stepVel > 0) {
                    // (fijo sobre el piso; la altura, la de la animacion)
                    const float u = point(da, U);
                    for (int k = 0; k < 3; k++) da[k] -= U[k] * u;
                }
                for (int k = 0; k < 3; k++) obj[k] = T[k] + da[k] * s.weight[f];
            }
        }
        memcpy(T, obj, sizeof(T));
        // (el giro del pie: clavado, el pie queda como apoyo en el mundo; en el aire vuelve de a poco al del clip)
        float turnWeight = 0;
        if (s.stepT[f] >= 0) {
            const float u = hit(s.stepT[f], 0, 1);
            turnWeight = 1 - u * u * (3 - 2 * u);
        } else {
            turnWeight = s.weight[f];
        }
        float H[3], K[3], A[3];
        to_world(model, m.p[P.hip], H);
        to_world(model, m.p[P.knee], K);
        to_world(model, m.p[P.ankle], A);
        const float dT[3] = { T[0] - A[0], T[1] - A[1], T[2] - A[2] };
        if (point(dT, dT) < 1e-8f && turnWeight < 1e-3f) continue;
        // (la rodilla hacia donde la dobla la animacion si esta bien doblada; con la pierna casi derecha -el idle- esa
        // direccion es ruido y daria vuelta la rodilla: hacia el frente del modelo, o la ultima buena)
        float pol[3];
        {
            const float b[3] = { K[0] - (H[0] + A[0]) * 0.5f, K[1] - (H[1] + A[1]) * 0.5f, K[2] - (H[2] + A[2]) * 0.5f };
            float bm[3];
            dir_a(I, b, bm);
            const float l = sqrtf(point(bm, bm));
            if (l > 0.04f) {
                for (int k = 0; k < 3; k++) s.pole[f][k] = bm[k] / l;
                s.hasPole[f] = true;
            } else if (point(d.front, d.front) > 1e-6f) {
                memcpy(s.pole[f], d.front, sizeof(d.front));
                s.hasPole[f] = true;
            }
            if (s.hasPole[f]) {
                for (int k = 0; k < 3; k++)
                    pol[k] = K[k] + model[k] * s.pole[f][0] + model[3 + k] * s.pole[f][1] + model[6 + k] * s.pole[f][2];
            } else {
                for (int k = 0; k < 3; k++) pol[k] = K[k] + b[k];
            }
        }
        // (el objetivo nunca mas lejos que la pierna estirada: la rodilla no se traba derecha ni salta)
        {
            const float l1 = sqrtf((K[0] - H[0]) * (K[0] - H[0]) + (K[1] - H[1]) * (K[1] - H[1]) + (K[2] - H[2]) * (K[2] - H[2]));
            const float l2 = sqrtf((A[0] - K[0]) * (A[0] - K[0]) + (A[1] - K[1]) * (A[1] - K[1]) + (A[2] - K[2]) * (A[2] - K[2]));
            const float dh[3] = { T[0] - H[0], T[1] - H[1], T[2] - H[2] };
            const float dl = sqrtf(point(dh, dh)), limit = (l1 + l2) * 0.999f;
            if (dl > limit && dl > 1e-6f) for (int k = 0; k < 3; k++) T[k] = H[k] + dh[k] * (limit / dl);
        }
        float K2[3], A2[3];
        W3dIkTwoBones(H, K, A, T, pol, K2, A2);
        // del mundo al modelo y a giros de hueso: el muslo, la pantorrilla; el pie conserva su giro del modelo
        float Km[3], Am[3];
        to_world(I, K2, Km);
        to_world(I, A2, Am);
        float footQ[4];
        memcpy(footQ, m.q[P.ankle], sizeof(footQ));
        if (turnWeight > 1e-3f && s.hasFrontFoot[f]) {
            // (lo que giro el cuerpo desde que apoyo, alrededor del arriba, de vuelta en el pie)
            float cr[3] = { fr[1] * s.front[f][2] - fr[2] * s.front[f][1], fr[2] * s.front[f][0] - fr[0] * s.front[f][2],
                            fr[0] * s.front[f][1] - fr[1] * s.front[f][0] };
            const float ang = atan2f(point(cr, U), point(fr, s.front[f])) * turnWeight;
            float um[3];
            dir_a(I, U, um);
            const float lu = sqrtf(point(um, um));
            if (lu > 1e-6f && fabsf(ang) > 1e-4f) {
                for (int k = 0; k < 3; k++) um[k] /= lu;
                float g[4], r[4];
                q_axis(um, ang, g);
                q_mul(g, footQ, r);
                memcpy(footQ, r, sizeof(r));
            }
        }
        float a[3], b[3], G[4];
        for (int k = 0; k < 3; k++) { a[k] = m.p[P.knee][k] - m.p[P.hip][k]; b[k] = Km[k] - m.p[P.hip][k]; }
        W3dIkTurnBetween(a, b, G);
        rotate(e, q, m, P.hip, G);
        fk(e, q, t, m);
        for (int k = 0; k < 3; k++) { a[k] = m.p[P.ankle][k] - m.p[P.knee][k]; b[k] = Am[k] - m.p[P.knee][k]; }
        W3dIkTurnBetween(a, b, G);
        rotate(e, q, m, P.knee, G);
        fk(e, q, t, m);
        float pi[4];
        q_inv(m.q[P.knee], pi);
        if (e.parent[P.ankle] == P.knee) q_mul(pi, footQ, q[P.ankle]);
    }
}
