// ============================================================================
//  w3dGraphicsGLES2.cpp — backend OpenGL ES 2.0 / WebGL de w3dEngine.
//
//  ES2/WebGL NO tiene pipeline fijo: todo es shaders. Este backend EMULA el pipeline
//  fijo que usa el Core (la MISMA API que w3dGraphics.cpp, el backend fijo) con:
//    - stack de matrices POR SOFTWARE (Matrix4) -> uniforms (MVP + modelview + normal + textura)
//    - un uber-shader que hace LAS MISMAS CUENTAS que OpenGL ES 1.1, para que web/Android se vean como el
//      PC y el N95: luz POR VERTICE (hasta 8 luces, ambiente global, atenuacion, cono, especular sin
//      observador local, el color recortado a [0,1] antes de la textura), matriz de textura, sphere-map y
//      matcap, el combiner (modulate / replace / DOT3 / solo alfa), la segunda unidad (mezcla de
//      texturas) y la niebla
//    - los vertex arrays client-side -> VBOs (WebGL no acepta punteros client-side)
//
//  Se elige en compile-time con W3D_WEBGL (en vez de w3dGraphics.cpp). En desktop las
//  funciones GL2.0 se cargan con GLES2Init(getProc = SDL_GL_GetProcAddress); en Emscripten
//  van directas. Mismo codigo -> se verifica en PC (GL 2.1, cmake -DW3D_PC_GLES2=ON) y compila
//  a WebGL con emcc.
//
//  Lo que sigue sin equivalente: el sombreado PLANO (GLSL ES 1.00 no tiene 'flat') y el
//  wireframe por glPolygonMode.
// ============================================================================
#include "w3dGraphics.h"
#include "graphics/w3dBackend.h"   // el namespace propio del backend (varios en un ejecutable)
#include "w3dlog.h"
#include "math/Matrix4.h"
#include <vector>
#include <string>
#include <map>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // getenv (W3D_GLES2_BY_DRAW)
#include <time.h>     // clock_gettime (g_measureDraws)
#ifdef __ANDROID__
#include <dlfcn.h>    // el alto de la ventana por EGL (CopyScreenRow)
#endif

// W3D_ES2_NATIVE: la plataforma trae ES 2.0 de verdad (headers + lib), sin cargar funciones a mano ni GLSL de escritorio.
// W3D_VKGL: este archivo compilado como el backend VULKAN (graphics/vulkan/w3dGraphicsVk.cpp): sus glFoo son vkglFoo, un ES 2.0
// hecho con Vulkan (graphics/vulkan/w3dVkGL.cpp), y los shaders van como en ES (los traduce el)
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(W3D_SYMBIAN) || defined(W3D_VKGL)
    #define W3D_ES2_NATIVE
#endif

#if defined(W3D_VKGL)
    #include "graphics/vulkan/w3dVkGL.h"
#elif defined(W3D_ES2_NATIVE)
    #include <GLES2/gl2.h>
