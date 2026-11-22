// ============================================================================
//  W3dRigidWorld.cpp — ver W3dRigidWorld.h. Dialecto C++03 (Symbian compila esto).
//  Comentarios en espanol sin acentos.
//
//  La deteccion de caja contra caja y el solver son los de W3dRigid.cpp (SAT de
//  15 ejes con cada semieje por SU dot en los ejes cruzados, recorte de la cara
//  incidente, impulsos secuenciales con sesgo medido contra un paso de 1/60 s);
//  aca trabajan sobre indices en vez de Object y con un "cuerpo" -1 que es el
//  mundo estatico (masa infinita, quieto).
// ============================================================================
#include "physics/W3dRigidWorld.h"
#include <math.h>

static const int CONTACTS_WORLD = 16;   // contactos contra el mundo por cuerpo y sub-paso

W3dRigidWorld::W3dRigidWorld() : worldFn(0), worldData(0), nAwake(0) {}

void W3dRigidWorld::Clear() {
    c.clear();
    ms.clear();
    awake.clear();
    nAwake = 0;
}

void W3dRigidWorld::Update(Body& k) {
    k.u[0] = k.rot * Vector3(1, 0, 0);
    k.u[1] = k.rot * Vector3(0, 1, 0);
    k.u[2] = k.rot * Vector3(0, 0, 1);
}

// I_mundo^-1 * v (R * diag(invI) * R^T * v); el mundo (-1) y los dormidos no giran (para el solver un dormido es
// fijo hasta que algo lo despierte: si no, junta velocidad que no integra y salta al despertarse)
Vector3 W3dRigidWorld::InertiaInv(int i, const Vector3& v) const {
    if (i < 0 || c[i].asleep) return Vector3(0, 0, 0);
    const Body& k = c[i];
    return k.u[0] * (k.invInertia.x * k.u[0].Dot(v)) + k.u[1] * (k.invInertia.y * k.u[1].Dot(v)) +
           k.u[2] * (k.invInertia.z * k.u[2].Dot(v));
}

Vector3 W3dRigidWorld::VelIn(int i, const Vector3& p) const {
    if (i < 0) return Vector3(0, 0, 0);
    const Body& k = c[i];
    return k.vel + Vector3::Cross(k.velAng, p - k.pos);
}

// ---------------------------------------------------------------------------
//  cuerpos
// ---------------------------------------------------------------------------
int W3dRigidWorld::Add(const Vector3& center, const Quaternion& rot, const Vector3& semi, float mass,
                            float friction, float bounce) {
    Body k;
    k.pos = center;
    k.rot = rot.Normalized();
    k.half = semi;
    if (k.half.x < 0.001f) k.half.x = 0.001f;
    if (k.half.y < 0.001f) k.half.y = 0.001f;
    if (k.half.z < 0.001f) k.half.z = 0.001f;
    const float m = mass > 0.0001f ? mass : 1.0f;
    // inercia de una caja maciza: I = m/12 * (a^2 + b^2) por eje
    const float ex = 2.0f * k.half.x, ey = 2.0f * k.half.y, ez = 2.0f * k.half.z;
    k.invSavedInertia = Vector3(12.0f / (m * (ey * ey + ez * ez)), 12.0f / (m * (ex * ex + ez * ez)),
                                   12.0f / (m * (ex * ex + ey * ey)));
    k.invSavedMass = 1.0f / m;
    k.fixed = mass <= 0.0f;
    k.invMass = k.fixed ? 0.0f : k.invSavedMass;
    k.invInertia = k.fixed ? Vector3(0, 0, 0) : k.invSavedInertia;
    k.friction = friction;
    k.bounce = bounce;
    k.hit   = 0.0f;
    k.group = 0;
    k.stillness = 0;
    k.live = true;
    k.asleep = true;
    k.active = true;
    k.noGravity = false;
    k.moved    = false;
    k.withAxis = false;
    Update(k);
    c.push_back(k);
    return (int)c.size() - 1;
}

void W3dRigidWorld::Remove(int i) {
    if (!Exists(i)) return;
    WakeNeighbors(i, 0.05f);
    c[i].live = false;
    c[i].active = false;
}

