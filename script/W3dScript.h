#ifndef W3DSCRIPT_H
#define W3DSCRIPT_H
#include <string>
#include <vector>
#include <utility>

// ---- LOS EJES DE LUA: Z ARRIBA (desde el 2026-11-11) -------------------------------------------------------------------
// Lua usa los mismos ejes que ve la persona en el panel de Propiedades: X a la derecha, Y hacia adelante y Z ARRIBA. El
// motor (y el C++ de Whisk3D-Core) sigue con Y arriba. Un vector de Lua (x, y, z) es el (x, z, y) del motor: se
// intercambian la y y la z, y el cambio es su propio inverso (el mismo sirve para entrar y para salir).
// TODA funcion de Lua que recibe o devuelve un vector 3D pasa por aca: posiciones, rotaciones (grados por eje), escalas,
// velocidades, fuerzas, impulsos, la gravedad, direcciones, normales, rayos, tamanios de cajas... Los COLORES y lo 2D
// (pixeles de pantalla, UV) NO son vectores 3D y no se tocan. Antes Lua era Y arriba y el panel Z arriba: el campo "Z"
// del panel era la 'y' del script, y los tutoriales tenian que explicarlo.
inline void W3dLuaAxes(float& y, float& z) { const float t = y; y = z; z = t; }
inline void W3dLuaAxes(double& y, double& z) { const double t = y; y = z; z = t; }
// un orden de rotacion de Lua ("XYZ", "YXZ"...) en los ejes del motor: la Y y la Z se intercambian tambien en el nombre
inline std::string W3dLuaOrder(std::string o) {
    for (size_t i = 0; i < o.size(); i++) {
        if (o[i] == 'Y') o[i] = 'Z'; else if (o[i] == 'Z') o[i] = 'Y';
        else if (o[i] == 'y') o[i] = 'z'; else if (o[i] == 'z') o[i] = 'y';
    }
    return o;
}

class Object;

// ============================================================================
//  W3dScript — scripts LUA estilo Unity: cualquier objeto puede tener un .lua
//  colgado. El interprete (Lua 5.4, C plano, bien liviano) vive en el Core:
//  el juego final y el editor usan EXACTAMENTE el mismo runtime.
//
//  El .lua declara una tabla global `propiedades` con lo que quiere que el
//  editor exponga (estilo Unity): referencias a objetos, desplegables de
//  opciones y VALORES editables por instancia (numero / bool / texto):
//
//      propiedades = {
//        pelota     = "objeto",                          -- referencia
//        dificultad = { "facil", "normal", "dificil" },  -- desplegable
//        frame      = 0,                                 -- valor numerico
//        agarrada   = false,                             -- valor bool
//        etiqueta   = "x",                               -- valor de texto
//      }
//      function start() ... end            -- una vez, al dar PLAY
//      function update(dt) ... end      -- cada frame (dt en segundos)
//
//  El .lua lee lo configurado con object("pelota"), option("dificultad") y
//  property("frame") (si la instancia no configuro nada, property() devuelve
//  el default declarado en la tabla).
//
//  Cada instancia corre en su PROPIO lua_State (aislada). El orden de
//  ejecucion es el del arbol de la escena (como se dibuja / outliner).
//  Para pasarse datos entre scripts (fruta.lua -> el HUD del principal) esta el
//  ESTADO COMPARTIDO: setShared("suma", 3) escribe en un mapa C++ unico del
//  juego y getShared("suma") lo lee desde CUALQUIER script (nil si no existe).
//  Acepta numero/bool/string; setShared(nombre, nil) borra la clave. El
//  mapa entero se limpia al parar el juego (W3dScriptUnloadAll).
// ============================================================================

// ---- PREFABS (bind lua `instanciar`): el Core no conoce el parser del .w3d,
// asi que instanciar un prefab de la Biblioteca pasa por este hook, que el
// importador del editor instala al cargar una Biblioteca. posMotor = NULL o
// {x,y,z} en coordenadas del MOTOR (como setPosition). NULL = no hay prefab
// con ese nombre (o no hay Biblioteca cargada): el bind devuelve nil y el
// script reintenta el frame siguiente si quiere.
extern Object* (*W3dInstantiatePrefabHook)(const char* name, const float* posMotor);

