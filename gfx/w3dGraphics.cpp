// ============================================================================
//  Whisk3DCore (engine) — abstraccion del ESTADO de graficos
//  Backend: pipeline fijo de OpenGL (escritorio) / OpenGL ES 1.1 (Symbian,
//  Android viejo). Ambos comparten esta misma API de estado, asi que una sola
//  implementacion sirve para los dos; solo cambia el header de GL. Un backend
//  DirectX/Vulkan tendria su propio w3dGraphics*.cpp.
//  Ver w3dGraphics.h y ARQUITECTURA.md.
// ============================================================================

#include "w3dGraphics.h"

#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>
#else
    #ifdef _WIN32
        #define WIN32_LEAN_AND_MEAN
        #include <windows.h>
    #else
        // DESKTOP LINUX/Mac: los VBO (glGenBuffers/glBindBuffer/glBufferData/glDeleteBuffers) son GL 1.5, NO estan en
        // <GL/gl.h> (que suele ser GL 1.1). Mesa/libGL SI los exporta como simbolos reales -> con GL_GLEXT_PROTOTYPES +
        // <GL/glext.h> se declara el prototipo y linkea directo (sin cargar por glXGetProcAddress). En Windows NO se
        // hace esto (opengl32 solo exporta GL 1.1) -> alla se cargan los punteros por wglGetProcAddress (mas abajo).
        #define GL_GLEXT_PROTOTYPES
    #endif
    #include <GL/gl.h>
    #ifndef _WIN32
        #include <GL/glext.h>
    #endif
#endif
#include "w3dGLdc.h"   // Dreamcast: lo que GLdc no trae (vacio en el resto)
#include <math.h>  // tanf (Perspective)
#include <stdio.h> // printf (AuditarEstado: reporte de desyncs del cache)

#ifndef GL_CLAMP_TO_EDGE
    #define GL_CLAMP_TO_EDGE 0x812F // GL 1.2+: por si el GL/gl.h del sistema es 1.1
#endif
#ifndef GL_POINT_SPRITE
    #define GL_POINT_SPRITE 0x8861 // GL 1.4/OES: el GL/gl.h del sistema (1.1) no lo trae
#endif
#ifndef GL_COORD_REPLACE
    #define GL_COORD_REPLACE 0x8862 // idem (point sprite coord replace)
#endif
#ifndef GL_FUNC_ADD
    #define GL_FUNC_ADD 0x8006 // blend equation (GL 1.4+/GLES2); el header 1.1 no lo trae
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
    #define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif
#ifndef GL_MULTISAMPLE
    #define GL_MULTISAMPLE 0x809D // GL 1.3/ES1: apagar el MSAA durante el color-ID pick
#endif

// ---- BUFFER OBJECTS (VBO/IBO): enums GL 1.5/ES1.1 (el GL/gl.h de escritorio 1.1 no los trae) ----
#ifndef GL_ARRAY_BUFFER
    #define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
    #define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STATIC_DRAW
    #define GL_STATIC_DRAW 0x88E4
#endif
#include <stddef.h> // ptrdiff_t / size_t (offset de VBO)
#include <map>        // la memoria de los VBO vivos (VBOMemoria)

// En Symbian/GLES 1.1 los buffer objects son CORE (<GLES/gl.h> los declara). En desktop GL 1.1 (Windows, opengl32)
// son GL 1.5 -> hay que cargar los punteros por wglGetProcAddress. Se abstrae con macros GLB_* + un flag gVBOok.
#if defined(_WIN32) && !defined(W3D_SYMBIAN)
    typedef void (APIENTRY *W3D_PFNGENBUF)(GLsizei, GLuint*);
    typedef void (APIENTRY *W3D_PFNDELBUF)(GLsizei, const GLuint*);
    typedef void (APIENTRY *W3D_PFNBINDBUF)(GLenum, GLuint);
    typedef void (APIENTRY *W3D_PFNBUFDATA)(GLenum, ptrdiff_t, const void*, GLenum);
    typedef void (APIENTRY *W3D_PFNBLENDEQ)(GLenum); // glBlendEquation (GL 1.4): tambien por wglGetProcAddress
    static W3D_PFNGENBUF  w3d_glGenBuffers    = 0;
    static W3D_PFNDELBUF  w3d_glDeleteBuffers = 0;
    static W3D_PFNBINDBUF w3d_glBindBuffer    = 0;
    static W3D_PFNBUFDATA w3d_glBufferData    = 0;
    static bool w3d_vboLoaded = false, w3d_vboOk = false;
    // multitextura (GL 1.3: glActiveTexture / glClientActiveTexture): opengl32 solo exporta GL 1.1 -> por punteros
    typedef void (APIENTRY *W3D_PFNACTIVETEX)(GLenum);
    static W3D_PFNACTIVETEX w3d_glActiveTexture = 0, w3d_glClientActiveTexture = 0;
    static void w3d_CargarVBO(){
        if (w3d_vboLoaded) return; w3d_vboLoaded = true;
        w3d_glGenBuffers    = (W3D_PFNGENBUF) wglGetProcAddress("glGenBuffers");
        w3d_glDeleteBuffers = (W3D_PFNDELBUF) wglGetProcAddress("glDeleteBuffers");
        w3d_glBindBuffer    = (W3D_PFNBINDBUF)wglGetProcAddress("glBindBuffer");
        w3d_glBufferData    = (W3D_PFNBUFDATA)wglGetProcAddress("glBufferData");
        w3d_vboOk = w3d_glGenBuffers && w3d_glDeleteBuffers && w3d_glBindBuffer && w3d_glBufferData;
        w3d_glActiveTexture       = (W3D_PFNACTIVETEX)wglGetProcAddress("glActiveTexture");
        w3d_glClientActiveTexture = (W3D_PFNACTIVETEX)wglGetProcAddress("glClientActiveTexture");
    }
    #define GLB_GEN    w3d_glGenBuffers
    #define GLB_DEL    w3d_glDeleteBuffers
    #define GLB_BIND   w3d_glBindBuffer
    #define GLB_DATA   w3d_glBufferData
    #define GLMT_OK     (w3d_glActiveTexture && w3d_glClientActiveTexture)
    #define GLMT_ACTIVE w3d_glActiveTexture
    #define GLMT_CLIENT w3d_glClientActiveTexture
#elif defined(W3D_DREAMCAST)
    // GLdc: sin buffer objects (todo por arreglos en RAM) y la multitextura con los nombres ARB
    static bool w3d_vboOk = false;
    static void w3d_CargarVBO(){}
    #define GLMT_OK     true
    #define GLMT_ACTIVE glActiveTextureARB
    #define GLMT_CLIENT glClientActiveTextureARB
    #define GLB_GEN    glGenBuffers
    #define GLB_DEL    glDeleteBuffers
    #define GLB_BIND   glBindBuffer
    #define GLB_DATA   glBufferData
#else
    static bool w3d_vboOk = true;         // GLES 1.1 / ES2: core
    static void w3d_CargarVBO(){}
    // multitextura: core en GL ES 1.1 (N95) y en GL >= 1.3 (Linux: los prototipos vienen con GL_GLEXT_PROTOTYPES)
    #define GLMT_OK     true
    #define GLMT_ACTIVE glActiveTexture
    #define GLMT_CLIENT glClientActiveTexture
    #define GLB_GEN    glGenBuffers
    #define GLB_DEL    glDeleteBuffers
    #define GLB_BIND   glBindBuffer
    #define GLB_DATA   glBufferData
#endif

namespace w3dEngine {

// ============================================================================
//  EL CONTADOR DE LLAMADAS: cada llamada REAL al driver (cada gl*) pasa por W3D_GLC y suma 1 a la categoria del que
//  esta dibujando (escena, particulas, UI del juego, editor). Las ahorradas por el cache no llegan aca. StatsCuadro()
//  cierra el cuadro (ver w3dGraphics.h)
// ============================================================================
int g_statLlamadas[StatCatCount_] = { 0 };
static int gStatCat = StatCatEditor;   // categoria del que esta dibujando (el cuadro arranca en Editor)
#define W3D_GLC(llamada) (g_statLlamadas[gStatCat]++, g_trazaGL ? TrazaGL(#llamada) : (void)0, (llamada))
bool g_trazaGL = false;
bool g_trazaGLSec = false;           // ademas, cada llamada EN ORDEN (con su categoria): de donde sale cada una
static const char* gTrazaTxt[512];   // (un lugar del codigo = un texto: se compara el puntero)
static int gTrazaN[512], gTrazaCant = 0;
void TrazaGL(const char* l) {
    if (g_trazaGLSec) printf("      [glsec] %d %s\n", gStatCat, l);
    for (int i = 0; i < gTrazaCant; i++) if (gTrazaTxt[i] == l) { gTrazaN[i]++; return; }
    if (gTrazaCant < 512) { gTrazaTxt[gTrazaCant] = l; gTrazaN[gTrazaCant++] = 1; }
}
void TrazaGLVer() {
    for (int k = 0; k < gTrazaCant; k++) {
        int m = k;
        for (int i = k + 1; i < gTrazaCant; i++) if (gTrazaN[i] > gTrazaN[m]) m = i;
        const char* t = gTrazaTxt[k]; gTrazaTxt[k] = gTrazaTxt[m]; gTrazaTxt[m] = t;
        int n = gTrazaN[k]; gTrazaN[k] = gTrazaN[m]; gTrazaN[m] = n;
        printf("      [gltraza] %6d  %s\n", gTrazaN[k], gTrazaTxt[k]);
    }
    gTrazaCant = 0;
}

// ============================================================================
//  CACHE de estado: evita llamadas GL redundantes (cambiar a lo que ya esta
//  puesto no hace nada). Valido solo si TODO el estado pasa por el motor; tras
//  GL directo, Invalidate() resincroniza.
// ============================================================================
static const int kNumCaps = CapCount_; // el enum manda: agregar una Cap ya no puede desbordar
static bool gCapOn[kNumCaps] = { false };    // estado que el motor cree puesto
static bool gCapKnown[kNumCaps] = { false }; // false = no lo conocemos -> forzar la llamada
static unsigned int gTexBound = 0;           // estado inicial de GL: sin textura

// ---- cache FINO de estado (camino de render eficiente, P1) ----
// AplicarMaterial y el epilogo de cada malla RE-AFIRMAN depth func/mask, blend,
// texenv, texgen/matcap, shade model, alpha-test y los client arrays en CADA
// malla (la leccion del "estado que leakea" esta pagada: afirmar es correcto).
// Lo que era caro no era afirmar sino LLAMAR AL DRIVER: con estos caches,
// re-poner lo que ya esta puesto no llama (ni suma al contador de cambios).
// Invalidate() los vuelve "desconocido", igual que las caps.
// EXCEPCION: TexMatrixMatcap(true) SIEMPRE ejecuta (su matriz sale del modelview
// ACTUAL, cambia por malla); solo el off->off se ahorra.
static int   gDepthFuncCache = -1;   // DepthCmp (-1 = desconocido)
static int   gDepthMaskCache = -1;   // 0/1
static int   gBlendFuncCache = -1;   // firma del glBlendFunc puesto (interna, ver FirmaMezcla)
static int   gBlendEqCache   = -1;   // 0 = FUNC_ADD, 1 = REVERSE_SUBTRACT
static int   gTexEnvCache    = -1;   // 0=modulate 1=replace 2=dot3 3=alphaonly
static int   gTexGenCache    = -1;   // 0/1 (sphere-map por texgen + su matriz de textura)
static int   gMatcapCache    = -1;   // 0 identidad / 1 matcap / 2 desplazamiento de UV (la matriz de textura)
static float gUVMat[4]       = { 0, 0, 1, 1 };   // el desplazamiento puesto (gMatcapCache 2)
static int   gSmoothCache    = -1;   // 0/1 (shade model)
static float gAlphaRefCache  = -1e9f;
static signed char gArrOn[4]    = { -1, -1, -1, -1 }; // client arrays: -1 desconocido / 0 / 1
// MEZCLA DE TEXTURAS (la unidad 1, ver MezclaTexturas mas abajo): su estado cacheado y el ULTIMO puntero de UV de la
// unidad 0, para replicarlo en la 1 (gUV0Tipo: 0 ninguno / 1 RAM de 2 floats / 2 VBO)
static int      gMezclaTexOn = -1;              // -1 desconocido / 0 apagada / 1 prendida
static unsigned gMezclaTexId = 0xFFFFFFFFu;     // la textura bindeada en la unidad 1
static float    gMezclaTexK  = -1.0f;           // el k puesto en el color constante del combiner
static int      gMezclaTexFiltro = -1, gMezclaTexRepetir = -1;
static int      gMezclaTexModo = -1;            // 0 interpolate / 1 multiply / 2 multiply 2x (el env puesto)
// la UV PROPIA de la unidad 1 (0 = sigue a la unidad 0 / 1 RAM / 2 VBO)
static int gUV1Tipo = 0; static const float* gUV1Ptr = 0; static unsigned gUV1Vbo = 0;
// lo que la unidad 1 YA tiene puesto como puntero de UV (el puntero de cliente persiste aunque la unidad se apague y
// se prenda): -1 = desconocido
static int gUV1PuestoTipo = -1; static const float* gUV1PuestoPtr = 0; static int gUV1PuestoStride = 0; static unsigned gUV1PuestoVbo = 0;
static int      gMezclaSoportada = -1;          // -1 = sin preguntarle al driver
static bool     gMezclaAlgunaVez = false;       // se prendio alguna vez en el proceso (si no, la unidad 1 esta como GL la deja: apagada)
static int gUV0Tipo = 0; static const float* gUV0Ptr = 0; static int gUV0Stride = 0; static unsigned gUV0Vbo = 0;
static void MezclaUnidad1Array(bool on);        // el array de UV de la unidad 1 (sigue al de la 0)

// ---- cache de MATERIAL y COLOR actual (por VALOR) ----
// AplicarMaterial re-manda los 4 glMaterialfv + shininess + glColor4f por CADA
// malla: con N mallas del mismo material eran ~6N llamadas identicas por frame
// (el reporte del dueno: Claude, 16 mallas, UN material). El valor ES la clave:
// mandar lo que ya esta puesto no llama al driver.
// OJO GL: con COLOR_MATERIAL prendido, ambient/diffuse del material TRACKEAN el
// color actual -> ahi el cache de esos dos canales se invalida (Enable/Disable
// de la cap, Color4f/4ub, y todo draw con ColorArray, que ademas deja el color
// actual INDEFINIDO segun la spec).
static float gMatCache[4][4];
static bool  gMatKnown[4] = { false, false, false, false };
static float gShinCache   = -1e9f;
static float gColCache[4];
static bool  gColKnown    = false;
#ifdef W3D_DREAMCAST
// la lectura de pixeles por software (ver ReadPixelsBegin, mas abajo) y el estado de GL que necesita y no se guardaba
static bool  gLectActiva  = false;
static float gClearCol[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static bool  gColorMaskOn = true;
static float gLineaAncho  = 1.0f;
static bool  gFrenteCCW   = true, gCullFrente = false;
enum LectPrim { LectTriangulos, LectLineas, LectTiraLineas, LectPuntos };
static void LectDibujar(LectPrim p, int primero, int n, const void* idx, int bytesIdx);
static void LectLimpiar(int bits);
// mientras la lectura esta prendida, un draw se rasteriza en ella y no va a la PVR
#define W3D_LECT(prim, primero, n, idx, bytesIdx) if (gLectActiva) { LectDibujar(prim, primero, n, idx, bytesIdx); return; }
#else
#define W3D_LECT(prim, primero, n, idx, bytesIdx)
#endif

// ---- capacidades ----
static GLenum CapGL(Cap c) {
    switch (c) {
        case DepthTest:     return GL_DEPTH_TEST;
        case CullFace:      return GL_CULL_FACE;
        case Texture2D:     return GL_TEXTURE_2D;
        case Blend:         return GL_BLEND;
        case Lighting:      return GL_LIGHTING;
        case Normalize:     return GL_NORMALIZE;
        case Fog:           return GL_FOG;
        case ColorMaterial: return GL_COLOR_MATERIAL;
        case ScissorTest:   return GL_SCISSOR_TEST;
        case PolygonOffsetFill: return GL_POLYGON_OFFSET_FILL;
        case PointSprite:       return GL_POINT_SPRITE;
        case Dither:            return GL_DITHER;
        case Light0:            return GL_LIGHT0;
        case Multisample:       return GL_MULTISAMPLE;
    }
    return 0;
}

// contador de cambios de estado REALES (presupuesto medible, ver w3dGraphics.h).
// Se declara aca arriba porque lo tocan Enable/Disable; se define junto al resto
// de las estadisticas mas abajo.
extern int g_statStateChanges;

void DepthFunc(DepthCmp c) {
    if (gDepthFuncCache == (int)c) return;   // ya puesta -> ni driver ni contador
    gDepthFuncCache = (int)c;
    GLenum f = GL_LESS;
    if      (c == DepthEqual)  { f = GL_EQUAL; }
    else if (c == DepthLEqual) { f = GL_LEQUAL; }
    else if (c == DepthAlways) { f = GL_ALWAYS; }
    g_statStateChanges++;
    W3D_GLC(glDepthFunc(f));
}

void Enable(Cap c) {
    if (gCapKnown[c] && gCapOn[c]) { return; } // ya prendida y la conocemos -> nada
    gCapOn[c] = true; gCapKnown[c] = true;
#ifdef W3D_DREAMCAST
    if (c == PointSprite || c == Dither || c == Multisample) return;   // (GLdc no las conoce: solo daban GL_INVALID_ENUM)
#endif
    g_statStateChanges++;
    W3D_GLC(glEnable(CapGL(c)));
    // COLOR_MATERIAL: al prenderse, ambient/diffuse del material pasan a seguir
    // el color actual -> lo que el cache creia puesto ya no vale
    if (c == ColorMaterial) { gMatKnown[MatAmbient] = false; gMatKnown[MatDiffuse] = false; }
}

void Disable(Cap c) {
    if (gCapKnown[c] && !gCapOn[c]) { return; } // ya apagada y la conocemos -> nada
    gCapOn[c] = false; gCapKnown[c] = true;
#ifdef W3D_DREAMCAST
    if (c == PointSprite || c == Dither || c == Multisample) return;
#endif
    g_statStateChanges++;
    W3D_GLC(glDisable(CapGL(c)));
    // COLOR_MATERIAL: al apagarse, el material queda con lo ULTIMO trackeado
    // (el color que hubiera), no con lo que mando glMaterial -> desconocido
    if (c == ColorMaterial) { gMatKnown[MatAmbient] = false; gMatKnown[MatDiffuse] = false; }
}

// ---- arrays de vertices ----
static GLenum ArrayGL(VArray a) {
    switch (a) {
        case VertexArray:   return GL_VERTEX_ARRAY;
        case TexCoordArray: return GL_TEXTURE_COORD_ARRAY;
        case NormalArray:   return GL_NORMAL_ARRAY;
        case ColorArray:    return GL_COLOR_ARRAY;
    }
    return 0;
}
void EnableArray(VArray a)  {
    if (gArrOn[a] == 1) return;
    gArrOn[a] = 1;
    g_statStateChanges++; W3D_GLC(glEnableClientState(ArrayGL(a)));
    if (a == TexCoordArray && gMezclaTexOn == 1) MezclaUnidad1Array(true);   // (la unidad 1 sigue a la 0)
}
void DisableArray(VArray a) {
    if (gArrOn[a] == 0) return;
    gArrOn[a] = 0;
    g_statStateChanges++; W3D_GLC(glDisableClientState(ArrayGL(a)));
    if (a == TexCoordArray && gMezclaTexOn == 1) MezclaUnidad1Array(false);
}
int ArrayEstado(VArray a) { return gArrOn[a]; }

// ---- buffers ----
#ifdef W3D_DREAMCAST
void ClearColor(float r, float g, float b, float a) { gClearCol[0] = r; gClearCol[1] = g; gClearCol[2] = b; gClearCol[3] = a; W3D_GLC(glClearColor(r, g, b, a)); }
#else
void ClearColor(float r, float g, float b, float a) { W3D_GLC(glClearColor(r, g, b, a)); }
#endif
void Clear(int bits) {
#ifdef W3D_DREAMCAST
    if (gLectActiva) { LectLimpiar(bits); return; }
#endif
    GLbitfield m = 0;
    if (bits & ColorBuffer)   { m |= GL_COLOR_BUFFER_BIT; }
    if (bits & DepthBuffer)   { m |= GL_DEPTH_BUFFER_BIT; }
    if (bits & StencilBuffer) { m |= GL_STENCIL_BUFFER_BIT; }
    W3D_GLC(glClear(m));
}

// ---- planos de recorte (user clip planes) ----
// GL de escritorio: glClipPlane toma DOUBLE. GLES 1.1: glClipPlanef (float).
// El tope real lo da el driver (GL_MAX_CLIP_PLANES; el minimo del estandar es 6
// en GL de escritorio y 1 en GLES 1.1), se consulta UNA vez y se cachea.
static int gMaxClip = -1;
static int gClipOn  = 0;   // cuantos hay prendidos ahora (para apagarlos rapido)

int PlanosRecorteMax() {
#ifdef W3D_DREAMCAST
    return 0;   // la PVR no tiene planos de recorte: el espejo se recorta solo por la vista
#endif
    if (gMaxClip < 0) {
        GLint n = 0;
        W3D_GLC(glGetIntegerv(GL_MAX_CLIP_PLANES, &n));
        if (n < 0) n = 0;
        if (n > 6) n = 6;   // el espejo necesita 5; mas no se usan
        gMaxClip = (int)n;
    }
    return gMaxClip;
}

void PlanosRecorte(int n, const float* eq4) {
    const int maxN = PlanosRecorteMax();
    if (n > maxN) n = maxN;
    if (n < 0) n = 0;
    if (n > 0) MatricesSincronizar();   // (la ecuacion se lleva con la modelview vigente: la perezosa tiene que estar en GL)
    for (int i = 0; i < n; i++) {
#ifdef W3D_SYMBIAN
        GLfloat p[4] = { eq4[i*4+0], eq4[i*4+1], eq4[i*4+2], eq4[i*4+3] };
        W3D_GLC(glClipPlanef((GLenum)(GL_CLIP_PLANE0 + i), p));
#else
        GLdouble p[4] = { eq4[i*4+0], eq4[i*4+1], eq4[i*4+2], eq4[i*4+3] };
        W3D_GLC(glClipPlane((GLenum)(GL_CLIP_PLANE0 + i), p));
#endif
        W3D_GLC(glEnable((GLenum)(GL_CLIP_PLANE0 + i)));
    }
    for (int i = n; i < gClipOn; i++) W3D_GLC(glDisable((GLenum)(GL_CLIP_PLANE0 + i)));
    gClipOn = n;
}

// ---- estencil ----
static int gStencilBits = -1;

int EstencilBits() {
    if (gStencilBits < 0) {
        GLint b = 0;
        W3D_GLC(glGetIntegerv(GL_STENCIL_BITS, &b));
        gStencilBits = (b > 0) ? (int)b : 0;
    }
    return gStencilBits;
}

void EstencilLimpiar(int valor) {
    W3D_GLC(glClearStencil(valor));
    W3D_GLC(glStencilMask(0xFFu));      // sin mascara de escritura el glClear no borra nada
    W3D_GLC(glClear(GL_STENCIL_BUFFER_BIT));
}

void EstencilMarcar(int valor) {
    W3D_GLC(glEnable(GL_STENCIL_TEST));
    W3D_GLC(glStencilMask(0xFFu));
    W3D_GLC(glStencilFunc(GL_ALWAYS, valor, 0xFFu));
    W3D_GLC(glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE));   // escribe donde PASA el z-test
}

void EstencilDentro(int valor) {
    W3D_GLC(glEnable(GL_STENCIL_TEST));
    W3D_GLC(glStencilMask(0x00u));                        // no tocar el buffer
    W3D_GLC(glStencilFunc(GL_EQUAL, valor, 0xFFu));
    W3D_GLC(glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP));
}

