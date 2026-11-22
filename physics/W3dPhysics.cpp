// ============================================================================
//  W3dPhysics.cpp — ver W3dPhysics.h. Dialecto C++03 (Symbian compila esto).
//  Comentarios en espanol sin acentos.
// ============================================================================
#include "physics/W3dPhysics.h"
#include "objects/Objects.h"
#include "script/W3dScript.h"   // W3dScriptParamObject (binds) + W3dScriptRedraw (editor event-driven)
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
//  OJO: los W3dBody* se INVALIDAN al crear otro (push_back). Nunca guardar dos
//  punteros a la vez con un FindOrCreate en el medio.
// ---------------------------------------------------------------------------
struct W3dBody {
    Object* o;
    float v[3];       // velocidad por eje (px/seg de lienzo en 2D, unidades/seg en 3D)
    float box[3];     // caja de colision propia (ancho, alto, profundo); solo si boxProp
    bool  boxProp;
};

static std::vector<W3dBody> gBodies;
static W3dPhysicsCanvasFn gCanvas  = NULL;
static W3dPhysicsSize2DFn  gSize2D = NULL;
static W3dPhysicsBoxUIFn gBoxUI    = NULL;
static W3dRawPhysicsFn  gRaw       = NULL;

void W3dPhysicsSetAdapter2D(W3dPhysicsCanvasFn canvas, W3dPhysicsSize2DFn size2d,
                             W3dPhysicsBoxUIFn cajaui, W3dRawPhysicsFn raw) {
    gCanvas = canvas; gSize2D = size2d; gBoxUI = cajaui; gRaw = raw;
}

static W3dBody* Find(Object* o) {
    if (!o) return NULL;
    for (size_t i = 0; i < gBodies.size(); i++)
        if (gBodies[i].o == o) return &gBodies[i];
    return NULL;
}
static W3dBody* FindOrCreate(Object* o) {
    if (!o) return NULL;
    W3dBody* c = Find(o);
    if (c) return c;
    W3dBody n;
    n.o = o;
    n.v[0] = n.v[1] = n.v[2] = 0.0f;
    n.box[0] = n.box[1] = n.box[2] = 0.0f;
    n.boxProp = false;
    gBodies.push_back(n);
    return &gBodies[gBodies.size() - 1];
}

bool W3dPhysicsHas(Object* o) { return Find(o) != NULL; }
bool W3dPhysicsAny(void) { return !gBodies.empty(); }
void W3dPhysicsClear(void) { gBodies.clear(); }
void W3dPhysicsForget(Object* o) {
    for (size_t i = 0; i < gBodies.size(); i++)
        if (gBodies[i].o == o) { gBodies.erase(gBodies.begin() + i); return; }
}

