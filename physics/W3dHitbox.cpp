// ============================================================================
//  W3dHitbox.cpp — ver W3dHitbox.h. Dialecto C++03 (Symbian compila esto).
//  Comentarios en espanol sin acentos.
//
//  Un tick (W3dHitboxStep):
//    1. CONTACTOS de los cuerpos rigidos (gEvents de W3dRigido) -> alTocar en
//       los pares que EMPIEZAN a tocarse.
//    2. RECOLECTAR: recorrer el arbol y juntar los hitbox activos y visibles con
//       su caja en MUNDO (+ los cuerpos rigidos si algun hitbox los detecta).
//    3. BROADPHASE (grilla uniforme en cubetas de hash) + poda por esfera y por caja
//       alineada + SAT exacto -> los pares solapados.
//    4. DIFERENCIA con los pares del tick anterior -> alSalir / alEntrar /
//       alQuedarse, por LADO (cada lado filtra por su cuenta).
//  IDENTIDAD vs ORDEN: los pares se identifican (y se ordenan para la diferencia)
//  por Object::serial, que es de la sesion (otra carga = otros seriales); los
//  AVISOS salen en el orden del ARBOL (preorden), que es el contenido de la escena.
//  Todo en vectores estaticos que se reusan: despues del primer tick no hay
//  allocations (salvo que crezca la escena).
// ============================================================================
#include "physics/W3dHitbox.h"
#include "physics/W3dRigid.h"
#include "objects/Objects.h"
#include "math/Matrix4.h"
#include "script/W3dScript.h"   // W3dScriptEvent / W3dScriptParamObject / W3dScriptRedraw
#include "base/w3dlog.h"
#include <math.h>
#include <algorithm>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

bool g_w3dHitboxVer = false;

// hitbox VIVOS en el proceso (los cuenta el constructor/destructor de la base): con cero, el
// paso de hitbox no recorre nada. Es el "costo cero si nadie lo usa".
static int gLive = 0;

W3dHitboxBase::W3dHitboxBase(Object* parent, const std::string& name, const Vector3& pos)
    : Object(parent, name, pos), active(true), detectBodies(false) {
    size[0] = size[1] = size[2] = 1.0f;
    center[0] = center[1] = center[2] = 0.0f;
    gLive++;
}

W3dHitboxBase::~W3dHitboxBase() {
    gLive--;
}

bool W3dHitboxHas(void) { return gLive > 0; }

Object* W3dHitboxOwner(Object* hb) {
    if (!hb) return NULL;
    Object* p = hb->Parent;
    // la raiz de la escena y las colecciones solo ORDENAN el arbol: no son un dueno
    if (!p || p == SceneCollection || p->getType() == ObjectType::collection ||
        p->getType() == ObjectType::scene)
        return hb;
    return p;
}

// ---------------------------------------------------------------------------
//  estado del motor
// ---------------------------------------------------------------------------
// un participante del broadphase: un hitbox o un cuerpo rigido, con su caja en MUNDO
struct HbEntry {
    Object* o;               // el hitbox o el cuerpo
    Object* owner;           // lo que el OTRO lado ve como 'otro'
    W3dHitboxBase* hb;       // NULL = cuerpo rigido
    unsigned serial;
    int order;               // posicion en el ARBOL (preorden): el orden de los avisos
    Vector3 c, u[3], h;      // centro, ejes y semiejes
    float r;                 // radio de la esfera envolvente
    float ext[3];            // semiejes de la caja ALINEADA a los ejes que la envuelve (poda barata)
    int c0[3], c1[3];        // rango de celdas de la grilla (inclusive)
};

// un objeto CONOCIDO este tick (hitbox en el arbol, participe o no, o cuerpo rigido): con esto
// se resuelve el serial de un par que SALIO sin tocar punteros viejos
struct HbKnown {
    unsigned serial;
    Object* o;
    W3dHitboxBase* hb;       // NULL = cuerpo rigido
    int order;               // posicion en el arbol ESTE tick (ver HbEntry)
};
struct HbKnownLower {
    bool operator()(const HbKnown& a, const HbKnown& b) const { return a.serial < b.serial; }
    bool operator()(const HbKnown& a, unsigned s) const { return a.serial < s; }
    bool operator()(unsigned s, const HbKnown& a) const { return s < a.serial; }
};

// un par SOLAPADO (sa < sb: la identidad del par). Los punteros valen para el paso que lo
// calculo; W3dHitboxForget saca los pares de un objeto que se destruye.
struct HbPair {
    unsigned sa, sb;
    Object* a; Object* b;
    W3dHitboxBase* hbA; W3dHitboxBase* hbB;   // NULL = ese lado es un cuerpo rigido
    Object* ownerA; Object* ownerB;
    bool aVeB, bVeA;                          // cada lado REACCIONA al otro (su filtro)
    int oa, ob;                               // orden en el arbol de a y b (el paso que lo calculo)
};
struct HbLowerPair {
    bool operator()(const HbPair& x, const HbPair& y) const {
        return x.sa < y.sa || (x.sa == y.sa && x.sb < y.sb);
    }
};

// una celda de la grilla ocupada por la entrada idx
struct HbCell {
    int x, y, z, idx;
};

static size_t HashCell(const HbCell& c, size_t nb) {
    const unsigned h = (unsigned)c.x * 73856093u ^ (unsigned)c.y * 19349663u ^ (unsigned)c.z * 83492791u;
    return (size_t)h & (nb - 1);
}

// un par de cuerpos rigidos en contacto (seriales, sa < sb)
struct HbContact {
    unsigned sa, sb;
    int ev;        // indice en los eventos de W3dRigido de ESTE paso (-1 = sostenido sin evento)
    float missing; // segundos SEGUIDOS sin evento (ver HB_GRACE_CONTACT)
    bool operator<(const HbContact& o) const { return sa < o.sa || (sa == o.sa && sb < o.sb); }
};
// estado de un cuerpo este tick (para sostener el contacto de los que duermen)
struct HbBody {
    unsigned serial;
    Object* o;
    bool active, still;
    bool visible;            // el cuerpo y sus ancestros (oculto = no toca a nadie)
    int order;               // indice en W3dRigido: el orden del arbol al dar Play (ver alTocar)
    bool operator<(const HbBody& b) const { return serial < b.serial; }
};

