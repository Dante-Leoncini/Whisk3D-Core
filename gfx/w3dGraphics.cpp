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
    static void w3d_CargarVBO(){
        if (w3d_vboLoaded) return; w3d_vboLoaded = true;
        w3d_glGenBuffers    = (W3D_PFNGENBUF) wglGetProcAddress("glGenBuffers");
        w3d_glDeleteBuffers = (W3D_PFNDELBUF) wglGetProcAddress("glDeleteBuffers");
        w3d_glBindBuffer    = (W3D_PFNBINDBUF)wglGetProcAddress("glBindBuffer");
        w3d_glBufferData    = (W3D_PFNBUFDATA)wglGetProcAddress("glBufferData");
        w3d_vboOk = w3d_glGenBuffers && w3d_glDeleteBuffers && w3d_glBindBuffer && w3d_glBufferData;
    }
    #define GLB_GEN    w3d_glGenBuffers
    #define GLB_DEL    w3d_glDeleteBuffers
    #define GLB_BIND   w3d_glBindBuffer
    #define GLB_DATA   w3d_glBufferData
#else
    static bool w3d_vboOk = true;         // GLES 1.1 / ES2: core
    static void w3d_CargarVBO(){}
    #define GLB_GEN    glGenBuffers
    #define GLB_DEL    glDeleteBuffers
    #define GLB_BIND   glBindBuffer
    #define GLB_DATA   glBufferData
#endif

namespace w3dEngine {

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
static int   gMatcapCache    = -1;   // 0/1 (matriz de textura del matcap)
static int   gSmoothCache    = -1;   // 0/1 (shade model)
static float gAlphaRefCache  = -1e9f;
static signed char gArrOn[4]    = { -1, -1, -1, -1 }; // client arrays: -1 desconocido / 0 / 1

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
    glDepthFunc(f);
}

void Enable(Cap c) {
    if (gCapKnown[c] && gCapOn[c]) { return; } // ya prendida y la conocemos -> nada
    gCapOn[c] = true; gCapKnown[c] = true;
    g_statStateChanges++;
    glEnable(CapGL(c));
    // COLOR_MATERIAL: al prenderse, ambient/diffuse del material pasan a seguir
    // el color actual -> lo que el cache creia puesto ya no vale
    if (c == ColorMaterial) { gMatKnown[MatAmbient] = false; gMatKnown[MatDiffuse] = false; }
}

void Disable(Cap c) {
    if (gCapKnown[c] && !gCapOn[c]) { return; } // ya apagada y la conocemos -> nada
    gCapOn[c] = false; gCapKnown[c] = true;
    g_statStateChanges++;
    glDisable(CapGL(c));
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
    g_statStateChanges++; glEnableClientState(ArrayGL(a));
}
void DisableArray(VArray a) {
    if (gArrOn[a] == 0) return;
    gArrOn[a] = 0;
    g_statStateChanges++; glDisableClientState(ArrayGL(a));
}

// ---- buffers ----
void ClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
void Clear(int bits) {
    GLbitfield m = 0;
    if (bits & ColorBuffer)   { m |= GL_COLOR_BUFFER_BIT; }
    if (bits & DepthBuffer)   { m |= GL_DEPTH_BUFFER_BIT; }
    if (bits & StencilBuffer) { m |= GL_STENCIL_BUFFER_BIT; }
    glClear(m);
}

// ---- planos de recorte (user clip planes) ----
// GL de escritorio: glClipPlane toma DOUBLE. GLES 1.1: glClipPlanef (float).
// El tope real lo da el driver (GL_MAX_CLIP_PLANES; el minimo del estandar es 6
// en GL de escritorio y 1 en GLES 1.1), se consulta UNA vez y se cachea.
static int gMaxClip = -1;
static int gClipOn  = 0;   // cuantos hay prendidos ahora (para apagarlos rapido)

