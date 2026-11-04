// ============================================================================
//  w3dVkGL.cpp - OPENGL ES 2.0 HECHO CON VULKAN. Es lo que usan el backend de shaders (gles2/w3dGraphicsGLES2.cpp,
//  compilado otra vez como w3dEngine::vk en vk/w3dGraphicsVk.cpp) y el cargador de texturas (gfx/w3dTexture.cpp): las
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
static PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT_ = 0;
static PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT_ = 0;

// ============================================================================
//  shaderc (GLSL -> SPIR-V), abierto a mano: la API de C es estable y chica
// ============================================================================
typedef void* ShcCompilador; typedef void* ShcOpciones; typedef void* ShcResultado;
static ShcCompilador (*shc_compiler_initialize)(void) = 0;
static void (*shc_compiler_release)(ShcCompilador) = 0;
static ShcOpciones (*shc_compile_options_initialize)(void) = 0;
static void (*shc_compile_options_release)(ShcOpciones) = 0;
static void (*shc_compile_options_set_target_env)(ShcOpciones, int, uint32_t) = 0;
static void (*shc_compile_options_set_optimization_level)(ShcOpciones, int) = 0;
static ShcResultado (*shc_compile_into_spv)(ShcCompilador, const char*, size_t, int, const char*, const char*, ShcOpciones) = 0;
static ShcResultado (*shc_compile_into_preprocessed_text)(ShcCompilador, const char*, size_t, int, const char*, const char*, ShcOpciones) = 0;
static void (*shc_result_release)(ShcResultado) = 0;
static size_t (*shc_result_get_length)(ShcResultado) = 0;
static const char* (*shc_result_get_bytes)(ShcResultado) = 0;
static int (*shc_result_get_compilation_status)(ShcResultado) = 0;
static const char* (*shc_result_get_error_message)(ShcResultado) = 0;
enum { kShcVertex = 0, kShcFragment = 1, kShcVulkan = 0, kShcOptimRendimiento = 2 };
static const uint32_t kShcVulkan10 = 1u << 22;
static ShcCompilador gShc = 0;
static ShcOpciones gShcOpc = 0;

static void* AbrirLib(const char* nombre) {
#ifdef _WIN32
    return (void*)LoadLibraryA(nombre);
#else
    return dlopen(nombre, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void* Simbolo(void* lib, const char* n) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, n);
#else
    return dlsym(lib, n);
#endif
}
// shaderc_shared: al lado del ejecutable (lo copia el build) o en el SDK de Vulkan instalado
static void* gLibShc = 0;
static bool AbrirShaderc() {
    if (gLibShc) return true;
#ifdef _WIN32
    const char* nombres[] = { "shaderc_shared.dll" };
    const char* sub = "\\Bin\\shaderc_shared.dll";
#else
    const char* nombres[] = { "libshaderc_shared.so.1", "libshaderc_shared.so" };
    const char* sub = "/lib/libshaderc_shared.so";
#endif
    for (size_t i = 0; i < sizeof(nombres) / sizeof(nombres[0]) && !gLibShc; i++) gLibShc = AbrirLib(nombres[i]);
    if (!gLibShc) { const char* sdk = getenv("VULKAN_SDK"); if (sdk) gLibShc = AbrirLib((std::string(sdk) + sub).c_str()); }
    if (!gLibShc) return false;
#define W3D_SHC(n) *(void**)&shc_##n = Simbolo(gLibShc, "shaderc_" #n); if (!shc_##n) { gLibShc = 0; return false; }
    W3D_SHC(compiler_initialize) W3D_SHC(compiler_release) W3D_SHC(compile_options_initialize)
    W3D_SHC(compile_options_release) W3D_SHC(compile_options_set_target_env) W3D_SHC(compile_options_set_optimization_level)
    W3D_SHC(compile_into_spv) W3D_SHC(compile_into_preprocessed_text) W3D_SHC(result_release) W3D_SHC(result_get_length)
    W3D_SHC(result_get_bytes) W3D_SHC(result_get_compilation_status) W3D_SHC(result_get_error_message)
#undef W3D_SHC
    return true;
}

// GLSL 4.50 -> SPIR-V para Vulkan, para quien arme su propio dispositivo (el trazado de rayos por GPU). etapa: 0 vertex,
// 1 fragment, 2 compute. Con su propio compilador (no depende de que el backend Vulkan este andando)
bool VkCompilarGLSL(const char* fuente, int etapa, std::vector<uint32_t>& spv, std::string& err, int vulkanMenor) {
    if (!AbrirShaderc()) { err = "sin shaderc_shared"; return false; }
    static ShcCompilador comp = 0; static ShcOpciones opc[3] = { 0, 0, 0 };   // (Vulkan 1.0 / 1.1 / 1.2)
    const int v = vulkanMenor < 0 ? 0 : vulkanMenor > 2 ? 2 : vulkanMenor;
    if (!comp) comp = shc_compiler_initialize();
    if (comp && !opc[v]) {
        opc[v] = shc_compile_options_initialize();
        if (opc[v]) {
            shc_compile_options_set_target_env(opc[v], kShcVulkan, kShcVulkan10 | ((uint32_t)v << 12));
            shc_compile_options_set_optimization_level(opc[v], kShcOptimRendimiento);
        }
    }
    if (!comp || !opc[v]) { err = "shaderc no inicio"; return false; }
    ShcResultado r = shc_compile_into_spv(comp, fuente, strlen(fuente), etapa, "w3d.comp", "main", opc[v]);
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
    return AbrirShaderc();
}

// ============================================================================
//  El dispositivo
// ============================================================================
static VkInstance gInst = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT gDebug = VK_NULL_HANDLE;
static VkSurfaceKHR gSup = VK_NULL_HANDLE;
static VkPhysicalDevice gFis = VK_NULL_HANDLE;
static VkDevice gDev = VK_NULL_HANDLE;
static VkQueue gCola = VK_NULL_HANDLE;
static uint32_t gFamilia = 0;
static VkPhysicalDeviceProperties gProps;
static VkPhysicalDeviceMemoryProperties gMemProps;
static VkPhysicalDeviceFeatures gFeat;   // las que se prendieron
static W3dVkVentana gVen;
static bool gListo = false;
static std::string gNombreGPU;
static int gVsync = 1;
static VkFormat gFmtProf = VK_FORMAT_UNDEFINED;
static const VkFormat kFmtColor = VK_FORMAT_R8G8B8A8_UNORM;

static inline VkDeviceSize Alinear(VkDeviceSize v, VkDeviceSize a) { return a > 1 ? (v + a - 1) / a * a : v; }
static GLenum gErrorGL = GL_NO_ERROR;
static void ErrorGL(GLenum e) { if (gErrorGL == GL_NO_ERROR) gErrorGL = e; }

// ============================================================================
//  MEMORIA: bloques grandes repartidos en pedazos (Windows deja ~4096 asignaciones; cada malla y cada textura pedia una)
// ============================================================================
struct Bloque { VkDeviceMemory mem; VkDeviceSize tam; uint32_t tipo; bool imagen; unsigned char* ptr; std::map<VkDeviceSize, VkDeviceSize> libres; };
struct Asig { Bloque* b; VkDeviceSize off, tam; unsigned char* ptr; };
static std::vector<Bloque*> gBloques;
static const VkDeviceSize kBloque = 64ull << 20;
static int TipoMemoria(uint32_t bits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < gMemProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (gMemProps.memoryTypes[i].propertyFlags & props) == props) return (int)i;
    return -1;
}
static bool Reservar(const VkMemoryRequirements& r, VkMemoryPropertyFlags props, bool imagen, Asig& a) {
    int tipo = TipoMemoria(r.memoryTypeBits, props);
    if (tipo < 0 && (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) tipo = TipoMemoria(r.memoryTypeBits, props & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (tipo < 0) return false;
    for (size_t k = 0; k < gBloques.size(); k++) {
        Bloque* b = gBloques[k];
        if (b->tipo != (uint32_t)tipo || b->imagen != imagen) continue;
        for (std::map<VkDeviceSize, VkDeviceSize>::iterator it = b->libres.begin(); it != b->libres.end(); ++it) {
            const VkDeviceSize ini = it->first, largo = it->second, off = Alinear(ini, r.alignment);
            if (off + r.size > ini + largo) continue;
            b->libres.erase(it);
            if (off > ini) b->libres[ini] = off - ini;
            if (off + r.size < ini + largo) b->libres[off + r.size] = ini + largo - off - r.size;
            a.b = b; a.off = off; a.tam = r.size; a.ptr = b->ptr ? b->ptr + off : 0;
            return true;
        }
    }
    Bloque* b = new Bloque();
    b->tam = r.size > kBloque ? Alinear(r.size, 1 << 20) : kBloque;
    b->tipo = (uint32_t)tipo; b->imagen = imagen; b->ptr = 0;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = b->tam; ai.memoryTypeIndex = (uint32_t)tipo;
    if (vkAllocateMemory(gDev, &ai, 0, &b->mem) != VK_SUCCESS) {
        // (sin lugar para un bloque entero: uno justo)
        b->tam = r.size; ai.allocationSize = b->tam;
        if (vkAllocateMemory(gDev, &ai, 0, &b->mem) != VK_SUCCESS) { delete b; return false; }
    }
    if (gMemProps.memoryTypes[tipo].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* p = 0; vkMapMemory(gDev, b->mem, 0, VK_WHOLE_SIZE, 0, &p); b->ptr = (unsigned char*)p;
    }
    if (r.size < b->tam) b->libres[r.size] = b->tam - r.size;
    gBloques.push_back(b);
    a.b = b; a.off = 0; a.tam = r.size; a.ptr = b->ptr;
    return true;
}
static void Liberar(Asig& a) {
    if (!a.b) return;
    std::map<VkDeviceSize, VkDeviceSize>& l = a.b->libres;
    VkDeviceSize off = a.off, tam = a.tam;
    std::map<VkDeviceSize, VkDeviceSize>::iterator sig = l.lower_bound(off);
    if (sig != l.end() && sig->first == off + tam) { tam += sig->second; l.erase(sig); }
    std::map<VkDeviceSize, VkDeviceSize>::iterator ant = l.lower_bound(off);
    if (ant != l.begin()) { --ant; if (ant->first + ant->second == off) { off = ant->first; tam += ant->second; l.erase(ant); } }
    l[off] = tam;
    a.b = 0;
}

struct BufVk { VkBuffer b; Asig a; VkDeviceSize tam; };
static bool CrearBuffer(VkDeviceSize tam, VkBufferUsageFlags uso, VkMemoryPropertyFlags props, BufVk& out) {
    memset(&out, 0, sizeof(out));
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = tam < 16 ? 16 : tam; bi.usage = uso; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(gDev, &bi, 0, &out.b) != VK_SUCCESS) return false;
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(gDev, out.b, &r);
    if (!Reservar(r, props, false, out.a)) { vkDestroyBuffer(gDev, out.b, 0); out.b = VK_NULL_HANDLE; return false; }
    vkBindBufferMemory(gDev, out.b, out.a.b->mem, out.a.off);
    out.tam = bi.size;
    return true;
}

// ---- lo que se borra mientras la GPU puede estar usandolo: se destruye cuando termino el cuadro ----
struct Basura { VkBuffer buf; VkImage img; VkImageView vista; VkFramebuffer fb; VkPipeline pipe; VkShaderModule mod; Asig a; };
static const int kCuadros = 2;
struct Trozo { BufVk buf; VkDeviceSize usado; };
struct Cuadro {
    VkCommandBuffer cmd, cmdPre;
    VkFence fence;
    VkSemaphore semAdq;
    bool enVuelo;
    std::vector<Trozo> trozos; size_t trozo;
    std::vector<VkDescriptorPool> pools; size_t pool;
    std::vector<Basura> basura;
};
static Cuadro gCuadros[kCuadros];
static int gCur = 0;
static VkCommandPool gPool = VK_NULL_HANDLE;
static void Tirar(const Basura& b) { gCuadros[gCur].basura.push_back(b); }
static Basura BasuraVacia() { Basura b; memset(&b, 0, sizeof(b)); return b; }
static void TirarBuffer(BufVk& b) { if (!b.b) return; Basura x = BasuraVacia(); x.buf = b.b; x.a = b.a; Tirar(x); b.b = VK_NULL_HANDLE; b.a.b = 0; }
static void Destruir(Basura& b) {
    if (b.pipe) vkDestroyPipeline(gDev, b.pipe, 0);
    if (b.mod) vkDestroyShaderModule(gDev, b.mod, 0);
    if (b.fb) vkDestroyFramebuffer(gDev, b.fb, 0);
    if (b.vista) vkDestroyImageView(gDev, b.vista, 0);
    if (b.img) vkDestroyImage(gDev, b.img, 0);
    if (b.buf) vkDestroyBuffer(gDev, b.buf, 0);
    Liberar(b.a);
}
static void LiberarBasura(Cuadro& c) { for (size_t i = 0; i < c.basura.size(); i++) Destruir(c.basura[i]); c.basura.clear(); }

// ============================================================================
//  EL ANILLO: los datos de cada draw del cuadro (atributos, uniforms, las copias a texturas y buffers, las lecturas)
// ============================================================================
struct Anillo { VkBuffer b; VkDeviceSize off; unsigned char* p; };
static const VkDeviceSize kTrozo = 8ull << 20;
static unsigned gEpoca = 1;   // sube cada vez que un anillo se reinicia: lo subido antes ya no esta
static VkDeviceSize gAlinUBO = 256;
static Anillo Pedir(VkDeviceSize tam, VkDeviceSize alin) {
    Cuadro& c = gCuadros[gCur];
    if (tam == 0) tam = 4;
    for (;;) {
        if (c.trozo < c.trozos.size()) {
            Trozo& t = c.trozos[c.trozo];
            const VkDeviceSize off = Alinear(t.usado, alin);
            if (off + tam <= t.buf.tam) { t.usado = off + tam; Anillo r = { t.buf.b, off, t.buf.a.ptr + off }; return r; }
            c.trozo++;
            continue;
        }
        Trozo t; t.usado = 0;
        const VkDeviceSize n = tam > kTrozo ? Alinear(tam, 1 << 20) : kTrozo;
        if (!CrearBuffer(n, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, t.buf) || !t.buf.a.ptr) {
            w3dLogfE("[vk] sin memoria para el anillo (%u KB)", (unsigned)(n >> 10));
            static std::vector<unsigned char> basura;   // (para que quien copie no escriba afuera; el draw no sale)
            if (basura.size() < tam) basura.resize((size_t)tam);
            Anillo r = { VK_NULL_HANDLE, 0, &basura[0] }; return r;
        }
        c.trozos.push_back(t);
        c.trozo = c.trozos.size() - 1;
    }
}
static VkDeviceSize AnilloUsado() {
    const Cuadro& c = gCuadros[gCur]; VkDeviceSize n = 0;
    for (size_t i = 0; i < c.trozos.size() && i <= c.trozo; i++) n += c.trozos[i].usado;
    return n;
}

// ============================================================================
//  Grabar: el command buffer del cuadro (y el de ANTES: las subidas de lo nuevo, que van antes que todo lo dibujado)
// ============================================================================
static bool gGrabando = false, gPreGrabando = false, gEnPase = false;
static unsigned gSerialCmd = 1;   // cambia con cada command buffer: una textura "usada en lo grabado" tiene este numero
static VkRenderPass gRpDef = VK_NULL_HANDLE, gRpColor = VK_NULL_HANDLE;
static VkDescriptorSetLayout gSetLayout = VK_NULL_HANDLE;
static VkPipelineLayout gLayout = VK_NULL_HANDLE;
static VkPipelineCache gPipeCache = VK_NULL_HANDLE;
// lo que el command buffer ya tiene puesto (no repetirlo en cada draw)
struct EstadoCmd { VkPipeline pipe; VkViewport vp; VkRect2D sc; float linea, biasC, biasP; uint32_t sCmp, sWr, sRef; bool ok; };
static EstadoCmd gEc;
static void OlvidarEstadoCmd() { memset(&gEc, 0, sizeof(gEc)); }
static void Comenzar(VkCommandBuffer cb) {
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
}
static VkCommandBuffer Cmd() {
    Cuadro& c = gCuadros[gCur];
    if (!gGrabando) { Comenzar(c.cmd); gGrabando = true; gSerialCmd++; OlvidarEstadoCmd(); }
    return c.cmd;
}
static VkCommandBuffer CmdPre() {
    Cuadro& c = gCuadros[gCur];
    if (!gPreGrabando) { Comenzar(c.cmdPre); gPreGrabando = true; }
    return c.cmdPre;
}
// barrera de imagen (gruesa: todo lo de antes contra todo lo de despues; las copias son pocas)
static void Barrera(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp, VkImageLayout de, VkImageLayout a,
                    uint32_t nivel0 = 0, uint32_t niveles = VK_REMAINING_MIP_LEVELS) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = de; b.newLayout = a;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = asp; b.subresourceRange.baseMipLevel = nivel0; b.subresourceRange.levelCount = niveles;
    b.subresourceRange.baseArrayLayer = 0; b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}
