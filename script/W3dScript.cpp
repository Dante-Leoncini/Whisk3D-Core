// ============================================================================
//  W3dScript.cpp — ver W3dScript.h. Dialecto C++03 (Symbian compila esto).
// ============================================================================
#include "script/W3dScript.h"
#include "objects/Objects.h"
#include "animation/VertexAnimation.h"   // animate(): FindTargetAnim / nextAnim
#include "animation/Animation.h"         // w3dGetTicks(): reloj self-contained del Core (sembrar el random)
#include "objects/Mesh.h"
#include "objects/Light.h"   // binds de LUZ: getColor()/setColor()/getEnergy()/setEnergy()
#include "objects/Armature.h"            // binds de ESQUELETO: animClip()/animFrame()/bonePoint()...
#include "animation/SkeletalAnimation.h" // W3dArmaturePlayClip / EvaluatePoseSkeleton
#include "animation/W3dAnimSet.h"        // las CAPAS de clips de jerarquia de una raiz (objectLayer...)
#include "math/Quaternion.h"
#include "w3dFilesystem.h"   // leer el .lua para ORDENAR las propiedades como estan declaradas
#include "base/W3dConfig.h"  // getConfig()/setConfig()/mute(): persistencia + mute para los juegos lua
#include "physics/W3dPhysics.h"  // fisica minima del Core: getVelocity/bounce/... (los binds los registra ella)
#include "physics/W3dRigid.h"
#include "physics/W3dColMesh.h" // colision contra triangulos (colGround/colWall/colRay)  // cuerpos rigidos del Core (physicsVel/physicsRay/contacts/...)
#include "physics/W3dHitbox.h"  // hitbox: binds + limpiar pares al Stop
#include "net/W3dNet.h"         // jugar en red: binds red*, se atiende al fin de cada cuadro y se cierra al Stop
#include "io/W3dResources.h"    // las FUENTES de los scripts como recurso (el streaming las pide antes)
#include "w3dlog.h"
#include <math.h>
#include <map>
#include <algorithm>
#include <stdio.h>   // snprintf: formatear el default numerico / el dump de getShared()
#include <string.h>
#include <time.h>    // time(): fuente que VARIA entre ejecuciones para la semilla del random; date(): localtime/gmtime
#if defined(_WIN32) && !defined(W3D_SYMBIAN)
#include <sys/timeb.h>   // unixTime(): _ftime (los milisegundos, sin windows.h)
#else
#include <sys/time.h>    // unixTime(): gettimeofday (como objects/WrittenShader.cpp, en todas las plataformas)
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>   // touch(): el navegador dice si la pantalla se toca
#endif

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#ifdef W3D_NO_PHYSICS
// ============================================================================
//  BUILD SIN FISICA (-DW3D_SIN_FISICA): physics/W3dPhysics.cpp NO se compila.
//  Aca se implementa TODA la API de physics/W3dPhysics.h como stubs no-op C++03:
//  los getters devuelven 0/false, los setters no hacen nada, y los binds lua
//  (getVelocity/accelerate/box/bounce/bounceOn/bounceInside) se registran igual
//  pero vacios -> un script que los llame NO crashea, solo avisa UNA vez por log.
//  Los callers (w3drun, SimJuego, BindsJuego) no cambian: linkean contra esto.
//  OJO: con esta macro definida NO compilar W3dPhysics.cpp (simbolo duplicado).
// ============================================================================
static bool gPhysicsNotice = false;
static void PhysicsNoticeOff() {
    if (gPhysicsNotice) return;
    gPhysicsNotice = true;
    w3dLogW("script: fisica desactivada en este build (W3D_NO_PHYSICS); velocidad/rebotar no hacen nada");
}
void W3dPhysicsStep(float) {}
void W3dPhysicsClear(void) {}
void W3dPhysicsForget(Object*) {}
bool W3dPhysicsHas(Object*) { return false; }
bool W3dPhysicsAny(void) { return false; }
void W3dPhysicsGetVel(Object*, float* vx, float* vy, float* vz) {
    if (vx) *vx = 0.0f; if (vy) *vy = 0.0f; if (vz) *vz = 0.0f;
}
void W3dPhysicsSetVel(Object*, float, float, float) { PhysicsNoticeOff(); }
void W3dPhysicsSetAdapter2D(W3dPhysicsCanvasFn, W3dPhysicsSize2DFn, W3dPhysicsBoxUIFn,
                             W3dRawPhysicsFn) {}
// ---- binds lua stub: misma ARIDAD de retorno que los reales ----------------
// getVelocity(obj) -> 0,0,0 ; getVelocity(obj, vx, vy) -> nada
static int LPhysicsStubVelocity(lua_State* L) {
    PhysicsNoticeOff();
    if (lua_gettop(L) <= 1) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    return 0;
}
// accelerate(obj, factor) -> rapidez resultante (0 sin fisica)
static int LPhysicsStubAccelerate(lua_State* L) { PhysicsNoticeOff(); lua_pushnumber(L, 0); return 1; }
// box(obj) -> 0,0,0 ; box(obj, w, h) -> nada
static int LPhysicsStubBox(lua_State* L) {
    PhysicsNoticeOff();
    if (lua_gettop(L) <= 1) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    return 0;
}
// bounce/bounceOn/bounceInside -> false (nunca hay rebote)
static int LPhysicsStubBounce(lua_State* L) { PhysicsNoticeOff(); lua_pushboolean(L, 0); return 1; }
void W3dPhysicsRegisterBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LPhysicsStubVelocity); lua_setglobal(L, "getVelocity");
    lua_pushcfunction(L, LPhysicsStubAccelerate);  lua_setglobal(L, "accelerate");
    lua_pushcfunction(L, LPhysicsStubBox);      lua_setglobal(L, "box");
    lua_pushcfunction(L, LPhysicsStubBounce);    lua_setglobal(L, "bounce");
    lua_pushcfunction(L, LPhysicsStubBounce);    lua_setglobal(L, "bounceOn");
    lua_pushcfunction(L, LPhysicsStubBounce);    lua_setglobal(L, "bounceInside");
}
#endif // W3D_NO_PHYSICS

// una instancia viva = un lua_State propio + sus referencias resueltas
// los EVENTOS que se cachean por instancia (ver W3dScriptEvent): el indice es el de eventos[]
static const char* const kEventsScript[] = { "onEnter", "onStay", "onExit", "onTouch" };
enum { W3D_EVENTS_SCRIPT = 4 };

struct W3dScriptInst {
    lua_State* L;
    std::map<std::string, Object*> refs;
    std::map<std::string, std::string> options;   // desplegables elegidos
    std::map<std::string, std::string> values;    // valores por instancia (tipo 2), como texto
    Object* owner;
    // el script DEFINE el evento i? -1 = sin mirar todavia, 0 = no, 1 = si. Se mira una vez
    // (al primer evento) y queda: un hitbox con cien pares por tick no hace cien getglobal.
    signed char events[W3D_EVENTS_SCRIPT];
    // apagado (el ojo de la lista de scripts): no se llama a ninguna de sus funciones. startDone: su start() ya
    // corrio (apagado al darle Play no corre; si despues lo prenden, corre ahi)
    bool off, startDone;
    W3dScriptInst() : L(NULL), owner(NULL), off(false), startDone(false) {
        for (int i = 0; i < W3D_EVENTS_SCRIPT; i++) events[i] = -1;
    }
};

// ---- estado COMPARTIDO entre scripts (ver W3dScript.h) ---------------------
// Los lua_State estan aislados a proposito; este mapa C++ unico es el canal
// entre scripts (fruta.lua suma, el HUD del principal lee). Vive el PLAY
// entero y muere en W3dScriptUnloadAll.
struct W3dSharedVal {
    int type;            // 0 = numero, 1 = bool, 2 = texto
    double num;
    bool b;
    std::string txt;
    W3dSharedVal() : type(0), num(0.0), b(false) {}
};
static std::map<std::string, W3dSharedVal> gShared;

static std::map<Object*, std::vector<W3dScriptInst*> > gInstances;
static std::map<std::string, bool> gKeys;
// SNAPSHOT del frame ANTERIOR (para el FLANCO de keyPressed()/buttonPressed(): true solo en el frame en
// que la key/button pasa de suelto a apretado). Mismo criterio que el gPunPrev del runtime (w3drun): se
// copia UNA vez por frame al final de W3dScriptUpdate, DESPUES de correr los scripts, para que todas
// las llamadas del frame vean el mismo estado previo.
static std::map<std::string, bool> gKeysPrev;
// las teclas que BAJARON desde el snapshot (aunque ya se hayan soltado): con el juego lento (el N95) una pulsacion
// corta baja y sube entre dos frames y gKeys nunca la mostraba; keyPressed() la cuenta igual
static std::map<std::string, bool> gPressedKeys;
static bool gEndFrameExternal = false;   // alguien llama W3dScriptEndFrame: el snapshot es suyo (ver el .h)
static float gSticks[2][2] = { {0,0}, {0,0} };   // [izq/der][x/y]
// LOS MANDOS POR SEPARADO (dos jugadores en el mismo equipo): ademas de la mezcla de todos (gSticks / gButtonsPad: lo
// que leen button("a") y stick("left")), el estado de cada uno. button("a", 2), stick("left", 2), trigger("right", 2) y
// buttonPressed("a", 2) leen el mando 2 (1..W3D_GAMEPADS, en el orden en que se conectaron)
enum { W3D_GAMEPADS = 4 };
static float gSticksM[W3D_GAMEPADS][2][2];
static float gTriggersM[W3D_GAMEPADS][2];
static std::map<std::string, bool> gButtonsM[W3D_GAMEPADS], gButtonsMPrev[W3D_GAMEPADS];
static float gTriggers[2] = { 0, 0 };              // [izq/der] 0..1 (los gatillos analogicos del mando: pedales)
static std::map<std::string, bool> gButtonsPad;
static std::map<std::string, bool> gButtonsPadPrev;   // gemelo de gKeysPrev para el gamepad
// MULTI-TOUCH: hasta 4 dedos. tap() = dedo 0 (compat); finger(n) (1-based en lua) = dedo n-1.
// Sirve para el 2 jugadores tactil: un dedo por pala (mitad izquierda/derecha de la pantalla).
#define W3D_MAXDEDOS 4
static float gFingerX[W3D_MAXDEDOS] = {0,0,0,0}, gFingerY[W3D_MAXDEDOS] = {0,0,0,0};
static bool  gFingerAct[W3D_MAXDEDOS] = {false,false,false,false};
// AUDIO para el beep() de los juegos (efectos estilo WhiskPaddle). Forward-decl del modulo de audio del Core
// (evita el include del dir de audio). Sin -DW3D_ENABLE_AUDIO estas son stubs no-op -> beep() es mudo.
namespace w3dEngine { class W3dSound; W3dSound* W3dSoundBeep(float,int,float); int W3dSoundPlay(W3dSound*,float,bool); }
static std::map<int, w3dEngine::W3dSound*> gBeeps;   // cache por (freq,ms): no regenerar el tono cada frame
static std::string gLastError;
static W3dScriptBindFn gBindExtra = NULL;
// ver W3dScript.h: main/ lo registra para prender g_redraw + g_objectsMoved; en el runtime queda NULL
static W3dScriptRedrawFn gRedraw = NULL;
// ENTROPIA EXTRA opcional para la semilla del random. El Core ya se siembra self-contained (time() +
// w3dGetTicks() + direccion del lua_State + contador); la plataforma puede REFORZARLO con W3dScriptSeed()
// (ej time(0) del desktop). 0 = nadie la seteo (el Core se sembra igual con sus fuentes propias).
static unsigned gSeedExtra = 0;
static unsigned gSemCounter = 0;   // asegura que CADA lua_State (los 4 del whiskpaddle) arranque distinto

const char* W3dScriptLastError() { return gLastError.c_str(); }
void W3dScriptSetBindExtra(W3dScriptBindFn fn) { gBindExtra = fn; }
void W3dScriptSetRedraw(W3dScriptRedrawFn fn) { gRedraw = fn; }
// TODO setter de objeto termina llamando a esto (ver W3dScript.h): sin el, en el editor el cambio
// no se ve hasta que el usuario mueva el mouse.
void W3dScriptRedraw() { if (gRedraw) gRedraw(); }
static void Redraw() { W3dScriptRedraw(); }
void W3dScriptSeed(unsigned s) { gSeedExtra = s; }
// SEMILLA FIJA (el harness): cada lua_State arranca con una semilla que solo depende de su orden de creacion, asi dos
// corridas del mismo script dan el MISMO juego (las capturas se pueden comparar)
static bool gSeedFixed = false;
void W3dScriptFixedSeed(bool on) { gSeedFixed = on; gSemCounter = 0; }

void W3dScriptKey(const char* name, bool pressed) {
    if (!name) return;
    if (pressed) gPressedKeys[name] = true;
    gKeys[name] = pressed;
}
void W3dScriptReleaseKeys() {
    gKeys.clear(); gButtonsPad.clear();
    gKeysPrev.clear(); gButtonsPadPrev.clear();   // el reset tambien limpia el snapshot del flanco
    gPressedKeys.clear();
    gSticks[0][0] = gSticks[0][1] = gSticks[1][0] = gSticks[1][1] = 0.0f;
    gTriggers[0] = gTriggers[1] = 0.0f;
    for (int m = 0; m < W3D_GAMEPADS; m++) {
        gButtonsM[m].clear(); gButtonsMPrev[m].clear();
        gSticksM[m][0][0] = gSticksM[m][0][1] = gSticksM[m][1][0] = gSticksM[m][1][1] = 0.0f;
        gTriggersM[m][0] = gTriggersM[m][1] = 0.0f;
    }
    for (int i=0;i<W3D_MAXDEDOS;i++){ gFingerX[i]=gFingerY[i]=0.0f; gFingerAct[i]=false; }
}
void W3dScriptStickOf(int gamepad, int which, float x, float y) {
    if (gamepad < 0 || gamepad >= W3D_GAMEPADS || which < 0 || which > 1) return;
    gSticksM[gamepad][which][0] = x; gSticksM[gamepad][which][1] = y;
}
void W3dScriptButtonPadOf(int gamepad, const char* name, bool v) {
    if (gamepad >= 0 && gamepad < W3D_GAMEPADS && name) gButtonsM[gamepad][name] = v;
}
void W3dScriptTriggerOf(int gamepad, int which, float v) {
    if (gamepad < 0 || gamepad >= W3D_GAMEPADS || which < 0 || which > 1) return;
    gTriggersM[gamepad][which] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
void W3dScriptStick(int which, float x, float y) {
    if (which < 0 || which > 1) return;
    gSticks[which][0] = x; gSticks[which][1] = y;
}
void W3dScriptButtonPad(const char* name, bool v) { if (name) gButtonsPad[name] = v; }
void W3dScriptTrigger(int which, float v) {
    if (which < 0 || which > 1) return;
    gTriggers[which] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
// LECTURA (la MISMA fuente que stick()/button() de lua): la usa el harness para
// asertar que el d-pad y las flechas del teclado llegan de verdad al juego.
void W3dScriptStickRead(int which, float* x, float* y) {
    if (which < 0 || which > 1) { if (x) *x = 0.0f; if (y) *y = 0.0f; return; }
    if (x) *x = gSticks[which][0];
    if (y) *y = gSticks[which][1];
}
bool W3dScriptButtonPadRead(const char* name) {
    if (!name) return false;
    std::map<std::string, bool>::const_iterator it = gButtonsPad.find(name);
    return it != gButtonsPad.end() && it->second;
}
void W3dScriptTouch(float x, float y, bool active) { gFingerX[0]=x; gFingerY[0]=y; gFingerAct[0]=active; } // dedo 0 (compat)
// TACTIL: la pantalla se toca. Lo sabe la plataforma (Android; el navegador de un telefono) o se aprende con el primer
// dedo: aca solo llegan dedos de verdad (el mouse va por W3dScriptTouch)
static bool gTouch = false;
void W3dScriptFinger(int i, float x, float y, bool active) {
    if (i>=0 && i<W3D_MAXDEDOS){ gFingerX[i]=x; gFingerY[i]=y; gFingerAct[i]=active; }
    if (active) gTouch = true;
}
static bool PlatformTouch() {
#if defined(__ANDROID__)
    return true;
#elif defined(__EMSCRIPTEN__)
    static int yes = -1;
    if (yes < 0) yes = EM_ASM_INT({ return (navigator.maxTouchPoints > 0 || ('ontouchstart' in window)) ? 1 : 0; });
    return yes != 0;
#else
    return false;
#endif
}
// RATON: posicion del mouse en el lienzo del juego (px, centro 0,0) SIN necesidad de clic -> mouse-over
// de los menus. 'dentro' = el mouse esta sobre la pantalla del juego. El clic llega por finger(1)/tap().
static float gMouseX = 0, gMouseY = 0; static bool gMouseInside = false;
void W3dScriptMouse(float x, float y, bool inside) { gMouseX = x; gMouseY = y; gMouseInside = inside; }
// MOUSE CAPTURADO (juego estilo FPS en PC): captureMouse(true) desde lua pide que la plataforma esconda el cursor
// y entregue el movimiento RELATIVO (SDL_SetRelativeMouseMode en main.cpp; ESC pausa el juego y lo suelta).
// mouseRel() devuelve el delta acumulado en px desde la ultima lectura y lo deja en cero. En Symbian no hay
// mouse: mouseRel() da 0,0 y el script sigue con el dedo / las flechas.
static float gMouseDX = 0, gMouseDY = 0; static bool gMouseCapture = false;
void W3dScriptMouseRel(float dx, float dy) { gMouseDX += dx; gMouseDY += dy; }
bool W3dScriptMouseCapture() { return gMouseCapture; }
static bool gMouseCaptured = false;   // lo que la plataforma logro de verdad (PC: cursor oculto y clavado al centro)
void W3dScriptCapturedMouseSet(bool v) { gMouseCaptured = v; }

// ---- funciones BASE que ven todos los scripts ------------------------------
// key("w") -> true si esta apretada
static int LKey(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    std::map<std::string, bool>::iterator it = gKeys.find(n);
    lua_pushboolean(L, it != gKeys.end() && it->second);
    return 1;
}
// helper: estado (apretado?) de un nombre en un mapa (false si no esta)
static bool KeyState(std::map<std::string, bool>& m, const char* n) {
    std::map<std::string, bool>::iterator it = m.find(n);
    return it != m.end() && it->second;
}
// keyPressed("w") -> true SOLO en el frame en que la tecla pasa de suelta a apretada (flanco de subida).
// Gemelo de pressed() (que es tactil): usa gKeys (estado actual) vs gKeysPrev (snapshot del frame anterior),
// y tambien cuenta la que bajo y subio entre dos frames (gPressedKeys: un juego lento no pierde el tiro).
static int LPressedKey(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    lua_pushboolean(L, (KeyState(gKeys, n) && !KeyState(gKeysPrev, n)) || KeyState(gPressedKeys, n));
    return 1;
}
// el mando pedido por un bind (el 2do argumento, 1..W3D_GAMEPADS): -1 = todos mezclados (sin argumento)
static int GamepadArg(lua_State* L, int i) {
    if (lua_isnoneornil(L, i)) return -1;
    const int m = (int)luaL_checkinteger(L, i) - 1;
    return (m >= 0 && m < W3D_GAMEPADS) ? m : -2;   // (-2: uno que no existe: todo suelto)
}
// buttonPressed("a" [, mando]) -> idem para los botones del gamepad (mismo indexado por nombre que button()).
static int LPressedButton(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    const int m = GamepadArg(L, 2);
    if (m == -1) lua_pushboolean(L, KeyState(gButtonsPad, n) && !KeyState(gButtonsPadPrev, n));
    else lua_pushboolean(L, m >= 0 && KeyState(gButtonsM[m], n) && !KeyState(gButtonsMPrev[m], n));
    return 1;
}
// random() -> real en [0,1);  random(min,max) -> ENTERO en [min,max] inclusive.
// Delega en math.random del PROPIO lua_State -> comparte el generador (xoshiro256**) ya sembrado, asi
// random() y math.random varian juntos entre ejecuciones. C++03-safe (sin C++11).
static void PushMathRandom(lua_State* L) {   // deja solo math.random en el stack
    lua_getglobal(L, "math");
    lua_getfield(L, -1, "random");
    lua_replace(L, -2);
}
static int LRandom(lua_State* L) {
    if (lua_gettop(L) >= 2) {
        lua_Integer lo = luaL_checkinteger(L, 1);
        lua_Integer hi = luaL_checkinteger(L, 2);
        if (hi < lo) { lua_Integer t = lo; lo = hi; hi = t; }   // tolera min>max (lo damos vuelta)
        lua_settop(L, 0);
        PushMathRandom(L);
        lua_pushinteger(L, lo);
        lua_pushinteger(L, hi);
        lua_call(L, 2, 1);                   // math.random(lo, hi) -> entero inclusive
        return 1;
    }
    lua_settop(L, 0);
    PushMathRandom(L);
    lua_call(L, 0, 1);                       // math.random() -> [0,1)
    return 1;
}

// ---- LA HORA DEL SISTEMA ------------------------------------------------------------------------------------------------
// unixTime() -> segundos, ms: el TIEMPO UNIX (los segundos desde el 1/1/1970 00:00 UTC, el reloj de la maquina) y los
// milisegundos de ese segundo (0..999). Van en DOS enteros a proposito: con el lua de 32 bits (LUA_32BITS, la Dreamcast)
// un solo numero con decimales no alcanza para los segundos de hoy (~1.800.000.000) sin perder minutos. Con dos enteros
// la cuenta es exacta en todas las plataformas:
//   local s0, ms0 = unixTime()                      -- en start(): la hora de arranque
//   local s, ms = unixTime()                        -- despues
//   local paso = (s - s0) + (ms - ms0) / 1000       -- segundos que pasaron, con decimales
// Es el reloj de PARED: mide el tiempo real aunque el juego vaya lento, pero si alguien le cambia la hora a la maquina,
// salta. Para medir cuanto tarda un pedazo de codigo esta clockMs().
static void UnixNow(lua_Integer* s, int* ms) {
#if defined(_WIN32) && !defined(W3D_SYMBIAN)
    struct _timeb tb;
    _ftime(&tb);
    *s = (lua_Integer)tb.time;
    *ms = (int)tb.millitm;
#else
    struct timeval tv;
    if (gettimeofday(&tv, 0) == 0) {
        *s = (lua_Integer)tv.tv_sec;
        *ms = (int)(tv.tv_usec / 1000);
    } else {
        *s = (lua_Integer)time(NULL);
        *ms = 0;
    }
#endif
}
static int LUnixTime(lua_State* L) {
    lua_Integer s = 0;
    int ms = 0;
    UnixNow(&s, &ms);
    lua_pushinteger(L, s);
    lua_pushinteger(L, ms);
    return 2;
}
// unixTimeMs() -> milisegundos y unixTimeSec() -> segundos con decimales: el MISMO tiempo UNIX en un solo numero, para
// que medir sea una resta:
//   local inicio = unixTimeSec()            -- en start()
//   local t = unixTimeSec() - inicio        -- despues: los segundos que pasaron (con los milisegundos)
// En un lua de 64 bits (todas las plataformas salvo la Dreamcast) son exactos: los ms como ENTERO y los segundos en un
// double (exacto al milisegundo por miles de anios). Con el lua de 32 bits (LUA_32BITS, la Dreamcast) un solo numero no
// alcanza para el tiempo de hoy y pierden precision: ahi va unixTime(), que da los dos enteros.
static int LUnixTimeMs(lua_State* L) {
    lua_Integer s = 0;
    int ms = 0;
    UnixNow(&s, &ms);
    if (sizeof(lua_Integer) >= 8) lua_pushinteger(L, s * 1000 + ms);
    else                          lua_pushnumber(L, (lua_Number)s * 1000 + ms);   // (32 bits: aproximado)
    return 1;
}
static int LUnixTimeSec(lua_State* L) {
    lua_Integer s = 0;
    int ms = 0;
    UnixNow(&s, &ms);
    lua_pushnumber(L, (lua_Number)s + (lua_Number)ms / 1000);
    return 1;
}
// date([segundos [, utc]]) -> tabla {year, month, day, hour, min, sec, wday, yday}: la fecha y la hora de ese tiempo
// UNIX (sin argumentos o con nil: AHORA), en la hora LOCAL de la maquina, o en UTC si utc es true. month 1..12,
// day 1..31, hour 0..23, wday 1..7 (1 = domingo, como os.date de lua), yday 1..366. Es la parte segura de
// os.date("*t"): la libreria os no se abre (un juego no toca el disco).
static int LDate(lua_State* L) {
    time_t t;
    if (lua_isnoneornil(L, 1)) t = time(NULL);
    else t = (time_t)floor(luaL_checknumber(L, 1));
    const bool utc = lua_toboolean(L, 2) != 0;
    struct tm* tmv = utc ? gmtime(&t) : localtime(&t);
    if (!tmv) tmv = gmtime(&t);   // (una plataforma sin zona horaria: queda en UTC)
    if (!tmv) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, tmv->tm_year + 1900); lua_setfield(L, -2, "year");
    lua_pushinteger(L, tmv->tm_mon + 1);     lua_setfield(L, -2, "month");
    lua_pushinteger(L, tmv->tm_mday);        lua_setfield(L, -2, "day");
    lua_pushinteger(L, tmv->tm_hour);        lua_setfield(L, -2, "hour");
    lua_pushinteger(L, tmv->tm_min);         lua_setfield(L, -2, "min");
    lua_pushinteger(L, tmv->tm_sec);         lua_setfield(L, -2, "sec");
    lua_pushinteger(L, tmv->tm_wday + 1);    lua_setfield(L, -2, "wday");
    lua_pushinteger(L, tmv->tm_yday + 1);    lua_setfield(L, -2, "yday");
    return 1;
}
// el hook de PREFABS (ver W3dScript.h): lo instala el importador del editor
Object* (*W3dInstantiatePrefabHook)(const char* name, const float* posMotor) = 0;
// ...y el de los prefabs del proyecto (io/Prefabs) + quien corre la partida (ver W3dScript.h)
W3dPrefabInstantiateFn W3dPrefabInstantiateHook = 0;
Object* W3dPrefabInstantiateOwner = 0;
Object* W3dPrefabInstantiateParent = 0;
// ...y los de los scripts de una LIBRERIA externa (ver W3dScript.h)
std::string (*W3dScriptLibHook)(Object* owner) = 0;
std::string (*W3dScriptLoosePathHook)(const std::string& lib, const std::string& path) = 0;
std::string W3dScriptLib(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    if (!L) return std::string();
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_lib");
    const char* s = lua_isstring(L, -1) ? lua_tostring(L, -1) : 0;
    const std::string lib = s ? s : "";
    lua_pop(L, 1);
    return lib;
}
std::string W3dScriptLibPath(void* L, const std::string& path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos) return std::string();
    const std::string lib = W3dScriptLib(L);
    if (lib.empty()) return std::string();
    // una ENTRADA de su libreria (en el juego compilado: su copia empaquetada, donde viajan tambien sus sueltos)
    const std::string r = "lib:" + lib + "/" + path;
    if (w3dFileSystem::FileExists(r)) return r;
    // un archivo SUELTO al lado de su .w3d (el editor)
    if (W3dScriptLoosePathHook) {
        const std::string d = W3dScriptLoosePathHook(lib, path);
        if (!d.empty()) return d;
    }
    return std::string();
}
static W3dScriptObjectFn gNewObjectFn = 0;
static W3dScriptObjectFn gObjectDestroyFn = 0;
void W3dScriptSetNewObject(W3dScriptObjectFn fn) { gNewObjectFn = fn; }
void W3dScriptSetObjectDestroy(W3dScriptObjectFn fn) { gObjectDestroyFn = fn; }
// ---- el STREAMING por las rutas de instantiate()/destroy() (ver W3dScript.h) ----
bool W3dScriptHasSession() { return gNewObjectFn != 0; }
bool W3dScriptNotifyNewObject(Object* root) {
    if (!gNewObjectFn || !root) return false;
    gNewObjectFn(root);
    return true;
}
static W3dScriptObjectFn gObjectFreeFn = 0;
void W3dScriptSetObjectFree(W3dScriptObjectFn fn) { gObjectFreeFn = fn; }
void W3dScriptFreeNow(Object* o) {
    if (!o) return;
    if (gObjectFreeFn) gObjectFreeFn(o);
    else W3dScriptDestroyObject(o);
}
int (*W3dStreamingLuaHook)(Object* o, int request) = 0;
int (*W3dTexturesLuaHook)(int load) = 0;

