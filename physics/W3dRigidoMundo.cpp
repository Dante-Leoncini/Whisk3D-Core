// ============================================================================
//  W3dRigidoMundo.cpp — ver W3dRigidoMundo.h. Dialecto C++03 (Symbian compila esto).
//  Comentarios en espanol sin acentos.
//
//  La deteccion de caja contra caja y el solver son los de W3dRigido.cpp (SAT de
//  15 ejes con cada semieje por SU dot en los ejes cruzados, recorte de la cara
//  incidente, impulsos secuenciales con sesgo medido contra un paso de 1/60 s);
//  aca trabajan sobre indices en vez de Object y con un "cuerpo" -1 que es el
//  mundo estatico (masa infinita, quieto).
// ============================================================================
#include "physics/W3dRigidoMundo.h"
#include <math.h>

static const int CONTACTOS_MUNDO = 16;   // contactos contra el mundo por cuerpo y sub-paso

W3dRigidoMundo::W3dRigidoMundo() : mundoFn(0), mundoDato(0), nDespiertos(0) {}

void W3dRigidoMundo::Limpiar() {
    c.clear();
    ms.clear();
    despiertos.clear();
    nDespiertos = 0;
}

void W3dRigidoMundo::Actualizar(Cuerpo& k) {
    k.u[0] = k.rot * Vector3(1, 0, 0);
    k.u[1] = k.rot * Vector3(0, 1, 0);
    k.u[2] = k.rot * Vector3(0, 0, 1);
}

// I_mundo^-1 * v (R * diag(invI) * R^T * v); el mundo (-1) y los dormidos no giran (para el solver un dormido es
// fijo hasta que algo lo despierte: si no, junta velocidad que no integra y salta al despertarse)
Vector3 W3dRigidoMundo::InerciaInv(int i, const Vector3& v) const {
    if (i < 0 || c[i].dormido) return Vector3(0, 0, 0);
    const Cuerpo& k = c[i];
    return k.u[0] * (k.invInercia.x * k.u[0].Dot(v)) + k.u[1] * (k.invInercia.y * k.u[1].Dot(v)) +
           k.u[2] * (k.invInercia.z * k.u[2].Dot(v));
}

Vector3 W3dRigidoMundo::VelEn(int i, const Vector3& p) const {
    if (i < 0) return Vector3(0, 0, 0);
    const Cuerpo& k = c[i];
    return k.vel + Vector3::Cross(k.velAng, p - k.pos);
}

// ---------------------------------------------------------------------------
//  cuerpos
// ---------------------------------------------------------------------------
int W3dRigidoMundo::Agregar(const Vector3& centro, const Quaternion& rot, const Vector3& semi, float masa,
                            float friccion, float rebote) {
    Cuerpo k;
    k.pos = centro;
    k.rot = rot.Normalized();
    k.half = semi;
    if (k.half.x < 0.001f) k.half.x = 0.001f;
    if (k.half.y < 0.001f) k.half.y = 0.001f;
    if (k.half.z < 0.001f) k.half.z = 0.001f;
    const float m = masa > 0.0001f ? masa : 1.0f;
    // inercia de una caja maciza: I = m/12 * (a^2 + b^2) por eje
    const float ex = 2.0f * k.half.x, ey = 2.0f * k.half.y, ez = 2.0f * k.half.z;
    k.invInerciaGuardada = Vector3(12.0f / (m * (ey * ey + ez * ez)), 12.0f / (m * (ex * ex + ez * ez)),
                                   12.0f / (m * (ex * ex + ey * ey)));
    k.invMasaGuardada = 1.0f / m;
    k.fijo = masa <= 0.0f;
    k.invMasa = k.fijo ? 0.0f : k.invMasaGuardada;
    k.invInercia = k.fijo ? Vector3(0, 0, 0) : k.invInerciaGuardada;
    k.friccion = friccion;
    k.rebote = rebote;
    k.golpe = 0.0f;
    k.grupo = 0;
    k.quietud = 0;
    k.vivo = true;
    k.dormido = true;
    k.activo = true;
    k.sinGravedad = false;
    k.movido = false;
    k.conEje = false;
    Actualizar(k);
    c.push_back(k);
    return (int)c.size() - 1;
}