int PlanosRecorteMax() {
    if (gMaxClip < 0) {
        GLint n = 0;
        glGetIntegerv(GL_MAX_CLIP_PLANES, &n);
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
    for (int i = 0; i < n; i++) {
#ifdef W3D_SYMBIAN
        GLfloat p[4] = { eq4[i*4+0], eq4[i*4+1], eq4[i*4+2], eq4[i*4+3] };
        glClipPlanef((GLenum)(GL_CLIP_PLANE0 + i), p);
#else
        GLdouble p[4] = { eq4[i*4+0], eq4[i*4+1], eq4[i*4+2], eq4[i*4+3] };
        glClipPlane((GLenum)(GL_CLIP_PLANE0 + i), p);
#endif
        glEnable((GLenum)(GL_CLIP_PLANE0 + i));
    }
    for (int i = n; i < gClipOn; i++) glDisable((GLenum)(GL_CLIP_PLANE0 + i));
    gClipOn = n;
}

// ---- estencil ----
static int gStencilBits = -1;

int EstencilBits() {
    if (gStencilBits < 0) {
        GLint b = 0;
        glGetIntegerv(GL_STENCIL_BITS, &b);
        gStencilBits = (b > 0) ? (int)b : 0;
    }
    return gStencilBits;
}

void EstencilLimpiar(int valor) {
    glClearStencil(valor);
    glStencilMask(0xFFu);      // sin mascara de escritura el glClear no borra nada
    glClear(GL_STENCIL_BUFFER_BIT);
}

void EstencilMarcar(int valor) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFFu);
    glStencilFunc(GL_ALWAYS, valor, 0xFFu);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);   // escribe donde PASA el z-test
}

void EstencilDentro(int valor) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x00u);                        // no tocar el buffer
    glStencilFunc(GL_EQUAL, valor, 0xFFu);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void EstencilApagar() {
    glStencilMask(0xFFu);
    glDisable(GL_STENCIL_TEST);
}

// ---- viewport / recorte ----
void Viewport(int x, int y, int w, int h) { glViewport(x, y, w, h); }
void Scissor(int x, int y, int w, int h)  { glScissor(x, y, w, h); }

// ---- matrices ----
static GLenum MatGL(Matrix m) {
    switch (m) {
        case Projection:    return GL_PROJECTION;
        case ModelView:     return GL_MODELVIEW;
        case TextureMatrix: return GL_TEXTURE;
    }
    return GL_MODELVIEW;
}

void MatrixMode(Matrix m) { glMatrixMode(MatGL(m)); }
void LoadIdentity()       { glLoadIdentity(); }
void PushMatrix()                          { glPushMatrix(); }
void PopMatrix()                           { glPopMatrix(); }
void Translatef(float x, float y, float z) { glTranslatef(x, y, z); }
void Rotatef(float a, float x, float y, float z) { glRotatef(a, x, y, z); }
void Scalef(float x, float y, float z)           { glScalef(x, y, z); }
void MultMatrix(const float* m)                  { glMultMatrixf(m); }
void LoadMatrix(const float* m)                  { glLoadMatrixf(m); }
void Ortho(float l, float r, float b, float t, float n, float f) {
    #if defined(W3D_SYMBIAN) || defined(__ANDROID__)
        glOrthof(l, r, b, t, n, f);
    #else
        glOrtho(l, r, b, t, n, f);
    #endif
}

void Frustum(float l, float r, float b, float t, float n, float f) {
    #if defined(W3D_SYMBIAN) || defined(__ANDROID__)
        glFrustumf(l, r, b, t, n, f);
    #else
        glFrustum(l, r, b, t, n, f);
    #endif
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
    g_statStateChanges++; glDepthMask(escribir ? GL_TRUE : GL_FALSE);
}
void LineWidth(float px)       { glLineWidth(px); }
void SmoothShading(bool suave) {
    if (gSmoothCache == (suave ? 1 : 0)) return;
    gSmoothCache = suave ? 1 : 0;
    g_statStateChanges++; glShadeModel(suave ? GL_SMOOTH : GL_FLAT);
}
void FastPerspective()         { glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST); }

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
int g_statDcCat[StatCatCount_] = { 0, 0, 0 };
static int gStatCat = StatCatEscena;  // categoria del pase que esta dibujando
void StatCategoria(StatCat c) { gStatCat = (int)c; }
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
    PostDrawColor();   // todo draw de triangulos pasa por aca
}
void StatsReset() {
    g_statDrawTris = 0; g_statDrawVBO = 0; g_statIndices = 0; g_statTexBinds = 0; g_statMeshes = 0;
    g_statStateChanges = 0; g_statUploadBytes = 0; g_statTrisOpacos = 0; g_statTrisBlend = 0;
    for (int i = 0; i < StatCatCount_; i++) g_statDcCat[i] = 0;
    gStatCat = StatCatEscena;
}
void Finish() { glFinish(); }