#else
    #ifdef _WIN32
        #define NOMINMAX
        #include <windows.h>
    #endif
    #include <GL/gl.h> // GL 1.1 core (glClear/glEnable/glDrawElements/glGenTextures...)
    #include <stddef.h> // ptrdiff_t (no confiar en que otro header lo traiga)
    #ifndef APIENTRY
        #define APIENTRY
    #endif
    // tipos + enums de GL2.0 que <GL/gl.h> (1.1) no trae
    typedef char GLchar;
    typedef ptrdiff_t GLsizeiptr;
    #define GL_FRAGMENT_SHADER      0x8B30
    #define GL_VERTEX_SHADER        0x8B31
    #define GL_COMPILE_STATUS       0x8B81
    #define GL_LINK_STATUS          0x8B82
    #define GL_ARRAY_BUFFER         0x8892
    #define GL_ELEMENT_ARRAY_BUFFER 0x8893
    #define GL_STATIC_DRAW          0x88E4
    #define GL_DYNAMIC_DRAW         0x88E8
    #define GL_TEXTURE0             0x84C0
    #ifndef GL_CLAMP_TO_EDGE
        #define GL_CLAMP_TO_EDGE    0x812F
    #endif
    // punteros a las funciones GL2.0 (se cargan en GLES2Init)
    typedef GLuint (APIENTRY* PFcreateShader)(GLenum);
    typedef void   (APIENTRY* PFshaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    typedef void   (APIENTRY* PFcompileShader)(GLuint);
    typedef void   (APIENTRY* PFgetShaderiv)(GLuint, GLenum, GLint*);
    typedef void   (APIENTRY* PFgetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    typedef GLuint (APIENTRY* PFcreateProgram)(void);
    typedef void   (APIENTRY* PFattachShader)(GLuint, GLuint);
    typedef void   (APIENTRY* PFlinkProgram)(GLuint);
    typedef void   (APIENTRY* PFgetProgramiv)(GLuint, GLenum, GLint*);
    typedef void   (APIENTRY* PFgetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    typedef void   (APIENTRY* PFuseProgram)(GLuint);
    typedef GLint  (APIENTRY* PFgetUniformLocation)(GLuint, const GLchar*);
    typedef GLint  (APIENTRY* PFgetAttribLocation)(GLuint, const GLchar*);
    typedef void   (APIENTRY* PFuniform1i)(GLint, GLint);
    typedef void   (APIENTRY* PFuniform1f)(GLint, GLfloat);
    typedef void   (APIENTRY* PFuniform4fv)(GLint, GLsizei, const GLfloat*);
    typedef void   (APIENTRY* PFuniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat*);
    typedef void   (APIENTRY* PFuniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
    typedef void   (APIENTRY* PFgenBuffers)(GLsizei, GLuint*);
    typedef void   (APIENTRY* PFbindBuffer)(GLenum, GLuint);
    typedef void   (APIENTRY* PFbufferData)(GLenum, GLsizeiptr, const void*, GLenum);
    typedef void   (APIENTRY* PFvertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    typedef void   (APIENTRY* PFenableVAA)(GLuint);
    typedef void   (APIENTRY* PFdisableVAA)(GLuint);
    typedef void   (APIENTRY* PFvertexAttrib4fv)(GLuint, const GLfloat*);
    typedef void   (APIENTRY* PFactiveTexture)(GLenum);
    typedef void   (APIENTRY* PFbindAttribLocation)(GLuint, GLuint, const GLchar*);
    typedef void   (APIENTRY* PFdeleteShader)(GLuint);
    typedef void   (APIENTRY* PFdeleteBuffers)(GLsizei, const GLuint*);
    typedef void   (APIENTRY* PFdeleteProgram)(GLuint);
    typedef void   (APIENTRY* PFblendEquation)(GLenum);   // (GL 1.4: el <GL/gl.h> 1.1 de Windows no la trae)
    // los FBO (GL 3.0 / ARB_framebuffer_object): el mapa de las sombras en tiempo real
    typedef void   (APIENTRY* PFgenFramebuffers)(GLsizei, GLuint*);
    typedef void   (APIENTRY* PFdeleteFramebuffers)(GLsizei, const GLuint*);
    typedef void   (APIENTRY* PFbindFramebuffer)(GLenum, GLuint);
    typedef void   (APIENTRY* PFframebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    typedef GLenum (APIENTRY* PFcheckFramebufferStatus)(GLenum);
    typedef void   (APIENTRY* PFgenRenderbuffers)(GLsizei, GLuint*);
    typedef void   (APIENTRY* PFdeleteRenderbuffers)(GLsizei, const GLuint*);
    typedef void   (APIENTRY* PFbindRenderbuffer)(GLenum, GLuint);
    typedef void   (APIENTRY* PFrenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    typedef void   (APIENTRY* PFframebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    #ifndef GL_FRAMEBUFFER
    #define GL_FRAMEBUFFER           0x8D40
    #define GL_COLOR_ATTACHMENT0     0x8CE0
    #define GL_FRAMEBUFFER_COMPLETE  0x8CD5
    #endif
    #ifndef GL_FUNC_ADD
    #define GL_FUNC_ADD              0x8006
    #define GL_FUNC_SUBTRACT         0x800A
    #define GL_FUNC_REVERSE_SUBTRACT 0x800B
    #endif
    static PFcreateShader        glCreateShader;
    static PFshaderSource        glShaderSource;
    static PFcompileShader       glCompileShader;
    static PFgetShaderiv         glGetShaderiv;
    static PFgetShaderInfoLog    glGetShaderInfoLog;
    static PFcreateProgram       glCreateProgram;
    static PFattachShader        glAttachShader;
    static PFlinkProgram         glLinkProgram;
    static PFgetProgramiv        glGetProgramiv;
    static PFgetProgramInfoLog   glGetProgramInfoLog;
    static PFuseProgram          glUseProgram;
    static PFgetUniformLocation  glGetUniformLocation;
    static PFgetAttribLocation   glGetAttribLocation;
    static PFuniform1i           glUniform1i;
    static PFuniform1f           glUniform1f;
    static PFuniform4fv          glUniform4fv;
    static PFuniformMatrix3fv    glUniformMatrix3fv;
    static PFuniformMatrix4fv    glUniformMatrix4fv;
    static PFgenBuffers          glGenBuffers;
    static PFbindBuffer          glBindBuffer;
    static PFbufferData          glBufferData;
    static PFvertexAttribPointer glVertexAttribPointer;
    static PFenableVAA           glEnableVertexAttribArray;
    static PFdisableVAA          glDisableVertexAttribArray;
    static PFvertexAttrib4fv     glVertexAttrib4fv;
    // (el <GL/gl.h> de Mesa ya declara glActiveTexture -GL 1.3-: el puntero va con otro nombre)
    #define glActiveTexture w3dGlActiveTexture
    static PFactiveTexture       glActiveTexture;
    static PFbindAttribLocation  glBindAttribLocation;
    static PFdeleteShader        glDeleteShader;
    static PFdeleteBuffers       glDeleteBuffers;
    static PFdeleteProgram       glDeleteProgram;
    // (glBlendEquation tambien: el de Mesa la declara y el de Windows no)
    #define glBlendEquation w3dGlBlendEquation
    static PFblendEquation       glBlendEquation;
    static PFgenFramebuffers         glGenFramebuffers;
    static PFdeleteFramebuffers      glDeleteFramebuffers;
    static PFbindFramebuffer         glBindFramebuffer;
    static PFframebufferTexture2D    glFramebufferTexture2D;
    static PFcheckFramebufferStatus  glCheckFramebufferStatus;
    static PFgenRenderbuffers        glGenRenderbuffers;
    static PFdeleteRenderbuffers     glDeleteRenderbuffers;
    static PFbindRenderbuffer        glBindRenderbuffer;
    static PFrenderbufferStorage     glRenderbufferStorage;
    static PFframebufferRenderbuffer glFramebufferRenderbuffer;
    #ifndef GL_TEXTURE1
        #define GL_TEXTURE1         0x84C1
    #endif
    #ifndef GL_VERTEX_PROGRAM_POINT_SIZE
        #define GL_VERTEX_PROGRAM_POINT_SIZE 0x8642   // gl_PointSize del shader (en ES2 siempre vale)
    #endif
    #ifndef GL_POINT_SPRITE
        #define GL_POINT_SPRITE     0x8861            // gl_PointCoord (en ES2 siempre vale)
    #endif
#endif

// estado de render compartido (lo setea el editor antes de dibujar la escena; el Core lo LEE).
// Son los MISMOS globales que define el backend de escritorio (w3dGraphics.cpp): al usar el
// backend ES2 en su lugar hay que definirlos aca o quedan sin resolver al linkear.
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool w3dRenderWireframe = false;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool w3dRenderWireframeOnlyVisible = true; // wireframe SOLO de la lista activa (pvsFaces)
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool w3dRenderSolid    = false;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool w3dRenderNoLight    = false;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool w3dRenderLights     = true;   // (el render: las luces de la escena. El editor pone su modo de vista)
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool w3dRenderNormalColor = false;
#endif
// (las sondas de luz y el horneado: mas unidades de textura, cube maps y RGB565. El GL/gl.h de Windows es 1.1; fuera de las ramas: vale tambien para W3D_VKGL)
#ifndef GL_TEXTURE2
    #define GL_TEXTURE2         0x84C2
#endif
#ifndef GL_TEXTURE3
    #define GL_TEXTURE3         0x84C3
#endif
#ifndef GL_TEXTURE4
    #define GL_TEXTURE4         0x84C4
#endif
#ifndef GL_TEXTURE_CUBE_MAP
    #define GL_TEXTURE_CUBE_MAP            0x8513
#endif
#ifndef GL_TEXTURE_CUBE_MAP_POSITIVE_X
    #define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
    #define GL_UNSIGNED_SHORT_5_6_5        0x8363
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool w3dRenderAlpha       = false; // pase ALPHA (matte): blanco unlit + solo el alpha de la textura
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool w3dRenderOverlays    = true;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool w3dShowSelection      = true;  // "Ver seleccion" (menu Select): OFF = ni contorno ni tinte de seleccion
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool g_xray               = false; // X-Ray OFF por defecto (lo togglea el menu Overlays)
#endif

namespace w3dEngine {
W3D_BACKEND_OPEN   // (con varios backends: w3dEngine::es2)

// el CONTADOR DE LLAMADAS (el mismo que el backend fijo, ver w3dGraphics.h): cada gl* real suma 1 a la categoria del que
// dibuja. Aca no hay cache de estado: un draw cuesta ~45 llamadas (uniforms, atributos, programa)
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
int g_statCalls[StatCatCount_] = { 0 };
#endif
static int gStatCat = StatCatEditor;
// (g_measureDraws, ver StatsFrame) cuanto tarda cada draw: se mide entero con un MeasureDraw al principio
struct MeasureDraw { double t0; MeasureDraw(); ~MeasureDraw(); };
static int gLastIdx = 0, gLastVerts = 0, gLastVBO = 0;   // (los del draw en curso)
// la TRAZA (medir), misma que el backend GLES 1.1: con g_traceGL cada llamada suma en su texto ("glBindTexture(...)")
// y TraceGLVer la imprime ordenada; con g_traceGLSec ademas sale cada llamada en orden, con su categoria
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool g_traceGL = false;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
float g_alphaColor = 1.0f;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool g_traceGLSec = false;
#endif
static const char* gTraceTxt[512];   // (un lugar del codigo = un texto: se compara el puntero)
static int gTraceN[512], gTraceCount = 0;
void TraceGL(const char* l) {
    if (g_traceGLSec) printf("      [glsec] %d %s\n", gStatCat, l);
    for (int i = 0; i < gTraceCount; i++) if (gTraceTxt[i] == l) { gTraceN[i]++; return; }
    if (gTraceCount < 512) { gTraceTxt[gTraceCount] = l; gTraceN[gTraceCount++] = 1; }
}
void TraceGLVer() {
    for (int k = 0; k < gTraceCount; k++) {
        int m = k;
        for (int i = k + 1; i < gTraceCount; i++) if (gTraceN[i] > gTraceN[m]) m = i;
        const char* t = gTraceTxt[k]; gTraceTxt[k] = gTraceTxt[m]; gTraceTxt[m] = t;
        int n = gTraceN[k]; gTraceN[k] = gTraceN[m]; gTraceN[m] = n;
        printf("      [gltraza] %6d  %s\n", gTraceN[k], gTraceTxt[k]);
    }
    gTraceCount = 0;
}
// (antes de cada llamada a GL se dibuja el LOTE juntado, si hay: ver BatchEmpty)
static int batchN = 0;
static void BatchEmpty();
#define W3D_GLC(call) (batchN ? BatchEmpty() : (void)0, g_statCalls[gStatCat]++, g_traceGL ? TraceGL(#call) : (void)0, (call))

// ============================================================================
//  Estado emulado del pipeline fijo
// ============================================================================
static bool cap_depth=false, cap_cull=false, cap_tex=false, cap_light=false, cap_blend=false, cap_fog=false, cap_scissor=false;
static bool cap_offset=false;   // GL_POLYGON_OFFSET_FILL
// GL_DITHER: GL lo trae PRENDIDO. En una pantalla de 16 bits (la VideoCore IV del Ace) corre los colores que no caen
// justo en un nivel: lo que se dibuja para LEER (los numeros de objeto de la sombra proyectada, el pick por color)
// lo apaga, y sin esto quedaba prendido (la sombra salia en puntos sobre todo)
static bool cap_dither=true;
static bool cap_colormat=false; // GL_COLOR_MATERIAL: el color del VERTICE reemplaza el diffuse/ambient del material
static int  matMode = 1; // 0=Projection, 1=ModelView, 2=Texture
static Matrix4 curProj, curMV;
static Matrix4 curTex;     // matriz de TEXTURA de la unidad 0 (TexMatrixUV / matcap / el flip-V del sphere-map)
static std::vector<Matrix4> stkProj, stkMV, stkTex;
// LAS LUCES del pipeline fijo, con sus valores de fabrica de GL: la 0 difusa y especular blancas, las otras negras;
// el ambiente de cada luz negro; sin atenuacion; sin cono. La posicion y la direccion del cono se guardan en OJO
// (transformadas con la modelview del momento, como glLightfv). LA DE AREA (LightArea): sus medios lados en OJO
struct Light { bool on; float pos[4], amb[4], dif[4], spec[4], att[3], dir[3], cutCos, expo; bool area, areaRound; float ax[3], ay[3]; };
static Light lights[LightsMax];
static float ambGlobal[4] = {0.2f,0.2f,0.2f,1};   // GL_LIGHT_MODEL_AMBIENT
static bool  lightsDirty = true;                    // hay que volver a mandar las luces al shader
static void FactoryLights(){
    for(int i=0;i<LightsMax;i++){
        Light& L=lights[i];
        L.on=false;
        L.pos[0]=0; L.pos[1]=0; L.pos[2]=1; L.pos[3]=0;
        for(int k=0;k<4;k++){ L.amb[k]=(k==3)?1.0f:0.0f; L.dif[k]=(i==0||k==3)?1.0f:0.0f; L.spec[k]=L.dif[k]; }
        L.att[0]=1; L.att[1]=0; L.att[2]=0;
        L.dir[0]=0; L.dir[1]=0; L.dir[2]=-1;
        L.cutCos=-1.0f; L.expo=0.0f;   // corte 180 = sin cono
        L.area=false; L.areaRound=false;
    }
    lightsDirty=true;
}
static float matDiff[4]   = {0.8f,0.8f,0.8f,1};
static float matAmb[4]    = {0.2f,0.2f,0.2f,1};
static float matSpec[4]   = {0,0,0,1};
static float matEmis[4]   = {0,0,0,1};
static float matShine     = 0.0f;
// los valores PBR del material (MaterialPBR): metalico, rugosidad, F0 (del IOR) y transmision. rug < 0 = la luz de siempre
static float matPbr[4]    = {0.0f,-1.0f,0.04f,0.0f};
static float matExtra[4]  = {0.0f,0.0f,0.0f,0.0f};   // x = reflejos en pantalla (SSR)
// EL ENTORNO (Entorno): la textura 360, su copia borrosa y las 3 filas de la matriz ojo -> imagen (w de la 1ra: prendido)
static unsigned int envTex = 0, envTexB = 0;
static bool envCube = false;   // envTex / envTexB son CUBEMAPS (EnvironmentCube: las GPU chicas)
// LO DE ATRAS del vidrio (SceneCopy): la copia del viewport en la unidad 4 y el rectangulo de donde salio (w = 0: apagado)
static GLuint texScene = 0; static int texEscW = 0, texEscH = 0; static float escVp[4] = {0,0,0,0};
// ...y su PROFUNDIDAD (unidad 5), para los reflejos en pantalla: con ella el reflejo recorre el rayo y encuentra lo que
// toca (un espejo de verdad). Solo en GL de escritorio: ES 2.0 no copia la profundidad a una textura (sin ella, el reflejo
// aproximado de siempre). escZState: 0 sin probar, 1 anda, -1 este driver no la copia
// LOS MAPAS DEL MATERIAL (MaterialMaps): normal, rugosidad, metalico, alfa, emision, en las unidades 6..10. Solo con el
// programa entero de GL de escritorio o Vulkan (con un programa por draw -las GPU chicas- o un ES 2.0 de 8 unidades, no)
static GLuint mapTex[5] = {0,0,0,0,0};
static int mapsOk = -1;   // -1 sin saber; 0 este GL no tiene unidades para ellos; 1 si
static bool MapsHas();
static GLuint texEscZ = 0; static int texEscZW = 0, texEscZH = 0; static bool escZ = false; static int escZState = 0;
static bool escZTurn = false;   // (Vulkan) la copia de la profundidad tiene la fila de arriba primero
static float envF[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
static float curColor[4]  = {1,1,1,1};   // color uniforme (Color4*) cuando NO hay array de color
static float fogColor[4]  = {0,0,0,1};
static float fogStart=0, fogEnd=1;
static int   fogMode=0;       // 0 lineal / 1 exp / 2 exp2 (el objeto Niebla), igual que GL_FOG_MODE
static float fogDens=1.0f;
static int   texEnv=0;        // el combiner de la unidad 0: 0 modulate, 1 replace, 2 DOT3, 3 solo alfa
static bool  texGenSphere=false;   // GL_SPHERE_MAP: las UV salen del reflejo (en el shader)
// LA MATRIZ DE TEXTURA la tocan tres cosas (sphere-map, matcap, desplazamiento de UV) con los MISMOS caches que el
// backend fijo: asi se reinicia exactamente en los mismos momentos (un "apagar" de lo que ya estaba apagado no la toca)
static int texGenCache = -1;   // 0/1
static int matcapCache = -1;   // 0 identidad / 1 matcap / 2 desplazamiento de UV
static float uvMat[4] = { 0, 0, 1, 1 };
static float uvAff[6] = { 1, 0, 0, 1, 0, 0 };   // la transformacion afin puesta (matcapCache 3)
static bool  pointSprite=false; // point sprite: samplear la textura con gl_PointCoord (iconos: cursor/origen/luz)
static float pointSize=1.0f;
static float vpCurrent[4]={0,0,1,1};   // el glViewport actual (el sprite saca su centro en pixeles de ventana)
static int passEsp = 0;   // el PASE ESPECIAL prendido (0 = ninguno; ver SpecialPass; 5 = el mapa de sombras)
static unsigned boundTex=0;
// LA UNIDAD 1 (mezcla de texturas): la textura B, como se combina y sus UV (propias o las de la unidad 0)
static bool     mixOn=false;
static unsigned mixTex=0;
static int      texMixMode=0;   // 0 interpolar con k, 1 multiplicar, 2 multiplicar x2, 3 restar
static float    mixK=1.0f;
static const float* mixUV=0; // UV propias de la unidad 1 (0 = las mismas de la unidad 0)
static GLuint mixUVVbo=0;    // o en un buffer object
// un atributo: de RAM (ptr) o de un buffer object (vbo, desde el byte 0: ver VertexVBO y compania)
struct Arr { bool on; int size; unsigned type; int stride; const void* ptr; GLuint vbo; };
static Arr aPos={false,3,0,0,0}, aNrm={false,3,0,0,0}, aUV={false,2,0,0,0}, aCol={false,4,0,0,0};

// ---- objetos GL + locations ----
static GLuint prog=0, vboP=0, vboN=0, vboT=0, vboC=0, vboT1=0, ibo=0;
static GLint uMVP,uMV,uNMat,uTexM,uTexGen,uUseTex,uTex,uTex1,uEnv,uLightOn,uFogOn,uPointSize,uPointSprite,uColorMat,uBlurY,uAlphaRef,uAlphaDown;
static GLint uMix,uMixK,uViewport;
static GLint uClipN,uClip[5];   // planos de recorte (ver PlanesClip): equivalente al GL_CLIP_PLANEi del pipeline fijo
static float blurY = 0.0f;
static float alphaRef = 0.0f;
static int   alphaDown = 0;
static GLint uMDiff,uMAmb,uMSpec,uMyME,uShine,uAmbGlobal,uNLights,uLPos,uLAmb,uLDif,uLSpec,uLAtt,uLCone,uMPbr,uEnvTex,uEnvTexB,uEnvF0,uEnvF1,uEnvF2,uScene,uSceneVp,uMExtra,uSceneZ,uProj,uSceneZOn,uMapN,uMapR,uMapM,uMapA,uMapE,uMapOn,uMapEOn,uSHa,uSHb,uSHc,uSHOn,uEyeWorld,
    uSHaV,uSHbV,uSHcV,uSHOnV,uEyeWorldV,uShadowLightV,uShadowNV,uShadowCV,uShadowOn,uShadowLight,uShadowTex,uShadowM0,uShadowM1,uShadowM2,uShadowM3,uShadowBox0,uShadowBox1,uShadowBox2,uShadowBox3,uShadowCuts,uShadowRad,uShadowP,uShadowQ,uShadowBake,uShadowRV,uShadowColor,uShadowBakeMode,uShadowFloor;
static GLint uFogColor,uFogStart,uFogEnd,uFogMode,uFogDens;
// los atributos, en lugares fijos (los mismos en todos los programas, ver linkear)
static const GLint aLpos=0, aLnrm=1, aLuv=2, aLcol=3, aLuv1=4;
static bool ready=false;
// los planos de recorte en OJO (ver PlanesClip), para subirlos al programa de cada draw
static int clipN=0;
static float clipEq[5][4];
// lo que GL tiene en cada atributo (los 5 lugares fijos) y en GL_ARRAY_BUFFER, para no repetir llamadas. El valor
// constante y el prendido son del contexto (no del programa)
struct AttrGL { int on; float cte[4]; bool hasCte; const void* ptr; GLuint vbo; int size; unsigned type; GLboolean norm; int stride; };
static AttrGL attrGL[5];
static GLint bufArrayGL = -1, bufIndicesGL = -1;
static void AttrForget(){ memset(attrGL, 0, sizeof(attrGL)); for(int i=0;i<5;i++) attrGL[i].on = -1; bufArrayGL = bufIndicesGL = -1; }
static void BindArray(GLuint b){ if(bufArrayGL != (GLint)b){ W3D_GLC(glBindBuffer(GL_ARRAY_BUFFER,b)); bufArrayGL = (GLint)b; } }

// ============================================================================
//  Shaders (GLSL ES 1.00). La ILUMINACION va en el VERTEX shader, con la cuenta de OpenGL ES 1.1 (es lo que
//  hace el pipeline fijo: Gouraud), asi una malla se ve igual que en el PC y el N95:
//    color = emision + ambiente_global * amb_mat
//          + suma por luz de atenuacion * cono * ( amb_luz * amb_mat + (N.L) * dif_luz * dif_mat
//                                                 + (N.L > 0 ? (N.H)^brillo * esp_luz * esp_mat : 0) )
//  con H = L + (0,0,1) (sin observador local) y el resultado recortado a [0,1] ANTES de la textura.
// ============================================================================
static const char* VS =
"uniform mat4 uMVP; uniform mat4 uMV; uniform mat3 uNMat; uniform mat4 uTexM; uniform float uPointSize;\n"
"uniform int uTexGen; uniform vec4 uViewport;\n"
// (la luz POR PIXEL la declara y la calcula el fragment shader: que los dos declaren los mismos uniforms con distinta
// precision es un error de linkeo en GLES)
"#ifndef W3D_LIGHT_PIXEL\n"
"uniform int uLightOn; uniform int uColorMat;\n"
"uniform vec4 uMDiff; uniform vec4 uMAmb; uniform vec4 uMSpec; uniform vec4 uMyME; uniform float uShine; uniform vec4 uAmbGlobal;\n"
"uniform int uNLights; uniform vec4 uLPos[8]; uniform vec4 uLAmb[8]; uniform vec4 uLDif[8]; uniform vec4 uLSpec[8];\n"
"uniform vec4 uLAtt[8];\n"    // constante, lineal, cuadratica, exponente del cono
"uniform vec4 uLCone[8];\n"   // direccion del cono (ojo) + coseno del corte (-1 = sin cono)
"#endif\n"
// LAS SOMBRAS EN TIEMPO REAL (W3D_SOMBRA, ver ShadowReceptors): la posicion del pixel en OJO corrida un poco por la normal
// (el sesgo de cada cascada: sin eso la superficie se hace sombra a si misma, el "acne") y, con la luz por vertice, la luz
// que da sombra APARTE (vSomC): el fragment shader la multiplica por la sombra. Los uniforms del VS van con otro nombre que
// los del FS (con distinta precision no linkean en GLES)
"#ifdef W3D_SHADOW\n"
"uniform int uShadowLightV; uniform vec4 uShadowNV; uniform vec4 uShadowCV; uniform float uShadowRV;\n"
"varying vec3 vShadowP; varying vec3 vShadowC;\n"
"#endif\n"
// LA SONDA DE LUZ CON LA LUZ POR VERTICE (W3D_SH_V): la misma cuenta que el fragment shader (SHEval) en cada vertice,
// con otros nombres (los del FS tienen otra precision). La luz de la sonda segun hacia donde mira la normal, sobre el
// color difuso: sin esto un material comun (por vertice) solo recibia el promedio por su ambiente (un 30%)
"#if defined(W3D_SH_V) && defined(W3D_LIGHT) && !defined(W3D_LIGHT_PIXEL)\n"
"uniform vec4 uSHaV[3]; uniform vec4 uSHbV[3]; uniform vec4 uSHcV[3]; uniform float uSHOnV; uniform mat3 uEyeWorldV;\n"
"vec3 SHEvalV(vec3 n){ return uSHaV[0].xyz*0.282095+uSHaV[1].xyz*(0.488603*n.y)+uSHaV[2].xyz*(0.488603*n.z)"
"+uSHbV[0].xyz*(0.488603*n.x)+uSHbV[1].xyz*(1.092548*n.x*n.y)+uSHbV[2].xyz*(1.092548*n.y*n.z)"
"+uSHcV[0].xyz*(0.315392*(3.0*n.z*n.z-1.0))+uSHcV[1].xyz*(1.092548*n.x*n.z)+uSHcV[2].xyz*(0.546274*(n.x*n.x-n.y*n.y)); }\n"
"#endif\n"
"attribute vec3 aPos; attribute vec3 aNrm; attribute vec4 aUV; attribute vec4 aCol; attribute vec2 aUV1;\n"
"varying vec3 vP; varying vec2 vT; varying vec2 vT1; varying vec4 vC; varying float vFogZ;\n"
"varying vec2 vCenter; varying float vSize;\n"   // el centro y el tamanio del punto en pixeles de ventana (para el sprite)
"#ifdef W3D_LIGHT_PIXEL\n"
"varying vec3 vNe; varying vec3 vPe;\n"   // la normal y la posicion en OJO: la luz se calcula en cada pixel
"#endif\n"
"void main(){\n"
"  gl_Position=uMVP*vec4(aPos,1.0);\n"
// (la posicion en OJO solo para lo que la usa: sin eso uMV no queda en el programa y no se sube en cada draw)
"#if defined(W3D_LIGHT) || defined(W3D_TEXGEN) || defined(W3D_CLIP) || defined(W3D_FOG)\n"
"  vec4 eye=uMV*vec4(aPos,1.0);\n"
"#endif\n"
"#ifdef W3D_SPRITE\n"
"  vCenter=(gl_Position.xy/gl_Position.w*0.5+0.5)*uViewport.zw+uViewport.xy; vSize=uPointSize;\n"
"#endif\n"
"#if !defined(W3D_SPECIAL) || defined(W3D_POINTS)\n"   // (el tamanio del punto, solo dibujando puntos)
"  gl_PointSize=uPointSize;\n"
"#endif\n"
"#ifdef W3D_CLIP\n"
"  vP=eye.xyz;\n"
"#endif\n"
"#ifdef W3D_FOG\n"
"  vFogZ=-eye.z;\n"
"#endif\n"
"#ifdef W3D_MIX\n"
"  vT1=aUV1;\n"
"#endif\n"
"#if defined(W3D_LIGHT) || defined(W3D_TEXGEN)\n"
"  vec3 n=uNMat*aNrm; float nl=length(n); vec3 N=(nl>0.0)?n/nl:vec3(0.0,0.0,1.0);\n"   // GL_NORMALIZE
"#endif\n"
"#ifdef W3D_TEX\n"
"  vec4 uv=aUV;\n"
"#ifdef W3D_TEXGEN\n"
"  if(uTexGen==1){\n"   // GL_SPHERE_MAP: el reflejo de la vista en la normal (en ojo)
"    vec3 r=reflect(normalize(eye.xyz),N); float m=2.0*sqrt(r.x*r.x+r.y*r.y+(r.z+1.0)*(r.z+1.0));\n"
"    uv=vec4(r.x/m+0.5,r.y/m+0.5,0.0,1.0);\n"
"  }\n"
"#endif\n"
"#if defined(W3D_SPECIAL) && !defined(W3D_TEXM)\n"   // (la matriz de textura es la identidad)
"  vT=uv.xy;\n"
"#else\n"
"  vec4 t=uTexM*uv; vT=t.xy/t.w;\n"
"#endif\n"
"#endif\n"
"#if defined(W3D_MAPS) && !defined(W3D_TEX)\n"
"  { vec4 t=uTexM*aUV; vT=t.xy/t.w; }\n"   // (los mapas del material leen la UV aunque no haya textura base, con su transformacion)
"#endif\n"
"  vec4 c=aCol;\n"
"#ifdef W3D_LIGHT_PIXEL\n"
"  vNe=N; vPe=eye.xyz;\n"
"#endif\n"
"#ifdef W3D_SHADOW\n"
"  { float dz=-eye.z; float k=(dz<uShadowCV.x)?uShadowNV.x:(dz<uShadowCV.y)?uShadowNV.y:(dz<uShadowCV.z)?uShadowNV.z:uShadowNV.w;\n"
"    k*=1.0+uShadowRV*dz;\n"   // (con el desenfoque, el filtro es mas ancho lejos: mas corrimiento)
"    vShadowP=eye.xyz+N*k; vShadowC=vec3(0.0); }\n"
"#define ADD_LIGHT(i,v) if(i==uShadowLightV) vShadowC+=v; else acc+=v;\n"
"#else\n"
"#define ADD_LIGHT(i,v) acc+=v;\n"
"#endif\n"
"#if defined(W3D_LIGHT) && !defined(W3D_LIGHT_PIXEL)\n"
"  if(uLightOn==1){\n"
"    vec4 md=(uColorMat==1)?aCol:uMDiff; vec4 ma=(uColorMat==1)?aCol:uMAmb;\n"   // COLOR_MATERIAL = AMBIENT_AND_DIFFUSE
"    vec3 acc=uMyME.rgb+ma.rgb*uAmbGlobal.rgb;\n"
"#ifdef W3D_SH_V\n"   // (una sonda de luz manda sobre el ambiente, como en la luz por pixel)
"    if(uSHOnV>0.5) acc=uMyME.rgb+md.rgb*max(SHEvalV(normalize(uEyeWorldV*N)),vec3(0.0));\n"
"#endif\n"
// las luces una por una (LUCES = LUZ(0) LUZ(1) ..., ver DefsProgram) y no con un for: el compilador del driver Broadcom
// (VideoCore IV, Android 2.3) desenrolla mal ese for y no linkea el programa ("Indexing with an integral constant
// expression greater than declared size"). La macro va en UNA linea (sin \n entre los pedazos): GLSL ES 1.00 no
// continua lineas con '\'
"#define LUZ(i) if(i<uNLights){ vec3 L; float at=1.0;"
"      if(uLPos[i].w!=0.0){ vec3 d=uLPos[i].xyz/uLPos[i].w-eye.xyz; float dist=length(d); L=d/max(dist,1e-6);"
"        at=1.0/max(uLAtt[i].x+uLAtt[i].y*dist+uLAtt[i].z*dist*dist,1e-6); }"
"      else L=normalize(uLPos[i].xyz);"
"      if(uLCone[i].w>-0.5){ float sd=dot(-L,normalize(uLCone[i].xyz)); at*=(sd>=uLCone[i].w)?pow(max(sd,0.0),uLAtt[i].w):0.0; }"
"      float ndl=max(dot(N,L),0.0);"
"      vec3 col=ma.rgb*uLAmb[i].rgb+md.rgb*uLDif[i].rgb*ndl;"
"      if(ndl>0.0){ float nh=max(dot(N,normalize(L+vec3(0.0,0.0,1.0))),0.0);"
"        col+=uMSpec.rgb*uLSpec[i].rgb*((uShine>0.0)?pow(nh,uShine):1.0); }"
"      ADD_LIGHT(i,at*col) }\n"
"    LUCES\n"
"    c=vec4(acc,md.a);\n"
"  }\n"
"#endif\n"
"  vC=clamp(c,0.0,1.0);\n"
"}\n";
static const char* FS =
"#ifdef GL_ES\n"
"precision mediump float;\n"
"precision mediump int;\n"
"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
"#define W3D_HIGHP highp\n"
"#else\n"
"#define W3D_HIGHP mediump\n"
"#endif\n"
"#else\n"
"#define W3D_HIGHP\n"
"#endif\n"
// LOS PEDAZOS DE LA LUZ PBR POR PIXEL: el programa ENTERO los tiene todos (se prenden con uniforms); con un programa por
// draw (W3D_ESPECIAL: la VideoCore IV de Android 2.3) solo los que usa ese draw (ver PickProgram). Con todos juntos
// el driver del Galaxy Ace no podia dibujar la malla (GL_OUT_OF_MEMORY en el glDrawElements y la pantalla en negro)
"#ifndef W3D_SPECIAL\n#define W3D_PBR\n#define W3D_ENVIRONMENT\n#define W3D_SCREEN\n#endif\n"
"uniform int uUseTex; uniform int uEnv; uniform int uFogOn;\n"
"uniform float uBlurY;\n"   // blur vertical (0 = apagado)
"uniform float uAlphaRef;\n" // descarte por alpha (0 = apagado)
"uniform int uAlphaDown;\n"  // 1 = el ALPHA viene en la mitad de ABAJO de la textura
"uniform int uPointSprite;\n"
"uniform sampler2D uTex; uniform sampler2D uTex1; uniform int uMix; uniform float uMixK;\n"
"uniform vec4 uFogColor; uniform float uFogStart; uniform float uFogEnd; uniform int uFogMode; uniform float uFogDens;\n"
"uniform int uClipN; uniform vec4 uClip0,uClip1,uClip2,uClip3,uClip4;\n"   // planos de recorte, en OJO
"#ifdef W3D_MAPS\n"   // los mapas del material (x normal, y rugosidad, z metalico, w alfa; uMapEOn emision): 1 = con textura
"uniform sampler2D uMapN; uniform sampler2D uMapR; uniform sampler2D uMapM; uniform sampler2D uMapA; uniform sampler2D uMapE;\n"
"uniform vec4 uMapOn; uniform float uMapEOn;\n"
"#endif\n"
// LAS SOMBRAS EN TIEMPO REAL: el mapa de sombras (la profundidad vista desde la luz, en RGBA de 8 bits: anda en cualquier
// ES 2.0) con hasta 4 CASCADAS una al lado de la otra. Cada una: la matriz OJO -> (u, v, profundidad) y su rectangulo en el
// mapa. Cortes: hasta donde llega cada una (distancia a la camara). Rad: el desenfoque de cada una en texeles (0 = el
// filtro de 2x2). P: el tamanio del mapa y donde empieza a desvanecerse / la distancia maxima. Q: intensidad, -, el sesgo
// de la profundidad, suaves (1) o duras (0)
"#ifdef W3D_SHADOW\n"
"uniform float uShadowOn; uniform int uShadowLight; uniform sampler2D uShadowTex;\n"
"uniform W3D_HIGHP mat4 uShadowM0; uniform W3D_HIGHP mat4 uShadowM1; uniform W3D_HIGHP mat4 uShadowM2; uniform W3D_HIGHP mat4 uShadowM3;\n"
"uniform W3D_HIGHP vec4 uShadowBox0; uniform W3D_HIGHP vec4 uShadowBox1; uniform W3D_HIGHP vec4 uShadowBox2; uniform W3D_HIGHP vec4 uShadowBox3;\n"
"uniform W3D_HIGHP vec4 uShadowCuts; uniform vec4 uShadowRad; uniform W3D_HIGHP vec4 uShadowP; uniform W3D_HIGHP vec4 uShadowQ;\n"
"uniform float uShadowBake;\n"   // 1 = sobre la luz HORNEADA: solo tapa lo que se mueve (la profundidad de BA)
// (y DONDE esta la luz horneada: 1 = no se sabe -se oscurece el color-, 2 = en la unidad 1, 3 / 4 = es lo que dibuja esta
//  pasada, Multiply / Multiply 2x. Piso: rgb hasta donde baja sin mascara -el ambiente mas el color-, a = hay mascara)
"uniform float uShadowBakeMode; uniform vec4 uShadowFloor;\n"
"uniform vec4 uShadowColor;\n"   // el COLOR de la sombra (negro = sin luz; azulado = la luz del cielo que llega igual)
"varying W3D_HIGHP vec3 vShadowP; varying vec3 vShadowC;\n"
// la luz en un texel: DOS profundidades de 16 bits. RG: lo mas cercano al sol de todo lo que proyecta; BA: lo mas cercano
// de lo que SE MUEVE (su pasada aparte, SombrasTiempoReal). Sobre la luz horneada cuenta solo BA: la sombra de lo
// estatico ya esta en la textura, y si lo estatico tapaba a lo que se mueve (un solo valor: el mas cercano) la sombra de
// lo que se mueve desaparecia ahi y asomaba la penumbra horneada con el borde duro del mapa
"W3D_HIGHP float ShadowZ(vec4 t){ return dot((uShadowBake>0.5)?t.ba:t.rg,vec2(1.0,1.0/255.0)); }\n"
"float ShadowLight(W3D_HIGHP vec2 uv,W3D_HIGHP float z){ return step(z,ShadowZ(texture2D(uShadowTex,uv))); }\n"
// el filtro de 2x2 con los pesos de la interpolacion bilineal (como el PCF de las GPU con textura de profundidad), sin
// salirse del rectangulo de la cascada
"float ShadowPCF(W3D_HIGHP vec2 uv,W3D_HIGHP float z,W3D_HIGHP vec4 box){\n"
"  W3D_HIGHP vec2 d=1.0/uShadowP.xy; W3D_HIGHP vec2 lo=box.xy+0.5*d; W3D_HIGHP vec2 hi=box.zw-0.5*d;\n"
"  if(uShadowQ.w<0.5) return ShadowLight(clamp(uv,lo,hi),z);\n"
"  W3D_HIGHP vec2 t=uv*uShadowP.xy-0.5; vec2 f=fract(t); W3D_HIGHP vec2 b=(floor(t)+0.5)*d;\n"
"  float s0=ShadowLight(clamp(b,lo,hi),z); float s1=ShadowLight(clamp(b+vec2(d.x,0.0),lo,hi),z);\n"
"  float s2=ShadowLight(clamp(b+vec2(0.0,d.y),lo,hi),z); float s3=ShadowLight(clamp(b+d,lo,hi),z);\n"
"  return mix(mix(s0,s1,f.x),mix(s2,s3,f.x),f.y); }\n"
// LA PENUMBRA DE CONTACTO (PCSS, la de NVIDIA): donde la sombra toca lo que la proyecta es NITIDA y se va difuminando a
// medida que se aleja (la base de un poste nitida, la punta de su sombra borrosa), como la de un sol que no es un punto.
// 1) se busca alrededor (3 x 3, hasta la penumbra maxima) la profundidad promedio de lo que TAPA la luz; 2) la penumbra
// es esa distancia (receptor - lo que tapa) por k (los texeles de penumbra por unidad de profundidad: el tamanio del sol);
// 3) el filtro de 2x2 en 8 lugares de un anillo de ese radio. k = 0: la sombra de siempre (nitida o con el 2x2)
"W3D_HIGHP float ShadowBloq(W3D_HIGHP vec2 uv,W3D_HIGHP float z,W3D_HIGHP vec4 box,float r){\n"
"  W3D_HIGHP vec2 d=1.0/uShadowP.xy; W3D_HIGHP vec2 lo=box.xy+0.5*d; W3D_HIGHP vec2 hi=box.zw-0.5*d;\n"
"  W3D_HIGHP float acc=0.0; float n=0.0;\n"
"  for(int j=-1;j<=1;j++) for(int i=-1;i<=1;i++){\n"
"    W3D_HIGHP float zb=ShadowZ(texture2D(uShadowTex,clamp(uv+vec2(float(i),float(j))*r*d,lo,hi)));\n"
"    float c=step(zb,z);\n"
"    acc+=zb*c; n+=c; }\n"
"  return (n>0.0)?acc/n:-1.0; }\n"
// la penumbra ancha: 16 muestras en un disco de Poisson GIRADO al azar en cada pixel (sin el giro se veian copias
// escalonadas de la sombra; con el, un granulado fino que se funde)
"float ShadowDisk(W3D_HIGHP vec2 uv,W3D_HIGHP float z,W3D_HIGHP vec4 box,float rad){\n"
"  W3D_HIGHP vec2 d=1.0/uShadowP.xy; W3D_HIGHP vec2 lo=box.xy+0.5*d; W3D_HIGHP vec2 hi=box.zw-0.5*d;\n"
"  float a=6.2831853*fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453);\n"
"  vec2 cs=vec2(cos(a),sin(a)); W3D_HIGHP vec2 o=rad*d; float s=0.0;\n"
"#define SPD(pa,pb) s+=ShadowLight(clamp(uv+vec2(cs.x*(pa)-cs.y*(pb),cs.y*(pa)+cs.x*(pb))*o,lo,hi),z);\n"
"  SPD(-0.9420,-0.3991)\n"
"  SPD(0.9456,-0.7689)\n"
"  SPD(-0.0942,-0.9294)\n"
"  SPD(0.3450,0.2939)\n"
"  SPD(-0.9159,0.4577)\n"
"  SPD(-0.8154,-0.8791)\n"
"  SPD(-0.3828,0.2768)\n"
"  SPD(0.9748,0.7565)\n"
"  SPD(0.4432,-0.9751)\n"
"  SPD(0.5374,-0.4737)\n"
"  SPD(-0.2650,-0.4189)\n"
"  SPD(0.7920,0.1909)\n"
"  SPD(-0.2419,0.9971)\n"
"  SPD(-0.8141,0.9144)\n"
"  SPD(0.1998,0.7864)\n"
"  SPD(0.1438,-0.1410)\n"
"  return s*0.0625; }\n"
"float ShadowCascade(W3D_HIGHP mat4 M,W3D_HIGHP vec4 box,float k){\n"
"  W3D_HIGHP vec3 s=(M*vec4(vShadowP,1.0)).xyz;\n"
"  if(s.z>=1.0) return 1.0;\n"
"  W3D_HIGHP float z=s.z-uShadowQ.z;\n"
"  if(k<=0.0 || uShadowQ.y<0.5) return ShadowPCF(s.xy,z,box);\n"
"  W3D_HIGHP float zb=ShadowBloq(s.xy,z,box,uShadowQ.y*0.5);\n"
"  if(zb<0.0) return ShadowPCF(s.xy,z,box);\n"
"  float rad=min((z-zb)*k,uShadowQ.y);\n"
"  if(rad<0.75) return ShadowPCF(s.xy,z,box);\n"
"  return mix(ShadowPCF(s.xy,z,box),ShadowDisk(s.xy,z,box,rad),clamp(rad-0.75,0.0,1.0)); }\n"
// la luz que llega (1 = toda, 0 = sombra entera por la intensidad): la cascada de su distancia y el BORDE (se desvanece
// hasta la distancia maxima, sin un corte)
"float Shadow(){\n"
"  W3D_HIGHP float dz=-vShadowP.z; float s;\n"
"  if(dz>=uShadowP.w) return 1.0;\n"
"  if(dz<uShadowCuts.x) s=ShadowCascade(uShadowM0,uShadowBox0,uShadowRad.x);\n"
"  else if(dz<uShadowCuts.y) s=ShadowCascade(uShadowM1,uShadowBox1,uShadowRad.y);\n"
"  else if(dz<uShadowCuts.z) s=ShadowCascade(uShadowM2,uShadowBox2,uShadowRad.z);\n"
"  else s=ShadowCascade(uShadowM3,uShadowBox3,uShadowRad.w);\n"
"  return mix(s,1.0,clamp((dz-uShadowP.z)/max(uShadowP.w-uShadowP.z,1e-4),0.0,1.0)); }\n"
// la luz que queda en la sombra: de toda (1) al COLOR de la sombra, por la intensidad. 'piso' = lo que queda de la luz
// sobre la luz HORNEADA (la indirecta tambien esta en la textura: un 25% aunque la sombra sea negra)
"vec3 ShadowF(float s,float floor){ return mix(vec3(1.0),floor+(1.0-floor)*uShadowColor.rgb,uShadowQ.x*(1.0-s)); }\n"
// LA SOMBRA DE LO QUE SE MUEVE SOBRE LA LUZ HORNEADA (como el "Shadowmask" de Unity): L es la luz de la textura horneada
// en el texel y 'a' su MASCARA DEL SOL (el alfa: que fraccion de L es la directa del sol). La sombra le saca solo esa
// parte (tenida por el color, por la intensidad): queda la luz del ambiente y de los rebotes, la misma que hay en la
// sombra horneada, y donde las dos se tocan no hay costura ni se oscurece dos veces. Sin mascara (un horneado de antes)
// baja hasta el PISO -el ambiente- sin pasarse: lo que ya estaba mas oscuro (la sombra horneada) no se toca
"vec3 ShadowBakeL(vec3 L,float a){ float q=uShadowQ.x*(1.0-Shadow());\n"
"  if(uShadowFloor.a>0.5) return L*(vec3(1.0)-a*q*(vec3(1.0)-uShadowColor.rgb));\n"
"  return min(L,mix(L,uShadowFloor.rgb,q)); }\n"
"#endif\n"
"varying vec3 vP; varying vec2 vT; varying vec2 vT1; varying vec4 vC; varying float vFogZ;\n"
"varying W3D_HIGHP vec2 vCenter; varying W3D_HIGHP float vSize;\n"
"#ifdef W3D_LIGHT_PIXEL\n"
"uniform int uLightOn; uniform int uColorMat;\n"
"uniform W3D_HIGHP vec4 uMDiff; uniform W3D_HIGHP vec4 uMAmb; uniform W3D_HIGHP vec4 uMSpec; uniform W3D_HIGHP vec4 uMyME;\n"
"uniform W3D_HIGHP float uShine; uniform W3D_HIGHP vec4 uAmbGlobal; uniform int uNLights;\n"
"uniform W3D_HIGHP vec4 uLPos[W3D_NLIGHTS]; uniform W3D_HIGHP vec4 uLAmb[W3D_NLIGHTS]; uniform W3D_HIGHP vec4 uLDif[W3D_NLIGHTS];\n"
"uniform W3D_HIGHP vec4 uLSpec[W3D_NLIGHTS]; uniform W3D_HIGHP vec4 uLAtt[W3D_NLIGHTS]; uniform W3D_HIGHP vec4 uLCone[W3D_NLIGHTS];\n"
"varying W3D_HIGHP vec3 vNe; varying W3D_HIGHP vec3 vPe;\n"
"#ifdef W3D_PBR\n"
"uniform vec4 uMPbr;\n"   // los valores PBR (MaterialPBR): x metalico, y rugosidad (<0 = apagado), z F0 del IOR, w transmision
// LA SONDA DE LUZ del objeto (AmbientSH): 9 coeficientes RGB de armonicos esfericos (en el MUNDO, con la convolucion del
// coseno) y la matriz ojo -> mundo. La luz difusa que llega a una normal n del mundo es sum c_i * Y_i(n)
"uniform vec4 uSHa[3]; uniform vec4 uSHb[3]; uniform vec4 uSHc[3]; uniform float uSHOn; uniform mat3 uEyeWorld;\n"
"vec3 SHEval(vec3 n){ return uSHa[0].xyz*0.282095+uSHa[1].xyz*(0.488603*n.y)+uSHa[2].xyz*(0.488603*n.z)"
"+uSHb[0].xyz*(0.488603*n.x)+uSHb[1].xyz*(1.092548*n.x*n.y)+uSHb[2].xyz*(1.092548*n.y*n.z)"
"+uSHc[0].xyz*(0.315392*(3.0*n.z*n.z-1.0))+uSHc[1].xyz*(1.092548*n.x*n.z)+uSHc[2].xyz*(0.546274*(n.x*n.x-n.y*n.y)); }\n"
"#endif\n"
"#ifdef W3D_ENVIRONMENT\n"
// EL ENTORNO COMO CUBEMAP (las GPU chicas, ver EnvironmentCube): el cubo se lee con la direccion, sin cuentas
"#ifdef W3D_ENVIRONMENT_CUBE\n"
"uniform samplerCube uEnvTex; uniform samplerCube uEnvTexB; uniform vec4 uEnvF0; uniform vec4 uEnvF1; uniform vec4 uEnvF2;\n"
"vec3 EnvDir(vec3 d){ return vec3(dot(uEnvF0.xyz,d),dot(uEnvF1.xyz,d),dot(uEnvF2.xyz,d)); }\n"
"#define ENV_TYPE vec3\n#define ENV_COORD(d) EnvDir(d)\n#define ENV_TEX(t,c) textureCube(t,c)\n#define ENV_TEX_LOD(t,c,b) textureCube(t,c,b)\n"
"#else\n"
"uniform sampler2D uEnvTex; uniform sampler2D uEnvTexB; uniform vec4 uEnvF0; uniform vec4 uEnvF1; uniform vec4 uEnvF2;\n"
"#define ENV_TYPE vec2\n#define ENV_COORD(d) EnvUV(d)\n#define ENV_TEX(t,c) texture2D(t,c)\n#define ENV_TEX_LOD(t,c,b) texture2D(t,c,b)\n"
// la direccion del OJO -> el punto de la imagen 360 (la misma cuenta que W3dBackgroundDirectionUV: el cielo y el trazado)
"#ifdef W3D_SPECIAL\n"
// (un programa por draw, la VideoCore IV: atan y acos no estan en la GPU y el compilador los arma con muchas cuentas.
//  Aproximados con polinomios: el error, menos de un pixel de la copia borrosa)
"float W3dAtan2(float y,float x){ float ax=abs(x),ay=abs(y); float a=min(ax,ay)/max(max(ax,ay),1e-6); float s=a*a;"
" float r=((-0.0464964749*s+0.15931422)*s-0.327622764)*s*a+a; if(ay>ax) r=1.57079637-r; if(x<0.0) r=3.14159274-r;"
" return (y<0.0)?-r:r; }\n"
"float W3dAcos(float x){ float ax=abs(x); float r=sqrt(1.0-ax)*(1.5707288+ax*(-0.2121144+ax*(0.0742610-0.0187293*ax)));"
" return (x<0.0)?3.14159274-r:r; }\n"
"vec2 EnvUV(vec3 d){ vec3 q=vec3(dot(uEnvF0.xyz,d),dot(uEnvF1.xyz,d),dot(uEnvF2.xyz,d));"
" return vec2(0.5+W3dAtan2(q.y,q.x)*0.15915494,W3dAcos(clamp(q.z,-1.0,1.0))*0.31830989); }\n"
"#else\n"
"vec2 EnvUV(vec3 d){ vec3 q=vec3(dot(uEnvF0.xyz,d),dot(uEnvF1.xyz,d),dot(uEnvF2.xyz,d));"
" return vec2(0.5+atan(q.y,q.x)*0.15915494,acos(clamp(q.z,-1.0,1.0))*0.31830989); }\n"
"#endif\n"
"#endif\n"   // (W3D_ENTORNO_CUBO)
"#endif\n"
"#ifdef W3D_SCREEN\n"
"uniform sampler2D uScene; uniform vec4 uSceneVp; uniform vec4 uMExtra;\n"
// LA PROFUNDIDAD de la copia (GL de escritorio): el punto de la copia -> su z en OJO (negativa delante de la camara), y
// un punto en OJO -> su lugar en la copia (la proyeccion del pase)
"#ifdef W3D_SSR_Z\n"
"uniform sampler2D uSceneZ; uniform mat4 uProj; uniform float uSceneZOn;\n"
"vec2 EscSp(vec2 s){ return (uSceneVp.w<0.0)?vec2(s.x,1.0-s.y):s; }\n"   // (la copia dada vuelta)
"float EscZ(vec2 s){ vec2 q=EscSp(s); if(uSceneZOn>1.5) q.y=1.0-q.y; float d=texture2D(uSceneZ,q).r*2.0-1.0;"
" return (uProj[2][3]!=0.0)?-uProj[3][2]/(d+uProj[2][2]):(d-uProj[3][2])/uProj[2][2]; }\n"
"vec2 EscProj(vec3 p){ vec4 c=uProj*vec4(p,1.0); return c.xy/c.w*0.5+0.5; }\n"
"#endif\n"
"#endif\n"
"#endif\n"
// LA LUZ DE AREA (Light.h; W3D_AREA, ver DefsProgram). Llega en los arreglos de siempre (UploadLights): uLPos = el centro,
// uLCone = hacia donde alumbra (w = -2: es de area), uLAtt = el medio ancho (xyz, en OJO) y el medio alto (w; negativo =
// elipse). La DIFUSA es la cuenta EXACTA de la superficie: el factor de forma del poligono (el rectangulo, o la elipse
// como un octogono de la misma superficie) por sus aristas, recortado por el horizonte del pixel (la integral de cada
// arista sin acos, la de Heitz), en las unidades de una puntual (I / d^2 de lejos). El BRILLO va con el PUNTO
// REPRESENTATIVO (Karis, UE4): el punto de la lampara mas cerca del reflejo, como una puntual que mira para alla; 'aw' =
// cuanto abre el brillo PBR lo que tapa la lampara (que de cerca no queme)
"#ifdef W3D_AREA\n"
"W3D_HIGHP vec3 AreaArista(W3D_HIGHP vec3 a,W3D_HIGHP vec3 b){ W3D_HIGHP float x=dot(a,b); W3D_HIGHP float y=abs(x);"
" W3D_HIGHP float v=(0.8543985+(0.4965155+0.0145206*y)*y)/(3.4175940+(4.1616724+y)*y);"
" return cross(a,b)*((x>0.0)?v:0.5*inversesqrt(max(1.0-x*x,1e-7))-v); }\n"
// (sin lo que queda bajo el horizonte; 'sal' / 'ent' = donde el poligono lo cruza, para cerrar el recortado por el horizonte)
"void AreaAristaH(W3D_HIGHP vec3 a,W3D_HIGHP vec3 b,W3D_HIGHP vec3 N,inout W3D_HIGHP vec3 F,inout W3D_HIGHP vec3 sal,inout W3D_HIGHP vec3 ent){\n"
"  W3D_HIGHP float za=dot(N,a); W3D_HIGHP float zb=dot(N,b);\n"
"  if(za>=0.0 && zb>=0.0){ F+=AreaArista(a,b); return; }\n"
"  if(za<0.0 && zb<0.0) return;\n"
"  W3D_HIGHP vec3 x=normalize((za<zb)?a*zb-b*za:b*za-a*zb);\n"
"  if(za>=0.0){ F+=AreaArista(a,x); sal=x; } else { F+=AreaArista(x,b); ent=x; } }\n"
"W3D_HIGHP float AreaLuz(W3D_HIGHP vec3 c,W3D_HIGHP vec3 nl,W3D_HIGHP vec4 ax4,W3D_HIGHP vec3 p,W3D_HIGHP vec3 N,out W3D_HIGHP vec3 L,out float at,out float aw){\n"
"  nl=normalize(nl); L=-nl; at=0.0; aw=0.0;\n"
"  W3D_HIGHP vec3 ax=ax4.xyz; W3D_HIGHP float hx=length(ax); W3D_HIGHP float hy=abs(ax4.w); bool elip=ax4.w<0.0;\n"
"  W3D_HIGHP vec3 xd=ax/max(hx,1e-6); W3D_HIGHP vec3 yd=cross(xd,nl); W3D_HIGHP vec3 ay=yd*hy;\n"
"  W3D_HIGHP float A=(elip?3.14159265:4.0)*hx*hy; W3D_HIGHP float h=dot(p-c,nl);\n"
"  if(h<=0.0 || A<=0.0) return 0.0;\n"   // (detras de la lampara: alumbra de un solo lado)
"  W3D_HIGHP vec3 F=vec3(0.0); W3D_HIGHP vec3 sal=vec3(0.0); W3D_HIGHP vec3 ent=vec3(0.0);\n"
"  if(!elip){ W3D_HIGHP vec3 v0=normalize(c+ax+ay-p); W3D_HIGHP vec3 v1=normalize(c-ax+ay-p);\n"
"    W3D_HIGHP vec3 v2=normalize(c-ax-ay-p); W3D_HIGHP vec3 v3=normalize(c+ax-ay-p);\n"
"    AreaAristaH(v0,v1,N,F,sal,ent); AreaAristaH(v1,v2,N,F,sal,ent); AreaAristaH(v2,v3,N,F,sal,ent); AreaAristaH(v3,v0,N,F,sal,ent); }\n"
"  else { W3D_HIGHP vec3 va=normalize(c+ax*1.0539-p);\n"   // (el octogono con la superficie de la elipse: 1.0539 = sqrt(pi / (2 sqrt 2)))
"    for(int k=1;k<=8;k++){ float a=float(k)*0.78539816;\n"
"      W3D_HIGHP vec3 vb=normalize(c+(ax*cos(a)+ay*sin(a))*1.0539-p); AreaAristaH(va,vb,N,F,sal,ent); va=vb; } }\n"
"  if(dot(sal,sal)>0.5 && dot(ent,ent)>0.5) F+=AreaArista(sal,ent);\n"
// el punto representativo: donde el reflejo corta el plano de la lampara, llevado adentro de su forma
"  W3D_HIGHP vec3 R=reflect(normalize(p),N); W3D_HIGHP float dr=dot(R,nl);\n"
"  W3D_HIGHP vec3 q=((dr<-1e-4)?p-R*(h/dr):p+R*length(p-c))-c;\n"
"  W3D_HIGHP float u=dot(q,xd); W3D_HIGHP float w=dot(q,yd);\n"
"  if(!elip){ u=clamp(u,-hx,hx); w=clamp(w,-hy,hy); }\n"
"  else { W3D_HIGHP float le=length(vec2(u/hx,w/hy)); if(le>1.0){ u/=le; w/=le; } }\n"
"  W3D_HIGHP vec3 s=c+xd*u+yd*w-p; W3D_HIGHP float ds=length(s); L=s/max(ds,1e-6);\n"
"  at=max(dot(-L,nl),0.0)/(ds*ds+A*0.31830989);\n"   // (como una puntual de esa intensidad que mira para alla; de cerca no explota)
"  aw=sqrt(A*0.31830989)/(2.0*max(ds,1e-4));\n"
"  return max(dot(N,F),0.0)*3.14159265/A; }\n"
"#define AREA_VARS W3D_HIGHP float ed=0.0; float dk=1.0; float aw=0.0;\n"
"#define AREA_LIGHT(i) if(uLCone[i].w<-1.5){ ed=AreaLuz(uLPos[i].xyz,uLCone[i].xyz,uLAtt[i],eye,N,L,at,aw); dk=0.0; }\n"
"#define AREA_DK *dk\n"
"#define AREA_DIF(i) +md.rgb*uLDif[i].rgb*(ed*kd)\n"
"#define AREA_EXPO ((aw>0.0)?2.0/(min(rugP+aw,1.0)*min(rugP+aw,1.0))-1.0:expo)\n"
"#else\n"
"#define AREA_VARS\n#define AREA_LIGHT(i)\n#define AREA_DK\n#define AREA_DIF(i)\n#define AREA_EXPO expo\n"
"#endif\n"
"void main(){\n"
"#ifdef W3D_CLIP\n"
"  vec4 Pe = vec4(vP,1.0);\n"   // recorte por planos (desenrollado: GLSL ES 1.00 no indexa uniforms por variable)
"  if(uClipN>0 && dot(Pe,uClip0)<0.0) discard;\n"
"  if(uClipN>1 && dot(Pe,uClip1)<0.0) discard;\n"
"  if(uClipN>2 && dot(Pe,uClip2)<0.0) discard;\n"
"  if(uClipN>3 && dot(Pe,uClip3)<0.0) discard;\n"
"  if(uClipN>4 && dot(Pe,uClip4)<0.0) discard;\n"
"#endif\n"
"#ifdef W3D_TEX\n"
"  vec2 uv = vT;\n"
// POINT SPRITE (iconos: luz, origen, cursor 3D): la coordenada adentro del punto sale de la posicion del pixel respecto
// del centro del punto, no de gl_PointCoord: el driver iris de Mesa (Intel, Linux) la entrega en 0,0 -tambien a traves de
// Chrome- y el icono salia invisible (se leia siempre la esquina transparente). gl_FragCoord anda en todos los drivers.
"#ifdef W3D_SPRITE\n"
"  if(uPointSprite==1){ W3D_HIGHP vec2 d=(gl_FragCoord.xy-vCenter)/vSize; uv=vec2(0.5+d.x,0.5-d.y); }\n"
"#endif\n"
"#endif\n"
"  vec4 cPrim = vC;\n"  // el color primario: la luz por vertice (interpolada), o el color del vertice
"#if defined(W3D_SHADOW) && !defined(W3D_LIGHT_PIXEL)\n"   // (la luz que da sombra vino aparte: por la sombra)
"  cPrim.rgb=min(cPrim.rgb+vShadowC*((uShadowOn>0.5)?ShadowF(Shadow(),0.0):vec3(1.0)),1.0);\n"
"#endif\n"
"#if defined(W3D_LIGHT_PIXEL) && defined(W3D_PBR)\n"
"  vec3 brightnessPbr=vec3(0.0);\n"   // el brillo PBR va DESPUES de la textura (no lo apaga una textura oscura)
"#endif\n"
// LUZ POR PIXEL: la MISMA cuenta que la del vertex shader (ver el comentario de arriba), con la normal y la posicion
// interpoladas en cada pixel (el brillo especular y las luces cercanas salen redondos en vez de facetados)
"#ifdef W3D_LIGHT_PIXEL\n"
"  if(uLightOn==1){\n"
"    W3D_HIGHP vec3 N=normalize(vNe); W3D_HIGHP vec3 eye=vPe;\n"
// EL NORMAL MAP: la normal de la textura (tangente) llevada al espacio del ojo con la base que sale de las derivadas de la
// posicion y de la UV en la pantalla (sin tangentes por vertice). Como en Blender: verde para arriba (OpenGL)
"#ifdef W3D_MAPS\n"
"    if(uMapOn.x>0.5){ vec3 tn=texture2D(uMapN,vT).xyz*2.0-1.0;\n"
"      W3D_HIGHP vec3 dp1=dFdx(eye); W3D_HIGHP vec3 dp2=dFdy(eye); vec2 du1=dFdx(vT); vec2 du2=dFdy(vT);\n"
"      W3D_HIGHP vec3 d2p=cross(dp2,N); W3D_HIGHP vec3 d1p=cross(N,dp1);\n"
"      W3D_HIGHP vec3 T=d2p*du1.x+d1p*du2.x; W3D_HIGHP vec3 B=d2p*du1.y+d1p*du2.y;\n"
"      float im=inversesqrt(max(max(dot(T,T),dot(B,B)),1e-20));\n"
"      N=normalize(T*im*tn.x+B*im*tn.y+N*tn.z); }\n"
"#endif\n"
"    vec4 md=(uColorMat==1)?vC:uMDiff; vec4 ma=(uColorMat==1)?vC:uMAmb;\n"
"#ifdef W3D_MAPS\n"
"    vec3 acc=((uMapEOn>0.5)?texture2D(uMapE,vT).rgb*uMyME.a:uMyME.rgb)+ma.rgb*uAmbGlobal.rgb;\n"   // (la emision de su textura, x su fuerza)
"#else\n"
"    vec3 acc=uMyME.rgb+ma.rgb*uAmbGlobal.rgb;\n"
"#endif\n"
// PBR (los valores del material que usa el trazado de rayos): el METALICO apaga el difuso y tine el brillo con el
// color; la RUGOSIDAD abre el brillo (exponente 2/r^2-1, Blinn-Phong normalizado); el IOR da el F0 de Fresnel (mas
// reflejo de costado); la TRANSMISION hace transparente lo que no refleja. Sin PBR (rug < 0): la luz de siempre
"#ifdef W3D_PBR\n"
"    bool pbr=uMPbr.y>=0.0;\n"
"    vec4 mp=uMPbr;\n"   // (los valores PBR: el metalico y la rugosidad pueden venir de su textura)
"#ifdef W3D_MAPS\n"
"    if(uMapOn.y>0.5) mp.y=texture2D(uMapR,vT).r;\n"
"    if(uMapOn.z>0.5) mp.x=texture2D(uMapM,vT).r;\n"
"#endif\n"
"    W3D_HIGHP vec3 V=normalize(-eye);\n"
// (PBR de las dos caras, como el trazado de rayos: la normal mira hacia la camara. Un plano visto de atras
//  reflejaba lo de abajo en vez del cielo)
"    if(pbr && dot(N,V)<0.0) N=-N;\n"
"    float rugP=max(mp.y,0.05); float expo=min(2.0/(rugP*rugP)-1.0,512.0);\n"
// (IOR 1 con transmision llega con F0 = 0: no refleja nada, ni de costado: transparente perfecto)
"    float borderF=(mp.z>0.0 || mp.x>0.0)?pow(1.0-max(dot(N,V),0.0),5.0):0.0;\n"
"    float fres=mp.z+(1.0-mp.z)*borderF;\n"
// FRESNEL (Schlick), el de PBR: de frente refleja F0 (el del IOR en lo que no es metal; el color en el metal) y hacia
// los bordes -mirando de costado- sube hasta reflejar todo, en blanco. El agua: de frente se ve a traves, de costado refleja
"    float weightB=1.0;\n"
"    vec3 f0B=mix(vec3(mp.z),md.rgb,mp.x);\n"
"    vec3 colB=f0B+(vec3(1.0)-f0B)*borderF;\n"
"    float kd=pbr?(1.0-mp.x):1.0;\n"
// EL AMBIENTE PBR es la LUZ DEL FONDO (EscenaRender: BackgroundAmbient) sobre el color difuso, como en Blender: el color
// del fondo, o la irradiancia del 360 en la direccion de la normal por su factor (el alfa del ambiente). Sin PBR sigue el
// ambiente del material por el global (el pipeline fijo)
"    vec3 ambPbr=uAmbGlobal.rgb;\n"
"#define LIGHT_BRIGHTNESS(i) if(pbr){ W3D_HIGHP vec3 Hh=normalize(L+V); float nh=max(dot(N,Hh),0.0); float vh=max(dot(V,Hh),0.0);"
" vec3 fl=f0B+(vec3(1.0)-f0B)*((mp.z>0.0 || mp.x>0.0)?pow(1.0-vh,5.0):0.0); float exL=AREA_EXPO;"
" brightnessPbr+=at*tint*fl*uLSpec[i].rgb*(weightB*pow(nh,exL)*(exL+2.0)*0.125*ndl); }"
" else { float nh=max(dot(N,normalize(L+vec3(0.0,0.0,1.0))),0.0); col1+=uMSpec.rgb*uLSpec[i].rgb*((uShine>0.0)?pow(nh,uShine):1.0); }\n"
"#else\n"   // (sin PBR: la luz de siempre)
"    const float kd=1.0;\n"
"#define LIGHT_BRIGHTNESS(i) { float nh=max(dot(N,normalize(L+vec3(0.0,0.0,1.0))),0.0); col1+=uMSpec.rgb*uLSpec[i].rgb*((uShine>0.0)?pow(nh,uShine):1.0); }\n"
"#endif\n"
"#ifdef W3D_SHADOW\n"   // (la luz que da sombra, por la sombra: la del ambiente de la luz tambien)
"    vec3 shadow=(uShadowOn>0.5)?ShadowF(Shadow(),0.0):vec3(1.0);\n"
"#define SHADOW_LIGHT(i) if(i==uShadowLight) tint=shadow;\n"
"#else\n"
"#define SHADOW_LIGHT(i)\n"
"#endif\n"
"#define LUZ(i) if(i<uNLights){ W3D_HIGHP vec3 L; float at=1.0; vec3 tint=vec3(1.0); AREA_VARS"
"      if(uLPos[i].w!=0.0){ W3D_HIGHP vec3 d=uLPos[i].xyz/uLPos[i].w-eye; float dist=length(d); L=d/max(dist,1e-6);"
"        at=1.0/max(uLAtt[i].x+uLAtt[i].y*dist+uLAtt[i].z*dist*dist,1e-6); }"
"      else L=normalize(uLPos[i].xyz);"
"      if(uLCone[i].w>-0.5){ float sd=dot(-L,normalize(uLCone[i].xyz)); at*=(sd>=uLCone[i].w)?pow(max(sd,0.0),uLAtt[i].w):0.0; }"
"      AREA_LIGHT(i)"
"      SHADOW_LIGHT(i)"
"      float ndl=max(dot(N,L),0.0);"
"      vec3 col1=ma.rgb*uLAmb[i].rgb+md.rgb*uLDif[i].rgb*ndl*kd AREA_DK;"
"      if(ndl>0.0){ LIGHT_BRIGHTNESS(i) }"
"      acc+=(at*col1 AREA_DIF(i))*tint; }\n"
"    LUCES\n"
// EL ENTORNO 360: la luz difusa (la imagen borrosa en la direccion de la normal) y el reflejo (en la direccion
// reflejada; mas rugoso, mas borroso), con el mismo Fresnel y el mismo tinte del metal que el brillo de las luces
"#if defined(W3D_ENVIRONMENT) || defined(W3D_SCREEN)\n"
"    vec3 reflection=vec3(0.0);\n"
"#endif\n"
"#ifdef W3D_ENVIRONMENT\n"
"    if(pbr && uEnvF0.w>0.5){\n"
"      ambPbr=ENV_TEX(uEnvTexB,ENV_COORD(N)).rgb*uAmbGlobal.a;\n"
"      W3D_HIGHP vec3 R=reflect(-V,N); ENV_TYPE cR=ENV_COORD(R);\n"
"      reflection=mix(ENV_TEX_LOD(uEnvTex,cR,mp.y*8.0).rgb,ENV_TEX(uEnvTexB,cR).rgb,clamp(mp.y*1.6-0.6,0.0,1.0));\n"
"    }\n"
"#endif\n"
"#ifdef W3D_PBR\n"
"    if(uSHOn>0.5) ambPbr=max(SHEval(normalize(uEyeWorld*N)),vec3(0.0));\n"   // (una sonda de luz manda sobre el fondo)
"    if(pbr) acc+=md.rgb*kd*ambPbr-ma.rgb*uAmbGlobal.rgb;\n"   // (el ambiente del material de arriba deja su lugar)
"#endif\n"
// REFLEJOS EN PANTALLA (la casilla del material): lo que el reflejo encuentra en la copia de la pantalla, en la
// direccion reflejada (sin profundidad: una aproximacion), menos cuanto mas rugoso y apagandose en los bordes de la
// pantalla y hacia la camara (eso no esta en la pantalla); afuera queda el fondo 360
"#ifdef W3D_SCREEN\n"
"    if(pbr && uMExtra.x>0.5 && uSceneVp.z>0.0){\n"
"      W3D_HIGHP vec3 R=reflect(-V,N);\n"
"      vec2 sp; float msk;\n"
// CON LA PROFUNDIDAD (GL de escritorio): EL RAYO REFLEJADO, recorrido en OJO desde el punto. Cada paso se proyecta a la
// copia y se mira si el rayo esta ADELANTE o ATRAS de lo que se ve ahi. Cada vez que CAMBIA (entra atras de algo o sale
// de atras) el cruce se afina partiendo el paso al medio, y vale como toque solo si ahi el rayo esta SOBRE la superficie
// de los dos lados (la diferencia es chica): entro por una cara que se ve, o -lo mas comun con algo apoyado en el piso-
// entro por abajo, por donde la camara no ve, y SALE por una cara que si se ve. Eso da la imagen espejada de lo que
// esta en pantalla (el objeto dado vuelta, pegado a donde se apoya). Si la diferencia es grande es el borde de una
// silueta (paso por atras de algo): no es un toque, sigue. Lo que no esta en la pantalla no se encuentra: queda el
// fondo 360. Los pasos crecen con la distancia (cerca, finos; lejos, largos)
"#ifdef W3D_SSR_Z\n"
"      if(uSceneZOn>0.5){\n"
"        float hit=0.0; vec2 hs=vec2(0.5); float hasR=0.0; vec2 hsR=vec2(0.5);\n"
"        float step=max(-vPe.z,0.5)*0.012;\n"
"        vec3 pa=vPe; vec3 p=vPe+R*step; bool back=false;\n"
"        for(int i=0;i<80;i++){\n"
"          vec2 s=EscProj(p);\n"
"          if(p.z>-0.001 || s.x<0.0 || s.x>1.0 || s.y<0.0 || s.y>1.0) break;\n"
"          bool det=(EscZ(s)-p.z)>0.0;\n"   // (el rayo esta detras de lo que se ve en ese lugar)
"          if(i>0 && det!=back){\n"
"            vec3 a=pa; vec3 b=p;\n"   // (a: del lado de antes; b: del lado de ahora)
"            for(int k=0;k<7;k++){ vec3 m=(a+b)*0.5; if(((EscZ(EscProj(m))-m.z)>0.0)==back) a=m; else b=m; }\n"
"            vec2 sa=EscProj(a); vec2 sb=EscProj(b);\n"
"            float tol=0.02*(-b.z)+0.01;\n"
"            if(abs(EscZ(sa)-a.z)<tol && abs(EscZ(sb)-b.z)<tol){ hs=det?sb:sa; hit=1.0; break; }\n"
// (de RESPALDO: la primera vez que entra atras de algo cercano -la parte de abajo de un objeto apoyado, que la camara no
//  ve-. Si despues no hay un toque de verdad, se usa ese borde: sin esto quedaba un hueco entre el objeto y su reflejo)
"            if(det && hasR<0.5 && (EscZ(sb)-b.z)<0.06*(-b.z)){ hsR=EscProj(b+R*(0.012*(-b.z))); hasR=1.0; }\n"
"          }\n"
"          back=det; pa=p; step*=1.045; p+=R*step;\n"
"        }\n"
"        if(hit<0.5 && hasR>0.5){ hs=hsR; hit=1.0; }\n"
"        sp=EscSp(hs);\n"
"        vec2 bd=min(hs,1.0-hs);\n"
"        msk=hit*clamp(min(bd.x,bd.y)*12.0,0.0,1.0)*(1.0-mp.y);\n"
"      } else\n"
"#endif\n"
"      {\n"
// SIN LA PROFUNDIDAD (ES 2.0): una aproximacion, lo de la copia corrido en la direccion reflejada
"      sp=(gl_FragCoord.xy-uSceneVp.xy)/abs(uSceneVp.zw)+R.xy*0.35;\n"
"      if(uSceneVp.w<0.0) sp.y=1.0-sp.y;\n"   // (la copia dada vuelta: ver SceneCopy)
"      vec2 bd=min(sp,1.0-sp);\n"
"      msk=clamp(min(bd.x,bd.y)*8.0,0.0,1.0)*(1.0-mp.y)*clamp(1.0-R.z*2.0,0.0,1.0);\n"
"      }\n"
"#ifdef W3D_SPECIAL\n"
"      vec3 ss=texture2D(uScene,sp).rgb;\n"   // (la VideoCore IV: una muestra)
"#else\n"
"      float rr=mp.y*mp.y*0.04;\n"
"      vec3 ss=texture2D(uScene,sp).rgb*0.4+(texture2D(uScene,sp+vec2(rr,0.0)).rgb+texture2D(uScene,sp-vec2(rr,0.0)).rgb"
"+texture2D(uScene,sp+vec2(0.0,rr)).rgb+texture2D(uScene,sp-vec2(0.0,rr)).rgb)*0.15;\n"
"#endif\n"
"      reflection=mix(reflection,ss,msk);\n"
"    }\n"
"#endif\n"
"#if defined(W3D_ENVIRONMENT) || defined(W3D_SCREEN)\n"
"    if(pbr) brightnessPbr+=colB*weightB*reflection;\n"
"#endif\n"
// la TRANSMISION: lo que no refleja (1 - Fresnel) deja pasar lo de atras (el material se dibuja con mezcla)
"#ifdef W3D_PBR\n"
"    float alpha=md.a*(1.0-mp.w*(1.0-fres));\n"
"#else\n"
"    float alpha=md.a;\n"
"#endif\n"
"    cPrim=clamp(vec4(acc,alpha),0.0,1.0);\n"
// EL VIDRIO CON LO DE ATRAS (SceneCopy): lo que esta detras se lee de la copia de la pantalla corrido por la
// REFRACCION (el IOR: cuanto se dobla el rayo al entrar) y desenfocado por la RUGOSIDAD, tenido por el color; se mezcla
// con la parte que refleja (Fresnel). Sale opaco: la mezcla con lo de atras ya la hizo el shader
"#ifdef W3D_SCREEN\n"
"    if(pbr && mp.w>0.0 && uSceneVp.z>0.0){\n"
"      float sf=sqrt(clamp(mp.z,0.0,0.9)); float ior=(1.0+sf)/(1.0-sf);\n"
"      W3D_HIGHP vec3 Rf=refract(-V,N,1.0/ior);\n"
"      vec2 sp=(gl_FragCoord.xy-uSceneVp.xy)/abs(uSceneVp.zw)+(Rf.xy+V.xy)*0.25;\n"
"      if(uSceneVp.w<0.0) sp.y=1.0-sp.y;\n"
"      float r=mp.y*0.045;\n"   // (el ESMERILADO, el radio con la rugosidad)
"#ifdef W3D_SPECIAL\n"   // (la VideoCore IV: 5 muestras, la cruz)
"      vec3 at=texture2D(uScene,sp).rgb*0.2+(texture2D(uScene,sp+vec2(r,0.0)).rgb+texture2D(uScene,sp-vec2(r,0.0)).rgb"
"+texture2D(uScene,sp+vec2(0.0,r)).rgb+texture2D(uScene,sp-vec2(0.0,r)).rgb)*0.2;\n"
"#else\n"   // (9 muestras en dos anillos)
"      float r2=r*0.7071;\n"
"      vec3 at=texture2D(uScene,sp).rgb*0.2+(texture2D(uScene,sp+vec2(r,0.0)).rgb+texture2D(uScene,sp-vec2(r,0.0)).rgb"
"+texture2D(uScene,sp+vec2(0.0,r)).rgb+texture2D(uScene,sp-vec2(0.0,r)).rgb+texture2D(uScene,sp+vec2(r2,r2)).rgb"
"+texture2D(uScene,sp-vec2(r2,r2)).rgb+texture2D(uScene,sp+vec2(r2,-r2)).rgb+texture2D(uScene,sp-vec2(r2,-r2)).rgb)*0.1;\n"
"#endif\n"
"      float tw=mp.w*(1.0-fres);\n"
"      cPrim=vec4(mix(clamp(acc,0.0,1.0),at*md.rgb,tw),md.a);\n"
"    }\n"
"#endif\n"
"  }\n"
"#endif\n"
"  vec4 col = cPrim;\n"   // sin textura: el color primario
"#ifdef W3D_TEX\n"
"#ifdef W3D_SPECIAL\n"   // (con un programa por draw, W3D_TEX ya es "hay textura")
"  {\n"
"#else\n"
"  if(uUseTex==1){\n"
"#endif\n"
"    vec4 tex;\n"
"#ifdef W3D_BLUR\n"
"    if(uBlurY > 0.0){\n"                       // 9 muestras a lo largo de V (motion blur)
"      vec4 acc = vec4(0.0);\n"
"      for(int i=-4;i<=4;i++){\n"
"        float vv = uv.y + float(i)*uBlurY*0.25;\n"
"        float m = step(0.0,vv)*step(vv,1.0);\n"   // fuera de la textura NO aporta: si no, el
"        acc += texture2D(uTex, vec2(uv.x, vv)) * m;\n" // CLAMP repite el borde y deja chorreones
"      }\n"
"      tex = acc * (1.0/9.0);\n"
"    } else tex = texture2D(uTex,uv);\n"
"#else\n"
"    tex = texture2D(uTex,uv);\n"
"#endif\n"
"#ifdef W3D_ALPHA_DOWN\n"
"    if(uAlphaDown==1){\n"                      // video con alpha EMPAQUETADO. El frame se sube
"                                             \n"  // invertido en Y, asi que en la TEXTURA el color
"                                             \n"  // queda en la mitad de ABAJO y la mascara arriba.
"      tex.a = texture2D(uTex, vec2(uv.x, uv.y - 0.5)).r;\n"  // la mascara, media textura mas abajo
"    }\n"
"#endif\n"
"#if defined(W3D_ENV)\n"   // (el modo, constante del programa)
"#if W3D_ENV==1\n    col=tex;\n"
"#elif W3D_ENV==2\n    col=vec4(vec3(clamp(dot(tex.rgb*2.0-1.0,cPrim.rgb*2.0-1.0),0.0,1.0)),tex.a*cPrim.a);\n"
"#elif W3D_ENV==3\n    col=vec4(cPrim.rgb,tex.a*cPrim.a);\n"
"#else\n    col=cPrim*tex;\n#endif\n"
"#else\n"
"    if(uEnv==1) col=tex;\n"                                                         // GL_REPLACE
"    else if(uEnv==2) col=vec4(vec3(clamp(dot(tex.rgb*2.0-1.0,cPrim.rgb*2.0-1.0),0.0,1.0)),tex.a*cPrim.a);\n" // GL_DOT3_RGB
"    else if(uEnv==3) col=vec4(cPrim.rgb,tex.a*cPrim.a);\n"                              // solo alfa (por el del color)
"    else col=cPrim*tex;\n"                                                            // GL_MODULATE
"#endif\n"
"  }\n"
"#endif\n"
"#if defined(W3D_LIGHT_PIXEL) && defined(W3D_PBR)\n"
"  col.rgb=min(col.rgb+brightnessPbr,1.0);\n"
"#endif\n"
"#ifdef W3D_MAPS\n"
"  if(uMapOn.w>0.5){ vec4 ta=texture2D(uMapA,vT); col.a*=(ta.a<0.999)?ta.a:ta.r; }\n"   // (como el trazado de rayos)
"#endif\n"
"#ifdef W3D_MIX\n"
"#ifdef W3D_MIX_MODE\n"
"  { vec4 b=texture2D(uTex1,vT1);\n"
"#if W3D_MIX_MODE==0\n    col.rgb=mix(col.rgb,b.rgb,uMixK);\n"
"#elif W3D_MIX_MODE==3\n    col.rgb=max(col.rgb-b.rgb,0.0);\n"
"#elif W3D_MIX_MODE==2\n    col.rgb=min(col.rgb*b.rgb*2.0,1.0);\n"
"#else\n    col.rgb=min(col.rgb*b.rgb,1.0);\n#endif\n"
"  }\n"
"#else\n"
"  if(uMix>=0){ vec4 b=texture2D(uTex1,vT1);\n"   // la unidad 1 sobre lo que salio de la 0 (el alfa no cambia)
"#ifdef W3D_SHADOW\n"   // (la luz horneada en la unidad 1: la sombra de lo que se mueve, sobre ella)
"    if(uShadowBakeMode>1.5 && uShadowBakeMode<2.5 && uMix>=1 && uMix<=2){ float k=(uMix==2)?2.0:1.0; b.rgb=ShadowBakeL(b.rgb*k,b.a)/k; }\n"
"#endif\n"
"    if(uMix==0) col.rgb=mix(col.rgb,b.rgb,uMixK);\n"
"    else if(uMix==3) col.rgb=max(col.rgb-b.rgb,0.0);\n"
"    else col.rgb=min(col.rgb*b.rgb*((uMix==2)?2.0:1.0),1.0); }\n"
"#endif\n"
"#endif\n"
"#ifdef W3D_SHADOW\n"   // (sobre la luz HORNEADA: esta pasada ES la textura horneada -3 / 4- o no se sabe donde esta -1: se
"  if(uShadowBakeMode>2.5){ float k=(uShadowBakeMode>3.5)?2.0:1.0; col.rgb=ShadowBakeL(col.rgb*k,col.a)/k; col.a=1.0; }\n"
"  else if(uShadowBakeMode>0.5 && uShadowBakeMode<1.5) col.rgb*=ShadowF(Shadow(),0.25);\n"   //  oscurece el color hasta un 75%)
"#endif\n"
"#ifdef W3D_FOG\n"
"#ifdef W3D_FOG_MODE\n"
"  { float f;\n"
"#if W3D_FOG_MODE==1\n    f=exp(-uFogDens*vFogZ);\n"
"#elif W3D_FOG_MODE==2\n    float e=uFogDens*vFogZ; f=exp(-e*e);\n"
"#else\n    f=(uFogEnd-vFogZ)/(uFogEnd-uFogStart);\n#endif\n"
"    col.rgb=mix(uFogColor.rgb,col.rgb,clamp(f,0.0,1.0)); }\n"
"#else\n"
"  if(uFogOn==1){ float f;\n"
"    if(uFogMode==1) f=exp(-uFogDens*vFogZ);\n"                                   // GL_EXP
"    else if(uFogMode==2){ float e=uFogDens*vFogZ; f=exp(-e*e); }\n"            // GL_EXP2
"    else f=(uFogEnd-vFogZ)/(uFogEnd-uFogStart);\n"                              // GL_LINEAR
"    col.rgb=mix(uFogColor.rgb,col.rgb,clamp(f,0.0,1.0)); }\n"
"#endif\n"
"#endif\n"
"#ifdef W3D_DISCARD\n"
"#ifdef W3D_SPECIAL\n"
"  if(col.a < uAlphaRef) discard;\n"
"#else\n"
"  if(uAlphaRef > 0.0 && col.a < uAlphaRef) discard;\n"   // no pinta NI escribe z-buffer
"#endif\n"
"#endif\n"
"  gl_FragColor=col;\n"
"}\n";

// ============================================================================
//  Helpers de matrices (column-major, convencion GL)
// ============================================================================
static Matrix4 mTranslate(float x,float y,float z){ Matrix4 m; m.Identity(); m.m[12]=x; m.m[13]=y; m.m[14]=z; return m; }
static Matrix4 mScale(float x,float y,float z){ Matrix4 m; m.Identity(); m.m[0]=x; m.m[5]=y; m.m[10]=z; return m; }
static Matrix4 mRotate(float deg,float x,float y,float z){
    float r=deg*0.01745329252f, c=cosf(r), s=sinf(r);
    float len=sqrtf(x*x+y*y+z*z); if(len>1e-6f){ x/=len; y/=len; z/=len; }
    float ic=1.0f-c;
    Matrix4 m; m.Identity();
    m.m[0]=c+x*x*ic;    m.m[1]=y*x*ic+z*s;  m.m[2]=z*x*ic-y*s;
    m.m[4]=x*y*ic-z*s;  m.m[5]=c+y*y*ic;    m.m[6]=z*y*ic+x*s;
    m.m[8]=x*z*ic+y*s;  m.m[9]=y*z*ic-x*s;  m.m[10]=c+z*z*ic;
    return m;
}
static Matrix4 mFrustum(float l,float r,float b,float t,float n,float f){
    Matrix4 m; for(int i=0;i<16;i++) m.m[i]=0;
    m.m[0]=2*n/(r-l); m.m[5]=2*n/(t-b); m.m[8]=(r+l)/(r-l); m.m[9]=(t+b)/(t-b);
    m.m[10]=-(f+n)/(f-n); m.m[11]=-1; m.m[14]=-(2*f*n)/(f-n);
    return m;
}
static Matrix4 mOrtho(float l,float r,float b,float t,float n,float f){
    Matrix4 m; m.Identity();
    m.m[0]=2/(r-l); m.m[5]=2/(t-b); m.m[10]=-2/(f-n);
    m.m[12]=-(r+l)/(r-l); m.m[13]=-(t+b)/(t-b); m.m[14]=-(f+n)/(f-n);
    return m;
}
static Matrix4 mPerspective(float fovyDeg,float aspect,float n,float f){
    float t=tanf(fovyDeg*0.5f*0.01745329252f);
    Matrix4 m; for(int i=0;i<16;i++) m.m[i]=0;
    m.m[0]=1.0f/(aspect*t); m.m[5]=1.0f/t; m.m[10]=-(f+n)/(f-n); m.m[11]=-1; m.m[14]=-(2.0f*f*n)/(f-n);
    return m;
}
static Matrix4& cur(){ return matMode==0 ? curProj : (matMode==2 ? curTex : curMV); }

// ============================================================================
//  Compilar / linkear el programa
// ============================================================================
// (defs: "#define ..." que van antes del codigo)
static GLuint compile(GLenum type,const char* defs,const char* src){
#if defined(W3D_ES2_NATIVE)
    const char* parts[2] = { defs, src };
    const int n = 2;
#else
    // PC (GL 2.1, -DW3D_PC_GLES2): GLSL 1.20 (gl_PointCoord) en vez del GLSL ES 1.00 implicito
    const char* parts[3] = { "#version 120\n", defs, src };
    const int n = 3;
#endif
    GLuint s=W3D_GLC(glCreateShader(type)); W3D_GLC(glShaderSource(s,n,parts,0)); W3D_GLC(glCompileShader(s));
    GLint ok=0; W3D_GLC(glGetShaderiv(s,GL_COMPILE_STATUS,&ok));
    // (al log del editor: en Android y en la web stderr no se ve en ningun lado)
    if(!ok){ char log[1024]; log[0]=0; W3D_GLC(glGetShaderInfoLog(s,1024,0,log)); w3dLogfE("[w3dGLES2] shader compile error: %s",log); }
    return s;
}
// ============================================================================
//  PROGRAMAS. Donde la GPU se banca el programa ENTERO (PC, web, el Android de siempre) hay uno solo, con todo, y cada
//  funcion se prende o apaga con uniforms. Donde no entra (la VideoCore IV de Android 2.3, ver buildProgram) cada draw
//  usa uno armado con lo que necesita: un draw de la UI sin luz ni niebla ni recorte tiene un shader chiquito.
//  La clave de un programa: un bit por funcion + la cantidad de luces (desde el bit PLights)
// ============================================================================
enum { PLight=1<<0, PTexGen=1<<1, PSprite=1<<2, PTex=1<<3, PBlur=1<<4, PAlphaDown=1<<5, PMix=1<<6, PFog=1<<7,
       PClip=1<<8, PDiscard=1<<9, PAll=(1<<10)-1, PLightPixel=1<<10, PLights=11, PShadow=1<<15 };
// (PShadow: las sombras en tiempo real, solo en los programas ENTEROS que se arman cuando un objeto las recibe; el bit 15
//  queda entre las luces -11..14- y lo de un programa por draw -desde PState-)
// (PLightPixel no va en PAll: el programa ENTERO tiene la luz por vertice; el de la luz por pixel es OTRO entero aparte)
// Con un programa por draw la clave lleva ademas, desde el bit PState, lo que el driver de la VideoCore IV usa para
// traducir el programa: la primitiva, el blending, la mascara de color y como llega cada atributo que el programa lee
// (arreglo o constante, tipo, tamanio). El driver guarda pocas traducciones por programa: usado con mas combinaciones
// en un mismo cuadro, las iba pisando y re-traducia el programa en cada cuadro (~35 ms cada vez). Con la combinacion
// en la clave cada programa tiene una sola y se traduce una vez
typedef unsigned long long Key;
static const int PState = 16;
static const char* kDefFunction[11] = { "W3D_LIGHT", "W3D_TEXGEN", "W3D_SPRITE", "W3D_TEX", "W3D_BLUR", "W3D_ALPHA_DOWN",
                                       "W3D_MIX", "W3D_FOG", "W3D_CLIP", "W3D_DISCARD", "W3D_LIGHT_PIXEL" };
// las luces que entran en un programa armado por draw (las demas no iluminan)
static const int kLightsByDraw = 4;
// donde quedo cada uniform en un programa (-1: ese programa no lo tiene, y glUniform con -1 no hace nada)
#define W3D_UNIFORMS(X) X(uMVP) X(uMV) X(uNMat) X(uTexM) X(uTexGen) X(uUseTex) X(uTex) X(uTex1) X(uEnv) X(uLightOn) \
    X(uFogOn) X(uPointSize) X(uPointSprite) X(uColorMat) X(uBlurY) X(uAlphaRef) X(uAlphaDown) X(uMix) X(uMixK) \
    X(uViewport) X(uClipN) X(uMDiff) X(uMAmb) X(uMSpec) X(uMyME) X(uShine) X(uAmbGlobal) X(uNLights) X(uLPos) X(uLAmb) \
    X(uLDif) X(uLSpec) X(uLAtt) X(uLCone) X(uFogColor) X(uFogStart) X(uFogEnd) X(uFogMode) X(uFogDens) X(uMPbr) \
    X(uEnvTex) X(uEnvTexB) X(uEnvF0) X(uEnvF1) X(uEnvF2) X(uScene) X(uSceneVp) X(uMExtra) X(uSceneZ) X(uProj) X(uSceneZOn) \
    X(uMapN) X(uMapR) X(uMapM) X(uMapA) X(uMapE) X(uMapOn) X(uMapEOn) X(uSHa) X(uSHb) X(uSHc) X(uSHOn) X(uEyeWorld) \
    X(uSHaV) X(uSHbV) X(uSHcV) X(uSHOnV) X(uEyeWorldV) \
    X(uShadowLightV) X(uShadowNV) X(uShadowCV) X(uShadowOn) X(uShadowLight) X(uShadowTex) X(uShadowM0) X(uShadowM1) X(uShadowM2) X(uShadowM3) \
    X(uShadowBox0) X(uShadowBox1) X(uShadowBox2) X(uShadowBox3) X(uShadowCuts) X(uShadowRad) X(uShadowP) X(uShadowQ) X(uShadowBake) X(uShadowRV) X(uShadowColor) \
    X(uShadowBakeMode) X(uShadowFloor)
// lo ultimo que se subio a un uniform (n = 0: nada todavia). Una llamada a GL que no cambia nada no se hace: en la
// VideoCore IV de Android 2.3 cada una es un mensaje al chip, y un draw subia ~40 uniforms
struct UCache { unsigned char b[64]; int n; };
struct Program {
    GLuint id;
    Key key;               // la PEDIDA (con la que el cache lo encuentra)
    Key built;             // con la que se armo: la pedida, o menos si no entraba en la GPU (ver ShrinkProgram)
    int test;              // 1 = su primer draw se mira (un programa por draw: ver ProgramNotEntered)
    unsigned showLights;   // las luces que tiene subidas (ver UploadLights)
#define W3D_U(n) GLint n; UCache c_##n;
    W3D_UNIFORMS(W3D_U)
#undef W3D_U
    GLint uClip[5]; UCache c_uClip[5];
};
// (los programas se crean recien cuando se arman: cada uno son ~7 KB con su cache de uniforms, y los 96 fijos eran 650 KB de
//  RAM desde el arranque; un juego usa uno a cuatro, el modo de un programa por draw unos diez)
static const int kProgramsMax = 96;
static Program* programs[kProgramsMax];
static Program& Prog(int i) { if (!programs[i]) programs[i] = new Program(); return *programs[i]; }
static int nPrograms = 0;
static Program* progUse  = 0;   // el del ultimo draw
static GLuint activeProg = 0;   // el que tiene GL (glUseProgram)

static bool byDraw = false;       // un programa por draw (el entero no entra en la GPU)
static bool lightPixel = false;   // el material pide la luz POR PIXEL (ShadingPerPixel)
static int  progPixel = -1;       // (programa ENTERO) donde quedo el de la luz por pixel: -1 sin armar, -2 no entra en esta GPU
// LAS SOMBRAS EN TIEMPO REAL (ShadowMapBegin ... ShadowReceive, mas abajo): los programas ENTEROS con W3D_SOMBRA
// ([0] la luz por vertice, [1] por pixel; -1 sin armar, -2 no entra en esta GPU) y lo que dice si este draw las recibe
static int  progShadow[2] = { -1, -1 };
static bool shadowActive = false;     // hay sombras en este pase (ShadowReceptors)
static int  shadowMode = 0;           // el material que se dibuja las recibe (ShadowReceive: 1 luz de tiempo real, 2 horneada;
                                   // 3 su luz horneada en la unidad 1, 4 / 5 esta pasada es la horneada Multiply / 2x)
static bool shadowMasc = false;       // (2..5) la textura horneada trae la mascara del sol en el alfa
// los vertices de cada draw como arreglos del cliente (el driver los copia en el draw) y no subidos a un VBO: en la
// misma GPU (la VideoCore IV de Android 2.3) un glBufferData por draw al mismo VBO a veces deja a un draw con los datos
// del siguiente (los rotulos "X" "Y" "Z" de Properties salian "X" "Z" "Z"), y ademas cuesta mas
static bool noVBO = false;
static unsigned showLights = 1;   // sube cada vez que cambian las luces
// (W3D_TEST_FILL=1, para medir: cada draw y cada limpieza solo en el pixel de abajo a la izquierda. La GPU casi no
// pinta y lo que queda es la geometria, la CPU y el swap: separa lo que cuestan los pixeles)
static bool noFill = false;
static void OnlyUnPixel(){ glEnable(GL_SCISSOR_TEST); glScissor(0,0,1,1); }

// LA LUZ DE AREA (Light.h) va con la luz POR PIXEL del programa ENTERO (un uniform dice cual es de area). Los demas -la
// luz por vertice- la ven como el foco que la reemplaza (UploadLights). Con un programa POR DRAW (el entero no entra en la
// GPU: la VideoCore IV del Android 2.3) tambien va el foco: su cuenta se repite en el shader por cada luz (el poligono de
// la lampara, 4 aristas u 8 si es elipse, recortado por el horizonte) y el driver de la VideoCore IV se cae traduciendolo
// en el primer draw (SIGSEGV adentro de libGLES_hgl.so, una recursion suya: el Bongo Cat, con 4 luces y una de area, en
// un Galaxy Ace). No hay como atajarlo: no devuelve error, mata el proceso
static bool ProgramWithArea(Key key){
    if(!(key & PLightPixel) || byDraw) return false;
    const Key e = key >> PState;
    return e == 0 || ((e >> 43) & 1);
}
// los #define de un programa: sus funciones y LUCES = LUZ(0) LUZ(1) ... con sus luces
static void DefsProgram(Key key, char* d, int size){
    int n = 0;
    for(int b=0;b<PLights;b++) if(key&(1ull<<b)) n += snprintf(d+n, size-n, "#define %s\n", kDefFunction[b]);
    // UN PROGRAMA POR DRAW: los modos que el programa entero elige con uniforms van como constantes (ver PickProgram).
    // El compilador de la VideoCore IV no saltea las ramas de un if por uniform: hacia las cuentas de todas (la niebla
    // con dos exp por pixel aunque fuera lineal). Medido en el Galaxy Ace: ~13 ms por cuadro de la GPU son los pixeles
    const Key e = key >> PState;
    if(e){
        n += snprintf(d+n, size-n, "#define W3D_SPECIAL\n");
        if((e & 3) == 0) n += snprintf(d+n, size-n, "#define W3D_POINTS\n");
        if(key & PTex){ n += snprintf(d+n, size-n, "#define W3D_ENV %d\n", (int)((e >> 31) & 3));
                          if((e >> 33) & 1) n += snprintf(d+n, size-n, "#define W3D_TEXM\n"); }
        if(key & PFog) n += snprintf(d+n, size-n, "#define W3D_FOG_MODE %d\n", (int)((e >> 34) & 3));
        if(key & PMix) n += snprintf(d+n, size-n, "#define W3D_MIX_MODE %d\n", (int)((e >> 36) & 3));
        if((e >> 38) & 1) n += snprintf(d+n, size-n, "#define W3D_PBR\n");       // (los pedazos de la luz PBR: ver el FS)
        if((e >> 39) & 1) n += snprintf(d+n, size-n, "#define W3D_ENVIRONMENT\n");
        if((e >> 40) & 1) n += snprintf(d+n, size-n, "#define W3D_SCREEN\n");
        if((e >> 41) & 1) n += snprintf(d+n, size-n, "#define W3D_ENVIRONMENT_CUBE\n");
        if((e >> 42) & 1) n += snprintf(d+n, size-n, "#define W3D_SH_V\n");   // (la sonda de luz por vertice)
    } else n += snprintf(d+n, size-n, "#define W3D_SH_V\n");   // (el programa ENTERO: con un uniform)
    if(ProgramWithArea(key)) n += snprintf(d+n, size-n, "#define W3D_AREA\n");   // (la luz de area por pixel)
    if(!byDraw && MapsHas()) n += snprintf(d+n, size-n, "#define W3D_MAPS\n");   // (los mapas del material)
    if(key & PShadow) n += snprintf(d+n, size-n, "#define W3D_SHADOW\n");   // (las sombras en tiempo real)
#if !defined(W3D_ES2_NATIVE) || defined(W3D_VKGL)
    n += snprintf(d+n, size-n, "#define W3D_SSR_Z\n");   // (GL de escritorio: los reflejos en pantalla con la profundidad)
#endif
    { const int nl = (int)((key>>PLights)&15);   // (el tamanio de los arreglos de luces del fragment shader)
      n += snprintf(d+n, size-n, "#define W3D_NLIGHTS %d\n", nl > 0 ? nl : 1); }
    n += snprintf(d+n, size-n, "#define LUCES");
    for(int i=0;i<(int)((key>>PLights)&15);i++) n += snprintf(d+n, size-n, " LUZ(%d)", i);
    snprintf(d+n, size-n, "\n");
}
// el programa, con defs delante de los dos shaders
static GLuint link(const char* defs){
    GLuint vs=compile(GL_VERTEX_SHADER,defs,VS), fs=compile(GL_FRAGMENT_SHADER,defs,FS);
    GLuint p=W3D_GLC(glCreateProgram()); W3D_GLC(glAttachShader(p,vs)); W3D_GLC(glAttachShader(p,fs));
    // los atributos en lugares fijos: al cambiar de programa no cambian (la posicion en el 0: GL de escritorio lo pide)
    W3D_GLC(glBindAttribLocation(p,aLpos,"aPos")); W3D_GLC(glBindAttribLocation(p,aLnrm,"aNrm"));
    W3D_GLC(glBindAttribLocation(p,aLuv,"aUV")); W3D_GLC(glBindAttribLocation(p,aLcol,"aCol"));
    W3D_GLC(glBindAttribLocation(p,aLuv1,"aUV1"));
    W3D_GLC(glLinkProgram(p));
    W3D_GLC(glDeleteShader(vs)); W3D_GLC(glDeleteShader(fs));   // (se van con el programa)
    GLint ok=0; W3D_GLC(glGetProgramiv(p,GL_LINK_STATUS,&ok));
    if(!ok){ char log[1024]; log[0]=0; W3D_GLC(glGetProgramInfoLog(p,1024,0,log)); w3dLogfE("[w3dGLES2] link error: %s",log); }
    return p;
}
static bool Linked(GLuint p){ GLint ok=0; W3D_GLC(glGetProgramiv(p,GL_LINK_STATUS,&ok)); return ok!=0; }
static void BuildProgram(Program& p, Key key){
    char defs[768]; DefsProgram(key, defs, (int)sizeof(defs));
    memset(&p, 0, sizeof(p));   // (los caches vacios)
    p.id = link(defs); p.key = key; p.built = key; p.test = byDraw ? 1 : 0; p.showLights = 0;
    // LA GPU NO TIENE REGISTROS PARA TANTO (la VideoCore III del N8: "Out of temporaries" al linkear): en el modo de un
    // programa por draw se reintenta con la mitad de las luces. Se ven menos luces en ese draw, pero se dibuja; sin
    // esto el draw fallaba (GL_INVALID_OPERATION) y la pantalla quedaba en el color de fondo. (La clave sigue siendo
    // la pedida: el cache lo encuentra igual.)
    Key c = key;
    while(byDraw && !Linked(p.id) && ((c >> PLights) & 15) > 0){
        const int n = (int)((c >> PLights) & 15) / 2;
        W3D_GLC(glDeleteProgram(p.id));
        c = (c & ~((Key)15 << PLights)) | ((Key)n << PLights);
        DefsProgram(c, defs, (int)sizeof(defs));
        p.id = link(defs);
        w3dLogfW("[w3dGLES2] el programa no entraba en esta GPU: reintento con %d luces (%s)", n, Linked(p.id) ? "entered" : "tampoco");
    }
#define W3D_U(n) p.n=W3D_GLC(glGetUniformLocation(p.id,#n));
    W3D_UNIFORMS(W3D_U)
#undef W3D_U
    p.uClip[0]=W3D_GLC(glGetUniformLocation(p.id,"uClip0")); p.uClip[1]=W3D_GLC(glGetUniformLocation(p.id,"uClip1"));
    p.uClip[2]=W3D_GLC(glGetUniformLocation(p.id,"uClip2")); p.uClip[3]=W3D_GLC(glGetUniformLocation(p.id,"uClip3"));
    p.uClip[4]=W3D_GLC(glGetUniformLocation(p.id,"uClip4"));
}
// lo deja en uso: prog y los u* son los suyos
static void UseProgram(Program& p){
    progUse = &p; prog = p.id;
#define W3D_U(n) n=p.n;
    W3D_UNIFORMS(W3D_U)
#undef W3D_U
    for(int i=0;i<5;i++) uClip[i]=p.uClip[i];
    if(activeProg != prog){ W3D_GLC(glUseProgram(prog)); activeProg = prog; }
}
static void DeleteProgram(Program& p){
    if(activeProg == p.id) activeProg = 0;   // (un id nuevo puede salir igual al borrado)
    W3D_GLC(glDeleteProgram(p.id));
}
// subir un uniform del programa en uso solo si cambio (nombre = el global u*, que tiene su c_* en el Programa)
static inline bool UNew(UCache& c, const void* v, int n){
    if(c.n == n && memcmp(c.b, v, (size_t)n) == 0) return false;
    memcpy(c.b, v, (size_t)n); c.n = n; return true;
}
#define U_1I(name, val)  do{ GLint v_ = (GLint)(val);   if(name>=0 && UNew(progUse->c_##name, &v_, (int)sizeof(v_))) W3D_GLC(glUniform1i(name, v_)); }while(0)
#define U_1F(name, val)  do{ GLfloat v_ = (GLfloat)(val); if(name>=0 && UNew(progUse->c_##name, &v_, (int)sizeof(v_))) W3D_GLC(glUniform1f(name, v_)); }while(0)
#define U_4F(name, p)    do{ if(name>=0 && UNew(progUse->c_##name, (p), 16)) W3D_GLC(glUniform4fv(name, 1, (p))); }while(0)
#define U_M3(name, p)    do{ if(name>=0 && UNew(progUse->c_##name, (p), 36)) W3D_GLC(glUniformMatrix3fv(name, 1, GL_FALSE, (p))); }while(0)
#define U_M4(name, p)    do{ if(name>=0 && UNew(progUse->c_##name, (p), 64)) W3D_GLC(glUniformMatrix4fv(name, 1, GL_FALSE, (p))); }while(0)
// el programa ENTRA en la GPU? El driver de Broadcom (VideoCore IV, Android 2.3) recien traduce los shaders al primer
// draw, y si no le entran ese draw y todos los que siguen fallan con GL_OUT_OF_MEMORY: la pantalla queda negra.
// Un triangulo de prueba sin escribir color ni profundidad (el atributo 0 en vboP)
static bool enters(GLuint p){
    static const float tri[9] = { -1,-1,0, 1,-1,0, 0,1,0 };
    for(int i=0;i<8 && glGetError()!=GL_NO_ERROR;i++){}
    W3D_GLC(glUseProgram(p)); activeProg = p;
    W3D_GLC(glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE)); W3D_GLC(glDepthMask(GL_FALSE));
    W3D_GLC(glBindBuffer(GL_ARRAY_BUFFER,vboP)); W3D_GLC(glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)sizeof(tri),tri,GL_DYNAMIC_DRAW));
    W3D_GLC(glEnableVertexAttribArray(0)); W3D_GLC(glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,(const void*)0));
    W3D_GLC(glDrawArrays(GL_TRIANGLES,0,3));
    // VideoCore IV (Android 2.3): linkea pero el draw da GL_OUT_OF_MEMORY. VideoCore III (el N8): ni linkea ("Out of
    // temporaries") y el draw da GL_INVALID_OPERATION
    const GLenum err = glGetError();
    const bool yes = Linked(p) && err!=GL_OUT_OF_MEMORY && err!=GL_INVALID_OPERATION;
    W3D_GLC(glDisableVertexAttribArray(0));
    W3D_GLC(glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE)); W3D_GLC(glDepthMask(GL_TRUE));
    AttrForget();
    return yes;
}
// LA COPIA DE LA PANTALLA, DADA VUELTA? GL copia un rectangulo de la pantalla a una textura (glCopyTexSubImage2D: las
// capturas de los juegos) con la fila de ABAJO en la fila 0, y el que la muestra la da vuelta con las UV. En el Galaxy
// Ace (VideoCore IV, Android 2.3) el fundido de los cortes salia patas arriba. Se prueba una vez al armar el backend:
// abajo a la izquierda dos pixeles (el de abajo rojo, el de arriba azul) y arriba dos mas (verde y amarillo), copiados
// los dos de abajo a una textura de 2x2 que se lee con un FBO. Si la fila 0 sale azul, el driver la copio al reves y las
// UV de la captura se dan vuelta (CopyScreenFlipped). Si sale amarillo (la de mas arriba), el driver cuenta las
// filas DESDE ARRIBA (el Ace: "fila 0 amarillo, fila 1 verde"): copia al reves y de otro lugar, asi que ademas se le
// pide la fila espejada (CopyScreenRow, con el alto de la ventana). En la PC no se prueba (sin FBO sin extensiones)
static bool givenCopyTurn = false, copyFromUp = false;
static void ForgetProgramsEsp();    // (los pases especiales y el postprocesado: mas abajo, junto a setupState)
static void ForgetProgramsPost();
static void ForgetWrittenPrograms();   // (los shaders escritos de los materiales: se vuelven a armar desde su texto)
#if defined(__ANDROID__)
// el alto de la ventana (la superficie de EGL), para la copia de un driver que cuenta las filas desde arriba. Por dlsym:
// el editor y los juegos no linkean libEGL (la abre SDL)
static int WindowHeight(){
    typedef void* (*FnDisplay)();
    typedef void* (*FnSurface)(int);
    typedef unsigned (*FnQuery)(void*, void*, int, int*);
    static FnDisplay display = 0; static FnSurface surface = 0; static FnQuery query = 0;
    static bool found = false;
    if(!found){
        found = true;
        void* lib = dlopen("libEGL.so", RTLD_NOW);
        if(lib){
            display = (FnDisplay)dlsym(lib, "eglGetCurrentDisplay");
            surface = (FnSurface)dlsym(lib, "eglGetCurrentSurface");
            query = (FnQuery)dlsym(lib, "eglQuerySurface");
        }
    }
    int height = 0;
    if(display && surface && query && query(display(), surface(0x3059 /*EGL_DRAW*/), 0x3056 /*EGL_HEIGHT*/,
                                                       &height)) return height;
    return 0;
}
#endif
#if defined(W3D_ES2_NATIVE)
static const char* ColorOf(const unsigned char* p){
    const bool r = p[0] > 128, g = p[1] > 128, b = p[2] > 128;
    return (r && !g && !b) ? "red" : (!r && !g && b) ? "blue" : (!r && g && !b) ? "green" : (r && g && !b) ? "yellow"
                                                                                                        : "other";
}
static void TestScreenCopy(){
    GLint box[4], vp[4]; W3D_GLC(glGetIntegerv(GL_SCISSOR_BOX,box)); W3D_GLC(glGetIntegerv(GL_VIEWPORT,vp));
    const GLboolean clip = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat background[4]; W3D_GLC(glGetFloatv(GL_COLOR_CLEAR_VALUE,background));
    const int height = vp[3] > 4 ? vp[1] + vp[3] : 4;   // (al empezar, el viewport es la ventana)
    static const struct { int y; float r, g, b; } kPaint[4] = {
        {0, 1,0,0}, {1, 0,0,1}, {-2, 0,1,0}, {-1, 1,1,0} };   // (negativos: desde arriba)
    W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER,0));
    W3D_GLC(glEnable(GL_SCISSOR_TEST));
    for(int i=0;i<4;i++){
        W3D_GLC(glScissor(0,kPaint[i].y >= 0 ? kPaint[i].y : height + kPaint[i].y,1,1));
        W3D_GLC(glClearColor(kPaint[i].r,kPaint[i].g,kPaint[i].b,1)); W3D_GLC(glClear(GL_COLOR_BUFFER_BIT));
    }
    unsigned char reading[4] = {0,0,0,0};
    W3D_GLC(glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,reading));
    GLuint t = 0, f = 0;
    W3D_GLC(glGenTextures(1,&t)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,t));
    W3D_GLC(glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,2,2,0,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,NULL));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST));
    W3D_GLC(glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,1,2));
    W3D_GLC(glGenFramebuffers(1,&f)); W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER,f));
    W3D_GLC(glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t,0));
    unsigned char copy[8] = {0,0,0,0, 0,0,0,0};
    const bool fbo = glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    if(fbo) W3D_GLC(glReadPixels(0,0,1,2,GL_RGBA,GL_UNSIGNED_BYTE,copy));
    W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER,0)); W3D_GLC(glDeleteFramebuffers(1,&f));
    W3D_GLC(glDeleteTextures(1,&t)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex));
    W3D_GLC(glScissor(box[0],box[1],box[2],box[3]));
    if(!clip) W3D_GLC(glDisable(GL_SCISSOR_TEST));
    W3D_GLC(glClearColor(background[0],background[1],background[2],background[3]));
    for(int i=0;i<8 && glGetError()!=GL_NO_ERROR;i++){}
    if(!fbo){ w3dLogf("[w3dGLES2] la copia de la pantalla: sin FBO para probarla (se supone bien)"); return; }
    const char* c0 = ColorOf(copy); const char* c1 = ColorOf(copy + 4);
    copyFromUp = strcmp(c0,"amarillo")==0;
    givenCopyTurn = copyFromUp || strcmp(c0,"blue")==0;
    w3dLogf("[w3dGLES2] la copia de la pantalla: fila 0 %s, fila 1 %s (%s); la lectura de abajo: %s", c0, c1,
            copyFromUp ? "CUENTA LAS FILAS DESDE ARRIBA: se le pide la espejada y las UV se dan vuelta" :
            givenCopyTurn ? "DADA VUELTA: las UV de las capturas se dan vuelta" : "good", ColorOf(reading));
}
#endif
bool CopyScreenFlipped(){ return givenCopyTurn; }
int CopyScreenRow(int y, int h){
#if defined(__ANDROID__)
    if(copyFromUp){ const int height = WindowHeight(); if(height > 0) return height - y - h; }
#endif
    (void)h;
    return y;
}