void W3dRigidoMundo::Quitar(int i) {
    if (!Existe(i)) return;
    DespertarVecinos(i, 0.05f);
    c[i].vivo = false;
    c[i].activo = false;
}

void W3dRigidoMundo::Pose(int i, Vector3* centro, Quaternion* rot) const {
    if (i < 0 || i >= (int)c.size()) return;
    if (centro) *centro = c[i].pos;
    if (rot) *rot = c[i].rot;
}
void W3dRigidoMundo::Ejes(int i, Vector3 ejes[3]) const {
    if (i < 0 || i >= (int)c.size()) return;
    ejes[0] = c[i].u[0]; ejes[1] = c[i].u[1]; ejes[2] = c[i].u[2];
}
Vector3 W3dRigidoMundo::Semi(int i) const { return (i >= 0 && i < (int)c.size()) ? c[i].half : Vector3(0, 0, 0); }
float W3dRigidoMundo::Masa(int i) const {
    return (i >= 0 && i < (int)c.size()) ? 1.0f / c[i].invMasaGuardada : 0.0f;
}
void W3dRigidoMundo::SetPose(int i, const Vector3& centro, const Quaternion& rot) {
    if (!Existe(i)) return;
    c[i].pos = centro;
    c[i].rot = rot.Normalized();
    Actualizar(c[i]);
    c[i].movido = true;
    Despertar(i);
}
Vector3 W3dRigidoMundo::Vel(int i) const { return Existe(i) ? c[i].vel : Vector3(0, 0, 0); }
Vector3 W3dRigidoMundo::VelAng(int i) const { return Existe(i) ? c[i].velAng : Vector3(0, 0, 0); }
void W3dRigidoMundo::SetVel(int i, const Vector3& v) {
    if (!Existe(i) || c[i].fijo) return;
    c[i].vel = v;
    Despertar(i);
}
void W3dRigidoMundo::SetVelAng(int i, const Vector3& w) {
    if (!Existe(i) || c[i].fijo) return;
    c[i].velAng = w;
    Despertar(i);
}
void W3dRigidoMundo::Impulso(int i, const Vector3& imp, const Vector3& punto) {
    if (!Existe(i) || c[i].fijo) return;
    c[i].vel += imp * c[i].invMasa;
    c[i].velAng += InerciaInv(i, Vector3::Cross(punto - c[i].pos, imp));
    Despertar(i);
}
void W3dRigidoMundo::Despertar(int i) {
    if (!Existe(i) || c[i].fijo) return;
    c[i].dormido = false;
    c[i].quietud = 0;
}
void W3dRigidoMundo::DespertarVecinos(int i, float margen) {
    if (!Existe(i)) return;
    Vector3 mn, mx;
    Caja(i, &mn, &mx);
    for (int j = 0; j < (int)c.size(); j++) {
        if (j == i || !c[j].vivo || !c[j].dormido) continue;
        Vector3 a, b;
        Caja(j, &a, &b);
        if (a.x > mx.x + margen || b.x < mn.x - margen || a.y > mx.y + margen || b.y < mn.y - margen ||
            a.z > mx.z + margen || b.z < mn.z - margen)
            continue;
        Despertar(j);
    }
}
bool W3dRigidoMundo::Dormido(int i) const { return !Existe(i) || c[i].dormido || c[i].fijo; }
void W3dRigidoMundo::SetActivo(int i, bool si) {
    if (!Existe(i) || c[i].activo == si) return;
    if (!si) DespertarVecinos(i, 0.05f);
    c[i].activo = si;
    if (si) Despertar(i);
}
bool W3dRigidoMundo::Activo(int i) const { return Existe(i) && c[i].activo; }
void W3dRigidoMundo::SetFijo(int i, bool fijo) {
    if (!Existe(i) || c[i].fijo == fijo) return;
    Cuerpo& k = c[i];
    k.fijo = fijo;
    k.invMasa = fijo ? 0.0f : k.invMasaGuardada;
    k.invInercia = fijo ? Vector3(0, 0, 0) : k.invInerciaGuardada;
    k.vel = Vector3(0, 0, 0);
    k.velAng = Vector3(0, 0, 0);
    k.dormido = true;
    if (!fijo) Despertar(i);
}
bool W3dRigidoMundo::Fijo(int i) const { return Existe(i) && c[i].fijo; }
void W3dRigidoMundo::SetSinGravedad(int i, bool si) {
    if (Existe(i)) c[i].sinGravedad = si;
}
void W3dRigidoMundo::SetEje(int i, const Vector3* eje, const Vector3* pivote) {
    if (!Existe(i)) return;
    Cuerpo& k = c[i];
    k.conEje = eje != 0;
    if (!eje) return;
    k.eje = eje->Normalized();
    // (el punto de la recta mas cerca del centro: el brazo queda perpendicular al eje)
    const Vector3 p = pivote ? *pivote : k.pos;
    k.pivote = p + k.eje * k.eje.Dot(k.pos - p);
    const Vector3 b = k.pos - k.pivote;
    k.brazo = Vector3(k.u[0].Dot(b), k.u[1].Dot(b), k.u[2].Dot(b));
}
void W3dRigidoMundo::SetGrupo(int i, unsigned sinChoque) {
    if (Existe(i)) c[i].grupo = sinChoque;
}
bool W3dRigidoMundo::Movido(int i) const { return Existe(i) && c[i].movido; }
float W3dRigidoMundo::GolpeImpulso(int i) const { return Existe(i) ? c[i].golpe : 0.0f; }