// ---- textura activa (cacheada) ----
void BindTexture(unsigned int id) {
    if (id == gTexBound) { return; } // ya esta bindeada -> ahorramos el bind
    gTexBound = id;
    g_statTexBinds++;
    glBindTexture(GL_TEXTURE_2D, id);
}


// CHROME: genera las UV con sphere-map del pipeline fijo (PC). En GLES1 (Symbian) no hay glTexGen ->
// stub (calcular las UV del chrome por software a partir del normal y la camara).
void TexGenSphere(bool on) {
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    (void)on;
#else
    if (gTexGenCache == (on ? 1 : 0)) return;   // ya esta (la matriz del texgen es fija)
    gTexGenCache = on ? 1 : 0;
    g_statStateChanges++;
    if (on) {
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
        // el sphere-map genera la V al reves de las UV del modelo (que ya hacen 1-v por el stb top-first):
        // flip-V con la matriz de textura para que el reflejo no salga dado vuelta verticalmente. v -> 1-v
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glTranslatef(0.0f, 1.0f, 0.0f);
        glScalef(1.0f, -1.0f, 1.0f);
        glMatrixMode(GL_MODELVIEW);
    } else {
        glDisable(GL_TEXTURE_GEN_S);
        glDisable(GL_TEXTURE_GEN_T);
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity(); // saca el flip-V (que no leakee a las UV normales de la proxima textura)
        glMatrixMode(GL_MODELVIEW);
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
    glMatrixMode(GL_TEXTURE);
    if (on) {
        GLfloat mv[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, mv); // estado de matriz (CPU-side, barato; no es un readback de GPU)
        const GLfloat s = 0.5f / 127.0f;
        GLfloat T[16];
        // column-major. fila0 -> u = s*(MV*N).x + 0.5 ; fila1 -> v = 0.5 - s*(MV*N).y (flip-V)
        T[0] = s*mv[0];  T[1] = -s*mv[1]; T[2] = 0.0f; T[3] = 0.0f;
        T[4] = s*mv[4];  T[5] = -s*mv[5]; T[6] = 0.0f; T[7] = 0.0f;
        T[8] = s*mv[8];  T[9] = -s*mv[9]; T[10]= 0.0f; T[11]= 0.0f;
        T[12]= 0.5f;     T[13]= 0.5f;     T[14]= 0.0f; T[15]= 1.0f;
        glLoadMatrixf(T);
    } else {
        glLoadIdentity(); // resetea (que no leakee a las UV de la proxima textura)
    }
    glMatrixMode(GL_MODELVIEW);
}

// true si el backend tiene glTexGen (sphere-map por HARDWARE). PC = si; GLES1 del N95 = NO -> el matcap se
// calcula por SOFTWARE (sphere-map por vertice, como el equirect). Lo usa el render del chrome para elegir.
bool TieneTexGen() {
#ifdef W3D_SYMBIAN
    return false;
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
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, on ? GL_REPLACE : GL_MODULATE);
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
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,   GL_DOT3_RGB);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB,      GL_TEXTURE);       // arg0 = normal del mapa
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB,  GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB,      GL_PRIMARY_COLOR); // arg1 = L (color del vertice)
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB,  GL_SRC_COLOR);
    } else {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
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
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,    GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB,       GL_PRIMARY_COLOR); // RGB = color plano
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB,   GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,  GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA,     GL_TEXTURE);       // ALPHA = alpha de la textura
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
    } else {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }
}

// ---- color uniforme ----
void FrontFace(bool ccw) { glFrontFace(ccw ? GL_CCW : GL_CW); }
void ColorMask(bool r, bool g, bool b, bool a) { glColorMask(r, g, b, a); }
void PointSpriteCoordReplace(bool on) { glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, on ? GL_TRUE : GL_FALSE); }

