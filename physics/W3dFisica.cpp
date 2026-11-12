// ============================================================================
//  W3dFisica.cpp — ver W3dFisica.h. Dialecto C++03 (Symbian compila esto).
//  Comentarios en espanol sin acentos.
// ============================================================================
#include "physics/W3dFisica.h"
#include "objects/Objects.h"
#include "script/W3dScript.h"   // W3dScriptParamObjeto (binds) + W3dScriptRedibujar (editor event-driven)
#include <vector>
#include <math.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ---------------------------------------------------------------------------
//  CUERPO: el unico estado de la fisica. Un slot por objeto que se mueve (o al
//  que le fijaron una caja). Es un vector chico y contiguo a proposito: la
//  busqueda lineal sobre 2..10 elementos es mas barata que un mapa y no aloca
//  nada por frame (Symbian agradece).
//  OJO: los W3dCuerpo* se INVALIDAN al crear otro (push_back). Nunca guardar dos
//  punteros a la vez con un BuscarOCrear en el medio.
// ---------------------------------------------------------------------------
struct W3dCuerpo {
    Object* o;
    float v[3];        // velocidad por eje (px/seg de lienzo en 2D, unidades/seg en 3D)
    float caja[3];     // caja de colision propia (ancho, alto, profundo); solo si cajaProp
    bool  cajaProp;
};

static std::vector<W3dCuerpo> gCuerpos;
static W3dFisicaLienzoFn gLienzo = NULL;
static W3dFisicaTam2DFn  gTam2D  = NULL;
static W3dFisicaCajaUIFn gCajaUI = NULL;
static W3dFisicaCrudaFn  gCruda  = NULL;

void W3dFisicaSetAdaptador2D(W3dFisicaLienzoFn lienzo, W3dFisicaTam2DFn tam2d,
                             W3dFisicaCajaUIFn cajaui, W3dFisicaCrudaFn cruda) {
    gLienzo = lienzo; gTam2D = tam2d; gCajaUI = cajaui; gCruda = cruda;
}

static W3dCuerpo* Buscar(Object* o) {
    if (!o) return NULL;
    for (size_t i = 0; i < gCuerpos.size(); i++)
        if (gCuerpos[i].o == o) return &gCuerpos[i];
    return NULL;
}
static W3dCuerpo* BuscarOCrear(Object* o) {
    if (!o) return NULL;
    W3dCuerpo* c = Buscar(o);
    if (c) return c;
    W3dCuerpo n;
    n.o = o;
    n.v[0] = n.v[1] = n.v[2] = 0.0f;
    n.caja[0] = n.caja[1] = n.caja[2] = 0.0f;
    n.cajaProp = false;
    gCuerpos.push_back(n);
    return &gCuerpos[gCuerpos.size() - 1];
}

bool W3dFisicaTiene(Object* o) { return Buscar(o) != NULL; }
bool W3dFisicaHay(void) { return !gCuerpos.empty(); }
void W3dFisicaLimpiar(void) { gCuerpos.clear(); }
void W3dFisicaOlvidar(Object* o) {
    for (size_t i = 0; i < gCuerpos.size(); i++)
        if (gCuerpos[i].o == o) { gCuerpos.erase(gCuerpos.begin() + i); return; }
}