// ---- LAS FUENTES DE LOS SCRIPTS como recurso del almacen (W3DREC_SCRIPT, ver W3dScript.h) ----
static void* OpLoadSource(const std::string& id, long* bytes) {
    std::vector<unsigned char>* v = new std::vector<unsigned char>();
    if (!w3dFileSystem::ReadFileBytes(id, *v) || v->empty()) { delete v; return 0; }
    if (bytes) *bytes = (long)v->size();
    return v;
}
static bool OpFreeSource(void* data) {
    delete (std::vector<unsigned char>*)data;
    return true;
}
void W3dScriptSourcesEnsureOps() {
    static bool ready = false;
    if (ready) return;
    W3dResourceOps ops;
    ops.Load = OpLoadSource;
    ops.Free = OpFreeSource;
    W3dResourcesRegisterType(W3DREC_SCRIPT, ops);
    ready = true;
}

// ---- LA FRONTERA DE EJES Lua <-> motor (ver W3dLuaAxes en W3dScript.h) --------------------------------------------
// Lua ve Z ARRIBA (los ejes del panel de Propiedades); el motor sigue con Y arriba. TODO vector 3D que entra o sale
// por un bind de este archivo pasa por W3dLuaAxes: el (x, y, z) de Lua es el (x, z, y) del motor. Estas dos ayudas
// cubren el caso comun (tres numeros seguidos en la pila); los demas binds llaman a W3dLuaAxes a mano.
// lee los numeros i, i+1, i+2 de la pila (un vector de Lua) y los devuelve en ejes del MOTOR
static Vector3 ReadVec3Lua(lua_State* L, int i) {
    float x = (float)luaL_checknumber(L, i);
    float y = (float)luaL_checknumber(L, i + 1);
    float z = (float)luaL_checknumber(L, i + 2);
    W3dLuaAxes(y, z);
    return Vector3(x, y, z);
}
// empuja un vector del MOTOR como lo ve Lua (x, z, y): tres valores en la pila
static void PushVec3Lua(lua_State* L, float x, float y, float z) {
    W3dLuaAxes(y, z);
    lua_pushnumber(L, x); lua_pushnumber(L, y); lua_pushnumber(L, z);
}

// instantiate("nombre" [, x, y, z [, rotZ [, padre]]]) -> clona un Prefab de la
// Biblioteca del .w3d como hijo de la escena y devuelve el objeto (light userdata,
// como object()). x/y/z en los ejes de LUA (Z arriba): se pasan al motor con
// W3dLuaAxes antes del hook (que recibe coords del MOTOR). rotZ = el giro en grados
// alrededor del eje VERTICAL (la Z de Lua, la Y del motor: el mismo numero, no se
// convierte). nil = no existe el prefab (o el recurso todavia no esta LISTO):
// reintentar el proximo frame.
static int LInstantiate(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    float pos[3]; const float* pp = 0;
    if (lua_gettop(L) >= 4) {
        const Vector3 p = ReadVec3Lua(L, 2);
        pos[0] = p.x; pos[1] = p.y; pos[2] = p.z;
        pp = pos;
    }
    // un PREFAB DEL PROYECTO primero (con el giro vertical opcional, el 5to argumento); si no hay, el de la
    // Biblioteca del formato texto (que no gira ni avisa a la partida: queda como estaba)
    Object* o = 0;
    if (W3dPrefabInstantiateHook) {
        // (el giro alrededor de la vertical: la Z de Lua es la Y del motor, el hook lo espera como rotY)
        const float rotY = (lua_gettop(L) >= 5) ? (float)luaL_checknumber(L, 5) : 0.0f;
        // (quien llama: un script de una libreria externa nombra primero los prefabs de SU libreria)
        lua_getfield(L, LUA_REGISTRYINDEX, "w3d_owner");
        W3dPrefabInstantiateOwner = lua_islightuserdata(L, -1) ? (Object*)lua_touserdata(L, -1) : 0;
        lua_pop(L, 1);
        // (el padre, si vino: un objeto vivo; otra cosa = la raiz activa)
        W3dPrefabInstantiateParent = (lua_gettop(L) >= 6) ? W3dScriptParamObject((void*)L, 6) : 0;
        o = W3dPrefabInstantiateHook(n, pp, rotY);
        W3dPrefabInstantiateOwner = 0;
        W3dPrefabInstantiateParent = 0;
        if (o && gNewObjectFn) gNewObjectFn(o);   // sus scripts, sus cuerpos: los arranca la partida
    }
    if (!o && W3dInstantiatePrefabHook) o = W3dInstantiatePrefabHook(n, pp);
    if (o) lua_pushlightuserdata(L, o); else lua_pushnil(L);
    return 1;
}

// ---- destroy(obj): DIFERIDO al final del frame (ver W3dScript.h) ----
// lo pedido va por SERIAL: si el objeto se libera antes por otro camino (un changeScene, otro destruir de
// un ancestro) el pedido se descarta solo
struct W3dDestroyRequest { Object* o; unsigned serial; };
static std::vector<W3dDestroyRequest> gDestroy;
static int LDestroy(lua_State* L) {
    Object* o = W3dScriptParamObject((void*)L, 1);
    extern Object* SceneCollection;
    if (!o || o == SceneCollection || W3dIsRootRegistered(o) || W3dScriptIsSceneUI(o)) { lua_pushboolean(L, 0); return 1; }
    for (size_t i = 0; i < gDestroy.size(); i++)
        if (gDestroy[i].o == o && gDestroy[i].serial == o->serial) { lua_pushboolean(L, 1); return 1; }
    W3dDestroyRequest p; p.o = o; p.serial = o->serial;
    gDestroy.push_back(p);
    lua_pushboolean(L, 1);
    return 1;
}

// ---- streamLoad(obj) / streamUnload(obj) / streamAuto(obj) / isLoaded(obj): el STREAMING de una instancia de prefab o un
// proxy (main/io/Streaming.h, por el hook). Los tres primeros devuelven true si 'obj' es una instancia (el pedido
// queda anotado: se aplica en el tick del streaming); isLoaded() dice si sus hijos estan generados. nil = sin streaming
static int LStreaming(lua_State* L, int request) {
    Object* o = W3dScriptParamObject((void*)L, 1);
    if (!W3dStreamingLuaHook || !o) { lua_pushnil(L); return 1; }
    const int e = W3dStreamingLuaHook(o, request);
    if (request == 0) lua_pushboolean(L, e == 2);
    else lua_pushboolean(L, e >= 0);
    return 1;
}
// pendingTextures() -> cuantas texturas esperan en la cola diferida (sube una por cuadro). loadTextures() -> las sube
// TODAS ya (bloquea: en el N95 cada pagina de atlas tarda) y devuelve las que quedan (0). Para el principio de una escena
// pedida con changeScene(): sin esto lo que aparece se ve blanco hasta que llega su textura
static int LPendingTextures(lua_State* L) { lua_pushinteger(L, W3dTexturesLuaHook ? W3dTexturesLuaHook(0) : 0); return 1; }
static int LLoadTextures(lua_State* L)     { lua_pushinteger(L, W3dTexturesLuaHook ? W3dTexturesLuaHook(1) : 0); return 1; }
static int LLoad(lua_State* L)    { return LStreaming(L, 1); }
static int LUnload(lua_State* L) { return LStreaming(L, -1); }
static int LLoadAuto(lua_State* L) { return LStreaming(L, 2); }
static int LLoaded(lua_State* L)   { return LStreaming(L, 0); }

// find("nombre") -> el objeto de la escena con ese nombre (busqueda recursiva
// por el arbol), o nil. Es la via para agarrar lo que entro EN RUNTIME
// (importW3D), que no puede estar cableado en `propiedades` porque no existia
// cuando el editor resolvio las referencias.
static Object* FindRecName(Object* parent, const char* n) {
    if (!parent) return NULL;
    for (size_t i = 0; i < parent->Childrens.size(); i++) {
        Object* h = parent->Childrens[i];
        if (!h) continue;
        if (h->name == n) return h;
        Object* r = FindRecName(h, n);
        if (r) return r;
    }
    return NULL;
}
// selected() -> { obj, obj, ... }: los objetos ELEGIDOS en el editor (el activo incluido), en orden de seleccion.
// Para los botones de las propiedades usados como complementos del editor: "a lo que este elegido, hacele esto".
static int LSelected(lua_State* L) {
    lua_newtable(L);
    int k = 1;
    for (size_t i = 0; i < ObjSelects.size(); i++) {
        if (!ObjSelects[i]) continue;
        lua_pushlightuserdata(L, ObjSelects[i]);
        lua_rawseti(L, -2, k++);
    }
    return 1;
}
static int LFind(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    extern Object* SceneCollection;
    // find("nombre" [, raiz]): con raiz busca SOLO en ese subarbol. Es la
    // manera de agarrar la pieza de UN auto cuando hay dos importados con las
    // mismas partes ("door_lf_dummy" del taxi Y del patrullero).
    Object* root = SceneCollection;
    if (lua_islightuserdata(L, 2)) {
        Object* r = W3dScriptParamObject((void*)L, 2);
        if (r) root = r;
    } else {
        // SIN raiz: como lo ve el objeto del script. Uno GENERADO por una instancia de prefab busca primero
        // adentro de SU instancia (su "Arma", no la del primer enemigo) y despues en toda la escena
        lua_getfield(L, LUA_REGISTRYINDEX, "w3d_owner");
        Object* owner = lua_islightuserdata(L, -1) ? (Object*)lua_touserdata(L, -1) : NULL;
        lua_pop(L, 1);
        if (owner && W3dInstanceOf(owner)) {
            Object* o = W3dFindNameFrom(owner, n, SceneCollection);
            if (o) lua_pushlightuserdata(L, o); else lua_pushnil(L);
            return 1;
        }
    }
    if (root && root->name == n) { lua_pushlightuserdata(L, root); return 1; }
    Object* o = FindRecName(root, n);
    if (o) lua_pushlightuserdata(L, o); else lua_pushnil(L);
    return 1;
}

// worldPoint(obj, lx,ly,lz) -> x,y,z : transforma un punto LOCAL del objeto a
// MUNDO (cadena completa de padres). Sirve para anclar cosas a un objeto que
// la fisica mueve y rota (el asiento de un auto, la punta de un canio).
// Entrada y salida en los ejes de Lua (Z arriba; W3dLuaAxes en la frontera).
static int LPointWorld(lua_State* L) {
    Object* o = W3dScriptParamObject((void*)L, 1);
    if (!o) { lua_pushnil(L); return 1; }
    Vector3 w = o->LocalToWorld(ReadVec3Lua(L, 2));
    PushVec3Lua(L, w.x, w.y, w.z);
    return 3;
}

// animScene("nombre" [, loop=true]) -> reproduce una ANIMACION DE ESCENA (las
// curvas de transform de objetos) durante el juego, por nombre. Devuelve la
// duracion en SEGUNDOS (para armar timers en lua), o nil si no existe. loop
// false = one-shot: al terminar queda clavada en la ultima pose. UNA activa por
// vez; el tick lo corre la simulacion (W3dAnimSceneTick).
// stopScene(): corta la ANIMACION DE ESCENA que este sonando (animScene). Los objetos quedan donde estaban y
// su pista "Camara activa" deja de mandar (sino una one-shot terminada seguia imponiendo su ultima camara).
// animSceneFrame(f): fija el frame de la animacion de escena que suena y apaga su reloj propio (el script manda).
// Una cinematica lo llama cada tick con el MISMO frame que usa para los clips de los personajes (animFrame).
static int LAnimSceneFrame(lua_State* L) {
    W3dAnimSceneSet((float)luaL_checknumber(L, 1));
    return 0;
}
// animSceneCurrent() -> frame, nombre de la animacion de escena que esta sonando (nil si ninguna). Lo
// usa una CINEMATICA para saber cuando termino su animacion (una ESCENA pedida con changeScene la
// reproduce una vez desde el inicio) y pedir la siguiente.
static int LAnimCurrentScene(lua_State* L) {
    const int i = W3dAnimScenePlaying();
    if (i < 0 || i >= (int)SceneAnimations.size() || !SceneAnimations[(size_t)i]) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (lua_Number)W3dAnimSceneFrame());
    lua_pushstring(L, SceneAnimations[(size_t)i]->name.c_str());
    return 2;
}
static int LStopScene(lua_State* L) {
    (void)L;
    W3dAnimSceneReset();
    return 0;
}
static int LAnimScene(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    const bool loop = lua_isnoneornil(L, 2) ? true : (lua_toboolean(L, 2) != 0);
    int idx = W3dAnimSceneIdx(n);
    if (idx < 0 || !W3dAnimScenePlay(idx, loop)) {
        static bool notified = false;   // una vez por sesion (no spamear el log)
        if (!notified) { notified = true; w3dLogfW("animEscena(): no existe la animacion '%s'", n); }
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, W3dAnimSceneDur(idx));
    return 1;
}