// un aviso a despachar: 'yo' es el hitbox (o el cuerpo) de ESTE lado
struct HbNotice {
    Object* yo;
    W3dHitboxBase* miHb;     // NULL = este lado es un cuerpo
    Object* other;           // el dueno del otro lado
    W3dHitboxBase* suHb;     // NULL = el otro lado es un cuerpo
    int k0, k1, side;        // ORDEN del aviso: el par en el arbol (menor, mayor) y el lado
};
// los avisos de un grupo salen en el orden del ARBOL del par; de un mismo par, primero el lado
// que esta antes en el arbol
struct HbLowerNotice {
    bool operator()(const HbNotice& x, const HbNotice& y) const {
        if (x.k0 != y.k0) return x.k0 < y.k0;
        if (x.k1 != y.k1) return x.k1 < y.k1;
        return x.side < y.side;
    }
};
// un objeto con fisica visto al recorrer el arbol: su posicion en el preorden
struct HbOrderBody {
    unsigned serial;
    int order;
    bool operator<(const HbOrderBody& b) const { return serial < b.serial; }
};

static std::vector<HbEntry>  gEnt;
static std::vector<HbKnown> gKnown;
static std::vector<HbCell>    gCells;
static std::vector<int>        gBig;
static std::vector<int>        gBucket;      // inicio de cada cubeta de la grilla en gOrder
static std::vector<int>        gFull;
static std::vector<int>        gOrder;        // indices de gCells agrupados por cubeta
static std::vector<HbPair>      gPairs;       // los del ULTIMO paso (ordenados)
static std::vector<HbPair>      gNew;
static std::vector<unsigned>   gTouching;     // hitbox con algun par que les importa (ordenado)
static std::vector<HbNotice>    gExit, gEnter, gRemain;
static std::vector<HbContact> gContacts;   // pares de cuerpos en contacto (ordenados)
// un contacto que la fisica deja de reportar un RATO CORTO sigue "tocandose": al aterrizar, la
// correccion posicional (Baumgarte) empuja la caja hacia afuera y hay pasos sin impulso aunque no
// se separo de verdad; sin esta gracia eso era un segundo alTocar (un segundo golpe falso). En
// SEGUNDOS y no en pasos: el juego compilado corre con dt variable. Un rebote de verdad (mas de
// 0,15 s en el aire) si vuelve a avisar.
static const float HB_GRACE_CONTACT = 0.15f;
static std::vector<HbContact> gNewContacts;
static std::vector<HbBody>   gBodies;
static std::vector<HbOrderBody> gOrderBodies;   // los objetos con fisica del arbol (por serial)
static int gPreorder = 0;                       // contador del recorrido del arbol de este tick
// un contacto NUEVO a avisar (alTocar), con su orden en el arbol
struct HbTouch {
    int k0, k1, ev;
    bool operator<(const HbTouch& b) const {
        return k0 < b.k0 || (k0 == b.k0 && (k1 < b.k1 || (k1 == b.k1 && ev < b.ev)));
    }
};
static std::vector<HbTouch>    gTouch;
static W3dHitboxStats gStats;

W3dHitboxStats W3dHitboxGetStats(void) { return gStats; }

void W3dHitboxClear(void) {
    gPairs.clear();
    gNew.clear();
    gTouching.clear();
    gContacts.clear();
    gNewContacts.clear();
    gExit.clear(); gEnter.clear(); gRemain.clear();
    gStats = W3dHitboxStats();
}

void W3dHitboxEndSession(void) {
    W3dHitboxClear();
    // el overlay de debug lo prende un SCRIPT: muere con la partida (si no, el Stop lo dejaba
    // prendido y las cajas salian en el Render y en el proximo archivo que se abra)
    g_w3dHitboxVer = false;
}

// ~Object avisa ACA (ver Objects.cpp): los pares que nombran al que se destruye se olvidan SIN
// evento, asi ninguna consulta (hitboxInside, el color del editor) toca un puntero muerto.
void W3dHitboxForget(Object* o) {
    if (!o || gPairs.empty()) return;
    size_t w = 0;
    for (size_t i = 0; i < gPairs.size(); i++) {
        const HbPair& p = gPairs[i];
        if (p.a == o || p.b == o || p.ownerA == o || p.ownerB == o) continue;
        gPairs[w++] = gPairs[i];
    }
    gPairs.resize(w);
    gTouching.clear();
    for (size_t i = 0; i < gPairs.size(); i++) {
        if (gPairs[i].hbA && gPairs[i].aVeB) gTouching.push_back(gPairs[i].sa);
        if (gPairs[i].hbB && gPairs[i].bVeA) gTouching.push_back(gPairs[i].sb);
    }
    std::sort(gTouching.begin(), gTouching.end());
}

// ---------------------------------------------------------------------------
//  SAT de dos cajas orientadas (el test de W3dRigido::Colision sin el manifold)
// ---------------------------------------------------------------------------
// los tres ejes son ortonormales? (una caja de verdad). Un hitbox que cuelga de un padre con
// escala NO uniforme y esta rotado tiene una matriz de mundo con CIZALLA: sus ejes (las columnas
// normalizadas) no son perpendiculares y la caja es un PARALELEPIPEDO (es lo que dibuja el editor)
static bool OrthonormalAxes(const Vector3* u) {
    return fabsf(u[0].Dot(u[1])) < 1e-4f && fabsf(u[1].Dot(u[2])) < 1e-4f && fabsf(u[0].Dot(u[2])) < 1e-4f;
}

// semiancho de la proyeccion de un paralelepipedo sobre el eje n: cada semieje con SU producto
// punto (vale para ejes no ortogonales)
static float RadiusOver(const Vector3& n, const Vector3* u, const float* h) {
    return h[0] * fabsf(n.Dot(u[0])) + h[1] * fabsf(n.Dot(u[1])) + h[2] * fabsf(n.Dot(u[2]));
}