// ---------------------------------------------------------------------------
//  ESPACIO de cada objeto (ver la cabecera): 2D = px de lienzo, 3D = unidades.
// ---------------------------------------------------------------------------
// true si el objeto es un elemento de la UI 2D del juego (su pos esta guardada
// como FRACCION del lienzo). Sin adaptador registrado no hay 2D posible.
static bool Es2D(Object* o) {
    if (!o || !gLienzo) return false;
    switch (o->getType().v) {
        case ObjectType::ui:
        case ObjectType::texto2d:
        case ObjectType::imagen2d:
        case ObjectType::rect2d:
        case ObjectType::cont2d:
        case ObjectType::slice9:
        case ObjectType::boton2d:
        case ObjectType::expandir2d:
        case ObjectType::video2d:
            return true;
        default:
            return false;
    }
}
static void Lienzo(float* w, float* h) {
    *w = 1.0f; *h = 1.0f;
    if (gLienzo) gLienzo(w, h);
    if (*w < 1.0f) *w = 1.0f;
    if (*h < 1.0f) *h = 1.0f;
}
// CENTRO del objeto en su espacio (c[0..2]) + MEDIAS extensiones de su caja (h[0..2]).
// 2D: centro en px de lienzo y caja = el rect del elemento (lo mismo que compara chocan()).
// 3D: centro = pos local y caja = cubo de RadioFoco() (bounding de la malla). En los dos
// casos, caja(obj, w, h, d) desde lua pisa el default.
static bool CajaDe(Object* o, float* c, float* h) {
    if (!o) return false;
    bool dosd = Es2D(o);
    if (dosd) {
        float lw, lh; Lienzo(&lw, &lh);
        c[0] = o->pos.x * lw; c[1] = o->pos.y * lh; c[2] = o->pos.z;
    } else {
        c[0] = o->pos.x; c[1] = o->pos.y; c[2] = o->pos.z;
    }
    W3dCuerpo* cu = Buscar(o);
    if (cu && cu->cajaProp) {
        h[0] = cu->caja[0] * 0.5f; h[1] = cu->caja[1] * 0.5f; h[2] = cu->caja[2] * 0.5f;
    } else if (dosd) {
        float aw = 0.0f, ah = 0.0f;
        if (gTam2D) gTam2D(o, &aw, &ah);
        h[0] = aw * 0.5f; h[1] = ah * 0.5f; h[2] = 0.0f;
    } else {
        float r = o->RadioFoco();
        h[0] = r; h[1] = r; h[2] = r;
    }
    return true;
}
// Caja de un objeto usado como AREA (la cancha de rebotarEn/rebotarDentro). A
// diferencia de CajaDe, si es un objeto 2D que el layout ya resolvio se usa ESE
// rect (el mismo que devuelve cajaUI en lua): un area es un widget acomodado por
// el .w3dui, y su pos cruda no es donde termino dibujado. Si no hay adaptador o
// todavia no se dibujo, cae a la caja normal.
// DECISION: el area usa el resuelto SIEMPRE (aunque su pos cruda sea
// autoritativa): un area no se mueve por script, asi que no hay lag posible, y
// cambiarle la fuente moveria las paredes de juegos que ya funcionan.
static bool CajaArea(Object* o, float* c, float* h) {
    if (!o) return false;
    if (gCajaUI && Es2D(o)) {
        float cx = 0.0f, cy = 0.0f, aw = 0.0f, ah = 0.0f;
        if (gCajaUI(o, &cx, &cy, &aw, &ah) && (aw > 0.0f || ah > 0.0f)) {
            c[0] = cx; c[1] = cy; c[2] = o->pos.z;
            h[0] = aw * 0.5f; h[1] = ah * 0.5f; h[2] = 0.0f;
            return true;
        }
    }
    return CajaDe(o, c, h);
}
// Caja del OBSTACULO de rebotar(obj, otro): la regla CRUDO-vs-RESUELTO (la
// misma de chocan, ver la cabecera). Si la pos cruda del otro es AUTORITATIVA
// (adaptador 'cruda': hijo directo de la raiz, layout libre, ancla centro) se
// usa la caja normal CRUDA de este frame -> la pala que el script movio ESTE
// frame pega donde esta ahora, sin lag (el pong). Si lo acomoda el layout
// (flex / ancla a borde) la cruda es basura: se usa el rect RESUELTO del
// ultimo render (donde se dibuja). Con caja(obj, ...) fijada a mano, el tamano
// propio pisa al resuelto (el centro sigue siendo el resuelto: el layout pone
// el centro, caja() solo el tamano). Sin adaptador 'cruda' todo es crudo (el
// comportamiento historico).
static bool CajaObstaculo(Object* o, float* c, float* h) {
    if (!o) return false;
    if (gCajaUI && gCruda && Es2D(o) && !gCruda(o)) {
        float cx = 0.0f, cy = 0.0f, aw = 0.0f, ah = 0.0f;
        if (gCajaUI(o, &cx, &cy, &aw, &ah) && (aw > 0.0f || ah > 0.0f)) {
            c[0] = cx; c[1] = cy; c[2] = o->pos.z;
            W3dCuerpo* cu = Buscar(o);
            if (cu && cu->cajaProp) {
                h[0] = cu->caja[0] * 0.5f; h[1] = cu->caja[1] * 0.5f; h[2] = cu->caja[2] * 0.5f;
            } else {
                h[0] = aw * 0.5f; h[1] = ah * 0.5f; h[2] = 0.0f;
            }
            return true;
        }
    }
    return CajaDe(o, c, h);
}
// escribe el centro del objeto en UN eje (deshaciendo la conversion px->fraccion en 2D)
static void PonerCentro(Object* o, int eje, float val) {
    if (!o) return;
    if (Es2D(o) && eje < 2) {
        float lw, lh; Lienzo(&lw, &lh);
        if (eje == 0) o->pos.x = val / lw;
        else          o->pos.y = val / lh;
        return;
    }
    if (eje == 0)      o->pos.x = val;
    else if (eje == 1) o->pos.y = val;
    else               o->pos.z = val;
}

