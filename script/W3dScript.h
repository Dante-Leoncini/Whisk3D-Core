#ifndef W3DSCRIPT_H
#define W3DSCRIPT_H
#include <string>
#include <vector>
#include <utility>

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
//      function inicio() ... end            -- una vez, al dar PLAY
//      function actualizar(dt) ... end      -- cada frame (dt en segundos)
//
//  El .lua lee lo configurado con objeto("pelota"), opcion("dificultad") y
//  propiedad("frame") (si la instancia no configuro nada, propiedad() devuelve
//  el default declarado en la tabla).
//
//  Cada instancia corre en su PROPIO lua_State (aislada). El orden de
//  ejecucion es el del arbol de la escena (como se dibuja / outliner).
//  Para pasarse datos entre scripts (fruta.lua -> el HUD del principal) esta el
//  ESTADO COMPARTIDO: setCompartido("suma", 3) escribe en un mapa C++ unico del
//  juego y compartido("suma") lo lee desde CUALQUIER script (nil si no existe).
//  Acepta numero/bool/string; setCompartido(nombre, nil) borra la clave. El
//  mapa entero se limpia al parar el juego (W3dScriptDescargarTodo).
// ============================================================================

// ---- PREFABS (bind lua `instanciar`): el Core no conoce el parser del .w3d,
// asi que instanciar un prefab de la Biblioteca pasa por este hook, que el
// importador del editor instala al cargar una Biblioteca. posMotor = NULL o
// {x,y,z} en coordenadas del MOTOR (como setPosicion). NULL = no hay prefab
// con ese nombre (o no hay Biblioteca cargada): el bind devuelve nil y el
// script reintenta el frame siguiente si quiere.
extern Object* (*W3dInstanciarPrefabHook)(const char* nombre, const float* posMotor);

// ---- PREFABS DEL PROYECTO (instanciar / destruir de lua; ver main/io/Prefabs.h) ----------------
//   instanciar("Enemigo", x, y, z [, rotY [, padre]])  -> la INSTANCIA nueva (sus hijos ya generados) o nil.
//       Primero se busca un prefab del proyecto (este hook, lo instala io/Prefabs en el editor y en
//       el juego compilado); si no hay, el de la Biblioteca del formato texto (el hook de arriba).
//       pos = {x,y,z} del motor (NULL = el origen); rotY en grados. Con 'padre' la instancia cuelga de ese objeto
//       (pos y rotY relativos a el): una rutina con el paso "Dibujar hijos" la dibuja con su estado.
//   destruir(obj)  -> DIFERIDO al final del frame (W3dScriptFinFrame): nada se libera en medio de
//       un actualizar() (el objeto puede ser el mismo que esta corriendo). Devuelve true si quedo pedido.
typedef Object* (*W3dPrefabInstanciarFn)(const char* nombre, const float* pos, float rotYGrados);
extern W3dPrefabInstanciarFn W3dPrefabInstanciarHook;
// mientras corre el hook de arriba: el DUENO del script que llamo a instanciar() (NULL fuera de el). Un script de
// una LIBRERIA externa (su dueno lo genero un proxy de ella) nombra primero los prefabs de SU libreria
extern Object* W3dPrefabInstanciarDuenio;
// mientras corre el hook de arriba: el PADRE que pidio instanciar() (su 6to argumento; NULL = la raiz activa)
extern Object* W3dPrefabInstanciarPadre;

// ---- STREAMING (main/io/Streaming.h): cargar(obj) / descargar(obj) / cargaAuto(obj) / cargado(obj) -----------------
// 'pedido': 1 = cargar (la FIJA cargada: la distancia deja de decidir), -1 = descargar (la fija descargada), 2 = volver a
// la carga automatica (la de su nodo: siempre o por distancia), 0 = solo consultar. Se APLICA en el tick del streaming
// (al final del frame: un script puede descargar la instancia que lo genero). Devuelve el estado de la instancia
// (0 descargada, 1 pidiendo sus recursos, 2 cargada) o -1 si 'o' no es una instancia de prefab ni un proxy. Lo instala
// io/Streaming (editor y juego compilado); NULL = sin streaming (los binds devuelven nil)
extern int (*W3dStreamingLuaHook)(Object* o, int pedido);
// ---- LA COLA DIFERIDA DE TEXTURAS (main/importers/import_obj.cpp): el motor sube UNA por cuadro, y una escena pedida con
// cambiarEscena() llega con las suyas en la cola (en el N95 cada pagina de 1024 tarda: lo que aparece se ve blanco hasta que
// sube la suya). 'cargar' 0 = solo contar, 1 = cargarlas TODAS ya (bloquea). Devuelve las que quedan. Lo instala import_obj;
// NULL = sin cola (los binds devuelven 0)
extern int (*W3dTexturasLuaHook)(int cargar);
// las FUENTES de los scripts como recurso del almacen (io/W3dRecursos.h, W3DREC_SCRIPT, id = la ruta del script): el
// streaming las pide ASYNC antes de generar y el script que arranca usa la que este LISTA en vez de leer el archivo
void W3dScriptFuentesAsegurarOps();