static void ShadowsForgetGL();
static void buildProgram(){
    W3D_GLC(glGenBuffers(1,&vboP)); W3D_GLC(glGenBuffers(1,&vboN)); W3D_GLC(glGenBuffers(1,&vboT)); W3D_GLC(glGenBuffers(1,&vboC));
    W3D_GLC(glGenBuffers(1,&vboT1)); W3D_GLC(glGenBuffers(1,&ibo));
    // (de nuevo si el contexto GL se perdio y se volvio a armar: lo de antes ya no existe)
    progUse = 0; byDraw = false; noVBO = false; progPixel = -1; progShadow[0] = progShadow[1] = -1;
    ShadowsForgetGL();   // (el mapa de sombras del contexto anterior ya no existe)
    ForgetProgramsEsp(); ForgetProgramsPost(); ForgetWrittenPrograms();
    { const char* r = getenv("W3D_TEST_FILL"); noFill = r && r[0] == '1'; }
    BuildProgram(Prog(0), (Key)(PAll | (LightsMax << PLights)));
    nPrograms = 1;
    // (W3D_GLES2_BY_DRAW=1: el camino de la VideoCore IV en cualquier GPU, para probarlo en la PC)
    const char* force = getenv("W3D_GLES2_BY_DRAW");
    if(!(force && force[0] == '1') && enters(Prog(0).id)){
        UseProgram(Prog(0));
    } else {
        // (y no siempre: con los formatos de los atributos y el blending el driver le agrega instrucciones, y uno que
        // entra en la prueba puede no entrar en un draw)
        w3dLogfW("[w3dGLES2] el programa entero no entra en esta GPU: uno por draw, con hasta %d luces", kLightsByDraw);
        DeleteProgram(Prog(0));
        nPrograms = 0;
        byDraw = true;
        noVBO = true;
    }
#if defined(W3D_ES2_NATIVE)
    TestScreenCopy();
#endif
    FactoryLights();
    ready=true;
}