// ---- PREFABS DEL PROYECTO (instanciar / destruir de lua; ver main/io/Prefabs.h) ----------------
//   instantiate("Enemigo", x, y, z [, rotZ [, padre]])  -> la INSTANCIA nueva (sus hijos ya generados) o nil. En Lua x, y,
//       z son ejes de Lua (Z arriba) y rotZ, el giro alrededor de la vertical; el bind los pasa al hook en ejes del motor.
//       Primero se busca un prefab del proyecto (este hook, lo instala io/Prefabs en el editor y en
//       el juego compilado); si no hay, el de la Biblioteca del formato texto (el hook de arriba).
//       pos = {x,y,z} del MOTOR (Y arriba; NULL = el origen); rotY: el giro en grados alrededor de la vertical. Con 'padre' la instancia cuelga de ese objeto
//       (pos y rotY relativos a el): una rutina con el paso "Dibujar hijos" la dibuja con su estado.
//   destroy(obj)  -> DIFERIDO al final del frame (W3dScriptEndFrame): nada se libera en medio de
//       un update() (el objeto puede ser el mismo que esta corriendo). Devuelve true si quedo pedido.
typedef Object* (*W3dPrefabInstantiateFn)(const char* name, const float* pos, float rotYDegrees);
extern W3dPrefabInstantiateFn W3dPrefabInstantiateHook;
// mientras corre el hook de arriba: el DUENO del script que llamo a instantiate() (NULL fuera de el). Un script de
// una LIBRERIA externa (su dueno lo genero un proxy de ella) nombra primero los prefabs de SU libreria
extern Object* W3dPrefabInstantiateOwner;
// mientras corre el hook de arriba: el PADRE que pidio instantiate() (su 6to argumento; NULL = la raiz activa)
extern Object* W3dPrefabInstantiateParent;

// ---- STREAMING (main/io/Streaming.h): streamLoad(obj) / streamUnload(obj) / streamAuto(obj) / isLoaded(obj) -----------------
// 'pedido': 1 = cargar (la FIJA cargada: la distancia deja de decidir), -1 = descargar (la fija descargada), 2 = volver a
// la carga automatica (la de su nodo: siempre o por distancia), 0 = solo consultar. Se APLICA en el tick del streaming
// (al final del frame: un script puede descargar la instancia que lo genero). Devuelve el estado de la instancia
// (0 descargada, 1 pidiendo sus recursos, 2 cargada) o -1 si 'o' no es una instancia de prefab ni un proxy. Lo instala
// io/Streaming (editor y juego compilado); NULL = sin streaming (los binds devuelven nil)
extern int (*W3dStreamingLuaHook)(Object* o, int request);
// ---- LA COLA DIFERIDA DE TEXTURAS (libs/Whisk3DCore/importers/import_obj.cpp): el motor sube UNA por cuadro, y una escena pedida con
// changeScene() llega con las suyas en la cola (en el N95 cada pagina de 1024 tarda: lo que aparece se ve blanco hasta que
// sube la suya). 'cargar' 0 = solo contar, 1 = cargarlas TODAS ya (bloquea). Devuelve las que quedan. Lo instala import_obj;
// NULL = sin cola (los binds devuelven 0)
extern int (*W3dTexturesLuaHook)(int load);
// las FUENTES de los scripts como recurso del almacen (io/W3dResources.h, W3DREC_SCRIPT, id = la ruta del script): el
// streaming las pide ASYNC antes de generar y el script que arranca usa la que este LISTA en vez de leer el archivo
void W3dScriptSourcesEnsureOps();