// SAT GENERAL de dos paralelepipedos: 3 normales de CARA de cada uno (el producto cruz de sus
// otros dos ejes, que NO es el eje mismo cuando hay cizalla) + los 9 cruces de aristas, cada
// eje con el radio de las dos cajas proyectadas de verdad. Con ejes ortonormales da lo mismo que
// el SAT clasico (que es el que se usa en ese caso, bit a bit igual que antes)
static bool OverlapGeneral(const Vector3& d, const Vector3* uA, const float* ha,
                           const Vector3* uB, const float* hb) {
    for (int side = 0; side < 2; side++) {
        const Vector3* u = side ? uB : uA;
        for (int i = 0; i < 3; i++) {
            Vector3 n = Vector3::Cross(u[(i + 1) % 3], u[(i + 2) % 3]);
            const float len = n.Length();
            // caja aplastada (dos ejes paralelos): el eje propio sigue siendo un eje valido
            n = (len > 1e-6f) ? n * (1.0f / len) : u[i];
            if (fabsf(n.Dot(d)) - (RadiusOver(n, uA, ha) + RadiusOver(n, uB, hb)) > 0.0f) return false;
        }
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vector3 axis = Vector3::Cross(uA[i], uB[j]);
            const float len = axis.Length();
            if (len < 1e-4f) continue;           // casi paralelos: lo cubren las caras
            axis = axis * (1.0f / len);
            if (fabsf(axis.Dot(d)) - (RadiusOver(axis, uA, ha) + RadiusOver(axis, uB, hb)) > 0.0f) return false;
        }
    return true;
}

bool W3dObbOverlap(const Vector3& cA, const Vector3* uA, const Vector3& hA,
                   const Vector3& cB, const Vector3* uB, const Vector3& hB) {
    const Vector3 d = cB - cA;
    const float ha[3] = { hA.x, hA.y, hA.z };
    const float hb[3] = { hB.x, hB.y, hB.z };
    // con cizalla el SAT clasico (radio propio = el semieje) SUBESTIMA la caja y pierde solapes
    if (!OrthonormalAxes(uA) || !OrthonormalAxes(uB)) return OverlapGeneral(d, uA, ha, uB, hb);
    float c[3][3], ac[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            c[i][j] = uA[i].Dot(uB[j]);
            ac[i][j] = fabsf(c[i][j]) + 1e-5f;   // epsilon: ejes casi paralelos (el clasico del SAT)
        }
    // caras de A
    for (int i = 0; i < 3; i++) {
        const float r = ha[i] + hb[0] * ac[i][0] + hb[1] * ac[i][1] + hb[2] * ac[i][2];
        if (fabsf(uA[i].Dot(d)) - r > 0.0f) return false;
    }
    // caras de B
    for (int j = 0; j < 3; j++) {
        const float r = hb[j] + ha[0] * ac[0][j] + ha[1] * ac[1][j] + ha[2] * ac[2][j];
        if (fabsf(uB[j].Dot(d)) - r > 0.0f) return false;
    }
    // aristas: cada semieje proyectado con SU dot
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vector3 axis = Vector3::Cross(uA[i], uB[j]);
            const float len = axis.Length();
            if (len < 1e-4f) continue;           // casi paralelos: lo cubren las caras
            axis = axis * (1.0f / len);
            float r = 0.0f;
            for (int k = 0; k < 3; k++)
                r += ha[k] * fabsf(axis.Dot(uA[k])) + hb[k] * fabsf(axis.Dot(uB[k]));
            if (fabsf(axis.Dot(d)) - r > 0.0f) return false;
        }
    return true;
}

// ---------------------------------------------------------------------------
//  recolectar
// ---------------------------------------------------------------------------
// el objeto y TODOS sus ancestros visibles? (oculto = no participa: hitbox, cuerpos y alTocar)
static bool VisibleInTree(const Object* o) {
    for (; o; o = o->Parent)
        if (!o->visible) return false;
    return true;
}

// el FILTRO de un hitbox deja pasar esta etiqueta? (vacio = todo)
static bool Accepts(const W3dHitboxBase* hb, const std::string& otherLabel) {
    return hb->filter.empty() || hb->filter == otherLabel;
}

// la caja alineada a los ejes que envuelve a la orientada (para la poda antes del SAT)
static void ComputeExt(HbEntry& e) {
    const float hh[3] = { e.h.x, e.h.y, e.h.z };
    for (int a = 0; a < 3; a++) {
        float s = 0.0f;
        for (int k = 0; k < 3; k++) s += fabsf(e.u[k][a]) * hh[k];
        e.ext[a] = s;
    }
}

// la caja del hitbox en MUNDO: la matriz de mundo EFECTIVA (constraints incluidos: un hitbox
// pegado a un hueso sigue la animacion) aplicada al centro, y la escala de cada columna a los
// semiejes (la escala de mundo se aplica, como pide el tamano en espacio local)
static void HitboxBox(W3dHitboxBase* hb, HbEntry& e) {
    Matrix4 M;
    hb->GetWorldMatrix(M);
    const Vector3 col[3] = { Vector3(M.m[0], M.m[1], M.m[2]),
                             Vector3(M.m[4], M.m[5], M.m[6]),
                             Vector3(M.m[8], M.m[9], M.m[10]) };
    const Vector3 base[3] = { Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1) };
    float hh[3];
    for (int k = 0; k < 3; k++) {
        const float len = col[k].Length();
        e.u[k] = (len > 1e-8f) ? col[k] * (1.0f / len) : base[k];
        hh[k] = 0.5f * fabsf(hb->size[k]) * len;
        if (hh[k] < 1e-4f) hh[k] = 1e-4f;
    }
    e.h = Vector3(hh[0], hh[1], hh[2]);
    e.c = M * Vector3(hb->center[0], hb->center[1], hb->center[2]);
    e.r = e.h.Length();
    if (!OrthonormalAxes(e.u)) {
        // paralelepipedo (cizalla): la esquina mas lejana del centro es la diagonal MAS LARGA,
        // que puede pasar de |h| (con |h| la esfera y la grilla perdian pares que el SAT acepta)
        const Vector3 a = e.u[0] * hh[0], b = e.u[1] * hh[1], c = e.u[2] * hh[2];
        const Vector3 diag[4] = { a + b + c, a + b - c, a - b + c, a - b - c };
        for (int k = 0; k < 4; k++) {
            const float l = diag[k].Length();
            if (l > e.r) e.r = l;
        }
    }
    ComputeExt(e);
}