void Color4f(float r, float g, float b, float a) {
    if (gColKnown && gColCache[0] == r && gColCache[1] == g &&
        gColCache[2] == b && gColCache[3] == a) return;   // mismo color -> nada
    gColCache[0] = r; gColCache[1] = g; gColCache[2] = b; gColCache[3] = a;
    gColKnown = true;
    // con COLOR_MATERIAL prendido este color PISA ambient/diffuse del material
    if (gCapOn[ColorMaterial]) { gMatKnown[MatAmbient] = false; gMatKnown[MatDiffuse] = false; }
    glColor4f(r, g, b, a);
}
// glColor4fv NO existe en GLES1 (solo glColor4f/4ub/4x): expandir a Color4f (cacheado)
void Color4fv(const float* c) { Color4f(c[0], c[1], c[2], c[3]); }

// --- niebla ---
void FogMode(bool linear) { glFogf(GL_FOG_MODE, (float)(linear ? GL_LINEAR : GL_EXP)); }
void FogStart(float z)    { glFogf(GL_FOG_START, z); }
void FogEnd(float z)      { glFogf(GL_FOG_END, z); }
void FogColor(const float* c) { glFogfv(GL_FOG_COLOR, c); }

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
void Light0fv(LightFv p, const float* v) { glLightfv(GL_LIGHT0, LightFvGL(p), v); }
static GLenum LightFGL(LightF p) {
    switch (p) {
        case LightConstantAtt:  return GL_CONSTANT_ATTENUATION;
        case LightLinearAtt:    return GL_LINEAR_ATTENUATION;
        case LightQuadraticAtt: return GL_QUADRATIC_ATTENUATION;
    }
    return GL_CONSTANT_ATTENUATION;
}
void Light0f(LightF p, float v) { glLightf(GL_LIGHT0, LightFGL(p), v); }
void SetLightEnabled(unsigned int id, bool on) { if (on) glEnable(id); else glDisable(id); }

// query del CACHE (todas las Enable/Disable pasan por el motor, asi que es fiel)
bool IsEnabled(Cap c) { return gCapOn[c]; }

void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* p) {
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p);
}
void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    gColKnown = false;   // otra precision: el cache de Color4f deja de valer
    if (gCapOn[ColorMaterial]) { gMatKnown[MatAmbient] = false; gMatKnown[MatDiffuse] = false; }
    glColor4ub(r, g, b, a);
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
    glMaterialxv(GL_FRONT_AND_BACK, MatParamGL(p), x);
#else
    glMaterialfv(GL_FRONT_AND_BACK, MatParamGL(p), rgba);
#endif
}
void MaterialShininess(float s) {
    if (gShinCache == s) return;
    gShinCache = s;
#ifdef __ANDROID__
    glMaterialx(GL_FRONT_AND_BACK, GL_SHININESS, (GLfixed)(s * 65536.0f));
#else
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, s);
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
    FzSubAprox        // ZERO, ONE_MINUS_SRC_COLOR (aprox Symbian)
};
static void PonerBlendFunc(int firma) {
    if (gBlendFuncCache == firma) return;
    gBlendFuncCache = firma;
    g_statStateChanges++;
    switch (firma) {
        case FzOff:      glBlendFunc(GL_ONE, GL_ZERO); break;
        case FzAdd:      glBlendFunc(GL_ONE, GL_ONE); break;
        case FzAddAlpha: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
        case FzMultiply: glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
        case FzScreen:   glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
        case FzPremult:  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        case FzSubAprox: glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); break;
        case FzAlpha: default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
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
    if (pfn) pfn(e);
#else
    glBlendEquation(e);
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
    if (ref > 0.0f) { glAlphaFunc(GL_GREATER, ref); glEnable(GL_ALPHA_TEST); }
    else            { glDisable(GL_ALPHA_TEST); }
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
void BlendMode(int modo) { // capa multi-pass sobre lo de abajo
#ifndef W3D_SYMBIAN
    PonerBlendEq(0);   // las capas mezclan con FUNC_ADD (mismo porque que BlendAlpha)
#endif
    if (modo == 1)      PonerBlendFunc(FzMultiply);  // Multiply (oscurece)
    else if (modo == 2) PonerBlendFunc(FzAdd);       // Add (aclara)
    else                PonerBlendFunc(FzAlpha);     // Mix (alpha encima)
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fmin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}
void TexWrap(bool repeat) {
    if (gTexBound != 0xFFFFFFFFu &&
        !TexWrapCambia(gTexBound, repeat ? 1 : 0)) return;
    GLfloat w = (GLfloat)(repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, w);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, w);
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, nivel < 0 ? 0 : nivel);
#endif
}