// ---- LOS SCRIPTS DE UNA LIBRERIA EXTERNA (main/io/Libraries.h) ------------------------------------------------
// Un script es DE UNA LIBRERIA si su .lua es una entrada de ella ("lib:<libreria>/scripts/x.lua") o si su dueno lo
// genero un proxy de ella (el hook de abajo, que instala io/Prefabs en el editor y en el juego compilado). Las
// rutas RELATIVAS que ese script nombra (sound(), setTexture(), importW3D()) son primero de SU libreria: una
// entrada suya ("lib:<libreria>/<ruta>", o su copia empaquetada en el juego) o un archivo suelto al lado de su .w3d
// (el hook de la ruta suelta, solo el editor); si no tiene nada con ese nombre, la regla de siempre (el proyecto).
extern std::string (*W3dScriptLibHook)(Object* owner);
extern std::string (*W3dScriptLoosePathHook)(const std::string& lib, const std::string& path);
// la libreria del script que corre en el lua_State 'L' ("" = del proyecto)
std::string W3dScriptLib(void* L);
// la ruta RELATIVA 'ruta' que nombra el script de 'L', resuelta en SU libreria ("" = no es de una libreria, o su
// libreria no tiene nada con ese nombre: el que llama sigue con su regla)
std::string W3dScriptLibPath(void* L, const std::string& path);
// QUIEN CORRE LA PARTIDA se entera de lo que un script crea y destruye (el Play del editor y el runtime
// compilado registran los suyos). NULL = nadie:
//   ObjetoNuevo(raiz)     despues de instantiate(): arrancar sus scripts (cargar + refs + start()), sus
//                         cuerpos rigidos... (sin partida registrada, lo nuevo queda quieto)
//   ObjetoDestruir(obj)   al final del frame, por cada destroy() pedido (el objeto sigue vivo): hacer
//                         lo propio (sacarlo de sus listas) y DESTRUIRLO. Sin funcion registrada se
//                         destruye con W3dScriptDestroyObject.
typedef void (*W3dScriptObjectFn)(Object* root);
void W3dScriptSetNewObject(W3dScriptObjectFn fn);
void W3dScriptSetObjectDestroy(W3dScriptObjectFn fn);
// destruir desde C++ (las CAPAS de escenas, W3dScene.h: al descargar una escena 3D cargada encima), EN EL ACTO:
// llamarla fuera de los update() (al final del frame). Avisa a quien corre la partida; sin partida registrada
// usa W3dScriptDestroyObject. (Lo nuevo se avisa con W3dScriptNotifyNewObject, mas abajo)
void W3dScriptDestroyNow(Object* o);

// ---- UN VALOR DE LUA QUE VIAJA DE UN SCRIPT A OTRO (los mensajes entre escenas: enviar / alRecibir) ----
// Los lua_State estan aislados: el valor se COPIA (nil, booleano, numero, texto, un objeto -lightuserdata- y tablas
// de eso, anidadas hasta 16 niveles; una funcion o una tabla que se nombra a si misma llegan como nil)
struct W3dValueLua {
    enum Type { Nil = 0, Bool, Num, Text, Obj, Table };
    int type;
    double num;
    bool integer;            // un numero entero de lua (5, no 5.0): llega entero
    std::string text;
    void* obj;
    std::vector<std::pair<W3dValueLua, W3dValueLua> > fields;   // (clave, valor) de una tabla
    W3dValueLua() : type(Nil), num(0.0), integer(false), obj(0) {}
};
// lee el valor de la pila de 'L' (lua_State*) en 'idx'
W3dValueLua W3dValueLuaRead(void* L, int idx);
// llama fn(texto, valor, de) en CADA script de 'duenio' que defina esa funcion global. true = alguno corrio
bool W3dScriptCallWithData(Object* owner, const char* fn, const std::string& text, const W3dValueLua& value,
                             const std::string& of);