void W3dRigidWorld::Pose(int i, Vector3* center, Quaternion* rot) const {
    if (i < 0 || i >= (int)c.size()) return;
    if (center) *center = c[i].pos;
    if (rot) *rot = c[i].rot;
}
void W3dRigidWorld::Axes(int i, Vector3 axes[3]) const {
    if (i < 0 || i >= (int)c.size()) return;
    axes[0] = c[i].u[0]; axes[1] = c[i].u[1]; axes[2] = c[i].u[2];
}
Vector3 W3dRigidWorld::Semi(int i) const { return (i >= 0 && i < (int)c.size()) ? c[i].half : Vector3(0, 0, 0); }
float W3dRigidWorld::Mass(int i) const {
    return (i >= 0 && i < (int)c.size()) ? 1.0f / c[i].invSavedMass : 0.0f;
}
void W3dRigidWorld::SetPose(int i, const Vector3& center, const Quaternion& rot) {
    if (!Exists(i)) return;
    c[i].pos = center;
    c[i].rot = rot.Normalized();
    Update(c[i]);
    c[i].moved = true;
    Wake(i);
}
Vector3 W3dRigidWorld::Vel(int i) const { return Exists(i) ? c[i].vel : Vector3(0, 0, 0); }
Vector3 W3dRigidWorld::VelAng(int i) const { return Exists(i) ? c[i].velAng : Vector3(0, 0, 0); }
void W3dRigidWorld::SetVel(int i, const Vector3& v) {
    if (!Exists(i) || c[i].fixed) return;
    c[i].vel = v;
    Wake(i);
}
void W3dRigidWorld::SetVelAng(int i, const Vector3& w) {
    if (!Exists(i) || c[i].fixed) return;
    c[i].velAng = w;
    Wake(i);
}
void W3dRigidWorld::Impulse(int i, const Vector3& imp, const Vector3& point) {
    if (!Exists(i) || c[i].fixed) return;
    c[i].vel += imp * c[i].invMass;
    c[i].velAng += InertiaInv(i, Vector3::Cross(point - c[i].pos, imp));
    Wake(i);
}
void W3dRigidWorld::Wake(int i) {
    if (!Exists(i) || c[i].fixed) return;
    c[i].asleep    = false;
    c[i].stillness = 0;
}
void W3dRigidWorld::WakeNeighbors(int i, float margin) {
    if (!Exists(i)) return;
    Vector3 mn, mx;
    Box(i, &mn, &mx);
    for (int j = 0; j < (int)c.size(); j++) {
        if (j == i || !c[j].live || !c[j].asleep) continue;
        Vector3 a, b;
        Box(j, &a, &b);
        if (a.x > mx.x + margin || b.x < mn.x - margin || a.y > mx.y + margin || b.y < mn.y - margin ||
            a.z > mx.z + margin || b.z < mn.z - margin)
            continue;
        Wake(j);
    }
}
bool W3dRigidWorld::Asleep(int i) const { return !Exists(i) || c[i].asleep || c[i].fixed; }
void W3dRigidWorld::SetActive(int i, bool yes) {
    if (!Exists(i) || c[i].active == yes) return;
    if (!yes) WakeNeighbors(i, 0.05f);
    c[i].active = yes;
    if (yes) Wake(i);
}
bool W3dRigidWorld::Active(int i) const { return Exists(i) && c[i].active; }
void W3dRigidWorld::SetFixed(int i, bool fixed) {
    if (!Exists(i) || c[i].fixed == fixed) return;
    Body& k = c[i];
    k.fixed = fixed;
    k.invMass = fixed ? 0.0f : k.invSavedMass;
    k.invInertia = fixed ? Vector3(0, 0, 0) : k.invSavedInertia;
    k.vel = Vector3(0, 0, 0);
    k.velAng = Vector3(0, 0, 0);
    k.asleep = true;
    if (!fixed) Wake(i);
}
bool W3dRigidWorld::Fixed(int i) const { return Exists(i) && c[i].fixed; }
void W3dRigidWorld::SetNoGravity(int i, bool yes) {
    if (Exists(i)) c[i].noGravity = yes;
}
void W3dRigidWorld::SetAxis(int i, const Vector3* axis, const Vector3* pivot) {
    if (!Exists(i)) return;
    Body& k = c[i];
    k.withAxis = axis != 0;
    if (!axis) return;
    k.axis = axis->Normalized();
    // (el punto de la recta mas cerca del centro: el brazo queda perpendicular al eje)
    const Vector3 p = pivot ? *pivot : k.pos;
    k.pivot = p + k.axis * k.axis.Dot(k.pos - p);
    const Vector3 b = k.pos - k.pivot;
    k.arm = Vector3(k.u[0].Dot(b), k.u[1].Dot(b), k.u[2].Dot(b));
}
void W3dRigidWorld::SetGroup(int i, unsigned noCollision) {
    if (Exists(i)) c[i].group = noCollision;
}
bool W3dRigidWorld::Moved(int i) const { return Exists(i) && c[i].moved; }
float W3dRigidWorld::HitImpulse(int i) const { return Exists(i) ? c[i].hit : 0.0f; }