// ---- punteros de los arrays ----
void VertexPointer2f(int strideBytes, const float* p)   { glVertexPointer(2, GL_FLOAT, strideBytes, p); }
void VertexPointer3f(int strideBytes, const float* p)   { glVertexPointer(3, GL_FLOAT, strideBytes, p); }
void VertexPointer3s(int strideBytes, const short* p)   { glVertexPointer(3, GL_SHORT, strideBytes, p); }
void VertexPointer2s(int strideBytes, const short* p)   { glVertexPointer(2, GL_SHORT, strideBytes, p); }
void ColorPointer4ub(const unsigned char* p)            { glColorPointer(4, GL_UNSIGNED_BYTE, 0, p); }
void NormalPointer3b(const signed char* p)              { glNormalPointer(GL_BYTE, 0, p); }
void TexCoordPointer2f(int strideBytes, const float* p) { glTexCoordPointer(2, GL_FLOAT, strideBytes, p); }
// normales como texcoords (matcap HW). count = cantidad de vertices (para poder convertir el tipo en desktop).
void TexCoordPointer3b(const signed char* p, int count) {
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    (void)count;
    glTexCoordPointer(3, GL_BYTE, 0, p); // GLES1 (N95/Android): GL_BYTE ES un tipo valido para texcoords
#else
    // OpenGL de ESCRITORIO: glTexCoordPointer NO acepta GL_BYTE -> daba GL_INVALID_ENUM y la llamada se ignoraba,
    // asi que el matcap por matriz de textura no tenia sus texcoords -> la textura salia PLANA y no seguia la camara.
    // Se convierte a GL_SHORT (si valido en desktop). El factor s=0.5/127 de TexMatrixMatcap sigue igual: GL_SHORT
    // NO se normaliza (127 -> 127.0). Buffer estatico que crece = sin alloc por-frame tras el warmup.
    static GLshort* tmp = 0; static int cap = 0;
    int n = count * 3;
    if (n > cap) { delete[] tmp; tmp = new GLshort[n]; cap = n; }
    for (int i = 0; i < n; i++) tmp[i] = (GLshort)p[i];
    glTexCoordPointer(3, GL_SHORT, 0, n > 0 ? tmp : (const GLshort*)0);
#endif
}

// ---- BUFFER OBJECTS (VBO/IBO): subir 1 vez, dibujar desde GPU (ver w3dGraphics.h) ----
bool VBOSoportado(){ w3d_CargarVBO(); return w3d_vboOk; }
unsigned int GenBuffer(){ w3d_CargarVBO(); if (!w3d_vboOk) return 0; GLuint b = 0; GLB_GEN(1, &b); return (unsigned int)b; }
void DeleteBuffer(unsigned int h){ if (h && w3d_vboOk){ GLuint b = (GLuint)h; GLB_DEL(1, &b); } }
void ArrayBufferData(unsigned int h, const void* data, int bytes){
    if (!w3d_vboOk || !h) return; g_statUploadBytes += bytes; GLB_BIND(GL_ARRAY_BUFFER, (GLuint)h); GLB_DATA(GL_ARRAY_BUFFER, (ptrdiff_t)bytes, data, GL_STATIC_DRAW); }
void IndexBufferData(unsigned int h, const void* data, int bytes){
    if (!w3d_vboOk || !h) return; g_statUploadBytes += bytes; GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, (GLuint)h); GLB_DATA(GL_ELEMENT_ARRAY_BUFFER, (ptrdiff_t)bytes, data, GL_STATIC_DRAW); }
