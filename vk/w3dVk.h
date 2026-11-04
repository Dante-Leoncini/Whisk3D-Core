#ifndef W3D_VK_H
#define W3D_VK_H
// ============================================================================
//  w3dVk.h - EL BACKEND VULKAN visto desde la plataforma (main.cpp): arrancarlo sobre la ventana, presentar el cuadro y
//  cerrarlo. Nada de esto enlaza Vulkan: la ventana (SDL) trae el cargador (vulkan-1.dll / libvulkan.so.1) y el
//  compilador de shaders (shaderc_shared) se abre al arrancar. Si falta cualquiera de los dos, o la GPU no tiene Vulkan,
//  VkGLIniciar devuelve false y el editor sigue con OpenGL.
//
//  El backend es el MISMO de OpenGL ES 2.0 (gles2/w3dGraphicsGLES2.cpp) compilado otra vez (vk/w3dGraphicsVk.cpp) sobre
//  vk/w3dVkGL.cpp, que hace de OpenGL ES 2.0 con Vulkan: asi se ve exactamente igual que el de shaders.
// ============================================================================
#include <string>
#include <vector>
#include <stdint.h>

struct W3dVkVentana {
    void* getInstanceProcAddr;              // PFN_vkGetInstanceProcAddr (SDL_Vulkan_GetVkGetInstanceProcAddr)
    const char* const* extensiones;         // las de instancia que pide la ventana (SDL_Vulkan_GetInstanceExtensions)
    unsigned nExtensiones;
    bool (*crearSuperficie)(void* user, void* instancia, void* superficieOut);   // VkInstance -> VkSurfaceKHR*
    void (*tamanio)(void* user, int* w, int* h);                                 // el tamanio en pixeles de la ventana
    void* user;
};

bool VkGLCargable();                                   // estan el cargador de Vulkan y shaderc (sin abrir nada mas)
bool VkGLIniciar(const W3dVkVentana& v, std::string* err);
void VkGLPresentar();                                  // el cuadro dibujado a la ventana (en vez de SDL_GL_SwapWindow)
void VkGLVsync(int intervalo);                         // 1 = esperar al monitor, 0 = no (como SDL_GL_SetSwapInterval)
void VkGLTerminar();
bool VkGLListo();
const char* VkGLDispositivo();                         // el nombre de la GPU
// GLSL 4.50 -> SPIR-V (etapa 0 vertex, 1 fragment, 2 compute), con shaderc. Para los que arman su propio dispositivo
// de Vulkan (el trazado de rayos por GPU, render/RTGpu.cpp)
// (vulkanMenor: la version de Vulkan del SPIR-V, 0 = 1.0; el trazado con los nucleos RTX pide 1.2)
bool VkCompilarGLSL(const char* fuente, int etapa, std::vector<uint32_t>& spv, std::string& err, int vulkanMenor = 0);
#endif
