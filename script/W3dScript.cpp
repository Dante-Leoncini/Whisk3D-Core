// ============================================================================
//  W3dScript.cpp — ver W3dScript.h. Dialecto C++03 (Symbian compila esto).
// ============================================================================
#include "script/W3dScript.h"
#include "objects/Objects.h"
#include "animation/VertexAnimation.h"   // animar(): FindTargetAnim / nextAnim
#include "animation/Animation.h"         // w3dGetTicks(): reloj self-contained del Core (sembrar el random)
#include "objects/Mesh.h"
#include "objects/Light.h"   // binds de LUZ: color()/setColor()/energia()/setEnergia()
#include "objects/Armature.h"            // binds de ESQUELETO: animClip()/animFrame()/huesoPunto()...
#include "animation/SkeletalAnimation.h" // W3dArmaturePlayClip / EvaluarPoseEsqueleto
#include "animation/W3dAnimSet.h"        // las CAPAS de clips de jerarquia de una raiz (objetoCapa...)
#include "math/Quaternion.h"
#include "w3dFilesystem.h"   // leer el .lua para ORDENAR las propiedades como estan declaradas
#include "base/W3dConfig.h"  // config()/setConfig()/silenciar(): persistencia + mute para los juegos lua
#include "physics/W3dFisica.h"  // fisica minima del Core: velocidad/rebotar/... (los binds los registra ella)
#include "physics/W3dRigido.h"
#include "physics/W3dColMalla.h" // colision contra triangulos (colSuelo/colPared/colRayo)  // cuerpos rigidos del Core (fisicaVel/fisicaRayo/contactos/...)
#include "physics/W3dHitbox.h"   // hitbox: binds + limpiar pares al Stop
#include "net/W3dRed.h"          // jugar en red: binds red*, se atiende al fin de cada cuadro y se cierra al Stop
#include "io/W3dRecursos.h"      // las FUENTES de los scripts como recurso (el streaming las pide antes)
#include "w3dlog.h"
#include <math.h>
#include <map>
#include <algorithm>
#include <stdio.h>   // snprintf: formatear el default numerico / el dump de compartido()
#include <string.h>
#include <time.h>    // time(): fuente que VARIA entre ejecuciones para la semilla del random

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#ifdef W3D_SIN_FISICA
// ============================================================================
//  BUILD SIN FISICA (-DW3D_SIN_FISICA): physics/W3dFisica.cpp NO se compila.
//  Aca se implementa TODA la API de physics/W3dFisica.h como stubs no-op C++03:
//  los getters devuelven 0/false, los setters no hacen nada, y los binds lua
//  (velocidad/acelerar/caja/rebotar/rebotarEn/rebotarDentro) se registran igual
//  pero vacios -> un script que los llame NO crashea, solo avisa UNA vez por log.
//  Los callers (w3drun, SimJuego, BindsJuego) no cambian: linkean contra esto.
//  OJO: con esta macro definida NO compilar W3dFisica.cpp (simbolo duplicado).
// ============================================================================
static bool gFisicaAviso = false;
static void FisicaAvisoOff() {
    if (gFisicaAviso) return;
    gFisicaAviso = true;
    w3dLogW("script: fisica desactivada en este build (W3D_SIN_FISICA); velocidad/rebotar no hacen nada");
}
void W3dFisicaPaso(float) {}
void W3dFisicaLimpiar(void) {}
void W3dFisicaOlvidar(Object*) {}
bool W3dFisicaTiene(Object*) { return false; }
bool W3dFisicaHay(void) { return false; }
void W3dFisicaGetVel(Object*, float* vx, float* vy, float* vz) {
    if (vx) *vx = 0.0f; if (vy) *vy = 0.0f; if (vz) *vz = 0.0f;
}
void W3dFisicaSetVel(Object*, float, float, float) { FisicaAvisoOff(); }
void W3dFisicaSetAdaptador2D(W3dFisicaLienzoFn, W3dFisicaTam2DFn, W3dFisicaCajaUIFn,
                             W3dFisicaCrudaFn) {}
// ---- binds lua stub: misma ARIDAD de retorno que los reales ----------------
// velocidad(obj) -> 0,0,0 ; velocidad(obj, vx, vy) -> nada
static int LFisicaStubVelocidad(lua_State* L) {
    FisicaAvisoOff();
    if (lua_gettop(L) <= 1) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    return 0;
}
// acelerar(obj, factor) -> rapidez resultante (0 sin fisica)
static int LFisicaStubAcelerar(lua_State* L) { FisicaAvisoOff(); lua_pushnumber(L, 0); return 1; }
// caja(obj) -> 0,0,0 ; caja(obj, w, h) -> nada
static int LFisicaStubCaja(lua_State* L) {
    FisicaAvisoOff();
    if (lua_gettop(L) <= 1) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    return 0;
}
// rebotar/rebotarEn/rebotarDentro -> false (nunca hay rebote)
static int LFisicaStubRebote(lua_State* L) { FisicaAvisoOff(); lua_pushboolean(L, 0); return 1; }
void W3dFisicaRegistrarBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LFisicaStubVelocidad); lua_setglobal(L, "velocidad");
    lua_pushcfunction(L, LFisicaStubAcelerar);  lua_setglobal(L, "acelerar");
    lua_pushcfunction(L, LFisicaStubCaja);      lua_setglobal(L, "caja");
    lua_pushcfunction(L, LFisicaStubRebote);    lua_setglobal(L, "rebotar");
    lua_pushcfunction(L, LFisicaStubRebote);    lua_setglobal(L, "rebotarEn");
    lua_pushcfunction(L, LFisicaStubRebote);    lua_setglobal(L, "rebotarDentro");
}
#endif // W3D_SIN_FISICA

// una instancia viva = un lua_State propio + sus referencias resueltas
// los EVENTOS que se cachean por instancia (ver W3dScriptEvento): el indice es el de eventos[]
static const char* const kEventosScript[] = { "alEntrar", "alQuedarse", "alSalir", "alTocar" };
enum { W3D_EVENTOS_SCRIPT = 4 };

struct W3dScriptInst {
    lua_State* L;
    std::map<std::string, Object*> refs;
    std::map<std::string, std::string> opciones;   // desplegables elegidos
    std::map<std::string, std::string> valores;    // valores por instancia (tipo 2), como texto
    Object* duenio;
    // el script DEFINE el evento i? -1 = sin mirar todavia, 0 = no, 1 = si. Se mira una vez
    // (al primer evento) y queda: un hitbox con cien pares por tick no hace cien getglobal.
    signed char eventos[W3D_EVENTOS_SCRIPT];
    W3dScriptInst() : L(NULL), duenio(NULL) {
        for (int i = 0; i < W3D_EVENTOS_SCRIPT; i++) eventos[i] = -1;
    }
};

// ---- estado COMPARTIDO entre scripts (ver W3dScript.h) ---------------------
// Los lua_State estan aislados a proposito; este mapa C++ unico es el canal
// entre scripts (fruta.lua suma, el HUD del principal lee). Vive el PLAY
// entero y muere en W3dScriptDescargarTodo.
struct W3dCompartidoVal {
    int tipo;            // 0 = numero, 1 = bool, 2 = texto
    double num;
    bool b;
    std::string txt;
    W3dCompartidoVal() : tipo(0), num(0.0), b(false) {}
};
static std::map<std::string, W3dCompartidoVal> gCompartido;

static std::map<Object*, std::vector<W3dScriptInst*> > gInstancias;
static std::map<std::string, bool> gTeclas;
// SNAPSHOT del frame ANTERIOR (para el FLANCO de teclaApretada()/botonApretado(): true solo en el frame en
// que la tecla/boton pasa de suelto a apretado). Mismo criterio que el gPunPrev del runtime (w3drun): se
// copia UNA vez por frame al final de W3dScriptActualizar, DESPUES de correr los scripts, para que todas
// las llamadas del frame vean el mismo estado previo.
static std::map<std::string, bool> gTeclasPrev;
// las teclas que BAJARON desde el snapshot (aunque ya se hayan soltado): con el juego lento (el N95) una pulsacion
// corta baja y sube entre dos frames y gTeclas nunca la mostraba; teclaApretada() la cuenta igual
static std::map<std::string, bool> gTeclasPulsadas;
static bool gFinFrameExterno = false;   // alguien llama W3dScriptFinFrame: el snapshot es suyo (ver el .h)
static float gSticks[2][2] = { {0,0}, {0,0} };   // [izq/der][x/y]
// LOS MANDOS POR SEPARADO (dos jugadores en el mismo equipo): ademas de la mezcla de todos (gSticks / gBotonesPad: lo
// que leen boton("a") y stick("izq")), el estado de cada uno. boton("a", 2), stick("izq", 2), gatillo("der", 2) y
// botonApretado("a", 2) leen el mando 2 (1..W3D_MANDOS, en el orden en que se conectaron)
enum { W3D_MANDOS = 4 };
static float gSticksM[W3D_MANDOS][2][2];
static float gGatillosM[W3D_MANDOS][2];
static std::map<std::string, bool> gBotonesM[W3D_MANDOS], gBotonesMPrev[W3D_MANDOS];
static float gGatillos[2] = { 0, 0 };              // [izq/der] 0..1 (los gatillos analogicos del mando: pedales)
static std::map<std::string, bool> gBotonesPad;
static std::map<std::string, bool> gBotonesPadPrev;   // gemelo de gTeclasPrev para el gamepad
// MULTI-TOUCH: hasta 4 dedos. toque() = dedo 0 (compat); dedo(n) (1-based en lua) = dedo n-1.
// Sirve para el 2 jugadores tactil: un dedo por pala (mitad izquierda/derecha de la pantalla).
#define W3D_MAXDEDOS 4
static float gDedoX[W3D_MAXDEDOS] = {0,0,0,0}, gDedoY[W3D_MAXDEDOS] = {0,0,0,0};
static bool  gDedoAct[W3D_MAXDEDOS] = {false,false,false,false};
// AUDIO para el beep() de los juegos (efectos estilo WhiskPaddle). Forward-decl del modulo de audio del Core
// (evita el include del dir de audio). Sin -DW3D_ENABLE_AUDIO estas son stubs no-op -> beep() es mudo.
namespace w3dEngine { class W3dSound; W3dSound* W3dSoundBeep(float,int,float); int W3dSoundPlay(W3dSound*,float,bool); }
static std::map<int, w3dEngine::W3dSound*> gBeeps;   // cache por (freq,ms): no regenerar el tono cada frame
static std::string gUltimoError;
static W3dScriptBindFn gBindExtra = NULL;
// ver W3dScript.h: main/ lo registra para prender g_redraw + g_objetosMovidos; en el runtime queda NULL
static W3dScriptRedibujoFn gRedibujo = NULL;
// ENTROPIA EXTRA opcional para la semilla del random. El Core ya se siembra self-contained (time() +
// w3dGetTicks() + direccion del lua_State + contador); la plataforma puede REFORZARLO con W3dScriptSemilla()
// (ej time(0) del desktop). 0 = nadie la seteo (el Core se sembra igual con sus fuentes propias).
static unsigned gSemillaExtra = 0;
static unsigned gSemContador = 0;   // asegura que CADA lua_State (los 4 del whiskpaddle) arranque distinto

const char* W3dScriptUltimoError() { return gUltimoError.c_str(); }
void W3dScriptSetBindExtra(W3dScriptBindFn fn) { gBindExtra = fn; }
void W3dScriptSetRedibujo(W3dScriptRedibujoFn fn) { gRedibujo = fn; }
// TODO setter de objeto termina llamando a esto (ver W3dScript.h): sin el, en el editor el cambio
// no se ve hasta que el usuario mueva el mouse.
void W3dScriptRedibujar() { if (gRedibujo) gRedibujo(); }
static void Redibujar() { W3dScriptRedibujar(); }
void W3dScriptSemilla(unsigned s) { gSemillaExtra = s; }
// SEMILLA FIJA (el harness): cada lua_State arranca con una semilla que solo depende de su orden de creacion, asi dos
// corridas del mismo script dan el MISMO juego (las capturas se pueden comparar)
static bool gSemillaFija = false;
void W3dScriptSemillaFija(bool on) { gSemillaFija = on; gSemContador = 0; }

