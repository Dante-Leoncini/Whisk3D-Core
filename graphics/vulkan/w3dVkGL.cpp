// ============================================================================
//  w3dVkGL.cpp - OPENGL ES 2.0 HECHO CON VULKAN. Es lo que usan el backend de shaders (graphics/gles2/w3dGraphicsGLES2.cpp,
//  compilado otra vez como w3dEngine::vk en graphics/vulkan/w3dGraphicsVk.cpp) y el cargador de texturas (graphics/w3dTexture.cpp): las
//  ~80 funciones de GL que llaman, con el mismo comportamiento. Asi el backend Vulkan dibuja EXACTAMENTE lo mismo que el
//  de OpenGL ES 2.0 sin escribir el pipeline fijo emulado otra vez.
//
//  Nada se enlaza: el cargador de Vulkan lo abre la ventana (SDL_Vulkan_LoadLibrary) y el compilador de GLSL a SPIR-V
//  (shaderc_shared, del SDK de Vulkan; va al lado del ejecutable) se abre aca. Sin alguno de los dos no arranca y el
//  editor queda en OpenGL.
//
//  Como queda GL arriba de Vulkan:
//    - LA PANTALLA es una imagen propia (RGBA 8 + profundidad/estencil) que se copia a la ventana al presentar: se puede
//      leer (glReadPixels, las capturas) y copiar (glCopyTexSubImage2D) en cualquier momento, y no se borra entre cuadros.
//      Se dibuja con el viewport DADO VUELTA (alto negativo): la fila 0 de la imagen es la de ARRIBA de la ventana, como
//      la ve Vulkan, y las coordenadas de GL (abajo a la izquierda) se convierten al pedir viewport / scissor / lecturas.
//      Ese viewport tambien deja el sentido de giro de los triangulos igual que en GL. Una TEXTURA como destino
//      (glBindFramebuffer) se dibuja sin dar vuelta: su fila 0 es la de abajo, como en GL.
//    - la profundidad de GL va de -1 a 1 y la de Vulkan de 0 a 1: el vertex shader la corrige al final.
//    - LOS SHADERS (GLSL ES 1.00) se pasan a GLSL 4.50 de Vulkan al linkear: los uniforms sueltos van a un bloque std140
//      (uno por programa, con su copia en la CPU: glUniform escribe ahi y cada draw sube el bloque si cambio), los
//      samplers a bindings, attribute / varying a locations, gl_FragColor a una salida y gl_FragCoord a la convencion
//      de GL. shaderc los preprocesa y compila.
//    - EL ESTADO (blending, profundidad, estencil, giro, primitiva, formatos de los atributos) arma la clave de un
//      pipeline, que se crea la primera vez que se usa y queda en el programa.
//    - LOS DATOS de cada draw (atributos desde la RAM, buffers dinamicos, el bloque de uniforms, las copias a texturas)
//      van a un ANILLO por cuadro en memoria visible desde la CPU. Los buffers estaticos (las mallas) van a la GPU.
//    - dos cuadros en vuelo. Lo que se borra se destruye cuando la GPU termino de usarlo.
// ============================================================================
#include "w3dVk.h"
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif
#include <GL/gl.h>
#include "w3dVkGLApi.h"
#define VK_NO_PROTOTYPES
#include "vulkan/vulkan_core.h"
#include "w3dlog.h"
#include <vector>
#include <map>
#include <string>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#ifndef GL_CLAMP
    #define GL_CLAMP 0x2900
#endif

// ============================================================================
//  Las funciones de Vulkan (se piden al cargador: nada enlazado)
// ============================================================================
#define W3D_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)
#define W3D_VK_INST(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define W3D_VK_DEV(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkQueuePresentKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
    X(vkDestroySampler) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) \
    X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetLineWidth) X(vkCmdSetDepthBias) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdClearAttachments) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdBlitImage) \
    X(vkCmdCopyImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage)
#define W3D_VK_DECL(n) static PFN_##n n = 0;
W3D_VK_GLOBAL(W3D_VK_DECL)
W3D_VK_INST(W3D_VK_DECL)
W3D_VK_DEV(W3D_VK_DECL)
#undef W3D_VK_DECL
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_ = 0;
static PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion_ = 0;
static PFN_vkCreateDebugUtilsMessengerEXT vkCreateUsefulDebugMessengerEXT_ = 0;
static PFN_vkDestroyDebugUtilsMessengerEXT vkUsefulDestroyDebugMessengerEXT_ = 0;

// ============================================================================
//  shaderc (GLSL -> SPIR-V), abierto a mano: la API de C es estable y chica
// ============================================================================
typedef void* ShcCompiler; typedef void* ShcOptions; typedef void* ShcResult;
static ShcCompiler (*shc_compiler_initialize)(void) = 0;
static void (*shc_compiler_release)(ShcCompiler) = 0;
static ShcOptions (*shc_compile_options_initialize)(void) = 0;
static void (*shc_compile_options_release)(ShcOptions) = 0;
static void (*shc_compile_options_set_target_env)(ShcOptions, int, uint32_t) = 0;
static void (*shc_compile_options_set_optimization_level)(ShcOptions, int) = 0;
static ShcResult (*shc_compile_into_spv)(ShcCompiler, const char*, size_t, int, const char*, const char*, ShcOptions) = 0;
static ShcResult (*shc_compile_into_preprocessed_text)(ShcCompiler, const char*, size_t, int, const char*, const char*, ShcOptions) = 0;
static void (*shc_result_release)(ShcResult) = 0;
static size_t (*shc_result_get_length)(ShcResult) = 0;
static const char* (*shc_result_get_bytes)(ShcResult) = 0;
static int (*shc_result_get_compilation_status)(ShcResult) = 0;
static const char* (*shc_result_get_error_message)(ShcResult) = 0;
enum { kShcVertex = 0, kShcFragment = 1, kShcVulkan = 0, kShcOptimPerformance = 2 };
static const uint32_t kShcVulkan10 = 1u << 22;
static ShcCompiler gShc = 0;
static ShcOptions gShcOpt = 0;

static void* OpenLib(const char* name) {
#ifdef _WIN32
    return (void*)LoadLibraryA(name);
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void* Symbol(void* lib, const char* n) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, n);
#else
    return dlsym(lib, n);
#endif
}
#ifdef W3D_SHADERC_STATIC
// W3D_SHADERC_STATIC: shaderc va ADENTRO del ejecutable (libshaderc_combined.a: los juegos compilados, que no llevan
// bibliotecas al lado). Las mismas funciones de su API de C, enlazadas en vez de abiertas
extern "C" {
void* shaderc_compiler_initialize(void);
void shaderc_compiler_release(void*);
void* shaderc_compile_options_initialize(void);
void shaderc_compile_options_release(void*);
void shaderc_compile_options_set_target_env(void*, int, uint32_t);
void shaderc_compile_options_set_optimization_level(void*, int);
void* shaderc_compile_into_spv(void*, const char*, size_t, int, const char*, const char*, void*);
void* shaderc_compile_into_preprocessed_text(void*, const char*, size_t, int, const char*, const char*, void*);
void shaderc_result_release(void*);
size_t shaderc_result_get_length(void*);
const char* shaderc_result_get_bytes(void*);
int shaderc_result_get_compilation_status(void*);
const char* shaderc_result_get_error_message(void*);
}
static bool OpenShaderc() {
    shc_compiler_initialize = shaderc_compiler_initialize; shc_compiler_release = shaderc_compiler_release;
    shc_compile_options_initialize = shaderc_compile_options_initialize;
    shc_compile_options_release = shaderc_compile_options_release;
    shc_compile_options_set_target_env = shaderc_compile_options_set_target_env;
    shc_compile_options_set_optimization_level = shaderc_compile_options_set_optimization_level;
    shc_compile_into_spv = shaderc_compile_into_spv;
    shc_compile_into_preprocessed_text = shaderc_compile_into_preprocessed_text;
    shc_result_release = shaderc_result_release; shc_result_get_length = shaderc_result_get_length;
    shc_result_get_bytes = shaderc_result_get_bytes;
    shc_result_get_compilation_status = shaderc_result_get_compilation_status;
    shc_result_get_error_message = shaderc_result_get_error_message;
    return true;
}
#else
// shaderc_shared: al lado del ejecutable (lo copia el build) o en el SDK de Vulkan instalado
static void* gLibShc = 0;
static bool OpenShaderc() {
    if (gLibShc) return true;
#ifdef _WIN32
    const char* names[] = { "shaderc_shared.dll" };
    const char* sub = "\\Bin\\shaderc_shared.dll";
#else
    // (el del SDK de Vulkan, y libshaderc.so.1: el paquete libshaderc1 de Debian y Ubuntu, la misma biblioteca)
    const char* names[] = { "libshaderc_shared.so.1", "libshaderc_shared.so", "libshaderc.so.1" };
    const char* sub = "/lib/libshaderc_shared.so";
#endif
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !gLibShc; i++) gLibShc = OpenLib(names[i]);
    if (!gLibShc) { const char* sdk = getenv("VULKAN_SDK"); if (sdk) gLibShc = OpenLib((std::string(sdk) + sub).c_str()); }
    if (!gLibShc) return false;
#define W3D_SHC(n) *(void**)&shc_##n = Symbol(gLibShc, "shaderc_" #n); if (!shc_##n) { gLibShc = 0; return false; }
    W3D_SHC(compiler_initialize) W3D_SHC(compiler_release) W3D_SHC(compile_options_initialize)
    W3D_SHC(compile_options_release) W3D_SHC(compile_options_set_target_env) W3D_SHC(compile_options_set_optimization_level)
    W3D_SHC(compile_into_spv) W3D_SHC(compile_into_preprocessed_text) W3D_SHC(result_release) W3D_SHC(result_get_length)
    W3D_SHC(result_get_bytes) W3D_SHC(result_get_compilation_status) W3D_SHC(result_get_error_message)
#undef W3D_SHC
    return true;
}
#endif

// GLSL 4.50 -> SPIR-V para Vulkan, para quien arme su propio dispositivo (el trazado de rayos por GPU). etapa: 0 vertex,
// 1 fragment, 2 compute. Con su propio compilador (no depende de que el backend Vulkan este andando)
bool VkCompileGLSL(const char* source, int stage, std::vector<uint32_t>& spv, std::string& err, int lowerVulkan) {
    if (!OpenShaderc()) { err = "sin shaderc_shared"; return false; }
    static ShcCompiler comp = 0; static ShcOptions opt[3] = { 0, 0, 0 };   // (Vulkan 1.0 / 1.1 / 1.2)
    const int v = lowerVulkan < 0 ? 0 : lowerVulkan > 2 ? 2 : lowerVulkan;
    if (!comp) comp = shc_compiler_initialize();
    if (comp && !opt[v]) {
        opt[v] = shc_compile_options_initialize();
        if (opt[v]) {
            shc_compile_options_set_target_env(opt[v], kShcVulkan, kShcVulkan10 | ((uint32_t)v << 12));
            shc_compile_options_set_optimization_level(opt[v], kShcOptimPerformance);
        }
    }
    if (!comp || !opt[v]) { err = "shaderc no inicio"; return false; }
    ShcResult r = shc_compile_into_spv(comp, source, strlen(source), stage, "w3d.comp", "main", opt[v]);
    const bool ok = r && shc_result_get_compilation_status(r) == 0;
    if (ok) { const size_t n = shc_result_get_length(r); spv.resize(n / 4); memcpy(&spv[0], shc_result_get_bytes(r), n); }
    else err = r ? shc_result_get_error_message(r) : "shaderc";
    if (r) shc_result_release(r);
    return ok;
}

bool VkGLCargable() {
#ifdef _WIN32
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) return false;
    FreeLibrary(vk);
#else
    void* vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) vk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!vk) return false;
    dlclose(vk);
#endif
    return OpenShaderc();
}

// ============================================================================
//  El dispositivo
// ============================================================================
static VkInstance gInst = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT gDebug = VK_NULL_HANDLE;
static VkSurfaceKHR gSup = VK_NULL_HANDLE;
static VkPhysicalDevice gPhys = VK_NULL_HANDLE;
static VkDevice gDev  = VK_NULL_HANDLE;
static VkQueue gQueue = VK_NULL_HANDLE;
static uint32_t gFamily = 0;
static VkPhysicalDeviceProperties gProps;
static VkPhysicalDeviceMemoryProperties gMemProps;
static VkPhysicalDeviceFeatures gFeat;   // las que se prendieron
static W3dVkWindow gShow;
static bool gReady = false;
static std::string gGPUName;
static int gVsync = 1;
static VkFormat gFmtProf = VK_FORMAT_UNDEFINED;
static const VkFormat kFmtColor = VK_FORMAT_R8G8B8A8_UNORM;

static inline VkDeviceSize Align(VkDeviceSize v, VkDeviceSize a) { return a > 1 ? (v + a - 1) / a * a : v; }
static GLenum gErrorGL = GL_NO_ERROR;
static void ErrorGL(GLenum e) { if (gErrorGL == GL_NO_ERROR) gErrorGL = e; }

// ============================================================================
//  MEMORIA: bloques grandes repartidos en pedazos (Windows deja ~4096 asignaciones; cada malla y cada textura pedia una)
// ============================================================================
struct Block { VkDeviceMemory mem; VkDeviceSize size; uint32_t type; bool image; unsigned char* ptr; std::map<VkDeviceSize, VkDeviceSize> free; };
struct Assign { Block* b; VkDeviceSize off, size; unsigned char* ptr; };
static std::vector<Block*> gBlocks;
static const VkDeviceSize kBlock = 64ull << 20;
static int MemoryType(uint32_t bits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < gMemProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (gMemProps.memoryTypes[i].propertyFlags & props) == props) return (int)i;
    return -1;
}
static bool Reserve(const VkMemoryRequirements& r, VkMemoryPropertyFlags props, bool image, Assign& a) {
    int type = MemoryType(r.memoryTypeBits, props);
    if (type < 0 && (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = MemoryType(r.memoryTypeBits, props & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) return false;
    for (size_t k = 0; k < gBlocks.size(); k++) {
        Block* b = gBlocks[k];
        if (b->type != (uint32_t)type || b->image != image) continue;
        for (std::map<VkDeviceSize, VkDeviceSize>::iterator it = b->free.begin(); it != b->free.end(); ++it) {
            const VkDeviceSize start = it->first, length = it->second, off = Align(start, r.alignment);
            if (off + r.size > start + length) continue;
            b->free.erase(it);
            if (off > start) b->free[start] = off - start;
            if (off + r.size < start + length) b->free[off + r.size] = start + length - off - r.size;
            a.b = b; a.off = off; a.size = r.size; a.ptr = b->ptr ? b->ptr + off : 0;
            return true;
        }
    }
    Block* b = new Block();
    b->size = r.size > kBlock ? Align(r.size, 1 << 20) : kBlock;
    b->type = (uint32_t)type; b->image = image; b->ptr = 0;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = b->size; ai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(gDev, &ai, 0, &b->mem) != VK_SUCCESS) {
        // (sin lugar para un bloque entero: uno justo)
        b->size = r.size; ai.allocationSize = b->size;
        if (vkAllocateMemory(gDev, &ai, 0, &b->mem) != VK_SUCCESS) { delete b; return false; }
    }
    if (gMemProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* p = 0; vkMapMemory(gDev, b->mem, 0, VK_WHOLE_SIZE, 0, &p); b->ptr = (unsigned char*)p;
    }
    if (r.size < b->size) b->free[r.size] = b->size - r.size;
    gBlocks.push_back(b);
    a.b = b; a.off = 0; a.size = r.size; a.ptr = b->ptr;
    return true;
}
static void Free(Assign& a) {
    if (!a.b) return;
    std::map<VkDeviceSize, VkDeviceSize>& l = a.b->free;
    VkDeviceSize off = a.off, size = a.size;
    std::map<VkDeviceSize, VkDeviceSize>::iterator next = l.lower_bound(off);
    if (next != l.end() && next->first == off + size) { size += next->second; l.erase(next); }
    std::map<VkDeviceSize, VkDeviceSize>::iterator ant = l.lower_bound(off);
    if (ant != l.begin()) { --ant; if (ant->first + ant->second == off) { off = ant->first; size += ant->second; l.erase(ant); } }
    l[off] = size;
    a.b = 0;
}

struct BufVk { VkBuffer b; Assign a; VkDeviceSize size; };
static bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags use, VkMemoryPropertyFlags props, BufVk& out) {
    memset(&out, 0, sizeof(out));
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size < 16 ? 16 : size; bi.usage = use; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(gDev, &bi, 0, &out.b) != VK_SUCCESS) return false;
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(gDev, out.b, &r);
    if (!Reserve(r, props, false, out.a)) { vkDestroyBuffer(gDev, out.b, 0); out.b = VK_NULL_HANDLE; return false; }
    vkBindBufferMemory(gDev, out.b, out.a.b->mem, out.a.off);
    out.size = bi.size;
    return true;
}

// ---- lo que se borra mientras la GPU puede estar usandolo: se destruye cuando termino el cuadro ----
struct Garbage { VkBuffer buf; VkImage img; VkImageView view; VkFramebuffer fb; VkPipeline pipe; VkShaderModule mod; Assign a; };
static const int kFrames = 2;
struct Chunk { BufVk buf; VkDeviceSize used; };
struct Frame {
    VkCommandBuffer cmd, cmdPre;
    VkFence fence;
    VkSemaphore semAdq;
    bool inFlight;
    std::vector<Chunk> chunks; size_t chunk;
    std::vector<VkDescriptorPool> pools; size_t pool;
    std::vector<Garbage> garbage;
};
static Frame gFrames[kFrames];
static int gCur = 0;
static VkCommandPool gPool = VK_NULL_HANDLE;
static void Throw(const Garbage& b) { gFrames[gCur].garbage.push_back(b); }
static Garbage EmptyGarbage() { Garbage b; memset(&b, 0, sizeof(b)); return b; }
static void ThrowBuffer(BufVk& b) { if (!b.b) return; Garbage x = EmptyGarbage(); x.buf = b.b; x.a = b.a; Throw(x); b.b = VK_NULL_HANDLE; b.a.b = 0; }
static void Destroy(Garbage& b) {
    if (b.pipe) vkDestroyPipeline(gDev, b.pipe, 0);
    if (b.mod) vkDestroyShaderModule(gDev, b.mod, 0);
    if (b.fb) vkDestroyFramebuffer(gDev, b.fb, 0);
    if (b.view) vkDestroyImageView(gDev, b.view, 0);
    if (b.img) vkDestroyImage(gDev, b.img, 0);
    if (b.buf) vkDestroyBuffer(gDev, b.buf, 0);
    Free(b.a);
}
static void FreeGarbage(Frame& c) { for (size_t i = 0; i < c.garbage.size(); i++) Destroy(c.garbage[i]); c.garbage.clear(); }