// ---- LOS SCRIPTS DE UNA LIBRERIA EXTERNA (main/io/Librerias.h) ------------------------------------------------
// Un script es DE UNA LIBRERIA si su .lua es una entrada de ella ("lib:<libreria>/scripts/x.lua") o si su dueno lo
// genero un proxy de ella (el hook de abajo, que instala io/Prefabs en el editor y en el juego compilado). Las
// rutas RELATIVAS que ese script nombra (sonido(), setTextura(), importarW3D()) son primero de SU libreria: una
// entrada suya ("lib:<libreria>/<ruta>", o su copia empaquetada en el juego) o un archivo suelto al lado de su .w3d
// (el hook de la ruta suelta, solo el editor); si no tiene nada con ese nombre, la regla de siempre (el proyecto).
extern std::string (*W3dScriptLibreriaHook)(Object* duenio);
extern std::string (*W3dScriptRutaSueltaHook)(const std::string& libreria, const std::string& ruta);
// la libreria del script que corre en el lua_State 'L' ("" = del proyecto)
std::string W3dScriptLibreria(void* L);
// la ruta RELATIVA 'ruta' que nombra el script de 'L', resuelta en SU libreria ("" = no es de una libreria, o su
// libreria no tiene nada con ese nombre: el que llama sigue con su regla)
std::string W3dScriptRutaDeLibreria(void* L, const std::string& ruta);
// QUIEN CORRE LA PARTIDA se entera de lo que un script crea y destruye (el Play del editor y el runtime
// compilado registran los suyos). NULL = nadie:
//   ObjetoNuevo(raiz)     despues de instanciar(): arrancar sus scripts (cargar + refs + inicio()), sus
//                         cuerpos rigidos... (sin partida registrada, lo nuevo queda quieto)
//   ObjetoDestruir(obj)   al final del frame, por cada destruir() pedido (el objeto sigue vivo): hacer
//                         lo propio (sacarlo de sus listas) y DESTRUIRLO. Sin funcion registrada se
//                         destruye con W3dScriptDestruirObjeto.
typedef void (*W3dScriptObjetoFn)(Object* raiz);
void W3dScriptSetObjetoNuevo(W3dScriptObjetoFn fn);
void W3dScriptSetObjetoDestruir(W3dScriptObjetoFn fn);
// LA DESTRUCCION GENERICA de un objeto y su subarbol: descarga sus scripts, olvida su fisica minima, lo
// saca de las referencias de los scripts vivos (objeto("x")), de las curvas de las animaciones de escena y
// de la seleccion, lo descuelga y lo libera (~Object olvida sus cuerpos rigidos, hitbox y pares).
void W3dScriptDestruirObjeto(Object* o);
// ningun script vivo sigue teniendo a estos objetos en objeto("x") (la ref queda en NULL: objeto() da nil). La
// usa W3dScriptDestruirObjeto y el Play del editor, que NO libera lo del usuario que un script destruye
void W3dScriptSoltarRefsA(const std::vector<Object*>& objs);
// una ESCENA UI (una raiz 2D: el nombre de cambiarEscena) no se destruye con destruir(): el sistema de escenas
// la sigue nombrando (se oculta con visible, o se cambia de escena). true = 'o' es una
bool W3dScriptEsEscenaUI(const Object* o);
// el STREAMING (main/io/Streaming.h) genera y destruye lo de una instancia diferida POR LAS MISMAS RUTAS que
// instanciar() y destruir(): con una partida registrada (ObjetoNuevo != NULL: el Play del editor, el runtime) lo que
// genera arranca como lo instanciado, y lo que descarga se LIBERA como lo que destruir() libera (W3dScriptDestruirObjeto:
// sus scripts, su fisica, sus refs, sus curvas). Quien corre la partida puede registrar ObjetoLiberar para sacarlo antes
// de sus listas y fotos (el Play del editor: a diferencia de destruir(), lo del usuario NO se descuelga hasta el Stop,
// se libera -lo generado sale de su definicion, el Stop lo vuelve a generar-). Sin partida (la vista previa del
// editor) no hace nada de esto.
bool W3dScriptHayPartida();
// avisa a la partida que nacio 'raiz' (sus scripts, sus cuerpos...). false = no hay partida (no hizo nada)
bool W3dScriptAvisarObjetoNuevo(Object* raiz);
void W3dScriptSetObjetoLiberar(W3dScriptObjetoFn fn);
// libera YA (no al final del frame: el que llama sabe que no corre ningun actualizar()) lo que el streaming descarga
void W3dScriptLiberarYa(Object* o);
// los destruir() pedidos se aplican ahora (lo llama W3dScriptFinFrame; el Stop los descarta)
void W3dScriptDestruirPendientes();
// cuantos destruir() hay pedidos (el harness)
int  W3dScriptDestruirPedidos();

