// ============================================================================
//  W3dHitbox.cpp — ver W3dHitbox.h. Dialecto C++03 (Symbian compila esto).
//  Comentarios en espanol sin acentos.
//
//  Un tick (W3dHitboxPaso):
//    1. CONTACTOS de los cuerpos rigidos (gEventos de W3dRigido) -> alTocar en
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
#include "physics/W3dRigido.h"
#include "objects/Objects.h"
#include "math/Matrix4.h"
#include "script/W3dScript.h"   // W3dScriptEvento / W3dScriptParamObjeto / W3dScriptRedibujar
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
static int gVivos = 0;

W3dHitboxBase::W3dHitboxBase(Object* parent, const std::string& nombre, const Vector3& pos)
    : Object(parent, nombre, pos), activo(true), detectarCuerpos(false) {
    tam[0] = tam[1] = tam[2] = 1.0f;
    centro[0] = centro[1] = centro[2] = 0.0f;
    gVivos++;
}

W3dHitboxBase::~W3dHitboxBase() {
    gVivos--;
}

bool W3dHitboxHay(void) { return gVivos > 0; }

Object* W3dHitboxDuenio(Object* hb) {
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
struct HbEntrada {
    Object* o;               // el hitbox o el cuerpo
    Object* duenio;          // lo que el OTRO lado ve como 'otro'
    W3dHitboxBase* hb;       // NULL = cuerpo rigido
    unsigned serial;
    int orden;               // posicion en el ARBOL (preorden): el orden de los avisos
    Vector3 c, u[3], h;      // centro, ejes y semiejes
    float r;                 // radio de la esfera envolvente
    float ext[3];            // semiejes de la caja ALINEADA a los ejes que la envuelve (poda barata)
    int c0[3], c1[3];        // rango de celdas de la grilla (inclusive)
};

// un objeto CONOCIDO este tick (hitbox en el arbol, participe o no, o cuerpo rigido): con esto
// se resuelve el serial de un par que SALIO sin tocar punteros viejos
struct HbConocido {
    unsigned serial;
    Object* o;
    W3dHitboxBase* hb;       // NULL = cuerpo rigido
    int orden;               // posicion en el arbol ESTE tick (ver HbEntrada)
};
struct HbConocidoMenor {
    bool operator()(const HbConocido& a, const HbConocido& b) const { return a.serial < b.serial; }
    bool operator()(const HbConocido& a, unsigned s) const { return a.serial < s; }
    bool operator()(unsigned s, const HbConocido& a) const { return s < a.serial; }
};

// un par SOLAPADO (sa < sb: la identidad del par). Los punteros valen para el paso que lo
// calculo; W3dHitboxOlvidar saca los pares de un objeto que se destruye.
struct HbPar {
    unsigned sa, sb;
    Object* a; Object* b;
    W3dHitboxBase* hbA; W3dHitboxBase* hbB;   // NULL = ese lado es un cuerpo rigido
    Object* duenioA; Object* duenioB;
    bool aVeB, bVeA;                          // cada lado REACCIONA al otro (su filtro)
    int oa, ob;                               // orden en el arbol de a y b (el paso que lo calculo)
};
struct HbParMenor {
    bool operator()(const HbPar& x, const HbPar& y) const {
        return x.sa < y.sa || (x.sa == y.sa && x.sb < y.sb);
    }
};

// una celda de la grilla ocupada por la entrada idx
struct HbCelda {
    int x, y, z, idx;
};

static size_t HashCelda(const HbCelda& c, size_t nb) {
    const unsigned h = (unsigned)c.x * 73856093u ^ (unsigned)c.y * 19349663u ^ (unsigned)c.z * 83492791u;
    return (size_t)h & (nb - 1);
}

// un par de cuerpos rigidos en contacto (seriales, sa < sb)
struct HbContacto {
    unsigned sa, sb;
    int ev;      // indice en los eventos de W3dRigido de ESTE paso (-1 = sostenido sin evento)
    float falta; // segundos SEGUIDOS sin evento (ver HB_GRACIA_CONTACTO)
    bool operator<(const HbContacto& o) const { return sa < o.sa || (sa == o.sa && sb < o.sb); }
};
// estado de un cuerpo este tick (para sostener el contacto de los que duermen)
struct HbCuerpo {
    unsigned serial;
    Object* o;
    bool activo, quieto;
    bool visible;            // el cuerpo y sus ancestros (oculto = no toca a nadie)
    int orden;               // indice en W3dRigido: el orden del arbol al dar Play (ver alTocar)
    bool operator<(const HbCuerpo& b) const { return serial < b.serial; }
};

// un aviso a despachar: 'yo' es el hitbox (o el cuerpo) de ESTE lado
struct HbAviso {
    Object* yo;
    W3dHitboxBase* miHb;     // NULL = este lado es un cuerpo
    Object* otro;            // el dueno del otro lado
    W3dHitboxBase* suHb;     // NULL = el otro lado es un cuerpo
    int k0, k1, lado;        // ORDEN del aviso: el par en el arbol (menor, mayor) y el lado
};
// los avisos de un grupo salen en el orden del ARBOL del par; de un mismo par, primero el lado
// que esta antes en el arbol
struct HbAvisoMenor {
    bool operator()(const HbAviso& x, const HbAviso& y) const {
        if (x.k0 != y.k0) return x.k0 < y.k0;
        if (x.k1 != y.k1) return x.k1 < y.k1;
        return x.lado < y.lado;
    }
};
// un objeto con fisica visto al recorrer el arbol: su posicion en el preorden
struct HbOrdenCuerpo {
    unsigned serial;
    int orden;
    bool operator<(const HbOrdenCuerpo& b) const { return serial < b.serial; }
};

static std::vector<HbEntrada>  gEnt;
static std::vector<HbConocido> gConocidos;
static std::vector<HbCelda>    gCeldas;
static std::vector<int>        gGrandes;
static std::vector<int>        gCubeta;      // inicio de cada cubeta de la grilla en gOrden
static std::vector<int>        gLlenas;
static std::vector<int>        gOrden;       // indices de gCeldas agrupados por cubeta
static std::vector<HbPar>      gPares;       // los del ULTIMO paso (ordenados)
static std::vector<HbPar>      gNuevos;
static std::vector<unsigned>   gTocando;     // hitbox con algun par que les importa (ordenado)
static std::vector<HbAviso>    gSalen, gEntran, gQuedan;
static std::vector<HbContacto> gContactos;   // pares de cuerpos en contacto (ordenados)
// un contacto que la fisica deja de reportar un RATO CORTO sigue "tocandose": al aterrizar, la
// correccion posicional (Baumgarte) empuja la caja hacia afuera y hay pasos sin impulso aunque no
// se separo de verdad; sin esta gracia eso era un segundo alTocar (un segundo golpe falso). En
// SEGUNDOS y no en pasos: el juego compilado corre con dt variable. Un rebote de verdad (mas de
// 0,15 s en el aire) si vuelve a avisar.
static const float HB_GRACIA_CONTACTO = 0.15f;
static std::vector<HbContacto> gContactosNuevos;
static std::vector<HbCuerpo>   gCuerpos;
static std::vector<HbOrdenCuerpo> gOrdenCuerpos;   // los objetos con fisica del arbol (por serial)
static int gPreorden = 0;                          // contador del recorrido del arbol de este tick
// un contacto NUEVO a avisar (alTocar), con su orden en el arbol
struct HbTocar {
    int k0, k1, ev;
    bool operator<(const HbTocar& b) const {
        return k0 < b.k0 || (k0 == b.k0 && (k1 < b.k1 || (k1 == b.k1 && ev < b.ev)));
    }
};
static std::vector<HbTocar>    gTocar;
static W3dHitboxStats gStats;

W3dHitboxStats W3dHitboxEstadisticas(void) { return gStats; }

void W3dHitboxLimpiar(void) {
    gPares.clear();
    gNuevos.clear();
    gTocando.clear();
    gContactos.clear();
    gContactosNuevos.clear();
    gSalen.clear(); gEntran.clear(); gQuedan.clear();
    gStats = W3dHitboxStats();
}

void W3dHitboxFinPartida(void) {
    W3dHitboxLimpiar();
    // el overlay de debug lo prende un SCRIPT: muere con la partida (si no, el Stop lo dejaba
    // prendido y las cajas salian en el Render y en el proximo archivo que se abra)
    g_w3dHitboxVer = false;
}

// ~Object avisa ACA (ver Objects.cpp): los pares que nombran al que se destruye se olvidan SIN
// evento, asi ninguna consulta (hitboxDentro, el color del editor) toca un puntero muerto.
void W3dHitboxOlvidar(Object* o) {
    if (!o || gPares.empty()) return;
    size_t w = 0;
    for (size_t i = 0; i < gPares.size(); i++) {
        const HbPar& p = gPares[i];
        if (p.a == o || p.b == o || p.duenioA == o || p.duenioB == o) continue;
        gPares[w++] = gPares[i];
    }
    gPares.resize(w);
    gTocando.clear();
    for (size_t i = 0; i < gPares.size(); i++) {
        if (gPares[i].hbA && gPares[i].aVeB) gTocando.push_back(gPares[i].sa);
        if (gPares[i].hbB && gPares[i].bVeA) gTocando.push_back(gPares[i].sb);
    }
    std::sort(gTocando.begin(), gTocando.end());
}

// ---------------------------------------------------------------------------
//  SAT de dos cajas orientadas (el test de W3dRigido::Colision sin el manifold)
// ---------------------------------------------------------------------------
// los tres ejes son ortonormales? (una caja de verdad). Un hitbox que cuelga de un padre con
// escala NO uniforme y esta rotado tiene una matriz de mundo con CIZALLA: sus ejes (las columnas
// normalizadas) no son perpendiculares y la caja es un PARALELEPIPEDO (es lo que dibuja el editor)
static bool EjesOrtonormales(const Vector3* u) {
    return fabsf(u[0].Dot(u[1])) < 1e-4f && fabsf(u[1].Dot(u[2])) < 1e-4f && fabsf(u[0].Dot(u[2])) < 1e-4f;
}

// semiancho de la proyeccion de un paralelepipedo sobre el eje n: cada semieje con SU producto
// punto (vale para ejes no ortogonales)
static float RadioSobre(const Vector3& n, const Vector3* u, const float* h) {
    return h[0] * fabsf(n.Dot(u[0])) + h[1] * fabsf(n.Dot(u[1])) + h[2] * fabsf(n.Dot(u[2]));
}

// SAT GENERAL de dos paralelepipedos: 3 normales de CARA de cada uno (el producto cruz de sus
// otros dos ejes, que NO es el eje mismo cuando hay cizalla) + los 9 cruces de aristas, cada
// eje con el radio de las dos cajas proyectadas de verdad. Con ejes ortonormales da lo mismo que
// el SAT clasico (que es el que se usa en ese caso, bit a bit igual que antes)
static bool SolapanGeneral(const Vector3& d, const Vector3* uA, const float* ha,
                           const Vector3* uB, const float* hb) {
    for (int lado = 0; lado < 2; lado++) {
        const Vector3* u = lado ? uB : uA;
        for (int i = 0; i < 3; i++) {
            Vector3 n = Vector3::Cross(u[(i + 1) % 3], u[(i + 2) % 3]);
            const float len = n.Length();
            // caja aplastada (dos ejes paralelos): el eje propio sigue siendo un eje valido
            n = (len > 1e-6f) ? n * (1.0f / len) : u[i];
            if (fabsf(n.Dot(d)) - (RadioSobre(n, uA, ha) + RadioSobre(n, uB, hb)) > 0.0f) return false;
        }
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vector3 eje = Vector3::Cross(uA[i], uB[j]);
            const float len = eje.Length();
            if (len < 1e-4f) continue;           // casi paralelos: lo cubren las caras
            eje = eje * (1.0f / len);
            if (fabsf(eje.Dot(d)) - (RadioSobre(eje, uA, ha) + RadioSobre(eje, uB, hb)) > 0.0f) return false;
        }
    return true;
}

bool W3dObbSolapan(const Vector3& cA, const Vector3* uA, const Vector3& hA,
                   const Vector3& cB, const Vector3* uB, const Vector3& hB) {
    const Vector3 d = cB - cA;
    const float ha[3] = { hA.x, hA.y, hA.z };
    const float hb[3] = { hB.x, hB.y, hB.z };
    // con cizalla el SAT clasico (radio propio = el semieje) SUBESTIMA la caja y pierde solapes
    if (!EjesOrtonormales(uA) || !EjesOrtonormales(uB)) return SolapanGeneral(d, uA, ha, uB, hb);
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
            Vector3 eje = Vector3::Cross(uA[i], uB[j]);
            const float len = eje.Length();
            if (len < 1e-4f) continue;           // casi paralelos: lo cubren las caras
            eje = eje * (1.0f / len);
            float r = 0.0f;
            for (int k = 0; k < 3; k++)
                r += ha[k] * fabsf(eje.Dot(uA[k])) + hb[k] * fabsf(eje.Dot(uB[k]));
            if (fabsf(eje.Dot(d)) - r > 0.0f) return false;
        }
    return true;
}