// ============================================================================
//  EL ANILLO: los datos de cada draw del cuadro (atributos, uniforms, las copias a texturas y buffers, las lecturas)
// ============================================================================
struct Ring { VkBuffer b; VkDeviceSize off; unsigned char* p; };
static const VkDeviceSize kChunk = 8ull << 20;
static unsigned gEpoch = 1;   // sube cada vez que un anillo se reinicia: lo subido antes ya no esta
static VkDeviceSize gAlignUBO = 256;
static Ring Request(VkDeviceSize size, VkDeviceSize align) {
    Frame& c = gFrames[gCur];
    if (size == 0) size = 4;
    for (;;) {
        if (c.chunk < c.chunks.size()) {
            Chunk& t = c.chunks[c.chunk];
            const VkDeviceSize off = Align(t.used, align);
            if (off + size <= t.buf.size) { t.used = off + size; Ring r = { t.buf.b, off, t.buf.a.ptr + off }; return r; }
            c.chunk++;
            continue;
        }
        Chunk t; t.used = 0;
        const VkDeviceSize n = size > kChunk ? Align(size, 1 << 20) : kChunk;
        if (!CreateBuffer(n, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, t.buf) || !t.buf.a.ptr) {
            w3dLogfE("[vk] sin memoria para el anillo (%u KB)", (unsigned)(n >> 10));
            static std::vector<unsigned char> garbage;   // (para que quien copie no escriba afuera; el draw no sale)
            if (garbage.size() < size) garbage.resize((size_t)size);
            Ring r = { VK_NULL_HANDLE, 0, &garbage[0] }; return r;
        }
        c.chunks.push_back(t);
        c.chunk = c.chunks.size() - 1;
    }
}
static VkDeviceSize UsedRing() {
    const Frame& c = gFrames[gCur]; VkDeviceSize n = 0;
    for (size_t i = 0; i < c.chunks.size() && i <= c.chunk; i++) n += c.chunks[i].used;
    return n;
}

// ============================================================================
//  Grabar: el command buffer del cuadro (y el de ANTES: las subidas de lo nuevo, que van antes que todo lo dibujado)
// ============================================================================
static bool gRecording = false, gPreRecording = false, gInPass = false;
static unsigned gSerialCmd = 1;   // cambia con cada command buffer: una textura "usada en lo grabado" tiene este numero
static VkRenderPass gRpDef = VK_NULL_HANDLE, gRpColor = VK_NULL_HANDLE;
static VkDescriptorSetLayout gSetLayout = VK_NULL_HANDLE;
static VkPipelineLayout gLayout = VK_NULL_HANDLE;
static VkPipelineCache gPipeCache = VK_NULL_HANDLE;
// lo que el command buffer ya tiene puesto (no repetirlo en cada draw)
struct StateCmd { VkPipeline pipe; VkViewport vp; VkRect2D sc; float line, biasC, biasP; uint32_t sCmp, sWr, sRef; bool ok; };
static StateCmd gEc;
static void ForgetStateCmd() { memset(&gEc, 0, sizeof(gEc)); }
static void Begin(VkCommandBuffer cb) {
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
}
static VkCommandBuffer Cmd() {
    Frame& c = gFrames[gCur];
    if (!gRecording) { Begin(c.cmd); gRecording = true; gSerialCmd++; ForgetStateCmd(); }
    return c.cmd;
}
static VkCommandBuffer CmdPre() {
    Frame& c = gFrames[gCur];
    if (!gPreRecording) { Begin(c.cmdPre); gPreRecording = true; }
    return c.cmdPre;
}
// barrera de imagen (gruesa: todo lo de antes contra todo lo de despues; las copias son pocas)
static void Barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp, VkImageLayout of, VkImageLayout a,
                    uint32_t level0 = 0, uint32_t levels = VK_REMAINING_MIP_LEVELS) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = of; b.newLayout = a;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = asp; b.subresourceRange.baseMipLevel = level0; b.subresourceRange.levelCount = levels;
    b.subresourceRange.baseArrayLayer = 0; b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}
static void BarrierMemory(VkCommandBuffer cb) {
    VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, 0, 0, 0);
}

// ============================================================================
//  Estado de GL
// ============================================================================
// (16 unidades y 16 samplers por programa: el minimo que garantiza Vulkan. Los mapas del material -normal, rugosidad,
//  metalico, alfa, emision- van en las unidades 6..10)
static const int kAttrs = 8, kUnits = 16, kSamples = 16;
struct StateGL {
    bool blend, depth, cull, scissor, stencil, offset;
    GLenum bSrc, bDst, bEq, dFunc; bool dMask; bool cMask[4];
    GLenum sFunc; GLint sRef; GLuint sMaskR, sMaskW; GLenum sFail, sZFail, sZPass;
    float clear[4]; GLint clearS; float clearZ;
    GLint vp[4], sc[4]; GLenum front, cullMode; float line, offF, offU, zn, zf;
    GLint unpack, pack;
};
static StateGL g;
static void FactoryState(int w, int h) {
    memset(&g, 0, sizeof(g));
    g.bSrc = GL_ONE; g.bDst = GL_ZERO; g.bEq = GL_FUNC_ADD; g.dFunc = GL_LESS; g.dMask = true;
    for (int i = 0; i < 4; i++) g.cMask[i] = true;
    g.sFunc = GL_ALWAYS; g.sMaskR = g.sMaskW = 0xFFFFFFFFu; g.sFail = g.sZFail = g.sZPass = GL_KEEP;
    g.clearZ = 1.0f;
    g.vp[2] = g.sc[2] = w; g.vp[3] = g.sc[3] = h;
    g.front = GL_CCW; g.cullMode = GL_BACK; g.line = 1.0f; g.zf = 1.0f;
    g.unpack = g.pack = 4;
}

// ---- texturas ----
struct TexGL {
    VkImage img; Assign a; VkImageView view, view0; VkFramebuffer fb;
    int w, h, levels, levelsDef; bool has, started;
    bool prof;   // una imagen de PROFUNDIDAD (vkglCopyDepth), no de color
    GLint minF, magF, wrapS, wrapT; float aniso; bool genMip;
    unsigned useCmd;
};
static std::map<GLuint, TexGL*> gTex;
static GLuint gNextTex = 1;
static GLuint gUnit[kUnits];
static int gActiveUnit = 0;
static TexGL* gBlack = 0;   // la de una unidad vacia (GL muestrea negro)
static std::map<uint32_t, VkSampler> gSamplers;

// ---- buffers ----
struct BufGL {
    size_t size; bool isStatic, elements; BufVk vk;
    std::vector<unsigned char> copy;
    Ring uploaded; unsigned epoch;
    std::map<uint64_t, BufVk> conv;   // (los formatos que la GPU no lee, pasados a float)
};
static std::map<GLuint, BufGL*> gBufs;
static GLuint gNextBuf = 1, gBufArray = 0, gBufElem = 0;
static bool gSaveCopies = false;   // la GPU no lee algun formato de atributo: los buffers guardan su copia

struct AttrGL { bool on; GLint size; GLenum type; GLboolean norm; GLsizei stride; const void* ptr; GLuint buf; float cte[4]; };
static AttrGL gAttr[kAttrs];

// ---- framebuffers ----
static std::map<GLuint, GLuint> gFbos;   // fbo -> textura
static GLuint gNextFbo = 1, gCurrentFbo = 0;
// ---- renderbuffers de PROFUNDIDAD (glRenderbufferStorage): un fbo con uno usa el render pass de la pantalla (color +
// profundidad), asi los pipelines son los mismos. Las sombras en tiempo real dibujan los objetos desde la luz con z-buffer
struct RbGL { VkImage img; Assign a; VkImageView v; int w, h; };
static std::map<GLuint, RbGL*> gRbs;
static GLuint gNextRb = 1, gCurrentRb = 0;
static std::map<GLuint, GLuint> gFboProf;   // fbo -> renderbuffer de profundidad
// el framebuffer de Vulkan de un fbo con profundidad (la vista de su textura + la del renderbuffer)
struct FbProf { VkFramebuffer fb; VkImageView vc, vp; };
static std::map<GLuint, FbProf> gFbProfVk;
static VkFramebuffer FbWithProf(GLuint fbo, TexGL* t, RbGL* rb);

// ---- la pantalla (la imagen propia) y la ventana ----
struct Screen { VkImage color, prof; Assign ac, ap; VkImageView vc, vp; VkFramebuffer fb; int w, h; };
static Screen gPan;
static VkSwapchainKHR gSc = VK_NULL_HANDLE;
static std::vector<VkImage> gScImgs;
static std::vector<VkSemaphore> gScSem;
static VkExtent2D gScSize = { 0, 0 };
static VkFormat gScFmt = VK_FORMAT_UNDEFINED;
static bool gScRedo    = false;
static bool gScBlit = true;