// una propiedad expuesta por el .lua (lo que la tarjeta del editor muestra)
struct W3dScriptProp {
    std::string nombre;
    int tipo;                          // 0 = objeto, 1 = opciones (desplegable), 2 = valor
    std::vector<std::string> opciones; // solo tipo 1 (la primera es el default)
    int subtipo;                       // solo tipo 2: 0 = numero, 1 = bool, 2 = texto
    std::string defecto;               // solo tipo 2: el default declarado (como texto)
    W3dScriptProp() : tipo(0), subtipo(0) {}
};

// UN script asignado: su .lua + lo elegido en el editor para sus propiedades
// (el NOMBRE del objeto referenciado, o la opcion del desplegable)
struct W3dScriptEntrada {
    std::string ruta;
    std::vector<std::pair<std::string, std::string> > refs;
};

// datos de script GUARDADOS en el objeto (puntero opcional: casi nadie lo usa).
// Un objeto puede tener VARIOS scripts: corren en orden.
struct W3dScriptDatos {
    std::vector<W3dScriptEntrada> scripts;
    // cual esta SELECCIONADO en el panel (estado de UI, no se guarda en el .w3d):
    // la tarjeta de abajo muestra las propiedades de ESTE. Mismo rol que
    // Mesh::modificadorActivo para el stack de modificadores.
    int activo;
    W3dScriptDatos() : activo(0) {}
};

// ---- runtime (instancias vivas durante el PLAY) ----------------------------
// crea UNA instancia por script del objeto (lee propiedades; false si fallo todo)
bool W3dScriptCargar(Object* duenio);
// resuelve una referencia expuesta del script 'idx' (el editor busca por nombre)
void W3dScriptResolverRef(Object* duenio, int idx, const std::string& prop, Object* obj);
// asigna una OPCION elegida (los desplegables); el .lua la lee con opcion("x")
void W3dScriptResolverOpcion(Object* duenio, int idx, const std::string& prop, const std::string& valor);
// asigna un VALOR configurado por instancia (tipo 2); el .lua lo lee con
// propiedad("x") (numero/bool/string segun el default declarado en el script)
void W3dScriptResolverValor(Object* duenio, int idx, const std::string& prop, const std::string& valor);
bool W3dScriptInicio(Object* duenio);              // llama inicio() de cada script
bool W3dScriptActualizar(Object* duenio, float dt);// llama actualizar(dt) de cada uno
// EVENTOS estilo Unity (hitbox, contactos de cuerpos rigidos): llama fn(objs..., nums...) en
// CADA script de 'duenio' que defina esa funcion global. Los objetos viajan como siempre
// (lightuserdata; NULL = nil) y despues los numeros. Los eventos conocidos (alEntrar,
// alQuedarse, alSalir, alTocar) se CACHEAN por instancia: un script que no define la funcion
// no paga un lua_getglobal por par y por tick. La funcion tiene que existir cuando llega el
// primer evento (definida en el cuerpo del .lua, como inicio/actualizar). true = alguno corrio.
bool W3dScriptEvento(Object* duenio, const char* fn, Object* const* objs, int nObjs,
                     const float* nums, int nNums);
