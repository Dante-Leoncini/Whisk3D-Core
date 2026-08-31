// ============================================================================
//  W3dRigido.cpp — ver W3dRigido.h. Dialecto C++03 (Symbian compila esto).
//  Comentarios en espanol sin acentos.
//
//  La tecnica es la clasica de los motores de impulsos (de libro):
//    * deteccion OBB vs OBB por SAT (15 ejes) + manifold por RECORTE de la
//      cara incidente contra la cara de referencia (Sutherland-Hodgman)
//    * resolucion por IMPULSOS SECUENCIALES iterados, con impulso normal
//      acumulado, friccion de Coulomb en dos tangentes y un sesgo posicional
//      (Baumgarte) para que nada quede hundido
//    * integracion semi-implicita (primero velocidad, despues posicion), en
//      DOS sub-pasos por frame para que 30 fps sea estable
//    * cuerpos DORMIDOS: un auto estacionado cuesta cero hasta que algo lo toca
// ============================================================================
#include "physics/W3dRigido.h"
#include "objects/Objects.h"
#include "math/Quaternion.h"
#include "script/W3dScript.h"   // W3dScriptParamObjeto (para los binds)
#include "base/w3dlog.h"
#include <math.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ---------------------------------------------------------------------------
//  constantes de la simulacion
// ---------------------------------------------------------------------------
static const int   SUBPASOS       = 2;      // sub-pasos por W3dRigidosPaso
static const int   ITERACIONES    = 8;      // vueltas del solver por sub-paso
static const float BAUMGARTE      = 0.2f;   // correccion posicional (0..1)
static const float PENETRACION_OK = 0.01f;  // hundimiento tolerado (slop)
static const float UMBRAL_REBOTE  = 1.0f;   // vel normal minima para rebotar
static const float DORMIR_LIN2    = 0.005f; // vel lineal^2 para contar quietud
static const float DORMIR_ANG2    = 0.01f;  // vel angular^2 idem
static const int   DORMIR_FRAMES  = 45;     // pasos quieto -> a dormir
static const float DESPERTAR_VEL2 = 0.04f;  // vecino mas rapido que esto despierta

static float gGravedad = 9.81f;

// ---------------------------------------------------------------------------
//  el cuerpo runtime
// ---------------------------------------------------------------------------
struct Rigido {
    Object* obj;
    int     tipo;            // 0 estatico | 1 dinamico | 2 personaje
    float   invMasa;
    Vector3 half;            // semiejes de la caja (escala del objeto aplicada)
    Vector3 centroLocal;     // centro de la caja en local (escalado)
    Vector3 invInercia;      // diagonal en espacio local (0 = rotacion bloqueada)
    float   friccion, rebote;

    // estado (el centro de la CAJA en mundo, no el origen del objeto)
    Vector3    pos;
    Quaternion rot;
    Vector3    vel, velAng;
    Vector3    fAcum, tAcum; // fuerzas/torques del frame (fisicaFuerza de lua)

    // cache del sub-paso: ejes locales en mundo (columnas de R)
    Vector3 u[3];

    // lo ultimo que la fisica ESCRIBIO (o leyo) del objeto: si el script lo
    // movio por afuera (setPos3/setRotacion), el proximo paso lo detecta y
    // re-sincroniza el cuerpo (teletransporte sin pelearse con lua)
    Vector3    prevObjPos;
    Quaternion prevObjRot;

    int  quietud;
    bool dormido;
    bool activo;     // false = el cuerpo no colisiona ni se integra
                     // (fisicaActiva(obj,false): Claude adentro del auto)
};

static std::vector<Rigido> gR;
static std::vector<W3dRigidoContacto> gEventos;

static void ActualizarEjes(Rigido& c) {
    c.u[0] = c.rot * Vector3(1, 0, 0);
    c.u[1] = c.rot * Vector3(0, 1, 0);
    c.u[2] = c.rot * Vector3(0, 0, 1);
}

// I_mundo^-1 * v  (R * diag(invI) * R^T * v, sin armar la matriz)
static Vector3 InerciaInv(const Rigido& c, const Vector3& v) {
    return c.u[0] * (c.invInercia.x * c.u[0].Dot(v)) +
           c.u[1] * (c.invInercia.y * c.u[1].Dot(v)) +
           c.u[2] * (c.invInercia.z * c.u[2].Dot(v));
}

static Rigido* Buscar(Object* o) {
    if (!o) return NULL;
    for (size_t i = 0; i < gR.size(); i++)
        if (gR[i].obj == o) return &gR[i];
    return NULL;
}

static void Despertar(Rigido& c) {
    c.dormido = false;
    c.quietud = 0;
}

