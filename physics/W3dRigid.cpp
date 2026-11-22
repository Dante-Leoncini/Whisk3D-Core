// ============================================================================
//  W3dRigid.cpp — ver W3dRigid.h. Dialecto C++03 (Symbian compila esto).
//  Comentarios en espanol sin acentos.
//
//  La tecnica es la clasica de los motores de impulsos (de libro):
//    * deteccion OBB vs OBB por SAT (15 ejes) + manifold por RECORTE de la
//      cara incidente contra la cara de referencia (Sutherland-Hodgman)
//    * resolucion por IMPULSOS SECUENCIALES iterados, con impulso normal
//      acumulado, friccion de Coulomb en dos tangentes y un sesgo posicional
//      (Baumgarte) para que nada quede hundido
//    * integracion semi-implicita (primero velocidad, despues posicion), en
//      DOS sub-pasos por frame para que 30 fps sea estable (mas si el frame es
//      lento: ningun sub-paso pasa de 1/60 s, ver SUBSTEP_MAX)
//    * cuerpos DORMIDOS: un auto estacionado cuesta cero hasta que algo lo toca
//    * MALLAS estaticas: la caja contra cada triangulo cercano (sus esquinas
//      hundidas en el triangulo y las aristas del triangulo que la atraviesan),
//      un manifold por triangulo con su normal, al mismo solver
// ============================================================================
#include "physics/W3dRigid.h"
#include "physics/W3dColMesh.h"   // los cuerpos de tipo MALLA: sus triangulos por grilla
#include "objects/Objects.h"
#include "math/Quaternion.h"
#include "script/W3dScript.h"   // W3dScriptParamObject (para los binds)
#include "base/w3dlog.h"
#include <math.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ---------------------------------------------------------------------------
//  constantes de la simulacion
// ---------------------------------------------------------------------------
static const int   SUBSTEPS         = 2;      // sub-pasos por W3dRigidStep (minimo)
static const float SUBSTEP_MAX      = 1.0f / 60.0f;   // sub-paso mas largo (s): un frame lento hace mas
static const int   ITERATIONS       = 8;      // vueltas del solver por sub-paso
static const float BAUMGARTE        = 0.2f;   // correccion posicional (0..1)
static const float PENETRATION_OK   = 0.01f;  // hundimiento tolerado (slop)
static const float THRESHOLD_BOUNCE = 1.0f;   // vel normal minima para rebotar
static const float SLEEP_LIN2       = 0.005f; // vel lineal^2 para contar quietud
static const float SLEEP_ANG2       = 0.01f;  // vel angular^2 idem
static const int   SLEEP_FRAMES     = 45;     // pasos quieto -> a dormir
static const float WAKE_VEL2        = 0.04f;  // vecino mas rapido que esto despierta
static const int   MESH_TRIS        = 128;    // triangulos de una malla que mira UNA caja por sub-paso
static const int   MESH_MANIF       = 12;     // manifolds (triangulos con contacto) por par caja-malla

static float gGravity = 9.81f;

// ---------------------------------------------------------------------------
//  el cuerpo runtime
// ---------------------------------------------------------------------------
struct Rigid {
    Object* obj;
    int     type;            // 0 estatico | 1 dinamico | 2 personaje | 3 malla
    float   invMass;
    Vector3 half;            // semiejes de la caja (escala del objeto aplicada)
    Vector3 centerLocal;     // centro de la caja en local (escalado)
    Vector3 invInertia;      // diagonal en espacio local (0 = rotacion bloqueada)
    float   friction, bounce;

    // estado (el centro de la CAJA en mundo, no el origen del objeto)
    Vector3    pos;
    Quaternion rot;
    Vector3    vel, velAng;
    Vector3    fAccum, tAccum; // fuerzas/torques del frame (physicsForce de lua)

    // cache del sub-paso: ejes locales en mundo (columnas de R)
    Vector3 u[3];

    // lo ultimo que la fisica ESCRIBIO (o leyo) del objeto: si el script lo
    // movio por afuera (setPos3/setRotation), el proximo paso lo detecta y
    // re-sincroniza el cuerpo (teletransporte sin pelearse con lua)
    Vector3    prevObjPos;
    Quaternion prevObjRot;
    // idem con el ESPACIO DEL PADRE (lo generado por una instancia de prefab, ver SpaceOf): si la
    // instancia (o cualquier eslabon de arriba) se movio, se roto o cambio de escala, el cuerpo la sigue
    Vector3    prevParentP, prevParentS;
    Quaternion prevParentR;

    int  stillness;
    bool asleep;
    bool active;     // false = el cuerpo no colisiona ni se integra
                     // (physicsActive(obj,false): Claude adentro del auto)
    bool meshList; // tipo MALLA: su caja (pos/half) ya se leyo de los triangulos
};

// no se mueve nunca: estatico o malla
static inline bool Fixed(const Rigid& c) { return c.type == 0 || c.type == 3; }

static std::vector<Rigid> gR;
static std::vector<W3dRigidContact> gEvents;

static void UpdateAxes(Rigid& c) {
    c.u[0] = c.rot * Vector3(1, 0, 0);
    c.u[1] = c.rot * Vector3(0, 1, 0);
    c.u[2] = c.rot * Vector3(0, 0, 1);
}

// I_mundo^-1 * v  (R * diag(invI) * R^T * v, sin armar la matriz)
static Vector3 InertiaInv(const Rigid& c, const Vector3& v) {
    return c.u[0] * (c.invInertia.x * c.u[0].Dot(v)) +
           c.u[1] * (c.invInertia.y * c.u[1].Dot(v)) +
           c.u[2] * (c.invInertia.z * c.u[2].Dot(v));
}

static Rigid* Find(Object* o) {
    if (!o) return NULL;
    for (size_t i = 0; i < gR.size(); i++)
        if (gR[i].obj == o) return &gR[i];
    return NULL;
}

static void Wake(Rigid& c) {
    c.asleep    = false;
    c.stillness = 0;
}