// ============================================================================
//  Capacidades (Enable/Disable/IsEnabled)
// ============================================================================
void Enable(Cap c){
    switch(c){
        case DepthTest: if(!(byDraw && cap_depth)){ W3D_GLC(glEnable(GL_DEPTH_TEST)); cap_depth=true; } break;
        case CullFace:  if(!(byDraw && cap_cull)){ W3D_GLC(glEnable(GL_CULL_FACE)); cap_cull=true; } break;
        case Blend:     if(!(byDraw && cap_blend)){ W3D_GLC(glEnable(GL_BLEND)); cap_blend=true; } break;
        case ScissorTest: if(!(byDraw && cap_scissor)){ W3D_GLC(glEnable(GL_SCISSOR_TEST)); cap_scissor=true; } break;
        case Texture2D: cap_tex=true;   break; // el shader decide (uUseTex)
        case Lighting:  cap_light=true; break;
        case Fog:       cap_fog=true;   break;
        case PolygonOffsetFill: if(!(byDraw && cap_offset)){ W3D_GLC(glEnable(GL_POLYGON_OFFSET_FILL)); cap_offset=true; } break; // pick de CARAS: empuja la malla de
                       // oclusion atras para que los triangulos-ID pasen el depth test (sino no se seleccionan)
        case ColorMaterial: cap_colormat=true; break; // el shader usa el vertex color como diffuse/ambient
        case Light0: if(!lights[0].on){ lights[0].on=true; lightsDirty=true; } break;
        case Normalize:
        case Dither: if(!cap_dither){ BatchEmpty(); W3D_GLC(glEnable(GL_DITHER)); cap_dither=true; } break;
        case PointSprite: case Multisample: break; // no aplican / el shader ya normaliza
    }
}
void Disable(Cap c){
    switch(c){
        case DepthTest: if(!(byDraw && !cap_depth)){ W3D_GLC(glDisable(GL_DEPTH_TEST)); cap_depth=false; } break;
        case CullFace:  if(!(byDraw && !cap_cull)){ W3D_GLC(glDisable(GL_CULL_FACE)); cap_cull=false; } break;
        case Blend:     if(!(byDraw && !cap_blend)){ W3D_GLC(glDisable(GL_BLEND)); cap_blend=false; } break;
        case ScissorTest: if(!(byDraw && !cap_scissor)){ W3D_GLC(glDisable(GL_SCISSOR_TEST)); cap_scissor=false; } break;
        case Texture2D: cap_tex=false;   break;
        case Lighting:  cap_light=false; break;
        case Fog:       cap_fog=false;   break;
        case ColorMaterial: cap_colormat=false; break;
        case Light0: if(lights[0].on){ lights[0].on=false; lightsDirty=true; } break;
        case PolygonOffsetFill: if(!(byDraw && !cap_offset)){ W3D_GLC(glDisable(GL_POLYGON_OFFSET_FILL)); cap_offset=false; } break;
        case Dither: if(cap_dither){ BatchEmpty(); W3D_GLC(glDisable(GL_DITHER)); cap_dither=false; } break;
        default: break;
    }
}
bool IsEnabled(Cap c){
    switch(c){
        case DepthTest: return cap_depth; case CullFace: return cap_cull; case Blend: return cap_blend;
        case ScissorTest: return cap_scissor; case Texture2D: return cap_tex; case Lighting: return cap_light;
        case Fog: return cap_fog; case ColorMaterial: return cap_colormat; case Light0: return lights[0].on;
        case Dither: return cap_dither;
        default: return false;
    }
}

// ============================================================================
//  Matrices
// ============================================================================
void MatrixMode(Matrix m){ matMode = (m==Projection)?0:(m==TextureMatrix?2:1); }
void LoadIdentity(){ cur().Identity(); }
void PushMatrix(){ if(matMode==0) stkProj.push_back(curProj); else if(matMode==2) stkTex.push_back(curTex); else stkMV.push_back(curMV); }
void PopMatrix(){ if(matMode==0){ if(!stkProj.empty()){ curProj=stkProj.back(); stkProj.pop_back(); } }
                  else if(matMode==2){ if(!stkTex.empty()){ curTex=stkTex.back(); stkTex.pop_back(); } }
                  else          { if(!stkMV.empty()){ curMV=stkMV.back(); stkMV.pop_back(); } } }
void Translatef(float x,float y,float z){ cur() = cur() * mTranslate(x,y,z); }
void Rotatef(float a,float x,float y,float z){ cur() = cur() * mRotate(a,x,y,z); }
void Scalef(float x,float y,float z){ cur() = cur() * mScale(x,y,z); }
void MultMatrix(const float* m16){ Matrix4 M; memcpy(M.m,m16,sizeof(float)*16); cur() = cur() * M; }
void LoadMatrix(const float* m16){ memcpy(cur().m,m16,sizeof(float)*16); }
void Ortho(float l,float r,float b,float t,float n,float f){ cur() = cur() * mOrtho(l,r,b,t,n,f); }
void Frustum(float l,float r,float b,float t,float n,float f){ cur() = cur() * mFrustum(l,r,b,t,n,f); }
void Perspective(float fovy,float aspect,float n,float f){ cur() = cur() * mPerspective(fovy,aspect,n,f); }

// ============================================================================
//  Buffers / viewport / scissor / estado suelto
// ============================================================================
static float clearCol[4] = {0,0,0,0};   // (lo usa el clear dibujado, ver ClearWithQuad)
void ClearColor(float r,float g,float b,float a){ clearCol[0]=r; clearCol[1]=g; clearCol[2]=b; clearCol[3]=a; W3D_GLC(glClearColor(r,g,b,a)); }
// EL CLEAR CON SCISSOR DE LA VIDEOCORE III (el N8, Symbian^3): el driver lo toma como "el cuadro empieza de nuevo" y
// deja de cargar TODO lo dibujado antes, no solo el rectangulo: de la pantalla del editor solo quedaba lo que se
// dibujaba despues del ultimo clear de un viewport (Properties y el menu abierto; el 3D, el outliner y el timeline en
// negro). Ahi el clear con scissor se DIBUJA: un rectangulo del color de fondo (y la profundidad al fondo). Solo en esa
// GPU (GLES2Init la reconoce por GL_RENDERER); un clear sin scissor sigue siendo glClear
static bool clearScissorDrawn = false;
static void ClearWithQuad(bool color, bool prof);
void Clear(int bits){ GLbitfield m=0; if(bits&ColorBuffer) m|=GL_COLOR_BUFFER_BIT; if(bits&DepthBuffer) m|=GL_DEPTH_BUFFER_BIT;
                      if(bits&StencilBuffer) m|=GL_STENCIL_BUFFER_BIT; if(noFill) OnlyUnPixel();
                      if(clearScissorDrawn && cap_scissor && ready){ ClearWithQuad((bits&ColorBuffer)!=0, (bits&DepthBuffer)!=0); return; }
                      W3D_GLC(glClear(m)); }

// ---- planos de recorte: el pipeline fijo los hace en hardware (GL_CLIP_PLANEi);
//      aca es un discard en el shader. La ecuacion llega en el espacio de la
//      MODELVIEW ACTUAL (misma regla que glClipPlane) y se pasa a OJO con
//      (MV^-1)^T, que es lo que hace el pipeline fijo por dentro.
int PlanesClipMax(){ return 5; }
void PlanesClip(int n, const float* eq4){
    if(n<0) n=0; if(n>5) n=5;
    if(n>0){
        Matrix4 inv; if(!Matrix4::InvertAffine(curMV, inv)) { n=0; }
        for(int i=0;i<n;i++){
            const float* p=&eq4[i*4];
            // (MV^-1)^T * p  =  filas de MV^-1 punto p
            float e[4];
            for(int k=0;k<4;k++)
                e[k]= inv.m[0*4+k]*p[0] + inv.m[1*4+k]*p[1] + inv.m[2*4+k]*p[2] + inv.m[3*4+k]*p[3];
            for(int k=0;k<4;k++) clipEq[i][k]=e[k];
        }
    }
    clipN=n;   // (los sube setupState al programa del draw)
}

// ---- estencil (GL ES 2.0 lo trae en el core) ----
int StencilBits(){ GLint b=0; W3D_GLC(glGetIntegerv(GL_STENCIL_BITS,&b)); return b>0?(int)b:0; }
void StencilClear(int v){ W3D_GLC(glClearStencil(v)); W3D_GLC(glStencilMask(0xFFu)); W3D_GLC(glClear(GL_STENCIL_BUFFER_BIT)); }
void StencilMark(int v){ W3D_GLC(glEnable(GL_STENCIL_TEST)); W3D_GLC(glStencilMask(0xFFu)); W3D_GLC(glStencilFunc(GL_ALWAYS,v,0xFFu)); W3D_GLC(glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE)); }
void StencilInside(int v){ W3D_GLC(glEnable(GL_STENCIL_TEST)); W3D_GLC(glStencilMask(0x00u)); W3D_GLC(glStencilFunc(GL_EQUAL,v,0xFFu)); W3D_GLC(glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP)); }
void StencilTurnOff(){ W3D_GLC(glStencilMask(0xFFu)); W3D_GLC(glDisable(GL_STENCIL_TEST)); }
// LO QUE GL YA TIENE (con un programa por draw): no se vuelve a pedir. Ademas de la llamada, cada una dibujaria el lote
// juntado (ver BatchEmpty) y la UI repite el mismo scissor, blending o textura en cada widget. GLES2Init los olvida
static GLint scissorGL[4] = {-1,-1,-1,-1}, viewportGL[4] = {-1,-1,-1,-1};
static int depthMaskGL = -1, depthFuncGL = -1, frontFaceGL = -1, blendGL = -1, colorMaskGL = -1;
static float lineWidthGL = -1.0f;
struct TexParamGL { GLint minF, magF, wrap; };
static std::map<unsigned, TexParamGL> texParamGL;
static void StateGLForget(){
    for(int i=0;i<4;i++){ scissorGL[i] = -1; viewportGL[i] = -1; }
    depthMaskGL = depthFuncGL = frontFaceGL = blendGL = colorMaskGL = -1; lineWidthGL = -1.0f;
    texParamGL.clear();
}
static inline bool SameRect(const GLint* r, int x, int y, int w, int h){ return r[0]==x && r[1]==y && r[2]==w && r[3]==h; }
// ver w3dGraphics.h: lo juntado se dibuja con lo que tenia, se olvidan los rectangulos y parametros recordados y las
// capacidades y la textura vuelven a GL como el motor cree que estan
void GLES2Resync(){
    DrawPending();
    StateGLForget();
    if(cap_depth) W3D_GLC(glEnable(GL_DEPTH_TEST)); else W3D_GLC(glDisable(GL_DEPTH_TEST));
    if(cap_cull) W3D_GLC(glEnable(GL_CULL_FACE)); else W3D_GLC(glDisable(GL_CULL_FACE));
    if(cap_blend) W3D_GLC(glEnable(GL_BLEND)); else W3D_GLC(glDisable(GL_BLEND));
    if(cap_scissor) W3D_GLC(glEnable(GL_SCISSOR_TEST)); else W3D_GLC(glDisable(GL_SCISSOR_TEST));
    if(cap_offset) W3D_GLC(glEnable(GL_POLYGON_OFFSET_FILL)); else W3D_GLC(glDisable(GL_POLYGON_OFFSET_FILL));
    W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex));
}
void Viewport(int x,int y,int w,int h){ if(w<0)w=0; if(h<0)h=0;
    if(!(byDraw && SameRect(viewportGL,x,y,w,h))){ W3D_GLC(glViewport(x,y,w,h)); viewportGL[0]=x; viewportGL[1]=y; viewportGL[2]=w; viewportGL[3]=h; }
    vpCurrent[0]=(float)x; vpCurrent[1]=(float)y; vpCurrent[2]=(float)w; vpCurrent[3]=(float)h; }
// clamp de w/h a >=0: un ancho/alto negativo (viewport muy angosto - iconos/margenes en el outliner)
// es GL_INVALID_VALUE en WebGL y deja el scissor en mal estado. Clampeando no rompe el clipping.
static int scissorRequest[4] = {0,0,0,0}, depthFuncRequest = (int)DepthLess, depthMaskRequest = 1;   // (lo ultimo pedido, aunque GL ya lo tuviera)
void Scissor(int x,int y,int w,int h){ if(w<0)w=0; if(h<0)h=0;
    scissorRequest[0]=x; scissorRequest[1]=y; scissorRequest[2]=w; scissorRequest[3]=h;
    if(byDraw && SameRect(scissorGL,x,y,w,h)) return;
    W3D_GLC(glScissor(x,y,w,h)); scissorGL[0]=x; scissorGL[1]=y; scissorGL[2]=w; scissorGL[3]=h; }
void GetViewport(int v[4]){ GLint g[4]; W3D_GLC(glGetIntegerv(GL_VIEWPORT,g)); for(int i=0;i<4;i++) v[i]=(int)g[i]; }
void GetScissor(int v[4]){ GLint g[4]; W3D_GLC(glGetIntegerv(GL_SCISSOR_BOX,g)); for(int i=0;i<4;i++) v[i]=(int)g[i]; }
bool ScissorActive(){ return W3D_GLC(glIsEnabled(GL_SCISSOR_TEST))==GL_TRUE; }
void DepthFunc(DepthCmp c){ depthFuncRequest=(int)c; const GLenum f = c==DepthLEqual?GL_LEQUAL:(c==DepthEqual?GL_EQUAL:(c==DepthAlways?GL_ALWAYS:GL_LESS));
    if(byDraw && depthFuncGL==(int)f) return; W3D_GLC(glDepthFunc(f)); depthFuncGL=(int)f; }
void DepthMask(bool w){ depthMaskRequest=w?1:0; if(byDraw && depthMaskGL==(w?1:0)) return; W3D_GLC(glDepthMask(w?GL_TRUE:GL_FALSE)); depthMaskGL=w?1:0; }
static unsigned maskColor = 15;   // (para la clave del programa, ver PickProgram)
void ColorMask(bool r,bool g,bool b,bool a){
    const unsigned m = (r?1u:0u)|(g?2u:0u)|(b?4u:0u)|(a?8u:0u);
    if(byDraw && colorMaskGL==(int)m){ maskColor = m; return; }
    W3D_GLC(glColorMask(r,g,b,a)); maskColor = m; colorMaskGL=(int)m; }
void LineWidth(float px){ if(byDraw && lineWidthGL==px) return; W3D_GLC(glLineWidth(px)); lineWidthGL=px; }
void FrontFace(bool ccw){ if(byDraw && frontFaceGL==(ccw?1:0)) return; W3D_GLC(glFrontFace(ccw?GL_CCW:GL_CW)); frontFaceGL=ccw?1:0; }
void CullFrontalFaces(bool on){ W3D_GLC(glCullFace(on?GL_FRONT:GL_BACK)); }
void PolygonOffset(float factor,float units){ W3D_GLC(glPolygonOffset(factor,units)); }
static void FitProjection(float bias, float n, float f);
void DepthRange(float n,float f){ FitProjection(-1e30f, n, f); }   // (en la proyeccion: ver FitProjection)
void ReadPixelsBegin(int,int,int,int){}   // (lee el framebuffer de verdad)
void ReadPixelsRGBA(int x,int y,int w,int h,unsigned char* px){ W3D_GLC(glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px)); }
void BlurY(float amt){ blurY = (amt > 0.0f) ? amt : 0.0f; }   // blur vertical por shader
void AlphaTest(float ref){ alphaRef = (ref > 0.0f) ? ref : 0.0f; }  // descarte por alpha
void AlphaPacked(bool yes){ alphaDown = yes ? 1 : 0; }   // alpha en la mitad de abajo
// la ECUACION tambien (mismo porque que el backend fijo, ver w3dGraphics.cpp:
// tras un pase SUSTRACTIVO, reponer solo la func dejaba todo lo alpha-blended
// RESTANDO del framebuffer: silueta negra con el alpha bien).
// el modo de blending puesto, como numero chico (para la clave del programa, ver PickProgram)
static unsigned blendMode = 0;
enum { BAlpha, BOff, BAdd, BAddAlpha, BMultiply, BScreen, BPremult, BSubtract, BMLast2x, BMLastPartial };
// (con un programa por draw: el modo que GL ya tiene no se vuelve a poner)
static bool BlendAlreadyPlaced(unsigned m){
    if(byDraw && blendGL==(int)m){ blendMode = m; return true; }
    BatchEmpty();   // (lo juntado, con el modo que tenia)
    blendMode = m; blendGL = (int)m; return false; }
void BlendAlpha(){ if(BlendAlreadyPlaced(BAlpha)) return; W3D_GLC(glBlendEquation(GL_FUNC_ADD)); W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); }
// (los MISMOS modos que el backend fijo: antes aca solo estaban Multiply y Add, y Multiply 2x, Subtract y Screen salian
//  como Mix)
void BlendMode(int mode){
    if(BlendAlreadyPlaced(mode==1 ? BMultiply : mode==2 ? BAdd : mode==3 ? BMLast2x : mode==4 ? BSubtract : mode==5 ? BScreen :
                     mode==6 ? BMLastPartial : BAlpha)) return;
    W3D_GLC(glBlendEquation(mode==4 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD));   // (Subtract: la resta de verdad)
    if(mode==1)      W3D_GLC(glBlendFunc(GL_DST_COLOR, GL_ZERO));         // Multiply
    else if(mode==2) W3D_GLC(glBlendFunc(GL_ONE, GL_ONE));               // Add
    else if(mode==3) W3D_GLC(glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR));   // Multiply 2x
    else if(mode==4) W3D_GLC(glBlendFunc(GL_ONE, GL_ONE));               // Subtract (REVERSE_SUBTRACT)
    else if(mode==5) W3D_GLC(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR)); // Screen
    else if(mode==6) W3D_GLC(glBlendFunc(GL_DST_COLOR, GL_SRC_ALPHA));   // Multiply con su factor
    else             W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); // Mix
}
// modos de mezcla nombrados (ver enum Mezcla). GLES2/WebGL: glBlendEquation es nativo.
void SetMix(int m){
    if(BlendAlreadyPlaced(m==MixOff ? BOff : m==MixAdd ? BAdd : m==MixAddAlpha ? BAddAlpha : m==MixMultiply ? BMultiply :
                     m==MixScreen ? BScreen : m==MixPremult ? BPremult : m==MixSubtract ? BSubtract : BAlpha)) return;
    W3D_GLC(glBlendEquation(GL_FUNC_ADD)); // reset (por si venia de Substract)
    switch(m){
        case MixOff:      W3D_GLC(glBlendFunc(GL_ONE, GL_ZERO)); break;
        case MixAdd:      W3D_GLC(glBlendFunc(GL_ONE, GL_ONE)); break;
        case MixAddAlpha: W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE)); break;
        case MixMultiply: W3D_GLC(glBlendFunc(GL_DST_COLOR, GL_ZERO)); break;
        case MixScreen:   W3D_GLC(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR)); break;
        case MixPremult:  W3D_GLC(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)); break;
        case MixSubtract: W3D_GLC(glBlendEquation(GL_FUNC_REVERSE_SUBTRACT)); W3D_GLC(glBlendFunc(GL_ONE, GL_ONE)); break;
        case MixAlpha:
        default:             W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); break;
    }
}
void SmoothShading(bool){}   // ES2 siempre interpola (smooth); FLAT seria un shader aparte -> TODO
void FastPerspective(){}     // ES2 corrige perspectiva siempre
void Invalidate(){ texGenCache = -1; matcapCache = -1; }   // (solo los caches de la matriz de textura, como el fijo)
// LA PROYECCION AJUSTADA: el sesgo en metros (BiasMeters) y el RANGO DE PROFUNDIDAD (DepthRange) se ponen en la matriz
// de proyeccion, sobre la del pase (la que estaba puesta mientras los dos eran neutros; BaseState los vuelve a neutros en
// cada cuadro). El rango no va con glDepthRangef: el driver de la VideoCore IV (Galaxy Ace, Android 2.3) dejaba mal la
// profundidad de lo que se dibujaba despues de un rango (1,1) -el cielo de Daytona- y el asfalto translucido tapaba la
// parte de abajo de los autos. En la matriz: z' = (f - n) z + (f + n - 1) w (y el lejano un pelo antes del plano de
// corte: con z = w justo, el redondeo lo recortaba)
static float gSettingsBase[16];
static float gSettingsBias = 0.0f, gSettingsN = 0.0f, gSettingsF = 1.0f;
static void FitProjection(float bias, float n, float f) {
    if (bias == -1e30f) bias = gSettingsBias;   // (DepthRange: el sesgo queda)
    if (n < 0.0f) n = 0.0f; if (f > 1.0f) f = 1.0f;
    if (bias == gSettingsBias && n == gSettingsN && f == gSettingsF) return;
    if (gSettingsBias == 0.0f && gSettingsN == 0.0f && gSettingsF == 1.0f) GetMatrix(Projection, gSettingsBase);
    float P[16]; for (int i = 0; i < 16; i++) P[i] = gSettingsBase[i];
    P[14] -= P[10] * bias;
    if (!(n == 0.0f && f == 1.0f)) {
        const float a = f - n, b = (f + n - 1.0f) * 0.99999f;
        for (int c = 0; c < 4; c++) P[c * 4 + 2] = a * P[c * 4 + 2] + b * P[c * 4 + 3];   // (fila z; la w: c*4+3)
    }
    MatrixMode(Projection); LoadMatrix(P); MatrixMode(ModelView);
    gSettingsBias = bias; gSettingsN = n; gSettingsF = f;
}
void BiasMeters(float m){ FitProjection(m, gSettingsN, gSettingsF); }
// (lo MISMO que el backend fijo: antes faltaban el modo de textura, su matriz y la unidad 1. Un CONSTRUCTOR de una rutina
//  con "Modo de textura: reemplazar" -los juegos 2D, que fijan el estado una vez- dejaba el texenv en REPLACE y toda la
//  interfaz que se dibujaba despues de la escena salia BLANCA: las barras, las tarjetas, el boton verde del cuadro)
void BaseState(){
    lightPixel = false; FitProjection(0.0f, 0.0f, 1.0f);
    BiasMeters(0.0f); AlphaTest(0.0f); Disable(PolygonOffsetFill);
    TexMatrixMatcap(false); TexGenSphere(false); TexEnvReplace(false); MixTexturesOff();
    SmoothShading(true); Disable(Lighting); Disable(ColorMaterial);
}
bool SnapshotStateSupported(){ return false; }   // sin cache: el constructor de las rutinas corre en cada cuadro
void SnapshotStateTake(){}
void SnapshotStateRestore(){}