// ---------------------------------------------------------------------------
//  recolectar
// ---------------------------------------------------------------------------
// el objeto y TODOS sus ancestros visibles? (oculto = no participa: hitbox, cuerpos y alTocar)
static bool VisibleEnArbol(const Object* o) {
    for (; o; o = o->Parent)
        if (!o->visible) return false;
    return true;
}

// el FILTRO de un hitbox deja pasar esta etiqueta? (vacio = todo)
static bool Acepta(const W3dHitboxBase* hb, const std::string& etiquetaOtro) {
    return hb->filtro.empty() || hb->filtro == etiquetaOtro;
}

// la caja alineada a los ejes que envuelve a la orientada (para la poda antes del SAT)
static void CalcularExt(HbEntrada& e) {
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
static void CajaDeHitbox(W3dHitboxBase* hb, HbEntrada& e) {
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
        hh[k] = 0.5f * fabsf(hb->tam[k]) * len;
        if (hh[k] < 1e-4f) hh[k] = 1e-4f;
    }
    e.h = Vector3(hh[0], hh[1], hh[2]);
    e.c = M * Vector3(hb->centro[0], hb->centro[1], hb->centro[2]);
    e.r = e.h.Length();
    if (!EjesOrtonormales(e.u)) {
        // paralelepipedo (cizalla): la esquina mas lejana del centro es la diagonal MAS LARGA,
        // que puede pasar de |h| (con |h| la esfera y la grilla perdian pares que el SAT acepta)
        const Vector3 a = e.u[0] * hh[0], b = e.u[1] * hh[1], c = e.u[2] * hh[2];
        const Vector3 diag[4] = { a + b + c, a + b - c, a - b + c, a - b - c };
        for (int k = 0; k < 4; k++) {
            const float l = diag[k].Length();
            if (l > e.r) e.r = l;
        }
    }
    CalcularExt(e);
}