// ---- CLIPS DE JERARQUIA (animation/W3dAnimSet.h: una raiz anima a si misma y a sus hijos por ruta de nombres) ----
// animObject(raiz, "clip" [, loop=true]) -> duracion en SEGUNDOS, o nil si no hay un clip con ese nombre.
// El clip se busca en la biblioteca de la raiz; si no esta, se acepta "Biblioteca/clip" (o el nombre suelto
// si es el unico): cualquier jerarquia con los mismos nombres lo puede reproducir (lo que no tiene se ignora).
// Cada raiz tiene SU cabezal: varias animaciones suenan a la vez e independientes (la puerta, el ascensor,
// el auto), y conviven con animScene() y con los clips de los armatures (animClip). Una raiz puede estar
// ADENTRO de otra que suena (la puerta del auto con su propio clip): se posa primero el ancestro y la pista
// "." de la de adentro se suma a lo que el le hace, sin importar cual se arranco antes (Animation.cpp).
// loop false = una pasada: al terminar queda en la ultima pose (animObjectFinished da true).
// una INSTANCIA (de un prefab, de una ESCENA, un proxy) se anima como una UNIDAD: las funciones de clips de jerarquia
// le hablan a su objeto raiz generado (el que trae los clips) y las de esqueleto a su primer armature
static Object* ObjectAnimable(lua_State* L, int i) {
    Object* o = W3dScriptParamObject((void*)L, i);
    if (o && W3dIsInstanceType(o->getType()) && !o->Childrens.empty() && o->Childrens[0]) return o->Childrens[0];
    return o;
}
static Object* FirstArmature(Object* o) {
    if (!o) return NULL;
    if (o->getType() == ObjectType::armature) return o;
    for (size_t i = 0; i < o->Childrens.size(); i++) { Object* a = FirstArmature(o->Childrens[i]); if (a) return a; }
    return NULL;
}
static int LAnimObject(lua_State* L) {
    Object* o = ObjectAnimable(L, 1);
    const char* n = luaL_checkstring(L, 2);
    const bool loop = lua_isnoneornil(L, 3) ? true : (lua_toboolean(L, 3) != 0);
    const float dur = o ? W3dAnimObjectPlay(o, n, loop) : -1.0f;
    if (dur < 0.0f) {
        static bool notified = false;   // una vez por sesion (no spamear el log)
        if (!notified) { notified = true; w3dLogfW("animObjeto(): '%s' no tiene un clip '%s'", o ? o->name.c_str() : "?", n); }
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, dur);
    return 1;
}
// animObjectFrame(obj [, f]) -> el frame del clip que suena en el objeto (nil si ninguno). Con f mueve su
// cabezal ahi (acotado al rango del clip) y el clip sigue sonando desde ese frame.
static int LAnimObjectFrame(lua_State* L) {
    Object* o = ObjectAnimable(L, 1);
    if (o && !lua_isnoneornil(L, 2)) W3dAnimObjectSetFrame(o, (float)luaL_checknumber(L, 2));
    const float f = o ? W3dAnimObjectFrame(o) : -1.0f;
    if (f < 0.0f) lua_pushnil(L); else lua_pushnumber(L, f);
    return 1;
}
// animObjectFinished(obj) -> true si no le suena nada o si su clip de una pasada llego al final
static int LAnimEndedObject(lua_State* L) {
    Object* o = ObjectAnimable(L, 1);
    lua_pushboolean(L, W3dAnimObjectEnded(o) ? 1 : 0);
    return 1;
}
// animObjectCurrent(obj) -> el nombre del clip que suena (nil si ninguno)
static int LAnimCurrentObject(lua_State* L) {
    Object* o = ObjectAnimable(L, 1);
    const std::string c = o ? W3dAnimObjectClip(o) : std::string();
    if (c.empty()) lua_pushnil(L); else lua_pushstring(L, c.c_str());
    return 1;
}
// animObjectStop(obj): corta su clip (el objeto queda donde esta)
static int LAnimObjectStop(lua_State* L) {
    Object* o = ObjectAnimable(L, 1);
    if (o) W3dAnimObjectStop(o);
    return 0;
}
// animObjectSpeed(raiz, v) -> true si le suena un clip. 1 = sus fps, 2 = el doble, negativa = hacia atras
static int LAnimObjectVelocity(lua_State* L) {
    Object* o = ObjectAnimable(L, 1);
    const float v = (float)luaL_checknumber(L, 2);
    lua_pushboolean(L, (o && W3dAnimObjectVelocity(o, v)) ? 1 : 0);
    return 1;
}
// animRetarget(obj, "completo" | "rotaciones") : REUTILIZAR ANIMACIONES ENTRE TAMANOS DISTINTOS. Con
// "rotaciones" el armature (o la jerarquia) usa solo las rotaciones de sus clips: cada hueso/nodo conserva su
// traslacion y su escala de reposo (sus proporciones) y el movimiento de la raiz se escala por la razon de
// tamanos (el suyo / el del esqueleto que grabo el clip). "completo" (el de siempre) = el clip tal cual.
// Tambien acepta 0 / 1. En una jerarquia, "clip" vuelve al modo que diga cada clip.
static int LAnimRetarget(lua_State* L) {
    Object* o = W3dScriptParamObject((void*)L, 1);
    if (!o) return 0;
    int mode = W3D_RETARGET_COMPLETE;
    if (lua_type(L, 2) == LUA_TNUMBER) mode = (int)lua_tonumber(L, 2);
    else {
        const char* m = luaL_optstring(L, 2, "complete");
        if (!strcmp(m, "rotations")) mode = W3D_RETARGET_ROTATIONS;
        else if (!strcmp(m, "clip")) mode = -1;
    }
    if (o->getType() == ObjectType::armature) W3dArmatureRetarget((Armature*)o, mode < 0 ? W3D_RETARGET_COMPLETE : mode);
    else W3dAnimObjectRetarget(o, mode);
    return 0;
}