void W3dRigidoMundo::Caja(int i, Vector3* mn, Vector3* mx) const {
    const Cuerpo& k = c[i];
    const Vector3 e(fabsf(k.u[0].x) * k.half.x + fabsf(k.u[1].x) * k.half.y + fabsf(k.u[2].x) * k.half.z,
                    fabsf(k.u[0].y) * k.half.x + fabsf(k.u[1].y) * k.half.y + fabsf(k.u[2].y) * k.half.z,
                    fabsf(k.u[0].z) * k.half.x + fabsf(k.u[1].z) * k.half.y + fabsf(k.u[2].z) * k.half.z);
    if (mn) *mn = k.pos - e;
    if (mx) *mx = k.pos + e;
}

// ---------------------------------------------------------------------------
//  rayo contra las cajas (losas en el espacio de cada una)
// ---------------------------------------------------------------------------
float W3dRigidoMundo::Rayo(const Vector3& origen, const Vector3& dir, float maxDist, int ignorar, int* cuerpo,
                           Vector3* normal) const {
    float mejor = -1.0f;
    for (int i = 0; i < (int)c.size(); i++) {
        const Cuerpo& k = c[i];
        if (i == ignorar || !k.vivo || !k.activo) continue;
        const Vector3 rel = origen - k.pos;
        float tMin = 0.0f, tMax = (mejor >= 0.0f) ? mejor : maxDist;
        int eje = -1;
        float signo = 1.0f;
        bool afuera = false;
        for (int e = 0; e < 3 && !afuera; e++) {
            const float o = k.u[e].Dot(rel), d = k.u[e].Dot(dir), h = k.half[e];
            if (fabsf(d) < 1e-8f) {
                if (o < -h || o > h) afuera = true;
                continue;
            }
            float t1 = (-h - o) / d, t2 = (h - o) / d, sg = -1.0f;
            if (t1 > t2) { const float x = t1; t1 = t2; t2 = x; sg = 1.0f; }
            if (t1 > tMin) { tMin = t1; eje = e; signo = sg; }
            if (t2 < tMax) tMax = t2;
            if (tMin > tMax) afuera = true;
        }
        if (afuera) continue;
        mejor = tMin;
        if (cuerpo) *cuerpo = i;
        if (normal) *normal = eje >= 0 ? k.u[eje] * signo : -dir;
    }
    return mejor;
}