static bool gHasDetectors = false;   // algun hitbox participante detecta cuerpos

// recorre el arbol en PREORDEN (el orden del outliner): cada objeto recibe su numero de orden;
// los hitbox quedan conocidos (y participan si estan activos y visibles) y los objetos con fisica
// anotan su orden para cuando entren como cuerpos (AddBodies)
static void CollectRec(Object* o, bool visible) {
    for (size_t i = 0; i < o->Childrens.size(); i++) {
        Object* h = o->Childrens[i];
        if (!h) continue;
        if (h->getType() == ObjectType::ui) continue;   // las interfaces 2D no tienen hitbox
        const int order = gPreorder++;
        const bool vis = visible && h->visible;
        if (h->physics) {
            HbOrderBody oc; oc.serial = h->serial; oc.order = order;
            gOrderBodies.push_back(oc);
        }
        if (h->getType() == ObjectType::hitbox) {
            W3dHitboxBase* hb = static_cast<W3dHitboxBase*>(h);
            HbKnown k; k.serial = h->serial; k.o = h; k.hb = hb; k.order = order;
            gKnown.push_back(k);
            if (vis && hb->active) {
                HbEntry e;
                e.o = h; e.hb = hb; e.serial = h->serial; e.order = order;
                e.owner = W3dHitboxOwner(h);
                HitboxBox(hb, e);
                gEnt.push_back(e);
                if (hb->detectBodies) gHasDetectors = true;
            }
        }
        CollectRec(h, vis);
    }
}

static void CollectBodies() {
    gBodies.clear();
    const int n = W3dRigidCount();
    for (int i = 0; i < n; i++) {
        Object* o = NULL; Vector3 c, u[3], h; bool active = false, still = false;
        if (!W3dRigidBox(i, &o, &c, u, &h, &active, &still) || !o) continue;
        HbBody cu; cu.serial = o->serial; cu.o = o; cu.active = active; cu.still = still;
        cu.visible = VisibleInTree(o);
        // W3dRigidEnsure crea los cuerpos recorriendo el ARBOL (y son de primer nivel): su
        // indice es el orden del arbol al dar Play, igual en el editor y en el juego compilado
        cu.order = i;
        gBodies.push_back(cu);
    }
    std::sort(gBodies.begin(), gBodies.end());
}

// el orden en el arbol de un objeto con fisica (lo anoto CollectRec este tick); si no se vio
// (no deberia), despues de todo el arbol, en el orden de W3dRigido
static int BodyOrder(unsigned serial, int index) {
    HbOrderBody k; k.serial = serial; k.order = 0;
    std::vector<HbOrderBody>::const_iterator it = std::lower_bound(gOrderBodies.begin(), gOrderBodies.end(), k);
    if (it != gOrderBodies.end() && it->serial == serial) return it->order;
    return gPreorder + index;
}

// los cuerpos rigidos: SIEMPRE conocidos (un par hitbox-cuerpo que sale tiene que resolverse
// aunque ya ningun hitbox detecte cuerpos) y al broadphase solo si algun hitbox los detecta
static void AddBodies(bool toBroadphase) {
    const int n = W3dRigidCount();
    if (n > 0) std::sort(gOrderBodies.begin(), gOrderBodies.end());
    for (int i = 0; i < n; i++) {
        Object* o = NULL; Vector3 c, u[3], h; bool active = false, still = false;
        if (!W3dRigidBox(i, &o, &c, u, &h, &active, &still) || !o) continue;
        // un hitbox con fisica cuenta como HITBOX (un serial, una identidad)
        if (o->getType() == ObjectType::hitbox) continue;
        const int order = BodyOrder(o->serial, i);
        HbKnown k; k.serial = o->serial; k.o = o; k.hb = NULL; k.order = order;
        gKnown.push_back(k);
        if (!toBroadphase || !active || !VisibleInTree(o)) continue;   // apagado u oculto: no participa
        HbEntry e;
        e.o = o; e.hb = NULL; e.serial = o->serial; e.owner = o; e.order = order;
        e.c = c; e.u[0] = u[0]; e.u[1] = u[1]; e.u[2] = u[2]; e.h = h;
        e.r = h.Length();
        ComputeExt(e);
        gEnt.push_back(e);
        gStats.bodies++;
    }
}

// ---------------------------------------------------------------------------
//  pares
// ---------------------------------------------------------------------------
static void TestPair(int i, int j) {
    const HbEntry& A = gEnt[i];
    const HbEntry& B = gEnt[j];
    if (!A.hb && !B.hb) return;                       // cuerpo-cuerpo: eso es de W3dRigido
    if (A.o == B.o) return;
    bool aVeB, bVeA;
    if (A.hb && B.hb) {
        // los hitbox del MISMO dueno (cabeza y cuerpo de un enemigo) no se detectan entre si, ni
        // un hitbox con el hitbox que es su dueno
        if (A.owner == B.owner && A.owner != A.o) return;
        if (A.owner == B.o || B.owner == A.o) return;
        aVeB = Accepts(A.hb, B.hb->label);
        bVeA = Accepts(B.hb, A.hb->label);
        if (!aVeB && !bVeA) return;
    } else {
        const HbEntry& H = A.hb ? A : B;            // el hitbox
        const HbEntry& C = A.hb ? B : A;            // el cuerpo
        // los cuerpos no tienen etiqueta: un hitbox con filtro no los ve (y ellos a el tampoco)
        if (!H.hb->detectBodies || !H.hb->filter.empty()) return;
        if (H.owner == C.o) return;                  // el sensor de su propio cuerpo
        aVeB = bVeA = true;
    }
    const Vector3 d = B.c - A.c;
    const float rr = A.r + B.r;
    if (d.LengthSq() > rr * rr) return;               // esferas: ni se acercan
    // cajas alineadas a los ejes: descarta casi todo lo que la esfera deja pasar (el SAT cuesta)
    if (fabsf(d.x) > A.ext[0] + B.ext[0] || fabsf(d.y) > A.ext[1] + B.ext[1] ||
        fabsf(d.z) > A.ext[2] + B.ext[2]) return;
    gStats.tested++;
    if (!W3dObbOverlap(A.c, A.u, A.h, B.c, B.u, B.h)) return;
    const bool aFirst = A.serial < B.serial;
    const HbEntry& P = aFirst ? A : B;
    const HbEntry& Q = aFirst ? B : A;
    HbPair p;
    p.sa = P.serial; p.sb = Q.serial;
    p.a = P.o; p.b = Q.o;
    p.hbA = P.hb; p.hbB = Q.hb;
    p.ownerA = P.owner; p.ownerB = Q.owner;
    p.aVeB = aFirst ? aVeB : bVeA;
    p.bVeA = aFirst ? bVeA : aVeB;
    p.oa = P.order; p.ob = Q.order;
    gNew.push_back(p);
}