// ---- PANTALLA DIVIDIDA (dos jugadores en el mismo equipo): el juego pide 2 vistas y la camara de cada una; el render
// (el viewport del editor, el runtime compilado) dibuja la escena una vez por vista en su mitad del cuadro y el HUD
// una vez encima. Antes de cada vista llama a beforeView(i) en el script que las pidio: ahi el juego da vuelta hacia
// ESA camara lo que mira a la camara (llamas, chispas, lineas) y esconde lo que esa vista no tiene que ver
static int gViews = 1, gViewsMode = 0;          // 1 o 2; 0 = lado a lado, 1 = una arriba de la otra
static Object* gViewCam[2] = { NULL, NULL };
static Object* gViewsOwner = NULL;
static int gViewDrawing = 0;
int W3dViewsCount() { return gViews; }
int W3dViewsMode() { return gViewsMode; }
Object* W3dCameraView(int i) { return (i >= 0 && i < 2) ? gViewCam[i] : NULL; }
int W3dViewDrawing() { return gViewDrawing; }
void W3dViewBefore(int i) {
    gViewDrawing = i + 1;
    if (!gViewsOwner) return;
    const float n = (float)(i + 1);
    W3dScriptEvent(gViewsOwner, "beforeView", (Object* const*)NULL, 0, &n, 1);
}
void W3dViewAfter() { gViewDrawing = 0; }
static void ViewsRestart() {
    gViews = 1; gViewsMode = 0; gViewCam[0] = gViewCam[1] = NULL; gViewsOwner = NULL; gViewDrawing = 0;
}
// setViews(n [, "side" | "up"]): 1 = la pantalla entera (lo normal), 2 = dividida (lado a lado o una arriba de la
// otra). El script que la llama recibe beforeView(i) antes de que se dibuje cada una
static int LSetViews(lua_State* L) {
    const int n = (int)luaL_checkinteger(L, 1);
    const char* mode = luaL_optstring(L, 2, "side");
    gViews = n >= 2 ? 2 : 1;
    gViewsMode = (mode && mode[0] == 'u') ? 1 : 0;   // "up": una arriba de la otra
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_owner");
    gViewsOwner = (Object*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    W3dScriptRedraw();
    return 0;
}
// setCameraView(i, camara): la camara de la vista i (1 o 2). Sin poner, la activa
static int LSetCameraView(lua_State* L) {
    const int i = (int)luaL_checkinteger(L, 1) - 1;
    if (i < 0 || i > 1) return 0;
    gViewCam[i] = W3dScriptParamObject(L, 2);
    W3dScriptRedraw();
    return 0;
}
// view() -> la vista que se esta dibujando (1 o 2; 0 fuera del dibujo) y cuantas hay
static int LView(lua_State* L) {
    lua_pushinteger(L, gViewDrawing);
    lua_pushinteger(L, gViews);
    return 2;
}

// self() -> el objeto que tiene ESTE script (el mismo script puesto en varios objetos: cada instancia se maneja a si
// misma sin buscarse por nombre)
static int LYo(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_owner");
    if (!lua_islightuserdata(L, -1)) { lua_pop(L, 1); lua_pushnil(L); }
    return 1;
}

// object("prop") -> la referencia expuesta (light userdata; nil si no se asigno)
static int LObject(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_inst");
    W3dScriptInst* inst = (W3dScriptInst*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (inst) {
        std::map<std::string, Object*>::iterator it = inst->refs.find(n);
        if (it != inst->refs.end() && it->second) {
            lua_pushlightuserdata(L, it->second);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// option("dificultad") -> la opcion elegida en el editor. Si no se eligio ninguna, la DE FABRICA: la primera de la
// lista declarada en `propiedades` (dificultad = { "facil", "normal", "dificil" } -> "facil"), la misma que muestra
// el panel de Propiedades. Antes devolvia "" y el juego no hacia lo que se veia en el editor. "" solo si no hay
// lista declarada.
static int LOption(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_inst");
    W3dScriptInst* inst = (W3dScriptInst*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (inst) {
        std::map<std::string, std::string>::iterator it = inst->options.find(n);
        if (it != inst->options.end() && !it->second.empty()) {
            lua_pushstring(L, it->second.c_str());
            return 1;
        }
    }
    // la de fabrica: propiedades[n][1], si es un texto
    lua_getglobal(L, "properties");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, n);
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            if (lua_type(L, -1) == LUA_TSTRING) {
                lua_replace(L, -3);         // queda el texto donde estaba `propiedades`
                lua_pop(L, 1);              // saco la lista
                return 1;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushstring(L, "");
    return 1;
}

// property("frame") -> el VALOR configurado en el editor para ESTA instancia,
// convertido al tipo del default declarado en la tabla `propiedades` (numero ->
// number, bool -> boolean, texto -> string). Si la instancia no configuro nada,
// devuelve el default del propio script (nil si tampoco esta declarado).
static int LProperty(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_inst");
    W3dScriptInst* inst = (W3dScriptInst*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    // el default declarado (y su TIPO) sale de la tabla global `propiedades`
    // del propio lua_State: el script ya corrio, la tabla esta viva
    int tdecl = LUA_TNIL;
    lua_getglobal(L, "properties");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, n);
        tdecl = lua_type(L, -1);
        lua_remove(L, -2);          // saco la tabla, queda el default en el tope
    } else {
        lua_pop(L, 1);
        lua_pushnil(L);             // sin tabla: el default es nil
    }
    const std::string* cfg = NULL;
    if (inst) {
        std::map<std::string, std::string>::iterator it = inst->values.find(n);
        if (it != inst->values.end()) cfg = &it->second;
    }
    if (!cfg) return 1;             // sin valor por instancia: va el default declarado
    lua_pop(L, 1);                  // hay configurado: el default no hace falta
    if (tdecl == LUA_TNUMBER) {
        // lua_stringtonumber empuja el numero si el texto parsea ("3", "0.5")
        if (!lua_stringtonumber(L, cfg->c_str())) lua_pushnumber(L, 0);
    } else if (tdecl == LUA_TBOOLEAN) {
        lua_pushboolean(L, (*cfg == "true" || *cfg == "1") ? 1 : 0);
    } else {
        lua_pushstring(L, cfg->c_str());
    }
    return 1;
}

// LAS PROPIEDADES COMO VARIABLES: un nombre que el script NO definio pero que esta en su tabla `properties` se lee
// con la funcion que le corresponde: speed -> property("speed"), difficulty (una lista) -> option("difficulty"),
// ball ("object") -> object("ball"), y un boton, su funcion. Asi alcanza con escribir `speed` en vez de
// property("speed"). Es el __index de la tabla global: Lua solo lo consulta con lo que NO es una variable del script,
// asi que lo de siempre (la API, las variables) no cambia ni cuesta mas. Se lee en el momento: ve lo que se cambie en
// Propiedades con el juego andando. `speed = 5` crea una variable del script, que desde ahi tapa a la propiedad
static int LGlobalIndex(lua_State* L) {
    // (1 = la tabla global, 2 = el nombre)
    if (lua_type(L, 2) != LUA_TSTRING) { lua_pushnil(L); return 1; }
    lua_pushliteral(L, "properties");
    lua_rawget(L, 1);                            // (rawget: sin volver a pasar por aca)
    if (!lua_istable(L, -1)) { lua_pushnil(L); return 1; }
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);                           // properties[nombre]: lo declarado
    lua_CFunction read = LProperty;              // numero, si/no o texto
    switch (lua_type(L, -1)) {
        case LUA_TNIL:      return 1;            // no es una propiedad: nil, como cualquier global sin definir
        case LUA_TFUNCTION: return 1;            // un boton: su funcion
        case LUA_TSTRING:
            if (strcmp(lua_tostring(L, -1), "object") == 0) read = LObject;
            break;
        case LUA_TTABLE: {                       // una lista de opciones, o un boton { "texto", funcion }
            const int n = (int)lua_rawlen(L, -1);
            for (int i = 1; i <= n; i++) {
                lua_rawgeti(L, -1, i);
                if (lua_isfunction(L, -1)) return 1;   // el boton: su funcion
                lua_pop(L, 1);
            }
            read = LOption;
            break;
        }
        default: break;
    }
    lua_settop(L, 2);
    lua_pushcfunction(L, read);
    lua_pushvalue(L, 2);
    lua_call(L, 1, 1);
    return 1;
}
static void PropertiesAsVariables(lua_State* L) {
    lua_pushglobaltable(L);
    lua_newtable(L);
    lua_pushcfunction(L, LGlobalIndex);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);
}

// getShared("suma") -> el valor del mapa compartido entre scripts (nil si no
// existe). Es la lectura del canal unico entre lua_States (ver W3dScript.h).
static int LShared(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    std::map<std::string, W3dSharedVal>::iterator it = gShared.find(n);
    if (it == gShared.end()) { lua_pushnil(L); return 1; }
    if (it->second.type == 0)      lua_pushnumber(L, (lua_Number)it->second.num);
    else if (it->second.type == 1) lua_pushboolean(L, it->second.b ? 1 : 0);
    else                           lua_pushstring(L, it->second.txt.c_str());
    return 1;
}
// setShared("suma", v): escribe en el mapa compartido (numero/bool/string;
// nil borra la clave). Cualquier script del juego lo ve con getShared().
static int LSetShared(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    int t = lua_type(L, 2);
    if (t == LUA_TNIL || t == LUA_TNONE) { gShared.erase(n); return 0; }
    W3dSharedVal v;
    if (t == LUA_TNUMBER)       { v.type = 0; v.num = (double)lua_tonumber(L, 2); }
    else if (t == LUA_TBOOLEAN) { v.type = 1; v.b = lua_toboolean(L, 2) != 0; }
    else                        { v.type = 2; v.txt = lua_tostring(L, 2) ? lua_tostring(L, 2) : ""; }
    gShared[n] = v;
    return 0;
}

int W3dSharedScriptCount() { return (int)gShared.size(); }
bool W3dSharedScriptPair(int i, std::string* name, std::string* value) {
    if (i < 0 || i >= (int)gShared.size()) return false;
    std::map<std::string, W3dSharedVal>::iterator it = gShared.begin();
    for (int k = 0; k < i; k++) ++it;
    if (name) *name = it->first;
    if (value) {
        const W3dSharedVal& v = it->second;
        if (v.type == 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%g", v.num);
            *value = buf;
        } else if (v.type == 1) *value = v.b ? "true" : "false";
        else *value = v.txt;
    }
    return true;
}

// stick("left"|"right" [, mando]) -> x, y del stick analogico (-1..1, deadzone ya aplicada); sin mando, todos juntos
static int LStick(lua_State* L) {
    const char* n = luaL_optstring(L, 1, "left");
    int which = (n && n[0] == 'r') ? 1 : 0;   // ("right"; el refactor dejo la 'd' de "der": stick("right") leia el izquierdo)
    const int m = GamepadArg(L, 2);
    lua_pushnumber(L, m == -1 ? gSticks[which][0] : (m >= 0 ? gSticksM[m][which][0] : 0.0f));
    lua_pushnumber(L, m == -1 ? gSticks[which][1] : (m >= 0 ? gSticksM[m][which][1] : 0.0f));
    return 2;
}
// trigger("left"|"right" [, mando]) -> 0..1: el gatillo analogico del mando (LT / RT, L2 / R2): un pedal (acelerador, freno)
static int LTrigger(lua_State* L) {
    const char* n = luaL_optstring(L, 1, "right");
    const int which = (n && n[0] == 'l') ? 0 : 1;   // ("left"; con la 'i' de "izq", trigger("left") leia el derecho)
    const int m = GamepadArg(L, 2);
    lua_pushnumber(L, m == -1 ? gTriggers[which] : (m >= 0 ? gTriggersM[m][which] : 0.0f));
    return 1;
}
// tap() -> x, y, tocando: el finger/mouse sobre el juego (px del lienzo, centro 0,0)
// ---- tilt() y vibrate() (ver W3dScript.h) ----
static float gTilt[3] = { 0.0f, 0.0f, 0.0f };
static bool gTiltHas = false;
void W3dScriptTilt(float x, float y, float z) { gTilt[0] = x; gTilt[1] = y; gTilt[2] = z; gTiltHas = true; }
static int LTilt(lua_State* L) {
    if (!gTiltHas) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, gTilt[0]); lua_pushnumber(L, gTilt[1]); lua_pushnumber(L, gTilt[2]);
    return 3;
}
void (*W3dScriptVibrateHook)(int ms, float force) = 0;
void (*W3dScriptGamepadsVibrateHook)(int ms, float force) = 0;
static int LVibrate(lua_State* L) {
    const int ms = (int)luaL_optinteger(L, 1, 40);
    float f = (float)luaL_optnumber(L, 2, 1.0);
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    if (ms <= 0) return 0;
    if (W3dScriptVibrateHook) W3dScriptVibrateHook(ms, f);
    else if (W3dScriptGamepadsVibrateHook) W3dScriptGamepadsVibrateHook(ms, f);
    return 0;
}
static int LTap(lua_State* L) {
    lua_pushnumber(L, gFingerX[0]);
    lua_pushnumber(L, gFingerY[0]);
    lua_pushboolean(L, gFingerAct[0]);
    return 3;
}
// mouse(): posicion del mouse en el lienzo del juego (px, centro 0,0) + si esta DENTRO de la pantalla.
// Sirve para el mouse-over de los menus (el clic sigue viniendo por finger(1)/tap()).
static int LMouse(lua_State* L) {
    lua_pushnumber(L, gMouseX); lua_pushnumber(L, gMouseY); lua_pushboolean(L, gMouseInside);
    return 3;
}
// finger(n): n es 1-based (finger(1) = primer dedo). Devuelve x, y (px, centro 0,0) y si esta activo.
static int LFinger(lua_State* L) {
    int i = (int)luaL_optinteger(L, 1, 1) - 1;
    if (i < 0) i = 0; if (i >= W3D_MAXDEDOS) i = W3D_MAXDEDOS - 1;
    lua_pushnumber(L, gFingerX[i]);
    lua_pushnumber(L, gFingerY[i]);
    lua_pushboolean(L, gFingerAct[i]);
    return 3;
}
// touch(): la pantalla se toca (un telefono, una tableta, el navegador de un telefono) o ya se la toco: el juego
// puede mostrar controles en pantalla
static int LTouch(lua_State* L) {
    lua_pushboolean(L, gTouch || PlatformTouch());
    return 1;
}
// mouseRel() -> dx, dy: movimiento del mouse capturado (px) desde la ultima llamada; lo consume (queda en 0,0)
static int LMouseRel(lua_State* L) {
    lua_pushnumber(L, gMouseDX); lua_pushnumber(L, gMouseDY);
    gMouseDX = 0; gMouseDY = 0;
    return 2;
}
// captureMouse(true/false): pide (o suelta) la captura del mouse para mirar estilo FPS. En PC y en el navegador
// (pointer lock, ver main.cpp); en Symbian y Android no hace nada.
static int LCaptureMouse(lua_State* L) { gMouseCapture = lua_toboolean(L, 1) != 0; return 0; }
// mouseCaptured() -> true si la plataforma tiene el mouse capturado AHORA (para que el script no mire tambien
// con el arrastre del dedo mientras el cursor esta clavado al centro). En Symbian siempre false.
static int LMouseCaptured(lua_State* L) { lua_pushboolean(L, gMouseCaptured ? 1 : 0); return 1; }
// button("a") -> true si el boton del gamepad esta apretado
// beep(freq, ms, vol): reproduce un TONO de onda cuadrada (efecto estilo WhiskPaddle, copyright-free).
// freq en Hz, ms de duracion (default 90), vol 0..1 (default 1). Cachea el tono por (freq,ms).
static int LBeep(lua_State* L) {
    if (w3dEngine::ConfigMute()) return 0;   // mute global del Core: no suena (el juego no envuelve beep a mano)
    int freq = (int)luaL_checknumber(L, 1);
    int ms   = (int)luaL_optnumber(L, 2, 90);
    float vol = (float)luaL_optnumber(L, 3, 1.0);
    if (freq < 1) freq = 1; if (ms < 1) ms = 1; if (ms > 5000) ms = 5000;
    int key = (freq << 16) | (ms & 0xFFFF);
    std::map<int, w3dEngine::W3dSound*>::iterator it = gBeeps.find(key);
    w3dEngine::W3dSound* s = (it != gBeeps.end()) ? it->second : NULL;
    if (!s) { s = w3dEngine::W3dSoundBeep((float)freq, ms, 1.0f); gBeeps[key] = s; } // NULL si audio off -> cachea NULL
    if (s) w3dEngine::W3dSoundPlay(s, vol, false);
    return 0;
}
// button("a" [, mando]): sin mando, el de cualquiera
static int LButton(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    const int m = GamepadArg(L, 2);
    if (m == -1) lua_pushboolean(L, KeyState(gButtonsPad, n));
    else lua_pushboolean(L, m >= 0 && KeyState(gButtonsM[m], n));
    return 1;
}

// ---- CONFIG persistente + mute (facilidad del Core para los juegos lua) -----
// getConfig(clave, def) -> devuelve el string guardado, o 'def' si la clave no existe.
static int LConfig(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const char* def = luaL_optstring(L, 2, "");   // aunque pasen un number, optstring lo da como texto
    lua_pushstring(L, w3dEngine::ConfigGetStr(key, def));
    return 1;
}
// setConfig(clave, valor): guarda EN MEMORIA (acepta string o number). No persiste solo:
// llama saveConfig() cuando quieras escribir a disco/localStorage.
static int LSetConfig(lua_State* L) {
    const char* key   = luaL_checkstring(L, 1);
    const char* value = luaL_optstring(L, 2, "");   // optstring convierte un number a su texto
    w3dEngine::ConfigSetStr(key, value);
    return 0;
}
// saveConfig() -> escribe la config al almacenamiento de la plataforma. Devuelve true si pudo.
static int LSaveConfig(lua_State* L) { lua_pushboolean(L, w3dEngine::ConfigSave()); return 1; }
// loadConfig() -> relee la config del almacenamiento. Devuelve true si habia algo guardado.
static int LLoadConfig(lua_State* L) { lua_pushboolean(L, w3dEngine::ConfigLoad()); return 1; }
// mute(bool): prende/apaga el mute global (lo respeta beep()). Deja "mute" en la config
// (falta saveConfig() para que persista).
static int LSilence(lua_State* L) {
    bool m = lua_toboolean(L, 1) != 0;
    w3dEngine::ConfigSetMute(m);
    w3dEngine::ConfigSetInt("mute", m ? 1 : 0);
    return 0;
}
// isMuted() -> true si el sonido esta silenciado.
static int LIsMute(lua_State* L) { lua_pushboolean(L, w3dEngine::ConfigMute()); return 1; }

// ---- LOG para los juegos lua (info/warning/error/debugLog + la mini-consola) ---
// MODO DEBUG: la macro que manda es W3D_DEV_LOG (base/w3dlog.h). La regla:
//   * W3D_DEV_LOG=1 (el default del header) = build DEBUG: el log y el ring
//     buffer estan prendidos, debugLog() emite y isDebug() devuelve true.
//   * -DW3D_DEV_LOG=0 = build PRODUCCION: w3dLog* son stubs inline no-op, el
//     ring queda apagado (logCount() = 0, logLine() = ""), debugLog() es
//     no-op y isDebug() devuelve false. info()/warning()/error() tambien quedan
//     mudos porque el sink entero desaparece (build mas liviano).
// El checkbox "Modo debug" de la tarjeta Juego del editor define esta macro en
// los 4 targets generados por CompileGame (tildado = 1, destildado = 0). El
// EDITOR compila con el default (1): en Play siempre se ve todo.
static const char* LogText(lua_State* L) {
    const char* s = lua_tostring(L, 1);   // acepta string y number (lua convierte el number solo)
    return s ? s : "";
}
// info(msg) / warning(msg): al log del Core con nivel INFO / WARN (los mismos
// niveles que colorea el viewport Console del editor).
static int LLogInfo(lua_State* L)  { w3dLog(LogText(L));  return 0; }
static int LLogNotice(lua_State* L) { w3dLogW(LogText(L)); return 0; }
// error(msg): nivel ERROR. OJO: PISA el error() de la stdlib de lua (el que
// cortaba el script con una excepcion): en un juego Whisk3D error() LOGUEA y
// el script sigue. Ningun ejemplo usaba el error() nativo.
static int LLogError(lua_State* L) { w3dLogE(LogText(L)); return 0; }
// debugLog(msg): mensaje de DEBUG. SOLO emite en un build debug (W3D_DEV_LOG=1),
// como INFO con el prefijo "debug: "; en produccion es un no-op total.
static int LLogDebug(lua_State* L) {
#if W3D_DEV_LOG
    w3dLogf("debug: %s", LogText(L));
#else
    (void)L;
#endif
    return 0;
}
// isDebug() -> true si este build tiene el modo debug activo. El patron
// didactico: show(btnDepurar, isDebug()) -> mismo codigo, el build decide.
static int LIsDebug(lua_State* L) {
#if W3D_DEV_LOG
    lua_pushboolean(L, 1);
#else
    lua_pushboolean(L, 0);
#endif
    return 1;
}
// logCount() -> cuantas lineas guarda el ring buffer del log (0 en produccion).
static int LLogCount(lua_State* L) { lua_pushinteger(L, w3dLogRingCount()); return 1; }
// logLine(i) -> texto, nivel de la linea i del ring (1-based: 1 = la mas VIEJA).
// El nivel es "info" | "aviso" | "error" ("" si i esta fuera de rango o el ring
// esta apagado). Con esto un juego dibuja su propia mini-consola (ver ui/consola).
static int LLogLine(lua_State* L) {
    int i = (int)luaL_checkinteger(L, 1);
    const char* s = w3dLogRingLine(i - 1);   // el ring es 0-based; fuera de rango da ""
    // el ring guarda "[TAG] mensaje": separar el tag del texto para lua
    const char* level = "";
    if      (strncmp(s, "[INFO] ", 7) == 0)  { level = "info";  s += 7; }
    else if (strncmp(s, "[WARN] ", 7) == 0)  { level = "warning"; s += 7; }
    else if (strncmp(s, "[ERROR] ", 8) == 0) { level = "error"; s += 8; }
    lua_pushstring(L, s);
    lua_pushstring(L, level);
    return 2;
}
// getPos3(o) -> x, y, z: la posicion LOCAL del objeto 3D (relativa a su padre; si no tiene padre es la
// de mundo). ALIAS historico de getPosition(): los scripts viejos lo siguen usando. Ejes de Lua (Z arriba).
static int LPos3(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (!o) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    PushVec3Lua(L, o->pos.x, o->pos.y, o->pos.z);
    return 3;
}
// setPos3(o, x, y, z): ALIAS historico de setPosition() (ver mas abajo). Ejes de Lua (Z arriba).
static int LSetPos3(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (o) {
        o->pos = ReadVec3Lua(L, 2);
        Redraw();
    }
    return 0;
}
// rotateTowards(o, dx, dy, factor): gira SUAVE al objeto hacia esa direccion del piso
// (el slerp del quaternion vive aca: en lua seria un dolor). El piso de Lua es el
// plano XY (Z arriba): la 'dy' de Lua es la z del motor, cuyo piso es el XZ. Antes
// el segundo numero se llamaba 'dz' (ejes del motor): es el MISMO numero, solo
// cambio el nombre con la convencion nueva, asi que los scripts no cambian.
static int LRotateTowards(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (!o) return 0;
    float dx = (float)luaL_checknumber(L, 2);
    float dz = (float)luaL_checknumber(L, 3);   // la 'dy' de Lua = la z del motor
    float f  = (float)luaL_optnumber(L, 4, 0.2);
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 0.0001f) return 0;
    Vector3 dir(dx / len, 0.0f, dz / len);      // (motor: el piso es XZ, la Y es arriba)
    Quaternion q = Quaternion::FromDirection(dir, Vector3(0, 1, 0));
    o->SetRot(Quaternion::Slerp(o->Rot(), q, f));   // la puerta deja euler/axis-angle al dia
    Redraw();
    return 0;
}
// animate(mesh, n [, frameInicial]): elige la vertex-animation que sigue (0 idle,
// 1 correr, ... segun como se armaron las animaciones del modelo).
//   CON DOS ARGUMENTOS el cambio queda PENDIENTE: el clip nuevo entra cuando el
//   actual termina. Es lo que se quiere para encadenar (saludar -> idle).
//   CON TRES el cambio es EN EL ACTO y arranca en ese cuadro. Es el EMPALME POR
//   PIE de una locomocion: el juego que lo motivo pasa de caminar a correr saltando
//   a cuadros concretos, para que el pie de apoyo no se teletransporte. Sin el
//   tercer argumento eso no se podia expresar (habia que esperar al final del
//   ciclo y el personaje "patinaba" en el cambio).
static int LAnimate(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    int n = (int)luaL_checkinteger(L, 2);
    const bool hasStart = !lua_isnoneornil(L, 3);
    const float f0 = (float)luaL_optnumber(L, 3, 0.0);
    if (o && o->getType() == ObjectType::mesh) {
        VertexAnimationActive* va = FindTargetAnim((Mesh*)o);
        if (va) {
            if (hasStart) va->ChangeNow(n, f0);   // corta ya y cae en ese cuadro
            else           va->nextAnim = n;      // al terminar el clip actual
        }
        else {
            static bool notified = false;   // una sola vez por sesion (no spamear)
            if (!notified) { notified = true; w3dLogfW("animar(): '%s' no tiene vertex-animation activa", o->name.c_str()); }
        }
    }
    return 0;
}

// ===========================================================================
//  VERTICES POR SCRIPT (malla) — el caso que lo pidio fue una lamina de agua
//  animada por frame: mover ALGUNOS
//  vertices por frame (circulitos), tenirlos, y no tocar el resto.
//
//  EL GRUPO nace del sidecar <modelo>.groups.json (main/importers/import_wobj):
//  un VertexGroup con indices en dominio CONTROL-POINT (la linea 'v' del OBJ).
//  vertexGroup() lo traduce UNA VEZ a indices de RENDER via vertCtrlPoint
//  (render -> control-point; el mismo mapa del skinning/weight paint) y el juego
//  trabaja siempre con esos indices. Llamarla en start() y guardar la tabla.
//
//  INDICES 1-BASED en lua (como finger()): vertexGroup ya los devuelve asi y
//  vertexPos/setVertexPos/setVertexColor/setVertices los reciben asi.
//
//  EJES: las posiciones de vertices (locales de la malla) van en los ejes de
//  Lua (Z arriba): vertexPos/vertexPosAnim/setVertexPos/setVertices pasan
//  cada (x,y,z) por W3dLuaAxes en la frontera. vertex[] sigue en los del motor.
//
//  EFICIENCIA: los setters escriben vertex[]/vertexColor[] EN EL LUGAR (sin
//  allocs) y suben skinGeomVersion, el MISMO camino por el que la AnimacionUV
//  hace que el VBO se re-suba solo (ver Mesh::TickUVAnimStrip / UploadVBO). Para
//  muchos vertices por frame esta setVertices() (tabla plana {i,x,y,z,...}):
//  UN solo cruce lua<->C por frame.
//
//  OJO editor: esto mueve la malla EN VIVO (pensado para el Play / runtime);
//  no re-ancla marcas sharp/seam ni pasa por undo (como el playback de una
//  vertex anim). Guardar el proyecto con la malla "posada" por el script
//  hornea esas posiciones.
// ===========================================================================
static Mesh* AsMesh(lua_State* L, int idx) {
    Object* o = W3dScriptParamObject(L, idx);
    if (!o || o->getType() != ObjectType::mesh) return NULL;
    return (Mesh*)o;
}
// vertexGroup(m, "agua") -> tabla (array 1..n) con los indices de RENDER del
// grupo de vertices 'agua' de la malla. Tabla VACIA si no hay malla/grupo.
// Es la llamada "cara" (arma el mapeo, aloca la tabla): UNA vez en start().
static int LGroupVertices(lua_State* L) {
    Mesh* m = AsMesh(L, 1);
    const char* name = luaL_checkstring(L, 2);
    lua_newtable(L);
    if (!m) return 1;
    VertexGroup* vg = NULL;
    for (size_t g = 0; g < m->vertexGroups.size(); g++)
        if (m->vertexGroups[g] && m->vertexGroups[g]->name == name) { vg = m->vertexGroups[g]; break; }
    if (!vg) return 1;
    int n = 0;
    if (!m->vertCtrlPoint.empty()) {
        // dominio CONTROL-POINT -> render: todos los render-verts que salieron de
        // esa linea 'v' (con noMerge es 1:1; con merge puede haber varios por CP)
        int maxCP = -1;
        for (size_t i = 0; i < vg->verts.size(); i++)
            if (vg->verts[i] > maxCP) maxCP = vg->verts[i];
        if (maxCP < 0) return 1;
        std::vector<char> inGroup((size_t)maxCP + 1, 0);
        for (size_t i = 0; i < vg->verts.size(); i++)
            if (vg->verts[i] >= 0) inGroup[(size_t)vg->verts[i]] = 1;
        size_t limit = m->vertCtrlPoint.size();
        if ((size_t)m->vertexSize < limit) limit = (size_t)m->vertexSize;
        for (size_t i = 0; i < limit; i++) {
            int cp = m->vertCtrlPoint[i];
            if (cp >= 0 && cp <= maxCP && inGroup[(size_t)cp]) {
                lua_pushinteger(L, (lua_Integer)i + 1);   // 1-based para lua
                lua_rawseti(L, -2, ++n);
            }
        }
    } else {
        // sin mapa render->CP: los indices del grupo se toman como de RENDER
        // directamente (fallback; el sidecar siempre deja el mapa armado)
        for (size_t i = 0; i < vg->verts.size(); i++) {
            int rv = vg->verts[i];
            if (rv >= 0 && rv < m->vertexSize) {
                lua_pushinteger(L, (lua_Integer)rv + 1);
                lua_rawseti(L, -2, ++n);
            }
        }
    }
    return 1;
}
// vertexPos(m, i) -> x, y, z LOCALES del vertice de render i (los indices que
// da vertexGroup, 1-based). Ceros si la malla/indice no valen.
static int LVertexPos(lua_State* L) {
    Mesh* m = AsMesh(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || !m->vertex || i < 0 || i >= m->vertexSize) {
        lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 3;
    }
    PushVec3Lua(L, m->vertex[i*3], m->vertex[i*3+1], m->vertex[i*3+2]);
    return 3;
}
// vertexPosAnim(m, i) -> x, y, z: la posicion ANIMADA del render-vert i (1-based) en coords locales de la malla:
// con skinning (+ shape keys) es la posicion posada del ultimo skinning calculado (skinVertex); sin skinning es
// la de vertexPos. Para seguir un punto de una malla que se deforma (ej: proyectar el iris sobre el ojo).
static int LVertexPosAnim(lua_State* L) {
    Mesh* m = AsMesh(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || !m->vertex || i < 0 || i >= m->vertexSize) {
        lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 3;
    }
    // (la pose al dia: con la piel rigida el dibujo no deforma vertices y skinVertex podia quedar de antes)
    if (m->skinArmature) { extern void SkinMesh(Mesh*); SkinMesh(m); }
    const GLfloat* src = (m->skinArmature && m->skinVertex && m->skinShapeSerial != 0xFFFFFFFFu) ? m->skinVertex : m->vertex;
    PushVec3Lua(L, src[i*3], src[i*3+1], src[i*3+2]);
    return 3;
}
// vertexCount(m) -> cantidad de render-verts de la malla (el rango valido de vertexPos/vertexPosAnim/setVertexUV)
static int LNumVertices(lua_State* L) { Mesh* m = AsMesh(L, 1); lua_pushinteger(L, m ? m->vertexSize : 0); return 1; }
// setVertexUV(m, i, u, v [, capa]): la UV del render-vert i (1-based). capa 0 (default) = la UV base; capa >= 1 =
// una capa UV extra del render (Mesh::uvExtra, la que usa una capa de textura con uvMap = capa). Las capas se
// dibujan de client arrays, asi que el cambio se ve en el proximo frame sin rehacer nada.
// EL DESHACER DE LOS SCRIPTS FUERA DEL JUEGO: antes de que una funcion de lua cambie la malla (vertices, UV, colores),
// el editor (si esta grabando: un boton de las propiedades usado como complemento) guarda su geometria para Ctrl+Z.
// Durante el gameplay el gancho no hace nada (no hay historial: al Parar se pierde todo igual).
void (*W3dScriptBeforeTouchMeshHook)(Object* o) = NULL;
static void BeforeTouchMesh(Mesh* m) { if (W3dScriptBeforeTouchMeshHook && m) W3dScriptBeforeTouchMeshHook((Object*)m); }
static int LSetVertexUV(lua_State* L) {
    Mesh* m = AsMesh(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || i < 0 || i >= m->vertexSize) return 0;
    const GLfloat u = (GLfloat)luaL_checknumber(L, 3), v = (GLfloat)luaL_checknumber(L, 4);
    const int layer = (int)luaL_optinteger(L, 5, 0);
    BeforeTouchMesh(m);   // (Ctrl+Z de un script fuera del juego; despues de leer los argumentos)
    if (layer <= 0) {
        if (!m->uv) return 0;
        m->UninstantiateData(W3DMD_UV);   // COW: geometria compartida (MeshData.h)
        m->uv[(size_t)i * 2] = u; m->uv[(size_t)i * 2 + 1] = v;
        m->skinGeomVersion++;   // el VBO se re-sube solo (camino de la AnimacionUV)
    } else {
        if ((int)m->uvExtra.size() <= layer) m->uvExtra.resize((size_t)layer + 1);
        std::vector<GLfloat>& cap = m->uvExtra[(size_t)layer];
        if ((int)cap.size() < m->vertexSize * 2) cap.resize((size_t)m->vertexSize * 2, 0.0f);
        cap[(size_t)i * 2] = u; cap[(size_t)i * 2 + 1] = v;
    }
    Redraw();
    return 0;
}
// scrollUV(m, du, dv): TODA la UV base de la malla desplazada (du, dv), en una llamada y una sola
// re-subida del VBO. La base se captura en la primera llamada (las UV del archivo, en el orden de
// render-verts del motor, que no tiene por que coincidir con el del exportador) y se vuelve a capturar
// si otro sistema reescribio uv[] desde la ultima aplicacion. Es el reflejo que desliza la textura
// (parabrisas de los autos de Daytona USA). Devuelve la cantidad de vertices tocados.
static int LScrollUV(lua_State* L) {
    Mesh* m = AsMesh(L, 1);
    const GLfloat du = (GLfloat)luaL_checknumber(L, 2), dv = (GLfloat)luaL_checknumber(L, 3);
    if (!m || !m->uv || m->vertexSize < 1) { lua_pushinteger(L, 0); return 1; }
    BeforeTouchMesh(m);   // (Ctrl+Z de un script fuera del juego)
    const size_t n = (size_t)m->vertexSize * 2;
    bool intact = (m->uvOffsetBase.size() == n);
    for (size_t i = 0; i < n && intact; i += 2)
        if (m->uv[i] != m->uvOffsetBase[i] + m->uvOffsetU || m->uv[i + 1] != m->uvOffsetBase[i + 1] + m->uvOffsetV) intact = false;
    if (!intact) { m->uvOffsetBase.assign(m->uv, m->uv + n); m->uvOffsetU = m->uvOffsetV = 0.0f; }
    if (du == m->uvOffsetU && dv == m->uvOffsetV) { lua_pushinteger(L, m->vertexSize); return 1; }
    m->UninstantiateData(W3DMD_UV);   // COW: geometria compartida (MeshData.h)
    for (size_t i = 0; i < n; i += 2) {
        m->uv[i]     = m->uvOffsetBase[i]     + du;
        m->uv[i + 1] = m->uvOffsetBase[i + 1] + dv;
    }
    m->uvOffsetU = du; m->uvOffsetV = dv;
    m->skinGeomVersion++;   // UNA sola invalidacion: el VBO de UV se re-sube en el proximo dibujo
    Redraw();
    lua_pushinteger(L, m->vertexSize);
    return 1;
}
// copyUV(destino, origen): la UV base de otra malla con la MISMA cantidad de render-verts (por ejemplo las
// fases de una rueda que solo cambian sus UV: el arcade alterna 8 modelos de ruedas para que giren). Copia en el
// orden de vertices del motor, una re-subida del VBO. Devuelve true si copio.
static int LCopyUV(lua_State* L) {
    Mesh* d = AsMesh(L, 1);
    Mesh* o = AsMesh(L, 2);
    if (!d || !o || d == o || !d->uv || !o->uv || d->vertexSize != o->vertexSize || d->vertexSize < 1) { lua_pushboolean(L, 0); return 1; }
    BeforeTouchMesh(d);   // (Ctrl+Z de un script fuera del juego)
    const size_t n = (size_t)d->vertexSize * 2;
    if (memcmp(d->uv, o->uv, n * sizeof(GLfloat)) != 0) {
        d->UninstantiateData(W3DMD_UV);   // COW: geometria compartida (MeshData.h)
        memcpy(d->uv, o->uv, n * sizeof(GLfloat));
        d->skinGeomVersion++;              // el VBO de UV se re-sube en el proximo dibujo
        Redraw();
    }
    lua_pushboolean(L, 1);
    return 1;
}
// setVertexPos(m, i, x, y, z): mueve UN vertice (coords LOCALES, ejes de Lua). Para
// mover muchos por frame usa setVertices() (batch, un solo cruce lua<->C).
static int LSetVertexPos(lua_State* L) {
    Mesh* m = AsMesh(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || !m->vertex || i < 0 || i >= m->vertexSize) return 0;
    const Vector3 p = ReadVec3Lua(L, 3);   // (antes de tocar la malla: un argumento malo corta aca)
    BeforeTouchMesh(m);   // (Ctrl+Z de un script fuera del juego)
    m->UninstantiateData(W3DMD_POS);   // COW: geometria compartida (MeshData.h)
    GLfloat* v = m->vertex + (size_t)i * 3;
    v[0] = (GLfloat)p.x;
    v[1] = (GLfloat)p.y;
    v[2] = (GLfloat)p.z;
    m->skinGeomVersion++;   // el VBO se re-sube solo (camino de la AnimacionUV)
    Redraw();
    return 0;
}
// helper: 0..1 -> canal GLubyte saturado
static GLubyte ChannelColor(lua_Number v) {
    lua_Number n = v * 255.0;
    if (n < 0.0) n = 0.0;
    if (n > 255.0) n = 255.0;
    return (GLubyte)n;
}
// setVertexColor(m, i, r, g, b [,a]): color por vertice (0..1; alfa opcional,
// default 1). Solo tiene efecto visible si el material dibuja vertex color.
static int LSetVertexColor(lua_State* L) {
    Mesh* m = AsMesh(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || !m->vertexColor || i < 0 || i >= m->vertexSize) return 0;
    BeforeTouchMesh(m);   // (Ctrl+Z de un script fuera del juego)
    m->UninstantiateData(W3DMD_COL);   // COW: geometria compartida (MeshData.h)
    GLubyte* c = m->vertexColor + (size_t)i * 4;
    c[0] = ChannelColor(luaL_checknumber(L, 3));
    c[1] = ChannelColor(luaL_checknumber(L, 4));
    c[2] = ChannelColor(luaL_checknumber(L, 5));
    c[3] = ChannelColor(luaL_optnumber(L, 6, 1.0));
    m->skinGeomVersion++;   // re-subir tambien los atributos estaticos (color)
    Redraw();
    return 0;
}
// setVerticesColor(m, t[, n]): BATCH de setVertexColor — t es una tabla PLANA
// {i1,r1,g1,b1,a1, i2,r2,g2,b2,a2, ...} (0..1). UN cruce lua<->C y UNA
// invalidacion por tanda, en vez de una por vertice (cristal, halos del portal).
// El 3er argumento opcional acota cuantas entradas se leen (multiplo de 5).
static int LSetVerticesColor(lua_State* L) {
    Mesh* m = AsMesh(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (!m || !m->vertexColor) return 0;
    BeforeTouchMesh(m);   // (Ctrl+Z de un script fuera del juego)
    m->UninstantiateData(W3DMD_COL);   // COW: geometria compartida (MeshData.h)
    lua_Integer n = luaL_optinteger(L, 3, 0);
    if (n <= 0) n = (lua_Integer)lua_rawlen(L, 2);
    bool change = false;
    for (lua_Integer k = 1; k + 4 <= n; k += 5) {
        lua_rawgeti(L, 2, k);
        lua_rawgeti(L, 2, k + 1);
        lua_rawgeti(L, 2, k + 2);
        lua_rawgeti(L, 2, k + 3);
        lua_rawgeti(L, 2, k + 4);
        int i = (int)lua_tointeger(L, -5) - 1;
        if (i >= 0 && i < m->vertexSize) {
            GLubyte* c = m->vertexColor + (size_t)i * 4;
            c[0] = ChannelColor(lua_tonumber(L, -4));
            c[1] = ChannelColor(lua_tonumber(L, -3));
            c[2] = ChannelColor(lua_tonumber(L, -2));
            c[3] = ChannelColor(lua_tonumber(L, -1));
            change = true;
        }
        lua_pop(L, 5);
    }
    if (change) {
        m->skinGeomVersion++;   // UNA sola invalidacion por batch
        Redraw();
    }
    return 0;
}
// setVertices(m, t[, n]): BATCH — t es una tabla PLANA {i1,x1,y1,z1, i2,x2,y2,z2, ...}
// con los indices de vertexGroup (cada x,y,z en los ejes de Lua: Z arriba, se
// intercambian con W3dLuaAxes al escribir). UN solo cruce lua<->C por frame y CERO allocs
// en C: es la forma eficiente de animar el agua (mover N vertices en circulos).
// El 3er argumento opcional acota cuantas ENTRADAS de la tabla se leen (en
// numeros, multiplo de 4): el script rellena solo el prefijo y evita rearmar
// la tabla o pagar rawlen sobre una tabla gigante reciclada.
// Los indices invalidos se saltean; las entradas incompletas del final se ignoran.
static int LSetVertices(lua_State* L) {
    Mesh* m = AsMesh(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (!m || !m->vertex) return 0;
    BeforeTouchMesh(m);   // (Ctrl+Z de un script fuera del juego)
    m->UninstantiateData(W3DMD_POS);   // COW: geometria compartida (MeshData.h)
    lua_Integer n = luaL_optinteger(L, 3, 0);
    if (n <= 0) n = (lua_Integer)lua_rawlen(L, 2);
    bool change = false;
    for (lua_Integer k = 1; k + 3 <= n; k += 4) {
        lua_rawgeti(L, 2, k);
        lua_rawgeti(L, 2, k + 1);
        lua_rawgeti(L, 2, k + 2);
        lua_rawgeti(L, 2, k + 3);
        int i = (int)lua_tointeger(L, -4) - 1;
        if (i >= 0 && i < m->vertexSize) {
            GLfloat* v = m->vertex + (size_t)i * 3;
            float y = (float)lua_tonumber(L, -2), z = (float)lua_tonumber(L, -1);
            W3dLuaAxes(y, z);   // Lua (Z arriba) -> motor (Y arriba)
            v[0] = (GLfloat)lua_tonumber(L, -3);
            v[1] = (GLfloat)y;
            v[2] = (GLfloat)z;
            change = true;
        }
        lua_pop(L, 4);
    }
    if (change) {
        m->skinGeomVersion++;   // UNA sola invalidacion por batch
        Redraw();
    }
    return 0;
}

// ===========================================================================
//  API DE OBJETOS — sirve para CUALQUIER tipo de objeto (mesh, luz, camara,
//  vacio, ui, texto...), porque todo lo que se toca aca vive en Object.
//
//  MODELO MENTAL (dos familias de verbos, no mezclarlas):
//    * los "set..." son ABSOLUTOS: setPosition/setRotation/setScale PONEN el
//      valor final. Es un TELETRANSPORTE (salto instantaneo, sin recorrido):
//      respawn, colocar algo, acomodar la escena en start().
//    * move/rotate son RELATIVOS y por-frame: SUMAN al estado actual. Son los
//      que se usan con dt para getVelocity/gravedad/inercia, y son el punto de
//      enganche de la fisica (ver ApplyMotion).
//
//  EJES: los de LUA = los del panel de Propiedades: X a la derecha, Y hacia
//  adelante y Z ARRIBA (desde el 2026-11-11; antes eran los del motor, Y arriba,
//  y el campo "Z" del panel era la 'y' de estos binds). El motor sigue con Y
//  arriba: cada bind convierte en la FRONTERA con W3dLuaAxes (W3dScript.h), o
//  sea el (x, y, z) de Lua es el (x, z, y) del motor, sin cambios de signo, igual
//  que el panel. Vale para posicion, rotacion (grados por eje: la rot de Lua
//  (rx, ry, rz) es el rotEuler del motor (x, z, y)) y escala; un ORDEN de
//  rotacion ("YXZ") se traduce con W3dLuaOrder.
//  POSICION/ROTACION/ESCALA son LOCALES (relativas al padre). Sin padre, la
//  local ES la de mundo (el caso normal de un juego).
//  Ninguno rompe con nil ni con un objeto del tipo equivocado: los getters
//  devuelven ceros/false y los setters no hacen nada.
// ===========================================================================

// PUNTO DE ENGANCHE de la fisica: TODO desplazamiento relativo pasa por aca. Hoy es una suma cruda
// (movimiento libre, atraviesa todo). Cuando exista colision, este es el UNICO lugar donde hay que
// resolver el barrido (mover hasta el choque / deslizar) para que move() y lo que venga despues lo
// hereden solos. setPosition NO pasa por aca a proposito: teletransportar ignora la fisica.
// (dx, dy, dz) en ejes del MOTOR: el bind ya los convirtio.
static void ApplyMotion(Object* o, float dx, float dy, float dz) {
    o->pos.x += dx; o->pos.y += dy; o->pos.z += dz;
}

// getType(o) -> string con el tipo del objeto: "mesh", "luz", "camara", "vacio", "ui", "texto",
// "imagen", "rect", "boton", "coleccion", ... (ver W3dTypeName en objects/Objects.h).
// Con nil devuelve "" (asi `if getType(o) == "luz"` nunca revienta).
static int LType(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    lua_pushstring(L, o ? W3dTypeName(o->getType()) : "");
    return 1;
}
// getName(o) -> el nombre del objeto en el outliner ("" si es nil)
static int LName(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    lua_pushstring(L, o ? o->name.c_str() : "");
    return 1;
}

// ---- POSICION -------------------------------------------------------------
// getPosition(o) -> x, y, z (local, ejes de Lua: Z arriba)
static int LPosition(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (!o) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    PushVec3Lua(L, o->pos.x, o->pos.y, o->pos.z);
    return 3;
}
// setPosition(o, x, y, z): TELETRANSPORTAR. Set ABSOLUTO, salto instantaneo, sin recorrido y sin
// fisica (no choca con nada en el camino). Para el movimiento del juego se usa move().
static int LSetPosition(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (o) {
        o->pos = ReadVec3Lua(L, 2);
        Redraw();
    }
    return 0;
}
// move(o, dx, dy, dz): DESPLAZAMIENTO RELATIVO (suma a la posicion actual), en los ejes del padre.
// Es la forma buena de mover algo cada frame: move(pelota, vx*dt, vy*dt, vz*dt) (vz = para arriba).
// A diferencia de setPosition, pasa por ApplyMotion -> cuando haya colision, el motor va a
// resolver el recorrido aca (frenar/deslizar) en vez de atravesar la pared.
static int LMove(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (o) {
        const Vector3 d = ReadVec3Lua(L, 2);
        ApplyMotion(o, d.x, d.y, d.z);
        Redraw();
    }
    return 0;
}

// ---- ROTACION (grados) ----------------------------------------------------
// getRotation(o) -> x, y, z en GRADOS (el euler de display: conserva las vueltas enteras). Los mismos tres
// numeros que muestra el panel: la 'z' de Lua es el giro alrededor de la vertical (la Y del motor).
static int LRotation(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (!o) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    PushVec3Lua(L, o->rotEuler.x, o->rotEuler.y, o->rotEuler.z);
    return 3;
}
// setRotation(o, x, y, z): orientacion ABSOLUTA en grados, el euler del PANEL (lo mismo que devuelve
// getRotation(): van y vuelven sin perderse). Usa SetRotEuler para que el euler mande y el quaternion se
// derive: asi no se pierden las vueltas ni se desincroniza el panel. OJO: ese euler es el XYZ del MOTOR
// (q = Rx * Ry * Rz con la Y del motor arriba); en las letras de Lua es el orden "XZY" (R = Rx * Rz * Ry).
// setRotation(o, x, y, z, "ZXY"): los mismos angulos (x sobre el eje X, y sobre Y, z sobre Z, ejes de Lua)
// compuestos en ese ORDEN (R = Rz * Rx * Ry: un vehiculo que rola y cabecea en su marco y despues apunta a
// su rumbo; con Y arriba eso era "YXZ"). El orden se pasa a los ejes del motor con W3dLuaOrder; si da el
// "XYZ" del motor se usa SetRotEuler (conserva las vueltas). El motor arma el quaternion directo y deriva
// el euler del panel; antes el script armaba la matriz y la descomponia con trigonometria en lua (en el
// N95, lo caro).
static int LSetRotation(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (o) {
        const Vector3 r = ReadVec3Lua(L, 2);   // (grados, ya en ejes del motor)
        const char* orderLua = lua_type(L, 5) == LUA_TSTRING ? lua_tostring(L, 5) : NULL;
        const std::string order = orderLua ? W3dLuaOrder(orderLua) : std::string();
        if (order.size() != 3 || order == "XYZ" || order == "xyz") {
            o->SetRotEuler(r);
        } else {
            Quaternion q;   // (identidad)
            for (int k = 0; k < 3; k++) {
                const char c = order[(size_t)k];
                if (c == 'X' || c == 'x')      q = q * Quaternion::FromAxisAngle(1, 0, 0, r.x);
                else if (c == 'Y' || c == 'y') q = q * Quaternion::FromAxisAngle(0, 1, 0, r.y);
                else if (c == 'Z' || c == 'z') q = q * Quaternion::FromAxisAngle(0, 0, 1, r.z);
            }
            o->SetRot(q);
        }
        Redraw();
    }
    return 0;
}
// ---- UN EJE SOLO ------------------------------------------------------------------------------------------------------
// getRotationX/Y/Z(o) -> grados; setRotationX/Y/Z(o, grados): lo mismo que getRotation()/setRotation(), pero de UN eje (los
// otros dos quedan como estaban). Ejes de Lua: getRotationZ es el giro sobre la VERTICAL, la Z del panel. Para lo de todos
// los dias, sin armar los tres numeros: setRotationZ(o, getRotationZ(o) + 90 * dt) (pedido del dueno, 2026-11-11: "mas
// directo y menos codigo"). En el motor la Y y la Z van cruzadas (W3dLuaAxes): la Z de Lua es rotEuler.y.
static float* AxisRotMotor(Vector3& r, int axisLua) { return axisLua == 0 ? &r.x : (axisLua == 1 ? &r.z : &r.y); }
static int AxisRotation(lua_State* L, int axisLua) {
    Object* o = W3dScriptParamObject(L, 1);
    if (!o) { lua_pushnumber(L, 0); return 1; }
    Vector3 r = o->rotEuler;
    lua_pushnumber(L, *AxisRotMotor(r, axisLua));
    return 1;
}
static int SetAxisRotation(lua_State* L, int axisLua) {
    Object* o = W3dScriptParamObject(L, 1);
    if (o) {
        Vector3 r = o->rotEuler;                     // (el euler del panel: los otros dos ejes quedan igual)
        *AxisRotMotor(r, axisLua) = (float)luaL_checknumber(L, 2);
        o->SetRotEuler(r);
        Redraw();
    }
    return 0;
}
static int LRotationX(lua_State* L) { return AxisRotation(L, 0); }
static int LRotationY(lua_State* L) { return AxisRotation(L, 1); }
static int LRotationZ(lua_State* L) { return AxisRotation(L, 2); }
static int LSetRotationX(lua_State* L) { return SetAxisRotation(L, 0); }
static int LSetRotationY(lua_State* L) { return SetAxisRotation(L, 1); }
static int LSetRotationZ(lua_State* L) { return SetAxisRotation(L, 2); }
// rotate(o, dx, dy, dz): giro RELATIVO en grados sobre los ejes PROPIOS del objeto (suma a la
// rotacion actual), en los ejes de Lua. El de cada frame: rotate(moneda, 0, 0, 90*dt) (la moneda
// gira sobre la vertical, la Z). Se aplican como RotateLocal del motor: primero el de la vertical.
static int LRotate(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (o) {
        const Vector3 d = ReadVec3Lua(L, 2);
        o->RotateLocal(d.x, d.y, d.z);
        o->UpdateDisplayRot();   // RotateLocal solo toca 'rot': hay que refrescar el display
        Redraw();
    }
    return 0;
}

// ---- ESCALA ---------------------------------------------------------------
// getScale(o) -> sx, sy, sz del objeto (ejes de Lua: sz = la escala vertical).
// OJO: getScale() SIN argumento es OTRA cosa (el factor responsive de pantalla, min(w,h)/480) y vive
// en libs/Whisk3DCore/script/BindsGame.cpp, que se registra DESPUES y pisa a este. Por eso alla el bind mira si
// le pasaron un objeto y, si es asi, devuelve esta misma escala: las dos formas conviven (y alla
// tambien tiene que salir con W3dLuaAxes).
static int LScaleObj(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (!o) { lua_pushnumber(L, 1); lua_pushnumber(L, 1); lua_pushnumber(L, 1); return 3; }
    PushVec3Lua(L, o->scale.x, o->scale.y, o->scale.z);
    return 3;
}
// setScale(o, sx, sy, sz): escala ABSOLUTA (ejes de Lua). Con UN solo numero es uniforme: setScale(o, 2).
static int LSetScale(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (o) {
        float sx = (float)luaL_checknumber(L, 2);
        float sy = (float)luaL_optnumber(L, 3, sx);
        float sz = (float)luaL_optnumber(L, 4, sx);
        W3dLuaAxes(sy, sz);   // Lua (Z arriba) -> motor (Y arriba)
        o->scale.x = sx; o->scale.y = sy; o->scale.z = sz;
        Redraw();
    }
    return 0;
}
// scale(o, f): escala RELATIVA (multiplica la actual). El gemelo de move/rotate para crecer o
// achicarse de a poco: scale(globo, 1 + 0.5*dt).
static int LScale(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (o) {
        float f = (float)luaL_checknumber(L, 2);
        o->scale.x *= f; o->scale.y *= f; o->scale.z *= f;
        Redraw();
    }
    return 0;
}

// ---- VISIBILIDAD ----------------------------------------------------------
// isVisible(o) -> true/false (false si es nil)
static int LVisible(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    lua_pushboolean(L, (o && o->visible) ? 1 : 0);
    return 1;
}
// setVisible(o, bool): prende/apaga el dibujado. Sirve para cualquier objeto (una luz apagada es
// una luz invisible: Object::Render corta antes de subirla a GL). show() es el alias 2D historico
// de esto (vive en BindsGame.cpp y hace exactamente lo mismo).
// las mallas skinneadas del subarbol olvidan la caja de su ultima pose (la del autocull por frustum de
// Mesh::RenderObject): hasta el proximo skinneo no se cullean. Un juego que REUSA personajes de un pool
// (prenderlos en otro lugar con otro clip) los veia aparecer tarde: la caja vieja era la del uso anterior,
// fuera de camara, y el autocull solo la refresca tras 30 cuadros culleada.
static void ForgetBoxSkin(Object* o) {
    if (!o) return;
    if (o->getType() == ObjectType::mesh) {
        Mesh* m = (Mesh*)o;
        if (m->skinArmature) { m->skinAabbOk = false; m->consecutiveSkinCull = 0; }
    }
    for (size_t i = 0; i < o->Childrens.size(); i++) ForgetBoxSkin(o->Childrens[i]);
}
static int LSetVisible(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (o) {
        const bool v = lua_toboolean(L, 2) != 0;
        if (v && !o->visible) ForgetBoxSkin(o);   // se prende: su pose puede ser otra (ver arriba)
        o->visible = v; Redraw();
    }
    return 0;
}

// ---- ESQUELETOS (animacion de huesos en el juego) --------------------------
// Jugando, cada armature reproduce su clip activo con su propio cabezal (Armature::gameFrame, lo
// avanza W3dArmaturesGameTick con el FrameRate del clip). El frame global del timeline no participa.
static Armature* ArmArg(lua_State* L, int i) {
    Object* o = W3dScriptParamObject(L, i);
    if (o && W3dIsInstanceType(o->getType())) o = FirstArmature(o);   // (una instancia: su esqueleto)
    return (o && o->getType() == ObjectType::armature) ? (Armature*)o : NULL;
}
static int ClipArg(lua_State* L, Armature* a, int i) {
    if (lua_type(L, i) == LUA_TNUMBER) return (int)lua_tonumber(L, i) - 1;   // indices de lua: base 1
    const char* n = lua_tostring(L, i);
    return n ? W3dArmatureClipByName(a, n) : -1;
}
// animClip(arm, "nombre"|indice [, loop=true [, reiniciar=false]]) -> true si el clip existe.
// Elegir el MISMO clip que ya suena no lo reinicia (salvo reiniciar=true): se puede llamar todos los frames.
static int LAnimClip(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    int c = a ? ClipArg(L, a, 2) : -1;
    bool loop = lua_isnoneornil(L, 3) ? true : (lua_toboolean(L, 3) != 0);
    bool restart = lua_toboolean(L, 4) != 0;
    if (a && (c != a->activeAnim || restart)) ForgetBoxSkin(a);   // otro clip: la caja de la pose vieja no vale
    lua_pushboolean(L, W3dArmaturePlayClip(a, c, loop, restart) ? 1 : 0);
    return 1;
}
// getAnim(arm) -> nombre del clip que suena (nil si ninguno)
static int LCurrentAnim(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    if (!a || a->activeAnim < 0 || a->activeAnim >= (int)a->animations.size()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, a->animations[a->activeAnim]->name.c_str());
    return 1;
}
// animFrame(arm) -> frames desde el inicio del clip (con decimales) ; animFrame(arm, f) lo fija
static int LAnimFrame(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    if (!a) { lua_pushnumber(L, 0); return 1; }
    if (!lua_isnoneornil(L, 2)) { a->gameFrame = (float)lua_tonumber(L, 2); a->gameEnded = false; }
    lua_pushnumber(L, a->gameFrame);
    return 1;
}
// animLength(arm [, clip]) -> cantidad de frames del clip (activo por defecto)
static int LAnimLength(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    int c = !a ? -1 : (lua_isnoneornil(L, 2) ? a->activeAnim : ClipArg(L, a, 2));
    if (!a || c < 0 || c >= (int)a->animations.size()) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, a->animations[c]->endFrame - a->animations[c]->startFrame + 1);
    return 1;
}
// animFinished(arm) -> true cuando un clip SIN loop llego a su ultimo frame
static int LEndedAnim(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    lua_pushboolean(L, (a && a->gameEnded) ? 1 : 0);
    return 1;
}
// animSpeed(arm, v): multiplicador del FrameRate del clip (1 = normal, 0 = pausa, -1 no soportado)
static int LAnimVelocity(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    if (a && !lua_isnoneornil(L, 2)) { float v = (float)lua_tonumber(L, 2); a->gameVel = v < 0.0f ? 0.0f : v; }
    lua_pushnumber(L, a ? a->gameVel : 0.0f);
    return 1;
}
// ---- CAPAS (el MIX de animaciones en el juego; ver W3dLayerAnim) ----
// Con al menos una capa el armature muestra la MEZCLA de sus capas (animClip deja de mandar). Indices base 1.
static W3dLayerAnim* LayerArg(lua_State* L, Armature* a, int i, bool create) {
    if (!a) return NULL;
    int n = (int)luaL_checkinteger(L, i);
    if (n < 1 || n > 16) return NULL;
    if ((int)a->layers.size() < n) { if (!create) return NULL; a->layers.resize(n); }
    return &a->layers[n - 1];
}
// animLayer(arm, n, "clip" [, influencia 0..1 = 1 [, "mezclar"|"sumar"|"restar" [, loop = true [, reiniciar = false]]]])
// Crea las capas que falten. Cambiar de clip arranca la capa de 0 (el mismo clip sigue donde iba).
static int LAnimLayer(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    W3dLayerAnim* c = LayerArg(L, a, 2, true);
    const char* clip = lua_tostring(L, 3);
    if (!c || !clip) { lua_pushboolean(L, 0); return 1; }
    bool restart = lua_toboolean(L, 7) != 0;
    if (c->anim != clip || restart) { c->anim = clip; c->clipCache = -1; c->gameFrame = 0.0f; c->gameEnded = false; }
    if (!lua_isnoneornil(L, 4)) c->influence = (float)lua_tonumber(L, 4) * 100.0f;
    if (!lua_isnoneornil(L, 5)) { const char* m = lua_tostring(L, 5);
        c->mode = (m && !strcmp(m, "add")) ? 1 : (m && !strcmp(m, "subtract")) ? 2 : 0; }
    c->loop = lua_isnoneornil(L, 6) ? true : (lua_toboolean(L, 6) != 0);
    if (c->loop) c->gameEnded = false;
    c->visible = true;
    lua_pushboolean(L, W3dLayerClip(a, *c) >= 0 ? 1 : 0);
    return 1;
}
// animLayerWeight(arm, n [, influencia 0..1]) -> influencia
static int LAnimLayerWeight(lua_State* L) {
    W3dLayerAnim* c = LayerArg(L, ArmArg(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) c->influence = (float)lua_tonumber(L, 3) * 100.0f;
    lua_pushnumber(L, c ? c->influence * 0.01f : 0.0f);
    return 1;
}
// animLayerFrame(arm, n [, f]) -> frame de la capa (desde el inicio de su clip)
static int LAnimLayerFrame(lua_State* L) {
    W3dLayerAnim* c = LayerArg(L, ArmArg(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) { c->gameFrame = (float)lua_tonumber(L, 3); c->gameEnded = false; }
    lua_pushnumber(L, c ? c->gameFrame : 0.0f);
    return 1;
}
// animLayerFinished(arm, n) -> true si su clip sin loop llego al final
static int LAnimEndedLayer(lua_State* L) {
    W3dLayerAnim* c = LayerArg(L, ArmArg(L, 1), 2, false);
    lua_pushboolean(L, (c && c->gameEnded) ? 1 : 0);
    return 1;
}
// animLayerBone(arm, n, "hueso"|nil): la capa solo afecta a ese hueso y sus hijos (nil = todo)
static int LAnimLayerBone(lua_State* L) {
    W3dLayerAnim* c = LayerArg(L, ArmArg(L, 1), 2, false);
    if (c) { const char* h = lua_tostring(L, 3); c->bone = h ? h : ""; }
    return 0;
}
// animLayerSpeed(arm, n, v): velocidad de la capa (1 = normal)
static int LAnimLayerVel(lua_State* L) {
    W3dLayerAnim* c = LayerArg(L, ArmArg(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) { float v = (float)lua_tonumber(L, 3); c->vel = v < 0.0f ? 0.0f : v; }
    lua_pushnumber(L, c ? c->vel : 0.0f);
    return 1;
}
// animLayers(arm [, n]) -> cantidad de capas; con n recorta a n (0 = vuelve al clip unico de animClip)
static int LAnimLayers(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    if (a && !lua_isnoneornil(L, 2)) { int n = (int)lua_tointeger(L, 2); if (n < 0) n = 0; if (n < (int)a->layers.size()) a->layers.resize(n); }
    lua_pushinteger(L, a ? (lua_Integer)a->layers.size() : 0);
    return 1;
}

// ---- CAPAS DE ESCENA (el mix de animaciones de ESCENA en el juego: personajes hechos de objetos sueltos) ----
static W3dLayerAnim* EscLayerArg(lua_State* L, int i, bool create) {
    int n = (int)luaL_checkinteger(L, i);
    if (n < 1 || n > 16) return NULL;
    if ((int)g_mixScenes.size() < n) { if (!create) return NULL; g_mixScenes.resize(n); }
    return &g_mixScenes[n - 1];
}
// sceneLayer(n, "escena" [, influencia 0..1 [, "mezclar"|"sumar"|"restar" [, loop = true [, reiniciar]]]])
static int LSceneLayer(lua_State* L) {
    W3dLayerAnim* c = EscLayerArg(L, 1, true);
    const char* name = lua_tostring(L, 2);
    if (!c || !name) { lua_pushboolean(L, 0); return 1; }
    bool restart = lua_toboolean(L, 6) != 0;
    if (c->anim != name || restart) { c->anim = name; c->gameFrame = 0.0f; c->gameEnded = false; }
    if (!lua_isnoneornil(L, 3)) c->influence = (float)lua_tonumber(L, 3) * 100.0f;
    if (!lua_isnoneornil(L, 4)) { const char* m = lua_tostring(L, 4);
        c->mode = (m && !strcmp(m, "add")) ? 1 : (m && !strcmp(m, "subtract")) ? 2 : 0; }
    c->loop = lua_isnoneornil(L, 5) ? true : (lua_toboolean(L, 5) != 0);
    if (c->loop) c->gameEnded = false;
    c->visible = true;
    lua_pushboolean(L, W3dAnimSceneIdx(name) >= 0 ? 1 : 0);
    return 1;
}
static int LSceneLayerWeight(lua_State* L) {
    W3dLayerAnim* c = EscLayerArg(L, 1, false);
    if (c && !lua_isnoneornil(L, 2)) c->influence = (float)lua_tonumber(L, 2) * 100.0f;
    lua_pushnumber(L, c ? c->influence * 0.01f : 0.0f); return 1;
}
static int LSceneLayerFrame(lua_State* L) {
    W3dLayerAnim* c = EscLayerArg(L, 1, false);
    if (c && !lua_isnoneornil(L, 2)) { c->gameFrame = (float)lua_tonumber(L, 2); c->gameEnded = false; }
    lua_pushnumber(L, c ? c->gameFrame : 0.0f); return 1;
}
static int LSceneEndedLayer(lua_State* L) {
    W3dLayerAnim* c = EscLayerArg(L, 1, false);
    lua_pushboolean(L, (c && c->gameEnded) ? 1 : 0); return 1;
}
static int LSceneLayers(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) { int n = (int)lua_tointeger(L, 1); if (n < 0) n = 0;
        if (n < (int)g_mixScenes.size()) { g_mixScenes.resize(n); if (n == 0) W3dMixScenesRelease(); } }
    lua_pushinteger(L, (lua_Integer)g_mixScenes.size()); return 1;
}

// ---- CAPAS DE CLIPS DE JERARQUIA (el mix de una RAIZ: varios clips de jerarquia mezclados sobre ella y sus hijos;
//      ver W3dHierRoot::capas en animation/W3dAnimSet.h). Con al menos una capa la raiz muestra la MEZCLA y su
//      animObject deja de posarla (como un armature con animLayer). Indices base 1. El clip: el de su biblioteca o
//      "Biblioteca/clip" de una biblioteca cargada (una capa no carga bibliotecas). ----
static W3dLayerAnim* HierLayerArg(lua_State* L, Object* o, int i, bool create) {
    if (!o) return NULL;
    int n = (int)luaL_checkinteger(L, i);
    if (n < 1 || n > 16) return NULL;
    if (!o->clipsHier || (int)o->clipsHier->layers.size() < n) {
        if (!create) return NULL;
        if (!o->clipsHier) o->clipsHier = new W3dHierRoot();
        o->clipsHier->layers.resize(n);
        W3dHierLayersChanged(o);
    }
    return &o->clipsHier->layers[n - 1];
}
// objectLayer(raiz, n, "clip" [, influencia 0..1 = 1 [, "mezclar"|"sumar"|"restar" [, loop = true [, reiniciar = false]]]])
// -> true si encontro el clip. Crea las capas que falten. Cambiar de clip arranca la capa de 0.
static int LObjectLayer(lua_State* L) {
    Object* o = ObjectAnimable(L, 1);
    W3dLayerAnim* c = HierLayerArg(L, o, 2, true);
    const char* clip = lua_tostring(L, 3);
    if (!c || !clip) { lua_pushboolean(L, 0); return 1; }
    bool restart = lua_toboolean(L, 7) != 0;
    if (c->anim != clip || restart) { c->anim = clip; c->retClip = 0; c->gameFrame = 0.0f; c->gameEnded = false; }
    if (!lua_isnoneornil(L, 4)) c->influence = (float)lua_tonumber(L, 4) * 100.0f;
    if (!lua_isnoneornil(L, 5)) { const char* m = lua_tostring(L, 5);
        c->mode = (m && !strcmp(m, "add")) ? 1 : (m && !strcmp(m, "subtract")) ? 2 : 0; }
    c->loop = lua_isnoneornil(L, 6) ? true : (lua_toboolean(L, 6) != 0);
    if (c->loop) c->gameEnded = false;
    c->visible = true;
    lua_pushboolean(L, W3dHierLayerClip(o, *c, 0) ? 1 : 0);
    return 1;
}
// objectLayerWeight(raiz, n [, influencia 0..1]) -> influencia
static int LObjectLayerWeight(lua_State* L) {
    W3dLayerAnim* c = HierLayerArg(L, ObjectAnimable(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) c->influence = (float)lua_tonumber(L, 3) * 100.0f;
    lua_pushnumber(L, c ? c->influence * 0.01f : 0.0f);
    return 1;
}
// objectLayerFrame(raiz, n [, f]) -> frame de la capa (desde el inicio de su clip)
static int LObjectLayerFrame(lua_State* L) {
    W3dLayerAnim* c = HierLayerArg(L, ObjectAnimable(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) { c->gameFrame = (float)lua_tonumber(L, 3); c->gameEnded = false; }
    lua_pushnumber(L, c ? c->gameFrame : 0.0f);
    return 1;
}
// objectLayerFinished(raiz, n) -> true si su clip sin loop llego al final
static int LObjectEndedLayer(lua_State* L) {
    W3dLayerAnim* c = HierLayerArg(L, ObjectAnimable(L, 1), 2, false);
    lua_pushboolean(L, (c && c->gameEnded) ? 1 : 0);
    return 1;
}
// objectLayerSpeed(raiz, n, v): velocidad de la capa (1 = normal)
static int LObjectLayerVel(lua_State* L) {
    W3dLayerAnim* c = HierLayerArg(L, ObjectAnimable(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) { float v = (float)lua_tonumber(L, 3); c->vel = v < 0.0f ? 0.0f : v; }
    lua_pushnumber(L, c ? c->vel : 0.0f);
    return 1;
}
// objectLayers(raiz [, n]) -> cantidad de capas; con n recorta a n (0 = vuelve al clip unico de animObject; los
// nodos quedan donde los dejo la mezcla)
static int LObjectLayers(lua_State* L) {
    Object* o = ObjectAnimable(L, 1);
    if (o && o->clipsHier && !lua_isnoneornil(L, 2)) {
        int n = (int)lua_tointeger(L, 2); if (n < 0) n = 0;
        if (n < (int)o->clipsHier->layers.size()) { o->clipsHier->layers.resize(n); W3dHierLayersChanged(o); }
    }
    lua_pushinteger(L, (o && o->clipsHier) ? (lua_Integer)o->clipsHier->layers.size() : 0);
    return 1;
}

// animTransition(arm, frames [, fps = 30]): congela la pose de AHORA y la funde hacia lo que suene despues durante
// 'frames' (el crossfade de pose de cualquier juego). Llamarla ANTES de cambiar el clip de la capa.
static int LAnimTransition(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    float fr = (float)luaL_optnumber(L, 2, 8), fps = (float)luaL_optnumber(L, 3, 30);
    if (a && fps > 0.0f) W3dArmatureTransition(a, fr / fps);
    return 0;
}
// lookAt(o, x, y, z): orienta el objeto para que su frente (el de una camara: el -Z local del motor, que en los
// ejes de Lua es el -Y local) apunte al punto de mundo (x, y, z en los ejes de Lua, Z arriba), sin rolido (el
// "arriba" del objeto queda hacia +Z de Lua, la +Y del motor). Pensado para objetos sin padre (camaras de juego).
// La cuenta de abajo es toda en ejes del MOTOR: solo el punto cruza la frontera.
static int LLookTo(lua_State* L) {
    Object* o = W3dScriptParamObject(L, 1);
    if (!o) return 0;
    Vector3 p = o->GetGlobalPosition();
    const Vector3 t = ReadVec3Lua(L, 2);
    Vector3 f(t.x - p.x, t.y - p.y, t.z - p.z);
    float l = sqrtf(f.x*f.x + f.y*f.y + f.z*f.z); if (l < 1e-6f) return 0;
    f = f * (1.0f / l);
    Vector3 z(-f.x, -f.y, -f.z);                       // el eje +Z local mira para ATRAS
    Vector3 x(z.z, 0.0f, -z.x);                          // up (0,1,0) x z
    float lx = sqrtf(x.x*x.x + x.z*x.z); if (lx < 1e-6f) return 0;   // mirando derecho arriba/abajo: indefinido
    x = x * (1.0f / lx);
    Vector3 y(z.y*x.z - z.z*x.y, z.z*x.x - z.x*x.z, z.x*x.y - z.y*x.x);
    Matrix4 R; R.Identity();
    R.m[0] = x.x; R.m[1] = x.y; R.m[2] = x.z; R.m[4] = y.x; R.m[5] = y.y; R.m[6] = y.z; R.m[8] = z.x; R.m[9] = z.y; R.m[10] = z.z;
    o->SetRot(Quaternion::FromMatrix(R));
    Redraw();
    return 0;
}

// bonePoint(arm, "hueso" [, x, y, z]) -> x, y, z en MUNDO del punto (x,y,z) dado en el espacio del hueso,
// en la pose de ESTE frame (la boca de un arma, el origen de un laser...). nil si el hueso no esta.
// El punto y el resultado van en los ejes de Lua (Z arriba; tambien el espacio del hueso se ve con la
// y y la z intercambiadas, como todo lo local): W3dLuaAxes en la entrada y en la salida.
static int LBonePoint(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    const char* hn = lua_tostring(L, 2);
    if (!a || !hn) { lua_pushnil(L); return 1; }
    int h = -1;
    for (size_t i = 0; i < a->bones.size(); i++) if (a->bones[i].name == hn) { h = (int)i; break; }
    if (h < 0) { lua_pushnil(L); return 1; }
    EvaluatePoseSkeleton(a, CurrentFrame);
    Matrix4 W; a->GetWorldMatrix(W);
    float px = (float)luaL_optnumber(L, 3, 0), py = (float)luaL_optnumber(L, 4, 0), pz = (float)luaL_optnumber(L, 5, 0);
    W3dLuaAxes(py, pz);   // Lua -> motor
    Vector3 w = (W * a->bones[h].poseWorld) * Vector3(px, py, pz);
    PushVec3Lua(L, w.x, w.y, w.z);
    return 3;
}

// ---- LUZ ------------------------------------------------------------------
// La luz del motor es OpenGL fixed-function: no tiene un campo "energia", tiene el color difuso
// (rgb). Aca se parte en las dos cosas que uno quiere manejar desde un juego:
//   getColor(l)   = el TONO (rgb ya normalizado, con el componente mas alto en 1)
//   getEnergy(l) = el BRILLO (0..1) = el componente mas alto del difuso
// Asi getColor()/getEnergy() y setColor()/setEnergy() son independientes y van y vuelven sin perderse.
// PORTABILIDAD: en web/Android solo la PRIMERA luz tiene efecto; en Symbian no hay cono ni
// atenuacion cuadratica. Si el objeto no es una luz, todo esto es no-op / ceros.
static Light* AsLight(lua_State* L, int idx) {
    Object* o = W3dScriptParamObject(L, idx);
    if (!o || o->getType() != ObjectType::light) return NULL;
    return (Light*)o;
}
// el componente mas alto del difuso = el brillo de la luz
static float BrightnessOf(Light* l) {
    float m = l->diffuse[0];
    if (l->diffuse[1] > m) m = l->diffuse[1];
    if (l->diffuse[2] > m) m = l->diffuse[2];
    return m;
}
// TONO recordado de cada luz. Hace falta porque con energia 0 el difuso queda en (0,0,0) y el negro NO
// tiene tono: sin esta memoria, apagar una luz roja y volver a prenderla la devolvia BLANCA (y un
// parpadeo o un fundido perdian el color). La clave es el puntero pero NUNCA se desreferencia (solo se
// busca), asi que una luz borrada no puede romper nada; se limpia al descargar los scripts.
static std::map<Light*, Vector3> gToneLight;
// tono (rgb normalizado, el mas alto en 1) de la luz: del propio difuso si esta prendida, del recuerdo
// si esta en negro, y blanco si nunca se supo. De paso refresca el recuerdo.
static Vector3 ToneOf(Light* l) {
    float m = BrightnessOf(l);
    if (m > 0.0001f) {
        Vector3 t(l->diffuse[0] / m, l->diffuse[1] / m, l->diffuse[2] / m);
        gToneLight[l] = t;
        return t;
    }
    std::map<Light*, Vector3>::iterator it = gToneLight.find(l);
    if (it != gToneLight.end()) return it->second;
    return Vector3(1, 1, 1);
}
// escribe el difuso como tono * brillo (y recuerda el tono)
static void SetLight(Light* l, const Vector3& tone, float e) {
    gToneLight[l] = tone;
    l->diffuse[0] = tone.x * e; l->diffuse[1] = tone.y * e; l->diffuse[2] = tone.z * e;
    l->diffuse[3] = 1.0f;
    Redraw();
}
// getColor(l) -> r, g, b del TONO (0..1, con el componente mas alto en 1). El BRILLO va aparte, en
// getEnergy(): asi los dos se manejan sin pisarse. Ceros si el objeto no es una luz.
static int LColor(lua_State* L) {
    Light* l = AsLight(L, 1);
    if (!l) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    Vector3 t = ToneOf(l);
    lua_pushnumber(L, t.x); lua_pushnumber(L, t.y); lua_pushnumber(L, t.z);
    return 3;
}
// setColor(l, r, g, b): cambia el TONO (0..1; 1,0,0 = rojo) conservando el brillo actual. Si la luz
// estaba apagada la prende. No hay que reaplicar nada: Light::RenderObject resube el color cada frame.
static int LSetColor(lua_State* L) {
    Light* l = AsLight(L, 1);
    if (!l) return 0;
    float r = (float)luaL_checknumber(L, 2);
    float g = (float)luaL_checknumber(L, 3);
    float b = (float)luaL_checknumber(L, 4);
    if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;
    float m = r; if (g > m) m = g; if (b > m) m = b;
    Vector3 tone = (m > 0.0001f) ? Vector3(r / m, g / m, b / m) : Vector3(1, 1, 1);
    float e = BrightnessOf(l);
    if (e <= 0.0001f) e = (m > 0.0001f) ? m : 1.0f;   // estaba apagada: prenderla al ponerle un color
    SetLight(l, tone, e);
    return 0;
}
// getEnergy(l) -> 0..1: el BRILLO de la luz (0 = apagada). 0 si el objeto no es una luz.
static int LEnergy(lua_State* L) {
    Light* l = AsLight(L, 1);
    lua_pushnumber(L, l ? BrightnessOf(l) : 0.0);
    return 1;
}
// setEnergy(l, n): brillo 0..1 CONSERVANDO el tono. Es lo que se usa para prender, apagar, fundir o
// hacer parpadear una luz: setEnergy(l, 0) y despues setEnergy(l, 1) devuelve el mismo color.
static int LSetEnergy(lua_State* L) {
    Light* l = AsLight(L, 1);
    if (!l) return 0;
    float e = (float)luaL_checknumber(L, 2);
    if (e < 0.0f) e = 0.0f; if (e > 1.0f) e = 1.0f;
    SetLight(l, ToneOf(l), e);
    return 0;
}

Object* W3dScriptParamObject(void* Lv, int idx) {
    lua_State* L = (lua_State*)Lv;
    if (!lua_islightuserdata(L, idx)) return NULL;
    Object* o = (Object*)lua_touserdata(L, idx);
    // un puntero que lua guardo de un objeto que ya no existe (destroy(), una escena descargada): NULL, no
    // memoria liberada
    return W3dObjectLive(o) ? o : NULL;
}

// BUG que arregla esto: antes math.random NUNCA se re-sembraba desde el Core, asi que cada partida salia
// con la MISMA secuencia (la pelota del WhiskPaddle arrancaba igual siempre). Aca sembramos UNA vez por
// lua_State llamando a math.randomseed(semilla) via la API C, con una semilla que VARIA entre ejecuciones.
// Fuentes (todas self-contained del Core): time() (varia por segundo entre corridas), w3dGetTicks()
// (jitter sub-segundo del reloj del motor), la direccion del lua_State (distinta con ASLR) y un contador
// que garantiza que los VARIOS lua_States de un mismo juego (los 4 del whiskpaddle) no queden correlados.
// gSeedExtra la puede reforzar la plataforma via W3dScriptSeed() (ej time(0) del desktop).
static void SeedRandom(lua_State* L) {
    unsigned s = 0;
    if (!gSeedFixed) {
        s = (unsigned)time(NULL);
        s ^= (unsigned)w3dGetTicks() * 2654435761u;      // hash de Knuth: dispersa el tick
        s ^= (unsigned)(size_t)L;                        // direccion del state (ASLR)
        s ^= gSeedExtra;                                 // refuerzo opcional de la plataforma
    }
    s += (++gSemCounter) * 2246822519u;             // cada lua_State: semilla distinta
    lua_getglobal(L, "math");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "randomseed");
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, (lua_Integer)s);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) lua_pop(L, 1);   // no deberia fallar; si falla, saco el error
        } else {
            lua_pop(L, 1);   // randomseed no era funcion
        }
    }
    lua_pop(L, 1);   // math
}

// print de lua -> LOG del Core. El print estandar de luaopen_base escribe a stdout, y en Windows eso ABRE UNA
// CONSOLA (la "terminal blanca" que no deberia aparecer nunca: Whisk3D tiene su propio log). Concatena los args
// con tab igual que el print de Lua, y respeta __tostring.
static int LPrint(lua_State* L) {
    int n = lua_gettop(L);
    std::string s;
    for (int i = 1; i <= n; i++) {
        if (i > 1) s += '\t';
        size_t len = 0;
        const char* p = luaL_tolstring(L, i, &len);   // convierte cualquier valor a string
        if (p) s.append(p, len);
        lua_pop(L, 1);                                  // luaL_tolstring empuja el string: sacarlo
    }
    w3dLogfW("[lua] %s", s.c_str());
    return 0;
}

// QUE CODIGO SE PUEDE CARGAR: en el editor solo TEXTO. El bytecode de Lua no tiene verificador: uno armado a mano lee y
// escribe cualquier memoria, y un proyecto bajado de internet ejecutaria codigo nativo con solo apretar Play (o con
// elegir el objeto: la tarjeta del script lee sus propiedades corriendolo). El juego compilado si carga bytecode: es el
// suyo, el que armo CompileGame (los 2D se compilan sin W3D_NO_EDITOR: todos llevan W3D_GAME_RUNTIME)
#if defined(W3D_NO_EDITOR) || defined(W3D_GAME_RUNTIME)
static const char* const kLoadMode = "bt";
#else
static const char* const kLoadMode = "t";
#endif
// load(chunk [, nombre [, modo [, env]]]) con el modo de arriba, diga lo que diga el script. El load original va como
// UPVALUE de esta funcion (en una global, el script lo llamaria directo con el modo que quisiera)
static int LLoadText(lua_State* L) {
    const int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);                                   // el texto (o la funcion que lo va dando)
    if (n >= 2) lua_pushvalue(L, 2); else lua_pushnil(L);  // el nombre
    lua_pushstring(L, kLoadMode);                          // el modo: siempre el de kLoadMode
    int nArgs = 3;
    if (n >= 4) { lua_pushvalue(L, 4); nArgs = 4; }        // el entorno
    lua_call(L, nArgs, LUA_MULTRET);
    return lua_gettop(L) - n;
}

// stdlib SEGURA: base + table + string + math (sin io/os: un juego no toca discos)
static void OpenLibs(lua_State* L) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);       lua_pop(L, 1);
    lua_pushcfunction(L, LPrint); lua_setglobal(L, "print");   // pisa el print de stdout por el que va al log
    // (loadfile y dofile leen cualquier archivo por su ruta; load queda con el modo de kLoadMode)
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_getglobal(L, "load"); lua_pushcclosure(L, LLoadText, 1); lua_setglobal(L, "load");
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pushnil(L); lua_setfield(L, -2, "dump");   // (sin string.dump no se arma bytecode desde un script)
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
    SeedRandom(L);   // sembrar el random JUSTO despues de abrir math (una vez por lua_State)
}