static bool gHayDetectores = false;   // algun hitbox participante detecta cuerpos

// recorre el arbol en PREORDEN (el orden del outliner): cada objeto recibe su numero de orden;
// los hitbox quedan conocidos (y participan si estan activos y visibles) y los objetos con fisica
// anotan su orden para cuando entren como cuerpos (AgregarCuerpos)
static void RecolectarRec(Object* o, bool visible) {
    for (size_t i = 0; i < o->Childrens.size(); i++) {
        Object* h = o->Childrens[i];
        if (!h) continue;
        if (h->getType() == ObjectType::ui) continue;   // las interfaces 2D no tienen hitbox
        const int orden = gPreorden++;
        const bool vis = visible && h->visible;
        if (h->fisica) {
            HbOrdenCuerpo oc; oc.serial = h->serial; oc.orden = orden;
            gOrdenCuerpos.push_back(oc);
        }
        if (h->getType() == ObjectType::hitbox) {
            W3dHitboxBase* hb = static_cast<W3dHitboxBase*>(h);
            HbConocido k; k.serial = h->serial; k.o = h; k.hb = hb; k.orden = orden;
            gConocidos.push_back(k);
            if (vis && hb->activo) {
                HbEntrada e;
                e.o = h; e.hb = hb; e.serial = h->serial; e.orden = orden;
                e.duenio = W3dHitboxDuenio(h);
                CajaDeHitbox(hb, e);
                gEnt.push_back(e);
                if (hb->detectarCuerpos) gHayDetectores = true;
            }
        }
        RecolectarRec(h, vis);
    }
}

static void RecolectarCuerpos() {
    gCuerpos.clear();
    const int n = W3dRigidosCantidad();
    for (int i = 0; i < n; i++) {
        Object* o = NULL; Vector3 c, u[3], h; bool activo = false, quieto = false;
        if (!W3dRigidoCaja(i, &o, &c, u, &h, &activo, &quieto) || !o) continue;
        HbCuerpo cu; cu.serial = o->serial; cu.o = o; cu.activo = activo; cu.quieto = quieto;
        cu.visible = VisibleEnArbol(o);
        // W3dRigidosAsegurar crea los cuerpos recorriendo el ARBOL (y son de primer nivel): su
        // indice es el orden del arbol al dar Play, igual en el editor y en el juego compilado
        cu.orden = i;
        gCuerpos.push_back(cu);
    }
    std::sort(gCuerpos.begin(), gCuerpos.end());
}

