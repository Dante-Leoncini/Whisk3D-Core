// ============================================================================
//  W3dScript.cpp — ver W3dScript.h. Dialecto C++03 (Symbian compila esto).
// ============================================================================
#include "script/W3dScript.h"
#include "objects/Objects.h"
#include "animation/VertexAnimation.h"   // animar(): FindTargetAnim / nextAnim
#include "objects/Mesh.h"
#include "math/Quaternion.h"
#include "w3dFilesystem.h"   // leer el .lua para ORDENAR las propiedades como estan declaradas
#include "w3dlog.h"
#include <math.h>
#include <map>
#include <algorithm>
#include <string.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// una instancia viva = un lua_State propio + sus referencias resueltas
struct W3dScriptInst {
    lua_State* L;
    std::map<std::string, Object*> refs;
    std::map<std::string, std::string> opciones;   // desplegables elegidos
    Object* duenio;
    W3dScriptInst() : L(NULL), duenio(NULL) {}
};

static std::map<Object*, std::vector<W3dScriptInst*> > gInstancias;
static std::map<std::string, bool> gTeclas;
static float gSticks[2][2] = { {0,0}, {0,0} };   // [izq/der][x/y]
static std::map<std::string, bool> gBotonesPad;
static std::string gUltimoError;
static W3dScriptBindFn gBindExtra = NULL;

const char* W3dScriptUltimoError() { return gUltimoError.c_str(); }
void W3dScriptSetBindExtra(W3dScriptBindFn fn) { gBindExtra = fn; }

void W3dScriptTecla(const char* nombre, bool apretada) {
    if (nombre) gTeclas[nombre] = apretada;
}
void W3dScriptSoltarTeclas() {
    gTeclas.clear(); gBotonesPad.clear();
    gSticks[0][0] = gSticks[0][1] = gSticks[1][0] = gSticks[1][1] = 0.0f;
}
void W3dScriptStick(int cual, float x, float y) {
    if (cual < 0 || cual > 1) return;
    gSticks[cual][0] = x; gSticks[cual][1] = y;
}
void W3dScriptBotonPad(const char* nombre, bool v) { if (nombre) gBotonesPad[nombre] = v; }

// ---- funciones BASE que ven todos los scripts ------------------------------
// tecla("w") -> true si esta apretada
static int LTecla(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    std::map<std::string, bool>::iterator it = gTeclas.find(n);
    lua_pushboolean(L, it != gTeclas.end() && it->second);
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

// stick("izq"|"der") -> x, y del stick analogico (-1..1, deadzone ya aplicada)
static int LStick(lua_State* L) {
    const char* n = luaL_optstring(L, 1, "izq");
    int cual = (n && n[0] == 'd') ? 1 : 0;
    lua_pushnumber(L, gSticks[cual][0]);
    lua_pushnumber(L, gSticks[cual][1]);
    return 2;
}
// boton("a") -> true si el boton del gamepad esta apretado
static int LBoton(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    std::map<std::string, bool>::iterator it = gBotonesPad.find(n);
    lua_pushboolean(L, it != gBotonesPad.end() && it->second);
    return 1;
}
// pos3(o) -> x, y, z (coordenadas de MUNDO del objeto 3D)
static int LPos3(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (!o) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    lua_pushnumber(L, o->pos.x); lua_pushnumber(L, o->pos.y); lua_pushnumber(L, o->pos.z);
    return 3;
}
static int LSetPos3(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    if (o) {
        o->pos.x = (float)luaL_checknumber(L, 2);
        o->pos.y = (float)luaL_checknumber(L, 3);
        o->pos.z = (float)luaL_checknumber(L, 4);
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
    o->rot = Quaternion::Slerp(o->rot, q, f);
    o->ActualizarDisplayRot();
    return 0;
}
// animar(mesh, n): elige la vertex-animation que sigue (0 idle, 1 correr, ... segun
// como se armaron las animaciones del modelo)
static int LAnimar(lua_State* L) {
    Object* o = W3dScriptParamObjeto(L, 1);
    int n = (int)luaL_checkinteger(L, 2);
    if (o && o->getType() == ObjectType::mesh) {
        VertexAnimationActive* va = FindTargetAnim((Mesh*)o);
        if (va) va->nextAnim = n;
        else {
            static bool avisado = false;   // una sola vez por sesion (no spamear)
            if (!avisado) { avisado = true; w3dLogfW("animar(): '%s' no tiene vertex-animation activa", o->name.c_str()); }
        }
    }
    return 0;
}

Object* W3dScriptParamObjeto(void* Lv, int idx) {
    lua_State* L = (lua_State*)Lv;
    if (!lua_islightuserdata(L, idx)) return NULL;
    return (Object*)lua_touserdata(L, idx);
}

// stdlib SEGURA: base + table + string + math (sin io/os: un juego no toca discos)
static void AbrirLibs(lua_State* L) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);       lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
}

static void RegistrarAPI(lua_State* L) {
    lua_pushcfunction(L, LTecla);  lua_setglobal(L, "tecla");
    lua_pushcfunction(L, LObjeto); lua_setglobal(L, "objeto");
    lua_pushcfunction(L, LOpcion); lua_setglobal(L, "opcion");
    lua_pushcfunction(L, LStick);  lua_setglobal(L, "stick");
    lua_pushcfunction(L, LBoton);  lua_setglobal(L, "boton");
    lua_pushcfunction(L, LPos3);   lua_setglobal(L, "pos3");
    lua_pushcfunction(L, LSetPos3);lua_setglobal(L, "setPos3");
    lua_pushcfunction(L, LGirarHacia); lua_setglobal(L, "girarHacia");
    lua_pushcfunction(L, LAnimar); lua_setglobal(L, "animar");
    if (gBindExtra) gBindExtra((void*)L);   // la API 2D del editor / plataforma
}

static bool CorrerArchivo(lua_State* L, const std::string& ruta) {
    // los .luac compilados cargan por el MISMO camino (loadfile detecta el binario)
    if (luaL_loadfile(L, ruta.c_str()) != LUA_OK ||
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

// llama una funcion global sin argumentos o con dt; false si no existe o fallo
static bool Llamar(W3dScriptInst* inst, const char* fn, bool conDt, float dt) {
    lua_State* L = inst->L;
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
    return ok;
}

void W3dScriptDescargarDe(Object* duenio) {
    std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.find(duenio);
    if (it == gInstancias.end()) return;
    for (size_t i = 0; i < it->second.size(); i++)
        if (it->second[i]) {
            if (it->second[i]->L) lua_close(it->second[i]->L);
            delete it->second[i];
        }
    gInstancias.erase(it);
}

void W3dScriptDescargarTodo() {
    for (std::map<Object*, std::vector<W3dScriptInst*> >::iterator it = gInstancias.begin();
         it != gInstancias.end(); ++it)
        for (size_t i = 0; i < it->second.size(); i++)
            if (it->second[i]) {
                if (it->second[i]->L) lua_close(it->second[i]->L);
                delete it->second[i];
            }
    gInstancias.clear();
}

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
                    if (lua_istable(L, -1)) {
                        // tabla de strings = DESPLEGABLE de opciones
                        p.tipo = 1;
                        int n = (int)lua_rawlen(L, -1);
                        for (int i = 1; i <= n; i++) {
                            lua_rawgeti(L, -1, i);
                            if (lua_isstring(L, -1)) p.opciones.push_back(lua_tostring(L, -1));
                            lua_pop(L, 1);
                        }
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
            OrdenDecl cmp; cmp.texto = &texto;
            std::stable_sort(props->begin(), props->end(), cmp);
        }
    }
    lua_close(L);
    return ok;
}