// ---------------------------------------------------------------------------
//  PASO de integracion. Corre ANTES de los scripts del frame (ver la cabecera).
// ---------------------------------------------------------------------------
void W3dFisicaPaso(float dt) {
    if (gCuerpos.empty() || dt == 0.0f) return;   // sin cuerpos no cuesta nada
    float lw, lh; Lienzo(&lw, &lh);
    bool movio = false;
    for (size_t i = 0; i < gCuerpos.size(); i++) {
        W3dCuerpo& c = gCuerpos[i];
        if (!c.o) continue;
        if (c.v[0] == 0.0f && c.v[1] == 0.0f && c.v[2] == 0.0f) continue;
        if (Es2D(c.o)) {
            // en 2D la velocidad es en px de LIENZO por segundo, pero el objeto guarda
            // la posicion como fraccion -> se divide por el lienzo al aplicarla
            c.o->pos.x += c.v[0] * dt / lw;
            c.o->pos.y += c.v[1] * dt / lh;
            c.o->pos.z += c.v[2] * dt;          // profundidad del elemento: ya esta en px
        } else {
            c.o->pos.x += c.v[0] * dt;
            c.o->pos.y += c.v[1] * dt;
            c.o->pos.z += c.v[2] * dt;
        }
        movio = true;
    }
    if (movio) W3dScriptRedibujar();   // el editor es event-driven: sin esto no repinta
}

void W3dFisicaGetVel(Object* o, float* vx, float* vy, float* vz) {
    W3dCuerpo* c = Buscar(o);
    if (vx) *vx = c ? c->v[0] : 0.0f;
    if (vy) *vy = c ? c->v[1] : 0.0f;
    if (vz) *vz = c ? c->v[2] : 0.0f;
}
void W3dFisicaSetVel(Object* o, float vx, float vy, float vz) {
    W3dCuerpo* c = BuscarOCrear(o);
    if (!c) return;
    c->v[0] = vx; c->v[1] = vy; c->v[2] = vz;
}