// ============================================================================
//  Luz / material
// ============================================================================
// ---- luces: se guardan como las guarda GL (posicion y direccion del cono ya en OJO, con la modelview de AHORA)
static void InEyePoint(const float* v, float* out){   // posicion: w 0 = direccional (solo rota), 1 = puntual
    const float* m=curMV.m;
    for(int k=0;k<3;k++) out[k]=m[k]*v[0]+m[4+k]*v[1]+m[8+k]*v[2]+m[12+k]*v[3];
    out[3]=v[3];
}
void LightNfv(int i,LightFv p,const float* v){
    if(i<0 || i>=LightsMax) return;
    Light& L=lights[i];
    if(p==LightPosition){    InEyePoint(v,L.pos); L.area=false; }   // (cada luz se arma desde la posicion: la de area despues)
    else if(p==LightArea){   // LA DE AREA: sus medios lados en OJO con la modelview de AHORA (la escala la agranda)
        const float* m=curMV.m;
        for(int k=0;k<3;k++){ L.ax[k]=m[k]*v[0]; L.ay[k]=m[4+k]*v[1]; }
        L.area=(v[0]>0.0f && v[1]>0.0f); L.areaRound=(v[2]>0.5f);
    }
    else if(p==LightDiffuse)  for(int k=0;k<4;k++) L.dif[k]=v[k];
    else if(p==LightAmbient)  for(int k=0;k<4;k++) L.amb[k]=v[k];
    else if(p==LightSpecular) for(int k=0;k<4;k++) L.spec[k]=v[k];
    lightsDirty=true;
}
void LightNf(int i,LightF p,float v){
    if(i<0 || i>=LightsMax) return;
    lights[i].att[p==LightConstantAtt ? 0 : (p==LightLinearAtt ? 1 : 2)]=v;
    lightsDirty=true;
}
void LightNSpot(int i,float cut,float exponent,const float* dir3){
    if(i<0 || i>=LightsMax) return;
    Light& L=lights[i];
    const float* m=curMV.m;   // la direccion solo rota
    for(int k=0;k<3;k++) L.dir[k]=m[k]*dir3[0]+m[4+k]*dir3[1]+m[8+k]*dir3[2];
    L.cutCos=(cut>=180.0f) ? -1.0f : cosf(cut*0.01745329252f);
    L.expo=exponent;
    L.area=false;   // (un foco comun; la de area manda sus medidas despues: LightArea)
    lightsDirty=true;
}
void LightN(int i,bool on){
    if(i<0 || i>=LightsMax || lights[i].on==on) return;
    lights[i].on=on; lightsDirty=true;
}
void Light0fv(LightFv p,const float* v){ LightNfv(0,p,v); }
void Light0f(LightF p,float v){ LightNf(0,p,v); }
void SetLightEnabled(unsigned int id,bool on){
#ifdef GL_LIGHT0
    const unsigned int l0=(unsigned int)GL_LIGHT0;
#else
    const unsigned int l0=0x4000u;
#endif
    if(id>=l0 && id<l0+(unsigned int)LightsMax) LightN((int)(id-l0),on);
}
void LightModelAmbient(const float* c){ for(int k=0;k<4;k++) ambGlobal[k]=c[k]; }
static bool twoFaces = false;   // LightModelTwoSide: la cara de atras con la normal dada vuelta (el pase Solido)
void LightModelTwoSide(bool on){ twoFaces = on; }   // (el programa entero ilumina una cara)
void Material(MatParam p,const float* rgba){
    if(p==MatDiffuse)       for(int i=0;i<4;i++) matDiff[i]=rgba[i];
    else if(p==MatAmbient)  for(int i=0;i<4;i++) matAmb[i]=rgba[i];
    else if(p==MatSpecular) for(int i=0;i<4;i++) matSpec[i]=rgba[i];
    else if(p==MatEmission) for(int i=0;i<4;i++) matEmis[i]=rgba[i];
}
void MaterialShininess(float s){ matShine=s; } // exponente specular (0..128)
static bool MapsHas(){
    if(mapsOk < 0){
#if defined(W3D_VKGL)
        mapsOk = 1;   // (la capa de Vulkan: 16 unidades)
#elif defined(W3D_ES2_NATIVE)
        mapsOk = 0;   // (ES 2.0 de verdad: sin las derivadas de la normal y con pocas unidades)
#else
        GLint u = 0; glGetIntegerv(0x8872 /*GL_MAX_TEXTURE_IMAGE_UNITS*/, &u); while(glGetError() != GL_NO_ERROR){}
        mapsOk  = (u >= 11) ? 1 : 0;
#endif
    }
    return mapsOk == 1;
}
// LA SONDA DE LUZ del objeto que se dibuja (AmbientSH): los 9 coeficientes como vec4 (w sin usar) y ojo -> mundo
static float shU[36]; static float eyeWorld[9] = {1,0,0, 0,1,0, 0,0,1}; static bool shOn = false;
void AmbientSH(const float* sh, const float* m){
    if(!sh){ shOn = false; return; }
    for(int k=0;k<9;k++){ shU[k*4]=sh[k*3]; shU[k*4+1]=sh[k*3+1]; shU[k*4+2]=sh[k*3+2]; shU[k*4+3]=0.0f; }
    if(m) for(int i=0;i<9;i++) eyeWorld[i]=m[i];
    shOn = true;
}
bool MaterialMaps(unsigned normal, unsigned rug, unsigned met, unsigned alpha, unsigned emis){
    mapTex[0] = normal; mapTex[1] = rug; mapTex[2] = met; mapTex[3] = alpha; mapTex[4] = emis;
    return !byDraw && MapsHas();
}
void MaterialPBR(float metal,float rug,float f0,float trans,bool ssr){ matPbr[0]=metal; matPbr[1]=rug; matPbr[2]=f0; matPbr[3]=trans; matExtra[0]=ssr?1.0f:0.0f; }
// ============================================================================
//  LAS SOMBRAS EN TIEMPO REAL (ver w3dGraphics.h). EL MAPA: la profundidad vista desde la luz, empaquetada en una textura
//  RGBA de 8 bits por canal (sin texturas de profundidad: anda en cualquier ES 2.0, en la PC y en Vulkan) con su z-buffer
//  en un renderbuffer, los dos en un FBO. Se dibuja con el pase especial 5 (W3D_P_SOMBRA). LOS QUE LAS RECIBEN: el programa
//  entero con W3D_SOMBRA (PickProgramInteger) lee el mapa en la unidad ShadowUnit() con lo que dejo ShadowReceptors
// ============================================================================
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16 0x81A5
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
static GLuint shadowFbo = 0, shadowTex = 0, shadowRb = 0; static int shadowW = 0, shadowH = 0;
static int  shadowOk = -1;             // -1 sin probar; 0 este GL no arma el FBO; 1 anda
static float shadowD[ShadowDataN];     // los datos del pase (la disposicion, en w3dGraphics.h)
static float shadowVpPrev[4]; static bool shadowScPrev = false; static int shadowScRect[4];
// la unidad del mapa: la 11 donde estan los mapas del material (6..10); si no, la 6 (los mapas no se usan)
static int ShadowUnit(){ return MapsHas() ? 11 : 6; }
// LA CAPTURA FUERA DE PANTALLA (CaptureBegin, mas abajo): su FBO, mientras esta puesta el mapa de sombras vuelve a el
static GLuint capFbo = 0, capTex = 0, capRb = 0; static int capW = 0, capH = 0; static bool capActive = false;
static float capVpPrev[4]; static bool capScPrev = false; static int capScRect[4]; static float capClearPrev[4];
static void ShadowsForgetGL(){ shadowFbo = shadowTex = shadowRb = 0; shadowW = shadowH = 0; shadowOk = -1; shadowActive = false;
                               capFbo = capTex = capRb = 0; capW = capH = 0; capActive = false; }
static void ShadowMapDelete(){
    if(shadowFbo){ W3D_GLC(glDeleteFramebuffers(1, &shadowFbo)); shadowFbo = 0; }
    if(shadowRb){ W3D_GLC(glDeleteRenderbuffers(1, &shadowRb)); shadowRb = 0; }
    if(shadowTex){ texParamGL.erase(shadowTex); W3D_GLC(glDeleteTextures(1, &shadowTex)); shadowTex = 0; }
    shadowW = shadowH = 0;
}
bool ShadowsSupported(){
    if(byDraw || shadowOk == 0) return false;   // (un programa por draw: las GPU chicas, sin sombras)
#if defined(W3D_VKGL)
    return true;
#else
#if !defined(W3D_ES2_NATIVE)
    if(!glGenFramebuffers || !glGenRenderbuffers || !glFramebufferRenderbuffer || !glRenderbufferStorage) return false;
#endif
    static int units = -1;
    if(units < 0){ GLint u = 0; glGetIntegerv(0x8872 /*GL_MAX_TEXTURE_IMAGE_UNITS*/, &u); while(glGetError() != GL_NO_ERROR){} units = (int)u; }
    return units >= 7;
#endif
}
bool ShadowMapBegin(int w, int h){
    if(!ready || w <= 0 || h <= 0 || !ShadowsSupported()) return false;
    BatchEmpty();
    if(!shadowFbo || w != shadowW || h != shadowH){
        ShadowMapDelete();
        const int u = ShadowUnit();
        W3D_GLC(glGenTextures(1, &shadowTex)); texParamGL.erase(shadowTex);
        W3D_GLC(glActiveTexture(GL_TEXTURE0 + u)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, shadowTex));
        W3D_GLC(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));   // (la profundidad empaquetada no se mezcla)
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, boundTex));
        W3D_GLC(glGenRenderbuffers(1, &shadowRb)); W3D_GLC(glBindRenderbuffer(GL_RENDERBUFFER, shadowRb));
        W3D_GLC(glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h));
        W3D_GLC(glGenFramebuffers(1, &shadowFbo)); W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo));
        W3D_GLC(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadowTex, 0));
        W3D_GLC(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, shadowRb));
        if(W3D_GLC(glCheckFramebufferStatus(GL_FRAMEBUFFER)) != GL_FRAMEBUFFER_COMPLETE){
            W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER, 0));
            ShadowMapDelete(); shadowOk = 0;
            w3dLogfW("[w3dGLES2] el mapa de sombras (%dx%d) no se puede armar en este GL: sin sombras", w, h);
            return false;
        }
        shadowW = w; shadowH = h; shadowOk = 1;
    } else W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo));
    for(int i=0;i<4;i++) shadowVpPrev[i] = vpCurrent[i];
    shadowScPrev = cap_scissor; for(int i=0;i<4;i++) shadowScRect[i] = scissorRequest[i];
    if(cap_scissor) Disable(ScissorTest);
    Viewport(0, 0, w, h);
    // todo LEJOS (blanco = la profundidad mas grande) y el z-buffer al fondo
    W3D_GLC(glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
    DepthMask(true); ColorMask(true, true, true, true);
    W3D_GLC(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    W3D_GLC(glClearColor(clearCol[0], clearCol[1], clearCol[2], clearCol[3]));
    passEsp = 5;
    return true;
}
void ShadowMapBox(int x, int y, int w, int h){ if(passEsp == 5){ BatchEmpty(); Viewport(x, y, w, h); } }
void ShadowMapFinish(){
    if(passEsp != 5) return;
    BatchEmpty();
    passEsp = 0;
    W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER, capActive ? capFbo : 0));
    Viewport((int)shadowVpPrev[0], (int)shadowVpPrev[1], (int)shadowVpPrev[2], (int)shadowVpPrev[3]);
    if(shadowScPrev){ Scissor(shadowScRect[0], shadowScRect[1], shadowScRect[2], shadowScRect[3]); Enable(ScissorTest); }
}
static void CaptureDelete(){
    if(capFbo){ W3D_GLC(glDeleteFramebuffers(1, &capFbo)); capFbo = 0; }
    if(capRb){ W3D_GLC(glDeleteRenderbuffers(1, &capRb)); capRb = 0; }
    if(capTex){ texParamGL.erase(capTex); W3D_GLC(glDeleteTextures(1, &capTex)); capTex = 0; }
    capW = capH = 0;
}
bool CaptureBegin(int w, int h){
    // (un programa por draw -las GPU chicas-: la sonda de reflejo de tiempo real no, por lo que cuesta; el render a
    // archivo si (w3dRenderFinal): en Android 2.3 sus franjas van aca para no salir con el RGB565 de la ventana. Ahi la
    // sonda tampoco entra: la captura ya esta puesta)
    if(!ready || w <= 0 || h <= 0 || (byDraw && !w3dRenderFinal) || capActive || passEsp != 0) return false;
#if !defined(W3D_ES2_NATIVE) && !defined(W3D_VKGL)
    if(!glGenFramebuffers || !glGenRenderbuffers || !glFramebufferRenderbuffer || !glRenderbufferStorage) return false;
#endif
    BatchEmpty();
    if(!capFbo || w != capW || h != capH){
        CaptureDelete();
        W3D_GLC(glGenTextures(1, &capTex)); texParamGL.erase(capTex);
        W3D_GLC(glBindTexture(GL_TEXTURE_2D, capTex));
        W3D_GLC(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        W3D_GLC(glBindTexture(GL_TEXTURE_2D, boundTex));
        W3D_GLC(glGenRenderbuffers(1, &capRb)); W3D_GLC(glBindRenderbuffer(GL_RENDERBUFFER, capRb));
        W3D_GLC(glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h));
        W3D_GLC(glGenFramebuffers(1, &capFbo)); W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER, capFbo));
        W3D_GLC(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capTex, 0));
        W3D_GLC(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, capRb));
        if(W3D_GLC(glCheckFramebufferStatus(GL_FRAMEBUFFER)) != GL_FRAMEBUFFER_COMPLETE){
            W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER, 0));
            CaptureDelete();
            w3dLogfW("[w3dGLES2] la imagen fuera de pantalla (%dx%d) no se puede armar en este GL", w, h);
            return false;
        }
        capW = w; capH = h;
    } else W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER, capFbo));
    for(int i=0;i<4;i++){ capVpPrev[i] = vpCurrent[i]; capClearPrev[i] = clearCol[i]; }
    capScPrev = cap_scissor; for(int i=0;i<4;i++) capScRect[i] = scissorRequest[i];
    if(cap_scissor) Disable(ScissorTest);
    Viewport(0, 0, w, h);
    capActive = true;
    return true;
}
void CaptureFinish(){
    if(!capActive) return;
    BatchEmpty();
    capActive = false;
    W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    Viewport((int)capVpPrev[0], (int)capVpPrev[1], (int)capVpPrev[2], (int)capVpPrev[3]);
    if(capScPrev){ Scissor(capScRect[0], capScRect[1], capScRect[2], capScRect[3]); Enable(ScissorTest); }
    else if(cap_scissor) Disable(ScissorTest);
    ClearColor(capClearPrev[0], capClearPrev[1], capClearPrev[2], capClearPrev[3]);
}
void ShadowReceptors(const float* d){
    if(!d || !shadowTex){ shadowActive = false; return; }
    memcpy(shadowD, d, sizeof(shadowD)); shadowActive = true;
}
void ShadowReceive(int mode){ shadowMode = mode & 7; shadowMasc = (mode & 8) != 0; }
static void UploadShadows(){
    int idx = -1;   // (la luz que da sombra, en el lugar que le toco entre las prendidas: ver UploadLights)
    { const int slot = (int)shadowD[100];
      if(slot >= 0 && slot < LightsMax && lights[slot].on){ idx = 0; for(int i=0;i<slot;i++) if(lights[i].on) idx++; } }
    U_1I(uShadowLightV, idx); U_1I(uShadowLight, idx); U_1F(uShadowOn, shadowActive && shadowMode == 1 ? 1.0f : 0.0f);
    U_1F(uShadowBake, shadowActive && shadowMode >= 2 ? 1.0f : 0.0f);
    U_1F(uShadowBakeMode, shadowActive && shadowMode >= 2 ? (float)(shadowMode - 1) : 0.0f);
    {   // el PISO sin mascara: el ambiente de este objeto mas el color de la sombra (hasta 1)
        GLfloat pz[4];
        for (int k = 0; k < 3; k++) { const float v = ambGlobal[k] + shadowD[101 + k]; pz[k] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
        pz[3] = shadowMasc ? 1.0f : 0.0f;
        U_4F(uShadowFloor, pz);
    }
    U_4F(uShadowNV, &shadowD[88]); U_4F(uShadowCV, &shadowD[80]);
    U_1F(uShadowRV, 0.0f);   // (antes: el desenfoque lejano corria mas la normal lejos; la penumbra de contacto no lo necesita)
    { const GLfloat c[4] = { shadowD[101], shadowD[102], shadowD[103], 1.0f }; U_4F(uShadowColor, c); }   // el color de la sombra
    const int u = ShadowUnit();
    U_1I(uShadowTex, u);
    U_M4(uShadowM0, &shadowD[0]); U_M4(uShadowM1, &shadowD[16]); U_M4(uShadowM2, &shadowD[32]); U_M4(uShadowM3, &shadowD[48]);
    U_4F(uShadowBox0, &shadowD[64]); U_4F(uShadowBox1, &shadowD[68]); U_4F(uShadowBox2, &shadowD[72]); U_4F(uShadowBox3, &shadowD[76]);
    U_4F(uShadowCuts, &shadowD[80]); U_4F(uShadowRad, &shadowD[84]); U_4F(uShadowP, &shadowD[92]); U_4F(uShadowQ, &shadowD[96]);
    W3D_GLC(glActiveTexture(GL_TEXTURE0 + u)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, shadowTex));
    W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, boundTex));
}
// LAS COPIAS DE LA PANTALLA (el vidrio, los shaders escritos) en el FORMATO DE LA PANTALLA: con una de 16 bits (565: el
// Galaxy Ace y muchos Android) copiarla a una textura de 8 bits por canal obligaba al driver a convertir cada pixel (en el
// Ace, 60 ms de 180 por cuadro). Igual de nitida: la pantalla ya es 565
static GLenum CopyTypeScreen(){
    static GLenum type = 0;
    if(!type){
        GLint r = 8, g = 8, b = 8;
        glGetIntegerv(GL_RED_BITS, &r); glGetIntegerv(GL_GREEN_BITS, &g); glGetIntegerv(GL_BLUE_BITS, &b);
        while(glGetError() != GL_NO_ERROR){}   // (un GL de escritorio moderno no las tiene: queda el de 8 bits)
        type = (r == 5 && g == 6 && b == 5) ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE;
    }
    return type;
}
void SceneCopy(bool on){
    escVp[2] = 0.0f; escZ = false;
    if(!on) return;
    const int x = (int)vpCurrent[0], y = (int)vpCurrent[1], w = (int)vpCurrent[2], h = (int)vpCurrent[3];
    if(w <= 0 || h <= 0) return;
    W3D_GLC(glActiveTexture(GL_TEXTURE4));
    if(!texScene){ W3D_GLC(glGenTextures(1, &texScene)); texEscW = texEscH = 0; }
    W3D_GLC(glBindTexture(GL_TEXTURE_2D, texScene));
    if(w != texEscW || h != texEscH){
        W3D_GLC(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, CopyTypeScreen(), 0));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        texEscW = w; texEscH = h;
    }
    // (la fila que entiende el driver: el del Galaxy Ace cuenta desde ARRIBA y la deja dada vuelta. Sin esto el vidrio
    //  mostraba otra parte de la pantalla: la interfaz de abajo. Dada vuelta, el alto va negativo y el shader da vuelta la V)
    W3D_GLC(glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, CopyScreenRow(y, h), w, h));
#if defined(W3D_VKGL)
    // (Vulkan: la profundidad de la pantalla se copia tal cual, con la fila de arriba primero: el shader la lee dada vuelta)
    if(escZState >= 0){
        W3D_GLC(glActiveTexture(GL_TEXTURE0 + 5));
        if(!texEscZ){ W3D_GLC(glGenTextures(1, &texEscZ)); }
        W3D_GLC(glBindTexture(GL_TEXTURE_2D, texEscZ));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        // (si ESTA copia no se pudo -se dibujaba a una textura, el rectangulo se salia de la pantalla- solo este pase
        //  queda sin profundidad. Antes un fallo la apagaba para siempre: una miniatura o un render a textura dejaba
        //  los reflejos de toda la sesion en la aproximacion vieja, sin espejar)
        if(vkglCopyDepth(x, y, w, h)){ escZ = true; escZState = 1; escZTurn = true; }
        else escZ = false;
    }
#elif !defined(W3D_ES2_NATIVE)
    // LA PROFUNDIDAD del mismo rectangulo (los reflejos en pantalla: ver texEscZ). Si el driver no la copia (un error),
    // no se vuelve a intentar: quedan los reflejos aproximados
    if(escZState >= 0){
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
        while(glGetError() != GL_NO_ERROR){}
        W3D_GLC(glActiveTexture(GL_TEXTURE0 + 5));
        if(!texEscZ){ W3D_GLC(glGenTextures(1, &texEscZ)); texEscZW = texEscZH = 0; }
        W3D_GLC(glBindTexture(GL_TEXTURE_2D, texEscZ));
        if(w != texEscZW || h != texEscZH){
            W3D_GLC(glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 0));
            W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
            W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
            W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
            W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
            texEscZW = w; texEscZH = h;
        }
        W3D_GLC(glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, CopyScreenRow(y, h), w, h));
        const GLenum e = glGetError();
        static int failed = 0;
        if(e == GL_NO_ERROR){ escZ = true; escZState = 1; failed = 0; }
        else if(++failed < 30){ escZ = false; texEscZW = texEscZH = 0; }   // (un fallo suelto: se reintenta en el proximo pase)
        else { escZ = false; escZState = -1;
               w3dLogfW("[w3dGLES2] este driver no copia la profundidad (0x%x): los reflejos en pantalla quedan aproximados", (unsigned)e); }
    }
#endif
    W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, boundTex));
    escVp[0] = (float)x; escVp[1] = (float)y; escVp[2] = (float)w; escVp[3] = givenCopyTurn ? -(float)h : (float)h;
}
// el entorno como CUBEMAP (ver w3dGraphics.h): las 6 caras en el orden de GL, sin piramide (lineal)
unsigned int UploadCube(const unsigned char* const faces[6], int side){
    if(!ready || side <= 0) return 0;
    GLuint t = 0;
    W3D_GLC(glGenTextures(1, &t));
    W3D_GLC(glBindTexture(GL_TEXTURE_CUBE_MAP, t));
    for(int i=0;i<6;i++)
        W3D_GLC(glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, faces[i]));
    W3D_GLC(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    W3D_GLC(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    W3D_GLC(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    W3D_GLC(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    W3D_GLC(glBindTexture(GL_TEXTURE_CUBE_MAP, 0));
    return t;
}
void EnvironmentCube(unsigned int cube,unsigned int cubeB,const float* m){
    Environment(cube, cubeB, m);
    envCube = cube != 0;
}
void Environment(unsigned int tex,unsigned int texB,const float* m){
    envCube = false;
    envTex=tex; envTexB=texB?texB:tex;
    if(tex && m){ for(int f=0;f<3;f++){ envF[f*4]=m[f*3]; envF[f*4+1]=m[f*3+1]; envF[f*4+2]=m[f*3+2]; envF[f*4+3]=0.0f; } envF[3]=1.0f; }
    else envF[3]=0.0f;
}

// ============================================================================
//  Textura / texenv
// ============================================================================
void BindTexture(unsigned int id){ if(byDraw && id==boundTex) return; boundTex=id; W3D_GLC(glBindTexture(GL_TEXTURE_2D,id)); }
// el min-filter con la PIRAMIDE si la textura la subio y los mipmaps estan prendidos (como el backend fijo): sin esto lo
// lejano se veia con serrucho y titilaba en web/Android y no en el PC
static GLint FilterMin(unsigned int id, bool linear){
    if(MipmapsGlobal() && TexHasMips(id)) return linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    return linear ? GL_LINEAR : GL_NEAREST;
}
void TexFilter(bool linear){
    const GLint mn = FilterMin(boundTex,linear), mg = linear?GL_LINEAR:GL_NEAREST;
    if(byDraw){ std::map<unsigned,TexParamGL>::iterator it = texParamGL.find(boundTex);
        if(it != texParamGL.end() && it->second.minF==mn && it->second.magF==mg) return;
        TexParamGL& t = texParamGL[boundTex]; if(it == texParamGL.end()) t.wrap = -1; t.minF = mn; t.magF = mg; }
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,mn)); W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,mg)); }
void TexWrap(bool repeat){
    const GLint w = repeat?GL_REPEAT:GL_CLAMP_TO_EDGE;
    if(byDraw){ std::map<unsigned,TexParamGL>::iterator it = texParamGL.find(boundTex);
        if(it != texParamGL.end() && it->second.wrap==w) return;
        TexParamGL& t = texParamGL[boundTex]; if(it == texParamGL.end()){ t.minF = -1; t.magF = -1; } t.wrap = w; }
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,w)); W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,w)); }
// el combiner de la unidad 0, como glTexEnv: apagar cualquiera de los modos vuelve a GL_MODULATE
void TexEnvDot3(bool on){ texEnv = on ? 2 : 0; }
void ShadingPerPixel(bool on){ lightPixel = on; }
bool ShadingPerPixelSupported(){ return progPixel != -2; }
void TexEnvReplace(bool on){ texEnv = on ? 1 : 0; }
void TexEnvAlphaOnly(bool on){ texEnv = on ? 3 : 0; }
// MEZCLA DE TEXTURAS: la unidad 1 del pipeline fijo (lightmaps, la textura B del material, la rampa del toon). En el
// shader es un segundo sampler sobre lo que salio de la unidad 0: interpolar con k, multiplicar (x1 / x2) o restar.
// Los parametros de filtro y repeticion son del OBJETO textura B (igual que en el backend fijo).
bool MixTexturesSupported(){ return true; }
void MixTextures(unsigned int idB, float k, bool filtered, bool repeat, int mode){
    if(!idB){ MixTexturesOff(); return; }
    if(k<0.0f) k=0.0f; if(k>1.0f) k=1.0f;
    if(mode<0 || mode>4) mode=0;
    if(mode==4) mode=1;   // (multiplicar simple = multiplicar: en el shader es lo mismo)
    mixOn=true; mixTex=idB; mixK=k; texMixMode=mode;
    texParamGL.erase(idB);   // (sus parametros se ponen aca, sin el cache)
    W3D_GLC(glActiveTexture(GL_TEXTURE1));
    W3D_GLC(glBindTexture(GL_TEXTURE_2D,idB));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,FilterMin(idB,filtered)));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,filtered?GL_LINEAR:GL_NEAREST));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE));
    W3D_GLC(glActiveTexture(GL_TEXTURE0));
}
void MixTexturesOff(){ mixOn=false; mixUV=0; mixUVVbo=0; }
bool MixTexturesActive(){ return mixOn; }
void MixTexturesUV(const float* p){ mixUV=p; if(p) mixUVVbo=0; }
void MixTexturesUVVBO(unsigned int h){ mixUVVbo=h; if(h) mixUV=0; }
// (sin UV enteras en la unidad 1 de este backend: la unidad 1 sigue a la 0)
void MixTexturesUV2s(const short*){ mixUV=0; mixUVVbo=0; }
// (sin matriz de textura en la unidad 1 de este backend: la segunda capa queda quieta)
void MixTexturesMatrixUV(float,float,float,float){}
void MixDeletedTexture(unsigned int id){ if(id && id==mixTex){ mixTex=0; mixOn=false; } texParamGL.erase(id); }
unsigned int ErrorGL(){ return (unsigned int)glGetError(); }
// SPHERE-MAP (GL_SPHERE_MAP): las UV salen del reflejo de la vista en la normal (en el vertex shader). Como el
// backend fijo, la matriz de textura da vuelta la V para que el reflejo no salga cabeza abajo.
void TexGenSphere(bool on){
    if(texGenCache == (on ? 1 : 0)) return;
    texGenCache = on ? 1 : 0;
    matcapCache = -1;
    texGenSphere=on;
    curTex.Identity();
    if(on){ curTex.m[5]=-1.0f; curTex.m[13]=1.0f; }   // v -> 1 - v
}
// MATCAP sin texgen: las normales llegan como UV en bytes (TexCoordPointer3b) y esta matriz de textura las lleva a
// OJO y a [0,1]: la MISMA matriz que arma el backend fijo (con la modelview de AHORA, la del objeto)
void TexMatrixMatcap(bool on){
    if(!on && matcapCache == 0) return;
    matcapCache = on ? 1 : 0;
    curTex.Identity();
    if(!on) return;
    const float* mv=curMV.m;
    const float s=0.5f/127.0f;
    float* T=curTex.m;
    T[0]=s*mv[0];  T[1]=-s*mv[1]; T[2]=0.0f;  T[3]=0.0f;
    T[4]=s*mv[4];  T[5]=-s*mv[5]; T[6]=0.0f;  T[7]=0.0f;
    T[8]=s*mv[8];  T[9]=-s*mv[9]; T[10]=0.0f; T[11]=0.0f;
    T[12]=0.5f;    T[13]=0.5f;    T[14]=0.0f; T[15]=1.0f;
}
// desplazamiento + escala de las UV (la rutina anima con esto el agua, los reflejos, las cintas...)
void TexMatrixUV(float du,float dv,float su,float sv){
    const bool ident = du == 0.0f && dv == 0.0f && su == 1.0f && sv == 1.0f;
    if(ident ? matcapCache == 0 : (matcapCache == 2 && du == uvMat[0] && dv == uvMat[1] && su == uvMat[2] && sv == uvMat[3])) return;
    matcapCache = ident ? 0 : 2;
    uvMat[0] = du; uvMat[1] = dv; uvMat[2] = su; uvMat[3] = sv;
    curTex.Identity();
    curTex.m[0]=su; curTex.m[5]=sv; curTex.m[12]=du; curTex.m[13]=dv;
}
// la TRANSFORMACION UV de un material (afin: desplazamiento + escala + rotacion, ver w3dGraphics.h). Comparte el cache
// con el matcap y TexMatrixUV (modo 3): cualquiera de los dos la resetea
void TexMatrixUVAffine(const float* m6){
    if(!m6){ TexMatrixUV(0.0f,0.0f,1.0f,1.0f); return; }
    if(matcapCache == 3){ bool same = true; for(int k=0;k<6 && same;k++) same = (uvAff[k] == m6[k]); if(same) return; }
    matcapCache = 3;
    for(int k=0;k<6;k++) uvAff[k] = m6[k];
    curTex.Identity();
    curTex.m[0]=m6[0]; curTex.m[1]=m6[1]; curTex.m[4]=m6[2]; curTex.m[5]=m6[3]; curTex.m[12]=m6[4]; curTex.m[13]=m6[5];
}
bool HasTexGen(){ return true; }   // el sphere-map y el matcap van por el shader (como el texgen de GL)

// ============================================================================
//  Color uniforme (cuando NO hay array de color)
// ============================================================================
// (el alfa por g_alphaColor: la UI translucida, ver w3dGraphics.h)
void Color4f(float r,float g,float b,float a){ curColor[0]=r; curColor[1]=g; curColor[2]=b; curColor[3]=a*g_alphaColor; }
void Color4fv(const float* c){ for(int i=0;i<4;i++) curColor[i]=c[i]; curColor[3]*=g_alphaColor; }
void Color4ub(unsigned char r,unsigned char g,unsigned char b,unsigned char a){ curColor[0]=r/255.0f; curColor[1]=g/255.0f; curColor[2]=b/255.0f; curColor[3]=a/255.0f*g_alphaColor; }

// ============================================================================
//  Fog
// ============================================================================
void FogMode(int m){ fogMode = (m < 0 || m > 2) ? 0 : m; }
void FogDensity(float d){ fogDens = d; }
void FogStart(float z){ fogStart=z; }
void FogEnd(float z){ fogEnd=z; }
void FogColor(const float* rgba){ for(int i=0;i<4;i++) fogColor[i]=rgba[i]; }
void PointSize(float px){ pointSize=px; } // gl_PointSize en el VS (ES2 no tiene glPointSize)

// ============================================================================
//  Vertex arrays -> se suben a VBOs en el draw
// ============================================================================
void EnableArray(VArray a){ if(a==VertexArray)aPos.on=true; else if(a==NormalArray)aNrm.on=true; else if(a==TexCoordArray)aUV.on=true; else if(a==ColorArray)aCol.on=true; }
void DisableArray(VArray a){ if(a==VertexArray)aPos.on=false; else if(a==NormalArray)aNrm.on=false; else if(a==TexCoordArray)aUV.on=false; else if(a==ColorArray)aCol.on=false; }
int ArrayState(VArray a){ return (a==VertexArray ? aPos.on : a==NormalArray ? aNrm.on : a==TexCoordArray ? aUV.on : aCol.on) ? 1 : 0; }
void VertexPointer2f(int stride,const float* p){ aPos.size=2; aPos.type=GL_FLOAT; aPos.stride=stride; aPos.ptr=p; aPos.vbo=0; }
void VertexPointer3f(int stride,const float* p){ aPos.size=3; aPos.type=GL_FLOAT; aPos.stride=stride; aPos.ptr=p; aPos.vbo=0; }
void VertexPointer4f(int stride,const float* p){ aPos.size=4; aPos.type=GL_FLOAT; aPos.stride=stride; aPos.ptr=p; aPos.vbo=0; }
void VertexPointer3s(int stride,const short* p){ aPos.size=3; aPos.type=GL_SHORT; aPos.stride=stride; aPos.ptr=p; aPos.vbo=0; }
void VertexPointer2s(int stride,const short* p){ aPos.size=2; aPos.type=GL_SHORT; aPos.stride=stride; aPos.ptr=p; aPos.vbo=0; }
void NormalPointer3b(const signed char* p){ aNrm.size=3; aNrm.type=GL_BYTE; aNrm.stride=0; aNrm.ptr=p; aNrm.vbo=0; }
void ColorPointer4ub(const unsigned char* p){ aCol.size=4; aCol.type=GL_UNSIGNED_BYTE; aCol.stride=0; aCol.ptr=p; aCol.vbo=0; }
void ColorPointer4f(int stride,const float* p){ aCol.size=4; aCol.type=GL_FLOAT; aCol.stride=stride; aCol.ptr=p; aCol.vbo=0; }
void TexCoordPointer2f(int stride,const float* p){ aUV.size=2; aUV.type=GL_FLOAT; aUV.stride=stride; aUV.ptr=p; aUV.vbo=0; }
void TexCoordPointer2s(int stride,const short* p){ aUV.size=2; aUV.type=GL_SHORT; aUV.stride=stride; aUV.ptr=p; aUV.vbo=0; }
void TexCoordPointer3b(const signed char* p, int count){ (void)count; aUV.size=3; aUV.type=GL_BYTE; aUV.stride=0; aUV.ptr=p; aUV.vbo=0; } // matcap: normales como UV (sin normalizar, como GL)