// atajo: fn(otro, nums...)
bool W3dScriptEvento(Object* duenio, const char* fn, Object* otro, const float* nums, int nNums);
// FIN DEL FRAME de scripts: toma el snapshot de teclas/botones para el FLANCO de teclaApretada()/
// botonApretado() del proximo frame. Llamarla UNA vez por frame, DESPUES de correr los actualizar de TODOS
// los objetos. Quien la llama al menos una vez pasa a mandar: W3dScriptActualizar deja de tomar el
// snapshot por su cuenta (sino, con varios objetos con script, solo el PRIMERO veia el flanco: el
// snapshot se tomaba al terminar cada objeto y el segundo ya veia la tecla como "apretada antes").
// Un runtime de un solo duenio que no la llama sigue igual que siempre.
void W3dScriptFinFrame();
void W3dScriptDescargarTodo();                     // STOP: mata todas las instancias
void W3dScriptDescargarDe(Object* duenio);         // solo las de ESTE objeto (edicion en vivo)

// lee SOLO la tabla `propiedades` de un .lua (para la tarjeta del editor,
// sin arrancar el juego). false si el archivo no parsea.
bool W3dScriptLeerPropiedades(const std::string& ruta, std::vector<W3dScriptProp>* props);

// ---- estado COMPARTIDO entre scripts (ver el bloque de arriba) -------------
// Los lua_State son independientes a proposito (aislamiento); este mapa C++
// unico es el UNICO canal entre scripts: setCompartido()/compartido() en lua.
// Se limpia solo en W3dScriptDescargarTodo (Stop del editor / cierre del juego).
// Accesores para el harness/depuracion: cantidad de claves y el par i-esimo
// (el valor formateado como texto: "3", "true", "hola"). false = fuera de rango.
int  W3dScriptCompartidoCantidad();
bool W3dScriptCompartidoPar(int i, std::string* nombre, std::string* valor);

// teclado para los scripts: la plataforma lo alimenta (nombres en minuscula:
// "w", "s", "arriba", "abajo", ...). tecla("w") lo lee el .lua.
void W3dScriptTecla(const char* nombre, bool apretada);
void W3dScriptSoltarTeclas();
// GAMEPAD analogico: la plataforma alimenta los sticks (con la deadzone ya aplicada)
// y los botones; el .lua los lee con stick("izq") -> x,y y boton("a") -> bool
void W3dScriptStick(int cual, float x, float y);        // 0 = izq, 1 = der
void W3dScriptBotonPad(const char* nombre, bool v);     // "a" "b" "x" "y" "arriba"...
void W3dScriptGatillo(int cual, float v);              // gatillo analogico 0..1 (0 = izq / LT, 1 = der / RT)
// ...y cada MANDO por separado (mando 0..3, en el orden en que se conectaron): lo leen boton("a", 2), stick("izq", 2),
// gatillo("der", 2) y botonApretado("a", 2) para dos jugadores en el mismo equipo. Lo de arriba sigue siendo la mezcla
void W3dScriptStickDe(int mando, int cual, float x, float y);
void W3dScriptBotonPadDe(int mando, const char* nombre, bool v);
void W3dScriptGatilloDe(int mando, int cual, float v);
// LO QUE EL SCRIPT VE ahora mismo (harness de pruebas y overlays de debug): el
// mismo valor que devuelven stick("izq") y boton("a") adentro del .lua.
void W3dScriptStickLeer(int cual, float* x, float* y);
bool W3dScriptBotonPadLeer(const char* nombre);
// TOQUE/arrastre (dedo o mouse) en coordenadas del LIENZO del juego (px, centro
// en 0,0 como posPx). activo=false al soltar. Lua: toque() -> x, y, tocando
void W3dScriptToque(float x, float y, bool activo);      // dedo 0 (compat)
// la INCLINACION del equipo (el acelerometro, en g) en los ejes de la PANTALLA como se la ve (la plataforma ya la giro):
// lua: inclinacion() -> x, y, z. x = cuanto esta girada a la derecha, como un volante (sen del angulo); y = 1 parada y
// derecha; z = 1 acostada boca arriba. Sin llamar nunca a esto (sin sensor) inclinacion() da nil
void W3dScriptInclinacion(float x, float y, float z);
// vibrar(ms [, fuerza 0..1]) de lua: el vibrador del EQUIPO (base/W3dVibrar.cpp lo instala solo donde se compila: el
// telefono, la web; respeta la opcion "vibracion" y tambien hace vibrar los mandos) y el de los MANDOS
// (SDL_GameControllerRumble: lo pone quien los abre, el editor o el juego compilado). Sin el primero el bind usa el segundo
extern void (*W3dScriptVibrarHook)(int ms, float fuerza);
extern void (*W3dScriptMandosVibrarHook)(int ms, float fuerza);
void W3dScriptDedo(int i, float x, float y, bool activo); // multi-touch: dedo i (0-based)
void W3dScriptRaton(float x, float y, bool dentro);       // mouse-over (posicion sin clic)
void W3dScriptRatonRel(float dx, float dy);               // mouse CAPTURADO (PC, juego estilo FPS): acumula el delta en px para ratonRel() de lua
bool W3dScriptRatonCapturar();                            // un script pidio capturarRaton(true): la plataforma captura el mouse mientras el juego corre
void W3dScriptRatonCapturadoSet(bool v);                  // la plataforma avisa si DE VERDAD tiene el mouse capturado (ratonCapturado() en lua)