void EstencilApagar() {
    W3D_GLC(glStencilMask(0xFFu));
    W3D_GLC(glDisable(GL_STENCIL_TEST));
}

// ---- viewport / recorte ----
#ifdef W3D_DREAMCAST
// GLdc no deja consultar el viewport: se recuerda el ultimo que se puso
static int gViewportPuesto[4] = { 0, 0, 640, 480 };
void Viewport(int x, int y, int w, int h) { gViewportPuesto[0] = x; gViewportPuesto[1] = y; gViewportPuesto[2] = w; gViewportPuesto[3] = h; W3D_GLC(glViewport(x, y, w, h)); }
void GetViewport(int v[4]) { for (int i = 0; i < 4; i++) v[i] = gViewportPuesto[i]; }
#else
void Viewport(int x, int y, int w, int h) { W3D_GLC(glViewport(x, y, w, h)); }
void GetViewport(int v[4]) { GLint g[4]; W3D_GLC(glGetIntegerv(GL_VIEWPORT, g)); for (int i = 0; i < 4; i++) v[i] = (int)g[i]; }
#endif
void GetScissor(int v[4])  { GLint g[4]; W3D_GLC(glGetIntegerv(GL_SCISSOR_BOX, g)); for (int i = 0; i < 4; i++) v[i] = (int)g[i]; }
bool ScissorActivo()       { return W3D_GLC(glIsEnabled(GL_SCISSOR_TEST)) == GL_TRUE; }
void Scissor(int x, int y, int w, int h)  { W3D_GLC(glScissor(x, y, w, h)); }

// ---- matrices ----
static GLenum MatGL(Matrix m) {
    switch (m) {
        case Projection:    return GL_PROJECTION;
        case ModelView:     return GL_MODELVIEW;
        case TextureMatrix: return GL_TEXTURE;
    }
    return GL_MODELVIEW;
}

int g_statOtras = 0;
// el MODO de matriz puesto (cache: -1 desconocido) y la PROYECCION del pase sin sesgo (la base de SesgoMetros): lo que
// toca la proyeccion por otro lado (el viewport la arma, el HUD pone su ortho) la vuelve desconocida y sin sesgo
static int   gMatMode = -1;
static float gProyBase[16];
static bool  gProyBaseOk = false;
static float gSesgoPuesto = 0.0f;
static void ProyCambio() { if (gMatMode != (int)ModelView) { gProyBaseOk = false; gSesgoPuesto = 0.0f; } }

// ---- la SOMBRA de las matrices: la modelview y la proyeccion llevadas en la CPU ----
// GetMatrix las lee de aca y no de GL: en el N95 (tiler MBX) un glGet espera al GPU, y se leian ~20 por cuadro (el
// autocull de cada malla, la base de cada rutina). Cada operacion de matriz del motor la lleva al dia (GL es column-
// major: M = M * X). Lo que no sabe (una pila mas honda que la sombra) queda marcado y esa lectura va a GL una vez.
// Sin modo conocido (despues de Invalidate) es la modelview: lo mismo que supone el recorrido de la escena
enum { kSombraHondo = 32 };
static float gSombra[2][kSombraHondo][16];        // [0] modelview, [1] proyeccion (la de textura no se lee nunca)
static bool  gSombraOk[2][kSombraHondo];
static int   gSombraNivel[2] = { 0, 0 };           // (puede pasar de kSombraHondo: esos niveles no se saben)
bool g_matrizAuditar = false;                      // (harness 'matrizaudit') compara cada lectura con GL
#ifdef W3D_SYMBIAN
bool g_invalidarPorMalla = true;                   // (la plataforma lo apaga en el modo juego puro)
#else
bool g_invalidarPorMalla = false;
#endif
int  g_matrizDesyncs = 0;
// ---- la PILA PEREZOSA de la modelview (ver w3dGraphics.h): cada nivel de la sombra lleva la SERIE de su matriz (un
// numero nuevo en cada cambio) y se recuerda la serie de la que GL tiene cargada: distintas = "sucia", y lo proximo que
// use la matriz manda UN glLoadMatrixf. Un nivel abierto sin glPushMatrix (perezoso) se cierra sin glPopMatrix: todos
// esos niveles comparten el unico de GL. Se apila de verdad solo sin la matriz en la sombra (mas honda que la sombra)
bool g_matricesPerezosas = true;
static unsigned gSerie[kSombraHondo];           // la serie de la matriz de cada nivel de la modelview
static bool     gNivelReal[kSombraHondo];       // ese nivel se abrio con un glPushMatrix de verdad (se cierra con glPopMatrix)
static unsigned gSerieProxima = 1;
static unsigned gSerieEnGL = 0;                 // la que tiene GL (0 = no se sabe: lo proximo que la use la manda)
static unsigned SerieNueva() { if (++gSerieProxima == 0) gSerieProxima = 1; return gSerieProxima; }
static int SombraPila() { return gMatMode == (int)Projection ? 1 : (gMatMode == (int)TextureMatrix ? -1 : 0); }
static float* SombraTope(int s) {
    const int n = gSombraNivel[s];
    return (n < kSombraHondo && gSombraOk[s][n]) ? gSombra[s][n] : 0;
}
static void SombraCargar(const float* m) {
    const int s = SombraPila(); if (s < 0) return;
    const int n = gSombraNivel[s]; if (n >= kSombraHondo) return;
    for (int i = 0; i < 16; i++) gSombra[s][n][i] = m[i];
    gSombraOk[s][n] = true;
    if (s == 0) gSerie[n] = SerieNueva();
}
// (el MISMO orden de operaciones que el glMultMatrixf de Mesa -m_matrix.c, matmul4-: la sombra da los mismos bits que
// GL y la imagen con la pila perezosa es identica a la inmediata; lo prueba tools/pruebas/prueba_menos_llamadas.w3s)
static void SombraMult(const float* b) {
    const int s = SombraPila(); if (s < 0) return;
    float* a = SombraTope(s); if (!a) return;
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int f = 0; f < 4; f++)
            r[c * 4 + f] = a[f] * b[c * 4] + a[4 + f] * b[c * 4 + 1] + a[8 + f] * b[c * 4 + 2] + a[12 + f] * b[c * 4 + 3];
    for (int i = 0; i < 16; i++) a[i] = r[i];
    if (s == 0) gSerie[gSombraNivel[0]] = SerieNueva();
}
static void SombraPush(bool real) {
    const int s = SombraPila(); if (s < 0) return;
    const int n = gSombraNivel[s]++;
    if (n + 1 >= kSombraHondo) return;
    gSombraOk[s][n + 1] = n < kSombraHondo && gSombraOk[s][n];
    if (gSombraOk[s][n + 1]) for (int i = 0; i < 16; i++) gSombra[s][n + 1][i] = gSombra[s][n][i];
    if (s == 0) { gSerie[n + 1] = gSerie[n]; gNivelReal[n + 1] = real; }
}
static void SombraPop() {
    const int s = SombraPila(); if (s < 0) return;
    if (gSombraNivel[s] > 0) gSombraNivel[s]--;
    else gSombraOk[s][0] = false;          // (un pop de mas: lo que quedo en GL no se sabe)
}
static const float kIdentidad[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

void MatrixMode(Matrix m) { if (gMatMode == (int)m) return; gMatMode = (int)m; g_statOtras++; W3D_GLC(glMatrixMode(MatGL(m))); }

// la modelview: la sucia es la que la sombra sabe y GL no tiene
static bool MVSucia() { const int n = gSombraNivel[0]; return n < kSombraHondo && gSombraOk[0][n] && gSerie[n] != gSerieEnGL; }
// la manda a GL si quedo algo perezoso. Lo llaman los Draw*, las luces, los planos de recorte y el GL crudo de afuera.
// Con otra matriz activa (casi nunca: los draws van con la modelview) pasa a la modelview y vuelve: por eso cambiar de
// matriz no obliga a mandarla (el HUD que arma su orto no paga la vista de la escena que ya no usa)
void MatricesSincronizar() {
    if (!MVSucia()) return;
    const int n = gSombraNivel[0], modo = gMatMode;
    if (modo != (int)ModelView) MatrixMode(ModelView);   // (desconocido -tras Invalidate- tambien: queda conocido)
    g_statOtras++; W3D_GLC(glLoadMatrixf(gSombra[0][n]));
    gSerieEnGL = gSerie[n];
    if (modo == (int)Projection || modo == (int)TextureMatrix) MatrixMode((Matrix)modo);
}
// la operacion sobre la modelview queda en la sombra: opcion prendida, la modelview activa (o desconocida, que el
// recorrido supone modelview) y la sombra sabe la matriz. Una CARGA no necesita la anterior: alcanza con que el nivel quepa
static bool Perezosa() { return g_matricesPerezosas && SombraPila() == 0 && SombraTope(0) != 0; }
static bool CargaPerezosa() { return g_matricesPerezosas && SombraPila() == 0 && gSombraNivel[0] < kSombraHondo; }
// una operacion INMEDIATA sobre la modelview: antes, GL con lo ultimo de la sombra; despues, GL y la sombra iguales
static void AntesInmediata() { if (SombraPila() == 0) MatricesSincronizar(); }
static void DespuesInmediata() { if (SombraPila() == 0 && SombraTope(0)) gSerieEnGL = gSerie[gSombraNivel[0]]; }

void LoadIdentity() {
    ProyCambio();
    if (CargaPerezosa()) { SombraCargar(kIdentidad); return; }
    g_statOtras++; W3D_GLC(glLoadIdentity()); SombraCargar(kIdentidad); DespuesInmediata();
}
void PushMatrix() {
    if (Perezosa() && gSombraNivel[0] + 1 < kSombraHondo) { SombraPush(false); return; }
    AntesInmediata(); g_statOtras++; W3D_GLC(glPushMatrix()); SombraPush(true);
}
void PopMatrix() {
    ProyCambio();
    if (SombraPila() == 0) {
        const int n = gSombraNivel[0];
        if (n > 0 && n < kSombraHondo && !gNivelReal[n]) {   // (se abrio solo en la sombra: se cierra solo en la sombra)
            SombraPop();
            if (!g_matricesPerezosas) MatricesSincronizar();   // (la opcion se apago con la pila abierta)
            return;
        }
        if (n == 0) MatricesSincronizar();   // un pop de mas: lo que GL tenga pasa a ser la verdad (la sombra lo olvida)
        g_statOtras++; W3D_GLC(glPopMatrix()); SombraPop();
        DespuesInmediata();   // (GL vuelve a la del nivel de abajo: se sincronizo antes de su glPushMatrix)
        return;
    }
    g_statOtras++; W3D_GLC(glPopMatrix()); SombraPop();
}
void Translatef(float x, float y, float z) {
    ProyCambio();
    const float t[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 };
    if (Perezosa()) { SombraMult(t); return; }
    AntesInmediata(); g_statOtras++; W3D_GLC(glTranslatef(x, y, z));
    SombraMult(t); DespuesInmediata();
}
// la rotacion como la arma el glRotatef de Mesa (m_matrix.c): el angulo a radianes en double, los ejes x/y/z sueltos con
// su caso exacto y sin rotacion por debajo de 1e-4 de largo o con angulo 0. Asi la sombra da los mismos bits que GL (la
// imagen perezosa es la misma que la inmediata: prueba_menos_llamadas.w3s, una rutina que rota). false = no rota
static bool MatrizRotacion(float a, float x, float y, float z, float m[16]) {
    if (a == 0.0f) return false;
    const float rad = (float)((double)a * 3.14159265358979323846 / 180.0);
    const float s = sinf(rad), c = cosf(rad);
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    // M(fila, col) = m[col * 4 + fila]
    if (x == 0.0f && y == 0.0f && z != 0.0f) {          // solo z
        m[0] = c; m[5] = c;
        if (z < 0.0f) { m[4] = s;  m[1] = -s; } else { m[4] = -s; m[1] = s; }
        return true;
    }
    if (x == 0.0f && y != 0.0f && z == 0.0f) {          // solo y
        m[0] = c; m[10] = c;
        if (y < 0.0f) { m[8] = -s; m[2] = s; } else { m[8] = s;  m[2] = -s; }
        return true;
    }
    if (x != 0.0f && y == 0.0f && z == 0.0f) {          // solo x
        m[5] = c; m[10] = c;
        if (x < 0.0f) { m[9] = s;  m[6] = -s; } else { m[9] = -s; m[6] = s; }
        return true;
    }
    const float mag = sqrtf(x * x + y * y + z * z);
    if (mag <= 1.0e-4f) return false;
    x /= mag; y /= mag; z /= mag;
    const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, yz = y * z, zx = z * x;
    const float xs = x * s, ys = y * s, zs = z * s, k = 1.0f - c;
    m[0] = (k * xx) + c;  m[4] = (k * xy) - zs; m[8]  = (k * zx) + ys;
    m[1] = (k * xy) + zs; m[5] = (k * yy) + c;  m[9]  = (k * yz) - xs;
    m[2] = (k * zx) - ys; m[6] = (k * yz) + xs; m[10] = (k * zz) + c;
    return true;
}
void Rotatef(float a, float x, float y, float z) {
    ProyCambio();
    float m[16];
    const bool rota = MatrizRotacion(a, x, y, z, m);
    if (Perezosa()) { if (rota) SombraMult(m); return; }
    AntesInmediata(); g_statOtras++; W3D_GLC(glRotatef(a, x, y, z));
    if (rota) SombraMult(m);
    DespuesInmediata();
}
void Scalef(float x, float y, float z) {
    ProyCambio();
    const float m[16] = { x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1 };
    if (Perezosa()) { SombraMult(m); return; }
    AntesInmediata(); g_statOtras++; W3D_GLC(glScalef(x, y, z));
    SombraMult(m); DespuesInmediata();
}
void MultMatrix(const float* m) {
    ProyCambio();
    if (Perezosa()) { SombraMult(m); return; }
    AntesInmediata(); g_statOtras++; W3D_GLC(glMultMatrixf(m)); SombraMult(m); DespuesInmediata();
}
void LoadMatrix(const float* m) {
    ProyCambio();
    if (CargaPerezosa()) { SombraCargar(m); return; }
    g_statOtras++; W3D_GLC(glLoadMatrixf(m)); SombraCargar(m); DespuesInmediata();
}
void SesgoMetros(float m) {
    if (m == gSesgoPuesto) return;   // (sin base conocida el sesgo es 0: la proyeccion puesta es la del pase)
    if (!gProyBaseOk) { GetMatrix(Projection, gProyBase); gProyBaseOk = true; }
    float P[16];
    for (int i = 0; i < 16; i++) P[i] = gProyBase[i];
    P[14] -= P[10] * m;   // z de camara corrida m metros (+ aleja, - acerca) sin mover la geometria en pantalla
    MatrixMode(Projection);
    W3D_GLC(glLoadMatrixf(P));
    SombraCargar(P);
    MatrixMode(ModelView);
    gSesgoPuesto = m;
}
void GetMatrix(Matrix m, float* out16) {
    const int s = (m == Projection) ? 1 : (m == TextureMatrix ? -1 : 0);
    const float* sombra = (s >= 0) ? SombraTope(s) : 0;
    if (sombra && !g_matrizAuditar) { for (int i = 0; i < 16; i++) out16[i] = sombra[i]; return; }
    if (s == 0) MatricesSincronizar();   // (lo que se lee de GL, con lo perezoso al dia)
    g_statOtras++;
    W3D_GLC(glGetFloatv(m == Projection ? GL_PROJECTION_MATRIX : m == TextureMatrix ? GL_TEXTURE_MATRIX : GL_MODELVIEW_MATRIX, out16));
    if (s < 0) return;
    if (sombra) {
        // (auditoria) la sombra tiene que dar lo mismo que GL
        for (int i = 0; i < 16; i++)
            if (fabsf(sombra[i] - out16[i]) > 1e-3f * (1.0f + fabsf(out16[i]))) { g_matrizDesyncs++; break; }
        return;
    }
    const int n = gSombraNivel[s];
    if (n < kSombraHondo) {
        for (int i = 0; i < 16; i++) gSombra[s][n][i] = out16[i];
        gSombraOk[s][n] = true;
        if (s == 0) gSerieEnGL = gSerie[n] = SerieNueva();   // (la leida es la que GL tiene)
    }
}
// (en la modelview -casi nunca- van inmediatas, con lo perezoso al dia antes)
void Ortho(float l, float r, float b, float t, float n, float f) {
    ProyCambio();
    AntesInmediata();
    #if defined(W3D_SYMBIAN) || defined(__ANDROID__)
        W3D_GLC(glOrthof(l, r, b, t, n, f));
    #else
        W3D_GLC(glOrtho(l, r, b, t, n, f));
    #endif
    const float m[16] = { 2.0f / (r - l), 0, 0, 0,  0, 2.0f / (t - b), 0, 0,  0, 0, -2.0f / (f - n), 0,
                          -(r + l) / (r - l), -(t + b) / (t - b), -(f + n) / (f - n), 1 };
    SombraMult(m);
    DespuesInmediata();
}

void Frustum(float l, float r, float b, float t, float n, float f) {
    ProyCambio();
    AntesInmediata();
    #if defined(W3D_SYMBIAN) || defined(__ANDROID__)
        W3D_GLC(glFrustumf(l, r, b, t, n, f));
    #else
        W3D_GLC(glFrustum(l, r, b, t, n, f));
    #endif
    const float m[16] = { 2.0f * n / (r - l), 0, 0, 0,  0, 2.0f * n / (t - b), 0, 0,
                          (r + l) / (r - l), (t + b) / (t - b), -(f + n) / (f - n), -1,  0, 0, -2.0f * f * n / (f - n), 0 };
    SombraMult(m);
    DespuesInmediata();
}

// perspectiva via Frustum (no usa GLU): mismo resultado que gluPerspective
void Perspective(float fovYDeg, float aspect, float n, float f) {
    float top   = n * tanf(fovYDeg * 3.14159265f / 360.0f);
    float right = top * aspect;
    Frustum(-right, right, -top, top, n, f);
}

// ---- estado suelto ----
void DepthMask(bool escribir)  {
    if (gDepthMaskCache == (escribir ? 1 : 0)) return;
    gDepthMaskCache = escribir ? 1 : 0;
    g_statStateChanges++; W3D_GLC(glDepthMask(escribir ? GL_TRUE : GL_FALSE));
}
#ifdef W3D_DREAMCAST
void LineWidth(float px)       { gLineaAncho = px; W3D_GLC(glLineWidth(px)); }
#else
void LineWidth(float px)       { W3D_GLC(glLineWidth(px)); }
#endif
void SmoothShading(bool suave) {
    if (gSmoothCache == (suave ? 1 : 0)) return;
    gSmoothCache = suave ? 1 : 0;
    g_statStateChanges++; W3D_GLC(glShadeModel(suave ? GL_SMOOTH : GL_FLAT));
}
void FastPerspective()         { W3D_GLC(glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST)); }