// ============================================================================
//  Draw: setup compartido de uniforms + atributos, y las variantes
// ============================================================================
static int bytesOf(unsigned type){ return (type==GL_FLOAT)?4:((type==GL_SHORT)?2:1); }
static void bindAttr(GLint loc,const Arr& ar,GLuint vbo,int nVerts,const float* def,bool allowNorm){
    if(loc<0) return;
    AttrGL& g = attrGL[loc];
    if(ar.on && ar.vbo){   // de un buffer object (la malla ya esta en la GPU): solo el puntero, si cambio
        BindArray(ar.vbo);
        GLboolean norm = (allowNorm && ar.type!=GL_FLOAT)?GL_TRUE:GL_FALSE;
        if(g.on != 1){ W3D_GLC(glEnableVertexAttribArray(loc)); g.on = 1; }
        if(g.vbo!=ar.vbo || g.ptr!=0 || g.size!=ar.size || g.type!=ar.type || g.norm!=norm || g.stride!=ar.stride){
            W3D_GLC(glVertexAttribPointer(loc,ar.size,ar.type,norm,ar.stride,(const void*)0));
            g.vbo = ar.vbo; g.ptr = 0; g.size = ar.size; g.type = ar.type; g.norm = norm; g.stride = ar.stride;
        }
    } else if(ar.on && ar.ptr && nVerts>0){
        int elem = ar.size * bytesOf(ar.type);
        // el ultimo vertice solo aporta 'elem' bytes: con stride>elem no hay que leer el relleno final
        int bytes = ar.stride ? ar.stride*(nVerts-1)+elem : elem*nVerts;
        BindArray(noVBO ? 0 : vbo);
        if(!noVBO) W3D_GLC(glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)bytes,ar.ptr,GL_DYNAMIC_DRAW));
        // normal (byte -127..127 -> -1..1) y color (ubyte 0..255 -> 0..1) SI se normalizan.
        // la POSICION NO: un short es una coord ENTERA de pantalla/mundo (ej. 0..800), NO un [-1,1]
        // -> normalizarla la mandaba a ~0 (el borde del viewport / sprites 2D quedaban invisibles).
        GLboolean norm = (allowNorm && ar.type!=GL_FLOAT)?GL_TRUE:GL_FALSE;
        if(g.on != 1){ W3D_GLC(glEnableVertexAttribArray(loc)); g.on = 1; }
        // (con arreglos del cliente el driver lee los datos en el draw: el mismo puntero con otro contenido no hace
        // falta volver a darlo. Con el VBO se da siempre)
        if(!noVBO || g.vbo!=0 || g.ptr!=ar.ptr || g.size!=ar.size || g.type!=ar.type || g.norm!=norm || g.stride!=ar.stride){
            W3D_GLC(glVertexAttribPointer(loc,ar.size,ar.type,norm,ar.stride,noVBO ? ar.ptr : (const void*)0));
            g.vbo = noVBO ? 0 : vbo; g.ptr = ar.ptr; g.size = ar.size; g.type = ar.type; g.norm = norm; g.stride = ar.stride;
        }
    } else {
        if(g.on != 0){ W3D_GLC(glDisableVertexAttribArray(loc)); g.on = 0; }
        if(!g.hasCte || memcmp(g.cte, def, sizeof(g.cte)) != 0){
            W3D_GLC(glVertexAttrib4fv(loc,def)); // constante (normal +Z, color = curColor)
            memcpy(g.cte, def, sizeof(g.cte)); g.hasCte = true;
        }
    }
}
// la matriz de las NORMALES: la inversa transpuesta de la modelview (3x3), como el pipeline fijo. Con una escala
// despareja la modelview sola tuerce las normales (y la luz). Columnas = productos cruz de las columnas / determinante.
static void MatrixNormal(float out[9]){
    const float* m=curMV.m;
    const float c0[3]={m[0],m[1],m[2]}, c1[3]={m[4],m[5],m[6]}, c2[3]={m[8],m[9],m[10]};
    float n0[3]={c1[1]*c2[2]-c1[2]*c2[1], c1[2]*c2[0]-c1[0]*c2[2], c1[0]*c2[1]-c1[1]*c2[0]};
    float n1[3]={c2[1]*c0[2]-c2[2]*c0[1], c2[2]*c0[0]-c2[0]*c0[2], c2[0]*c0[1]-c2[1]*c0[0]};
    float n2[3]={c0[1]*c1[2]-c0[2]*c1[1], c0[2]*c1[0]-c0[0]*c1[2], c0[0]*c1[1]-c0[1]*c1[0]};
    const float det=c0[0]*n0[0]+c0[1]*n0[1]+c0[2]*n0[2];
    if(fabsf(det)<1e-12f){ for(int k=0;k<3;k++){ out[k]=c0[k]; out[3+k]=c1[k]; out[6+k]=c2[k]; } return; }
    const float id=1.0f/det;
    for(int k=0;k<3;k++){ out[k]=n0[k]*id; out[3+k]=n1[k]*id; out[6+k]=n2[k]*id; }
}
// la primitiva del draw en curso (0 puntos, 1 lineas, 2 triangulos): la pone cada Draw* antes de setupState
static int primDraw = 2;
static void Prim(GLenum mode){ primDraw = (mode==GL_POINTS) ? 0 : (mode==GL_LINES || mode==GL_LINE_STRIP || mode==GL_LINE_LOOP) ? 1 : 2; }
// como llega un atributo: 0 = constante; si no, el tipo (1 float, 2 short, 3 byte, 4 ubyte) y el tamanio
static unsigned SignatureAttr(const Arr& a, int nV){
    if(!(a.on && (a.vbo || (a.ptr && nV>0)))) return 0;
    const unsigned t = (a.type==GL_FLOAT) ? 1u : (a.type==GL_SHORT) ? 2u : (a.type==GL_BYTE) ? 3u : 4u;
    return t | ((unsigned)(a.size-1) << 3);
}
// PROGRAMA ENTERO: el de siempre (luz por vertice) o, si el draw pide la luz por pixel, su gemelo con la luz en el
// fragment shader (se arma la primera vez que hace falta; si no entra en la GPU se queda el de siempre)
static void PickProgramInteger(){
    const bool light = cap_light && !(cap_tex && texEnv==1);
    const bool pix   = lightPixel && light;
    if(pix && progPixel == -1 && nPrograms < kProgramsMax){
        BuildProgram(Prog(nPrograms), (Key)(PAll | PLightPixel | (LightsMax << PLights)));
        if(Linked(Prog(nPrograms).id)) progPixel = nPrograms++;
        else { DeleteProgram(Prog(nPrograms)); progPixel = -2; w3dLogfW("[w3dGLES2] la luz por pixel no entra en esta GPU: queda por vertice"); }
    }
    // LAS SOMBRAS: el objeto que las recibe usa el gemelo con W3D_SOMBRA (por vertice o por pixel, como sin sombras). Se
    // arma la primera vez que hace falta; si no entra en la GPU, se dibuja sin sombras
    if(shadowActive && ((shadowMode == 1 && light) || shadowMode >= 2)){
        const int k = (pix && progPixel >= 0) ? 1 : 0;
        if(progShadow[k] == -1 && nPrograms < kProgramsMax){
            BuildProgram(Prog(nPrograms), (Key)(PAll | PShadow | (k ? PLightPixel : 0) | (LightsMax << PLights)));
            if(Linked(Prog(nPrograms).id)) progShadow[k] = nPrograms++;
            else { DeleteProgram(Prog(nPrograms)); progShadow[k] = -2; w3dLogfW("[w3dGLES2] las sombras no entran en esta GPU (luz por %s)", k ? "pixel" : "vertex"); }
        }
        if(progShadow[k] >= 0){ Program& ps = Prog(progShadow[k]); if(progUse != &ps) UseProgram(ps); return; }
    }
    Program& p = (pix && progPixel >= 0) ? Prog(progPixel) : Prog(0);
    if(progUse != &p) UseProgram(p);
}
// el programa de este draw, si hay uno por draw: el que tiene justo lo que se va a usar
static void PickProgram(int nV){
    if(!byDraw){ PickProgramInteger(); return; }
    const bool light = cap_light && !(cap_tex && texEnv==1);   // (como uLightOn, ver setupState)
    const bool mix = mixOn && mixTex;
    int nl = 0;
    if(light) for(int i=0;i<LightsMax;i++) if(lights[i].on) nl++;
    if(nl > kLightsByDraw) nl = kLightsByDraw;
    const unsigned f = (light ? PLight : 0) | ((texGenSphere && cap_tex) ? PTexGen : 0) | (pointSprite ? PSprite : 0)
                     | (cap_tex ? PTex : 0) | ((cap_tex && blurY > 0.0f) ? PBlur : 0) | ((cap_tex && alphaDown) ? PAlphaDown : 0)
                     | (mix ? PMix : 0) | (cap_fog ? PFog : 0) | (clipN > 0 ? PClip : 0)
                     | (alphaRef > 0.0f ? PDiscard : 0) | ((light && lightPixel) ? PLightPixel : 0) | ((unsigned)nl << PLights);
    // lo del driver (ver PState): solo los atributos que este programa lee
    const bool uv1 = mix && (mixUV || mixUVVbo || (aUV.on && aUV.type==GL_FLOAT && (aUV.vbo || (aUV.ptr && nV>0))));
    Key e = (Key)primDraw | ((Key)(cap_blend ? 1u + blendMode : 0u) << 2) | ((Key)maskColor << 6)
            | ((Key)SignatureAttr(aPos, nV) << 10) | ((Key)SignatureAttr(aCol, nV) << 15);
    if(f & (PLight|PTexGen)) e |= (Key)SignatureAttr(aNrm, nV) << 20;
    if(f & PTex)           e |= (Key)SignatureAttr(aUV, nV) << 25;
    if(uv1)                e |= (Key)1 << 30;
    // (los modos, constantes del programa: ver DefsProgram)
    if(f & PTex){ static const float kIdent[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                  e |= (Key)(texEnv & 3) << 31;
                  if(memcmp(curTex.m, kIdent, sizeof(kIdent)) != 0) e |= (Key)1 << 33; }
    if(f & PFog)        e |= (Key)(fogMode & 3) << 34;
    if(f & PMix)        e |= (Key)(texMixMode & 3) << 36;
    // la LUZ PBR por pixel, solo con lo que usa este draw: el PBR (rugosidad >= 0), el entorno 360 y la copia de la
    // pantalla (el vidrio y los reflejos en pantalla). Cada pedazo es mucho shader para la VideoCore IV
    if(f & PLightPixel){
        const bool pbr = matPbr[1] >= 0.0f;
        if(pbr) e |= (Key)1 << 38;
        if(pbr && envTex && envF[3] > 0.5f) e |= (Key)1 << 39;
        if(pbr && envTex && envF[3] > 0.5f && envCube) e |= (Key)1 << 41;   // (el entorno es un cubemap)
        if(pbr && escVp[2] > 0.0f && texScene && (matPbr[3] > 0.0f || matExtra[0] > 0.5f)) e |= (Key)1 << 40;
        // (el bit 43, alguna luz de area, no va: con un programa por draw la de area es el foco, ver ProgramWithArea)
    } else if((f & PLight) && shOn) e |= (Key)1 << 42;   // (la sonda de luz con la luz por vertice)
    const Key key = (Key)f | (e << PState);
    if(progUse && progUse->key == key) return;
    for(int i=0;i<nPrograms;i++) if(Prog(i).key == key){ UseProgram(Prog(i)); return; }
    if(nPrograms == kProgramsMax){   // (lleno: se tiran todos y se vuelven a armar los que hagan falta)
        for(int i=0;i<nPrograms;i++) DeleteProgram(Prog(i));
        nPrograms = 0;
    }
    BuildProgram(Prog(nPrograms), key);
    UseProgram(Prog(nPrograms++));
    // (cada uno nuevo le cuesta al driver ~35 ms la primera vez que dibuja: el log dice cuantos hay y cuando aparecen)
    w3dLogf("[w3dGLES2] programa %d (clave %08x%08x)", nPrograms, (unsigned)(key >> 32), (unsigned)key);
}
static void UploadShadows();
// las luces prendidas, juntas al principio de los arreglos del shader (solo cuando cambiaron o el programa todavia no
// las tiene: los uniforms quedan en cada programa)
static void UploadLights(){
    if(lightsDirty){ showLights++; lightsDirty=false; }
    if(progUse->showLights == showLights || uNLights < 0) return;
    float pos[LightsMax*4], amb[LightsMax*4], dif[LightsMax*4], spec[LightsMax*4], att[LightsMax*4], cone[LightsMax*4];
    const bool areaPix = ProgramWithArea(progUse->built);
    int n=0;
    for(int i=0;i<LightsMax;i++){
        const Light& L=lights[i];
        if(!L.on) continue;
        for(int k=0;k<4;k++){ pos[n*4+k]=L.pos[k]; amb[n*4+k]=L.amb[k]; dif[n*4+k]=L.dif[k]; spec[n*4+k]=L.spec[k]; }
        att[n*4]=L.att[0]; att[n*4+1]=L.att[1]; att[n*4+2]=L.att[2]; att[n*4+3]=L.expo;
        cone[n*4]=L.dir[0]; cone[n*4+1]=L.dir[1]; cone[n*4+2]=L.dir[2]; cone[n*4+3]=L.cutCos;
        // LA DE AREA en un programa con su cuenta (W3D_AREA): en los MISMOS arreglos (ningun uniform de mas: en un ES 2.0
        // de verdad no sobran). La atenuacion pasa a ser el medio ancho (xyz) y el medio alto (w; negativo = elipse), y el
        // corte del cono -2 dice que es de area. En los demas programas queda el foco que la reemplaza (Light.cpp)
        if(areaPix && L.area){
            const float hy=sqrtf(L.ay[0]*L.ay[0]+L.ay[1]*L.ay[1]+L.ay[2]*L.ay[2]);
            att[n*4]=L.ax[0]; att[n*4+1]=L.ax[1]; att[n*4+2]=L.ax[2]; att[n*4+3]=L.areaRound ? -hy : hy;
            cone[n*4+3]=-2.0f;
        }
        n++;
    }
    W3D_GLC(glUniform1i(uNLights,n));
    if(n>0){
        W3D_GLC(glUniform4fv(uLPos,n,pos)); W3D_GLC(glUniform4fv(uLAmb,n,amb)); W3D_GLC(glUniform4fv(uLDif,n,dif));
        W3D_GLC(glUniform4fv(uLSpec,n,spec)); W3D_GLC(glUniform4fv(uLAtt,n,att)); W3D_GLC(glUniform4fv(uLCone,n,cone));
    }
    progUse->showLights=showLights;
}
// ============================================================================
//  PASES ESPECIALES CON SHADER (ver SpecialPass en graphics/w3dGraphics.h). Lo que en GL ES 1.1 se pinta POR VERTICE desde
//  la CPU (la normal hecha color, el movimiento en la pantalla) aca lo calcula la GPU en cada pixel, y la vista Solido
//  usa un programa chico propio (la luz 0 por pixel, sin texturas). Son programas APARTE del entero/por draw: no tocan
//  sus claves ni sus caches. Mientras un pase esta prendido, cada draw usa el suyo (setupState -> setupSpecial).
// ============================================================================
static const char* VS_ESP =
"uniform mat4 uMVP; uniform mat4 uMV; uniform mat3 uNMat; uniform mat4 uVPAnt; uniform mat4 uVPAct;\n"
"attribute vec3 aPos; attribute vec3 aNrm; attribute vec4 aUV; attribute vec3 aPrev;\n"
"varying vec3 vN; varying vec2 vT; varying vec4 vAct; varying vec4 vAnt; varying float vZ;\n"
"void main(){\n"
"  gl_Position=uMVP*vec4(aPos,1.0);\n"
"#if defined(W3D_P_NORMAL) || defined(W3D_P_SOLID)\n"
"  vN=uNMat*aNrm; vT=aUV.xy;\n"
"#endif\n"
"#if defined(W3D_P_SOLID)\n"
"  vAct=uMV*vec4(aPos,1.0);\n"   // (la posicion en ojo: la luz puntual)
"#endif\n"
"#if defined(W3D_P_VEC) || defined(W3D_P_DATA)\n"
"  vAct=uVPAct*vec4(aPos,1.0); vAnt=uVPAnt*vec4(aPrev,1.0); vZ=-(uMV*vec4(aPos,1.0)).z;\n"
"#endif\n"
"#if defined(W3D_P_SHADOW)\n"   // (la proyeccion de la luz es ortografica: la profundidad sale derecho de la z)
"  vZ=gl_Position.z*0.5+0.5;\n"
"#endif\n"
"}\n";
static const char* FS_ESP =
"#ifdef GL_ES\n"
"precision mediump float;\n"
"#ifdef GL_FRAGMENT_PRECISION_HIGH\n#define W3D_HIGHP highp\n#else\n#define W3D_HIGHP mediump\n#endif\n"
"#else\n#define W3D_HIGHP\n#endif\n"
"uniform int uUseTex; uniform sampler2D uTex; uniform int uTwoFaces;\n"
"uniform vec4 uMDiff; uniform vec4 uMAmb; uniform vec4 uMSpec; uniform vec4 uMyME; uniform float uShine; uniform vec4 uAmbGlobal;\n"
"uniform vec4 uLPos; uniform vec4 uLAmb; uniform vec4 uLDif; uniform vec4 uLSpec;\n"
"uniform W3D_HIGHP vec4 uSize; uniform W3D_HIGHP float uFar;\n"
"varying vec3 vN; varying vec2 vT; varying W3D_HIGHP vec4 vAct; varying W3D_HIGHP vec4 vAnt; varying W3D_HIGHP float vZ;\n"
// el movimiento en PIXELES de la imagen (X a la derecha, Y hacia abajo) codificado como el pase de la CPU:
// 128 + d * 2 (render/MotionVectors.h), de 1 a 255
"float Cod(float d){ return clamp(128.0+d*2.0,1.0,255.0)/255.0; }\n"
"void main(){\n"
"#if defined(W3D_P_NORMAL)\n"
"  vec3 N=normalize(vN); float a=1.0;\n"
"  if(uUseTex==1) a=texture2D(uTex,vT).a;\n"   // (los transparentes: solo el alfa de su textura, como la CPU)
"  gl_FragColor=vec4(N*0.5+0.5,a);\n"
"#elif defined(W3D_P_SHADOW)\n"
// EL MAPA DE SOMBRAS: la profundidad vista desde la luz en 16 bits, en RG y en BA (R / B la parte alta): la mascara de
// color elige cual se escribe (RG: todo lo que proyecta; BA: lo que se mueve). Sin texturas de profundidad: anda igual en
// cualquier ES 2.0, en la PC y en Vulkan
"  W3D_HIGHP vec2 e=fract(clamp(vZ,0.0,0.99999)*vec2(1.0,255.0));\n"
"  e.x-=e.y*(1.0/255.0);\n"
"  gl_FragColor=vec4(e,e);\n"
"#elif defined(W3D_P_SOLID)\n"
"  vec3 N=normalize(vN); if(uTwoFaces==1 && !gl_FrontFacing) N=-N;\n"
"  vec3 L; if(uLPos.w!=0.0) L=normalize(uLPos.xyz/uLPos.w-vAct.xyz); else L=normalize(uLPos.xyz);\n"
"  float ndl=max(dot(N,L),0.0);\n"
"  vec3 c=uMyME.rgb+uMAmb.rgb*uAmbGlobal.rgb+uMAmb.rgb*uLAmb.rgb+uMDiff.rgb*uLDif.rgb*ndl;\n"
"  if(ndl>0.0){ float nh=max(dot(N,normalize(L+vec3(0.0,0.0,1.0))),0.0); c+=uMSpec.rgb*uLSpec.rgb*((uShine>0.0)?pow(nh,uShine):1.0); }\n"
"  gl_FragColor=vec4(clamp(c,0.0,1.0),uMDiff.a);\n"
"#else\n"
"  W3D_HIGHP vec2 a=vAct.xy/vAct.w, b=vAnt.xy/max(vAnt.w,1e-6);\n"
"  W3D_HIGHP vec2 d=(a-b)*0.5*uSize.xy; d.y=-d.y;\n"
"  if(vAnt.w<=1e-6) d=vec2(0.0);\n"   // (el anterior detras de la camara: quieto, como la CPU)
"#if defined(W3D_P_DATA)\n"
// la PROFUNDIDAD (distancia de la camara / uFar) en 24 bits: R la parte alta, G la del medio, B la baja. Sin el alfa:
// la pantalla y las copias de la pantalla pueden no tenerlo (el movimiento va en su propio pase, PassVectors)
"  W3D_HIGHP float z=clamp(vZ/uFar,0.0,1.0)*16777215.0;\n"
"  W3D_HIGHP float r=floor(z/65536.0); W3D_HIGHP float g=floor((z-r*65536.0)/256.0); W3D_HIGHP float b=z-r*65536.0-g*256.0;\n"
"  gl_FragColor=vec4(r/255.0,g/255.0,b/255.0,1.0); d.x+=0.0;\n"
"#else\n"
"  gl_FragColor=vec4(Cod(d.x),Cod(d.y),1.0,1.0);\n"
"#endif\n"
"#endif\n"
"}\n";
struct ProgEsp { GLuint id; bool built; GLint uMVP, uMV, uNMat, uVPAnt, uVPAct, uSize, uFar, uUseTex, uTex, uTwoFaces,
                 uMDiff, uMAmb, uMSpec, uMyME, uShine, uAmbGlobal, uLPos, uLAmb, uLDif, uLSpec; };
static ProgEsp progEsp[6];          // por modo (PassNormal .. PassData, 5 = el mapa de sombras)
// (passEsp: el pase prendido, 0 = ninguno; declarado arriba con las sombras, que prenden el 5)
static float passP[35];             // sus parametros (VP del anterior, tamanio de la imagen, el lejos, VP de ahora)
static const float* prevPtr = 0;
static ProgEsp* ProgOfPass(int mode){
    if(mode < 1 || mode > 5) return 0;
    ProgEsp& p = progEsp[mode];
    if(p.built) return p.id ? &p : 0;
    p.built = true;
    static const char* kDef[6] = { "", "#define W3D_P_NORMAL\n", "#define W3D_P_SOLID\n", "#define W3D_P_VEC\n", "#define W3D_P_DATA\n",
                                   "#define W3D_P_SHADOW\n" };
    GLuint vs = compile(GL_VERTEX_SHADER, kDef[mode], VS_ESP), fs = compile(GL_FRAGMENT_SHADER, kDef[mode], FS_ESP);
    GLuint id = W3D_GLC(glCreateProgram()); W3D_GLC(glAttachShader(id, vs)); W3D_GLC(glAttachShader(id, fs));
    W3D_GLC(glBindAttribLocation(id, aLpos, "aPos")); W3D_GLC(glBindAttribLocation(id, aLnrm, "aNrm"));
    W3D_GLC(glBindAttribLocation(id, aLuv, "aUV")); W3D_GLC(glBindAttribLocation(id, aLuv1, "aPrev"));
    W3D_GLC(glLinkProgram(id)); W3D_GLC(glDeleteShader(vs)); W3D_GLC(glDeleteShader(fs));
    if(!Linked(id)){
        char log[1024]; log[0] = 0; W3D_GLC(glGetProgramInfoLog(id, 1024, 0, log));
        w3dLogfE("[w3dGLES2] el pase especial %d no linkea: %s", mode, log);
        W3D_GLC(glDeleteProgram(id)); p.id = 0; return 0;
    }
    p.id = id;
#define W3D_UE(n) p.n = W3D_GLC(glGetUniformLocation(id, #n));
    W3D_UE(uMVP) W3D_UE(uMV) W3D_UE(uNMat) W3D_UE(uVPAnt) W3D_UE(uVPAct) W3D_UE(uSize) W3D_UE(uFar) W3D_UE(uUseTex) W3D_UE(uTex)
    W3D_UE(uTwoFaces) W3D_UE(uMDiff) W3D_UE(uMAmb) W3D_UE(uMSpec) W3D_UE(uMyME) W3D_UE(uShine) W3D_UE(uAmbGlobal)
    W3D_UE(uLPos) W3D_UE(uLAmb) W3D_UE(uLDif) W3D_UE(uLSpec)
#undef W3D_UE
    return &p;
}
static void ForgetProgramsEsp(){ for(int i=0;i<6;i++){ progEsp[i].id = 0; progEsp[i].built = false; } }
// el draw en un pase especial: su programa, sus uniforms (pocos: se suben siempre) y solo los atributos que lee.
// false = el pase no tiene programa en esta GPU (el draw sigue con el normal)
static bool setupSpecial(int nV){
    ProgEsp* p = ProgOfPass(passEsp);
    if(!p) return false;
    if(activeProg != p->id){ W3D_GLC(glUseProgram(p->id)); activeProg = p->id; AttrForget(); }
    Matrix4 mvp = curProj * curMV;
    float nmat[9]; MatrixNormal(nmat);
    if(p->uMVP >= 0) W3D_GLC(glUniformMatrix4fv(p->uMVP, 1, GL_FALSE, mvp.m));
    if(p->uMV >= 0) W3D_GLC(glUniformMatrix4fv(p->uMV, 1, GL_FALSE, curMV.m));
    if(p->uNMat >= 0) W3D_GLC(glUniformMatrix3fv(p->uNMat, 1, GL_FALSE, nmat));
    if(p->uVPAnt >= 0) W3D_GLC(glUniformMatrix4fv(p->uVPAnt, 1, GL_FALSE, passP));
    if(p->uVPAct >= 0) W3D_GLC(glUniformMatrix4fv(p->uVPAct, 1, GL_FALSE, passP + 19));
    if(p->uSize >= 0){ const float t4[4] = { passP[16], passP[17], 0, 0 }; W3D_GLC(glUniform4fv(p->uSize, 1, t4)); }
    if(p->uFar >= 0) W3D_GLC(glUniform1f(p->uFar, passP[18] > 0.0f ? passP[18] : 1.0f));
    const bool tex = cap_tex && passEsp == 1;
    if(p->uUseTex >= 0) W3D_GLC(glUniform1i(p->uUseTex, tex ? 1 : 0));
    if(p->uTex >= 0) W3D_GLC(glUniform1i(p->uTex, 0));
    if(passEsp == 2){   // SOLIDO: el material y la luz 0, como el pipeline fijo
        W3D_GLC(glUniform1i(p->uTwoFaces, twoFaces ? 1 : 0));
        W3D_GLC(glUniform4fv(p->uMDiff, 1, matDiff)); W3D_GLC(glUniform4fv(p->uMAmb, 1, matAmb));
        W3D_GLC(glUniform4fv(p->uMSpec, 1, matSpec)); W3D_GLC(glUniform4fv(p->uMyME, 1, matEmis));
        W3D_GLC(glUniform1f(p->uShine, matShine)); W3D_GLC(glUniform4fv(p->uAmbGlobal, 1, ambGlobal));
        const Light& L = lights[0];
        static const float zero[4] = { 0, 0, 0, 0 };
        W3D_GLC(glUniform4fv(p->uLPos, 1, L.pos)); W3D_GLC(glUniform4fv(p->uLAmb, 1, L.on ? L.amb : zero));
        W3D_GLC(glUniform4fv(p->uLDif, 1, L.on ? L.dif : zero)); W3D_GLC(glUniform4fv(p->uLSpec, 1, L.on ? L.spec : zero));
    }
    static const float defNrm[4] = { 0, 0, 1, 0 }, defUV[4] = { 0, 0, 0, 1 };
    static const Arr off = { false, 3, GL_FLOAT, 0, 0 };
    const bool nrm = (passEsp == 1 || passEsp == 2);
    bindAttr(aLpos, aPos, vboP, nV, defNrm, false);
    bindAttr(aLnrm, nrm ? aNrm : off, vboN, nV, defNrm, true);
    bindAttr(aLuv, tex ? aUV : off, vboT, nV, defUV, false);
    bindAttr(aLcol, off, vboC, nV, curColor, true);
    if(passEsp == 3 || passEsp == 4){   // las posiciones del cuadro ANTERIOR (sin anterior: las mismas, quieto)
        Arr ant = { true, 3, GL_FLOAT, 0, prevPtr ? (const void*)prevPtr : aPos.ptr, prevPtr ? 0u : aPos.vbo };
        bindAttr(aLuv1, ant, vboT1, nV, defUV, false);
    } else bindAttr(aLuv1, off, vboT1, nV, defUV, false);
    return true;
}

bool PassesWithShader(){ return true; }
void SpecialPass(int mode, const float* p){
    passEsp = (mode >= 1 && mode <= 4) ? mode : 0;
    if(p && passEsp >= 3) memcpy(passP, p, sizeof(float) * 35);
    if(!passEsp) prevPtr = 0;
}
void PrevPointer3f(const float* p){ prevPtr = p; }

// ============================================================================
//  SHADERS ESCRITOS (el "Shader personalizado" de los materiales; objects/WrittenShader.h). El usuario escribe solo
//  Fragment() (y Vertex() si quiere); aca va lo de este backend: el PROLOGO (las entradas w3d_*, la precision) y el
//  main. Es GLSL ES 1.00: lo compila el driver de GLES 2.0 / WebGL, el GL 2.1 de la PC (con #version 120) y la capa de
//  Vulkan (graphics/vulkan/w3dVkGL.cpp lo pasa a SPIR-V). Un backend futuro que no hable GLSL ES pondria SU prologo y su traduccion;
//  el archivo del usuario no cambia.
// ============================================================================
static const char* VS_ESC_PRE =
"uniform mat4 w3d_mvp; uniform mat4 w3d_mv; uniform mat3 w3d_nmat; uniform float w3d_time;\n"
"attribute vec3 aPos; attribute vec3 aNrm; attribute vec4 aUV; attribute vec4 aCol;\n"
"varying vec2 w3d_uv; varying vec3 w3d_normal; varying vec3 w3d_viewPos; varying vec4 w3d_vertexColor;\n";
static const char* VS_ESC_MAIN =
"\nvoid main(){\n"
"  vec3 p=aPos; vec3 n=aNrm;\n"
"#ifdef W3D_HAS_VERTEX\n"
"  Vertex(p,n);\n"
"#endif\n"
"  w3d_uv=aUV.xy; w3d_normal=w3d_nmat*n; w3d_viewPos=(w3d_mv*vec4(p,1.0)).xyz; w3d_vertexColor=aCol;\n"
"  gl_Position=w3d_mvp*vec4(p,1.0);\n"
"}\n";
static const char* FS_ESC_PRE =
"#ifdef GL_ES\n"
"precision mediump float;\n"
"#ifdef GL_FRAGMENT_PRECISION_HIGH\n#define W3D_HIGHP highp\n#else\n#define W3D_HIGHP mediump\n#endif\n"
"#else\n#define W3D_HIGHP\n#endif\n"
"uniform W3D_HIGHP float w3d_time;\n"   // (highp como en el vertex shader: un uniform de los dos tiene que coincidir)
"uniform vec4 w3d_color; uniform vec4 w3d_emission; uniform vec4 w3d_material;\n"
"uniform sampler2D w3d_texture; uniform float w3d_hasTexture;\n"
"uniform sampler2D w3d_screenTex; uniform W3D_HIGHP vec4 w3d_view; uniform vec4 w3d_screenUV;\n"
"uniform vec4 w3d_lightPos; uniform vec4 w3d_lightColor; uniform vec4 w3d_ambient; uniform mat3 w3d_viewToWorld;\n"
"varying vec2 w3d_uv; varying vec3 w3d_normal; varying vec3 w3d_viewPos; varying vec4 w3d_vertexColor;\n"
"#define w3d_ior w3d_material.x\n"
"#define w3d_transmission w3d_material.y\n"
"#define w3d_roughness w3d_material.z\n"
"#define w3d_metallic w3d_material.w\n"
"vec2 w3d_ScreenPoint(){ return (gl_FragCoord.xy-w3d_view.xy)/w3d_view.zw; }\n"
"vec2 w3d_Pixel(){ return 1.0/w3d_view.zw; }\n"
"vec4 w3d_Screen(vec2 p){ return texture2D(w3d_screenTex, w3d_screenUV.xy+clamp(p,0.0,1.0)*w3d_screenUV.zw); }\n"
"vec4 w3d_Texture(vec2 uv){ return w3d_hasTexture>0.5 ? texture2D(w3d_texture,uv) : vec4(1.0); }\n"
"vec3 w3d_Normal(){ vec3 n=normalize(w3d_normal); return gl_FrontFacing ? n : -n; }\n"
"vec3 w3d_ToCamera(){ return normalize(-w3d_viewPos); }\n"
"vec3 w3d_ToLight(){ return w3d_lightPos.w!=0.0 ? normalize(w3d_lightPos.xyz/w3d_lightPos.w-w3d_viewPos) : normalize(w3d_lightPos.xyz); }\n"
"vec3 w3d_Light(vec3 n){ return w3d_ambient.rgb+w3d_lightColor.rgb*max(dot(n,w3d_ToLight()),0.0); }\n"
"vec3 w3d_ToWorld(vec3 v){ return w3d_viewToWorld*v; }\n"
// FRESNEL (Schlick): cuanto refleja el punto segun el angulo. De frente, f0; hacia los bordes sube hasta 1.
// w3d_Fresnel() usa el IOR del material (f0 = ((ior-1)/(ior+1))^2); w3d_FresnelF0(f0), el que se le pase
"float w3d_FresnelF0(float f0){ return f0+(1.0-f0)*pow(1.0-max(dot(w3d_Normal(),w3d_ToCamera()),0.0),5.0); }\n"
"float w3d_Fresnel(){ float r=(w3d_ior-1.0)/(w3d_ior+1.0); return w3d_FresnelF0(r*r); }\n";
static const char* FS_ESC_MAIN = "\nvoid main(){ gl_FragColor=Fragment(); }\n";
struct WrittenProg {
    std::string vs, fs; bool withVertex, screen;
    GLuint id; bool built, live;
    GLint uMVP, uMV, uNMat, uTime, uColor, uEmission, uMaterial, uTex, uHasTex, uPant, uView, uPantUV, uLightPos, uLightColor, uAmb, uAWorld;
};
static std::vector<WrittenProg> written;   // (el id es el lugar + 1)
static int   writtenActive = 0;
static float writtenP[ShaderWrittenParams];
static GLuint texScreen = 0; static int texPantW = 0, texPantH = 0;
static bool unit1Screen = false;        // la unidad 1 quedo con la copia de la pantalla (no con la de la mezcla)
static void ForgetWrittenPrograms(){
    for(size_t i=0;i<written.size();i++){ written[i].id = 0; written[i].built = false; }
    texScreen = 0; texPantW = texPantH = 0; unit1Screen = false;
}
static GLuint CompileWritten(GLenum type, const char* defs, const std::string& pre, const std::string& user, const char* end, std::string& log){
    const std::string s = pre + user + end;
    GLuint sh = compile(type, defs, s.c_str());
    GLint ok = 0; W3D_GLC(glGetShaderiv(sh, GL_COMPILE_STATUS, &ok));
    if(!ok){ char l[2048]; l[0] = 0; W3D_GLC(glGetShaderInfoLog(sh, sizeof(l), 0, l)); log += (type == GL_VERTEX_SHADER) ? "@vertex: " : "@fragment: "; log += l; }
    return sh;
}
static bool BuildWritten(WrittenProg& p, std::string& log){
    p.built = true; p.id = 0;
    const char* defs = p.withVertex ? "#define W3D_HAS_VERTEX\n" : "";
    GLuint vs = CompileWritten(GL_VERTEX_SHADER, defs, VS_ESC_PRE, p.vs, VS_ESC_MAIN, log);
    GLuint fs = CompileWritten(GL_FRAGMENT_SHADER, "", FS_ESC_PRE, p.fs, FS_ESC_MAIN, log);
    GLuint id = W3D_GLC(glCreateProgram()); W3D_GLC(glAttachShader(id, vs)); W3D_GLC(glAttachShader(id, fs));
    W3D_GLC(glBindAttribLocation(id, aLpos, "aPos")); W3D_GLC(glBindAttribLocation(id, aLnrm, "aNrm"));
    W3D_GLC(glBindAttribLocation(id, aLuv, "aUV")); W3D_GLC(glBindAttribLocation(id, aLcol, "aCol"));
    W3D_GLC(glLinkProgram(id)); W3D_GLC(glDeleteShader(vs)); W3D_GLC(glDeleteShader(fs));
    if(!Linked(id)){
        char l[2048]; l[0] = 0; W3D_GLC(glGetProgramInfoLog(id, sizeof(l), 0, l));
        if(log.empty()) log = l;   // (si un shader ya dijo su error, el del link repite lo mismo)
        W3D_GLC(glDeleteProgram(id));
        return false;
    }
    p.id = id;
#define W3D_UE(c, n) p.c = W3D_GLC(glGetUniformLocation(id, n));
    W3D_UE(uMVP, "w3d_mvp") W3D_UE(uMV, "w3d_mv") W3D_UE(uNMat, "w3d_nmat") W3D_UE(uTime, "w3d_time")
    W3D_UE(uColor, "w3d_color") W3D_UE(uEmission, "w3d_emission") W3D_UE(uMaterial, "w3d_material")
    W3D_UE(uTex, "w3d_texture") W3D_UE(uHasTex, "w3d_hasTexture") W3D_UE(uPant, "w3d_screenTex") W3D_UE(uView, "w3d_view")
    W3D_UE(uPantUV, "w3d_screenUV") W3D_UE(uLightPos, "w3d_lightPos") W3D_UE(uLightColor, "w3d_lightColor") W3D_UE(uAmb, "w3d_ambient")
    W3D_UE(uAWorld, "w3d_viewToWorld")
#undef W3D_UE
    return true;
}
bool ShadersWritten(){ return ready; }
int WrittenShaderCompile(const char* vertex, const char* fragment, char* log, int logMax){
    if(log && logMax > 0) log[0] = 0;
    if(!ready || !fragment) return 0;
    WrittenProg p;
    p.vs = vertex ? vertex : ""; p.fs = fragment;
    p.withVertex = strstr(p.vs.c_str(), "Vertex") != 0;
    p.screen = strstr(p.fs.c_str(), "w3d_Screen") != 0;
    p.live = true;
    BatchEmpty();
    std::string l;
    const GLuint prev = activeProg;
    if(!BuildWritten(p, l)){
        if(log && logMax > 0){ strncpy(log, l.c_str(), (size_t)logMax - 1); log[logMax - 1] = 0; }
        return 0;
    }
    if(prev) { W3D_GLC(glUseProgram(prev)); } activeProg = prev;
    for(size_t i=0;i<written.size();i++) if(!written[i].live){ written[i] = p; return (int)i + 1; }
    written.push_back(p);
    return (int)written.size();
}
void WrittenShaderDelete(int id){
    if(id <= 0 || id > (int)written.size()) return;
    WrittenProg& p = written[(size_t)id - 1];
    if(p.id){ if(activeProg == p.id){ W3D_GLC(glUseProgram(0)); activeProg = 0; } W3D_GLC(glDeleteProgram(p.id)); }
    p.id = 0; p.live = false; p.vs.clear(); p.fs.clear();
    if(writtenActive == id) writtenActive = 0;
}
void WrittenShaderUse(int id, const float* p){
    if(id != writtenActive) BatchEmpty();
    writtenActive = (id > 0 && id <= (int)written.size() && written[(size_t)id - 1].live) ? id : 0;
    if(p) memcpy(writtenP, p, sizeof(writtenP));
    if(!writtenActive && unit1Screen){   // la unidad 1 vuelve a tener lo de la mezcla de texturas
        W3D_GLC(glActiveTexture(GL_TEXTURE1)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, mixOn ? mixTex : 0));
        W3D_GLC(glActiveTexture(GL_TEXTURE0));
        unit1Screen = false;
    }
}
// la COPIA DE LA PANTALLA (w3d_Screen): el viewport de ahora, a una textura del mismo tamanio
static void CopyWrittenScreen(){
    const int x = (int)vpCurrent[0], y = (int)vpCurrent[1], w = (int)vpCurrent[2], h = (int)vpCurrent[3];
    if(w <= 0 || h <= 0) return;
    W3D_GLC(glActiveTexture(GL_TEXTURE1));
    if(!texScreen){ W3D_GLC(glGenTextures(1, &texScreen)); texPantW = texPantH = 0; }
    W3D_GLC(glBindTexture(GL_TEXTURE_2D, texScreen));
    if(w != texPantW || h != texPantH){
        W3D_GLC(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, CopyTypeScreen(), 0));   // (el formato de la pantalla)
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        texPantW = w; texPantH = h;
    }
    W3D_GLC(glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, y, w, h));
    W3D_GLC(glActiveTexture(GL_TEXTURE0));
    unit1Screen = true;
}
// el draw con el shader escrito: su programa, sus uniforms y los atributos. false = no hay (el draw sigue normal)
static bool setupWritten(int nV){
    if(writtenActive <= 0 || writtenActive > (int)written.size()) return false;
    WrittenProg& p = written[(size_t)writtenActive - 1];
    if(!p.live) return false;
    if(!p.built){ std::string l; if(!BuildWritten(p, l)) w3dLogfE("[w3dGLES2] el shader escrito no compila: %s", l.c_str()); }
    if(!p.id) return false;
    if(p.screen) CopyWrittenScreen();
    if(activeProg != p.id){ W3D_GLC(glUseProgram(p.id)); activeProg = p.id; AttrForget(); }
    Matrix4 mvp = curProj * curMV;
    float nmat[9]; MatrixNormal(nmat);
    if(p.uMVP >= 0) W3D_GLC(glUniformMatrix4fv(p.uMVP, 1, GL_FALSE, mvp.m));
    if(p.uMV >= 0) W3D_GLC(glUniformMatrix4fv(p.uMV, 1, GL_FALSE, curMV.m));
    if(p.uNMat >= 0) W3D_GLC(glUniformMatrix3fv(p.uNMat, 1, GL_FALSE, nmat));
    if(p.uTime >= 0) W3D_GLC(glUniform1f(p.uTime, writtenP[0]));
    if(p.uColor >= 0) W3D_GLC(glUniform4fv(p.uColor, 1, matDiff));
    if(p.uEmission >= 0) W3D_GLC(glUniform4fv(p.uEmission, 1, matEmis));
    if(p.uMaterial >= 0) W3D_GLC(glUniform4fv(p.uMaterial, 1, writtenP + 1));
    if(p.uAWorld >= 0) W3D_GLC(glUniformMatrix3fv(p.uAWorld, 1, GL_FALSE, writtenP + 8));
    if(p.uTex >= 0) W3D_GLC(glUniform1i(p.uTex, 0));
    if(p.uHasTex >= 0) W3D_GLC(glUniform1f(p.uHasTex, (cap_tex && boundTex) ? 1.0f : 0.0f));
    if(p.uPant >= 0) W3D_GLC(glUniform1i(p.uPant, 1));
    if(p.uView >= 0) W3D_GLC(glUniform4fv(p.uView, 1, vpCurrent));
    if(p.uPantUV >= 0){ static const float uv[4] = { 0, 0, 1, 1 }; W3D_GLC(glUniform4fv(p.uPantUV, 1, uv)); }
    {   // la luz 0 (la que ilumina los previews y la primera de la escena) y el ambiente
        const Light& L = lights[0];
        static const float zero[4] = { 0, 0, 0, 0 };
        float amb[4] = { ambGlobal[0], ambGlobal[1], ambGlobal[2], 1 };
        if(L.on) for(int c=0;c<3;c++) amb[c] += L.amb[c];
        if(p.uLightPos >= 0) W3D_GLC(glUniform4fv(p.uLightPos, 1, L.pos));
        if(p.uLightColor >= 0) W3D_GLC(glUniform4fv(p.uLightColor, 1, L.on ? L.dif : zero));
        if(p.uAmb >= 0) W3D_GLC(glUniform4fv(p.uAmb, 1, amb));
    }
    static const float defNrm[4] = { 0, 0, 1, 0 }, defUV[4] = { 0, 0, 0, 1 };
    const float col[4] = { curColor[0], curColor[1], curColor[2], curColor[3] };
    bindAttr(aLpos, aPos, vboP, nV, defNrm, false);
    bindAttr(aLnrm, aNrm, vboN, nV, defNrm, true);
    bindAttr(aLuv, aUV, vboT, nV, defUV, false);
    bindAttr(aLcol, aCol, vboC, nV, col, true);
    static const Arr off = { false, 3, GL_FLOAT, 0, 0 };
    bindAttr(aLuv1, off, vboT1, nV, defUV, false);
    return true;
}