void W3dRigidWorld::Box(int i, Vector3* mn, Vector3* mx) const {
    const Body& k = c[i];
    const Vector3 e(fabsf(k.u[0].x) * k.half.x + fabsf(k.u[1].x) * k.half.y + fabsf(k.u[2].x) * k.half.z,
                    fabsf(k.u[0].y) * k.half.x + fabsf(k.u[1].y) * k.half.y + fabsf(k.u[2].y) * k.half.z,
                    fabsf(k.u[0].z) * k.half.x + fabsf(k.u[1].z) * k.half.y + fabsf(k.u[2].z) * k.half.z);
    if (mn) *mn = k.pos - e;
    if (mx) *mx = k.pos + e;
}

// ---------------------------------------------------------------------------
//  rayo contra las cajas (losas en el espacio de cada una)
// ---------------------------------------------------------------------------
float W3dRigidWorld::Ray(const Vector3& origin, const Vector3& dir, float maxDist, int ignore, int* body,
                           Vector3* normal) const {
    float best = -1.0f;
    for (int i = 0; i < (int)c.size(); i++) {
        const Body& k = c[i];
        if (i == ignore || !k.live || !k.active) continue;
        const Vector3 rel = origin - k.pos;
        float tMin = 0.0f, tMax = (best >= 0.0f) ? best : maxDist;
        int axis = -1;
        float sign   = 1.0f;
        bool outside = false;
        for (int e = 0; e < 3 && !outside; e++) {
            const float o = k.u[e].Dot(rel), d = k.u[e].Dot(dir), h = k.half[e];
            if (fabsf(d) < 1e-8f) {
                if (o < -h || o > h) outside = true;
                continue;
            }
            float t1 = (-h - o) / d, t2 = (h - o) / d, sg = -1.0f;
            if (t1 > t2) { const float x = t1; t1 = t2; t2 = x; sg = 1.0f; }
            if (t1 > tMin) { tMin = t1; axis = e; sign = sg; }
            if (t2 < tMax) tMax = t2;
            if (tMin > tMax) outside = true;
        }
        if (outside) continue;
        best = tMin;
        if (body) *body = i;
        if (normal) *normal = axis >= 0 ? k.u[axis] * sign : -dir;
    }
    return best;
}

// ---------------------------------------------------------------------------
//  caja contra caja: SAT + manifold por recorte (lo de W3dRigid.cpp)
// ---------------------------------------------------------------------------
void W3dRigidWorld::BoxFace(const Body& k, int axis, float sg, Vector3 out[4]) const {
    const int e1 = (axis + 1) % 3, e2 = (axis + 2) % 3;
    const Vector3 center = k.pos + k.u[axis] * (sg * k.half[axis]);
    const Vector3 a = k.u[e1] * k.half[e1];
    const Vector3 b = k.u[e2] * k.half[e2];
    out[0] = center + a + b;
    out[1] = center - a + b;
    out[2] = center - a - b;
    out[3] = center + a - b;
}