// LA DESTRUCCION GENERICA de un objeto y su subarbol: descarga sus scripts, olvida su fisica minima, lo
// saca de las referencias de los scripts vivos (object("x")), de las curvas de las animaciones de escena y
// de la seleccion, lo descuelga y lo libera (~Object olvida sus cuerpos rigidos, hitbox y pares).
void W3dScriptDestroyObject(Object* o);
// ningun script vivo sigue teniendo a estos objetos en object("x") (la ref queda en NULL: object() da nil). La
// usa W3dScriptDestroyObject y el Play del editor, que NO libera lo del usuario que un script destruye
void W3dScriptReleaseRefsA(const std::vector<Object*>& objs);
// una ESCENA UI (una raiz 2D: el nombre de changeScene) no se destruye con destroy(): el sistema de escenas
// la sigue nombrando (se oculta con visible, o se cambia de escena). true = 'o' es una
bool W3dScriptIsSceneUI(const Object* o);
// el STREAMING (main/io/Streaming.h) genera y destruye lo de una instancia diferida POR LAS MISMAS RUTAS que
// instantiate() y destroy(): con una partida registrada (ObjetoNuevo != NULL: el Play del editor, el runtime) lo que
// genera arranca como lo instanciado, y lo que descarga se LIBERA como lo que destroy() libera (W3dScriptDestroyObject:
// sus scripts, su fisica, sus refs, sus curvas). Quien corre la partida puede registrar ObjetoLiberar para sacarlo antes
// de sus listas y fotos (el Play del editor: a diferencia de destroy(), lo del usuario NO se descuelga hasta el Stop,
// se libera -lo generado sale de su definicion, el Stop lo vuelve a generar-). Sin partida (la vista previa del
// editor) no hace nada de esto.
bool W3dScriptHasSession();
// avisa a la partida que nacio 'raiz' (sus scripts, sus cuerpos...). false = no hay partida (no hizo nada)
bool W3dScriptNotifyNewObject(Object* root);
void W3dScriptSetObjectFree(W3dScriptObjectFn fn);
// libera YA (no al final del frame: el que llama sabe que no corre ningun update()) lo que el streaming descarga
void W3dScriptFreeNow(Object* o);
// los destroy() pedidos se aplican ahora (lo llama W3dScriptEndFrame; el Stop los descarta)
void W3dScriptDestroyPending();
// cuantos destroy() hay pedidos (el harness)
int  W3dScriptDestroyRequests();
void W3dScriptTakeDestroyRequests(std::vector<Object*>& out);   // (los saca de la cola sin aplicarlos)

// una propiedad expuesta por el .lua (lo que la tarjeta del editor muestra)
struct W3dScriptProp {
    std::string name;
    int type;                         // 0 = objeto, 1 = opciones (desplegable), 2 = valor, 3 = BOTON (una funcion)
    std::vector<std::string> options; // solo tipo 1 (la primera es el default)
    int subtype;                      // solo tipo 2: 0 = numero, 1 = bool, 2 = texto
    std::string defaultValue;         // solo tipo 2: el default declarado (como texto)
    std::string label;                // solo tipo 3: el texto del boton (sin uno, el nombre)
    W3dScriptProp() : type(0), subtype(0) {}
};
// EL BOTON de una propiedad (tipo 3: en `propiedades`, una funcion, o { "Etiqueta", funcion }): lo toca la persona en
// la tarjeta del script y llama a esa funcion. Con el juego andando, en la instancia viva; sin el juego, el script se
// carga SOLO para esta llamada y se descarga (un complemento del editor: lo que la funcion le haga a la escena queda).
// false si no hay script, no es una funcion o dio error (el error queda en el log, como cualquier error de lua).
bool W3dScriptCallButton(Object* owner, int idx, const std::string& prop);
// el editor lo engancha mientras GRABA el deshacer de un script fuera del juego: antes de que lua toque la malla de 'o'
// (vertices, UV, colores) guarda su geometria. NULL = no se graba (el juego, el runtime).
extern void (*W3dScriptBeforeTouchMeshHook)(Object* o);