// ============================================================================
//  El destino del dibujo: la pantalla o una textura (glBindFramebuffer)
// ============================================================================
struct Destination { VkFramebuffer fb; VkRenderPass rp; int w, h; bool flipped, withProf; TexGL* tex; VkImage img; };
static Destination gPass;   // el del pase abierto
static TexGL* TexOf(GLuint id) { std::map<GLuint, TexGL*>::iterator it = gTex.find(id); return it == gTex.end() ? 0 : it->second; }
static bool FboOfTexture(TexGL* t) {
    if (t->fb) return true;
    if (!t->has) return false;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kFmtColor;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    if (!t->view0 && vkCreateImageView(gDev, &vi, 0, &t->view0) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = gRpColor; fi.attachmentCount = 1; fi.pAttachments = &t->view0;
    fi.width = (uint32_t)t->w; fi.height = (uint32_t)t->h; fi.layers = 1;
    return vkCreateFramebuffer(gDev, &fi, 0, &t->fb) == VK_SUCCESS;
}
static Destination CurrentDestination() {
    Destination d; memset(&d, 0, sizeof(d));
    if (gCurrentFbo) {
        std::map<GLuint, GLuint>::iterator it = gFbos.find(gCurrentFbo);
        TexGL* t = it != gFbos.end() ? TexOf(it->second) : 0;
        if (t && FboOfTexture(t)) {
            d.fb = t->fb; d.rp = gRpColor; d.w = t->w; d.h = t->h; d.flipped = false; d.withProf = false; d.tex = t; d.img = t->img;
            // con un renderbuffer de profundidad del mismo tamanio: el render pass de la pantalla (color + profundidad)
            std::map<GLuint, GLuint>::iterator ip = gFboProf.find(gCurrentFbo);
            std::map<GLuint, RbGL*>::iterator go = ip != gFboProf.end() ? gRbs.find(ip->second) : gRbs.end();
            if (go != gRbs.end() && go->second->img && go->second->w == t->w && go->second->h == t->h) {
                VkFramebuffer fb = FbWithProf(gCurrentFbo, t, go->second);
                if (fb) { d.fb = fb; d.rp = gRpDef; d.withProf = true; }
            }
            return d;
        }
    }
    d.fb = gPan.fb; d.rp = gRpDef; d.w = gPan.w; d.h = gPan.h; d.flipped = true; d.withProf = true; d.img = gPan.color;
    return d;
}
static void FinishPass() {
    if (!gInPass) return;
    VkCommandBuffer cb = Cmd();
    vkCmdEndRenderPass(cb);
    if (gPass.tex) Barrier(cb, gPass.tex->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    gInPass = false;
}
static bool BeginPass() {
    if (gInPass) return true;
    Destination d = CurrentDestination();
    if (!d.fb || d.w <= 0 || d.h <= 0) return false;
    VkCommandBuffer cb = Cmd();
    if (d.tex) {
        Barrier(cb, d.tex->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
        d.tex->useCmd = gSerialCmd;
    }
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = d.rp; bi.framebuffer = d.fb;
    bi.renderArea.extent.width = (uint32_t)d.w; bi.renderArea.extent.height = (uint32_t)d.h;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    gPass = d; gInPass = true;
    ForgetStateCmd();
    return true;
}

// ============================================================================
//  ENVIAR: el command buffer a la GPU. SendAndWait + Reciclar a mitad de cuadro (lecturas, glFinish, anillo lleno)
// ============================================================================
static bool Send(VkSemaphore wait, VkSemaphore senial) {
    Frame& c = gFrames[gCur];
    FinishPass();
    VkCommandBuffer cbs[2]; uint32_t n = 0;
    if (gPreRecording) { BarrierMemory(c.cmdPre); vkEndCommandBuffer(c.cmdPre); cbs[n++] = c.cmdPre; gPreRecording = false; }
    if (gRecording) { vkEndCommandBuffer(c.cmd); cbs[n++] = c.cmd; gRecording = false; }
    if (!n && !wait && !senial) return false;
    if (c.inFlight) { vkWaitForFences(gDev, 1, &c.fence, VK_TRUE, UINT64_MAX); c.inFlight = false; }
    vkResetFences(gDev, 1, &c.fence);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo yes = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    yes.waitSemaphoreCount = wait ? 1 : 0; yes.pWaitSemaphores = &wait; yes.pWaitDstStageMask = &stage;
    yes.commandBufferCount = n; yes.pCommandBuffers = cbs;
    yes.signalSemaphoreCount = senial ? 1 : 0; yes.pSignalSemaphores = &senial;
    const VkResult r = vkQueueSubmit(gQueue, 1, &yes, c.fence);
    if (r != VK_SUCCESS) { w3dLogfE("[vk] vkQueueSubmit: %d", (int)r); return false; }
    c.inFlight = true;
    return true;
}
static std::map<std::string, VkDescriptorSet> gCacheSets;
static void Restart(Frame& c) {
    FreeGarbage(c);
    for (size_t i = 0; i < c.chunks.size(); i++) c.chunks[i].used = 0;
    c.chunk = 0;
    for (size_t i = 0; i < c.pools.size(); i++) vkResetDescriptorPool(gDev, c.pools[i], 0);
    c.pool = 0;
    gEpoch++;
    gCacheSets.clear();
}
static void SendAndWait() {
    Send(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkQueueWaitIdle(gQueue);
}
// (despues de SendAndWait: la GPU no tiene nada; todo lo de los dos cuadros se puede volver a usar)
static void RecycleAll() {
    for (int i = 0; i < kFrames; i++) { if (gFrames[i].inFlight) { vkWaitForFences(gDev, 1, &gFrames[i].fence, VK_TRUE, UINT64_MAX); gFrames[i].inFlight = false; } Restart(gFrames[i]); }
}

// ============================================================================
//  Texturas
// ============================================================================
static int LevelsOf(int w, int h) { int n = 1, m = w > h ? w : h; while (m > 1) { m >>= 1; n++; } return n; }
static void ThrowImageTex(TexGL* t) {
    if (!t->has) return;
    Garbage b = EmptyGarbage(); b.img = t->img; b.view = t->view; b.a = t->a; Throw(b);
    if (t->view0 || t->fb) { Garbage c = EmptyGarbage(); c.view = t->view0; c.fb = t->fb; Throw(c); }
    t->img = VK_NULL_HANDLE; t->view = t->view0 = VK_NULL_HANDLE; t->fb = VK_NULL_HANDLE; t->a.b = 0;
    t->has = false; t->started = false;
}
static bool RedoView(TexGL* t) {
    if (t->view) { Garbage b = EmptyGarbage(); b.view = t->view; Throw(b); t->view = VK_NULL_HANDLE; }
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kFmtColor;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = (uint32_t)(t->levelsDef > 0 ? t->levelsDef : 1); vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(gDev, &vi, 0, &t->view) == VK_SUCCESS;
}
static bool CreateImageTex(TexGL* t, int w, int h) {
    ThrowImageTex(t);
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = kFmtColor;
    ii.extent.width = (uint32_t)w; ii.extent.height = (uint32_t)h; ii.extent.depth = 1;
    ii.mipLevels = (uint32_t)LevelsOf(w, h); ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(gDev, &ii, 0, &t->img) != VK_SUCCESS) return false;
    VkMemoryRequirements r; vkGetImageMemoryRequirements(gDev, t->img, &r);
    if (!Reserve(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, t->a)) { vkDestroyImage(gDev, t->img, 0); t->img = VK_NULL_HANDLE; return false; }
    vkBindImageMemory(gDev, t->img, t->a.b->mem, t->a.off);
    t->w = w; t->h = h; t->levels = (int)ii.mipLevels; t->levelsDef = 1; t->has = true; t->started = false; t->useCmd = 0;
    return RedoView(t);
}
// las copias a una textura: en orden con lo dibujado si ya se uso en lo grabado; si no, antes de todo el cuadro
static VkCommandBuffer CmdCopyA(TexGL* t) {
    if (gRecording && t->useCmd == gSerialCmd) { FinishPass(); return Cmd(); }
    return CmdPre();
}
// la textura lista para copiarle: en TRANSFER_DST (la primera vez, en negro transparente)
static void TexOpenCopy(VkCommandBuffer cb, TexGL* t) {
    Barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, t->started ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (!t->started) {
        VkClearColorValue zero; memset(&zero, 0, sizeof(zero));
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
        vkCmdClearColorImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rr);
        t->started = true;
    }
}
static void TexCloseCopy(VkCommandBuffer cb, TexGL* t) {
    Barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
static int BppOf(GLenum fmt, GLenum type) {
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) return 2;
    switch (fmt) { case GL_RGBA: case GL_BGRA: return 4; case GL_RGB: return 3; case GL_LUMINANCE_ALPHA: return 2; default: return 1; }
}
// los pixeles de GL (cualquier formato del motor) a RGBA 8
static void ARGBA8(GLenum fmt, GLenum type, int w, int h, const unsigned char* s0, int align, unsigned char* d) {
    const int bpp = BppOf(fmt, type);
    const size_t row = (size_t)Align((VkDeviceSize)w * bpp, align > 0 ? align : 1);
    if (type == GL_UNSIGNED_BYTE && fmt == GL_RGBA && row == (size_t)w * 4) { memcpy(d, s0, (size_t)w * h * 4); return; }
    for (int y = 0; y < h; y++) {
        const unsigned char* s = s0 + row * y;
        for (int x = 0; x < w; x++, d += 4) {
            if (type == GL_UNSIGNED_BYTE) {
                const unsigned char* p = s + x * bpp;
                switch (fmt) {
                    case GL_RGBA: d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3]; break;
                    case GL_BGRA: d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = p[3]; break;
                    case GL_RGB: d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 255; break;
                    case GL_LUMINANCE_ALPHA: d[0] = d[1] = d[2] = p[0]; d[3] = p[1]; break;
                    case GL_ALPHA: d[0] = d[1] = d[2] = 0; d[3] = p[0]; break;
                    default: d[0] = d[1] = d[2] = p[0]; d[3] = 255; break;   // GL_LUMINANCE
                }
            } else {
                unsigned v; memcpy(&v, s + x * 2, 2); v &= 0xFFFF;
                if (type == GL_UNSIGNED_SHORT_5_6_5) {
                    d[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31); d[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
                    d[2] = (unsigned char)((v & 31) * 255 / 31); d[3] = 255;
                } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
                    d[0] = (unsigned char)(((v >> 12) & 15) * 17); d[1] = (unsigned char)(((v >> 8) & 15) * 17);
                    d[2] = (unsigned char)(((v >> 4) & 15) * 17); d[3] = (unsigned char)((v & 15) * 17);
                } else {
                    d[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31); d[1] = (unsigned char)(((v >> 6) & 31) * 255 / 31);
                    d[2] = (unsigned char)(((v >> 1) & 31) * 255 / 31); d[3] = (v & 1) ? 255 : 0;
                }
            }
        }
    }
}
static void Upload(TexGL* t, int level, int x, int y, int w, int h, GLenum fmt, GLenum type, const void* data) {
    VkCommandBuffer cb = CmdCopyA(t);
    TexOpenCopy(cb, t);
    if (data && w > 0 && h > 0) {
        Ring r = Request((VkDeviceSize)w * h * 4, 16);
        if (r.b) {
            ARGBA8(fmt, type, w, h, (const unsigned char*)data, g.unpack, r.p);
            VkBufferImageCopy c; memset(&c, 0, sizeof(c));
            c.bufferOffset = r.off;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.layerCount = 1;
            c.imageOffset.x = x; c.imageOffset.y = y; c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cb, r.b, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
    }
    TexCloseCopy(cb, t);
}
static void GenerateMips(TexGL* t) {
    if (!t->has || t->levels < 2) return;
    VkCommandBuffer cb = CmdCopyA(t);
    TexOpenCopy(cb, t);
    for (int i = 1; i < t->levels; i++) {
        Barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (uint32_t)i - 1, 1);
        VkImageBlit b; memset(&b, 0, sizeof(b));
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.mipLevel = (uint32_t)i - 1; b.dstSubresource.mipLevel = (uint32_t)i;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        const int sw = t->w >> (i - 1), sh = t->h >> (i - 1), dw = t->w >> i, dh = t->h >> i;
        b.srcOffsets[1].x = sw > 0 ? sw : 1; b.srcOffsets[1].y = sh > 0 ? sh : 1; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = dw > 0 ? dw : 1; b.dstOffsets[1].y = dh > 0 ? dh : 1; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    Barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, (uint32_t)t->levels - 1);
    Barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, (uint32_t)t->levels - 1, 1);
    if (t->levelsDef != t->levels) { t->levelsDef = t->levels; RedoView(t); }
}
static VkSampler SamplerOf(const TexGL* t) {
    const GLint mn = t->minF, mg = t->magF;
    const int fm = (mn == GL_NEAREST) ? 0 : (mn == GL_LINEAR) ? 1 : (mn == GL_NEAREST_MIPMAP_NEAREST) ? 2 : (mn == GL_LINEAR_MIPMAP_NEAREST) ? 3
                 : (mn == GL_NEAREST_MIPMAP_LINEAR) ? 4 : 5;
    int an = (int)(t->aniso + 0.5f); if (an < 1) an = 1; if (an > 16) an = 16;
    if (!gFeat.samplerAnisotropy) an = 1;
    // SIN FILTRADO (GL_NEAREST de reduccion: la UI pixel perfect) no hay anisotropico: en Vulkan un sampler con la
    // anisotropia prendida filtra LINEAL aunque diga NEAREST, y el atlas de la interfaz (subido con filtro y mipmaps, y
    // por eso con anisotropia) salia BORROSO al agrandarlo (escala 2 o 3). GL la ignora con NEAREST
    if (mn == GL_NEAREST || mn == GL_NEAREST_MIPMAP_NEAREST || mn == GL_NEAREST_MIPMAP_LINEAR) an = 1;
    const int ws = t->wrapS == GL_REPEAT ? 0 : t->wrapS == GL_MIRRORED_REPEAT ? 1 : 2, wt = t->wrapT == GL_REPEAT ? 0 : t->wrapT == GL_MIRRORED_REPEAT ? 1 : 2;
    const uint32_t key = (uint32_t)fm | ((mg == GL_NEAREST ? 0u : 1u) << 3) | ((uint32_t)ws << 4) | ((uint32_t)wt << 6) | ((uint32_t)an << 8);
    std::map<uint32_t, VkSampler>::iterator it = gSamplers.find(key);
    if (it != gSamplers.end()) return it->second;
    static const VkSamplerAddressMode kWrap[3] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSamplerCreateInfo yes = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    yes.magFilter = mg == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    yes.minFilter = (fm == 0 || fm == 2 || fm == 4) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    yes.mipmapMode = (fm == 4 || fm == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    yes.addressModeU = kWrap[ws]; yes.addressModeV = kWrap[wt]; yes.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    yes.anisotropyEnable = an > 1 ? VK_TRUE : VK_FALSE;
    yes.maxAnisotropy = (float)an > gProps.limits.maxSamplerAnisotropy ? gProps.limits.maxSamplerAnisotropy : (float)an;
    yes.minLod = 0.0f; yes.maxLod = fm < 2 ? 0.25f : VK_LOD_CLAMP_NONE;   // (sin mipmaps: solo el nivel 0, como GL)
    yes.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(gDev, &yes, 0, &s);
    gSamplers[key] = s;
    return s;
}
static TexGL* NewTex() {
    TexGL* t = new TexGL(); memset(t, 0, sizeof(*t));
    t->minF = GL_NEAREST_MIPMAP_LINEAR; t->magF = GL_LINEAR; t->wrapS = t->wrapT = GL_REPEAT; t->aniso = 1.0f;
    return t;
}
static TexGL* CurrentTex() {
    const GLuint id = gUnit[gActiveUnit];
    if (!id) return 0;
    TexGL*& t = gTex[id];
    if (!t) t = NewTex();
    return t;
}

// ============================================================================
//  LOS SHADERS: GLSL ES 1.00 -> GLSL 4.50 de Vulkan
// ============================================================================
struct GLSLType { const char* n; int size, align, cols, cls; };   // clase: 0 float, 1 int, 2 bool, 3 matriz, 4 sampler
static const GLSLType kTypes[] = {
    { "float", 4, 4, 1, 0 }, { "vec2", 8, 8, 1, 0 }, { "vec3", 12, 16, 1, 0 }, { "vec4", 16, 16, 1, 0 },
    { "int", 4, 4, 1, 1 }, { "ivec2", 8, 8, 1, 1 }, { "ivec3", 12, 16, 1, 1 }, { "ivec4", 16, 16, 1, 1 },
    { "uint", 4, 4, 1, 1 }, { "bool", 4, 4, 1, 2 }, { "bvec2", 8, 8, 1, 2 }, { "bvec3", 12, 16, 1, 2 }, { "bvec4", 16, 16, 1, 2 },
    { "mat2", 32, 16, 2, 3 }, { "mat3", 48, 16, 3, 3 }, { "mat4", 64, 16, 4, 3 },
    { "sampler2D", 0, 0, 1, 4 }, { "samplerCube", 0, 0, 1, 4 },
};
static const GLSLType* TypeOf(const std::string& n) {
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) if (n == kTypes[i].n) return &kTypes[i];
    return 0;
}
// cuantas locations ocupa un varying / atributo
static int LocsOf(const std::string& type) { return type == "mat4" ? 4 : type == "mat3" ? 3 : type == "mat2" ? 2 : 1; }
static inline bool IsIdent(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
// cambiar una PALABRA entera (un identificador) por otra
static std::string ChangeWord(const std::string& s, const char* of, const char* a) {
    std::string o; o.reserve(s.size());
    const size_t n = strlen(of);
    for (size_t i = 0; i < s.size();) {
        if (IsIdent(s[i])) {
            size_t j = i; while (j < s.size() && IsIdent(s[j])) j++;
            if (j - i == n && s.compare(i, n, of) == 0) o += a; else o.append(s, i, j - i);
            i = j;
        } else o += s[i++];
    }
    return o;
}
static std::string Crop(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
struct VarGLSL { std::string cls, type, name; int arr; };
// el texto preprocesado: las declaraciones globales (uniform / attribute / varying) aparte; el resto (las funciones) al cuerpo
static void Parse(const std::string& pre, std::vector<VarGLSL>& vars, std::string& body) {
    // (fuera las directivas que deja el preprocesador: #version, #extension. Los #line SE QUEDAN: con ellos los errores
    //  dicen el renglon del texto que se paso, y el cuerpo conserva los saltos de linea para que la cuenta siga)
    std::string s;
    for (size_t i = 0; i < pre.size();) {
        size_t f = pre.find('\n', i); if (f == std::string::npos) f = pre.size();
        const size_t k = pre.find_first_not_of(" \t\r", i);
        if (!(k < f && pre[k] == '#') || pre.compare(k, 5, "#line") == 0) s.append(pre, i, f - i);
        s += '\n';
        i = f + 1;
    }
    s = ChangeWord(ChangeWord(ChangeWord(s, "lowp", ""), "mediump", ""), "highp", "");
    std::string chunk; int prof = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (prof > 0) { body += c; if (c == '{') prof++; else if (c == '}') --prof; continue; }
        if (c == '#') {   // un #line entre declaraciones: al cuerpo (a mitad de una declaracion no puede ir: se pierde)
            size_t f = s.find('\n', i); if (f == std::string::npos) f = s.size();
            if (Crop(chunk).empty()) { body += chunk; body.append(s, i, f - i); chunk.clear(); }
            i = f - 1;
            continue;
        }
        if (c == '{') { body += chunk; body += c; chunk.clear(); prof = 1; continue; }
        if (c != ';') { chunk += c; continue; }
        const std::string t = Crop(chunk);
        size_t jumps = 0, prev = 0;   // (los renglones de la declaracion: los de adelante y los de adentro)
        { bool full = false; for (size_t k = 0; k < chunk.size(); k++) if (chunk[k] == '\n') { jumps++; if (!full) prev++; } else if (chunk[k] != ' ' && chunk[k] != '\t' && chunk[k] != '\r') full = true; }
        chunk.clear();
        size_t e = 0; while (e < t.size() && IsIdent(t[e])) e++;
        const std::string cls = t.substr(0, e);
        if (cls == "precision") { body.append(jumps, '\n'); continue; }
        if (cls != "uniform" && cls != "attribute" && cls != "varying") { body.append(prev, '\n'); body += t; body += ';'; continue; }
        body.append(jumps, '\n');
        size_t i0 = t.find_first_not_of(" \t\r\n", e), i1 = i0; while (i1 < t.size() && IsIdent(t[i1])) i1++;
        if (i0 == std::string::npos) continue;
        const std::string type = t.substr(i0, i1 - i0);
        std::string rest; for (size_t k = i1; k < t.size(); k++) if (t[k] != ' ' && t[k] != '\t' && t[k] != '\r' && t[k] != '\n') rest += t[k];
        size_t p = 0;
        while (p <= rest.size()) {
            size_t q = rest.find(',', p); if (q == std::string::npos) q = rest.size();
            const std::string d = rest.substr(p, q - p);
            if (!d.empty()) {
                VarGLSL v; v.cls = cls; v.type = type; v.arr = 0;
                const size_t c1 = d.find('[');
                v.name = d.substr(0, c1);
                if (c1 != std::string::npos) v.arr = atoi(d.c_str() + c1 + 1);
                vars.push_back(v);
            }
            p = q + 1;
        }
    }
    body += chunk;
}
static std::string Decl(const VarGLSL& v) {
    char a[32] = ""; if (v.arr > 0) snprintf(a, sizeof(a), "[%d]", v.arr);
    return v.type + " " + v.name + a;
}

struct ShaderGL { GLenum type; std::string source, pre, log; bool ok; };
struct UniGL { std::string name; const GLSLType* t; int off, stride, elems, loc0, sample; };
struct LocGL { int uni, elem; };
struct KeyPipe {
    uint32_t a, b;
    uint8_t fmt[kAttrs];
    uint16_t stride[kAttrs];
    bool operator<(const KeyPipe& o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};
struct ProgGL {
    GLuint vs, fs; std::map<std::string, int> attribRequests;
    bool ok; std::string log;
    VkShaderModule mVS, mFS;
    std::vector<UniGL> unis; std::vector<LocGL> locs;
    std::vector<unsigned char> block; int blockSize, offFit;
    int nSamples; int sampleUni[kSamples];   // el uniform (sampler) de cada binding 1..n
    int unit[kSamples];                           // la unidad de textura que dice cada sampler (glUniform1i)
    std::map<std::string, int> attribs;           // nombre -> location (los del vertex shader)
    unsigned maskAttrib;
    bool dirty; unsigned epochUpload; Ring uploaded;
    std::map<KeyPipe, VkPipeline> pipes;
};
static std::map<GLuint, ShaderGL*> gShaders;
static std::map<GLuint, ProgGL*> gProgs;
static GLuint gNextName = 1;   // (shaders y programas comparten los nombres, como en GL)
static ProgGL* gProg = 0;
static GLuint gProgId = 0;

// "#line N" de GLSL ES 1.00 dice que el renglon SIGUIENTE es el N + 1; en el GLSL 450 con el que se compila aca, que es
// el N. Se corre uno (los errores de un shader escrito dicen el renglon de SU archivo, como en GLES 2.0)
static std::string ESLines(const std::string& src) {
    if (src.find("#line") == std::string::npos) return src;
    std::string o; o.reserve(src.size() + 16);
    for (size_t i = 0; i < src.size();) {
        size_t f = src.find('\n', i); if (f == std::string::npos) f = src.size();
        const size_t k = src.find_first_not_of(" \t\r", i);
        if (k < f && src.compare(k, 5, "#line") == 0) {
            char b[32]; snprintf(b, sizeof(b), "#line %d", atoi(src.c_str() + k + 5) + 1);
            o += b;
        } else o.append(src, i, f - i);
        if (f < src.size()) o += '\n';
        i = f + 1;
    }
    return o;
}
static bool Preprocess(const std::string& src, int type, std::string& out, std::string& log) {
    const std::string s = "#version 450\n#define W3D_VULKAN 1\n" + ESLines(src);
    ShcResult r = shc_compile_into_preprocessed_text(gShc, s.c_str(), s.size(), type, "w3d", "main", gShcOpt);
    const bool ok = r && shc_result_get_compilation_status(r) == 0;
    if (ok) out.assign(shc_result_get_bytes(r), shc_result_get_length(r));
    else log = r ? shc_result_get_error_message(r) : "shaderc";
    if (r) shc_result_release(r);
    return ok;
}
static bool CompileSPV(const std::string& src, int type, VkShaderModule& mod, std::string& log) {
    ShcResult r = shc_compile_into_spv(gShc, src.c_str(), src.size(), type, type == kShcVertex ? "w3d.vert" : "w3d.frag", "main", gShcOpt);
    const bool ok = r && shc_result_get_compilation_status(r) == 0;
    if (!ok) log = r ? shc_result_get_error_message(r) : "shaderc";
    else {
        VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mi.codeSize = shc_result_get_length(r); mi.pCode = (const uint32_t*)shc_result_get_bytes(r);
        if (vkCreateShaderModule(gDev, &mi, 0, &mod) != VK_SUCCESS) { log = "vkCreateShaderModule"; if (r) shc_result_release(r); return false; }
    }
    if (r) shc_result_release(r);
    return ok;
}
static std::string FixBody(const std::string& c, bool vs) {
    std::string s = ChangeWord(ChangeWord(ChangeWord(c, "texture2D", "texture"), "texture2DLod", "textureLod"), "texture2DProj", "textureProj");
    s = ChangeWord(s, "textureCube", "texture");
    if (vs) s = ChangeWord(s, "main", "w3d_main_");
    else s = ChangeWord(ChangeWord(s, "gl_FragColor", "w3d_Color"), "gl_FragCoord", "w3dFragCoord()");
    return s;
}
// LINKEAR: los dos shaders traducidos, con el MISMO bloque de uniforms (el de los dos juntos) y los varyings en las
// mismas locations
static bool Link(ProgGL* p) {
    ShaderGL* vs = gShaders.count(p->vs) ? gShaders[p->vs] : 0;
    ShaderGL* fs = gShaders.count(p->fs) ? gShaders[p->fs] : 0;
    if (!vs || !fs || !vs->ok || !fs->ok) { p->log = "falta un shader (o no compilo)"; return false; }
    std::vector<VarGLSL> vv, vf; std::string cv, cf;
    Parse(vs->pre, vv, cv); Parse(fs->pre, vf, cf);
    // los uniforms de los dos (cada nombre una vez) y los samplers
    std::vector<VarGLSL> unis, samps;
    for (int k = 0; k < 2; k++) {
        const std::vector<VarGLSL>& l = k ? vf : vv;
        for (size_t i = 0; i < l.size(); i++) {
            if (l[i].cls != "uniform") continue;
            std::vector<VarGLSL>& dst = (TypeOf(l[i].type) && TypeOf(l[i].type)->cls == 4) ? samps : unis;
            bool already = false; for (size_t j = 0; j < dst.size(); j++) if (dst[j].name == l[i].name) already = true;
            if (!already) dst.push_back(l[i]);
        }
    }
    if ((int)samps.size() > kSamples) { p->log = "demasiados samplers"; return false; }
    // el bloque std140
    p->unis.clear(); p->locs.clear();
    int off = 0;
    std::string block = "layout(std140, set=0, binding=0) uniform W3dBlock {\n";
    for (size_t i = 0; i < unis.size(); i++) {
        const GLSLType* t = TypeOf(unis[i].type);
        if (!t) { p->log = "tipo de uniform desconocido: " + unis[i].type; return false; }
        UniGL u; u.name = unis[i].name; u.t = t; u.sample = -1;
        u.elems = unis[i].arr > 0 ? unis[i].arr : 1;
        u.stride = unis[i].arr > 0 ? (int)Align((VkDeviceSize)t->size, 16) : t->size;
        off = (int)Align((VkDeviceSize)off, unis[i].arr > 0 ? 16 : (VkDeviceSize)t->align);
        u.off = off; off += u.stride * u.elems;
        u.loc0 = (int)p->locs.size();
        for (int e = 0; e < u.elems; e++) { LocGL l = { (int)p->unis.size(), e }; p->locs.push_back(l); }
        p->unis.push_back(u);
        block += "  " + Decl(unis[i]) + ";\n";
    }
    off = (int)Align((VkDeviceSize)off, 16); p->offFit = off; off += 16;
    p->blockSize = (int)Align((VkDeviceSize)off, 16);
    block += "  vec4 w3d_Fit;\n};\n";   // (gl_FragCoord: x = destino dado vuelta, y = su alto)
    p->block.assign((size_t)p->blockSize, 0);
    p->nSamples = (int)samps.size();
    for (size_t i = 0; i < samps.size(); i++) {
        char l[160]; snprintf(l, sizeof(l), "layout(set=0, binding=%d) uniform %s %s;\n", (int)i + 1, samps[i].type.c_str(), samps[i].name.c_str());
        block += l;
        UniGL u; u.name = samps[i].name; u.t = TypeOf(samps[i].type); u.off = 0; u.stride = 0; u.elems = 1; u.sample = (int)i;
        u.loc0 = (int)p->locs.size();
        LocGL lg = { (int)p->unis.size(), 0 }; p->locs.push_back(lg);
        p->sampleUni[i] = (int)p->unis.size(); p->unit[i] = 0;
        p->unis.push_back(u);
    }
    // VERTEX: los atributos en sus lugares (glBindAttribLocation, o el primero libre) y los varyings en orden
    std::string ov = "#version 450\n" + block, of = "#version 450\n" + block;
    p->attribs.clear(); p->maskAttrib = 0;
    unsigned busy = 0;
    for (std::map<std::string, int>::iterator it = p->attribRequests.begin(); it != p->attribRequests.end(); ++it) busy |= 1u << it->second;
    std::map<std::string, int> locVar; int nextVar = 0;
    for (size_t i = 0; i < vv.size(); i++) {
        char l[160];
        if (vv[i].cls == "attribute") {
            int loc = -1;
            if (p->attribRequests.count(vv[i].name)) loc = p->attribRequests[vv[i].name];
            else { for (int k = 0; k < kAttrs; k++) if (!(busy & (1u << k))) { loc = k; break; } }
            if (loc < 0 || loc >= kAttrs) { p->log = "sin lugar para el atributo " + vv[i].name; return false; }
            busy |= 1u << loc;
            p->attribs[vv[i].name] = loc; p->maskAttrib |= 1u << loc;
            snprintf(l, sizeof(l), "layout(location=%d) in %s;\n", loc, Decl(vv[i]).c_str());
            ov += l;
        } else if (vv[i].cls == "varying") {
            locVar[vv[i].name] = nextVar;
            snprintf(l, sizeof(l), "layout(location=%d) out %s;\n", nextVar, Decl(vv[i]).c_str());
            ov += l;
            nextVar += LocsOf(vv[i].type) * (vv[i].arr > 0 ? vv[i].arr : 1);
        }
    }
    for (size_t i = 0; i < vf.size(); i++) {
        if (vf[i].cls != "varying") continue;
        int loc;
        if (locVar.count(vf[i].name)) loc = locVar[vf[i].name];
        else { loc = nextVar; nextVar += LocsOf(vf[i].type) * (vf[i].arr > 0 ? vf[i].arr : 1); }
        char l[160]; snprintf(l, sizeof(l), "layout(location=%d) in %s;\n", loc, Decl(vf[i]).c_str());
        of += l;
    }
    ov += FixBody(cv, true);
    ov += "\nvoid main(){ w3d_main_(); gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; }\n";   // (la z de GL, -1..1, a 0..1)
    of += "layout(location=0) out vec4 w3d_Color;\n"
          "vec4 w3dFragCoord(){ return vec4(gl_FragCoord.x, w3d_Fit.x > 0.5 ? w3d_Fit.y - gl_FragCoord.y : gl_FragCoord.y, gl_FragCoord.zw); }\n";
    of += FixBody(cf, false);
    std::string log;
    if (!CompileSPV(ov, kShcVertex, p->mVS, log)) { p->log = "vertex: " + log; if (getenv("W3D_VK_SHADERS")) w3dLogf("%s", ov.c_str()); return false; }
    if (!CompileSPV(of, kShcFragment, p->mFS, log)) { p->log = "fragment: " + log; if (getenv("W3D_VK_SHADERS")) w3dLogf("%s", of.c_str()); return false; }
    p->dirty = true; p->epochUpload = 0;
    return true;
}
static void DeleteModulesAndPipes(ProgGL* p) {
    for (std::map<KeyPipe, VkPipeline>::iterator it = p->pipes.begin(); it != p->pipes.end(); ++it) { Garbage b = EmptyGarbage(); b.pipe = it->second; Throw(b); }
    p->pipes.clear();
    if (p->mVS) { Garbage b = EmptyGarbage(); b.mod = p->mVS; Throw(b); p->mVS = VK_NULL_HANDLE; }
    if (p->mFS) { Garbage b = EmptyGarbage(); b.mod = p->mFS; Throw(b); p->mFS = VK_NULL_HANDLE; }
}

// ============================================================================
//  PIPELINES: uno por combinacion de estado (la clave), creado al primer draw que la usa
// ============================================================================
static VkBlendFactor FactorVk(GLenum f) {
    switch (f) {
        case GL_ZERO: return VK_BLEND_FACTOR_ZERO; case GL_ONE: return VK_BLEND_FACTOR_ONE;
        case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_ONE;
}
static uint32_t OpStencil(GLenum o) {
    switch (o) {
        case GL_ZERO: return VK_STENCIL_OP_ZERO; case GL_REPLACE: return VK_STENCIL_OP_REPLACE;
        case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case GL_INVERT: return VK_STENCIL_OP_INVERT; case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}
static uint32_t TopologyVk(GLenum m) {
    switch (m) {
        case GL_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case GL_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case GL_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; case GL_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case GL_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}
static uint32_t EqVk(GLenum e) {
    switch (e) { case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT; case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
                 case GL_MIN: return VK_BLEND_OP_MIN; case GL_MAX: return VK_BLEND_OP_MAX; }
    return VK_BLEND_OP_ADD;
}
static void StateKey(GLenum mode, KeyPipe& k) {
    const bool tri = mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN;
    uint32_t cull = 0;
    if (g.cull && tri) cull = g.cullMode == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : g.cullMode == GL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_AND_BACK;
    // (dado vuelta, la pantalla deja el giro como en GL; una textura destino lo invierte)
    const bool ccw = (g.front == GL_CCW) == gPass.flipped;
    const bool prof = gPass.withProf;
    const uint32_t cm = (g.cMask[0] ? 1u : 0u) | (g.cMask[1] ? 2u : 0u) | (g.cMask[2] ? 4u : 0u) | (g.cMask[3] ? 8u : 0u);
    k.a = TopologyVk(mode) | (cull << 3) | ((ccw ? 1u : 0u) << 5) | (((prof && g.depth) ? 1u : 0u) << 6)
        | (((prof && g.depth && g.dMask) ? 1u : 0u) << 7) | (((uint32_t)(g.dFunc - GL_NEVER) & 7u) << 8) | ((g.blend ? 1u : 0u) << 11)
        | ((g.blend ? (uint32_t)FactorVk(g.bSrc) : 0u) << 12) | ((g.blend ? (uint32_t)FactorVk(g.bDst) : 0u) << 16)
        | ((g.blend ? EqVk(g.bEq) : 0u) << 20) | (cm << 23) | (((prof && g.stencil) ? 1u : 0u) << 27)
        | (((g.offset && tri) ? 1u : 0u) << 28) | ((gPass.rp == gRpColor ? 1u : 0u) << 29);
    k.b = (prof && g.stencil) ? (((uint32_t)(g.sFunc - GL_NEVER) & 7u) | (OpStencil(g.sFail) << 3) | (OpStencil(g.sZFail) << 6) | (OpStencil(g.sZPass) << 9)) : 0u;
}
static VkPipeline CreatePipe(ProgGL* p, const KeyPipe& k) {
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof(st));
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = p->mVS; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = p->mFS; st[1].pName = "main";
    VkVertexInputBindingDescription vb[kAttrs]; VkVertexInputAttributeDescription va[kAttrs]; uint32_t nv = 0;
    for (int i = 0; i < kAttrs; i++) {
        if (!(p->maskAttrib & (1u << i))) continue;
        vb[nv].binding = (uint32_t)i; vb[nv].stride = k.stride[i]; vb[nv].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        va[nv].location = (uint32_t)i; va[nv].binding = (uint32_t)i; va[nv].format = (VkFormat)k.fmt[i]; va[nv].offset = 0;
        nv++;
    }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = nv; vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = nv; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = (VkPrimitiveTopology)(k.a & 7u);
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = (k.a >> 3) & 3u;
    rs.frontFace = ((k.a >> 5) & 1u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = ((k.a >> 28) & 1u) ? VK_TRUE : VK_FALSE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = ((k.a >> 6) & 1u) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = ((k.a >> 7) & 1u) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = (VkCompareOp)((k.a >> 8) & 7u);
    ds.stencilTestEnable = ((k.a >> 27) & 1u) ? VK_TRUE : VK_FALSE;
    ds.front.compareOp = (VkCompareOp)(k.b & 7u); ds.front.failOp = (VkStencilOp)((k.b >> 3) & 7u);
    ds.front.depthFailOp = (VkStencilOp)((k.b >> 6) & 7u); ds.front.passOp = (VkStencilOp)((k.b >> 9) & 7u);
    ds.front.compareMask = ds.front.writeMask = 0xFF; ds.back = ds.front;
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState ba; memset(&ba, 0, sizeof(ba));
    ba.blendEnable = ((k.a >> 11) & 1u) ? VK_TRUE : VK_FALSE;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = (VkBlendFactor)((k.a >> 12) & 15u);
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = (VkBlendFactor)((k.a >> 16) & 15u);
    ba.colorBlendOp = ba.alphaBlendOp = (VkBlendOp)((k.a >> 20) & 7u);
    ba.colorWriteMask = (k.a >> 23) & 15u;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState din[7] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
                              VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 7; dy.pDynamicStates = din;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vps;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy; pi.layout = gLayout; pi.renderPass = ((k.a >> 29) & 1u) ? gRpColor : gRpDef;
    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(gDev, gPipeCache, 1, &pi, 0, &pipe);
    if (r != VK_SUCCESS) w3dLogfE("[vk] vkCreateGraphicsPipelines: %d", (int)r);
    return pipe;
}

// ============================================================================
//  ATRIBUTOS: de que buffer y con que formato los lee cada draw
// ============================================================================
static int BytesType(GLenum t) { return (t == GL_FLOAT || t == GL_INT || t == GL_UNSIGNED_INT) ? 4 : (t == GL_SHORT || t == GL_UNSIGNED_SHORT) ? 2 : 1; }
static VkFormat FormatAttr(GLenum t, int n, bool norm) {
    if (n < 1 || n > 4) return VK_FORMAT_UNDEFINED;
    static const VkFormat kF[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    static const VkFormat kUB[2][4] = { { VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED },
                                         { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM } };
    static const VkFormat kB[2][4] = { { VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED },
                                        { VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM } };
    static const VkFormat kUS[2][4] = { { VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED },
                                         { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM } };
    static const VkFormat kS[2][4] = { { VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED },
                                        { VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM } };
    switch (t) {
        case GL_FLOAT: return kF[n - 1];
        case GL_UNSIGNED_BYTE: return kUB[norm ? 1 : 0][n - 1];
        case GL_BYTE: return kB[norm ? 1 : 0][n - 1];
        case GL_UNSIGNED_SHORT: return kUS[norm ? 1 : 0][n - 1];
        case GL_SHORT: return kS[norm ? 1 : 0][n - 1];
    }
    return VK_FORMAT_UNDEFINED;
}
static std::map<int, bool> gFmtVertex;
static bool FormatReadable(VkFormat f) {
    std::map<int, bool>::iterator it = gFmtVertex.find((int)f);
    if (it != gFmtVertex.end()) return it->second;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(gPhys, f, &fp);
    const bool ok = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    gFmtVertex[(int)f] = ok;
    return ok;
}
static float ReadComp(GLenum t, bool norm, const unsigned char* p) {
    switch (t) {
        case GL_FLOAT: { float f; memcpy(&f, p, 4); return f; }
        case GL_UNSIGNED_BYTE: return norm ? p[0] / 255.0f : (float)p[0];
        case GL_BYTE: { const float v = (float)(signed char)p[0]; return norm ? (v / 127.0f < -1.0f ? -1.0f : v / 127.0f) : v; }
        case GL_UNSIGNED_SHORT: { unsigned short v; memcpy(&v, p, 2); return norm ? v / 65535.0f : (float)v; }
        case GL_SHORT: { short v; memcpy(&v, p, 2); return norm ? (v / 32767.0f < -1.0f ? -1.0f : v / 32767.0f) : (float)v; }
    }
    return 0.0f;
}
// un atributo que la GPU no lee (3 bytes, 3 shorts en algunas): a float4 (lo que falta como GL: 0, 0, 1)
static void AFloat4(const AttrGL& a, const unsigned char* src, int stride, int n, float* d) {
    const int bc = BytesType(a.type);
    for (int v = 0; v < n; v++, d += 4) {
        d[0] = d[1] = d[2] = 0.0f; d[3] = 1.0f;
        for (int c = 0; c < a.size && c < 4; c++) d[c] = ReadComp(a.type, a.norm != 0, src + (size_t)v * stride + c * bc);
    }
}
static BufGL* BufOf(GLuint id) { std::map<GLuint, BufGL*>::iterator it = gBufs.find(id); return it == gBufs.end() ? 0 : it->second; }
static Ring UploadDynamic(BufGL* b) {
    if (b->epoch != gEpoch) {
        b->uploaded = Request(b->copy.size(), 16);
        if (!b->copy.empty()) memcpy(b->uploaded.p, &b->copy[0], b->copy.size());
        b->epoch = gEpoch;
    }
    return b->uploaded;
}
static bool UploadStatic(BufVk& dst, const void* data, size_t size) {
    if (!CreateBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dst)) return false;
    if (!size) return true;
    Ring r = Request(size, 16);
    if (!r.b) return false;
    if (data) memcpy(r.p, data, size); else memset(r.p, 0, size);
    VkBufferCopy c = { r.off, 0, size };
    vkCmdCopyBuffer(CmdPre(), r.b, dst.b, 1, &c);   // (buffer nuevo: nadie lo uso todavia, la copia va antes de todo)
    return true;
}
static void Constant(const AttrGL& a, VkBuffer& buf, VkDeviceSize& off, uint8_t& fmt, uint16_t& stride) {
    Ring r = Request(16, 16); memcpy(r.p, a.cte, 16);
    buf = r.b; off = r.off; fmt = (uint8_t)VK_FORMAT_R32G32B32A32_SFLOAT; stride = 0;
}
static void AttributeOfDraw(int L, int nVerts, VkBuffer& buf, VkDeviceSize& off, uint8_t& fmt, uint16_t& stride) {
    const AttrGL& a = gAttr[L];
    if (!a.on) { Constant(a, buf, off, fmt, stride); return; }
    const int elem = a.size * BytesType(a.type);
    const int st = a.stride ? a.stride : elem;
    const VkFormat f = FormatAttr(a.type, a.size, a.norm != 0);
    const bool readable = f != VK_FORMAT_UNDEFINED && FormatReadable(f);
    if (a.buf) {
        BufGL* b = BufOf(a.buf);
        const size_t o = (size_t)a.ptr;
        if (!b || b->size < o + elem) { Constant(a, buf, off, fmt, stride); return; }
        if (readable) {
            if (b->isStatic) { buf = b->vk.b; off = o; }
            else { Ring r = UploadDynamic(b); buf = r.b; off = r.off + o; }
            fmt = (uint8_t)f; stride = (uint16_t)st;
            return;
        }
        if (b->copy.empty()) { Constant(a, buf, off, fmt, stride); return; }
        const int n = (int)((b->size - o - elem) / st) + 1;
        if (b->isStatic) {
            const uint64_t key = (uint64_t)a.type | ((uint64_t)a.size << 16) | ((uint64_t)(a.norm ? 1 : 0) << 20) | ((uint64_t)st << 24) | ((uint64_t)o << 40);
            std::map<uint64_t, BufVk>::iterator it = b->conv.find(key);
            if (it == b->conv.end()) {
                std::vector<float> f4((size_t)n * 4);
                AFloat4(a, &b->copy[o], st, n, &f4[0]);
                BufVk nb; if (!UploadStatic(nb, &f4[0], f4.size() * 4)) { Constant(a, buf, off, fmt, stride); return; }
                it = b->conv.insert(std::make_pair(key, nb)).first;
            }
            buf = it->second.b; off = 0;
        } else {
            Ring r = Request((VkDeviceSize)n * 16, 16);
            AFloat4(a, &b->copy[o], st, n, (float*)r.p);
            buf = r.b; off = r.off;
        }
        fmt = (uint8_t)VK_FORMAT_R32G32B32A32_SFLOAT; stride = 16;
        return;
    }
    if (!a.ptr || nVerts <= 0) { Constant(a, buf, off, fmt, stride); return; }
    // (arreglo de la RAM: lo que lee el draw, al anillo)
    if (readable) {
        const size_t bytes = (size_t)st * (nVerts - 1) + elem;
        Ring r = Request(bytes, 16); memcpy(r.p, a.ptr, bytes);
        buf = r.b; off = r.off; fmt = (uint8_t)f; stride = (uint16_t)st;
    } else {
        Ring r = Request((VkDeviceSize)nVerts * 16, 16);
        AFloat4(a, (const unsigned char*)a.ptr, st, nVerts, (float*)r.p);
        buf = r.b; off = r.off; fmt = (uint8_t)VK_FORMAT_R32G32B32A32_SFLOAT; stride = 16;
    }
}
static bool HasRAMArrays(const ProgGL* p) {
    for (int i = 0; i < kAttrs; i++) if ((p->maskAttrib & (1u << i)) && gAttr[i].on && !gAttr[i].buf && gAttr[i].ptr) return true;
    return false;
}

// ============================================================================
//  EL DRAW
// ============================================================================
static VkDescriptorSet SetOfDraw(ProgGL* p, const Ring& u) {
    // (la clave: el buffer de los uniforms y lo que lee cada sampler)
    std::string key((const char*)&u.b, sizeof(u.b));
    key.append((const char*)&p->blockSize, sizeof(int));
    VkImageView views[kSamples]; VkSampler samps[kSamples];
    for (int i = 0; i < p->nSamples; i++) {
        const int un = p->unit[i] >= 0 && p->unit[i] < kUnits ? p->unit[i] : 0;
        TexGL* t = gUnit[un] ? TexOf(gUnit[un]) : 0;
        if (!t || !t->has || !t->started) t = gBlack;
        t->useCmd = gSerialCmd;
        views[i]  = t->view; samps[i] = SamplerOf(t);
        key.append((const char*)&views[i], sizeof(VkImageView)); key.append((const char*)&samps[i], sizeof(VkSampler));
    }
    std::map<std::string, VkDescriptorSet>::iterator it = gCacheSets.find(key);
    if (it != gCacheSets.end()) return it->second;
    Frame& c = gFrames[gCur];
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
        if (c.pool >= c.pools.size()) {
            VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 * 8 } };
            VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pi.maxSets = 1024; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(gDev, &pi, 0, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
            c.pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = c.pools[c.pool]; ai.descriptorSetCount = 1; ai.pSetLayouts = &gSetLayout;
        if (vkAllocateDescriptorSets(gDev, &ai, &set) == VK_SUCCESS) break;
        c.pool++;
    }
    VkDescriptorBufferInfo bi = { u.b, 0, (VkDeviceSize)p->blockSize };
    VkDescriptorImageInfo ii[kSamples];
    VkWriteDescriptorSet w[1 + kSamples]; memset(w, 0, sizeof(w));
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &bi;
    for (int i = 0; i < p->nSamples; i++) {
        ii[i].sampler = samps[i]; ii[i].imageView = views[i]; ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1 + i].dstSet = set; w[1 + i].dstBinding = (uint32_t)i + 1;
        w[1 + i].descriptorCount = 1; w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1 + i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(gDev, (uint32_t)(1 + p->nSamples), w, 0, 0);
    gCacheSets[key] = set;
    return set;
}
// el recorte del draw (el scissor de GL en la imagen); false = no queda nada
static bool ClipRegion(VkRect2D& r) {
    int x = 0, y = 0, w = gPass.w, h = gPass.h;
    if (g.scissor) {
        x = g.sc[0]; w = g.sc[2]; h = g.sc[3];
        y = gPass.flipped ? gPass.h - g.sc[1] - g.sc[3] : g.sc[1];
        if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
        if (x + w > gPass.w) w = gPass.w - x; if (y + h > gPass.h) h = gPass.h - y;
    }
    if (w <= 0 || h <= 0) return false;
    r.offset.x = x; r.offset.y = y; r.extent.width = (uint32_t)w; r.extent.height = (uint32_t)h;
    return true;
}
static VkCommandBuffer PrepareDraw(GLenum mode, int nVerts) {
    ProgGL* p = gProg;
    if (!p || !p->ok || g.vp[2] <= 0 || g.vp[3] <= 0) return VK_NULL_HANDLE;
    if (!BeginPass()) return VK_NULL_HANDLE;
    VkRect2D sc; if (!ClipRegion(sc)) return VK_NULL_HANDLE;
    KeyPipe k; memset(&k, 0, sizeof(k));
    VkBuffer bufs[kAttrs]; VkDeviceSize offs[kAttrs];
    for (int i = 0; i < kAttrs; i++) if (p->maskAttrib & (1u << i)) AttributeOfDraw(i, nVerts, bufs[i], offs[i], k.fmt[i], k.stride[i]);
    // el bloque de uniforms (y lo del destino que usa gl_FragCoord)
    const float settings[4] = { gPass.flipped ? 1.0f : 0.0f, (float)gPass.h, 0.0f, 0.0f };
    if (memcmp(&p->block[p->offFit], settings, 16) != 0) { memcpy(&p->block[p->offFit], settings, 16); p->dirty = true; }
    if (p->dirty || p->epochUpload != gEpoch) {
        p->uploaded = Request((VkDeviceSize)p->blockSize, gAlignUBO);
        memcpy(p->uploaded.p, &p->block[0], (size_t)p->blockSize);
        p->dirty = false; p->epochUpload = gEpoch;
    }
    VkDescriptorSet set = SetOfDraw(p, p->uploaded);
    StateKey(mode, k);
    std::map<KeyPipe, VkPipeline>::iterator it = p->pipes.find(k);
    VkPipeline pipe = it != p->pipes.end() ? it->second : (p->pipes[k] = CreatePipe(p, k));
    if (!pipe || !set) return VK_NULL_HANDLE;
    VkCommandBuffer cb = Cmd();
    if (gEc.pipe != pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); gEc.pipe = pipe; }
    VkViewport vp;
    vp.x = (float)g.vp[0]; vp.width = (float)g.vp[2];
    if (gPass.flipped) { vp.y = (float)(gPass.h - g.vp[1]); vp.height = -(float)g.vp[3]; }
    else { vp.y = (float)g.vp[1]; vp.height = (float)g.vp[3]; }
    vp.minDepth = g.zn; vp.maxDepth = g.zf;
    if (!gEc.ok || memcmp(&vp, &gEc.vp, sizeof(vp)) != 0) { vkCmdSetViewport(cb, 0, 1, &vp); gEc.vp = vp; }
    if (!gEc.ok || memcmp(&sc, &gEc.sc, sizeof(sc)) != 0) { vkCmdSetScissor(cb, 0, 1, &sc); gEc.sc = sc; }
    const float line = gFeat.wideLines ? (g.line < 1.0f ? 1.0f : g.line > gProps.limits.lineWidthRange[1] ? gProps.limits.lineWidthRange[1] : g.line) : 1.0f;
    if (!gEc.ok || line != gEc.line) { vkCmdSetLineWidth(cb, line); gEc.line = line; }
    if (!gEc.ok || g.offU != gEc.biasC || g.offF != gEc.biasP) { vkCmdSetDepthBias(cb, g.offU, 0.0f, g.offF); gEc.biasC = g.offU; gEc.biasP = g.offF; }
    if (!gEc.ok || (g.sMaskR & 0xFF) != gEc.sCmp) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g.sMaskR & 0xFF); gEc.sCmp = g.sMaskR & 0xFF; }
    if (!gEc.ok || (g.sMaskW & 0xFF) != gEc.sWr) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g.sMaskW & 0xFF); gEc.sWr = g.sMaskW & 0xFF; }
    if (!gEc.ok || ((uint32_t)g.sRef & 0xFF) != gEc.sRef) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)g.sRef & 0xFF); gEc.sRef = (uint32_t)g.sRef & 0xFF; }
    gEc.ok = true;
    const uint32_t dyn = (uint32_t)p->uploaded.off;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gLayout, 0, 1, &set, 1, &dyn);
    for (int i = 0; i < kAttrs; i++) if (p->maskAttrib & (1u << i)) vkCmdBindVertexBuffers(cb, (uint32_t)i, 1, &bufs[i], &offs[i]);
    return cb;
}
// (el anillo crece con cada draw: si un cuadro sin presentar junta demasiado, se manda a la GPU y se recicla)
static void RingControl() {
    if (UsedRing() > (256ull << 20) || gFrames[gCur].garbage.size() > 4096) { SendAndWait(); RecycleAll(); }
}
static void DrawIndices32(GLenum mode, const uint32_t* idx, int n, int nVerts) {
    Ring r = Request((VkDeviceSize)n * 4, 16); memcpy(r.p, idx, (size_t)n * 4);
    VkCommandBuffer cb = PrepareDraw(mode, nVerts);
    if (!cb) return;
    vkCmdBindIndexBuffer(cb, r.b, r.off, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, (uint32_t)n, 1, 0, 0, 0);
}
void vkglDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!gReady || count <= 0 || first < 0) return;
    RingControl();
    if (mode == GL_LINE_LOOP) {   // (Vulkan no tiene lineas cerradas: una tira que vuelve al primero)
        std::vector<uint32_t> idx((size_t)count + 1);
        for (int i = 0; i < count; i++) idx[i] = (uint32_t)(first + i);
        idx[count] = (uint32_t)first;
        DrawIndices32(GL_LINE_STRIP, &idx[0], count + 1, first + count);
        return;
    }
    VkCommandBuffer cb = PrepareDraw(mode, first + count);
    if (cb) vkCmdDraw(cb, (uint32_t)count, 1, (uint32_t)first, 0);
}
void vkglDrawElements(GLenum mode, GLsizei count, GLenum idxType, const void* ind) {
    if (!gReady || count <= 0 || !gProg) return;
    RingControl();
    const size_t isz = idxType == GL_UNSIGNED_INT ? 4 : idxType == GL_UNSIGNED_SHORT ? 2 : 1;
    const unsigned char* cpu = 0; VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize io = 0;
    BufGL* b = gBufElem ? BufOf(gBufElem) : 0;
    if (b) {
        const size_t off = (size_t)ind;
        if (off + (size_t)count * isz > b->size) { ErrorGL(GL_INVALID_OPERATION); return; }
        cpu = b->copy.empty() ? 0 : &b->copy[off];
        if (b->isStatic) { ib = b->vk.b; io = off; } else { Ring r = UploadDynamic(b); ib = r.b; io = r.off + off; }
    } else {
        cpu = (const unsigned char*)ind;
        if (!cpu) return;
    }
    const bool ram = HasRAMArrays(gProg);
    if (idxType == GL_UNSIGNED_BYTE || mode == GL_LINE_LOOP || !b || (ram && cpu)) {
        if (!cpu) { ErrorGL(GL_INVALID_OPERATION); return; }
        std::vector<uint32_t> idx((size_t)count + (mode == GL_LINE_LOOP ? 1 : 0));
        uint32_t maxI = 0;
        for (int i = 0; i < count; i++) {
            const uint32_t v = isz == 4 ? ((const uint32_t*)cpu)[i] : isz == 2 ? ((const unsigned short*)cpu)[i] : cpu[i];
            idx[i] = v; if (v > maxI) maxI = v;
        }
        if (mode == GL_LINE_LOOP) idx[count] = idx[0];
        DrawIndices32(mode == GL_LINE_LOOP ? GL_LINE_STRIP : mode, &idx[0], (int)idx.size(), (int)maxI + 1);
        return;
    }
    VkCommandBuffer cb = PrepareDraw(mode, 0);
    if (!cb) return;
    vkCmdBindIndexBuffer(cb, ib, io, isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cb, (uint32_t)count, 1, 0, 0, 0);
}