// recorta un poligono contra el semiespacio dot(n,p) <= d
static int CropPlane(const Vector3& n, float d, const Vector3* in, int nIn, Vector3* out) {
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

bool W3dRigidWorld::Collision(int ia, int ib, Manifold& m) {
    const Body& A   = c[ia];
    const Body& B   = c[ib];
    const Vector3 d = B.pos - A.pos;
    float hA[3], hB[3];
    for (int e = 0; e < 3; e++) { hA[e] = A.half[e]; hB[e] = B.half[e]; }
    float ac[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) ac[i][j] = fabsf(A.u[i].Dot(B.u[j])) + 1e-5f;
    float bestFace = -1e30f;
    int faceAxis = -1, faceOf = 0;
    for (int i = 0; i < 3; i++) {
        const float sep = fabsf(A.u[i].Dot(d)) - (hA[i] + hB[0] * ac[i][0] + hB[1] * ac[i][1] + hB[2] * ac[i][2]);
        if (sep > 0.0f) return false;
        if (sep > bestFace) { bestFace = sep; faceAxis = i; faceOf = 0; }
    }
    for (int j = 0; j < 3; j++) {
        const float sep = fabsf(B.u[j].Dot(d)) - (hB[j] + hA[0] * ac[0][j] + hA[1] * ac[1][j] + hA[2] * ac[2][j]);
        if (sep > 0.0f) return false;
        if (sep > bestFace) { bestFace = sep; faceAxis = j; faceOf = 1; }
    }
    float bestEdge = -1e30f;
    int aAxisA = -1, aAxisB = -1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vector3 axis = Vector3::Cross(A.u[i], B.u[j]);
            const float len = axis.Length();
            if (len < 1e-4f) continue;
            axis = axis * (1.0f / len);
            const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            // (cada semieje con SU dot)
            const float r = hA[i1] * fabsf(axis.Dot(A.u[i1])) + hA[i2] * fabsf(axis.Dot(A.u[i2])) +
                            hB[j1] * fabsf(axis.Dot(B.u[j1])) + hB[j2] * fabsf(axis.Dot(B.u[j2]));
            const float sep = fabsf(axis.Dot(d)) - r;
            if (sep > 0.0f) return false;
            if (sep > bestEdge) { bestEdge = sep; aAxisA = i; aAxisB = j; }
        }
    m.ia = ia; m.ib = ib; m.n = 0;
    const bool useEdge = aAxisA >= 0 && bestEdge > bestFace * 0.95f + 0.005f;
    if (!useEdge) {
        const Body& R = faceOf == 0 ? A : B;
        const Body& I = faceOf == 0 ? B : A;
        const float sg = R.u[faceAxis].Dot(I.pos - R.pos) >= 0.0f ? 1.0f : -1.0f;
        const Vector3 nRef = R.u[faceAxis] * sg;
        int axisInc = 0;
        float worst = 0.0f;
        for (int e = 0; e < 3; e++) {
            const float dp = I.u[e].Dot(nRef);
            if (e == 0 || fabsf(dp) > fabsf(worst)) { worst = dp; axisInc = e; }
        }
        Vector3 face[8], tmp[8];
        BoxFace(I, axisInc, worst > 0.0f ? -1.0f : 1.0f, face);
        const int e1 = (faceAxis + 1) % 3, e2 = (faceAxis + 2) % 3;
        const float h1 = R.half[e1], h2 = R.half[e2];
        int n = 4;
        n = CropPlane(R.u[e1], R.u[e1].Dot(R.pos) + h1, face, n, tmp);
        n = CropPlane(-R.u[e1], -(R.u[e1].Dot(R.pos) - h1), tmp, n, face);
        n = CropPlane(R.u[e2], R.u[e2].Dot(R.pos) + h2, face, n, tmp);
        n = CropPlane(-R.u[e2], -(R.u[e2].Dot(R.pos) - h2), tmp, n, face);
        const float dFace = nRef.Dot(R.pos + nRef * R.half[faceAxis]);
        Point pts[8];
        int np = 0;
        for (int i = 0; i < n && np < 8; i++) {
            const float hund = dFace - nRef.Dot(face[i]);
            if (hund > 0.0f) {
                pts[np].pos = face[i];
                pts[np].sunk = hund;
                np++;
            }
        }
        // (mas de 4: los 4 mas hundidos)
        while (np > 4) {
            int lower = 0;
            for (int i = 1; i < np; i++) if (pts[i].sunk < pts[lower].sunk) lower = i;
            pts[lower] = pts[np - 1];
            np--;
        }
        for (int i = 0; i < np; i++) {
            m.p[i] = pts[i];
            m.p[i].accumN = m.p[i].accumT1 = m.p[i].accumT2 = m.p[i].vnInitial = 0.0f;
        }
        m.n = np;
        m.normal = faceOf == 0 ? nRef : -nRef;
    } else {
        Vector3 axis = Vector3::Cross(A.u[aAxisA], B.u[aAxisB]).Normalized();
        if (axis.Dot(d) < 0.0f) axis = -axis;
        Vector3 pA = A.pos, pB = B.pos;
        for (int e = 0; e < 3; e++) {
            if (e != aAxisA) pA += A.u[e] * ((axis.Dot(A.u[e]) >= 0.0f ? 1.0f : -1.0f) * hA[e]);
            if (e != aAxisB) pB -= B.u[e] * ((axis.Dot(B.u[e]) >= 0.0f ? 1.0f : -1.0f) * hB[e]);
        }
        const Vector3 dirA = A.u[aAxisA], dirB = B.u[aAxisB], r = pB - pA;
        const float ab = dirA.Dot(dirB), ra = dirA.Dot(r), rb = dirB.Dot(r);
        const float den = 1.0f - ab * ab;
        float s = fabsf(den) > 1e-8f ? (ra - rb * ab) / den : 0.0f;
        if (s > hA[aAxisA]) s = hA[aAxisA];
        if (s < -hA[aAxisA]) s = -hA[aAxisA];
        const Vector3 cA = pA + dirA * s;
        float t = dirB.Dot(cA - pB);
        if (t > hB[aAxisB]) t = hB[aAxisB];
        if (t < -hB[aAxisB]) t = -hB[aAxisB];
        const Vector3 cB = pB + dirB * t;
        m.p[0].pos = (cA + cB) * 0.5f;
        m.p[0].sunk = -bestEdge;
        m.p[0].accumN = m.p[0].accumT1 = m.p[0].accumT2 = m.p[0].vnInitial = 0.0f;
        m.n = 1;
        m.normal = axis;
    }
    return m.n > 0;
}

