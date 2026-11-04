// ============================================================================
//  w3dGraphicsDespacho.cpp - VARIOS BACKENDS GRAFICOS EN UN EJECUTABLE (ver gfx/w3dBackend.h). Solo se compila con
//  W3D_MULTIBACKEND (la PC). Tiene:
//    - los GLOBALES compartidos del motor grafico (los contadores, las banderas del editor): antes los definia cada
//      backend; ahora el de cada uno vive en su namespace y estos son los que ven todos;
//    - w3dEngine::Foo(...) -> el backend activo (gfx/w3dDespacho.inc, generado por tools/generar_despacho.py);
//    - la ELECCION del backend (antes de crear la ventana).
// ============================================================================
#include "w3dGraphics.h"
#include "w3dBackend.h"
#include "w3dBackendTabla.h"
#include <string.h>
#include "w3dBase.h"
#ifdef W3D_VULKAN
#include "../vk/w3dVk.h"
#endif
// w3dSetColor (w3dBase.h): por la abstraccion, al backend activo
void w3dSetColor(const ColorType c[4]) { w3dEngine::Color4f(c[0], c[1], c[2], c[3]); }

// ---- el estado de render que setea el editor (el Core lo LEE) ----
bool w3dRenderWireframe = false;
bool w3dRenderWireframeSoloVisible = true;
bool w3dRenderSolido    = false;
bool w3dRenderSinLuz    = false;
bool w3dRenderLuces     = false;
bool w3dRenderNormalColor = false;
bool w3dRenderAlpha       = false;
bool w3dRenderOverlays    = true;
bool w3dVerSeleccion      = true;
bool g_xray               = false;

namespace w3dEngine {
// ---- los contadores y las banderas del motor (w3dGraphics.h) ----
int g_statLlamadas[StatCatCount_] = { 0 };
bool g_trazaGL = false;
bool g_trazaGLSec = false;
int g_statOtras = 0;
bool g_matrizAuditar = false;
bool g_invalidarPorMalla = false;
int  g_matrizDesyncs = 0;
bool g_matricesPerezosas = true;
int g_statDrawTris = 0, g_statDrawVBO = 0, g_statIndices = 0, g_statTexBinds = 0, g_statMeshes = 0;
int g_statStateChanges = 0, g_statUploadBytes = 0, g_statTrisOpacos = 0, g_statTrisBlend = 0;
int g_statDcCat[StatCatCount_] = { 0 };
StatsDeCuadro g_statCuadro;
bool g_medirDraws = false;
bool g_punterosCache = true;
bool g_auditarEscena = false;
int  g_auditDesyncs  = 0;

// ---- los backends que trae este ejecutable (cada uno llena su tabla: gfx/w3dBackendLlenar.inc) ----
namespace gl  { void W3dBackendLlenar_gl(W3dBackendTabla& t); }
namespace es2 { void W3dBackendLlenar_es2(W3dBackendTabla& t); void GLES2Init(void* (*getProc)(const char*)); void GLES2Resincronizar(); }
#ifdef W3D_VULKAN
// (Vulkan: el backend de ES 2.0 compilado otra vez sobre vk/w3dVkGL.cpp)
namespace vk  { void W3dBackendLlenar_vk(W3dBackendTabla& t); void GLES2Init(void* (*getProc)(const char*)); void GLES2Resincronizar(); }
#endif

static W3dBackendTabla gTablas[Backends_];
static bool gHay[Backends_] = { false, false, false };
static int gActivo = BackendOpenGL;
static W3dBackendTabla* gW3dBackend = 0;
static void Armar() {
    if (gW3dBackend) return;
    memset(gTablas, 0, sizeof gTablas);
    gl::W3dBackendLlenar_gl(gTablas[BackendOpenGL]);   gHay[BackendOpenGL] = true;
    es2::W3dBackendLlenar_es2(gTablas[BackendGLES2]);  gHay[BackendGLES2] = true;
#ifdef W3D_VULKAN
    vk::W3dBackendLlenar_vk(gTablas[BackendVulkan]);   gHay[BackendVulkan] = true;
#endif
    gW3dBackend = &gTablas[gActivo];
}
// (el primer uso arma las tablas: puede ser antes de main, por un constructor estatico)
struct ArmarAlArrancar { ArmarAlArrancar() { Armar(); } };
static ArmarAlArrancar gArmar;

const char* BackendNombre(int b) {
    switch (b) { case BackendOpenGL: return "OpenGL"; case BackendGLES2: return "OpenGL ES 2.0"; case BackendVulkan: return "Vulkan"; }
    return "?";
}
static bool IgualSinMayus(const char* a, const char* b) {
    for (; *a && *b; a++, b++) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return false; }
    return *a == *b;
}
int BackendDeNombre(const char* n) {
    if (!n || !*n) return -1;
    for (int b = 0; b < Backends_; b++) if (IgualSinMayus(n, BackendNombre(b))) return b;
    // (los nombres de antes en la configuracion, y los cortos de la linea de comandos)
    if (IgualSinMayus(n, "OpenGL ES") || IgualSinMayus(n, "opengles") || IgualSinMayus(n, "es2") || IgualSinMayus(n, "gles2")) return BackendGLES2;
    if (IgualSinMayus(n, "vk")) return BackendVulkan;
    return -1;
}
bool BackendDisponible(int b) {
    Armar();
    if (b < 0 || b >= Backends_ || !gHay[b]) return false;
#ifdef W3D_VULKAN
    // (Vulkan esta compilado siempre, pero hace falta el cargador del driver y el compilador de shaders: vk/w3dVk.h)
    if (b == BackendVulkan) { static int hay = -1; if (hay < 0) hay = VkGLCargable() ? 1 : 0; return hay == 1; }
#endif
    return true;
}
bool BackendUsar(int b) {
    Armar();
    if (!BackendDisponible(b)) return false;
    gActivo = b;
    gW3dBackend = &gTablas[b];
    return true;
}
int  BackendActivo() { return gActivo; }
bool BackendConShaders() { return gActivo != BackendOpenGL; }

// ---- lo que solo tiene el backend de shaders ----
void GLES2Init(void* (*getProc)(const char*)) {
    if (gActivo == BackendGLES2) es2::GLES2Init(getProc);
#ifdef W3D_VULKAN
    if (gActivo == BackendVulkan) vk::GLES2Init(getProc);
#endif
}
void GLES2Resincronizar() {
    if (gActivo == BackendGLES2) es2::GLES2Resincronizar();
#ifdef W3D_VULKAN
    if (gActivo == BackendVulkan) vk::GLES2Resincronizar();
#endif
}

// ---- cada funcion del motor, al backend activo ----
static inline W3dBackendTabla* TablaArmada() { Armar(); return gW3dBackend; }
#define W3D_TABLA() (gW3dBackend ? gW3dBackend : TablaArmada())
#include "w3dDespacho.inc"
}