// ---- ESTADISTICAS de frame (bench/overlay): contadores baratos, ver w3dGraphics.h ----
int g_statDrawTris = 0;
int g_statDrawVBO  = 0;
int g_statIndices  = 0;
int g_statTexBinds = 0;
int g_statMeshes   = 0;
int g_statStateChanges = 0;   // cambios de estado REALES (los cacheados no cuentan)
int g_statUploadBytes  = 0;   // bytes subidos a buffer objects este frame
int g_statTrisOpacos   = 0;   // triangulos con Blend apagado
int g_statTrisBlend    = 0;   // triangulos con Blend prendido
int g_statDcCat[StatCatCount_] = { 0 };
void StatCategoria(StatCat c) { gStatCat = (int)c; }
StatCat StatCategoriaActual() { return (StatCat)gStatCat; }
void StatRaw(int n) { g_statLlamadas[gStatCat] += n; }
// el CUADRO: lo que se llamo desde el cierre anterior, por categoria, y los demas contadores como diferencia (no se
// ponen en 0: los que miden por diferencia -el bench, el perfil de las rutinas- siguen andando)
StatsDeCuadro g_statCuadro;
static int gCuadroDraws0 = 0, gCuadroBinds0 = 0, gCuadroEstados0 = 0, gCuadroIdx0 = 0;
void StatsCuadro() {
    StatsDeCuadro& c = g_statCuadro;
    c.total = 0;
    for (int i = 0; i < StatCatCount_; i++) { c.llamadas[i] = g_statLlamadas[i]; c.total += g_statLlamadas[i]; g_statLlamadas[i] = 0; }
    c.draws = g_statDrawTris - gCuadroDraws0;       gCuadroDraws0 = g_statDrawTris;
    c.binds = g_statTexBinds - gCuadroBinds0;       gCuadroBinds0 = g_statTexBinds;
    c.estados = g_statStateChanges - gCuadroEstados0; gCuadroEstados0 = g_statStateChanges;
    c.tris = (g_statIndices - gCuadroIdx0) / 3;     gCuadroIdx0 = g_statIndices;
    c.cuadros++;
    gStatCat = StatCatEditor;
}
// el conteo comun de TODO draw de triangulos: total + categoria + opaco/blend.
// gCapOn[Blend] es el cache del motor: fiel mientras todo el estado pase por aca.
// tras un draw con ColorArray prendido el "color actual" queda INDEFINIDO (spec
// de GL): el cache de Color4f no puede confiar en lo ultimo seteado. Y si ademas
// COLOR_MATERIAL estaba prendido, arrastra ambient/diffuse del material.
static inline void PostDrawColor() {
    if (gArrOn[ColorArray] == 1) {
        gColKnown = false;
        if (gCapOn[ColorMaterial]) { gMatKnown[MatAmbient] = false; gMatKnown[MatDiffuse] = false; }
    }
}
static inline void StatDrawTri(int nIndices) {
    g_statDrawTris++; g_statIndices += nIndices;
    g_statDcCat[gStatCat]++;
    if (gCapOn[Blend]) g_statTrisBlend  += nIndices / 3;
    else               g_statTrisOpacos += nIndices / 3;
#ifndef W3D_SYMBIAN
    if (g_trazaGLSec) printf("      [glsec] tris %d\n", nIndices / 3);   // (harness) de quien es cada triangulo
#endif
    PostDrawColor();   // todo draw de triangulos pasa por aca
}
void StatsReset() {
    g_statDrawTris = 0; g_statDrawVBO = 0; g_statIndices = 0; g_statTexBinds = 0; g_statMeshes = 0;
    g_statStateChanges = 0; g_statUploadBytes = 0; g_statTrisOpacos = 0; g_statTrisBlend = 0; g_statOtras = 0;
    for (int i = 0; i < StatCatCount_; i++) { g_statDcCat[i] = 0; g_statLlamadas[i] = 0; }
    gCuadroDraws0 = gCuadroBinds0 = gCuadroEstados0 = gCuadroIdx0 = 0;
    gStatCat = StatCatEscena;
}
void Finish() { W3D_GLC(glFinish()); }

// ---- textura activa (cacheada) ----
void BindTexture(unsigned int id) {
    if (id == gTexBound) { return; } // ya esta bindeada -> ahorramos el bind
    gTexBound = id;
    g_statTexBinds++;
    W3D_GLC(glBindTexture(GL_TEXTURE_2D, id));
}


// CHROME: genera las UV con sphere-map del pipeline fijo (PC). En GLES1 (Symbian) no hay glTexGen ->
// stub (calcular las UV del chrome por software a partir del normal y la camara).
void TexGenSphere(bool on) {
#if defined(W3D_SYMBIAN) || defined(__ANDROID__) || defined(W3D_DREAMCAST)
    (void)on;
#else
    if (gTexGenCache == (on ? 1 : 0)) return;   // ya esta (la matriz del texgen es fija)
    gTexGenCache = on ? 1 : 0;
    gMatcapCache = -1;   // (carga su propia matriz de textura)
    g_statStateChanges++;
    if (on) {
        W3D_GLC(glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP));
        W3D_GLC(glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP));
        W3D_GLC(glEnable(GL_TEXTURE_GEN_S));
        W3D_GLC(glEnable(GL_TEXTURE_GEN_T));
        // el sphere-map genera la V al reves de las UV del modelo (que ya hacen 1-v por el stb top-first):
        // flip-V con la matriz de textura para que el reflejo no salga dado vuelta verticalmente. v -> 1-v
        MatrixMode(TextureMatrix);
        W3D_GLC(glLoadIdentity());
        W3D_GLC(glTranslatef(0.0f, 1.0f, 0.0f));
        W3D_GLC(glScalef(1.0f, -1.0f, 1.0f));
        MatrixMode(ModelView);
    } else {
        W3D_GLC(glDisable(GL_TEXTURE_GEN_S));
        W3D_GLC(glDisable(GL_TEXTURE_GEN_T));
        MatrixMode(TextureMatrix);
        W3D_GLC(glLoadIdentity()); // saca el flip-V (que no leakee a las UV normales de la proxima textura)
        MatrixMode(ModelView);
    }
#endif
}

// MATCAP por hardware SIN texgen (sirve en GLES1/N95). Las normales se mandan como texcoords GLbyte [-127,127]
// (TexCoordPointer3b); esta matriz de TEXTURA las convierte en UV de matcap: T = bias(0.5,0.5) * flipV * (0.5/127)
// * rotacionDelModelview. El 1/127 normaliza el byte; la rotacion del modelview lleva la normal a espacio de OJO
// (el matcap sigue a la camara); el flip-V matchea la orientacion del GL_SPHERE_MAP del PC. Es el "normal del ojo"
// (aprox del sphere-map exacto: usa la normal, no el vector reflejado -> no necesita la posicion del vertice).
void TexMatrixMatcap(bool on) {
    // off->off se ahorra; on SIEMPRE ejecuta: la matriz sale del MODELVIEW ACTUAL
    // (cambia por malla), cachear "on" dibujaria el matcap con la matriz de otra.
    if (!on && gMatcapCache == 0) return;
    gMatcapCache = on ? 1 : 0;
    g_statStateChanges++;
    MatrixMode(TextureMatrix);
    if (on) {
        GLfloat mv[16];
        GetMatrix(ModelView, mv);   // (de la sombra: sin glGet)
        const GLfloat s = 0.5f / 127.0f;
        GLfloat T[16];
        // column-major. fila0 -> u = s*(MV*N).x + 0.5 ; fila1 -> v = 0.5 - s*(MV*N).y (flip-V)
        T[0] = s*mv[0];  T[1] = -s*mv[1]; T[2] = 0.0f; T[3] = 0.0f;
        T[4] = s*mv[4];  T[5] = -s*mv[5]; T[6] = 0.0f; T[7] = 0.0f;
        T[8] = s*mv[8];  T[9] = -s*mv[9]; T[10]= 0.0f; T[11]= 0.0f;
        T[12]= 0.5f;     T[13]= 0.5f;     T[14]= 0.0f; T[15]= 1.0f;
        W3D_GLC(glLoadMatrixf(T));
    } else {
        W3D_GLC(glLoadIdentity()); // resetea (que no leakee a las UV de la proxima textura)
    }
    MatrixMode(ModelView);
}

void TexMatrixUV(float du, float dv, float su, float sv) {
    const bool ident = du == 0.0f && dv == 0.0f && su == 1.0f && sv == 1.0f;
    if (ident ? gMatcapCache == 0 : (gMatcapCache == 2 && du == gUVMat[0] && dv == gUVMat[1] && su == gUVMat[2] && sv == gUVMat[3]))
        return;
    gMatcapCache = ident ? 0 : 2;
    gUVMat[0] = du; gUVMat[1] = dv; gUVMat[2] = su; gUVMat[3] = sv;
    g_statStateChanges++;
    // T(du, dv) * S(su, sv) armada aca: UNA carga en vez de identidad + trasladar + escalar
    const GLfloat M[16] = { su, 0, 0, 0,  0, sv, 0, 0,  0, 0, 1, 0,  du, dv, 0, 1 };
    MatrixMode(TextureMatrix);
    if (ident) W3D_GLC(glLoadIdentity()); else W3D_GLC(glLoadMatrixf(M));
    MatrixMode(ModelView);
}

// true si el backend tiene glTexGen (sphere-map por HARDWARE). PC = si; GLES1 del N95 = NO -> el matcap se
// calcula por SOFTWARE (sphere-map por vertice, como el equirect). Lo usa el render del chrome para elegir.
bool TieneTexGen() {
#if defined(W3D_SYMBIAN) || defined(W3D_DREAMCAST)
    return false;   // (Dreamcast: GLdc no tiene glTexGen)
#else
    return true;
#endif
}

// CHROME (matcap o equirect) = ESPEJO: la textura va DIRECTA (GL_REPLACE), NO modulada por la luz/color ->
// el reflejo es independiente de la iluminacion. false = GL_MODULATE normal (UI/texturas tintadas por color).
void TexEnvReplace(bool on) {
    int destino = on ? 1 : 0;   // 1=replace 0=modulate (ver gTexEnvCache)
    if (gTexEnvCache == destino) return;
    gTexEnvCache = destino;
    g_statStateChanges++;
    W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, on ? GL_REPLACE : GL_MODULATE));
}

// DOT3 normal mapping: el texture combiner calcula N.L POR PIXEL (N = textura activa, L = COLOR por vertice).
// Usa solo glTexEnvi (esta en GL 1.1) -> portable PC + N95 SIN cargar funciones de multitextura. Los enums son
// GL 1.3/GLES1: en Windows GL 1.1 pueden faltar, asi que los #define con su valor estandar. Reset -> MODULATE.
#ifndef GL_COMBINE
#define GL_COMBINE        0x8570
#endif
#ifndef GL_COMBINE_RGB
#define GL_COMBINE_RGB    0x8571
#endif
#ifndef GL_SRC0_RGB
#define GL_SRC0_RGB       0x8580
#endif
#ifndef GL_SRC1_RGB
#define GL_SRC1_RGB       0x8581
#endif
#ifndef GL_OPERAND0_RGB
#define GL_OPERAND0_RGB   0x8590
#endif
#ifndef GL_OPERAND1_RGB
#define GL_OPERAND1_RGB   0x8591
#endif
#ifndef GL_DOT3_RGB
#define GL_DOT3_RGB       0x86AE
#endif
#ifndef GL_PRIMARY_COLOR
#define GL_PRIMARY_COLOR  0x8577
#endif
void TexEnvDot3(bool on) {
    int destino = on ? 2 : 0;   // 2=dot3 0=modulate
    if (gTexEnvCache == destino) return;
    gTexEnvCache = destino;
    g_statStateChanges++;
    if (on) {
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE));
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,   GL_DOT3_RGB));
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB,      GL_TEXTURE));       // arg0 = normal del mapa
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB,  GL_SRC_COLOR));
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB,      GL_PRIMARY_COLOR)); // arg1 = L (color del vertice)
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB,  GL_SRC_COLOR));
    } else {
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE));
    }
}