void VertexVBO(unsigned int h){ GLB_BIND(GL_ARRAY_BUFFER, (GLuint)h); glVertexPointer(3, GL_FLOAT, 0, (const GLvoid*)0); }
void NormalVBO(unsigned int h){ GLB_BIND(GL_ARRAY_BUFFER, (GLuint)h); glNormalPointer(GL_BYTE, 0, (const GLvoid*)0); }
void ColorVBO(unsigned int h){ GLB_BIND(GL_ARRAY_BUFFER, (GLuint)h); glColorPointer(4, GL_UNSIGNED_BYTE, 0, (const GLvoid*)0); }
void TexCoordVBO(unsigned int h){ GLB_BIND(GL_ARRAY_BUFFER, (GLuint)h); glTexCoordPointer(2, GL_FLOAT, 0, (const GLvoid*)0); }
void BindIndexVBO(unsigned int h){ GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, (GLuint)h); }
void DrawTrianglesVBO(int count, int firstIndex){
    StatDrawTri(count); g_statDrawVBO++;
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, (const GLvoid*)(size_t)(firstIndex * 2)); // 16-bit
#else
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, (const GLvoid*)(size_t)(firstIndex * 4));   // 32-bit
#endif
}
void UnbindVBOs(){ if (!w3d_vboOk) return; GLB_BIND(GL_ARRAY_BUFFER, 0); GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, 0); }

// (P3) indices de CLIENTE sobre atributos en VBO (ver w3dGraphics.h): desbindea el
// ELEMENT_ARRAY (los indices se leen de RAM en el draw) y cuenta esos bytes como
// "subidos" (son lo unico que cruza el bus por frame con la visibilidad activa).
void DrawTrianglesClientIdx(int count, const MeshIndex* indices) {
    if (w3d_vboOk) GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, 0);
    g_statUploadBytes += count * (int)sizeof(MeshIndex);
    StatDrawTri(count); g_statDrawVBO++;   // los ATRIBUTOS salen del VBO (solo los indices viajan)
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, indices);
#else
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, indices);
#endif
}

// LINEAS con indices de CLIENTE sobre los atributos que esten bindeados (VBO o
// client arrays): el gemelo de DrawTrianglesClientIdx para las aristas de un
// material con `lineas` (el mismo camino sirve en los dos modos de dibujo).
void DrawLinesClientIdx(int count, const MeshIndex* indices) {
    if (w3d_vboOk) GLB_BIND(GL_ELEMENT_ARRAY_BUFFER, 0);
    PostDrawColor();
    g_statUploadBytes += count * (int)sizeof(MeshIndex);
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    glDrawElements(GL_LINES, count, GL_UNSIGNED_SHORT, indices);
#else
    glDrawElements(GL_LINES, count, GL_UNSIGNED_INT, indices);
#endif
}

// ---- dibujo de triangulos indexados ----
void DrawTriangles(int count, const MeshIndex* indices) {
    StatDrawTri(count);
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, indices); // GLES1.1: solo 16 bits
#else
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, indices);   // PC/WebGL/N8: 32 bits
#endif
}
// triangulos NO indexados (glDrawArrays): la UI 2D arma sus verts en orden
void DrawTrianglesArray(int vertexCount) {
    StatDrawTri(vertexCount);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
}
void DrawTrianglesArrayFrom(int first, int count) { // glDrawArrays con offset (chrome equirect por-corner)
    StatDrawTri(count);
    glDrawArrays(GL_TRIANGLES, first, count);
}
void DrawTrianglesByte(int count, const unsigned char* indices) {
    StatDrawTri(count);
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_BYTE, indices);
}

// lineas sueltas (overlay de normales): dibuja vertexCount vertices de a pares
void DrawLines(int vertexCount) {
    PostDrawColor();
    glDrawArrays(GL_LINES, 0, vertexCount);
}
void DrawLineStrip(int vertexCount) {
    PostDrawColor();
    glDrawArrays(GL_LINE_STRIP, 0, vertexCount);
}
void DrawLineStripIndexed(int count, const unsigned short* indices) {
    PostDrawColor();
    glDrawElements(GL_LINE_STRIP, count, GL_UNSIGNED_SHORT, indices);
}

// puntos (vertices en Edit Mode). glDrawArrays(GL_POINTS) anda en la fase 3D
// (no es la fase 2D rota del N95, y no usa glDrawElements)
void DrawPoints(int vertexCount) {
    PostDrawColor();
    glDrawArrays(GL_POINTS, 0, vertexCount);
}
void PointSize(float px) { glPointSize(px); }

// lineas INDEXADAS (bordes de edit mode con vertex color compartido): en la fase
// 3D glDrawElements funciona (la malla rellena ya lo usa para los triangulos).
void DrawLinesIndexed(int count, const unsigned short* indices) {
    PostDrawColor();
    glDrawElements(GL_LINES, count, GL_UNSIGNED_SHORT, indices);
}