// ---------------------------------------------------------------------------
//  caja contra caja: SAT + manifold por recorte (lo de W3dRigido.cpp)
// ---------------------------------------------------------------------------
void W3dRigidoMundo::CaraDeCaja(const Cuerpo& k, int eje, float sg, Vector3 out[4]) const {
    const int e1 = (eje + 1) % 3, e2 = (eje + 2) % 3;
    const Vector3 centro = k.pos + k.u[eje] * (sg * k.half[eje]);
    const Vector3 a = k.u[e1] * k.half[e1];
    const Vector3 b = k.u[e2] * k.half[e2];
    out[0] = centro + a + b;
    out[1] = centro - a + b;
    out[2] = centro - a - b;
    out[3] = centro + a - b;
}

// recorta un poligono contra el semiespacio dot(n,p) <= d
static int RecortarPlano(const Vector3& n, float d, const Vector3* in, int nIn, Vector3* out) {
    int nOut = 0;
    for (int i = 0; i < nIn; i++) {
        const Vector3& a = in[i];
        const Vector3& b = in[(i + 1) % nIn];
        const float da = n.Dot(a) - d, db = n.Dot(b) - d;
        if (da <= 0.0f) out[nOut++] = a;
        if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) out[nOut++] = a + (b - a) * (da / (da - db));
        if (nOut >= 8) break;
    }
    return nOut;
}