// ---------------------------------------------------------------------------
//  caja contra el mundo estatico: los contactos de la funcion del juego, un manifold por normal parecida
// ---------------------------------------------------------------------------
void W3dRigidWorld::ContactsWorld(int i) {
    if (!worldFn) return;
    W3dRmContact out[CONTACTS_WORLD];
    const Body& k = c[i];
    int n = worldFn(worldData, i, k.pos, k.u, k.half, out, CONTACTS_WORLD);
    if (n > CONTACTS_WORLD) n = CONTACTS_WORLD;
    const size_t first = ms.size();
    for (int q = 0; q < n; q++) {
        const Vector3 nrm(out[q].normal[0], out[q].normal[1], out[q].normal[2]);
        const Vector3 pos(out[q].point[0], out[q].point[1], out[q].point[2]);
        if (out[q].sunk <= 0.0f) continue;
        // (al manifold de este cuerpo con la misma normal: el piso es un plano con cuatro esquinas)
        Manifold* m = 0;
        for (size_t k2 = first; k2 < ms.size(); k2++)
            if (ms[k2].normal.Dot(nrm) > 0.98f) { m = &ms[k2]; break; }
        if (!m) {
            Manifold newValue;
            newValue.ia = -1; newValue.ib = i; newValue.n = 0; newValue.normal = nrm;
            ms.push_back(newValue);
            m = &ms.back();
        }
        // (lleno: quedan los primeros, que el juego da en orden -las esquinas antes que los centros de las caras-;
        // cambiar el punto menos hundido por uno mas hundido dejaba el apoyo torcido y la caja se arrastraba sola)
        if (m->n >= 4) continue;
        Point* p = &m->p[m->n++];
        p->pos = pos;
        p->sunk = out[q].sunk;
        p->accumN = p->accumT1 = p->accumT2 = p->vnInitial = 0.0f;
    }
}

