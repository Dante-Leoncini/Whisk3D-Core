// ============================================================================
//  W3dRagdoll.cpp — ver W3dRagdoll.h. Dialecto C++03 (Symbian compila esto).
//  Comentarios en espanol sin acentos.
//
//  Las cuentas, de un vistazo (h = el paso fijo):
//    * Verlet:            x' = x + (x - xp) * (1 - amortiguar * h) + g * h^2
//    * las particulas:    x_i = centro + R * (SIGNO_i * brazo), un tetraedro centrado
//    * un punto P del cuerpo = sum(w_i * x_i) con sum(w_i) = 1; los pesos salen de sus
//      coordenadas en el marco de la forma: w_i = (1 + SIGNO_i . (local / brazo)) / 4
//    * corregir P en D:   x_i += invm * w_i * D / sum(invm * w_j^2)   (de los dos lados
//      de una articulacion con su masa)
//    * rigido otra vez:   el giro que mejor calza (Kabsch) por Muller (una vuelta por
//      iteracion, de primer orden, con el giro anterior de arranque) y las 4 particulas
//      puestas en su lugar alrededor del centro de masa de ahora
//    * los limites:       qJ = izq * giroPadre^-1 * giro * der (el reposo y el marco ya
//      juntados en izq y der), partido en giro propio (sobre x) y columpio (y, z) en
//      tangentes del medio angulo; cada componente al rango y lo que sobra repartido por
//      la inercia, cada cuerpo girando alrededor de su centro de masa
// ============================================================================
#include "physics/W3dRagdoll.h"
#include <math.h>
#include <string.h>

namespace {

const int   STEPS_MAX = 4;           // pasos por llamada como mucho (un cuadro lento)
const float ARM_MIN   = 0.02f;       // las particulas no mas cerca que esto del centro
const float STILL_VEL = 0.03f;       // velocidad (unidades por segundo) debajo de la cual un cuerpo esta quieto
const int   STILL_STEPS = 30;        // pasos quieto -> a dormir (medio segundo)
const int   LIFE_PLANE = 3;          // pasos que dura el plano que encontro la funcion del juego
const float MARGIN = 0.01f;          // lo que se alarga el segmento de cada punto (el contacto apoyado no se pierde)
// las 4 particulas: un tetraedro centrado en la forma (cuatro esquinas alternadas de la caja de semiejes 'brazo').
// El centro de masa es el de la forma y la inercia queda diagonal en sus ejes; con brazo = semieje / raiz de 3 es la
// de una caja maciza de esos semiejes.
const float SIGN[4][3] = { { 1, 1, 1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, 1 } };
const float INV_ROOT3 = 0.57735027f;

// ---------------------------------------------------------------- vectores
inline void v_copy(float* o, const float* a) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }
inline float v_point(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void v_cross(const float* a, const float* b, float* o) {
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
inline float v_length(const float* a) { return sqrtf(v_point(a, a)); }
inline void v_normalize(float* a) {
    const float l = v_length(a);
    if (l > 1e-12f) { a[0] /= l; a[1] /= l; a[2] /= l; }
}

// ---------------------------------------------------------------- cuaterniones (x y z w)
inline void q_identity(float* q) { q[0] = q[1] = q[2] = 0; q[3] = 1; }
inline void q_conj(const float* q, float* o) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
void q_mul(const float* a, const float* b, float* o) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
void q_normalize(float* q) {
    const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-12f) { q_identity(q); return; }
    q[0] /= l; q[1] /= l; q[2] /= l; q[3] /= l;
}
// las columnas de la matriz del giro (9 floats: x, y, z)
void q_a_matrix(const float* q, float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + w * z);     m[2] = 2 * (x * z - w * y);
    m[3] = 2 * (x * y - w * z);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z + w * x);
    m[6] = 2 * (x * z + w * y);     m[7] = 2 * (y * z - w * x);     m[8] = 1 - 2 * (x * x + y * y);
}
// de las columnas de una rotacion pura al cuaternion
void q_of_matrix(const float* m, float* q) {
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (m[5] - m[7]) / s; q[1] = (m[6] - m[2]) / s; q[2] = (m[1] - m[3]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[5] - m[7]) / s; q[0] = 0.25f * s; q[1] = (m[3] + m[1]) / s; q[2] = (m[6] + m[2]) / s;
    } else if (m[4] > m[8]) {
        const float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[6] - m[2]) / s; q[0] = (m[3] + m[1]) / s; q[1] = 0.25f * s; q[2] = (m[7] + m[5]) / s;
    } else {
        const float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[1] - m[3]) / s; q[0] = (m[6] + m[2]) / s; q[1] = (m[7] + m[5]) / s; q[2] = 0.25f * s;
    }
    q_normalize(q);
}
void q_rotate(const float* q, const float* v, float* o) {
    float m[9];
    q_a_matrix(q, m);
    const float x = v[0], y = v[1], z = v[2];
    o[0] = m[0] * x + m[3] * y + m[6] * z;
    o[1] = m[1] * x + m[4] * y + m[7] * z;
    o[2] = m[2] * x + m[5] * y + m[8] * z;
}

// ---------------------------------------------------------------- afines 3x4 por columnas
void a_point(const float* a, const float* p, float* o) {
    const float x = p[0], y = p[1], z = p[2];
    o[0] = a[0] * x + a[3] * y + a[6] * z + a[9];
    o[1] = a[1] * x + a[4] * y + a[7] * z + a[10];
    o[2] = a[2] * x + a[5] * y + a[8] * z + a[11];
}
// la traspuesta de la rotacion por un vector (el inverso de una rotacion pura)
void a_vector_t(const float* a, const float* p, float* o) {
    const float x = p[0], y = p[1], z = p[2];
    o[0] = a[0] * x + a[1] * y + a[2] * z;
    o[1] = a[3] * x + a[4] * y + a[5] * z;
    o[2] = a[6] * x + a[7] * y + a[8] * z;
}
void a_mul(const float* A, const float* B, float* R) {
    float r[12];
    for (int c = 0; c < 4; c++) {
        const float x = B[c * 3], y = B[c * 3 + 1], z = B[c * 3 + 2];
        r[c * 3] = A[0] * x + A[3] * y + A[6] * z;
        r[c * 3 + 1] = A[1] * x + A[4] * y + A[7] * z;
        r[c * 3 + 2] = A[2] * x + A[5] * y + A[8] * z;
    }
    r[9] += A[9]; r[10] += A[10]; r[11] += A[11];
    memcpy(R, r, sizeof(r));
}
// la inversa de una afin RIGIDA (rotacion + traslacion)
void a_inv_rigid(const float* a, float* o) {
    float r[12];
    r[0] = a[0]; r[1] = a[3]; r[2] = a[6];
    r[3] = a[1]; r[4] = a[4]; r[5] = a[7];
    r[6] = a[2]; r[7] = a[5]; r[8] = a[8];
    r[9] = -(a[0] * a[9] + a[1] * a[10] + a[2] * a[11]);
    r[10] = -(a[3] * a[9] + a[4] * a[10] + a[5] * a[11]);
    r[11] = -(a[6] * a[9] + a[7] * a[10] + a[8] * a[11]);
    memcpy(o, r, sizeof(r));
}
// sin escala (las columnas a largo 1, ortogonalizadas: z = x por y, y = z por x); devuelve las escalas
void a_rigid(const float* a, float* o, float* scale) {
    float x[3] = { a[0], a[1], a[2] }, y[3] = { a[3], a[4], a[5] }, z[3];
    if (scale) { scale[0] = v_length(a); scale[1] = v_length(a + 3); scale[2] = v_length(a + 6); }
    v_normalize(x);
    v_cross(x, y, z);
    v_normalize(z);
    v_cross(z, x, y);
    v_copy(o, x); v_copy(o + 3, y); v_copy(o + 6, z);
    o[9] = a[9]; o[10] = a[10]; o[11] = a[11];
}
void a_of_qt(const float* q, const float* t, float* a) {
    q_a_matrix(q, a);
    a[9] = t[0]; a[10] = t[1]; a[11] = t[2];
}

