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

const int   PASOS_MAX = 4;            // pasos por llamada como mucho (un cuadro lento)
const float BRAZO_MIN = 0.02f;        // las particulas no mas cerca que esto del centro
const float QUIETO_VEL = 0.03f;       // velocidad (unidades por segundo) debajo de la cual un cuerpo esta quieto
const int   QUIETO_PASOS = 30;        // pasos quieto -> a dormir (medio segundo)
const int   VIDA_PLANO = 3;           // pasos que dura el plano que encontro la funcion del juego
const float MARGEN = 0.01f;           // lo que se alarga el segmento de cada punto (el contacto apoyado no se pierde)
// las 4 particulas: un tetraedro centrado en la forma (cuatro esquinas alternadas de la caja de semiejes 'brazo').
// El centro de masa es el de la forma y la inercia queda diagonal en sus ejes; con brazo = semieje / raiz de 3 es la
// de una caja maciza de esos semiejes.
const float SIGNO[4][3] = { { 1, 1, 1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, 1 } };
const float INV_RAIZ3 = 0.57735027f;

// ---------------------------------------------------------------- vectores
inline void v_copiar(float* o, const float* a) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }
inline float v_punto(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void v_cruz(const float* a, const float* b, float* o) {
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
inline float v_largo(const float* a) { return sqrtf(v_punto(a, a)); }
inline void v_normalizar(float* a) {
    const float l = v_largo(a);
    if (l > 1e-12f) { a[0] /= l; a[1] /= l; a[2] /= l; }
}

// ---------------------------------------------------------------- cuaterniones (x y z w)
inline void q_identidad(float* q) { q[0] = q[1] = q[2] = 0; q[3] = 1; }
inline void q_conj(const float* q, float* o) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
void q_mul(const float* a, const float* b, float* o) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
void q_normalizar(float* q) {
    const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-12f) { q_identidad(q); return; }
    q[0] /= l; q[1] /= l; q[2] /= l; q[3] /= l;
}
// las columnas de la matriz del giro (9 floats: x, y, z)
void q_a_matriz(const float* q, float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + w * z);     m[2] = 2 * (x * z - w * y);
    m[3] = 2 * (x * y - w * z);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z + w * x);
    m[6] = 2 * (x * z + w * y);     m[7] = 2 * (y * z - w * x);     m[8] = 1 - 2 * (x * x + y * y);
}
// de las columnas de una rotacion pura al cuaternion
void q_de_matriz(const float* m, float* q) {
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
    q_normalizar(q);
}
void q_rotar(const float* q, const float* v, float* o) {
    float m[9];
    q_a_matriz(q, m);
    const float x = v[0], y = v[1], z = v[2];
    o[0] = m[0] * x + m[3] * y + m[6] * z;
    o[1] = m[1] * x + m[4] * y + m[7] * z;
    o[2] = m[2] * x + m[5] * y + m[8] * z;
}