bool W3dRigidoMundo::Colision(int ia, int ib, Manifold& m) {
    const Cuerpo& A = c[ia];
    const Cuerpo& B = c[ib];
    const Vector3 d = B.pos - A.pos;
    float hA[3], hB[3];
    for (int e = 0; e < 3; e++) { hA[e] = A.half[e]; hB[e] = B.half[e]; }
    float ac[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) ac[i][j] = fabsf(A.u[i].Dot(B.u[j])) + 1e-5f;
    float mejorCara = -1e30f;
    int caraEje = -1, caraDe = 0;
    for (int i = 0; i < 3; i++) {
        const float sep = fabsf(A.u[i].Dot(d)) - (hA[i] + hB[0] * ac[i][0] + hB[1] * ac[i][1] + hB[2] * ac[i][2]);
        if (sep > 0.0f) return false;
        if (sep > mejorCara) { mejorCara = sep; caraEje = i; caraDe = 0; }
    }
    for (int j = 0; j < 3; j++) {
        const float sep = fabsf(B.u[j].Dot(d)) - (hB[j] + hA[0] * ac[0][j] + hA[1] * ac[1][j] + hA[2] * ac[2][j]);
        if (sep > 0.0f) return false;
        if (sep > mejorCara) { mejorCara = sep; caraEje = j; caraDe = 1; }
    }
    float mejorArista = -1e30f;
    int aEjeA = -1, aEjeB = -1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vector3 eje = Vector3::Cross(A.u[i], B.u[j]);
            const float len = eje.Length();
            if (len < 1e-4f) continue;
            eje = eje * (1.0f / len);
            const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            // (cada semieje con SU dot)
            const float r = hA[i1] * fabsf(eje.Dot(A.u[i1])) + hA[i2] * fabsf(eje.Dot(A.u[i2])) +
                            hB[j1] * fabsf(eje.Dot(B.u[j1])) + hB[j2] * fabsf(eje.Dot(B.u[j2]));
            const float sep = fabsf(eje.Dot(d)) - r;
            if (sep > 0.0f) return false;
            if (sep > mejorArista) { mejorArista = sep; aEjeA = i; aEjeB = j; }
        }
    m.ia = ia; m.ib = ib; m.n = 0;
    const bool usarArista = aEjeA >= 0 && mejorArista > mejorCara * 0.95f + 0.005f;
    if (!usarArista) {
        const Cuerpo& R = caraDe == 0 ? A : B;
        const Cuerpo& I = caraDe == 0 ? B : A;
        const float sg = R.u[caraEje].Dot(I.pos - R.pos) >= 0.0f ? 1.0f : -1.0f;
        const Vector3 nRef = R.u[caraEje] * sg;
        int ejeInc = 0;
        float peor = 0.0f;
        for (int e = 0; e < 3; e++) {
            const float dp = I.u[e].Dot(nRef);
            if (e == 0 || fabsf(dp) > fabsf(peor)) { peor = dp; ejeInc = e; }
        }
        Vector3 cara[8], tmp[8];
        CaraDeCaja(I, ejeInc, peor > 0.0f ? -1.0f : 1.0f, cara);
        const int e1 = (caraEje + 1) % 3, e2 = (caraEje + 2) % 3;
        const float h1 = R.half[e1], h2 = R.half[e2];
        int n = 4;
        n = RecortarPlano(R.u[e1], R.u[e1].Dot(R.pos) + h1, cara, n, tmp);
        n = RecortarPlano(-R.u[e1], -(R.u[e1].Dot(R.pos) - h1), tmp, n, cara);
        n = RecortarPlano(R.u[e2], R.u[e2].Dot(R.pos) + h2, cara, n, tmp);
        n = RecortarPlano(-R.u[e2], -(R.u[e2].Dot(R.pos) - h2), tmp, n, cara);
        const float dCara = nRef.Dot(R.pos + nRef * R.half[caraEje]);
        Punto pts[8];
        int np = 0;
        for (int i = 0; i < n && np < 8; i++) {
            const float hund = dCara - nRef.Dot(cara[i]);
            if (hund > 0.0f) {
                pts[np].pos = cara[i];
                pts[np].hundido = hund;
                np++;
            }
        }
        // (mas de 4: los 4 mas hundidos)
        while (np > 4) {
            int menor = 0;
            for (int i = 1; i < np; i++) if (pts[i].hundido < pts[menor].hundido) menor = i;
            pts[menor] = pts[np - 1];
            np--;
        }
        for (int i = 0; i < np; i++) {
            m.p[i] = pts[i];
            m.p[i].acumN = m.p[i].acumT1 = m.p[i].acumT2 = m.p[i].vnInicial = 0.0f;
        }
        m.n = np;
        m.normal = caraDe == 0 ? nRef : -nRef;
    } else {
        Vector3 eje = Vector3::Cross(A.u[aEjeA], B.u[aEjeB]).Normalized();
        if (eje.Dot(d) < 0.0f) eje = -eje;
        Vector3 pA = A.pos, pB = B.pos;
        for (int e = 0; e < 3; e++) {
            if (e != aEjeA) pA += A.u[e] * ((eje.Dot(A.u[e]) >= 0.0f ? 1.0f : -1.0f) * hA[e]);
            if (e != aEjeB) pB -= B.u[e] * ((eje.Dot(B.u[e]) >= 0.0f ? 1.0f : -1.0f) * hB[e]);
        }
        const Vector3 dirA = A.u[aEjeA], dirB = B.u[aEjeB], r = pB - pA;
        const float ab = dirA.Dot(dirB), ra = dirA.Dot(r), rb = dirB.Dot(r);
        const float den = 1.0f - ab * ab;
        float s = fabsf(den) > 1e-8f ? (ra - rb * ab) / den : 0.0f;
        if (s > hA[aEjeA]) s = hA[aEjeA];
        if (s < -hA[aEjeA]) s = -hA[aEjeA];
        const Vector3 cA = pA + dirA * s;
        float t = dirB.Dot(cA - pB);
        if (t > hB[aEjeB]) t = hB[aEjeB];
        if (t < -hB[aEjeB]) t = -hB[aEjeB];
        const Vector3 cB = pB + dirB * t;
        m.p[0].pos = (cA + cB) * 0.5f;
        m.p[0].hundido = -mejorArista;
        m.p[0].acumN = m.p[0].acumT1 = m.p[0].acumT2 = m.p[0].vnInicial = 0.0f;
        m.n = 1;
        m.normal = eje;
    }
    return m.n > 0;
}