static void RegisterAPI(lua_State* L) {
    lua_pushcfunction(L, LKey);  lua_setglobal(L, "key");
    lua_pushcfunction(L, LPressedKey); lua_setglobal(L, "keyPressed");
    lua_pushcfunction(L, LPendingTextures); lua_setglobal(L, "pendingTextures");
    lua_pushcfunction(L, LLoadTextures);     lua_setglobal(L, "loadTextures");
    lua_pushcfunction(L, LPressedButton); lua_setglobal(L, "buttonPressed");
    lua_pushcfunction(L, LRandom);   lua_setglobal(L, "random");
    lua_pushcfunction(L, LUnixTime); lua_setglobal(L, "unixTime");   // la hora del sistema (tiempo UNIX + ms)
    lua_pushcfunction(L, LUnixTimeMs);  lua_setglobal(L, "unixTimeMs");    // el mismo tiempo, en milisegundos
    lua_pushcfunction(L, LUnixTimeSec); lua_setglobal(L, "unixTimeSec");   // ...y en segundos con decimales
    lua_pushcfunction(L, LDate);     lua_setglobal(L, "date");       // fecha y hora (local o UTC) de un tiempo UNIX
    lua_pushcfunction(L, LObject); lua_setglobal(L, "object");
    lua_pushcfunction(L, LOption); lua_setglobal(L, "option");
    lua_pushcfunction(L, LProperty); lua_setglobal(L, "property");
    lua_pushcfunction(L, LYo); lua_setglobal(L, "self");
    // estado compartido entre scripts (el canal unico entre lua_States)
    lua_pushcfunction(L, LShared);    lua_setglobal(L, "getShared");
    lua_pushcfunction(L, LSetShared); lua_setglobal(L, "setShared");
    lua_pushcfunction(L, LStick);  lua_setglobal(L, "stick");
    lua_pushcfunction(L, LTrigger); lua_setglobal(L, "trigger");
    lua_pushcfunction(L, LTap);  lua_setglobal(L, "tap");
    lua_pushcfunction(L, LTilt); lua_setglobal(L, "tilt");
    lua_pushcfunction(L, LVibrate); lua_setglobal(L, "vibrate");
    lua_pushcfunction(L, LFinger);   lua_setglobal(L, "finger");
    lua_pushcfunction(L, LTouch); lua_setglobal(L, "touch");
    lua_pushcfunction(L, LMouse);  lua_setglobal(L, "mouse");
    lua_pushcfunction(L, LMouseRel); lua_setglobal(L, "mouseRel");
    lua_pushcfunction(L, LCaptureMouse); lua_setglobal(L, "captureMouse");
    lua_pushcfunction(L, LMouseCaptured); lua_setglobal(L, "mouseCaptured");
    lua_pushcfunction(L, LBeep);   lua_setglobal(L, "beep");
    lua_pushcfunction(L, LButton);  lua_setglobal(L, "button");
    lua_pushcfunction(L, LSetViews);      lua_setglobal(L, "setViews");
    lua_pushcfunction(L, LSetCameraView); lua_setglobal(L, "setCameraView");
    lua_pushcfunction(L, LView);          lua_setglobal(L, "view");
    lua_pushcfunction(L, LPos3);   lua_setglobal(L, "getPos3");
    lua_pushcfunction(L, LSetPos3);lua_setglobal(L, "setPos3");
    lua_pushcfunction(L, LRotateTowards); lua_setglobal(L, "rotateTowards");
    lua_pushcfunction(L, LAnimate); lua_setglobal(L, "animate");
    lua_pushcfunction(L, LInstantiate); lua_setglobal(L, "instantiate");
    lua_pushcfunction(L, LDestroy); lua_setglobal(L, "destroy");
    lua_pushcfunction(L, LLoad);    lua_setglobal(L, "streamLoad");      // el streaming de una instancia / proxy
    lua_pushcfunction(L, LUnload); lua_setglobal(L, "streamUnload");
    lua_pushcfunction(L, LLoadAuto); lua_setglobal(L, "streamAuto");
    lua_pushcfunction(L, LLoaded);   lua_setglobal(L, "isLoaded");
    lua_pushcfunction(L, LFind); lua_setglobal(L, "find");
    lua_pushcfunction(L, LSelected); lua_setglobal(L, "selected");
    lua_pushcfunction(L, LPointWorld); lua_setglobal(L, "worldPoint");
    lua_pushcfunction(L, LAnimScene); lua_setglobal(L, "animScene");
    lua_pushcfunction(L, LStopScene); lua_setglobal(L, "stopScene");
    lua_pushcfunction(L, LAnimSceneFrame); lua_setglobal(L, "animSceneFrame");
    lua_pushcfunction(L, LAnimCurrentScene); lua_setglobal(L, "animSceneCurrent");
    // clips de JERARQUIA: un cabezal por raiz (varias animaciones a la vez) + el retarget por tamano
    lua_pushcfunction(L, LAnimObject);        lua_setglobal(L, "animObject");
    lua_pushcfunction(L, LAnimObjectFrame);   lua_setglobal(L, "animObjectFrame");
    lua_pushcfunction(L, LAnimEndedObject); lua_setglobal(L, "animObjectFinished");
    lua_pushcfunction(L, LAnimCurrentObject);  lua_setglobal(L, "animObjectCurrent");
    lua_pushcfunction(L, LAnimObjectStop);   lua_setglobal(L, "animObjectStop");
    lua_pushcfunction(L, LAnimObjectVelocity); lua_setglobal(L, "animObjectSpeed");
    lua_pushcfunction(L, LAnimRetarget);      lua_setglobal(L, "animRetarget");
    // vertices por grupo (sidecar .grupos.json; ver el bloque VERTICES POR SCRIPT)
    lua_pushcfunction(L, LGroupVertices);   lua_setglobal(L, "vertexGroup");
    lua_pushcfunction(L, LVertexPos);      lua_setglobal(L, "vertexPos");
    lua_pushcfunction(L, LSetVertexPos);   lua_setglobal(L, "setVertexPos");
    lua_pushcfunction(L, LVertexPosAnim); lua_setglobal(L, "vertexPosAnim");
    lua_pushcfunction(L, LSetVertexUV); lua_setglobal(L, "setVertexUV");
    lua_pushcfunction(L, LScrollUV); lua_setglobal(L, "scrollUV");
    lua_pushcfunction(L, LCopyUV); lua_setglobal(L, "copyUV");
    lua_pushcfunction(L, LNumVertices); lua_setglobal(L, "vertexCount");
    lua_pushcfunction(L, LSetVertexColor); lua_setglobal(L, "setVertexColor");
    lua_pushcfunction(L, LSetVertices);     lua_setglobal(L, "setVertices");
    lua_pushcfunction(L, LSetVerticesColor);lua_setglobal(L, "setVerticesColor");
    // ---- API DE OBJETOS (cualquier tipo: mesh, luz, camara, vacio, ui, texto...) ----
    // identidad
    lua_pushcfunction(L, LType);        lua_setglobal(L, "getType");
    lua_pushcfunction(L, LName);      lua_setglobal(L, "getName");
    // posicion: setPosition = teletransporte (absoluto) / mover = relativo por frame (con fisica)
    lua_pushcfunction(L, LPosition);    lua_setglobal(L, "getPosition");
    lua_pushcfunction(L, LSetPosition); lua_setglobal(L, "setPosition");
    lua_pushcfunction(L, LMove);       lua_setglobal(L, "move");
    // rotacion en grados: setRotation = absoluta / girar = relativa por frame
    lua_pushcfunction(L, LRotation);    lua_setglobal(L, "getRotation");
    lua_pushcfunction(L, LSetRotation); lua_setglobal(L, "setRotation");
    lua_pushcfunction(L, LRotationX);    lua_setglobal(L, "getRotationX");     // (un eje solo; ver AxisRotation)
    lua_pushcfunction(L, LRotationY);    lua_setglobal(L, "getRotationY");
    lua_pushcfunction(L, LRotationZ);    lua_setglobal(L, "getRotationZ");
    lua_pushcfunction(L, LSetRotationX); lua_setglobal(L, "setRotationX");
    lua_pushcfunction(L, LSetRotationY); lua_setglobal(L, "setRotationY");
    lua_pushcfunction(L, LSetRotationZ); lua_setglobal(L, "setRotationZ");
    lua_pushcfunction(L, LRotate);       lua_setglobal(L, "rotate");
    // escala: setScale = absoluta / escalar = relativa. OJO: 'escala' lo vuelve a registrar
    // BindsJuego (main/) con la version que ademas atiende getScale() sin argumentos.
    lua_pushcfunction(L, LScaleObj);   lua_setglobal(L, "getScale");
    lua_pushcfunction(L, LSetScale);   lua_setglobal(L, "setScale");
    lua_pushcfunction(L, LScale);     lua_setglobal(L, "scale");
    // visibilidad (show() de BindsJuego es el alias historico de setVisible)
    lua_pushcfunction(L, LVisible);     lua_setglobal(L, "isVisible");
    lua_pushcfunction(L, LSetVisible);  lua_setglobal(L, "setVisible");
    lua_pushcfunction(L, LAnimClip);      lua_setglobal(L, "animClip");
    lua_pushcfunction(L, LCurrentAnim);    lua_setglobal(L, "getAnim");
    lua_pushcfunction(L, LAnimFrame);     lua_setglobal(L, "animFrame");
    lua_pushcfunction(L, LAnimLength);     lua_setglobal(L, "animLength");
    lua_pushcfunction(L, LEndedAnim);   lua_setglobal(L, "animFinished");
    lua_pushcfunction(L, LAnimVelocity); lua_setglobal(L, "animSpeed");
    lua_pushcfunction(L, LBonePoint);    lua_setglobal(L, "bonePoint");
    lua_pushcfunction(L, LAnimLayer);        lua_setglobal(L, "animLayer");
    lua_pushcfunction(L, LAnimLayerWeight);    lua_setglobal(L, "animLayerWeight");
    lua_pushcfunction(L, LAnimLayerFrame);   lua_setglobal(L, "animLayerFrame");
    lua_pushcfunction(L, LAnimEndedLayer); lua_setglobal(L, "animLayerFinished");
    lua_pushcfunction(L, LAnimLayerBone);   lua_setglobal(L, "animLayerBone");
    lua_pushcfunction(L, LAnimLayerVel);     lua_setglobal(L, "animLayerSpeed");
    lua_pushcfunction(L, LAnimLayers);       lua_setglobal(L, "animLayers");
    lua_pushcfunction(L, LAnimTransition);  lua_setglobal(L, "animTransition");
    lua_pushcfunction(L, LSceneLayer);        lua_setglobal(L, "sceneLayer");
    lua_pushcfunction(L, LSceneLayerWeight);    lua_setglobal(L, "sceneLayerWeight");
    lua_pushcfunction(L, LSceneLayerFrame);   lua_setglobal(L, "sceneLayerFrame");
    lua_pushcfunction(L, LSceneEndedLayer); lua_setglobal(L, "sceneLayerFinished");
    lua_pushcfunction(L, LSceneLayers);       lua_setglobal(L, "sceneLayers");
    lua_pushcfunction(L, LObjectLayer);        lua_setglobal(L, "objectLayer");
    lua_pushcfunction(L, LObjectLayerWeight);    lua_setglobal(L, "objectLayerWeight");
    lua_pushcfunction(L, LObjectLayerFrame);   lua_setglobal(L, "objectLayerFrame");
    lua_pushcfunction(L, LObjectEndedLayer); lua_setglobal(L, "objectLayerFinished");
    lua_pushcfunction(L, LObjectLayerVel);     lua_setglobal(L, "objectLayerSpeed");
    lua_pushcfunction(L, LObjectLayers);       lua_setglobal(L, "objectLayers");
    lua_pushcfunction(L, LLookTo);          lua_setglobal(L, "lookAt");
    // luz
    lua_pushcfunction(L, LColor);       lua_setglobal(L, "getColor");
    lua_pushcfunction(L, LSetColor);    lua_setglobal(L, "setColor");
    lua_pushcfunction(L, LEnergy);     lua_setglobal(L, "getEnergy");
    lua_pushcfunction(L, LSetEnergy);  lua_setglobal(L, "setEnergy");
    // config persistente (idioma, mute, ...) + mute global
    lua_pushcfunction(L, LConfig);        lua_setglobal(L, "getConfig");
    lua_pushcfunction(L, LSetConfig);     lua_setglobal(L, "setConfig");
    lua_pushcfunction(L, LSaveConfig); lua_setglobal(L, "saveConfig");
    lua_pushcfunction(L, LLoadConfig);  lua_setglobal(L, "loadConfig");
    lua_pushcfunction(L, LSilence);     lua_setglobal(L, "mute");
    lua_pushcfunction(L, LIsMute);      lua_setglobal(L, "isMuted");
    // log del Core (los niveles del viewport Console) + la mini-consola por script
    lua_pushcfunction(L, LLogInfo);     lua_setglobal(L, "info");
    lua_pushcfunction(L, LLogNotice);    lua_setglobal(L, "warning");
    lua_pushcfunction(L, LLogError);    lua_setglobal(L, "error");   // pisa el error() de lua (ver arriba)
    lua_pushcfunction(L, LLogDebug);  lua_setglobal(L, "debugLog");
    lua_pushcfunction(L, LIsDebug);     lua_setglobal(L, "isDebug");
    lua_pushcfunction(L, LLogCount); lua_setglobal(L, "logCount");
    lua_pushcfunction(L, LLogLine);    lua_setglobal(L, "logLine");
    // FISICA minima del Core (getVelocity/accelerate/box/bounce/bounceOn/bounceInside): el motor
    // integra y rebota, el lua no calcula posiciones a mano. Ver physics/W3dPhysics.h.
    W3dPhysicsRegisterBinds((void*)L);
    // CUERPOS RIGIDOS (physicsVel/physicsImpulse/physicsForce/physicsRay/contacts/
    // physicsActive/physicsGravity): cajas con masa, gravedad e impulsos. Ver physics/W3dRigid.h.
    W3dRigidRegisterBinds((void*)L);
    // COLISION CONTRA MALLAS (colGround/colWall/colRay/colForget): piso y paredes de un escenario
    // para un personaje movido por script. Ver physics/W3dColMesh.h.
    W3dColMeshRegisterBinds((void*)L);
    // HITBOX (hitboxActive/setHitboxActive/setHitboxSize/hitboxInside/hitboxTouching/showHitboxes):
    // cajas de deteccion con eventos alEntrar/alQuedarse/alSalir. Ver physics/W3dHitbox.h.
    W3dHitboxRegisterBinds((void*)L);
    // RED (netHost/netFind/netMatches/netJoin/netSend/netReceive...): LAN, wifi o internet, con el
    // permiso que de la aplicacion. Ver net/W3dNet.h.
    w3dNet::RegisterBinds((void*)L);
    if (gBindExtra) gBindExtra((void*)L);   // la API 2D del editor / plataforma
    PropertiesAsVariables(L);               // speed en vez de property("speed") (ver LGlobalIndex)
}