// ---------------------------------------------------------------------------
//  ESPACIO de cada objeto (ver la cabecera): 2D = px de lienzo, 3D = unidades.
// ---------------------------------------------------------------------------
// true si el objeto es un elemento de la UI 2D del juego (su pos esta guardada
// como FRACCION del lienzo). Sin adaptador registrado no hay 2D posible.
static bool Es2D(Object* o) {
    if (!o || !gCanvas) return false;
    switch (o->getType().v) {
        case ObjectType::ui:
        case ObjectType::text2d:
        case ObjectType::image2d:
        case ObjectType::rect2d:
        case ObjectType::cont2d:
        case ObjectType::slice9:
        case ObjectType::button2d:
        case ObjectType::expand2d:
        case ObjectType::video2d:
            return true;
        default:
            return false;
    }
}
static void Canvas(float* w, float* h) {
    *w = 1.0f; *h = 1.0f;
    if (gCanvas) gCanvas(w, h);
    if (*w < 1.0f) *w = 1.0f;
    if (*h < 1.0f) *h = 1.0f;
}
// CENTRO del objeto en su espacio (c[0..2]) + MEDIAS extensiones de su caja (h[0..2]).
// 2D: centro en px de lienzo y caja = el rect del elemento (lo mismo que compara collide()).
// 3D: centro = pos local y caja = cubo de FocusRadius() (bounding de la malla). En los dos
// casos, box(obj, w, h, d) desde lua pisa el default.
static bool BoxOf(Object* o, float* c, float* h) {
    if (!o) return false;
    bool dosd = Es2D(o);
    if (dosd) {
        float lw, lh; Canvas(&lw, &lh);
        c[0] = o->pos.x * lw; c[1] = o->pos.y * lh; c[2] = o->pos.z;
    } else {
        c[0] = o->pos.x; c[1] = o->pos.y; c[2] = o->pos.z;
    }
    W3dBody* cu = Find(o);
    if (cu && cu->boxProp) {
        h[0] = cu->box[0] * 0.5f; h[1] = cu->box[1] * 0.5f; h[2] = cu->box[2] * 0.5f;
    } else if (dosd) {
        float aw = 0.0f, ah = 0.0f;
        if (gSize2D) gSize2D(o, &aw, &ah);
        h[0] = aw * 0.5f; h[1] = ah * 0.5f; h[2] = 0.0f;
    } else {
        float r = o->FocusRadius();
        h[0] = r; h[1] = r; h[2] = r;
    }
    return true;
}
// Caja de un objeto usado como AREA (la cancha de bounceOn/bounceInside). A
// diferencia de BoxOf, si es un objeto 2D que el layout ya resolvio se usa ESE
// rect (el mismo que devuelve uiBox en lua): un area es un widget acomodado por
// el .w3dui, y su pos cruda no es donde termino dibujado. Si no hay adaptador o
// todavia no se dibujo, cae a la caja normal.
// DECISION: el area usa el resuelto SIEMPRE (aunque su pos cruda sea
// autoritativa): un area no se mueve por script, asi que no hay lag posible, y
// cambiarle la fuente moveria las paredes de juegos que ya funcionan.
static bool BoxArea(Object* o, float* c, float* h) {
    if (!o) return false;
    if (gBoxUI && Es2D(o)) {
        float cx = 0.0f, cy = 0.0f, aw = 0.0f, ah = 0.0f;
        if (gBoxUI(o, &cx, &cy, &aw, &ah) && (aw > 0.0f || ah > 0.0f)) {
            c[0] = cx; c[1] = cy; c[2] = o->pos.z;
            h[0] = aw * 0.5f; h[1] = ah * 0.5f; h[2] = 0.0f;
            return true;
        }
    }
    return BoxOf(o, c, h);
}
// Caja del OBSTACULO de bounce(obj, otro): la regla CRUDO-vs-RESUELTO (la
// misma de chocan, ver la cabecera). Si la pos cruda del otro es AUTORITATIVA
// (adaptador 'cruda': hijo directo de la raiz, layout libre, ancla centro) se
// usa la caja normal CRUDA de este frame -> la pala que el script movio ESTE
// frame pega donde esta ahora, sin lag (el pong). Si lo acomoda el layout
// (flex / ancla a borde) la cruda es basura: se usa el rect RESUELTO del
// ultimo render (donde se dibuja). Con box(obj, ...) fijada a mano, el tamano
// propio pisa al resuelto (el centro sigue siendo el resuelto: el layout pone
// el centro, box() solo el tamano). Sin adaptador 'cruda' todo es crudo (el
// comportamiento historico).
static bool BoxObstacle(Object* o, float* c, float* h) {
    if (!o) return false;
    if (gBoxUI && gRaw && Es2D(o) && !gRaw(o)) {
        float cx = 0.0f, cy = 0.0f, aw = 0.0f, ah = 0.0f;
        if (gBoxUI(o, &cx, &cy, &aw, &ah) && (aw > 0.0f || ah > 0.0f)) {
            c[0] = cx; c[1] = cy; c[2] = o->pos.z;
            W3dBody* cu = Find(o);
            if (cu && cu->boxProp) {
                h[0] = cu->box[0] * 0.5f; h[1] = cu->box[1] * 0.5f; h[2] = cu->box[2] * 0.5f;
            } else {
                h[0] = aw * 0.5f; h[1] = ah * 0.5f; h[2] = 0.0f;
            }
            return true;
        }
    }
    return BoxOf(o, c, h);
}
// escribe el centro del objeto en UN eje (deshaciendo la conversion px->fraccion en 2D)
static void SetCenter(Object* o, int axis, float val) {
    if (!o) return;
    if (Es2D(o) && axis < 2) {
        float lw, lh; Canvas(&lw, &lh);
        if (axis == 0) o->pos.x = val / lw;
        else          o->pos.y = val / lh;
        return;
    }
    if (axis == 0)      o->pos.x = val;
    else if (axis == 1) o->pos.y = val;
    else               o->pos.z = val;
}