// ---------------------------------------------------------------------------
//  REBOTES. Todos comparten la misma regla: se busca el eje por el que MENOS
//  metido esta (el lado por el que choco), se separa por ese eje lo justo para
//  despegarlo, y se INVIERTE la velocidad de ese eje solo si venia yendo hacia
//  el obstaculo (si ya se estaba yendo no se toca: eso es lo que evita que
//  quede "vibrando" pegado cuando dos rebotes caen en el mismo frame).
// ---------------------------------------------------------------------------
// rebote de 'a' (que tiene cuerpo) contra la caja de 'b'. false si no se tocan.
// 'a' es el MOVIL: SIEMPRE su caja cruda (la fisica integra y separa escribiendo
// esa pos; ver la cabecera). 'b' es el obstaculo: regla crudo-vs-resuelto.
static bool ReboteContra(W3dCuerpo* ca, Object* b) {
    if (!ca || !ca->o || !b || ca->o == b) return false;
    float ac[3], ah[3], bc[3], bh[3];
    if (!CajaDe(ca->o, ac, ah) || !CajaObstaculo(b, bc, bh)) return false;
    // dos objetos 2D se comparan SOLO en x,y (su caja no tiene profundidad y estan
    // en capas distintas de z): es la misma regla que usa chocan().
    int ejes = (Es2D(ca->o) && Es2D(b)) ? 2 : 3;
    int mejor = -1;
    float pen = 0.0f, dist = 0.0f;
    for (int e = 0; e < ejes; e++) {
        float d = ac[e] - bc[e];
        float p = (ah[e] + bh[e]) - (d < 0.0f ? -d : d);
        if (p <= 0.0f) return false;                       // separados por este eje: no hay choque
        if (mejor < 0 || p < pen) { mejor = e; pen = p; dist = d; }
    }
    float s = (dist > 0.0f) ? 1.0f : ((dist < 0.0f) ? -1.0f : 0.0f);
    if (s == 0.0f) s = (ca->v[mejor] > 0.0f) ? -1.0f : 1.0f;   // centros pegados: salir por donde venia
    PonerCentro(ca->o, mejor, ac[mejor] + s * pen);            // despegarlo
    if (ca->v[mejor] * s < 0.0f) ca->v[mejor] = -ca->v[mejor]; // reflejar solo si iba hacia b
    W3dScriptRedibujar();
    return true;
}
// mantiene al objeto DENTRO de [lo,hi] en un eje (hayLo/hayHi = ese lado existe).
static bool ReboteLimite(W3dCuerpo* c, int eje, float lo, bool hayLo, float hi, bool hayHi) {
    if (!c || !c->o || (!hayLo && !hayHi)) return false;
    float cc[3], ch[3];
    if (!CajaDe(c->o, cc, ch)) return false;
    float centro = cc[eje], media = ch[eje];
    bool hit = false;
    if (hayLo && centro - media < lo) {
        centro = lo + media;
        if (c->v[eje] < 0.0f) c->v[eje] = -c->v[eje];
        hit = true;
    }
    if (hayHi && centro + media > hi) {
        centro = hi - media;
        if (c->v[eje] > 0.0f) c->v[eje] = -c->v[eje];
        hit = true;
    }
    if (hit) { PonerCentro(c->o, eje, centro); W3dScriptRedibujar(); }
    return hit;
}

// ---------------------------------------------------------------------------
//  LADOS de un area: la lista "arriba abajo" del rebotarEn con area. Se busca la
//  palabra entera (separadores: espacio, coma, +, |, /). Sin lib de strings ni
//  allocations: un scan y listo (Symbian).
// ---------------------------------------------------------------------------
static bool EsSepLado(char c) {
    return c == ' ' || c == ',' || c == '+' || c == '|' || c == '/' || c == '\t';
}
static bool TieneLado(const char* lista, const char* pal) {
    if (!lista || !pal) return false;
    const char* q = lista;
    while (*q) {
        while (*q && EsSepLado(*q)) q++;              // saltar separadores
        const char* ini = q;
        while (*q && !EsSepLado(*q)) q++;             // la palabra
        int n = (int)(q - ini);
        if (n > 0) {
            int k = 0;
            while (k < n && pal[k] && ini[k] == pal[k]) k++;
            if (k == n && pal[k] == '\0') return true;   // coincide entera
        }
    }
    return false;
}
// nombres de los 6 lados, por eje y por extremo (menor, mayor). El +y del lienzo
// va hacia ABAJO, por eso el extremo MENOR de y es "arriba".
static const char* kLadoNom[3][2]   = { { "izq",       "der"     },
                                        { "arriba",    "abajo"   },
                                        { "frente",    "fondo"   } };
static const char* kLadoAlias[3][2] = { { "izquierda", "derecha" },
                                        { "techo",     "piso"    },
                                        { "adelante",  "atras"   } };