// ---------------------------------------------------------------------------
//  ciclo de vida
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  EL ESPACIO DEL PADRE de un cuerpo. Los cuerpos son de PRIMER NIVEL (la fisica trabaja en mundo y un padre
//  cualquiera con transform los descuadraria), con UNA excepcion: lo GENERADO por una INSTANCIA DE PREFAB
//  (ObjectType::prefab, main/objects/InstancePrefab.h), a cualquier profundidad adentro de ella y con la
//  instancia colgada de donde sea. Para esos el espacio del padre es su transform de MUNDO (la cadena BASE
//  entera: posicion, rotacion y ESCALA): la fisica simula en mundo, con la caja medida con la escala de
//  mundo, y escribe en el espacio del padre. Asi el enemigo con fisica que genera instantiate("Enemigo", ...)
//  cae, choca y se empuja como uno puesto a mano, aunque su cuerpo este dos niveles adentro del prefab o la
//  instancia este escalada; y si un script mueve la INSTANCIA, el cuerpo la sigue (ver W3dRigidStep).
//  (la rotacion de mundo sale de los campos, RotGlobalOf: con una escala NO uniforme en la cadena, la caja
//   rotada es una aproximacion, como en el resto del motor)
// ---------------------------------------------------------------------------
struct SpaceParent {
    bool world;      // de primer nivel: el espacio del padre ES el mundo (sin cuentas: lo de siempre, bit a bit)
    Matrix4 m;       // la matriz de mundo BASE del padre (T*R*S de cada eslabon)
    Quaternion r;    // su rotacion de mundo
    Vector3 s;       // su escala de mundo (el producto de las de la cadena)
    SpaceParent() : world(true), r(), s(1.0f, 1.0f, 1.0f) { m.Identity(); }
};
// false = el cuerpo cuelga de algo que no es su instancia de prefab: no participa
static bool SpaceOf(const Object* o, SpaceParent& e) {
    extern Object* SceneCollection;
    e = SpaceParent();
    Object* p = o->Parent;
    if (!p || p == SceneCollection) return true;
    if (!W3dIsGenerated(o)) return false;
    e.world = false;
    p->GetWorldMatrixBase(e.m);
    e.r = RotGlobalOf(p).Normalized();
    for (Object* q = p; q; q = q->Parent) { e.s.x *= q->scale.x; e.s.y *= q->scale.y; e.s.z *= q->scale.z; }
    return true;
}
static Vector3 PosWorld(const Object* o, const SpaceParent& e) {
    return e.world ? o->pos : e.m * o->pos;
}
static Quaternion RotWorld(const Object* o, const SpaceParent& e) {
    return e.world ? o->Rot().Normalized() : (e.r * o->Rot()).Normalized();
}
// escribe en el objeto una pose de MUNDO (la pasa al espacio de su padre)
static void WritePose(Object* o, const SpaceParent& e, const Vector3& pos, const Quaternion& rot, bool withRot) {
    if (e.world) {
        o->pos = pos;
        if (withRot) o->SetRot(rot);
        return;
    }
    Matrix4 inv;
    Matrix4::InvertAffine(e.m, inv);
    o->pos = inv * pos;
    if (withRot) o->SetRot(e.r.Inverted() * rot);
}
// la foto del espacio del padre (con la del objeto: prevObjPos/prevObjRot)
static void AnnotateParent(Rigid& c, const SpaceParent& e) {
    c.prevParentP = Vector3(e.m.m[12], e.m.m[13], e.m.m[14]);
    c.prevParentR = e.r;
    c.prevParentS = e.s;
}
static bool ParentChange(const Rigid& c, const SpaceParent& e) {
    if (e.world) return false;
    if ((Vector3(e.m.m[12], e.m.m[13], e.m.m[14]) - c.prevParentP).LengthSq() > 1e-8f) return true;
    if ((e.s - c.prevParentS).LengthSq() > 1e-10f) return true;
    const float dq = fabsf(e.r.w * c.prevParentR.w + e.r.x * c.prevParentR.x + e.r.y * c.prevParentR.y + e.r.z * c.prevParentR.z);
    return dq < 0.99999f;
}
// las medidas de la caja con la escala de MUNDO del objeto (la suya por la de su padre) y su inercia
static void Size(Rigid& c, const W3dRigidDef* d, const Vector3& esc) {
    c.half = Vector3(0.5f * d->box[0] * esc.x,
                     0.5f * d->box[1] * esc.y,
                     0.5f * d->box[2] * esc.z);
    if (c.half.x < 0.01f) c.half.x = 0.01f;
    if (c.half.y < 0.01f) c.half.y = 0.01f;
    if (c.half.z < 0.01f) c.half.z = 0.01f;
    c.centerLocal = Vector3(d->center[0] * esc.x,
                            d->center[1] * esc.y,
                            d->center[2] * esc.z);
    float m = (d->mass > 0.001f) ? d->mass : 0.001f;
    if (c.type == 1) {
        // inercia de una caja solida: I = m/12 * (h^2 + p^2) por eje
        float ex = 2.0f * c.half.x, ey = 2.0f * c.half.y, ez = 2.0f * c.half.z;
        float ix = m * (ey * ey + ez * ez) / 12.0f;
        float iy = m * (ex * ex + ez * ez) / 12.0f;
        float iz = m * (ex * ex + ey * ey) / 12.0f;
        c.invInertia = Vector3(1.0f / ix, 1.0f / iy, 1.0f / iz);
    } else {
        // estatico o personaje: la rotacion no la toca la fisica
        c.invInertia = Vector3(0, 0, 0);
    }
}
static Vector3 WorldScale(const Object* o, const SpaceParent& e) {
    return e.world ? o->scale : Vector3(e.s.x * o->scale.x, e.s.y * o->scale.y, e.s.z * o->scale.z);
}

static void CreateBody(Object* o) {
    const W3dRigidDef* d = o->physics;
    if (!d || Find(o)) return;
    // los cuerpos se asumen de primer nivel (o generados por una instancia de prefab: ver SpaceOf)
    SpaceParent e;
    if (!SpaceOf(o, e)) {
        w3dLogfW("[rigido] '%s' tiene fisica pero esta emparentado: se salta",
                 o->name.c_str());
        return;
    }
    if (d->type == 3 && o->getType() != ObjectType::mesh) {
        w3dLogfW("[rigido] '%s' es de tipo malla pero no es una malla: se salta", o->name.c_str());
        return;
    }
    Rigid c;
    c.obj  = o;
    c.type = d->type;
    c.meshList = false;
    Size(c, d, WorldScale(o, e));
    c.friction = d->friction;
    c.bounce   = d->bounce;
    float m = (d->mass > 0.001f) ? d->mass : 0.001f;
    c.invMass = Fixed(c) ? 0.0f : 1.0f / m;
    c.rot = RotWorld(o, e);
    c.pos = PosWorld(o, e) + c.rot * c.centerLocal;
    c.vel = Vector3(0, 0, 0);
    c.velAng = Vector3(0, 0, 0);
    c.fAccum = Vector3(0, 0, 0);
    c.tAccum = Vector3(0, 0, 0);
    c.stillness = 0;
    c.asleep    = !Fixed(c);        // los dinamicos ARRANCAN dormidos: nada se
    c.active = true;                // cae solo hasta que algo lo toque o empuje
    c.prevObjPos = o->pos;
    c.prevObjRot = o->Rot();
    AnnotateParent(c, e);
    UpdateAxes(c);
    gR.push_back(c);
}

static void EnsureRec(Object* o) {
    if (!o) return;
    if (o->physics) CreateBody(o);
    for (size_t i = 0; i < o->Childrens.size(); i++)
        EnsureRec(o->Childrens[i]);
}

void W3dRigidEnsure(Object* root) {
    if (!root) return;
    EnsureRec(root);
}

void W3dRigidClear(void) {
    gR.clear();
    gEvents.clear();
}

void W3dRigidForget(Object* o) {
    for (size_t i = 0; i < gR.size(); i++)
        if (gR[i].obj == o) { gR.erase(gR.begin() + i); return; }
}

bool W3dRigidHas(void) { return !gR.empty(); }

const std::vector<W3dRigidContact>& W3dRigidContacts(void) { return gEvents; }

// ---------------------------------------------------------------------------
//  consultas / acciones
// ---------------------------------------------------------------------------
bool W3dRigidVel(Object* o, Vector3* out) {
    Rigid* c = Find(o);
    if (!c) return false;
    if (out) *out = c->vel;
    return true;
}
void W3dRigidSetVel(Object* o, const Vector3& v) {
    Rigid* c = Find(o);
    if (!c || Fixed(*c)) return;
    c->vel = v;
    Wake(*c);
}
bool W3dRigidVelAng(Object* o, Vector3* out) {
    Rigid* c = Find(o);
    if (!c) return false;
    if (out) *out = c->velAng;
    return true;
}
void W3dRigidSetVelAng(Object* o, const Vector3& v) {
    Rigid* c = Find(o);
    if (!c || c->type != 1) return;
    c->velAng = v;
    Wake(*c);
}
void W3dRigidImpulse(Object* o, const Vector3& imp, const Vector3& point) {
    Rigid* c = Find(o);
    if (!c || Fixed(*c)) return;
    c->vel += imp * c->invMass;
    c->velAng += InertiaInv(*c, Vector3::Cross(point - c->pos, imp));
    Wake(*c);
}
void W3dRigidForce(Object* o, const Vector3& f, const Vector3& point) {
    Rigid* c = Find(o);
    if (!c || Fixed(*c)) return;
    c->fAccum += f;
    c->tAccum += Vector3::Cross(point - c->pos, f);
    // OJO: NO resetea la quietud. Una fuerza CONTINUA y balanceada (la
    // suspension de un auto detenido) no debe impedir que el cuerpo se
    // duerma; el criterio de sueno es la VELOCIDAD. Un impulso si despierta.
    c->asleep = false;
}
void W3dRigidGravity(float g) { gGravity = g; }
float W3dRigidCurrentGravity(void) { return gGravity; }

int W3dRigidCount(void) { return (int)gR.size(); }
bool W3dRigidBox(int i, Object** o, Vector3* center, Vector3 axes[3], Vector3* semi,
                   bool* active, bool* still) {
    if (i < 0 || i >= (int)gR.size()) return false;
    const Rigid& c = gR[i];
    if (c.type == 3) return false;   // una malla no es una caja (los hitbox no la ven)
    if (o)      *o = c.obj;
    if (center) *center = c.pos;
    if (axes)   { axes[0] = c.u[0]; axes[1] = c.u[1]; axes[2] = c.u[2]; }
    if (semi)   *semi = c.half;
    if (active) *active = c.active;
    if (still) *still = (c.type == 0 || c.asleep);
    return true;
}

// prende/apaga el cuerpo sin sacarlo (apagado = no colisiona ni se integra);
// al prenderlo se re-sincroniza con el objeto y arranca despierto
void W3dRigidActive(Object* o, bool active) {
    Rigid* c = Find(o);
    if (!c) return;
    if (active && !c->active && c->type != 3) {
        SpaceParent e;
        SpaceOf(o, e);
        c->rot = RotWorld(o, e);
        c->pos = PosWorld(o, e) + c->rot * c->centerLocal;
        c->vel = Vector3(0, 0, 0);
        c->velAng = Vector3(0, 0, 0);
        c->prevObjPos = o->pos;
        c->prevObjRot = o->Rot();
        AnnotateParent(*c, e);
        UpdateAxes(*c);
        Wake(*c);
    }
    c->active = active;
}

// ---------------------------------------------------------------------------
//  rayo vs cajas (slab test en el espacio local de cada caja)
// ---------------------------------------------------------------------------
float W3dRigidRay(const Vector3& origin, const Vector3& dir, float maxDist,
                     Object* ignore, Vector3* normalOut, Object** touchedOut) {
    float best = -1.0f;
    Vector3 bestN(0, 1, 0);
    Object* bestO = NULL;
    for (size_t i = 0; i < gR.size(); i++) {
        Rigid& c = gR[i];
        if (c.obj == ignore || !c.active) continue;
        if (c.type == 3) {   // malla: el primer triangulo (la grilla de W3dColMalla)
            Vector3 n;
            const float t = W3dColRay(c.obj, origin, dir, (best >= 0.0f && best < maxDist) ? best : maxDist, &n);
            if (t >= 0.0f && (best < 0.0f || t < best)) {
                best = t;
                bestN = (n.Dot(dir) > 0.0f) ? -n : n;   // la cara que mira al rayo
                bestO = c.obj;
            }
            continue;
        }
        // al espacio de la caja
        Vector3 rel = origin - c.pos;
        float o0 = c.u[0].Dot(rel), o1 = c.u[1].Dot(rel), o2 = c.u[2].Dot(rel);
        float d0 = c.u[0].Dot(dir), d1 = c.u[1].Dot(dir), d2 = c.u[2].Dot(dir);
        float ol[3]; ol[0] = o0; ol[1] = o1; ol[2] = o2;
        float dl[3]; dl[0] = d0; dl[1] = d1; dl[2] = d2;
        float hl[3]; hl[0] = c.half.x; hl[1] = c.half.y; hl[2] = c.half.z;
        float tMin  = 0.0f, tMax = maxDist;
        int axisMin = -1; float signMin = 1.0f;
        bool outside = false;
        for (int e = 0; e < 3 && !outside; e++) {
            if (fabsf(dl[e]) < 1e-8f) {
                if (ol[e] < -hl[e] || ol[e] > hl[e]) outside = true;
            } else {
                float inv = 1.0f / dl[e];
                float t1 = (-hl[e] - ol[e]) * inv;
                float t2 = ( hl[e] - ol[e]) * inv;
                float sign = -1.0f;
                if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; sign = 1.0f; }
                if (t1 > tMin) { tMin = t1; axisMin = e; signMin = sign; }
                if (t2 < tMax) tMax = t2;
                if (tMin > tMax) outside = true;
            }
        }
        if (outside || axisMin < 0) continue;
        if (best < 0.0f || tMin < best) {
            best = tMin;
            bestN = c.u[axisMin] * signMin;
            bestO = c.obj;
        }
    }
    if (best >= 0.0f) {
        if (normalOut) *normalOut = bestN;
        if (touchedOut) *touchedOut = bestO;
    }
    return best;
}