// ---------------------------------------------------------------------------
//  ciclo de vida
// ---------------------------------------------------------------------------
static void CrearCuerpo(Object* o) {
    const W3dRigidoDef* d = o->fisica;
    if (!d || Buscar(o)) return;
    // los cuerpos se asumen de primer nivel: la fisica escribe pos/rot del
    // objeto EN MUNDO y un padre con transform los descuadraria
    extern Object* SceneCollection;
    if (o->Parent && o->Parent != SceneCollection) {
        w3dLogfW("[rigido] '%s' tiene fisica pero esta emparentado: se salta",
                 o->name.c_str());
        return;
    }
    Rigido c;
    c.obj  = o;
    c.tipo = d->tipo;
    c.half = Vector3(0.5f * d->caja[0] * o->scale.x,
                     0.5f * d->caja[1] * o->scale.y,
                     0.5f * d->caja[2] * o->scale.z);
    if (c.half.x < 0.01f) c.half.x = 0.01f;
    if (c.half.y < 0.01f) c.half.y = 0.01f;
    if (c.half.z < 0.01f) c.half.z = 0.01f;
    c.centroLocal = Vector3(d->centro[0] * o->scale.x,
                            d->centro[1] * o->scale.y,
                            d->centro[2] * o->scale.z);
    c.friccion = d->friccion;
    c.rebote   = d->rebote;
    float m = (d->masa > 0.001f) ? d->masa : 0.001f;
    c.invMasa = (c.tipo == 0) ? 0.0f : 1.0f / m;
    if (c.tipo == 1) {
        // inercia de una caja solida: I = m/12 * (h^2 + p^2) por eje
        float ex = 2.0f * c.half.x, ey = 2.0f * c.half.y, ez = 2.0f * c.half.z;
        float ix = m * (ey * ey + ez * ez) / 12.0f;
        float iy = m * (ex * ex + ez * ez) / 12.0f;
        float iz = m * (ex * ex + ey * ey) / 12.0f;
        c.invInercia = Vector3(1.0f / ix, 1.0f / iy, 1.0f / iz);
    } else {
        // estatico o personaje: la rotacion no la toca la fisica
        c.invInercia = Vector3(0, 0, 0);
    }
    c.rot = o->Rot().Normalized();
    c.pos = o->pos + c.rot * c.centroLocal;
    c.vel = Vector3(0, 0, 0);
    c.velAng = Vector3(0, 0, 0);
    c.fAcum = Vector3(0, 0, 0);
    c.tAcum = Vector3(0, 0, 0);
    c.quietud = 0;
    c.dormido = (c.tipo != 0);   // los dinamicos ARRANCAN dormidos: nada se
    c.activo = true;             // cae solo hasta que algo lo toque o empuje
    c.prevObjPos = o->pos;
    c.prevObjRot = o->Rot();
    ActualizarEjes(c);
    gR.push_back(c);
}

static void AsegurarRec(Object* o) {
    if (!o) return;
    if (o->fisica) CrearCuerpo(o);
    for (size_t i = 0; i < o->Childrens.size(); i++)
        AsegurarRec(o->Childrens[i]);
}

void W3dRigidosAsegurar(Object* raiz) {
    if (!raiz) return;
    AsegurarRec(raiz);
}

void W3dRigidosLimpiar(void) {
    gR.clear();
    gEventos.clear();
}

void W3dRigidosOlvidar(Object* o) {
    for (size_t i = 0; i < gR.size(); i++)
        if (gR[i].obj == o) { gR.erase(gR.begin() + i); return; }
}

bool W3dRigidosHay(void) { return !gR.empty(); }

const std::vector<W3dRigidoContacto>& W3dRigidosContactos(void) { return gEventos; }

// ---------------------------------------------------------------------------
//  consultas / acciones
// ---------------------------------------------------------------------------
bool W3dRigidoVel(Object* o, Vector3* out) {
    Rigido* c = Buscar(o);
    if (!c) return false;
    if (out) *out = c->vel;
    return true;
}
void W3dRigidoSetVel(Object* o, const Vector3& v) {
    Rigido* c = Buscar(o);
    if (!c || c->tipo == 0) return;
    c->vel = v;
    Despertar(*c);
}
bool W3dRigidoVelAng(Object* o, Vector3* out) {
    Rigido* c = Buscar(o);
    if (!c) return false;
    if (out) *out = c->velAng;
    return true;
}
void W3dRigidoSetVelAng(Object* o, const Vector3& v) {
    Rigido* c = Buscar(o);
    if (!c || c->tipo != 1) return;
    c->velAng = v;
    Despertar(*c);
}
void W3dRigidoImpulso(Object* o, const Vector3& imp, const Vector3& punto) {
    Rigido* c = Buscar(o);
    if (!c || c->tipo == 0) return;
    c->vel += imp * c->invMasa;
    c->velAng += InerciaInv(*c, Vector3::Cross(punto - c->pos, imp));
    Despertar(*c);
}
void W3dRigidoFuerza(Object* o, const Vector3& f, const Vector3& punto) {
    Rigido* c = Buscar(o);
    if (!c || c->tipo == 0) return;
    c->fAcum += f;
    c->tAcum += Vector3::Cross(punto - c->pos, f);
    // OJO: NO resetea la quietud. Una fuerza CONTINUA y balanceada (la
    // suspension de un auto detenido) no debe impedir que el cuerpo se
    // duerma; el criterio de sueno es la VELOCIDAD. Un impulso si despierta.
    c->dormido = false;
}
void W3dRigidosGravedad(float g) { gGravedad = g; }
float W3dRigidosGravedadActual(void) { return gGravedad; }