// el marco de la forma en mundo a partir del hueso (rigido): R = Rhueso * giro, t = hueso * centro
void shape_in_world(const W3dRagdollBone& d, const float* bone, float* shape) {
    float fl[12];
    a_of_qt(d.turn, d.center, fl);
    a_mul(bone, fl, shape);
}

// los pesos de un punto con esas coordenadas en el marco de la forma (la matriz de los signos es de Hadamard: su
// inversa es su traspuesta / 4); devuelve sum(w^2)
float weights(const float* local, const float* arm, float* w) {
    const float u = local[0] / arm[0], v = local[1] / arm[1], t = local[2] / arm[2];
    float s2 = 0;
    for (int i = 0; i < 4; i++) {
        w[i] = (1.0f + SIGN[i][0] * u + SIGN[i][1] * v + SIGN[i][2] * t) * 0.25f;
        s2 += w[i] * w[i];
    }
    return s2;
}

inline void center_of(const float (*x)[3], float* c) {
    for (int e = 0; e < 3; e++) c[e] = (x[0][e] + x[1][e] + x[2][e] + x[3][e]) * 0.25f;
}

// las 4 particulas de una forma con ese giro (columnas R) y ese centro
void set_particles(float (*x)[3], const float* R, const float* c, const float* arm) {
    for (int i = 0; i < 4; i++) {
        const float a = SIGN[i][0] * arm[0], b = SIGN[i][1] * arm[1], z = SIGN[i][2] * arm[2];
        for (int e = 0; e < 3; e++) x[i][e] = c[e] + R[e] * a + R[3 + e] * b + R[6 + e] * z;
    }
}

inline void mix(const float (*x)[3], const float* w, float* o) {
    for (int e = 0; e < 3; e++) o[e] = w[0] * x[0][e] + w[1] * x[1][e] + w[2] * x[2][e] + w[3] * x[3][e];
}

inline float limit(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

} // namespace

// ============================================================================ la definicion
W3dRagdollBone::W3dRagdollBone() {
    parent = -1;
    memset(rest, 0, sizeof(rest));
    rest[0] = rest[4] = rest[8] = 1.0f;
    shape = W3D_RAG_NO_SHAPE;
    semi[0] = semi[1] = semi[2] = 0.05f;
    axis = 0;
    center[0] = center[1] = center[2] = 0;
    q_identity(turn);
    mass = 1.0f;
    joint = W3D_RAG_KNEE;
    q_identity(frame);
    for (int k = 0; k < 3; k++) { min[k] = -0.5f; max[k] = 0.5f; }
}

W3dRagdollDef::W3dRagdollDef() {
    gravity[0] = 0; gravity[1] = -9.81f; gravity[2] = 0;
    damp     = 0.5f;
    friction = 0.6f;
    iterations = 4;
    step = 1.0f / 60.0f;
}

void W3dRagdollDef::Box(int h, const float* c, const float* s) {
    if (h < 0 || h >= (int)bones.size()) return;
    W3dRagdollBone& d = bones[h];
    d.shape = W3D_RAG_BOX;
    v_copy(d.center, c);
    v_copy(d.semi, s);
    q_identity(d.turn);
}