// enums de la parte ALPHA del combiner (los RGB ya estan arriba). GL 1.3/GLES1; en Windows GL 1.1 faltan.
#ifndef GL_COMBINE_ALPHA
#define GL_COMBINE_ALPHA  0x8572
#endif
#ifndef GL_SRC0_ALPHA
#define GL_SRC0_ALPHA     0x8588
#endif
#ifndef GL_OPERAND0_ALPHA
#define GL_OPERAND0_ALPHA 0x8598
#endif
#ifndef GL_SRC1_ALPHA
#define GL_SRC1_ALPHA     0x8589
#endif
#ifndef GL_OPERAND1_ALPHA
#define GL_OPERAND1_ALPHA 0x8599
#endif
// ALPHA-ONLY: color del fragmento = PRIMARY (color de vertice/uniforme), pero el ALPHA = alpha de la
// TEXTURA. Para los PASES PLANOS (zbuffer/alpha/normal): la forma del objeto transparente sale SOLO del
// alpha de su textura, con el color plano (blanco o normal), sin mostrar el color de la textura.
// false = GL_MODULATE normal. Solo glTexEnvi (GL 1.1) -> portable PC + N95.
void TexEnvAlphaOnly(bool on) {
    int destino = on ? 3 : 0;   // 3=alpha-only 0=modulate
    if (gTexEnvCache == destino) return;
    gTexEnvCache = destino;
    g_statStateChanges++;
    if (on) {
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE));
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,    GL_REPLACE));
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB,       GL_PRIMARY_COLOR)); // RGB = color plano
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB,   GL_SRC_COLOR));
        // ALPHA = alpha de la textura por el del color (con el color opaco, el de la textura: los pases planos del
        // editor; con alfa por vertice, un desvanecido: el agua de una rutina)
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,  GL_MODULATE));
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA,     GL_TEXTURE));
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA));
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ALPHA,     GL_PRIMARY_COLOR));
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA));
    } else {
        W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE));
    }
}

// ---- color uniforme ----
#ifdef W3D_DREAMCAST
void FrontFace(bool ccw) { gFrenteCCW = ccw; W3D_GLC(glFrontFace(ccw ? GL_CCW : GL_CW)); }
void CullCarasFrontales(bool on) { gCullFrente = on; W3D_GLC(glCullFace(on ? GL_FRONT : GL_BACK)); g_statStateChanges++; }
void ColorMask(bool r, bool g, bool b, bool a) { gColorMaskOn = r || g || b; W3D_GLC(glColorMask(r, g, b, a)); }
#else
void FrontFace(bool ccw) { W3D_GLC(glFrontFace(ccw ? GL_CCW : GL_CW)); }
void CullCarasFrontales(bool on) { W3D_GLC(glCullFace(on ? GL_FRONT : GL_BACK)); g_statStateChanges++; }
void ColorMask(bool r, bool g, bool b, bool a) { W3D_GLC(glColorMask(r, g, b, a)); }
#endif
#ifdef W3D_DREAMCAST
void PointSpriteCoordReplace(bool on) { (void)on; }   // (los puntos de la Dreamcast ya van con la textura entera)
#else
void PointSpriteCoordReplace(bool on) { W3D_GLC(glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, on ? GL_TRUE : GL_FALSE)); }
#endif

void Color4f(float r, float g, float b, float a) {
    if (gColKnown && gColCache[0] == r && gColCache[1] == g &&
        gColCache[2] == b && gColCache[3] == a) return;   // mismo color -> nada
    gColCache[0] = r; gColCache[1] = g; gColCache[2] = b; gColCache[3] = a;
    gColKnown = true;
    // con COLOR_MATERIAL prendido este color PISA ambient/diffuse del material
    if (gCapOn[ColorMaterial]) { gMatKnown[MatAmbient] = false; gMatKnown[MatDiffuse] = false; }
    W3D_GLC(glColor4f(r, g, b, a));
}
// glColor4fv NO existe en GLES1 (solo glColor4f/4ub/4x): expandir a Color4f (cacheado)
void Color4fv(const float* c) { Color4f(c[0], c[1], c[2], c[3]); }

// --- niebla ---
#ifdef W3D_DREAMCAST
// GLdc toma el modo de la niebla solo como entero (glFogf con GL_FOG_MODE da GL_INVALID_ENUM y queda la de fabrica,
// que nublaba toda la vista)
void FogMode(bool linear) { W3D_GLC(glFogi(GL_FOG_MODE, linear ? GL_LINEAR : GL_EXP)); }
#else
void FogMode(bool linear) { W3D_GLC(glFogf(GL_FOG_MODE, (float)(linear ? GL_LINEAR : GL_EXP))); }
#endif
void FogStart(float z)    { W3D_GLC(glFogf(GL_FOG_START, z)); }
void FogEnd(float z)      { W3D_GLC(glFogf(GL_FOG_END, z)); }
void FogColor(const float* c) { W3D_GLC(glFogfv(GL_FOG_COLOR, c)); }
#ifdef W3D_DREAMCAST
void FogModo(int m)       { W3D_GLC(glFogi(GL_FOG_MODE, m == 2 ? GL_EXP2 : (m == 1 ? GL_EXP : GL_LINEAR))); }
#else
void FogModo(int m)       { W3D_GLC(glFogf(GL_FOG_MODE, (float)(m == 2 ? GL_EXP2 : (m == 1 ? GL_EXP : GL_LINEAR)))); }
#endif
void FogDensidad(float d) { W3D_GLC(glFogf(GL_FOG_DENSITY, d)); }

// --- luz 0 ---
static GLenum LightFvGL(LightFv p) {
    switch (p) {
        case LightAmbient:  return GL_AMBIENT;
        case LightDiffuse:  return GL_DIFFUSE;
        case LightSpecular: return GL_SPECULAR;
        case LightPosition: return GL_POSITION;
    }
    return GL_AMBIENT;
}
// (la POSICION se transforma con la modelview vigente: la perezosa tiene que estar en GL)
void Light0fv(LightFv p, const float* v) { if (p == LightPosition) MatricesSincronizar(); W3D_GLC(glLightfv(GL_LIGHT0, LightFvGL(p), v)); }
static GLenum LightFGL(LightF p) {
    switch (p) {
        case LightConstantAtt:  return GL_CONSTANT_ATTENUATION;
        case LightLinearAtt:    return GL_LINEAR_ATTENUATION;
        case LightQuadraticAtt: return GL_QUADRATIC_ATTENUATION;
    }
    return GL_CONSTANT_ATTENUATION;
}
void Light0f(LightF p, float v) { W3D_GLC(glLightf(GL_LIGHT0, LightFGL(p), v)); }
// (las luces 1..7 con su cache: el pase de escena apaga las 8 en cada cuadro -por si una luz cambio de numero- y casi
// siempre ya estan apagadas. La 0 es una Cap con el cache de siempre)
static signed char gLuzOn[LucesMax] = { -1, -1, -1, -1, -1, -1, -1, -1 };
void SetLightEnabled(unsigned int id, bool on) {
    if (id >= (unsigned int)GL_LIGHT0 && id < (unsigned int)GL_LIGHT0 + LucesMax) { LightN((int)(id - GL_LIGHT0), on); return; }
    if (on) W3D_GLC(glEnable(id)); else W3D_GLC(glDisable(id));
}
void LightNfv(int i, LightFv p, const float* v) {
    if (i < 0 || i >= LucesMax) return;
    if (p == LightPosition) MatricesSincronizar();
    g_statOtras++;
    W3D_GLC(glLightfv(GL_LIGHT0 + (GLenum)i, LightFvGL(p), v));
}
void LightNf(int i, LightF p, float v) {
    if (i < 0 || i >= LucesMax) return;
    g_statOtras++;
    W3D_GLC(glLightf(GL_LIGHT0 + (GLenum)i, LightFGL(p), v));
}
void LightNSpot(int i, float corte, float exponente, const float* dir3) {
    if (i < 0 || i >= LucesMax) return;
    MatricesSincronizar();   // (GL transforma la direccion con SU modelview)
    g_statOtras += 3;
    W3D_GLC(glLightf(GL_LIGHT0 + (GLenum)i, GL_SPOT_CUTOFF, corte));
    W3D_GLC(glLightf(GL_LIGHT0 + (GLenum)i, GL_SPOT_EXPONENT, exponente));
    W3D_GLC(glLightfv(GL_LIGHT0 + (GLenum)i, GL_SPOT_DIRECTION, dir3));
}
void LightN(int i, bool on) {
    if (i == 0) { if (on) Enable(Light0); else Disable(Light0); return; }   // (la 0 tiene cache)
    if (i < 0 || i >= LucesMax) return;
    if (gLuzOn[i] == (on ? 1 : 0)) return;
    gLuzOn[i] = on ? 1 : 0;
    g_statStateChanges++;
    if (on) W3D_GLC(glEnable(GL_LIGHT0 + (GLenum)i)); else W3D_GLC(glDisable(GL_LIGHT0 + (GLenum)i));
}
void LightModelAmbient(const float* c) { g_statOtras++; W3D_GLC(glLightModelfv(GL_LIGHT_MODEL_AMBIENT, c)); }

// query del CACHE (todas las Enable/Disable pasan por el motor, asi que es fiel)
bool IsEnabled(Cap c) { return gCapOn[c]; }

#ifndef W3D_DREAMCAST
void ReadPixelsBegin(int, int, int, int) {}   // (GL lee el framebuffer de verdad: no hace falta preparar nada)
void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* p) {
    W3D_GLC(glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p));
}
#endif
void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    gColKnown = false;   // otra precision: el cache de Color4f deja de valer
    if (gCapOn[ColorMaterial]) { gMatKnown[MatAmbient] = false; gMatKnown[MatDiffuse] = false; }
    W3D_GLC(glColor4ub(r, g, b, a));
}

// ---- material: float en escritorio / GL ES 1.1 (Symbian); punto fijo en
//      GL ES 1.0 (Android viejo). Aca vive el unico #ifdef ANDROID. ----
static GLenum MatParamGL(MatParam p) {
    switch (p) {
        case MatAmbient:  return GL_AMBIENT;
        case MatDiffuse:  return GL_DIFFUSE;
        case MatSpecular: return GL_SPECULAR;
        case MatEmission: return GL_EMISSION;
    }
    return GL_DIFFUSE;
}
void Material(MatParam p, const float* rgba) {
    float* c = gMatCache[p];
    if (gMatKnown[p] && c[0] == rgba[0] && c[1] == rgba[1] &&
        c[2] == rgba[2] && c[3] == rgba[3]) return;   // ya puesto -> nada
    c[0] = rgba[0]; c[1] = rgba[1]; c[2] = rgba[2]; c[3] = rgba[3];
    // con COLOR_MATERIAL prendido, GL trackea ambient/diffuse desde el color
    // actual: lo que se mande aca queda pisado -> no se puede dar por puesto
    gMatKnown[p] = !(gCapOn[ColorMaterial] && (p == MatAmbient || p == MatDiffuse));
#ifdef __ANDROID__
    GLfixed x[4]; // 16.16: float * 65536
    for (int i = 0; i < 4; i++) x[i] = (GLfixed)(rgba[i] * 65536.0f);
    W3D_GLC(glMaterialxv(GL_FRONT_AND_BACK, MatParamGL(p), x));
#else
    W3D_GLC(glMaterialfv(GL_FRONT_AND_BACK, MatParamGL(p), rgba));
#endif
}
void MaterialShininess(float s) {
    if (gShinCache == s) return;
    gShinCache = s;
#ifdef __ANDROID__
    W3D_GLC(glMaterialx(GL_FRONT_AND_BACK, GL_SHININESS, (GLfixed)(s * 65536.0f)));
#else
    W3D_GLC(glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, s));
#endif
}

// las FIRMAS de glBlendFunc que usa el motor (para el cache): un numero por par
// src/dst. BlendAlpha/BlendMode y SetMezcla reponen TAMBIEN la blend equation
// (cacheada aparte en gBlendEqCache): solo MezclaSubtract la deja en
// REVERSE_SUBTRACT, y el proximo que pida alpha/capas vuelve a FUNC_ADD.
enum {
    FzOff = 0,        // ONE, ZERO
    FzAdd,            // ONE, ONE
    FzAddAlpha,       // SRC_ALPHA, ONE
    FzMultiply,       // DST_COLOR, ZERO
    FzScreen,         // ONE, ONE_MINUS_SRC_COLOR
    FzPremult,        // ONE, ONE_MINUS_SRC_ALPHA
    FzAlpha,          // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    FzSubAprox,       // ZERO, ONE_MINUS_SRC_COLOR (aprox Symbian)
    FzMultiply2x      // DST_COLOR, SRC_COLOR (= 2 * dst * src: el "x2" de un lightmap con overbright)
};
static void PonerBlendFunc(int firma) {
    if (gBlendFuncCache == firma) return;
    gBlendFuncCache = firma;
    g_statStateChanges++;
    switch (firma) {
        case FzOff:      W3D_GLC(glBlendFunc(GL_ONE, GL_ZERO)); break;
        case FzAdd:      W3D_GLC(glBlendFunc(GL_ONE, GL_ONE)); break;
        case FzAddAlpha: W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE)); break;
        case FzMultiply: W3D_GLC(glBlendFunc(GL_DST_COLOR, GL_ZERO)); break;
        case FzScreen:   W3D_GLC(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR)); break;
        case FzPremult:  W3D_GLC(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)); break;
        case FzSubAprox: W3D_GLC(glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR)); break;
        case FzMultiply2x: W3D_GLC(glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR)); break;
        case FzAlpha: default: W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); break;
    }
}
#ifndef W3D_SYMBIAN
static void PonerBlendEq(int eq) {   // 0 = FUNC_ADD, 1 = REVERSE_SUBTRACT
    if (gBlendEqCache == eq) return;
    gBlendEqCache = eq;
    g_statStateChanges++;
    const GLenum e = (GLenum)(eq ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
#if defined(_WIN32) && !defined(W3D_SYMBIAN)
    // opengl32 solo exporta GL 1.1: glBlendEquation (1.4) se carga por wglGetProcAddress
    // (contexto ya activo cuando se dibuja). Si el driver no lo tiene, se deja el eq previo.
    static W3D_PFNBLENDEQ pfn = (W3D_PFNBLENDEQ)wglGetProcAddress("glBlendEquation");
    if (pfn) W3D_GLC(pfn(e));
#else
    W3D_GLC(glBlendEquation(e));
#endif
}
#endif
// LA ECUACION TAMBIEN: el que pide "alpha comun" espera FUNC_ADD. Si el ultimo
// dibujo fue SUSTRACTIVO (SetMezcla(MezclaSubtract) -> eq REVERSE_SUBTRACT, ej.
// el humo de chimenea de un juego) y aca solo se reponia la FUNC, todo lo
// alpha-blended que siguiera RESTABA del framebuffer: dst*(1-a) - src*a = una
// silueta NEGRA con el alpha bien (la fruta del HUD "parpadeando a negro" cada
// vez que el pase 3D terminaba en una particula sustractiva). Con el cache,
// re-poner ADD ya puesto no llama al driver: costo cero en el frame comun.
void BlendAlpha() {
#ifndef W3D_SYMBIAN
    PonerBlendEq(0);
#endif
    PonerBlendFunc(FzAlpha);
}
// BLUR por shader: el pipeline FIJO no tiene shaders -> no-op (la app no se rompe, solo no difumina).
void BlurY(float) {}

void AlfaEmpaquetado(bool) {}   // sin shaders no hay forma: se ignora (el video sale opaco)

void AlphaTest(float ref) {
    if (gAlphaRefCache == ref) return;
    gAlphaRefCache = ref;
    g_statStateChanges++;
    if (ref > 0.0f) { W3D_GLC(glAlphaFunc(GL_GREATER, ref)); W3D_GLC(glEnable(GL_ALPHA_TEST)); }
    else            { W3D_GLC(glDisable(GL_ALPHA_TEST)); }
}

// modos de mezcla nombrados (ver enum Mezcla). Fixed-function: anda en PC y GL ES 1.1 (N95).
// Con el cache: mismo modo ya puesto = cero llamadas (eq y func se cachean por separado,
// mismo efecto neto que el reset+set de antes).
void SetMezcla(int m) {
    int firma;
    switch (m) {
        case MezclaOff:      firma = FzOff; break;
        case MezclaAdd:      firma = FzAdd; break;
        case MezclaAddAlpha: firma = FzAddAlpha; break;
        case MezclaMultiply: firma = FzMultiply; break;
        case MezclaScreen:   firma = FzScreen; break;
        case MezclaPremult:  firma = FzPremult; break;
#ifndef W3D_SYMBIAN
        case MezclaSubtract: firma = FzAdd; break;      // resta real: ONE,ONE + eq REVERSE_SUBTRACT
#else
        case MezclaSubtract: firma = FzSubAprox; break; // aprox (oscurece)
#endif
        case MezclaAlpha:
        default:             firma = FzAlpha; break;
    }
#ifndef W3D_SYMBIAN
    PonerBlendEq(m == MezclaSubtract ? 1 : 0);
#endif
    PonerBlendFunc(firma);
}
void BlendMode(int modo) { // capa multi-pass sobre lo de abajo (W3dCapaMezcla, objects/Materials.h)
#ifndef W3D_SYMBIAN
    PonerBlendEq(modo == 4 ? 1 : 0);   // solo Subtract deja REVERSE_SUBTRACT; BlendAlpha lo repone
#endif
    if (modo == 1)      PonerBlendFunc(FzMultiply);    // Multiply (oscurece)
    else if (modo == 2) PonerBlendFunc(FzAdd);         // Add (aclara)
    else if (modo == 3) PonerBlendFunc(FzMultiply2x);  // Multiply 2x (lightmaps con overbright)
    else if (modo == 4) {
#ifndef W3D_SYMBIAN
        PonerBlendFunc(FzAdd);                         // resta real: ONE,ONE + REVERSE_SUBTRACT
#else
        PonerBlendFunc(FzSubAprox);                    // GL ES 1.1 sin blend equation: aprox (oscurece)
#endif
    }
    else if (modo == 5) PonerBlendFunc(FzScreen);      // Screen (aclara suave)
    else                PonerBlendFunc(FzAlpha);       // Mix (alpha encima)
}

// ---- parametros de la textura activa ----
// glTexParameter es estado DEL OBJETO textura: persiste con ella. La firma de lo
// puesto se cachea POR TEXTURA (w3dTexture.cpp): re-mandar lo mismo por cada
// malla que la usa era churn puro (Claude: 16 mallas x 4 glTexParameter = 64
// llamadas identicas por frame). Con el bind desconocido se manda sin cachear.
void TexFilter(bool linear) {
    GLint f = linear ? GL_LINEAR : GL_NEAREST;
    // min-filter con MIPMAPS si esta textura subio piramide (TexTieneMips) y el
    // mipmapping global esta prendido; pedirselo a una sin piramide = incompleta.
    GLint fmin = f;
    const bool conMips = MipmapsGlobal() && TexTieneMips(gTexBound);
    if (conMips)
        fmin = linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    if (gTexBound != 0xFFFFFFFFu &&
        !TexFiltroCambia(gTexBound, (linear ? 1 : 0) | (conMips ? 2 : 0))) return;
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fmin));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f));
}
void TexWrap(bool repeat) {
    if (gTexBound != 0xFFFFFFFFu &&
        !TexWrapCambia(gTexBound, repeat ? 1 : 0)) return;
    GLfloat w = (GLfloat)(repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    W3D_GLC(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, w));
    W3D_GLC(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, w));
}