// UN script asignado: su .lua + lo elegido en el editor para sus propiedades
// (el NOMBRE del objeto referenciado, o la opcion del desplegable)
struct W3dScriptEntry {
    std::string path;
    std::vector<std::pair<std::string, std::string> > refs;
    // APAGADO (el ojo de la lista de scripts, en la tarjeta Control): el script sigue en el objeto y se carga, pero el
    // motor no llama a ninguna de sus funciones (inicio, actualizar, los eventos). Se guarda en el .w3d ("apagado").
    bool off;
    W3dScriptEntry() : off(false) {}
};

// datos de script GUARDADOS en el objeto (puntero opcional: casi nadie lo usa).
// Un objeto puede tener VARIOS scripts: corren en orden.
struct W3dScriptData {
    std::vector<W3dScriptEntry> scripts;
    // cual esta SELECCIONADO en el panel (estado de UI, no se guarda en el .w3d):
    // la tarjeta de abajo muestra las propiedades de ESTE. Mismo rol que
    // Mesh::activeModifier para el stack de modificadores.
    int active;
    W3dScriptData() : active(0) {}
};

// ---- runtime (instancias vivas durante el PLAY) ----------------------------
// crea UNA instancia por script del objeto (lee propiedades; false si fallo todo)
bool W3dScriptLoad(Object* owner);
// prende/apaga el script 'idx' con el juego andando (el ojo de la lista). Prenderlo por primera vez llama a su
// start(), si todavia no corrio (como en Unity: Start corre la primera vez que el componente esta activo)
void W3dScriptSetOff(Object* owner, int idx, bool off);
// resuelve una referencia expuesta del script 'idx' (el editor busca por nombre)
void W3dScriptResolveRef(Object* owner, int idx, const std::string& prop, Object* obj);
// asigna una OPCION elegida (los desplegables); el .lua la lee con option("x")
void W3dScriptResolveOption(Object* owner, int idx, const std::string& prop, const std::string& value);
// asigna un VALOR configurado por instancia (tipo 2); el .lua lo lee con
// property("x") (numero/bool/string segun el default declarado en el script)
void W3dScriptResolveValue(Object* owner, int idx, const std::string& prop, const std::string& value);
bool W3dScriptStart(Object* owner);              // llama start() de cada script
bool W3dScriptUpdate(Object* owner, float dt);// llama update(dt) de cada uno
// EVENTOS estilo Unity (hitbox, contactos de cuerpos rigidos): llama fn(objs..., nums...) en
// CADA script de 'duenio' que defina esa funcion global. Los objetos viajan como siempre
// (lightuserdata; NULL = nil) y despues los numeros. Los eventos conocidos (alEntrar,
// alQuedarse, alSalir, alTocar) se CACHEAN por instancia: un script que no define la funcion
// no paga un lua_getglobal por par y por tick. La funcion tiene que existir cuando llega el
// primer evento (definida en el cuerpo del .lua, como inicio/actualizar). true = alguno corrio.
bool W3dScriptEvent(Object* owner, const char* fn, Object* const* objs, int nObjs,
                     const float* nums, int nNums);
// atajo: fn(otro, nums...)
bool W3dScriptEvent(Object* owner, const char* fn, Object* other, const float* nums, int nNums);
// FIN DEL FRAME de scripts: toma el snapshot de teclas/botones para el FLANCO de keyPressed()/
// buttonPressed() del proximo frame. Llamarla UNA vez por frame, DESPUES de correr los actualizar de TODOS
// los objetos. Quien la llama al menos una vez pasa a mandar: W3dScriptUpdate deja de tomar el
// snapshot por su cuenta (sino, con varios objetos con script, solo el PRIMERO veia el flanco: el
// snapshot se tomaba al terminar cada objeto y el segundo ya veia la tecla como "apretada antes").
// Un runtime de un solo duenio que no la llama sigue igual que siempre.
void W3dScriptEndFrame();
void W3dScriptUnloadAll();                       // STOP: mata todas las instancias
void W3dScriptUnloadFrom(Object* owner);         // solo las de ESTE objeto (edicion en vivo)