// ============================================================================
//  POSTPROCESADO (PostEffect): un quad que cubre el viewport, con el programa del efecto. La imagen y los datos
//  (movimiento en RG, profundidad en BA: el pase PassData) son copias de la pantalla (CopyScreen)
// ============================================================================
static const char* VS_POST =
"attribute vec2 aPos; attribute vec2 aUV; varying vec2 vT;\n"
"void main(){ gl_Position=vec4(aPos,0.0,1.0); vT=aUV; }\n";
static const char* FS_POST =
"#ifdef GL_ES\n"
"precision mediump float;\n"
"#ifdef GL_FRAGMENT_PRECISION_HIGH\n#define W3D_HIGHP highp\n#else\n#define W3D_HIGHP mediump\n#endif\n"
"#else\n#define W3D_HIGHP\n#endif\n"
"uniform sampler2D uImg; uniform sampler2D uDat; uniform W3D_HIGHP vec4 uP0; uniform W3D_HIGHP vec4 uP1;\n"
"uniform W3D_HIGHP vec4 uTexel; uniform W3D_HIGHP vec4 uUV;\n"   // (1/tamanio de la textura; el rectangulo de la imagen)
"varying W3D_HIGHP vec2 vT;\n"
"W3D_HIGHP float Prof(W3D_HIGHP vec2 t){ vec4 d=texture2D(uDat,t); return (d.r*255.0*65536.0+d.g*255.0*256.0+d.b*255.0)/16777215.0; }\n"
"W3D_HIGHP vec2 Inside(W3D_HIGHP vec2 t){ return clamp(t,min(uUV.xy,uUV.zw),max(uUV.xy,uUV.zw)); }\n"
"#if defined(W3D_POST_BLOOM_BLUR)\n"
// lo que BRILLA (uP0: umbral, rodilla, tope): recortado al tope, por encima del umbral con la rodilla suave (cuadratica)
"vec3 Brightness(vec3 s){ s=min(s,vec3(uP0.z)); float br=max(s.r,max(s.g,s.b)); float k=uP0.y;"
" float rq=clamp(br-uP0.x+k,0.0,2.0*k); rq=(k>0.0)?rq*rq/(4.0*k+0.0001):0.0;"
" return s*max(max(rq,br-uP0.x)/max(br,0.0001),0.0); }\n"
"#endif\n"
"void main(){\n"
"  vec4 c=texture2D(uImg,vT);\n"
"#if defined(W3D_POST_GRAIN)\n"
// uP0: intensidad, tamanio del grano (px), semilla, color (0/1). El ruido de cada grano (un hash del pixel)
"  W3D_HIGHP vec2 g=floor(gl_FragCoord.xy/max(uP0.y,1.0));\n"
"  W3D_HIGHP float s=uP0.z*0.618;\n"
"  W3D_HIGHP vec3 n=vec3(fract(sin(dot(g+s,vec2(12.9898,78.233)))*43758.5453));\n"
"  if(uP0.w>0.5){ n.g=fract(sin(dot(g+s,vec2(39.346,11.135)))*43758.5453); n.b=fract(sin(dot(g+s,vec2(73.156,52.235)))*43758.5453); }\n"
"  c.rgb=clamp(c.rgb+(n-0.5)*uP0.x,0.0,1.0);\n"
"#elif defined(W3D_POST_FOCUS)\n"
// uP0: foco (m), rango nitido (m), radio maximo (px), lejos (m: la escala de la profundidad). Disco de 16 muestras con
// el radio del circulo de confusion de cada pixel; las muestras mas cerca que el pixel no se mezclan si estan nitidas
"  W3D_HIGHP float z=Prof(vT)*uP0.w;\n"
"  W3D_HIGHP float r=clamp((abs(z-uP0.x)-uP0.y*0.5)/max(uP0.y,0.01),0.0,1.0)*uP0.z;\n"
"  if(r>0.5){\n"
"    vec4 acc=c; float n=1.0;\n"
"#define SAMPLE(x,y) { W3D_HIGHP vec2 t=Inside(vT+vec2(x,y)*r*uTexel.xy); W3D_HIGHP float zs=Prof(t)*uP0.w;"
"      W3D_HIGHP float rs=clamp((abs(zs-uP0.x)-uP0.y*0.5)/max(uP0.y,0.01),0.0,1.0)*uP0.z;"
"      float w=(zs<z-0.05 && rs<r*0.5)?0.0:1.0; acc+=texture2D(uImg,t)*w; n+=w; }\n"
"    SAMPLE(1.0,0.0) SAMPLE(-1.0,0.0) SAMPLE(0.0,1.0) SAMPLE(0.0,-1.0)\n"
"    SAMPLE(0.707,0.707) SAMPLE(-0.707,0.707) SAMPLE(0.707,-0.707) SAMPLE(-0.707,-0.707)\n"
"    SAMPLE(0.5,0.0) SAMPLE(-0.5,0.0) SAMPLE(0.0,0.5) SAMPLE(0.0,-0.5)\n"
"    SAMPLE(0.92,0.38) SAMPLE(-0.38,0.92) SAMPLE(-0.92,-0.38) SAMPLE(0.38,-0.92)\n"
"    c=acc/n;\n"
"  }\n"
"#elif defined(W3D_POST_BLOOM_BLUR)\n"
// el desenfoque de UN eje del bloom: uP0.w = rapido (7 muestras en vez de 13); uP1.xy = el paso entre muestras (px);
// uP1.z = 1 en la primera pasada: filtra el brillo de cada muestra
"  vec3 acc=vec3(0.0); float wt=0.0;\n"
"  for(int i=-6;i<=6;i++){\n"
"    if(uP0.w>0.5 && (i<-3 || i>3)) continue;\n"
"    float t=float(i); float w=exp(-t*t/(uP0.w>0.5?4.5:18.0));\n"
"    vec3 s=texture2D(uImg,Inside(vT+t*uP1.xy*uTexel.xy)).rgb;\n"
"    if(uP1.z>0.5) s=Brightness(s);\n"
"    acc+=s*w; wt+=w;\n"
"  }\n"
"  c.rgb=acc/wt;\n"
"#elif defined(W3D_POST_BLOOM_MIX)\n"
// la imagen + el brillo desenfocado (uDat) por la intensidad (uP0.x) y el tinte (uP1.rgb)
"  c.rgb=clamp(c.rgb+texture2D(uDat,vT).rgb*uP0.x*uP1.rgb,0.0,1.0);\n"
"#elif defined(W3D_POST_MOTION)\n"
// uP0: obturador (0..1), muestras. El vector del pixel (en pixeles, Y hacia abajo) escalado por el obturador; las
// lecturas a lo largo de el, centradas
"  vec4 d=texture2D(uDat,vT);\n"
"  W3D_HIGHP vec2 v=vec2(d.r*255.0-128.0,-(d.g*255.0-128.0))*0.5*uP0.x*uTexel.xy;\n"
"  if(abs(v.x)+abs(v.y)>uTexel.x*0.5){\n"
"    vec4 acc=vec4(0.0);\n"
"    for(int i=0;i<12;i++){ W3D_HIGHP float t=(float(i)+0.5)/12.0-0.5; acc+=texture2D(uImg,Inside(vT+v*t)); }\n"
"    c=acc/12.0;\n"
"  }\n"
"#endif\n"
"  gl_FragColor=vec4(c.rgb,1.0);\n"
"}\n";
struct ProgPost { GLuint id; bool built; GLint uImg, uDat, uP0, uP1, uTexel, uUV; };
static ProgPost progPost[6];   // 0 copiar, 1 grano, 2 foco, 3 movimiento, 4 el desenfoque del bloom, 5 su mezcla
static void ForgetProgramsPost(){ for(int i=0;i<6;i++){ progPost[i].id = 0; progPost[i].built = false; } }
static ProgPost* ProgOfPost(int e){
    if(e < 0 || e > 5) return 0;
    ProgPost& p = progPost[e];
    if(p.built) return p.id ? &p : 0;
    p.built = true;
    static const char* kDef[6] = { "", "#define W3D_POST_GRAIN\n", "#define W3D_POST_FOCUS\n", "#define W3D_POST_MOTION\n",
                                   "#define W3D_POST_BLOOM_BLUR\n", "#define W3D_POST_BLOOM_MIX\n" };
    GLuint vs = compile(GL_VERTEX_SHADER, kDef[e], VS_POST), fs = compile(GL_FRAGMENT_SHADER, kDef[e], FS_POST);
    GLuint id = W3D_GLC(glCreateProgram()); W3D_GLC(glAttachShader(id, vs)); W3D_GLC(glAttachShader(id, fs));
    W3D_GLC(glBindAttribLocation(id, aLpos, "aPos")); W3D_GLC(glBindAttribLocation(id, aLuv, "aUV"));
    W3D_GLC(glLinkProgram(id)); W3D_GLC(glDeleteShader(vs)); W3D_GLC(glDeleteShader(fs));
    if(!Linked(id)){
        char log[1024]; log[0] = 0; W3D_GLC(glGetProgramInfoLog(id, 1024, 0, log));
        w3dLogfE("[w3dGLES2] el postprocesado %d no linkea: %s", e, log);
        W3D_GLC(glDeleteProgram(id)); p.id = 0; return 0;
    }
    p.id = id;
    p.uImg = W3D_GLC(glGetUniformLocation(id, "uImg")); p.uDat = W3D_GLC(glGetUniformLocation(id, "uDat"));
    p.uP0 = W3D_GLC(glGetUniformLocation(id, "uP0")); p.uP1 = W3D_GLC(glGetUniformLocation(id, "uP1"));
    p.uTexel = W3D_GLC(glGetUniformLocation(id, "uTexel")); p.uUV = W3D_GLC(glGetUniformLocation(id, "uUV"));
    return &p;
}
bool PostEffect(int effect, unsigned int texImg, unsigned int texDat, int texW, int texH, const float* uv4, const float* p8){
    if(!ready || !texImg || texW <= 0 || texH <= 0 || !uv4) return false;
    ProgPost* p = ProgOfPost(effect);
    if(!p) return false;
    BatchEmpty();
    if(activeProg != p->id){ W3D_GLC(glUseProgram(p->id)); activeProg = p->id; }
    static const float zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const float* q = p8 ? p8 : zero;
    if(p->uImg >= 0) W3D_GLC(glUniform1i(p->uImg, 0));
    if(p->uDat >= 0) W3D_GLC(glUniform1i(p->uDat, 1));
    if(p->uP0 >= 0) W3D_GLC(glUniform4fv(p->uP0, 1, q));
    if(p->uP1 >= 0) W3D_GLC(glUniform4fv(p->uP1, 1, q + 4));
    if(p->uTexel >= 0){ const float t4[4] = { 1.0f / (float)texW, 1.0f / (float)texH, 0, 0 }; W3D_GLC(glUniform4fv(p->uTexel, 1, t4)); }
    if(p->uUV >= 0) W3D_GLC(glUniform4fv(p->uUV, 1, uv4));
    W3D_GLC(glActiveTexture(GL_TEXTURE1)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, texDat ? texDat : texImg));
    if(texDat){   // (los datos van CODIFICADOS en bytes: interpolarlos da basura en los bordes)
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    }
    W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, texImg));
    // (pixel a pixel: con el filtro lineal, un driver que muestrea medio texel corrido suavizaba las lineas finas)
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    W3D_GLC(glDisable(GL_DEPTH_TEST)); W3D_GLC(glDisable(GL_BLEND)); W3D_GLC(glDisable(GL_CULL_FACE));
    W3D_GLC(glDepthMask(GL_FALSE));
    // el quad: (x, y, u, v) x 4, en un solo arreglo
    const float q4[16] = { -1, -1, uv4[0], uv4[1],   1, -1, uv4[2], uv4[1],   -1, 1, uv4[0], uv4[3],   1, 1, uv4[2], uv4[3] };
    W3D_GLC(glBindBuffer(GL_ARRAY_BUFFER, vboP)); W3D_GLC(glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(q4), q4, GL_DYNAMIC_DRAW));
    for(int i=0;i<5;i++) if(i != aLpos && i != aLuv) W3D_GLC(glDisableVertexAttribArray(i));
    W3D_GLC(glEnableVertexAttribArray(aLpos)); W3D_GLC(glVertexAttribPointer(aLpos, 2, GL_FLOAT, GL_FALSE, 16, (const void*)0));
    W3D_GLC(glEnableVertexAttribArray(aLuv)); W3D_GLC(glVertexAttribPointer(aLuv, 2, GL_FLOAT, GL_FALSE, 16, (const void*)8));
    W3D_GLC(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    g_statDrawTris++; g_statDcCat[gStatCat]++;
    // el estado de antes (los atributos se vuelven a dar en el proximo draw)
    AttrForget();
    W3D_GLC(glActiveTexture(GL_TEXTURE1)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, mixOn ? mixTex : 0));
    W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, boundTex));
    if(cap_depth) W3D_GLC(glEnable(GL_DEPTH_TEST));
    if(cap_blend) W3D_GLC(glEnable(GL_BLEND));
    if(cap_cull) W3D_GLC(glEnable(GL_CULL_FACE));
    W3D_GLC(glDepthMask(GL_TRUE));
    return true;
}

static int gSetupNV = 0; static bool gTestDraw = false;   // (ver ProgramNotEntered)
static void setupState(int nV){
    BatchEmpty();
    gSetupNV = nV; gTestDraw = false;
    gLastVerts = nV;   // (lo juntado sale ANTES de elegir nada para este draw: vaciarlo a mitad de camino pisaria el programa)
    if(passEsp && primDraw == 2 && setupSpecial(nV)) return;   // (un pase especial: los triangulos con su programa)
    if(writtenActive && primDraw == 2 && setupWritten(nV)) return;   // (el shader escrito de un material)
    Matrix4 mvp = curProj * curMV;
    float nmat[9]; MatrixNormal(nmat);
    PickProgram(nV);
    if(activeProg != prog){ W3D_GLC(glUseProgram(prog)); activeProg = prog; }
    if(noFill) OnlyUnPixel();
    if(uClipN>=0){
        U_1I(uClipN, clipN);
        for(int i=0;i<clipN;i++) if(uClip[i]>=0 && UNew(progUse->c_uClip[i], clipEq[i], 16)) W3D_GLC(glUniform4fv(uClip[i],1,clipEq[i]));
    }
    U_M4(uMVP, mvp.m);
    U_M4(uMV, curMV.m);
    U_M3(uNMat, nmat);
    U_M4(uTexM, curTex.m);
    U_1I(uTexGen, texGenSphere?1:0);
    U_1I(uUseTex, cap_tex?1:0);
    U_1I(uEnv, texEnv);
    // la luz no hace falta si la textura REEMPLAZA el color (el resultado es el mismo y ahorra el vertex shader)
    U_1I(uLightOn, (cap_light && !(cap_tex && texEnv==1))?1:0);
    U_1I(uColorMat, cap_colormat?1:0);
    U_1F(uBlurY, blurY);
    U_1F(uAlphaRef, alphaRef);
    U_1I(uAlphaDown, alphaDown);
    U_1I(uFogOn, cap_fog?1:0);
    U_1I(uTex, 0);
    U_1F(uPointSize, pointSize);
    U_1I(uPointSprite, pointSprite?1:0);
    U_4F(uViewport, vpCurrent);
    U_4F(uMDiff, matDiff); U_4F(uMAmb, matAmb);
    U_4F(uMSpec, matSpec); U_4F(uMyME, matEmis); U_1F(uShine, matShine);
    U_4F(uMPbr, matPbr); U_4F(uMExtra, matExtra);
    // LA SONDA DE LUZ: la luz por pixel (uSH*) o la de cada vertice (uSH*V, los materiales comunes)
    {
        const bool sh = shOn && lightPixel;
        U_1F(uSHOn, sh ? 1.0f : 0.0f);
        if(sh){
            if(uSHa>=0 && UNew(progUse->c_uSHa, &shU[0], 48))  W3D_GLC(glUniform4fv(uSHa, 3, &shU[0]));
            if(uSHb>=0 && UNew(progUse->c_uSHb, &shU[12], 48)) W3D_GLC(glUniform4fv(uSHb, 3, &shU[12]));
            if(uSHc>=0 && UNew(progUse->c_uSHc, &shU[24], 48)) W3D_GLC(glUniform4fv(uSHc, 3, &shU[24]));
            U_M3(uEyeWorld, eyeWorld);
        }
        const bool shv = shOn && !lightPixel;
        U_1F(uSHOnV, shv ? 1.0f : 0.0f);
        if(shv){
            if(uSHaV>=0 && UNew(progUse->c_uSHaV, &shU[0], 48))  W3D_GLC(glUniform4fv(uSHaV, 3, &shU[0]));
            if(uSHbV>=0 && UNew(progUse->c_uSHbV, &shU[12], 48)) W3D_GLC(glUniform4fv(uSHbV, 3, &shU[12]));
            if(uSHcV>=0 && UNew(progUse->c_uSHcV, &shU[24], 48)) W3D_GLC(glUniform4fv(uSHcV, 3, &shU[24]));
            U_M3(uEyeWorldV, eyeWorld);
        }
    }
    // EL ENTORNO 360 en las unidades 2 y 3 (apagado, los samplers miran la 0: sin textura en una unidad el navegador avisa)
    {
        const bool env = envTex != 0 && envF[3] > 0.5f && lightPixel;
        U_1I(uEnvTex, env ? 2 : 0); U_1I(uEnvTexB, env ? 3 : 0);
        static const float off[4] = {1,0,0,0};
        U_4F(uEnvF0, env ? &envF[0] : off); U_4F(uEnvF1, &envF[4]); U_4F(uEnvF2, &envF[8]);
        // lo de atras del vidrio (unidad 4)
        const bool esc = escVp[2] > 0.0f && lightPixel && texScene;
        U_1I(uScene, esc ? 4 : 0);
        static const float noEsc[4] = {0,0,0,0};
        U_4F(uSceneVp, esc ? escVp : noEsc);
        if(esc && uScene >= 0){
            W3D_GLC(glActiveTexture(GL_TEXTURE4)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,texScene));
            W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex));
        }
        // LOS MAPAS DEL MATERIAL en las unidades 6..10 (sin textura: el sampler mira la 0 y el interruptor queda en 0)
        if(uMapOn >= 0){
            const float on[4] = { mapTex[0] ? 1.0f : 0.0f, mapTex[1] ? 1.0f : 0.0f, mapTex[2] ? 1.0f : 0.0f, mapTex[3] ? 1.0f : 0.0f };
            U_4F(uMapOn, on); U_1F(uMapEOn, mapTex[4] ? 1.0f : 0.0f);
            U_1I(uMapN, mapTex[0] ? 6 : 0); U_1I(uMapR, mapTex[1] ? 7 : 0); U_1I(uMapM, mapTex[2] ? 8 : 0);
            U_1I(uMapA, mapTex[3] ? 9 : 0); U_1I(uMapE, mapTex[4] ? 10 : 0);
            bool any = false;
            for(int k=0;k<5;k++) if(mapTex[k]){ W3D_GLC(glActiveTexture(GL_TEXTURE0 + 6 + k)); W3D_GLC(glBindTexture(GL_TEXTURE_2D, mapTex[k])); any = true; }
            if(any){ W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex)); }
        }
        // su profundidad (unidad 5) y la proyeccion: el reflejo recorre el rayo en OJO y lo proyecta a la copia
        const bool escZOk = esc && escZ && texEscZ;
        U_1I(uSceneZ, escZOk ? 5 : 0); U_1F(uSceneZOn, escZOk ? (escZTurn ? 2.0f : 1.0f) : 0.0f);   // (2: dada vuelta)
        if(escZOk){
            U_M4(uProj, curProj.m);
            if(uSceneZ >= 0){
                W3D_GLC(glActiveTexture(GL_TEXTURE0 + 5)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,texEscZ));
                W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex));
            }
        }
        if(env && (uEnvTex >= 0 || uEnvTexB >= 0)){
            const GLenum destination = envCube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;   // (el cubo de las GPU chicas)
            W3D_GLC(glActiveTexture(GL_TEXTURE2)); W3D_GLC(glBindTexture(destination,envTex));
            W3D_GLC(glActiveTexture(GL_TEXTURE3)); W3D_GLC(glBindTexture(destination,envTexB));
            W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex));
        }
    }
    U_4F(uAmbGlobal, ambGlobal);
    UploadLights();
    if(progUse->key & PShadow) UploadShadows();
    U_4F(uFogColor, fogColor); U_1F(uFogStart, fogStart); U_1F(uFogEnd, fogEnd);
    U_1I(uFogMode, fogMode); U_1F(uFogDens, fogDens);
    // la unidad 1: la textura B en el sampler 1. Apagada, el sampler apunta a la unidad 0 (sin textura en la 1 el
    // navegador avisa en cada draw)
    const bool mix = mixOn && mixTex;
    U_1I(uMix, mix ? texMixMode : -1);
    U_1I(uTex1, mix ? 1 : 0);
    if(mix){
        U_1F(uMixK, mixK);
        W3D_GLC(glActiveTexture(GL_TEXTURE1)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,mixTex));
        W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex));
    }
    // (sin la mezcla la unidad activa es siempre la 0 y su textura la deja BindTexture: no hace falta repetirla)
    static const float defNrm[4]={0,0,1,0};
    static const float defUV[4]={0,0,0,1};   // la texcoord de fabrica de GL
    // (con un programa por draw, los atributos que el programa no lee van apagados: si no, cambian la traduccion del
    // driver sin hacer falta, ver PState)
    static const Arr off={false,3,GL_FLOAT,0,0};
    const bool leeNrm = !byDraw || (progUse->key & (PLight|PTexGen));
    const bool leeUV  = !byDraw || (progUse->key & PTex);
    bindAttr(aLpos,aPos,vboP,nV, defNrm,  false); // POSICION: NUNCA normalizar (short = coords enteras de pantalla/mundo)
    bindAttr(aLnrm,leeNrm ? aNrm : off,vboN,nV, defNrm,  true);  // normal: byte -> [-1,1]
    bindAttr(aLuv, leeUV ? aUV : off, vboT,nV, defUV,   false); // uv: float, o las normales en bytes del matcap (GL no las normaliza)
    bindAttr(aLcol,aCol,vboC,nV, curColor,true);  // color: ubyte -> [0,1]. sin array -> color uniforme (Color4*)
    if(aLuv1>=0){
        if(mix && (mixUV || mixUVVbo)){   // UV propias de la unidad 1
            Arr uv1={true,2,GL_FLOAT,0,mixUV,mixUVVbo};
            bindAttr(aLuv1,uv1,vboT1,nV,defUV,false);
        } else if(mix && aUV.on && aUV.type==GL_FLOAT && (aUV.vbo || (aUV.ptr && nV>0))){   // las mismas de la unidad 0 (ya subidas)
            Arr uv0={true,2,GL_FLOAT,aUV.stride,aUV.ptr,aUV.vbo};
            if(noVBO || aUV.vbo) bindAttr(aLuv1,uv0,0,nV,defUV,false);
            else {
                BindArray(vboT);
                if(attrGL[aLuv1].on != 1){ W3D_GLC(glEnableVertexAttribArray(aLuv1)); attrGL[aLuv1].on = 1; }
                W3D_GLC(glVertexAttribPointer(aLuv1,2,GL_FLOAT,GL_FALSE,aUV.stride,(const void*)0));
            }
        } else {
            Arr nothing={false,2,GL_FLOAT,0,0};
            bindAttr(aLuv1,nothing,0,0,defUV,false);
        }
    }
    // el PRIMER draw de un programa armado por draw se mira (ProgramNotEntered): sin errores de antes que lo confundan
    if(byDraw && progUse && progUse->test){
        gTestDraw = true;
        for(int i=0;i<8 && glGetError()!=GL_NO_ERROR;i++){}
    }
}
// UN PROGRAMA QUE NO ENTRA EN LA GPU: el driver de la VideoCore IV (Android 2.3) linkea un shader que no le entra y
// recien falla al dibujar (GL_OUT_OF_MEMORY: la malla no aparecia; con el error pendiente, ni el cuadro). El primer
// draw de cada programa se mira y, si fallo, el programa se vuelve a armar con MENOS: sin la copia de la pantalla (el
// vidrio y los reflejos en pantalla), sin el entorno 360, sin el PBR y por ultimo con la luz por vertice. Se ve con
// menos detalle, pero se ve. El cache lo sigue encontrando con la clave pedida: se prueba una sola vez
static bool ShrinkProgram(Program& p){
    const Key requested = p.key;
    Key c = p.built;
    const Key e = c >> PState;
    const char* sin;
    // (primero el 360: en una esfera de vidrio importa mas lo de atras doblado por el IOR que el reflejo del cielo)
    if((e >> 39) & 1)      { c &= ~(((Key)1 << (39 + PState)) | ((Key)1 << (41 + PState))); sin = "el entorno 360"; }
    else if((e >> 40) & 1) { c &= ~((Key)1 << (40 + PState)); sin = "la copia de la pantalla (vidrio / reflejos)"; }
    else if((e >> 38) & 1) { c &= ~((Key)1 << (38 + PState)); sin = "el PBR"; }
    else if((e >> 42) & 1) { c &= ~((Key)1 << (42 + PState)); sin = "la sonda de luz por vertice"; }
    else if(c & PLightPixel) { c &= ~(Key)PLightPixel;             sin = "la luz por pixel"; }
    else return false;
    DeleteProgram(p);
    BuildProgram(p, c);
    p.key = requested;
    w3dDrawsLost++;   // (con el draw fallido se fue lo que ya estaba dibujado: el render a archivo repite la franja)
    if(progUse == &p) progUse = 0;   // (el proximo setupState lo vuelve a poner en uso: su id y sus uniforms)
    w3dLogfW("[w3dGLES2] un programa no entraba en esta GPU al dibujar: se arma sin %s (clave %08x%08x)", sin,
             (unsigned)(requested >> 32), (unsigned)requested);
    return true;
}
// despues del draw: true = no entro y se achico (quien dibujo vuelve a llamar a setupState y repite el draw)
static bool ProgramNotEntered(){
    if(!gTestDraw) return false;
    gTestDraw  = false;
    Program* p = progUse;
    if(!p) return false;
    p->test = 0;
    const GLenum err = glGetError();
    if(err != GL_OUT_OF_MEMORY && err != GL_INVALID_OPERATION) return false;
    return ShrinkProgram(*p);
}
// un draw con la prueba de arriba: si el programa no entro, se achica y se repite
#define W3D_DRAW_TESTED(call) do{ W3D_GLC(call); while(ProgramNotEntered()){ setupState(gSetupNV); W3D_GLC(call); } }while(0)
// dibujo INDEXADO (indices ushort; convierte desde MeshIndex/ubyte)
static void drawIndexed(GLenum mode,int count,const unsigned short* idx16){
    gLastIdx = count; gLastVBO = 0;
    const GLint b = noVBO ? 0 : (GLint)ibo;
    if(bufIndicesGL != b){ W3D_GLC(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,(GLuint)b)); bufIndicesGL = b; }
    if(!noVBO) W3D_GLC(glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(count*2),idx16,GL_DYNAMIC_DRAW));
    g_statDrawTris++; g_statIndices += count; g_statDcCat[gStatCat]++;   // (un draw: lineas y puntos tambien)
    W3D_DRAW_TESTED(glDrawElements(mode,count,GL_UNSIGNED_SHORT,noVBO ? (const void*)idx16 : (const void*)0));
}
static int maxIndex(const MeshIndex* ind,int count){ int n=0; for(int i=0;i<count;i++) if((int)ind[i]+1>n) n=(int)ind[i]+1; return n; }
static int maxIndexU16(const unsigned short* ind,int count){ int n=0; for(int i=0;i<count;i++) if((int)ind[i]+1>n) n=(int)ind[i]+1; return n; }