// SEMILLA del random: refuerzo OPCIONAL de la plataforma para math.random / azar(). El Core ya se siembra
// solo (self-contained: time() + reloj del motor + direccion del state + contador), asi que math.random ya
// varia entre ejecuciones sin llamar a esto. Si la plataforma quiere sumar mas entropia (ej time(0) del
// desktop), llama a W3dScriptSemilla(s) ANTES de cargar los scripts; se mezcla en la semilla de cada state.
void W3dScriptSemilla(unsigned s);
// el harness: semillas FIJAS (dos corridas del mismo script juegan igual: las capturas se comparan). Reinicia la cuenta
void W3dScriptSemillaFija(bool on);

// PANTALLA DIVIDIDA (setVistas / setVistaCamara de lua): cuantas vistas pidio el juego (1 o 2), como (0 lado a lado,
// 1 una arriba de la otra) y la camara de cada una (NULL = la activa). El render, antes de dibujar la vista i (0-based),
// llama a W3dVistaAntes(i) -- el script reorienta lo que mira a la camara (antesDeVista) -- y al terminar las dos,
// W3dVistaDespues(). vista() de lua dice cual se esta dibujando
int     W3dVistasCuantas();
int     W3dVistasModo();
Object* W3dVistaCamara(int i);
int     W3dVistaDibujando();
void    W3dVistaAntes(int i);
void    W3dVistaDespues();

// el ultimo error de lua ("" si no hubo): para mostrarlo en el editor
const char* W3dScriptUltimoError();

// binding EXTRA por plataforma (el editor registra su API 2D: posPx, setTexto,
// pantalla...). Recibe el lua_State crudo como void* para no arrastrar lua.h.
typedef void (*W3dScriptBindFn)(void* L);
void W3dScriptSetBindExtra(W3dScriptBindFn fn);

// HOOK de REDIBUJO: el editor es EVENT-DRIVEN (solo repinta si g_redraw esta prendido), asi que un
// cambio hecho desde lua (setPosicion/mover/girar/setEscala/setVisible/setColor/...) no se veria hasta
// el proximo evento del mouse. El Core NO puede tocar g_redraw / g_objetosMovidos (viven en main/), asi
// que main/ registra aca una funcion que los prende y CADA setter 3D del Core la llama al final.
// En el runtime compilado no hace falta (el loop dibuja siempre): queda en NULL y no cuesta nada.
typedef void (*W3dScriptRedibujoFn)(void);
void W3dScriptSetRedibujo(W3dScriptRedibujoFn fn);
// dispara ese hook (no hace nada si nadie lo registro). Lo llaman los modulos del Core que
// mueven objetos por su cuenta, como la fisica (physics/W3dFisica.cpp).
void W3dScriptRedibujar(void);

// helpers para los bindings de plataforma (empujan/leen el Object* del stack). Un objeto que ya NO EXISTE
// (lua lo guardo y despues se destruyo: destruir(), cambiarEscena) llega como NULL (ver W3dObjetoVivo)
Object* W3dScriptParamObjeto(void* L, int idx);
// (harness / depuracion) corre un pedazo de codigo lua en el estado de CADA script de ese objeto (sus globales y sus
// funciones: teletransportar al personaje, forzar un estado del juego). false + error si no compila o falla
bool W3dScriptEjecutar(Object* duenio, const char* codigo, std::string& error);

#endif // W3DSCRIPT_H