// lee SOLO la tabla `propiedades` de un .lua (para la tarjeta del editor,
// sin arrancar el juego). false si el archivo no parsea.
bool W3dScriptReadProperties(const std::string& path, std::vector<W3dScriptProp>* props);
// LOS NOMBRES QUE VE UN SCRIPT (el autocompletado del IDE): las funciones del motor y de Lua, y lo de adentro de sus
// tablas como "math.floor". Salen de un lua_State armado igual que el de un script, asi que son siempre los que existen
void W3dScriptApiNames(std::vector<std::string>* out);

// ---- estado COMPARTIDO entre scripts (ver el bloque de arriba) -------------
// Los lua_State son independientes a proposito (aislamiento); este mapa C++
// unico es el UNICO canal entre scripts: setShared()/getShared() en lua.
// Se limpia solo en W3dScriptUnloadAll (Stop del editor / cierre del juego).
// Accesores para el harness/depuracion: cantidad de claves y el par i-esimo
// (el valor formateado como texto: "3", "true", "hola"). false = fuera de rango.
int  W3dSharedScriptCount();
bool W3dSharedScriptPair(int i, std::string* name, std::string* value);

// teclado para los scripts: la plataforma lo alimenta (nombres en minuscula:
// "w", "s", "up", "down", ...). key("w") lo lee el .lua.
void W3dScriptKey(const char* name, bool pressed);
void W3dScriptReleaseKeys();
// GAMEPAD analogico: la plataforma alimenta los sticks (con la deadzone ya aplicada)
// y los botones; el .lua los lee con stick("left") -> x,y y button("a") -> bool
void W3dScriptStick(int which, float x, float y);      // 0 = izq, 1 = der
void W3dScriptButtonPad(const char* name, bool v);     // "a" "b" "x" "y" "up"...
void W3dScriptTrigger(int which, float v);              // gatillo analogico 0..1 (0 = izq / LT, 1 = der / RT)
// ...y cada MANDO por separado (mando 0..3, en el orden en que se conectaron): lo leen button("a", 2), stick("izq", 2),
// trigger("der", 2) y buttonPressed("a", 2) para dos jugadores en el mismo equipo. Lo de arriba sigue siendo la mezcla
void W3dScriptStickOf(int gamepad, int which, float x, float y);
void W3dScriptButtonPadOf(int gamepad, const char* name, bool v);
void W3dScriptTriggerOf(int gamepad, int which, float v);
// LO QUE EL SCRIPT VE ahora mismo (harness de pruebas y overlays de debug): el
// mismo valor que devuelven stick("izq") y button("a") adentro del .lua.
void W3dScriptStickRead(int which, float* x, float* y);
bool W3dScriptButtonPadRead(const char* name);
// TOQUE/arrastre (dedo o mouse) en coordenadas del LIENZO del juego (px, centro
// en 0,0 como getPosPx). activo=false al soltar. Lua: tap() -> x, y, tocando
void W3dScriptTouch(float x, float y, bool active);      // dedo 0 (compat)
// la INCLINACION del equipo (el acelerometro, en g) en los ejes de la PANTALLA como se la ve (la plataforma ya la giro):
// lua: tilt() -> x, y, z. x = cuanto esta girada a la derecha, como un volante (sen del angulo); y = 1 parada y
// derecha; z = 1 acostada boca arriba. Sin llamar nunca a esto (sin sensor) tilt() da nil
void W3dScriptTilt(float x, float y, float z);
// vibrate(ms [, fuerza 0..1]) de lua: el vibrador del EQUIPO (input/W3dVibrate.cpp lo instala solo donde se compila: el
// telefono, la web; respeta la opcion "vibracion" y tambien hace vibrar los mandos) y el de los MANDOS
// (SDL_GameControllerRumble: lo pone quien los abre, el editor o el juego compilado). Sin el primero el bind usa el segundo
extern void (*W3dScriptVibrateHook)(int ms, float force);
extern void (*W3dScriptGamepadsVibrateHook)(int ms, float force);
void W3dScriptFinger(int i, float x, float y, bool active); // multi-touch: dedo i (0-based)
void W3dScriptMouse(float x, float y, bool inside);         // mouse-over (posicion sin clic)
void W3dScriptMouseRel(float dx, float dy);                 // mouse CAPTURADO (PC, juego estilo FPS): acumula el delta en px para mouseRel() de lua
bool W3dScriptMouseCapture();                               // un script pidio captureMouse(true): la plataforma captura el mouse mientras el juego corre
void W3dScriptCapturedMouseSet(bool v);                     // la plataforma avisa si DE VERDAD tiene el mouse capturado (mouseCaptured() en lua)