// ---------------------------------------------------------------------------
//  PASO de integracion. Corre ANTES de los scripts del frame (ver la cabecera).
// ---------------------------------------------------------------------------
void W3dPhysicsStep(float dt) {
    if (gBodies.empty() || dt == 0.0f) return;   // sin cuerpos no cuesta nada
    float lw, lh; Canvas(&lw, &lh);
    bool moved = false;
    for (size_t i = 0; i < gBodies.size(); i++) {
        W3dBody& c = gBodies[i];
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
        moved = true;
    }
    if (moved) W3dScriptRedraw();   // el editor es event-driven: sin esto no repinta
}

void W3dPhysicsGetVel(Object* o, float* vx, float* vy, float* vz) {
    W3dBody* c = Find(o);
    if (vx) *vx = c ? c->v[0] : 0.0f;
    if (vy) *vy = c ? c->v[1] : 0.0f;
    if (vz) *vz = c ? c->v[2] : 0.0f;
}
void W3dPhysicsSetVel(Object* o, float vx, float vy, float vz) {
    W3dBody* c = FindOrCreate(o);
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
static bool BounceAgainst(W3dBody* ca, Object* b) {
    if (!ca || !ca->o || !b || ca->o == b) return false;
    float ac[3], ah[3], bc[3], bh[3];
    if (!BoxOf(ca->o, ac, ah) || !BoxObstacle(b, bc, bh)) return false;
    // dos objetos 2D se comparan SOLO en x,y (su caja no tiene profundidad y estan
    // en capas distintas de z): es la misma regla que usa collide().
    int axes = (Es2D(ca->o) && Es2D(b)) ? 2 : 3;
    int best  = -1;
    float pen = 0.0f, dist = 0.0f;
    for (int e = 0; e < axes; e++) {
        float d = ac[e] - bc[e];
        float p = (ah[e] + bh[e]) - (d < 0.0f ? -d : d);
        if (p <= 0.0f) return false;                       // separados por este eje: no hay choque
        if (best < 0 || p < pen) { best = e; pen = p; dist = d; }
    }
    float s = (dist > 0.0f) ? 1.0f : ((dist < 0.0f) ? -1.0f : 0.0f);
    if (s == 0.0f) s = (ca->v[best] > 0.0f) ? -1.0f : 1.0f; // centros pegados: salir por donde venia
    SetCenter(ca->o, best, ac[best] + s * pen);             // despegarlo
    if (ca->v[best] * s < 0.0f) ca->v[best] = -ca->v[best]; // reflejar solo si iba hacia b
    W3dScriptRedraw();
    return true;
}
// mantiene al objeto DENTRO de [lo,hi] en un eje (hasLo/hasHi = ese lado existe).
static bool BounceLimit(W3dBody* c, int axis, float lo, bool hasLo, float hi, bool hasHi) {
    if (!c || !c->o || (!hasLo && !hasHi)) return false;
    float cc[3], ch[3];
    if (!BoxOf(c->o, cc, ch)) return false;
    float center = cc[axis], mean = ch[axis];
    bool hit = false;
    if (hasLo && center - mean < lo) {
        center = lo + mean;
        if (c->v[axis] < 0.0f) c->v[axis] = -c->v[axis];
        hit = true;
    }
    if (hasHi && center + mean > hi) {
        center = hi - mean;
        if (c->v[axis] > 0.0f) c->v[axis] = -c->v[axis];
        hit = true;
    }
    if (hit) { SetCenter(c->o, axis, center); W3dScriptRedraw(); }
    return hit;
}

// ---------------------------------------------------------------------------
//  LADOS de un area: la lista "arriba abajo" del bounceOn con area. Se busca la
//  palabra entera (separadores: espacio, coma, +, |, /). Sin lib de strings ni
//  allocations: un scan y listo (Symbian).
// ---------------------------------------------------------------------------
static bool IsSepSide(char c) {
    return c == ' ' || c == ',' || c == '+' || c == '|' || c == '/' || c == '\t';
}
static bool HasSide(const char* list, const char* pal) {
    if (!list || !pal) return false;
    const char* q = list;
    while (*q) {
        while (*q && IsSepSide(*q)) q++;              // saltar separadores
        const char* start = q;
        while (*q && !IsSepSide(*q)) q++;             // la palabra
        int n = (int)(q - start);
        if (n > 0) {
            int k = 0;
            while (k < n && pal[k] && start[k] == pal[k]) k++;
            if (k == n && pal[k] == '\0') return true;   // coincide entera
        }
    }
    return false;
}
// nombres de los 6 lados, por eje y por extremo (menor, mayor). El +y del lienzo
// va hacia ABAJO, por eso el extremo MENOR de y es el de arriba.
static const char* kSideName[3][2] = { { "left", "right" },
                                      { "top",     "bottom"    },
                                      { "front",    "back"   } };

// Rebote contra los bordes de la caja de un objeto AREA. 'lados' NULL o "todos"
// = las 4 (o 6) paredes cerradas; si no, solo rebotan los lados nombrados y el
// resto queda ABIERTO (se puede salir por ahi).
static bool BounceInArea(W3dBody* c, Object* area, const char* sides) {
    float ac[3], ah[3];
    if (!c || !c->o || !area || !BoxArea(area, ac, ah)) return false;
    bool all = (sides == NULL) || HasSide(sides, "all");
    int axes = (Es2D(c->o) && Es2D(area)) ? 2 : 3;
    bool hit = false;
    for (int e = 0; e < axes; e++) {
        if (ah[e] <= 0.0f) continue;   // el area no tiene tamano en este eje: no encierra nada
        bool lo = all || HasSide(sides, kSideName[e][0]);
        bool hi = all || HasSide(sides, kSideName[e][1]);
        if (BounceLimit(c, e, ac[e] - ah[e], lo, ac[e] + ah[e], hi)) hit = true;
    }
    return hit;
}

// ===========================================================================
//  BINDS LUA. Todos toleran nil y el tipo equivocado: los getters devuelven
//  ceros y los setters/rebotes no hacen nada (devuelven false).
//  EJES: en 3D lua usa Z ARRIBA (los ejes del panel): un (x,y,z) de lua es el
//  (x,z,y) del motor, y la conversion se hace aca, en la frontera, con
//  W3dLuaAxes (ver W3dScript.h). Lo de adentro (el paso, BoxOf, los rebotes)
//  sigue en los ejes del motor. En 2D NO se convierte nada: x derecha, y abajo
//  y z la profundidad en px, como getPosPx.
// ===========================================================================

// getVelocity(obj) -> vx, vy, vz   (0,0,0 si el objeto no tiene velocidad)
// getVelocity(obj, vx, vy [, vz])  -> la fija y deja al objeto EN MANOS DEL MOTOR:
//   desde ese momento el paso de fisica lo mueve solo cada frame (pos += vel*dt).
//   Unidades: px de lienzo por segundo en 2D (mismo espacio que getPosPx), unidades
//   del motor por segundo en 3D. Ejes 2D: +x derecha, +y ABAJO. Ejes 3D: los de
//   lua (Z arriba): vz es la vertical.
//   getVelocity(obj, 0, 0) lo deja quieto (sigue siendo un cuerpo, no cuesta nada).
static int LVelocity(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    const bool threeD = (o != NULL) && !Es2D(o);
    if (lua_gettop(L) <= 1) {
        float vx = 0, vy = 0, vz = 0;
        W3dPhysicsGetVel(o, &vx, &vy, &vz);
        if (threeD) W3dLuaAxes(vy, vz);
        lua_pushnumber(L, vx); lua_pushnumber(L, vy); lua_pushnumber(L, vz);
        return 3;
    }
    if (!o) return 0;
    float vx = (float)luaL_checknumber(L, 2);
    float vy = (float)luaL_optnumber(L, 3, 0.0);
    float vz = (float)luaL_optnumber(L, 4, 0.0);
    if (threeD) W3dLuaAxes(vy, vz);
    W3dPhysicsSetVel(o, vx, vy, vz);
    return 0;
}

// accelerate(obj, factor [, tope]) -> rapidez resultante.
//   Multiplica la velocidad por 'factor' conservando la direccion. Con 'tope'
//   (>0) la rapidez no pasa de ese valor. El clasico "5% mas rapido por golpe,
//   con techo" del pong es UNA linea: accelerate(pelota, 1.05, VEL_MAX).
static int LAccelerate(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    W3dBody* c = Find(o);
    if (!c) { lua_pushnumber(L, 0); return 1; }
    float f = (float)luaL_optnumber(L, 2, 1.0);
    c->v[0] *= f; c->v[1] *= f; c->v[2] *= f;
    float r = sqrtf(c->v[0]*c->v[0] + c->v[1]*c->v[1] + c->v[2]*c->v[2]);
    float limit = (float)luaL_optnumber(L, 3, 0.0);
    if (limit > 0.0f && r > limit) {
        float k = limit / r;
        c->v[0] *= k; c->v[1] *= k; c->v[2] *= k;
        r = limit;
    }
    lua_pushnumber(L, r);
    return 1;
}

// box(obj) -> x, y, z: el tamanio de la caja de colision que se esta usando.
// box(obj, x, y [, z]): la fija a mano. 2D: ancho, alto (y profundidad) en px de
//   lienzo. 3D: el tamanio por eje de LUA, en unidades del motor (x = ancho,
//   y = fondo, z = ALTO). Sirve para afinar (una pelota con caja mas chica que su
//   dibujo) y es la forma de darle caja a un objeto 3D que no es malla.
static int LBox(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    const bool threeD = (o != NULL) && !Es2D(o);
    if (lua_gettop(L) <= 1) {
        float c[3], h[3];
        if (!BoxOf(o, c, h)) { h[0] = h[1] = h[2] = 0.0f; }
        if (threeD) W3dLuaAxes(h[1], h[2]);
        lua_pushnumber(L, h[0] * 2.0f); lua_pushnumber(L, h[1] * 2.0f); lua_pushnumber(L, h[2] * 2.0f);
        return 3;
    }
    if (!o) return 0;
    W3dBody* cu = FindOrCreate(o);
    if (!cu) return 0;
    cu->box[0] = (float)luaL_checknumber(L, 2);
    cu->box[1] = (float)luaL_optnumber(L, 3, 0.0);
    cu->box[2] = (float)luaL_optnumber(L, 4, 0.0);
    if (threeD) W3dLuaAxes(cu->box[1], cu->box[2]);
    cu->boxProp = true;
    return 0;
}

// bounce(obj, otro [, otro2, ...]) -> bool
//   Si la caja de 'obj' se solapa con la de alguno de los otros, el MOTOR lo
//   despega y le invierte la velocidad del eje por el que choco. Devuelve true
//   si hubo rebote (para el beep del lua). Cada "otro" puede ser un objeto o una
//   TABLA de objetos: bounce(pelota, paredes) con paredes = { a, b, c }.
//   'obj' tiene que tener velocidad (getVelocity(obj, ...)): sin cuerpo no rebota.
static int LBounce(lua_State* L) {
    Object* a = W3dScriptParamObject(L, 1);
    W3dBody* ca = Find(a);
    if (!ca) { lua_pushboolean(L, 0); return 1; }
    bool hit = false;
    int n = lua_gettop(L);
    for (int i = 2; i <= n; i++) {
        if (lua_istable(L, i)) {
            int len = (int)lua_rawlen(L, i);
            for (int k = 1; k <= len; k++) {
                lua_rawgeti(L, i, k);
                Object* b = W3dScriptParamObject(L, -1);
                lua_pop(L, 1);
                if (b && BounceAgainst(ca, b)) hit = true;
            }
        } else {
            Object* b = W3dScriptParamObject(L, i);
            if (b && BounceAgainst(ca, b)) hit = true;
        }
    }
    lua_pushboolean(L, hit ? 1 : 0);
    return 1;
}

// bounceOn(obj, area [, "lados"]) -> bool          <- la forma RECOMENDADA
//   Mantiene al objeto DENTRO de la caja de OTRO objeto ('area': la cancha, un
//   contenedor de la UI, una habitacion 3D). La caja del area sale del LAYOUT (el
//   mismo rect que da uiBox), asi que el juego NO calcula ningun borde: donde
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
//   atadas al extremo menor/mayor de cada eje del MOTOR (kSideName), como antes.
//   El pong es una linea: bounceOn(pelota, cancha, "arriba abajo") -> rebota en
//   el techo y en el piso, y por los costados se va (eso es justamente el punto).
//
// bounceOn(obj, x0, y0, x1, y1 [, z0, z1]) -> bool  <- la forma con NUMEROS
//   Igual pero el rect/box se da a mano. (x0,y0) es la esquina de valores
//   MENORES: en 2D eso es izquierda y ARRIBA. Cualquier lado puede ser nil = ese
//   lado esta ABIERTO. Solo hace falta si el area no es un objeto de la escena.
//   En 3D los limites van en ejes de LUA (Z arriba): y0/y1 limitan la Z del motor
//   y z0/z1 (la vertical) la Y del motor.
static int LBounceIn(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    W3dBody* c = Find(o);
    if (!c) { lua_pushboolean(L, 0); return 1; }
    // el 2do argumento decide la forma: un OBJETO es un area, un numero/nil son bordes
    Object* area = W3dScriptParamObject(L, 2);
    if (area) {
        lua_pushboolean(L, BounceInArea(c, area, lua_tostring(L, 3)) ? 1 : 0);
        return 1;
    }
    bool hx0 = !lua_isnoneornil(L, 2), hy0 = !lua_isnoneornil(L, 3);
    bool hx1 = !lua_isnoneornil(L, 4), hy1 = !lua_isnoneornil(L, 5);
    bool hz0 = !lua_isnoneornil(L, 6), hz1 = !lua_isnoneornil(L, 7);
    // el eje del MOTOR de la y y de la z de lua: en 3D se intercambian (W3dLuaAxes), en 2D no
    int axisY = 1, axisZ = 2;
    if (!Es2D(o)) { axisY = 2; axisZ = 1; }
    bool hit = false;
    if (BounceLimit(c, 0, (float)luaL_optnumber(L, 2, 0.0), hx0,
                           (float)luaL_optnumber(L, 4, 0.0), hx1)) hit = true;
    if (BounceLimit(c, axisY, (float)luaL_optnumber(L, 3, 0.0), hy0,
                              (float)luaL_optnumber(L, 5, 0.0), hy1)) hit = true;
    if (BounceLimit(c, axisZ, (float)luaL_optnumber(L, 6, 0.0), hz0,
                              (float)luaL_optnumber(L, 7, 0.0), hz1)) hit = true;
    lua_pushboolean(L, hit ? 1 : 0);
    return 1;
}

// bounceInside(obj, area) -> bool
//   El objeto queda ENCERRADO en la caja del area (las 4/6 paredes cerradas).
//   Es exactamente bounceOn(obj, area) sin lista de lados; se mantiene el nombre
//   porque se lee muy bien cuando no hay ningun lado abierto.
static int LBounceInside(lua_State* L) {
    W3dBody* c = Find(W3dScriptParamObject(L, 1));
    lua_pushboolean(L, BounceInArea(c, W3dScriptParamObject(L, 2), NULL) ? 1 : 0);
    return 1;
}

void W3dPhysicsRegisterBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LVelocity);     lua_setglobal(L, "getVelocity");
    lua_pushcfunction(L, LAccelerate);      lua_setglobal(L, "accelerate");
    lua_pushcfunction(L, LBox);          lua_setglobal(L, "box");
    lua_pushcfunction(L, LBounce);       lua_setglobal(L, "bounce");
    lua_pushcfunction(L, LBounceIn);     lua_setglobal(L, "bounceOn");
    lua_pushcfunction(L, LBounceInside); lua_setglobal(L, "bounceInside");
}