static bool RunFile(lua_State* L, const std::string& path) {
    // leer por el filesystem del Core (asset-aware): en Android el .lua vive en los assets del APK,
    // no en disco, y luaL_loadfile (que usa fopen) fallaba. luaL_loadbuffer detecta el bytecode .luac
    // por el header igual que loadfile, asi que los compilados siguen cargando por el mismo camino.
    std::vector<unsigned char> src;
    // la FUENTE que el streaming ya trajo a memoria (W3DREC_SCRIPT): sin tocar el disco. Solo en el JUEGO COMPILADO,
    // donde los scripts no cambian; el editor los lee frescos siempre (el IDE los edita y los recarga jugando, y una
    // copia retenida haria correr la version vieja)
    const std::vector<unsigned char>* inMemory = 0;
#ifdef W3D_NO_EDITOR
    { W3dResource* rf = W3dResourceFind(W3DREC_SCRIPT, path);
      inMemory = rf ? (const std::vector<unsigned char>*)W3dResourceResolve(rf) : 0; }
#endif
    if (inMemory && !inMemory->empty()) src = *inMemory;
    else if (!w3dFileSystem::ReadFileBytes(path, src) || src.empty()) {
        gLastError = "no pude leer el script: " + path;
        w3dLogfE("Script: %s", gLastError.c_str());
        return false;
    }
    // saltar el BOM UTF-8 (EF BB BF) que algunos editores ponen a los .lua de texto: luaL_loadfile lo hacia
    // y loadbuffer no, asi que sin esto un script con BOM daba un error de sintaxis raro. El bytecode .luac
    // empieza con 0x1B (nunca con BOM), asi que no se toca.
    size_t off = (src.size() >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) ? 3 : 0;
    // chunkname con '@': asi los errores salen "whiskpaddle.lua:12: ..." (igual que loadfile) en vez de
    // "[string \"...\"]:12" -> el usuario lee claro DONDE esta el error de su juego.
    std::string chunk = "@" + path;
    if (luaL_loadbufferx(L, (const char*)&src[0] + off, src.size() - off, chunk.c_str(), kLoadMode) != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        gLastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error de lua";
        w3dLogfE("Script: %s", gLastError.c_str());
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool W3dScriptLoad(Object* owner) {
    if (!owner || !owner->scriptData || owner->scriptData->scripts.empty()) return false;
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
    if (it != gInstances.end()) return true;   // ya cargadas
    std::vector<W3dScriptInst*> list;
    gLastError.clear();
    for (size_t i = 0; i < owner->scriptData->scripts.size(); i++) {
        const W3dScriptEntry& ent = owner->scriptData->scripts[i];
        // los indices del vector de instancias ESPEJAN los de scripts[] (ResolverRef
        // usa el mismo idx); una entrada vacia o rota deja un NULL en su lugar
        W3dScriptInst* inst = NULL;
        if (!ent.path.empty()) {
            inst = new W3dScriptInst();
            inst->owner = owner;
            inst->off = ent.off;
            inst->L = luaL_newstate();
            OpenLibs(inst->L);
            lua_pushlightuserdata(inst->L, inst);
            lua_setfield(inst->L, LUA_REGISTRYINDEX, "w3d_inst");
            lua_pushlightuserdata(inst->L, owner);
            lua_setfield(inst->L, LUA_REGISTRYINDEX, "w3d_owner");
            // SU LIBRERIA (si es de una): su .lua es una entrada de ella, o a su dueno lo genero un proxy de ella. Las
            // rutas que nombra son primero de ella (W3dScriptLibPath)
            {
                std::string lib;
                if (ent.path.compare(0, 4, "lib:") == 0) {
                    const size_t b = ent.path.find('/', 4);
                    if (b != std::string::npos && b > 4) lib = ent.path.substr(4, b - 4);
                }
                if (lib.empty() && W3dScriptLibHook) lib = W3dScriptLibHook(owner);
                if (!lib.empty()) {
                    lua_pushstring(inst->L, lib.c_str());
                    lua_setfield(inst->L, LUA_REGISTRYINDEX, "w3d_lib");
                }
            }
            RegisterAPI(inst->L);
            if (!RunFile(inst->L, ent.path)) {
                lua_close(inst->L);
                delete inst;
                inst = NULL;
            }
        }
        list.push_back(inst);
    }
    bool any = false;
    for (size_t i = 0; i < list.size(); i++) if (list[i]) any = true;
    if (!any) return false;
    gInstances[owner] = list;
    w3dLogf("Script: %s cargo %d script(s)", owner->name.c_str(), (int)list.size());
    return true;
}

static W3dScriptInst* InstOf(Object* owner, int idx) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
    if (it == gInstances.end()) return NULL;
    if (idx < 0 || idx >= (int)it->second.size()) return NULL;
    return it->second[idx];
}

void W3dScriptResolveRef(Object* owner, int idx, const std::string& prop, Object* obj) {
    W3dScriptInst* inst = InstOf(owner, idx);
    if (inst) inst->refs[prop] = obj;
}

void W3dScriptResolveOption(Object* owner, int idx, const std::string& prop, const std::string& value) {
    W3dScriptInst* inst = InstOf(owner, idx);
    if (inst) inst->options[prop] = value;
}

void W3dScriptResolveValue(Object* owner, int idx, const std::string& prop, const std::string& value) {
    W3dScriptInst* inst = InstOf(owner, idx);
    if (inst) inst->values[prop] = value;
}

// llama una funcion global sin argumentos o con dt; false si no existe o fallo
// las llamadas del motor a lua (inicio/actualizar): el perfil por muestreo (luaProfile, script/BindsGame.cpp) tira la
// primera muestra de cada una, que trae el tiempo de afuera de lua (el render del cuadro anterior)
unsigned g_w3dScriptCalls = 0;
static bool Call(W3dScriptInst* inst, const char* fn, bool withDt, float dt) {
    if (inst->off) return false;   // apagado con el ojo: el motor no lo llama
    lua_State* L = inst->L;
    g_w3dScriptCalls++;
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    if (withDt) lua_pushnumber(L, dt);
    if (lua_pcall(L, withDt ? 1 : 0, 0, 0) != LUA_OK) {
        gLastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error de lua";
        w3dLogfE("Script %s(): %s", fn, gLastError.c_str());
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool W3dScriptStart(Object* owner) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
    if (it == gInstances.end()) return false;
    bool ok = false;
    for (size_t i = 0; i < it->second.size(); i++)
        if (it->second[i] && !it->second[i]->off) {
            it->second[i]->startDone = true;
            if (Call(it->second[i], "start", false, 0.0f)) ok = true;
        }
    return ok;
}

bool W3dScriptCallButton(Object* owner, int idx, const std::string& prop) {
    if (!owner) return false;
    // sin el juego andando no hay instancias: se carga el script para esta llamada (y despues se descarga)
    bool temp = false;
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
    if (it == gInstances.end()) {
        if (!W3dScriptLoad(owner)) return false;
        temp = true;
        it = gInstances.find(owner);
        if (it == gInstances.end()) return false;
    }
    bool ok = false;
    if (idx >= 0 && idx < (int)it->second.size() && it->second[(size_t)idx] && it->second[(size_t)idx]->L) {
        lua_State* L = it->second[(size_t)idx]->L;
        lua_getglobal(L, "properties");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, prop.c_str());
            if (lua_istable(L, -1)) {   // { "Etiqueta", funcion }: la primera funcion de la tabla
                const int n = (int)lua_rawlen(L, -1);
                bool has = false;
                for (int i = 1; i <= n && !has; i++) {
                    lua_rawgeti(L, -1, i);
                    if (lua_isfunction(L, -1)) { lua_replace(L, -2); has = true; } else lua_pop(L, 1);
                }
                if (!has) { lua_pop(L, 1); lua_pushnil(L); }
            }
            if (lua_isfunction(L, -1)) {
                g_w3dScriptCalls++;
                if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                    gLastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error de lua";
                    w3dLogfE("Script (boton %s): %s", prop.c_str(), gLastError.c_str());
                    lua_pop(L, 1);
                } else ok = true;
            } else lua_pop(L, 1);
        }
        lua_pop(L, 1);   // propiedades
    }
    if (temp) W3dScriptUnloadFrom(owner);
    Redraw();
    return ok;
}