static int Floor(float v, float cell) {
    float q = v / cell;
    if (q >  1.0e8f) q =  1.0e8f;
    if (q < -1.0e8f) q = -1.0e8f;
    return (int)floorf(q);
}

static void Broadphase() {
    gCells.clear();
    gBig.clear();
    const int n = (int)gEnt.size();
    if (n < 2) return;
    // celda = el doble del radio MEDIO: una caja tipica ocupa 1..8 celdas
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += gEnt[i].r;
    float cell = (float)(2.0 * sum / (double)n);
    if (cell < 0.05f) cell = 0.05f;
    for (int i = 0; i < n; i++) {
        HbEntry& e = gEnt[i];
        const float cc[3] = { e.c.x, e.c.y, e.c.z };
        int count = 1;
        for (int k = 0; k < 3; k++) {
            e.c0[k] = Floor(cc[k] - e.r, cell);
            e.c1[k] = Floor(cc[k] + e.r, cell);
            int side = e.c1[k] - e.c0[k] + 1;       // (acotado ANTES de multiplicar: long es de 32
            if (side > 65) side = 65;               //  bits en Symbian/Windows)
            count *= side;
            if (count > 64) count = 65;
        }
        // una caja ENORME (una zona de todo el nivel) no se reparte en miles de celdas: se prueba
        // contra todas las demas
        if (count > 64) { gBig.push_back(i); continue; }
        for (int x = e.c0[0]; x <= e.c1[0]; x++)
            for (int y = e.c0[1]; y <= e.c1[1]; y++)
                for (int z = e.c0[2]; z <= e.c1[2]; z++) {
                    HbCell ce; ce.x = x; ce.y = y; ce.z = z; ce.idx = i;
                    gCells.push_back(ce);
                }
    }
    // las celdas a CUBETAS por hash (orden por conteo, O(n): nada de ordenar decenas de miles de
    // celdas por frame). Dos celdas distintas pueden caer en la misma cubeta: adentro se comparan
    // las coordenadas exactas.
    const size_t nc = gCells.size();
    size_t nb = 1;
    while (nb < 2 * nc) nb <<= 1;
    gBucket.assign(nb + 1, 0);
    for (size_t i = 0; i < nc; i++) gBucket[HashCell(gCells[i], nb) + 1]++;
    for (size_t b = 0; b < nb; b++) gBucket[b + 1] += gBucket[b];
    gOrder.resize(nc);
    gFull.assign(gBucket.begin(), gBucket.end() - 1);
    for (size_t i = 0; i < nc; i++) gOrder[gFull[HashCell(gCells[i], nb)]++] = (int)i;
    // cada par se prueba UNA vez: en la PRIMERA celda que comparten (el maximo de los minimos)
    for (size_t b = 0; b < nb; b++) {
        const int start = gBucket[b], end = gBucket[b + 1];
        for (int p = start; p < end; p++)
            for (int q = p + 1; q < end; q++) {
                const HbCell& cp = gCells[gOrder[p]];
                const HbCell& cq = gCells[gOrder[q]];
                if (cp.x != cq.x || cp.y != cq.y || cp.z != cq.z) continue;   // otra celda, misma cubeta
                const HbEntry& A = gEnt[cp.idx];
                const HbEntry& B = gEnt[cq.idx];
                const int mx = A.c0[0] > B.c0[0] ? A.c0[0] : B.c0[0];
                const int my = A.c0[1] > B.c0[1] ? A.c0[1] : B.c0[1];
                const int mz = A.c0[2] > B.c0[2] ? A.c0[2] : B.c0[2];
                if (mx != cp.x || my != cp.y || mz != cp.z) continue;
                // idx menor primero (gEnt esta en el orden del arbol): el par no depende del hash
                if (cp.idx < cq.idx) TestPair(cp.idx, cq.idx); else TestPair(cq.idx, cp.idx);
            }
    }
    // las grandes contra todo (las grandes entre si, una vez)
    for (size_t g = 0; g < gBig.size(); g++) {
        const int i = gBig[g];
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            const bool jBig = std::binary_search(gBig.begin(), gBig.end(), j);
            if (jBig && j < i) continue;
            TestPair(i, j);
        }
    }
}

static const HbKnown* Known(unsigned s) {
    std::vector<HbKnown>::const_iterator it =
        std::lower_bound(gKnown.begin(), gKnown.end(), s, HbKnownLower());
    if (it == gKnown.end() || it->serial != s) return NULL;
    return &*it;
}