// ---------------------------------------------------------------------------
//  deteccion OBB vs OBB: SAT + manifold por recorte de caras
// ---------------------------------------------------------------------------
struct PointContact {
    Vector3 pos;       // en mundo
    float   sunk;      // penetracion (>0)
    float   accumN;    // impulso normal acumulado (el solver lo llena)
    float   accumT1, accumT2;
    float   vnInitial;  // vel normal relativa al armar el contacto (rebote)
};
struct Manifold {
    int     ia, ib;     // indices en gR
    Vector3 normal;     // de A hacia B
    int     n;
    PointContact p[8];
};

// vertices (mundo) de la cara de 'c' cuyo indice de eje es 'eje' y signo 'sg'
static void BoxFace(const Rigid& c, int axis, float sg, Vector3 out[4]) {
    int e1 = (axis + 1) % 3, e2 = (axis + 2) % 3;
    float h[3]; h[0] = c.half.x; h[1] = c.half.y; h[2] = c.half.z;
    Vector3 center = c.pos + c.u[axis] * (sg * h[axis]);
    Vector3 a = c.u[e1] * h[e1];
    Vector3 b = c.u[e2] * h[e2];
    out[0] = center + a + b;
    out[1] = center - a + b;
    out[2] = center - a - b;
    out[3] = center + a - b;
}

// recorta un poligono contra el semiespacio dot(n,p) <= d (quedan los de adentro)
static int CropPlane(const Vector3& n, float d, const Vector3* in, int nIn,
                         Vector3* out) {
    int nOut = 0;
    for (int i = 0; i < nIn; i++) {
        const Vector3& a = in[i];
        const Vector3& b = in[(i + 1) % nIn];
        float da = n.Dot(a) - d;
        float db = n.Dot(b) - d;
        if (da <= 0.0f) out[nOut++] = a;
        if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
            float t = da / (da - db);
            out[nOut++] = a + (b - a) * t;
        }
        if (nOut >= 8) break;
    }
    return nOut;
}