void W3dScriptSetOff(Object* owner, int idx, bool off) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
    if (it == gInstances.end() || idx < 0 || idx >= (int)it->second.size() || !it->second[idx]) return;
    W3dScriptInst* inst = it->second[idx];
    inst->off = off;
    if (!off && !inst->startDone) {   // apagado desde el Play: su start() corre ahora, la primera vez
        inst->startDone = true;
        Call(inst, "start", false, 0.0f);
    }
}

bool W3dScriptUpdate(Object* owner, float dt) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
    if (it == gInstances.end()) return false;
    bool ok = false;
    for (size_t i = 0; i < it->second.size(); i++)
        if (it->second[i] && Call(it->second[i], "update", true, dt)) ok = true;
    // SNAPSHOT para el flanco de keyPressed()/buttonPressed(). Con W3dScriptEndFrame (el editor y el
    // runtime 3D la llaman una vez por frame, despues de TODOS los objetos) el snapshot lo toma ella; aca solo
    // queda el camino viejo para los runtimes de un unico duenio que no la llaman.
    if (!gEndFrameExternal) {
        gKeysPrev = gKeys;
        gButtonsPadPrev = gButtonsPad;
        for (int m = 0; m < W3D_GAMEPADS; m++) gButtonsMPrev[m] = gButtonsM[m];
        gPressedKeys.clear();
    }
    return ok;
}