// fija el nivel BASE de la piramide de la textura activa (0 = normal). Lo usa el
// inspector de mipmaps del editor UV para VER un nivel puntual. Solo escritorio:
// GLES 1.1 (N95) no tiene GL_TEXTURE_BASE_LEVEL.
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL 0x813C
#endif
void TexBaseLevel(int nivel) {
#if defined(W3D_SYMBIAN) || defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    (void)nivel;
#else
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, nivel < 0 ? 0 : nivel));
#endif
}

// ---- punteros de los arrays ----
// el VBO de atributos bindeado (cache): un puntero de RAM con un VBO bindeado seria un OFFSET adentro del VBO (lo que
// dejo la escena rompia la interfaz). Los punteros de RAM lo desbindean si hace falta (gratis si ya esta en 0)
static unsigned gArrBuf = 0xFFFFFFFFu;   // (desconocido)
static void BindArrBuf(unsigned h) { if (gArrBuf == h) return; gArrBuf = h; W3D_GLC(GLB_BIND(GL_ARRAY_BUFFER, (GLuint)h)); }
static void SinVBO() { if (w3d_vboOk && gArrBuf != 0) BindArrBuf(0); }
// CACHE DE PUNTEROS (ver w3dGraphics.h): lo que GL tiene en cada array de la unidad 0 (indice = VArray). GL guarda con
// cada puntero el buffer bindeado al ponerlo, asi que uno igual no necesita ni el puntero ni el bind. Y el IBO bindeado
// (0xFFFFFFFF = no se sabe). La unidad 1 (mezcla de texturas) lleva su propio cache: gUV1Puesto*
bool g_punterosCache = true;
struct PunteroPuesto { unsigned buf; const void* ptr; int tam; unsigned tipo; int paso; };
static PunteroPuesto gPtr[4];
static bool gPtrOk[4] = { false, false, false, false };
static unsigned gIdxBuf = 0xFFFFFFFFu;
// true si GL ya tiene ESE puntero en el array (no hay que llamar); si no, queda anotado como el puesto
static bool PtrIgual(int a, unsigned buf, const void* p, int tam, unsigned tipo, int paso) {
    PunteroPuesto& q = gPtr[a];
    if (g_punterosCache && gPtrOk[a] && q.buf == buf && q.ptr == p && q.tam == tam && q.tipo == tipo && q.paso == paso) return true;
    q.buf = buf; q.ptr = p; q.tam = tam; q.tipo = tipo; q.paso = paso; gPtrOk[a] = true;
    return false;
}
// un draw con indices de RAM necesita el ELEMENT_ARRAY en 0 (el IBO que dejo la malla anterior: UnbindVBOs es
// perezoso). Con el cache apagado, como antes: despues de Invalidate se supone 0
static void SinIBO() {
    if (!w3d_vboOk || gIdxBuf == 0) return;
    if (gIdxBuf == 0xFFFFFFFFu && !g_punterosCache) return;
    gIdxBuf = 0; g_statOtras++; W3D_GLC(GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, 0));
}

// ---------------------------------------------------------------------------
//  MEZCLA DE TEXTURAS en UNA pasada (multitextura, la unidad 1): fragmento = B * k + unidad0 * (1 - k).
//  La unidad 0 queda como siempre (la textura del material con su luz / color / modo). La unidad 1 bindea B con el
//  combiner GL_INTERPOLATE: arg0 = su textura (B), arg1 = GL_PREVIOUS (lo que salio de la unidad 0), arg2 = el alfa
//  del color constante del env (k). El alfa del fragmento sigue siendo el de la unidad 0 (REPLACE de PREVIOUS). Las
//  UV de la unidad 1 son las MISMAS que las de la 0: cada puntero / array de UV de la 0 se replica en la 1 mientras
//  la mezcla esta prendida. Todo vuelve a la unidad 0 antes de salir de cada funcion: el resto del motor no sabe que
//  existe la unidad 1 (sus caches son de la 0). GL 1.3 / GL ES 1.1: 2 unidades garantizadas (N95 incluido).
//  Cache propio: no se repite lo que ya esta puesto (prender = 12 llamadas una vez; cambiar k = 1 glTexEnv).
// ---------------------------------------------------------------------------
#ifndef GL_TEXTURE0
#define GL_TEXTURE0           0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1           0x84C1
#endif
#ifndef GL_MAX_TEXTURE_UNITS
#define GL_MAX_TEXTURE_UNITS  0x84E2
#endif
#ifndef GL_INTERPOLATE
#define GL_INTERPOLATE        0x8575
#endif
#ifndef GL_CONSTANT
#define GL_CONSTANT           0x8576
#endif
#ifndef GL_PREVIOUS
#define GL_PREVIOUS           0x8578
#endif
#ifndef GL_SRC2_RGB
#define GL_SRC2_RGB           0x8582
#endif
#ifndef GL_OPERAND2_RGB
#define GL_OPERAND2_RGB       0x8592
#endif
#ifndef GL_RGB_SCALE
#define GL_RGB_SCALE          0x8573
#endif
#ifndef GL_SUBTRACT
#define GL_SUBTRACT           0x84E7
#endif
bool MezclaTexturasSoportada() {
#ifdef W3D_DREAMCAST
    return false;   // GLdc no tiene el combiner (GL_COMBINE): la mezcla de texturas va en varias pasadas
#endif
    if (gMezclaSoportada < 0) {
        w3d_CargarVBO();   // (Windows: carga tambien glActiveTexture / glClientActiveTexture)
        GLint n = 1;
        if (GLMT_OK) { W3D_GLC(glGetIntegerv(GL_MAX_TEXTURE_UNITS, &n)); }   // (una vez: en un tiler es un flush)
        gMezclaSoportada = (GLMT_OK && n >= 2) ? 1 : 0;
    }
    return gMezclaSoportada == 1;
}
bool MezclaTexturasActiva() { return gMezclaTexOn == 1; }
static void MezclaUnidad1Array(bool on) {
    W3D_GLC(GLMT_CLIENT(GL_TEXTURE1));
    if (on) { W3D_GLC(glEnableClientState(GL_TEXTURE_COORD_ARRAY)); }
    else    { W3D_GLC(glDisableClientState(GL_TEXTURE_COORD_ARRAY)); }
    W3D_GLC(GLMT_CLIENT(GL_TEXTURE0));
    g_statStateChanges++;
}
// el puntero de UV de la unidad 1: el PROPIO (MezclaTexturasUV) o, si no tiene, el de la unidad 0 replicado.
// 'rebindear': el VBO bindeado puede no ser el del puntero (los punteros de abajo lo bindean antes de apuntar: aca se
// repone; el proximo puntero de la unidad 0 vuelve a bindear el suyo, el cache no repite)
static void MezclaReplicarUV(bool rebindear) {
    const int tipo = gUV1Tipo ? gUV1Tipo : gUV0Tipo;
    if (tipo == 0) return;
    const unsigned vbo = gUV1Tipo ? gUV1Vbo : gUV0Vbo;
    const float* ptr = gUV1Tipo ? gUV1Ptr : gUV0Ptr;
    const int stride = gUV1Tipo ? 0 : gUV0Stride;
    // ya apunta ahi (la rutina de un mundo apaga y prende la unidad 1 entre pasadas: el puntero quedo)
    if (gUV1PuestoTipo == tipo && (tipo == 2 ? gUV1PuestoVbo == vbo : (gUV1PuestoPtr == ptr && gUV1PuestoStride == stride))) return;
    if (rebindear || gUV1Tipo) BindArrBuf(tipo == 2 ? vbo : 0u);
    W3D_GLC(GLMT_CLIENT(GL_TEXTURE1));
    if (tipo == 2)         { W3D_GLC(glTexCoordPointer(2, GL_FLOAT, 0, (const GLvoid*)0)); }
    else                   { W3D_GLC(glTexCoordPointer(2, GL_FLOAT, stride, ptr)); }
    W3D_GLC(GLMT_CLIENT(GL_TEXTURE0));
    g_statOtras += 3;
    gUV1PuestoTipo = tipo; gUV1PuestoVbo = vbo; gUV1PuestoPtr = ptr; gUV1PuestoStride = stride;
}
void MezclaTexturasUV(const float* p) {
    if (gMezclaTexOn == 1 && gUV1Tipo == (p ? 1 : 0) && gUV1Ptr == p) return;   // (ya apunta ahi)
    gUV1Tipo = p ? 1 : 0; gUV1Ptr = p;
    if (gMezclaTexOn == 1) MezclaReplicarUV(true);
}
void MezclaTexturasUVVBO(unsigned int h) {
    if (gMezclaTexOn == 1 && gUV1Tipo == (h ? 2 : 0) && gUV1Vbo == h) return;
    gUV1Tipo = h ? 2 : 0; gUV1Vbo = h;
    if (gMezclaTexOn == 1) MezclaReplicarUV(true);
}
void MezclaTexturasOff() {
    gUV1Tipo = 0;   // (la UV propia es de este uso)
    if (gMezclaTexOn == 0) return;
    // (nunca se prendio -un proyecto sin mezcla de texturas- o sin soporte: la unidad 1 ya esta apagada, cuesta cero)
    if (!gMezclaAlgunaVez || !MezclaTexturasSoportada()) { gMezclaTexOn = 0; return; }
    // (tambien desde "desconocido" -tras Invalidate-: una vez, para dejar la unidad 1 como el motor la supone)
    W3D_GLC(GLMT_ACTIVE(GL_TEXTURE1));
    W3D_GLC(glDisable(GL_TEXTURE_2D));
    W3D_GLC(GLMT_ACTIVE(GL_TEXTURE0));
    MezclaUnidad1Array(false);   // (el array si: apagada la unidad, un puntero a RAM que se libero no se lee)
    g_statStateChanges += 1;
    // el env del combiner, la textura bindeada y el puntero de UV QUEDAN en la unidad 1 (nadie mas la toca): volver a
    // prenderla con lo mismo -un mundo que apaga el lightmap entre pasadas- son 6 llamadas, no 30. Desde "desconocido"
    // no hay nada que recordar.
    if (gMezclaTexOn < 0) { gMezclaTexId = 0xFFFFFFFFu; gMezclaTexK = -1.0f; gMezclaTexModo = -1; gUV1PuestoTipo = -1; }
    gMezclaTexOn = 0;
}
// GL desbindea una textura borrada de TODAS las unidades (y recicla el id): el cache de la unidad 1 lo tiene que saber
void MezclaTexturaBorrada(unsigned int id) {
    if (id && gMezclaTexId == id) gMezclaTexId = 0xFFFFFFFFu;
}
// el ultimo error de GL (0 = ninguno): vacia la cola de errores. Quien sube buffers grandes (MundoBsp) pregunta despues
// de cada glBufferData: en el N95 uno que no entra falla en silencio y el VBO queda vacio
unsigned int ErrorGL() { return (unsigned int)glGetError(); }
void MezclaTexturas(unsigned int idB, float k, bool filtrado, bool repetir, int modo) {
    if (!idB || !MezclaTexturasSoportada()) { MezclaTexturasOff(); return; }
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    if (modo < 0 || modo > 4) modo = 0;
    if (gMezclaTexOn < 0) { gMezclaTexId = 0xFFFFFFFFu; gMezclaTexK = -1.0f; gMezclaTexModo = -1; gUV1PuestoTipo = -1; }   // (desconocido)
    const bool prender = gMezclaTexOn != 1;
    const bool bind    = gMezclaTexId != idB;
    const bool envModo = gMezclaTexModo != modo;
    const bool env     = envModo || (modo == 0 && gMezclaTexK != k);
    // los parametros son DEL OBJETO textura (la misma firma por textura que TexFilter / TexWrap de la unidad 0, asi las dos
    // unidades no se pisan); el min-filter con la piramide si la textura la subio (un lightmap lejos, sin moire)
    const bool conMips = MipmapsGlobal() && TexTieneMips(idB);
    const bool filtro  = TexFiltroCambia(idB, (filtrado ? 1 : 0) | (conMips ? 2 : 0));
    const bool wrap    = TexWrapCambia(idB, repetir ? 1 : 0);
    gMezclaTexFiltro = filtrado ? 1 : 0; gMezclaTexRepetir = repetir ? 1 : 0;   // (lo pedido: la foto del constructor lo repone)
    if (!prender && !bind && !env && !filtro && !wrap) return;
    gMezclaAlgunaVez = true;
    W3D_GLC(GLMT_ACTIVE(GL_TEXTURE1));
    if (prender) {
        W3D_GLC(glEnable(GL_TEXTURE_2D));
        g_statStateChanges++;
        gMezclaTexOn = 1;
    }
    if (envModo) {
        if (modo == 0) {
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,    GL_INTERPOLATE));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB,       GL_TEXTURE));      // arg0 = B
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB,   GL_SRC_COLOR));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB,       GL_PREVIOUS));     // arg1 = lo que salio de la unidad 0
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB,   GL_SRC_COLOR));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB,       GL_CONSTANT));     // arg2 = k (el alfa del color constante)
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB,   GL_SRC_ALPHA));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,  GL_REPLACE));      // el alfa: el de la unidad 0
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA,     GL_PREVIOUS));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA));
            W3D_GLC(glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE,      1.0f));
            g_statStateChanges += 12;
        } else if (modo == 3) {
            // SUBTRACT: unidad0 - B (GL ES 1.1 lo tiene en el combiner). El sombreado toon de la rampa: B es cuanto se
            // oscurece cada punto segun su (normal . luz); el alfa sigue siendo el de la unidad 0
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,    GL_SUBTRACT));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB,       GL_PREVIOUS));     // arg0 = lo que salio de la unidad 0
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB,   GL_SRC_COLOR));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB,       GL_TEXTURE));      // arg1 = B (la rampa)
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB,   GL_SRC_COLOR));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,  GL_REPLACE));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA,     GL_PREVIOUS));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA));
            W3D_GLC(glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE,      1.0f));
            g_statStateChanges += 10;
        } else if (modo == 4) {
            // MULTIPLY SIMPLE: el GL_MODULATE de siempre (unidad0 * B; el alfa por el de B), sin el combiner: el camino que
            // todo driver GL ES 1.1 tiene (el del MBX del N95 incluido). El toon "multiplicar" (la rampa invertida)
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE));
            g_statStateChanges += 1;
        } else {
            // MULTIPLY: unidad0 * B (x2 con GL_RGB_SCALE, que solo vale con el env en COMBINE): un lightmap en la misma pasada
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,    GL_MODULATE));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB,       GL_TEXTURE));      // arg0 = B (el lightmap)
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB,   GL_SRC_COLOR));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB,       GL_PREVIOUS));     // arg1 = lo que salio de la unidad 0
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB,   GL_SRC_COLOR));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,  GL_REPLACE));      // el alfa: el de la unidad 0
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA,     GL_PREVIOUS));
            W3D_GLC(glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA));
            W3D_GLC(glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE,      modo == 2 ? 2.0f : 1.0f));
            g_statStateChanges += 10;
        }
        gMezclaTexModo = modo; gMezclaTexK = -1.0f;
    }
    if (bind) { W3D_GLC(glBindTexture(GL_TEXTURE_2D, (GLuint)idB)); g_statTexBinds++; gMezclaTexId = idB; }
    if (filtro) {
        const GLint f = filtrado ? GL_LINEAR : GL_NEAREST;
        const GLint fmin = conMips ? (filtrado ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR) : f;
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fmin));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f));
        g_statStateChanges += 2;
    }
    if (wrap) {
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repetir ? GL_REPEAT : GL_CLAMP_TO_EDGE));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repetir ? GL_REPEAT : GL_CLAMP_TO_EDGE));
        g_statStateChanges += 2;
    }
    if (modo == 0 && gMezclaTexK != k) {
        const GLfloat c[4] = { 0.0f, 0.0f, 0.0f, k };
        W3D_GLC(glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c));
        g_statStateChanges++;
        gMezclaTexK = k;
    }
    W3D_GLC(GLMT_ACTIVE(GL_TEXTURE0));
    if (prender) {
        // el array de UV de la unidad 1 sigue al de la 0 (desconocido = prendido: toda textura va con UV) y apunta a
        // lo mismo (si ya apuntaba ahi, no se manda)
        MezclaUnidad1Array(gArrOn[TexCoordArray] != 0);
        MezclaReplicarUV(true);
    }
}
void VertexPointer2f(int strideBytes, const float* p)   { if (PtrIgual(VertexArray, 0, p, 2, GL_FLOAT, strideBytes)) return; SinVBO(); g_statOtras++; W3D_GLC(glVertexPointer(2, GL_FLOAT, strideBytes, p)); }
void VertexPointer3f(int strideBytes, const float* p)   { if (PtrIgual(VertexArray, 0, p, 3, GL_FLOAT, strideBytes)) return; SinVBO(); g_statOtras++; W3D_GLC(glVertexPointer(3, GL_FLOAT, strideBytes, p)); }
void VertexPointer3s(int strideBytes, const short* p)   { if (PtrIgual(VertexArray, 0, p, 3, GL_SHORT, strideBytes)) return; SinVBO(); W3D_GLC(glVertexPointer(3, GL_SHORT, strideBytes, p)); }
void VertexPointer2s(int strideBytes, const short* p)   { if (PtrIgual(VertexArray, 0, p, 2, GL_SHORT, strideBytes)) return; SinVBO(); W3D_GLC(glVertexPointer(2, GL_SHORT, strideBytes, p)); }
void ColorPointer4ub(const unsigned char* p)            { if (PtrIgual(ColorArray, 0, p, 4, GL_UNSIGNED_BYTE, 0)) return; SinVBO(); g_statOtras++; W3D_GLC(glColorPointer(4, GL_UNSIGNED_BYTE, 0, p)); }
void NormalPointer3b(const signed char* p)              { if (PtrIgual(NormalArray, 0, p, 3, GL_BYTE, 0)) return; SinVBO(); g_statOtras++; W3D_GLC(glNormalPointer(GL_BYTE, 0, p)); }
void ColorPointer4f(int strideBytes, const float* p)    { if (PtrIgual(ColorArray, 0, p, 4, GL_FLOAT, strideBytes)) return; SinVBO(); g_statOtras++; W3D_GLC(glColorPointer(4, GL_FLOAT, strideBytes, p)); }
void TexCoordPointer2f(int strideBytes, const float* p) {
    const bool igual = PtrIgual(TexCoordArray, 0, p, 2, GL_FLOAT, strideBytes);
    if (!igual) { SinVBO(); g_statOtras++; W3D_GLC(glTexCoordPointer(2, GL_FLOAT, strideBytes, p)); }
    gUV0Tipo = 1; gUV0Ptr = p; gUV0Stride = strideBytes;
    // (la unidad 1 apunta a lo mismo, salvo UV propia. Sin el puntero de recien, el ARRAY_BUFFER bindeado puede ser
    // otro: que lo bindee ella)
    if (gMezclaTexOn == 1 && !gUV1Tipo) MezclaReplicarUV(igual);
}
// normales como texcoords (matcap HW). count = cantidad de vertices (para poder convertir el tipo en desktop).
void TexCoordPointer3b(const signed char* p, int count) {
    gUV0Tipo = 0;   // (no se replica en la unidad 1: la mezcla de texturas no va con el matcap)
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    (void)count;
    if (PtrIgual(TexCoordArray, 0, p, 3, GL_BYTE, 0)) return;
    SinVBO();
    W3D_GLC(glTexCoordPointer(3, GL_BYTE, 0, p)); // GLES1 (N95/Android): GL_BYTE ES un tipo valido para texcoords
#else
    // OpenGL de ESCRITORIO: glTexCoordPointer NO acepta GL_BYTE -> daba GL_INVALID_ENUM y la llamada se ignoraba,
    // asi que el matcap por matriz de textura no tenia sus texcoords -> la textura salia PLANA y no seguia la camara.
    // Se convierte a GL_SHORT (si valido en desktop). El factor s=0.5/127 de TexMatrixMatcap sigue igual: GL_SHORT
    // NO se normaliza (127 -> 127.0). Buffer estatico que crece = sin alloc por-frame tras el warmup.
    static GLshort* tmp = 0; static int cap = 0;
    int n = count * 3;
    if (n > cap) { delete[] tmp; tmp = new GLshort[n]; cap = n; }
    for (int i = 0; i < n; i++) tmp[i] = (GLshort)p[i];
    // (el mismo buffer en cada llamada con otros datos: GL los lee al dibujar, asi que el puntero igual sirve. El
    // matcap de prueba_menos_llamadas.w3s da la misma imagen con el cache y sin el)
    const GLshort* q = n > 0 ? tmp : (const GLshort*)0;
    if (PtrIgual(TexCoordArray, 0, q, 3, GL_SHORT, 0)) return;
    SinVBO();
    W3D_GLC(glTexCoordPointer(3, GL_SHORT, 0, q));
#endif
}