// ---------------------------------------------------------------- afines 3x4 por columnas
void a_punto(const float* a, const float* p, float* o) {
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
void a_inv_rigida(const float* a, float* o) {
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
void a_rigida(const float* a, float* o, float* escala) {
    float x[3] = { a[0], a[1], a[2] }, y[3] = { a[3], a[4], a[5] }, z[3];
    if (escala) { escala[0] = v_largo(a); escala[1] = v_largo(a + 3); escala[2] = v_largo(a + 6); }
    v_normalizar(x);
    v_cruz(x, y, z);
    v_normalizar(z);
    v_cruz(z, x, y);
    v_copiar(o, x); v_copiar(o + 3, y); v_copiar(o + 6, z);
    o[9] = a[9]; o[10] = a[10]; o[11] = a[11];
}
void a_de_qt(const float* q, const float* t, float* a) {
    q_a_matriz(q, a);
    a[9] = t[0]; a[10] = t[1]; a[11] = t[2];
}

// el marco de la forma en mundo a partir del hueso (rigido): R = Rhueso * giro, t = hueso * centro
void forma_en_mundo(const W3dRagdollHueso& d, const float* hueso, float* forma) {
    float fl[12];
    a_de_qt(d.giro, d.centro, fl);
    a_mul(hueso, fl, forma);
}

// los pesos de un punto con esas coordenadas en el marco de la forma (la matriz de los signos es de Hadamard: su
// inversa es su traspuesta / 4); devuelve sum(w^2)
float pesos(const float* local, const float* brazo, float* w) {
    const float u = local[0] / brazo[0], v = local[1] / brazo[1], t = local[2] / brazo[2];
    float s2 = 0;
    for (int i = 0; i < 4; i++) {
        w[i] = (1.0f + SIGNO[i][0] * u + SIGNO[i][1] * v + SIGNO[i][2] * t) * 0.25f;
        s2 += w[i] * w[i];
    }
    return s2;
}

inline void centro_de(const float (*x)[3], float* c) {
    for (int e = 0; e < 3; e++) c[e] = (x[0][e] + x[1][e] + x[2][e] + x[3][e]) * 0.25f;
}

// las 4 particulas de una forma con ese giro (columnas R) y ese centro
void poner_particulas(float (*x)[3], const float* R, const float* c, const float* brazo) {
    for (int i = 0; i < 4; i++) {
        const float a = SIGNO[i][0] * brazo[0], b = SIGNO[i][1] * brazo[1], z = SIGNO[i][2] * brazo[2];
        for (int e = 0; e < 3; e++) x[i][e] = c[e] + R[e] * a + R[3 + e] * b + R[6 + e] * z;
    }
}

inline void mezcla(const float (*x)[3], const float* w, float* o) {
    for (int e = 0; e < 3; e++) o[e] = w[0] * x[0][e] + w[1] * x[1][e] + w[2] * x[2][e] + w[3] * x[3][e];
}

inline float limitar(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

} // namespace

// ============================================================================ la definicion
W3dRagdollHueso::W3dRagdollHueso() {
    padre = -1;
    memset(reposo, 0, sizeof(reposo));
    reposo[0] = reposo[4] = reposo[8] = 1.0f;
    forma = W3D_RAG_SIN_FORMA;
    semi[0] = semi[1] = semi[2] = 0.05f;
    eje = 0;
    centro[0] = centro[1] = centro[2] = 0;
    q_identidad(giro);
    masa = 1.0f;
    articulacion = W3D_RAG_ROTULA;
    q_identidad(marco);
    for (int k = 0; k < 3; k++) { min[k] = -0.5f; max[k] = 0.5f; }
}

W3dRagdollDef::W3dRagdollDef() {
    gravedad[0] = 0; gravedad[1] = -9.81f; gravedad[2] = 0;
    amortiguar = 0.5f;
    rozamiento = 0.6f;
    iteraciones = 4;
    paso = 1.0f / 60.0f;
}

void W3dRagdollDef::Caja(int h, const float* c, const float* s) {
    if (h < 0 || h >= (int)huesos.size()) return;
    W3dRagdollHueso& d = huesos[h];
    d.forma = W3D_RAG_CAJA;
    v_copiar(d.centro, c);
    v_copiar(d.semi, s);
    q_identidad(d.giro);
}

void W3dRagdollDef::Capsula(int h, const float* a, const float* b, float radio) {
    if (h < 0 || h >= (int)huesos.size()) return;
    W3dRagdollHueso& d = huesos[h];
    d.forma = W3D_RAG_CAPSULA;
    float eje[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float largo = v_largo(eje);
    for (int e = 0; e < 3; e++) d.centro[e] = (a[e] + b[e]) * 0.5f;
    d.semi[0] = radio;
    d.semi[1] = largo * 0.5f;
    d.semi[2] = radio;
    d.eje = 0;
    // la x de la forma sobre el segmento
    const float x[3] = { 1, 0, 0 };
    if (largo > 1e-9f) { v_normalizar(eje); W3dIkGiroEntre(x, eje, d.giro); }
    else q_identidad(d.giro);
}

void W3dRagdollDef::Rotula(int h, float cono, float giroMin, float giroMax) {
    if (h < 0 || h >= (int)huesos.size()) return;
    W3dRagdollHueso& d = huesos[h];
    d.articulacion = W3D_RAG_ROTULA;
    d.min[0] = giroMin; d.max[0] = giroMax;
    d.min[1] = d.min[2] = -cono;
    d.max[1] = d.max[2] = cono;
}

void W3dRagdollDef::Bisagra(int h, const float* eje, float mn, float mx) {
    if (h < 0 || h >= (int)huesos.size()) return;
    W3dRagdollHueso& d = huesos[h];
    d.articulacion = W3D_RAG_BISAGRA;
    float e[3] = { eje[0], eje[1], eje[2] };
    v_normalizar(e);
    const float x[3] = { 1, 0, 0 };
    W3dIkGiroEntre(x, e, d.marco);
    d.min[0] = mn; d.max[0] = mx;
    d.min[1] = d.max[1] = d.min[2] = d.max[2] = 0;
}

// ============================================================================ el muneco
W3dRagdoll::W3dRagdoll()
    : acum_(0), hayPiso_(false), trazo_(0), usuario_(0), dormido_(false), quietud_(0) {
    piso_[0] = 0; piso_[1] = 1; piso_[2] = 0; piso_[3] = 0;
}

W3dRagdoll::~W3dRagdoll() {}

void W3dRagdoll::Soltar() {
    cuerpos_.clear();
    cuerpoDe_.clear();
    seguidores_.clear();
    escala_.clear();
    mundos_.clear();
}

void W3dRagdoll::ArmarCuerpo(Cuerpo& c, int hueso) {
    const W3dRagdollHueso& d = def_.huesos[hueso];
    c.hueso = hueso;
    const float masa = d.masa > 1e-6f ? d.masa : 1.0f;
    c.invm = 4.0f / masa;
    const bool capsula = d.forma == W3D_RAG_CAPSULA;
    const int eje = d.eje < 0 || d.eje > 2 ? 0 : d.eje;
    // el tetraedro de las particulas: la inercia de la caja de la forma (la capsula: la de su caja envolvente)
    for (int k = 0; k < 3; k++) c.brazo[k] = (capsula ? d.semi[0] : d.semi[k]) * INV_RAIZ3;
    if (capsula) c.brazo[eje] = (d.semi[1] + d.semi[0]) * INV_RAIZ3;
    for (int k = 0; k < 3; k++)
        if (c.brazo[k] < BRAZO_MIN) c.brazo[k] = BRAZO_MIN;
    // la inercia del tetraedro sobre cada eje es masa * (los otros dos brazos al cuadrado): el promedio de los tres
    c.invI = 1.0f / (masa * (2.0f / 3.0f) * (c.brazo[0] * c.brazo[0] + c.brazo[1] * c.brazo[1] + c.brazo[2] * c.brazo[2]));
    // los puntos que chocan: las 8 esquinas de la caja o las 2 puntas del tramo recto de la capsula (con su radio)
    c.radio = capsula ? d.semi[0] : 0.0f;
    c.alcance = capsula ? d.semi[1] + d.semi[0] : v_largo(d.semi);
    c.npts = capsula ? 2 : 8;
    for (int k = 0; k < c.npts; k++) {
        float l[3] = { 0, 0, 0 };
        if (capsula) l[eje] = k ? d.semi[1] : -d.semi[1];
        else {
            l[0] = (k & 1) ? d.semi[0] : -d.semi[0];
            l[1] = (k & 2) ? d.semi[1] : -d.semi[1];
            l[2] = (k & 4) ? d.semi[2] : -d.semi[2];
        }
        c.sp[k] = pesos(l, c.brazo, c.wp[k]);
    }
    for (int k = 0; k < 8; k++) { c.vida[k] = 0; c.toca[k] = false; }
    // el pivote del lado del hijo: el origen del hueso en el marco de la forma
    float gi[4], l[3];
    const float menosC[3] = { -d.centro[0], -d.centro[1], -d.centro[2] };
    q_conj(d.giro, gi);
    q_rotar(gi, menosC, l);
    c.sPiv = pesos(l, c.brazo, c.wPiv);
}

bool W3dRagdoll::Armar(const W3dRagdollDef& def) {
    Soltar();
    def_ = def;
    const int n = (int)def_.huesos.size();
    if (n <= 0) return false;
    cuerpoDe_.assign(n, -1);
    // los cuerpos en orden de profundidad (los padres antes que los hijos)
    std::vector<int> prof(n, 0);
    int maxProf = 0;
    for (int i = 0; i < n; i++) {
        int p = def_.huesos[i].padre, k = 0;
        while (p >= 0 && p < n && k < n) { k++; p = def_.huesos[p].padre; }
        prof[i] = k;
        if (k > maxProf) maxProf = k;
    }
    for (int nivel = 0; nivel <= maxProf; nivel++)
        for (int i = 0; i < n; i++) {
            if (prof[i] != nivel || def_.huesos[i].forma == W3D_RAG_SIN_FORMA) continue;
            Cuerpo c;
            memset(&c, 0, sizeof(c));
            ArmarCuerpo(c, i);
            q_identidad(c.q);
            cuerpoDe_[i] = (int)cuerpos_.size();
            cuerpos_.push_back(c);
        }
    if (cuerpos_.empty()) return false;
    // el padre de cada cuerpo: el ancestro mas cercano con forma, y el giro de reposo entre los dos huesos
    for (size_t k = 0; k < cuerpos_.size(); k++) {
        Cuerpo& c = cuerpos_[k];
        c.padre = -1;
        int p = def_.huesos[c.hueso].padre, vueltas = 0;
        while (p >= 0 && p < n && vueltas++ < n) {
            if (cuerpoDe_[p] >= 0) { c.padre = cuerpoDe_[p]; break; }
            p = def_.huesos[p].padre;
        }
        ArmarArticulacion(c);
    }
    // los huesos sin forma: su ancla (el cuerpo del ancestro mas cercano; si no tiene, el cuerpo mas alto de su
    // descendencia; si tampoco, el primero)
    for (int i = 0; i < n; i++) {
        if (cuerpoDe_[i] >= 0) continue;
        Seguidor s;
        s.hueso = i;
        s.ancla = -1;
        int p = def_.huesos[i].padre, vueltas = 0;
        while (p >= 0 && p < n && vueltas++ < n && s.ancla < 0) {
            if (cuerpoDe_[p] >= 0) s.ancla = cuerpoDe_[p];
            p = def_.huesos[p].padre;
        }
        for (size_t k = 0; k < cuerpos_.size() && s.ancla < 0; k++) {
            int a = def_.huesos[cuerpos_[k].hueso].padre, v = 0;
            while (a >= 0 && a < n && v++ < n && a != i) a = def_.huesos[a].padre;
            if (a == i) s.ancla = (int)k;
        }
        if (s.ancla < 0) s.ancla = 0;
        memset(s.rel, 0, sizeof(s.rel));
        s.rel[0] = s.rel[4] = s.rel[8] = 1.0f;
        seguidores_.push_back(s);
    }
    escala_.assign(n * 3, 1.0f);
    mundos_.assign(n * 12, 0.0f);
    for (int i = 0; i < n; i++) memcpy(&mundos_[i * 12], def_.huesos[i].reposo, sizeof(float) * 12);
    acum_ = 0;
    dormido_ = true;
    quietud_ = 0;
    return true;
}

// qJ = marco^-1 * L0^-1 * (giroHuesoPadre^-1 * giroHueso) * marco, con giroHueso = q * giroForma^-1:
// izq = marco^-1 * L0^-1 * giroFormaPadre y der = giroForma^-1 * marco (L0 = el giro del hueso contra el del padre
// en reposo)
void W3dRagdoll::ArmarArticulacion(Cuerpo& c) {
    const W3dRagdollHueso& d = def_.huesos[c.hueso];
    q_identidad(c.izq);
    q_identidad(c.der);
    if (c.padre < 0) return;
    const W3dRagdollHueso& dp = def_.huesos[cuerpos_[c.padre].hueso];
    float r[12], qp[4], qc[4], t[4], L0[4], L0i[4], mi[4], gi[4];
    a_rigida(dp.reposo, r, 0);
    q_de_matriz(r, qp);
    a_rigida(d.reposo, r, 0);
    q_de_matriz(r, qc);
    q_conj(qp, t);
    q_mul(t, qc, L0);
    q_conj(L0, L0i);
    q_conj(d.marco, mi);
    q_mul(mi, L0i, t);
    q_mul(t, dp.giro, c.izq);
    q_conj(d.giro, gi);
    q_mul(gi, d.marco, c.der);
    // (los limites en tangentes del medio angulo: sin trigonometria en cada vuelta; hasta casi media vuelta)
    for (int k = 0; k < 3; k++) {
        const float mn = limitar(d.min[k], -3.1f, 3.1f), mx = limitar(d.max[k], -3.1f, 3.1f);
        c.tanMin[k] = tanf(mn * 0.5f);
        c.tanMax[k] = tanf((mx < mn ? mn : mx) * 0.5f);
    }
    if (d.articulacion == W3D_RAG_BISAGRA) c.tanMin[1] = c.tanMax[1] = c.tanMin[2] = c.tanMax[2] = 0;
}

void W3dRagdoll::CambiarLimites(int hueso, const W3dRagdollHueso& h) {
    if (hueso < 0 || hueso >= Huesos() || cuerpoDe_[hueso] < 0) return;
    W3dRagdollHueso& d = def_.huesos[hueso];
    d.articulacion = h.articulacion;
    memcpy(d.marco, h.marco, sizeof(d.marco));
    memcpy(d.min, h.min, sizeof(d.min));
    memcpy(d.max, h.max, sizeof(d.max));
    ArmarArticulacion(cuerpos_[cuerpoDe_[hueso]]);
    Despertar();
}

// ---------------------------------------------------------------- de matrices a particulas y vuelta
void W3dRagdoll::MatrizACuerpo(Cuerpo& c, const float* hueso, bool anterior) {
    float f[12];
    forma_en_mundo(def_.huesos[c.hueso], hueso, f);
    poner_particulas(anterior ? c.xp : c.x, f, f + 9, c.brazo);
    if (!anterior) q_de_matriz(f, c.q);
}

void W3dRagdoll::CuerpoAMatriz(const Cuerpo& c, bool anterior, float* hueso) const {
    const W3dRagdollHueso& d = def_.huesos[c.hueso];
    const float (*x)[3] = anterior ? c.xp : c.x;
    // el centro de la forma es el centro de masa de las 4
    float R[9], centro[3];
    q_a_matriz(c.q, R);
    centro_de(x, centro);
    // hueso = forma * (giro, centro)^-1
    float f[12], fl[12], fli[12];
    memcpy(f, R, sizeof(R));
    v_copiar(f + 9, centro);
    a_de_qt(d.giro, d.centro, fl);
    a_inv_rigida(fl, fli);
    a_mul(f, fli, hueso);
}

// el pivote del lado del padre: el origen del hueso hijo de ahora, en el marco de la forma del padre
void W3dRagdoll::PivotesDe(Cuerpo& c) {
    if (c.padre < 0) return;
    const Cuerpo& p = cuerpos_[c.padre];
    float piv[3], R[9], cm[3], l[3], d[3];
    mezcla(c.x, c.wPiv, piv);
    q_a_matriz(p.q, R);
    centro_de(p.x, cm);
    for (int e = 0; e < 3; e++) d[e] = piv[e] - cm[e];
    l[0] = R[0] * d[0] + R[1] * d[1] + R[2] * d[2];
    l[1] = R[3] * d[0] + R[4] * d[1] + R[5] * d[2];
    l[2] = R[6] * d[0] + R[7] * d[1] + R[8] * d[2];
    c.sPivPadre = pesos(l, p.brazo, c.wPivPadre);
}

void W3dRagdoll::ArmarMundos() {
    const int n = Huesos();
    for (size_t k = 0; k < cuerpos_.size(); k++) {
        float* m = &mundos_[cuerpos_[k].hueso * 12];
        CuerpoAMatriz(cuerpos_[k], false, m);
        const float* s = &escala_[cuerpos_[k].hueso * 3];
        for (int e = 0; e < 3; e++) { m[e] *= s[0]; m[3 + e] *= s[1]; m[6 + e] *= s[2]; }
    }
    for (size_t k = 0; k < seguidores_.size(); k++) {
        const Seguidor& s = seguidores_[k];
        float a[12];
        CuerpoAMatriz(cuerpos_[s.ancla], false, a);
        a_mul(a, s.rel, &mundos_[s.hueso * 12]);
    }
    (void)n;
}

// ---------------------------------------------------------------- empezar a caer
void W3dRagdoll::Empezar(const float* mundos, const float* antes, float dtAntes) {
    if (cuerpos_.empty() || !mundos) return;
    float rig[12], rigA[12];
    for (size_t k = 0; k < cuerpos_.size(); k++) {
        Cuerpo& c = cuerpos_[k];
        a_rigida(mundos + c.hueso * 12, rig, &escala_[c.hueso * 3]);
        MatrizACuerpo(c, rig, false);
        if (antes && dtAntes > 1e-5f) {
            a_rigida(antes + c.hueso * 12, rigA, 0);
            MatrizACuerpo(c, rigA, true);
            // la velocidad de la animacion, al paso fijo
            float k2 = PasoFijo() / dtAntes;
            if (k2 > 1.0f) k2 = 1.0f;
            for (int i = 0; i < 4; i++)
                for (int e = 0; e < 3; e++) c.xp[i][e] = c.x[i][e] - (c.x[i][e] - c.xp[i][e]) * k2;
        } else {
            memcpy(c.xp, c.x, sizeof(c.x));
        }
        for (int i = 0; i < 8; i++) { c.vida[i] = 0; c.toca[i] = false; }
    }
    for (size_t k = 0; k < cuerpos_.size(); k++) PivotesDe(cuerpos_[k]);
    for (size_t k = 0; k < seguidores_.size(); k++) {
        Seguidor& s = seguidores_[k];
        float a[12], ai[12];
        a_rigida(mundos + cuerpos_[s.ancla].hueso * 12, a, 0);
        a_inv_rigida(a, ai);
        a_mul(ai, mundos + s.hueso * 12, s.rel);
    }
    acum_ = 0;
    dormido_ = false;
    quietud_ = 0;
    ArmarMundos();
}

void W3dRagdoll::Piso(const float* n, float d) {
    hayPiso_ = true;
    piso_[0] = n[0]; piso_[1] = n[1]; piso_[2] = n[2]; piso_[3] = d;
    Despertar();
}

bool W3dRagdoll::CuerpoDe(int hueso, float* centro, float* vel, float* masa) const {
    if (cuerpos_.empty() || hueso < 0 || hueso >= Huesos()) return false;
    int ci = cuerpoDe_[hueso];
    for (size_t k = 0; ci < 0 && k < seguidores_.size(); k++)
        if (seguidores_[k].hueso == hueso) ci = seguidores_[k].ancla;
    if (ci < 0) return false;
    const Cuerpo& c = cuerpos_[ci];
    float a[3], b[3];
    centro_de(c.x, a);
    centro_de(c.xp, b);
    if (centro) v_copiar(centro, a);
    if (vel)
        for (int e = 0; e < 3; e++) vel[e] = (a[e] - b[e]) / PasoFijo();
    if (masa) *masa = 4.0f / c.invm;
    return true;
}

void W3dRagdoll::Impulso(int hueso, const float* punto, const float* imp) {
    if (cuerpos_.empty() || hueso < 0 || hueso >= Huesos()) return;
    int ci = cuerpoDe_[hueso];
    for (size_t k = 0; ci < 0 && k < seguidores_.size(); k++)
        if (seguidores_[k].hueso == hueso) ci = seguidores_[k].ancla;
    if (ci < 0) return;
    Cuerpo& c = cuerpos_[ci];
    float R[9], cm[3], d[3], l[3], w[4];
    q_a_matriz(c.q, R);
    centro_de(c.x, cm);
    for (int e = 0; e < 3; e++) d[e] = punto[e] - cm[e];
    l[0] = R[0] * d[0] + R[1] * d[1] + R[2] * d[2];
    l[1] = R[3] * d[0] + R[4] * d[1] + R[5] * d[2];
    l[2] = R[6] * d[0] + R[7] * d[1] + R[8] * d[2];
    // (el punto dentro de la forma: un punto lejos del cuerpo lo haria girar como loco)
    for (int e = 0; e < 3; e++) l[e] = limitar(l[e], -c.brazo[e] * 1.7320508f, c.brazo[e] * 1.7320508f);
    pesos(l, c.brazo, w);
    // dv_i = invm * w_i * J (el ajuste rigido lo reparte: el centro de masa recibe J / masa)
    for (int i = 0; i < 4; i++)
        for (int e = 0; e < 3; e++) c.xp[i][e] -= c.invm * w[i] * imp[e] * PasoFijo();
    Despertar();
}

void W3dRagdoll::CambiarForma(int hueso, const W3dRagdollHueso& h) {
    if (hueso < 0 || hueso >= Huesos() || cuerpoDe_[hueso] < 0) return;
    Cuerpo& c = cuerpos_[cuerpoDe_[hueso]];
    float B[12], Bp[12];
    CuerpoAMatriz(c, false, B);
    CuerpoAMatriz(c, true, Bp);
    W3dRagdollHueso& d = def_.huesos[hueso];
    d.forma = h.forma == W3D_RAG_SIN_FORMA ? W3D_RAG_CAJA : h.forma;
    memcpy(d.semi, h.semi, sizeof(d.semi));
    d.eje = h.eje;
    memcpy(d.centro, h.centro, sizeof(d.centro));
    memcpy(d.giro, h.giro, sizeof(d.giro));
    d.masa = h.masa;
    ArmarCuerpo(c, hueso);
    MatrizACuerpo(c, Bp, true);
    MatrizACuerpo(c, B, false);
    PivotesDe(c);
    ArmarArticulacion(c);
    for (size_t k = 0; k < cuerpos_.size(); k++)
        if (cuerpos_[k].padre == cuerpoDe_[hueso]) { PivotesDe(cuerpos_[k]); ArmarArticulacion(cuerpos_[k]); }
    ArmarMundos();
    Despertar();
}

// ---------------------------------------------------------------- el paso
void W3dRagdoll::Paso(float dt) {
    if (cuerpos_.empty() || dormido_ || dt <= 0) return;
    const float h = PasoFijo();
    acum_ += dt;
    int n = 0;
    while (acum_ >= h && n < PASOS_MAX) {
        Subpaso(h);
        acum_ -= h;
        n++;
        if (dormido_) break;
    }
    if (acum_ >= h) acum_ = 0;   // (un cuadro muy lento: lo que sobra se pierde)
    if (n) ArmarMundos();
}

void W3dRagdoll::Subpaso(float h) {
    const float k = 1.0f - def_.amortiguar * h;
    const float g[3] = { def_.gravedad[0] * h * h, def_.gravedad[1] * h * h, def_.gravedad[2] * h * h };
    for (size_t b = 0; b < cuerpos_.size(); b++) {
        Cuerpo& c = cuerpos_[b];
        for (int i = 0; i < 4; i++)
            for (int e = 0; e < 3; e++) {
                const float x = c.x[i][e];
                c.x[i][e] += (x - c.xp[i][e]) * k + g[e];
                c.xp[i][e] = x;
            }
        for (int p = 0; p < c.npts; p++) c.toca[p] = false;
    }
    Contactos();
    const int it = def_.iteraciones > 0 ? def_.iteraciones : 1;
    for (int i = 0; i < it; i++) Iteracion(i == it - 1);
    Friccion();
    // quieto?
    float v2 = 0;
    for (size_t b = 0; b < cuerpos_.size(); b++) {
        const Cuerpo& c = cuerpos_[b];
        for (int i = 0; i < 4; i++) {
            const float d[3] = { c.x[i][0] - c.xp[i][0], c.x[i][1] - c.xp[i][1], c.x[i][2] - c.xp[i][2] };
            const float l = v_punto(d, d);
            if (l > v2) v2 = l;
        }
    }
    const float tope = QUIETO_VEL * h;
    if (v2 < tope * tope) {
        if (++quietud_ >= QUIETO_PASOS) {
            dormido_ = true;
            for (size_t b = 0; b < cuerpos_.size(); b++) memcpy(cuerpos_[b].xp, cuerpos_[b].x, sizeof(cuerpos_[b].x));
        }
    } else {
        quietud_ = 0;
    }
}

// la funcion del juego, una vez por punto: del CENTRO de la forma al punto (alargado su radio y un poco mas). El
// centro esta siempre afuera del mundo (adentro del cuerpo), asi que una esquina que quedo metida en una pared se
// encuentra igual, con el plano por el que entro.
void W3dRagdoll::Contactos() {
    for (size_t b = 0; b < cuerpos_.size(); b++) {
        Cuerpo& c = cuerpos_[b];
        float a[3];
        centro_de(c.x, a);
        for (int p = 0; p < c.npts; p++) {
            if (c.vida[p] > 0) c.vida[p]--;
            if (!trazo_) continue;
            float z[3];
            mezcla(c.x, c.wp[p], z);
            float d[3] = { z[0] - a[0], z[1] - a[1], z[2] - a[2] };
            const float l = v_largo(d);
            if (l < 1e-6f) continue;
            const float extra = (c.radio + MARGEN) / l;
            for (int e = 0; e < 3; e++) z[e] += d[e] * extra;
            float punto[3], n[3];
            if (trazo_(usuario_, a, z, punto, n)) {
                v_normalizar(n);
                v_copiar(c.plano[p], n);
                c.plano[p][3] = v_punto(n, punto);
                c.vida[p] = VIDA_PLANO;
            }
        }
    }
}

void W3dRagdoll::Iteracion(bool ultima) {
    // 1. los pivotes de las articulaciones (el hijo y el padre se reparten la correccion por la masa)
    for (size_t b = 0; b < cuerpos_.size(); b++) {
        Cuerpo& c = cuerpos_[b];
        if (c.padre < 0) continue;
        Cuerpo& p = cuerpos_[c.padre];
        float pc[3], pp[3];
        mezcla(c.x, c.wPiv, pc);
        mezcla(p.x, c.wPivPadre, pp);
        const float s = c.invm * c.sPiv + p.invm * c.sPivPadre;
        if (s < 1e-12f) continue;
        const float l[3] = { (pp[0] - pc[0]) / s, (pp[1] - pc[1]) / s, (pp[2] - pc[2]) / s };
        for (int i = 0; i < 4; i++) {
            const float kc = c.invm * c.wPiv[i], kp = p.invm * c.wPivPadre[i];
            for (int e = 0; e < 3; e++) {
                c.x[i][e] += kc * l[e];
                p.x[i][e] -= kp * l[e];
            }
        }
    }
    // 2. los choques (el piso en cada vuelta; los planos de la funcion del juego mientras duren)
    for (int vuelta = 0; vuelta < (ultima ? 2 : 1); vuelta++) {
        for (size_t b = 0; b < cuerpos_.size(); b++) {
            Cuerpo& c = cuerpos_[b];
            // (el piso solo si la forma llega: la distancia del centro contra lo que mide)
            bool piso = hayPiso_;
            if (piso) {
                float cm[3];
                centro_de(c.x, cm);
                piso = v_punto(piso_, cm) - piso_[3] < c.alcance + c.radio;
            }
            // (la altura de cada particula sobre el piso: la de cada punto es la mezcla de las cuatro)
            float hp[4] = { 0, 0, 0, 0 };
            if (piso)
                for (int i = 0; i < 4; i++) hp[i] = v_punto(piso_, c.x[i]) - piso_[3];
            for (int p = 0; p < c.npts; p++) {
                for (int plano = 0; plano < 2; plano++) {
                    const float* pl = plano == 0 ? (piso ? piso_ : 0) : (c.vida[p] > 0 ? c.plano[p] : 0);
                    if (!pl) continue;
                    const float* w = c.wp[p];
                    float d;
                    if (plano == 0) d = w[0] * hp[0] + w[1] * hp[1] + w[2] * hp[2] + w[3] * hp[3] - c.radio;
                    else {
                        float P[3];
                        mezcla(c.x, w, P);
                        d = v_punto(pl, P) - pl[3] - c.radio;
                    }
                    if (d >= 0) continue;
                    const float k = -d / c.sp[p];
                    for (int i = 0; i < 4; i++) {
                        for (int e = 0; e < 3; e++) c.x[i][e] += w[i] * k * pl[e];
                        if (piso) hp[i] = v_punto(piso_, c.x[i]) - piso_[3];
                    }
                    // (la friccion usa la normal del plano que toco; un plano de la funcion del juego que todavia
                    // empuja sigue vivo: lo apoyado no se suelta)
                    if (plano == 0) { memcpy(c.plano[p], piso_, sizeof(piso_)); if (c.vida[p] < 1) c.vida[p] = 1; }
                    else if (c.vida[p] < VIDA_PLANO) c.vida[p] = VIDA_PLANO;
                    c.toca[p] = true;
                }
            }
        }
        // 3. rigidos otra vez
        for (size_t b = 0; b < cuerpos_.size(); b++) Rigidizar(cuerpos_[b]);
        if (vuelta == 0)
            // 4. los limites del giro
            for (size_t b = 0; b < cuerpos_.size(); b++) Limitar(cuerpos_[b]);
    }
}

// el giro que mejor calza las 4 particulas (Muller, "A Robust Method to Extract the Rotational Part of
// Deformations"), y las particulas a su lugar alrededor del centro de masa
void W3dRagdoll::Rigidizar(Cuerpo& c) {
    float cm[3];
    centro_de(c.x, cm);
    // A = sum((x_i - cm) * r_i^T), con r_i la particula en reposo (signo * brazo): A = R * S con S simetrica
    float A[3][3];
    for (int k = 0; k < 3; k++)
        for (int e = 0; e < 3; e++) {
            float a = 0;
            for (int i = 0; i < 4; i++) a += SIGNO[i][k] * (c.x[i][e] - cm[e]);
            A[k][e] = a * c.brazo[k];
        }
    float R[9];
    for (int vuelta = 0; vuelta < 1; vuelta++) {   // (una por iteracion alcanza: arranca del giro de la anterior)
        q_a_matriz(c.q, R);
        float w[3] = { 0, 0, 0 }, den = 0, t[3];
        for (int k = 0; k < 3; k++) {
            v_cruz(R + k * 3, A[k], t);
            w[0] += t[0]; w[1] += t[1]; w[2] += t[2];
            den += v_punto(R + k * 3, A[k]);
        }
        den = fabsf(den) + 1e-12f;
        w[0] /= den; w[1] /= den; w[2] /= den;
        const float largo = v_largo(w);
        if (!(largo > 1e-6f && largo < 1e6f)) break;   // (quieto, o las particulas aplastadas: queda el giro de antes)
        // el giro chico de primer orden (sin trigonometria), de a un radian por vuelta como mucho
        const float k = (largo < 1.0f ? 1.0f : 1.0f / largo) * 0.5f;
        float dq[4] = { w[0] * k, w[1] * k, w[2] * k, 1.0f }, nq[4];
        q_mul(dq, c.q, nq);
        q_normalizar(nq);
        memcpy(c.q, nq, sizeof(nq));
    }
    q_a_matriz(c.q, R);
    poner_particulas(c.x, R, cm, c.brazo);
}

// gira las particulas del cuerpo por q alrededor de 'piv'
static void girar_particulas(float (*x)[3], float* qc, const float* q, const float* piv) {
    float m[9];
    q_a_matriz(q, m);
    for (int i = 0; i < 4; i++) {
        const float d[3] = { x[i][0] - piv[0], x[i][1] - piv[1], x[i][2] - piv[2] };
        for (int e = 0; e < 3; e++) x[i][e] = piv[e] + m[e] * d[0] + m[3 + e] * d[1] + m[6 + e] * d[2];
    }
    float n[4];
    q_mul(q, qc, n);
    q_normalizar(n);
    memcpy(qc, n, sizeof(n));
}

void W3dRagdoll::Limitar(Cuerpo& c) {
    if (c.padre < 0 || def_.huesos[c.hueso].articulacion == W3D_RAG_LIBRE) return;
    Cuerpo& p = cuerpos_[c.padre];
    // el giro de la articulacion contra el reposo, en su marco
    float pi[4], t[4], u[4], qJ[4];
    q_conj(p.q, pi);
    q_mul(c.izq, pi, t);
    q_mul(t, c.q, u);
    q_mul(u, c.der, qJ);
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
    const float nx = limitar(tx, c.tanMin[0], c.tanMax[0]);
    const float ny = limitar(ty, c.tanMin[1], c.tanMax[1]);
    const float nz = limitar(tz, c.tanMin[2], c.tanMax[2]);
    if (nx == tx && ny == ty && nz == tz) return;
    // el giro dentro del rango, y lo que falta: d = nuevo * qJ^-1, llevado al mundo por W = p.q * izq^-1
    float tw[4] = { nx, 0, 0, 1 }, sw[4] = { 0, ny, nz, 1 }, nqJ[4], qJi[4], dJ[4], W[4], Wi[4], izqi[4], dq[4];
    q_normalizar(tw);
    q_normalizar(sw);
    q_mul(sw, tw, nqJ);
    q_conj(qJ, qJi);
    q_mul(nqJ, qJi, dJ);
    q_conj(c.izq, izqi);
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
    q_normalizar(dc);
    q_normalizar(dp);
    float cc[3], cp[3];
    centro_de(c.x, cc);
    centro_de(p.x, cp);
    girar_particulas(c.x, c.q, dc, cc);
    girar_particulas(p.x, p.q, dp, cp);
}

// la friccion: lo tangencial de cada punto que toca se frena (cambiando su posicion anterior: solo la velocidad)
void W3dRagdoll::Friccion() {
    const float mu = limitar(def_.rozamiento, 0, 1);
    if (mu <= 0) return;
    for (size_t b = 0; b < cuerpos_.size(); b++) {
        Cuerpo& c = cuerpos_[b];
        for (int p = 0; p < c.npts; p++) {
            if (!c.toca[p]) continue;
            const float* n = c.plano[p];
            float P[3], A[3];
            mezcla(c.x, c.wp[p], P);
            mezcla(c.xp, c.wp[p], A);
            float v[3] = { P[0] - A[0], P[1] - A[1], P[2] - A[2] };
            const float vn = v_punto(v, n);
            for (int e = 0; e < 3; e++) v[e] = (v[e] - n[e] * vn) * mu / c.sp[p];
            for (int i = 0; i < 4; i++)
                for (int e = 0; e < 3; e++) c.xp[i][e] += c.wp[p][i] * v[e];
        }
    }
}

// ============================================================================ el rayo y las lineas
namespace {
// el rayo (local) contra una esfera; t >= 0 o -1
float rayo_esfera(const float* o, const float* d, const float* c, float r) {
    const float m[3] = { o[0] - c[0], o[1] - c[1], o[2] - c[2] };
    const float b = v_punto(m, d), cc = v_punto(m, m) - r * r;
    if (cc > 0 && b > 0) return -1;
    const float disc = b * b - cc;
    if (disc < 0) return -1;
    const float t = -b - sqrtf(disc);
    return t < 0 ? -1 : t;
}
}

bool W3dRagdollRayo(const W3dRagdollDef& def, const float* mundos, const float* origen, const float* dir,
                    float maxDist, W3dRagdollImpacto* out) {
    if (!mundos) return false;
    float mejor = maxDist;
    bool toco = false;
    for (size_t i = 0; i < def.huesos.size(); i++) {
        const W3dRagdollHueso& d = def.huesos[i];
        if (d.forma == W3D_RAG_SIN_FORMA) continue;
        float B[12], F[12];
        a_rigida(mundos + i * 12, B, 0);
        forma_en_mundo(d, B, F);
        // el rayo en el marco de la forma
        const float r[3] = { origen[0] - F[9], origen[1] - F[10], origen[2] - F[11] };
        float o[3], v[3];
        a_vector_t(F, r, o);
        a_vector_t(F, dir, v);
        float t = -1, nl[3] = { 0, 0, 0 };
        if (d.forma == W3D_RAG_CAJA) {
            float t0 = 0, t1 = mejor;
            int eje = -1;
            float signo = 0;
            bool fuera = false;
            for (int e = 0; e < 3 && !fuera; e++) {
                if (fabsf(v[e]) < 1e-9f) {
                    if (o[e] < -d.semi[e] || o[e] > d.semi[e]) fuera = true;
                    continue;
                }
                float a = (-d.semi[e] - o[e]) / v[e], b = (d.semi[e] - o[e]) / v[e];
                float sg = -1;
                if (a > b) { const float x = a; a = b; b = x; sg = 1; }
                if (a > t0) { t0 = a; eje = e; signo = sg; }
                if (b < t1) t1 = b;
                if (t0 > t1) fuera = true;
            }
            if (!fuera && eje >= 0) { t = t0; nl[eje] = signo; }
        } else if (d.forma == W3D_RAG_CAPSULA) {
            const int e = d.eje < 0 || d.eje > 2 ? 0 : d.eje;
            const float rad = d.semi[0], hl = d.semi[1];
            // el cilindro
            float o2[3] = { o[0], o[1], o[2] }, v2[3] = { v[0], v[1], v[2] };
            o2[e] = 0; v2[e] = 0;
            const float a = v_punto(v2, v2), b = 2 * v_punto(o2, v2), c = v_punto(o2, o2) - rad * rad;
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
                const float ts = rayo_esfera(o, v, cc, rad);
                if (ts >= 0 && (t < 0 || ts < t)) {
                    t = ts;
                    for (int j = 0; j < 3; j++) nl[j] = (o[j] + v[j] * ts - cc[j]) / rad;
                }
            }
        }
        if (t < 0 || t >= mejor) continue;
        mejor = t;
        toco = true;
        if (out) {
            out->hueso = (int)i;
            out->distancia = t;
            for (int k = 0; k < 3; k++) out->punto[k] = origen[k] + dir[k] * t;
            out->normal[0] = F[0] * nl[0] + F[3] * nl[1] + F[6] * nl[2];
            out->normal[1] = F[1] * nl[0] + F[4] * nl[1] + F[7] * nl[2];
            out->normal[2] = F[2] * nl[0] + F[5] * nl[1] + F[8] * nl[2];
        }
    }
    return toco;
}

int W3dRagdollLineas(const W3dRagdollDef& def, const float* mundos, float* out, int max) {
    if (!mundos || !out) return 0;
    int n = 0;
    for (size_t i = 0; i < def.huesos.size(); i++) {
        const W3dRagdollHueso& d = def.huesos[i];
        if (d.forma == W3D_RAG_SIN_FORMA) continue;
        float B[12], F[12];
        a_rigida(mundos + i * 12, B, 0);
        forma_en_mundo(d, B, F);
        if (d.forma == W3D_RAG_CAJA) {
            if (n + 24 > max) break;
            float esq[8][3];
            for (int k = 0; k < 8; k++) {
                const float l[3] = { (k & 1) ? d.semi[0] : -d.semi[0], (k & 2) ? d.semi[1] : -d.semi[1],
                                     (k & 4) ? d.semi[2] : -d.semi[2] };
                a_punto(F, l, esq[k]);
            }
            // las 12 aristas: las esquinas que difieren en un solo bit
            for (int k = 0; k < 8; k++)
                for (int b = 1; b < 8; b <<= 1)
                    if (!(k & b)) {
                        v_copiar(out + n * 3, esq[k]);
                        v_copiar(out + n * 3 + 3, esq[k | b]);
                        n += 2;
                    }
        } else {
            if (n + 72 > max) break;
            const int e = d.eje < 0 || d.eje > 2 ? 0 : d.eje, u = (e + 1) % 3, w = (e + 2) % 3;
            const float rad = d.semi[0], hl = d.semi[1];
            float l[3], a[3], b[3];
            // los dos anillos de 8 lados y las 4 lineas del largo
            for (int k = 0; k < 8; k++) {
                const float a0 = k * 0.7853982f, a1 = (k + 1) * 0.7853982f;
                for (int tapa = 0; tapa < 2; tapa++) {
                    l[e] = tapa ? hl : -hl;
                    l[u] = cosf(a0) * rad; l[w] = sinf(a0) * rad;
                    a_punto(F, l, a);
                    l[u] = cosf(a1) * rad; l[w] = sinf(a1) * rad;
                    a_punto(F, l, b);
                    v_copiar(out + n * 3, a); v_copiar(out + n * 3 + 3, b);
                    n += 2;
                }
                if (k & 1) continue;
                l[u] = cosf(a0) * rad; l[w] = sinf(a0) * rad;
                l[e] = -hl; a_punto(F, l, a);
                l[e] = hl; a_punto(F, l, b);
                v_copiar(out + n * 3, a); v_copiar(out + n * 3 + 3, b);
                n += 2;
            }
            // las tapas: dos medios arcos de 4 tramos en cada una
            for (int tapa = 0; tapa < 2; tapa++)
                for (int plano = 0; plano < 2; plano++)
                    for (int k = 0; k < 4; k++) {
                        const int lado = plano ? w : u, otro = plano ? u : w;
                        const float a0 = k * 0.3926991f * 2.0f, a1 = (k + 1) * 0.3926991f * 2.0f;
                        const float sg = tapa ? 1.0f : -1.0f;
                        l[otro] = 0;
                        l[lado] = cosf(a0) * rad; l[e] = sg * (hl + sinf(a0) * rad);
                        a_punto(F, l, a);
                        l[lado] = cosf(a1) * rad; l[e] = sg * (hl + sinf(a1) * rad);
                        a_punto(F, l, b);
                        v_copiar(out + n * 3, a); v_copiar(out + n * 3 + 3, b);
                        n += 2;
                    }
        }
    }
    return n;
}

// ============================================================================ IK
void W3dIkGiroEntre(const float* a0, const float* b0, float* q) {
    float a[3] = { a0[0], a0[1], a0[2] }, b[3] = { b0[0], b0[1], b0[2] };
    v_normalizar(a);
    v_normalizar(b);
    const float d = v_punto(a, b);
    if (d < -0.999999f) {
        // opuestos: media vuelta sobre cualquier perpendicular
        float p[3], x[3] = { 1, 0, 0 };
        v_cruz(x, a, p);
        if (v_largo(p) < 1e-4f) { const float y[3] = { 0, 1, 0 }; v_cruz(y, a, p); }
        v_normalizar(p);
        q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = 0;
        return;
    }
    float c[3];
    v_cruz(a, b, c);
    q[0] = c[0]; q[1] = c[1]; q[2] = c[2]; q[3] = 1.0f + d;
    q_normalizar(q);
}

bool W3dIkDosHuesos(const float* raiz, const float* codo, const float* punta, const float* objetivo,
                    const float* polo, float* codoOut, float* puntaOut) {
    const float ab[3] = { codo[0] - raiz[0], codo[1] - raiz[1], codo[2] - raiz[2] };
    const float bc[3] = { punta[0] - codo[0], punta[1] - codo[1], punta[2] - codo[2] };
    const float l1 = v_largo(ab), l2 = v_largo(bc);
    float d[3] = { objetivo[0] - raiz[0], objetivo[1] - raiz[1], objetivo[2] - raiz[2] };
    float dist = v_largo(d);
    if (dist < 1e-6f) { v_copiar(codoOut, codo); v_copiar(puntaOut, punta); return false; }
    const float dir[3] = { d[0] / dist, d[1] / dist, d[2] / dist };
    if (dist >= l1 + l2) {
        for (int e = 0; e < 3; e++) { codoOut[e] = raiz[e] + dir[e] * l1; puntaOut[e] = raiz[e] + dir[e] * (l1 + l2); }
        return false;
    }
    const float minimo = fabsf(l1 - l2) + 1e-5f;
    if (dist < minimo) dist = minimo;
    // hacia donde dobla: lo del polo perpendicular a la direccion (si no sirve, el codo de antes; si tampoco, uno)
    float perp[3] = { 0, 0, 0 };
    const float* refs[2] = { polo, codo };
    for (int r = 0; r < 2 && v_largo(perp) < 1e-5f; r++) {
        if (!refs[r]) continue;
        const float v[3] = { refs[r][0] - raiz[0], refs[r][1] - raiz[1], refs[r][2] - raiz[2] };
        const float k = v_punto(v, dir);
        for (int e = 0; e < 3; e++) perp[e] = v[e] - dir[e] * k;
    }
    if (v_largo(perp) < 1e-5f) {
        const float y[3] = { 0, 1, 0 }, x[3] = { 1, 0, 0 };
        v_cruz(dir, fabsf(dir[1]) < 0.9f ? y : x, perp);
    }
    v_normalizar(perp);
    const float x = (l1 * l1 - l2 * l2 + dist * dist) / (2.0f * dist);
    const float h2 = l1 * l1 - x * x;
    const float h = h2 > 0 ? sqrtf(h2) : 0;
    for (int e = 0; e < 3; e++) {
        codoOut[e] = raiz[e] + dir[e] * x + perp[e] * h;
        puntaOut[e] = raiz[e] + dir[e] * dist;
    }
    return true;
}

int W3dIkFabrik(float* pos, int n, const float* objetivo, int vueltas, float tolerancia) {
    if (n < 2 || !pos) return 0;
    enum { MAX = 64 };
    if (n > MAX) n = MAX;
    float largo[MAX], total = 0;
    for (int i = 0; i + 1 < n; i++) {
        const float d[3] = { pos[i * 3 + 3] - pos[i * 3], pos[i * 3 + 4] - pos[i * 3 + 1], pos[i * 3 + 5] - pos[i * 3 + 2] };
        largo[i] = v_largo(d);
        total += largo[i];
    }
    const float raiz[3] = { pos[0], pos[1], pos[2] };
    const float hasta[3] = { objetivo[0] - raiz[0], objetivo[1] - raiz[1], objetivo[2] - raiz[2] };
    // no llega: la cadena derecha hacia el objetivo
    if (v_largo(hasta) >= total) {
        float dir[3] = { hasta[0], hasta[1], hasta[2] };
        v_normalizar(dir);
        for (int i = 1; i < n; i++)
            for (int e = 0; e < 3; e++) pos[i * 3 + e] = pos[i * 3 - 3 + e] + dir[e] * largo[i - 1];
        return 1;
    }
    int v = 0;
    for (; v < vueltas; v++) {
        const float* p = pos + (n - 1) * 3;
        const float f[3] = { p[0] - objetivo[0], p[1] - objetivo[1], p[2] - objetivo[2] };
        if (v_largo(f) <= tolerancia) break;
        // de la punta a la raiz
        v_copiar(pos + (n - 1) * 3, objetivo);
        for (int i = n - 2; i >= 0; i--) {
            float d[3] = { pos[i * 3] - pos[i * 3 + 3], pos[i * 3 + 1] - pos[i * 3 + 4], pos[i * 3 + 2] - pos[i * 3 + 5] };
            v_normalizar(d);
            for (int e = 0; e < 3; e++) pos[i * 3 + e] = pos[i * 3 + 3 + e] + d[e] * largo[i];
        }
        // de la raiz a la punta
        v_copiar(pos, raiz);
        for (int i = 1; i < n; i++) {
            float d[3] = { pos[i * 3] - pos[i * 3 - 3], pos[i * 3 + 1] - pos[i * 3 - 2], pos[i * 3 + 2] - pos[i * 3 - 1] };
            v_normalizar(d);
            for (int e = 0; e < 3; e++) pos[i * 3 + e] = pos[i * 3 - 3 + e] + d[e] * largo[i - 1];
        }
    }
    return v;
}