// el aviso de UN lado de un par: 'yo' = el objeto de ese lado, 'otro' = el del lado opuesto;
// orderYo / otherOrder = sus posiciones en el arbol (el orden en que sale el aviso)
static void Notify(std::vector<HbNotice>& list, Object* yo, W3dHitboxBase* miHb,
                   Object* otherObj, W3dHitboxBase* suHb, Object* otherOwner,
                   int orderYo, int otherOrder) {
    HbNotice a;
    a.yo = yo; a.miHb = miHb; a.suHb = suHb;
    a.other = otherOwner ? otherOwner : otherObj;
    a.k0 = orderYo < otherOrder ? orderYo : otherOrder;
    a.k1 = orderYo < otherOrder ? otherOrder : orderYo;
    a.side = orderYo < otherOrder ? 0 : 1;
    list.push_back(a);
}

static void Dispatch(std::vector<HbNotice>& list, const char* fn) {
    // la diferencia se armo en el orden de los SERIALES (la identidad); se avisa en el del ARBOL
    std::sort(list.begin(), list.end(), HbLowerNotice());
    for (size_t i = 0; i < list.size(); i++) {
        const HbNotice& a = list[i];
        Object* objs[3];
        objs[0] = a.other;
        objs[1] = a.miHb;
        objs[2] = a.suHb;
        if (a.miHb) {
            // el hitbox y su dueno (si tiene uno de verdad)
            W3dScriptEvent(a.miHb, fn, objs, 3, NULL, 0);
            Object* d = W3dHitboxOwner(a.miHb);
            if (d && d != a.miHb) W3dScriptEvent(d, fn, objs, 3, NULL, 0);
        } else {
            W3dScriptEvent(a.yo, fn, objs, 3, NULL, 0);   // un cuerpo rigido que entro a un hitbox
        }
    }
}

// ---------------------------------------------------------------------------
//  contactos de los cuerpos rigidos -> alTocar (solo cuando EMPIEZAN a tocarse)
// ---------------------------------------------------------------------------
static const HbBody* BodyOf(unsigned s) {
    HbBody k; k.serial = s; k.o = NULL; k.active = k.still = k.visible = false; k.order = 0;
    std::vector<HbBody>::const_iterator it = std::lower_bound(gBodies.begin(), gBodies.end(), k);
    if (it == gBodies.end() || it->serial != s) return NULL;
    return &*it;
}

static void DispatchContacts(float dt) {
    const std::vector<W3dRigidContact>& ev = W3dRigidContacts();
    if (ev.empty() && gContacts.empty()) return;
    CollectBodies();
    gNewContacts.clear();
    for (size_t i = 0; i < ev.size(); i++) {
        if (!ev[i].a || !ev[i].b) continue;
        // un cuerpo OCULTO (o con un ancestro oculto) no participa: la fisica lo sigue simulando,
        // pero no dispara alTocar ni queda "tocandose" (al mostrarse, un contacto nuevo avisa)
        const HbBody* ca = BodyOf(ev[i].a->serial);
        const HbBody* cb = BodyOf(ev[i].b->serial);
        if (!ca || !cb || !ca->visible || !cb->visible) continue;
        HbContact c;
        const unsigned sa = ev[i].a->serial, sb = ev[i].b->serial;
        c.sa = sa < sb ? sa : sb; c.sb = sa < sb ? sb : sa; c.ev = (int)i; c.missing = 0.0f;
        gNewContacts.push_back(c);
    }
    std::sort(gNewContacts.begin(), gNewContacts.end());
    // un par repetido en el paso (no deberia: W3dRigido ya fusiona los sub-pasos) queda una vez
    size_t w = 0;
    for (size_t i = 0; i < gNewContacts.size(); i++)
        if (w == 0 || gNewContacts[w - 1] < gNewContacts[i]) gNewContacts[w++] = gNewContacts[i];
    gNewContacts.resize(w);
    // los NUEVOS avisan; los viejos que faltan en este paso siguen tocandose si la fisica NO los
    // evaluo (dos QUIETOS -dormido/estatico- no se prueban: sin esto un cuerpo que se duerme
    // apoyado en el piso "se separaria" y al despertar volveria a tocar, un alTocar falso) o si
    // faltan hace pocos pasos (HB_GRACE_CONTACT)
    const size_t nNew = gNewContacts.size();
    size_t j = 0;
    for (size_t i = 0; i < gContacts.size(); i++) {
        const HbContact& v = gContacts[i];
        while (j < nNew && gNewContacts[j] < v) j++;
        if (j < nNew && !(v < gNewContacts[j])) continue;   // sigue en este paso
        const HbBody* ca = BodyOf(v.sa);
        const HbBody* cb = BodyOf(v.sb);
        if (!ca || !cb || !ca->active || !cb->active) continue;   // se fue / se apago: se separaron
        if (!ca->visible || !cb->visible) continue;               // se oculto: deja de participar
        HbContact s = v; s.ev = -1;
        if (ca->still && cb->still) { s.missing = 0.0f; gNewContacts.push_back(s); }
        else if (v.missing + dt <= HB_GRACE_CONTACT) { s.missing = v.missing + dt; gNewContacts.push_back(s); }
    }
    // los que EMPIEZAN, en el orden del ARBOL del par (el indice de W3dRigido; el serial es solo
    // la identidad y cambia entre cargas)
    gTouch.clear();
    size_t k = 0;
    for (size_t i = 0; i < nNew; i++) {
        const HbContact& c = gNewContacts[i];
        while (k < gContacts.size() && gContacts[k] < c) k++;
        const bool alreadyWas = (k < gContacts.size() && !(c < gContacts[k]));
        if (alreadyWas || c.ev < 0) continue;
        const HbBody* ca = BodyOf(c.sa);
        const HbBody* cb = BodyOf(c.sb);
        const int oa = ca ? ca->order : 0, ob = cb ? cb->order : 0;
        HbTouch t; t.k0 = oa < ob ? oa : ob; t.k1 = oa < ob ? ob : oa; t.ev = c.ev;
        gTouch.push_back(t);
    }
    std::sort(gTouch.begin(), gTouch.end());
    for (size_t i = 0; i < gTouch.size(); i++) {
        const W3dRigidContact& e = ev[gTouch[i].ev];
        // onTouch(otro, impulso, x,y,z, nx,ny,nz): el punto y la normal van a LUA, en sus ejes (Z arriba): la y y la z
        // del motor se intercambian con W3dLuaAxes (ver W3dScript.h)
        float nums[7];
        nums[0] = e.impulse;
        nums[1] = e.point[0]; nums[2] = e.point[1]; nums[3] = e.point[2];
        nums[4] = e.normal[0]; nums[5] = e.normal[1]; nums[6] = e.normal[2];   // de a hacia b
        W3dLuaAxes(nums[2], nums[3]);
        W3dLuaAxes(nums[5], nums[6]);
        W3dScriptEvent(e.a, "onTouch", e.b, nums, 7);
        nums[4] = -nums[4]; nums[5] = -nums[5]; nums[6] = -nums[6];         // para b: de b hacia a
        W3dScriptEvent(e.b, "onTouch", e.a, nums, 7);
    }
    std::sort(gNewContacts.begin(), gNewContacts.end());
    gContacts.swap(gNewContacts);
    gStats.contacts = (int)gContacts.size();
}