// offset de profundidad del relleno (slope-aware). Negativo = lo tira hacia la
// camara; se usa para el contorno: los rellenos tapan las lineas internas.
// Cacheado: AplicarMaterial re-manda (0,0) por cada malla comun.
static float gPolyOffF = -1e9f, gPolyOffU = -1e9f;
void PolygonOffset(float factor, float units) {
    if (gPolyOffF == factor && gPolyOffU == units) return;
    gPolyOffF = factor; gPolyOffU = units;
    glPolygonOffset(factor, units);
}

// empuja el rango de profundidad (contorno: las lineas internas quedan tapadas
// por la malla y solo se ve el borde/silueta)
void DepthRange(float n, float f) {
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    glDepthRangef(n, f);
#else
    glDepthRange(n, f);
#endif
}

// ---- wireframe (solo GL de escritorio tiene glPolygonMode) ----
void Wireframe(bool on) {
#if !defined(W3D_SYMBIAN) && !defined(__ANDROID__)
    glPolygonMode(GL_FRONT_AND_BACK, on ? GL_LINE : GL_FILL);
#else
    (void)on; // GL ES: sin polygonmode -> sale relleno (como hoy)
#endif
}

// ---- invalidar el cache tras GL directo ----
// NO consulta GL (glIsEnabled fuerza un FLUSH del GPU en renderers de tiles como
// el del N95: carisimo y se llamaba varias veces por frame -> de 60 a 30 fps).
// Solo marca cada cap como "desconocido": la proxima Enable/Disable por cap fuerza
// la llamada (barata, sin flush) y vuelve a cachear.
void Invalidate() {
    for (int i = 0; i < kNumCaps; i++) gCapKnown[i] = false;
    // idem el binding de textura: el proximo BindTexture hace la llamada
    gTexBound = 0xFFFFFFFFu;
    // y el cache FINO (P1): todo "desconocido" -> la proxima llamada de cada
    // estado va al driver y re-cachea
    gDepthFuncCache = -1; gDepthMaskCache = -1;
    gBlendFuncCache = -1; gBlendEqCache = -1;
    gTexEnvCache = -1; gTexGenCache = -1; gMatcapCache = -1;
    gSmoothCache = -1; gAlphaRefCache = -1e9f;
    for (int i = 0; i < 4; i++) gArrOn[i] = -1;
    // material/color actual: tambien a "desconocido".
    // El cache POR TEXTURA (TexFiltroCambia/TexWrapCambia) NO se toca a
    // proposito: glTexParameter es estado del OBJETO textura, el GL directo de
    // un pase ajeno no lo cambia (y quien borra texturas pasa por DeleteTexture,
    // que olvida su entrada).
    for (int i = 0; i < 4; i++) gMatKnown[i] = false;
    gShinCache = -1e9f;
    gColKnown = false;
    gPolyOffF = gPolyOffU = -1e9f;
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
#if defined(W3D_SYMBIAN) || defined(__ANDROID__)
    return 0;   // en dispositivo no se audita (glGet = flush del tiler)
#else
    int malos = 0;
    for (int i = 0; i < kNumCaps; i++) {
        if (!gCapKnown[i]) continue;
        GLenum e = CapGL((Cap)i);
        if (!e) continue;
        bool real = glIsEnabled(e) == GL_TRUE;
        if (real != gCapOn[i]) {
            printf("      [glaudit] DESYNC cap %d: motor=%d GL=%d\n", i, (int)gCapOn[i], (int)real);
            malos++;
        }
    }
    if (gDepthMaskCache >= 0) {
        GLboolean dm = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
        if ((dm == GL_TRUE ? 1 : 0) != gDepthMaskCache) {
            printf("      [glaudit] DESYNC depthMask: motor=%d GL=%d\n", gDepthMaskCache, (int)(dm == GL_TRUE));
            malos++;
        }
    }
    if (gTexBound != 0xFFFFFFFFu) {
        GLint t = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
        if ((unsigned)t != gTexBound) {
            printf("      [glaudit] DESYNC textura: motor=%u GL=%d\n", gTexBound, (int)t);
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

