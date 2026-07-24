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
//  editor exponga (estilo Unity): referencias a objetos y desplegables de
//  opciones:
//
//      propiedades = {
//        pelota     = "objeto",                          -- referencia
//        dificultad = { "facil", "normal", "dificil" },  -- desplegable
//      }
//      function inicio() ... end            -- una vez, al dar PLAY
//      function actualizar(dt) ... end      -- cada frame (dt en segundos)
//
//  Cada instancia corre en su PROPIO lua_State (aislada). El orden de
//  ejecucion es el del arbol de la escena (como se dibuja / outliner).
// ============================================================================

// una propiedad expuesta por el .lua (lo que la tarjeta del editor muestra)
struct W3dScriptProp {
    std::string nombre;
    int tipo;                          // 0 = objeto, 1 = opciones (desplegable)
    std::vector<std::string> opciones; // solo tipo 1 (la primera es el default)
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
};

// ---- runtime (instancias vivas durante el PLAY) ----------------------------
// crea UNA instancia por script del objeto (lee propiedades; false si fallo todo)
bool W3dScriptCargar(Object* duenio);
// resuelve una referencia expuesta del script 'idx' (el editor busca por nombre)
void W3dScriptResolverRef(Object* duenio, int idx, const std::string& prop, Object* obj);
// asigna una OPCION elegida (los desplegables); el .lua la lee con opcion("x")
void W3dScriptResolverOpcion(Object* duenio, int idx, const std::string& prop, const std::string& valor);
bool W3dScriptInicio(Object* duenio);              // llama inicio() de cada script
bool W3dScriptActualizar(Object* duenio, float dt);// llama actualizar(dt) de cada uno
void W3dScriptDescargarTodo();                     // STOP: mata todas las instancias
void W3dScriptDescargarDe(Object* duenio);         // solo las de ESTE objeto (edicion en vivo)

// lee SOLO la tabla `propiedades` de un .lua (para la tarjeta del editor,
// sin arrancar el juego). false si el archivo no parsea.
bool W3dScriptLeerPropiedades(const std::string& ruta, std::vector<W3dScriptProp>* props);

// teclado para los scripts: la plataforma lo alimenta (nombres en minuscula:
// "w", "s", "arriba", "abajo", ...). tecla("w") lo lee el .lua.
void W3dScriptTecla(const char* nombre, bool apretada);
void W3dScriptSoltarTeclas();
// GAMEPAD analogico: la plataforma alimenta los sticks (con la deadzone ya aplicada)
// y los botones; el .lua los lee con stick("izq") -> x,y y boton("a") -> bool
void W3dScriptStick(int cual, float x, float y);        // 0 = izq, 1 = der
void W3dScriptBotonPad(const char* nombre, bool v);     // "a" "b" "x" "y" 

// el ultimo error de lua ("" si no hubo): para mostrarlo en el editor
const char* W3dScriptUltimoError();

// binding EXTRA por plataforma (el editor registra su API 2D: posPx, setTexto,
// pantalla...). Recibe el lua_State crudo como void* para no arrastrar lua.h.
typedef void (*W3dScriptBindFn)(void* L);
void W3dScriptSetBindExtra(W3dScriptBindFn fn);

// helpers para los bindings de plataforma (empujan/leen el Object* del stack)
Object* W3dScriptParamObjeto(void* L, int idx);

#endif // W3DSCRIPT_H