// ---------------------------------------------------------------------------
//  solver de impulsos secuenciales
// ---------------------------------------------------------------------------
void W3dRigidWorld::Resolve(float h) {
    for (size_t k = 0; k < ms.size(); k++) {
        Manifold& m = ms[k];
        for (int i = 0; i < m.n; i++) m.p[i].vnInitial = (VelIn(m.ib, m.p[i].pos) - VelIn(m.ia, m.p[i].pos)).Dot(m.normal);
    }
    const float hBias = h < def.substepMax * 0.999f ? def.substepMax : h;
    for (int iter = 0; iter < def.iterations; iter++) {
        for (size_t k = 0; k < ms.size(); k++) {
            Manifold& m = ms[k];
            const int ia = m.ia, ib = m.ib;
            Body* A = ia >= 0 ? &c[ia] : 0;
            Body& B = c[ib];
            const Vector3& n = m.normal;
            Vector3 t1 = Vector3::Cross(n, Vector3(0, 1, 0));
            if (t1.LengthSq() < 1e-6f) t1 = Vector3::Cross(n, Vector3(1, 0, 0));
            t1 = t1.Normalized();
            const Vector3 t2 = Vector3::Cross(n, t1);
            // (el mundo: friccion 0.6, sin rebote)
            const float mu = sqrtf((A ? A->friction : 0.6f) * B.friction);
            const float e = A && A->bounce > B.bounce ? A->bounce : B.bounce;
            const float imA = InvMassOf(ia), imB = InvMassOf(ib);
            const Vector3 pA = A ? A->pos : Vector3(0, 0, 0);
            for (int i = 0; i < m.n; i++) {
                Point& p = m.p[i];
                const Vector3 ra = p.pos - pA, rb = p.pos - B.pos;
                float vn = (VelIn(ib, p.pos) - VelIn(ia, p.pos)).Dot(n);
                const float kn = imA + imB + n.Dot(Vector3::Cross(InertiaInv(ia, Vector3::Cross(ra, n)), ra)) +
                                 n.Dot(Vector3::Cross(InertiaInv(ib, Vector3::Cross(rb, n)), rb));
                if (kn < 1e-8f) continue;
                float bias = (def.baumgarte / hBias) * (p.sunk - def.sunkOk);
                if (bias < 0.0f) bias = 0.0f;
                if (bias > def.biasMax) bias = def.biasMax;
                const float bounce = p.vnInitial < -def.thresholdBounce ? -e * p.vnInitial : 0.0f;
                float dj = (-vn + bias + bounce) / kn;
                const float a0 = p.accumN;
                p.accumN = a0 + dj;
                if (p.accumN < 0.0f) p.accumN = 0.0f;
                dj = p.accumN - a0;
                const Vector3 imp = n * dj;
                if (A) { A->vel -= imp * imA; A->velAng -= InertiaInv(ia, Vector3::Cross(ra, imp)); }
                B.vel += imp * imB;
                B.velAng += InertiaInv(ib, Vector3::Cross(rb, imp));
                const float limit = mu * p.accumN;
                for (int tt = 0; tt < 2; tt++) {
                    const Vector3& t = tt == 0 ? t1 : t2;
                    float* accumT = tt == 0 ? &p.accumT1 : &p.accumT2;
                    const float vt = (VelIn(ib, p.pos) - VelIn(ia, p.pos)).Dot(t);
                    const float kt = imA + imB + t.Dot(Vector3::Cross(InertiaInv(ia, Vector3::Cross(ra, t)), ra)) +
                                     t.Dot(Vector3::Cross(InertiaInv(ib, Vector3::Cross(rb, t)), rb));
                    if (kt < 1e-8f) continue;
                    const float t0 = *accumT;
                    *accumT = t0 - vt / kt;
                    if (*accumT > limit) *accumT = limit;
                    if (*accumT < -limit) *accumT = -limit;
                    const Vector3 impT = t * (*accumT - t0);
                    if (A) { A->vel -= impT * imA; A->velAng -= InertiaInv(ia, Vector3::Cross(ra, impT)); }
                    B.vel += impT * imB;
                    B.velAng += InertiaInv(ib, Vector3::Cross(rb, impT));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  el paso
// ---------------------------------------------------------------------------
void W3dRigidWorld::Step(float dt) {
    nAwake = 0;
    for (size_t i = 0; i < c.size(); i++) { c[i].moved = false; c[i].hit = 0.0f; }
    if (dt <= 0.0f || c.empty()) return;
    if (dt > 0.1f) dt = 0.1f;
    // los despiertos (los unicos que se integran y miran contactos)
    awake.clear();
    for (int i = 0; i < (int)c.size(); i++)
        if (c[i].live && c[i].active && !c[i].fixed && !c[i].asleep) awake.push_back(i);
    if (awake.empty()) return;
    int substeps = def.substepsMin;
    while (dt / (float)substeps > def.substepMax * 1.001f) substeps++;
    const float h = dt / (float)substeps;
    for (int step = 0; step < substeps; step++) {
        for (size_t q = 0; q < awake.size(); q++) {
            Body& k = c[awake[q]];
            if (k.asleep) continue;
            if (!k.noGravity && !k.withAxis) k.vel += def.gravity * h;
            if (def.dampLin > 0.0f) k.vel = k.vel * (1.0f / (1.0f + h * def.dampLin));
            if (def.dampAng > 0.0f) k.velAng = k.velAng * (1.0f / (1.0f + h * def.dampAng));
        }
        ms.clear();
        newValue.clear();
        for (size_t q = 0; q < awake.size(); q++) {
            const int i = awake[q];
            Body& A = c[i];
            if (A.asleep) continue;
            ContactsWorld(i);
            const float ra = A.half.Length();
            for (int j = 0; j < (int)c.size(); j++) {
                if (j == i) continue;
                Body& B = c[j];
                if (!B.live || !B.active || (A.group & B.group)) continue;
                // (dos despiertos: el par una vez, desde el de indice menor)
                const bool bAwake = !B.fixed && !B.asleep;
                if (bAwake && j < i) continue;
                const float rb = B.half.Length();
                if ((B.pos - A.pos).LengthSq() > (ra + rb) * (ra + rb)) continue;
                Manifold m;
                if (!Collision(i, j, m)) continue;
                // (el toque despierta al dormido si este se mueve de verdad: se integra desde el sub-paso que sigue)
                if (B.asleep && !B.fixed && (A.vel.LengthSq() > def.wakeVel2 || A.velAng.LengthSq() > def.wakeVel2))
                    newValue.push_back(j);
                ms.push_back(m);
            }
        }
        if (!ms.empty()) Resolve(h);
        for (size_t q = 0; q < newValue.size(); q++)
            if (c[newValue[q]].asleep) { Wake(newValue[q]); awake.push_back(newValue[q]); }
        for (size_t q = 0; q < awake.size(); q++) {
            Body& k = c[awake[q]];
            if (k.asleep) continue;
            if (k.withAxis) {   // (la bisagra: girando solo sobre su eje; el centro, en su brazo alrededor del pivote)
                k.velAng = k.axis * k.axis.Dot(k.velAng);
                k.vel = Vector3::Cross(k.velAng, k.pos - k.pivot);
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
            Update(k);
            if (k.withAxis) k.pos = k.pivot + k.u[0] * k.arm.x + k.u[1] * k.arm.y + k.u[2] * k.arm.z;
            k.moved = true;
        }
        for (size_t k = 0; k < ms.size(); k++) {
            float total = 0.0f;
            for (int i = 0; i < ms[k].n; i++) total += ms[k].p[i].accumN;
            c[ms[k].ib].hit += total;
            if (ms[k].ia >= 0) c[ms[k].ia].hit += total;
        }
    }
    // dormir a los quietos
    for (size_t q = 0; q < awake.size(); q++) {
        Body& k = c[awake[q]];
        if (k.asleep) continue;
        nAwake++;
        // (con histeresis: el temblor de las cajas apiladas -contactos que se rehacen en cada paso- no vuelve a empezar
        // la cuenta; solo un movimiento de verdad, cuatro veces el umbral)
        const float l2 = k.vel.LengthSq(), a2 = k.velAng.LengthSq();
        if (l2 < def.sleepLin2 && a2 < def.sleepAng2) {
            k.stillness += substeps;
            if (k.stillness >= def.sleepSteps) {
                k.asleep = true;
                k.vel = Vector3(0, 0, 0);
                k.velAng = Vector3(0, 0, 0);
            }
        } else if (l2 > def.sleepLin2 * 4.0f || a2 > def.sleepAng2 * 4.0f) {
            k.stillness = 0;
        }
    }
}