// ---- EVENTOS (hitbox / contactos): fn(objs..., nums...) --------------------
static int EventIndex(const char* fn) {
    for (int i = 0; i < W3D_EVENTS_SCRIPT; i++)
        if (strcmp(fn, kEventsScript[i]) == 0) return i;
    return -1;
}

static bool CallEvent(W3dScriptInst* inst, const char* fn, int idx, Object* const* objs, int nObjs,
                         const float* nums, int nNums) {
    if (inst->off) return false;                          // apagado con el ojo: no recibe eventos
    if (idx >= 0 && inst->events[idx] == 0) return false;   // ya se sabe que no lo define
    lua_State* L = inst->L;
    g_w3dScriptCalls++;
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        if (idx >= 0) inst->events[idx] = 0;
        return false;
    }
    if (idx >= 0) inst->events[idx] = 1;
    // los objetos viajan como TODO Object* hacia lua: lightuserdata (mismo tipo que self()/object())
    for (int i = 0; i < nObjs; i++) {
        if (objs && objs[i]) lua_pushlightuserdata(L, objs[i]); else lua_pushnil(L);
    }
    for (int i = 0; i < nNums; i++) lua_pushnumber(L, nums ? nums[i] : 0.0f);
    if (lua_pcall(L, nObjs + nNums, 0, 0) != LUA_OK) {
        gLastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error de lua";
        w3dLogfE("Script %s(): %s", fn, gLastError.c_str());
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool W3dScriptRun(Object* owner, const char* code, std::string& error) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
    if (it == gInstances.end() || it->second.empty()) { error = "el objeto no tiene scripts corriendo"; return false; }
    for (size_t i = 0; i < it->second.size(); i++) {
        lua_State* L = it->second[i]->L;
        if (!L) continue;
        if (luaL_loadbufferx(L, code, strlen(code), "=consola", "t") != LUA_OK || lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
            error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
            lua_pop(L, 1);
            return false;
        }
    }
    return true;
}

bool W3dScriptEvent(Object* owner, const char* fn, Object* const* objs, int nObjs,
                     const float* nums, int nNums) {
    if (!owner || !fn) return false;
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
    if (it == gInstances.end()) return false;
    const int idx = EventIndex(fn);
    bool ok = false;
    // por INDICE y releyendo el mapa en cada vuelta: el evento puede recargar los scripts de
    // su propio duenio (edicion en vivo) y dejar el vector viejo liberado
    for (size_t i = 0; ; i++) {
        it = gInstances.find(owner);
        if (it == gInstances.end() || i >= it->second.size()) break;
        W3dScriptInst* inst = it->second[i];
        if (inst && CallEvent(inst, fn, idx, objs, nObjs, nums, nNums)) ok = true;
    }
    return ok;
}

bool W3dScriptEvent(Object* owner, const char* fn, Object* other, const float* nums, int nNums) {
    Object* objs[1]; objs[0] = other;
    return W3dScriptEvent(owner, fn, objs, 1, nums, nNums);
}

void W3dScriptEndFrame() {
    gEndFrameExternal = true;
    gKeysPrev = gKeys;
    gButtonsPadPrev = gButtonsPad;
    for (int m = 0; m < W3D_GAMEPADS; m++) gButtonsMPrev[m] = gButtonsM[m];
    gPressedKeys.clear();
    // los destroy() del frame: ahora que no corre ningun update()
    W3dScriptDestroyPending();
    // la red: manda lo que los scripts pidieron, lee lo que llego, reenvia lo confiable que falta
    w3dNet::Update();
}

// ---- LA DESTRUCCION (destruir de lua, ver W3dScript.h) ----
static void CollectSubtree(Object* o, std::vector<Object*>& out) {
    if (!o) return;
    out.push_back(o);
    for (size_t i = 0; i < o->Childrens.size(); i++) CollectSubtree(o->Childrens[i], out);
}
static void RemoveCurvesOf(std::vector<AnimationObject>& v, const std::set<Object*>& s) {
    for (size_t i = v.size(); i-- > 0; ) if (s.count(v[i].obj)) v.erase(v.begin() + (long)i);
}
// una raiz 2D (escena UI): la registra el sistema de escenas (W3dEscena) y el juego compilado la sigue
// dibujando y corriendo como la escena activa; liberarla dejaba ese puntero colgado (SIGSEGV al frame siguiente)
bool W3dScriptIsSceneUI(const Object* o) {
    extern Object* SceneCollection;
    if (!o || const_cast<Object*>(o)->getType() != ObjectType::ui) return false;
    return !o->Parent || o->Parent == SceneCollection;
}
void W3dScriptReleaseRefsA(const std::vector<Object*>& objs) {
    if (objs.empty()) return;
    std::set<Object*> all(objs.begin(), objs.end());
    for (std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.begin(); it != gInstances.end(); ++it)
        for (size_t k = 0; k < it->second.size(); k++) {
            W3dScriptInst* inst = it->second[k];
            if (!inst) continue;
            for (std::map<std::string, Object*>::iterator r = inst->refs.begin(); r != inst->refs.end(); ++r)
                if (all.count(r->second)) r->second = NULL;
        }
}
void W3dScriptDestroyObject(Object* o) {
    extern Object* SceneCollection;
    if (!o || o == SceneCollection || W3dIsRootRegistered(o) || W3dScriptIsSceneUI(o)) return;
    std::vector<Object*> sub;
    CollectSubtree(o, sub);
    std::set<Object*> all(sub.begin(), sub.end());
    for (size_t i = 0; i < sub.size(); i++) {
        W3dScriptUnloadFrom(sub[i]);   // (su lua_State se cierra: no queda nadie que lo nombre como self())
        W3dPhysicsForget(sub[i]);
    }
    // nadie lo sigue teniendo en object("x")
    W3dScriptReleaseRefsA(sub);
    // las curvas de las animaciones de escena que lo nombran (la activa y las demas de la raiz activa)
    RemoveCurvesOf(AnimationObjects, all);
    for (size_t e = 0; e < SceneAnimations.size(); e++) if (SceneAnimations[e]) RemoveCurvesOf(SceneAnimations[e]->objects, all);
    // la seleccion
    for (size_t i = ObjSelects.size(); i-- > 0; ) if (all.count(ObjSelects[i])) ObjSelects.erase(ObjSelects.begin() + (long)i);
    if (all.count(ActiveObj)) ActiveObj = NULL;
    if (all.count(CollectionActive)) CollectionActive = SceneCollection;
    // descolgarlo y liberarlo (~Object olvida sus cuerpos rigidos, sus hitbox y sus pares)
    if (o->Parent) {
        std::vector<Object*>& ch = o->Parent->Childrens;
        for (size_t i = 0; i < ch.size(); i++) if (ch[i] == o) { ch.erase(ch.begin() + (long)i); break; }
    }
    W3dFreeSubtree(o);
    Redraw();
}
void W3dScriptDestroyNow(Object* o) {
    if (!o) return;
    if (gObjectDestroyFn) gObjectDestroyFn(o);
    else W3dScriptDestroyObject(o);
}

// ---- un valor de lua que viaja entre scripts (ver W3dScript.h) ----
static W3dValueLua ReadValueRec(lua_State* L, int idx, int prof) {
    W3dValueLua v;
    if (idx < 0) idx = lua_gettop(L) + idx + 1;   // (absoluto: la tabla se recorre con pushes encima)
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN: v.type = W3dValueLua::Bool; v.num = lua_toboolean(L, idx) ? 1.0 : 0.0; break;
        case LUA_TNUMBER:
            v.type = W3dValueLua::Num;
            v.integer = lua_isinteger(L, idx) != 0;
            v.num = v.integer ? (double)lua_tointeger(L, idx) : (double)lua_tonumber(L, idx);
            break;
        case LUA_TSTRING:  { size_t n = 0; const char* s = lua_tolstring(L, idx, &n);
                             v.type = W3dValueLua::Text; v.text.assign(s ? s : "", n); break; }
        case LUA_TLIGHTUSERDATA: v.type = W3dValueLua::Obj; v.obj = lua_touserdata(L, idx); break;
        case LUA_TTABLE:
            if (prof >= 16) break;
            v.type = W3dValueLua::Table;
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                W3dValueLua k = ReadValueRec(L, -2, prof + 1);
                W3dValueLua x = ReadValueRec(L, -1, prof + 1);
                if (k.type != W3dValueLua::Nil) v.fields.push_back(std::make_pair(k, x));
                lua_pop(L, 1);
            }
            break;
        default: break;   // (nil, funciones, corrutinas, userdata pesados)
    }
    return v;
}
W3dValueLua W3dValueLuaRead(void* L, int idx) { return ReadValueRec((lua_State*)L, idx, 0); }
static void SetValue(lua_State* L, const W3dValueLua& v) {
    switch (v.type) {
        case W3dValueLua::Bool:  lua_pushboolean(L, v.num != 0.0); break;
        case W3dValueLua::Num:
            if (v.integer) lua_pushinteger(L, (lua_Integer)v.num); else lua_pushnumber(L, (lua_Number)v.num);
            break;
        case W3dValueLua::Text: lua_pushlstring(L, v.text.data(), v.text.size()); break;
        case W3dValueLua::Obj:   lua_pushlightuserdata(L, v.obj); break;
        case W3dValueLua::Table:
            lua_createtable(L, 0, (int)v.fields.size());
            for (size_t i = 0; i < v.fields.size(); i++) {
                SetValue(L, v.fields[i].first);
                SetValue(L, v.fields[i].second);
                lua_settable(L, -3);
            }
            break;
        default: lua_pushnil(L); break;
    }
}
bool W3dScriptCallWithData(Object* owner, const char* fn, const std::string& text, const W3dValueLua& value,
                             const std::string& of) {
    if (!owner || !fn) return false;
    bool ok = false;
    // (por INDICE y releyendo el mapa: alRecibir puede recargar o destruir scripts)
    for (size_t i = 0; ; i++) {
        std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
        if (it == gInstances.end() || i >= it->second.size()) break;
        W3dScriptInst* inst = it->second[i];
        if (!inst || !inst->L || inst->off) continue;   // (apagado con el ojo: tampoco recibe mensajes)
        lua_State* L = inst->L;
        lua_getglobal(L, fn);
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
        g_w3dScriptCalls++;
        lua_pushlstring(L, text.data(), text.size());
        SetValue(L, value);
        lua_pushlstring(L, of.data(), of.size());
        if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
            gLastError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error de lua";
            w3dLogfE("Script %s(): %s", fn, gLastError.c_str());
            lua_pop(L, 1);
            continue;
        }
        ok = true;
    }
    return ok;
}

void W3dScriptDestroyPending() {
    if (gDestroy.empty()) return;
    std::vector<W3dDestroyRequest> list;
    list.swap(gDestroy);   // (destruir uno puede correr codigo que pida otro: va al frame siguiente)
    for (size_t i = 0; i < list.size(); i++) {
        Object* o = list[i].o;
        if (!W3dLiveObjectSerial(o, list[i].serial)) continue;   // ya se fue (un ancestro, otra via)
        if (gObjectDestroyFn) gObjectDestroyFn(o);
        else W3dScriptDestroyObject(o);
    }
}
int W3dScriptDestroyRequests() { return (int)gDestroy.size(); }
// los destroy() pedidos, SIN aplicarlos: los vivos van a 'out' y la cola queda vacia. Lo usa el editor con el juego
// parado (un boton de las propiedades): los borra el con su Ctrl+Z, en vez de liberarlos aca
void W3dScriptTakeDestroyRequests(std::vector<Object*>& out) {
    std::vector<W3dDestroyRequest> list;
    list.swap(gDestroy);
    for (size_t i = 0; i < list.size(); i++)
        if (W3dLiveObjectSerial(list[i].o, list[i].serial)) out.push_back(list[i].o);
}

// cierra el lua_State y libera cada instancia de una lista (lo comparten DescargarDe/DescargarTodo)
static void CloseInstances(std::vector<W3dScriptInst*>& list) {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i]) {
            if (list[i]->L) lua_close(list[i]->L);
            delete list[i];
        }
}

void W3dScriptUnloadFrom(Object* owner) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.find(owner);
    if (it == gInstances.end()) return;
    CloseInstances(it->second);
    gInstances.erase(it);
}

void W3dScriptUnloadAll() {
    gDestroy.clear();   // (lo pedido y no aplicado: el Stop repone la escena como estaba)
    for (std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstances.begin();
         it != gInstances.end(); ++it)
        CloseInstances(it->second);
    gInstances.clear();
    gToneLight.clear();   // el recuerdo del tono de las luces muere con la partida (se re-deriva del difuso)
    gShared.clear(); // el estado compartido entre scripts es DEL PLAY: la proxima partida arranca limpia
    gMouseCapture = false; gMouseDX = 0; gMouseDY = 0;   // la captura del mouse muere con la partida (STOP la suelta)
    W3dPhysicsClear(); // y las velocidades tambien: el Stop deja la escena quieta (y sin punteros viejos)
    W3dHitboxEndSession(); // y los pares de hitbox / contactos (la proxima partida arranca sin
                           // "adentro") y el overlay showHitboxes que haya prendido un script
    w3dNet::Close();     // y la sesion de red (el Stop se despide de los otros)
    ViewsRestart();      // y la pantalla dividida (el editor vuelve a una vista)
}

// ordena las propiedades por la posicion en que estan DECLARADAS en el archivo (ver abajo).
// Va a nivel de archivo a proposito: en C++03 (Symbian/RVCT) un tipo LOCAL no se puede usar como
// argumento de plantilla, asi que un functor declarado adentro de la funcion no compila en stable_sort.
struct OrderDecl {
    const std::string* text;
    bool operator()(const W3dScriptProp& a, const W3dScriptProp& b) const {
        size_t pa = text->find(a.name), pb = text->find(b.name);
        if (pa == std::string::npos && pb == std::string::npos) return a.name < b.name;
        if (pa == std::string::npos) return false;
        if (pb == std::string::npos) return true;
        if (pa != pb) return pa < pb;
        return a.name < b.name;
    }
};

// (leer las propiedades corre el cuerpo del script en el EDITOR, al elegir el objeto: uno que no termina colgaba el
//  editor. El cuerpo de un script de verdad declara cosas: un millon de instrucciones sobra)
static void PropsHook(lua_State* L, lua_Debug*) { luaL_error(L, "el script tarda demasiado al leer sus propiedades"); }
void W3dScriptApiNames(std::vector<std::string>* out) {
    if (!out) return;
    out->clear();
    lua_State* L = luaL_newstate();
    OpenLibs(L);
    RegisterAPI(L);
    lua_pushglobaltable(L);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const std::string name = lua_tostring(L, -2);
            const int t = lua_type(L, -1);
            if (t == LUA_TFUNCTION) out->push_back(name);
            else if (t == LUA_TTABLE && name != "_G" && name != "package") {   // math, string, table...
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    if (lua_type(L, -2) == LUA_TSTRING) out->push_back(name + "." + lua_tostring(L, -2));
                    lua_pop(L, 1);
                }
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_close(L);
    std::sort(out->begin(), out->end());
}

bool W3dScriptReadProperties(const std::string& path, std::vector<W3dScriptProp>* props) {
    if (props) props->clear();
    if (path.empty()) return false;
    lua_State* L = luaL_newstate();
    OpenLibs(L);
    RegisterAPI(L);   // el script puede llamar a la API en su cuerpo: que no reviente
    lua_sethook(L, PropsHook, LUA_MASKCOUNT, 1000000);
    bool ok = RunFile(L, path);
    if (ok && props) {
        lua_getglobal(L, "properties");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    W3dScriptProp p;
                    p.name = lua_tostring(L, -2);
                    p.type = 0;
                    int tv = lua_type(L, -1);
                    if (tv == LUA_TFUNCTION) {
                        // una FUNCION = un BOTON en la tarjeta del script (su texto, el nombre)
                        p.type = 3; p.label = p.name;
                    } else if (tv == LUA_TTABLE) {
                        // tabla de strings = DESPLEGABLE de opciones; con una FUNCION adentro = un BOTON, y el primer
                        // texto es lo que dice: { "Volver a cero", function() ... end }
                        p.type = 1;
                        int n = (int)lua_rawlen(L, -1);
                        bool withFunction = false;
                        for (int i = 1; i <= n; i++) {
                            lua_rawgeti(L, -1, i);
                            if (lua_isfunction(L, -1)) withFunction = true;
                            else if (lua_type(L, -1) == LUA_TSTRING) p.options.push_back(lua_tostring(L, -1));
                            lua_pop(L, 1);
                        }
                        if (withFunction) {
                            p.type = 3;
                            p.label = p.options.empty() ? p.name : p.options[0];
                            p.options.clear();
                        }
                    } else if (tv == LUA_TNUMBER) {
                        // default numerico = propiedad de VALOR editable (frame, id...)
                        p.type = 2; p.subtype = 0;
                        char buf[48];
                        if (lua_isinteger(L, -1))
                            snprintf(buf, sizeof(buf), "%lld", (long long)lua_tointeger(L, -1));
                        else
                            snprintf(buf, sizeof(buf), "%g", (double)lua_tonumber(L, -1));
                        p.defaultValue = buf;
                    } else if (tv == LUA_TBOOLEAN) {
                        // default bool = checkbox por instancia (agarrada...)
                        p.type = 2; p.subtype = 1;
                        p.defaultValue = lua_toboolean(L, -1) ? "true" : "false";
                    } else if (tv == LUA_TSTRING) {
                        // "objeto" es LA convencion historica de referencia; cualquier
                        // otro string es un default de TEXTO editable (etiqueta...)
                        const char* s = lua_tostring(L, -1);
                        if (strcmp(s, "object") != 0) { p.type = 2; p.subtype = 2; p.defaultValue = s; }
                    }
                    props->push_back(p);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        // el orden de una tabla lua es INDETERMINADO (hash): cada lectura barajaba
        // las filas del panel. Se ordena por la posicion en el ARCHIVO (el orden en
        // que el autor las declaro); si no aparecen (bytecode raro), alfabetico.
        {
            std::vector<unsigned char> src;
            std::string text;
            if (w3dFileSystem::ReadFileBytes(path, src) && !src.empty())
                text.assign((const char*)&src[0], src.size());
            OrderDecl cmp; cmp.text = &text;
            std::stable_sort(props->begin(), props->end(), cmp);
        }
    }
    lua_close(L);
    return ok;
}