// SAT completo; si hay solape arma el manifold. true = chocan.
static bool Collision(int ia, int ib, Manifold& m) {
    Rigid& A  = gR[ia];
    Rigid& B  = gR[ib];
    Vector3 d = B.pos - A.pos;

    float hA[3]; hA[0] = A.half.x; hA[1] = A.half.y; hA[2] = A.half.z;
    float hB[3]; hB[0] = B.half.x; hB[1] = B.half.y; hB[2] = B.half.z;

    // c[i][j] = dot(uA[i], uB[j]) y sus modulos (con epsilon para ejes casi
    // paralelos, el clasico del SAT de cajas)
    float c[3][3], ac[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            c[i][j] = A.u[i].Dot(B.u[j]);
            ac[i][j] = fabsf(c[i][j]) + 1e-5f;
        }

    float bestFace = -1e30f; int faceAxis = -1; int faceOf = 0;   // 0=A 1=B
    // caras de A
    for (int i = 0; i < 3; i++) {
        float da = A.u[i].Dot(d);
        float r = hA[i] + hB[0] * ac[i][0] + hB[1] * ac[i][1] + hB[2] * ac[i][2];
        float sep = fabsf(da) - r;               // <0 = solapan en este eje
        if (sep > 0.0f) return false;
        if (sep > bestFace) { bestFace = sep; faceAxis = i; faceOf = 0; }
    }
    // caras de B
    for (int j = 0; j < 3; j++) {
        float db = B.u[j].Dot(d);
        float r = hB[j] + hA[0] * ac[0][j] + hA[1] * ac[1][j] + hA[2] * ac[2][j];
        float sep = fabsf(db) - r;
        if (sep > 0.0f) return false;
        if (sep > bestFace) { bestFace = sep; faceAxis = j; faceOf = 1; }
    }
    // ejes cruzados (aristas)
    float bestEdge = -1e30f; int aAxisA = -1, aAxisB = -1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vector3 axis = Vector3::Cross(A.u[i], B.u[j]);
            float len = axis.Length();
            if (len < 1e-4f) continue;           // casi paralelos: lo cubren las caras
            axis = axis * (1.0f / len);
            float da = axis.Dot(d);
            int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
            int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            // proyeccion de cada caja sobre el eje: CADA semieje con SU dot
            // (el eje es perpendicular a u[i]/u[j], por eso esos dos no aportan)
            float r = hA[i1] * fabsf(axis.Dot(A.u[i1])) + hA[i2] * fabsf(axis.Dot(A.u[i2])) +
                      hB[j1] * fabsf(axis.Dot(B.u[j1])) + hB[j2] * fabsf(axis.Dot(B.u[j2]));
            float sep = fabsf(da) - r;
            if (sep > 0.0f) return false;
            if (sep > bestEdge) { bestEdge = sep; aAxisA = i; aAxisB = j; }
        }

    m.ia = ia; m.ib = ib; m.n = 0;

    // preferir el contacto de CARA salvo que la arista sea claramente mejor
    // (el sesgo evita que el manifold salte entre cara y arista y vibre)
    bool useEdge = (aAxisA >= 0) && (bestEdge > bestFace * 0.95f + 0.005f);

    if (!useEdge) {
        // ---- cara de referencia + recorte de la cara incidente -------------
        const Rigid& R = faceOf == 0 ? A : B;    // caja de referencia
        const Rigid& I = faceOf == 0 ? B : A;    // caja incidente
        Vector3 dRI = I.pos - R.pos;
        float sg = (R.u[faceAxis].Dot(dRI) >= 0.0f) ? 1.0f : -1.0f;
        Vector3 nRef = R.u[faceAxis] * sg;         // normal de ref hacia I
        // cara incidente: la de I mas enfrentada a nRef
        int axisInc = 0; float worst = 0.0f;
        for (int e = 0; e < 3; e++) {
            float dp = I.u[e].Dot(nRef);
            if (e == 0 || fabsf(dp) > fabsf(worst)) { worst = dp; axisInc = e; }
        }
        float sgInc = (worst > 0.0f) ? -1.0f : 1.0f;
        Vector3 face[8], tmp[8];
        BoxFace(I, axisInc, sgInc, face);
        int nPts = 4;
        // recortar contra los 4 planos laterales de la cara de referencia
        float hR[3]; hR[0] = R.half.x; hR[1] = R.half.y; hR[2] = R.half.z;
        int e1 = (faceAxis + 1) % 3, e2 = (faceAxis + 2) % 3;
        nPts = CropPlane(R.u[e1],  R.u[e1].Dot(R.pos) + hR[e1], face, nPts, tmp);
        nPts = CropPlane(-R.u[e1], -(R.u[e1].Dot(R.pos) - hR[e1]), tmp, nPts, face);
        nPts = CropPlane(R.u[e2],  R.u[e2].Dot(R.pos) + hR[e2], face, nPts, tmp);
        nPts = CropPlane(-R.u[e2], -(R.u[e2].Dot(R.pos) - hR[e2]), tmp, nPts, face);
        // quedarse con los que penetran la cara de referencia
        float dFace = nRef.Dot(R.pos + nRef * hR[faceAxis]);
        for (int i = 0; i < nPts && m.n < 8; i++) {
            float hund = dFace - nRef.Dot(face[i]);
            if (hund > 0.0f) {
                PointContact& p = m.p[m.n++];
                p.pos = face[i];
                p.sunk = hund;
                p.accumN = p.accumT1 = p.accumT2 = 0.0f;
                p.vnInitial = 0.0f;
            }
        }
        // normal SIEMPRE de A hacia B (el solver la asume asi)
        m.normal = (faceOf == 0) ? nRef : -nRef;
        // si el recorte quedo con mas de 4, reducir a los 4 mas hundidos
        while (m.n > 4) {
            int lower = 0;
            for (int i = 1; i < m.n; i++)
                if (m.p[i].sunk < m.p[lower].sunk) lower = i;
            m.p[lower] = m.p[m.n - 1];
            m.n--;
        }
    } else {
        // ---- arista vs arista: punto mas cercano entre los dos segmentos ---
        Vector3 axis = Vector3::Cross(A.u[aAxisA], B.u[aAxisB]).Normalized();
        if (axis.Dot(d) < 0.0f) axis = -axis;        // de A hacia B
        // arranco de los centros y me muevo a la arista de cada caja
        Vector3 pA = A.pos, pB = B.pos;
        for (int e = 0; e < 3; e++) {
            if (e != aAxisA) pA += A.u[e] * ((axis.Dot(A.u[e]) >= 0.0f ? 1.0f : -1.0f) * hA[e]);
            if (e != aAxisB) pB -= B.u[e] * ((axis.Dot(B.u[e]) >= 0.0f ? 1.0f : -1.0f) * hB[e]);
        }
        // punto mas cercano entre las rectas pA+s*dirA y pB+t*dirB
        Vector3 dirA = A.u[aAxisA], dirB = B.u[aAxisB];
        Vector3 r = pB - pA;
        float a2 = dirA.Dot(dirA), b2 = dirB.Dot(dirB), ab = dirA.Dot(dirB);
        float ra = dirA.Dot(r), rb = dirB.Dot(r);
        float den = a2 * b2 - ab * ab;
        float s = (fabsf(den) > 1e-8f) ? (ra * b2 - rb * ab) / den : 0.0f;
        if (s >  hA[aAxisA]) s =  hA[aAxisA];
        if (s < -hA[aAxisA]) s = -hA[aAxisA];
        Vector3 cA = pA + dirA * s;
        float t = dirB.Dot(cA - pB);
        if (t >  hB[aAxisB]) t =  hB[aAxisB];
        if (t < -hB[aAxisB]) t = -hB[aAxisB];
        Vector3 cB = pB + dirB * t;
        PointContact& p = m.p[0];
        p.pos = (cA + cB) * 0.5f;
        p.sunk = -bestEdge;
        p.accumN = p.accumT1 = p.accumT2 = 0.0f;
        p.vnInitial = 0.0f;
        m.n = 1;
        m.normal = axis;
    }
    return m.n > 0;
}