void DrawTriangles(int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    MeasureDraw measure;
    Prim(GL_TRIANGLES); setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_TRIANGLES,count,&idx[0]);
}
void DrawElements(Primitive p,int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    MeasureDraw measure;
    static const GLenum kMode[7]={GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_LINE_LOOP,GL_TRIANGLES,GL_TRIANGLE_STRIP,GL_TRIANGLE_FAN};
    Prim(kMode[(int)p>=0&&(int)p<7?(int)p:4]); setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(kMode[(int)p>=0&&(int)p<7?(int)p:4],count,&idx[0]);
}
void DrawElements16(Primitive p,int count,const unsigned short* indices){
    if(!ready || count<=0 || !indices) return;
    MeasureDraw measure;
    static const GLenum kMode[7]={GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_LINE_LOOP,GL_TRIANGLES,GL_TRIANGLE_STRIP,GL_TRIANGLE_FAN};
    int nV=0; for(int i=0;i<count;i++) if((int)indices[i]+1>nV) nV=(int)indices[i]+1;
    Prim(kMode[(int)p>=0&&(int)p<7?(int)p:4]); setupState(nV);
    drawIndexed(kMode[(int)p>=0&&(int)p<7?(int)p:4],count,indices);
}
static void DrawFromIBO(GLenum mode, int count, int first);
void DrawElementsVBO(Primitive p,int count,int firstIndex){
    static const GLenum kMode[7]={GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_LINE_LOOP,GL_TRIANGLES,GL_TRIANGLE_STRIP,GL_TRIANGLE_FAN};
    DrawFromIBO(kMode[(int)p>=0&&(int)p<7?(int)p:4],count,firstIndex);
}
void DrawTrianglesByte(int count,const unsigned char* indices){
    if(!ready || count<=0 || !indices) return;
    MeasureDraw measure;
    int nV=0; for(int i=0;i<count;i++) if((int)indices[i]+1>nV) nV=(int)indices[i]+1;   // bytes: sin overload
    Prim(GL_TRIANGLES); setupState(nV);
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_TRIANGLES,count,&idx[0]);
}
void DrawLinesIndexed(int count,const unsigned short* indices){
    if(!ready || count<=0 || !indices) return;
    Prim(GL_LINES); setupState(maxIndexU16(indices,count)); drawIndexed(GL_LINES,count,indices);
}
void DrawLineStripIndexed(int count,const unsigned short* indices){
    if(!ready || count<=0 || !indices) return;
    Prim(GL_LINE_STRIP); setupState(maxIndexU16(indices,count)); drawIndexed(GL_LINE_STRIP,count,indices);
}

// ---------------------------------------------------------------------------
//  INDICES DE CLIENTE sobre los atributos bindeados (ver w3dGraphics.h): dibujar SOLO
//  la lista de triangulos visible (PVS) sin re-subir la geometria. Los atributos son
//  los que esten puestos (en un VBO de la malla o de RAM): el mismo camino que
//  DrawTriangles/DrawLinesIndexed. Faltaban: sin ellas el APK de un juego 3D NO
//  LINKEABA (el pase 2D nunca las usaba, por eso no se habia notado).
// ---------------------------------------------------------------------------
void DrawTrianglesClientIdx(int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    MeasureDraw measure;
    Prim(GL_TRIANGLES); setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_TRIANGLES,count,&idx[0]);
}
void DrawLinesClientIdx(int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    Prim(GL_LINES); setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_LINES,count,&idx[0]);
}

// ---------------------------------------------------------------------------
//  ESTADISTICAS: el total de llamadas por categoria (W3D_GLC) y los draws (drawIndexed / glDrawArrays). Los binds y
//  los cambios de estado no se separan: sin cache, cada draw los vuelve a mandar (estan en el total).
// ---------------------------------------------------------------------------
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
int g_statMeshes = 0;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
int g_statDrawTris = 0, g_statDrawVBO = 0, g_statIndices = 0, g_statTexBinds = 0, g_statStateChanges = 0, g_otherStat = 0;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
int g_statUploadBytes = 0, g_opaqueStatTris = 0, g_statTrisBlend = 0;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
int g_statDcCat[StatCatCount_] = { 0 };
#endif
void StatCategory(StatCat c) { gStatCat = (int)c; }
StatCat StatCurrentCategory() { return (StatCat)gStatCat; }
void StatRaw(int n) { g_statCalls[gStatCat] += n; }
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
FrameStats g_statFrame;
#endif
static int gFrameDraws0 = 0, gFrameIdx0 = 0;
// EL TIEMPO DE LOS DRAWS (g_measureDraws): cuanto tarda la CPU adentro de cada uno (elegir el programa, los uniforms, los
// atributos, la llamada de GL: con arreglos del cliente, el driver copia los vertices ahi) y el mas lento del cuadro
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool g_measureDraws = false;
#endif
static double gMsDraws = 0, gMsDrawMax = 0;
static int gDrawMaxIdx = 0, gDrawMaxVerts = 0, gDrawMaxVBO = 0;
static double ClockMs() {
#if defined(_WIN32) || defined(W3D_SYMBIAN)   // (Open C no tiene clock_gettime; es solo para medir)
    return 0.0;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec * 1e-6;
#endif
}
MeasureDraw::MeasureDraw() : t0(g_measureDraws ? ClockMs() : 0.0) {}
MeasureDraw::~MeasureDraw() {
    if (!g_measureDraws || t0 == 0.0) return;
    const double d = ClockMs() - t0;
    gMsDraws += d;
    if (d > gMsDrawMax) { gMsDrawMax = d; gDrawMaxIdx = gLastIdx; gDrawMaxVerts = gLastVerts; gDrawMaxVBO = gLastVBO; }
}
void StatsFrame() {
    FrameStats& c = g_statFrame;
    c.msDraws = (float)gMsDraws; c.msDrawMax = (float)gMsDrawMax;
    c.drawMaxIdx = gDrawMaxIdx; c.drawMaxVerts = gDrawMaxVerts; c.drawMaxVBO = gDrawMaxVBO;
    gMsDraws = gMsDrawMax = 0; gDrawMaxIdx = gDrawMaxVerts = gDrawMaxVBO = 0;
    c.total = 0;
    for (int i = 0; i < StatCatCount_; i++) { c.calls[i] = g_statCalls[i]; c.total += g_statCalls[i]; g_statCalls[i] = 0; }
    c.draws = g_statDrawTris - gFrameDraws0; gFrameDraws0 = g_statDrawTris;
    c.binds = 0; c.states = 0;
    c.tris = (g_statIndices - gFrameIdx0) / 3; gFrameIdx0 = g_statIndices;
    c.frames++;
    gStatCat = StatCatEditor;
}
void StatsReset() {
    g_statDrawTris = 0; g_statDrawVBO = 0; g_statIndices = 0; g_statTexBinds = 0; g_statMeshes = 0;
    g_statStateChanges = 0; g_statUploadBytes = 0; g_opaqueStatTris = 0; g_statTrisBlend = 0; g_otherStat = 0;
    for (int i = 0; i < StatCatCount_; i++) { g_statDcCat[i] = 0; g_statCalls[i] = 0; }
    gFrameDraws0 = gFrameIdx0 = 0;
    gStatCat = StatCatScene;
}
void Finish() { W3D_GLC(glFinish()); }
void Flush() { W3D_GLC(glFlush()); }
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool g_auditScene = false;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
int  g_auditDesyncs  = 0;
#endif
int  AuditState() { return 0; }      // en dispositivo no se audita (glGet = flush del tiler)
// MATRICES: en este backend las tres pilas ya viven en la CPU (curProj/curMV/curTex), asi que GetMatrix es una copia
// directa, sin glGet y sin una sombra aparte que pueda desincronizarse: la auditoria no tiene nada que comparar.
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool g_matrixAudit = false;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
int  g_matrixDesyncs = 0;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool g_invalidatePerMesh = false;      // el Invalidate por malla es del editor de Symbian (GLES 1.1); aca no aplica
#endif
void GetMatrix(Matrix m, float* out16) {
    const Matrix4& M = (m == Projection) ? curProj : (m == TextureMatrix ? curTex : curMV);
    for (int i = 0; i < 16; i++) out16[i] = M.m[i];
}
void TexBaseLevel(int level) { (void)level; }   // ES2 no tiene GL_TEXTURE_BASE_LEVEL (PC solamente)
// BUFFER OBJECTS: la malla sube UNA vez a la GPU (ArrayBufferData / IndexBufferData) y cada draw solo da los punteros
// (VertexVBO y compania, desde el byte 0) en vez de mandar todos los vertices desde la RAM. Los indices de la malla son
// de 32 bits (MeshIndex) y GL ES 2 sin OES_element_index_uint solo dibuja de 16: se convierten al subirlos
struct BufInfo { long bytes; int verts; bool u32; };   // verts = el indice mas alto + 1 (los IBO)
static std::map<GLuint, BufInfo> buffers;
static GLuint iboVBO = 0;   // el IBO de BindIndexVBO
static int indicesU32 = -1; // el driver dibuja indices de 32 bits? (-1 = no se pregunto todavia)
bool VBOSupported(){ return ready; }
unsigned int GenBuffer(){ GLuint h=0; W3D_GLC(glGenBuffers(1,&h)); return h; }
void DeleteBuffer(unsigned int h){
    if(!h) return;
    W3D_GLC(glDeleteBuffers(1,&h));
    buffers.erase(h);
    Arr* a[4]={&aPos,&aNrm,&aUV,&aCol};
    for(int i=0;i<4;i++) if(a[i]->vbo==h) a[i]->vbo=0;
    if(iboVBO==h) iboVBO=0;
    if(mixUVVbo==h) mixUVVbo=0;
    AttrForget();   // (GL lo desbindeo de donde estuviera: el cache ya no sabe)
}
void ArrayBufferData(unsigned int h, const void* data, int bytes){
    if(!h || bytes<=0) return;
    BindArray(h);
    W3D_GLC(glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)bytes,data,GL_STATIC_DRAW));
    BufInfo& b = buffers[h]; b.bytes = bytes; b.verts = 0; b.u32 = false;
    g_statUploadBytes += bytes;
}
void IndexBufferData(unsigned int h, const void* data, int bytes){
    if(!h || bytes<=0 || !data) return;
    const MeshIndex* idx = (const MeshIndex*)data;
    const int n = bytes / (int)sizeof(MeshIndex);
    int maxI = 0; for(int i=0;i<n;i++) if((int)idx[i] > maxI) maxI = (int)idx[i];
    if(indicesU32 < 0){ const char* ext=(const char*)glGetString(GL_EXTENSIONS); indicesU32 = (ext && strstr(ext,"GL_OES_element_index_uint")) ? 1 : 0; }
    const bool u32 = sizeof(MeshIndex) == 4 && maxI > 65535 && indicesU32 == 1;
    if(sizeof(MeshIndex) == 4 && maxI > 65535 && !u32) w3dLogfW("[w3dGLES2] malla de %d vertices: el driver no dibuja indices de 32 bits", maxI + 1);
    if(bufIndicesGL != (GLint)h){ W3D_GLC(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,h)); bufIndicesGL = (GLint)h; }
    if(u32 || sizeof(MeshIndex) == 2) W3D_GLC(glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)bytes,data,GL_STATIC_DRAW));
    else {
        static std::vector<unsigned short> i16; i16.resize(n);
        for(int i=0;i<n;i++) i16[i] = (unsigned short)idx[i];
        W3D_GLC(glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(n*2),&i16[0],GL_STATIC_DRAW));
    }
    BufInfo& b = buffers[h]; b.bytes = u32 ? bytes : n*2; b.verts = maxI + 1; b.u32 = u32;
    g_statUploadBytes += (int)b.bytes;
}
long VBOMemory(){ long t=0; for(std::map<GLuint,BufInfo>::const_iterator it=buffers.begin(); it!=buffers.end(); ++it) t+=it->second.bytes; return t; }
// (los formatos son los de la malla: pos 3 float, normal 3 byte, color 4 ubyte, uv 2 float)
void VertexVBO(unsigned int h){ aPos.size=3; aPos.type=GL_FLOAT; aPos.stride=0; aPos.ptr=0; aPos.vbo=h; }
void NormalVBO(unsigned int h){ aNrm.size=3; aNrm.type=GL_BYTE; aNrm.stride=0; aNrm.ptr=0; aNrm.vbo=h; }
void ColorVBO(unsigned int h){ aCol.size=4; aCol.type=GL_UNSIGNED_BYTE; aCol.stride=0; aCol.ptr=0; aCol.vbo=h; }
void TexCoordVBO(unsigned int h){ aUV.size=2; aUV.type=GL_FLOAT; aUV.stride=0; aUV.ptr=0; aUV.vbo=h; }
void TexCoordVBO2s(unsigned int h){ aUV.size=2; aUV.type=GL_SHORT; aUV.stride=0; aUV.ptr=0; aUV.vbo=h; }
void VertexVBO3s(unsigned int h){ aPos.size=3; aPos.type=GL_SHORT; aPos.stride=0; aPos.ptr=0; aPos.vbo=h; }
void BindIndexVBO(unsigned int h){ iboVBO=h; }
// del IBO bindeado: 'first' indices adelante
static void DrawFromIBO(GLenum mode, int count, int first){
    if(!ready || count<=0 || !iboVBO) return;
    MeasureDraw measure; gLastIdx = count; gLastVBO = 1;
    std::map<GLuint,BufInfo>::const_iterator it = buffers.find(iboVBO);
    const int nV = (it != buffers.end()) ? it->second.verts : 0;
    const bool u32 = (it != buffers.end()) && it->second.u32;
    Prim(mode); setupState(nV);
    if(bufIndicesGL != (GLint)iboVBO){ W3D_GLC(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,iboVBO)); bufIndicesGL = (GLint)iboVBO; }
    g_statDrawTris++; g_statDrawVBO++; g_statIndices += count; g_statDcCat[gStatCat]++;
    W3D_DRAW_TESTED(glDrawElements(mode,count,u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,(const void*)(size_t)(first * (u32 ? 4 : 2))));
}
void DrawTrianglesVBO(int count, int firstIndex){ DrawFromIBO(GL_TRIANGLES,count,firstIndex); }
void UnbindVBOs(){ aPos.vbo=aNrm.vbo=aUV.vbo=aCol.vbo=0; iboVBO=0; }
// la pila perezosa y el cache de punteros son del pipeline fijo: aca la pila ya vive en la CPU y va al shader por
// uniforms en cada draw (perezosa de por si) y los punteros se leen recien al dibujar. Las opciones existen para el harness
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool g_matricesLazy = true;
#endif
#ifndef W3D_BACKEND_NS   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
bool g_pointersCache = true;
#endif
void MatricesSync() {}

// ============================================================================
//  LOTE (con un programa por draw: la VideoCore IV de Android 2.3). En ese driver cada draw cuesta ~0.15 ms y la UI hace
//  ~150 por cuadro (un texto, un fondo, un icono: un draw cada uno). Los draws de triangulos sin indices, sin luz,
//  niebla, recorte ni mezcla, se juntan -ya pasados por la modelview en la CPU, con su color- y salen en UN draw cuando
//  cambia algo de lo que no se junta (la textura, la proyeccion...). Cualquier llamada a GL dibuja antes lo juntado
//  (W3D_GLC): nada se adelanta a un draw pendiente. DrawPending lo dibuja antes del swap
// ============================================================================
struct Batch { bool tex; int env; float alphaRef; unsigned texture; Matrix4 proj, texm; };
static Batch batch;
static std::vector<float> batchPos, batchUV, batchCol;
static const int kBatchMax = 6000;   // vertices
static void BatchEmpty(){
    if(!batchN) return;
    const int n = batchN; batchN = 0;   // (antes de cualquier llamada a GL: W3D_GLC volveria a entrar aca)
    // el estado de cuando se junto (lo que el lote no admite, apagado); el de ahora se repone al final
    const bool tex0=cap_tex, light0=cap_light, fog0=cap_fog, spr0=pointSprite, tg0=texGenSphere, mez0=mixOn;
    const int env0=texEnv, clip0=clipN, aa0=alphaDown, prim0=primDraw;
    const float ar0=alphaRef, blur0=blurY;
    const Matrix4 proj0=curProj, mv0=curMV, texm0=curTex;
    const Arr pos0=aPos, nrm0=aNrm, uv0=aUV, col0=aCol;
    cap_tex=batch.tex; cap_light=cap_fog=pointSprite=texGenSphere=mixOn=false; texEnv=batch.env; clipN=0; alphaDown=0;
    alphaRef=batch.alphaRef; blurY=0.0f;
    curProj=batch.proj; curMV.Identity(); curTex=batch.texm;
    const Arr p={true,3,GL_FLOAT,0,&batchPos[0],0}, c={true,4,GL_FLOAT,0,&batchCol[0],0};
    const Arr u={batch.tex,2,GL_FLOAT,0,batch.tex ? &batchUV[0] : 0,0}, nothing={false,3,GL_FLOAT,0,0,0};
    aPos=p; aCol=c; aUV=u; aNrm=nothing;
    Prim(GL_TRIANGLES); setupState(n);
    g_statDrawTris++; g_statIndices += n; g_statDcCat[gStatCat]++;
    W3D_GLC(glDrawArrays(GL_TRIANGLES,0,n));
    cap_tex=tex0; cap_light=light0; cap_fog=fog0; pointSprite=spr0; texGenSphere=tg0; mixOn=mez0;
    texEnv=env0; clipN=clip0; alphaDown=aa0; primDraw=prim0; alphaRef=ar0; blurY=blur0;
    curProj=proj0; curMV=mv0; curTex=texm0;
    aPos=pos0; aNrm=nrm0; aUV=uv0; aCol=col0;
}
// (y en la VideoCore IV de Android 2.3, antes del swap, los ERRORES DE GL PENDIENTES se vacian: con uno pendiente el
//  eglSwapBuffers de ese driver NO muestra el cuadro -"eglSwapBuffers Error 0x505" en el log- y la pantalla quedaba
//  NEGRA aunque todo lo demas se hubiera dibujado. El log dice cada error distinto la primera vez)
void DrawPending(){
    BatchEmpty();
    if(!byDraw) return;
    for(int k = 0; k < 8; k++){
        const GLenum e = glGetError();
        if(e == GL_NO_ERROR) break;
        static unsigned seenList[4] = {0,0,0,0};
        bool seen = false;
        for(int i = 0; i < 4; i++) if(seenList[i] == (unsigned)e){ seen = true; break; }
        if(!seen){
            for(int i = 0; i < 4; i++) if(!seenList[i]){ seenList[i] = (unsigned)e; break; }
            w3dLogfW("[w3dGLES2] error de GL 0x%x en el cuadro (se vacia antes del swap: con uno pendiente el driver no muestra el cuadro)", (unsigned)e);
        }
    }
}
// el clear con scissor DIBUJADO (ver Clear): un rectangulo en el recorte, con el color de borrado y la profundidad al
// fondo, sin nada de lo que haya puesto (textura, luz, niebla, blending, matrices); despues todo vuelve como estaba
// una GPU CHICA (ver w3dGraphics.h): la que no se banca el programa entero
bool GpuLimited(){ return byDraw; }
static void ClearWithQuad(bool color, bool prof){
    BatchEmpty();
    if(!color && !prof) return;
    const bool tex0=cap_tex, light0=cap_light, fog0=cap_fog, spr0=pointSprite, tg0=texGenSphere, mez0=mixOn, cm0=cap_colormat;
    const bool blend0=cap_blend, depth0=cap_depth, cull0=cap_cull;
    const int env0=texEnv, clip0=clipN, aa0=alphaDown, prim0=primDraw;
    const float ar0=alphaRef, blur0=blurY;
    const Matrix4 proj0=curProj, mv0=curMV, texm0=curTex;
    const Arr pos0=aPos, nrm0=aNrm, uv0=aUV, col0=aCol;
    float colorPrev[4]; for(int i=0;i<4;i++) colorPrev[i]=curColor[i];
    const unsigned mask0=maskColor; const int dfunc0=depthFuncRequest, dmask0=depthMaskRequest;
    const float vp0[4]={vpCurrent[0],vpCurrent[1],vpCurrent[2],vpCurrent[3]};
    cap_tex=cap_light=cap_fog=pointSprite=texGenSphere=mixOn=cap_colormat=false;
    texEnv=0; clipN=0; alphaDown=0; alphaRef=0.0f; blurY=0.0f;
    curProj.Identity(); curMV.Identity(); curTex.Identity();
    Disable(Blend); Disable(CullFace);
    if(prof){ Enable(DepthTest); DepthFunc(DepthAlways); DepthMask(true); } else { Disable(DepthTest); DepthMask(false); }
    if(color) ColorMask((mask0&1)!=0,(mask0&2)!=0,(mask0&4)!=0,(mask0&8)!=0); else ColorMask(false,false,false,false);
    Viewport(scissorRequest[0],scissorRequest[1],scissorRequest[2],scissorRequest[3]);
    // (z un pelo antes del plano lejano: con z = w justo el redondeo lo recortaba, ver la proyeccion ajustada)
    static const float z = 0.99999f;
    static const float q[18] = { -1,-1,z, 1,-1,z, 1,1,z,  -1,-1,z, 1,1,z, -1,1,z };
    const Arr p={true,3,GL_FLOAT,0,q,0}, nothing={false,3,GL_FLOAT,0,0,0};
    aPos=p; aNrm=nothing; aUV=nothing; aCol=nothing;
    for(int i=0;i<4;i++) curColor[i]=clearCol[i];
    Prim(GL_TRIANGLES); setupState(6);
    W3D_GLC(glDrawArrays(GL_TRIANGLES,0,6));
    // todo como estaba
    cap_tex=tex0; cap_light=light0; cap_fog=fog0; pointSprite=spr0; texGenSphere=tg0; mixOn=mez0; cap_colormat=cm0;
    texEnv=env0; clipN=clip0; alphaDown=aa0; primDraw=prim0; alphaRef=ar0; blurY=blur0;
    curProj=proj0; curMV=mv0; curTex=texm0;
    aPos=pos0; aNrm=nrm0; aUV=uv0; aCol=col0;
    for(int i=0;i<4;i++) curColor[i]=colorPrev[i];
    if(blend0) Enable(Blend); if(cull0) Enable(CullFace);
    if(depth0) Enable(DepthTest); else Disable(DepthTest);
    DepthFunc((DepthCmp)dfunc0); DepthMask(dmask0!=0);
    ColorMask((mask0&1)!=0,(mask0&2)!=0,(mask0&4)!=0,(mask0&8)!=0);
    Viewport((int)vp0[0],(int)vp0[1],(int)vp0[2],(int)vp0[3]);
}
// un draw de triangulos sin indices (vertices first .. first+n-1) al lote, si se puede; false = se dibuja solo
static bool ToBatch(int first, int n){
    if(!byDraw || n > kBatchMax) return false;
    if(cap_light || cap_fog || pointSprite || (mixOn && mixTex) || clipN>0 || blurY>0.0f || alphaDown || (texGenSphere && cap_tex)) return false;
    if(!aPos.on || !aPos.ptr || aPos.vbo || aPos.type!=GL_FLOAT || (aPos.size!=2 && aPos.size!=3)) return false;
    if(cap_tex && (!aUV.on || !aUV.ptr || aUV.vbo || aUV.type!=GL_FLOAT || aUV.size!=2)) return false;
    const bool colArr = aCol.on && (aCol.ptr || aCol.vbo);
    if(colArr && (aCol.vbo || aCol.size!=4 || (aCol.type!=GL_UNSIGNED_BYTE && aCol.type!=GL_FLOAT))) return false;
    const float* M = curMV.m;
    if(M[3]!=0.0f || M[7]!=0.0f || M[11]!=0.0f || M[15]!=1.0f) return false;   // (solo modelview afin)
    if(batchN){
        bool equal = batch.tex==cap_tex && batch.alphaRef==alphaRef && batchN + n <= kBatchMax && memcmp(batch.proj.m, curProj.m, sizeof(curProj.m))==0;
        if(equal && cap_tex) equal = batch.env==texEnv && batch.texture==boundTex && memcmp(batch.texm.m, curTex.m, sizeof(curTex.m))==0;
        if(!equal) BatchEmpty();
    }
    if(!batchN){ batch.tex=cap_tex; batch.env=texEnv; batch.alphaRef=alphaRef; batch.texture=boundTex; batch.proj=curProj; batch.texm=curTex; }
    batchPos.resize((size_t)(batchN+n)*3); batchCol.resize((size_t)(batchN+n)*4);
    if(cap_tex) batchUV.resize((size_t)(batchN+n)*2);
    // posiciones por la modelview
    const int sp = aPos.stride ? aPos.stride : aPos.size*4;
    const unsigned char* bp = (const unsigned char*)aPos.ptr + (size_t)first*sp;
    float* dp = &batchPos[(size_t)batchN*3];
    for(int i=0;i<n;i++, dp+=3){
        const float* v = (const float*)(bp + (size_t)i*sp);
        const float x=v[0], y=v[1], z=(aPos.size==3) ? v[2] : 0.0f;
        dp[0]=M[0]*x+M[4]*y+M[8]*z+M[12]; dp[1]=M[1]*x+M[5]*y+M[9]*z+M[13]; dp[2]=M[2]*x+M[6]*y+M[10]*z+M[14];
    }
    if(cap_tex){
        const int su = aUV.stride ? aUV.stride : 8;
        const unsigned char* bu = (const unsigned char*)aUV.ptr + (size_t)first*su;
        float* du = &batchUV[(size_t)batchN*2];
        for(int i=0;i<n;i++, du+=2){ const float* t=(const float*)(bu + (size_t)i*su); du[0]=t[0]; du[1]=t[1]; }
    }
    float* dc = &batchCol[(size_t)batchN*4];
    if(colArr && aCol.type==GL_UNSIGNED_BYTE){   // (GL los normaliza igual: c / 255)
        const int sc = aCol.stride ? aCol.stride : 4;
        const unsigned char* bc = (const unsigned char*)aCol.ptr + (size_t)first*sc;
        for(int i=0;i<n;i++, dc+=4){ const unsigned char* k=bc + (size_t)i*sc; for(int j=0;j<4;j++) dc[j]=k[j]/255.0f; }
    } else if(colArr){
        const int sc = aCol.stride ? aCol.stride : 16;
        const unsigned char* bc = (const unsigned char*)aCol.ptr + (size_t)first*sc;
        for(int i=0;i<n;i++, dc+=4){ const float* k=(const float*)(bc + (size_t)i*sc); for(int j=0;j<4;j++) dc[j]=k[j]; }
    } else for(int i=0;i<n;i++, dc+=4) for(int j=0;j<4;j++) dc[j]=curColor[j];
    batchN += n;
    return true;
}

void DrawTrianglesArray(int vertexCount){ if(!ready||vertexCount<=0) return; MeasureDraw measure; gLastIdx = vertexCount; gLastVBO = 0; if(ToBatch(0,vertexCount)) return; Prim(GL_TRIANGLES); setupState(vertexCount); g_statDrawTris++; g_statIndices += vertexCount; g_statDcCat[gStatCat]++; W3D_DRAW_TESTED(glDrawArrays(GL_TRIANGLES,0,vertexCount)); }
void DrawTrianglesArrayFrom(int first,int count){ if(!ready||count<=0) return; MeasureDraw measure; gLastIdx = count; gLastVBO = 0; if(ToBatch(first,count)) return; Prim(GL_TRIANGLES); setupState(first+count); g_statDrawTris++; g_statIndices += count; g_statDcCat[gStatCat]++; W3D_DRAW_TESTED(glDrawArrays(GL_TRIANGLES,first,count)); }
void DrawLines(int vertexCount){ if(!ready||vertexCount<=0) return; Prim(GL_LINES); setupState(vertexCount); W3D_GLC(glDrawArrays(GL_LINES,0,vertexCount)); }
void DrawLineStrip(int vertexCount){ if(!ready||vertexCount<=0) return; Prim(GL_LINE_STRIP); setupState(vertexCount); W3D_GLC(glDrawArrays(GL_LINE_STRIP,0,vertexCount)); }
void DrawPoints(int vertexCount){ if(!ready||vertexCount<=0) return; Prim(GL_POINTS); setupState(vertexCount); W3D_GLC(glDrawArrays(GL_POINTS,0,vertexCount)); }

void Wireframe(bool){}              // ES2 no tiene glPolygonMode: se dibuja relleno (como GLES1). Wireframe real = draw de aristas.
void PointSpriteCoordReplace(bool on){ pointSprite=on; } // samplear la textura con gl_PointCoord (point sprites)

// ============================================================================
//  Init: carga las funciones GL2.0 (desktop) y compila el shader
// ============================================================================
void GLES2Init(void* (*getProc)(const char*)){
#if !defined(W3D_ES2_NATIVE)
    glCreateShader=(PFcreateShader)getProc("glCreateShader");
    glShaderSource=(PFshaderSource)getProc("glShaderSource");
    glCompileShader=(PFcompileShader)getProc("glCompileShader");
    glGetShaderiv=(PFgetShaderiv)getProc("glGetShaderiv");
    glGetShaderInfoLog=(PFgetShaderInfoLog)getProc("glGetShaderInfoLog");
    glCreateProgram=(PFcreateProgram)getProc("glCreateProgram");
    glAttachShader=(PFattachShader)getProc("glAttachShader");
    glLinkProgram=(PFlinkProgram)getProc("glLinkProgram");
    glGetProgramiv=(PFgetProgramiv)getProc("glGetProgramiv");
    glGetProgramInfoLog=(PFgetProgramInfoLog)getProc("glGetProgramInfoLog");
    glUseProgram=(PFuseProgram)getProc("glUseProgram");
    glGetUniformLocation=(PFgetUniformLocation)getProc("glGetUniformLocation");
    glGetAttribLocation=(PFgetAttribLocation)getProc("glGetAttribLocation");
    glUniform1i=(PFuniform1i)getProc("glUniform1i");
    glUniform1f=(PFuniform1f)getProc("glUniform1f");
    glUniform4fv=(PFuniform4fv)getProc("glUniform4fv");
    glUniformMatrix3fv=(PFuniformMatrix3fv)getProc("glUniformMatrix3fv");
    glUniformMatrix4fv=(PFuniformMatrix4fv)getProc("glUniformMatrix4fv");
    glGenBuffers=(PFgenBuffers)getProc("glGenBuffers");
    glBindBuffer=(PFbindBuffer)getProc("glBindBuffer");
    glBufferData=(PFbufferData)getProc("glBufferData");
    glVertexAttribPointer=(PFvertexAttribPointer)getProc("glVertexAttribPointer");
    glEnableVertexAttribArray=(PFenableVAA)getProc("glEnableVertexAttribArray");
    glDisableVertexAttribArray=(PFdisableVAA)getProc("glDisableVertexAttribArray");
    glVertexAttrib4fv=(PFvertexAttrib4fv)getProc("glVertexAttrib4fv");
    glActiveTexture=(PFactiveTexture)getProc("glActiveTexture");
    glBindAttribLocation=(PFbindAttribLocation)getProc("glBindAttribLocation");
    glDeleteShader=(PFdeleteShader)getProc("glDeleteShader");
    glDeleteBuffers=(PFdeleteBuffers)getProc("glDeleteBuffers");
    glDeleteProgram=(PFdeleteProgram)getProc("glDeleteProgram");
    glBlendEquation=(PFblendEquation)getProc("glBlendEquation");
    glGenFramebuffers=(PFgenFramebuffers)getProc("glGenFramebuffers");
    glDeleteFramebuffers=(PFdeleteFramebuffers)getProc("glDeleteFramebuffers");
    glBindFramebuffer=(PFbindFramebuffer)getProc("glBindFramebuffer");
    glFramebufferTexture2D=(PFframebufferTexture2D)getProc("glFramebufferTexture2D");
    glCheckFramebufferStatus=(PFcheckFramebufferStatus)getProc("glCheckFramebufferStatus");
    glGenRenderbuffers=(PFgenRenderbuffers)getProc("glGenRenderbuffers");
    glDeleteRenderbuffers=(PFdeleteRenderbuffers)getProc("glDeleteRenderbuffers");
    glBindRenderbuffer=(PFbindRenderbuffer)getProc("glBindRenderbuffer");
    glRenderbufferStorage=(PFrenderbufferStorage)getProc("glRenderbufferStorage");
    glFramebufferRenderbuffer=(PFframebufferRenderbuffer)getProc("glFramebufferRenderbuffer");
    // GL de escritorio: el tamanio del punto lo da el shader y el sprite tiene gl_PointCoord (en ES2 ya es asi)
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
    glEnable(GL_POINT_SPRITE);
#else
    (void)getProc;
#endif
    curProj.Identity(); curMV.Identity(); curTex.Identity();
    { const char* r = (const char*)glGetString(GL_RENDERER);
      // (W3D_CLEAR_DRAWN=1: el mismo camino en cualquier GPU, para probarlo en la PC)
      const char* forceClear = getenv("W3D_CLEAR_DRAWN");
      clearScissorDrawn = (r && strstr(r, "VideoCore III")) || (forceClear && forceClear[0] == '1');
      if(clearScissorDrawn) w3dLogfW("[w3dGLES2] %s: el clear con scissor se dibuja (el del driver se lleva el cuadro)", r); }
    // (un contexto nuevo arranca con todo lo de fabrica)
    cap_depth=cap_cull=cap_blend=cap_scissor=cap_offset=false; cap_dither=true; boundTex=0; StateGLForget();
    buildProgram();
    { GLint v[4]={0,0,1,1}; glGetIntegerv(GL_VIEWPORT,v); for(int i=0;i<4;i++) vpCurrent[i]=(float)v[i]; }
}

W3D_BACKEND_CLOSE   // (el namespace propio de este backend)
} // namespace w3dEngine

#if defined(__ANDROID__) && !defined(W3D_BACKEND_NS)   // (con varios backends lo define graphics/w3dGraphicsDispatch.cpp)
#include "w3dBase.h"
    void w3dSetColor(const ColorType c[4]) {
        w3dEngine::Color4f(c[0], c[1], c[2], c[3]);
    }
#endif

// ---- VARIOS BACKENDS en un ejecutable (graphics/w3dBackend.h): este llena SU tabla del despacho ----
#ifdef W3D_BACKEND_NS
#include "w3dBackendTable.h"
namespace w3dEngine { namespace W3D_BACKEND_NS {
#include "w3dBackendFill.inc"
} }
#endif