// ---------------------------------------------------------------------------
//  caja contra el mundo estatico: los contactos de la funcion del juego, un manifold por normal parecida
// ---------------------------------------------------------------------------
void W3dRigidoMundo::ContactosMundo(int i) {
    if (!mundoFn) return;
    W3dRmContacto out[CONTACTOS_MUNDO];
    const Cuerpo& k = c[i];
    int n = mundoFn(mundoDato, i, k.pos, k.u, k.half, out, CONTACTOS_MUNDO);
    if (n > CONTACTOS_MUNDO) n = CONTACTOS_MUNDO;
    const size_t primero = ms.size();
    for (int q = 0; q < n; q++) {
        const Vector3 nrm(out[q].normal[0], out[q].normal[1], out[q].normal[2]);
        const Vector3 pos(out[q].punto[0], out[q].punto[1], out[q].punto[2]);
        if (out[q].hundido <= 0.0f) continue;
        // (al manifold de este cuerpo con la misma normal: el piso es un plano con cuatro esquinas)
        Manifold* m = 0;
        for (size_t k2 = primero; k2 < ms.size(); k2++)
            if (ms[k2].normal.Dot(nrm) > 0.98f) { m = &ms[k2]; break; }
        if (!m) {
            Manifold nuevo;
            nuevo.ia = -1; nuevo.ib = i; nuevo.n = 0; nuevo.normal = nrm;
            ms.push_back(nuevo);
            m = &ms.back();
        }
        // (lleno: quedan los primeros, que el juego da en orden -las esquinas antes que los centros de las caras-;
        // cambiar el punto menos hundido por uno mas hundido dejaba el apoyo torcido y la caja se arrastraba sola)
        if (m->n >= 4) continue;
        Punto* p = &m->p[m->n++];
        p->pos = pos;
        p->hundido = out[q].hundido;
        p->acumN = p->acumT1 = p->acumT2 = p->vnInicial = 0.0f;
    }
}