// el orden en el arbol de un objeto con fisica (lo anoto RecolectarRec este tick); si no se vio
// (no deberia), despues de todo el arbol, en el orden de W3dRigido
static int OrdenDeCuerpo(unsigned serial, int indice) {
    HbOrdenCuerpo k; k.serial = serial; k.orden = 0;
    std::vector<HbOrdenCuerpo>::const_iterator it = std::lower_bound(gOrdenCuerpos.begin(), gOrdenCuerpos.end(), k);
    if (it != gOrdenCuerpos.end() && it->serial == serial) return it->orden;
    return gPreorden + indice;
}

// los cuerpos rigidos: SIEMPRE conocidos (un par hitbox-cuerpo que sale tiene que resolverse
// aunque ya ningun hitbox detecte cuerpos) y al broadphase solo si algun hitbox los detecta
static void AgregarCuerpos(bool alBroadphase) {
    const int n = W3dRigidosCantidad();
    if (n > 0) std::sort(gOrdenCuerpos.begin(), gOrdenCuerpos.end());
    for (int i = 0; i < n; i++) {
        Object* o = NULL; Vector3 c, u[3], h; bool activo = false, quieto = false;
        if (!W3dRigidoCaja(i, &o, &c, u, &h, &activo, &quieto) || !o) continue;
        // un hitbox con fisica cuenta como HITBOX (un serial, una identidad)
        if (o->getType() == ObjectType::hitbox) continue;
        const int orden = OrdenDeCuerpo(o->serial, i);
        HbConocido k; k.serial = o->serial; k.o = o; k.hb = NULL; k.orden = orden;
        gConocidos.push_back(k);
        if (!alBroadphase || !activo || !VisibleEnArbol(o)) continue;   // apagado u oculto: no participa
        HbEntrada e;
        e.o = o; e.hb = NULL; e.serial = o->serial; e.duenio = o; e.orden = orden;
        e.c = c; e.u[0] = u[0]; e.u[1] = u[1]; e.u[2] = u[2]; e.h = h;
        e.r = h.Length();
        CalcularExt(e);
        gEnt.push_back(e);
        gStats.cuerpos++;
    }
}

// ---------------------------------------------------------------------------
//  pares
// ---------------------------------------------------------------------------
static void ProbarPar(int i, int j) {
    const HbEntrada& A = gEnt[i];
    const HbEntrada& B = gEnt[j];
    if (!A.hb && !B.hb) return;                       // cuerpo-cuerpo: eso es de W3dRigido
    if (A.o == B.o) return;
    bool aVeB, bVeA;
    if (A.hb && B.hb) {
        // los hitbox del MISMO dueno (cabeza y cuerpo de un enemigo) no se detectan entre si, ni
        // un hitbox con el hitbox que es su dueno
        if (A.duenio == B.duenio && A.duenio != A.o) return;
        if (A.duenio == B.o || B.duenio == A.o) return;
        aVeB = Acepta(A.hb, B.hb->etiqueta);
        bVeA = Acepta(B.hb, A.hb->etiqueta);
        if (!aVeB && !bVeA) return;
    } else {
        const HbEntrada& H = A.hb ? A : B;            // el hitbox
        const HbEntrada& C = A.hb ? B : A;            // el cuerpo
        // los cuerpos no tienen etiqueta: un hitbox con filtro no los ve (y ellos a el tampoco)
        if (!H.hb->detectarCuerpos || !H.hb->filtro.empty()) return;
        if (H.duenio == C.o) return;                  // el sensor de su propio cuerpo
        aVeB = bVeA = true;
    }
    const Vector3 d = B.c - A.c;
    const float rr = A.r + B.r;
    if (d.LengthSq() > rr * rr) return;               // esferas: ni se acercan
    // cajas alineadas a los ejes: descarta casi todo lo que la esfera deja pasar (el SAT cuesta)
    if (fabsf(d.x) > A.ext[0] + B.ext[0] || fabsf(d.y) > A.ext[1] + B.ext[1] ||
        fabsf(d.z) > A.ext[2] + B.ext[2]) return;
    gStats.probados++;
    if (!W3dObbSolapan(A.c, A.u, A.h, B.c, B.u, B.h)) return;
    const bool aPrimero = A.serial < B.serial;
    const HbEntrada& P = aPrimero ? A : B;
    const HbEntrada& Q = aPrimero ? B : A;
    HbPar p;
    p.sa = P.serial; p.sb = Q.serial;
    p.a = P.o; p.b = Q.o;
    p.hbA = P.hb; p.hbB = Q.hb;
    p.duenioA = P.duenio; p.duenioB = Q.duenio;
    p.aVeB = aPrimero ? aVeB : bVeA;
    p.bVeA = aPrimero ? bVeA : aVeB;
    p.oa = P.orden; p.ob = Q.orden;
    gNuevos.push_back(p);
}

static int Piso(float v, float celda) {
    float q = v / celda;
    if (q >  1.0e8f) q =  1.0e8f;
    if (q < -1.0e8f) q = -1.0e8f;
    return (int)floorf(q);
}