// ---- BUFFER OBJECTS (VBO/IBO): subir 1 vez, dibujar desde GPU (ver w3dGraphics.h) ----
bool VBOSoportado(){ w3d_CargarVBO(); return w3d_vboOk; }
unsigned int GenBuffer(){ w3d_CargarVBO(); if (!w3d_vboOk) return 0; GLuint b = 0; W3D_GLC(GLB_GEN(1, &b)); return (unsigned int)b; }
// la MEMORIA de los VBO vivos: id -> bytes del ultimo glBufferData (el N95 la comparte con las texturas y un glBufferData
// que no entra falla en silencio: el log de las rutinas dice cuanto habia subido hasta ahi)
static std::map<unsigned int, long> g_vboBytes;
long VBOMemoria(){
    long t = 0;
    for (std::map<unsigned int, long>::const_iterator it = g_vboBytes.begin(); it != g_vboBytes.end(); ++it) t += it->second;
    return t;
}
// borrar un buffer lo desbindea de todo (GL lo pone en 0 y puede reciclar el numero): los punteros que lo usaban, el IBO
// y el UV de la unidad 1 dejan de saberse
void DeleteBuffer(unsigned int h){
    if (h && w3d_vboOk){
        g_vboBytes.erase(h);
        GLuint b = (GLuint)h; W3D_GLC(GLB_DEL(1, &b));
        if (gArrBuf == h) gArrBuf = 0;
        if (gIdxBuf == h) gIdxBuf = 0;
        for (int a = 0; a < 4; a++) if (gPtrOk[a] && gPtr[a].buf == h) gPtrOk[a] = false;
        if (gUV1PuestoTipo == 2 && gUV1PuestoVbo == h) gUV1PuestoTipo = -1;
    }
}
void ArrayBufferData(unsigned int h, const void* data, int bytes){
    if (!w3d_vboOk || !h) return; g_statUploadBytes += bytes; g_vboBytes[h] = bytes; BindArrBuf(h); W3D_GLC(GLB_DATA(GL_ARRAY_BUFFER, (ptrdiff_t)bytes, data, GL_STATIC_DRAW)); }
void IndexBufferData(unsigned int h, const void* data, int bytes){
    if (!w3d_vboOk || !h) return; g_statUploadBytes += bytes; g_vboBytes[h] = bytes; gIdxBuf = h; W3D_GLC(GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, (GLuint)h)); W3D_GLC(GLB_DATA(GL_ELEMENT_ARRAY_BUFFER, (ptrdiff_t)bytes, data, GL_STATIC_DRAW)); }
void VertexVBO(unsigned int h){ if (PtrIgual(VertexArray, h, 0, 3, GL_FLOAT, 0)) return; g_statOtras += 2; BindArrBuf(h); W3D_GLC(glVertexPointer(3, GL_FLOAT, 0, (const GLvoid*)0)); }
void NormalVBO(unsigned int h){ if (PtrIgual(NormalArray, h, 0, 3, GL_BYTE, 0)) return; g_statOtras += 2; BindArrBuf(h); W3D_GLC(glNormalPointer(GL_BYTE, 0, (const GLvoid*)0)); }
void ColorVBO(unsigned int h){ if (PtrIgual(ColorArray, h, 0, 4, GL_UNSIGNED_BYTE, 0)) return; g_statOtras += 2; BindArrBuf(h); W3D_GLC(glColorPointer(4, GL_UNSIGNED_BYTE, 0, (const GLvoid*)0)); }
void TexCoordVBO(unsigned int h){
    const bool igual = PtrIgual(TexCoordArray, h, 0, 2, GL_FLOAT, 0);
    if (!igual) { g_statOtras += 2; BindArrBuf(h); W3D_GLC(glTexCoordPointer(2, GL_FLOAT, 0, (const GLvoid*)0)); }
    gUV0Tipo = 2; gUV0Vbo = h;
    if (gMezclaTexOn == 1 && !gUV1Tipo) MezclaReplicarUV(igual);
}
void BindIndexVBO(unsigned int h){
    if (g_punterosCache && gIdxBuf == h) return;
    gIdxBuf = h; g_statOtras++; W3D_GLC(GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, (GLuint)h));
}
void DrawTrianglesVBO(int count, int firstIndex){
    W3D_LECT(LectTriangulos, 0, 0, 0, 0);   // (sin VBOs en la Dreamcast: no llega)
    MatricesSincronizar();
    StatDrawTri(count); g_statDrawVBO++;
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    W3D_GLC(glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, (const GLvoid*)(size_t)(firstIndex * 2))); // 16-bit
#else
    W3D_GLC(glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, (const GLvoid*)(size_t)(firstIndex * 4)));   // 32-bit
#endif
}
// (con el cache de punteros no hace nada: el 0 lo bindea el proximo que lo necesita, SinVBO / SinIBO; una malla seguida
// de otra con VBO se ahorra los dos binds)
void UnbindVBOs(){
    if (!w3d_vboOk || g_punterosCache) return;
    g_statOtras += 2; BindArrBuf(0); W3D_GLC(GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, 0)); gIdxBuf = 0;
}
// lo previo a TODO draw: la modelview perezosa en GL y, si los indices vienen de RAM, el ELEMENT_ARRAY en 0
static inline void AntesDeDibujar(bool indicesRAM) { MatricesSincronizar(); if (indicesRAM) SinIBO(); }

// (P3) indices de CLIENTE sobre atributos en VBO (ver w3dGraphics.h): desbindea el
// ELEMENT_ARRAY (los indices se leen de RAM en el draw) y cuenta esos bytes como
// "subidos" (son lo unico que cruza el bus por frame con la visibilidad activa).
void DrawTrianglesClientIdx(int count, const MeshIndex* indices) {
    W3D_LECT(LectTriangulos, 0, count, indices, (int)sizeof(MeshIndex));
    MatricesSincronizar();
    if (g_punterosCache) SinIBO();
    else if (w3d_vboOk) { W3D_GLC(GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, 0)); gIdxBuf = 0; }
    g_statUploadBytes += count * (int)sizeof(MeshIndex);
    StatDrawTri(count); g_statDrawVBO++;   // los ATRIBUTOS salen del VBO (solo los indices viajan)
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    W3D_GLC(glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, indices));
#else
    W3D_GLC(glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, indices));
#endif
}

// LINEAS con indices de CLIENTE sobre los atributos que esten bindeados (VBO o
// client arrays): el gemelo de DrawTrianglesClientIdx para las aristas de un
// material con `lineas` (el mismo camino sirve en los dos modos de dibujo).
void DrawLinesClientIdx(int count, const MeshIndex* indices) {
    W3D_LECT(LectLineas, 0, count, indices, (int)sizeof(MeshIndex));
    MatricesSincronizar();
    if (g_punterosCache) SinIBO();
    else if (w3d_vboOk) { W3D_GLC(GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, 0)); gIdxBuf = 0; }
    PostDrawColor();
    g_statUploadBytes += count * (int)sizeof(MeshIndex);
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    W3D_GLC(glDrawElements(GL_LINES, count, GL_UNSIGNED_SHORT, indices));
#else
    W3D_GLC(glDrawElements(GL_LINES, count, GL_UNSIGNED_INT, indices));
#endif
}

#ifdef W3D_DREAMCAST
// EL CAMINO RAPIDO DE GLdc: copia los vertices de una (y los transforma al pasar) solo si las posiciones son de 3
// floats, las UV de 2 floats y el color de 4 floats. La interfaz (texto, rectangulos, iconos, la grilla) manda
// posiciones de 2 floats o de shorts: GLdc las leia por el camino lento, atributo por atributo y vertice por vertice, y
// era lo mas caro del cuadro. Antes de esos draws las posiciones pasan a 3 floats (z = 0 si no hay) en un buffer
// propio, y despues vuelve el puntero de siempre.
static float* gPos3 = 0;
static int gPos3Cap = 0;
static bool PosConviene() {   // (barato: se mira antes de buscar el indice mas alto)
    const PunteroPuesto& v = gPtr[VertexArray];
    if (gArrOn[VertexArray] != 1 || !gPtrOk[VertexArray] || !v.ptr || v.buf) return false;
    if (v.tipo == GL_FLOAT && v.tam == 3) return false;   // (ya va por el camino rapido)
    // el resto de lo prendido tiene que estar en el formato rapido, si no GLdc va por el lento igual
    if (gArrOn[ColorArray] == 1 || gArrOn[NormalArray] == 1) return false;
    return gArrOn[TexCoordArray] != 1 || (gPtrOk[TexCoordArray] && gPtr[TexCoordArray].tipo == GL_FLOAT && gPtr[TexCoordArray].tam == 2);
}
static bool PosA3f(int desde, int hasta) {   // los vertices [desde, hasta) del arreglo puesto
    const PunteroPuesto& v = gPtr[VertexArray];
    if (hasta <= desde) return false;
    if (hasta > gPos3Cap) {
        float* nuevo = (float*)realloc(gPos3, (size_t)hasta * 3 * sizeof(float));
        if (!nuevo) return false;
        gPos3 = nuevo; gPos3Cap = hasta;
    }
    const int bytes = (v.tipo == GL_SHORT) ? 2 : 4;
    const int paso = v.paso ? v.paso : v.tam * bytes;
    for (int i = desde; i < hasta; i++) {
        const unsigned char* b = (const unsigned char*)v.ptr + (size_t)i * paso;
        float* d = gPos3 + (size_t)i * 3;
        if (v.tipo == GL_SHORT) { const short* q = (const short*)b; d[0] = q[0]; d[1] = q[1]; d[2] = v.tam > 2 ? q[2] : 0.0f; }
        else                    { const float* q = (const float*)b; d[0] = q[0]; d[1] = q[1]; d[2] = v.tam > 2 ? q[2] : 0.0f; }
    }
    glVertexPointer(3, GL_FLOAT, 0, gPos3);
    return true;
}
static void PosRestaurar() { const PunteroPuesto& v = gPtr[VertexArray]; glVertexPointer(v.tam, v.tipo, v.paso, v.ptr); }
template <typename T> static int MaxIndice(const T* idx, int n) { int m = -1; for (int i = 0; i < n; i++) if ((int)idx[i] > m) m = (int)idx[i]; return m; }
#define W3D_POS3F_ARR(primero, n) const bool pos3f_ = PosConviene() && PosA3f(primero, (primero) + (n));
#define W3D_POS3F_IDX(idx, n) const bool pos3f_ = PosConviene() && PosA3f(0, MaxIndice(idx, n) + 1);
#define W3D_POS3F_FIN() if (pos3f_) PosRestaurar();
#else
#define W3D_POS3F_ARR(primero, n)
#define W3D_POS3F_IDX(idx, n)
#define W3D_POS3F_FIN()
#endif

// ---- dibujo de triangulos indexados ----
void DrawTriangles(int count, const MeshIndex* indices) {
    W3D_LECT(LectTriangulos, 0, count, indices, (int)sizeof(MeshIndex));
    AntesDeDibujar(true);
    StatDrawTri(count);
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    W3D_GLC(glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, indices)); // GLES1.1: solo 16 bits
#else
    W3D_POS3F_IDX(indices, count);
    W3D_GLC(glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, indices));   // PC/WebGL/N8: 32 bits
    W3D_POS3F_FIN();
#endif
}
// ---- primitivas genericas (la Rutina) ----
static GLenum PrimGL(Primitiva p) {
    switch (p) {
        case PrimPuntos:         return GL_POINTS;
        case PrimLineas:         return GL_LINES;
        case PrimTiraLineas:     return GL_LINE_STRIP;
        case PrimLazoLineas:     return GL_LINE_LOOP;
        case PrimTiraTriangulos: return GL_TRIANGLE_STRIP;
        case PrimAbanico:        return GL_TRIANGLE_FAN;
        default:                 return GL_TRIANGLES;
    }
}
// un draw de cualquier primitiva: cuenta el draw y los triangulos que de verdad dibuja (una tira de n indices son n-2;
// lineas y puntos, ninguno: antes inflaban los triangulos de las rutinas)
static inline void StatDrawPrim(Primitiva p, int count) {
    const int eq = (p == PrimTriangulos) ? count :
                   (p == PrimTiraTriangulos || p == PrimAbanico) ? (count > 2 ? (count - 2) * 3 : 0) : 0;
    StatDrawTri(eq);
}
void DrawElements16(Primitiva p, int count, const unsigned short* indices) {
    AntesDeDibujar(true);
    StatDrawPrim(p, count);
    W3D_GLC(glDrawElements(PrimGL(p), count, GL_UNSIGNED_SHORT, indices));
}
void DrawElements(Primitiva p, int count, const MeshIndex* indices) {
    W3D_LECT(p == PrimTriangulos ? LectTriangulos : p == PrimLineas ? LectLineas : p == PrimTiraLineas ? LectTiraLineas : LectPuntos,
             0, (p == PrimTriangulos || p == PrimLineas || p == PrimTiraLineas || p == PrimPuntos) ? count : 0, indices, (int)sizeof(MeshIndex));
    AntesDeDibujar(true);
    StatDrawPrim(p, count);
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    W3D_GLC(glDrawElements(PrimGL(p), count, GL_UNSIGNED_SHORT, indices));
#else
    W3D_POS3F_IDX(indices, count);
    W3D_GLC(glDrawElements(PrimGL(p), count, GL_UNSIGNED_INT, indices));
    W3D_POS3F_FIN();
#endif
}
void DrawElementsVBO(Primitiva p, int count, int firstIndex) {
    W3D_LECT(LectTriangulos, 0, 0, 0, 0);
    AntesDeDibujar(false);
    StatDrawPrim(p, count); g_statDrawVBO++;
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    W3D_GLC(glDrawElements(PrimGL(p), count, GL_UNSIGNED_SHORT, (const GLvoid*)(size_t)(firstIndex * 2)));
#else
    W3D_GLC(glDrawElements(PrimGL(p), count, GL_UNSIGNED_INT, (const GLvoid*)(size_t)(firstIndex * 4)));
#endif
}

// triangulos NO indexados (glDrawArrays): la UI 2D arma sus verts en orden
void DrawTrianglesArray(int vertexCount) {
    W3D_LECT(LectTriangulos, 0, vertexCount, 0, 0);
    AntesDeDibujar(false);
    StatDrawTri(vertexCount);
    W3D_POS3F_ARR(0, vertexCount);
    W3D_GLC(glDrawArrays(GL_TRIANGLES, 0, vertexCount));
    W3D_POS3F_FIN();
}
void DrawTrianglesArrayFrom(int first, int count) { // glDrawArrays con offset (chrome equirect por-corner)
    W3D_LECT(LectTriangulos, first, count, 0, 0);
    AntesDeDibujar(false);
    StatDrawTri(count);
    W3D_POS3F_ARR(first, count);
    W3D_GLC(glDrawArrays(GL_TRIANGLES, first, count));
    W3D_POS3F_FIN();
}
void DrawTrianglesByte(int count, const unsigned char* indices) {
    W3D_LECT(LectTriangulos, 0, count, indices, 1);
    AntesDeDibujar(true);
    StatDrawTri(count);
    W3D_POS3F_IDX(indices, count);
    W3D_GLC(glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_BYTE, indices));
    W3D_POS3F_FIN();
}