// prende/apaga el cuerpo sin sacarlo (apagado = no colisiona ni se integra);
// al prenderlo se re-sincroniza con el objeto y arranca despierto
void W3dRigidoActivo(Object* o, bool activo) {
    Rigido* c = Buscar(o);
    if (!c) return;
    if (activo && !c->activo) {
        c->rot = o->Rot().Normalized();
        c->pos = o->pos + c->rot * c->centroLocal;
        c->vel = Vector3(0, 0, 0);
        c->velAng = Vector3(0, 0, 0);
        c->prevObjPos = o->pos;
        c->prevObjRot = o->Rot();
        ActualizarEjes(*c);
        Despertar(*c);
    }
    c->activo = activo;
}

// ---------------------------------------------------------------------------
//  rayo vs cajas (slab test en el espacio local de cada caja)
// ---------------------------------------------------------------------------
float W3dRigidosRayo(const Vector3& origen, const Vector3& dir, float maxDist,
                     Object* ignorar, Vector3* normalOut, Object** tocadoOut) {
    float mejor = -1.0f;
    Vector3 mejorN(0, 1, 0);
    Object* mejorO = NULL;
    for (size_t i = 0; i < gR.size(); i++) {
        Rigido& c = gR[i];
        if (c.obj == ignorar || !c.activo) continue;
        // al espacio de la caja
        Vector3 rel = origen - c.pos;
        float o0 = c.u[0].Dot(rel), o1 = c.u[1].Dot(rel), o2 = c.u[2].Dot(rel);
        float d0 = c.u[0].Dot(dir), d1 = c.u[1].Dot(dir), d2 = c.u[2].Dot(dir);
        float ol[3]; ol[0] = o0; ol[1] = o1; ol[2] = o2;
        float dl[3]; dl[0] = d0; dl[1] = d1; dl[2] = d2;
        float hl[3]; hl[0] = c.half.x; hl[1] = c.half.y; hl[2] = c.half.z;
        float tMin = 0.0f, tMax = maxDist;
        int ejeMin = -1; float signoMin = 1.0f;
        bool afuera = false;
        for (int e = 0; e < 3 && !afuera; e++) {
            if (fabsf(dl[e]) < 1e-8f) {
                if (ol[e] < -hl[e] || ol[e] > hl[e]) afuera = true;
            } else {
                float inv = 1.0f / dl[e];
                float t1 = (-hl[e] - ol[e]) * inv;
                float t2 = ( hl[e] - ol[e]) * inv;
                float signo = -1.0f;
                if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; signo = 1.0f; }
                if (t1 > tMin) { tMin = t1; ejeMin = e; signoMin = signo; }
                if (t2 < tMax) tMax = t2;
                if (tMin > tMax) afuera = true;
            }
        }
        if (afuera || ejeMin < 0) continue;
        if (mejor < 0.0f || tMin < mejor) {
            mejor = tMin;
            mejorN = c.u[ejeMin] * signoMin;
            mejorO = c.obj;
        }
    }
    if (mejor >= 0.0f) {
        if (normalOut) *normalOut = mejorN;
        if (tocadoOut) *tocadoOut = mejorO;
    }
    return mejor;
}

// ---------------------------------------------------------------------------
//  deteccion OBB vs OBB: SAT + manifold por recorte de caras
// ---------------------------------------------------------------------------
struct PuntoContacto {
    Vector3 pos;        // en mundo
    float   hundido;    // penetracion (>0)
    float   acumN;      // impulso normal acumulado (el solver lo llena)
    float   acumT1, acumT2;
    float   vnInicial;  // vel normal relativa al armar el contacto (rebote)
};
struct Manifold {
    int     ia, ib;     // indices en gR
    Vector3 normal;     // de A hacia B
    int     n;
    PuntoContacto p[8];
};

// vertices (mundo) de la cara de 'c' cuyo indice de eje es 'eje' y signo 'sg'
static void CaraDeCaja(const Rigido& c, int eje, float sg, Vector3 out[4]) {
    int e1 = (eje + 1) % 3, e2 = (eje + 2) % 3;
    float h[3]; h[0] = c.half.x; h[1] = c.half.y; h[2] = c.half.z;
    Vector3 centro = c.pos + c.u[eje] * (sg * h[eje]);
    Vector3 a = c.u[e1] * h[e1];
    Vector3 b = c.u[e2] * h[e2];
    out[0] = centro + a + b;
    out[1] = centro - a + b;
    out[2] = centro - a - b;
    out[3] = centro + a - b;
}