static void Broadphase() {
    gCeldas.clear();
    gGrandes.clear();
    const int n = (int)gEnt.size();
    if (n < 2) return;
    // celda = el doble del radio MEDIO: una caja tipica ocupa 1..8 celdas
    double suma = 0.0;
    for (int i = 0; i < n; i++) suma += gEnt[i].r;
    float celda = (float)(2.0 * suma / (double)n);
    if (celda < 0.05f) celda = 0.05f;
    for (int i = 0; i < n; i++) {
        HbEntrada& e = gEnt[i];
        const float cc[3] = { e.c.x, e.c.y, e.c.z };
        int cuantas = 1;
        for (int k = 0; k < 3; k++) {
            e.c0[k] = Piso(cc[k] - e.r, celda);
            e.c1[k] = Piso(cc[k] + e.r, celda);
            int lado = e.c1[k] - e.c0[k] + 1;       // (acotado ANTES de multiplicar: long es de 32
            if (lado > 65) lado = 65;               //  bits en Symbian/Windows)
            cuantas *= lado;
            if (cuantas > 64) cuantas = 65;
        }
        // una caja ENORME (una zona de todo el nivel) no se reparte en miles de celdas: se prueba
        // contra todas las demas
        if (cuantas > 64) { gGrandes.push_back(i); continue; }
        for (int x = e.c0[0]; x <= e.c1[0]; x++)
            for (int y = e.c0[1]; y <= e.c1[1]; y++)
                for (int z = e.c0[2]; z <= e.c1[2]; z++) {
                    HbCelda ce; ce.x = x; ce.y = y; ce.z = z; ce.idx = i;
                    gCeldas.push_back(ce);
                }
    }
    // las celdas a CUBETAS por hash (orden por conteo, O(n): nada de ordenar decenas de miles de
    // celdas por frame). Dos celdas distintas pueden caer en la misma cubeta: adentro se comparan
    // las coordenadas exactas.
    const size_t nc = gCeldas.size();
    size_t nb = 1;
    while (nb < 2 * nc) nb <<= 1;
    gCubeta.assign(nb + 1, 0);
    for (size_t i = 0; i < nc; i++) gCubeta[HashCelda(gCeldas[i], nb) + 1]++;
    for (size_t b = 0; b < nb; b++) gCubeta[b + 1] += gCubeta[b];
    gOrden.resize(nc);
    gLlenas.assign(gCubeta.begin(), gCubeta.end() - 1);
    for (size_t i = 0; i < nc; i++) gOrden[gLlenas[HashCelda(gCeldas[i], nb)]++] = (int)i;
    // cada par se prueba UNA vez: en la PRIMERA celda que comparten (el maximo de los minimos)
    for (size_t b = 0; b < nb; b++) {
        const int ini = gCubeta[b], fin = gCubeta[b + 1];
        for (int p = ini; p < fin; p++)
            for (int q = p + 1; q < fin; q++) {
                const HbCelda& cp = gCeldas[gOrden[p]];
                const HbCelda& cq = gCeldas[gOrden[q]];
                if (cp.x != cq.x || cp.y != cq.y || cp.z != cq.z) continue;   // otra celda, misma cubeta
                const HbEntrada& A = gEnt[cp.idx];
                const HbEntrada& B = gEnt[cq.idx];
                const int mx = A.c0[0] > B.c0[0] ? A.c0[0] : B.c0[0];
                const int my = A.c0[1] > B.c0[1] ? A.c0[1] : B.c0[1];
                const int mz = A.c0[2] > B.c0[2] ? A.c0[2] : B.c0[2];
                if (mx != cp.x || my != cp.y || mz != cp.z) continue;
                // idx menor primero (gEnt esta en el orden del arbol): el par no depende del hash
                if (cp.idx < cq.idx) ProbarPar(cp.idx, cq.idx); else ProbarPar(cq.idx, cp.idx);
            }
    }
    // las grandes contra todo (las grandes entre si, una vez)
    for (size_t g = 0; g < gGrandes.size(); g++) {
        const int i = gGrandes[g];
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            const bool jGrande = std::binary_search(gGrandes.begin(), gGrandes.end(), j);
            if (jGrande && j < i) continue;
            ProbarPar(i, j);
        }
    }
}

static const HbConocido* Conocido(unsigned s) {
    std::vector<HbConocido>::const_iterator it =
        std::lower_bound(gConocidos.begin(), gConocidos.end(), s, HbConocidoMenor());
    if (it == gConocidos.end() || it->serial != s) return NULL;
    return &*it;
}

// el aviso de UN lado de un par: 'yo' = el objeto de ese lado, 'otro' = el del lado opuesto;
// ordenYo / ordenOtro = sus posiciones en el arbol (el orden en que sale el aviso)
static void Avisar(std::vector<HbAviso>& lista, Object* yo, W3dHitboxBase* miHb,
                   Object* otroObj, W3dHitboxBase* suHb, Object* otroDuenio,
                   int ordenYo, int ordenOtro) {
    HbAviso a;
    a.yo = yo; a.miHb = miHb; a.suHb = suHb;
    a.otro = otroDuenio ? otroDuenio : otroObj;
    a.k0 = ordenYo < ordenOtro ? ordenYo : ordenOtro;
    a.k1 = ordenYo < ordenOtro ? ordenOtro : ordenYo;
    a.lado = ordenYo < ordenOtro ? 0 : 1;
    lista.push_back(a);
}

static void Despachar(std::vector<HbAviso>& lista, const char* fn) {
    // la diferencia se armo en el orden de los SERIALES (la identidad); se avisa en el del ARBOL
    std::sort(lista.begin(), lista.end(), HbAvisoMenor());
    for (size_t i = 0; i < lista.size(); i++) {
        const HbAviso& a = lista[i];
        Object* objs[3];
        objs[0] = a.otro;
        objs[1] = a.miHb;
        objs[2] = a.suHb;
        if (a.miHb) {
            // el hitbox y su dueno (si tiene uno de verdad)
            W3dScriptEvento(a.miHb, fn, objs, 3, NULL, 0);
            Object* d = W3dHitboxDuenio(a.miHb);
            if (d && d != a.miHb) W3dScriptEvento(d, fn, objs, 3, NULL, 0);
        } else {
            W3dScriptEvento(a.yo, fn, objs, 3, NULL, 0);   // un cuerpo rigido que entro a un hitbox
        }
    }
}