static void BarreraMemoria(VkCommandBuffer cb) {
    VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, 0, 0, 0);
}

// ============================================================================
//  Estado de GL
// ============================================================================
static const int kAttrs = 8, kUnidades = 8, kMuestras = 8;
struct EstadoGL {
    bool blend, depth, cull, scissor, stencil, offset;
    GLenum bSrc, bDst, bEq, dFunc; bool dMask; bool cMask[4];
    GLenum sFunc; GLint sRef; GLuint sMaskR, sMaskW; GLenum sFail, sZFail, sZPass;
    float clear[4]; GLint clearS; float clearZ;
    GLint vp[4], sc[4]; GLenum front, cullModo; float linea, offF, offU, zn, zf;
    GLint unpack, pack;
};
static EstadoGL g;
static void EstadoDeFabrica(int w, int h) {
    memset(&g, 0, sizeof(g));
    g.bSrc = GL_ONE; g.bDst = GL_ZERO; g.bEq = GL_FUNC_ADD; g.dFunc = GL_LESS; g.dMask = true;
    for (int i = 0; i < 4; i++) g.cMask[i] = true;
    g.sFunc = GL_ALWAYS; g.sMaskR = g.sMaskW = 0xFFFFFFFFu; g.sFail = g.sZFail = g.sZPass = GL_KEEP;
    g.clearZ = 1.0f;
    g.vp[2] = g.sc[2] = w; g.vp[3] = g.sc[3] = h;
    g.front = GL_CCW; g.cullModo = GL_BACK; g.linea = 1.0f; g.zf = 1.0f;
    g.unpack = g.pack = 4;
}

// ---- texturas ----
struct TexGL {
    VkImage img; Asig a; VkImageView vista, vista0; VkFramebuffer fb;
    int w, h, niveles, nivelesDef; bool hay, iniciada;
    GLint minF, magF, wrapS, wrapT; float aniso; bool genMip;
    unsigned usoCmd;
};
static std::map<GLuint, TexGL*> gTex;
static GLuint gSigTex = 1;
static GLuint gUnidad[kUnidades];
static int gUnidadActiva = 0;
static TexGL* gNegra = 0;   // la de una unidad vacia (GL muestrea negro)
static std::map<uint32_t, VkSampler> gSamplers;

// ---- buffers ----
struct BufGL {
    size_t tam; bool estatico, elementos; BufVk vk;
    std::vector<unsigned char> copia;
    Anillo subido; unsigned epoca;
    std::map<uint64_t, BufVk> conv;   // (los formatos que la GPU no lee, pasados a float)
};
static std::map<GLuint, BufGL*> gBufs;
static GLuint gSigBuf = 1, gBufArray = 0, gBufElem = 0;
static bool gGuardarCopias = false;   // la GPU no lee algun formato de atributo: los buffers guardan su copia

struct AttrGL { bool on; GLint size; GLenum type; GLboolean norm; GLsizei stride; const void* ptr; GLuint buf; float cte[4]; };
static AttrGL gAttr[kAttrs];

// ---- framebuffers ----
static std::map<GLuint, GLuint> gFbos;   // fbo -> textura
static GLuint gSigFbo = 1, gFboActual = 0;

// ---- la pantalla (la imagen propia) y la ventana ----
struct Pantalla { VkImage color, prof; Asig ac, ap; VkImageView vc, vp; VkFramebuffer fb; int w, h; };
static Pantalla gPan;
static VkSwapchainKHR gSc = VK_NULL_HANDLE;
static std::vector<VkImage> gScImgs;
static std::vector<VkSemaphore> gScSem;
static VkExtent2D gScTam = { 0, 0 };
static VkFormat gScFmt = VK_FORMAT_UNDEFINED;
static bool gScRehacer = false;
static bool gScBlit = true;

