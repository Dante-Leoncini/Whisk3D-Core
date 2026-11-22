// ============================================================================
//  w3dGraphicsDispatch.cpp - VARIOS BACKENDS GRAFICOS EN UN EJECUTABLE (ver graphics/w3dBackend.h). Solo se compila con
//  W3D_MULTIBACKEND (la PC). Tiene:
//    - los GLOBALES compartidos del motor grafico (los contadores, las banderas del editor): antes los definia cada
//      backend; ahora el de cada uno vive en su namespace y estos son los que ven todos;
//    - w3dEngine::Foo(...) -> el backend activo (graphics/w3dDispatch.inc, generado por tools/generar_despacho.py);
//    - la ELECCION del backend (antes de crear la ventana).
// ============================================================================
#include "w3dGraphics.h"
#include "w3dBackend.h"
#include "w3dBackendTable.h"
#include <string.h>
#include "w3dBase.h"
#ifdef W3D_VULKAN
#include "graphics/vulkan/w3dVk.h"
#endif
// w3dSetColor (w3dBase.h): por la abstraccion, al backend activo
void w3dSetColor(const ColorType c[4]) { w3dEngine::Color4f(c[0], c[1], c[2], c[3]); }

// ---- el estado de render que setea el editor (el Core lo LEE) ----
bool w3dRenderWireframe = false;
bool w3dRenderWireframeOnlyVisible = true;
bool w3dRenderSolid      = false;
bool w3dRenderNoLight    = false;
bool w3dRenderLights     = true;   // (el render: las luces de la escena. El editor pone su modo de vista)
bool w3dRenderNormalColor = false;
bool w3dRenderAlpha       = false;
bool w3dRenderOverlays    = true;
bool w3dShowSelection     = true;
bool g_xray               = false;

namespace w3dEngine {
// ---- los contadores y las banderas del motor (w3dGraphics.h) ----
int g_statCalls[StatCatCount_] = { 0 };
bool g_traceGL = false;
bool g_traceGLSec  = false;
float g_alphaColor = 1.0f;
int g_otherStat = 0;
bool g_matrixAudit = false;
bool g_invalidatePerMesh = false;
int  g_matrixDesyncs = 0;
bool g_matricesLazy = true;
int g_statDrawTris = 0, g_statDrawVBO = 0, g_statIndices = 0, g_statTexBinds = 0, g_statMeshes = 0;
int g_statStateChanges = 0, g_statUploadBytes = 0, g_opaqueStatTris = 0, g_statTrisBlend = 0;
int g_statDcCat[StatCatCount_] = { 0 };
FrameStats g_statFrame;
bool g_measureDraws = false;
bool g_pointersCache = true;
bool g_auditScene    = false;
int  g_auditDesyncs  = 0;

// ---- los backends que trae este ejecutable (cada uno llena su tabla: graphics/w3dBackendFill.inc) ----
namespace gl  { void W3dBackendFill_gl(W3dBackendTable& t); }
namespace es2 { void W3dBackendFill_es2(W3dBackendTable& t); void GLES2Init(void* (*getProc)(const char*)); void GLES2Resync(); }
#ifdef W3D_VULKAN
// (Vulkan: el backend de ES 2.0 compilado otra vez sobre graphics/vulkan/w3dVkGL.cpp)
namespace vk  { void W3dBackendFill_vk(W3dBackendTable& t); void GLES2Init(void* (*getProc)(const char*)); void GLES2Resync(); }
#endif

static W3dBackendTable gTables[Backends_];
static bool gHas[Backends_] = { false, false, false };
static int gActive = BackendOpenGL;
static W3dBackendTable* gW3dBackend = 0;
static void Build() {
    if (gW3dBackend) return;
    memset(gTables, 0, sizeof gTables);
    gl::W3dBackendFill_gl(gTables[BackendOpenGL]);   gHas[BackendOpenGL] = true;
    es2::W3dBackendFill_es2(gTables[BackendGLES2]);  gHas[BackendGLES2] = true;
#ifdef W3D_VULKAN
    vk::W3dBackendFill_vk(gTables[BackendVulkan]);   gHas[BackendVulkan] = true;
#endif
    gW3dBackend = &gTables[gActive];
}
// (el primer uso arma las tablas: puede ser antes de main, por un constructor estatico)
struct BuildOnStart { BuildOnStart() { Build(); } };
static BuildOnStart gBuild;

const char* BackendName(int b) {
    switch (b) { case BackendOpenGL: return "OpenGL"; case BackendGLES2: return "OpenGL ES 2.0"; case BackendVulkan: return "Vulkan"; }
    return "?";
}
static bool EqualNoCaps(const char* a, const char* b) {
    for (; *a && *b; a++, b++) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return false; }
    return *a == *b;
}
int BackendFromName(const char* n) {
    if (!n || !*n) return -1;
    for (int b = 0; b < Backends_; b++) if (EqualNoCaps(n, BackendName(b))) return b;
    // (los nombres de antes en la configuracion, y los cortos de la linea de comandos)
    if (EqualNoCaps(n, "OpenGL ES") || EqualNoCaps(n, "opengles") || EqualNoCaps(n, "es2") || EqualNoCaps(n, "gles2")) return BackendGLES2;
    if (EqualNoCaps(n, "vk")) return BackendVulkan;
    return -1;
}
bool BackendAvailable(int b) {
    Build();
    if (b < 0 || b >= Backends_ || !gHas[b]) return false;
#ifdef W3D_VULKAN
    // (Vulkan esta compilado siempre, pero hace falta el cargador del driver y el compilador de shaders: graphics/vulkan/w3dVk.h)
    if (b == BackendVulkan) { static int has = -1; if (has < 0) has = VkGLCargable() ? 1 : 0; return has == 1; }
#endif
    return true;
}
bool BackendUse(int b) {
    Build();
    if (!BackendAvailable(b)) return false;
    gActive = b;
    gW3dBackend = &gTables[b];
    return true;
}
int  ActiveBackend() { return gActive; }
bool BackendWithShaders() { return gActive != BackendOpenGL; }

// ---- lo que solo tiene el backend de shaders ----
void GLES2Init(void* (*getProc)(const char*)) {
    if (gActive == BackendGLES2) es2::GLES2Init(getProc);
#ifdef W3D_VULKAN
    if (gActive == BackendVulkan) vk::GLES2Init(getProc);
#endif
}
void GLES2Resync() {
    if (gActive == BackendGLES2) es2::GLES2Resync();
#ifdef W3D_VULKAN
    if (gActive == BackendVulkan) vk::GLES2Resync();
#endif
}

// ---- cada funcion del motor, al backend activo ----
static inline W3dBackendTable* BuiltTable() { Build(); return gW3dBackend; }
#define W3D_TABLE() (gW3dBackend ? gW3dBackend : BuiltTable())
#include "w3dDispatch.inc"
}
