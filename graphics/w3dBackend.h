#ifndef W3D_BACKEND_H
#define W3D_BACKEND_H
// ============================================================================
//  w3dBackend.h - EL BACKEND GRAFICO SE ELIGE AL ARRANCAR (varios en un mismo ejecutable).
//
//  En la PC (Windows / Linux, W3D_MULTIBACKEND) el ejecutable trae OpenGL fijo (graphics/opengl/w3dGraphics.cpp) y OpenGL ES 2.0
//  (graphics/gles2/w3dGraphicsGLES2.cpp, los shaders) y Vulkan (ese mismo de shaders compilado sobre graphics/vulkan/w3dVkGL.cpp, que hace
//  de OpenGL ES 2.0 con Vulkan). Cada backend vive en su propio namespace
//  (w3dEngine::gl, w3dEngine::es2, w3dEngine::vk) y llena una TABLA con sus funciones; w3dEngine::Foo(...) llama a la del backend
//  activo (graphics/w3dGraphicsDespacho.cpp, generado por tools/generar_despacho.py). En el resto de las plataformas (el
//  N95, el N8, Android, la web, las consolas) hay UN backend y nada de esto existe: las llamadas van directo.
//
//  Se elige ANTES de crear la ventana (lo pide la opcion "Graphics" de Ajustes, cfg.graphicsAPI; cambiarla vale al
//  reiniciar). Si el elegido no anda en esa maquina se usa OpenGL (Vulkan: sin el driver, sin la GPU, o sin el
//  compilador de shaders shaderc_shared al lado del ejecutable).
// ============================================================================
namespace w3dEngine {
    enum Backend { BackendOpenGL = 0, BackendGLES2 = 1, BackendVulkan = 2, Backends_ };
    const char* BackendNombre(int b);       // "OpenGL", "OpenGL ES 2.0", "Vulkan"
    int  BackendDeNombre(const char* n);    // -1 = no es un backend
    bool BackendDisponible(int b);          // esta compilado en este ejecutable
    bool BackendUsar(int b);                // (antes de crear la ventana) false = no esta: queda el que habia
    int  BackendActivo();
    bool BackendConShaders();               // el activo dibuja con shaders (las luces van por el shader: Light.cpp)
}

// ---- para los archivos de los backends: su namespace propio (solo con varios backends) ----
#ifdef W3D_BACKEND_NS
    #define W3D_BACKEND_ABRIR namespace W3D_BACKEND_NS {
    #define W3D_BACKEND_CERRAR }
    #define W3D_CAT2_(a, b) a##b
    #define W3D_CAT_(a, b) W3D_CAT2_(a, b)
    #define W3D_BACKEND_LLENAR W3D_CAT_(W3dBackendLlenar_, W3D_BACKEND_NS)
#else
    #define W3D_BACKEND_ABRIR
    #define W3D_BACKEND_CERRAR
#endif
#endif // W3D_BACKEND_H