// ---------------------------------------------------------------------------
//  deteccion OBB vs MALLA estatica: por triangulo cercano (grilla de W3dColMalla)
//    * las esquinas de la caja hundidas en el triangulo (dentro de su prisma y
//      a menos de 'hondo' del plano): el caso de apoyarse en un piso o una
//      rampa, o chocar de lleno contra una pared. Normal = la del triangulo.
//    * si ninguna esquina cae en el triangulo, sus ARISTAS que atraviesan la
//      caja (la esquina de un edificio contra el costado de un auto): el
//      punto medio del tramo de adentro, por la cara de la caja mas cercana.
//  Un manifold por triangulo (cada uno con su normal); A = la malla, B = la caja.
// ---------------------------------------------------------------------------
static W3dColTri gTris[MESH_TRIS];

static void NewPoint(Manifold& m, const Vector3& pos, float sunk) {
    PointContact* p;
    if (m.n < 4) p = &m.p[m.n++];
    else {   // lleno: reemplaza al menos hundido si este lo esta mas
        int lower = 0;
        for (int i = 1; i < m.n; i++) if (m.p[i].sunk < m.p[lower].sunk) lower = i;
        if (m.p[lower].sunk >= sunk) return;
        p = &m.p[lower];
    }
    p->pos = pos;
    p->sunk = sunk;
    p->accumN = p->accumT1 = p->accumT2 = 0.0f;
    p->vnInitial = 0.0f;
}