void W3dRagdollDef::Capsule(int h, const float* a, const float* b, float radius) {
    if (h < 0 || h >= (int)bones.size()) return;
    W3dRagdollBone& d = bones[h];
    d.shape = W3D_RAG_CAPSULE;
    float axis[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float length = v_length(axis);
    for (int e = 0; e < 3; e++) d.center[e] = (a[e] + b[e]) * 0.5f;
    d.semi[0] = radius;
    d.semi[1] = length * 0.5f;
    d.semi[2] = radius;
    d.axis = 0;
    // la x de la forma sobre el segmento
    const float x[3] = { 1, 0, 0 };
    if (length > 1e-9f) { v_normalize(axis); W3dIkTurnBetween(x, axis, d.turn); }
    else q_identity(d.turn);
}

void W3dRagdollDef::BallJoint(int h, float cone, float turnMin, float turnMax) {
    if (h < 0 || h >= (int)bones.size()) return;
    W3dRagdollBone& d = bones[h];
    d.joint = W3D_RAG_KNEE;
    d.min[0] = turnMin; d.max[0] = turnMax;
    d.min[1] = d.min[2] = -cone;
    d.max[1] = d.max[2] = cone;
}

void W3dRagdollDef::Hinge(int h, const float* axis, float mn, float mx) {
    if (h < 0 || h >= (int)bones.size()) return;
    W3dRagdollBone& d = bones[h];
    d.joint = W3D_RAG_HINGE;
    float e[3] = { axis[0], axis[1], axis[2] };
    v_normalize(e);
    const float x[3] = { 1, 0, 0 };
    W3dIkTurnBetween(x, e, d.frame);
    d.min[0] = mn; d.max[0] = mx;
    d.min[1] = d.max[1] = d.min[2] = d.max[2] = 0;
}

// ============================================================================ el muneco
W3dRagdoll::W3dRagdoll()
    : accum_(0), hasFloor_(false), stroke_(0), user_(0), asleep_(false), stillness_(0) {
    floor_[0] = 0; floor_[1] = 1; floor_[2] = 0; floor_[3] = 0;
}

W3dRagdoll::~W3dRagdoll() {}

void W3dRagdoll::Release() {
    bodies_.clear();
    bodyOf_.clear();
    followers_.clear();
    scale_.clear();
    worlds_.clear();
}

void W3dRagdoll::BuildBody(Body& c, int bone) {
    const W3dRagdollBone& d = def_.bones[bone];
    c.bone = bone;
    const float mass = d.mass > 1e-6f ? d.mass : 1.0f;
    c.invm = 4.0f / mass;
    const bool capsule = d.shape == W3D_RAG_CAPSULE;
    const int axis = d.axis < 0 || d.axis > 2 ? 0 : d.axis;
    // el tetraedro de las particulas: la inercia de la caja de la forma (la capsula: la de su caja envolvente)
    for (int k = 0; k < 3; k++) c.arm[k] = (capsule ? d.semi[0] : d.semi[k]) * INV_ROOT3;
    if (capsule) c.arm[axis] = (d.semi[1] + d.semi[0]) * INV_ROOT3;
    for (int k = 0; k < 3; k++)
        if (c.arm[k] < ARM_MIN) c.arm[k] = ARM_MIN;
    // la inercia del tetraedro sobre cada eje es masa * (los otros dos brazos al cuadrado): el promedio de los tres
    c.invI = 1.0f / (mass * (2.0f / 3.0f) * (c.arm[0] * c.arm[0] + c.arm[1] * c.arm[1] + c.arm[2] * c.arm[2]));
    // los puntos que chocan: las 8 esquinas de la caja o las 2 puntas del tramo recto de la capsula (con su radio)
    c.radius = capsule ? d.semi[0] : 0.0f;
    c.range = capsule ? d.semi[1] + d.semi[0] : v_length(d.semi);
    c.npts = capsule ? 2 : 8;
    for (int k = 0; k < c.npts; k++) {
        float l[3] = { 0, 0, 0 };
        if (capsule) l[axis] = k ? d.semi[1] : -d.semi[1];
        else {
            l[0] = (k & 1) ? d.semi[0] : -d.semi[0];
            l[1] = (k & 2) ? d.semi[1] : -d.semi[1];
            l[2] = (k & 4) ? d.semi[2] : -d.semi[2];
        }
        c.sp[k] = weights(l, c.arm, c.wp[k]);
    }
    for (int k = 0; k < 8; k++) { c.life[k] = 0; c.touches[k] = false; }
    // el pivote del lado del hijo: el origen del hueso en el marco de la forma
    float gi[4], l[3];
    const float minusC[3] = { -d.center[0], -d.center[1], -d.center[2] };
    q_conj(d.turn, gi);
    q_rotate(gi, minusC, l);
    c.sPiv = weights(l, c.arm, c.wPiv);
}

bool W3dRagdoll::Build(const W3dRagdollDef& def) {
    Release();
    def_ = def;
    const int n = (int)def_.bones.size();
    if (n <= 0) return false;
    bodyOf_.assign(n, -1);
    // los cuerpos en orden de profundidad (los padres antes que los hijos)
    std::vector<int> prof(n, 0);
    int maxProf = 0;
    for (int i = 0; i < n; i++) {
        int p = def_.bones[i].parent, k = 0;
        while (p >= 0 && p < n && k < n) { k++; p = def_.bones[p].parent; }
        prof[i] = k;
        if (k > maxProf) maxProf = k;
    }
    for (int level = 0; level <= maxProf; level++)
        for (int i = 0; i < n; i++) {
            if (prof[i] != level || def_.bones[i].shape == W3D_RAG_NO_SHAPE) continue;
            Body c;
            memset(&c, 0, sizeof(c));
            BuildBody(c, i);
            q_identity(c.q);
            bodyOf_[i] = (int)bodies_.size();
            bodies_.push_back(c);
        }
    if (bodies_.empty()) return false;
    // el padre de cada cuerpo: el ancestro mas cercano con forma, y el giro de reposo entre los dos huesos
    for (size_t k = 0; k < bodies_.size(); k++) {
        Body& c = bodies_[k];
        c.parent = -1;
        int p = def_.bones[c.bone].parent, turns = 0;
        while (p >= 0 && p < n && turns++ < n) {
            if (bodyOf_[p] >= 0) { c.parent = bodyOf_[p]; break; }
            p = def_.bones[p].parent;
        }
        BuildJoint(c);
    }
    // los huesos sin forma: su ancla (el cuerpo del ancestro mas cercano; si no tiene, el cuerpo mas alto de su
    // descendencia; si tampoco, el primero)
    for (int i = 0; i < n; i++) {
        if (bodyOf_[i] >= 0) continue;
        Follower s;
        s.bone   = i;
        s.anchor = -1;
        int p = def_.bones[i].parent, turns = 0;
        while (p >= 0 && p < n && turns++ < n && s.anchor < 0) {
            if (bodyOf_[p] >= 0) s.anchor = bodyOf_[p];
            p = def_.bones[p].parent;
        }
        for (size_t k = 0; k < bodies_.size() && s.anchor < 0; k++) {
            int a = def_.bones[bodies_[k].bone].parent, v = 0;
            while (a >= 0 && a < n && v++ < n && a != i) a = def_.bones[a].parent;
            if (a == i) s.anchor = (int)k;
        }
        if (s.anchor < 0) s.anchor = 0;
        memset(s.rel, 0, sizeof(s.rel));
        s.rel[0] = s.rel[4] = s.rel[8] = 1.0f;
        followers_.push_back(s);
    }
    scale_.assign(n * 3, 1.0f);
    worlds_.assign(n * 12, 0.0f);
    for (int i = 0; i < n; i++) memcpy(&worlds_[i * 12], def_.bones[i].rest, sizeof(float) * 12);
    accum_ = 0;
    asleep_    = true;
    stillness_ = 0;
    return true;
}

// qJ = marco^-1 * L0^-1 * (giroHuesoPadre^-1 * giroHueso) * marco, con giroHueso = q * giroForma^-1:
// izq = marco^-1 * L0^-1 * giroFormaPadre y der = giroForma^-1 * marco (L0 = el giro del hueso contra el del padre
// en reposo)
void W3dRagdoll::BuildJoint(Body& c) {
    const W3dRagdollBone& d = def_.bones[c.bone];
    q_identity(c.left);
    q_identity(c.right);
    if (c.parent < 0) return;
    const W3dRagdollBone& dp = def_.bones[bodies_[c.parent].bone];
    float r[12], qp[4], qc[4], t[4], L0[4], L0i[4], mi[4], gi[4];
    a_rigid(dp.rest, r, 0);
    q_of_matrix(r, qp);
    a_rigid(d.rest, r, 0);
    q_of_matrix(r, qc);
    q_conj(qp, t);
    q_mul(t, qc, L0);
    q_conj(L0, L0i);
    q_conj(d.frame, mi);
    q_mul(mi, L0i, t);
    q_mul(t, dp.turn, c.left);
    q_conj(d.turn, gi);
    q_mul(gi, d.frame, c.right);
    // (los limites en tangentes del medio angulo: sin trigonometria en cada vuelta; hasta casi media vuelta)
    for (int k = 0; k < 3; k++) {
        const float mn = limit(d.min[k], -3.1f, 3.1f), mx = limit(d.max[k], -3.1f, 3.1f);
        c.tanMin[k] = tanf(mn * 0.5f);
        c.tanMax[k] = tanf((mx < mn ? mn : mx) * 0.5f);
    }
    if (d.joint == W3D_RAG_HINGE) c.tanMin[1] = c.tanMax[1] = c.tanMin[2] = c.tanMax[2] = 0;
}

void W3dRagdoll::ChangeLimits(int bone, const W3dRagdollBone& h) {
    if (bone < 0 || bone >= Bones() || bodyOf_[bone] < 0) return;
    W3dRagdollBone& d = def_.bones[bone];
    d.joint = h.joint;
    memcpy(d.frame, h.frame, sizeof(d.frame));
    memcpy(d.min, h.min, sizeof(d.min));
    memcpy(d.max, h.max, sizeof(d.max));
    BuildJoint(bodies_[bodyOf_[bone]]);
    Wake();
}

// ---------------------------------------------------------------- de matrices a particulas y vuelta
void W3dRagdoll::MatrixToBody(Body& c, const float* bone, bool prev) {
    float f[12];
    shape_in_world(def_.bones[c.bone], bone, f);
    set_particles(prev ? c.xp : c.x, f, f + 9, c.arm);
    if (!prev) q_of_matrix(f, c.q);
}

void W3dRagdoll::BodyToMatrix(const Body& c, bool prev, float* bone) const {
    const W3dRagdollBone& d = def_.bones[c.bone];
    const float (*x)[3] = prev ? c.xp : c.x;
    // el centro de la forma es el centro de masa de las 4
    float R[9], center[3];
    q_a_matrix(c.q, R);
    center_of(x, center);
    // hueso = forma * (giro, centro)^-1
    float f[12], fl[12], fli[12];
    memcpy(f, R, sizeof(R));
    v_copy(f + 9, center);
    a_of_qt(d.turn, d.center, fl);
    a_inv_rigid(fl, fli);
    a_mul(f, fli, bone);
}

// el pivote del lado del padre: el origen del hueso hijo de ahora, en el marco de la forma del padre
void W3dRagdoll::PivotsOf(Body& c) {
    if (c.parent < 0) return;
    const Body& p = bodies_[c.parent];
    float piv[3], R[9], cm[3], l[3], d[3];
    mix(c.x, c.wPiv, piv);
    q_a_matrix(p.q, R);
    center_of(p.x, cm);
    for (int e = 0; e < 3; e++) d[e] = piv[e] - cm[e];
    l[0] = R[0] * d[0] + R[1] * d[1] + R[2] * d[2];
    l[1] = R[3] * d[0] + R[4] * d[1] + R[5] * d[2];
    l[2] = R[6] * d[0] + R[7] * d[1] + R[8] * d[2];
    c.sPivParent = weights(l, p.arm, c.wPivParent);
}

void W3dRagdoll::BuildWorlds() {
    const int n = Bones();
    for (size_t k = 0; k < bodies_.size(); k++) {
        float* m = &worlds_[bodies_[k].bone * 12];
        BodyToMatrix(bodies_[k], false, m);
        const float* s = &scale_[bodies_[k].bone * 3];
        for (int e = 0; e < 3; e++) { m[e] *= s[0]; m[3 + e] *= s[1]; m[6 + e] *= s[2]; }
    }
    for (size_t k = 0; k < followers_.size(); k++) {
        const Follower& s = followers_[k];
        float a[12];
        BodyToMatrix(bodies_[s.anchor], false, a);
        a_mul(a, s.rel, &worlds_[s.bone * 12]);
    }
    (void)n;
}

// ---------------------------------------------------------------- empezar a caer
void W3dRagdoll::Begin(const float* worlds, const float* prev, float dtPrev) {
    if (bodies_.empty() || !worlds) return;
    float rig[12], rigA[12];
    for (size_t k = 0; k < bodies_.size(); k++) {
        Body& c = bodies_[k];
        a_rigid(worlds + c.bone * 12, rig, &scale_[c.bone * 3]);
        MatrixToBody(c, rig, false);
        if (prev && dtPrev > 1e-5f) {
            a_rigid(prev + c.bone * 12, rigA, 0);
            MatrixToBody(c, rigA, true);
            // la velocidad de la animacion, al paso fijo
            float k2 = FixedStep() / dtPrev;
            if (k2 > 1.0f) k2 = 1.0f;
            for (int i = 0; i < 4; i++)
                for (int e = 0; e < 3; e++) c.xp[i][e] = c.x[i][e] - (c.x[i][e] - c.xp[i][e]) * k2;
        } else {
            memcpy(c.xp, c.x, sizeof(c.x));
        }
        for (int i = 0; i < 8; i++) { c.life[i] = 0; c.touches[i] = false; }
    }
    for (size_t k = 0; k < bodies_.size(); k++) PivotsOf(bodies_[k]);
    for (size_t k = 0; k < followers_.size(); k++) {
        Follower& s = followers_[k];
        float a[12], ai[12];
        a_rigid(worlds + bodies_[s.anchor].bone * 12, a, 0);
        a_inv_rigid(a, ai);
        a_mul(ai, worlds + s.bone * 12, s.rel);
    }
    accum_ = 0;
    asleep_    = false;
    stillness_ = 0;
    BuildWorlds();
}

void W3dRagdoll::Floor(const float* n, float d) {
    hasFloor_ = true;
    floor_[0] = n[0]; floor_[1] = n[1]; floor_[2] = n[2]; floor_[3] = d;
    Wake();
}

bool W3dRagdoll::BodyOf(int bone, float* center, float* vel, float* mass) const {
    if (bodies_.empty() || bone < 0 || bone >= Bones()) return false;
    int ci = bodyOf_[bone];
    for (size_t k = 0; ci < 0 && k < followers_.size(); k++)
        if (followers_[k].bone == bone) ci = followers_[k].anchor;
    if (ci < 0) return false;
    const Body& c = bodies_[ci];
    float a[3], b[3];
    center_of(c.x, a);
    center_of(c.xp, b);
    if (center) v_copy(center, a);
    if (vel)
        for (int e = 0; e < 3; e++) vel[e] = (a[e] - b[e]) / FixedStep();
    if (mass) *mass = 4.0f / c.invm;
    return true;
}

void W3dRagdoll::Impulse(int bone, const float* point, const float* imp) {
    if (bodies_.empty() || bone < 0 || bone >= Bones()) return;
    int ci = bodyOf_[bone];
    for (size_t k = 0; ci < 0 && k < followers_.size(); k++)
        if (followers_[k].bone == bone) ci = followers_[k].anchor;
    if (ci < 0) return;
    Body& c = bodies_[ci];
    float R[9], cm[3], d[3], l[3], w[4];
    q_a_matrix(c.q, R);
    center_of(c.x, cm);
    for (int e = 0; e < 3; e++) d[e] = point[e] - cm[e];
    l[0] = R[0] * d[0] + R[1] * d[1] + R[2] * d[2];
    l[1] = R[3] * d[0] + R[4] * d[1] + R[5] * d[2];
    l[2] = R[6] * d[0] + R[7] * d[1] + R[8] * d[2];
    // (el punto dentro de la forma: un punto lejos del cuerpo lo haria girar como loco)
    for (int e = 0; e < 3; e++) l[e] = limit(l[e], -c.arm[e] * 1.7320508f, c.arm[e] * 1.7320508f);
    weights(l, c.arm, w);
    // dv_i = invm * w_i * J (el ajuste rigido lo reparte: el centro de masa recibe J / masa)
    for (int i = 0; i < 4; i++)
        for (int e = 0; e < 3; e++) c.xp[i][e] -= c.invm * w[i] * imp[e] * FixedStep();
    Wake();
}

void W3dRagdoll::ChangeShape(int bone, const W3dRagdollBone& h) {
    if (bone < 0 || bone >= Bones() || bodyOf_[bone] < 0) return;
    Body& c = bodies_[bodyOf_[bone]];
    float B[12], Bp[12];
    BodyToMatrix(c, false, B);
    BodyToMatrix(c, true, Bp);
    W3dRagdollBone& d = def_.bones[bone];
    d.shape = h.shape == W3D_RAG_NO_SHAPE ? W3D_RAG_BOX : h.shape;
    memcpy(d.semi, h.semi, sizeof(d.semi));
    d.axis = h.axis;
    memcpy(d.center, h.center, sizeof(d.center));
    memcpy(d.turn, h.turn, sizeof(d.turn));
    d.mass = h.mass;
    BuildBody(c, bone);
    MatrixToBody(c, Bp, true);
    MatrixToBody(c, B, false);
    PivotsOf(c);
    BuildJoint(c);
    for (size_t k = 0; k < bodies_.size(); k++)
        if (bodies_[k].parent == bodyOf_[bone]) { PivotsOf(bodies_[k]); BuildJoint(bodies_[k]); }
    BuildWorlds();
    Wake();
}

// ---------------------------------------------------------------- el paso
void W3dRagdoll::Step(float dt) {
    if (bodies_.empty() || asleep_ || dt <= 0) return;
    const float h = FixedStep();
    accum_ += dt;
    int n = 0;
    while (accum_ >= h && n < STEPS_MAX) {
        Substep(h);
        accum_ -= h;
        n++;
        if (asleep_) break;
    }
    if (accum_ >= h) accum_ = 0;   // (un cuadro muy lento: lo que sobra se pierde)
    if (n) BuildWorlds();
}

void W3dRagdoll::Substep(float h) {
    const float k = 1.0f - def_.damp * h;
    const float g[3] = { def_.gravity[0] * h * h, def_.gravity[1] * h * h, def_.gravity[2] * h * h };
    for (size_t b = 0; b < bodies_.size(); b++) {
        Body& c = bodies_[b];
        for (int i = 0; i < 4; i++)
            for (int e = 0; e < 3; e++) {
                const float x = c.x[i][e];
                c.x[i][e] += (x - c.xp[i][e]) * k + g[e];
                c.xp[i][e] = x;
            }
        for (int p = 0; p < c.npts; p++) c.touches[p] = false;
    }
    Contacts();
    const int it = def_.iterations > 0 ? def_.iterations : 1;
    for (int i = 0; i < it; i++) Iteration(i == it - 1);
    Friction();
    // quieto?
    float v2 = 0;
    for (size_t b = 0; b < bodies_.size(); b++) {
        const Body& c = bodies_[b];
        for (int i = 0; i < 4; i++) {
            const float d[3] = { c.x[i][0] - c.xp[i][0], c.x[i][1] - c.xp[i][1], c.x[i][2] - c.xp[i][2] };
            const float l = v_point(d, d);
            if (l > v2) v2 = l;
        }
    }
    const float limit = STILL_VEL * h;
    if (v2 < limit * limit) {
        if (++stillness_ >= STILL_STEPS) {
            asleep_ = true;
            for (size_t b = 0; b < bodies_.size(); b++) memcpy(bodies_[b].xp, bodies_[b].x, sizeof(bodies_[b].x));
        }
    } else {
        stillness_ = 0;
    }
}

// la funcion del juego, una vez por punto: del CENTRO de la forma al punto (alargado su radio y un poco mas). El
// centro esta siempre afuera del mundo (adentro del cuerpo), asi que una esquina que quedo metida en una pared se
// encuentra igual, con el plano por el que entro.
void W3dRagdoll::Contacts() {
    for (size_t b = 0; b < bodies_.size(); b++) {
        Body& c = bodies_[b];
        float a[3];
        center_of(c.x, a);
        for (int p = 0; p < c.npts; p++) {
            if (c.life[p] > 0) c.life[p]--;
            if (!stroke_) continue;
            float z[3];
            mix(c.x, c.wp[p], z);
            float d[3] = { z[0] - a[0], z[1] - a[1], z[2] - a[2] };
            const float l = v_length(d);
            if (l < 1e-6f) continue;
            const float extra = (c.radius + MARGIN) / l;
            for (int e = 0; e < 3; e++) z[e] += d[e] * extra;
            float point[3], n[3];
            if (stroke_(user_, a, z, point, n)) {
                v_normalize(n);
                v_copy(c.plane[p], n);
                c.plane[p][3] = v_point(n, point);
                c.life[p] = LIFE_PLANE;
            }
        }
    }
}

void W3dRagdoll::Iteration(bool last) {
    // 1. los pivotes de las articulaciones (el hijo y el padre se reparten la correccion por la masa)
    for (size_t b = 0; b < bodies_.size(); b++) {
        Body& c = bodies_[b];
        if (c.parent < 0) continue;
        Body& p = bodies_[c.parent];
        float pc[3], pp[3];
        mix(c.x, c.wPiv, pc);
        mix(p.x, c.wPivParent, pp);
        const float s = c.invm * c.sPiv + p.invm * c.sPivParent;
        if (s < 1e-12f) continue;
        const float l[3] = { (pp[0] - pc[0]) / s, (pp[1] - pc[1]) / s, (pp[2] - pc[2]) / s };
        for (int i = 0; i < 4; i++) {
            const float kc = c.invm * c.wPiv[i], kp = p.invm * c.wPivParent[i];
            for (int e = 0; e < 3; e++) {
                c.x[i][e] += kc * l[e];
                p.x[i][e] -= kp * l[e];
            }
        }
    }
    // 2. los choques (el piso en cada vuelta; los planos de la funcion del juego mientras duren)
    for (int turn = 0; turn < (last ? 2 : 1); turn++) {
        for (size_t b = 0; b < bodies_.size(); b++) {
            Body& c = bodies_[b];
            // (el piso solo si la forma llega: la distancia del centro contra lo que mide)
            bool floor = hasFloor_;
            if (floor) {
                float cm[3];
                center_of(c.x, cm);
                floor = v_point(floor_, cm) - floor_[3] < c.range + c.radius;
            }
            // (la altura de cada particula sobre el piso: la de cada punto es la mezcla de las cuatro)
            float hp[4] = { 0, 0, 0, 0 };
            if (floor)
                for (int i = 0; i < 4; i++) hp[i] = v_point(floor_, c.x[i]) - floor_[3];
            for (int p = 0; p < c.npts; p++) {
                for (int plane = 0; plane < 2; plane++) {
                    const float* pl = plane == 0 ? (floor ? floor_ : 0) : (c.life[p] > 0 ? c.plane[p] : 0);
                    if (!pl) continue;
                    const float* w = c.wp[p];
                    float d;
                    if (plane == 0) d = w[0] * hp[0] + w[1] * hp[1] + w[2] * hp[2] + w[3] * hp[3] - c.radius;
                    else {
                        float P[3];
                        mix(c.x, w, P);
                        d = v_point(pl, P) - pl[3] - c.radius;
                    }
                    if (d >= 0) continue;
                    const float k = -d / c.sp[p];
                    for (int i = 0; i < 4; i++) {
                        for (int e = 0; e < 3; e++) c.x[i][e] += w[i] * k * pl[e];
                        if (floor) hp[i] = v_point(floor_, c.x[i]) - floor_[3];
                    }
                    // (la friccion usa la normal del plano que toco; un plano de la funcion del juego que todavia
                    // empuja sigue vivo: lo apoyado no se suelta)
                    if (plane == 0) { memcpy(c.plane[p], floor_, sizeof(floor_)); if (c.life[p] < 1) c.life[p] = 1; }
                    else if (c.life[p] < LIFE_PLANE) c.life[p] = LIFE_PLANE;
                    c.touches[p] = true;
                }
            }
        }
        // 3. rigidos otra vez
        for (size_t b = 0; b < bodies_.size(); b++) MakeRigid(bodies_[b]);
        if (turn == 0)
            // 4. los limites del giro
            for (size_t b = 0; b < bodies_.size(); b++) Limit(bodies_[b]);
    }
}

// el giro que mejor calza las 4 particulas (Muller, "A Robust Method to Extract the Rotational Part of
// Deformations"), y las particulas a su lugar alrededor del centro de masa
void W3dRagdoll::MakeRigid(Body& c) {
    float cm[3];
    center_of(c.x, cm);
    // A = sum((x_i - cm) * r_i^T), con r_i la particula en reposo (signo * brazo): A = R * S con S simetrica
    float A[3][3];
    for (int k = 0; k < 3; k++)
        for (int e = 0; e < 3; e++) {
            float a = 0;
            for (int i = 0; i < 4; i++) a += SIGN[i][k] * (c.x[i][e] - cm[e]);
            A[k][e] = a * c.arm[k];
        }
    float R[9];
    for (int turn = 0; turn < 1; turn++) {   // (una por iteracion alcanza: arranca del giro de la anterior)
        q_a_matrix(c.q, R);
        float w[3] = { 0, 0, 0 }, den = 0, t[3];
        for (int k = 0; k < 3; k++) {
            v_cross(R + k * 3, A[k], t);
            w[0] += t[0]; w[1] += t[1]; w[2] += t[2];
            den += v_point(R + k * 3, A[k]);
        }
        den = fabsf(den) + 1e-12f;
        w[0] /= den; w[1] /= den; w[2] /= den;
        const float length = v_length(w);
        if (!(length > 1e-6f && length < 1e6f)) break;   // (quieto, o las particulas aplastadas: queda el giro de antes)
        // el giro chico de primer orden (sin trigonometria), de a un radian por vuelta como mucho
        const float k = (length < 1.0f ? 1.0f : 1.0f / length) * 0.5f;
        float dq[4] = { w[0] * k, w[1] * k, w[2] * k, 1.0f }, nq[4];
        q_mul(dq, c.q, nq);
        q_normalize(nq);
        memcpy(c.q, nq, sizeof(nq));
    }
    q_a_matrix(c.q, R);
    set_particles(c.x, R, cm, c.arm);
}

// gira las particulas del cuerpo por q alrededor de 'piv'
static void rotate_particles(float (*x)[3], float* qc, const float* q, const float* piv) {
    float m[9];
    q_a_matrix(q, m);
    for (int i = 0; i < 4; i++) {
        const float d[3] = { x[i][0] - piv[0], x[i][1] - piv[1], x[i][2] - piv[2] };
        for (int e = 0; e < 3; e++) x[i][e] = piv[e] + m[e] * d[0] + m[3 + e] * d[1] + m[6 + e] * d[2];
    }
    float n[4];
    q_mul(q, qc, n);
    q_normalize(n);
    memcpy(qc, n, sizeof(n));
}

void W3dRagdoll::Limit(Body& c) {
    if (c.parent < 0 || def_.bones[c.bone].joint == W3D_RAG_FREE) return;
    Body& p = bodies_[c.parent];
    // el giro de la articulacion contra el reposo, en su marco
    float pi[4], t[4], u[4], qJ[4];
    q_conj(p.q, pi);
    q_mul(c.left, pi, t);
    q_mul(t, c.q, u);
    q_mul(u, c.right, qJ);
    if (qJ[3] < 0) { qJ[0] = -qJ[0]; qJ[1] = -qJ[1]; qJ[2] = -qJ[2]; qJ[3] = -qJ[3]; }
    // el giro propio (sobre x) y el columpio (qJ = columpio * propio), en tangentes del medio angulo:
    // propio = (tx, 0, 0, 1) y columpio = (0, ty, tz, 1) normalizados
    const float l = sqrtf(qJ[0] * qJ[0] + qJ[3] * qJ[3]);
    if (l < 1e-6f) return;   // (media vuelta justa: no hay giro propio que medir)
    const float a = qJ[3] / l, b = qJ[0] / l;   // propio = (b, 0, 0, a)
    // columpio = qJ * propio^-1
    const float sw0 = qJ[3] * a + qJ[0] * b;
    const float sy = (qJ[1] * a - qJ[2] * b), sz = (qJ[2] * a + qJ[1] * b);
    const float tx = b / a;
    const float ty = sw0 > 1e-4f ? sy / sw0 : (sy >= 0 ? 1e4f : -1e4f);
    const float tz = sw0 > 1e-4f ? sz / sw0 : (sz >= 0 ? 1e4f : -1e4f);
    const float nx = limit(tx, c.tanMin[0], c.tanMax[0]);
    const float ny = limit(ty, c.tanMin[1], c.tanMax[1]);
    const float nz = limit(tz, c.tanMin[2], c.tanMax[2]);
    if (nx == tx && ny == ty && nz == tz) return;
    // el giro dentro del rango, y lo que falta: d = nuevo * qJ^-1, llevado al mundo por W = p.q * izq^-1
    float tw[4] = { nx, 0, 0, 1 }, sw[4] = { 0, ny, nz, 1 }, nqJ[4], qJi[4], dJ[4], W[4], Wi[4], izqi[4], dq[4];
    q_normalize(tw);
    q_normalize(sw);
    q_mul(sw, tw, nqJ);
    q_conj(qJ, qJi);
    q_mul(nqJ, qJi, dJ);
    q_conj(c.left, izqi);
    q_mul(p.q, izqi, W);
    q_conj(W, Wi);
    q_mul(W, dJ, t);
    q_mul(t, Wi, dq);
    if (dq[3] < 0) { dq[0] = -dq[0]; dq[1] = -dq[1]; dq[2] = -dq[2]; dq[3] = -dq[3]; }
    // repartido por la inercia: cada cuerpo gira alrededor de SU centro de masa (no se mueve ninguno de los dos
    // centros: no se crea momento lineal) y el pivote, que se separa un poco, lo junta la articulacion en la vuelta
    // siguiente. La fraccion de un giro chico: lineal desde la identidad y normalizado.
    const float fc = c.invI / (c.invI + p.invI), fp = 1.0f - fc;
    float dc[4] = { dq[0] * fc, dq[1] * fc, dq[2] * fc, 1.0f - fc + dq[3] * fc };
    float dp[4] = { -dq[0] * fp, -dq[1] * fp, -dq[2] * fp, 1.0f - fp + dq[3] * fp };
    q_normalize(dc);
    q_normalize(dp);
    float cc[3], cp[3];
    center_of(c.x, cc);
    center_of(p.x, cp);
    rotate_particles(c.x, c.q, dc, cc);
    rotate_particles(p.x, p.q, dp, cp);
}

// la friccion: lo tangencial de cada punto que toca se frena (cambiando su posicion anterior: solo la velocidad)
void W3dRagdoll::Friction() {
    const float mu = limit(def_.friction, 0, 1);
    if (mu <= 0) return;
    for (size_t b = 0; b < bodies_.size(); b++) {
        Body& c = bodies_[b];
        for (int p = 0; p < c.npts; p++) {
            if (!c.touches[p]) continue;
            const float* n = c.plane[p];
            float P[3], A[3];
            mix(c.x, c.wp[p], P);
            mix(c.xp, c.wp[p], A);
            float v[3] = { P[0] - A[0], P[1] - A[1], P[2] - A[2] };
            const float vn = v_point(v, n);
            for (int e = 0; e < 3; e++) v[e] = (v[e] - n[e] * vn) * mu / c.sp[p];
            for (int i = 0; i < 4; i++)
                for (int e = 0; e < 3; e++) c.xp[i][e] += c.wp[p][i] * v[e];
        }
    }
}

// ============================================================================ el rayo y las lineas
namespace {
// el rayo (local) contra una esfera; t >= 0 o -1
float ray_sphere(const float* o, const float* d, const float* c, float r) {
    const float m[3] = { o[0] - c[0], o[1] - c[1], o[2] - c[2] };
    const float b = v_point(m, d), cc = v_point(m, m) - r * r;
    if (cc > 0 && b > 0) return -1;
    const float disc = b * b - cc;
    if (disc < 0) return -1;
    const float t = -b - sqrtf(disc);
    return t < 0 ? -1 : t;
}
}

bool W3dRagdollRay(const W3dRagdollDef& def, const float* worlds, const float* origin, const float* dir,
                    float maxDist, W3dRagdollImpact* out) {
    if (!worlds) return false;
    float best = maxDist;
    bool touched = false;
    for (size_t i = 0; i < def.bones.size(); i++) {
        const W3dRagdollBone& d = def.bones[i];
        if (d.shape == W3D_RAG_NO_SHAPE) continue;
        float B[12], F[12];
        a_rigid(worlds + i * 12, B, 0);
        shape_in_world(d, B, F);
        // el rayo en el marco de la forma
        const float r[3] = { origin[0] - F[9], origin[1] - F[10], origin[2] - F[11] };
        float o[3], v[3];
        a_vector_t(F, r, o);
        a_vector_t(F, dir, v);
        float t = -1, nl[3] = { 0, 0, 0 };
        if (d.shape == W3D_RAG_BOX) {
            float t0 = 0, t1 = best;
            int axis = -1;
            float sign = 0;
            bool outside = false;
            for (int e = 0; e < 3 && !outside; e++) {
                if (fabsf(v[e]) < 1e-9f) {
                    if (o[e] < -d.semi[e] || o[e] > d.semi[e]) outside = true;
                    continue;
                }
                float a = (-d.semi[e] - o[e]) / v[e], b = (d.semi[e] - o[e]) / v[e];
                float sg = -1;
                if (a > b) { const float x = a; a = b; b = x; sg = 1; }
                if (a > t0) { t0 = a; axis = e; sign = sg; }
                if (b < t1) t1 = b;
                if (t0 > t1) outside = true;
            }
            if (!outside && axis >= 0) { t = t0; nl[axis] = sign; }
        } else if (d.shape == W3D_RAG_CAPSULE) {
            const int e = d.axis < 0 || d.axis > 2 ? 0 : d.axis;
            const float rad = d.semi[0], hl = d.semi[1];
            // el cilindro
            float o2[3] = { o[0], o[1], o[2] }, v2[3] = { v[0], v[1], v[2] };
            o2[e] = 0; v2[e] = 0;
            const float a = v_point(v2, v2), b = 2 * v_point(o2, v2), c = v_point(o2, o2) - rad * rad;
            if (a > 1e-12f) {
                const float disc = b * b - 4 * a * c;
                if (disc >= 0) {
                    const float tc = (-b - sqrtf(disc)) / (2 * a);
                    const float z = o[e] + v[e] * tc;
                    if (tc >= 0 && z >= -hl && z <= hl) {
                        t = tc;
                        for (int k = 0; k < 3; k++) nl[k] = (o2[k] + v2[k] * tc) / rad;
                        nl[e] = 0;
                    }
                }
            }
            // las tapas
            for (int k = 0; k < 2; k++) {
                float cc[3] = { 0, 0, 0 };
                cc[e] = k ? hl : -hl;
                const float ts = ray_sphere(o, v, cc, rad);
                if (ts >= 0 && (t < 0 || ts < t)) {
                    t = ts;
                    for (int j = 0; j < 3; j++) nl[j] = (o[j] + v[j] * ts - cc[j]) / rad;
                }
            }
        }
        if (t < 0 || t >= best) continue;
        best = t;
        touched = true;
        if (out) {
            out->bone = (int)i;
            out->distance = t;
            for (int k = 0; k < 3; k++) out->point[k] = origin[k] + dir[k] * t;
            out->normal[0] = F[0] * nl[0] + F[3] * nl[1] + F[6] * nl[2];
            out->normal[1] = F[1] * nl[0] + F[4] * nl[1] + F[7] * nl[2];
            out->normal[2] = F[2] * nl[0] + F[5] * nl[1] + F[8] * nl[2];
        }
    }
    return touched;
}

int W3dRagdollLines(const W3dRagdollDef& def, const float* worlds, float* out, int max) {
    if (!worlds || !out) return 0;
    int n = 0;
    for (size_t i = 0; i < def.bones.size(); i++) {
        const W3dRagdollBone& d = def.bones[i];
        if (d.shape == W3D_RAG_NO_SHAPE) continue;
        float B[12], F[12];
        a_rigid(worlds + i * 12, B, 0);
        shape_in_world(d, B, F);
        if (d.shape == W3D_RAG_BOX) {
            if (n + 24 > max) break;
            float esq[8][3];
            for (int k = 0; k < 8; k++) {
                const float l[3] = { (k & 1) ? d.semi[0] : -d.semi[0], (k & 2) ? d.semi[1] : -d.semi[1],
                                     (k & 4) ? d.semi[2] : -d.semi[2] };
                a_point(F, l, esq[k]);
            }
            // las 12 aristas: las esquinas que difieren en un solo bit
            for (int k = 0; k < 8; k++)
                for (int b = 1; b < 8; b <<= 1)
                    if (!(k & b)) {
                        v_copy(out + n * 3, esq[k]);
                        v_copy(out + n * 3 + 3, esq[k | b]);
                        n += 2;
                    }
        } else {
            if (n + 72 > max) break;
            const int e = d.axis < 0 || d.axis > 2 ? 0 : d.axis, u = (e + 1) % 3, w = (e + 2) % 3;
            const float rad = d.semi[0], hl = d.semi[1];
            float l[3], a[3], b[3];
            // los dos anillos de 8 lados y las 4 lineas del largo
            for (int k = 0; k < 8; k++) {
                const float a0 = k * 0.7853982f, a1 = (k + 1) * 0.7853982f;
                for (int cap = 0; cap < 2; cap++) {
                    l[e] = cap ? hl : -hl;
                    l[u] = cosf(a0) * rad; l[w] = sinf(a0) * rad;
                    a_point(F, l, a);
                    l[u] = cosf(a1) * rad; l[w] = sinf(a1) * rad;
                    a_point(F, l, b);
                    v_copy(out + n * 3, a); v_copy(out + n * 3 + 3, b);
                    n += 2;
                }
                if (k & 1) continue;
                l[u] = cosf(a0) * rad; l[w] = sinf(a0) * rad;
                l[e] = -hl; a_point(F, l, a);
                l[e] = hl; a_point(F, l, b);
                v_copy(out + n * 3, a); v_copy(out + n * 3 + 3, b);
                n += 2;
            }
            // las tapas: dos medios arcos de 4 tramos en cada una
            for (int cap = 0; cap < 2; cap++)
                for (int plane = 0; plane < 2; plane++)
                    for (int k = 0; k < 4; k++) {
                        const int side = plane ? w : u, other = plane ? u : w;
                        const float a0 = k * 0.3926991f * 2.0f, a1 = (k + 1) * 0.3926991f * 2.0f;
                        const float sg = cap ? 1.0f : -1.0f;
                        l[other] = 0;
                        l[side]  = cosf(a0) * rad; l[e] = sg * (hl + sinf(a0) * rad);
                        a_point(F, l, a);
                        l[side] = cosf(a1) * rad; l[e] = sg * (hl + sinf(a1) * rad);
                        a_point(F, l, b);
                        v_copy(out + n * 3, a); v_copy(out + n * 3 + 3, b);
                        n += 2;
                    }
        }
    }
    return n;
}

// ============================================================================ IK
void W3dIkTurnBetween(const float* a0, const float* b0, float* q) {
    float a[3] = { a0[0], a0[1], a0[2] }, b[3] = { b0[0], b0[1], b0[2] };
    v_normalize(a);
    v_normalize(b);
    const float d = v_point(a, b);
    if (d < -0.999999f) {
        // opuestos: media vuelta sobre cualquier perpendicular
        float p[3], x[3] = { 1, 0, 0 };
        v_cross(x, a, p);
        if (v_length(p) < 1e-4f) { const float y[3] = { 0, 1, 0 }; v_cross(y, a, p); }
        v_normalize(p);
        q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = 0;
        return;
    }
    float c[3];
    v_cross(a, b, c);
    q[0] = c[0]; q[1] = c[1]; q[2] = c[2]; q[3] = 1.0f + d;
    q_normalize(q);
}

bool W3dIkTwoBones(const float* root, const float* elbow, const float* tip, const float* target,
                    const float* pole, float* elbowOut, float* tipOut) {
    const float ab[3] = { elbow[0] - root[0], elbow[1] - root[1], elbow[2] - root[2] };
    const float bc[3] = { tip[0] - elbow[0], tip[1] - elbow[1], tip[2] - elbow[2] };
    const float l1 = v_length(ab), l2 = v_length(bc);
    float d[3] = { target[0] - root[0], target[1] - root[1], target[2] - root[2] };
    float dist = v_length(d);
    if (dist < 1e-6f) { v_copy(elbowOut, elbow); v_copy(tipOut, tip); return false; }
    const float dir[3] = { d[0] / dist, d[1] / dist, d[2] / dist };
    if (dist >= l1 + l2) {
        for (int e = 0; e < 3; e++) { elbowOut[e] = root[e] + dir[e] * l1; tipOut[e] = root[e] + dir[e] * (l1 + l2); }
        return false;
    }
    const float min = fabsf(l1 - l2) + 1e-5f;
    if (dist < min) dist = min;
    // hacia donde dobla: lo del polo perpendicular a la direccion (si no sirve, el codo de antes; si tampoco, uno)
    float perp[3] = { 0, 0, 0 };
    const float* refs[2] = { pole, elbow };
    for (int r = 0; r < 2 && v_length(perp) < 1e-5f; r++) {
        if (!refs[r]) continue;
        const float v[3] = { refs[r][0] - root[0], refs[r][1] - root[1], refs[r][2] - root[2] };
        const float k = v_point(v, dir);
        for (int e = 0; e < 3; e++) perp[e] = v[e] - dir[e] * k;
    }
    if (v_length(perp) < 1e-5f) {
        const float y[3] = { 0, 1, 0 }, x[3] = { 1, 0, 0 };
        v_cross(dir, fabsf(dir[1]) < 0.9f ? y : x, perp);
    }
    v_normalize(perp);
    const float x = (l1 * l1 - l2 * l2 + dist * dist) / (2.0f * dist);
    const float h2 = l1 * l1 - x * x;
    const float h = h2 > 0 ? sqrtf(h2) : 0;
    for (int e = 0; e < 3; e++) {
        elbowOut[e] = root[e] + dir[e] * x + perp[e] * h;
        tipOut[e] = root[e] + dir[e] * dist;
    }
    return true;
}

int W3dIkFabrik(float* pos, int n, const float* target, int turns, float tolerance) {
    if (n < 2 || !pos) return 0;
    enum { MAX = 64 };
    if (n > MAX) n = MAX;
    float length[MAX], total = 0;
    for (int i = 0; i + 1 < n; i++) {
        const float d[3] = { pos[i * 3 + 3] - pos[i * 3], pos[i * 3 + 4] - pos[i * 3 + 1], pos[i * 3 + 5] - pos[i * 3 + 2] };
        length[i] = v_length(d);
        total += length[i];
    }
    const float root[3] = { pos[0], pos[1], pos[2] };
    const float until[3] = { target[0] - root[0], target[1] - root[1], target[2] - root[2] };
    // no llega: la cadena derecha hacia el objetivo
    if (v_length(until) >= total) {
        float dir[3] = { until[0], until[1], until[2] };
        v_normalize(dir);
        for (int i = 1; i < n; i++)
            for (int e = 0; e < 3; e++) pos[i * 3 + e] = pos[i * 3 - 3 + e] + dir[e] * length[i - 1];
        return 1;
    }
    int v = 0;
    for (; v < turns; v++) {
        const float* p = pos + (n - 1) * 3;
        const float f[3] = { p[0] - target[0], p[1] - target[1], p[2] - target[2] };
        if (v_length(f) <= tolerance) break;
        // de la punta a la raiz
        v_copy(pos + (n - 1) * 3, target);
        for (int i = n - 2; i >= 0; i--) {
            float d[3] = { pos[i * 3] - pos[i * 3 + 3], pos[i * 3 + 1] - pos[i * 3 + 4], pos[i * 3 + 2] - pos[i * 3 + 5] };
            v_normalize(d);
            for (int e = 0; e < 3; e++) pos[i * 3 + e] = pos[i * 3 + 3 + e] + d[e] * length[i];
        }
        // de la raiz a la punta
        v_copy(pos, root);
        for (int i = 1; i < n; i++) {
            float d[3] = { pos[i * 3] - pos[i * 3 - 3], pos[i * 3 + 1] - pos[i * 3 - 2], pos[i * 3 + 2] - pos[i * 3 - 1] };
            v_normalize(d);
            for (int e = 0; e < 3; e++) pos[i * 3 + e] = pos[i * 3 - 3 + e] + d[e] * length[i - 1];
        }
    }
    return v;
}