// ============================================================================
//  El destino del dibujo: la pantalla o una textura (glBindFramebuffer)
// ============================================================================
struct Destino { VkFramebuffer fb; VkRenderPass rp; int w, h; bool volteado, conProf; TexGL* tex; VkImage img; };
static Destino gPase;   // el del pase abierto
static TexGL* TexDe(GLuint id) { std::map<GLuint, TexGL*>::iterator it = gTex.find(id); return it == gTex.end() ? 0 : it->second; }
static bool FboDeTextura(TexGL* t) {
    if (t->fb) return true;
    if (!t->hay) return false;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kFmtColor;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    if (!t->vista0 && vkCreateImageView(gDev, &vi, 0, &t->vista0) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = gRpColor; fi.attachmentCount = 1; fi.pAttachments = &t->vista0;
    fi.width = (uint32_t)t->w; fi.height = (uint32_t)t->h; fi.layers = 1;
    return vkCreateFramebuffer(gDev, &fi, 0, &t->fb) == VK_SUCCESS;
}
static Destino DestinoActual() {
    Destino d; memset(&d, 0, sizeof(d));
    if (gFboActual) {
        std::map<GLuint, GLuint>::iterator it = gFbos.find(gFboActual);
        TexGL* t = it != gFbos.end() ? TexDe(it->second) : 0;
        if (t && FboDeTextura(t)) {
            d.fb = t->fb; d.rp = gRpColor; d.w = t->w; d.h = t->h; d.volteado = false; d.conProf = false; d.tex = t; d.img = t->img;
            return d;
        }
    }
    d.fb = gPan.fb; d.rp = gRpDef; d.w = gPan.w; d.h = gPan.h; d.volteado = true; d.conProf = true; d.img = gPan.color;
    return d;
}
static void TerminarPase() {
    if (!gEnPase) return;
    VkCommandBuffer cb = Cmd();
    vkCmdEndRenderPass(cb);
    if (gPase.tex) Barrera(cb, gPase.tex->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    gEnPase = false;
}
static bool EmpezarPase() {
    if (gEnPase) return true;
    Destino d = DestinoActual();
    if (!d.fb || d.w <= 0 || d.h <= 0) return false;
    VkCommandBuffer cb = Cmd();
    if (d.tex) {
        Barrera(cb, d.tex->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
        d.tex->usoCmd = gSerialCmd;
    }
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = d.rp; bi.framebuffer = d.fb;
    bi.renderArea.extent.width = (uint32_t)d.w; bi.renderArea.extent.height = (uint32_t)d.h;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    gPase = d; gEnPase = true;
    OlvidarEstadoCmd();
    return true;
}

// ============================================================================
//  ENVIAR: el command buffer a la GPU. EnviarYEsperar + Reciclar a mitad de cuadro (lecturas, glFinish, anillo lleno)
// ============================================================================
static bool Enviar(VkSemaphore esperar, VkSemaphore senial) {
    Cuadro& c = gCuadros[gCur];
    TerminarPase();
    VkCommandBuffer cbs[2]; uint32_t n = 0;
    if (gPreGrabando) { BarreraMemoria(c.cmdPre); vkEndCommandBuffer(c.cmdPre); cbs[n++] = c.cmdPre; gPreGrabando = false; }
    if (gGrabando) { vkEndCommandBuffer(c.cmd); cbs[n++] = c.cmd; gGrabando = false; }
    if (!n && !esperar && !senial) return false;
    if (c.enVuelo) { vkWaitForFences(gDev, 1, &c.fence, VK_TRUE, UINT64_MAX); c.enVuelo = false; }
    vkResetFences(gDev, 1, &c.fence);
    VkPipelineStageFlags etapa = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = esperar ? 1 : 0; si.pWaitSemaphores = &esperar; si.pWaitDstStageMask = &etapa;
    si.commandBufferCount = n; si.pCommandBuffers = cbs;
    si.signalSemaphoreCount = senial ? 1 : 0; si.pSignalSemaphores = &senial;
    const VkResult r = vkQueueSubmit(gCola, 1, &si, c.fence);
    if (r != VK_SUCCESS) { w3dLogfE("[vk] vkQueueSubmit: %d", (int)r); return false; }
    c.enVuelo = true;
    return true;
}
static std::map<std::string, VkDescriptorSet> gCacheSets;
static void Reiniciar(Cuadro& c) {
    LiberarBasura(c);
    for (size_t i = 0; i < c.trozos.size(); i++) c.trozos[i].usado = 0;
    c.trozo = 0;
    for (size_t i = 0; i < c.pools.size(); i++) vkResetDescriptorPool(gDev, c.pools[i], 0);
    c.pool = 0;
    gEpoca++;
    gCacheSets.clear();
}
static void EnviarYEsperar() {
    Enviar(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkQueueWaitIdle(gCola);
}
// (despues de EnviarYEsperar: la GPU no tiene nada; todo lo de los dos cuadros se puede volver a usar)
static void ReciclarTodo() {
    for (int i = 0; i < kCuadros; i++) { if (gCuadros[i].enVuelo) { vkWaitForFences(gDev, 1, &gCuadros[i].fence, VK_TRUE, UINT64_MAX); gCuadros[i].enVuelo = false; } Reiniciar(gCuadros[i]); }
}

// ============================================================================
//  Texturas
// ============================================================================
static int NivelesDe(int w, int h) { int n = 1, m = w > h ? w : h; while (m > 1) { m >>= 1; n++; } return n; }
static void TirarImagenTex(TexGL* t) {
    if (!t->hay) return;
    Basura b = BasuraVacia(); b.img = t->img; b.vista = t->vista; b.a = t->a; Tirar(b);
    if (t->vista0 || t->fb) { Basura c = BasuraVacia(); c.vista = t->vista0; c.fb = t->fb; Tirar(c); }
    t->img = VK_NULL_HANDLE; t->vista = t->vista0 = VK_NULL_HANDLE; t->fb = VK_NULL_HANDLE; t->a.b = 0;
    t->hay = false; t->iniciada = false;
}
static bool RehacerVista(TexGL* t) {
    if (t->vista) { Basura b = BasuraVacia(); b.vista = t->vista; Tirar(b); t->vista = VK_NULL_HANDLE; }
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kFmtColor;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = (uint32_t)(t->nivelesDef > 0 ? t->nivelesDef : 1); vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(gDev, &vi, 0, &t->vista) == VK_SUCCESS;
}
static bool CrearImagenTex(TexGL* t, int w, int h) {
    TirarImagenTex(t);
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = kFmtColor;
    ii.extent.width = (uint32_t)w; ii.extent.height = (uint32_t)h; ii.extent.depth = 1;
    ii.mipLevels = (uint32_t)NivelesDe(w, h); ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(gDev, &ii, 0, &t->img) != VK_SUCCESS) return false;
    VkMemoryRequirements r; vkGetImageMemoryRequirements(gDev, t->img, &r);
    if (!Reservar(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, t->a)) { vkDestroyImage(gDev, t->img, 0); t->img = VK_NULL_HANDLE; return false; }
    vkBindImageMemory(gDev, t->img, t->a.b->mem, t->a.off);
    t->w = w; t->h = h; t->niveles = (int)ii.mipLevels; t->nivelesDef = 1; t->hay = true; t->iniciada = false; t->usoCmd = 0;
    return RehacerVista(t);
}
// las copias a una textura: en orden con lo dibujado si ya se uso en lo grabado; si no, antes de todo el cuadro
static VkCommandBuffer CmdCopiaA(TexGL* t) {
    if (gGrabando && t->usoCmd == gSerialCmd) { TerminarPase(); return Cmd(); }
    return CmdPre();
}
// la textura lista para copiarle: en TRANSFER_DST (la primera vez, en negro transparente)
static void TexAbrirCopia(VkCommandBuffer cb, TexGL* t) {
    Barrera(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, t->iniciada ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (!t->iniciada) {
        VkClearColorValue cero; memset(&cero, 0, sizeof(cero));
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
        vkCmdClearColorImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cero, 1, &rr);
        t->iniciada = true;
    }
}
static void TexCerrarCopia(VkCommandBuffer cb, TexGL* t) {
    Barrera(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
static int BppDe(GLenum fmt, GLenum tipo) {
    if (tipo == GL_UNSIGNED_SHORT_5_6_5 || tipo == GL_UNSIGNED_SHORT_4_4_4_4 || tipo == GL_UNSIGNED_SHORT_5_5_5_1) return 2;
    switch (fmt) { case GL_RGBA: case GL_BGRA: return 4; case GL_RGB: return 3; case GL_LUMINANCE_ALPHA: return 2; default: return 1; }
}
// los pixeles de GL (cualquier formato del motor) a RGBA 8
static void ARGBA8(GLenum fmt, GLenum tipo, int w, int h, const unsigned char* s0, int alin, unsigned char* d) {
    const int bpp = BppDe(fmt, tipo);
    const size_t fila = (size_t)Alinear((VkDeviceSize)w * bpp, alin > 0 ? alin : 1);
    if (tipo == GL_UNSIGNED_BYTE && fmt == GL_RGBA && fila == (size_t)w * 4) { memcpy(d, s0, (size_t)w * h * 4); return; }
    for (int y = 0; y < h; y++) {
        const unsigned char* s = s0 + fila * y;
        for (int x = 0; x < w; x++, d += 4) {
            if (tipo == GL_UNSIGNED_BYTE) {
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
                if (tipo == GL_UNSIGNED_SHORT_5_6_5) {
                    d[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31); d[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
                    d[2] = (unsigned char)((v & 31) * 255 / 31); d[3] = 255;
                } else if (tipo == GL_UNSIGNED_SHORT_4_4_4_4) {
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
static void Subir(TexGL* t, int nivel, int x, int y, int w, int h, GLenum fmt, GLenum tipo, const void* datos) {
    VkCommandBuffer cb = CmdCopiaA(t);
    TexAbrirCopia(cb, t);
    if (datos && w > 0 && h > 0) {
        Anillo r = Pedir((VkDeviceSize)w * h * 4, 16);
        if (r.b) {
            ARGBA8(fmt, tipo, w, h, (const unsigned char*)datos, g.unpack, r.p);
            VkBufferImageCopy c; memset(&c, 0, sizeof(c));
            c.bufferOffset = r.off;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)nivel; c.imageSubresource.layerCount = 1;
            c.imageOffset.x = x; c.imageOffset.y = y; c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cb, r.b, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
    }
    TexCerrarCopia(cb, t);
}
static void GenerarMips(TexGL* t) {
    if (!t->hay || t->niveles < 2) return;
    VkCommandBuffer cb = CmdCopiaA(t);
    TexAbrirCopia(cb, t);
    for (int i = 1; i < t->niveles; i++) {
        Barrera(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (uint32_t)i - 1, 1);
        VkImageBlit b; memset(&b, 0, sizeof(b));
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.mipLevel = (uint32_t)i - 1; b.dstSubresource.mipLevel = (uint32_t)i;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        const int sw = t->w >> (i - 1), sh = t->h >> (i - 1), dw = t->w >> i, dh = t->h >> i;
        b.srcOffsets[1].x = sw > 0 ? sw : 1; b.srcOffsets[1].y = sh > 0 ? sh : 1; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = dw > 0 ? dw : 1; b.dstOffsets[1].y = dh > 0 ? dh : 1; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    Barrera(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, (uint32_t)t->niveles - 1);
    Barrera(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, (uint32_t)t->niveles - 1, 1);
    if (t->nivelesDef != t->niveles) { t->nivelesDef = t->niveles; RehacerVista(t); }
}
static VkSampler SamplerDe(const TexGL* t) {
    const GLint mn = t->minF, mg = t->magF;
    const int fm = (mn == GL_NEAREST) ? 0 : (mn == GL_LINEAR) ? 1 : (mn == GL_NEAREST_MIPMAP_NEAREST) ? 2 : (mn == GL_LINEAR_MIPMAP_NEAREST) ? 3
                 : (mn == GL_NEAREST_MIPMAP_LINEAR) ? 4 : 5;
    int an = (int)(t->aniso + 0.5f); if (an < 1) an = 1; if (an > 16) an = 16;
    if (!gFeat.samplerAnisotropy) an = 1;
    const int ws = t->wrapS == GL_REPEAT ? 0 : t->wrapS == GL_MIRRORED_REPEAT ? 1 : 2, wt = t->wrapT == GL_REPEAT ? 0 : t->wrapT == GL_MIRRORED_REPEAT ? 1 : 2;
    const uint32_t clave = (uint32_t)fm | ((mg == GL_NEAREST ? 0u : 1u) << 3) | ((uint32_t)ws << 4) | ((uint32_t)wt << 6) | ((uint32_t)an << 8);
    std::map<uint32_t, VkSampler>::iterator it = gSamplers.find(clave);
    if (it != gSamplers.end()) return it->second;
    static const VkSamplerAddressMode kWrap[3] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = mg == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.minFilter = (fm == 0 || fm == 2 || fm == 4) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.mipmapMode = (fm == 4 || fm == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = kWrap[ws]; si.addressModeV = kWrap[wt]; si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.anisotropyEnable = an > 1 ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = (float)an > gProps.limits.maxSamplerAnisotropy ? gProps.limits.maxSamplerAnisotropy : (float)an;
    si.minLod = 0.0f; si.maxLod = fm < 2 ? 0.25f : VK_LOD_CLAMP_NONE;   // (sin mipmaps: solo el nivel 0, como GL)
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(gDev, &si, 0, &s);
    gSamplers[clave] = s;
    return s;
}
static TexGL* NuevaTex() {
    TexGL* t = new TexGL(); memset(t, 0, sizeof(*t));
    t->minF = GL_NEAREST_MIPMAP_LINEAR; t->magF = GL_LINEAR; t->wrapS = t->wrapT = GL_REPEAT; t->aniso = 1.0f;
    return t;
}
static TexGL* TexActual() {
    const GLuint id = gUnidad[gUnidadActiva];
    if (!id) return 0;
    TexGL*& t = gTex[id];
    if (!t) t = NuevaTex();
    return t;
}

// ============================================================================
//  LOS SHADERS: GLSL ES 1.00 -> GLSL 4.50 de Vulkan
// ============================================================================
struct TipoGLSL { const char* n; int tam, alin, cols, clase; };   // clase: 0 float, 1 int, 2 bool, 3 matriz, 4 sampler
static const TipoGLSL kTipos[] = {
    { "float", 4, 4, 1, 0 }, { "vec2", 8, 8, 1, 0 }, { "vec3", 12, 16, 1, 0 }, { "vec4", 16, 16, 1, 0 },
    { "int", 4, 4, 1, 1 }, { "ivec2", 8, 8, 1, 1 }, { "ivec3", 12, 16, 1, 1 }, { "ivec4", 16, 16, 1, 1 },
    { "uint", 4, 4, 1, 1 }, { "bool", 4, 4, 1, 2 }, { "bvec2", 8, 8, 1, 2 }, { "bvec3", 12, 16, 1, 2 }, { "bvec4", 16, 16, 1, 2 },
    { "mat2", 32, 16, 2, 3 }, { "mat3", 48, 16, 3, 3 }, { "mat4", 64, 16, 4, 3 },
    { "sampler2D", 0, 0, 1, 4 }, { "samplerCube", 0, 0, 1, 4 },
};
static const TipoGLSL* TipoDe(const std::string& n) {
    for (size_t i = 0; i < sizeof(kTipos) / sizeof(kTipos[0]); i++) if (n == kTipos[i].n) return &kTipos[i];
    return 0;
}
// cuantas locations ocupa un varying / atributo
static int LocsDe(const std::string& tipo) { return tipo == "mat4" ? 4 : tipo == "mat3" ? 3 : tipo == "mat2" ? 2 : 1; }
static inline bool EsIdent(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
// cambiar una PALABRA entera (un identificador) por otra
static std::string CambiarPalabra(const std::string& s, const char* de, const char* a) {
    std::string o; o.reserve(s.size());
    const size_t n = strlen(de);
    for (size_t i = 0; i < s.size();) {
        if (EsIdent(s[i])) {
            size_t j = i; while (j < s.size() && EsIdent(s[j])) j++;
            if (j - i == n && s.compare(i, n, de) == 0) o += a; else o.append(s, i, j - i);
            i = j;
        } else o += s[i++];
    }
    return o;
}
static std::string Recortar(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
struct VarGLSL { std::string clase, tipo, nombre; int arr; };
// el texto preprocesado: las declaraciones globales (uniform / attribute / varying) aparte; el resto (las funciones) al cuerpo
static void Analizar(const std::string& pre, std::vector<VarGLSL>& vars, std::string& cuerpo) {
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
    s = CambiarPalabra(CambiarPalabra(CambiarPalabra(s, "lowp", ""), "mediump", ""), "highp", "");
    std::string trozo; int prof = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (prof > 0) { cuerpo += c; if (c == '{') prof++; else if (c == '}') --prof; continue; }
        if (c == '#') {   // un #line entre declaraciones: al cuerpo (a mitad de una declaracion no puede ir: se pierde)
            size_t f = s.find('\n', i); if (f == std::string::npos) f = s.size();
            if (Recortar(trozo).empty()) { cuerpo += trozo; cuerpo.append(s, i, f - i); trozo.clear(); }
            i = f - 1;
            continue;
        }
        if (c == '{') { cuerpo += trozo; cuerpo += c; trozo.clear(); prof = 1; continue; }
        if (c != ';') { trozo += c; continue; }
        const std::string t = Recortar(trozo);
        size_t saltos = 0, antes = 0;   // (los renglones de la declaracion: los de adelante y los de adentro)
        { bool lleno = false; for (size_t k = 0; k < trozo.size(); k++) if (trozo[k] == '\n') { saltos++; if (!lleno) antes++; } else if (trozo[k] != ' ' && trozo[k] != '\t' && trozo[k] != '\r') lleno = true; }
        trozo.clear();
        size_t e = 0; while (e < t.size() && EsIdent(t[e])) e++;
        const std::string clase = t.substr(0, e);
        if (clase == "precision") { cuerpo.append(saltos, '\n'); continue; }
        if (clase != "uniform" && clase != "attribute" && clase != "varying") { cuerpo.append(antes, '\n'); cuerpo += t; cuerpo += ';'; continue; }
        cuerpo.append(saltos, '\n');
        size_t i0 = t.find_first_not_of(" \t\r\n", e), i1 = i0; while (i1 < t.size() && EsIdent(t[i1])) i1++;
        if (i0 == std::string::npos) continue;
        const std::string tipo = t.substr(i0, i1 - i0);
        std::string resto; for (size_t k = i1; k < t.size(); k++) if (t[k] != ' ' && t[k] != '\t' && t[k] != '\r' && t[k] != '\n') resto += t[k];
        size_t p = 0;
        while (p <= resto.size()) {
            size_t q = resto.find(',', p); if (q == std::string::npos) q = resto.size();
            const std::string d = resto.substr(p, q - p);
            if (!d.empty()) {
                VarGLSL v; v.clase = clase; v.tipo = tipo; v.arr = 0;
                const size_t c1 = d.find('[');
                v.nombre = d.substr(0, c1);
                if (c1 != std::string::npos) v.arr = atoi(d.c_str() + c1 + 1);
                vars.push_back(v);
            }
            p = q + 1;
        }
    }
    cuerpo += trozo;
}
static std::string Decl(const VarGLSL& v) {
    char a[32] = ""; if (v.arr > 0) snprintf(a, sizeof(a), "[%d]", v.arr);
    return v.tipo + " " + v.nombre + a;
}

struct ShaderGL { GLenum tipo; std::string fuente, pre, log; bool ok; };
struct UniGL { std::string nombre; const TipoGLSL* t; int off, stride, elems, loc0, muestra; };
struct LocGL { int uni, elem; };
struct ClavePipe {
    uint32_t a, b;
    uint8_t fmt[kAttrs];
    uint16_t stride[kAttrs];
    bool operator<(const ClavePipe& o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};
struct ProgGL {
    GLuint vs, fs; std::map<std::string, int> atribPedidos;
    bool ok; std::string log;
    VkShaderModule mVS, mFS;
    std::vector<UniGL> unis; std::vector<LocGL> locs;
    std::vector<unsigned char> bloque; int tamBloque, offAjuste;
    int nMuestras; int muestraUni[kMuestras];   // el uniform (sampler) de cada binding 1..n
    int unidad[kMuestras];                       // la unidad de textura que dice cada sampler (glUniform1i)
    std::map<std::string, int> atribs;           // nombre -> location (los del vertex shader)
    unsigned mascaraAtrib;
    bool sucio; unsigned epocaSubida; Anillo subido;
    std::map<ClavePipe, VkPipeline> pipes;
};
static std::map<GLuint, ShaderGL*> gShaders;
static std::map<GLuint, ProgGL*> gProgs;
static GLuint gSigNombre = 1;   // (shaders y programas comparten los nombres, como en GL)
static ProgGL* gProg = 0;
static GLuint gProgId = 0;

// "#line N" de GLSL ES 1.00 dice que el renglon SIGUIENTE es el N + 1; en el GLSL 450 con el que se compila aca, que es
// el N. Se corre uno (los errores de un shader escrito dicen el renglon de SU archivo, como en GLES 2.0)
static std::string LineasDeES(const std::string& src) {
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
static bool Preprocesar(const std::string& src, int tipo, std::string& out, std::string& log) {
    const std::string s = "#version 450\n#define W3D_VULKAN 1\n" + LineasDeES(src);
    ShcResultado r = shc_compile_into_preprocessed_text(gShc, s.c_str(), s.size(), tipo, "w3d", "main", gShcOpc);
    const bool ok = r && shc_result_get_compilation_status(r) == 0;
    if (ok) out.assign(shc_result_get_bytes(r), shc_result_get_length(r));
    else log = r ? shc_result_get_error_message(r) : "shaderc";
    if (r) shc_result_release(r);
    return ok;
}
static bool CompilarSPV(const std::string& src, int tipo, VkShaderModule& mod, std::string& log) {
    ShcResultado r = shc_compile_into_spv(gShc, src.c_str(), src.size(), tipo, tipo == kShcVertex ? "w3d.vert" : "w3d.frag", "main", gShcOpc);
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
static std::string ArreglarCuerpo(const std::string& c, bool vs) {
    std::string s = CambiarPalabra(CambiarPalabra(CambiarPalabra(c, "texture2D", "texture"), "texture2DLod", "textureLod"), "texture2DProj", "textureProj");
    s = CambiarPalabra(s, "textureCube", "texture");
    if (vs) s = CambiarPalabra(s, "main", "w3d_main_");
    else s = CambiarPalabra(CambiarPalabra(s, "gl_FragColor", "w3d_Color"), "gl_FragCoord", "w3dFragCoord()");
    return s;
}
// LINKEAR: los dos shaders traducidos, con el MISMO bloque de uniforms (el de los dos juntos) y los varyings en las
// mismas locations
static bool Linkear(ProgGL* p) {
    ShaderGL* vs = gShaders.count(p->vs) ? gShaders[p->vs] : 0;
    ShaderGL* fs = gShaders.count(p->fs) ? gShaders[p->fs] : 0;
    if (!vs || !fs || !vs->ok || !fs->ok) { p->log = "falta un shader (o no compilo)"; return false; }
    std::vector<VarGLSL> vv, vf; std::string cv, cf;
    Analizar(vs->pre, vv, cv); Analizar(fs->pre, vf, cf);
    // los uniforms de los dos (cada nombre una vez) y los samplers
    std::vector<VarGLSL> unis, samps;
    for (int k = 0; k < 2; k++) {
        const std::vector<VarGLSL>& l = k ? vf : vv;
        for (size_t i = 0; i < l.size(); i++) {
            if (l[i].clase != "uniform") continue;
            std::vector<VarGLSL>& dst = (TipoDe(l[i].tipo) && TipoDe(l[i].tipo)->clase == 4) ? samps : unis;
            bool ya = false; for (size_t j = 0; j < dst.size(); j++) if (dst[j].nombre == l[i].nombre) ya = true;
            if (!ya) dst.push_back(l[i]);
        }
    }
    if ((int)samps.size() > kMuestras) { p->log = "demasiados samplers"; return false; }
    // el bloque std140
    p->unis.clear(); p->locs.clear();
    int off = 0;
    std::string bloque = "layout(std140, set=0, binding=0) uniform W3dBloque {\n";
    for (size_t i = 0; i < unis.size(); i++) {
        const TipoGLSL* t = TipoDe(unis[i].tipo);
        if (!t) { p->log = "tipo de uniform desconocido: " + unis[i].tipo; return false; }
        UniGL u; u.nombre = unis[i].nombre; u.t = t; u.muestra = -1;
        u.elems = unis[i].arr > 0 ? unis[i].arr : 1;
        u.stride = unis[i].arr > 0 ? (int)Alinear((VkDeviceSize)t->tam, 16) : t->tam;
        off = (int)Alinear((VkDeviceSize)off, unis[i].arr > 0 ? 16 : (VkDeviceSize)t->alin);
        u.off = off; off += u.stride * u.elems;
        u.loc0 = (int)p->locs.size();
        for (int e = 0; e < u.elems; e++) { LocGL l = { (int)p->unis.size(), e }; p->locs.push_back(l); }
        p->unis.push_back(u);
        bloque += "  " + Decl(unis[i]) + ";\n";
    }
    off = (int)Alinear((VkDeviceSize)off, 16); p->offAjuste = off; off += 16;
    p->tamBloque = (int)Alinear((VkDeviceSize)off, 16);
    bloque += "  vec4 w3d_Ajuste;\n};\n";   // (gl_FragCoord: x = destino dado vuelta, y = su alto)
    p->bloque.assign((size_t)p->tamBloque, 0);
    p->nMuestras = (int)samps.size();
    for (size_t i = 0; i < samps.size(); i++) {
        char l[160]; snprintf(l, sizeof(l), "layout(set=0, binding=%d) uniform %s %s;\n", (int)i + 1, samps[i].tipo.c_str(), samps[i].nombre.c_str());
        bloque += l;
        UniGL u; u.nombre = samps[i].nombre; u.t = TipoDe(samps[i].tipo); u.off = 0; u.stride = 0; u.elems = 1; u.muestra = (int)i;
        u.loc0 = (int)p->locs.size();
        LocGL lg = { (int)p->unis.size(), 0 }; p->locs.push_back(lg);
        p->muestraUni[i] = (int)p->unis.size(); p->unidad[i] = 0;
        p->unis.push_back(u);
    }
    // VERTEX: los atributos en sus lugares (glBindAttribLocation, o el primero libre) y los varyings en orden
    std::string ov = "#version 450\n" + bloque, of = "#version 450\n" + bloque;
    p->atribs.clear(); p->mascaraAtrib = 0;
    unsigned ocupados = 0;
    for (std::map<std::string, int>::iterator it = p->atribPedidos.begin(); it != p->atribPedidos.end(); ++it) ocupados |= 1u << it->second;
    std::map<std::string, int> locVar; int sigVar = 0;
    for (size_t i = 0; i < vv.size(); i++) {
        char l[160];
        if (vv[i].clase == "attribute") {
            int loc = -1;
            if (p->atribPedidos.count(vv[i].nombre)) loc = p->atribPedidos[vv[i].nombre];
            else { for (int k = 0; k < kAttrs; k++) if (!(ocupados & (1u << k))) { loc = k; break; } }
            if (loc < 0 || loc >= kAttrs) { p->log = "sin lugar para el atributo " + vv[i].nombre; return false; }
            ocupados |= 1u << loc;
            p->atribs[vv[i].nombre] = loc; p->mascaraAtrib |= 1u << loc;
            snprintf(l, sizeof(l), "layout(location=%d) in %s;\n", loc, Decl(vv[i]).c_str());
            ov += l;
        } else if (vv[i].clase == "varying") {
            locVar[vv[i].nombre] = sigVar;
            snprintf(l, sizeof(l), "layout(location=%d) out %s;\n", sigVar, Decl(vv[i]).c_str());
            ov += l;
            sigVar += LocsDe(vv[i].tipo) * (vv[i].arr > 0 ? vv[i].arr : 1);
        }
    }
    for (size_t i = 0; i < vf.size(); i++) {
        if (vf[i].clase != "varying") continue;
        int loc;
        if (locVar.count(vf[i].nombre)) loc = locVar[vf[i].nombre];
        else { loc = sigVar; sigVar += LocsDe(vf[i].tipo) * (vf[i].arr > 0 ? vf[i].arr : 1); }
        char l[160]; snprintf(l, sizeof(l), "layout(location=%d) in %s;\n", loc, Decl(vf[i]).c_str());
        of += l;
    }
    ov += ArreglarCuerpo(cv, true);
    ov += "\nvoid main(){ w3d_main_(); gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; }\n";   // (la z de GL, -1..1, a 0..1)
    of += "layout(location=0) out vec4 w3d_Color;\n"
          "vec4 w3dFragCoord(){ return vec4(gl_FragCoord.x, w3d_Ajuste.x > 0.5 ? w3d_Ajuste.y - gl_FragCoord.y : gl_FragCoord.y, gl_FragCoord.zw); }\n";
    of += ArreglarCuerpo(cf, false);
    std::string log;
    if (!CompilarSPV(ov, kShcVertex, p->mVS, log)) { p->log = "vertex: " + log; if (getenv("W3D_VK_SHADERS")) w3dLogf("%s", ov.c_str()); return false; }
    if (!CompilarSPV(of, kShcFragment, p->mFS, log)) { p->log = "fragment: " + log; if (getenv("W3D_VK_SHADERS")) w3dLogf("%s", of.c_str()); return false; }
    p->sucio = true; p->epocaSubida = 0;
    return true;
}
static void BorrarModulosYPipes(ProgGL* p) {
    for (std::map<ClavePipe, VkPipeline>::iterator it = p->pipes.begin(); it != p->pipes.end(); ++it) { Basura b = BasuraVacia(); b.pipe = it->second; Tirar(b); }
    p->pipes.clear();
    if (p->mVS) { Basura b = BasuraVacia(); b.mod = p->mVS; Tirar(b); p->mVS = VK_NULL_HANDLE; }
    if (p->mFS) { Basura b = BasuraVacia(); b.mod = p->mFS; Tirar(b); p->mFS = VK_NULL_HANDLE; }
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
static uint32_t OpEstencil(GLenum o) {
    switch (o) {
        case GL_ZERO: return VK_STENCIL_OP_ZERO; case GL_REPLACE: return VK_STENCIL_OP_REPLACE;
        case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case GL_INVERT: return VK_STENCIL_OP_INVERT; case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}
static uint32_t TopologiaVk(GLenum m) {
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
static void ClaveDeEstado(GLenum modo, ClavePipe& k) {
    const bool tri = modo == GL_TRIANGLES || modo == GL_TRIANGLE_STRIP || modo == GL_TRIANGLE_FAN;
    uint32_t cull = 0;
    if (g.cull && tri) cull = g.cullModo == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : g.cullModo == GL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_AND_BACK;
    // (dado vuelta, la pantalla deja el giro como en GL; una textura destino lo invierte)
    const bool ccw = (g.front == GL_CCW) == gPase.volteado;
    const bool prof = gPase.conProf;
    const uint32_t cm = (g.cMask[0] ? 1u : 0u) | (g.cMask[1] ? 2u : 0u) | (g.cMask[2] ? 4u : 0u) | (g.cMask[3] ? 8u : 0u);
    k.a = TopologiaVk(modo) | (cull << 3) | ((ccw ? 1u : 0u) << 5) | (((prof && g.depth) ? 1u : 0u) << 6)
        | (((prof && g.depth && g.dMask) ? 1u : 0u) << 7) | (((uint32_t)(g.dFunc - GL_NEVER) & 7u) << 8) | ((g.blend ? 1u : 0u) << 11)
        | ((g.blend ? (uint32_t)FactorVk(g.bSrc) : 0u) << 12) | ((g.blend ? (uint32_t)FactorVk(g.bDst) : 0u) << 16)
        | ((g.blend ? EqVk(g.bEq) : 0u) << 20) | (cm << 23) | (((prof && g.stencil) ? 1u : 0u) << 27)
        | (((g.offset && tri) ? 1u : 0u) << 28) | ((gPase.tex ? 1u : 0u) << 29);
    k.b = (prof && g.stencil) ? (((uint32_t)(g.sFunc - GL_NEVER) & 7u) | (OpEstencil(g.sFail) << 3) | (OpEstencil(g.sZFail) << 6) | (OpEstencil(g.sZPass) << 9)) : 0u;
}
static VkPipeline CrearPipe(ProgGL* p, const ClavePipe& k) {
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof(st));
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = p->mVS; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = p->mFS; st[1].pName = "main";
    VkVertexInputBindingDescription vb[kAttrs]; VkVertexInputAttributeDescription va[kAttrs]; uint32_t nv = 0;
    for (int i = 0; i < kAttrs; i++) {
        if (!(p->mascaraAtrib & (1u << i))) continue;
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
static int BytesTipo(GLenum t) { return (t == GL_FLOAT || t == GL_INT || t == GL_UNSIGNED_INT) ? 4 : (t == GL_SHORT || t == GL_UNSIGNED_SHORT) ? 2 : 1; }
static VkFormat FormatoAttr(GLenum t, int n, bool norm) {
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
static std::map<int, bool> gFmtVertice;
static bool FormatoLeible(VkFormat f) {
    std::map<int, bool>::iterator it = gFmtVertice.find((int)f);
    if (it != gFmtVertice.end()) return it->second;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(gFis, f, &fp);
    const bool ok = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    gFmtVertice[(int)f] = ok;
    return ok;
}
static float LeerComp(GLenum t, bool norm, const unsigned char* p) {
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
    const int bc = BytesTipo(a.type);
    for (int v = 0; v < n; v++, d += 4) {
        d[0] = d[1] = d[2] = 0.0f; d[3] = 1.0f;
        for (int c = 0; c < a.size && c < 4; c++) d[c] = LeerComp(a.type, a.norm != 0, src + (size_t)v * stride + c * bc);
    }
}
static BufGL* BufDe(GLuint id) { std::map<GLuint, BufGL*>::iterator it = gBufs.find(id); return it == gBufs.end() ? 0 : it->second; }
static Anillo SubirDinamico(BufGL* b) {
    if (b->epoca != gEpoca) {
        b->subido = Pedir(b->copia.size(), 16);
        if (!b->copia.empty()) memcpy(b->subido.p, &b->copia[0], b->copia.size());
        b->epoca = gEpoca;
    }
    return b->subido;
}
static bool SubirEstatico(BufVk& dst, const void* datos, size_t tam) {
    if (!CrearBuffer(tam, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dst)) return false;
    if (!tam) return true;
    Anillo r = Pedir(tam, 16);
    if (!r.b) return false;
    if (datos) memcpy(r.p, datos, tam); else memset(r.p, 0, tam);
    VkBufferCopy c = { r.off, 0, tam };
    vkCmdCopyBuffer(CmdPre(), r.b, dst.b, 1, &c);   // (buffer nuevo: nadie lo uso todavia, la copia va antes de todo)
    return true;
}
static void Constante(const AttrGL& a, VkBuffer& buf, VkDeviceSize& off, uint8_t& fmt, uint16_t& stride) {
    Anillo r = Pedir(16, 16); memcpy(r.p, a.cte, 16);
    buf = r.b; off = r.off; fmt = (uint8_t)VK_FORMAT_R32G32B32A32_SFLOAT; stride = 0;
}
static void AtributoDelDraw(int L, int nVerts, VkBuffer& buf, VkDeviceSize& off, uint8_t& fmt, uint16_t& stride) {
    const AttrGL& a = gAttr[L];
    if (!a.on) { Constante(a, buf, off, fmt, stride); return; }
    const int elem = a.size * BytesTipo(a.type);
    const int st = a.stride ? a.stride : elem;
    const VkFormat f = FormatoAttr(a.type, a.size, a.norm != 0);
    const bool leible = f != VK_FORMAT_UNDEFINED && FormatoLeible(f);
    if (a.buf) {
        BufGL* b = BufDe(a.buf);
        const size_t o = (size_t)a.ptr;
        if (!b || b->tam < o + elem) { Constante(a, buf, off, fmt, stride); return; }
        if (leible) {
            if (b->estatico) { buf = b->vk.b; off = o; }
            else { Anillo r = SubirDinamico(b); buf = r.b; off = r.off + o; }
            fmt = (uint8_t)f; stride = (uint16_t)st;
            return;
        }
        if (b->copia.empty()) { Constante(a, buf, off, fmt, stride); return; }
        const int n = (int)((b->tam - o - elem) / st) + 1;
        if (b->estatico) {
            const uint64_t clave = (uint64_t)a.type | ((uint64_t)a.size << 16) | ((uint64_t)(a.norm ? 1 : 0) << 20) | ((uint64_t)st << 24) | ((uint64_t)o << 40);
            std::map<uint64_t, BufVk>::iterator it = b->conv.find(clave);
            if (it == b->conv.end()) {
                std::vector<float> f4((size_t)n * 4);
                AFloat4(a, &b->copia[o], st, n, &f4[0]);
                BufVk nb; if (!SubirEstatico(nb, &f4[0], f4.size() * 4)) { Constante(a, buf, off, fmt, stride); return; }
                it = b->conv.insert(std::make_pair(clave, nb)).first;
            }
            buf = it->second.b; off = 0;
        } else {
            Anillo r = Pedir((VkDeviceSize)n * 16, 16);
            AFloat4(a, &b->copia[o], st, n, (float*)r.p);
            buf = r.b; off = r.off;
        }
        fmt = (uint8_t)VK_FORMAT_R32G32B32A32_SFLOAT; stride = 16;
        return;
    }
    if (!a.ptr || nVerts <= 0) { Constante(a, buf, off, fmt, stride); return; }
    // (arreglo de la RAM: lo que lee el draw, al anillo)
    if (leible) {
        const size_t bytes = (size_t)st * (nVerts - 1) + elem;
        Anillo r = Pedir(bytes, 16); memcpy(r.p, a.ptr, bytes);
        buf = r.b; off = r.off; fmt = (uint8_t)f; stride = (uint16_t)st;
    } else {
        Anillo r = Pedir((VkDeviceSize)nVerts * 16, 16);
        AFloat4(a, (const unsigned char*)a.ptr, st, nVerts, (float*)r.p);
        buf = r.b; off = r.off; fmt = (uint8_t)VK_FORMAT_R32G32B32A32_SFLOAT; stride = 16;
    }
}
static bool HayArreglosDeRAM(const ProgGL* p) {
    for (int i = 0; i < kAttrs; i++) if ((p->mascaraAtrib & (1u << i)) && gAttr[i].on && !gAttr[i].buf && gAttr[i].ptr) return true;
    return false;
}

// ============================================================================
//  EL DRAW
// ============================================================================
static VkDescriptorSet SetDelDraw(ProgGL* p, const Anillo& u) {
    // (la clave: el buffer de los uniforms y lo que lee cada sampler)
    std::string clave((const char*)&u.b, sizeof(u.b));
    clave.append((const char*)&p->tamBloque, sizeof(int));
    VkImageView vistas[kMuestras]; VkSampler samps[kMuestras];
    for (int i = 0; i < p->nMuestras; i++) {
        const int un = p->unidad[i] >= 0 && p->unidad[i] < kUnidades ? p->unidad[i] : 0;
        TexGL* t = gUnidad[un] ? TexDe(gUnidad[un]) : 0;
        if (!t || !t->hay || !t->iniciada) t = gNegra;
        t->usoCmd = gSerialCmd;
        vistas[i] = t->vista; samps[i] = SamplerDe(t);
        clave.append((const char*)&vistas[i], sizeof(VkImageView)); clave.append((const char*)&samps[i], sizeof(VkSampler));
    }
    std::map<std::string, VkDescriptorSet>::iterator it = gCacheSets.find(clave);
    if (it != gCacheSets.end()) return it->second;
    Cuadro& c = gCuadros[gCur];
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
        if (c.pool >= c.pools.size()) {
            VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 * 4 } };
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
    VkDescriptorBufferInfo bi = { u.b, 0, (VkDeviceSize)p->tamBloque };
    VkDescriptorImageInfo ii[kMuestras];
    VkWriteDescriptorSet w[1 + kMuestras]; memset(w, 0, sizeof(w));
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &bi;
    for (int i = 0; i < p->nMuestras; i++) {
        ii[i].sampler = samps[i]; ii[i].imageView = vistas[i]; ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1 + i].dstSet = set; w[1 + i].dstBinding = (uint32_t)i + 1;
        w[1 + i].descriptorCount = 1; w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1 + i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(gDev, (uint32_t)(1 + p->nMuestras), w, 0, 0);
    gCacheSets[clave] = set;
    return set;
}
// el recorte del draw (el scissor de GL en la imagen); false = no queda nada
static bool Recorte(VkRect2D& r) {
    int x = 0, y = 0, w = gPase.w, h = gPase.h;
    if (g.scissor) {
        x = g.sc[0]; w = g.sc[2]; h = g.sc[3];
        y = gPase.volteado ? gPase.h - g.sc[1] - g.sc[3] : g.sc[1];
        if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
        if (x + w > gPase.w) w = gPase.w - x; if (y + h > gPase.h) h = gPase.h - y;
    }
    if (w <= 0 || h <= 0) return false;
    r.offset.x = x; r.offset.y = y; r.extent.width = (uint32_t)w; r.extent.height = (uint32_t)h;
    return true;
}
static VkCommandBuffer PrepararDraw(GLenum modo, int nVerts) {
    ProgGL* p = gProg;
    if (!p || !p->ok || g.vp[2] <= 0 || g.vp[3] <= 0) return VK_NULL_HANDLE;
    if (!EmpezarPase()) return VK_NULL_HANDLE;
    VkRect2D sc; if (!Recorte(sc)) return VK_NULL_HANDLE;
    ClavePipe k; memset(&k, 0, sizeof(k));
    VkBuffer bufs[kAttrs]; VkDeviceSize offs[kAttrs];
    for (int i = 0; i < kAttrs; i++) if (p->mascaraAtrib & (1u << i)) AtributoDelDraw(i, nVerts, bufs[i], offs[i], k.fmt[i], k.stride[i]);
    // el bloque de uniforms (y lo del destino que usa gl_FragCoord)
    const float aj[4] = { gPase.volteado ? 1.0f : 0.0f, (float)gPase.h, 0.0f, 0.0f };
    if (memcmp(&p->bloque[p->offAjuste], aj, 16) != 0) { memcpy(&p->bloque[p->offAjuste], aj, 16); p->sucio = true; }
    if (p->sucio || p->epocaSubida != gEpoca) {
        p->subido = Pedir((VkDeviceSize)p->tamBloque, gAlinUBO);
        memcpy(p->subido.p, &p->bloque[0], (size_t)p->tamBloque);
        p->sucio = false; p->epocaSubida = gEpoca;
    }
    VkDescriptorSet set = SetDelDraw(p, p->subido);
    ClaveDeEstado(modo, k);
    std::map<ClavePipe, VkPipeline>::iterator it = p->pipes.find(k);
    VkPipeline pipe = it != p->pipes.end() ? it->second : (p->pipes[k] = CrearPipe(p, k));
    if (!pipe || !set) return VK_NULL_HANDLE;
    VkCommandBuffer cb = Cmd();
    if (gEc.pipe != pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); gEc.pipe = pipe; }
    VkViewport vp;
    vp.x = (float)g.vp[0]; vp.width = (float)g.vp[2];
    if (gPase.volteado) { vp.y = (float)(gPase.h - g.vp[1]); vp.height = -(float)g.vp[3]; }
    else { vp.y = (float)g.vp[1]; vp.height = (float)g.vp[3]; }
    vp.minDepth = g.zn; vp.maxDepth = g.zf;
    if (!gEc.ok || memcmp(&vp, &gEc.vp, sizeof(vp)) != 0) { vkCmdSetViewport(cb, 0, 1, &vp); gEc.vp = vp; }
    if (!gEc.ok || memcmp(&sc, &gEc.sc, sizeof(sc)) != 0) { vkCmdSetScissor(cb, 0, 1, &sc); gEc.sc = sc; }
    const float linea = gFeat.wideLines ? (g.linea < 1.0f ? 1.0f : g.linea > gProps.limits.lineWidthRange[1] ? gProps.limits.lineWidthRange[1] : g.linea) : 1.0f;
    if (!gEc.ok || linea != gEc.linea) { vkCmdSetLineWidth(cb, linea); gEc.linea = linea; }
    if (!gEc.ok || g.offU != gEc.biasC || g.offF != gEc.biasP) { vkCmdSetDepthBias(cb, g.offU, 0.0f, g.offF); gEc.biasC = g.offU; gEc.biasP = g.offF; }
    if (!gEc.ok || (g.sMaskR & 0xFF) != gEc.sCmp) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g.sMaskR & 0xFF); gEc.sCmp = g.sMaskR & 0xFF; }
    if (!gEc.ok || (g.sMaskW & 0xFF) != gEc.sWr) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g.sMaskW & 0xFF); gEc.sWr = g.sMaskW & 0xFF; }
    if (!gEc.ok || ((uint32_t)g.sRef & 0xFF) != gEc.sRef) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)g.sRef & 0xFF); gEc.sRef = (uint32_t)g.sRef & 0xFF; }
    gEc.ok = true;
    const uint32_t dyn = (uint32_t)p->subido.off;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gLayout, 0, 1, &set, 1, &dyn);
    for (int i = 0; i < kAttrs; i++) if (p->mascaraAtrib & (1u << i)) vkCmdBindVertexBuffers(cb, (uint32_t)i, 1, &bufs[i], &offs[i]);
    return cb;
}
// (el anillo crece con cada draw: si un cuadro sin presentar junta demasiado, se manda a la GPU y se recicla)
static void AnilloControlar() {
    if (AnilloUsado() > (256ull << 20) || gCuadros[gCur].basura.size() > 4096) { EnviarYEsperar(); ReciclarTodo(); }
}
static void DibujarIndices32(GLenum modo, const uint32_t* idx, int n, int nVerts) {
    Anillo r = Pedir((VkDeviceSize)n * 4, 16); memcpy(r.p, idx, (size_t)n * 4);
    VkCommandBuffer cb = PrepararDraw(modo, nVerts);
    if (!cb) return;
    vkCmdBindIndexBuffer(cb, r.b, r.off, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, (uint32_t)n, 1, 0, 0, 0);
}
void vkglDrawArrays(GLenum modo, GLint first, GLsizei count) {
    if (!gListo || count <= 0 || first < 0) return;
    AnilloControlar();
    if (modo == GL_LINE_LOOP) {   // (Vulkan no tiene lineas cerradas: una tira que vuelve al primero)
        std::vector<uint32_t> idx((size_t)count + 1);
        for (int i = 0; i < count; i++) idx[i] = (uint32_t)(first + i);
        idx[count] = (uint32_t)first;
        DibujarIndices32(GL_LINE_STRIP, &idx[0], count + 1, first + count);
        return;
    }
    VkCommandBuffer cb = PrepararDraw(modo, first + count);
    if (cb) vkCmdDraw(cb, (uint32_t)count, 1, (uint32_t)first, 0);
}
void vkglDrawElements(GLenum modo, GLsizei count, GLenum tipo, const void* ind) {
    if (!gListo || count <= 0 || !gProg) return;
    AnilloControlar();
    const size_t isz = tipo == GL_UNSIGNED_INT ? 4 : tipo == GL_UNSIGNED_SHORT ? 2 : 1;
    const unsigned char* cpu = 0; VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize io = 0;
    BufGL* b = gBufElem ? BufDe(gBufElem) : 0;
    if (b) {
        const size_t off = (size_t)ind;
        if (off + (size_t)count * isz > b->tam) { ErrorGL(GL_INVALID_OPERATION); return; }
        cpu = b->copia.empty() ? 0 : &b->copia[off];
        if (b->estatico) { ib = b->vk.b; io = off; } else { Anillo r = SubirDinamico(b); ib = r.b; io = r.off + off; }
    } else {
        cpu = (const unsigned char*)ind;
        if (!cpu) return;
    }
    const bool ram = HayArreglosDeRAM(gProg);
    if (tipo == GL_UNSIGNED_BYTE || modo == GL_LINE_LOOP || !b || (ram && cpu)) {
        if (!cpu) { ErrorGL(GL_INVALID_OPERATION); return; }
        std::vector<uint32_t> idx((size_t)count + (modo == GL_LINE_LOOP ? 1 : 0));
        uint32_t maxI = 0;
        for (int i = 0; i < count; i++) {
            const uint32_t v = isz == 4 ? ((const uint32_t*)cpu)[i] : isz == 2 ? ((const unsigned short*)cpu)[i] : cpu[i];
            idx[i] = v; if (v > maxI) maxI = v;
        }
        if (modo == GL_LINE_LOOP) idx[count] = idx[0];
        DibujarIndices32(modo == GL_LINE_LOOP ? GL_LINE_STRIP : modo, &idx[0], (int)idx.size(), (int)maxI + 1);
        return;
    }
    VkCommandBuffer cb = PrepararDraw(modo, 0);
    if (!cb) return;
    vkCmdBindIndexBuffer(cb, ib, io, isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cb, (uint32_t)count, 1, 0, 0, 0);
}

// ============================================================================
//  Estado
// ============================================================================
static bool* CapDe(GLenum c) {
    switch (c) {
        case GL_BLEND: return &g.blend; case GL_DEPTH_TEST: return &g.depth; case GL_CULL_FACE: return &g.cull;
        case GL_SCISSOR_TEST: return &g.scissor; case GL_STENCIL_TEST: return &g.stencil; case GL_POLYGON_OFFSET_FILL: return &g.offset;
    }
    return 0;
}
void vkglEnable(GLenum c) { bool* b = CapDe(c); if (b) *b = true; }
void vkglDisable(GLenum c) { bool* b = CapDe(c); if (b) *b = false; }
GLboolean vkglIsEnabled(GLenum c) { bool* b = CapDe(c); return (b && *b) ? GL_TRUE : GL_FALSE; }
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
void vkglLineWidth(GLfloat w) { g.linea = w; }
void vkglFrontFace(GLenum m) { g.front = m; }
void vkglCullFace(GLenum m) { g.cullModo = m; }
void vkglDepthRangef(GLfloat n, GLfloat f) { g.zn = n < 0 ? 0 : n > 1 ? 1 : n; g.zf = f < 0 ? 0 : f > 1 ? 1 : f; }
void vkglPixelStorei(GLenum p, GLint v) { if (p == GL_UNPACK_ALIGNMENT) g.unpack = v; else if (p == GL_PACK_ALIGNMENT) g.pack = v; }
void vkglFlush() {}
void vkglFinish() { if (gListo) { EnviarYEsperar(); ReciclarTodo(); } }
GLenum vkglGetError() { const GLenum e = gErrorGL; gErrorGL = GL_NO_ERROR; return e; }
void vkglClear(GLbitfield m) {
    if (!gListo || !EmpezarPase()) return;
    VkRect2D r; if (!Recorte(r)) return;
    VkClearAttachment at[2]; uint32_t n = 0;
    if ((m & GL_COLOR_BUFFER_BIT) && (g.cMask[0] || g.cMask[1] || g.cMask[2] || g.cMask[3])) {
        at[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; at[n].colorAttachment = 0;
        for (int i = 0; i < 4; i++) at[n].clearValue.color.float32[i] = g.clear[i];
        n++;
    }
    VkImageAspectFlags asp = 0;
    if (gPase.conProf && (m & GL_DEPTH_BUFFER_BIT) && g.dMask) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (gPase.conProf && (m & GL_STENCIL_BUFFER_BIT) && (g.sMaskW & 0xFF)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
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
        case GL_FRAMEBUFFER_BINDING: v[0] = (GLint)gFboActual; break;
        case GL_TEXTURE_BINDING_2D: v[0] = (GLint)gUnidad[gUnidadActiva]; break;
        default: v[0] = 0; break;
    }
}
void vkglGetFloatv(GLenum p, GLfloat* v) {
    switch (p) {
        case GL_COLOR_CLEAR_VALUE: for (int i = 0; i < 4; i++) v[i] = g.clear[i]; break;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: v[0] = gFeat.samplerAnisotropy ? gProps.limits.maxSamplerAnisotropy : 1.0f; break;
        case GL_LINE_WIDTH: v[0] = g.linea; break;
        default: v[0] = 0.0f; break;
    }
}
const GLubyte* vkglGetString(GLenum p) {
    static std::string s;
    switch (p) {
        case GL_VENDOR: s = "Vulkan"; break;
        case GL_RENDERER: s = gNombreGPU + " (Vulkan)"; break;
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
void vkglGenBuffers(GLsizei n, GLuint* b) { for (int i = 0; i < n; i++) { b[i] = gSigBuf++; BufGL* x = new BufGL(); x->tam = 0; x->estatico = x->elementos = false; memset(&x->vk, 0, sizeof(x->vk)); x->epoca = 0; gBufs[b[i]] = x; } }
static void TirarBufGL(BufGL* b) {
    TirarBuffer(b->vk);
    for (std::map<uint64_t, BufVk>::iterator it = b->conv.begin(); it != b->conv.end(); ++it) TirarBuffer(it->second);
    b->conv.clear();
}
void vkglDeleteBuffers(GLsizei n, const GLuint* b) {
    for (int i = 0; i < n; i++) {
        BufGL* x = BufDe(b[i]);
        if (!x) continue;
        TirarBufGL(x); delete x; gBufs.erase(b[i]);
        if (gBufArray == b[i]) gBufArray = 0;
        if (gBufElem == b[i]) gBufElem = 0;
        for (int k = 0; k < kAttrs; k++) if (gAttr[k].buf == b[i]) gAttr[k].buf = 0;
    }
}
void vkglBindBuffer(GLenum t, GLuint b) {
    if (b && !BufDe(b)) { BufGL* x = new BufGL(); x->tam = 0; x->estatico = x->elementos = false; memset(&x->vk, 0, sizeof(x->vk)); x->epoca = 0; gBufs[b] = x; }
    if (t == GL_ARRAY_BUFFER) gBufArray = b; else if (t == GL_ELEMENT_ARRAY_BUFFER) gBufElem = b;
}
void vkglBufferData(GLenum t, GLsizeiptr tam, const void* datos, GLenum uso) {
    BufGL* b = BufDe(t == GL_ELEMENT_ARRAY_BUFFER ? gBufElem : gBufArray);
    if (!b || tam < 0 || !gListo) { ErrorGL(GL_INVALID_OPERATION); return; }
    TirarBufGL(b);
    b->tam = (size_t)tam; b->elementos = t == GL_ELEMENT_ARRAY_BUFFER;
    b->estatico = uso == GL_STATIC_DRAW;
    b->epoca = 0;
    const bool copia = !b->estatico || b->elementos || gGuardarCopias;
    if (copia) { if (datos) b->copia.assign((const unsigned char*)datos, (const unsigned char*)datos + tam); else b->copia.assign((size_t)tam, 0); }
    else { std::vector<unsigned char>().swap(b->copia); }
    if (b->estatico && !SubirEstatico(b->vk, datos, (size_t)tam)) { ErrorGL(GL_OUT_OF_MEMORY); b->tam = 0; }
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
GLuint vkglCreateShader(GLenum tipo) { ShaderGL* s = new ShaderGL(); s->tipo = tipo; s->ok = false; const GLuint id = gSigNombre++; gShaders[id] = s; return id; }
void vkglShaderSource(GLuint id, GLsizei n, const GLchar* const* partes, const GLint* largos) {
    if (!gShaders.count(id)) return;
    std::string& f = gShaders[id]->fuente; f.clear();
    for (int i = 0; i < n; i++) { if (largos && largos[i] >= 0) f.append(partes[i], (size_t)largos[i]); else f += partes[i]; }
}
void vkglCompileShader(GLuint id) {
    if (!gShaders.count(id) || !gShc) return;
    ShaderGL* s = gShaders[id];
    s->log.clear();
    s->ok = Preprocesar(s->fuente, s->tipo == GL_VERTEX_SHADER ? kShcVertex : kShcFragment, s->pre, s->log);
}
void vkglGetShaderiv(GLuint id, GLenum p, GLint* v) {
    ShaderGL* s = gShaders.count(id) ? gShaders[id] : 0;
    if (!s) { v[0] = 0; return; }
    if (p == GL_COMPILE_STATUS) v[0] = s->ok ? 1 : 0; else if (p == GL_INFO_LOG_LENGTH) v[0] = (GLint)s->log.size() + 1; else v[0] = 0;
}
static void CopiarLog(const std::string& l, GLsizei max, GLsizei* largo, GLchar* out) {
    if (max <= 0) return;
    const size_t n = l.size() < (size_t)max - 1 ? l.size() : (size_t)max - 1;
    memcpy(out, l.c_str(), n); out[n] = 0;
    if (largo) *largo = (GLsizei)n;
}
void vkglGetShaderInfoLog(GLuint id, GLsizei max, GLsizei* largo, GLchar* log) { CopiarLog(gShaders.count(id) ? gShaders[id]->log : std::string(), max, largo, log); }
void vkglDeleteShader(GLuint id) { std::map<GLuint, ShaderGL*>::iterator it = gShaders.find(id); if (it != gShaders.end()) { delete it->second; gShaders.erase(it); } }
GLuint vkglCreateProgram() {
    ProgGL* p = new ProgGL();
    p->vs = p->fs = 0; p->ok = false; p->mVS = p->mFS = VK_NULL_HANDLE; p->tamBloque = 0; p->offAjuste = 0; p->nMuestras = 0;
    p->mascaraAtrib = 0; p->sucio = true; p->epocaSubida = 0; memset(&p->subido, 0, sizeof(p->subido));
    const GLuint id = gSigNombre++; gProgs[id] = p; return id;
}
static ProgGL* ProgDe(GLuint id) { std::map<GLuint, ProgGL*>::iterator it = gProgs.find(id); return it == gProgs.end() ? 0 : it->second; }
void vkglAttachShader(GLuint pid, GLuint sid) {
    ProgGL* p = ProgDe(pid); ShaderGL* s = gShaders.count(sid) ? gShaders[sid] : 0;
    if (!p || !s) return;
    if (s->tipo == GL_VERTEX_SHADER) p->vs = sid; else p->fs = sid;
}
void vkglBindAttribLocation(GLuint pid, GLuint i, const GLchar* n) { ProgGL* p = ProgDe(pid); if (p && n && i < (GLuint)kAttrs) p->atribPedidos[n] = (int)i; }
void vkglLinkProgram(GLuint pid) {
    ProgGL* p = ProgDe(pid);
    if (!p) return;
    BorrarModulosYPipes(p);
    p->log.clear();
    p->ok = gListo && Linkear(p);
    if (!p->ok) w3dLogfE("[vk] el programa no linkeo: %s", p->log.c_str());
}
void vkglGetProgramiv(GLuint pid, GLenum q, GLint* v) {
    ProgGL* p = ProgDe(pid);
    if (!p) { v[0] = 0; return; }
    if (q == GL_LINK_STATUS) v[0] = p->ok ? 1 : 0; else if (q == GL_INFO_LOG_LENGTH) v[0] = (GLint)p->log.size() + 1; else v[0] = 0;
}
void vkglGetProgramInfoLog(GLuint pid, GLsizei max, GLsizei* largo, GLchar* log) { ProgGL* p = ProgDe(pid); CopiarLog(p ? p->log : std::string(), max, largo, log); }
void vkglUseProgram(GLuint pid) { gProg = ProgDe(pid); gProgId = gProg ? pid : 0; }
void vkglDeleteProgram(GLuint pid) {
    ProgGL* p = ProgDe(pid);
    if (!p) return;
    BorrarModulosYPipes(p);
    if (gProg == p) { gProg = 0; gProgId = 0; }
    delete p; gProgs.erase(pid);
}
GLint vkglGetUniformLocation(GLuint pid, const GLchar* nombre) {
    ProgGL* p = ProgDe(pid);
    if (!p || !p->ok || !nombre) return -1;
    std::string n = nombre; int idx = 0;
    const size_t c = n.find('[');
    if (c != std::string::npos) { idx = atoi(n.c_str() + c + 1); n = n.substr(0, c); }
    for (size_t i = 0; i < p->unis.size(); i++)
        if (p->unis[i].nombre == n) return idx < p->unis[i].elems ? p->unis[i].loc0 + idx : -1;
    return -1;
}
GLint vkglGetAttribLocation(GLuint pid, const GLchar* nombre) {
    ProgGL* p = ProgDe(pid);
    if (!p || !nombre) return -1;
    std::map<std::string, int>::iterator it = p->atribs.find(nombre);
    return it == p->atribs.end() ? -1 : it->second;
}
// escribir en el bloque del programa en uso (sucio solo si cambio algo)
static void Escribir(ProgGL* p, int off, const void* v, size_t n) {
    if (off < 0 || (size_t)off + n > p->bloque.size()) return;
    if (memcmp(&p->bloque[(size_t)off], v, n) != 0) { memcpy(&p->bloque[(size_t)off], v, n); p->sucio = true; }
}
static const UniGL* UniDe(ProgGL* p, GLint loc, int& elem) {
    if (!p || loc < 0 || loc >= (GLint)p->locs.size()) return 0;
    elem = p->locs[loc].elem;
    return &p->unis[p->locs[loc].uni];
}
void vkglUniform1i(GLint loc, GLint v) {
    int e; const UniGL* u = UniDe(gProg, loc, e); if (!u) return;
    if (u->muestra >= 0) { gProg->unidad[u->muestra] = v; return; }
    if (u->t->clase == 0) { const float f = (float)v; Escribir(gProg, u->off + e * u->stride, &f, 4); }
    else { const int32_t i = u->t->clase == 2 ? (v != 0) : v; Escribir(gProg, u->off + e * u->stride, &i, 4); }
}
void vkglUniform1f(GLint loc, GLfloat v) {
    int e; const UniGL* u = UniDe(gProg, loc, e); if (!u || u->muestra >= 0) return;
    if (u->t->clase == 0) Escribir(gProg, u->off + e * u->stride, &v, 4);
    else { const int32_t i = u->t->clase == 2 ? (v != 0.0f) : (int32_t)v; Escribir(gProg, u->off + e * u->stride, &i, 4); }
}
void vkglUniform4fv(GLint loc, GLsizei n, const GLfloat* v) {
    int e; const UniGL* u = UniDe(gProg, loc, e); if (!u || u->muestra >= 0 || u->t->clase == 3) return;
    const int comps = u->t->tam / 4;
    for (int k = 0; k < n && e + k < u->elems; k++) Escribir(gProg, u->off + (e + k) * u->stride, v + k * 4, (size_t)(comps < 4 ? comps : 4) * 4);
}
void vkglUniformMatrix3fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat* v) {
    int e; const UniGL* u = UniDe(gProg, loc, e); if (!u || u->t->cols != 3) return;
    for (int k = 0; k < n && e + k < u->elems; k++) {
        const GLfloat* m = v + k * 9;
        for (int c = 0; c < 3; c++) {
            float col[3];
            for (int r = 0; r < 3; r++) col[r] = tr ? m[r * 3 + c] : m[c * 3 + r];
            Escribir(gProg, u->off + (e + k) * u->stride + c * 16, col, 12);
        }
    }
}
void vkglUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat* v) {
    int e; const UniGL* u = UniDe(gProg, loc, e); if (!u || u->t->cols != 4) return;
    for (int k = 0; k < n && e + k < u->elems; k++) {
        const GLfloat* m = v + k * 16;
        if (!tr) { Escribir(gProg, u->off + (e + k) * u->stride, m, 64); continue; }
        float t[16]; for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[c * 4 + r] = m[r * 4 + c];
        Escribir(gProg, u->off + (e + k) * u->stride, t, 64);
    }
}

// ============================================================================
//  Texturas (GL)
// ============================================================================
void vkglActiveTexture(GLenum u) { const int i = (int)u - GL_TEXTURE0; if (i >= 0 && i < kUnidades) gUnidadActiva = i; }
void vkglGenTextures(GLsizei n, GLuint* t) { for (int i = 0; i < n; i++) { while (gTex.count(gSigTex)) gSigTex++; t[i] = gSigTex++; gTex[t[i]] = NuevaTex(); } }
void vkglDeleteTextures(GLsizei n, const GLuint* t) {
    for (int i = 0; i < n; i++) {
        std::map<GLuint, TexGL*>::iterator it = gTex.find(t[i]);
        if (!t[i] || it == gTex.end()) continue;
        if (gListo) TirarImagenTex(it->second);
        delete it->second; gTex.erase(it);
        for (int k = 0; k < kUnidades; k++) if (gUnidad[k] == t[i]) gUnidad[k] = 0;
        for (std::map<GLuint, GLuint>::iterator f = gFbos.begin(); f != gFbos.end(); ++f) if (f->second == t[i]) f->second = 0;
    }
}
void vkglBindTexture(GLenum, GLuint t) { gUnidad[gUnidadActiva] = t; if (t && !gTex.count(t)) gTex[t] = NuevaTex(); }
void vkglTexImage2D(GLenum, GLint nivel, GLint, GLsizei w, GLsizei h, GLint, GLenum formato, GLenum tipo, const void* datos) {
    TexGL* t = TexActual();
    if (!t || !gListo || w <= 0 || h <= 0 || nivel < 0) { ErrorGL(GL_INVALID_VALUE); return; }
    if (nivel == 0) {
        if ((!t->hay || t->w != w || t->h != h) && !CrearImagenTex(t, w, h)) { ErrorGL(GL_OUT_OF_MEMORY); return; }
    } else if (!t->hay || nivel >= t->niveles) { ErrorGL(GL_INVALID_OPERATION); return; }
    if (datos || !t->iniciada) Subir(t, nivel, 0, 0, w, h, formato, tipo, datos);
    if (nivel > 0 && nivel == t->nivelesDef) { t->nivelesDef = nivel + 1; RehacerVista(t); }
    if (nivel == 0 && t->genMip) GenerarMips(t);
}
void vkglTexSubImage2D(GLenum, GLint nivel, GLint x, GLint y, GLsizei w, GLsizei h, GLenum formato, GLenum tipo, const void* datos) {
    TexGL* t = TexActual();
    if (!t || !t->hay || !datos || w <= 0 || h <= 0) return;
    const int nw = t->w >> nivel, nh = t->h >> nivel;
    if (x < 0 || y < 0 || x + w > (nw > 0 ? nw : 1) || y + h > (nh > 0 ? nh : 1)) { ErrorGL(GL_INVALID_VALUE); return; }
    Subir(t, nivel, x, y, w, h, formato, tipo, datos);
    if (nivel == 0 && t->genMip) GenerarMips(t);
}
void vkglTexParameteri(GLenum, GLenum p, GLint v) {
    TexGL* t = TexActual(); if (!t) return;
    switch (p) {
        case GL_TEXTURE_MIN_FILTER: t->minF = v; break;
        case GL_TEXTURE_MAG_FILTER: t->magF = v; break;
        case GL_TEXTURE_WRAP_S: t->wrapS = v; break;
        case GL_TEXTURE_WRAP_T: t->wrapT = v; break;
        case GL_GENERATE_MIPMAP: t->genMip = v != 0; break;
    }
}
void vkglTexParameterf(GLenum target, GLenum p, GLfloat v) {
    if (p == GL_TEXTURE_MAX_ANISOTROPY_EXT) { TexGL* t = TexActual(); if (t) t->aniso = v; return; }
    vkglTexParameteri(target, p, (GLint)v);
}
void vkglGenerateMipmap(GLenum) { TexGL* t = TexActual(); if (t && gListo) GenerarMips(t); }
// la imagen del destino actual, lista para leerla (TRANSFER_SRC); devuelve su layout de siempre para volver
static VkImageLayout AbrirLectura(VkCommandBuffer cb, const Destino& d) {
    const VkImageLayout de = d.tex ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    Barrera(cb, d.img, VK_IMAGE_ASPECT_COLOR_BIT, de, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    return de;
}
void vkglCopyTexSubImage2D(GLenum, GLint nivel, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h) {
    TexGL* t = TexActual();
    if (!t || !t->hay || !gListo || w <= 0 || h <= 0) return;
    const Destino d = DestinoActual();
    if (d.tex == t) return;   // (copiarse a si misma: no)
    // (lo que cae afuera del destino no se copia)
    if (x < 0) { xoff -= x; w += x; x = 0; } if (y < 0) { yoff -= y; h += y; y = 0; }
    if (x + w > d.w) w = d.w - x; if (y + h > d.h) h = d.h - y;
    if (w <= 0 || h <= 0) return;
    TerminarPase();
    VkCommandBuffer cb = Cmd();
    const VkImageLayout vuelta = AbrirLectura(cb, d);
    TexAbrirCopia(cb, t);
    VkImageBlit b; memset(&b, 0, sizeof(b));
    b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1; b.dstSubresource.mipLevel = (uint32_t)nivel;
    b.srcOffsets[0].x = x; b.srcOffsets[1].x = x + w; b.srcOffsets[0].z = 0; b.srcOffsets[1].z = 1;
    if (d.volteado) { b.srcOffsets[0].y = d.h - y; b.srcOffsets[1].y = d.h - y - h; }   // (la fila de abajo de GL primero)
    else { b.srcOffsets[0].y = y; b.srcOffsets[1].y = y + h; }
    b.dstOffsets[0].x = xoff; b.dstOffsets[0].y = yoff; b.dstOffsets[1].x = xoff + w; b.dstOffsets[1].y = yoff + h; b.dstOffsets[1].z = 1;
    vkCmdBlitImage(cb, d.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    TexCerrarCopia(cb, t);
    Barrera(cb, d.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vuelta, 0, 1);
    t->usoCmd = gSerialCmd;
}
void vkglReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum formato, GLenum tipo, void* px) {
    if (!gListo || w <= 0 || h <= 0 || !px) return;
    if (formato != GL_RGBA || tipo != GL_UNSIGNED_BYTE) { ErrorGL(GL_INVALID_ENUM); return; }
    const Destino d = DestinoActual();
    unsigned char* out = (unsigned char*)px;
    memset(out, 0, (size_t)w * h * 4);
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > d.w) x1 = d.w; if (y1 > d.h) y1 = d.h;
    if (x1 <= x0 || y1 <= y0) return;
    const int cw = x1 - x0, ch = y1 - y0;
    TerminarPase();
    VkCommandBuffer cb = Cmd();
    Anillo r = Pedir((VkDeviceSize)cw * ch * 4, 16);
    if (!r.b) return;
    const VkImageLayout vuelta = AbrirLectura(cb, d);
    VkBufferImageCopy c; memset(&c, 0, sizeof(c));
    c.bufferOffset = r.off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = x0; c.imageOffset.y = d.volteado ? d.h - y1 : y0;
    c.imageExtent.width = (uint32_t)cw; c.imageExtent.height = (uint32_t)ch; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, d.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.b, 1, &c);
    Barrera(cb, d.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vuelta, 0, 1);
    EnviarYEsperar();
    for (int fy = 0; fy < ch; fy++) {   // (GL: la fila de abajo primero)
        const int src = d.volteado ? ch - 1 - fy : fy;
        memcpy(out + ((size_t)(y0 - y + fy) * w + (x0 - x)) * 4, r.p + (size_t)src * cw * 4, (size_t)cw * 4);
    }
    ReciclarTodo();
}

// ============================================================================
//  Framebuffers
// ============================================================================
void vkglGenFramebuffers(GLsizei n, GLuint* f) { for (int i = 0; i < n; i++) { f[i] = gSigFbo++; gFbos[f[i]] = 0; } }
void vkglDeleteFramebuffers(GLsizei n, const GLuint* f) { for (int i = 0; i < n; i++) { if (gFboActual == f[i]) vkglBindFramebuffer(GL_FRAMEBUFFER, 0); gFbos.erase(f[i]); } }
void vkglBindFramebuffer(GLenum, GLuint f) { if (f == gFboActual) return; TerminarPase(); gFboActual = f; }
void vkglFramebufferTexture2D(GLenum, GLenum adjunto, GLenum, GLuint t, GLint) {
    if (!gFboActual || adjunto != GL_COLOR_ATTACHMENT0) return;
    TerminarPase();
    gFbos[gFboActual] = t;
}
GLenum vkglCheckFramebufferStatus(GLenum) {
    if (!gFboActual) return GL_FRAMEBUFFER_COMPLETE;
    TexGL* t = TexDe(gFbos[gFboActual]);
    return (t && t->hay && FboDeTextura(t)) ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
}

// ============================================================================
//  LA PANTALLA (la imagen propia) y LA VENTANA (la swapchain)
// ============================================================================
static void DestruirPantalla() {
    if (gPan.fb) vkDestroyFramebuffer(gDev, gPan.fb, 0);
    if (gPan.vc) vkDestroyImageView(gDev, gPan.vc, 0);
    if (gPan.vp) vkDestroyImageView(gDev, gPan.vp, 0);
    if (gPan.color) vkDestroyImage(gDev, gPan.color, 0);
    if (gPan.prof) vkDestroyImage(gDev, gPan.prof, 0);
    Liberar(gPan.ac); Liberar(gPan.ap);
    memset(&gPan, 0, sizeof(gPan));
}
static bool CrearImagen(VkFormat f, int w, int h, VkImageUsageFlags uso, VkImageAspectFlags asp, VkImage& img, Asig& a, VkImageView& v) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f; ii.extent.width = (uint32_t)w; ii.extent.height = (uint32_t)h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = uso;
    if (vkCreateImage(gDev, &ii, 0, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements r; vkGetImageMemoryRequirements(gDev, img, &r);
    if (!Reservar(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, a)) return false;
    vkBindImageMemory(gDev, img, a.b->mem, a.off);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
    vi.subresourceRange.aspectMask = asp; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(gDev, &vi, 0, &v) == VK_SUCCESS;
}
static bool CrearPantalla(int w, int h) {
    DestruirPantalla();
    if (w <= 0 || h <= 0) return false;
    if (!CrearImagen(kFmtColor, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, gPan.color, gPan.ac, gPan.vc)) return false;
    if (!CrearImagen(gFmtProf, w, h, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, gPan.prof, gPan.ap, gPan.vp)) return false;
    VkImageView vs[2] = { gPan.vc, gPan.vp };
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = gRpDef; fi.attachmentCount = 2; fi.pAttachments = vs; fi.width = (uint32_t)w; fi.height = (uint32_t)h; fi.layers = 1;
    if (vkCreateFramebuffer(gDev, &fi, 0, &gPan.fb) != VK_SUCCESS) return false;
    gPan.w = w; gPan.h = h;
    // en negro y la profundidad al fondo, en los layouts de siempre
    VkCommandBuffer cb = CmdPre();
    Barrera(cb, gPan.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue negro; memset(&negro, 0, sizeof(negro)); negro.float32[3] = 1.0f;
    VkImageSubresourceRange rc = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, gPan.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &negro, 1, &rc);
    Barrera(cb, gPan.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkImageAspectFlags ad = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    Barrera(cb, gPan.prof, ad, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue fondo = { 1.0f, 0 };
    VkImageSubresourceRange rd = { ad, 0, 1, 0, 1 };
    vkCmdClearDepthStencilImage(cb, gPan.prof, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &fondo, 1, &rd);
    Barrera(cb, gPan.prof, ad, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    return true;
}
static void DestruirSwapchain() {
    for (size_t i = 0; i < gScSem.size(); i++) vkDestroySemaphore(gDev, gScSem[i], 0);
    gScSem.clear(); gScImgs.clear();
    if (gSc) vkDestroySwapchainKHR(gDev, gSc, 0);
    gSc = VK_NULL_HANDLE;
}
static bool CrearSwapchain() {
    VkSurfaceCapabilitiesKHR cap;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gFis, gSup, &cap) != VK_SUCCESS) return false;
    VkExtent2D ext = cap.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, h = 0; gVen.tamanio(gVen.user, &w, &h);
        ext.width = (uint32_t)w; ext.height = (uint32_t)h;
    }
    if (ext.width == 0 || ext.height == 0) { DestruirSwapchain(); gScTam = ext; return false; }   // (minimizada)
    if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { w3dLogfE("[vk] la ventana no acepta copias"); return false; }
    uint32_t n = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(gFis, gSup, &n, 0);
    std::vector<VkSurfaceFormatKHR> fmts(n ? n : 1); vkGetPhysicalDeviceSurfaceFormatsKHR(gFis, gSup, &n, &fmts[0]);
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < n; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { fmt = fmts[i]; break; }
    uint32_t nm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(gFis, gSup, &nm, 0);
    std::vector<VkPresentModeKHR> modos(nm ? nm : 1); vkGetPhysicalDeviceSurfacePresentModesKHR(gFis, gSup, &nm, &modos[0]);
    VkPresentModeKHR modo = VK_PRESENT_MODE_FIFO_KHR;
    if (!gVsync) {
        for (uint32_t i = 0; i < nm; i++) if (modos[i] == VK_PRESENT_MODE_MAILBOX_KHR) modo = modos[i];
        if (modo == VK_PRESENT_MODE_FIFO_KHR) for (uint32_t i = 0; i < nm; i++) if (modos[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) modo = modos[i];
    }
    VkSwapchainCreateInfoKHR si = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    si.surface = gSup;
    si.minImageCount = cap.minImageCount + 1;
    if (cap.maxImageCount && si.minImageCount > cap.maxImageCount) si.minImageCount = cap.maxImageCount;
    si.imageFormat = fmt.format; si.imageColorSpace = fmt.colorSpace; si.imageExtent = ext; si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = cap.currentTransform;
    si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(cap.supportedCompositeAlpha & si.compositeAlpha))
        for (uint32_t b = 1; b; b <<= 1) if (cap.supportedCompositeAlpha & b) { si.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
    si.presentMode = modo; si.clipped = VK_TRUE; si.oldSwapchain = gSc;
    VkSwapchainKHR nuevo = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(gDev, &si, 0, &nuevo);
    DestruirSwapchain();
    if (r != VK_SUCCESS) { w3dLogfE("[vk] vkCreateSwapchainKHR: %d", (int)r); return false; }
    gSc = nuevo; gScTam = ext; gScFmt = fmt.format;
    uint32_t ni = 0; vkGetSwapchainImagesKHR(gDev, gSc, &ni, 0);
    gScImgs.resize(ni); vkGetSwapchainImagesKHR(gDev, gSc, &ni, &gScImgs[0]);
    gScSem.resize(ni);
    for (uint32_t i = 0; i < ni; i++) { VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; vkCreateSemaphore(gDev, &s, 0, &gScSem[i]); }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(gFis, gScFmt, &fp);
    gScBlit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
    return true;
}
// el tamanio de la ventana cambio (o la swapchain quedo vieja): todo de nuevo
static void Rehacer() {
    EnviarYEsperar(); ReciclarTodo();
    vkDeviceWaitIdle(gDev);
    CrearSwapchain();
    int w = 0, h = 0; gVen.tamanio(gVen.user, &w, &h);
    if (w > 0 && h > 0 && (w != gPan.w || h != gPan.h)) {
        CrearPantalla(w, h);
        // (GL: la ventana nueva arranca con el viewport y el scissor que tenia; el motor los vuelve a poner)
    }
    gScRehacer = false;
}
static void NuevoCuadro() {
    gCur = (gCur + 1) % kCuadros;
    Cuadro& c = gCuadros[gCur];
    if (c.enVuelo) { vkWaitForFences(gDev, 1, &c.fence, VK_TRUE, UINT64_MAX); c.enVuelo = false; }
    Reiniciar(c);
}
void VkGLPresentar() {
    if (!gListo) return;
    int w = 0, h = 0; gVen.tamanio(gVen.user, &w, &h);
    if (w > 0 && h > 0 && (w != gPan.w || h != gPan.h)) gScRehacer = true;
    if (!gSc || gScRehacer) {
        // (lo dibujado en este cuadro era para el tamanio de antes: se descarta)
        Rehacer();
        if (!gSc) return;
        NuevoCuadro();
        return;
    }
    Cuadro& c = gCuadros[gCur];
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(gDev, gSc, UINT64_MAX, c.semAdq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)) {
        EnviarYEsperar(); ReciclarTodo();
        gScRehacer = true;
        return;
    }
    TerminarPase();
    VkCommandBuffer cb = Cmd();
    Barrera(cb, gPan.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Barrera(cb, gScImgs[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (gScBlit) {
        VkImageBlit b; memset(&b, 0, sizeof(b));
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = gPan.w; b.srcOffsets[1].y = gPan.h; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = (int32_t)gScTam.width; b.dstOffsets[1].y = (int32_t)gScTam.height; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, gPan.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gScImgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    } else {
        VkImageCopy cp; memset(&cp, 0, sizeof(cp));
        cp.srcSubresource.aspectMask = cp.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cp.srcSubresource.layerCount = cp.dstSubresource.layerCount = 1;
        cp.extent.width = (uint32_t)(gPan.w < (int)gScTam.width ? gPan.w : (int)gScTam.width);
        cp.extent.height = (uint32_t)(gPan.h < (int)gScTam.height ? gPan.h : (int)gScTam.height); cp.extent.depth = 1;
        vkCmdCopyImage(cb, gPan.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gScImgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    }
    Barrera(cb, gScImgs[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    Barrera(cb, gPan.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (!Enviar(c.semAdq, gScSem[idx])) { vkDeviceWaitIdle(gDev); ReciclarTodo(); return; }
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &gScSem[idx];
    pi.swapchainCount = 1; pi.pSwapchains = &gSc; pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(gCola, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) gScRehacer = true;
    NuevoCuadro();
}
void VkGLVsync(int intervalo) {
    const int v = intervalo ? 1 : 0;
    if (v == gVsync) return;
    gVsync = v;
    gScRehacer = true;
}
bool VkGLListo() { return gListo; }
const char* VkGLDispositivo() { return gNombreGPU.c_str(); }

// ============================================================================
//  ARRANCAR
// ============================================================================
static VKAPI_ATTR VkBool32 VKAPI_CALL AvisoValidacion(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) w3dLogfW("[vk validacion] %s", d->pMessage);
    return VK_FALSE;
}
static bool TieneExtension(const std::vector<VkExtensionProperties>& l, const char* n) {
    for (size_t i = 0; i < l.size(); i++) if (strcmp(l[i].extensionName, n) == 0) return true;
    return false;
}
static bool Falla(std::string* err, const char* msg) { if (err) *err = msg; w3dLogfW("[vk] %s", msg); VkGLTerminar(); return false; }
bool VkGLIniciar(const W3dVkVentana& v, std::string* err) {
    if (gListo) return true;
    gVen = v;
    vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)v.getInstanceProcAddr;
    if (!vkGetInstanceProcAddr_) return Falla(err, "sin el cargador de Vulkan");
    if (!AbrirShaderc()) return Falla(err, "sin shaderc_shared (el compilador de shaders de Vulkan): va al lado del ejecutable");
#define W3D_VK_CARGAR_G(n) n = (PFN_##n)vkGetInstanceProcAddr_(VK_NULL_HANDLE, #n); if (!n) return Falla(err, "cargador de Vulkan incompleto: " #n);
    W3D_VK_GLOBAL(W3D_VK_CARGAR_G)
#undef W3D_VK_CARGAR_G
    vkEnumerateInstanceVersion_ = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr_(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t verInst = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion_) vkEnumerateInstanceVersion_(&verInst);
    // ---- la instancia (con la validacion si se pide: W3D_VK_VALIDACION=1, del SDK) ----
    std::vector<const char*> exts(v.extensiones, v.extensiones + v.nExtensiones);
    std::vector<const char*> capas;
    const char* pideVal = getenv("W3D_VK_VALIDACION");
    if (pideVal && pideVal[0] == '1') {
        uint32_t n = 0; vkEnumerateInstanceLayerProperties(&n, 0);
        std::vector<VkLayerProperties> l(n ? n : 1); vkEnumerateInstanceLayerProperties(&n, &l[0]);
        for (uint32_t i = 0; i < n; i++) if (strcmp(l[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) capas.push_back("VK_LAYER_KHRONOS_validation");
        uint32_t ne = 0; vkEnumerateInstanceExtensionProperties(0, &ne, 0);
        std::vector<VkExtensionProperties> le(ne ? ne : 1); vkEnumerateInstanceExtensionProperties(0, &ne, &le[0]); le.resize(ne);
        if (TieneExtension(le, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        w3dLogf("[vk] validacion: %s", capas.empty() ? "no esta (instalar el SDK de Vulkan)" : "prendida");
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Whisk3D"; app.pEngineName = "Whisk3DCore";
    app.apiVersion = verInst >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size(); ici.ppEnabledExtensionNames = exts.empty() ? 0 : &exts[0];
    ici.enabledLayerCount = (uint32_t)capas.size(); ici.ppEnabledLayerNames = capas.empty() ? 0 : &capas[0];
    if (vkCreateInstance(&ici, 0, &gInst) != VK_SUCCESS) return Falla(err, "vkCreateInstance fallo (driver sin Vulkan)");
#define W3D_VK_CARGAR_I(n) n = (PFN_##n)vkGetInstanceProcAddr_(gInst, #n); if (!n) return Falla(err, "instancia de Vulkan incompleta: " #n);
    W3D_VK_INST(W3D_VK_CARGAR_I)
#undef W3D_VK_CARGAR_I
    if (!capas.empty()) {
        vkCreateDebugUtilsMessengerEXT_ = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr_(gInst, "vkCreateDebugUtilsMessengerEXT");
        vkDestroyDebugUtilsMessengerEXT_ = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr_(gInst, "vkDestroyDebugUtilsMessengerEXT");
        if (vkCreateDebugUtilsMessengerEXT_) {
            VkDebugUtilsMessengerCreateInfoEXT di = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            di.pfnUserCallback = AvisoValidacion;
            vkCreateDebugUtilsMessengerEXT_(gInst, &di, 0, &gDebug);
        }
    }
    if (!v.crearSuperficie(v.user, (void*)gInst, (void*)&gSup) || !gSup) return Falla(err, "no se pudo crear la superficie de la ventana");
    // ---- la GPU: la que dibuja y presenta en esta ventana, la dedicada antes que la integrada (W3D_VK_GPU=n elige) ----
    uint32_t nf = 0; vkEnumeratePhysicalDevices(gInst, &nf, 0);
    if (!nf) return Falla(err, "ninguna GPU con Vulkan");
    std::vector<VkPhysicalDevice> fis(nf); vkEnumeratePhysicalDevices(gInst, &nf, &fis[0]);
    const char* pideGpu = getenv("W3D_VK_GPU");
    int mejor = -1, puntos = -1; uint32_t famMejor = 0;
    for (uint32_t i = 0; i < nf; i++) {
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(fis[i], 0, &ne, 0);
        std::vector<VkExtensionProperties> le(ne ? ne : 1); vkEnumerateDeviceExtensionProperties(fis[i], 0, &ne, &le[0]); le.resize(ne);
        if (!TieneExtension(le, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(fis[i], &pr);
        if (pr.apiVersion < VK_API_VERSION_1_1 && !TieneExtension(le, VK_KHR_MAINTENANCE1_EXTENSION_NAME)) continue;   // (el viewport dado vuelta)
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(fis[i], &nq, 0);
        std::vector<VkQueueFamilyProperties> q(nq ? nq : 1); vkGetPhysicalDeviceQueueFamilyProperties(fis[i], &nq, &q[0]);
        for (uint32_t f = 0; f < nq; f++) {
            VkBool32 pres = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(fis[i], f, gSup, &pres);
            if (!(q[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !pres) continue;
            int p = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (pideGpu && atoi(pideGpu) == (int)i) p = 100;
            if (p > puntos) { puntos = p; mejor = (int)i; famMejor = f; }
            break;
        }
    }
    if (mejor < 0) return Falla(err, "ninguna GPU puede dibujar en esta ventana con Vulkan");
    gFis = fis[mejor]; gFamilia = famMejor;
    vkGetPhysicalDeviceProperties(gFis, &gProps);
    vkGetPhysicalDeviceMemoryProperties(gFis, &gMemProps);
    gNombreGPU = gProps.deviceName;
    VkPhysicalDeviceFeatures hay; vkGetPhysicalDeviceFeatures(gFis, &hay);
    memset(&gFeat, 0, sizeof(gFeat));
    gFeat.wideLines = hay.wideLines; gFeat.largePoints = hay.largePoints; gFeat.samplerAnisotropy = hay.samplerAnisotropy;
    // ---- el dispositivo ----
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = gFamilia; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    std::vector<const char*> dext; dext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (gProps.apiVersion < VK_API_VERSION_1_1 || app.apiVersion < VK_API_VERSION_1_1) dext.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (uint32_t)dext.size(); dci.ppEnabledExtensionNames = &dext[0];
    dci.pEnabledFeatures = &gFeat;
    if (vkCreateDevice(gFis, &dci, 0, &gDev) != VK_SUCCESS) return Falla(err, "vkCreateDevice fallo");
#define W3D_VK_CARGAR_D(n) n = (PFN_##n)vkGetDeviceProcAddr(gDev, #n); if (!n) return Falla(err, "dispositivo de Vulkan incompleto: " #n);
    W3D_VK_DEV(W3D_VK_CARGAR_D)
#undef W3D_VK_CARGAR_D
    vkGetDeviceQueue(gDev, gFamilia, 0, &gCola);
    gAlinUBO = gProps.limits.minUniformBufferOffsetAlignment > 16 ? gProps.limits.minUniformBufferOffsetAlignment : 16;
    // ---- shaderc ----
    gShc = shc_compiler_initialize();
    gShcOpc = shc_compile_options_initialize();
    if (!gShc || !gShcOpc) return Falla(err, "shaderc no inicio");
    shc_compile_options_set_target_env(gShcOpc, kShcVulkan, kShcVulkan10);
    shc_compile_options_set_optimization_level(gShcOpc, kShcOptimRendimiento);
    // ---- la profundidad/estencil que tenga esta GPU ----
    const VkFormat profs[3] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT };
    for (int i = 0; i < 3 && gFmtProf == VK_FORMAT_UNDEFINED; i++) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(gFis, profs[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) gFmtProf = profs[i];
    }
    if (gFmtProf == VK_FORMAT_UNDEFINED) return Falla(err, "sin formato de profundidad con estencil");
    // los formatos de atributo que usa el motor: si alguno no se lee, los buffers guardan su copia para convertirlo
    { const VkFormat f[5] = { VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R8G8B8A8_UNORM };
      for (int i = 0; i < 5; i++) if (!FormatoLeible(f[i])) gGuardarCopias = true; }
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
        if (vkCreateRenderPass(gDev, &ri, 0, &gRpDef) != VK_SUCCESS) return Falla(err, "vkCreateRenderPass");
        sp.pDepthStencilAttachment = 0; ri.attachmentCount = 1;
        if (vkCreateRenderPass(gDev, &ri, 0, &gRpColor) != VK_SUCCESS) return Falla(err, "vkCreateRenderPass");
    }
    // ---- el layout de todos los programas: binding 0 los uniforms, 1..8 las texturas ----
    {
        VkDescriptorSetLayoutBinding b[1 + kMuestras]; memset(b, 0, sizeof(b));
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        for (int i = 1; i <= kMuestras; i++) {
            b[i].binding = (uint32_t)i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1 + kMuestras; li.pBindings = b;
        if (vkCreateDescriptorSetLayout(gDev, &li, 0, &gSetLayout) != VK_SUCCESS) return Falla(err, "vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pi.setLayoutCount = 1; pi.pSetLayouts = &gSetLayout;
        if (vkCreatePipelineLayout(gDev, &pi, 0, &gLayout) != VK_SUCCESS) return Falla(err, "vkCreatePipelineLayout");
        VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        vkCreatePipelineCache(gDev, &ci, 0, &gPipeCache);
    }
    // ---- los cuadros ----
    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = gFamilia;
        if (vkCreateCommandPool(gDev, &pi, 0, &gPool) != VK_SUCCESS) return Falla(err, "vkCreateCommandPool");
        for (int i = 0; i < kCuadros; i++) {
            Cuadro& c = gCuadros[i];
            VkCommandBuffer cbs[2];
            VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = gPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 2;
            if (vkAllocateCommandBuffers(gDev, &ai, cbs) != VK_SUCCESS) return Falla(err, "vkAllocateCommandBuffers");
            c.cmd = cbs[0]; c.cmdPre = cbs[1];
            VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            vkCreateFence(gDev, &fi, 0, &c.fence);
            VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            vkCreateSemaphore(gDev, &si, 0, &c.semAdq);
            c.enVuelo = false; c.trozo = 0; c.pool = 0;
        }
    }
    gListo = true;
    // ---- la ventana y la pantalla ----
    int w = 0, h = 0; v.tamanio(v.user, &w, &h);
    CrearSwapchain();
    if (!CrearPantalla(w > 0 ? w : 1, h > 0 ? h : 1)) return Falla(err, "no se pudo crear la imagen de la pantalla");
    EstadoDeFabrica(w, h);
    memset(gAttr, 0, sizeof(gAttr));
    for (int i = 0; i < kAttrs; i++) gAttr[i].cte[3] = 1.0f;
    memset(gUnidad, 0, sizeof(gUnidad)); gUnidadActiva = 0;
    // la textura de una unidad vacia: negro opaco (lo que da GL con una textura incompleta)
    gNegra = NuevaTex(); gNegra->minF = GL_NEAREST; gNegra->magF = GL_NEAREST;
    { const unsigned char negro[4] = { 0, 0, 0, 255 };
      if (!CrearImagenTex(gNegra, 1, 1)) return Falla(err, "no se pudo crear la textura vacia");
      Subir(gNegra, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, negro); }
    w3dLogf("[vk] %s, Vulkan %u.%u.%u (driver %u), %dx%d%s", gNombreGPU.c_str(), VK_API_VERSION_MAJOR(gProps.apiVersion),
            VK_API_VERSION_MINOR(gProps.apiVersion), VK_API_VERSION_PATCH(gProps.apiVersion), gProps.driverVersion, w, h,
            gGuardarCopias ? " (convierte formatos de atributos)" : "");
    return true;
}
void VkGLTerminar() {
    if (gDev && vkDeviceWaitIdle && vkDestroyDevice) {   // (si no llego a cargar sus funciones, queda sin cerrar)
        vkDeviceWaitIdle(gDev);
        for (int i = 0; i < kCuadros; i++) {
            Cuadro& c = gCuadros[i];
            LiberarBasura(c);
            for (size_t k = 0; k < c.trozos.size(); k++) { vkDestroyBuffer(gDev, c.trozos[k].buf.b, 0); }
            c.trozos.clear();
            for (size_t k = 0; k < c.pools.size(); k++) vkDestroyDescriptorPool(gDev, c.pools[k], 0);
            c.pools.clear();
            if (c.fence) vkDestroyFence(gDev, c.fence, 0);
            if (c.semAdq) vkDestroySemaphore(gDev, c.semAdq, 0);
            c.fence = VK_NULL_HANDLE; c.semAdq = VK_NULL_HANDLE;
        }
        for (std::map<GLuint, ProgGL*>::iterator it = gProgs.begin(); it != gProgs.end(); ++it) {
            for (std::map<ClavePipe, VkPipeline>::iterator p = it->second->pipes.begin(); p != it->second->pipes.end(); ++p) vkDestroyPipeline(gDev, p->second, 0);
            if (it->second->mVS) vkDestroyShaderModule(gDev, it->second->mVS, 0);
            if (it->second->mFS) vkDestroyShaderModule(gDev, it->second->mFS, 0);
            delete it->second;
        }
        gProgs.clear(); gProg = 0;
        for (std::map<GLuint, TexGL*>::iterator it = gTex.begin(); it != gTex.end(); ++it) {
            TexGL* t = it->second;
            if (t->fb) vkDestroyFramebuffer(gDev, t->fb, 0);
            if (t->vista0) vkDestroyImageView(gDev, t->vista0, 0);
            if (t->vista) vkDestroyImageView(gDev, t->vista, 0);
            if (t->img) vkDestroyImage(gDev, t->img, 0);
            delete t;
        }
        gTex.clear();
        if (gNegra) {
            if (gNegra->vista) vkDestroyImageView(gDev, gNegra->vista, 0);
            if (gNegra->img) vkDestroyImage(gDev, gNegra->img, 0);
            delete gNegra; gNegra = 0;
        }
        for (std::map<GLuint, BufGL*>::iterator it = gBufs.begin(); it != gBufs.end(); ++it) {
            if (it->second->vk.b) vkDestroyBuffer(gDev, it->second->vk.b, 0);
            for (std::map<uint64_t, BufVk>::iterator c = it->second->conv.begin(); c != it->second->conv.end(); ++c) vkDestroyBuffer(gDev, c->second.b, 0);
            delete it->second;
        }
        gBufs.clear();
        for (std::map<uint32_t, VkSampler>::iterator it = gSamplers.begin(); it != gSamplers.end(); ++it) vkDestroySampler(gDev, it->second, 0);
        gSamplers.clear();
        DestruirPantalla();
        DestruirSwapchain();
        if (gPool) vkDestroyCommandPool(gDev, gPool, 0);
        if (gPipeCache) vkDestroyPipelineCache(gDev, gPipeCache, 0);
        if (gLayout) vkDestroyPipelineLayout(gDev, gLayout, 0);
        if (gSetLayout) vkDestroyDescriptorSetLayout(gDev, gSetLayout, 0);
        if (gRpDef) vkDestroyRenderPass(gDev, gRpDef, 0);
        if (gRpColor) vkDestroyRenderPass(gDev, gRpColor, 0);
        for (size_t i = 0; i < gBloques.size(); i++) { vkFreeMemory(gDev, gBloques[i]->mem, 0); delete gBloques[i]; }
        gBloques.clear();
        vkDestroyDevice(gDev, 0);
    }
    if (gInst) {
        if (gSup && vkDestroySurfaceKHR) vkDestroySurfaceKHR(gInst, gSup, 0);
        if (gDebug && vkDestroyDebugUtilsMessengerEXT_) vkDestroyDebugUtilsMessengerEXT_(gInst, gDebug, 0);
        if (vkDestroyInstance) vkDestroyInstance(gInst, 0);
    }
    if (gShcOpc) shc_compile_options_release(gShcOpc);
    if (gShc) shc_compiler_release(gShc);
    gShcOpc = 0; gShc = 0;
    gDev = VK_NULL_HANDLE; gInst = VK_NULL_HANDLE; gSup = VK_NULL_HANDLE; gDebug = VK_NULL_HANDLE; gFis = VK_NULL_HANDLE;
    gSc = VK_NULL_HANDLE; gPool = VK_NULL_HANDLE; gPipeCache = VK_NULL_HANDLE; gLayout = VK_NULL_HANDLE; gSetLayout = VK_NULL_HANDLE;
    gRpDef = gRpColor = VK_NULL_HANDLE; gFmtProf = VK_FORMAT_UNDEFINED;
    gGrabando = gPreGrabando = gEnPase = false;
    gListo = false;
}