// SEMILLA del random: refuerzo OPCIONAL de la plataforma para math.random / random(). El Core ya se siembra
// solo (self-contained: time() + reloj del motor + direccion del state + contador), asi que math.random ya
// varia entre ejecuciones sin llamar a esto. Si la plataforma quiere sumar mas entropia (ej time(0) del
// desktop), llama a W3dScriptSeed(s) ANTES de cargar los scripts; se mezcla en la semilla de cada state.
void W3dScriptSeed(unsigned s);
// el harness: semillas FIJAS (dos corridas del mismo script juegan igual: las capturas se comparan). Reinicia la cuenta
void W3dScriptFixedSeed(bool on);

// PANTALLA DIVIDIDA (setViews / setCameraView de lua): cuantas vistas pidio el juego (1 o 2), como (0 lado a lado,
// 1 una arriba de la otra) y la camara de cada una (NULL = la activa). El render, antes de dibujar la vista i (0-based),
// llama a W3dViewBefore(i) -- el script reorienta lo que mira a la camara (antesDeVista) -- y al terminar las dos,
// W3dViewAfter(). view() de lua dice cual se esta dibujando
int     W3dViewsCount();
int     W3dViewsMode();
Object* W3dCameraView(int i);
int     W3dViewDrawing();
void    W3dViewBefore(int i);
void    W3dViewAfter();

// el ultimo error de lua ("" si no hubo): para mostrarlo en el editor
const char* W3dScriptLastError();

// binding EXTRA por plataforma (el editor registra su API 2D: getPosPx, setText,
// pantalla...). Recibe el lua_State crudo como void* para no arrastrar lua.h.
typedef void (*W3dScriptBindFn)(void* L);
void W3dScriptSetBindExtra(W3dScriptBindFn fn);

// HOOK de REDIBUJO: el editor es EVENT-DRIVEN (solo repinta si g_redraw esta prendido), asi que un
// cambio hecho desde lua (setPosition/move/rotate/setScale/setVisible/setColor/...) no se veria hasta
// el proximo evento del mouse. El Core NO puede tocar g_redraw / g_objectsMoved (viven en main/), asi
// que main/ registra aca una funcion que los prende y CADA setter 3D del Core la llama al final.
// En el runtime compilado no hace falta (el loop dibuja siempre): queda en NULL y no cuesta nada.
typedef void (*W3dScriptRedrawFn)(void);
void W3dScriptSetRedraw(W3dScriptRedrawFn fn);
// dispara ese hook (no hace nada si nadie lo registro). Lo llaman los modulos del Core que
// mueven objetos por su cuenta, como la fisica (physics/W3dPhysics.cpp).
void W3dScriptRedraw(void);

// helpers para los bindings de plataforma (empujan/leen el Object* del stack). Un objeto que ya NO EXISTE
// (lua lo guardo y despues se destruyo: destroy(), changeScene) llega como NULL (ver W3dObjectLive)
Object* W3dScriptParamObject(void* L, int idx);
// (harness / depuracion) corre un pedazo de codigo lua en el estado de CADA script de ese objeto (sus globales y sus
// funciones: teletransportar al personaje, forzar un estado del juego). false + error si no compila o falla
bool W3dScriptRun(Object* owner, const char* code, std::string& error);

#endif // W3DSCRIPT_H