// recorta un poligono contra el semiespacio dot(n,p) <= d (quedan los de adentro)
static int RecortarPlano(const Vector3& n, float d, const Vector3* in, int nIn,
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
static bool Colision(int ia, int ib, Manifold& m) {
    Rigido& A = gR[ia];
    Rigido& B = gR[ib];
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

    float mejorCara = -1e30f; int caraEje = -1; int caraDe = 0;   // 0=A 1=B
    // caras de A
    for (int i = 0; i < 3; i++) {
        float da = A.u[i].Dot(d);
        float r = hA[i] + hB[0] * ac[i][0] + hB[1] * ac[i][1] + hB[2] * ac[i][2];
        float sep = fabsf(da) - r;               // <0 = solapan en este eje
        if (sep > 0.0f) return false;
        if (sep > mejorCara) { mejorCara = sep; caraEje = i; caraDe = 0; }
    }
    // caras de B
    for (int j = 0; j < 3; j++) {
        float db = B.u[j].Dot(d);
        float r = hB[j] + hA[0] * ac[0][j] + hA[1] * ac[1][j] + hA[2] * ac[2][j];
        float sep = fabsf(db) - r;
        if (sep > 0.0f) return false;
        if (sep > mejorCara) { mejorCara = sep; caraEje = j; caraDe = 1; }
    }
    // ejes cruzados (aristas)
    float mejorArista = -1e30f; int aEjeA = -1, aEjeB = -1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vector3 eje = Vector3::Cross(A.u[i], B.u[j]);
            float len = eje.Length();
            if (len < 1e-4f) continue;           // casi paralelos: lo cubren las caras
            eje = eje * (1.0f / len);
            float da = eje.Dot(d);
            int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
            int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            // proyeccion de cada caja sobre el eje: CADA semieje con SU dot
            // (el eje es perpendicular a u[i]/u[j], por eso esos dos no aportan)
            float r = hA[i1] * fabsf(eje.Dot(A.u[i1])) + hA[i2] * fabsf(eje.Dot(A.u[i2])) +
                      hB[j1] * fabsf(eje.Dot(B.u[j1])) + hB[j2] * fabsf(eje.Dot(B.u[j2]));
            float sep = fabsf(da) - r;
            if (sep > 0.0f) return false;
            if (sep > mejorArista) { mejorArista = sep; aEjeA = i; aEjeB = j; }
        }

    m.ia = ia; m.ib = ib; m.n = 0;

    // preferir el contacto de CARA salvo que la arista sea claramente mejor
    // (el sesgo evita que el manifold salte entre cara y arista y vibre)
    bool usarArista = (aEjeA >= 0) && (mejorArista > mejorCara * 0.95f + 0.005f);

    if (!usarArista) {
        // ---- cara de referencia + recorte de la cara incidente -------------
        const Rigido& R = caraDe == 0 ? A : B;    // caja de referencia
        const Rigido& I = caraDe == 0 ? B : A;    // caja incidente
        Vector3 dRI = I.pos - R.pos;
        float sg = (R.u[caraEje].Dot(dRI) >= 0.0f) ? 1.0f : -1.0f;
        Vector3 nRef = R.u[caraEje] * sg;         // normal de ref hacia I
        // cara incidente: la de I mas enfrentada a nRef
        int ejeInc = 0; float peor = 0.0f;
        for (int e = 0; e < 3; e++) {
            float dp = I.u[e].Dot(nRef);
            if (e == 0 || fabsf(dp) > fabsf(peor)) { peor = dp; ejeInc = e; }
        }
        float sgInc = (peor > 0.0f) ? -1.0f : 1.0f;
        Vector3 cara[8], tmp[8];
        CaraDeCaja(I, ejeInc, sgInc, cara);
        int nPts = 4;
        // recortar contra los 4 planos laterales de la cara de referencia
        float hR[3]; hR[0] = R.half.x; hR[1] = R.half.y; hR[2] = R.half.z;
        int e1 = (caraEje + 1) % 3, e2 = (caraEje + 2) % 3;
        nPts = RecortarPlano(R.u[e1],  R.u[e1].Dot(R.pos) + hR[e1], cara, nPts, tmp);
        nPts = RecortarPlano(-R.u[e1], -(R.u[e1].Dot(R.pos) - hR[e1]), tmp, nPts, cara);
        nPts = RecortarPlano(R.u[e2],  R.u[e2].Dot(R.pos) + hR[e2], cara, nPts, tmp);
        nPts = RecortarPlano(-R.u[e2], -(R.u[e2].Dot(R.pos) - hR[e2]), tmp, nPts, cara);
        // quedarse con los que penetran la cara de referencia
        float dCara = nRef.Dot(R.pos + nRef * hR[caraEje]);
        for (int i = 0; i < nPts && m.n < 8; i++) {
            float hund = dCara - nRef.Dot(cara[i]);
            if (hund > 0.0f) {
                PuntoContacto& p = m.p[m.n++];
                p.pos = cara[i];
                p.hundido = hund;
                p.acumN = p.acumT1 = p.acumT2 = 0.0f;
                p.vnInicial = 0.0f;
            }
        }
        // normal SIEMPRE de A hacia B (el solver la asume asi)
        m.normal = (caraDe == 0) ? nRef : -nRef;
        // si el recorte quedo con mas de 4, reducir a los 4 mas hundidos
        while (m.n > 4) {
            int menor = 0;
            for (int i = 1; i < m.n; i++)
                if (m.p[i].hundido < m.p[menor].hundido) menor = i;
            m.p[menor] = m.p[m.n - 1];
            m.n--;
        }
    } else {
        // ---- arista vs arista: punto mas cercano entre los dos segmentos ---
        Vector3 eje = Vector3::Cross(A.u[aEjeA], B.u[aEjeB]).Normalized();
        if (eje.Dot(d) < 0.0f) eje = -eje;        // de A hacia B
        // arranco de los centros y me muevo a la arista de cada caja
        Vector3 pA = A.pos, pB = B.pos;
        for (int e = 0; e < 3; e++) {
            if (e != aEjeA) pA += A.u[e] * ((eje.Dot(A.u[e]) >= 0.0f ? 1.0f : -1.0f) * hA[e]);
            if (e != aEjeB) pB -= B.u[e] * ((eje.Dot(B.u[e]) >= 0.0f ? 1.0f : -1.0f) * hB[e]);
        }
        // punto mas cercano entre las rectas pA+s*dirA y pB+t*dirB
        Vector3 dirA = A.u[aEjeA], dirB = B.u[aEjeB];
        Vector3 r = pB - pA;
        float a2 = dirA.Dot(dirA), b2 = dirB.Dot(dirB), ab = dirA.Dot(dirB);
        float ra = dirA.Dot(r), rb = dirB.Dot(r);
        float den = a2 * b2 - ab * ab;
        float s = (fabsf(den) > 1e-8f) ? (ra * b2 - rb * ab) / den : 0.0f;
        if (s >  hA[aEjeA]) s =  hA[aEjeA];
        if (s < -hA[aEjeA]) s = -hA[aEjeA];
        Vector3 cA = pA + dirA * s;
        float t = dirB.Dot(cA - pB);
        if (t >  hB[aEjeB]) t =  hB[aEjeB];
        if (t < -hB[aEjeB]) t = -hB[aEjeB];
        Vector3 cB = pB + dirB * t;
        PuntoContacto& p = m.p[0];
        p.pos = (cA + cB) * 0.5f;
        p.hundido = -mejorArista;
        p.acumN = p.acumT1 = p.acumT2 = 0.0f;
        p.vnInicial = 0.0f;
        m.n = 1;
        m.normal = eje;
    }
    return m.n > 0;
}