// Rebote contra los bordes de la caja de un objeto AREA. 'lados' NULL o "todos"
// = las 4 (o 6) paredes cerradas; si no, solo rebotan los lados nombrados y el
// resto queda ABIERTO (se puede salir por ahi).
static bool ReboteEnArea(W3dCuerpo* c, Object* area, const char* lados) {
    float ac[3], ah[3];
    if (!c || !c->o || !area || !CajaArea(area, ac, ah)) return false;
    bool todos = (lados == NULL) || TieneLado(lados, "todos");
    int ejes = (Es2D(c->o) && Es2D(area)) ? 2 : 3;
    bool hit = false;
    for (int e = 0; e < ejes; e++) {
        if (ah[e] <= 0.0f) continue;   // el area no tiene tamano en este eje: no encierra nada
        bool lo = todos || TieneLado(lados, kLadoNom[e][0]) || TieneLado(lados, kLadoAlias[e][0]);
        bool hi = todos || TieneLado(lados, kLadoNom[e][1]) || TieneLado(lados, kLadoAlias[e][1]);
        if (ReboteLimite(c, e, ac[e] - ah[e], lo, ac[e] + ah[e], hi)) hit = true;
    }
    return hit;
}

// ===========================================================================
//  BINDS LUA. Todos toleran nil y el tipo equivocado: los getters devuelven
//  ceros y los setters/rebotes no hacen nada (devuelven false).
//  EJES: en 3D lua usa Z ARRIBA (los ejes del panel): un (x,y,z) de lua es el
//  (x,z,y) del motor, y la conversion se hace aca, en la frontera, con
//  W3dLuaEjes (ver W3dScript.h). Lo de adentro (el paso, CajaDe, los rebotes)
//  sigue en los ejes del motor. En 2D NO se convierte nada: x derecha, y abajo
//  y z la profundidad en px, como posPx.
// ===========================================================================

// velocidad(obj) -> vx, vy, vz   (0,0,0 si el objeto no tiene velocidad)
// velocidad(obj, vx, vy [, vz])  -> la fija y deja al objeto EN MANOS DEL MOTOR:
//   desde ese momento el paso de fisica lo mueve solo cada frame (pos += vel*dt).
//   Unidades: px de lienzo por segundo en 2D (mismo espacio que posPx), unidades
//   del motor por segundo en 3D. Ejes 2D: +x derecha, +y ABAJO. Ejes 3D: los de
//   lua (Z arriba): vz es la vertical.
//   velocidad(obj, 0, 0) lo deja quieto (sigue siendo un cuerpo, no cuesta nada).
static int LVelocidad(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    const bool tresD = (o != NULL) && !Es2D(o);
    if (lua_gettop(L) <= 1) {
        float vx = 0, vy = 0, vz = 0;
        W3dFisicaGetVel(o, &vx, &vy, &vz);
        if (tresD) W3dLuaEjes(vy, vz);
        lua_pushnumber(L, vx); lua_pushnumber(L, vy); lua_pushnumber(L, vz);
        return 3;
    }
    if (!o) return 0;
    float vx = (float)luaL_checknumber(L, 2);
    float vy = (float)luaL_optnumber(L, 3, 0.0);
    float vz = (float)luaL_optnumber(L, 4, 0.0);
    if (tresD) W3dLuaEjes(vy, vz);
    W3dFisicaSetVel(o, vx, vy, vz);
    return 0;
}

// acelerar(obj, factor [, tope]) -> rapidez resultante.
//   Multiplica la velocidad por 'factor' conservando la direccion. Con 'tope'
//   (>0) la rapidez no pasa de ese valor. El clasico "5% mas rapido por golpe,
//   con techo" del pong es UNA linea: acelerar(pelota, 1.05, VEL_MAX).
static int LAcelerar(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    W3dCuerpo* c = Buscar(o);
    if (!c) { lua_pushnumber(L, 0); return 1; }
    float f = (float)luaL_optnumber(L, 2, 1.0);
    c->v[0] *= f; c->v[1] *= f; c->v[2] *= f;
    float r = sqrtf(c->v[0]*c->v[0] + c->v[1]*c->v[1] + c->v[2]*c->v[2]);
    float tope = (float)luaL_optnumber(L, 3, 0.0);
    if (tope > 0.0f && r > tope) {
        float k = tope / r;
        c->v[0] *= k; c->v[1] *= k; c->v[2] *= k;
        r = tope;
    }
    lua_pushnumber(L, r);
    return 1;
}