// ---------------------------------------------------------------------------
//  contactos de los cuerpos rigidos -> alTocar (solo cuando EMPIEZAN a tocarse)
// ---------------------------------------------------------------------------
static const HbCuerpo* CuerpoDe(unsigned s) {
    HbCuerpo k; k.serial = s; k.o = NULL; k.activo = k.quieto = k.visible = false; k.orden = 0;
    std::vector<HbCuerpo>::const_iterator it = std::lower_bound(gCuerpos.begin(), gCuerpos.end(), k);
    if (it == gCuerpos.end() || it->serial != s) return NULL;
    return &*it;
}

static void DespacharContactos(float dt) {
    const std::vector<W3dRigidoContacto>& ev = W3dRigidosContactos();
    if (ev.empty() && gContactos.empty()) return;
    RecolectarCuerpos();
    gContactosNuevos.clear();
    for (size_t i = 0; i < ev.size(); i++) {
        if (!ev[i].a || !ev[i].b) continue;
        // un cuerpo OCULTO (o con un ancestro oculto) no participa: la fisica lo sigue simulando,
        // pero no dispara alTocar ni queda "tocandose" (al mostrarse, un contacto nuevo avisa)
        const HbCuerpo* ca = CuerpoDe(ev[i].a->serial);
        const HbCuerpo* cb = CuerpoDe(ev[i].b->serial);
        if (!ca || !cb || !ca->visible || !cb->visible) continue;
        HbContacto c;
        const unsigned sa = ev[i].a->serial, sb = ev[i].b->serial;
        c.sa = sa < sb ? sa : sb; c.sb = sa < sb ? sb : sa; c.ev = (int)i; c.falta = 0.0f;
        gContactosNuevos.push_back(c);
    }
    std::sort(gContactosNuevos.begin(), gContactosNuevos.end());
    // un par repetido en el paso (no deberia: W3dRigido ya fusiona los sub-pasos) queda una vez
    size_t w = 0;
    for (size_t i = 0; i < gContactosNuevos.size(); i++)
        if (w == 0 || gContactosNuevos[w - 1] < gContactosNuevos[i]) gContactosNuevos[w++] = gContactosNuevos[i];
    gContactosNuevos.resize(w);
    // los NUEVOS avisan; los viejos que faltan en este paso siguen tocandose si la fisica NO los
    // evaluo (dos QUIETOS -dormido/estatico- no se prueban: sin esto un cuerpo que se duerme
    // apoyado en el piso "se separaria" y al despertar volveria a tocar, un alTocar falso) o si
    // faltan hace pocos pasos (HB_GRACIA_CONTACTO)
    const size_t nNuevos = gContactosNuevos.size();
    size_t j = 0;
    for (size_t i = 0; i < gContactos.size(); i++) {
        const HbContacto& v = gContactos[i];
        while (j < nNuevos && gContactosNuevos[j] < v) j++;
        if (j < nNuevos && !(v < gContactosNuevos[j])) continue;   // sigue en este paso
        const HbCuerpo* ca = CuerpoDe(v.sa);
        const HbCuerpo* cb = CuerpoDe(v.sb);
        if (!ca || !cb || !ca->activo || !cb->activo) continue;   // se fue / se apago: se separaron
        if (!ca->visible || !cb->visible) continue;               // se oculto: deja de participar
        HbContacto s = v; s.ev = -1;
        if (ca->quieto && cb->quieto) { s.falta = 0.0f; gContactosNuevos.push_back(s); }
        else if (v.falta + dt <= HB_GRACIA_CONTACTO) { s.falta = v.falta + dt; gContactosNuevos.push_back(s); }
    }
    // los que EMPIEZAN, en el orden del ARBOL del par (el indice de W3dRigido; el serial es solo
    // la identidad y cambia entre cargas)
    gTocar.clear();
    size_t k = 0;
    for (size_t i = 0; i < nNuevos; i++) {
        const HbContacto& c = gContactosNuevos[i];
        while (k < gContactos.size() && gContactos[k] < c) k++;
        const bool yaEstaba = (k < gContactos.size() && !(c < gContactos[k]));
        if (yaEstaba || c.ev < 0) continue;
        const HbCuerpo* ca = CuerpoDe(c.sa);
        const HbCuerpo* cb = CuerpoDe(c.sb);
        const int oa = ca ? ca->orden : 0, ob = cb ? cb->orden : 0;
        HbTocar t; t.k0 = oa < ob ? oa : ob; t.k1 = oa < ob ? ob : oa; t.ev = c.ev;
        gTocar.push_back(t);
    }
    std::sort(gTocar.begin(), gTocar.end());
    for (size_t i = 0; i < gTocar.size(); i++) {
        const W3dRigidoContacto& e = ev[gTocar[i].ev];
        float nums[7];
        nums[0] = e.impulso;
        nums[1] = e.punto[0]; nums[2] = e.punto[1]; nums[3] = e.punto[2];
        nums[4] = e.normal[0]; nums[5] = e.normal[1]; nums[6] = e.normal[2];   // de a hacia b
        W3dScriptEvento(e.a, "alTocar", e.b, nums, 7);
        nums[4] = -e.normal[0]; nums[5] = -e.normal[1]; nums[6] = -e.normal[2];
        W3dScriptEvento(e.b, "alTocar", e.a, nums, 7);
    }
    std::sort(gContactosNuevos.begin(), gContactosNuevos.end());
    gContactos.swap(gContactosNuevos);
    gStats.contactos = (int)gContactos.size();
}