// ---------------------------------------------------------------------------
//  solver de impulsos secuenciales
// ---------------------------------------------------------------------------
void W3dRigidoMundo::Resolver(float h) {
    for (size_t k = 0; k < ms.size(); k++) {
        Manifold& m = ms[k];
        for (int i = 0; i < m.n; i++) m.p[i].vnInicial = (VelEn(m.ib, m.p[i].pos) - VelEn(m.ia, m.p[i].pos)).Dot(m.normal);
    }
    const float hSesgo = h < def.subpasoMax * 0.999f ? def.subpasoMax : h;
    for (int iter = 0; iter < def.iteraciones; iter++) {
        for (size_t k = 0; k < ms.size(); k++) {
            Manifold& m = ms[k];
            const int ia = m.ia, ib = m.ib;
            Cuerpo* A = ia >= 0 ? &c[ia] : 0;
            Cuerpo& B = c[ib];
            const Vector3& n = m.normal;
            Vector3 t1 = Vector3::Cross(n, Vector3(0, 1, 0));
            if (t1.LengthSq() < 1e-6f) t1 = Vector3::Cross(n, Vector3(1, 0, 0));
            t1 = t1.Normalized();
            const Vector3 t2 = Vector3::Cross(n, t1);
            // (el mundo: friccion 0.6, sin rebote)
            const float mu = sqrtf((A ? A->friccion : 0.6f) * B.friccion);
            const float e = A && A->rebote > B.rebote ? A->rebote : B.rebote;
            const float imA = InvMasaDe(ia), imB = InvMasaDe(ib);
            const Vector3 pA = A ? A->pos : Vector3(0, 0, 0);
            for (int i = 0; i < m.n; i++) {
                Punto& p = m.p[i];
                const Vector3 ra = p.pos - pA, rb = p.pos - B.pos;
                float vn = (VelEn(ib, p.pos) - VelEn(ia, p.pos)).Dot(n);
                const float kn = imA + imB + n.Dot(Vector3::Cross(InerciaInv(ia, Vector3::Cross(ra, n)), ra)) +
                                 n.Dot(Vector3::Cross(InerciaInv(ib, Vector3::Cross(rb, n)), rb));
                if (kn < 1e-8f) continue;
                float sesgo = (def.baumgarte / hSesgo) * (p.hundido - def.hundidoOk);
                if (sesgo < 0.0f) sesgo = 0.0f;
                if (sesgo > def.sesgoMax) sesgo = def.sesgoMax;
                const float rebote = p.vnInicial < -def.umbralRebote ? -e * p.vnInicial : 0.0f;
                float dj = (-vn + sesgo + rebote) / kn;
                const float a0 = p.acumN;
                p.acumN = a0 + dj;
                if (p.acumN < 0.0f) p.acumN = 0.0f;
                dj = p.acumN - a0;
                const Vector3 imp = n * dj;
                if (A) { A->vel -= imp * imA; A->velAng -= InerciaInv(ia, Vector3::Cross(ra, imp)); }
                B.vel += imp * imB;
                B.velAng += InerciaInv(ib, Vector3::Cross(rb, imp));
                const float tope = mu * p.acumN;
                for (int tt = 0; tt < 2; tt++) {
                    const Vector3& t = tt == 0 ? t1 : t2;
                    float* acumT = tt == 0 ? &p.acumT1 : &p.acumT2;
                    const float vt = (VelEn(ib, p.pos) - VelEn(ia, p.pos)).Dot(t);
                    const float kt = imA + imB + t.Dot(Vector3::Cross(InerciaInv(ia, Vector3::Cross(ra, t)), ra)) +
                                     t.Dot(Vector3::Cross(InerciaInv(ib, Vector3::Cross(rb, t)), rb));
                    if (kt < 1e-8f) continue;
                    const float t0 = *acumT;
                    *acumT = t0 - vt / kt;
                    if (*acumT > tope) *acumT = tope;
                    if (*acumT < -tope) *acumT = -tope;
                    const Vector3 impT = t * (*acumT - t0);
                    if (A) { A->vel -= impT * imA; A->velAng -= InerciaInv(ia, Vector3::Cross(ra, impT)); }
                    B.vel += impT * imB;
                    B.velAng += InerciaInv(ib, Vector3::Cross(rb, impT));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  el paso
// ---------------------------------------------------------------------------
void W3dRigidoMundo::Paso(float dt) {
    nDespiertos = 0;
    for (size_t i = 0; i < c.size(); i++) { c[i].movido = false; c[i].golpe = 0.0f; }
    if (dt <= 0.0f || c.empty()) return;
    if (dt > 0.1f) dt = 0.1f;
    // los despiertos (los unicos que se integran y miran contactos)
    despiertos.clear();
    for (int i = 0; i < (int)c.size(); i++)
        if (c[i].vivo && c[i].activo && !c[i].fijo && !c[i].dormido) despiertos.push_back(i);
    if (despiertos.empty()) return;
    int subpasos = def.subpasosMin;
    while (dt / (float)subpasos > def.subpasoMax * 1.001f) subpasos++;
    const float h = dt / (float)subpasos;
    for (int paso = 0; paso < subpasos; paso++) {
        for (size_t q = 0; q < despiertos.size(); q++) {
            Cuerpo& k = c[despiertos[q]];
            if (k.dormido) continue;
            if (!k.sinGravedad && !k.conEje) k.vel += def.gravedad * h;
            if (def.amortiguarLin > 0.0f) k.vel = k.vel * (1.0f / (1.0f + h * def.amortiguarLin));
            if (def.amortiguarAng > 0.0f) k.velAng = k.velAng * (1.0f / (1.0f + h * def.amortiguarAng));
        }
        ms.clear();
        nuevos.clear();
        for (size_t q = 0; q < despiertos.size(); q++) {
            const int i = despiertos[q];
            Cuerpo& A = c[i];
            if (A.dormido) continue;
            ContactosMundo(i);
            const float ra = A.half.Length();
            for (int j = 0; j < (int)c.size(); j++) {
                if (j == i) continue;
                Cuerpo& B = c[j];
                if (!B.vivo || !B.activo || (A.grupo & B.grupo)) continue;
                // (dos despiertos: el par una vez, desde el de indice menor)
                const bool bDespierto = !B.fijo && !B.dormido;
                if (bDespierto && j < i) continue;
                const float rb = B.half.Length();
                if ((B.pos - A.pos).LengthSq() > (ra + rb) * (ra + rb)) continue;
                Manifold m;
                if (!Colision(i, j, m)) continue;
                // (el toque despierta al dormido si este se mueve de verdad: se integra desde el sub-paso que sigue)
                if (B.dormido && !B.fijo && (A.vel.LengthSq() > def.despertarVel2 || A.velAng.LengthSq() > def.despertarVel2))
                    nuevos.push_back(j);
                ms.push_back(m);
            }
        }
        if (!ms.empty()) Resolver(h);
        for (size_t q = 0; q < nuevos.size(); q++)
            if (c[nuevos[q]].dormido) { Despertar(nuevos[q]); despiertos.push_back(nuevos[q]); }
        for (size_t q = 0; q < despiertos.size(); q++) {
            Cuerpo& k = c[despiertos[q]];
            if (k.dormido) continue;
            if (k.conEje) {   // (la bisagra: girando solo sobre su eje; el centro, en su brazo alrededor del pivote)
                k.velAng = k.eje * k.eje.Dot(k.velAng);
                k.vel = Vector3::Cross(k.velAng, k.pos - k.pivote);
            }
            k.pos += k.vel * h;
            if (k.velAng.LengthSq() > 1e-10f) {
                const Quaternion w(0.0f, k.velAng.x, k.velAng.y, k.velAng.z);
                const Quaternion dq = w * k.rot;
                k.rot.w += 0.5f * h * dq.w;
                k.rot.x += 0.5f * h * dq.x;
                k.rot.y += 0.5f * h * dq.y;
                k.rot.z += 0.5f * h * dq.z;
                k.rot.normalize();
            }
            Actualizar(k);
            if (k.conEje) k.pos = k.pivote + k.u[0] * k.brazo.x + k.u[1] * k.brazo.y + k.u[2] * k.brazo.z;
            k.movido = true;
        }
        for (size_t k = 0; k < ms.size(); k++) {
            float total = 0.0f;
            for (int i = 0; i < ms[k].n; i++) total += ms[k].p[i].acumN;
            c[ms[k].ib].golpe += total;
            if (ms[k].ia >= 0) c[ms[k].ia].golpe += total;
        }
    }
    // dormir a los quietos
    for (size_t q = 0; q < despiertos.size(); q++) {
        Cuerpo& k = c[despiertos[q]];
        if (k.dormido) continue;
        nDespiertos++;
        // (con histeresis: el temblor de las cajas apiladas -contactos que se rehacen en cada paso- no vuelve a empezar
        // la cuenta; solo un movimiento de verdad, cuatro veces el umbral)
        const float l2 = k.vel.LengthSq(), a2 = k.velAng.LengthSq();
        if (l2 < def.dormirLin2 && a2 < def.dormirAng2) {
            k.quietud += subpasos;
            if (k.quietud >= def.dormirPasos) {
                k.dormido = true;
                k.vel = Vector3(0, 0, 0);
                k.velAng = Vector3(0, 0, 0);
            }
        } else if (l2 > def.dormirLin2 * 4.0f || a2 > def.dormirAng2 * 4.0f) {
            k.quietud = 0;
        }
    }
}