// caja(obj) -> x, y, z: el tamanio de la caja de colision que se esta usando.
// caja(obj, x, y [, z]): la fija a mano. 2D: ancho, alto (y profundidad) en px de
//   lienzo. 3D: el tamanio por eje de LUA, en unidades del motor (x = ancho,
//   y = fondo, z = ALTO). Sirve para afinar (una pelota con caja mas chica que su
//   dibujo) y es la forma de darle caja a un objeto 3D que no es malla.
static int LCaja(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    const bool tresD = (o != NULL) && !Es2D(o);
    if (lua_gettop(L) <= 1) {
        float c[3], h[3];
        if (!CajaDe(o, c, h)) { h[0] = h[1] = h[2] = 0.0f; }
        if (tresD) W3dLuaEjes(h[1], h[2]);
        lua_pushnumber(L, h[0] * 2.0f); lua_pushnumber(L, h[1] * 2.0f); lua_pushnumber(L, h[2] * 2.0f);
        return 3;
    }
    if (!o) return 0;
    W3dCuerpo* cu = BuscarOCrear(o);
    if (!cu) return 0;
    cu->caja[0] = (float)luaL_checknumber(L, 2);
    cu->caja[1] = (float)luaL_optnumber(L, 3, 0.0);
    cu->caja[2] = (float)luaL_optnumber(L, 4, 0.0);
    if (tresD) W3dLuaEjes(cu->caja[1], cu->caja[2]);
    cu->cajaProp = true;
    return 0;
}

// rebotar(obj, otro [, otro2, ...]) -> bool
//   Si la caja de 'obj' se solapa con la de alguno de los otros, el MOTOR lo
//   despega y le invierte la velocidad del eje por el que choco. Devuelve true
//   si hubo rebote (para el beep del lua). Cada "otro" puede ser un objeto o una
//   TABLA de objetos: rebotar(pelota, paredes) con paredes = { a, b, c }.
//   'obj' tiene que tener velocidad (velocidad(obj, ...)): sin cuerpo no rebota.
static int LRebotar(lua_State* L) {
    Object* a = W3dScriptParamObjeto(L, 1);
    W3dCuerpo* ca = Buscar(a);
    if (!ca) { lua_pushboolean(L, 0); return 1; }
    bool hit = false;
    int n = lua_gettop(L);
    for (int i = 2; i <= n; i++) {
        if (lua_istable(L, i)) {
            int len = (int)lua_rawlen(L, i);
            for (int k = 1; k <= len; k++) {
                lua_rawgeti(L, i, k);
                Object* b = W3dScriptParamObjeto(L, -1);
                lua_pop(L, 1);
                if (b && ReboteContra(ca, b)) hit = true;
            }
        } else {
            Object* b = W3dScriptParamObjeto(L, i);
            if (b && ReboteContra(ca, b)) hit = true;
        }
    }
    lua_pushboolean(L, hit ? 1 : 0);
    return 1;
}

