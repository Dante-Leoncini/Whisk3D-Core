#ifndef W3D_SHADER_ESCRITO_H
#define W3D_SHADER_ESCRITO_H
// ============================================================================
//  SHADERS ESCRITOS de los materiales (el tilde "Shader personalizado" de la tarjeta Material). El shader es un ARCHIVO
//  DE TEXTO (.glsl): adentro del proyecto ("shaders/vidrio.glsl", se edita con el IDE) o en el disco. Tiene las
//  funciones del usuario y nada mas:
//
//      @vertice                         (opcional: sin esta seccion los vertices quedan como estan)
//      void Vertice(inout vec3 pos, inout vec3 normal) { ... }
//      @fragmento                       (sin marcas, el archivo entero es el fragmento)
//      vec4 Fragmento() { ... }
//
//  Lo demas (las entradas w3d_*, el main, lo propio de cada backend) lo pone el backend grafico: el mismo archivo anda
//  en GLES 2.0 (Android, web, N8, la PC con -backend es2) y en Vulkan. Ver docs/manual/<idioma>/editor/shaders.md
// ============================================================================
#include <string>

class Material;

// el programa del shader del material para dibujar: lo compila la primera vez (y despues de W3dShaderMaterialOlvidar).
// 0 = el material se dibuja como siempre (sin el tilde, sin archivo, no compila o el backend no tiene shaders)
int  W3dShaderMaterial(Material* m);
// COMPILAR AHORA (el boton "Compilar" del material): lee el archivo de nuevo. false = no compilo; el error (con los
// renglones del archivo) queda en m->shaderError
bool W3dShaderMaterialCompilar(Material* m);
// suelta el programa (cambio el archivo o se apago el tilde): el proximo dibujo lo vuelve a compilar
void W3dShaderMaterialOlvidar(Material* m);
// LA SECCION @vertice EN LA CPU (objects/ShaderCPU.h), para los backends SIN shaders (GLES 1.1 del N95, el GL fijo):
// el programa para mover los vertices antes de dibujar. NULL = el material no tiene shader, no tiene @vertice, no
// compila, o el backend tiene shaders (ahi lo corre la GPU)
struct W3dVertCPU;
const W3dVertCPU* W3dShaderVerticeCPU(Material* m);
// UN ARCHIVO sin material (el paso "Shader" de una Rutina): el programa de la GPU, compilado la primera vez que se pide
// (0 = no compila o el backend no tiene shaders; el error en *error). Olvidar = se cambio el archivo (el IDE lo guardo)
int  W3dShaderArchivo(const std::string& ruta, std::string* error = 0);
void W3dShaderArchivoOlvidar(const std::string& ruta);
// los parametros de gfx::ShaderEscritoUsar: el tiempo, los valores de material y la rotacion de la vista al mundo
void W3dShaderParametros(float* p, float ior, float transmision, float rugosidad, float metalico);
// todos los materiales (se perdio el contexto grafico, se cerro el proyecto)
void W3dShadersOlvidarTodos();
// parte el texto del archivo en sus dos secciones. Cada una empieza con "#line N" (los errores dicen el renglon del
// ARCHIVO). Sin la marca @vertice, 'vertice' queda vacio
void W3dShaderPartir(const std::string& texto, std::string& vertice, std::string& fragmento);
// el texto de un shader NUEVO (el boton "Nuevo" del material)
const char* W3dShaderPlantilla();
// w3d_tiempo, en segundos. En el EDITOR es el de la LINEA DE TIEMPO (el cuadro actual / los FPS): con pausa queda
// quieto (no hace falta redibujar), con Play avanza y arrastrando la linea de tiempo se ve cualquier momento. Fuera del
// editor (el juego exportado) es el reloj real desde que arranco el programa
float W3dShaderReloj();
extern bool g_shaderTiempoLineaDeTiempo;   // lo prende el editor al arrancar (W3dInitUI)
// algun material con shader que usa w3d_tiempo con el reloj real: hay que seguir redibujando (HayAnimacionActiva)
bool W3dShaderHayAnimados();

#endif