// ---------------------------------------------------------------------------
//  solver de impulsos secuenciales
// ---------------------------------------------------------------------------
static Vector3 VelEnPunto(const Rigido& c, const Vector3& p) {
    return c.vel + Vector3::Cross(c.velAng, p - c.pos);
}

static void Resolver(std::vector<Manifold>& ms, float h) {
    // velocidad normal inicial (para el rebote) y tangentes por manifold
    for (size_t k = 0; k < ms.size(); k++) {
        Manifold& m = ms[k];
        Rigido& A = gR[m.ia];
        Rigido& B = gR[m.ib];
        for (int i = 0; i < m.n; i++) {
            Vector3 vr = VelEnPunto(B, m.p[i].pos) - VelEnPunto(A, m.p[i].pos);
            m.p[i].vnInicial = vr.Dot(m.normal);
        }
    }
    for (int iter = 0; iter < ITERACIONES; iter++) {
        for (size_t k = 0; k < ms.size(); k++) {
            Manifold& m = ms[k];
            Rigido& A = gR[m.ia];
            Rigido& B = gR[m.ib];
            const Vector3& n = m.normal;
            // tangentes ortogonales a la normal
            Vector3 t1 = Vector3::Cross(n, Vector3(0, 1, 0));
            if (t1.LengthSq() < 1e-6f) t1 = Vector3::Cross(n, Vector3(1, 0, 0));
            t1 = t1.Normalized();
            Vector3 t2 = Vector3::Cross(n, t1);
            float mu = sqrtf(A.friccion * B.friccion);
            float e = A.rebote > B.rebote ? A.rebote : B.rebote;
            for (int i = 0; i < m.n; i++) {
                PuntoContacto& p = m.p[i];
                Vector3 ra = p.pos - A.pos;
                Vector3 rb = p.pos - B.pos;
                // ---- impulso normal (con Baumgarte y rebote) ---------------
                Vector3 vr = VelEnPunto(B, p.pos) - VelEnPunto(A, p.pos);
                float vn = vr.Dot(n);
                float kn = A.invMasa + B.invMasa +
                    n.Dot(Vector3::Cross(InerciaInv(A, Vector3::Cross(ra, n)), ra)) +
                    n.Dot(Vector3::Cross(InerciaInv(B, Vector3::Cross(rb, n)), rb));
                if (kn < 1e-8f) continue;
                float sesgo = (BAUMGARTE / h) * (p.hundido - PENETRACION_OK);
                if (sesgo < 0.0f) sesgo = 0.0f;
                if (sesgo > 4.0f) sesgo = 4.0f;   // tope: una penetracion honda
                                                  // empuja, no catapulta
                float rebote = 0.0f;
                if (p.vnInicial < -UMBRAL_REBOTE) rebote = -e * p.vnInicial;
                float dj = (-vn + sesgo + rebote) / kn;
                float acum0 = p.acumN;
                p.acumN = acum0 + dj;
                if (p.acumN < 0.0f) p.acumN = 0.0f;
                dj = p.acumN - acum0;
                Vector3 imp = n * dj;
                A.vel -= imp * A.invMasa;
                A.velAng -= InerciaInv(A, Vector3::Cross(ra, imp));
                B.vel += imp * B.invMasa;
                B.velAng += InerciaInv(B, Vector3::Cross(rb, imp));
                // ---- friccion en dos tangentes -----------------------------
                float topeT = mu * p.acumN;
                for (int tt = 0; tt < 2; tt++) {
                    const Vector3& t = tt == 0 ? t1 : t2;
                    float* acumT = tt == 0 ? &p.acumT1 : &p.acumT2;
                    vr = VelEnPunto(B, p.pos) - VelEnPunto(A, p.pos);
                    float vt = vr.Dot(t);
                    float kt = A.invMasa + B.invMasa +
                        t.Dot(Vector3::Cross(InerciaInv(A, Vector3::Cross(ra, t)), ra)) +
                        t.Dot(Vector3::Cross(InerciaInv(B, Vector3::Cross(rb, t)), rb));
                    if (kt < 1e-8f) continue;
                    float djt = -vt / kt;
                    float acumT0 = *acumT;
                    *acumT = acumT0 + djt;
                    if (*acumT >  topeT) *acumT =  topeT;
                    if (*acumT < -topeT) *acumT = -topeT;
                    djt = *acumT - acumT0;
                    Vector3 impT = t * djt;
                    A.vel -= impT * A.invMasa;
                    A.velAng -= InerciaInv(A, Vector3::Cross(ra, impT));
                    B.vel += impT * B.invMasa;
                    B.velAng += InerciaInv(B, Vector3::Cross(rb, impT));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  el paso
// ---------------------------------------------------------------------------
static void RegistrarEvento(const Manifold& m) {
    float total = 0.0f;
    int mejor = 0;
    for (int i = 0; i < m.n; i++) {
        total += m.p[i].acumN;
        if (m.p[i].hundido > m.p[mejor].hundido) mejor = i;
    }
    if (total <= 1e-4f) return;
    Object* a = gR[m.ia].obj;
    Object* b = gR[m.ib].obj;
    // acumular sobre un evento existente del MISMO par (los dos sub-pasos del
    // frame suman: el script ve UN golpe con el impulso total)
    for (size_t i = 0; i < gEventos.size(); i++) {
        if ((gEventos[i].a == a && gEventos[i].b == b) ||
            (gEventos[i].a == b && gEventos[i].b == a)) {
            gEventos[i].impulso += total;
            return;
        }
    }
    W3dRigidoContacto ev;
    ev.a = a; ev.b = b;
    ev.punto[0] = m.p[mejor].pos.x;
    ev.punto[1] = m.p[mejor].pos.y;
    ev.punto[2] = m.p[mejor].pos.z;
    ev.normal[0] = m.normal.x;
    ev.normal[1] = m.normal.y;
    ev.normal[2] = m.normal.z;
    ev.impulso = total;
    gEventos.push_back(ev);
}

void W3dRigidosPaso(float dt) {
    if (gR.empty()) return;
    if (dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;
    gEventos.clear();
    // ---- re-sincronizar lo que el script movio por afuera -------------------
    for (size_t i = 0; i < gR.size(); i++) {
        Rigido& c = gR[i];
        Object* o = c.obj;
        if ((o->pos - c.prevObjPos).LengthSq() > 1e-8f) {
            c.rot = o->Rot().Normalized();
            c.pos = o->pos + c.rot * c.centroLocal;
            ActualizarEjes(c);
        } else {
            float dq = fabsf(o->Rot().w * c.prevObjRot.w + o->Rot().x * c.prevObjRot.x +
                             o->Rot().y * c.prevObjRot.y + o->Rot().z * c.prevObjRot.z);
            if (dq < 0.99999f) {
                c.rot = o->Rot().Normalized();
                c.pos = o->pos + c.rot * c.centroLocal;
                ActualizarEjes(c);
            }
        }
    }
    float h = dt / (float)SUBPASOS;
    std::vector<Manifold> ms;
    for (int paso = 0; paso < SUBPASOS; paso++) {
        // ---- integrar velocidades (gravedad + fuerzas del frame) -----------
        for (size_t i = 0; i < gR.size(); i++) {
            Rigido& c = gR[i];
            if (c.tipo == 0 || c.dormido || !c.activo) continue;
            c.vel.y -= gGravedad * h;
            c.vel += c.fAcum * (c.invMasa * h);
            c.velAng += InerciaInv(c, c.tAcum) * h;
        }
        // ---- detectar contactos --------------------------------------------
        ms.clear();
        for (size_t i = 0; i < gR.size(); i++) {
            for (size_t j = i + 1; j < gR.size(); j++) {
                Rigido& A = gR[i];
                Rigido& B = gR[j];
                if (!A.activo || !B.activo) continue;
                if (A.tipo == 0 && B.tipo == 0) continue;
                bool aQuieto = A.dormido || A.tipo == 0;
                bool bQuieto = B.dormido || B.tipo == 0;
                if (aQuieto && bQuieto) continue;
                // poda por esfera envolvente (barato) antes del SAT
                float ra = A.half.Length(), rb = B.half.Length();
                if ((B.pos - A.pos).LengthSq() > (ra + rb) * (ra + rb)) continue;
                Manifold m;
                if (!Colision((int)i, (int)j, m)) continue;
                // el toque despierta al dormido si el otro se mueve de verdad
                if (A.dormido && (B.vel.LengthSq() > DESPERTAR_VEL2 || B.velAng.LengthSq() > DESPERTAR_VEL2)) Despertar(A);
                if (B.dormido && (A.vel.LengthSq() > DESPERTAR_VEL2 || A.velAng.LengthSq() > DESPERTAR_VEL2)) Despertar(B);
                ms.push_back(m);
            }
        }
        // ---- resolver -------------------------------------------------------
        if (!ms.empty()) Resolver(ms, h);
        // ---- integrar posiciones -------------------------------------------
        for (size_t i = 0; i < gR.size(); i++) {
            Rigido& c = gR[i];
            if (c.tipo == 0 || c.dormido || !c.activo) continue;
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
            ActualizarEjes(c);
        }
        // ---- eventos del sub-paso -------------------------------------------
        for (size_t k = 0; k < ms.size(); k++) RegistrarEvento(ms[k]);
    }
    // ---- fin de frame: consumir fuerzas, dormir quietos, escribir objetos ---
    for (size_t i = 0; i < gR.size(); i++) {
        Rigido& c = gR[i];
        c.fAcum = Vector3(0, 0, 0);
        c.tAcum = Vector3(0, 0, 0);
        if (c.tipo == 0 || c.dormido || !c.activo) {
            c.prevObjPos = c.obj->pos;
            c.prevObjRot = c.obj->Rot();
            continue;
        }
        if (c.vel.LengthSq() < DORMIR_LIN2 && c.velAng.LengthSq() < DORMIR_ANG2) {
            if (++c.quietud >= DORMIR_FRAMES) {
                c.dormido = true;
                c.vel = Vector3(0, 0, 0);
                c.velAng = Vector3(0, 0, 0);
            }
        } else {
            c.quietud = 0;
        }
        Object* o = c.obj;
        o->pos = c.pos - (c.rot * c.centroLocal);
        if (c.tipo != 2) {
            // el PERSONAJE no rota por fisica: su facing lo maneja el script
            o->SetRot(c.rot);
        }
        c.prevObjPos = o->pos;
        c.prevObjRot = o->Rot();
    }
}

// ---------------------------------------------------------------------------
//  binds lua
// ---------------------------------------------------------------------------
static Object* ObjArg(lua_State* L, int idx) {
    return W3dScriptParamObjeto((void*)L, idx);
}

// fisicaVel(obj) -> vx,vy,vz | fisicaVel(obj, vx,vy,vz)
static int LFisicaVel(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    if (lua_gettop(L) >= 4) {
        W3dRigidoSetVel(o, Vector3((float)luaL_checknumber(L, 2),
                                   (float)luaL_checknumber(L, 3),
                                   (float)luaL_checknumber(L, 4)));
        return 0;
    }
    Vector3 v;
    if (!W3dRigidoVel(o, &v)) return 0;
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z);
    return 3;
}

static int LFisicaVelAng(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    if (lua_gettop(L) >= 4) {
        W3dRigidoSetVelAng(o, Vector3((float)luaL_checknumber(L, 2),
                                      (float)luaL_checknumber(L, 3),
                                      (float)luaL_checknumber(L, 4)));
        return 0;
    }
    Vector3 v;
    if (!W3dRigidoVelAng(o, &v)) return 0;
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z);
    return 3;
}

// fisicaImpulso(obj, ix,iy,iz [,px,py,pz])
static int LFisicaImpulso(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    Vector3 imp((float)luaL_checknumber(L, 2),
                (float)luaL_checknumber(L, 3),
                (float)luaL_checknumber(L, 4));
    Vector3 p = o->pos;
    Rigido* c = Buscar(o);
    if (c) p = c->pos;
    if (lua_gettop(L) >= 7)
        p = Vector3((float)luaL_checknumber(L, 5),
                    (float)luaL_checknumber(L, 6),
                    (float)luaL_checknumber(L, 7));
    W3dRigidoImpulso(o, imp, p);
    return 0;
}

// fisicaFuerza(obj, fx,fy,fz [,px,py,pz]) : fuerza de ESTE frame
static int LFisicaFuerza(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    Vector3 f((float)luaL_checknumber(L, 2),
              (float)luaL_checknumber(L, 3),
              (float)luaL_checknumber(L, 4));
    Vector3 p = o->pos;
    Rigido* c = Buscar(o);
    if (c) p = c->pos;
    if (lua_gettop(L) >= 7)
        p = Vector3((float)luaL_checknumber(L, 5),
                    (float)luaL_checknumber(L, 6),
                    (float)luaL_checknumber(L, 7));
    W3dRigidoFuerza(o, f, p);
    return 0;
}

// fisicaRayo(ox,oy,oz, dx,dy,dz, max [,ignorar]) -> dist,nx,ny,nz,obj | nil
static int LFisicaRayo(lua_State* L) {
    Vector3 o((float)luaL_checknumber(L, 1),
              (float)luaL_checknumber(L, 2),
              (float)luaL_checknumber(L, 3));
    Vector3 d((float)luaL_checknumber(L, 4),
              (float)luaL_checknumber(L, 5),
              (float)luaL_checknumber(L, 6));
    float mx = (float)luaL_checknumber(L, 7);
    Object* ig = (lua_gettop(L) >= 8) ? ObjArg(L, 8) : NULL;
    d = d.Normalized();
    Vector3 n;
    Object* tocado = NULL;
    float t = W3dRigidosRayo(o, d, mx, ig, &n, &tocado);
    if (t < 0.0f) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, t);
    lua_pushnumber(L, n.x);
    lua_pushnumber(L, n.y);
    lua_pushnumber(L, n.z);
    if (tocado) lua_pushlightuserdata(L, tocado); else lua_pushnil(L);
    return 5;
}

// contactos() -> lista de golpes del ultimo paso
static int LContactos(lua_State* L) {
    const std::vector<W3dRigidoContacto>& evs = W3dRigidosContactos();
    lua_createtable(L, (int)evs.size(), 0);
    for (size_t i = 0; i < evs.size(); i++) {
        const W3dRigidoContacto& e = evs[i];
        lua_createtable(L, 0, 9);
        lua_pushlightuserdata(L, e.a);   lua_setfield(L, -2, "a");
        lua_pushlightuserdata(L, e.b);   lua_setfield(L, -2, "b");
        lua_pushnumber(L, e.punto[0]);   lua_setfield(L, -2, "x");
        lua_pushnumber(L, e.punto[1]);   lua_setfield(L, -2, "y");
        lua_pushnumber(L, e.punto[2]);   lua_setfield(L, -2, "z");
        lua_pushnumber(L, e.normal[0]);  lua_setfield(L, -2, "nx");
        lua_pushnumber(L, e.normal[1]);  lua_setfield(L, -2, "ny");
        lua_pushnumber(L, e.normal[2]);  lua_setfield(L, -2, "nz");
        lua_pushnumber(L, e.impulso);    lua_setfield(L, -2, "impulso");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// fisicaDespierto(obj) -> true si el cuerpo esta despierto (simulando). El
// juego lo usa para NO gastar en un auto dormido (suspension, ruedas).
static int LFisicaDespierto(lua_State* L) {
    Object* o = ObjArg(L, 1);
    Rigido* c = o ? Buscar(o) : NULL;
    lua_pushboolean(L, (c && c->activo && !c->dormido && c->tipo != 0) ? 1 : 0);
    return 1;
}

// fisicaActiva(obj, true/false): prende o apaga el cuerpo (Claude adentro
// del auto se apaga: sino su caja pelearia con la del auto todo el tiempo)
static int LFisicaActiva(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    W3dRigidoActivo(o, lua_toboolean(L, 2) != 0);
    return 0;
}

// fisicaGravedad([g])
static int LFisicaGravedad(lua_State* L) {
    if (lua_gettop(L) >= 1) {
        W3dRigidosGravedad((float)luaL_checknumber(L, 1));
        return 0;
    }
    lua_pushnumber(L, W3dRigidosGravedadActual());
    return 1;
}

void W3dRigidosRegistrarBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LFisicaVel);      lua_setglobal(L, "fisicaVel");
    lua_pushcfunction(L, LFisicaVelAng);   lua_setglobal(L, "fisicaVelAng");
    lua_pushcfunction(L, LFisicaImpulso);  lua_setglobal(L, "fisicaImpulso");
    lua_pushcfunction(L, LFisicaFuerza);   lua_setglobal(L, "fisicaFuerza");
    lua_pushcfunction(L, LFisicaRayo);     lua_setglobal(L, "fisicaRayo");
    lua_pushcfunction(L, LContactos);      lua_setglobal(L, "contactos");
    lua_pushcfunction(L, LFisicaGravedad); lua_setglobal(L, "fisicaGravedad");
    lua_pushcfunction(L, LFisicaActiva);   lua_setglobal(L, "fisicaActiva");
    lua_pushcfunction(L, LFisicaDespierto);lua_setglobal(L, "fisicaDespierto");
}