// lineas sueltas (overlay de normales): dibuja vertexCount vertices de a pares
void DrawLines(int vertexCount) {
    W3D_LECT(LectLineas, 0, vertexCount, 0, 0);
    AntesDeDibujar(false);
    PostDrawColor();
    W3D_POS3F_ARR(0, vertexCount);
    W3D_GLC(glDrawArrays(GL_LINES, 0, vertexCount));
    W3D_POS3F_FIN();
}
void DrawLineStrip(int vertexCount) {
    W3D_LECT(LectTiraLineas, 0, vertexCount, 0, 0);
    AntesDeDibujar(false);
    PostDrawColor();
    W3D_POS3F_ARR(0, vertexCount);
    W3D_GLC(glDrawArrays(GL_LINE_STRIP, 0, vertexCount));
    W3D_POS3F_FIN();
}
void DrawLineStripIndexed(int count, const unsigned short* indices) {
    W3D_LECT(LectTiraLineas, 0, count, indices, 2);
    AntesDeDibujar(true);
    PostDrawColor();
    W3D_GLC(glDrawElements(GL_LINE_STRIP, count, GL_UNSIGNED_SHORT, indices));
}

// puntos (vertices en Edit Mode). glDrawArrays(GL_POINTS) anda en la fase 3D
// (no es la fase 2D rota del N95, y no usa glDrawElements)
#ifdef W3D_DREAMCAST
// PUNTOS EN LA DREAMCAST: GLdc arma el cuadrado del punto antes de la division de perspectiva (el tamanio no queda en
// pixeles) y sin la textura entera encima. Aca cada punto es un cuadrado de 'tamanio' pixeles centrado donde cae en
// pantalla, dibujado con el modo inmediato de GLdc (que guarda y repone los arreglos): con la textura de 0 a 1 si es un
// sprite (iconos de luz, origen, cursor 3D), liso si no (los vertices de Edit Mode).
// El cuadrado va en coordenadas de CAMARA (modelview en identidad, la proyeccion de siempre), con el lado pasado de
// pixeles a metros a la distancia del punto: asi GLdc le da la misma profundidad que al resto de la escena (la PVR
// compara 1/w; con las dos matrices en identidad w valia 1 y la profundidad no tenia que ver con la de las caras).
// Va apenas hacia la camara, como las lineas del parche de GLdc: un vertice no pelea con sus caras.
// En dos triangulos y no en un GL_QUADS: con el scissor prendido (el viewport 3D) GLdc recorta los quads con el orden
// de vertices equivocado y no dibuja nada (los vertices de Edit Mode no se veian; los iconos van sin scissor).
static float gPuntoTam = 1.0f;
static bool  gPuntoSprite = false;
static void DibujarPuntosDC(int n) {
    const PunteroPuesto& v = gPtr[VertexArray];
    if (n <= 0 || !gPtrOk[VertexArray] || !v.ptr) return;
    float P[16], M[16];
    GetMatrix(Projection, P); GetMatrix(ModelView, M);
    if (P[0] == 0.0f || P[5] == 0.0f) return;
    const bool orto = (P[11] == 0.0f);   // (la perspectiva tiene -1 ahi: w = -z de camara)
    // medio lado en NDC -> en metros de camara: dx = hx * w / P[0] (w = 1 en ortografica, -z en perspectiva)
    const float hx = gPuntoTam / (float)(gViewportPuesto[2] > 0 ? gViewportPuesto[2] : 640) / P[0];
    const float hy = gPuntoTam / (float)(gViewportPuesto[3] > 0 ? gViewportPuesto[3] : 480) / P[5];
    const PunteroPuesto& col = gPtr[ColorArray];
    const bool conColor = gArrOn[ColorArray] == 1 && gPtrOk[ColorArray] && col.ptr;
    const int bytes = (v.tipo == GL_SHORT) ? 2 : 4;
    const int paso = v.paso ? v.paso : v.tam * bytes;
    const int modo = gMatMode;
    const bool cull = gCapKnown[CullFace] && gCapOn[CullFace];
    if (cull) glDisable(GL_CULL_FACE);   // (el cuadrado va de frente, pero que el culling no lo decida)
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < n; i++) {
        const unsigned char* b = (const unsigned char*)v.ptr + (size_t)i * paso;
        float x, y, z = 0.0f;
        if (v.tipo == GL_SHORT) { const short* q = (const short*)b; x = q[0]; y = q[1]; if (v.tam > 2) z = q[2]; }
        else                    { const float* q = (const float*)b; x = q[0]; y = q[1]; if (v.tam > 2) z = q[2]; }
        float ex = M[0] * x + M[4] * y + M[8]  * z + M[12];
        float ey = M[1] * x + M[5] * y + M[9]  * z + M[13];
        float ez = M[2] * x + M[6] * y + M[10] * z + M[14];
        float w = 1.0f;
        if (!orto) {
            if (ez >= 0.0f) continue;   // detras de la camara
            ex *= 0.996f; ey *= 0.996f; ez *= 0.996f;   // (hacia la camara por el mismo rayo: no se mueve en pantalla)
            w = -ez;
        } else {
            ez += 0.001f * (P[10] != 0.0f ? 2.0f / fabsf(P[10]) : 1.0f);   // (un milesimo del rango de z, hacia adelante)
        }
        const float dx = hx * w, dy = hy * w;
        if (conColor) { const unsigned char* k = (const unsigned char*)col.ptr + (size_t)i * 4; glColor4ub(k[0], k[1], k[2], k[3]); }
        else if (gColKnown) glColor4f(gColCache[0], gColCache[1], gColCache[2], gColCache[3]);
        // (antihorario) arriba-izq, abajo-izq, abajo-der | arriba-izq, abajo-der, arriba-der
        glTexCoord2f(0.0f, 0.0f); glVertex3f(ex - dx, ey + dy, ez);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(ex - dx, ey - dy, ez);
        glTexCoord2f(1.0f, 1.0f); glVertex3f(ex + dx, ey - dy, ez);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(ex - dx, ey + dy, ez);
        glTexCoord2f(1.0f, 1.0f); glVertex3f(ex + dx, ey - dy, ez);
        glTexCoord2f(1.0f, 0.0f); glVertex3f(ex + dx, ey + dy, ez);
    }
    glEnd();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    if (cull) glEnable(GL_CULL_FACE);
    if (modo == (int)Projection) glMatrixMode(GL_PROJECTION);
    else if (modo == (int)TextureMatrix) glMatrixMode(GL_TEXTURE);
    else gMatMode = (int)ModelView;
    (void)gPuntoSprite;
}
#endif
void DrawPoints(int vertexCount) {
    W3D_LECT(LectPuntos, 0, vertexCount, 0, 0);
    AntesDeDibujar(false);
    PostDrawColor();
#ifdef W3D_DREAMCAST
    DibujarPuntosDC(vertexCount);
#else
    W3D_GLC(glDrawArrays(GL_POINTS, 0, vertexCount));
#endif
}
#ifdef W3D_DREAMCAST
void PointSize(float px) { gPuntoTam = px; }
#else
void PointSize(float px) { W3D_GLC(glPointSize(px)); }
#endif

// lineas INDEXADAS (bordes de edit mode con vertex color compartido): en la fase
// 3D glDrawElements funciona (la malla rellena ya lo usa para los triangulos).
void DrawLinesIndexed(int count, const unsigned short* indices) {
    W3D_LECT(LectLineas, 0, count, indices, 2);
    AntesDeDibujar(true);
    PostDrawColor();
    W3D_POS3F_IDX(indices, count);
    W3D_GLC(glDrawElements(GL_LINES, count, GL_UNSIGNED_SHORT, indices));
    W3D_POS3F_FIN();
}

// offset de profundidad del relleno (slope-aware). Negativo = lo tira hacia la
// camara; se usa para el contorno: los rellenos tapan las lineas internas.
// Cacheado: AplicarMaterial re-manda (0,0) por cada malla comun.
static float gPolyOffF = -1e9f, gPolyOffU = -1e9f;
void PolygonOffset(float factor, float units) {
    if (gPolyOffF == factor && gPolyOffU == units) return;
    gPolyOffF = factor; gPolyOffU = units;
    W3D_GLC(glPolygonOffset(factor, units));
}

#ifdef W3D_DREAMCAST
// ---------------------------------------------------------------------------------------------------------------
//  LEER PIXELES EN LA DREAMCAST. La PVR arma el cuadro entero recien al cerrarlo (no hay un framebuffer para leer a
//  mitad de cuadro) y el glReadPixels de GLdc es un assert que corta el programa. Lo que el editor lee es la pasada de
//  seleccion: los objetos (o los vertices, aristas y caras) pintados con su ID como color plano. Esa pasada se dibuja
//  por software: ReadPixelsBegin(zona) la prende, los draws que siguen se rasterizan en un buffer chico de esa zona,
//  con su z, en vez de ir a la PVR, y ReadPixelsRGBA devuelve el buffer y la apaga.
//  Hace lo que usa la seleccion: triangulos, lineas con ancho, puntos con tamanio, color plano o por vertice, prueba y
//  escritura de z, recorte contra el plano cercano, culling, color mask y polygon offset.
// ---------------------------------------------------------------------------------------------------------------
#include <string.h>   // memset / memcpy
enum { kLectMax = 64 };
static int gLectX = 0, gLectY = 0, gLectW = 0, gLectH = 0;   // la zona, en pixeles de ventana de GL (abajo-izq)
static unsigned char gLectColor[kLectMax * kLectMax * 4];
static float gLectZ[kLectMax * kLectMax];

void ReadPixelsBegin(int x, int y, int w, int h) {
    if (w > kLectMax) w = kLectMax;
    if (h > kLectMax) h = kLectMax;
    gLectActiva = w > 0 && h > 0;
    gLectX = x; gLectY = y; gLectW = w; gLectH = h;
    memset(gLectColor, 0, sizeof(gLectColor));
    for (int i = 0; i < kLectMax * kLectMax; i++) gLectZ[i] = 1.0f;
}
void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* p) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const int lx = x + i - gLectX, ly = y + j - gLectY;
            unsigned char* d = p + ((size_t)j * w + i) * 4;
            if (gLectActiva && lx >= 0 && ly >= 0 && lx < gLectW && ly < gLectH) memcpy(d, &gLectColor[(ly * gLectW + lx) * 4], 4);
            else d[0] = d[1] = d[2] = d[3] = 0;   // (fuera de la zona, o sin ReadPixelsBegin: negro, el "nada" del pick)
        }
    gLectActiva = false;
}

static inline unsigned char LectByte(float v) { return v <= 0.0f ? 0 : (v >= 1.0f ? 255 : (unsigned char)(v * 255.0f + 0.5f)); }
static void LectLimpiar(int bits) {
    const int n = gLectW * gLectH;
    if (bits & ColorBuffer) {
        unsigned char c[4];
        for (int k = 0; k < 4; k++) c[k] = LectByte(gClearCol[k]);
        for (int i = 0; i < n; i++) memcpy(&gLectColor[i * 4], c, 4);
    }
    if (bits & DepthBuffer) for (int i = 0; i < n; i++) gLectZ[i] = 1.0f;
}

// un pixel de la zona (x, y de ventana) con la prueba de z de GL
static inline void LectPixel(int x, int y, float z, const unsigned char* c) {
    const int lx = x - gLectX, ly = y - gLectY;
    if (lx < 0 || ly < 0 || lx >= gLectW || ly >= gLectH) return;
    const int k = ly * gLectW + lx;
    if (gCapOn[DepthTest]) {
        const float zb = gLectZ[k];
        const bool pasa = gDepthFuncCache == DepthAlways ? true : (gDepthFuncCache == DepthEqual ? z == zb :
                          (gDepthFuncCache == DepthLEqual ? z <= zb : z < zb));
        if (!pasa) return;
        if (gDepthMaskCache != 0) gLectZ[k] = z;
    }
    if (gColorMaskOn) memcpy(&gLectColor[k * 4], c, 4);
}

struct LectVert { float x, y, z, w; unsigned char c[4]; };   // en el espacio de recorte, con su color

static inline int LectIndice(const void* idx, int bytes, int i) {
    if (!idx) return i;
    if (bytes == 1) return ((const unsigned char*)idx)[i];
    if (bytes == 2) return ((const unsigned short*)idx)[i];
    return (int)((const unsigned int*)idx)[i];
}
static void LectLeer(int i, const float* PM, const unsigned char* plano, LectVert& o) {
    const PunteroPuesto& v = gPtr[VertexArray];
    const int bytes = (v.tipo == GL_SHORT) ? 2 : 4;
    const unsigned char* b = (const unsigned char*)v.ptr + (size_t)i * (v.paso ? v.paso : v.tam * bytes);
    float x, y, z = 0.0f;
    if (v.tipo == GL_SHORT) { const short* q = (const short*)b; x = q[0]; y = q[1]; if (v.tam > 2) z = q[2]; }
    else                    { const float* q = (const float*)b; x = q[0]; y = q[1]; if (v.tam > 2) z = q[2]; }
    o.x = PM[0] * x + PM[4] * y + PM[8]  * z + PM[12];
    o.y = PM[1] * x + PM[5] * y + PM[9]  * z + PM[13];
    o.z = PM[2] * x + PM[6] * y + PM[10] * z + PM[14];
    o.w = PM[3] * x + PM[7] * y + PM[11] * z + PM[15];
    const PunteroPuesto& col = gPtr[ColorArray];
    if (gArrOn[ColorArray] == 1 && gPtrOk[ColorArray] && col.ptr && col.tipo == GL_UNSIGNED_BYTE)
        memcpy(o.c, (const unsigned char*)col.ptr + (size_t)i * (col.paso ? col.paso : 4), 4);
    else
        memcpy(o.c, plano, 4);
}
static void LectMezcla(const LectVert& a, const LectVert& b, float t, LectVert& o) {
    LectVert r;
    r.x = a.x + (b.x - a.x) * t; r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t; r.w = a.w + (b.w - a.w) * t;
    memcpy(r.c, a.c, 4);
    o = r;
}
static void LectVentana(const LectVert& v, float* o) {   // -> x, y, z de ventana
    const float iw = 1.0f / v.w;
    o[0] = gViewportPuesto[0] + (v.x * iw + 1.0f) * 0.5f * gViewportPuesto[2];
    o[1] = gViewportPuesto[1] + (v.y * iw + 1.0f) * 0.5f * gViewportPuesto[3];
    o[2] = (v.z * iw + 1.0f) * 0.5f;
}
static inline void LectCaja(float a0, float a1, float b0, float b1, int& x0, int& x1, int& y0, int& y1) {
    x0 = (int)floorf(a0); x1 = (int)ceilf(a1); y0 = (int)floorf(b0); y1 = (int)ceilf(b1);
    if (x0 < gLectX) x0 = gLectX;
    if (y0 < gLectY) y0 = gLectY;
    if (x1 > gLectX + gLectW - 1) x1 = gLectX + gLectW - 1;
    if (y1 > gLectY + gLectH - 1) y1 = gLectY + gLectH - 1;
}

static void LectTriVentana(const float* a, const float* b, const float* c, const unsigned char* col) {
    const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f) return;
    if (gCapOn[CullFace]) {
        const bool frente = gFrenteCCW ? area > 0.0f : area < 0.0f;
        if (frente == gCullFrente) return;   // GL_BACK descarta las de atras; GL_FRONT, las de adelante
    }
    float sesgo = 0.0f;
    if (gCapOn[PolygonOffsetFill]) {
        const float dzx = fabsf(((b[2] - a[2]) * (c[1] - a[1]) - (c[2] - a[2]) * (b[1] - a[1])) / area);
        const float dzy = fabsf(((c[2] - a[2]) * (b[0] - a[0]) - (b[2] - a[2]) * (c[0] - a[0])) / area);
        sesgo = gPolyOffF * (dzx > dzy ? dzx : dzy) + gPolyOffU * (1.0f / 1048576.0f);
    }
    int x0, x1, y0, y1;
    LectCaja(fminf(a[0], fminf(b[0], c[0])), fmaxf(a[0], fmaxf(b[0], c[0])),
             fminf(a[1], fminf(b[1], c[1])), fmaxf(a[1], fmaxf(b[1], c[1])), x0, x1, y0, y1);
    const float inv = 1.0f / area;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float wa = ((c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0])) * inv;
            const float wb = ((a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0])) * inv;
            const float wc = ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) * inv;
            if (wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;
            LectPixel(x, y, wa * a[2] + wb * b[2] + wc * c[2] + sesgo, col);
        }
}
static void LectTriangulo(const LectVert* v) {
    // recorte contra el plano cercano (z + w >= 0): el triangulo queda en 3 o 4 vertices, que se dibujan en abanico
    LectVert pol[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const LectVert& a = v[i];
        const LectVert& b = v[(i + 1) % 3];
        const float da = a.z + a.w, db = b.z + b.w;
        if (da >= 0.0f) pol[n++] = a;
        if ((da >= 0.0f) != (db >= 0.0f)) LectMezcla(a, b, da / (da - db), pol[n++]);
    }
    if (n < 3) return;
    float w[4][3];
    for (int i = 0; i < n; i++) LectVentana(pol[i], w[i]);
    for (int i = 1; i + 1 < n; i++) LectTriVentana(w[0], w[i], w[i + 1], v[0].c);
}
static void LectLinea(LectVert a, LectVert b) {
    const float da = a.z + a.w, db = b.z + b.w;
    if (da < 0.0f && db < 0.0f) return;
    if (da < 0.0f) LectMezcla(a, b, da / (da - db), a);
    else if (db < 0.0f) LectMezcla(b, a, db / (db - da), b);
    float p[3], q[3];
    LectVentana(a, p); LectVentana(b, q);
    const float r = (gLineaAncho > 1.0f ? gLineaAncho : 1.0f) * 0.5f;
    int x0, x1, y0, y1;
    LectCaja(fminf(p[0], q[0]) - r, fmaxf(p[0], q[0]) + r, fminf(p[1], q[1]) - r, fmaxf(p[1], q[1]) + r, x0, x1, y0, y1);
    const float dx = q[0] - p[0], dy = q[1] - p[1], l2 = dx * dx + dy * dy;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            float t = l2 > 0.0f ? ((px - p[0]) * dx + (py - p[1]) * dy) / l2 : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            const float ex = p[0] + dx * t - px, ey = p[1] + dy * t - py;
            if (ex * ex + ey * ey > r * r) continue;
            LectPixel(x, y, p[2] + (q[2] - p[2]) * t, a.c);
        }
}
static void LectPunto(const LectVert& v) {
    if (v.w <= 0.0f || v.z + v.w < 0.0f) return;   // (GL descarta el punto entero si el centro queda afuera)
    float p[3];
    LectVentana(v, p);
    const float h = (gPuntoTam > 1.0f ? gPuntoTam : 1.0f) * 0.5f;
    int x0, x1, y0, y1;
    LectCaja(p[0] - h, p[0] + h, p[1] - h, p[1] + h, x0, x1, y0, y1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            if (px < p[0] - h || px >= p[0] + h || py < p[1] - h || py >= p[1] + h) continue;
            LectPixel(x, y, p[2], v.c);
        }
}
static void LectDibujar(LectPrim prim, int primero, int n, const void* idx, int bytesIdx) {
    if (n <= 0 || !gPtrOk[VertexArray] || !gPtr[VertexArray].ptr) return;
    float P[16], M[16], PM[16];
    GetMatrix(Projection, P); GetMatrix(ModelView, M);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            PM[c * 4 + r] = P[0 * 4 + r] * M[c * 4 + 0] + P[1 * 4 + r] * M[c * 4 + 1] + P[2 * 4 + r] * M[c * 4 + 2] + P[3 * 4 + r] * M[c * 4 + 3];
    unsigned char plano[4] = { 255, 255, 255, 255 };
    if (gColKnown) for (int k = 0; k < 4; k++) plano[k] = LectByte(gColCache[k]);
    LectVert v[3];
    if (prim == LectTriangulos) {
        for (int i = 0; i + 2 < n; i += 3) {
            for (int k = 0; k < 3; k++) LectLeer(LectIndice(idx, bytesIdx, primero + i + k), PM, plano, v[k]);
            LectTriangulo(v);
        }
    } else if (prim == LectLineas || prim == LectTiraLineas) {
        const int paso = (prim == LectLineas) ? 2 : 1;
        for (int i = 0; i + 1 < n; i += paso) {
            LectLeer(LectIndice(idx, bytesIdx, primero + i), PM, plano, v[0]);
            LectLeer(LectIndice(idx, bytesIdx, primero + i + 1), PM, plano, v[1]);
            LectLinea(v[0], v[1]);
        }
    } else {
        for (int i = 0; i < n; i++) {
            LectLeer(LectIndice(idx, bytesIdx, primero + i), PM, plano, v[0]);
            LectPunto(v[0]);
        }
    }
}
#endif