// ---------------------------------------------------------------------------
//  el paso
// ---------------------------------------------------------------------------
void W3dHitboxStep(float dt) {
    gStats = W3dHitboxStats();
    if (dt < 0.0f) dt = 0.0f;
    DispatchContacts(dt);
    gStats.contacts = (int)gContacts.size();
    if (gLive <= 0 && gPairs.empty()) return;        // nadie usa hitbox: costo cero
    gEnt.clear();
    gKnown.clear();
    gNew.clear();
    gOrderBodies.clear();
    gPreorder = 0;
    gHasDetectors = false;
    if (SceneCollection && gLive > 0) CollectRec(SceneCollection, true);
    gStats.hitboxes = (int)gEnt.size();
    AddBodies(gHasDetectors);
    std::sort(gKnown.begin(), gKnown.end(), HbKnownLower());
    Broadphase();
    std::sort(gNew.begin(), gNew.end(), HbLowerPair());
    gStats.pairs = (int)gNew.size();

    // ---- diferencia con el tick anterior, POR LADO -------------------------
    gExit.clear(); gEnter.clear(); gRemain.clear();
    HbLowerPair lower;
    size_t i = 0, j = 0;
    while (i < gPairs.size() || j < gNew.size()) {
        const bool onlyOld = (j >= gNew.size()) || (i < gPairs.size() && lower(gPairs[i], gNew[j]));
        const bool onlyNew = !onlyOld && ((i >= gPairs.size()) || lower(gNew[j], gPairs[i]));
        if (onlyOld) {
            // SALIO: los dos se resuelven por serial contra lo que hay HOY (un destruido no avisa)
            const HbPair& v = gPairs[i];
            const HbKnown* ka = Known(v.sa);
            const HbKnown* kb = Known(v.sb);
            if (ka && kb) {
                Object* dA = ka->hb ? W3dHitboxOwner(ka->o) : ka->o;
                Object* dB = kb->hb ? W3dHitboxOwner(kb->o) : kb->o;
                if (v.aVeB) Notify(gExit, ka->o, ka->hb, kb->o, kb->hb, dB, ka->order, kb->order);
                if (v.bVeA) Notify(gExit, kb->o, kb->hb, ka->o, ka->hb, dA, kb->order, ka->order);
            }
            i++;
        } else if (onlyNew) {
            const HbPair& p = gNew[j];
            if (p.aVeB) Notify(gEnter, p.a, p.hbA, p.b, p.hbB, p.ownerB, p.oa, p.ob);
            if (p.bVeA) Notify(gEnter, p.b, p.hbB, p.a, p.hbA, p.ownerA, p.ob, p.oa);
            j++;
        } else {
            // SIGUE: por lado, porque el filtro pudo cambiar entre ticks (un lado que empieza a
            // reaccionar ENTRA; uno que deja de reaccionar SALE)
            const HbPair& v = gPairs[i];
            const HbPair& p = gNew[j];
            if (p.aVeB) Notify(v.aVeB ? gRemain : gEnter, p.a, p.hbA, p.b, p.hbB, p.ownerB, p.oa, p.ob);
            else if (v.aVeB) Notify(gExit, p.a, p.hbA, p.b, p.hbB, p.ownerB, p.oa, p.ob);
            if (p.bVeA) Notify(v.bVeA ? gRemain : gEnter, p.b, p.hbB, p.a, p.hbA, p.ownerA, p.ob, p.oa);
            else if (v.bVeA) Notify(gExit, p.b, p.hbB, p.a, p.hbA, p.ownerA, p.ob, p.oa);
            i++; j++;
        }
    }
    gPairs.swap(gNew);
    gTouching.clear();
    for (size_t k = 0; k < gPairs.size(); k++) {
        if (gPairs[k].hbA && gPairs[k].aVeB) gTouching.push_back(gPairs[k].sa);
        if (gPairs[k].hbB && gPairs[k].bVeA) gTouching.push_back(gPairs[k].sb);
    }
    std::sort(gTouching.begin(), gTouching.end());
    // los avisos van DESPUES de dejar el estado nuevo: hitboxInside() adentro de un alEntrar ya
    // ve al que acaba de entrar
    Dispatch(gExit, "onExit");
    Dispatch(gEnter, "onEnter");
    Dispatch(gRemain, "onStay");
}

// ---------------------------------------------------------------------------
//  consultas
// ---------------------------------------------------------------------------
bool W3dHitboxTouchesSomething(const Object* hb) {
    if (!hb || gTouching.empty()) return false;
    return std::binary_search(gTouching.begin(), gTouching.end(), hb->serial);
}

// 'o' si es un hitbox; si no, sus hitbox HIJOS directos
static void HitboxesOf(Object* o, std::vector<W3dHitboxBase*>& out) {
    out.clear();
    if (!o) return;
    if (o->getType() == ObjectType::hitbox) { out.push_back(static_cast<W3dHitboxBase*>(o)); return; }
    for (size_t i = 0; i < o->Childrens.size(); i++)
        if (o->Childrens[i] && o->Childrens[i]->getType() == ObjectType::hitbox)
            out.push_back(static_cast<W3dHitboxBase*>(o->Childrens[i]));
}