// el tramo del segmento a-b que queda adentro de la caja (slabs en sus ejes); su punto medio
static bool SegmentInBox(const Rigid& B, const Vector3& a, const Vector3& b, Vector3* middle) {
    float h[3]; h[0] = B.half.x; h[1] = B.half.y; h[2] = B.half.z;
    const Vector3 ra = a - B.pos, d = b - a;
    float t0 = 0.0f, t1 = 1.0f;
    for (int e = 0; e < 3; e++) {
        const float o = B.u[e].Dot(ra), v = B.u[e].Dot(d);
        if (fabsf(v) < 1e-9f) {
            if (o < -h[e] || o > h[e]) return false;
            continue;
        }
        float ta = (-h[e] - o) / v, tb = (h[e] - o) / v;
        if (ta > tb) { float x = ta; ta = tb; tb = x; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return false;
    }
    *middle = a + d * (0.5f * (t0 + t1));
    return true;
}

// todos los puntos del manifold por debajo de la altura 'y' (un contacto que solo toca el pie)
static bool IsUnder(const Manifold& m, float y) {
    for (int i = 0; i < m.n; i++) if (m.p[i].pos.y > y) return false;
    return true;
}

static void CollisionMesh(int ia, int ib, std::vector<Manifold>& ms) {
    const Rigid& B = gR[ib];
    const Vector3 ext(fabsf(B.u[0].x) * B.half.x + fabsf(B.u[1].x) * B.half.y + fabsf(B.u[2].x) * B.half.z,
                      fabsf(B.u[0].y) * B.half.x + fabsf(B.u[1].y) * B.half.y + fabsf(B.u[2].y) * B.half.z,
                      fabsf(B.u[0].z) * B.half.x + fabsf(B.u[1].z) * B.half.y + fabsf(B.u[2].z) * B.half.z);
    const int nt = W3dColTriangles(gR[ia].obj, B.pos - ext, B.pos + ext, gTris, MESH_TRIS);
    if (nt <= 0) return;
    float h[3]; h[0] = B.half.x; h[1] = B.half.y; h[2] = B.half.z;
    Vector3 esq[8];
    for (int k = 0; k < 8; k++)
        esq[k] = B.pos + B.u[0] * ((k & 1) ? h[0] : -h[0]) + B.u[1] * ((k & 2) ? h[1] : -h[1]) +
                 B.u[2] * ((k & 4) ? h[2] : -h[2]);
    // una esquina mas hundida que esto esta del OTRO lado (una pared fina, el piso de abajo): no es contacto
    float deep = h[0]; if (h[1] < deep) deep = h[1]; if (h[2] < deep) deep = h[2];
    // EL PERSONAJE SUBE ESCALONES: una pared que lo toca solo por debajo de esta altura (un cordon, un escalon) no
    // lo frena; avanza, sus esquinas de abajo quedan adentro del piso de arriba y ese contacto lo levanta. Tiene
    // que ser menor que 'hondo' (mas hundido el piso de arriba ya no cuenta como contacto)
    const float foot = B.pos.y - ext.y;
    const float step = (B.type == 2) ? (deep * 0.9f < 0.32f ? deep * 0.9f : 0.32f) : 0.0f;
    int done = 0;
    for (int t = 0; t < nt && done < MESH_MANIF; t++) {
        const W3dColTri& T = gTris[t];
        // la normal hacia el lado de la caja (las mallas de colision no siempre tienen el orden de vertices parejo)
        const Vector3 n = (T.n.Dot(B.pos - T.a) >= 0.0f) ? T.n : -T.n;
        const float dPlane = n.Dot(T.a);
        Manifold m;
        m.ia = ia; m.ib = ib; m.n = 0; m.normal = n;
        const Vector3 ab = T.b - T.a, bc = T.c - T.b, ca = T.a - T.c;
        for (int k = 0; k < 8; k++) {
            const float hund = dPlane - n.Dot(esq[k]);
            if (hund <= 0.0f || hund > deep) continue;
            // adentro del prisma del triangulo (con la normal de su orden de vertices, este como este girada)
            if (Vector3::Cross(ab, esq[k] - T.a).Dot(T.n) < 0.0f) continue;
            if (Vector3::Cross(bc, esq[k] - T.b).Dot(T.n) < 0.0f) continue;
            if (Vector3::Cross(ca, esq[k] - T.c).Dot(T.n) < 0.0f) continue;
            NewPoint(m, esq[k], hund);
        }
        if (m.n > 0) {
            if (!(step > 0.0f && fabsf(n.y) < 0.5f && IsUnder(m, foot + step))) { ms.push_back(m); done++; }
            continue;
        }
        // ninguna esquina: las aristas del triangulo que atraviesan la caja
        for (int e = 0; e < 3 && done < MESH_MANIF; e++) {
            const Vector3& p0 = e == 0 ? T.a : (e == 1 ? T.b : T.c);
            const Vector3& p1 = e == 0 ? T.b : (e == 1 ? T.c : T.a);
            Vector3 q;
            if (!SegmentInBox(B, p0, p1, &q)) continue;
            // sale por la cara de la caja mas cercana al punto: la caja se aleja de el
            const Vector3 r = q - B.pos;
            int axis = 0; float lower = 1e30f, sg = 1.0f;
            for (int k = 0; k < 3; k++) {
                const float l = B.u[k].Dot(r);
                const float missing = h[k] - fabsf(l);
                if (missing < lower) { lower = missing; axis = k; sg = (l >= 0.0f) ? 1.0f : -1.0f; }
            }
            if (lower <= 0.0f) continue;
            Manifold me;
            me.ia = ia; me.ib = ib; me.n = 0; me.normal = B.u[axis] * (-sg);
            NewPoint(me, q, lower);
            if (step > 0.0f && fabsf(me.normal.y) < 0.5f && IsUnder(me, foot + step)) continue;
            ms.push_back(me);
            done++;
        }
    }
}

// la caja de un cuerpo de tipo malla (la de sus triangulos, en mundo). La primera vez que hace falta:
// la malla se lee con su pose de ese momento (una instancia de prefab ya puesta en su lugar)
static bool MeshList(Rigid& c) {
    if (c.meshList) return true;
    Vector3 mn, mx;
    if (!W3dColBox(c.obj, &mn, &mx)) return false;
    c.pos = (mn + mx) * 0.5f;
    c.half = (mx - mn) * 0.5f;
    c.rot = Quaternion();
    UpdateAxes(c);
    c.meshList = true;
    return true;
}

// ---------------------------------------------------------------------------
//  solver de impulsos secuenciales
// ---------------------------------------------------------------------------
static Vector3 VelInPoint(const Rigid& c, const Vector3& p) {
    return c.vel + Vector3::Cross(c.velAng, p - c.pos);
}

static void Resolve(std::vector<Manifold>& ms, float h) {
    // velocidad normal inicial (para el rebote) y tangentes por manifold
    for (size_t k = 0; k < ms.size(); k++) {
        Manifold& m = ms[k];
        Rigid& A = gR[m.ia];
        Rigid& B = gR[m.ib];
        for (int i = 0; i < m.n; i++) {
            Vector3 vr = VelInPoint(B, m.p[i].pos) - VelInPoint(A, m.p[i].pos);
            m.p[i].vnInitial = vr.Dot(m.normal);
        }
    }
    for (int iter = 0; iter < ITERATIONS; iter++) {
        for (size_t k = 0; k < ms.size(); k++) {
            Manifold& m = ms[k];
            Rigid& A = gR[m.ia];
            Rigid& B = gR[m.ib];
            const Vector3& n = m.normal;
            // tangentes ortogonales a la normal
            Vector3 t1 = Vector3::Cross(n, Vector3(0, 1, 0));
            if (t1.LengthSq() < 1e-6f) t1 = Vector3::Cross(n, Vector3(1, 0, 0));
            t1 = t1.Normalized();
            Vector3 t2 = Vector3::Cross(n, t1);
            float mu = sqrtf(A.friction * B.friction);
            float e = A.bounce > B.bounce ? A.bounce : B.bounce;
            for (int i = 0; i < m.n; i++) {
                PointContact& p = m.p[i];
                Vector3 ra = p.pos - A.pos;
                Vector3 rb = p.pos - B.pos;
                // ---- impulso normal (con Baumgarte y rebote) ---------------
                Vector3 vr = VelInPoint(B, p.pos) - VelInPoint(A, p.pos);
                float vn = vr.Dot(n);
                float kn = A.invMass + B.invMass +
                    n.Dot(Vector3::Cross(InertiaInv(A, Vector3::Cross(ra, n)), ra)) +
                    n.Dot(Vector3::Cross(InertiaInv(B, Vector3::Cross(rb, n)), rb));
                if (kn < 1e-8f) continue;
                // el sesgo es una VELOCIDAD (hundido / paso) que queda puesta despues de sacar la
                // caja del piso: con sub-pasos cortisimos (el juego compilado sin vsync corre a
                // 1 ms por frame) 2 cm de hundido eran 4 m/s hacia arriba y la caja salia
                // CATAPULTADA (un segundo alTocar). Se mide contra un paso de 1/60 s como minimo:
                // a 30 fps (el Play) es el mismo paso de siempre y da exactamente lo mismo.
                const float hBias = (h < SUBSTEP_MAX * 0.999f) ? SUBSTEP_MAX : h;
                float bias = (BAUMGARTE / hBias) * (p.sunk - PENETRATION_OK);
                if (bias < 0.0f) bias = 0.0f;
                if (bias > 4.0f) bias = 4.0f;   // tope: una penetracion honda
                                                  // empuja, no catapulta
                float bounce = 0.0f;
                if (p.vnInitial < -THRESHOLD_BOUNCE) bounce = -e * p.vnInitial;
                float dj = (-vn + bias + bounce) / kn;
                float accum0 = p.accumN;
                p.accumN = accum0 + dj;
                if (p.accumN < 0.0f) p.accumN = 0.0f;
                dj = p.accumN - accum0;
                Vector3 imp = n * dj;
                A.vel -= imp * A.invMass;
                A.velAng -= InertiaInv(A, Vector3::Cross(ra, imp));
                B.vel += imp * B.invMass;
                B.velAng += InertiaInv(B, Vector3::Cross(rb, imp));
                // ---- friccion en dos tangentes -----------------------------
                float limitT = mu * p.accumN;
                for (int tt = 0; tt < 2; tt++) {
                    const Vector3& t = tt == 0 ? t1 : t2;
                    float* accumT = tt == 0 ? &p.accumT1 : &p.accumT2;
                    vr = VelInPoint(B, p.pos) - VelInPoint(A, p.pos);
                    float vt = vr.Dot(t);
                    float kt = A.invMass + B.invMass +
                        t.Dot(Vector3::Cross(InertiaInv(A, Vector3::Cross(ra, t)), ra)) +
                        t.Dot(Vector3::Cross(InertiaInv(B, Vector3::Cross(rb, t)), rb));
                    if (kt < 1e-8f) continue;
                    float djt = -vt / kt;
                    float accumT0 = *accumT;
                    *accumT = accumT0 + djt;
                    if (*accumT >  limitT) *accumT =  limitT;
                    if (*accumT < -limitT) *accumT = -limitT;
                    djt = *accumT - accumT0;
                    Vector3 impT = t * djt;
                    A.vel -= impT * A.invMass;
                    A.velAng -= InertiaInv(A, Vector3::Cross(ra, impT));
                    B.vel += impT * B.invMass;
                    B.velAng += InertiaInv(B, Vector3::Cross(rb, impT));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  el paso
// ---------------------------------------------------------------------------
static void RegisterEvent(const Manifold& m) {
    float total = 0.0f;
    int best = 0;
    for (int i = 0; i < m.n; i++) {
        total += m.p[i].accumN;
        if (m.p[i].sunk > m.p[best].sunk) best = i;
    }
    if (total <= 1e-4f) return;
    Object* a = gR[m.ia].obj;
    Object* b = gR[m.ib].obj;
    // acumular sobre un evento existente del MISMO par (los dos sub-pasos del
    // frame suman: el script ve UN golpe con el impulso total)
    for (size_t i = 0; i < gEvents.size(); i++) {
        if ((gEvents[i].a == a && gEvents[i].b == b) ||
            (gEvents[i].a == b && gEvents[i].b == a)) {
            gEvents[i].impulse += total;
            return;
        }
    }
    W3dRigidContact ev;
    ev.a = a; ev.b = b;
    ev.point[0] = m.p[best].pos.x;
    ev.point[1] = m.p[best].pos.y;
    ev.point[2] = m.p[best].pos.z;
    ev.normal[0] = m.normal.x;
    ev.normal[1] = m.normal.y;
    ev.normal[2] = m.normal.z;
    ev.impulse = total;
    gEvents.push_back(ev);
}

void W3dRigidStep(float dt) {
    if (gR.empty()) return;
    if (dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;
    gEvents.clear();
    // ---- re-sincronizar lo que el script movio por afuera -------------------
    for (size_t i = 0; i < gR.size(); i++) {
        Rigid& c = gR[i];
        if (c.type == 3) continue;   // la malla se lee una vez (quieta): su caja es la de los triangulos
        Object* o = c.obj;
        SpaceParent e;
        SpaceOf(o, e);
        // (lo generado por una instancia: tambien si la movieron a ELLA -o a algo de mas arriba-; si le cambiaron
        //  la escala, la caja se vuelve a medir)
        const bool parent = ParentChange(c, e);
        if (parent && o->physics && (e.s - c.prevParentS).LengthSq() > 1e-10f) Size(c, o->physics, WorldScale(o, e));
        if (parent || (o->pos - c.prevObjPos).LengthSq() > 1e-8f) {
            c.rot = RotWorld(o, e);
            c.pos = PosWorld(o, e) + c.rot * c.centerLocal;
            UpdateAxes(c);
        } else {
            float dq = fabsf(o->Rot().w * c.prevObjRot.w + o->Rot().x * c.prevObjRot.x +
                             o->Rot().y * c.prevObjRot.y + o->Rot().z * c.prevObjRot.z);
            if (dq < 0.99999f) {
                c.rot = RotWorld(o, e);
                c.pos = PosWorld(o, e) + c.rot * c.centerLocal;
                UpdateAxes(c);
            }
        }
        if (parent) AnnotateParent(c, e);
    }
    // un frame LENTO (un tiron del juego compilado, que corre con el dt real: 50+ ms) en dos
    // sub-pasos eran pasos mucho mas largos que los del Play: una caja que cae a 8 m/s se hundia
    // 23 cm en el piso en un solo sub-paso. Ningun sub-paso pasa de 1/60 s: a 30 fps o mas (el
    // Play) son los dos de siempre y da exactamente lo mismo que antes.
    int substeps = SUBSTEPS;
    while (dt / (float)substeps > SUBSTEP_MAX * 1.001f) substeps++;
    float h = dt / (float)substeps;
    std::vector<Manifold> ms;
    for (int step = 0; step < substeps; step++) {
        // ---- integrar velocidades (gravedad + fuerzas del frame) -----------
        for (size_t i = 0; i < gR.size(); i++) {
            Rigid& c = gR[i];
            if (Fixed(c) || c.asleep || !c.active) continue;
            c.vel.y -= gGravity * h;
            c.vel += c.fAccum * (c.invMass * h);
            c.velAng += InertiaInv(c, c.tAccum) * h;
        }
        // ---- detectar contactos --------------------------------------------
        ms.clear();
        for (size_t i = 0; i < gR.size(); i++) {
            for (size_t j = i + 1; j < gR.size(); j++) {
                Rigid& A = gR[i];
                Rigid& B = gR[j];
                if (!A.active || !B.active) continue;
                if (Fixed(A) && Fixed(B)) continue;
                bool aStill = A.asleep || Fixed(A);
                bool bStill = B.asleep || Fixed(B);
                if (aStill && bStill) continue;
                if (A.type == 3 || B.type == 3) {
                    // caja contra malla (la malla siempre como A): poda por la caja de la malla
                    const int im = (A.type == 3) ? (int)i : (int)j, ic = (A.type == 3) ? (int)j : (int)i;
                    Rigid& M = gR[im];
                    const Rigid& C = gR[ic];
                    if (!MeshList(M)) continue;
                    const float rc = C.half.Length();
                    const Vector3 dd = C.pos - M.pos;
                    if (fabsf(dd.x) > M.half.x + rc || fabsf(dd.y) > M.half.y + rc || fabsf(dd.z) > M.half.z + rc) continue;
                    CollisionMesh(im, ic, ms);
                    continue;
                }
                // poda por esfera envolvente (barato) antes del SAT
                float ra = A.half.Length(), rb = B.half.Length();
                if ((B.pos - A.pos).LengthSq() > (ra + rb) * (ra + rb)) continue;
                Manifold m;
                if (!Collision((int)i, (int)j, m)) continue;
                // el toque despierta al dormido si el otro se mueve de verdad
                if (A.asleep && (B.vel.LengthSq() > WAKE_VEL2 || B.velAng.LengthSq() > WAKE_VEL2)) Wake(A);
                if (B.asleep && (A.vel.LengthSq() > WAKE_VEL2 || A.velAng.LengthSq() > WAKE_VEL2)) Wake(B);
                ms.push_back(m);
            }
        }
        // ---- resolver -------------------------------------------------------
        if (!ms.empty()) Resolve(ms, h);
        // ---- integrar posiciones -------------------------------------------
        for (size_t i = 0; i < gR.size(); i++) {
            Rigid& c = gR[i];
            if (Fixed(c) || c.asleep || !c.active) continue;
            c.pos += c.vel * h;
            if (c.velAng.LengthSq() > 1e-10f) {
                // q' = q + h/2 * w*q (integracion estandar del cuaternion)
                Quaternion w(0.0f, c.velAng.x, c.velAng.y, c.velAng.z);
                Quaternion dq = w * c.rot;
                c.rot.w += 0.5f * h * dq.w;
                c.rot.x += 0.5f * h * dq.x;
                c.rot.y += 0.5f * h * dq.y;
                c.rot.z += 0.5f * h * dq.z;
                c.rot.normalize();
            }
            UpdateAxes(c);
        }
        // ---- eventos del sub-paso -------------------------------------------
        for (size_t k = 0; k < ms.size(); k++) RegisterEvent(ms[k]);
    }
    // ---- fin de frame: consumir fuerzas, dormir quietos, escribir objetos ---
    for (size_t i = 0; i < gR.size(); i++) {
        Rigid& c = gR[i];
        c.fAccum = Vector3(0, 0, 0);
        c.tAccum = Vector3(0, 0, 0);
        if (Fixed(c) || c.asleep || !c.active) {
            c.prevObjPos = c.obj->pos;
            c.prevObjRot = c.obj->Rot();
            continue;
        }
        if (c.vel.LengthSq() < SLEEP_LIN2 && c.velAng.LengthSq() < SLEEP_ANG2) {
            if (++c.stillness >= SLEEP_FRAMES) {
                c.asleep = true;
                c.vel = Vector3(0, 0, 0);
                c.velAng = Vector3(0, 0, 0);
            }
        } else {
            c.stillness = 0;
        }
        Object* o = c.obj;
        // (el PERSONAJE -tipo 2- no rota por fisica: su facing lo maneja el script)
        // (en el espacio de su padre TAL COMO ESTA AHORA: un padre que tambien es un cuerpo ya se escribio, los
        //  cuerpos van en preorden; el dormido de arriba NO anota el padre, asi si el suyo se movio lo sigue)
        SpaceParent e;
        SpaceOf(o, e);
        WritePose(o, e, c.pos - (c.rot * c.centerLocal), c.rot, c.type != 2);
        c.prevObjPos = o->pos;
        c.prevObjRot = o->Rot();
        AnnotateParent(c, e);
    }
}

// ---------------------------------------------------------------------------
//  binds lua
// ---------------------------------------------------------------------------
static Object* ObjArg(lua_State* L, int idx) {
    return W3dScriptParamObject((void*)L, idx);
}

// (los vectores 3D de estos binds van en los ejes de LUA, Z arriba: un (x,y,z) de lua es el (x,z,y) del motor y se
//  convierte en la frontera con W3dLuaAxes, al leer y al devolver; ver W3dScript.h)
// un vector 3D de lua (3 numeros desde 'idx') -> en los ejes del motor
static Vector3 LuaVec3(lua_State* L, int idx) {
    Vector3 v((float)luaL_checknumber(L, idx),
              (float)luaL_checknumber(L, idx + 1),
              (float)luaL_checknumber(L, idx + 2));
    W3dLuaAxes(v.y, v.z);
    return v;
}
// un vector 3D del motor -> 3 numeros a lua, en sus ejes
static void PushVec3(lua_State* L, Vector3 v) {
    W3dLuaAxes(v.y, v.z);
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z);
}

// physicsVel(obj) -> vx,vy,vz | physicsVel(obj, vx,vy,vz)   (ejes de lua: vz es la vertical)
static int LPhysicsVel(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    if (lua_gettop(L) >= 4) {
        W3dRigidSetVel(o, LuaVec3(L, 2));
        return 0;
    }
    Vector3 v;
    if (!W3dRigidVel(o, &v)) return 0;
    PushVec3(L, v);
    return 3;
}

// physicsAngVel(obj) -> wx,wy,wz | physicsAngVel(obj, wx,wy,wz): rad/s alrededor de cada eje de lua (wz = alrededor de
// la vertical). El sentido de giro de cada componente es el mismo que el del motor (como los grados del panel)
static int LPhysicsVelAng(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    if (lua_gettop(L) >= 4) {
        W3dRigidSetVelAng(o, LuaVec3(L, 2));
        return 0;
    }
    Vector3 v;
    if (!W3dRigidVelAng(o, &v)) return 0;
    PushVec3(L, v);
    return 3;
}

// physicsImpulse(obj, ix,iy,iz [,px,py,pz])   (el impulso y el punto en ejes de lua; sin punto = el centro del cuerpo)
static int LPhysicsImpulse(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    Vector3 imp = LuaVec3(L, 2);
    Vector3 p = o->pos;
    Rigid* c  = Find(o);
    if (c) p = c->pos;
    if (lua_gettop(L) >= 7) p = LuaVec3(L, 5);
    W3dRigidImpulse(o, imp, p);
    return 0;
}

// physicsForce(obj, fx,fy,fz [,px,py,pz]) : fuerza de ESTE frame   (la fuerza y el punto en ejes de lua)
static int LPhysicsForce(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    Vector3 f = LuaVec3(L, 2);
    Vector3 p = o->pos;
    Rigid* c  = Find(o);
    if (c) p = c->pos;
    if (lua_gettop(L) >= 7) p = LuaVec3(L, 5);
    W3dRigidForce(o, f, p);
    return 0;
}

// physicsRay(ox,oy,oz, dx,dy,dz, max [,ignorar]) -> dist,nx,ny,nz,obj | nil
// (origen, direccion y normal en ejes de lua)
static int LPhysicsRay(lua_State* L) {
    Vector3 o = LuaVec3(L, 1);
    Vector3 d = LuaVec3(L, 4);
    float mx = (float)luaL_checknumber(L, 7);
    Object* ig = (lua_gettop(L) >= 8) ? ObjArg(L, 8) : NULL;
    d = d.Normalized();
    Vector3 n;
    Object* touched = NULL;
    float t = W3dRigidRay(o, d, mx, ig, &n, &touched);
    if (t < 0.0f) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, t);
    PushVec3(L, n);
    if (touched) lua_pushlightuserdata(L, touched); else lua_pushnil(L);
    return 5;
}

// contacts() -> lista de golpes del ultimo paso: {a, b, x,y,z (punto), nx,ny,nz (normal), impulso}, en ejes de lua
static int LContacts(lua_State* L) {
    const std::vector<W3dRigidContact>& evs = W3dRigidContacts();
    lua_createtable(L, (int)evs.size(), 0);
    for (size_t i = 0; i < evs.size(); i++) {
        const W3dRigidContact& e = evs[i];
        float px = e.point[0], py = e.point[1], pz = e.point[2];
        float nx = e.normal[0], ny = e.normal[1], nz = e.normal[2];
        W3dLuaAxes(py, pz);
        W3dLuaAxes(ny, nz);
        lua_createtable(L, 0, 9);
        lua_pushlightuserdata(L, e.a);   lua_setfield(L, -2, "a");
        lua_pushlightuserdata(L, e.b);   lua_setfield(L, -2, "b");
        lua_pushnumber(L, px);           lua_setfield(L, -2, "x");
        lua_pushnumber(L, py);           lua_setfield(L, -2, "y");
        lua_pushnumber(L, pz);           lua_setfield(L, -2, "z");
        lua_pushnumber(L, nx);           lua_setfield(L, -2, "nx");
        lua_pushnumber(L, ny);           lua_setfield(L, -2, "ny");
        lua_pushnumber(L, nz);           lua_setfield(L, -2, "nz");
        lua_pushnumber(L, e.impulse);    lua_setfield(L, -2, "impulse");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// physicsAwake(obj) -> true si el cuerpo esta despierto (simulando). El
// juego lo usa para NO gastar en un auto dormido (suspension, ruedas).
static int LAwakePhysics(lua_State* L) {
    Object* o = ObjArg(L, 1);
    Rigid* c  = o ? Find(o) : NULL;
    lua_pushboolean(L, (c && c->active && !c->asleep && !Fixed(*c)) ? 1 : 0);
    return 1;
}

// physicsActive(obj, true/false): prende o apaga el cuerpo (Claude adentro
// del auto se apaga: sino su caja pelearia con la del auto todo el tiempo)
static int LActivePhysics(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    W3dRigidActive(o, lua_toboolean(L, 2) != 0);
    return 0;
}

// physicsGravity([g]): la gravedad es un ESCALAR (m/s^2, positivo = cae), siempre sobre la vertical (la Z de lua,
// la Y del motor): no hay vector que convertir
static int LPhysicsGravity(lua_State* L) {
    if (lua_gettop(L) >= 1) {
        W3dRigidGravity((float)luaL_checknumber(L, 1));
        return 0;
    }
    lua_pushnumber(L, W3dRigidCurrentGravity());
    return 1;
}

void W3dRigidRegisterBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LPhysicsVel);      lua_setglobal(L, "physicsVel");
    lua_pushcfunction(L, LPhysicsVelAng);   lua_setglobal(L, "physicsAngVel");
    lua_pushcfunction(L, LPhysicsImpulse);  lua_setglobal(L, "physicsImpulse");
    lua_pushcfunction(L, LPhysicsForce);   lua_setglobal(L, "physicsForce");
    lua_pushcfunction(L, LPhysicsRay);     lua_setglobal(L, "physicsRay");
    lua_pushcfunction(L, LContacts);      lua_setglobal(L, "contacts");
    lua_pushcfunction(L, LPhysicsGravity); lua_setglobal(L, "physicsGravity");
    lua_pushcfunction(L, LActivePhysics);   lua_setglobal(L, "physicsActive");
    lua_pushcfunction(L, LAwakePhysics);lua_setglobal(L, "physicsAwake");
}