// ============================================================================
//  Estado
// ============================================================================
static bool* CapOf(GLenum c) {
    switch (c) {
        case GL_BLEND: return &g.blend; case GL_DEPTH_TEST: return &g.depth; case GL_CULL_FACE: return &g.cull;
        case GL_SCISSOR_TEST: return &g.scissor; case GL_STENCIL_TEST: return &g.stencil; case GL_POLYGON_OFFSET_FILL: return &g.offset;
    }
    return 0;
}
void vkglEnable(GLenum c) { bool* b = CapOf(c); if (b) *b = true; }
void vkglDisable(GLenum c) { bool* b = CapOf(c); if (b) *b = false; }
GLboolean vkglIsEnabled(GLenum c) { bool* b = CapOf(c); return (b && *b) ? GL_TRUE : GL_FALSE; }
void vkglBlendFunc(GLenum s, GLenum d) { g.bSrc = s; g.bDst = d; }
void vkglBlendEquation(GLenum m) { g.bEq = m; }
void vkglDepthFunc(GLenum f) { g.dFunc = f; }
void vkglDepthMask(GLboolean w) { g.dMask = w != 0; }
void vkglColorMask(GLboolean r, GLboolean gg, GLboolean b, GLboolean a) { g.cMask[0] = r != 0; g.cMask[1] = gg != 0; g.cMask[2] = b != 0; g.cMask[3] = a != 0; }
void vkglStencilMask(GLuint m) { g.sMaskW = m; }
void vkglStencilFunc(GLenum f, GLint ref, GLuint mask) { g.sFunc = f; g.sRef = ref; g.sMaskR = mask; }
void vkglStencilOp(GLenum a, GLenum b, GLenum c) { g.sFail = a; g.sZFail = b; g.sZPass = c; }
void vkglClearStencil(GLint s) { g.clearS = s; }
void vkglClearColor(GLfloat r, GLfloat gg, GLfloat b, GLfloat a) { g.clear[0] = r; g.clear[1] = gg; g.clear[2] = b; g.clear[3] = a; }
void vkglScissor(GLint x, GLint y, GLsizei w, GLsizei h) { g.sc[0] = x; g.sc[1] = y; g.sc[2] = w; g.sc[3] = h; }
void vkglViewport(GLint x, GLint y, GLsizei w, GLsizei h) { g.vp[0] = x; g.vp[1] = y; g.vp[2] = w; g.vp[3] = h; }
void vkglPolygonOffset(GLfloat f, GLfloat u) { g.offF = f; g.offU = u; }
void vkglLineWidth(GLfloat w) { g.line = w; }
void vkglFrontFace(GLenum m) { g.front = m; }
void vkglCullFace(GLenum m) { g.cullMode = m; }
void vkglDepthRangef(GLfloat n, GLfloat f) { g.zn = n < 0 ? 0 : n > 1 ? 1 : n; g.zf = f < 0 ? 0 : f > 1 ? 1 : f; }
void vkglPixelStorei(GLenum p, GLint v) { if (p == GL_UNPACK_ALIGNMENT) g.unpack = v; else if (p == GL_PACK_ALIGNMENT) g.pack = v; }
void vkglFlush() {}
void vkglFinish() { if (gReady) { SendAndWait(); RecycleAll(); } }
GLenum vkglGetError() { const GLenum e = gErrorGL; gErrorGL = GL_NO_ERROR; return e; }
void vkglClear(GLbitfield m) {
    if (!gReady || !BeginPass()) return;
    VkRect2D r; if (!ClipRegion(r)) return;
    VkClearAttachment at[2]; uint32_t n = 0;
    if ((m & GL_COLOR_BUFFER_BIT) && (g.cMask[0] || g.cMask[1] || g.cMask[2] || g.cMask[3])) {
        at[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; at[n].colorAttachment = 0;
        for (int i = 0; i < 4; i++) at[n].clearValue.color.float32[i] = g.clear[i];
        n++;
    }
    VkImageAspectFlags asp = 0;
    if (gPass.withProf && (m & GL_DEPTH_BUFFER_BIT) && g.dMask) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (gPass.withProf && (m & GL_STENCIL_BUFFER_BIT) && (g.sMaskW & 0xFF)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (asp) {
        at[n].aspectMask = asp; at[n].colorAttachment = 0;
        at[n].clearValue.depthStencil.depth = g.clearZ; at[n].clearValue.depthStencil.stencil = (uint32_t)g.clearS & 0xFF;
        n++;
    }
    if (!n) return;
    VkClearRect cr; cr.rect = r; cr.baseArrayLayer = 0; cr.layerCount = 1;
    vkCmdClearAttachments(Cmd(), n, at, 1, &cr);
}
void vkglGetIntegerv(GLenum p, GLint* v) {
    switch (p) {
        case GL_VIEWPORT: for (int i = 0; i < 4; i++) v[i] = g.vp[i]; break;
        case GL_SCISSOR_BOX: for (int i = 0; i < 4; i++) v[i] = g.sc[i]; break;
        case GL_STENCIL_BITS: v[0] = 8; break;
        case GL_DEPTH_BITS: v[0] = 24; break;
        case GL_MAX_TEXTURE_SIZE: v[0] = (GLint)gProps.limits.maxImageDimension2D; break;
        case GL_UNPACK_ALIGNMENT: v[0] = g.unpack; break;
        case GL_PACK_ALIGNMENT: v[0] = g.pack; break;
        case GL_FRAMEBUFFER_BINDING: v[0] = (GLint)gCurrentFbo; break;
        case GL_TEXTURE_BINDING_2D: v[0] = (GLint)gUnit[gActiveUnit]; break;
        default: v[0] = 0; break;
    }
}
void vkglGetFloatv(GLenum p, GLfloat* v) {
    switch (p) {
        case GL_COLOR_CLEAR_VALUE: for (int i = 0; i < 4; i++) v[i] = g.clear[i]; break;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: v[0] = gFeat.samplerAnisotropy ? gProps.limits.maxSamplerAnisotropy : 1.0f; break;
        case GL_LINE_WIDTH: v[0] = g.line; break;
        default: v[0] = 0.0f; break;
    }
}
const GLubyte* vkglGetString(GLenum p) {
    static std::string s;
    switch (p) {
        case GL_VENDOR: s = "Vulkan"; break;
        case GL_RENDERER: s = gGPUName + " (Vulkan)"; break;
        case GL_VERSION: { char v[64]; snprintf(v, sizeof(v), "OpenGL ES 2.0 sobre Vulkan %u.%u", VK_API_VERSION_MAJOR(gProps.apiVersion), VK_API_VERSION_MINOR(gProps.apiVersion)); s = v; break; }
        case GL_EXTENSIONS: s = std::string("GL_OES_element_index_uint GL_OES_texture_npot GL_OES_rgb8_rgba8")
                                 + (gFeat.samplerAnisotropy ? " GL_EXT_texture_filter_anisotropic" : ""); break;
        default: s = ""; break;
    }
    return (const GLubyte*)s.c_str();
}

// ============================================================================
//  Buffers y atributos
// ============================================================================
void vkglGenBuffers(GLsizei n, GLuint* b) { for (int i = 0; i < n; i++) { b[i] = gNextBuf++; BufGL* x = new BufGL(); x->size = 0; x->isStatic = x->elements = false; memset(&x->vk, 0, sizeof(x->vk)); x->epoch = 0; gBufs[b[i]] = x; } }
static void ThrowBufGL(BufGL* b) {
    ThrowBuffer(b->vk);
    for (std::map<uint64_t, BufVk>::iterator it = b->conv.begin(); it != b->conv.end(); ++it) ThrowBuffer(it->second);
    b->conv.clear();
}
void vkglDeleteBuffers(GLsizei n, const GLuint* b) {
    for (int i = 0; i < n; i++) {
        BufGL* x = BufOf(b[i]);
        if (!x) continue;
        ThrowBufGL(x); delete x; gBufs.erase(b[i]);
        if (gBufArray == b[i]) gBufArray = 0;
        if (gBufElem == b[i]) gBufElem = 0;
        for (int k = 0; k < kAttrs; k++) if (gAttr[k].buf == b[i]) gAttr[k].buf = 0;
    }
}
void vkglBindBuffer(GLenum t, GLuint b) {
    if (b && !BufOf(b)) { BufGL* x = new BufGL(); x->size = 0; x->isStatic = x->elements = false; memset(&x->vk, 0, sizeof(x->vk)); x->epoch = 0; gBufs[b] = x; }
    if (t == GL_ARRAY_BUFFER) gBufArray = b; else if (t == GL_ELEMENT_ARRAY_BUFFER) gBufElem = b;
}
void vkglBufferData(GLenum t, GLsizeiptr size, const void* data, GLenum use) {
    BufGL* b = BufOf(t == GL_ELEMENT_ARRAY_BUFFER ? gBufElem : gBufArray);
    if (!b || size < 0 || !gReady) { ErrorGL(GL_INVALID_OPERATION); return; }
    ThrowBufGL(b);
    b->size = (size_t)size; b->elements = t == GL_ELEMENT_ARRAY_BUFFER;
    b->isStatic = use == GL_STATIC_DRAW;
    b->epoch = 0;
    const bool copy = !b->isStatic || b->elements || gSaveCopies;
    if (copy) { if (data) b->copy.assign((const unsigned char*)data, (const unsigned char*)data + size); else b->copy.assign((size_t)size, 0); }
    else { std::vector<unsigned char>().swap(b->copy); }
    if (b->isStatic && !UploadStatic(b->vk, data, (size_t)size)) { ErrorGL(GL_OUT_OF_MEMORY); b->size = 0; }
}
void vkglVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* p) {
    if (i >= (GLuint)kAttrs) { ErrorGL(GL_INVALID_VALUE); return; }
    AttrGL& a = gAttr[i]; a.size = size; a.type = type; a.norm = norm; a.stride = stride; a.ptr = p; a.buf = gBufArray;
}
void vkglEnableVertexAttribArray(GLuint i) { if (i < (GLuint)kAttrs) gAttr[i].on = true; }
void vkglDisableVertexAttribArray(GLuint i) { if (i < (GLuint)kAttrs) gAttr[i].on = false; }
void vkglVertexAttrib4fv(GLuint i, const GLfloat* v) { if (i < (GLuint)kAttrs) memcpy(gAttr[i].cte, v, 16); }

// ============================================================================
//  Shaders y programas
// ============================================================================
GLuint vkglCreateShader(GLenum type) { ShaderGL* s = new ShaderGL(); s->type = type; s->ok = false; const GLuint id = gNextName++; gShaders[id] = s; return id; }
void vkglShaderSource(GLuint id, GLsizei n, const GLchar* const* parts, const GLint* lengths) {
    if (!gShaders.count(id)) return;
    std::string& f = gShaders[id]->source; f.clear();
    for (int i = 0; i < n; i++) { if (lengths && lengths[i] >= 0) f.append(parts[i], (size_t)lengths[i]); else f += parts[i]; }
}
void vkglCompileShader(GLuint id) {
    if (!gShaders.count(id) || !gShc) return;
    ShaderGL* s = gShaders[id];
    s->log.clear();
    s->ok = Preprocess(s->source, s->type == GL_VERTEX_SHADER ? kShcVertex : kShcFragment, s->pre, s->log);
}
void vkglGetShaderiv(GLuint id, GLenum p, GLint* v) {
    ShaderGL* s = gShaders.count(id) ? gShaders[id] : 0;
    if (!s) { v[0] = 0; return; }
    if (p == GL_COMPILE_STATUS) v[0] = s->ok ? 1 : 0; else if (p == GL_INFO_LOG_LENGTH) v[0] = (GLint)s->log.size() + 1; else v[0] = 0;
}
static void CopyLog(const std::string& l, GLsizei max, GLsizei* length, GLchar* out) {
    if (max <= 0) return;
    const size_t n = l.size() < (size_t)max - 1 ? l.size() : (size_t)max - 1;
    memcpy(out, l.c_str(), n); out[n] = 0;
    if (length) *length = (GLsizei)n;
}
void vkglGetShaderInfoLog(GLuint id, GLsizei max, GLsizei* length, GLchar* log) { CopyLog(gShaders.count(id) ? gShaders[id]->log : std::string(), max, length, log); }
void vkglDeleteShader(GLuint id) { std::map<GLuint, ShaderGL*>::iterator it = gShaders.find(id); if (it != gShaders.end()) { delete it->second; gShaders.erase(it); } }
GLuint vkglCreateProgram() {
    ProgGL* p = new ProgGL();
    p->vs = p->fs = 0; p->ok = false; p->mVS = p->mFS = VK_NULL_HANDLE; p->blockSize = 0; p->offFit = 0; p->nSamples = 0;
    p->maskAttrib   = 0; p->dirty = true; p->epochUpload = 0; memset(&p->uploaded, 0, sizeof(p->uploaded));
    const GLuint id = gNextName++; gProgs[id] = p; return id;
}
static ProgGL* ProgOf(GLuint id) { std::map<GLuint, ProgGL*>::iterator it = gProgs.find(id); return it == gProgs.end() ? 0 : it->second; }
void vkglAttachShader(GLuint pid, GLuint sid) {
    ProgGL* p = ProgOf(pid); ShaderGL* s = gShaders.count(sid) ? gShaders[sid] : 0;
    if (!p || !s) return;
    if (s->type == GL_VERTEX_SHADER) p->vs = sid; else p->fs = sid;
}
void vkglBindAttribLocation(GLuint pid, GLuint i, const GLchar* n) { ProgGL* p = ProgOf(pid); if (p && n && i < (GLuint)kAttrs) p->attribRequests[n] = (int)i; }
void vkglLinkProgram(GLuint pid) {
    ProgGL* p = ProgOf(pid);
    if (!p) return;
    DeleteModulesAndPipes(p);
    p->log.clear();
    p->ok = gReady && Link(p);
    if (!p->ok) w3dLogfE("[vk] el programa no linkeo: %s", p->log.c_str());
}
void vkglGetProgramiv(GLuint pid, GLenum q, GLint* v) {
    ProgGL* p = ProgOf(pid);
    if (!p) { v[0] = 0; return; }
    if (q == GL_LINK_STATUS) v[0] = p->ok ? 1 : 0; else if (q == GL_INFO_LOG_LENGTH) v[0] = (GLint)p->log.size() + 1; else v[0] = 0;
}
void vkglGetProgramInfoLog(GLuint pid, GLsizei max, GLsizei* length, GLchar* log) { ProgGL* p = ProgOf(pid); CopyLog(p ? p->log : std::string(), max, length, log); }
void vkglUseProgram(GLuint pid) { gProg = ProgOf(pid); gProgId = gProg ? pid : 0; }
void vkglDeleteProgram(GLuint pid) {
    ProgGL* p = ProgOf(pid);
    if (!p) return;
    DeleteModulesAndPipes(p);
    if (gProg == p) { gProg = 0; gProgId = 0; }
    delete p; gProgs.erase(pid);
}
GLint vkglGetUniformLocation(GLuint pid, const GLchar* name) {
    ProgGL* p = ProgOf(pid);
    if (!p || !p->ok || !name) return -1;
    std::string n = name; int idx = 0;
    const size_t c = n.find('[');
    if (c != std::string::npos) { idx = atoi(n.c_str() + c + 1); n = n.substr(0, c); }
    for (size_t i = 0; i < p->unis.size(); i++)
        if (p->unis[i].name == n) return idx < p->unis[i].elems ? p->unis[i].loc0 + idx : -1;
    return -1;
}
GLint vkglGetAttribLocation(GLuint pid, const GLchar* name) {
    ProgGL* p = ProgOf(pid);
    if (!p || !name) return -1;
    std::map<std::string, int>::iterator it = p->attribs.find(name);
    return it == p->attribs.end() ? -1 : it->second;
}
// escribir en el bloque del programa en uso (sucio solo si cambio algo)
static void Write(ProgGL* p, int off, const void* v, size_t n) {
    if (off < 0 || (size_t)off + n > p->block.size()) return;
    if (memcmp(&p->block[(size_t)off], v, n) != 0) { memcpy(&p->block[(size_t)off], v, n); p->dirty = true; }
}
static const UniGL* UniOf(ProgGL* p, GLint loc, int& elem) {
    if (!p || loc < 0 || loc >= (GLint)p->locs.size()) return 0;
    elem = p->locs[loc].elem;
    return &p->unis[p->locs[loc].uni];
}
void vkglUniform1i(GLint loc, GLint v) {
    int e; const UniGL* u = UniOf(gProg, loc, e); if (!u) return;
    if (u->sample >= 0) { gProg->unit[u->sample] = v; return; }
    if (u->t->cls == 0) { const float f = (float)v; Write(gProg, u->off + e * u->stride, &f, 4); }
    else { const int32_t i = u->t->cls == 2 ? (v != 0) : v; Write(gProg, u->off + e * u->stride, &i, 4); }
}
void vkglUniform1f(GLint loc, GLfloat v) {
    int e; const UniGL* u = UniOf(gProg, loc, e); if (!u || u->sample >= 0) return;
    if (u->t->cls == 0) Write(gProg, u->off + e * u->stride, &v, 4);
    else { const int32_t i = u->t->cls == 2 ? (v != 0.0f) : (int32_t)v; Write(gProg, u->off + e * u->stride, &i, 4); }
}
void vkglUniform4fv(GLint loc, GLsizei n, const GLfloat* v) {
    int e; const UniGL* u = UniOf(gProg, loc, e); if (!u || u->sample >= 0 || u->t->cls == 3) return;
    const int comps = u->t->size / 4;
    for (int k = 0; k < n && e + k < u->elems; k++) Write(gProg, u->off + (e + k) * u->stride, v + k * 4, (size_t)(comps < 4 ? comps : 4) * 4);
}
void vkglUniformMatrix3fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat* v) {
    int e; const UniGL* u = UniOf(gProg, loc, e); if (!u || u->t->cols != 3) return;
    for (int k = 0; k < n && e + k < u->elems; k++) {
        const GLfloat* m = v + k * 9;
        for (int c = 0; c < 3; c++) {
            float col[3];
            for (int r = 0; r < 3; r++) col[r] = tr ? m[r * 3 + c] : m[c * 3 + r];
            Write(gProg, u->off + (e + k) * u->stride + c * 16, col, 12);
        }
    }
}
void vkglUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat* v) {
    int e; const UniGL* u = UniOf(gProg, loc, e); if (!u || u->t->cols != 4) return;
    for (int k = 0; k < n && e + k < u->elems; k++) {
        const GLfloat* m = v + k * 16;
        if (!tr) { Write(gProg, u->off + (e + k) * u->stride, m, 64); continue; }
        float t[16]; for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[c * 4 + r] = m[r * 4 + c];
        Write(gProg, u->off + (e + k) * u->stride, t, 64);
    }
}