void W3dScriptTecla(const char* nombre, bool apretada) {
    if (!nombre) return;
    if (apretada) gTeclasPulsadas[nombre] = true;
    gTeclas[nombre] = apretada;
}
void W3dScriptSoltarTeclas() {
    gTeclas.clear(); gBotonesPad.clear();
    gTeclasPrev.clear(); gBotonesPadPrev.clear();   // el reset tambien limpia el snapshot del flanco
    gTeclasPulsadas.clear();
    gSticks[0][0] = gSticks[0][1] = gSticks[1][0] = gSticks[1][1] = 0.0f;
    gGatillos[0] = gGatillos[1] = 0.0f;
    for (int m = 0; m < W3D_MANDOS; m++) {
        gBotonesM[m].clear(); gBotonesMPrev[m].clear();
        gSticksM[m][0][0] = gSticksM[m][0][1] = gSticksM[m][1][0] = gSticksM[m][1][1] = 0.0f;
        gGatillosM[m][0] = gGatillosM[m][1] = 0.0f;
    }
    for (int i=0;i<W3D_MAXDEDOS;i++){ gDedoX[i]=gDedoY[i]=0.0f; gDedoAct[i]=false; }
}
void W3dScriptStickDe(int mando, int cual, float x, float y) {
    if (mando < 0 || mando >= W3D_MANDOS || cual < 0 || cual > 1) return;
    gSticksM[mando][cual][0] = x; gSticksM[mando][cual][1] = y;
}
void W3dScriptBotonPadDe(int mando, const char* nombre, bool v) {
    if (mando >= 0 && mando < W3D_MANDOS && nombre) gBotonesM[mando][nombre] = v;
}
void W3dScriptGatilloDe(int mando, int cual, float v) {
    if (mando < 0 || mando >= W3D_MANDOS || cual < 0 || cual > 1) return;
    gGatillosM[mando][cual] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
void W3dScriptStick(int cual, float x, float y) {
    if (cual < 0 || cual > 1) return;
    gSticks[cual][0] = x; gSticks[cual][1] = y;
}
void W3dScriptBotonPad(const char* nombre, bool v) { if (nombre) gBotonesPad[nombre] = v; }
void W3dScriptGatillo(int cual, float v) {
    if (cual < 0 || cual > 1) return;
    gGatillos[cual] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
// LECTURA (la MISMA fuente que stick()/boton() de lua): la usa el harness para
// asertar que el d-pad y las flechas del teclado llegan de verdad al juego.
void W3dScriptStickLeer(int cual, float* x, float* y) {
    if (cual < 0 || cual > 1) { if (x) *x = 0.0f; if (y) *y = 0.0f; return; }
    if (x) *x = gSticks[cual][0];
    if (y) *y = gSticks[cual][1];
}
bool W3dScriptBotonPadLeer(const char* nombre) {
    if (!nombre) return false;
    std::map<std::string, bool>::const_iterator it = gBotonesPad.find(nombre);
    return it != gBotonesPad.end() && it->second;
}
void W3dScriptToque(float x, float y, bool activo) { gDedoX[0]=x; gDedoY[0]=y; gDedoAct[0]=activo; } // dedo 0 (compat)
void W3dScriptDedo(int i, float x, float y, bool activo) { if (i>=0 && i<W3D_MAXDEDOS){ gDedoX[i]=x; gDedoY[i]=y; gDedoAct[i]=activo; } }
// RATON: posicion del mouse en el lienzo del juego (px, centro 0,0) SIN necesidad de clic -> mouse-over
// de los menus. 'dentro' = el mouse esta sobre la pantalla del juego. El clic llega por dedo(1)/toque().
static float gRatonX = 0, gRatonY = 0; static bool gRatonDentro = false;
void W3dScriptRaton(float x, float y, bool dentro) { gRatonX = x; gRatonY = y; gRatonDentro = dentro; }
// MOUSE CAPTURADO (juego estilo FPS en PC): capturarRaton(true) desde lua pide que la plataforma esconda el cursor
// y entregue el movimiento RELATIVO (SDL_SetRelativeMouseMode en main.cpp; ESC pausa el juego y lo suelta).
// ratonRel() devuelve el delta acumulado en px desde la ultima lectura y lo deja en cero. En Symbian no hay
// mouse: ratonRel() da 0,0 y el script sigue con el dedo / las flechas.
static float gRatonDX = 0, gRatonDY = 0; static bool gRatonCapturar = false;
void W3dScriptRatonRel(float dx, float dy) { gRatonDX += dx; gRatonDY += dy; }
bool W3dScriptRatonCapturar() { return gRatonCapturar; }
static bool gRatonCapturado = false;   // lo que la plataforma logro de verdad (PC: cursor oculto y clavado al centro)
void W3dScriptRatonCapturadoSet(bool v) { gRatonCapturado = v; }

// ---- funciones BASE que ven todos los scripts ------------------------------
// tecla("w") -> true si esta apretada
static int LTecla(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    std::map<std::string, bool>::iterator it = gTeclas.find(n);
    lua_pushboolean(L, it != gTeclas.end() && it->second);
    return 1;
}
// helper: estado (apretado?) de un nombre en un mapa (false si no esta)
static bool EstadoTecla(std::map<std::string, bool>& m, const char* n) {
    std::map<std::string, bool>::iterator it = m.find(n);
    return it != m.end() && it->second;
}
// teclaApretada("w") -> true SOLO en el frame en que la tecla pasa de suelta a apretada (flanco de subida).
// Gemelo de apretado() (que es tactil): usa gTeclas (estado actual) vs gTeclasPrev (snapshot del frame anterior),
// y tambien cuenta la que bajo y subio entre dos frames (gTeclasPulsadas: un juego lento no pierde el tiro).
static int LTeclaApretada(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    lua_pushboolean(L, (EstadoTecla(gTeclas, n) && !EstadoTecla(gTeclasPrev, n)) || EstadoTecla(gTeclasPulsadas, n));
    return 1;
}
// el mando pedido por un bind (el 2do argumento, 1..W3D_MANDOS): -1 = todos mezclados (sin argumento)
static int MandoArg(lua_State* L, int i) {
    if (lua_isnoneornil(L, i)) return -1;
    const int m = (int)luaL_checkinteger(L, i) - 1;
    return (m >= 0 && m < W3D_MANDOS) ? m : -2;   // (-2: uno que no existe: todo suelto)
}
// botonApretado("a" [, mando]) -> idem para los botones del gamepad (mismo indexado por nombre que boton()).
static int LBotonApretado(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    const int m = MandoArg(L, 2);
    if (m == -1) lua_pushboolean(L, EstadoTecla(gBotonesPad, n) && !EstadoTecla(gBotonesPadPrev, n));
    else lua_pushboolean(L, m >= 0 && EstadoTecla(gBotonesM[m], n) && !EstadoTecla(gBotonesMPrev[m], n));
    return 1;
}
// azar() -> real en [0,1);  azar(min,max) -> ENTERO en [min,max] inclusive.
// Delega en math.random del PROPIO lua_State -> comparte el generador (xoshiro256**) ya sembrado, asi
// azar() y math.random varian juntos entre ejecuciones. C++03-safe (sin C++11).
static void PushMathRandom(lua_State* L) {   // deja solo math.random en el stack
    lua_getglobal(L, "math");
    lua_getfield(L, -1, "random");
    lua_replace(L, -2);
}
static int LAzar(lua_State* L) {
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
// el hook de PREFABS (ver W3dScript.h): lo instala el importador del editor
Object* (*W3dInstanciarPrefabHook)(const char* nombre, const float* posMotor) = 0;
// ...y el de los prefabs del proyecto (io/Prefabs) + quien corre la partida (ver W3dScript.h)
W3dPrefabInstanciarFn W3dPrefabInstanciarHook = 0;
Object* W3dPrefabInstanciarDuenio = 0;
// ...y los de los scripts de una LIBRERIA externa (ver W3dScript.h)
std::string (*W3dScriptLibreriaHook)(Object* duenio) = 0;
std::string (*W3dScriptRutaSueltaHook)(const std::string& libreria, const std::string& ruta) = 0;
std::string W3dScriptLibreria(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    if (!L) return std::string();
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_lib");
    const char* s = lua_isstring(L, -1) ? lua_tostring(L, -1) : 0;
    const std::string lib = s ? s : "";
    lua_pop(L, 1);
    return lib;
}
std::string W3dScriptRutaDeLibreria(void* L, const std::string& ruta) {
    if (ruta.empty() || ruta[0] == '/' || ruta[0] == '\\' || ruta.find(':') != std::string::npos) return std::string();
    const std::string lib = W3dScriptLibreria(L);
    if (lib.empty()) return std::string();
    // una ENTRADA de su libreria (en el juego compilado: su copia empaquetada, donde viajan tambien sus sueltos)
    const std::string r = "lib:" + lib + "/" + ruta;
    if (w3dFileSystem::FileExists(r)) return r;
    // un archivo SUELTO al lado de su .w3d (el editor)
    if (W3dScriptRutaSueltaHook) {
        const std::string d = W3dScriptRutaSueltaHook(lib, ruta);
        if (!d.empty()) return d;
    }
    return std::string();
}
static W3dScriptObjetoFn gObjetoNuevoFn = 0;
static W3dScriptObjetoFn gObjetoDestruirFn = 0;
void W3dScriptSetObjetoNuevo(W3dScriptObjetoFn fn) { gObjetoNuevoFn = fn; }
void W3dScriptSetObjetoDestruir(W3dScriptObjetoFn fn) { gObjetoDestruirFn = fn; }
// ---- el STREAMING por las rutas de instanciar()/destruir() (ver W3dScript.h) ----
bool W3dScriptHayPartida() { return gObjetoNuevoFn != 0; }
bool W3dScriptAvisarObjetoNuevo(Object* raiz) {
    if (!gObjetoNuevoFn || !raiz) return false;
    gObjetoNuevoFn(raiz);
    return true;
}
static W3dScriptObjetoFn gObjetoLiberarFn = 0;
void W3dScriptSetObjetoLiberar(W3dScriptObjetoFn fn) { gObjetoLiberarFn = fn; }
void W3dScriptLiberarYa(Object* o) {
    if (!o) return;
    if (gObjetoLiberarFn) gObjetoLiberarFn(o);
    else W3dScriptDestruirObjeto(o);
}
int (*W3dStreamingLuaHook)(Object* o, int pedido) = 0;
int (*W3dTexturasLuaHook)(int cargar) = 0;

// ---- LAS FUENTES DE LOS SCRIPTS como recurso del almacen (W3DREC_SCRIPT, ver W3dScript.h) ----
static void* OpCargarFuente(const std::string& id, long* bytes) {
    std::vector<unsigned char>* v = new std::vector<unsigned char>();
    if (!w3dFileSystem::ReadFileBytes(id, *v) || v->empty()) { delete v; return 0; }
    if (bytes) *bytes = (long)v->size();
    return v;
}
static bool OpLiberarFuente(void* dato) {
    delete (std::vector<unsigned char>*)dato;
    return true;
}
void W3dScriptFuentesAsegurarOps() {
    static bool listo = false;
    if (listo) return;
    W3dRecursoOps ops;
    ops.Cargar = OpCargarFuente;
    ops.Liberar = OpLiberarFuente;
    W3dRecursosRegistrarTipo(W3DREC_SCRIPT, ops);
    listo = true;
}

// instanciar("nombre" [, x, y, z]) -> clona un Prefab de la Biblioteca del .w3d
// como hijo de la escena y devuelve el objeto (light userdata, como objeto()).
// x/y/z en coords del MOTOR. nil = no existe el prefab (o el recurso todavia
// no esta LISTO): reintentar el proximo frame.
static int LInstanciar(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    float pos[3]; const float* pp = 0;
    if (lua_gettop(L) >= 4) {
        pos[0] = (float)luaL_checknumber(L, 2);
        pos[1] = (float)luaL_checknumber(L, 3);
        pos[2] = (float)luaL_checknumber(L, 4);
        pp = pos;
    }
    // un PREFAB DEL PROYECTO primero (con rotY opcional, el 5to argumento); si no hay, el de la Biblioteca del
    // formato texto (que no gira ni avisa a la partida: queda como estaba)
    Object* o = 0;
    if (W3dPrefabInstanciarHook) {
        const float rotY = (lua_gettop(L) >= 5) ? (float)luaL_checknumber(L, 5) : 0.0f;
        // (quien llama: un script de una libreria externa nombra primero los prefabs de SU libreria)
        lua_getfield(L, LUA_REGISTRYINDEX, "w3d_duenio");
        W3dPrefabInstanciarDuenio = lua_islightuserdata(L, -1) ? (Object*)lua_touserdata(L, -1) : 0;
        lua_pop(L, 1);
        o = W3dPrefabInstanciarHook(n, pp, rotY);
        W3dPrefabInstanciarDuenio = 0;
        if (o && gObjetoNuevoFn) gObjetoNuevoFn(o);   // sus scripts, sus cuerpos: los arranca la partida
    }
    if (!o && W3dInstanciarPrefabHook) o = W3dInstanciarPrefabHook(n, pp);
    if (o) lua_pushlightuserdata(L, o); else lua_pushnil(L);
    return 1;
}

// ---- destruir(obj): DIFERIDO al final del frame (ver W3dScript.h) ----
// lo pedido va por SERIAL: si el objeto se libera antes por otro camino (un cambiarEscena, otro destruir de
// un ancestro) el pedido se descarta solo
struct W3dDestruirPedido { Object* o; unsigned serial; };
static std::vector<W3dDestruirPedido> gDestruir;
static int LDestruir(lua_State* L) {
    Object* o = W3dScriptParamObjeto((void*)L, 1);
    extern Object* SceneCollection;
    if (!o || o == SceneCollection || W3dEsRaizRegistrada(o) || W3dScriptEsEscenaUI(o)) { lua_pushboolean(L, 0); return 1; }
    for (size_t i = 0; i < gDestruir.size(); i++)
        if (gDestruir[i].o == o && gDestruir[i].serial == o->serial) { lua_pushboolean(L, 1); return 1; }
    W3dDestruirPedido p; p.o = o; p.serial = o->serial;
    gDestruir.push_back(p);
    lua_pushboolean(L, 1);
    return 1;
}

// ---- cargar(obj) / descargar(obj) / cargaAuto(obj) / cargado(obj): el STREAMING de una instancia de prefab o un
// proxy (main/io/Streaming.h, por el hook). Los tres primeros devuelven true si 'obj' es una instancia (el pedido
// queda anotado: se aplica en el tick del streaming); cargado() dice si sus hijos estan generados. nil = sin streaming
static int LStreaming(lua_State* L, int pedido) {
    Object* o = W3dScriptParamObjeto((void*)L, 1);
    if (!W3dStreamingLuaHook || !o) { lua_pushnil(L); return 1; }
    const int e = W3dStreamingLuaHook(o, pedido);
    if (pedido == 0) lua_pushboolean(L, e == 2);
    else lua_pushboolean(L, e >= 0);
    return 1;
}
// texturasPendientes() -> cuantas texturas esperan en la cola diferida (sube una por cuadro). cargarTexturas() -> las sube
// TODAS ya (bloquea: en el N95 cada pagina de atlas tarda) y devuelve las que quedan (0). Para el principio de una escena
// pedida con cambiarEscena(): sin esto lo que aparece se ve blanco hasta que llega su textura
static int LTexturasPendientes(lua_State* L) { lua_pushinteger(L, W3dTexturasLuaHook ? W3dTexturasLuaHook(0) : 0); return 1; }
static int LCargarTexturas(lua_State* L)     { lua_pushinteger(L, W3dTexturasLuaHook ? W3dTexturasLuaHook(1) : 0); return 1; }
static int LCargar(lua_State* L)    { return LStreaming(L, 1); }
static int LDescargar(lua_State* L) { return LStreaming(L, -1); }
static int LCargaAuto(lua_State* L) { return LStreaming(L, 2); }
static int LCargado(lua_State* L)   { return LStreaming(L, 0); }

// buscar("nombre") -> el objeto de la escena con ese nombre (busqueda recursiva
// por el arbol), o nil. Es la via para agarrar lo que entro EN RUNTIME
// (importarW3D), que no puede estar cableado en `propiedades` porque no existia
// cuando el editor resolvio las referencias.
static Object* BuscarNombreRec(Object* padre, const char* n) {
    if (!padre) return NULL;
    for (size_t i = 0; i < padre->Childrens.size(); i++) {
        Object* h = padre->Childrens[i];
        if (!h) continue;
        if (h->name == n) return h;
        Object* r = BuscarNombreRec(h, n);
        if (r) return r;
    }
    return NULL;
}
static int LBuscar(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    extern Object* SceneCollection;
    // buscar("nombre" [, raiz]): con raiz busca SOLO en ese subarbol. Es la
    // manera de agarrar la pieza de UN auto cuando hay dos importados con las
    // mismas partes ("door_lf_dummy" del taxi Y del patrullero).
    Object* raiz = SceneCollection;
    if (lua_islightuserdata(L, 2)) {
        Object* r = W3dScriptParamObjeto((void*)L, 2);
        if (r) raiz = r;
    } else {
        // SIN raiz: como lo ve el objeto del script. Uno GENERADO por una instancia de prefab busca primero
        // adentro de SU instancia (su "Arma", no la del primer enemigo) y despues en toda la escena
        lua_getfield(L, LUA_REGISTRYINDEX, "w3d_duenio");
        Object* duenio = lua_islightuserdata(L, -1) ? (Object*)lua_touserdata(L, -1) : NULL;
        lua_pop(L, 1);
        if (duenio && W3dInstanciaDe(duenio)) {
            Object* o = W3dBuscarNombreDesde(duenio, n, SceneCollection);
            if (o) lua_pushlightuserdata(L, o); else lua_pushnil(L);
            return 1;
        }
    }
    if (raiz && raiz->name == n) { lua_pushlightuserdata(L, raiz); return 1; }
    Object* o = BuscarNombreRec(raiz, n);
    if (o) lua_pushlightuserdata(L, o); else lua_pushnil(L);
    return 1;
}

// puntoMundo(obj, lx,ly,lz) -> x,y,z : transforma un punto LOCAL del objeto a
// MUNDO (cadena completa de padres). Sirve para anclar cosas a un objeto que
// la fisica mueve y rota (el asiento de un auto, la punta de un canio).
static int LPuntoMundo(lua_State* L) {
    Object* o = W3dScriptParamObjeto((void*)L, 1);
    if (!o) { lua_pushnil(L); return 1; }
    Vector3 p((float)luaL_checknumber(L, 2),
              (float)luaL_checknumber(L, 3),
              (float)luaL_checknumber(L, 4));
    Vector3 w = o->LocalAMundo(p);
    lua_pushnumber(L, w.x);
    lua_pushnumber(L, w.y);
    lua_pushnumber(L, w.z);
    return 3;
}

// animEscena("nombre" [, loop=true]) -> reproduce una ANIMACION DE ESCENA (las
// curvas de transform de objetos) durante el juego, por nombre. Devuelve la
// duracion en SEGUNDOS (para armar timers en lua), o nil si no existe. loop
// false = one-shot: al terminar queda clavada en la ultima pose. UNA activa por
// vez; el tick lo corre la simulacion (W3dAnimEscenaTick).
// pararEscena(): corta la ANIMACION DE ESCENA que este sonando (animEscena). Los objetos quedan donde estaban y
// su pista "Camara activa" deja de mandar (sino una one-shot terminada seguia imponiendo su ultima camara).
// animEscenaFrame(f): fija el frame de la animacion de escena que suena y apaga su reloj propio (el script manda).
// Una cinematica lo llama cada tick con el MISMO frame que usa para los clips de los personajes (animFrame).
static int LAnimEscenaFrame(lua_State* L) {
    W3dAnimEscenaFijar((float)luaL_checknumber(L, 1));
    return 0;
}
// animEscenaActual() -> frame, nombre de la animacion de escena que esta sonando (nil si ninguna). Lo
// usa una CINEMATICA para saber cuando termino su animacion (una ESCENA pedida con cambiarEscena la
// reproduce una vez desde el inicio) y pedir la siguiente.
static int LAnimEscenaActual(lua_State* L) {
    const int i = W3dAnimEscenaSonando();
    if (i < 0 || i >= (int)SceneAnimations.size() || !SceneAnimations[(size_t)i]) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (lua_Number)W3dAnimEscenaFrame());
    lua_pushstring(L, SceneAnimations[(size_t)i]->name.c_str());
    return 2;
}
static int LPararEscena(lua_State* L) {
    (void)L;
    W3dAnimEscenaReset();
    return 0;
}
static int LAnimEscena(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    const bool loop = lua_isnoneornil(L, 2) ? true : (lua_toboolean(L, 2) != 0);
    int idx = W3dAnimEscenaIdx(n);
    if (idx < 0 || !W3dAnimEscenaPlay(idx, loop)) {
        static bool avisado = false;   // una vez por sesion (no spamear el log)
        if (!avisado) { avisado = true; w3dLogfW("animEscena(): no existe la animacion '%s'", n); }
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, W3dAnimEscenaDur(idx));
    return 1;
}

// ---- CLIPS DE JERARQUIA (animation/W3dAnimSet.h: una raiz anima a si misma y a sus hijos por ruta de nombres) ----
// animObjeto(raiz, "clip" [, loop=true]) -> duracion en SEGUNDOS, o nil si no hay un clip con ese nombre.
// El clip se busca en la biblioteca de la raiz; si no esta, se acepta "Biblioteca/clip" (o el nombre suelto
// si es el unico): cualquier jerarquia con los mismos nombres lo puede reproducir (lo que no tiene se ignora).
// Cada raiz tiene SU cabezal: varias animaciones suenan a la vez e independientes (la puerta, el ascensor,
// el auto), y conviven con animEscena() y con los clips de los armatures (animClip). Una raiz puede estar
// ADENTRO de otra que suena (la puerta del auto con su propio clip): se posa primero el ancestro y la pista
// "." de la de adentro se suma a lo que el le hace, sin importar cual se arranco antes (Animation.cpp).
// loop false = una pasada: al terminar queda en la ultima pose (animObjetoTermino da true).
// una INSTANCIA (de un prefab, de una ESCENA, un proxy) se anima como una UNIDAD: las funciones de clips de jerarquia
// le hablan a su objeto raiz generado (el que trae los clips) y las de esqueleto a su primer armature
static Object* ObjetoAnimable(lua_State* L, int i) {
    Object* o = W3dScriptParamObjeto((void*)L, i);
    if (o && W3dEsTipoInstancia(o->getType()) && !o->Childrens.empty() && o->Childrens[0]) return o->Childrens[0];
    return o;
}
static Object* PrimerArmature(Object* o) {
    if (!o) return NULL;
    if (o->getType() == ObjectType::armature) return o;
    for (size_t i = 0; i < o->Childrens.size(); i++) { Object* a = PrimerArmature(o->Childrens[i]); if (a) return a; }
    return NULL;
}
static int LAnimObjeto(lua_State* L) {
    Object* o = ObjetoAnimable(L, 1);
    const char* n = luaL_checkstring(L, 2);
    const bool loop = lua_isnoneornil(L, 3) ? true : (lua_toboolean(L, 3) != 0);
    const float dur = o ? W3dAnimObjetoPlay(o, n, loop) : -1.0f;
    if (dur < 0.0f) {
        static bool avisado = false;   // una vez por sesion (no spamear el log)
        if (!avisado) { avisado = true; w3dLogfW("animObjeto(): '%s' no tiene un clip '%s'", o ? o->name.c_str() : "?", n); }
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, dur);
    return 1;
}
// animObjetoFrame(obj [, f]) -> el frame del clip que suena en el objeto (nil si ninguno). Con f mueve su
// cabezal ahi (acotado al rango del clip) y el clip sigue sonando desde ese frame.
static int LAnimObjetoFrame(lua_State* L) {
    Object* o = ObjetoAnimable(L, 1);
    if (o && !lua_isnoneornil(L, 2)) W3dAnimObjetoFijarFrame(o, (float)luaL_checknumber(L, 2));
    const float f = o ? W3dAnimObjetoFrame(o) : -1.0f;
    if (f < 0.0f) lua_pushnil(L); else lua_pushnumber(L, f);
    return 1;
}
// animObjetoTermino(obj) -> true si no le suena nada o si su clip de una pasada llego al final
static int LAnimObjetoTermino(lua_State* L) {
    Object* o = ObjetoAnimable(L, 1);
    lua_pushboolean(L, W3dAnimObjetoTermino(o) ? 1 : 0);
    return 1;
}
// animObjetoActual(obj) -> el nombre del clip que suena (nil si ninguno)
static int LAnimObjetoActual(lua_State* L) {
    Object* o = ObjetoAnimable(L, 1);
    const std::string c = o ? W3dAnimObjetoClip(o) : std::string();
    if (c.empty()) lua_pushnil(L); else lua_pushstring(L, c.c_str());
    return 1;
}
// animObjetoParar(obj): corta su clip (el objeto queda donde esta)
static int LAnimObjetoParar(lua_State* L) {
    Object* o = ObjetoAnimable(L, 1);
    if (o) W3dAnimObjetoParar(o);
    return 0;
}
// animObjetoVelocidad(raiz, v) -> true si le suena un clip. 1 = sus fps, 2 = el doble, negativa = hacia atras
static int LAnimObjetoVelocidad(lua_State* L) {
    Object* o = ObjetoAnimable(L, 1);
    const float v = (float)luaL_checknumber(L, 2);
    lua_pushboolean(L, (o && W3dAnimObjetoVelocidad(o, v)) ? 1 : 0);
    return 1;
}
// animRetarget(obj, "completo" | "rotaciones") : REUTILIZAR ANIMACIONES ENTRE TAMANOS DISTINTOS. Con
// "rotaciones" el armature (o la jerarquia) usa solo las rotaciones de sus clips: cada hueso/nodo conserva su
// traslacion y su escala de reposo (sus proporciones) y el movimiento de la raiz se escala por la razon de
// tamanos (el suyo / el del esqueleto que grabo el clip). "completo" (el de siempre) = el clip tal cual.
// Tambien acepta 0 / 1. En una jerarquia, "clip" vuelve al modo que diga cada clip.
static int LAnimRetarget(lua_State* L) {
    Object* o = W3dScriptParamObjeto((void*)L, 1);
    if (!o) return 0;
    int modo = W3D_RETARGET_COMPLETO;
    if (lua_type(L, 2) == LUA_TNUMBER) modo = (int)lua_tonumber(L, 2);
    else {
        const char* m = luaL_optstring(L, 2, "completo");
        if (!strcmp(m, "rotaciones")) modo = W3D_RETARGET_ROTACIONES;
        else if (!strcmp(m, "clip")) modo = -1;
    }
    if (o->getType() == ObjectType::armature) W3dArmatureRetarget((Armature*)o, modo < 0 ? W3D_RETARGET_COMPLETO : modo);
    else W3dAnimObjetoRetarget(o, modo);
    return 0;
}

// ---- PANTALLA DIVIDIDA (dos jugadores en el mismo equipo): el juego pide 2 vistas y la camara de cada una; el render
// (el viewport del editor, el runtime compilado) dibuja la escena una vez por vista en su mitad del cuadro y el HUD
// una vez encima. Antes de cada vista llama a antesDeVista(i) en el script que las pidio: ahi el juego da vuelta hacia
// ESA camara lo que mira a la camara (llamas, chispas, lineas) y esconde lo que esa vista no tiene que ver
static int gVistas = 1, gVistasModo = 0;          // 1 o 2; 0 = lado a lado, 1 = una arriba de la otra
static Object* gVistaCam[2] = { NULL, NULL };
static Object* gVistasDuenio = NULL;
static int gVistaDibujando = 0;
int W3dVistasCuantas() { return gVistas; }
int W3dVistasModo() { return gVistasModo; }
Object* W3dVistaCamara(int i) { return (i >= 0 && i < 2) ? gVistaCam[i] : NULL; }
int W3dVistaDibujando() { return gVistaDibujando; }
void W3dVistaAntes(int i) {
    gVistaDibujando = i + 1;
    if (!gVistasDuenio) return;
    const float n = (float)(i + 1);
    W3dScriptEvento(gVistasDuenio, "antesDeVista", (Object* const*)NULL, 0, &n, 1);
}
void W3dVistaDespues() { gVistaDibujando = 0; }
static void VistasReiniciar() {
    gVistas = 1; gVistasModo = 0; gVistaCam[0] = gVistaCam[1] = NULL; gVistasDuenio = NULL; gVistaDibujando = 0;
}
// setVistas(n [, "lado" | "arriba"]): 1 = la pantalla entera (lo normal), 2 = dividida (lado a lado o una arriba de la
// otra). El script que la llama recibe antesDeVista(i) antes de que se dibuje cada una
static int LSetVistas(lua_State* L) {
    const int n = (int)luaL_checkinteger(L, 1);
    const char* modo = luaL_optstring(L, 2, "lado");
    gVistas = n >= 2 ? 2 : 1;
    gVistasModo = (modo && modo[0] == 'a') ? 1 : 0;
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_duenio");
    gVistasDuenio = (Object*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    W3dScriptRedibujar();
    return 0;
}
// setVistaCamara(i, camara): la camara de la vista i (1 o 2). Sin poner, la activa
static int LSetVistaCamara(lua_State* L) {
    const int i = (int)luaL_checkinteger(L, 1) - 1;
    if (i < 0 || i > 1) return 0;
    gVistaCam[i] = W3dScriptParamObjeto(L, 2);
    W3dScriptRedibujar();
    return 0;
}
// vista() -> la vista que se esta dibujando (1 o 2; 0 fuera del dibujo) y cuantas hay
static int LVista(lua_State* L) {
    lua_pushinteger(L, gVistaDibujando);
    lua_pushinteger(L, gVistas);
    return 2;
}

// yo() -> el objeto que tiene ESTE script (el mismo script puesto en varios objetos: cada instancia se maneja a si
// misma sin buscarse por nombre)
static int LYo(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_duenio");
    if (!lua_islightuserdata(L, -1)) { lua_pop(L, 1); lua_pushnil(L); }
    return 1;
}

// objeto("prop") -> la referencia expuesta (light userdata; nil si no se asigno)
static int LObjeto(lua_State* L) {
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

// opcion("dificultad") -> la opcion elegida en el editor ("" si no se asigno)
static int LOpcion(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_inst");
    W3dScriptInst* inst = (W3dScriptInst*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (inst) {
        std::map<std::string, std::string>::iterator it = inst->opciones.find(n);
        if (it != inst->opciones.end()) {
            lua_pushstring(L, it->second.c_str());
            return 1;
        }
    }
    lua_pushstring(L, "");
    return 1;
}

// propiedad("frame") -> el VALOR configurado en el editor para ESTA instancia,
// convertido al tipo del default declarado en la tabla `propiedades` (numero ->
// number, bool -> boolean, texto -> string). Si la instancia no configuro nada,
// devuelve el default del propio script (nil si tampoco esta declarado).
static int LPropiedad(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "w3d_inst");
    W3dScriptInst* inst = (W3dScriptInst*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    // el default declarado (y su TIPO) sale de la tabla global `propiedades`
    // del propio lua_State: el script ya corrio, la tabla esta viva
    int tdecl = LUA_TNIL;
    lua_getglobal(L, "propiedades");
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
        std::map<std::string, std::string>::iterator it = inst->valores.find(n);
        if (it != inst->valores.end()) cfg = &it->second;
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

// compartido("suma") -> el valor del mapa compartido entre scripts (nil si no
// existe). Es la lectura del canal unico entre lua_States (ver W3dScript.h).
static int LCompartido(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    std::map<std::string, W3dCompartidoVal>::iterator it = gCompartido.find(n);
    if (it == gCompartido.end()) { lua_pushnil(L); return 1; }
    if (it->second.tipo == 0)      lua_pushnumber(L, (lua_Number)it->second.num);
    else if (it->second.tipo == 1) lua_pushboolean(L, it->second.b ? 1 : 0);
    else                           lua_pushstring(L, it->second.txt.c_str());
    return 1;
}
// setCompartido("suma", v): escribe en el mapa compartido (numero/bool/string;
// nil borra la clave). Cualquier script del juego lo ve con compartido().
static int LSetCompartido(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    int t = lua_type(L, 2);
    if (t == LUA_TNIL || t == LUA_TNONE) { gCompartido.erase(n); return 0; }
    W3dCompartidoVal v;
    if (t == LUA_TNUMBER)       { v.tipo = 0; v.num = (double)lua_tonumber(L, 2); }
    else if (t == LUA_TBOOLEAN) { v.tipo = 1; v.b = lua_toboolean(L, 2) != 0; }
    else                        { v.tipo = 2; v.txt = lua_tostring(L, 2) ? lua_tostring(L, 2) : ""; }
    gCompartido[n] = v;
    return 0;
}

int W3dScriptCompartidoCantidad() { return (int)gCompartido.size(); }
bool W3dScriptCompartidoPar(int i, std::string* nombre, std::string* valor) {
    if (i < 0 || i >= (int)gCompartido.size()) return false;
    std::map<std::string, W3dCompartidoVal>::iterator it = gCompartido.begin();
    for (int k = 0; k < i; k++) ++it;
    if (nombre) *nombre = it->first;
    if (valor) {
        const W3dCompartidoVal& v = it->second;
        if (v.tipo == 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%g", v.num);
            *valor = buf;
        } else if (v.tipo == 1) *valor = v.b ? "true" : "false";
        else *valor = v.txt;
    }
    return true;
}

// stick("izq"|"der" [, mando]) -> x, y del stick analogico (-1..1, deadzone ya aplicada); sin mando, todos juntos
static int LStick(lua_State* L) {
    const char* n = luaL_optstring(L, 1, "izq");
    int cual = (n && n[0] == 'd') ? 1 : 0;
    const int m = MandoArg(L, 2);
    lua_pushnumber(L, m == -1 ? gSticks[cual][0] : (m >= 0 ? gSticksM[m][cual][0] : 0.0f));
    lua_pushnumber(L, m == -1 ? gSticks[cual][1] : (m >= 0 ? gSticksM[m][cual][1] : 0.0f));
    return 2;
}
// gatillo("izq"|"der" [, mando]) -> 0..1: el gatillo analogico del mando (LT / RT, L2 / R2): un pedal (acelerador, freno)
static int LGatillo(lua_State* L) {
    const char* n = luaL_optstring(L, 1, "der");
    const int cual = (n && n[0] == 'i') ? 0 : 1;
    const int m = MandoArg(L, 2);
    lua_pushnumber(L, m == -1 ? gGatillos[cual] : (m >= 0 ? gGatillosM[m][cual] : 0.0f));
    return 1;
}
// toque() -> x, y, tocando: el dedo/mouse sobre el juego (px del lienzo, centro 0,0)
static int LToque(lua_State* L) {
    lua_pushnumber(L, gDedoX[0]);
    lua_pushnumber(L, gDedoY[0]);
    lua_pushboolean(L, gDedoAct[0]);
    return 3;
}
// raton(): posicion del mouse en el lienzo del juego (px, centro 0,0) + si esta DENTRO de la pantalla.
// Sirve para el mouse-over de los menus (el clic sigue viniendo por dedo(1)/toque()).
static int LRaton(lua_State* L) {
    lua_pushnumber(L, gRatonX); lua_pushnumber(L, gRatonY); lua_pushboolean(L, gRatonDentro);
    return 3;
}
// dedo(n): n es 1-based (dedo(1) = primer dedo). Devuelve x, y (px, centro 0,0) y si esta activo.
static int LDedo(lua_State* L) {
    int i = (int)luaL_optinteger(L, 1, 1) - 1;
    if (i < 0) i = 0; if (i >= W3D_MAXDEDOS) i = W3D_MAXDEDOS - 1;
    lua_pushnumber(L, gDedoX[i]);
    lua_pushnumber(L, gDedoY[i]);
    lua_pushboolean(L, gDedoAct[i]);
    return 3;
}
// ratonRel() -> dx, dy: movimiento del mouse capturado (px) desde la ultima llamada; lo consume (queda en 0,0)
static int LRatonRel(lua_State* L) {
    lua_pushnumber(L, gRatonDX); lua_pushnumber(L, gRatonDY);
    gRatonDX = 0; gRatonDY = 0;
    return 2;
}
// capturarRaton(true/false): pide (o suelta) la captura del mouse para mirar estilo FPS. En PC y en el navegador
// (pointer lock, ver main.cpp); en Symbian y Android no hace nada.
static int LCapturarRaton(lua_State* L) { gRatonCapturar = lua_toboolean(L, 1) != 0; return 0; }
// ratonCapturado() -> true si la plataforma tiene el mouse capturado AHORA (para que el script no mire tambien
// con el arrastre del dedo mientras el cursor esta clavado al centro). En Symbian siempre false.
static int LRatonCapturado(lua_State* L) { lua_pushboolean(L, gRatonCapturado ? 1 : 0); return 1; }
// boton("a") -> true si el boton del gamepad esta apretado
// beep(freq, ms, vol): reproduce un TONO de onda cuadrada (efecto estilo WhiskPaddle, copyright-free).
// freq en Hz, ms de duracion (default 90), vol 0..1 (default 1). Cachea el tono por (freq,ms).
static int LBeep(lua_State* L) {
    if (w3dEngine::ConfigMudo()) return 0;   // mute global del Core: no suena (el juego no envuelve beep a mano)
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
// boton("a" [, mando]): sin mando, el de cualquiera
static int LBoton(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    const int m = MandoArg(L, 2);
    if (m == -1) lua_pushboolean(L, EstadoTecla(gBotonesPad, n));
    else lua_pushboolean(L, m >= 0 && EstadoTecla(gBotonesM[m], n));
    return 1;
}

// ---- CONFIG persistente + mute (facilidad del Core para los juegos lua) -----
// config(clave, def) -> devuelve el string guardado, o 'def' si la clave no existe.
static int LConfig(lua_State* L) {
    const char* clave = luaL_checkstring(L, 1);
    const char* def   = luaL_optstring(L, 2, "");   // aunque pasen un number, optstring lo da como texto
    lua_pushstring(L, w3dEngine::ConfigGetStr(clave, def));
    return 1;
}
// setConfig(clave, valor): guarda EN MEMORIA (acepta string o number). No persiste solo:
// llama guardarConfig() cuando quieras escribir a disco/localStorage.
static int LSetConfig(lua_State* L) {
    const char* clave = luaL_checkstring(L, 1);
    const char* valor = luaL_optstring(L, 2, "");   // optstring convierte un number a su texto
    w3dEngine::ConfigSetStr(clave, valor);
    return 0;
}
// guardarConfig() -> escribe la config al almacenamiento de la plataforma. Devuelve true si pudo.
static int LGuardarConfig(lua_State* L) { lua_pushboolean(L, w3dEngine::ConfigSave()); return 1; }
// cargarConfig() -> relee la config del almacenamiento. Devuelve true si habia algo guardado.
static int LCargarConfig(lua_State* L) { lua_pushboolean(L, w3dEngine::ConfigLoad()); return 1; }
// silenciar(bool): prende/apaga el mute global (lo respeta beep()). Deja "mute" en la config
// (falta guardarConfig() para que persista).
static int LSilenciar(lua_State* L) {
    bool m = lua_toboolean(L, 1) != 0;
    w3dEngine::ConfigSetMudo(m);
    w3dEngine::ConfigSetInt("mute", m ? 1 : 0);
    return 0;
}
// estaMudo() -> true si el sonido esta silenciado.
static int LEstaMudo(lua_State* L) { lua_pushboolean(L, w3dEngine::ConfigMudo()); return 1; }

// ---- LOG para los juegos lua (info/aviso/error/depurar + la mini-consola) ---
// MODO DEBUG: la macro que manda es W3D_DEV_LOG (base/w3dlog.h). La regla:
//   * W3D_DEV_LOG=1 (el default del header) = build DEBUG: el log y el ring
//     buffer estan prendidos, depurar() emite y esDebug() devuelve true.
//   * -DW3D_DEV_LOG=0 = build PRODUCCION: w3dLog* son stubs inline no-op, el
//     ring queda apagado (logCantidad() = 0, logLinea() = ""), depurar() es
//     no-op y esDebug() devuelve false. info()/aviso()/error() tambien quedan
//     mudos porque el sink entero desaparece (build mas liviano).
// El checkbox "Modo debug" de la tarjeta Juego del editor define esta macro en
// los 4 targets generados por CompilarJuego (tildado = 1, destildado = 0). El
// EDITOR compila con el default (1): en Play siempre se ve todo.
static const char* LogTexto(lua_State* L) {
    const char* s = lua_tostring(L, 1);   // acepta string y number (lua convierte el number solo)
    return s ? s : "";
}
// info(msg) / aviso(msg): al log del Core con nivel INFO / WARN (los mismos
// niveles que colorea el viewport Console del editor).
static int LLogInfo(lua_State* L)  { w3dLog(LogTexto(L));  return 0; }
static int LLogAviso(lua_State* L) { w3dLogW(LogTexto(L)); return 0; }
// error(msg): nivel ERROR. OJO: PISA el error() de la stdlib de lua (el que
// cortaba el script con una excepcion): en un juego Whisk3D error() LOGUEA y
// el script sigue. Ningun ejemplo usaba el error() nativo.
static int LLogError(lua_State* L) { w3dLogE(LogTexto(L)); return 0; }
// depurar(msg): mensaje de DEBUG. SOLO emite en un build debug (W3D_DEV_LOG=1),
// como INFO con el prefijo "debug: "; en produccion es un no-op total.
static int LLogDepurar(lua_State* L) {
#if W3D_DEV_LOG
    w3dLogf("debug: %s", LogTexto(L));
#else
    (void)L;
#endif
    return 0;
}
// esDebug() -> true si este build tiene el modo debug activo. El patron
// didactico: mostrar(btnDepurar, esDebug()) -> mismo codigo, el build decide.
static int LEsDebug(lua_State* L) {
#if W3D_DEV_LOG
    lua_pushboolean(L, 1);
#else
    lua_pushboolean(L, 0);
#endif
    return 1;
}
// logCantidad() -> cuantas lineas guarda el ring buffer del log (0 en produccion).
static int LLogCantidad(lua_State* L) { lua_pushinteger(L, w3dLogRingCount()); return 1; }
// logLinea(i) -> texto, nivel de la linea i del ring (1-based: 1 = la mas VIEJA).
// El nivel es "info" | "aviso" | "error" ("" si i esta fuera de rango o el ring
// esta apagado). Con esto un juego dibuja su propia mini-consola (ver ui/consola).
static int LLogLinea(lua_State* L) {
    int i = (int)luaL_checkinteger(L, 1);
    const char* s = w3dLogRingLinea(i - 1);   // el ring es 0-based; fuera de rango da ""
    // el ring guarda "[TAG] mensaje": separar el tag del texto para lua
    const char* niv = "";
    if      (strncmp(s, "[INFO] ", 7) == 0)  { niv = "info";  s += 7; }
    else if (strncmp(s, "[WARN] ", 7) == 0)  { niv = "aviso"; s += 7; }
    else if (strncmp(s, "[ERROR] ", 8) == 0) { niv = "error"; s += 8; }
    lua_pushstring(L, s);
    lua_pushstring(L, niv);
    return 2;
}
// pos3(o) -> x, y, z: la posicion LOCAL del objeto 3D (relativa a su padre; si no tiene padre es la
// de mundo). ALIAS historico de posicion(): los scripts viejos lo siguen usando.
static int LPos3(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (!o) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    lua_pushnumber(L, o->pos.x); lua_pushnumber(L, o->pos.y); lua_pushnumber(L, o->pos.z);
    return 3;
}
// setPos3(o, x, y, z): ALIAS historico de setPosicion() (ver mas abajo).
static int LSetPos3(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (o) {
        o->pos.x = (float)luaL_checknumber(L, 2);
        o->pos.y = (float)luaL_checknumber(L, 3);
        o->pos.z = (float)luaL_checknumber(L, 4);
        Redibujar();
    }
    return 0;
}
// girarHacia(o, dx, dz, factor): gira SUAVE al objeto hacia esa direccion del piso
// (el slerp del quaternion vive aca: en lua seria un dolor)
static int LGirarHacia(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (!o) return 0;
    float dx = (float)luaL_checknumber(L, 2);
    float dz = (float)luaL_checknumber(L, 3);
    float f  = (float)luaL_optnumber(L, 4, 0.2);
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 0.0001f) return 0;
    Vector3 dir(dx / len, 0.0f, dz / len);
    Quaternion q = Quaternion::FromDirection(dir, Vector3(0, 1, 0));
    o->SetRot(Quaternion::Slerp(o->Rot(), q, f));   // la puerta deja euler/axis-angle al dia
    Redibujar();
    return 0;
}
// animar(mesh, n [, frameInicial]): elige la vertex-animation que sigue (0 idle,
// 1 correr, ... segun como se armaron las animaciones del modelo).
//   CON DOS ARGUMENTOS el cambio queda PENDIENTE: el clip nuevo entra cuando el
//   actual termina. Es lo que se quiere para encadenar (saludar -> idle).
//   CON TRES el cambio es EN EL ACTO y arranca en ese cuadro. Es el EMPALME POR
//   PIE de una locomocion: el juego que lo motivo pasa de caminar a correr saltando
//   a cuadros concretos, para que el pie de apoyo no se teletransporte. Sin el
//   tercer argumento eso no se podia expresar (habia que esperar al final del
//   ciclo y el personaje "patinaba" en el cambio).
static int LAnimar(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    int n = (int)luaL_checkinteger(L, 2);
    const bool hayInicio = !lua_isnoneornil(L, 3);
    const float f0 = (float)luaL_optnumber(L, 3, 0.0);
    if (o && o->getType() == ObjectType::mesh) {
        VertexAnimationActive* va = FindTargetAnim((Mesh*)o);
        if (va) {
            if (hayInicio) va->CambiarYa(n, f0);   // corta ya y cae en ese cuadro
            else           va->nextAnim = n;       // al terminar el clip actual
        }
        else {
            static bool avisado = false;   // una sola vez por sesion (no spamear)
            if (!avisado) { avisado = true; w3dLogfW("animar(): '%s' no tiene vertex-animation activa", o->name.c_str()); }
        }
    }
    return 0;
}

// ===========================================================================
//  VERTICES POR SCRIPT (malla) — el caso que lo pidio fue una lamina de agua
//  animada por frame: mover ALGUNOS
//  vertices por frame (circulitos), tenirlos, y no tocar el resto.
//
//  EL GRUPO nace del sidecar <modelo>.grupos.json (main/importers/import_wobj):
//  un VertexGroup con indices en dominio CONTROL-POINT (la linea 'v' del OBJ).
//  grupoVertices() lo traduce UNA VEZ a indices de RENDER via vertCtrlPoint
//  (render -> control-point; el mismo mapa del skinning/weight paint) y el juego
//  trabaja siempre con esos indices. Llamarla en inicio() y guardar la tabla.
//
//  INDICES 1-BASED en lua (como dedo()): grupoVertices ya los devuelve asi y
//  verticePos/setVerticePos/setVerticeColor/setVertices los reciben asi.
//
//  EFICIENCIA: los setters escriben vertex[]/vertexColor[] EN EL LUGAR (sin
//  allocs) y suben skinGeomVersion, el MISMO camino por el que la AnimacionUV
//  hace que el VBO se re-suba solo (ver Mesh::TickUVAnimTira / SubirVBO). Para
//  muchos vertices por frame esta setVertices() (tabla plana {i,x,y,z,...}):
//  UN solo cruce lua<->C por frame.
//
//  OJO editor: esto mueve la malla EN VIVO (pensado para el Play / runtime);
//  no re-ancla marcas sharp/seam ni pasa por undo (como el playback de una
//  vertex anim). Guardar el proyecto con la malla "posada" por el script
//  hornea esas posiciones.
// ===========================================================================
static Mesh* ComoMalla(lua_State* L, int idx) {
    Object* o = W3dScriptParamObjeto(L, idx);
    if (!o || o->getType() != ObjectType::mesh) return NULL;
    return (Mesh*)o;
}
// grupoVertices(m, "agua") -> tabla (array 1..n) con los indices de RENDER del
// grupo de vertices 'agua' de la malla. Tabla VACIA si no hay malla/grupo.
// Es la llamada "cara" (arma el mapeo, aloca la tabla): UNA vez en inicio().
static int LGrupoVertices(lua_State* L) {
    Mesh* m = ComoMalla(L, 1);
    const char* nombre = luaL_checkstring(L, 2);
    lua_newtable(L);
    if (!m) return 1;
    VertexGroup* vg = NULL;
    for (size_t g = 0; g < m->vertexGroups.size(); g++)
        if (m->vertexGroups[g] && m->vertexGroups[g]->nombre == nombre) { vg = m->vertexGroups[g]; break; }
    if (!vg) return 1;
    int n = 0;
    if (!m->vertCtrlPoint.empty()) {
        // dominio CONTROL-POINT -> render: todos los render-verts que salieron de
        // esa linea 'v' (con noMerge es 1:1; con merge puede haber varios por CP)
        int maxCP = -1;
        for (size_t i = 0; i < vg->verts.size(); i++)
            if (vg->verts[i] > maxCP) maxCP = vg->verts[i];
        if (maxCP < 0) return 1;
        std::vector<char> enGrupo((size_t)maxCP + 1, 0);
        for (size_t i = 0; i < vg->verts.size(); i++)
            if (vg->verts[i] >= 0) enGrupo[(size_t)vg->verts[i]] = 1;
        size_t tope = m->vertCtrlPoint.size();
        if ((size_t)m->vertexSize < tope) tope = (size_t)m->vertexSize;
        for (size_t i = 0; i < tope; i++) {
            int cp = m->vertCtrlPoint[i];
            if (cp >= 0 && cp <= maxCP && enGrupo[(size_t)cp]) {
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
// verticePos(m, i) -> x, y, z LOCALES del vertice de render i (los indices que
// da grupoVertices, 1-based). Ceros si la malla/indice no valen.
static int LVerticePos(lua_State* L) {
    Mesh* m = ComoMalla(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || !m->vertex || i < 0 || i >= m->vertexSize) {
        lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 3;
    }
    lua_pushnumber(L, m->vertex[i*3]);
    lua_pushnumber(L, m->vertex[i*3+1]);
    lua_pushnumber(L, m->vertex[i*3+2]);
    return 3;
}
// verticePosAnim(m, i) -> x, y, z: la posicion ANIMADA del render-vert i (1-based) en coords locales de la malla:
// con skinning (+ shape keys) es la posicion posada del ultimo skinning calculado (skinVertex); sin skinning es
// la de verticePos. Para seguir un punto de una malla que se deforma (ej: proyectar el iris sobre el ojo).
static int LVerticePosAnim(lua_State* L) {
    Mesh* m = ComoMalla(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || !m->vertex || i < 0 || i >= m->vertexSize) {
        lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 3;
    }
    const GLfloat* src = (m->skinArmature && m->skinVertex && m->skinShapeSerial != 0xFFFFFFFFu) ? m->skinVertex : m->vertex;
    lua_pushnumber(L, src[i*3]);
    lua_pushnumber(L, src[i*3+1]);
    lua_pushnumber(L, src[i*3+2]);
    return 3;
}
// numVertices(m) -> cantidad de render-verts de la malla (el rango valido de verticePos/verticePosAnim/setVerticeUV)
static int LNumVertices(lua_State* L) { Mesh* m = ComoMalla(L, 1); lua_pushinteger(L, m ? m->vertexSize : 0); return 1; }
// setVerticeUV(m, i, u, v [, capa]): la UV del render-vert i (1-based). capa 0 (default) = la UV base; capa >= 1 =
// una capa UV extra del render (Mesh::uvExtra, la que usa una capa de textura con uvMapa = capa). Las capas se
// dibujan de client arrays, asi que el cambio se ve en el proximo frame sin rehacer nada.
static int LSetVerticeUV(lua_State* L) {
    Mesh* m = ComoMalla(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || i < 0 || i >= m->vertexSize) return 0;
    const GLfloat u = (GLfloat)luaL_checknumber(L, 3), v = (GLfloat)luaL_checknumber(L, 4);
    const int capa = (int)luaL_optinteger(L, 5, 0);
    if (capa <= 0) {
        if (!m->uv) return 0;
        m->DesinstanciarDatos(W3DMD_UV);   // COW: geometria compartida (MallaDatos.h)
        m->uv[(size_t)i * 2] = u; m->uv[(size_t)i * 2 + 1] = v;
        m->skinGeomVersion++;   // el VBO se re-sube solo (camino de la AnimacionUV)
    } else {
        if ((int)m->uvExtra.size() <= capa) m->uvExtra.resize((size_t)capa + 1);
        std::vector<GLfloat>& cap = m->uvExtra[(size_t)capa];
        if ((int)cap.size() < m->vertexSize * 2) cap.resize((size_t)m->vertexSize * 2, 0.0f);
        cap[(size_t)i * 2] = u; cap[(size_t)i * 2 + 1] = v;
    }
    Redibujar();
    return 0;
}
// desplazarUV(m, du, dv): TODA la UV base de la malla desplazada (du, dv), en una llamada y una sola
// re-subida del VBO. La base se captura en la primera llamada (las UV del archivo, en el orden de
// render-verts del motor, que no tiene por que coincidir con el del exportador) y se vuelve a capturar
// si otro sistema reescribio uv[] desde la ultima aplicacion. Es el reflejo que desliza la textura
// (parabrisas de los autos de Daytona USA). Devuelve la cantidad de vertices tocados.
static int LDesplazarUV(lua_State* L) {
    Mesh* m = ComoMalla(L, 1);
    const GLfloat du = (GLfloat)luaL_checknumber(L, 2), dv = (GLfloat)luaL_checknumber(L, 3);
    if (!m || !m->uv || m->vertexSize < 1) { lua_pushinteger(L, 0); return 1; }
    const size_t n = (size_t)m->vertexSize * 2;
    bool intacta = (m->uvDesplBase.size() == n);
    for (size_t i = 0; i < n && intacta; i += 2)
        if (m->uv[i] != m->uvDesplBase[i] + m->uvDesplU || m->uv[i + 1] != m->uvDesplBase[i + 1] + m->uvDesplV) intacta = false;
    if (!intacta) { m->uvDesplBase.assign(m->uv, m->uv + n); m->uvDesplU = m->uvDesplV = 0.0f; }
    if (du == m->uvDesplU && dv == m->uvDesplV) { lua_pushinteger(L, m->vertexSize); return 1; }
    m->DesinstanciarDatos(W3DMD_UV);   // COW: geometria compartida (MallaDatos.h)
    for (size_t i = 0; i < n; i += 2) {
        m->uv[i]     = m->uvDesplBase[i]     + du;
        m->uv[i + 1] = m->uvDesplBase[i + 1] + dv;
    }
    m->uvDesplU = du; m->uvDesplV = dv;
    m->skinGeomVersion++;   // UNA sola invalidacion: el VBO de UV se re-sube en el proximo dibujo
    Redibujar();
    lua_pushinteger(L, m->vertexSize);
    return 1;
}
// copiarUV(destino, origen): la UV base de otra malla con la MISMA cantidad de render-verts (por ejemplo las
// fases de una rueda que solo cambian sus UV: el arcade alterna 8 modelos de ruedas para que giren). Copia en el
// orden de vertices del motor, una re-subida del VBO. Devuelve true si copio.
static int LCopiarUV(lua_State* L) {
    Mesh* d = ComoMalla(L, 1);
    Mesh* o = ComoMalla(L, 2);
    if (!d || !o || d == o || !d->uv || !o->uv || d->vertexSize != o->vertexSize || d->vertexSize < 1) { lua_pushboolean(L, 0); return 1; }
    const size_t n = (size_t)d->vertexSize * 2;
    if (memcmp(d->uv, o->uv, n * sizeof(GLfloat)) != 0) {
        d->DesinstanciarDatos(W3DMD_UV);   // COW: geometria compartida (MallaDatos.h)
        memcpy(d->uv, o->uv, n * sizeof(GLfloat));
        d->skinGeomVersion++;              // el VBO de UV se re-sube en el proximo dibujo
        Redibujar();
    }
    lua_pushboolean(L, 1);
    return 1;
}
// setVerticePos(m, i, x, y, z): mueve UN vertice (coords LOCALES). Para mover
// muchos por frame usa setVertices() (batch, un solo cruce lua<->C).
static int LSetVerticePos(lua_State* L) {
    Mesh* m = ComoMalla(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || !m->vertex || i < 0 || i >= m->vertexSize) return 0;
    m->DesinstanciarDatos(W3DMD_POS);   // COW: geometria compartida (MallaDatos.h)
    GLfloat* v = m->vertex + (size_t)i * 3;
    v[0] = (GLfloat)luaL_checknumber(L, 3);
    v[1] = (GLfloat)luaL_checknumber(L, 4);
    v[2] = (GLfloat)luaL_checknumber(L, 5);
    m->skinGeomVersion++;   // el VBO se re-sube solo (camino de la AnimacionUV)
    Redibujar();
    return 0;
}
// helper: 0..1 -> canal GLubyte saturado
static GLubyte CanalColor(lua_Number v) {
    lua_Number n = v * 255.0;
    if (n < 0.0) n = 0.0;
    if (n > 255.0) n = 255.0;
    return (GLubyte)n;
}
// setVerticeColor(m, i, r, g, b [,a]): color por vertice (0..1; alfa opcional,
// default 1). Solo tiene efecto visible si el material dibuja vertex color.
static int LSetVerticeColor(lua_State* L) {
    Mesh* m = ComoMalla(L, 1);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    if (!m || !m->vertexColor || i < 0 || i >= m->vertexSize) return 0;
    m->DesinstanciarDatos(W3DMD_COL);   // COW: geometria compartida (MallaDatos.h)
    GLubyte* c = m->vertexColor + (size_t)i * 4;
    c[0] = CanalColor(luaL_checknumber(L, 3));
    c[1] = CanalColor(luaL_checknumber(L, 4));
    c[2] = CanalColor(luaL_checknumber(L, 5));
    c[3] = CanalColor(luaL_optnumber(L, 6, 1.0));
    m->skinGeomVersion++;   // re-subir tambien los atributos estaticos (color)
    Redibujar();
    return 0;
}
// setVerticesColor(m, t[, n]): BATCH de setVerticeColor — t es una tabla PLANA
// {i1,r1,g1,b1,a1, i2,r2,g2,b2,a2, ...} (0..1). UN cruce lua<->C y UNA
// invalidacion por tanda, en vez de una por vertice (cristal, halos del portal).
// El 3er argumento opcional acota cuantas entradas se leen (multiplo de 5).
static int LSetVerticesColor(lua_State* L) {
    Mesh* m = ComoMalla(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (!m || !m->vertexColor) return 0;
    m->DesinstanciarDatos(W3DMD_COL);   // COW: geometria compartida (MallaDatos.h)
    lua_Integer n = luaL_optinteger(L, 3, 0);
    if (n <= 0) n = (lua_Integer)lua_rawlen(L, 2);
    bool cambio = false;
    for (lua_Integer k = 1; k + 4 <= n; k += 5) {
        lua_rawgeti(L, 2, k);
        lua_rawgeti(L, 2, k + 1);
        lua_rawgeti(L, 2, k + 2);
        lua_rawgeti(L, 2, k + 3);
        lua_rawgeti(L, 2, k + 4);
        int i = (int)lua_tointeger(L, -5) - 1;
        if (i >= 0 && i < m->vertexSize) {
            GLubyte* c = m->vertexColor + (size_t)i * 4;
            c[0] = CanalColor(lua_tonumber(L, -4));
            c[1] = CanalColor(lua_tonumber(L, -3));
            c[2] = CanalColor(lua_tonumber(L, -2));
            c[3] = CanalColor(lua_tonumber(L, -1));
            cambio = true;
        }
        lua_pop(L, 5);
    }
    if (cambio) {
        m->skinGeomVersion++;   // UNA sola invalidacion por batch
        Redibujar();
    }
    return 0;
}
// setVertices(m, t[, n]): BATCH — t es una tabla PLANA {i1,x1,y1,z1, i2,x2,y2,z2, ...}
// con los indices de grupoVertices. UN solo cruce lua<->C por frame y CERO allocs
// en C: es la forma eficiente de animar el agua (mover N vertices en circulos).
// El 3er argumento opcional acota cuantas ENTRADAS de la tabla se leen (en
// numeros, multiplo de 4): el script rellena solo el prefijo y evita rearmar
// la tabla o pagar rawlen sobre una tabla gigante reciclada.
// Los indices invalidos se saltean; las entradas incompletas del final se ignoran.
static int LSetVertices(lua_State* L) {
    Mesh* m = ComoMalla(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (!m || !m->vertex) return 0;
    m->DesinstanciarDatos(W3DMD_POS);   // COW: geometria compartida (MallaDatos.h)
    lua_Integer n = luaL_optinteger(L, 3, 0);
    if (n <= 0) n = (lua_Integer)lua_rawlen(L, 2);
    bool cambio = false;
    for (lua_Integer k = 1; k + 3 <= n; k += 4) {
        lua_rawgeti(L, 2, k);
        lua_rawgeti(L, 2, k + 1);
        lua_rawgeti(L, 2, k + 2);
        lua_rawgeti(L, 2, k + 3);
        int i = (int)lua_tointeger(L, -4) - 1;
        if (i >= 0 && i < m->vertexSize) {
            GLfloat* v = m->vertex + (size_t)i * 3;
            v[0] = (GLfloat)lua_tonumber(L, -3);
            v[1] = (GLfloat)lua_tonumber(L, -2);
            v[2] = (GLfloat)lua_tonumber(L, -1);
            cambio = true;
        }
        lua_pop(L, 4);
    }
    if (cambio) {
        m->skinGeomVersion++;   // UNA sola invalidacion por batch
        Redibujar();
    }
    return 0;
}

// ===========================================================================
//  API DE OBJETOS — sirve para CUALQUIER tipo de objeto (mesh, luz, camara,
//  vacio, ui, texto...), porque todo lo que se toca aca vive en Object.
//
//  MODELO MENTAL (dos familias de verbos, no mezclarlas):
//    * los "set..." son ABSOLUTOS: setPosicion/setRotacion/setEscala PONEN el
//      valor final. Es un TELETRANSPORTE (salto instantaneo, sin recorrido):
//      respawn, colocar algo, acomodar la escena en inicio().
//    * mover/girar son RELATIVOS y por-frame: SUMAN al estado actual. Son los
//      que se usan con dt para velocidad/gravedad/inercia, y son el punto de
//      enganche de la fisica (ver AplicarMovimiento).
//
//  EJES: los del MOTOR (Y arriba), los mismos que ya usan pos3/girarHacia. El
//  panel del editor muestra Z arriba y hace el swap a mano, asi que el campo
//  "Z" del panel es la 'y' de estos binds y viceversa.
//  POSICION/ROTACION/ESCALA son LOCALES (relativas al padre). Sin padre, la
//  local ES la de mundo (el caso normal de un juego).
//  Ninguno rompe con nil ni con un objeto del tipo equivocado: los getters
//  devuelven ceros/false y los setters no hacen nada.
// ===========================================================================

// PUNTO DE ENGANCHE de la fisica: TODO desplazamiento relativo pasa por aca. Hoy es una suma cruda
// (movimiento libre, atraviesa todo). Cuando exista colision, este es el UNICO lugar donde hay que
// resolver el barrido (mover hasta el choque / deslizar) para que mover() y lo que venga despues lo
// hereden solos. setPosicion NO pasa por aca a proposito: teletransportar ignora la fisica.
static void AplicarMovimiento(Object* o, float dx, float dy, float dz) {
    o->pos.x += dx; o->pos.y += dy; o->pos.z += dz;
}

// tipo(o) -> string con el tipo del objeto: "mesh", "luz", "camara", "vacio", "ui", "texto",
// "imagen", "rect", "boton", "coleccion", ... (ver W3dNombreTipo en objects/Objects.h).
// Con nil devuelve "" (asi `if tipo(o) == "luz"` nunca revienta).
static int LTipo(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    lua_pushstring(L, o ? W3dNombreTipo(o->getType()) : "");
    return 1;
}
// nombre(o) -> el nombre del objeto en el outliner ("" si es nil)
static int LNombre(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    lua_pushstring(L, o ? o->name.c_str() : "");
    return 1;
}

// ---- POSICION -------------------------------------------------------------
// posicion(o) -> x, y, z (local)
static int LPosicion(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (!o) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    lua_pushnumber(L, o->pos.x); lua_pushnumber(L, o->pos.y); lua_pushnumber(L, o->pos.z);
    return 3;
}
// setPosicion(o, x, y, z): TELETRANSPORTAR. Set ABSOLUTO, salto instantaneo, sin recorrido y sin
// fisica (no choca con nada en el camino). Para el movimiento del juego se usa mover().
static int LSetPosicion(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (o) {
        o->pos.x = (float)luaL_checknumber(L, 2);
        o->pos.y = (float)luaL_checknumber(L, 3);
        o->pos.z = (float)luaL_checknumber(L, 4);
        Redibujar();
    }
    return 0;
}
// mover(o, dx, dy, dz): DESPLAZAMIENTO RELATIVO (suma a la posicion actual), en los ejes del padre.
// Es la forma buena de mover algo cada frame: mover(pelota, vx*dt, vy*dt, vz*dt). A diferencia de
// setPosicion, pasa por AplicarMovimiento -> cuando haya colision, el motor va a resolver el
// recorrido aca (frenar/deslizar) en vez de atravesar la pared.
static int LMover(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (o) {
        AplicarMovimiento(o, (float)luaL_checknumber(L, 2),
                             (float)luaL_checknumber(L, 3),
                             (float)luaL_checknumber(L, 4));
        Redibujar();
    }
    return 0;
}

// ---- ROTACION (grados) ----------------------------------------------------
// rotacion(o) -> x, y, z en GRADOS (el euler de display: conserva las vueltas enteras)
static int LRotacion(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (!o) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    lua_pushnumber(L, o->rotEuler.x); lua_pushnumber(L, o->rotEuler.y); lua_pushnumber(L, o->rotEuler.z);
    return 3;
}
// setRotacion(o, x, y, z): orientacion ABSOLUTA en grados (euler XYZ). Usa SetRotEuler para que el
// euler mande y el quaternion se derive: asi no se pierden las vueltas ni se desincroniza el panel.
// setRotacion(o, x, y, z, "YXZ"): los mismos angulos (x sobre el eje X, y sobre Y, z sobre Z) compuestos en
// ese ORDEN (R = Ry * Rx * Rz: un vehiculo que rola y cabecea en su marco y despues apunta a su rumbo). El
// motor arma el quaternion directo y deriva el euler XYZ del panel; antes el script armaba la matriz y la
// descomponia con trigonometria en lua (en el N95, lo caro).
static int LSetRotacion(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (o) {
        const float x = (float)luaL_checknumber(L, 2), y = (float)luaL_checknumber(L, 3), z = (float)luaL_checknumber(L, 4);
        const char* orden = lua_type(L, 5) == LUA_TSTRING ? lua_tostring(L, 5) : NULL;
        if (!orden || strcmp(orden, "XYZ") == 0 || strlen(orden) != 3) {
            o->SetRotEuler(Vector3(x, y, z));
        } else {
            Quaternion q;   // (identidad)
            for (int k = 0; k < 3; k++) {
                const char c = orden[k];
                if (c == 'X' || c == 'x')      q = q * Quaternion::FromAxisAngle(1, 0, 0, x);
                else if (c == 'Y' || c == 'y') q = q * Quaternion::FromAxisAngle(0, 1, 0, y);
                else if (c == 'Z' || c == 'z') q = q * Quaternion::FromAxisAngle(0, 0, 1, z);
            }
            o->SetRot(q);
        }
        Redibujar();
    }
    return 0;
}
// girar(o, dx, dy, dz): giro RELATIVO en grados sobre los ejes PROPIOS del objeto (suma a la
// rotacion actual). El de cada frame: girar(moneda, 0, 90*dt, 0).
static int LGirar(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (o) {
        o->RotateLocal((float)luaL_checknumber(L, 2),
                       (float)luaL_checknumber(L, 3),
                       (float)luaL_checknumber(L, 4));
        o->ActualizarDisplayRot();   // RotateLocal solo toca 'rot': hay que refrescar el display
        Redibujar();
    }
    return 0;
}

// ---- ESCALA ---------------------------------------------------------------
// escala(o) -> sx, sy, sz del objeto.
// OJO: escala() SIN argumento es OTRA cosa (el factor responsive de pantalla, min(w,h)/480) y vive
// en main/script/BindsJuego.cpp, que se registra DESPUES y pisa a este. Por eso alla el bind mira si
// le pasaron un objeto y, si es asi, devuelve esta misma escala: las dos formas conviven.
static int LEscalaObj(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (!o) { lua_pushnumber(L, 1); lua_pushnumber(L, 1); lua_pushnumber(L, 1); return 3; }
    lua_pushnumber(L, o->scale.x); lua_pushnumber(L, o->scale.y); lua_pushnumber(L, o->scale.z);
    return 3;
}
// setEscala(o, sx, sy, sz): escala ABSOLUTA. Con UN solo numero es uniforme: setEscala(o, 2).
static int LSetEscala(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (o) {
        float sx = (float)luaL_checknumber(L, 2);
        float sy = (float)luaL_optnumber(L, 3, sx);
        float sz = (float)luaL_optnumber(L, 4, sx);
        o->scale.x = sx; o->scale.y = sy; o->scale.z = sz;
        Redibujar();
    }
    return 0;
}
// escalar(o, f): escala RELATIVA (multiplica la actual). El gemelo de mover/girar para crecer o
// achicarse de a poco: escalar(globo, 1 + 0.5*dt).
static int LEscalar(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (o) {
        float f = (float)luaL_checknumber(L, 2);
        o->scale.x *= f; o->scale.y *= f; o->scale.z *= f;
        Redibujar();
    }
    return 0;
}

// ---- VISIBILIDAD ----------------------------------------------------------
// visible(o) -> true/false (false si es nil)
static int LVisible(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    lua_pushboolean(L, (o && o->visible) ? 1 : 0);
    return 1;
}
// setVisible(o, bool): prende/apaga el dibujado. Sirve para cualquier objeto (una luz apagada es
// una luz invisible: Object::Render corta antes de subirla a GL). mostrar() es el alias 2D historico
// de esto (vive en BindsJuego.cpp y hace exactamente lo mismo).
// las mallas skinneadas del subarbol olvidan la caja de su ultima pose (la del autocull por frustum de
// Mesh::RenderObject): hasta el proximo skinneo no se cullean. Un juego que REUSA personajes de un pool
// (prenderlos en otro lugar con otro clip) los veia aparecer tarde: la caja vieja era la del uso anterior,
// fuera de camara, y el autocull solo la refresca tras 30 cuadros culleada.
static void OlvidarCajaSkin(Object* o) {
    if (!o) return;
    if (o->getType() == ObjectType::mesh) {
        Mesh* m = (Mesh*)o;
        if (m->skinArmature) { m->skinAabbOk = false; m->skinCullSeguidos = 0; }
    }
    for (size_t i = 0; i < o->Childrens.size(); i++) OlvidarCajaSkin(o->Childrens[i]);
}
static int LSetVisible(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (o) {
        const bool v = lua_toboolean(L, 2) != 0;
        if (v && !o->visible) OlvidarCajaSkin(o);   // se prende: su pose puede ser otra (ver arriba)
        o->visible = v; Redibujar();
    }
    return 0;
}

// ---- ESQUELETOS (animacion de huesos en el juego) --------------------------
// Jugando, cada armature reproduce su clip activo con su propio cabezal (Armature::juegoFrame, lo
// avanza W3dArmaturesJuegoTick con el FrameRate del clip). El frame global del timeline no participa.
static Armature* ArmArg(lua_State* L, int i) {
    Object* o = W3dScriptParamObjeto(L, i);
    if (o && W3dEsTipoInstancia(o->getType())) o = PrimerArmature(o);   // (una instancia: su esqueleto)
    return (o && o->getType() == ObjectType::armature) ? (Armature*)o : NULL;
}
static int ClipArg(lua_State* L, Armature* a, int i) {
    if (lua_type(L, i) == LUA_TNUMBER) return (int)lua_tonumber(L, i) - 1;   // indices de lua: base 1
    const char* n = lua_tostring(L, i);
    return n ? W3dArmatureClipPorNombre(a, n) : -1;
}
// animClip(arm, "nombre"|indice [, loop=true [, reiniciar=false]]) -> true si el clip existe.
// Elegir el MISMO clip que ya suena no lo reinicia (salvo reiniciar=true): se puede llamar todos los frames.
static int LAnimClip(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    int c = a ? ClipArg(L, a, 2) : -1;
    bool loop = lua_isnoneornil(L, 3) ? true : (lua_toboolean(L, 3) != 0);
    bool reiniciar = lua_toboolean(L, 4) != 0;
    if (a && (c != a->animActiva || reiniciar)) OlvidarCajaSkin(a);   // otro clip: la caja de la pose vieja no vale
    lua_pushboolean(L, W3dArmaturePlayClip(a, c, loop, reiniciar) ? 1 : 0);
    return 1;
}
// animActual(arm) -> nombre del clip que suena (nil si ninguno)
static int LAnimActual(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    if (!a || a->animActiva < 0 || a->animActiva >= (int)a->animations.size()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, a->animations[a->animActiva]->name.c_str());
    return 1;
}
// animFrame(arm) -> frames desde el inicio del clip (con decimales) ; animFrame(arm, f) lo fija
static int LAnimFrame(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    if (!a) { lua_pushnumber(L, 0); return 1; }
    if (!lua_isnoneornil(L, 2)) { a->juegoFrame = (float)lua_tonumber(L, 2); a->juegoTermino = false; }
    lua_pushnumber(L, a->juegoFrame);
    return 1;
}
// animLargo(arm [, clip]) -> cantidad de frames del clip (activo por defecto)
static int LAnimLargo(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    int c = !a ? -1 : (lua_isnoneornil(L, 2) ? a->animActiva : ClipArg(L, a, 2));
    if (!a || c < 0 || c >= (int)a->animations.size()) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, a->animations[c]->endFrame - a->animations[c]->startFrame + 1);
    return 1;
}
// animTermino(arm) -> true cuando un clip SIN loop llego a su ultimo frame
static int LAnimTermino(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    lua_pushboolean(L, (a && a->juegoTermino) ? 1 : 0);
    return 1;
}
// animVelocidad(arm, v): multiplicador del FrameRate del clip (1 = normal, 0 = pausa, -1 no soportado)
static int LAnimVelocidad(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    if (a && !lua_isnoneornil(L, 2)) { float v = (float)lua_tonumber(L, 2); a->juegoVel = v < 0.0f ? 0.0f : v; }
    lua_pushnumber(L, a ? a->juegoVel : 0.0f);
    return 1;
}
// ---- CAPAS (el MIX de animaciones en el juego; ver W3dCapaAnim) ----
// Con al menos una capa el armature muestra la MEZCLA de sus capas (animClip deja de mandar). Indices base 1.
static W3dCapaAnim* CapaArg(lua_State* L, Armature* a, int i, bool crear) {
    if (!a) return NULL;
    int n = (int)luaL_checkinteger(L, i);
    if (n < 1 || n > 16) return NULL;
    if ((int)a->capas.size() < n) { if (!crear) return NULL; a->capas.resize(n); }
    return &a->capas[n - 1];
}
// animCapa(arm, n, "clip" [, influencia 0..1 = 1 [, "mezclar"|"sumar"|"restar" [, loop = true [, reiniciar = false]]]])
// Crea las capas que falten. Cambiar de clip arranca la capa de 0 (el mismo clip sigue donde iba).
static int LAnimCapa(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    W3dCapaAnim* c = CapaArg(L, a, 2, true);
    const char* clip = lua_tostring(L, 3);
    if (!c || !clip) { lua_pushboolean(L, 0); return 1; }
    bool reiniciar = lua_toboolean(L, 7) != 0;
    if (c->anim != clip || reiniciar) { c->anim = clip; c->clipCache = -1; c->juegoFrame = 0.0f; c->juegoTermino = false; }
    if (!lua_isnoneornil(L, 4)) c->influencia = (float)lua_tonumber(L, 4) * 100.0f;
    if (!lua_isnoneornil(L, 5)) { const char* m = lua_tostring(L, 5);
        c->modo = (m && !strcmp(m, "sumar")) ? 1 : (m && !strcmp(m, "restar")) ? 2 : 0; }
    c->loop = lua_isnoneornil(L, 6) ? true : (lua_toboolean(L, 6) != 0);
    if (c->loop) c->juegoTermino = false;
    c->visible = true;
    lua_pushboolean(L, W3dCapaClip(a, *c) >= 0 ? 1 : 0);
    return 1;
}
// animCapaPeso(arm, n [, influencia 0..1]) -> influencia
static int LAnimCapaPeso(lua_State* L) {
    W3dCapaAnim* c = CapaArg(L, ArmArg(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) c->influencia = (float)lua_tonumber(L, 3) * 100.0f;
    lua_pushnumber(L, c ? c->influencia * 0.01f : 0.0f);
    return 1;
}
// animCapaFrame(arm, n [, f]) -> frame de la capa (desde el inicio de su clip)
static int LAnimCapaFrame(lua_State* L) {
    W3dCapaAnim* c = CapaArg(L, ArmArg(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) { c->juegoFrame = (float)lua_tonumber(L, 3); c->juegoTermino = false; }
    lua_pushnumber(L, c ? c->juegoFrame : 0.0f);
    return 1;
}
// animCapaTermino(arm, n) -> true si su clip sin loop llego al final
static int LAnimCapaTermino(lua_State* L) {
    W3dCapaAnim* c = CapaArg(L, ArmArg(L, 1), 2, false);
    lua_pushboolean(L, (c && c->juegoTermino) ? 1 : 0);
    return 1;
}
// animCapaHueso(arm, n, "hueso"|nil): la capa solo afecta a ese hueso y sus hijos (nil = todo)
static int LAnimCapaHueso(lua_State* L) {
    W3dCapaAnim* c = CapaArg(L, ArmArg(L, 1), 2, false);
    if (c) { const char* h = lua_tostring(L, 3); c->hueso = h ? h : ""; }
    return 0;
}
// animCapaVel(arm, n, v): velocidad de la capa (1 = normal)
static int LAnimCapaVel(lua_State* L) {
    W3dCapaAnim* c = CapaArg(L, ArmArg(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) { float v = (float)lua_tonumber(L, 3); c->vel = v < 0.0f ? 0.0f : v; }
    lua_pushnumber(L, c ? c->vel : 0.0f);
    return 1;
}
// animCapas(arm [, n]) -> cantidad de capas; con n recorta a n (0 = vuelve al clip unico de animClip)
static int LAnimCapas(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    if (a && !lua_isnoneornil(L, 2)) { int n = (int)lua_tointeger(L, 2); if (n < 0) n = 0; if (n < (int)a->capas.size()) a->capas.resize(n); }
    lua_pushinteger(L, a ? (lua_Integer)a->capas.size() : 0);
    return 1;
}

// ---- CAPAS DE ESCENA (el mix de animaciones de ESCENA en el juego: personajes hechos de objetos sueltos) ----
static W3dCapaAnim* EscCapaArg(lua_State* L, int i, bool crear) {
    int n = (int)luaL_checkinteger(L, i);
    if (n < 1 || n > 16) return NULL;
    if ((int)g_mixEscenas.size() < n) { if (!crear) return NULL; g_mixEscenas.resize(n); }
    return &g_mixEscenas[n - 1];
}
// escenaCapa(n, "escena" [, influencia 0..1 [, "mezclar"|"sumar"|"restar" [, loop = true [, reiniciar]]]])
static int LEscenaCapa(lua_State* L) {
    W3dCapaAnim* c = EscCapaArg(L, 1, true);
    const char* nom = lua_tostring(L, 2);
    if (!c || !nom) { lua_pushboolean(L, 0); return 1; }
    bool reiniciar = lua_toboolean(L, 6) != 0;
    if (c->anim != nom || reiniciar) { c->anim = nom; c->juegoFrame = 0.0f; c->juegoTermino = false; }
    if (!lua_isnoneornil(L, 3)) c->influencia = (float)lua_tonumber(L, 3) * 100.0f;
    if (!lua_isnoneornil(L, 4)) { const char* m = lua_tostring(L, 4);
        c->modo = (m && !strcmp(m, "sumar")) ? 1 : (m && !strcmp(m, "restar")) ? 2 : 0; }
    c->loop = lua_isnoneornil(L, 5) ? true : (lua_toboolean(L, 5) != 0);
    if (c->loop) c->juegoTermino = false;
    c->visible = true;
    lua_pushboolean(L, W3dAnimEscenaIdx(nom) >= 0 ? 1 : 0);
    return 1;
}
static int LEscenaCapaPeso(lua_State* L) {
    W3dCapaAnim* c = EscCapaArg(L, 1, false);
    if (c && !lua_isnoneornil(L, 2)) c->influencia = (float)lua_tonumber(L, 2) * 100.0f;
    lua_pushnumber(L, c ? c->influencia * 0.01f : 0.0f); return 1;
}
static int LEscenaCapaFrame(lua_State* L) {
    W3dCapaAnim* c = EscCapaArg(L, 1, false);
    if (c && !lua_isnoneornil(L, 2)) { c->juegoFrame = (float)lua_tonumber(L, 2); c->juegoTermino = false; }
    lua_pushnumber(L, c ? c->juegoFrame : 0.0f); return 1;
}
static int LEscenaCapaTermino(lua_State* L) {
    W3dCapaAnim* c = EscCapaArg(L, 1, false);
    lua_pushboolean(L, (c && c->juegoTermino) ? 1 : 0); return 1;
}
static int LEscenaCapas(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) { int n = (int)lua_tointeger(L, 1); if (n < 0) n = 0;
        if (n < (int)g_mixEscenas.size()) { g_mixEscenas.resize(n); if (n == 0) W3dMixEscenasSoltar(); } }
    lua_pushinteger(L, (lua_Integer)g_mixEscenas.size()); return 1;
}

// ---- CAPAS DE CLIPS DE JERARQUIA (el mix de una RAIZ: varios clips de jerarquia mezclados sobre ella y sus hijos;
//      ver W3dJerRaiz::capas en animation/W3dAnimSet.h). Con al menos una capa la raiz muestra la MEZCLA y su
//      animObjeto deja de posarla (como un armature con animCapa). Indices base 1. El clip: el de su biblioteca o
//      "Biblioteca/clip" de una biblioteca cargada (una capa no carga bibliotecas). ----
static W3dCapaAnim* JerCapaArg(lua_State* L, Object* o, int i, bool crear) {
    if (!o) return NULL;
    int n = (int)luaL_checkinteger(L, i);
    if (n < 1 || n > 16) return NULL;
    if (!o->clipsJer || (int)o->clipsJer->capas.size() < n) {
        if (!crear) return NULL;
        if (!o->clipsJer) o->clipsJer = new W3dJerRaiz();
        o->clipsJer->capas.resize(n);
        W3dJerCapasCambiaron(o);
    }
    return &o->clipsJer->capas[n - 1];
}
// objetoCapa(raiz, n, "clip" [, influencia 0..1 = 1 [, "mezclar"|"sumar"|"restar" [, loop = true [, reiniciar = false]]]])
// -> true si encontro el clip. Crea las capas que falten. Cambiar de clip arranca la capa de 0.
static int LObjetoCapa(lua_State* L) {
    Object* o = ObjetoAnimable(L, 1);
    W3dCapaAnim* c = JerCapaArg(L, o, 2, true);
    const char* clip = lua_tostring(L, 3);
    if (!c || !clip) { lua_pushboolean(L, 0); return 1; }
    bool reiniciar = lua_toboolean(L, 7) != 0;
    if (c->anim != clip || reiniciar) { c->anim = clip; c->retClip = 0; c->juegoFrame = 0.0f; c->juegoTermino = false; }
    if (!lua_isnoneornil(L, 4)) c->influencia = (float)lua_tonumber(L, 4) * 100.0f;
    if (!lua_isnoneornil(L, 5)) { const char* m = lua_tostring(L, 5);
        c->modo = (m && !strcmp(m, "sumar")) ? 1 : (m && !strcmp(m, "restar")) ? 2 : 0; }
    c->loop = lua_isnoneornil(L, 6) ? true : (lua_toboolean(L, 6) != 0);
    if (c->loop) c->juegoTermino = false;
    c->visible = true;
    lua_pushboolean(L, W3dJerCapaClip(o, *c, 0) ? 1 : 0);
    return 1;
}
// objetoCapaPeso(raiz, n [, influencia 0..1]) -> influencia
static int LObjetoCapaPeso(lua_State* L) {
    W3dCapaAnim* c = JerCapaArg(L, ObjetoAnimable(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) c->influencia = (float)lua_tonumber(L, 3) * 100.0f;
    lua_pushnumber(L, c ? c->influencia * 0.01f : 0.0f);
    return 1;
}
// objetoCapaFrame(raiz, n [, f]) -> frame de la capa (desde el inicio de su clip)
static int LObjetoCapaFrame(lua_State* L) {
    W3dCapaAnim* c = JerCapaArg(L, ObjetoAnimable(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) { c->juegoFrame = (float)lua_tonumber(L, 3); c->juegoTermino = false; }
    lua_pushnumber(L, c ? c->juegoFrame : 0.0f);
    return 1;
}
// objetoCapaTermino(raiz, n) -> true si su clip sin loop llego al final
static int LObjetoCapaTermino(lua_State* L) {
    W3dCapaAnim* c = JerCapaArg(L, ObjetoAnimable(L, 1), 2, false);
    lua_pushboolean(L, (c && c->juegoTermino) ? 1 : 0);
    return 1;
}
// objetoCapaVel(raiz, n, v): velocidad de la capa (1 = normal)
static int LObjetoCapaVel(lua_State* L) {
    W3dCapaAnim* c = JerCapaArg(L, ObjetoAnimable(L, 1), 2, false);
    if (c && !lua_isnoneornil(L, 3)) { float v = (float)lua_tonumber(L, 3); c->vel = v < 0.0f ? 0.0f : v; }
    lua_pushnumber(L, c ? c->vel : 0.0f);
    return 1;
}
// objetoCapas(raiz [, n]) -> cantidad de capas; con n recorta a n (0 = vuelve al clip unico de animObjeto; los
// nodos quedan donde los dejo la mezcla)
static int LObjetoCapas(lua_State* L) {
    Object* o = ObjetoAnimable(L, 1);
    if (o && o->clipsJer && !lua_isnoneornil(L, 2)) {
        int n = (int)lua_tointeger(L, 2); if (n < 0) n = 0;
        if (n < (int)o->clipsJer->capas.size()) { o->clipsJer->capas.resize(n); W3dJerCapasCambiaron(o); }
    }
    lua_pushinteger(L, (o && o->clipsJer) ? (lua_Integer)o->clipsJer->capas.size() : 0);
    return 1;
}

// animTransicion(arm, frames [, fps = 30]): congela la pose de AHORA y la funde hacia lo que suene despues durante
// 'frames' (el crossfade de pose de cualquier juego). Llamarla ANTES de cambiar el clip de la capa.
static int LAnimTransicion(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    float fr = (float)luaL_optnumber(L, 2, 8), fps = (float)luaL_optnumber(L, 3, 30);
    if (a && fps > 0.0f) W3dArmatureTransicion(a, fr / fps);
    return 0;
}
// mirarA(o, x, y, z): orienta el objeto para que su -Z (el frente de una camara) apunte al punto de mundo, sin
// rolido (el "arriba" queda hacia +Y). Pensado para objetos sin padre (camaras de juego).
static int LMirarA(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (!o) return 0;
    Vector3 p = o->GetGlobalPosition();
    Vector3 f((float)luaL_checknumber(L, 2) - p.x, (float)luaL_checknumber(L, 3) - p.y, (float)luaL_checknumber(L, 4) - p.z);
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
    Redibujar();
    return 0;
}

// huesoPunto(arm, "hueso" [, x, y, z]) -> x, y, z en MUNDO del punto (x,y,z) dado en el espacio del hueso,
// en la pose de ESTE frame (la boca de un arma, el origen de un laser...). nil si el hueso no esta.
static int LHuesoPunto(lua_State* L) {
    Armature* a = ArmArg(L, 1);
    const char* hn = lua_tostring(L, 2);
    if (!a || !hn) { lua_pushnil(L); return 1; }
    int h = -1;
    for (size_t i = 0; i < a->bones.size(); i++) if (a->bones[i].name == hn) { h = (int)i; break; }
    if (h < 0) { lua_pushnil(L); return 1; }
    EvaluarPoseEsqueleto(a, CurrentFrame);
    Matrix4 W; a->GetWorldMatrix(W);
    Vector3 p((float)luaL_optnumber(L, 3, 0), (float)luaL_optnumber(L, 4, 0), (float)luaL_optnumber(L, 5, 0));
    Vector3 w = (W * a->bones[h].poseWorld) * p;
    lua_pushnumber(L, w.x); lua_pushnumber(L, w.y); lua_pushnumber(L, w.z);
    return 3;
}

// ---- LUZ ------------------------------------------------------------------
// La luz del motor es OpenGL fixed-function: no tiene un campo "energia", tiene el color difuso
// (rgb). Aca se parte en las dos cosas que uno quiere manejar desde un juego:
//   color(l)   = el TONO (rgb ya normalizado, con el componente mas alto en 1)
//   energia(l) = el BRILLO (0..1) = el componente mas alto del difuso
// Asi color()/energia() y setColor()/setEnergia() son independientes y van y vuelven sin perderse.
// PORTABILIDAD: en web/Android solo la PRIMERA luz tiene efecto; en Symbian no hay cono ni
// atenuacion cuadratica. Si el objeto no es una luz, todo esto es no-op / ceros.
static Light* ComoLuz(lua_State* L, int idx) {
    Object* o = W3dScriptParamObjeto(L, idx);
    if (!o || o->getType() != ObjectType::light) return NULL;
    return (Light*)o;
}
// el componente mas alto del difuso = el brillo de la luz
static float BrilloDe(Light* l) {
    float m = l->diffuse[0];
    if (l->diffuse[1] > m) m = l->diffuse[1];
    if (l->diffuse[2] > m) m = l->diffuse[2];
    return m;
}
// TONO recordado de cada luz. Hace falta porque con energia 0 el difuso queda en (0,0,0) y el negro NO
// tiene tono: sin esta memoria, apagar una luz roja y volver a prenderla la devolvia BLANCA (y un
// parpadeo o un fundido perdian el color). La clave es el puntero pero NUNCA se desreferencia (solo se
// busca), asi que una luz borrada no puede romper nada; se limpia al descargar los scripts.
static std::map<Light*, Vector3> gTonoLuz;
// tono (rgb normalizado, el mas alto en 1) de la luz: del propio difuso si esta prendida, del recuerdo
// si esta en negro, y blanco si nunca se supo. De paso refresca el recuerdo.
static Vector3 TonoDe(Light* l) {
    float m = BrilloDe(l);
    if (m > 0.0001f) {
        Vector3 t(l->diffuse[0] / m, l->diffuse[1] / m, l->diffuse[2] / m);
        gTonoLuz[l] = t;
        return t;
    }
    std::map<Light*, Vector3>::iterator it = gTonoLuz.find(l);
    if (it != gTonoLuz.end()) return it->second;
    return Vector3(1, 1, 1);
}
// escribe el difuso como tono * brillo (y recuerda el tono)
static void PonerLuz(Light* l, const Vector3& tono, float e) {
    gTonoLuz[l] = tono;
    l->diffuse[0] = tono.x * e; l->diffuse[1] = tono.y * e; l->diffuse[2] = tono.z * e;
    l->diffuse[3] = 1.0f;
    Redibujar();
}
// color(l) -> r, g, b del TONO (0..1, con el componente mas alto en 1). El BRILLO va aparte, en
// energia(): asi los dos se manejan sin pisarse. Ceros si el objeto no es una luz.
static int LColor(lua_State* L) {
    Light* l = ComoLuz(L, 1);
    if (!l) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    Vector3 t = TonoDe(l);
    lua_pushnumber(L, t.x); lua_pushnumber(L, t.y); lua_pushnumber(L, t.z);
    return 3;
}
// setColor(l, r, g, b): cambia el TONO (0..1; 1,0,0 = rojo) conservando el brillo actual. Si la luz
// estaba apagada la prende. No hay que reaplicar nada: Light::RenderObject resube el color cada frame.
static int LSetColor(lua_State* L) {
    Light* l = ComoLuz(L, 1);
    if (!l) return 0;
    float r = (float)luaL_checknumber(L, 2);
    float g = (float)luaL_checknumber(L, 3);
    float b = (float)luaL_checknumber(L, 4);
    if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;
    float m = r; if (g > m) m = g; if (b > m) m = b;
    Vector3 tono = (m > 0.0001f) ? Vector3(r / m, g / m, b / m) : Vector3(1, 1, 1);
    float e = BrilloDe(l);
    if (e <= 0.0001f) e = (m > 0.0001f) ? m : 1.0f;   // estaba apagada: prenderla al ponerle un color
    PonerLuz(l, tono, e);
    return 0;
}
// energia(l) -> 0..1: el BRILLO de la luz (0 = apagada). 0 si el objeto no es una luz.
static int LEnergia(lua_State* L) {
    Light* l = ComoLuz(L, 1);
    lua_pushnumber(L, l ? BrilloDe(l) : 0.0);
    return 1;
}
// setEnergia(l, n): brillo 0..1 CONSERVANDO el tono. Es lo que se usa para prender, apagar, fundir o
// hacer parpadear una luz: setEnergia(l, 0) y despues setEnergia(l, 1) devuelve el mismo color.
static int LSetEnergia(lua_State* L) {
    Light* l = ComoLuz(L, 1);
    if (!l) return 0;
    float e = (float)luaL_checknumber(L, 2);
    if (e < 0.0f) e = 0.0f; if (e > 1.0f) e = 1.0f;
    PonerLuz(l, TonoDe(l), e);
    return 0;
}

Object* W3dScriptParamObjeto(void* Lv, int idx) {
    lua_State* L = (lua_State*)Lv;
    if (!lua_islightuserdata(L, idx)) return NULL;
    Object* o = (Object*)lua_touserdata(L, idx);
    // un puntero que lua guardo de un objeto que ya no existe (destruir(), una escena descargada): NULL, no
    // memoria liberada
    return W3dObjetoVivo(o) ? o : NULL;
}

// BUG que arregla esto: antes math.random NUNCA se re-sembraba desde el Core, asi que cada partida salia
// con la MISMA secuencia (la pelota del WhiskPaddle arrancaba igual siempre). Aca sembramos UNA vez por
// lua_State llamando a math.randomseed(semilla) via la API C, con una semilla que VARIA entre ejecuciones.
// Fuentes (todas self-contained del Core): time() (varia por segundo entre corridas), w3dGetTicks()
// (jitter sub-segundo del reloj del motor), la direccion del lua_State (distinta con ASLR) y un contador
// que garantiza que los VARIOS lua_States de un mismo juego (los 4 del whiskpaddle) no queden correlados.
// gSemillaExtra la puede reforzar la plataforma via W3dScriptSemilla() (ej time(0) del desktop).
static void SembrarRandom(lua_State* L) {
    unsigned s = 0;
    if (!gSemillaFija) {
        s = (unsigned)time(NULL);
        s ^= (unsigned)w3dGetTicks() * 2654435761u;      // hash de Knuth: dispersa el tick
        s ^= (unsigned)(size_t)L;                        // direccion del state (ASLR)
        s ^= gSemillaExtra;                              // refuerzo opcional de la plataforma
    }
    s += (++gSemContador) * 2246822519u;             // cada lua_State: semilla distinta
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

// stdlib SEGURA: base + table + string + math (sin io/os: un juego no toca discos)
static void AbrirLibs(lua_State* L) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);       lua_pop(L, 1);
    lua_pushcfunction(L, LPrint); lua_setglobal(L, "print");   // pisa el print de stdout por el que va al log
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
    SembrarRandom(L);   // sembrar el random JUSTO despues de abrir math (una vez por lua_State)
}

static void RegistrarAPI(lua_State* L) {
    lua_pushcfunction(L, LTecla);  lua_setglobal(L, "tecla");
    lua_pushcfunction(L, LTeclaApretada); lua_setglobal(L, "teclaApretada");
    lua_pushcfunction(L, LTexturasPendientes); lua_setglobal(L, "texturasPendientes");
    lua_pushcfunction(L, LCargarTexturas);     lua_setglobal(L, "cargarTexturas");
    lua_pushcfunction(L, LBotonApretado); lua_setglobal(L, "botonApretado");
    lua_pushcfunction(L, LAzar);   lua_setglobal(L, "azar");
    lua_pushcfunction(L, LObjeto); lua_setglobal(L, "objeto");
    lua_pushcfunction(L, LOpcion); lua_setglobal(L, "opcion");
    lua_pushcfunction(L, LPropiedad); lua_setglobal(L, "propiedad");
    lua_pushcfunction(L, LYo); lua_setglobal(L, "yo");
    // estado compartido entre scripts (el canal unico entre lua_States)
    lua_pushcfunction(L, LCompartido);    lua_setglobal(L, "compartido");
    lua_pushcfunction(L, LSetCompartido); lua_setglobal(L, "setCompartido");
    lua_pushcfunction(L, LStick);  lua_setglobal(L, "stick");
    lua_pushcfunction(L, LGatillo); lua_setglobal(L, "gatillo");
    lua_pushcfunction(L, LToque);  lua_setglobal(L, "toque");
    lua_pushcfunction(L, LDedo);   lua_setglobal(L, "dedo");
    lua_pushcfunction(L, LRaton);  lua_setglobal(L, "raton");
    lua_pushcfunction(L, LRatonRel); lua_setglobal(L, "ratonRel");
    lua_pushcfunction(L, LCapturarRaton); lua_setglobal(L, "capturarRaton");
    lua_pushcfunction(L, LRatonCapturado); lua_setglobal(L, "ratonCapturado");
    lua_pushcfunction(L, LBeep);   lua_setglobal(L, "beep");
    lua_pushcfunction(L, LBoton);  lua_setglobal(L, "boton");
    lua_pushcfunction(L, LSetVistas);      lua_setglobal(L, "setVistas");
    lua_pushcfunction(L, LSetVistaCamara); lua_setglobal(L, "setVistaCamara");
    lua_pushcfunction(L, LVista);          lua_setglobal(L, "vista");
    lua_pushcfunction(L, LPos3);   lua_setglobal(L, "pos3");
    lua_pushcfunction(L, LSetPos3);lua_setglobal(L, "setPos3");
    lua_pushcfunction(L, LGirarHacia); lua_setglobal(L, "girarHacia");
    lua_pushcfunction(L, LAnimar); lua_setglobal(L, "animar");
    lua_pushcfunction(L, LInstanciar); lua_setglobal(L, "instanciar");
    lua_pushcfunction(L, LDestruir); lua_setglobal(L, "destruir");
    lua_pushcfunction(L, LCargar);    lua_setglobal(L, "cargar");      // el streaming de una instancia / proxy
    lua_pushcfunction(L, LDescargar); lua_setglobal(L, "descargar");
    lua_pushcfunction(L, LCargaAuto); lua_setglobal(L, "cargaAuto");
    lua_pushcfunction(L, LCargado);   lua_setglobal(L, "cargado");
    lua_pushcfunction(L, LBuscar); lua_setglobal(L, "buscar");
    lua_pushcfunction(L, LPuntoMundo); lua_setglobal(L, "puntoMundo");
    lua_pushcfunction(L, LAnimEscena); lua_setglobal(L, "animEscena");
    lua_pushcfunction(L, LPararEscena); lua_setglobal(L, "pararEscena");
    lua_pushcfunction(L, LAnimEscenaFrame); lua_setglobal(L, "animEscenaFrame");
    lua_pushcfunction(L, LAnimEscenaActual); lua_setglobal(L, "animEscenaActual");
    // clips de JERARQUIA: un cabezal por raiz (varias animaciones a la vez) + el retarget por tamano
    lua_pushcfunction(L, LAnimObjeto);        lua_setglobal(L, "animObjeto");
    lua_pushcfunction(L, LAnimObjetoFrame);   lua_setglobal(L, "animObjetoFrame");
    lua_pushcfunction(L, LAnimObjetoTermino); lua_setglobal(L, "animObjetoTermino");
    lua_pushcfunction(L, LAnimObjetoActual);  lua_setglobal(L, "animObjetoActual");
    lua_pushcfunction(L, LAnimObjetoParar);   lua_setglobal(L, "animObjetoParar");
    lua_pushcfunction(L, LAnimObjetoVelocidad); lua_setglobal(L, "animObjetoVelocidad");
    lua_pushcfunction(L, LAnimRetarget);      lua_setglobal(L, "animRetarget");
    // vertices por grupo (sidecar .grupos.json; ver el bloque VERTICES POR SCRIPT)
    lua_pushcfunction(L, LGrupoVertices);   lua_setglobal(L, "grupoVertices");
    lua_pushcfunction(L, LVerticePos);      lua_setglobal(L, "verticePos");
    lua_pushcfunction(L, LSetVerticePos);   lua_setglobal(L, "setVerticePos");
    lua_pushcfunction(L, LVerticePosAnim); lua_setglobal(L, "verticePosAnim");
    lua_pushcfunction(L, LSetVerticeUV); lua_setglobal(L, "setVerticeUV");
    lua_pushcfunction(L, LDesplazarUV); lua_setglobal(L, "desplazarUV");
    lua_pushcfunction(L, LCopiarUV); lua_setglobal(L, "copiarUV");
    lua_pushcfunction(L, LNumVertices); lua_setglobal(L, "numVertices");
    lua_pushcfunction(L, LSetVerticeColor); lua_setglobal(L, "setVerticeColor");
    lua_pushcfunction(L, LSetVertices);     lua_setglobal(L, "setVertices");
    lua_pushcfunction(L, LSetVerticesColor);lua_setglobal(L, "setVerticesColor");
    // ---- API DE OBJETOS (cualquier tipo: mesh, luz, camara, vacio, ui, texto...) ----
    // identidad
    lua_pushcfunction(L, LTipo);        lua_setglobal(L, "tipo");
    lua_pushcfunction(L, LNombre);      lua_setglobal(L, "nombre");
    // posicion: setPosicion = teletransporte (absoluto) / mover = relativo por frame (con fisica)
    lua_pushcfunction(L, LPosicion);    lua_setglobal(L, "posicion");
    lua_pushcfunction(L, LSetPosicion); lua_setglobal(L, "setPosicion");
    lua_pushcfunction(L, LMover);       lua_setglobal(L, "mover");
    // rotacion en grados: setRotacion = absoluta / girar = relativa por frame
    lua_pushcfunction(L, LRotacion);    lua_setglobal(L, "rotacion");
    lua_pushcfunction(L, LSetRotacion); lua_setglobal(L, "setRotacion");
    lua_pushcfunction(L, LGirar);       lua_setglobal(L, "girar");
    // escala: setEscala = absoluta / escalar = relativa. OJO: 'escala' lo vuelve a registrar
    // BindsJuego (main/) con la version que ademas atiende escala() sin argumentos.
    lua_pushcfunction(L, LEscalaObj);   lua_setglobal(L, "escala");
    lua_pushcfunction(L, LSetEscala);   lua_setglobal(L, "setEscala");
    lua_pushcfunction(L, LEscalar);     lua_setglobal(L, "escalar");
    // visibilidad (mostrar() de BindsJuego es el alias historico de setVisible)
    lua_pushcfunction(L, LVisible);     lua_setglobal(L, "visible");
    lua_pushcfunction(L, LSetVisible);  lua_setglobal(L, "setVisible");
    lua_pushcfunction(L, LAnimClip);      lua_setglobal(L, "animClip");
    lua_pushcfunction(L, LAnimActual);    lua_setglobal(L, "animActual");
    lua_pushcfunction(L, LAnimFrame);     lua_setglobal(L, "animFrame");
    lua_pushcfunction(L, LAnimLargo);     lua_setglobal(L, "animLargo");
    lua_pushcfunction(L, LAnimTermino);   lua_setglobal(L, "animTermino");
    lua_pushcfunction(L, LAnimVelocidad); lua_setglobal(L, "animVelocidad");
    lua_pushcfunction(L, LHuesoPunto);    lua_setglobal(L, "huesoPunto");
    lua_pushcfunction(L, LAnimCapa);        lua_setglobal(L, "animCapa");
    lua_pushcfunction(L, LAnimCapaPeso);    lua_setglobal(L, "animCapaPeso");
    lua_pushcfunction(L, LAnimCapaFrame);   lua_setglobal(L, "animCapaFrame");
    lua_pushcfunction(L, LAnimCapaTermino); lua_setglobal(L, "animCapaTermino");
    lua_pushcfunction(L, LAnimCapaHueso);   lua_setglobal(L, "animCapaHueso");
    lua_pushcfunction(L, LAnimCapaVel);     lua_setglobal(L, "animCapaVel");
    lua_pushcfunction(L, LAnimCapas);       lua_setglobal(L, "animCapas");
    lua_pushcfunction(L, LAnimTransicion);  lua_setglobal(L, "animTransicion");
    lua_pushcfunction(L, LEscenaCapa);        lua_setglobal(L, "escenaCapa");
    lua_pushcfunction(L, LEscenaCapaPeso);    lua_setglobal(L, "escenaCapaPeso");
    lua_pushcfunction(L, LEscenaCapaFrame);   lua_setglobal(L, "escenaCapaFrame");
    lua_pushcfunction(L, LEscenaCapaTermino); lua_setglobal(L, "escenaCapaTermino");
    lua_pushcfunction(L, LEscenaCapas);       lua_setglobal(L, "escenaCapas");
    lua_pushcfunction(L, LObjetoCapa);        lua_setglobal(L, "objetoCapa");
    lua_pushcfunction(L, LObjetoCapaPeso);    lua_setglobal(L, "objetoCapaPeso");
    lua_pushcfunction(L, LObjetoCapaFrame);   lua_setglobal(L, "objetoCapaFrame");
    lua_pushcfunction(L, LObjetoCapaTermino); lua_setglobal(L, "objetoCapaTermino");
    lua_pushcfunction(L, LObjetoCapaVel);     lua_setglobal(L, "objetoCapaVel");
    lua_pushcfunction(L, LObjetoCapas);       lua_setglobal(L, "objetoCapas");
    lua_pushcfunction(L, LMirarA);          lua_setglobal(L, "mirarA");
    // luz
    lua_pushcfunction(L, LColor);       lua_setglobal(L, "color");
    lua_pushcfunction(L, LSetColor);    lua_setglobal(L, "setColor");
    lua_pushcfunction(L, LEnergia);     lua_setglobal(L, "energia");
    lua_pushcfunction(L, LSetEnergia);  lua_setglobal(L, "setEnergia");
    // config persistente (idioma, mute, ...) + mute global
    lua_pushcfunction(L, LConfig);        lua_setglobal(L, "config");
    lua_pushcfunction(L, LSetConfig);     lua_setglobal(L, "setConfig");
    lua_pushcfunction(L, LGuardarConfig); lua_setglobal(L, "guardarConfig");
    lua_pushcfunction(L, LCargarConfig);  lua_setglobal(L, "cargarConfig");
    lua_pushcfunction(L, LSilenciar);     lua_setglobal(L, "silenciar");
    lua_pushcfunction(L, LEstaMudo);      lua_setglobal(L, "estaMudo");
    // log del Core (los niveles del viewport Console) + la mini-consola por script
    lua_pushcfunction(L, LLogInfo);     lua_setglobal(L, "info");
    lua_pushcfunction(L, LLogAviso);    lua_setglobal(L, "aviso");
    lua_pushcfunction(L, LLogError);    lua_setglobal(L, "error");   // pisa el error() de lua (ver arriba)
    lua_pushcfunction(L, LLogDepurar);  lua_setglobal(L, "depurar");
    lua_pushcfunction(L, LEsDebug);     lua_setglobal(L, "esDebug");
    lua_pushcfunction(L, LLogCantidad); lua_setglobal(L, "logCantidad");
    lua_pushcfunction(L, LLogLinea);    lua_setglobal(L, "logLinea");
    // FISICA minima del Core (velocidad/acelerar/caja/rebotar/rebotarEn/rebotarDentro): el motor
    // integra y rebota, el lua no calcula posiciones a mano. Ver physics/W3dFisica.h.
    W3dFisicaRegistrarBinds((void*)L);
    // CUERPOS RIGIDOS (fisicaVel/fisicaImpulso/fisicaFuerza/fisicaRayo/contactos/
    // fisicaActiva/fisicaGravedad): cajas con masa, gravedad e impulsos. Ver physics/W3dRigido.h.
    W3dRigidosRegistrarBinds((void*)L);
    // COLISION CONTRA MALLAS (colSuelo/colPared/colRayo/colOlvidar): piso y paredes de un escenario
    // para un personaje movido por script. Ver physics/W3dColMalla.h.
    W3dColMallaRegistrarBinds((void*)L);
    // HITBOX (hitboxActivo/setHitboxActivo/setHitboxTam/hitboxDentro/hitboxTocando/verHitboxes):
    // cajas de deteccion con eventos alEntrar/alQuedarse/alSalir. Ver physics/W3dHitbox.h.
    W3dHitboxRegistrarBinds((void*)L);
    // RED (redHostear/redBuscar/redPartidas/redUnirse/redEnviar/redRecibir...): LAN, wifi o internet, con el
    // permiso que de la aplicacion. Ver net/W3dRed.h.
    w3dRed::RegistrarBinds((void*)L);
    if (gBindExtra) gBindExtra((void*)L);   // la API 2D del editor / plataforma
}

static bool CorrerArchivo(lua_State* L, const std::string& ruta) {
    // leer por el filesystem del Core (asset-aware): en Android el .lua vive en los assets del APK,
    // no en disco, y luaL_loadfile (que usa fopen) fallaba. luaL_loadbuffer detecta el bytecode .luac
    // por el header igual que loadfile, asi que los compilados siguen cargando por el mismo camino.
    std::vector<unsigned char> src;
    // la FUENTE que el streaming ya trajo a memoria (W3DREC_SCRIPT): sin tocar el disco. Solo en el JUEGO COMPILADO,
    // donde los scripts no cambian; el editor los lee frescos siempre (el IDE los edita y los recarga jugando, y una
    // copia retenida haria correr la version vieja)
    const std::vector<unsigned char>* enMemoria = 0;
#ifdef W3D_SIN_EDITOR
    { W3dRecurso* rf = W3dRecursoBuscar(W3DREC_SCRIPT, ruta);
      enMemoria = rf ? (const std::vector<unsigned char>*)W3dRecursoResolver(rf) : 0; }
#endif
    if (enMemoria && !enMemoria->empty()) src = *enMemoria;
    else if (!w3dFileSystem::ReadFileBytes(ruta, src) || src.empty()) {
        gUltimoError = "no pude leer el script: " + ruta;
        w3dLogfE("Script: %s", gUltimoError.c_str());
        return false;
    }
    // saltar el BOM UTF-8 (EF BB BF) que algunos editores ponen a los .lua de texto: luaL_loadfile lo hacia
    // y loadbuffer no, asi que sin esto un script con BOM daba un error de sintaxis raro. El bytecode .luac
    // empieza con 0x1B (nunca con BOM), asi que no se toca.
    size_t off = (src.size() >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) ? 3 : 0;
    // chunkname con '@': asi los errores salen "whiskpaddle.lua:12: ..." (igual que loadfile) en vez de
    // "[string \"...\"]:12" -> el usuario lee claro DONDE esta el error de su juego.
    std::string chunk = "@" + ruta;
    if (luaL_loadbuffer(L, (const char*)&src[0] + off, src.size() - off, chunk.c_str()) != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        gUltimoError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error de lua";
        w3dLogfE("Script: %s", gUltimoError.c_str());
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool W3dScriptCargar(Object* duenio) {
    if (!duenio || !duenio->scriptDatos || duenio->scriptDatos->scripts.empty()) return false;
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.find(duenio);
    if (it != gInstancias.end()) return true;   // ya cargadas
    std::vector<W3dScriptInst*> lista;
    gUltimoError.clear();
    for (size_t i = 0; i < duenio->scriptDatos->scripts.size(); i++) {
        const W3dScriptEntrada& ent = duenio->scriptDatos->scripts[i];
        // los indices del vector de instancias ESPEJAN los de scripts[] (ResolverRef
        // usa el mismo idx); una entrada vacia o rota deja un NULL en su lugar
        W3dScriptInst* inst = NULL;
        if (!ent.ruta.empty()) {
            inst = new W3dScriptInst();
            inst->duenio = duenio;
            inst->L = luaL_newstate();
            AbrirLibs(inst->L);
            lua_pushlightuserdata(inst->L, inst);
            lua_setfield(inst->L, LUA_REGISTRYINDEX, "w3d_inst");
            lua_pushlightuserdata(inst->L, duenio);
            lua_setfield(inst->L, LUA_REGISTRYINDEX, "w3d_duenio");
            // SU LIBRERIA (si es de una): su .lua es una entrada de ella, o a su dueno lo genero un proxy de ella. Las
            // rutas que nombra son primero de ella (W3dScriptRutaDeLibreria)
            {
                std::string lib;
                if (ent.ruta.compare(0, 4, "lib:") == 0) {
                    const size_t b = ent.ruta.find('/', 4);
                    if (b != std::string::npos && b > 4) lib = ent.ruta.substr(4, b - 4);
                }
                if (lib.empty() && W3dScriptLibreriaHook) lib = W3dScriptLibreriaHook(duenio);
                if (!lib.empty()) {
                    lua_pushstring(inst->L, lib.c_str());
                    lua_setfield(inst->L, LUA_REGISTRYINDEX, "w3d_lib");
                }
            }
            RegistrarAPI(inst->L);
            if (!CorrerArchivo(inst->L, ent.ruta)) {
                lua_close(inst->L);
                delete inst;
                inst = NULL;
            }
        }
        lista.push_back(inst);
    }
    bool alguna = false;
    for (size_t i = 0; i < lista.size(); i++) if (lista[i]) alguna = true;
    if (!alguna) return false;
    gInstancias[duenio] = lista;
    w3dLogf("Script: %s cargo %d script(s)", duenio->name.c_str(), (int)lista.size());
    return true;
}

static W3dScriptInst* InstDe(Object* duenio, int idx) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.find(duenio);
    if (it == gInstancias.end()) return NULL;
    if (idx < 0 || idx >= (int)it->second.size()) return NULL;
    return it->second[idx];
}

void W3dScriptResolverRef(Object* duenio, int idx, const std::string& prop, Object* obj) {
    W3dScriptInst* inst = InstDe(duenio, idx);
    if (inst) inst->refs[prop] = obj;
}

void W3dScriptResolverOpcion(Object* duenio, int idx, const std::string& prop, const std::string& valor) {
    W3dScriptInst* inst = InstDe(duenio, idx);
    if (inst) inst->opciones[prop] = valor;
}

void W3dScriptResolverValor(Object* duenio, int idx, const std::string& prop, const std::string& valor) {
    W3dScriptInst* inst = InstDe(duenio, idx);
    if (inst) inst->valores[prop] = valor;
}

// llama una funcion global sin argumentos o con dt; false si no existe o fallo
// las llamadas del motor a lua (inicio/actualizar): el perfil por muestreo (perfilLua, script/BindsJuego.cpp) tira la
// primera muestra de cada una, que trae el tiempo de afuera de lua (el render del cuadro anterior)
unsigned g_w3dScriptLlamadas = 0;
static bool Llamar(W3dScriptInst* inst, const char* fn, bool conDt, float dt) {
    lua_State* L = inst->L;
    g_w3dScriptLlamadas++;
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    if (conDt) lua_pushnumber(L, dt);
    if (lua_pcall(L, conDt ? 1 : 0, 0, 0) != LUA_OK) {
        gUltimoError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error de lua";
        w3dLogfE("Script %s(): %s", fn, gUltimoError.c_str());
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool W3dScriptInicio(Object* duenio) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.find(duenio);
    if (it == gInstancias.end()) return false;
    bool ok = false;
    for (size_t i = 0; i < it->second.size(); i++)
        if (it->second[i] && Llamar(it->second[i], "inicio", false, 0.0f)) ok = true;
    return ok;
}

bool W3dScriptActualizar(Object* duenio, float dt) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.find(duenio);
    if (it == gInstancias.end()) return false;
    bool ok = false;
    for (size_t i = 0; i < it->second.size(); i++)
        if (it->second[i] && Llamar(it->second[i], "actualizar", true, dt)) ok = true;
    // SNAPSHOT para el flanco de teclaApretada()/botonApretado(). Con W3dScriptFinFrame (el editor y el
    // runtime 3D la llaman una vez por frame, despues de TODOS los objetos) el snapshot lo toma ella; aca solo
    // queda el camino viejo para los runtimes de un unico duenio que no la llaman.
    if (!gFinFrameExterno) {
        gTeclasPrev = gTeclas;
        gBotonesPadPrev = gBotonesPad;
        for (int m = 0; m < W3D_MANDOS; m++) gBotonesMPrev[m] = gBotonesM[m];
        gTeclasPulsadas.clear();
    }
    return ok;
}

// ---- EVENTOS (hitbox / contactos): fn(objs..., nums...) --------------------
static int IndiceEvento(const char* fn) {
    for (int i = 0; i < W3D_EVENTOS_SCRIPT; i++)
        if (strcmp(fn, kEventosScript[i]) == 0) return i;
    return -1;
}

static bool LlamarEvento(W3dScriptInst* inst, const char* fn, int idx, Object* const* objs, int nObjs,
                         const float* nums, int nNums) {
    if (idx >= 0 && inst->eventos[idx] == 0) return false;   // ya se sabe que no lo define
    lua_State* L = inst->L;
    g_w3dScriptLlamadas++;
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        if (idx >= 0) inst->eventos[idx] = 0;
        return false;
    }
    if (idx >= 0) inst->eventos[idx] = 1;
    // los objetos viajan como TODO Object* hacia lua: lightuserdata (mismo tipo que yo()/objeto())
    for (int i = 0; i < nObjs; i++) {
        if (objs && objs[i]) lua_pushlightuserdata(L, objs[i]); else lua_pushnil(L);
    }
    for (int i = 0; i < nNums; i++) lua_pushnumber(L, nums ? nums[i] : 0.0f);
    if (lua_pcall(L, nObjs + nNums, 0, 0) != LUA_OK) {
        gUltimoError = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error de lua";
        w3dLogfE("Script %s(): %s", fn, gUltimoError.c_str());
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool W3dScriptEjecutar(Object* duenio, const char* codigo, std::string& error) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.find(duenio);
    if (it == gInstancias.end() || it->second.empty()) { error = "el objeto no tiene scripts corriendo"; return false; }
    for (size_t i = 0; i < it->second.size(); i++) {
        lua_State* L = it->second[i]->L;
        if (!L) continue;
        if (luaL_dostring(L, codigo) != 0) {
            error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
            lua_pop(L, 1);
            return false;
        }
    }
    return true;
}

bool W3dScriptEvento(Object* duenio, const char* fn, Object* const* objs, int nObjs,
                     const float* nums, int nNums) {
    if (!duenio || !fn) return false;
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.find(duenio);
    if (it == gInstancias.end()) return false;
    const int idx = IndiceEvento(fn);
    bool ok = false;
    // por INDICE y releyendo el mapa en cada vuelta: el evento puede recargar los scripts de
    // su propio duenio (edicion en vivo) y dejar el vector viejo liberado
    for (size_t i = 0; ; i++) {
        it = gInstancias.find(duenio);
        if (it == gInstancias.end() || i >= it->second.size()) break;
        W3dScriptInst* inst = it->second[i];
        if (inst && LlamarEvento(inst, fn, idx, objs, nObjs, nums, nNums)) ok = true;
    }
    return ok;
}

bool W3dScriptEvento(Object* duenio, const char* fn, Object* otro, const float* nums, int nNums) {
    Object* objs[1]; objs[0] = otro;
    return W3dScriptEvento(duenio, fn, objs, 1, nums, nNums);
}

void W3dScriptFinFrame() {
    gFinFrameExterno = true;
    gTeclasPrev = gTeclas;
    gBotonesPadPrev = gBotonesPad;
    for (int m = 0; m < W3D_MANDOS; m++) gBotonesMPrev[m] = gBotonesM[m];
    gTeclasPulsadas.clear();
    // los destruir() del frame: ahora que no corre ningun actualizar()
    W3dScriptDestruirPendientes();
    // la red: manda lo que los scripts pidieron, lee lo que llego, reenvia lo confiable que falta
    w3dRed::Actualizar();
}

// ---- LA DESTRUCCION (destruir de lua, ver W3dScript.h) ----
static void JuntarSubarbol(Object* o, std::vector<Object*>& out) {
    if (!o) return;
    out.push_back(o);
    for (size_t i = 0; i < o->Childrens.size(); i++) JuntarSubarbol(o->Childrens[i], out);
}
static void QuitarCurvasDe(std::vector<AnimationObject>& v, const std::set<Object*>& s) {
    for (size_t i = v.size(); i-- > 0; ) if (s.count(v[i].obj)) v.erase(v.begin() + (long)i);
}
// una raiz 2D (escena UI): la registra el sistema de escenas (W3dEscena) y el juego compilado la sigue
// dibujando y corriendo como la escena activa; liberarla dejaba ese puntero colgado (SIGSEGV al frame siguiente)
bool W3dScriptEsEscenaUI(const Object* o) {
    extern Object* SceneCollection;
    if (!o || const_cast<Object*>(o)->getType() != ObjectType::ui) return false;
    return !o->Parent || o->Parent == SceneCollection;
}
void W3dScriptSoltarRefsA(const std::vector<Object*>& objs) {
    if (objs.empty()) return;
    std::set<Object*> todos(objs.begin(), objs.end());
    for (std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.begin(); it != gInstancias.end(); ++it)
        for (size_t k = 0; k < it->second.size(); k++) {
            W3dScriptInst* inst = it->second[k];
            if (!inst) continue;
            for (std::map<std::string, Object*>::iterator r = inst->refs.begin(); r != inst->refs.end(); ++r)
                if (todos.count(r->second)) r->second = NULL;
        }
}
void W3dScriptDestruirObjeto(Object* o) {
    extern Object* SceneCollection;
    if (!o || o == SceneCollection || W3dEsRaizRegistrada(o) || W3dScriptEsEscenaUI(o)) return;
    std::vector<Object*> sub;
    JuntarSubarbol(o, sub);
    std::set<Object*> todos(sub.begin(), sub.end());
    for (size_t i = 0; i < sub.size(); i++) {
        W3dScriptDescargarDe(sub[i]);   // (su lua_State se cierra: no queda nadie que lo nombre como yo())
        W3dFisicaOlvidar(sub[i]);
    }
    // nadie lo sigue teniendo en objeto("x")
    W3dScriptSoltarRefsA(sub);
    // las curvas de las animaciones de escena que lo nombran (la activa y las demas de la raiz activa)
    QuitarCurvasDe(AnimationObjects, todos);
    for (size_t e = 0; e < SceneAnimations.size(); e++) if (SceneAnimations[e]) QuitarCurvasDe(SceneAnimations[e]->objetos, todos);
    // la seleccion
    for (size_t i = ObjSelects.size(); i-- > 0; ) if (todos.count(ObjSelects[i])) ObjSelects.erase(ObjSelects.begin() + (long)i);
    if (todos.count(ObjActivo)) ObjActivo = NULL;
    if (todos.count(CollectionActive)) CollectionActive = SceneCollection;
    // descolgarlo y liberarlo (~Object olvida sus cuerpos rigidos, sus hitbox y sus pares)
    if (o->Parent) {
        std::vector<Object*>& ch = o->Parent->Childrens;
        for (size_t i = 0; i < ch.size(); i++) if (ch[i] == o) { ch.erase(ch.begin() + (long)i); break; }
    }
    W3dLiberarSubarbol(o);
    Redibujar();
}
void W3dScriptDestruirPendientes() {
    if (gDestruir.empty()) return;
    std::vector<W3dDestruirPedido> lista;
    lista.swap(gDestruir);   // (destruir uno puede correr codigo que pida otro: va al frame siguiente)
    for (size_t i = 0; i < lista.size(); i++) {
        Object* o = lista[i].o;
        if (!W3dObjetoVivoSerial(o, lista[i].serial)) continue;   // ya se fue (un ancestro, otra via)
        if (gObjetoDestruirFn) gObjetoDestruirFn(o);
        else W3dScriptDestruirObjeto(o);
    }
}
int W3dScriptDestruirPedidos() { return (int)gDestruir.size(); }

// cierra el lua_State y libera cada instancia de una lista (lo comparten DescargarDe/DescargarTodo)
static void CerrarInstancias(std::vector<W3dScriptInst*>& lista) {
    for (size_t i = 0; i < lista.size(); i++)
        if (lista[i]) {
            if (lista[i]->L) lua_close(lista[i]->L);
            delete lista[i];
        }
}

void W3dScriptDescargarDe(Object* duenio) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.find(duenio);
    if (it == gInstancias.end()) return;
    CerrarInstancias(it->second);
    gInstancias.erase(it);
}

void W3dScriptDescargarTodo() {
    gDestruir.clear();   // (lo pedido y no aplicado: el Stop repone la escena como estaba)
    for (std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.begin();
         it != gInstancias.end(); ++it)
        CerrarInstancias(it->second);
    gInstancias.clear();
    gTonoLuz.clear();   // el recuerdo del tono de las luces muere con la partida (se re-deriva del difuso)
    gCompartido.clear(); // el estado compartido entre scripts es DEL PLAY: la proxima partida arranca limpia
    gRatonCapturar = false; gRatonDX = 0; gRatonDY = 0;   // la captura del mouse muere con la partida (STOP la suelta)
    W3dFisicaLimpiar(); // y las velocidades tambien: el Stop deja la escena quieta (y sin punteros viejos)
    W3dHitboxFinPartida(); // y los pares de hitbox / contactos (la proxima partida arranca sin
                           // "adentro") y el overlay verHitboxes que haya prendido un script
    w3dRed::Cerrar();      // y la sesion de red (el Stop se despide de los otros)
    VistasReiniciar();     // y la pantalla dividida (el editor vuelve a una vista)
}

// ordena las propiedades por la posicion en que estan DECLARADAS en el archivo (ver abajo).
// Va a nivel de archivo a proposito: en C++03 (Symbian/RVCT) un tipo LOCAL no se puede usar como
// argumento de plantilla, asi que un functor declarado adentro de la funcion no compila en stable_sort.
struct OrdenDecl {
    const std::string* texto;
    bool operator()(const W3dScriptProp& a, const W3dScriptProp& b) const {
        size_t pa = texto->find(a.nombre), pb = texto->find(b.nombre);
        if (pa == std::string::npos && pb == std::string::npos) return a.nombre < b.nombre;
        if (pa == std::string::npos) return false;
        if (pb == std::string::npos) return true;
        if (pa != pb) return pa < pb;
        return a.nombre < b.nombre;
    }
};

bool W3dScriptLeerPropiedades(const std::string& ruta, std::vector<W3dScriptProp>* props) {
    if (props) props->clear();
    if (ruta.empty()) return false;
    lua_State* L = luaL_newstate();
    AbrirLibs(L);
    RegistrarAPI(L);   // el script puede llamar a la API en su cuerpo: que no reviente
    bool ok = CorrerArchivo(L, ruta);
    if (ok && props) {
        lua_getglobal(L, "propiedades");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    W3dScriptProp p;
                    p.nombre = lua_tostring(L, -2);
                    p.tipo = 0;
                    int tv = lua_type(L, -1);
                    if (tv == LUA_TTABLE) {
                        // tabla de strings = DESPLEGABLE de opciones
                        p.tipo = 1;
                        int n = (int)lua_rawlen(L, -1);
                        for (int i = 1; i <= n; i++) {
                            lua_rawgeti(L, -1, i);
                            if (lua_isstring(L, -1)) p.opciones.push_back(lua_tostring(L, -1));
                            lua_pop(L, 1);
                        }
                    } else if (tv == LUA_TNUMBER) {
                        // default numerico = propiedad de VALOR editable (frame, id...)
                        p.tipo = 2; p.subtipo = 0;
                        char buf[48];
                        if (lua_isinteger(L, -1))
                            snprintf(buf, sizeof(buf), "%lld", (long long)lua_tointeger(L, -1));
                        else
                            snprintf(buf, sizeof(buf), "%g", (double)lua_tonumber(L, -1));
                        p.defecto = buf;
                    } else if (tv == LUA_TBOOLEAN) {
                        // default bool = checkbox por instancia (agarrada...)
                        p.tipo = 2; p.subtipo = 1;
                        p.defecto = lua_toboolean(L, -1) ? "true" : "false";
                    } else if (tv == LUA_TSTRING) {
                        // "objeto" es LA convencion historica de referencia; cualquier
                        // otro string es un default de TEXTO editable (etiqueta...)
                        const char* s = lua_tostring(L, -1);
                        if (strcmp(s, "objeto") != 0) { p.tipo = 2; p.subtipo = 2; p.defecto = s; }
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
            std::string texto;
            if (w3dFileSystem::ReadFileBytes(ruta, src) && !src.empty())
                texto.assign((const char*)&src[0], src.size());
            OrdenDecl cmp; cmp.texto = &texto;
            std::stable_sort(props->begin(), props->end(), cmp);
        }
    }
    lua_close(L);
    return ok;
}