// rebotarEn(obj, area [, "lados"]) -> bool          <- la forma RECOMENDADA
//   Mantiene al objeto DENTRO de la caja de OTRO objeto ('area': la cancha, un
//   contenedor de la UI, una habitacion 3D). La caja del area sale del LAYOUT (el
//   mismo rect que da cajaUI), asi que el juego NO calcula ningun borde: donde
//   esta la cancha lo decide el .w3dui.
//   "lados" dice que paredes EXISTEN; las que no se nombran quedan ABIERTAS (se
//   puede salir por ahi). Separadores: espacio, coma, +, |, /. Palabras:
//       x:  "izq" ("izquierda")   "der" ("derecha")
//       y:  "arriba" ("techo")    "abajo" ("piso")     <- 2D: +y va hacia ABAJO
//       z:  "frente" ("adelante") "fondo" ("atras")    <- solo en 3D
//       "todos" = las 4/6 cerradas (lo mismo que no pasar el argumento)
//   En 3D las palabras nombran DIRECCIONES, no letras de lua: "arriba"/"abajo"
//   son las paredes de la vertical (la Z de lua, la Y del motor) y "frente"/
//   "fondo" las de la profundidad (la Y de lua, la Z del motor). Siguen
//   atadas al extremo menor/mayor de cada eje del MOTOR (kLadoNom), como antes.
//   El pong es una linea: rebotarEn(pelota, cancha, "arriba abajo") -> rebota en
//   el techo y en el piso, y por los costados se va (eso es justamente el punto).
//
// rebotarEn(obj, x0, y0, x1, y1 [, z0, z1]) -> bool  <- la forma con NUMEROS
//   Igual pero el rect/caja se da a mano. (x0,y0) es la esquina de valores
//   MENORES: en 2D eso es izquierda y ARRIBA. Cualquier lado puede ser nil = ese
//   lado esta ABIERTO. Solo hace falta si el area no es un objeto de la escena.
//   En 3D los limites van en ejes de LUA (Z arriba): y0/y1 limitan la Z del motor
//   y z0/z1 (la vertical) la Y del motor.
static int LRebotarEn(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    W3dCuerpo* c = Buscar(o);
    if (!c) { lua_pushboolean(L, 0); return 1; }
    // el 2do argumento decide la forma: un OBJETO es un area, un numero/nil son bordes
    Object* area = W3dScriptParamObjeto(L, 2);
    if (area) {
        lua_pushboolean(L, ReboteEnArea(c, area, lua_tostring(L, 3)) ? 1 : 0);
        return 1;
    }
    bool hx0 = !lua_isnoneornil(L, 2), hy0 = !lua_isnoneornil(L, 3);
    bool hx1 = !lua_isnoneornil(L, 4), hy1 = !lua_isnoneornil(L, 5);
    bool hz0 = !lua_isnoneornil(L, 6), hz1 = !lua_isnoneornil(L, 7);
    // el eje del MOTOR de la y y de la z de lua: en 3D se intercambian (W3dLuaEjes), en 2D no
    int ejeY = 1, ejeZ = 2;
    if (!Es2D(o)) { ejeY = 2; ejeZ = 1; }
    bool hit = false;
    if (ReboteLimite(c, 0, (float)luaL_optnumber(L, 2, 0.0), hx0,
                           (float)luaL_optnumber(L, 4, 0.0), hx1)) hit = true;
    if (ReboteLimite(c, ejeY, (float)luaL_optnumber(L, 3, 0.0), hy0,
                              (float)luaL_optnumber(L, 5, 0.0), hy1)) hit = true;
    if (ReboteLimite(c, ejeZ, (float)luaL_optnumber(L, 6, 0.0), hz0,
                              (float)luaL_optnumber(L, 7, 0.0), hz1)) hit = true;
    lua_pushboolean(L, hit ? 1 : 0);
    return 1;
}

// rebotarDentro(obj, area) -> bool
//   El objeto queda ENCERRADO en la caja del area (las 4/6 paredes cerradas).
//   Es exactamente rebotarEn(obj, area) sin lista de lados; se mantiene el nombre
//   porque se lee muy bien cuando no hay ningun lado abierto.
static int LRebotarDentro(lua_State* L) {
    W3dCuerpo* c = Buscar(W3dScriptParamObjeto(L, 1));
    lua_pushboolean(L, ReboteEnArea(c, W3dScriptParamObjeto(L, 2), NULL) ? 1 : 0);
    return 1;
}

void W3dFisicaRegistrarBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LVelocidad);     lua_setglobal(L, "velocidad");
    lua_pushcfunction(L, LAcelerar);      lua_setglobal(L, "acelerar");
    lua_pushcfunction(L, LCaja);          lua_setglobal(L, "caja");
    lua_pushcfunction(L, LRebotar);       lua_setglobal(L, "rebotar");
    lua_pushcfunction(L, LRebotarEn);     lua_setglobal(L, "rebotarEn");
    lua_pushcfunction(L, LRebotarDentro); lua_setglobal(L, "rebotarDentro");
}