static bool IsFrom(const std::vector<W3dHitboxBase*>& l, const Object* o) {
    for (size_t i = 0; i < l.size(); i++) if (l[i] == o) return true;
    return false;
}

// un "adentro" con el orden del par en el arbol (el mismo de los avisos)
struct HbInside {
    int k0, k1;
    Object* other;
};
struct HbInsideLower {
    bool operator()(const HbInside& x, const HbInside& y) const {
        return x.k0 < y.k0 || (x.k0 == y.k0 && x.k1 < y.k1);
    }
};

int W3dHitboxInside(Object* o, std::vector<Object*>* out) {
    if (out) out->clear();
    std::vector<W3dHitboxBase*> mine;
    HitboxesOf(o, mine);
    // gPairs esta en el orden de los SERIALES (la identidad): se lista en el del ARBOL, como los
    // eventos, asi el Play del editor y el juego compilado dan la misma tabla
    std::vector<HbInside> l;
    for (size_t k = 0; k < gPairs.size(); k++) {
        const HbPair& p = gPairs[k];
        Object* other = NULL;
        if (p.hbA && p.aVeB && IsFrom(mine, p.a)) other = p.ownerB ? p.ownerB : p.b;
        else if (p.hbB && p.bVeA && IsFrom(mine, p.b)) other = p.ownerA ? p.ownerA : p.a;
        if (!other) continue;
        HbInside d;
        d.k0 = p.oa < p.ob ? p.oa : p.ob; d.k1 = p.oa < p.ob ? p.ob : p.oa; d.other = other;
        l.push_back(d);
    }
    std::sort(l.begin(), l.end(), HbInsideLower());
    int n = 0;
    for (size_t k = 0; k < l.size(); k++) {
        bool repeated = false;
        for (size_t r = 0; r < k; r++) if (l[r].other == l[k].other) { repeated = true; break; }
        if (repeated) continue;
        if (out) out->push_back(l[k].other);
        n++;
    }
    return n;
}

bool W3dHitboxTouching(Object* o, Object* other) {
    if (!other) return false;
    std::vector<W3dHitboxBase*> mine;
    HitboxesOf(o, mine);
    for (size_t k = 0; k < gPairs.size(); k++) {
        const HbPair& p = gPairs[k];
        if (p.hbA && p.aVeB && IsFrom(mine, p.a) && (p.b == other || p.ownerB == other)) return true;
        if (p.hbB && p.bVeA && IsFrom(mine, p.b) && (p.a == other || p.ownerA == other)) return true;
    }
    return false;
}

bool W3dHitboxActive(Object* o) {
    std::vector<W3dHitboxBase*> mine;
    HitboxesOf(o, mine);
    for (size_t i = 0; i < mine.size(); i++) if (mine[i]->active) return true;
    return false;
}

void W3dHitboxSetActive(Object* o, bool active) {
    std::vector<W3dHitboxBase*> mine;
    HitboxesOf(o, mine);
    for (size_t i = 0; i < mine.size(); i++) mine[i]->active = active;
    if (!mine.empty()) W3dScriptRedraw();
}

void W3dHitboxSetSize(Object* o, float x, float y, float z) {
    std::vector<W3dHitboxBase*> mine;
    HitboxesOf(o, mine);
    for (size_t i = 0; i < mine.size(); i++) {
        mine[i]->size[0] = x < 0.0f ? -x : x;
        mine[i]->size[1] = y < 0.0f ? -y : y;
        mine[i]->size[2] = z < 0.0f ? -z : z;
    }
    if (!mine.empty()) W3dScriptRedraw();
}

// ---------------------------------------------------------------------------
//  binds lua
// ---------------------------------------------------------------------------
static Object* ObjArg(lua_State* L, int idx) {
    return W3dScriptParamObject((void*)L, idx);
}

// hitboxActive(o) -> bool
static int LActiveHitbox(lua_State* L) {
    lua_pushboolean(L, W3dHitboxActive(ObjArg(L, 1)) ? 1 : 0);
    return 1;
}
// setHitboxActive(o, bool)
static int LSetActiveHitbox(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (o) W3dHitboxSetActive(o, lua_toboolean(L, 2) != 0);
    return 0;
}
// setHitboxSize(o, x, y, z): el tamanio en los ejes de LUA (z = el alto); al motor va con la y y la z intercambiadas
static int LSetHitboxSize(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    float x = (float)luaL_checknumber(L, 2), y = (float)luaL_checknumber(L, 3), z = (float)luaL_checknumber(L, 4);
    W3dLuaAxes(y, z);
    W3dHitboxSetSize(o, x, y, z);
    return 0;
}
// hitboxInside(o) -> { otro, ... }
static int LHitboxInside(lua_State* L) {
    std::vector<Object*> l;
    W3dHitboxInside(ObjArg(L, 1), &l);
    lua_createtable(L, (int)l.size(), 0);
    for (size_t i = 0; i < l.size(); i++) {
        lua_pushlightuserdata(L, l[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
// hitboxTouching(o, otro) -> bool
static int LHitboxTouching(lua_State* L) {
    lua_pushboolean(L, W3dHitboxTouching(ObjArg(L, 1), ObjArg(L, 2)) ? 1 : 0);
    return 1;
}
// showHitboxes(bool): overlay de debug del juego compilado
static int LShowHitboxes(lua_State* L) {
    g_w3dHitboxVer = lua_toboolean(L, 1) != 0;
    W3dScriptRedraw();
    return 0;
}

void W3dHitboxRegisterBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LActiveHitbox);    lua_setglobal(L, "hitboxActive");
    lua_pushcfunction(L, LSetActiveHitbox); lua_setglobal(L, "setHitboxActive");
    lua_pushcfunction(L, LSetHitboxSize);    lua_setglobal(L, "setHitboxSize");
    lua_pushcfunction(L, LHitboxInside);    lua_setglobal(L, "hitboxInside");
    lua_pushcfunction(L, LHitboxTouching);   lua_setglobal(L, "hitboxTouching");
    lua_pushcfunction(L, LShowHitboxes);     lua_setglobal(L, "showHitboxes");
}
