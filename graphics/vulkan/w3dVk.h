#ifndef W3D_VK_H
#define W3D_VK_H
// ============================================================================
//  w3dVk.h - EL BACKEND VULKAN visto desde la plataforma (main.cpp): arrancarlo sobre la ventana, presentar el cuadro y
//  cerrarlo. Nada de esto enlaza Vulkan: la ventana (SDL) trae el cargador (vulkan-1.dll / libvulkan.so.1) y el
//  compilador de shaders (shaderc_shared) se abre al arrancar. Si falta cualquiera de los dos, o la GPU no tiene Vulkan,
//  VkGLStart devuelve false y el editor sigue con OpenGL.
//
//  El backend es el MISMO de OpenGL ES 2.0 (graphics/gles2/w3dGraphicsGLES2.cpp) compilado otra vez (graphics/vulkan/w3dGraphicsVk.cpp) sobre
//  graphics/vulkan/w3dVkGL.cpp, que hace de OpenGL ES 2.0 con Vulkan: asi se ve exactamente igual que el de shaders.
// ============================================================================
#include <string>
#include <vector>
#include <stdint.h>

struct W3dVkWindow {
    void* getInstanceProcAddr;             // PFN_vkGetInstanceProcAddr (SDL_Vulkan_GetVkGetInstanceProcAddr)
    const char* const* extensions;         // las de instancia que pide la ventana (SDL_Vulkan_GetInstanceExtensions)
    unsigned nExtensions;
    bool (*createSurface)(void* user, void* instance, void* surfaceOut);   // VkInstance -> VkSurfaceKHR*
    void (*size)(void* user, int* w, int* h);                              // el tamanio en pixeles de la ventana
    void* user;
};

bool VkGLCargable();                                   // estan el cargador de Vulkan y shaderc (sin abrir nada mas)
bool VkGLStart(const W3dVkWindow& v, std::string* err);
void VkGLPresent();                                   // el cuadro dibujado a la ventana (en vez de SDL_GL_SwapWindow)
void VkGLVsync(int interval);                         // 1 = esperar al monitor, 0 = no (como SDL_GL_SetSwapInterval)
void VkGLFinish();
bool VkGLReady();
const char* VkGLDevice();                         // el nombre de la GPU
// GLSL 4.50 -> SPIR-V (etapa 0 vertex, 1 fragment, 2 compute), con shaderc. Para los que arman su propio dispositivo
// de Vulkan (el trazado de rayos por GPU, render/RTGpu.cpp)
// (lowerVulkan: la version de Vulkan del SPIR-V, 0 = 1.0; el trazado con los nucleos RTX pide 1.2)
bool VkCompileGLSL(const char* source, int stage, std::vector<uint32_t>& spv, std::string& err, int lowerVulkan = 0);
#endif
