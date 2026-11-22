#ifndef W3D_SHADER_CPU_H
#define W3D_SHADER_CPU_H
// ============================================================================
//  LA SECCION @vertex DE UN SHADER ESCRITO, CORRIDA EN LA CPU (objects/WrittenShader.h). Donde el backend grafico no
//  tiene shaders (GLES 1.1 del N95, el OpenGL fijo de la PC, la 3DS) el MISMO texto que en GLES 2.0 / Vulkan corre la
//  GPU lo corre este interprete, vertice por vertice, antes de dibujar: el agua con olas anda igual en el N95.
//
//  Entiende el GLSL que se usa para mover vertices: float / int / bool / vec2 / vec3 / vec4, constantes, funciones
//  propias (con in / out / inout), if / else, for, return, los operadores, los swizzles (pos.y, n.xz), los
//  constructores (vec3(...)) y las funciones de GLSL de numeros (sin, cos, mix, clamp, smoothstep, dot, cross,
//  normalize, length, reflect...). Lo que no entiende (matrices, texturas, #define) es un error de compilacion con
//  su renglon. Las entradas: pos y normal (del objeto), w3d_time, w3d_ior, w3d_transmission, w3d_roughness y
//  w3d_metallic. C++03, sin excepciones (compila en RVCT para el N95).
// ============================================================================
#include <string>

struct W3dVertCPU;   // el programa compilado

// compila el texto de la seccion @vertex (con sus "#line" de W3dShaderSplit). NULL = no compila: el error, con su
// renglon del archivo, queda en 'error'. Un texto sin la funcion Vertice tambien es un error
W3dVertCPU* W3dVertCPUCompile(const std::string& text, std::string& error);
void        W3dVertCPUDelete(W3dVertCPU* p);

// las entradas que no son del vertice (las del material y el tiempo)
struct W3dVertCPUEntries {
    float time, ior, transmission, roughness, metallic;
    W3dVertCPUEntries() : time(0), ior(1), transmission(0), roughness(0.5f), metallic(0) {}
};
// corre Vertex() sobre 'n' vertices: posiciones (xyz) y normales (bytes con signo, -127..127; NULL = sin normales).
// Escribe en posOut / norOut (pueden ser los mismos arreglos). false = el programa fallo al correr (queda como estaba)
bool W3dVertCPUApply(const W3dVertCPU* p, const W3dVertCPUEntries& e, const float* pos, const signed char* nor, int n,
                       float* posOut, signed char* norOut);

#endif