// ---------------------------------------------------------------------------
//  el paso
// ---------------------------------------------------------------------------
void W3dHitboxPaso(float dt) {
    gStats = W3dHitboxStats();
    if (dt < 0.0f) dt = 0.0f;
    DespacharContactos(dt);
    gStats.contactos = (int)gContactos.size();
    if (gVivos <= 0 && gPares.empty()) return;        // nadie usa hitbox: costo cero
    gEnt.clear();
    gConocidos.clear();
    gNuevos.clear();
    gOrdenCuerpos.clear();
    gPreorden = 0;
    gHayDetectores = false;
    if (SceneCollection && gVivos > 0) RecolectarRec(SceneCollection, true);
    gStats.hitboxes = (int)gEnt.size();
    AgregarCuerpos(gHayDetectores);
    std::sort(gConocidos.begin(), gConocidos.end(), HbConocidoMenor());
    Broadphase();
    std::sort(gNuevos.begin(), gNuevos.end(), HbParMenor());
    gStats.pares = (int)gNuevos.size();

    // ---- diferencia con el tick anterior, POR LADO -------------------------
    gSalen.clear(); gEntran.clear(); gQuedan.clear();
    HbParMenor menor;
    size_t i = 0, j = 0;
    while (i < gPares.size() || j < gNuevos.size()) {
        const bool soloViejo = (j >= gNuevos.size()) || (i < gPares.size() && menor(gPares[i], gNuevos[j]));
        const bool soloNuevo = !soloViejo && ((i >= gPares.size()) || menor(gNuevos[j], gPares[i]));
        if (soloViejo) {
            // SALIO: los dos se resuelven por serial contra lo que hay HOY (un destruido no avisa)
            const HbPar& v = gPares[i];
            const HbConocido* ka = Conocido(v.sa);
            const HbConocido* kb = Conocido(v.sb);
            if (ka && kb) {
                Object* dA = ka->hb ? W3dHitboxDuenio(ka->o) : ka->o;
                Object* dB = kb->hb ? W3dHitboxDuenio(kb->o) : kb->o;
                if (v.aVeB) Avisar(gSalen, ka->o, ka->hb, kb->o, kb->hb, dB, ka->orden, kb->orden);
                if (v.bVeA) Avisar(gSalen, kb->o, kb->hb, ka->o, ka->hb, dA, kb->orden, ka->orden);
            }
            i++;
        } else if (soloNuevo) {
            const HbPar& p = gNuevos[j];
            if (p.aVeB) Avisar(gEntran, p.a, p.hbA, p.b, p.hbB, p.duenioB, p.oa, p.ob);
            if (p.bVeA) Avisar(gEntran, p.b, p.hbB, p.a, p.hbA, p.duenioA, p.ob, p.oa);
            j++;
        } else {
            // SIGUE: por lado, porque el filtro pudo cambiar entre ticks (un lado que empieza a
            // reaccionar ENTRA; uno que deja de reaccionar SALE)
            const HbPar& v = gPares[i];
            const HbPar& p = gNuevos[j];
            if (p.aVeB) Avisar(v.aVeB ? gQuedan : gEntran, p.a, p.hbA, p.b, p.hbB, p.duenioB, p.oa, p.ob);
            else if (v.aVeB) Avisar(gSalen, p.a, p.hbA, p.b, p.hbB, p.duenioB, p.oa, p.ob);
            if (p.bVeA) Avisar(v.bVeA ? gQuedan : gEntran, p.b, p.hbB, p.a, p.hbA, p.duenioA, p.ob, p.oa);
            else if (v.bVeA) Avisar(gSalen, p.b, p.hbB, p.a, p.hbA, p.duenioA, p.ob, p.oa);
            i++; j++;
        }
    }
    gPares.swap(gNuevos);
    gTocando.clear();
    for (size_t k = 0; k < gPares.size(); k++) {
        if (gPares[k].hbA && gPares[k].aVeB) gTocando.push_back(gPares[k].sa);
        if (gPares[k].hbB && gPares[k].bVeA) gTocando.push_back(gPares[k].sb);
    }
    std::sort(gTocando.begin(), gTocando.end());
    // los avisos van DESPUES de dejar el estado nuevo: hitboxDentro() adentro de un alEntrar ya
    // ve al que acaba de entrar
    Despachar(gSalen, "alSalir");
    Despachar(gEntran, "alEntrar");
    Despachar(gQuedan, "alQuedarse");
}

// ---------------------------------------------------------------------------
//  consultas
// ---------------------------------------------------------------------------
bool W3dHitboxTocaAlgo(const Object* hb) {
    if (!hb || gTocando.empty()) return false;
    return std::binary_search(gTocando.begin(), gTocando.end(), hb->serial);
}

// 'o' si es un hitbox; si no, sus hitbox HIJOS directos
static void HitboxesDe(Object* o, std::vector<W3dHitboxBase*>& out) {
    out.clear();
    if (!o) return;
    if (o->getType() == ObjectType::hitbox) { out.push_back(static_cast<W3dHitboxBase*>(o)); return; }
    for (size_t i = 0; i < o->Childrens.size(); i++)
        if (o->Childrens[i] && o->Childrens[i]->getType() == ObjectType::hitbox)
            out.push_back(static_cast<W3dHitboxBase*>(o->Childrens[i]));
}

static bool EsDe(const std::vector<W3dHitboxBase*>& l, const Object* o) {
    for (size_t i = 0; i < l.size(); i++) if (l[i] == o) return true;
    return false;
}