// empuja el rango de profundidad (contorno: las lineas internas quedan tapadas
// por la malla y solo se ve el borde/silueta)
void DepthRange(float n, float f) {
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    W3D_GLC(glDepthRangef(n, f));
#else
    W3D_GLC(glDepthRange(n, f));
#endif
}

// ---- wireframe (solo GL de escritorio tiene glPolygonMode) ----
void Wireframe(bool on) {
#if !defined(W3D_SYMBIAN) && !defined(__ANDROID__)
    W3D_GLC(glPolygonMode(GL_FRONT_AND_BACK, on ? GL_LINE : GL_FILL));
#else
    (void)on; // GL ES: sin polygonmode -> sale relleno (como hoy)
#endif
}

// ---- invalidar el cache tras GL directo ----
// NO consulta GL (glIsEnabled fuerza un FLUSH del GPU en renderers de tiles como
// el del N95: carisimo y se llamaba varias veces por frame -> de 60 a 30 fps).
// Solo marca cada cap como "desconocido": la proxima Enable/Disable por cap fuerza
// la llamada (barata, sin flush) y vuelve a cachear.
void EstadoBase() {
    SesgoMetros(0.0f);
    AlphaTest(0.0f);
    Disable(PolygonOffsetFill);
    TexMatrixMatcap(false); TexGenSphere(false);
    TexEnvReplace(false);
    MezclaTexturasOff();   // (la unidad 1 de un material con mezcla de texturas no le llega a la interfaz)
    SmoothShading(true);
    Disable(Lighting); Disable(ColorMaterial);
}

void Invalidate() {
    for (int i = 0; i < kNumCaps; i++) gCapKnown[i] = false;
    // idem el binding de textura: el proximo BindTexture hace la llamada
    gTexBound = 0xFFFFFFFFu;
    gArrBuf = 0xFFFFFFFFu;   // (y el VBO de atributos)
    // (los punteros y el IBO: un contexto nuevo los tiene en 0 y puede reciclar los numeros de buffer)
    gIdxBuf = 0xFFFFFFFFu;
    for (int a = 0; a < 4; a++) gPtrOk[a] = false;
    gMatMode = -1; gProyBaseOk = false; gSesgoPuesto = 0.0f;   // (y el modo de matriz; la proyeccion es la del pase)
    // (la modelview perezosa: lo que tenga GL no se sabe, la proxima que la use la vuelve a mandar. Con la opcion
    // apagada GL es la verdad, como siempre)
    if (g_matricesPerezosas) gSerieEnGL = 0;
    // y el cache FINO (P1): todo "desconocido" -> la proxima llamada de cada
    // estado va al driver y re-cachea
    gDepthFuncCache = -1; gDepthMaskCache = -1;
    gBlendFuncCache = -1; gBlendEqCache = -1;
    gTexEnvCache = -1; gTexGenCache = -1; gMatcapCache = -1;
    gMezclaTexOn = -1; gMezclaTexId = 0xFFFFFFFFu; gMezclaTexK = -1.0f; gMezclaTexFiltro = gMezclaTexRepetir = -1;   // (la unidad 1)
    gMezclaTexModo = -1; gUV1Tipo = 0; gUV1PuestoTipo = -1;
    gSmoothCache = -1; gAlphaRefCache = -1e9f;
    for (int i = 0; i < 4; i++) gArrOn[i] = -1;
    // material/color actual: tambien a "desconocido".
    // El cache POR TEXTURA (TexFiltroCambia/TexWrapCambia) NO se toca a
    // proposito: glTexParameter es estado del OBJETO textura, el GL directo de
    // un pase ajeno no lo cambia (y quien borra texturas pasa por DeleteTexture,
    // que olvida su entrada).
    for (int i = 0; i < 4; i++) gMatKnown[i] = false;
    for (int i = 0; i < LucesMax; i++) gLuzOn[i] = -1;
    gShinCache = -1e9f;
    gColKnown = false;
    gPolyOffF = gPolyOffU = -1e9f;
}

// ---- FOTO del estado (el constructor de las rutinas en modo solo-juego) ----
struct EstadoFoto {
    bool capOn[kNumCaps], capKnown[kNumCaps];
    unsigned int tex;
    int depthFunc, depthMask, blendFunc, blendEq, texEnv, smooth;
    float alphaRef;
    int mezclaOn; unsigned mezclaId; float mezclaK; int mezclaFiltro, mezclaRepetir, mezclaModo;   // la unidad 1 (mezcla de texturas)
    signed char arr[4];
    float mat[4][4]; bool matKnown[4]; float shin;
    float col[4]; bool colKnown;
    float polyF, polyU;
    bool hay;
};
static EstadoFoto gFoto;
bool EstadoFotoSoportada() { return true; }
void EstadoFotoTomar() {
    for (int i = 0; i < kNumCaps; i++) { gFoto.capOn[i] = gCapOn[i]; gFoto.capKnown[i] = gCapKnown[i]; }
    gFoto.tex = gTexBound;
    gFoto.depthFunc = gDepthFuncCache; gFoto.depthMask = gDepthMaskCache;
    gFoto.blendFunc = gBlendFuncCache; gFoto.blendEq = gBlendEqCache;
    gFoto.texEnv = gTexEnvCache; gFoto.smooth = gSmoothCache; gFoto.alphaRef = gAlphaRefCache;
    gFoto.mezclaOn = gMezclaTexOn; gFoto.mezclaId = gMezclaTexId; gFoto.mezclaK = gMezclaTexK;
    gFoto.mezclaFiltro = gMezclaTexFiltro; gFoto.mezclaRepetir = gMezclaTexRepetir; gFoto.mezclaModo = gMezclaTexModo;
    for (int i = 0; i < 4; i++) {
        gFoto.arr[i] = gArrOn[i]; gFoto.matKnown[i] = gMatKnown[i];
        for (int k = 0; k < 4; k++) gFoto.mat[i][k] = gMatCache[i][k];
        gFoto.col[i] = gColCache[i];
    }
    gFoto.shin = gShinCache; gFoto.colKnown = gColKnown;
    gFoto.polyF = gPolyOffF; gFoto.polyU = gPolyOffU;
    gFoto.hay = true;
}
void EstadoFotoReponer() {
    if (!gFoto.hay) return;
    for (int i = 0; i < kNumCaps; i++) {
        if (!gFoto.capKnown[i]) continue;
        if (gFoto.capOn[i]) Enable((Cap)i); else Disable((Cap)i);
    }
    if (gFoto.tex != 0xFFFFFFFFu) BindTexture(gFoto.tex);
    if (gFoto.depthFunc >= 0) DepthFunc((DepthCmp)gFoto.depthFunc);
    if (gFoto.depthMask >= 0) DepthMask(gFoto.depthMask != 0);
#ifndef W3D_SYMBIAN
    if (gFoto.blendEq >= 0) PonerBlendEq(gFoto.blendEq);
#endif
    if (gFoto.blendFunc >= 0) PonerBlendFunc(gFoto.blendFunc);
    if (gFoto.texEnv == 0) TexEnvReplace(false);
    else if (gFoto.texEnv == 1) TexEnvReplace(true);
    else if (gFoto.texEnv == 2) TexEnvDot3(true);
    else if (gFoto.texEnv == 3) TexEnvAlphaOnly(true);
    if (gFoto.mezclaOn == 1 && gFoto.mezclaId != 0xFFFFFFFFu && gFoto.mezclaModo >= 0)
        MezclaTexturas(gFoto.mezclaId, gFoto.mezclaK < 0.0f ? 0.0f : gFoto.mezclaK, gFoto.mezclaFiltro == 1, gFoto.mezclaRepetir == 1, gFoto.mezclaModo);
    else if (gFoto.mezclaOn == 0) MezclaTexturasOff();
    if (gFoto.smooth >= 0) SmoothShading(gFoto.smooth != 0);
    if (gFoto.alphaRef > -1e8f) AlphaTest(gFoto.alphaRef);
    static const VArray kArr[4] = { VertexArray, TexCoordArray, NormalArray, ColorArray };
    for (int i = 0; i < 4; i++) {
        if (gFoto.arr[i] < 0) continue;
        if (gFoto.arr[i]) EnableArray(kArr[i]); else DisableArray(kArr[i]);
    }
    for (int i = 0; i < 4; i++) if (gFoto.matKnown[i]) Material((MatParam)i, gFoto.mat[i]);
    if (gFoto.shin > -1e8f) MaterialShininess(gFoto.shin);
    if (gFoto.colKnown) Color4f(gFoto.col[0], gFoto.col[1], gFoto.col[2], gFoto.col[3]);
    if (gFoto.polyF > -1e8f) PolygonOffset(gFoto.polyF, gFoto.polyU);
}

// ---------------------------------------------------------------------------
//  AUDITORIA del cache (debug, solo escritorio): compara lo que el motor CREE
//  puesto contra lo que GL tiene de verdad. Un mismatch = algo del pase toco GL
//  crudo sin Invalidate() -> el cache miente y el render puede salir con estado
//  de otro. Devuelve la cantidad de mismatches (0 = coherente) y los loguea.
//  OJO: usa glGet/glIsEnabled (flush en tilers) -> JAMAS en el frame normal de
//  un dispositivo; es una herramienta de PC (harness 'glaudit' / debug).
// ---------------------------------------------------------------------------
bool g_auditarEscena = false;   // la prende el harness ('glaudit')
int  g_auditDesyncs  = 0;
int AuditarEstado() {
#if defined(W3D_SYMBIAN) || defined(__ANDROID__) || defined(W3D_DREAMCAST)
    return 0;   // en dispositivo no se audita (glGet = flush del tiler)
#else
    int malos = 0;
    for (int i = 0; i < kNumCaps; i++) {
        if (!gCapKnown[i]) continue;
        GLenum e = CapGL((Cap)i);
        if (!e) continue;
        bool real = W3D_GLC(glIsEnabled(e)) == GL_TRUE;
        if (real != gCapOn[i]) {
            printf("      [glaudit] DESYNC cap %d: motor=%d GL=%d\n", i, (int)gCapOn[i], (int)real);
            malos++;
        }
    }
    if (gDepthMaskCache >= 0) {
        GLboolean dm = GL_TRUE;
        W3D_GLC(glGetBooleanv(GL_DEPTH_WRITEMASK, &dm));
        if ((dm == GL_TRUE ? 1 : 0) != gDepthMaskCache) {
            printf("      [glaudit] DESYNC depthMask: motor=%d GL=%d\n", gDepthMaskCache, (int)(dm == GL_TRUE));
            malos++;
        }
    }
    if (gTexBound != 0xFFFFFFFFu) {
        GLint t = 0;
        W3D_GLC(glGetIntegerv(GL_TEXTURE_BINDING_2D, &t));
        if ((unsigned)t != gTexBound) {
            printf("      [glaudit] DESYNC textura: motor=%u GL=%d\n", gTexBound, (int)t);
            malos++;
        }
    }
    // la MODELVIEW perezosa: si el motor cree que GL tiene el tope de la sombra (no sucia), tiene que ser ese
    if (SombraTope(0) && gSerieEnGL != 0 && !MVSucia()) {
        GLfloat mv[16];
        W3D_GLC(glGetFloatv(GL_MODELVIEW_MATRIX, mv));
        const float* s = SombraTope(0);
        for (int i = 0; i < 16; i++)
            if (fabsf(s[i] - mv[i]) > 1e-4f * (1.0f + fabsf(mv[i]))) {
                printf("      [glaudit] DESYNC modelview [%d]: motor=%g GL=%g\n", i, s[i], mv[i]);
                malos++; break;
            }
    }
    // los BINDS de buffers y los PUNTEROS de la unidad 0 que el cache da por puestos
    if (w3d_vboOk) {
        GLint b = 0;
        if (gArrBuf != 0xFFFFFFFFu) {
            W3D_GLC(glGetIntegerv(0x8894 /*GL_ARRAY_BUFFER_BINDING*/, &b));
            if ((unsigned)b != gArrBuf) { printf("      [glaudit] DESYNC ARRAY_BUFFER: motor=%u GL=%d\n", gArrBuf, (int)b); malos++; }
        }
        if (gIdxBuf != 0xFFFFFFFFu) {
            W3D_GLC(glGetIntegerv(0x8895 /*GL_ELEMENT_ARRAY_BUFFER_BINDING*/, &b));
            if ((unsigned)b != gIdxBuf) { printf("      [glaudit] DESYNC ELEMENT_ARRAY_BUFFER: motor=%u GL=%d\n", gIdxBuf, (int)b); malos++; }
        }
    }
    // (por VArray: vertices, uv, normales, colores; las normales no tienen tamano)
    static const GLenum kBuf[4]  = { 0x8896, 0x889A, 0x8897, 0x8898 };   // GL_*_ARRAY_BUFFER_BINDING
    static const GLenum kPtr[4]  = { GL_VERTEX_ARRAY_POINTER, GL_TEXTURE_COORD_ARRAY_POINTER, GL_NORMAL_ARRAY_POINTER, GL_COLOR_ARRAY_POINTER };
    static const GLenum kTam[4]  = { GL_VERTEX_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_SIZE, 0, GL_COLOR_ARRAY_SIZE };
    static const GLenum kTipo[4] = { GL_VERTEX_ARRAY_TYPE, GL_TEXTURE_COORD_ARRAY_TYPE, GL_NORMAL_ARRAY_TYPE, GL_COLOR_ARRAY_TYPE };
    static const GLenum kPaso[4] = { GL_VERTEX_ARRAY_STRIDE, GL_TEXTURE_COORD_ARRAY_STRIDE, GL_NORMAL_ARRAY_STRIDE, GL_COLOR_ARRAY_STRIDE };
    for (int a = 0; a < 4; a++) {
        if (!gPtrOk[a]) continue;
        const PunteroPuesto& q = gPtr[a];
        GLint buf = 0, tam = q.tam, tipo = 0, paso = 0; GLvoid* p = 0;
        if (w3d_vboOk) W3D_GLC(glGetIntegerv(kBuf[a], &buf));
        if (kTam[a]) W3D_GLC(glGetIntegerv(kTam[a], &tam));
        W3D_GLC(glGetIntegerv(kTipo[a], &tipo));
        W3D_GLC(glGetIntegerv(kPaso[a], &paso));
        W3D_GLC(glGetPointerv(kPtr[a], &p));
        if ((w3d_vboOk && (unsigned)buf != q.buf) || p != q.ptr || tam != q.tam || (unsigned)tipo != q.tipo || paso != q.paso) {
            printf("      [glaudit] DESYNC puntero del array %d: motor=(vbo %u, %p, %d, 0x%x, %d) GL=(vbo %d, %p, %d, 0x%x, %d)\n",
                   a, q.buf, q.ptr, q.tam, q.tipo, q.paso, (int)buf, p, (int)tam, (unsigned)tipo, (int)paso);
            malos++;
        }
    }
    return malos;
#endif
}

} // namespace w3dEngine

// w3dSetColor: API libre del engine (declarada en w3dBase.h). Vive ACA (no en el backend
// w3dOpenGL, que es solo-PC) para compilar en PC y Symbian. Rutea por la abstraccion.
#include "w3dBase.h"
void w3dSetColor(const ColorType c[4]) {
    w3dEngine::Color4f(c[0], c[1], c[2], c[3]);

}

// estado de render (lo setea el editor desde su RenderType/view antes de dibujar la escena)
bool w3dRenderWireframe = false;
// wireframe SOLO de lo que se dibuja (default): con visibilidad por triangulo
// (Mesh::pvsFaces) las lineas siguen la lista ACTIVA, no la malla entera.
bool w3dRenderWireframeSoloVisible = true;
bool w3dRenderSolido    = false;
bool w3dRenderSinLuz    = false;
bool w3dRenderLuces     = false;

bool w3dRenderNormalColor = false;
bool w3dRenderAlpha       = false; // pase ALPHA (matte): blanco unlit + solo el alpha de la textura
bool w3dRenderOverlays    = true;
bool w3dVerSeleccion      = true;  // "Ver seleccion" (menu Select): OFF = ni contorno verde ni tinte de seleccion
bool g_xray               = false; // X-Ray OFF por defecto (lo togglea el menu Overlays)