// ============================================================================
//  Texturas (GL)
// ============================================================================
void vkglActiveTexture(GLenum u) { const int i = (int)u - GL_TEXTURE0; if (i >= 0 && i < kUnits) gActiveUnit = i; }
void vkglGenTextures(GLsizei n, GLuint* t) { for (int i = 0; i < n; i++) { while (gTex.count(gNextTex)) gNextTex++; t[i] = gNextTex++; gTex[t[i]] = NewTex(); } }
void vkglDeleteTextures(GLsizei n, const GLuint* t) {
    for (int i = 0; i < n; i++) {
        std::map<GLuint, TexGL*>::iterator it = gTex.find(t[i]);
        if (!t[i] || it == gTex.end()) continue;
        if (gReady) ThrowImageTex(it->second);
        delete it->second; gTex.erase(it);
        for (int k = 0; k < kUnits; k++) if (gUnit[k] == t[i]) gUnit[k] = 0;
        for (std::map<GLuint, GLuint>::iterator f = gFbos.begin(); f != gFbos.end(); ++f) if (f->second == t[i]) f->second = 0;
    }
}
void vkglBindTexture(GLenum, GLuint t) { gUnit[gActiveUnit] = t; if (t && !gTex.count(t)) gTex[t] = NewTex(); }
void vkglTexImage2D(GLenum, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const void* data) {
    TexGL* t = CurrentTex();
    if (!t || !gReady || w <= 0 || h <= 0 || level < 0) { ErrorGL(GL_INVALID_VALUE); return; }
    if (level == 0) {
        if ((!t->has || t->w != w || t->h != h) && !CreateImageTex(t, w, h)) { ErrorGL(GL_OUT_OF_MEMORY); return; }
    } else if (!t->has || level >= t->levels) { ErrorGL(GL_INVALID_OPERATION); return; }
    if (data || !t->started) Upload(t, level, 0, 0, w, h, format, type, data);
    if (level > 0 && level == t->levelsDef) { t->levelsDef = level + 1; RedoView(t); }
    if (level == 0 && t->genMip) GenerateMips(t);
}
void vkglTexSubImage2D(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* data) {
    TexGL* t = CurrentTex();
    if (!t || !t->has || !data || w <= 0 || h <= 0) return;
    const int nw = t->w >> level, nh = t->h >> level;
    if (x < 0 || y < 0 || x + w > (nw > 0 ? nw : 1) || y + h > (nh > 0 ? nh : 1)) { ErrorGL(GL_INVALID_VALUE); return; }
    Upload(t, level, x, y, w, h, format, type, data);
    if (level == 0 && t->genMip) GenerateMips(t);
}
void vkglTexParameteri(GLenum, GLenum p, GLint v) {
    TexGL* t = CurrentTex(); if (!t) return;
    switch (p) {
        case GL_TEXTURE_MIN_FILTER: t->minF = v; break;
        case GL_TEXTURE_MAG_FILTER: t->magF = v; break;
        case GL_TEXTURE_WRAP_S: t->wrapS = v; break;
        case GL_TEXTURE_WRAP_T: t->wrapT = v; break;
        case GL_GENERATE_MIPMAP: t->genMip = v != 0; break;
    }
}
void vkglTexParameterf(GLenum target, GLenum p, GLfloat v) {
    if (p == GL_TEXTURE_MAX_ANISOTROPY_EXT) { TexGL* t = CurrentTex(); if (t) t->aniso = v; return; }
    vkglTexParameteri(target, p, (GLint)v);
}
void vkglGenerateMipmap(GLenum) { TexGL* t = CurrentTex(); if (t && gReady) GenerateMips(t); }
// la imagen del destino actual, lista para leerla (TRANSFER_SRC); devuelve su layout de siempre para volver
static VkImageLayout OpenReading(VkCommandBuffer cb, const Destination& d) {
    const VkImageLayout of = d.tex ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    Barrier(cb, d.img, VK_IMAGE_ASPECT_COLOR_BIT, of, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    return of;
}
void vkglCopyTexSubImage2D(GLenum, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h) {
    TexGL* t = CurrentTex();
    if (!t || !t->has || !gReady || w <= 0 || h <= 0) return;
    const Destination d = CurrentDestination();
    if (d.tex == t) return;   // (copiarse a si misma: no)
    // (lo que cae afuera del destino no se copia)
    if (x < 0) { xoff -= x; w += x; x = 0; } if (y < 0) { yoff -= y; h += y; y = 0; }
    if (x + w > d.w) w = d.w - x; if (y + h > d.h) h = d.h - y;
    if (w <= 0 || h <= 0) return;
    FinishPass();
    VkCommandBuffer cb = Cmd();
    const VkImageLayout turn = OpenReading(cb, d);
    TexOpenCopy(cb, t);
    VkImageBlit b; memset(&b, 0, sizeof(b));
    b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1; b.dstSubresource.mipLevel = (uint32_t)level;
    b.srcOffsets[0].x = x; b.srcOffsets[1].x = x + w; b.srcOffsets[0].z = 0; b.srcOffsets[1].z = 1;
    if (d.flipped) { b.srcOffsets[0].y = d.h - y; b.srcOffsets[1].y = d.h - y - h; }   // (la fila de abajo de GL primero)
    else { b.srcOffsets[0].y = y; b.srcOffsets[1].y = y + h; }
    b.dstOffsets[0].x = xoff; b.dstOffsets[0].y = yoff; b.dstOffsets[1].x = xoff + w; b.dstOffsets[1].y = yoff + h; b.dstOffsets[1].z = 1;
    vkCmdBlitImage(cb, d.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    TexCloseCopy(cb, t);
    Barrier(cb, d.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, turn, 0, 1);
    t->useCmd = gSerialCmd;
}
// LA PROFUNDIDAD DE LA PANTALLA a la textura de la unidad activa (los reflejos en pantalla del backend de shaders: el rayo
// reflejado se compara con ella). La textura pasa a ser una imagen de profundidad del tamanio del rectangulo (x, y en
// coordenadas de GL). Se copia tal cual (sin dar vuelta: una copia de profundidad no puede): su fila 0 es la de ARRIBA
// del rectangulo, y el shader la lee dada vuelta. false = no se pudo (el que llama sigue sin profundidad)
static bool CreateImage(VkFormat f, int w, int h, VkImageUsageFlags use, VkImageAspectFlags asp, VkImage& img, Assign& a, VkImageView& v);
bool vkglCopyDepth(GLint x, GLint y, GLsizei w, GLsizei h) {
    TexGL* t = CurrentTex();
    if (!t || !gReady || w <= 0 || h <= 0) return false;
    const Destination d = CurrentDestination();
    if (d.tex || !d.withProf) return false;   // (solo de la pantalla)
    // (el rectangulo, recortado a la pantalla: lo que cae afuera queda con lo que tenia la textura)
    const int tw = w, th = h;          // (la textura, del tamanio pedido)
    int cx = x, cy = y, cw = w, ch = h;   // (lo que se copia: recortado a la pantalla)
    if (cx < 0) { cw += cx; cx = 0; } if (cy < 0) { ch += cy; cy = 0; }
    if (cx + cw > d.w) cw = d.w - cx; if (cy + ch > d.h) ch = d.h - cy;
    if (cw <= 0 || ch <= 0) return false;
    const VkImageAspectFlags ad = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!t->has || t->w != tw || t->h != th || !t->prof) {
        ThrowImageTex(t);
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(gPhys, gFmtProf, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
        if (!CreateImage(gFmtProf, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                         t->img, t->a, t->view)) { t->img = VK_NULL_HANDLE; t->view = VK_NULL_HANDLE; return false; }
        t->w = w; t->h = h; t->levels = 1; t->levelsDef = 1; t->has = true; t->started = false; t->prof = true; t->useCmd = 0;
    }
    FinishPass();
    VkCommandBuffer cb = Cmd();
    Barrier(cb, gPan.prof, ad, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    Barrier(cb, t->img, ad, t->started ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    VkImageCopy c; memset(&c, 0, sizeof(c));
    c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = ad;
    c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
    c.srcOffset.x = cx; c.srcOffset.y = d.flipped ? d.h - cy - ch : cy;
    // (en la textura, la fila 0 es la de ARRIBA del rectangulo pedido)
    c.dstOffset.x = cx - x; c.dstOffset.y = d.flipped ? (y + h) - (cy + ch) : cy - y;
    c.extent.width = (uint32_t)cw; c.extent.height = (uint32_t)ch; c.extent.depth = 1;
    vkCmdCopyImage(cb, gPan.prof, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Barrier(cb, t->img, ad, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    Barrier(cb, gPan.prof, ad, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
    t->started = true;
    t->useCmd = gSerialCmd;
    return true;
}
void vkglReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* px) {
    if (!gReady || w <= 0 || h <= 0 || !px) return;
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) { ErrorGL(GL_INVALID_ENUM); return; }
    const Destination d = CurrentDestination();
    unsigned char* out = (unsigned char*)px;
    memset(out, 0, (size_t)w * h * 4);
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > d.w) x1 = d.w; if (y1 > d.h) y1 = d.h;
    if (x1 <= x0 || y1 <= y0) return;
    const int cw = x1 - x0, ch = y1 - y0;
    FinishPass();
    VkCommandBuffer cb = Cmd();
    Ring r = Request((VkDeviceSize)cw * ch * 4, 16);
    if (!r.b) return;
    const VkImageLayout turn = OpenReading(cb, d);
    VkBufferImageCopy c; memset(&c, 0, sizeof(c));
    c.bufferOffset = r.off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = x0; c.imageOffset.y = d.flipped ? d.h - y1 : y0;
    c.imageExtent.width = (uint32_t)cw; c.imageExtent.height = (uint32_t)ch; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, d.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.b, 1, &c);
    Barrier(cb, d.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, turn, 0, 1);
    SendAndWait();
    for (int fy = 0; fy < ch; fy++) {   // (GL: la fila de abajo primero)
        const int src = d.flipped ? ch - 1 - fy : fy;
        memcpy(out + ((size_t)(y0 - y + fy) * w + (x0 - x)) * 4, r.p + (size_t)src * cw * 4, (size_t)cw * 4);
    }
    RecycleAll();
}

// ============================================================================
//  Framebuffers
// ============================================================================
void vkglGenFramebuffers(GLsizei n, GLuint* f) { for (int i = 0; i < n; i++) { f[i] = gNextFbo++; gFbos[f[i]] = 0; } }
void vkglDeleteFramebuffers(GLsizei n, const GLuint* f) {
    for (int i = 0; i < n; i++) {
        if (gCurrentFbo == f[i]) vkglBindFramebuffer(GL_FRAMEBUFFER, 0);
        gFbos.erase(f[i]); gFboProf.erase(f[i]);
        if (gReady) { std::map<GLuint, FbProf>::iterator it = gFbProfVk.find(f[i]);
                      if (it != gFbProfVk.end()) { if (it->second.fb) { Garbage b = EmptyGarbage(); b.fb = it->second.fb; Throw(b); } gFbProfVk.erase(it); } }
    }
}
void vkglBindFramebuffer(GLenum, GLuint f) { if (f == gCurrentFbo) return; FinishPass(); gCurrentFbo = f; }
void vkglFramebufferTexture2D(GLenum, GLenum attached, GLenum, GLuint t, GLint) {
    if (!gCurrentFbo || attached != GL_COLOR_ATTACHMENT0) return;
    FinishPass();
    gFbos[gCurrentFbo] = t;
}
// ---- renderbuffers: solo de PROFUNDIDAD (con estencil: el formato de la pantalla, gFmtProf) ----
static void ThrowFbProf(GLuint fbo) {
    std::map<GLuint, FbProf>::iterator it = gFbProfVk.find(fbo);
    if (it == gFbProfVk.end()) return;
    if (it->second.fb) { Garbage b = EmptyGarbage(); b.fb = it->second.fb; Throw(b); }
    gFbProfVk.erase(it);
}
static void ThrowRb(RbGL* r) {
    if (!r || !r->img) return;
    Garbage b = EmptyGarbage(); b.img = r->img; b.view = r->v; b.a = r->a; Throw(b);
    r->img = VK_NULL_HANDLE; r->v = VK_NULL_HANDLE; r->a.b = 0; r->w = r->h = 0;
}
static VkFramebuffer FbWithProf(GLuint fbo, TexGL* t, RbGL* rb) {
    std::map<GLuint, FbProf>::iterator it = gFbProfVk.find(fbo);
    if (it != gFbProfVk.end()) {
        if (it->second.vc == t->view0 && it->second.vp == rb->v) return it->second.fb;
        ThrowFbProf(fbo);   // (la textura o el renderbuffer son otros)
    }
    VkImageView vs[2] = { t->view0, rb->v };
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = gRpDef; fi.attachmentCount = 2; fi.pAttachments = vs;
    fi.width = (uint32_t)t->w; fi.height = (uint32_t)t->h; fi.layers = 1;
    FbProf f; memset(&f, 0, sizeof(f));
    if (vkCreateFramebuffer(gDev, &fi, 0, &f.fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    f.vc = t->view0; f.vp = rb->v;
    gFbProfVk[fbo] = f;
    return f.fb;
}
void vkglGenRenderbuffers(GLsizei n, GLuint* r) {
    for (int i = 0; i < n; i++) { RbGL* x = new RbGL(); memset(x, 0, sizeof(*x)); r[i] = gNextRb++; gRbs[r[i]] = x; }
}
void vkglDeleteRenderbuffers(GLsizei n, const GLuint* r) {
    for (int i = 0; i < n; i++) {
        std::map<GLuint, RbGL*>::iterator it = gRbs.find(r[i]);
        if (!r[i] || it == gRbs.end()) continue;
        for (std::map<GLuint, GLuint>::iterator f = gFboProf.begin(); f != gFboProf.end(); ++f)
            if (f->second == r[i]) { if (f->first == gCurrentFbo) FinishPass(); ThrowFbProf(f->first); f->second = 0; }
        if (gReady) ThrowRb(it->second);
        delete it->second; gRbs.erase(it);
        if (gCurrentRb == r[i]) gCurrentRb = 0;
    }
}
void vkglBindRenderbuffer(GLenum, GLuint r) { gCurrentRb = r; }
void vkglRenderbufferStorage(GLenum, GLenum, GLsizei w, GLsizei h) {
    std::map<GLuint, RbGL*>::iterator it = gRbs.find(gCurrentRb);
    if (it == gRbs.end() || !gReady || w <= 0 || h <= 0) return;
    RbGL* r = it->second;
    if (r->img && r->w == w && r->h == h) return;
    for (std::map<GLuint, GLuint>::iterator f = gFboProf.begin(); f != gFboProf.end(); ++f)
        if (f->second == gCurrentRb) { if (f->first == gCurrentFbo) FinishPass(); ThrowFbProf(f->first); }
    ThrowRb(r);
    if (!CreateImage(gFmtProf, w, h, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                     r->img, r->a, r->v)) { ErrorGL(GL_OUT_OF_MEMORY); r->img = VK_NULL_HANDLE; return; }
    r->w = w; r->h = h;
    // (en el layout de siempre, como la de la pantalla: el render pass la carga y la deja asi)
    Barrier(CmdPre(), r->img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}
void vkglFramebufferRenderbuffer(GLenum, GLenum attached, GLenum, GLuint r) {
    if (!gCurrentFbo || (attached != GL_DEPTH_ATTACHMENT && attached != 0x821A /*GL_DEPTH_STENCIL_ATTACHMENT*/)) return;
    FinishPass();
    ThrowFbProf(gCurrentFbo);
    if (r) gFboProf[gCurrentFbo] = r; else gFboProf.erase(gCurrentFbo);
}
GLenum vkglCheckFramebufferStatus(GLenum) {
    if (!gCurrentFbo) return GL_FRAMEBUFFER_COMPLETE;
    TexGL* t = TexOf(gFbos[gCurrentFbo]);
    return (t && t->has && FboOfTexture(t)) ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
}

// ============================================================================
//  LA PANTALLA (la imagen propia) y LA VENTANA (la swapchain)
// ============================================================================
static void DestroyScreen() {
    if (gPan.fb) vkDestroyFramebuffer(gDev, gPan.fb, 0);
    if (gPan.vc) vkDestroyImageView(gDev, gPan.vc, 0);
    if (gPan.vp) vkDestroyImageView(gDev, gPan.vp, 0);
    if (gPan.color) vkDestroyImage(gDev, gPan.color, 0);
    if (gPan.prof) vkDestroyImage(gDev, gPan.prof, 0);
    Free(gPan.ac); Free(gPan.ap);
    memset(&gPan, 0, sizeof(gPan));
}
static bool CreateImage(VkFormat f, int w, int h, VkImageUsageFlags use, VkImageAspectFlags asp, VkImage& img, Assign& a, VkImageView& v) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f; ii.extent.width = (uint32_t)w; ii.extent.height = (uint32_t)h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = use;
    if (vkCreateImage(gDev, &ii, 0, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements r; vkGetImageMemoryRequirements(gDev, img, &r);
    if (!Reserve(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, a)) return false;
    vkBindImageMemory(gDev, img, a.b->mem, a.off);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
    vi.subresourceRange.aspectMask = asp; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(gDev, &vi, 0, &v) == VK_SUCCESS;
}
static bool CreateScreen(int w, int h) {
    DestroyScreen();
    if (w <= 0 || h <= 0) return false;
    if (!CreateImage(kFmtColor, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, gPan.color, gPan.ac, gPan.vc)) return false;
    // (TRANSFER_SRC: la profundidad se copia a una textura para los reflejos en pantalla, vkglCopyDepth)
    if (!CreateImage(gFmtProf, w, h, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, gPan.prof, gPan.ap, gPan.vp)) return false;
    VkImageView vs[2] = { gPan.vc, gPan.vp };
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = gRpDef; fi.attachmentCount = 2; fi.pAttachments = vs; fi.width = (uint32_t)w; fi.height = (uint32_t)h; fi.layers = 1;
    if (vkCreateFramebuffer(gDev, &fi, 0, &gPan.fb) != VK_SUCCESS) return false;
    gPan.w = w; gPan.h = h;
    // en negro y la profundidad al fondo, en los layouts de siempre
    VkCommandBuffer cb = CmdPre();
    Barrier(cb, gPan.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black; memset(&black, 0, sizeof(black)); black.float32[3] = 1.0f;
    VkImageSubresourceRange rc = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, gPan.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rc);
    Barrier(cb, gPan.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkImageAspectFlags ad = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    Barrier(cb, gPan.prof, ad, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue background = { 1.0f, 0 };
    VkImageSubresourceRange rd = { ad, 0, 1, 0, 1 };
    vkCmdClearDepthStencilImage(cb, gPan.prof, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1, &rd);
    Barrier(cb, gPan.prof, ad, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    return true;
}
static void DestroySwapchain() {
    for (size_t i = 0; i < gScSem.size(); i++) vkDestroySemaphore(gDev, gScSem[i], 0);
    gScSem.clear(); gScImgs.clear();
    if (gSc) vkDestroySwapchainKHR(gDev, gSc, 0);
    gSc = VK_NULL_HANDLE;
}
static bool CreateSwapchain() {
    VkSurfaceCapabilitiesKHR cap;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gPhys, gSup, &cap) != VK_SUCCESS) return false;
    VkExtent2D ext = cap.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, h = 0; gShow.size(gShow.user, &w, &h);
        ext.width = (uint32_t)w; ext.height = (uint32_t)h;
    }
    if (ext.width == 0 || ext.height == 0) { DestroySwapchain(); gScSize = ext; return false; }   // (minimizada)
    if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { w3dLogfE("[vk] la ventana no acepta copias"); return false; }
    uint32_t n = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(gPhys, gSup, &n, 0);
    std::vector<VkSurfaceFormatKHR> fmts(n ? n : 1); vkGetPhysicalDeviceSurfaceFormatsKHR(gPhys, gSup, &n, &fmts[0]);
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < n; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { fmt = fmts[i]; break; }
    uint32_t nm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(gPhys, gSup, &nm, 0);
    std::vector<VkPresentModeKHR> modes(nm ? nm : 1); vkGetPhysicalDeviceSurfacePresentModesKHR(gPhys, gSup, &nm, &modes[0]);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!gVsync) {
        for (uint32_t i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
        if (mode == VK_PRESENT_MODE_FIFO_KHR) for (uint32_t i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    }
    VkSwapchainCreateInfoKHR yes = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    yes.surface = gSup;
    yes.minImageCount = cap.minImageCount + 1;
    if (cap.maxImageCount && yes.minImageCount > cap.maxImageCount) yes.minImageCount = cap.maxImageCount;
    yes.imageFormat = fmt.format; yes.imageColorSpace = fmt.colorSpace; yes.imageExtent = ext; yes.imageArrayLayers = 1;
    yes.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; yes.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    yes.preTransform = cap.currentTransform;
    yes.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(cap.supportedCompositeAlpha & yes.compositeAlpha))
        for (uint32_t b = 1; b; b <<= 1) if (cap.supportedCompositeAlpha & b) { yes.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
    yes.presentMode = mode; yes.clipped = VK_TRUE; yes.oldSwapchain = gSc;
    VkSwapchainKHR newValue = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(gDev, &yes, 0, &newValue);
    DestroySwapchain();
    if (r != VK_SUCCESS) { w3dLogfE("[vk] vkCreateSwapchainKHR: %d", (int)r); return false; }
    gSc = newValue; gScSize = ext; gScFmt = fmt.format;
    uint32_t ni = 0; vkGetSwapchainImagesKHR(gDev, gSc, &ni, 0);
    gScImgs.resize(ni); vkGetSwapchainImagesKHR(gDev, gSc, &ni, &gScImgs[0]);
    gScSem.resize(ni);
    for (uint32_t i = 0; i < ni; i++) { VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; vkCreateSemaphore(gDev, &s, 0, &gScSem[i]); }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(gPhys, gScFmt, &fp);
    gScBlit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
    return true;
}
// el tamanio de la ventana cambio (o la swapchain quedo vieja): todo de nuevo
static void Redo() {
    SendAndWait(); RecycleAll();
    vkDeviceWaitIdle(gDev);
    CreateSwapchain();
    int w = 0, h = 0; gShow.size(gShow.user, &w, &h);
    if (w > 0 && h > 0 && (w != gPan.w || h != gPan.h)) {
        CreateScreen(w, h);
        // (GL: la ventana nueva arranca con el viewport y el scissor que tenia; el motor los vuelve a poner)
    }
    // la limpieza de la pantalla nueva (CreateScreen la graba en el buffer "previo" de ESTE cuadro) va YA: el que llama
    // pasa enseguida al cuadro siguiente (NewFrame). Antes quedaba grabada sin enviar y con gPreRecording prendido, y
    // el cuadro siguiente mandaba a la GPU un buffer de comandos que nunca se empezo a grabar: al cambiar el tamanio de
    // la ventana se caia (lavapipe) o colgaba la GPU (Intel: el escritorio entero congelado)
    SendAndWait();
    gScRedo = false;
}
static void NewFrame() {
    gCur = (gCur + 1) % kFrames;
    Frame& c = gFrames[gCur];
    if (c.inFlight) { vkWaitForFences(gDev, 1, &c.fence, VK_TRUE, UINT64_MAX); c.inFlight = false; }
    Restart(c);
}
void VkGLPresent() {
    if (!gReady) return;
    int w = 0, h = 0; gShow.size(gShow.user, &w, &h);
    if (w > 0 && h > 0 && (w != gPan.w || h != gPan.h)) gScRedo = true;
    if (!gSc || gScRedo) {
        // (lo dibujado en este cuadro era para el tamanio de antes: se descarta)
        Redo();
        if (!gSc) return;
        NewFrame();
        return;
    }
    Frame& c = gFrames[gCur];
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(gDev, gSc, UINT64_MAX, c.semAdq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)) {
        SendAndWait(); RecycleAll();
        gScRedo = true;
        return;
    }
    FinishPass();
    VkCommandBuffer cb = Cmd();
    Barrier(cb, gPan.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Barrier(cb, gScImgs[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (gScBlit) {
        VkImageBlit b; memset(&b, 0, sizeof(b));
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = gPan.w; b.srcOffsets[1].y = gPan.h; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = (int32_t)gScSize.width; b.dstOffsets[1].y = (int32_t)gScSize.height; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, gPan.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gScImgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    } else {
        VkImageCopy cp; memset(&cp, 0, sizeof(cp));
        cp.srcSubresource.aspectMask = cp.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cp.srcSubresource.layerCount = cp.dstSubresource.layerCount = 1;
        cp.extent.width = (uint32_t)(gPan.w < (int)gScSize.width ? gPan.w : (int)gScSize.width);
        cp.extent.height = (uint32_t)(gPan.h < (int)gScSize.height ? gPan.h : (int)gScSize.height); cp.extent.depth = 1;
        vkCmdCopyImage(cb, gPan.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gScImgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    }
    Barrier(cb, gScImgs[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    Barrier(cb, gPan.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (!Send(c.semAdq, gScSem[idx])) { vkDeviceWaitIdle(gDev); RecycleAll(); return; }
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &gScSem[idx];
    pi.swapchainCount = 1; pi.pSwapchains = &gSc; pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(gQueue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) gScRedo = true;
    NewFrame();
}
void VkGLVsync(int interval) {
    const int v = interval ? 1 : 0;
    if (v == gVsync) return;
    gVsync = v;
    gScRedo = true;
}
bool VkGLReady() { return gReady; }
const char* VkGLDevice() { return gGPUName.c_str(); }

// ============================================================================
//  ARRANCAR
// ============================================================================
static VKAPI_ATTR VkBool32 VKAPI_CALL NoticeValidation(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) w3dLogfW("[vk validacion] %s", d->pMessage);
    return VK_FALSE;
}
static bool HasExtension(const std::vector<VkExtensionProperties>& l, const char* n) {
    for (size_t i = 0; i < l.size(); i++) if (strcmp(l[i].extensionName, n) == 0) return true;
    return false;
}
static bool Fault(std::string* err, const char* msg) { if (err) *err = msg; w3dLogfW("[vk] %s", msg); VkGLFinish(); return false; }
bool VkGLStart(const W3dVkWindow& v, std::string* err) {
    if (gReady) return true;
    gShow = v;
    vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)v.getInstanceProcAddr;
    if (!vkGetInstanceProcAddr_) return Fault(err, "sin el cargador de Vulkan");
    if (!OpenShaderc()) return Fault(err, "sin shaderc_shared (el compilador de shaders de Vulkan): va al lado del ejecutable");
#define W3D_VK_LOAD_G(n) n = (PFN_##n)vkGetInstanceProcAddr_(VK_NULL_HANDLE, #n); if (!n) return Fault(err, "cargador de Vulkan incompleto: " #n);
    W3D_VK_GLOBAL(W3D_VK_LOAD_G)
#undef W3D_VK_LOAD_G
    vkEnumerateInstanceVersion_ = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr_(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t showInst = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion_) vkEnumerateInstanceVersion_(&showInst);
    // ---- la instancia (con la validacion si se pide: W3D_VK_VALIDATION=1, del SDK) ----
    std::vector<const char*> exts(v.extensions, v.extensions + v.nExtensions);
    std::vector<const char*> layers;
    const char* requestsVal = getenv("W3D_VK_VALIDATION");
    if (requestsVal && requestsVal[0] == '1') {
        uint32_t n = 0; vkEnumerateInstanceLayerProperties(&n, 0);
        std::vector<VkLayerProperties> l(n ? n : 1); vkEnumerateInstanceLayerProperties(&n, &l[0]);
        for (uint32_t i = 0; i < n; i++) if (strcmp(l[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
        uint32_t ne = 0; vkEnumerateInstanceExtensionProperties(0, &ne, 0);
        std::vector<VkExtensionProperties> le(ne ? ne : 1); vkEnumerateInstanceExtensionProperties(0, &ne, &le[0]); le.resize(ne);
        if (HasExtension(le, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        w3dLogf("[vk] validacion: %s", layers.empty() ? "no esta (instalar el SDK de Vulkan)" : "on");
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Whisk3D"; app.pEngineName = "Whisk3DCore";
    app.apiVersion = showInst >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size(); ici.ppEnabledExtensionNames = exts.empty() ? 0 : &exts[0];
    ici.enabledLayerCount = (uint32_t)layers.size(); ici.ppEnabledLayerNames = layers.empty() ? 0 : &layers[0];
    if (vkCreateInstance(&ici, 0, &gInst) != VK_SUCCESS) return Fault(err, "vkCreateInstance failed (driver sin Vulkan)");
#define W3D_VK_LOAD_I(n) n = (PFN_##n)vkGetInstanceProcAddr_(gInst, #n); if (!n) return Fault(err, "instancia de Vulkan incompleta: " #n);
    W3D_VK_INST(W3D_VK_LOAD_I)
#undef W3D_VK_LOAD_I
    if (!layers.empty()) {
        vkCreateUsefulDebugMessengerEXT_ = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr_(gInst, "vkCreateDebugUtilsMessengerEXT");
        vkUsefulDestroyDebugMessengerEXT_ = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr_(gInst, "vkDestroyDebugUtilsMessengerEXT");
        if (vkCreateUsefulDebugMessengerEXT_) {
            VkDebugUtilsMessengerCreateInfoEXT di = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            di.pfnUserCallback = NoticeValidation;
            vkCreateUsefulDebugMessengerEXT_(gInst, &di, 0, &gDebug);
        }
    }
    if (!v.createSurface(v.user, (void*)gInst, (void*)&gSup) || !gSup) return Fault(err, "no se pudo crear la superficie de la ventana");
    // ---- la GPU: la que dibuja y presenta en esta ventana, la dedicada antes que la integrada (W3D_VK_GPU=n elige) ----
    uint32_t nf = 0; vkEnumeratePhysicalDevices(gInst, &nf, 0);
    if (!nf) return Fault(err, "ninguna GPU con Vulkan");
    std::vector<VkPhysicalDevice> phys(nf); vkEnumeratePhysicalDevices(gInst, &nf, &phys[0]);
    const char* requestsGpu = getenv("W3D_VK_GPU");
    int best = -1, points = -1; uint32_t bestFam = 0;
    for (uint32_t i = 0; i < nf; i++) {
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys[i], 0, &ne, 0);
        std::vector<VkExtensionProperties> le(ne ? ne : 1); vkEnumerateDeviceExtensionProperties(phys[i], 0, &ne, &le[0]); le.resize(ne);
        if (!HasExtension(le, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(phys[i], &pr);
        if (pr.apiVersion < VK_API_VERSION_1_1 && !HasExtension(le, VK_KHR_MAINTENANCE1_EXTENSION_NAME)) continue;   // (el viewport dado vuelta)
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, 0);
        std::vector<VkQueueFamilyProperties> q(nq ? nq : 1); vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, &q[0]);
        for (uint32_t f = 0; f < nq; f++) {
            VkBool32 pres = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(phys[i], f, gSup, &pres);
            if (!(q[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !pres) continue;
            int p = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (requestsGpu && atoi(requestsGpu) == (int)i) p = 100;
            if (p > points) { points = p; best = (int)i; bestFam = f; }
            break;
        }
    }
    if (best < 0) return Fault(err, "ninguna GPU puede dibujar en esta ventana con Vulkan");
    gPhys = phys[best]; gFamily = bestFam;
    vkGetPhysicalDeviceProperties(gPhys, &gProps);
    vkGetPhysicalDeviceMemoryProperties(gPhys, &gMemProps);
    gGPUName = gProps.deviceName;
    VkPhysicalDeviceFeatures has; vkGetPhysicalDeviceFeatures(gPhys, &has);
    memset(&gFeat, 0, sizeof(gFeat));
    gFeat.wideLines = has.wideLines; gFeat.largePoints = has.largePoints; gFeat.samplerAnisotropy = has.samplerAnisotropy;
    // ---- el dispositivo ----
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = gFamily; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    std::vector<const char*> dext; dext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (gProps.apiVersion < VK_API_VERSION_1_1 || app.apiVersion < VK_API_VERSION_1_1) dext.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (uint32_t)dext.size(); dci.ppEnabledExtensionNames = &dext[0];
    dci.pEnabledFeatures = &gFeat;
    if (vkCreateDevice(gPhys, &dci, 0, &gDev) != VK_SUCCESS) return Fault(err, "vkCreateDevice fallo");
#define W3D_VK_LOAD_D(n) n = (PFN_##n)vkGetDeviceProcAddr(gDev, #n); if (!n) return Fault(err, "dispositivo de Vulkan incompleto: " #n);
    W3D_VK_DEV(W3D_VK_LOAD_D)
#undef W3D_VK_LOAD_D
    vkGetDeviceQueue(gDev, gFamily, 0, &gQueue);
    gAlignUBO = gProps.limits.minUniformBufferOffsetAlignment > 16 ? gProps.limits.minUniformBufferOffsetAlignment : 16;
    // ---- shaderc ----
    gShc = shc_compiler_initialize();
    gShcOpt = shc_compile_options_initialize();
    if (!gShc || !gShcOpt) return Fault(err, "shaderc no inicio");
    shc_compile_options_set_target_env(gShcOpt, kShcVulkan, kShcVulkan10);
    shc_compile_options_set_optimization_level(gShcOpt, kShcOptimPerformance);
    // ---- la profundidad/estencil que tenga esta GPU ----
    const VkFormat profs[3] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT };
    for (int i = 0; i < 3 && gFmtProf == VK_FORMAT_UNDEFINED; i++) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(gPhys, profs[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) gFmtProf = profs[i];
    }
    if (gFmtProf == VK_FORMAT_UNDEFINED) return Fault(err, "sin formato de profundidad con estencil");
    // los formatos de atributo que usa el motor: si alguno no se lee, los buffers guardan su copia para convertirlo
    { const VkFormat f[5] = { VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R8G8B8A8_UNORM };
      for (int i = 0; i < 5; i++) if (!FormatReadable(f[i])) gSaveCopies = true; }
    // ---- los render passes (la pantalla: color + profundidad; una textura: solo color). Cargan y guardan: GL no borra ----
    {
        VkAttachmentDescription at[2]; memset(at, 0, sizeof(at));
        at[0].format = kFmtColor; at[0].samples = VK_SAMPLE_COUNT_1_BIT;
        at[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[0].initialLayout = at[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        at[1] = at[0]; at[1].format = gFmtProf;
        at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[1].initialLayout = at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference rc = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, rd = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp; memset(&sp, 0, sizeof(sp));
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &rc; sp.pDepthStencilAttachment = &rd;
        VkSubpassDependency dep[2]; memset(dep, 0, sizeof(dep));
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
        dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        for (int i = 0; i < 2; i++) {
            dep[i].srcStageMask = dep[i].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            dep[i].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; dep[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        }
        VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        ri.attachmentCount = 2; ri.pAttachments = at; ri.subpassCount = 1; ri.pSubpasses = &sp; ri.dependencyCount = 2; ri.pDependencies = dep;
        if (vkCreateRenderPass(gDev, &ri, 0, &gRpDef) != VK_SUCCESS) return Fault(err, "vkCreateRenderPass");
        sp.pDepthStencilAttachment = 0; ri.attachmentCount = 1;
        if (vkCreateRenderPass(gDev, &ri, 0, &gRpColor) != VK_SUCCESS) return Fault(err, "vkCreateRenderPass");
    }
    // ---- el layout de todos los programas: binding 0 los uniforms, 1..16 las texturas ----
    {
        VkDescriptorSetLayoutBinding b[1 + kSamples]; memset(b, 0, sizeof(b));
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        for (int i = 1; i <= kSamples; i++) {
            b[i].binding = (uint32_t)i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1 + kSamples; li.pBindings = b;
        if (vkCreateDescriptorSetLayout(gDev, &li, 0, &gSetLayout) != VK_SUCCESS) return Fault(err, "vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pi.setLayoutCount = 1; pi.pSetLayouts = &gSetLayout;
        if (vkCreatePipelineLayout(gDev, &pi, 0, &gLayout) != VK_SUCCESS) return Fault(err, "vkCreatePipelineLayout");
        VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        vkCreatePipelineCache(gDev, &ci, 0, &gPipeCache);
    }
    // ---- los cuadros ----
    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = gFamily;
        if (vkCreateCommandPool(gDev, &pi, 0, &gPool) != VK_SUCCESS) return Fault(err, "vkCreateCommandPool");
        for (int i = 0; i < kFrames; i++) {
            Frame& c = gFrames[i];
            VkCommandBuffer cbs[2];
            VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = gPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 2;
            if (vkAllocateCommandBuffers(gDev, &ai, cbs) != VK_SUCCESS) return Fault(err, "vkAllocateCommandBuffers");
            c.cmd = cbs[0]; c.cmdPre = cbs[1];
            VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            vkCreateFence(gDev, &fi, 0, &c.fence);
            VkSemaphoreCreateInfo yes = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            vkCreateSemaphore(gDev, &yes, 0, &c.semAdq);
            c.inFlight = false; c.chunk = 0; c.pool = 0;
        }
    }
    gReady = true;
    // ---- la ventana y la pantalla ----
    int w = 0, h = 0; v.size(v.user, &w, &h);
    CreateSwapchain();
    if (!CreateScreen(w > 0 ? w : 1, h > 0 ? h : 1)) return Fault(err, "no se pudo crear la imagen de la pantalla");
    FactoryState(w, h);
    memset(gAttr, 0, sizeof(gAttr));
    for (int i = 0; i < kAttrs; i++) gAttr[i].cte[3] = 1.0f;
    memset(gUnit, 0, sizeof(gUnit)); gActiveUnit = 0;
    // la textura de una unidad vacia: negro opaco (lo que da GL con una textura incompleta)
    gBlack = NewTex(); gBlack->minF = GL_NEAREST; gBlack->magF = GL_NEAREST;
    { const unsigned char black[4] = { 0, 0, 0, 255 };
      if (!CreateImageTex(gBlack, 1, 1)) return Fault(err, "no se pudo crear la textura vacia");
      Upload(gBlack, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black); }
    w3dLogf("[vk] %s, Vulkan %u.%u.%u (driver %u), %dx%d%s", gGPUName.c_str(), VK_API_VERSION_MAJOR(gProps.apiVersion),
            VK_API_VERSION_MINOR(gProps.apiVersion), VK_API_VERSION_PATCH(gProps.apiVersion), gProps.driverVersion, w, h,
            gSaveCopies ? " (converts formats of atributos)" : "");
    return true;
}
void VkGLFinish() {
    if (gDev && vkDeviceWaitIdle && vkDestroyDevice) {   // (si no llego a cargar sus funciones, queda sin cerrar)
        vkDeviceWaitIdle(gDev);
        for (int i = 0; i < kFrames; i++) {
            Frame& c = gFrames[i];
            FreeGarbage(c);
            for (size_t k = 0; k < c.chunks.size(); k++) { vkDestroyBuffer(gDev, c.chunks[k].buf.b, 0); }
            c.chunks.clear();
            for (size_t k = 0; k < c.pools.size(); k++) vkDestroyDescriptorPool(gDev, c.pools[k], 0);
            c.pools.clear();
            if (c.fence) vkDestroyFence(gDev, c.fence, 0);
            if (c.semAdq) vkDestroySemaphore(gDev, c.semAdq, 0);
            c.fence = VK_NULL_HANDLE; c.semAdq = VK_NULL_HANDLE;
        }
        for (std::map<GLuint, ProgGL*>::iterator it = gProgs.begin(); it != gProgs.end(); ++it) {
            for (std::map<KeyPipe, VkPipeline>::iterator p = it->second->pipes.begin(); p != it->second->pipes.end(); ++p) vkDestroyPipeline(gDev, p->second, 0);
            if (it->second->mVS) vkDestroyShaderModule(gDev, it->second->mVS, 0);
            if (it->second->mFS) vkDestroyShaderModule(gDev, it->second->mFS, 0);
            delete it->second;
        }
        gProgs.clear(); gProg = 0;
        for (std::map<GLuint, TexGL*>::iterator it = gTex.begin(); it != gTex.end(); ++it) {
            TexGL* t = it->second;
            if (t->fb) vkDestroyFramebuffer(gDev, t->fb, 0);
            if (t->view0) vkDestroyImageView(gDev, t->view0, 0);
            if (t->view) vkDestroyImageView(gDev, t->view, 0);
            if (t->img) vkDestroyImage(gDev, t->img, 0);
            delete t;
        }
        gTex.clear();
        for (std::map<GLuint, FbProf>::iterator it = gFbProfVk.begin(); it != gFbProfVk.end(); ++it)
            if (it->second.fb) vkDestroyFramebuffer(gDev, it->second.fb, 0);
        gFbProfVk.clear(); gFboProf.clear();
        for (std::map<GLuint, RbGL*>::iterator it = gRbs.begin(); it != gRbs.end(); ++it) {
            if (it->second->v) vkDestroyImageView(gDev, it->second->v, 0);
            if (it->second->img) vkDestroyImage(gDev, it->second->img, 0);
            delete it->second;
        }
        gRbs.clear();
        if (gBlack) {
            if (gBlack->view) vkDestroyImageView(gDev, gBlack->view, 0);
            if (gBlack->img) vkDestroyImage(gDev, gBlack->img, 0);
            delete gBlack; gBlack = 0;
        }
        for (std::map<GLuint, BufGL*>::iterator it = gBufs.begin(); it != gBufs.end(); ++it) {
            if (it->second->vk.b) vkDestroyBuffer(gDev, it->second->vk.b, 0);
            for (std::map<uint64_t, BufVk>::iterator c = it->second->conv.begin(); c != it->second->conv.end(); ++c) vkDestroyBuffer(gDev, c->second.b, 0);
            delete it->second;
        }
        gBufs.clear();
        for (std::map<uint32_t, VkSampler>::iterator it = gSamplers.begin(); it != gSamplers.end(); ++it) vkDestroySampler(gDev, it->second, 0);
        gSamplers.clear();
        DestroyScreen();
        DestroySwapchain();
        if (gPool) vkDestroyCommandPool(gDev, gPool, 0);
        if (gPipeCache) vkDestroyPipelineCache(gDev, gPipeCache, 0);
        if (gLayout) vkDestroyPipelineLayout(gDev, gLayout, 0);
        if (gSetLayout) vkDestroyDescriptorSetLayout(gDev, gSetLayout, 0);
        if (gRpDef) vkDestroyRenderPass(gDev, gRpDef, 0);
        if (gRpColor) vkDestroyRenderPass(gDev, gRpColor, 0);
        for (size_t i = 0; i < gBlocks.size(); i++) { vkFreeMemory(gDev, gBlocks[i]->mem, 0); delete gBlocks[i]; }
        gBlocks.clear();
        vkDestroyDevice(gDev, 0);
    }
    if (gInst) {
        if (gSup && vkDestroySurfaceKHR) vkDestroySurfaceKHR(gInst, gSup, 0);
        if (gDebug && vkUsefulDestroyDebugMessengerEXT_) vkUsefulDestroyDebugMessengerEXT_(gInst, gDebug, 0);
        if (vkDestroyInstance) vkDestroyInstance(gInst, 0);
    }
    if (gShcOpt) shc_compile_options_release(gShcOpt);
    if (gShc) shc_compiler_release(gShc);
    gShcOpt = 0; gShc = 0;
    gDev = VK_NULL_HANDLE; gInst = VK_NULL_HANDLE; gSup = VK_NULL_HANDLE; gDebug = VK_NULL_HANDLE; gPhys = VK_NULL_HANDLE;
    gSc = VK_NULL_HANDLE; gPool = VK_NULL_HANDLE; gPipeCache = VK_NULL_HANDLE; gLayout = VK_NULL_HANDLE; gSetLayout = VK_NULL_HANDLE;
    gRpDef = gRpColor = VK_NULL_HANDLE; gFmtProf = VK_FORMAT_UNDEFINED;
    gRecording = gPreRecording = gInPass = false;
    gReady = false;
}