// un "adentro" con el orden del par en el arbol (el mismo de los avisos)
struct HbDentro {
    int k0, k1;
    Object* otro;
};
struct HbDentroMenor {
    bool operator()(const HbDentro& x, const HbDentro& y) const {
        return x.k0 < y.k0 || (x.k0 == y.k0 && x.k1 < y.k1);
    }
};

int W3dHitboxDentro(Object* o, std::vector<Object*>* out) {
    if (out) out->clear();
    std::vector<W3dHitboxBase*> mios;
    HitboxesDe(o, mios);
    // gPares esta en el orden de los SERIALES (la identidad): se lista en el del ARBOL, como los
    // eventos, asi el Play del editor y el juego compilado dan la misma tabla
    std::vector<HbDentro> l;
    for (size_t k = 0; k < gPares.size(); k++) {
        const HbPar& p = gPares[k];
        Object* otro = NULL;
        if (p.hbA && p.aVeB && EsDe(mios, p.a)) otro = p.duenioB ? p.duenioB : p.b;
        else if (p.hbB && p.bVeA && EsDe(mios, p.b)) otro = p.duenioA ? p.duenioA : p.a;
        if (!otro) continue;
        HbDentro d;
        d.k0 = p.oa < p.ob ? p.oa : p.ob; d.k1 = p.oa < p.ob ? p.ob : p.oa; d.otro = otro;
        l.push_back(d);
    }
    std::sort(l.begin(), l.end(), HbDentroMenor());
    int n = 0;
    for (size_t k = 0; k < l.size(); k++) {
        bool repetido = false;
        for (size_t r = 0; r < k; r++) if (l[r].otro == l[k].otro) { repetido = true; break; }
        if (repetido) continue;
        if (out) out->push_back(l[k].otro);
        n++;
    }
    return n;
}

bool W3dHitboxTocando(Object* o, Object* otro) {
    if (!otro) return false;
    std::vector<W3dHitboxBase*> mios;
    HitboxesDe(o, mios);
    for (size_t k = 0; k < gPares.size(); k++) {
        const HbPar& p = gPares[k];
        if (p.hbA && p.aVeB && EsDe(mios, p.a) && (p.b == otro || p.duenioB == otro)) return true;
        if (p.hbB && p.bVeA && EsDe(mios, p.b) && (p.a == otro || p.duenioA == otro)) return true;
    }
    return false;
}

bool W3dHitboxActivo(Object* o) {
    std::vector<W3dHitboxBase*> mios;
    HitboxesDe(o, mios);
    for (size_t i = 0; i < mios.size(); i++) if (mios[i]->activo) return true;
    return false;
}

void W3dHitboxSetActivo(Object* o, bool activo) {
    std::vector<W3dHitboxBase*> mios;
    HitboxesDe(o, mios);
    for (size_t i = 0; i < mios.size(); i++) mios[i]->activo = activo;
    if (!mios.empty()) W3dScriptRedibujar();
}

void W3dHitboxSetTam(Object* o, float x, float y, float z) {
    std::vector<W3dHitboxBase*> mios;
    HitboxesDe(o, mios);
    for (size_t i = 0; i < mios.size(); i++) {
        mios[i]->tam[0] = x < 0.0f ? -x : x;
        mios[i]->tam[1] = y < 0.0f ? -y : y;
        mios[i]->tam[2] = z < 0.0f ? -z : z;
    }
    if (!mios.empty()) W3dScriptRedibujar();
}

// ---------------------------------------------------------------------------
//  binds lua
// ---------------------------------------------------------------------------
static Object* ObjArg(lua_State* L, int idx) {
    return W3dScriptParamObjeto((void*)L, idx);
}

// hitboxActivo(o) -> bool
static int LHitboxActivo(lua_State* L) {
    lua_pushboolean(L, W3dHitboxActivo(ObjArg(L, 1)) ? 1 : 0);
    return 1;
}
// setHitboxActivo(o, bool)
static int LSetHitboxActivo(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (o) W3dHitboxSetActivo(o, lua_toboolean(L, 2) != 0);
    return 0;
}
// setHitboxTam(o, x, y, z)
static int LSetHitboxTam(lua_State* L) {
    Object* o = ObjArg(L, 1);
    if (!o) return 0;
    W3dHitboxSetTam(o, (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
                    (float)luaL_checknumber(L, 4));
    return 0;
}
// hitboxDentro(o) -> { otro, ... }
static int LHitboxDentro(lua_State* L) {
    std::vector<Object*> l;
    W3dHitboxDentro(ObjArg(L, 1), &l);
    lua_createtable(L, (int)l.size(), 0);
    for (size_t i = 0; i < l.size(); i++) {
        lua_pushlightuserdata(L, l[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
// hitboxTocando(o, otro) -> bool
static int LHitboxTocando(lua_State* L) {
    lua_pushboolean(L, W3dHitboxTocando(ObjArg(L, 1), ObjArg(L, 2)) ? 1 : 0);
    return 1;
}
// verHitboxes(bool): overlay de debug del juego compilado
static int LVerHitboxes(lua_State* L) {
    g_w3dHitboxVer = lua_toboolean(L, 1) != 0;
    W3dScriptRedibujar();
    return 0;
}

void W3dHitboxRegistrarBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LHitboxActivo);    lua_setglobal(L, "hitboxActivo");
    lua_pushcfunction(L, LSetHitboxActivo); lua_setglobal(L, "setHitboxActivo");
    lua_pushcfunction(L, LSetHitboxTam);    lua_setglobal(L, "setHitboxTam");
    lua_pushcfunction(L, LHitboxDentro);    lua_setglobal(L, "hitboxDentro");
    lua_pushcfunction(L, LHitboxTocando);   lua_setglobal(L, "hitboxTocando");
    lua_pushcfunction(L, LVerHitboxes);     lua_setglobal(L, "verHitboxes");
}
