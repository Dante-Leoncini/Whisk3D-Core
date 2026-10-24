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
#include "w3dlog.h"
#include "math/Matrix4.h"
#include <vector>
#include <map>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // getenv (W3D_GLES2_POR_DRAW)
#include <time.h>     // clock_gettime (g_medirDraws)
#ifdef __ANDROID__
#include <dlfcn.h>    // el alto de la ventana por EGL (CopiaPantallaFila)
#endif

#ifdef __EMSCRIPTEN__
    #include <GLES2/gl2.h>
#elif defined(__ANDROID__)
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
bool w3dRenderWireframe = false;
bool w3dRenderWireframeSoloVisible = true; // wireframe SOLO de la lista activa (pvsFaces)
bool w3dRenderSolido    = false;
bool w3dRenderSinLuz    = false;
bool w3dRenderLuces     = false;
bool w3dRenderNormalColor = false;
bool w3dRenderAlpha       = false; // pase ALPHA (matte): blanco unlit + solo el alpha de la textura
bool w3dRenderOverlays    = true;
bool w3dVerSeleccion      = true;  // "Ver seleccion" (menu Select): OFF = ni contorno ni tinte de seleccion
bool g_xray               = false; // X-Ray OFF por defecto (lo togglea el menu Overlays)

namespace w3dEngine {

// el CONTADOR DE LLAMADAS (el mismo que el backend fijo, ver w3dGraphics.h): cada gl* real suma 1 a la categoria del que
// dibuja. Aca no hay cache de estado: un draw cuesta ~45 llamadas (uniforms, atributos, programa)
int g_statLlamadas[StatCatCount_] = { 0 };
static int gStatCat = StatCatEditor;
// (g_medirDraws, ver StatsCuadro) cuanto tarda cada draw: se mide entero con un MedirDraw al principio
struct MedirDraw { double t0; MedirDraw(); ~MedirDraw(); };
static int gUltimoIdx = 0, gUltimoVerts = 0, gUltimoVBO = 0;   // (los del draw en curso)
// la TRAZA (medir), misma que el backend GLES 1.1: con g_trazaGL cada llamada suma en su texto ("glBindTexture(...)")
// y TrazaGLVer la imprime ordenada; con g_trazaGLSec ademas sale cada llamada en orden, con su categoria
bool g_trazaGL = false;
bool g_trazaGLSec = false;
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
// (antes de cada llamada a GL se dibuja el LOTE juntado, si hay: ver LoteVaciar)
static int loteN = 0;
static void LoteVaciar();
#define W3D_GLC(llamada) (loteN ? LoteVaciar() : (void)0, g_statLlamadas[gStatCat]++, g_trazaGL ? TrazaGL(#llamada) : (void)0, (llamada))

// ============================================================================
//  Estado emulado del pipeline fijo
// ============================================================================
static bool cap_depth=false, cap_cull=false, cap_tex=false, cap_light=false, cap_blend=false, cap_fog=false, cap_scissor=false;
static bool cap_offset=false;   // GL_POLYGON_OFFSET_FILL
static bool cap_colormat=false; // GL_COLOR_MATERIAL: el color del VERTICE reemplaza el diffuse/ambient del material
static int  matMode = 1; // 0=Projection, 1=ModelView, 2=Texture
static Matrix4 curProj, curMV;
static Matrix4 curTex;     // matriz de TEXTURA de la unidad 0 (TexMatrixUV / matcap / el flip-V del sphere-map)
static std::vector<Matrix4> stkProj, stkMV, stkTex;
// LAS LUCES del pipeline fijo, con sus valores de fabrica de GL: la 0 difusa y especular blancas, las otras negras;
// el ambiente de cada luz negro; sin atenuacion; sin cono. La posicion y la direccion del cono se guardan en OJO
// (transformadas con la modelview del momento, como glLightfv).
struct Luz { bool on; float pos[4], amb[4], dif[4], spec[4], att[3], dir[3], corteCos, expo; };
static Luz luces[LucesMax];
static float ambGlobal[4] = {0.2f,0.2f,0.2f,1};   // GL_LIGHT_MODEL_AMBIENT
static bool  lucesSucias = true;                    // hay que volver a mandar las luces al shader
static void LucesDeFabrica(){
    for(int i=0;i<LucesMax;i++){
        Luz& L=luces[i];
        L.on=false;
        L.pos[0]=0; L.pos[1]=0; L.pos[2]=1; L.pos[3]=0;
        for(int k=0;k<4;k++){ L.amb[k]=(k==3)?1.0f:0.0f; L.dif[k]=(i==0||k==3)?1.0f:0.0f; L.spec[k]=L.dif[k]; }
        L.att[0]=1; L.att[1]=0; L.att[2]=0;
        L.dir[0]=0; L.dir[1]=0; L.dir[2]=-1;
        L.corteCos=-1.0f; L.expo=0.0f;   // corte 180 = sin cono
    }
    lucesSucias=true;
}
static float matDiff[4]   = {0.8f,0.8f,0.8f,1};
static float matAmb[4]    = {0.2f,0.2f,0.2f,1};
static float matSpec[4]   = {0,0,0,1};
static float matEmis[4]   = {0,0,0,1};
static float matShine     = 0.0f;
static float curColor[4]  = {1,1,1,1};   // color uniforme (Color4*) cuando NO hay array de color
static float fogColor[4]  = {0,0,0,1};
static float fogStart=0, fogEnd=1;
static int   fogModo=0;       // 0 lineal / 1 exp / 2 exp2 (el objeto Niebla), igual que GL_FOG_MODE
static float fogDens=1.0f;
static int   texEnv=0;        // el combiner de la unidad 0: 0 modulate, 1 replace, 2 DOT3, 3 solo alfa
static bool  texGenEsfera=false;   // GL_SPHERE_MAP: las UV salen del reflejo (en el shader)
// LA MATRIZ DE TEXTURA la tocan tres cosas (sphere-map, matcap, desplazamiento de UV) con los MISMOS caches que el
// backend fijo: asi se reinicia exactamente en los mismos momentos (un "apagar" de lo que ya estaba apagado no la toca)
static int texGenCache = -1;   // 0/1
static int matcapCache = -1;   // 0 identidad / 1 matcap / 2 desplazamiento de UV
static float uvMat[4] = { 0, 0, 1, 1 };
static bool  pointSprite=false; // point sprite: samplear la textura con gl_PointCoord (iconos: cursor/origen/luz)
static float pointSize=1.0f;
static float vpActual[4]={0,0,1,1};   // el glViewport actual (el sprite saca su centro en pixeles de ventana)
static unsigned boundTex=0;
// LA UNIDAD 1 (mezcla de texturas): la textura B, como se combina y sus UV (propias o las de la unidad 0)
static bool     mezclaOn=false;
static unsigned mezclaTex=0;
static int      mezclaModo=0;   // 0 interpolar con k, 1 multiplicar, 2 multiplicar x2, 3 restar
static float    mezclaK=1.0f;
static const float* mezclaUV=0; // UV propias de la unidad 1 (0 = las mismas de la unidad 0)
static GLuint mezclaUVVbo=0;    // o en un buffer object
// un atributo: de RAM (ptr) o de un buffer object (vbo, desde el byte 0: ver VertexVBO y compania)
struct Arr { bool on; int size; unsigned type; int stride; const void* ptr; GLuint vbo; };
static Arr aPos={false,3,0,0,0}, aNrm={false,3,0,0,0}, aUV={false,2,0,0,0}, aCol={false,4,0,0,0};

// ---- objetos GL + locations ----
static GLuint prog=0, vboP=0, vboN=0, vboT=0, vboC=0, vboT1=0, ibo=0;
static GLint uMVP,uMV,uNMat,uTexM,uTexGen,uUseTex,uTex,uTex1,uEnv,uLightOn,uFogOn,uPointSize,uPointSprite,uColorMat,uBlurY,uAlphaRef,uAlfaAbajo;
static GLint uMezcla,uMezclaK,uViewport;
static GLint uClipN,uClip[5];   // planos de recorte (ver PlanosRecorte): equivalente al GL_CLIP_PLANEi del pipeline fijo
static float blurY = 0.0f;
static float alphaRef = 0.0f;
static int   alfaAbajo = 0;
static GLint uMDiff,uMAmb,uMSpec,uMEmis,uShine,uAmbGlobal,uNLuces,uLPos,uLAmb,uLDif,uLSpec,uLAtt,uLCono;
static GLint uFogColor,uFogStart,uFogEnd,uFogModo,uFogDens;
// los atributos, en lugares fijos (los mismos en todos los programas, ver linkear)
static const GLint aLpos=0, aLnrm=1, aLuv=2, aLcol=3, aLuv1=4;
static bool ready=false;
// los planos de recorte en OJO (ver PlanosRecorte), para subirlos al programa de cada draw
static int clipN=0;
static float clipEq[5][4];
// lo que GL tiene en cada atributo (los 5 lugares fijos) y en GL_ARRAY_BUFFER, para no repetir llamadas. El valor
// constante y el prendido son del contexto (no del programa)
struct AttrGL { int on; float cte[4]; bool hayCte; const void* ptr; GLuint vbo; int size; unsigned type; GLboolean norm; int stride; };
static AttrGL attrGL[5];
static GLint bufArrayGL = -1, bufIndicesGL = -1;
static void AttrOlvidar(){ memset(attrGL, 0, sizeof(attrGL)); for(int i=0;i<5;i++) attrGL[i].on = -1; bufArrayGL = bufIndicesGL = -1; }
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
"uniform int uTexGen; uniform int uLightOn; uniform int uColorMat; uniform vec4 uViewport;\n"
"uniform vec4 uMDiff; uniform vec4 uMAmb; uniform vec4 uMSpec; uniform vec4 uMEmis; uniform float uShine; uniform vec4 uAmbGlobal;\n"
"uniform int uNLuces; uniform vec4 uLPos[8]; uniform vec4 uLAmb[8]; uniform vec4 uLDif[8]; uniform vec4 uLSpec[8];\n"
"uniform vec4 uLAtt[8];\n"    // constante, lineal, cuadratica, exponente del cono
"uniform vec4 uLCono[8];\n"   // direccion del cono (ojo) + coseno del corte (-1 = sin cono)
"attribute vec3 aPos; attribute vec3 aNrm; attribute vec4 aUV; attribute vec4 aCol; attribute vec2 aUV1;\n"
"varying vec3 vP; varying vec2 vT; varying vec2 vT1; varying vec4 vC; varying float vFogZ;\n"
"varying vec2 vCentro; varying float vTam;\n"   // el centro y el tamanio del punto en pixeles de ventana (para el sprite)
"void main(){\n"
"  gl_Position=uMVP*vec4(aPos,1.0);\n"
// (la posicion en OJO solo para lo que la usa: sin eso uMV no queda en el programa y no se sube en cada draw)
"#if defined(W3D_LUZ) || defined(W3D_TEXGEN) || defined(W3D_CLIP) || defined(W3D_NIEBLA)\n"
"  vec4 eye=uMV*vec4(aPos,1.0);\n"
"#endif\n"
"#ifdef W3D_SPRITE\n"
"  vCentro=(gl_Position.xy/gl_Position.w*0.5+0.5)*uViewport.zw+uViewport.xy; vTam=uPointSize;\n"
"#endif\n"
"#if !defined(W3D_ESPECIAL) || defined(W3D_PUNTOS)\n"   // (el tamanio del punto, solo dibujando puntos)
"  gl_PointSize=uPointSize;\n"
"#endif\n"
"#ifdef W3D_CLIP\n"
"  vP=eye.xyz;\n"
"#endif\n"
"#ifdef W3D_NIEBLA\n"
"  vFogZ=-eye.z;\n"
"#endif\n"
"#ifdef W3D_MEZCLA\n"
"  vT1=aUV1;\n"
"#endif\n"
"#if defined(W3D_LUZ) || defined(W3D_TEXGEN)\n"
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
"#if defined(W3D_ESPECIAL) && !defined(W3D_TEXM)\n"   // (la matriz de textura es la identidad)
"  vT=uv.xy;\n"
"#else\n"
"  vec4 t=uTexM*uv; vT=t.xy/t.w;\n"
"#endif\n"
"#endif\n"
"  vec4 c=aCol;\n"
"#ifdef W3D_LUZ\n"
"  if(uLightOn==1){\n"
"    vec4 md=(uColorMat==1)?aCol:uMDiff; vec4 ma=(uColorMat==1)?aCol:uMAmb;\n"   // COLOR_MATERIAL = AMBIENT_AND_DIFFUSE
"    vec3 acc=uMEmis.rgb+ma.rgb*uAmbGlobal.rgb;\n"
// las luces una por una (LUCES = LUZ(0) LUZ(1) ..., ver DefsPrograma) y no con un for: el compilador del driver Broadcom
// (VideoCore IV, Android 2.3) desenrolla mal ese for y no linkea el programa ("Indexing with an integral constant
// expression greater than declared size"). La macro va en UNA linea (sin \n entre los pedazos): GLSL ES 1.00 no
// continua lineas con '\'
"#define LUZ(i) if(i<uNLuces){ vec3 L; float at=1.0;"
"      if(uLPos[i].w!=0.0){ vec3 d=uLPos[i].xyz/uLPos[i].w-eye.xyz; float dist=length(d); L=d/max(dist,1e-6);"
"        at=1.0/max(uLAtt[i].x+uLAtt[i].y*dist+uLAtt[i].z*dist*dist,1e-6); }"
"      else L=normalize(uLPos[i].xyz);"
"      if(uLCono[i].w>-0.5){ float sd=dot(-L,normalize(uLCono[i].xyz)); at*=(sd>=uLCono[i].w)?pow(max(sd,0.0),uLAtt[i].w):0.0; }"
"      float ndl=max(dot(N,L),0.0);"
"      vec3 col=ma.rgb*uLAmb[i].rgb+md.rgb*uLDif[i].rgb*ndl;"
"      if(ndl>0.0){ float nh=max(dot(N,normalize(L+vec3(0.0,0.0,1.0))),0.0);"
"        col+=uMSpec.rgb*uLSpec[i].rgb*((uShine>0.0)?pow(nh,uShine):1.0); }"
"      acc+=at*col; }\n"
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
"#define W3D_ALTA highp\n"
"#else\n"
"#define W3D_ALTA mediump\n"
"#endif\n"
"#else\n"
"#define W3D_ALTA\n"
"#endif\n"
"uniform int uUseTex; uniform int uEnv; uniform int uFogOn;\n"
"uniform float uBlurY;\n"   // blur vertical (0 = apagado)
"uniform float uAlphaRef;\n" // descarte por alpha (0 = apagado)
"uniform int uAlfaAbajo;\n"  // 1 = el ALPHA viene en la mitad de ABAJO de la textura
"uniform int uPointSprite;\n"
"uniform sampler2D uTex; uniform sampler2D uTex1; uniform int uMezcla; uniform float uMezclaK;\n"
"uniform vec4 uFogColor; uniform float uFogStart; uniform float uFogEnd; uniform int uFogModo; uniform float uFogDens;\n"
"uniform int uClipN; uniform vec4 uClip0,uClip1,uClip2,uClip3,uClip4;\n"   // planos de recorte, en OJO
"varying vec3 vP; varying vec2 vT; varying vec2 vT1; varying vec4 vC; varying float vFogZ;\n"
"varying W3D_ALTA vec2 vCentro; varying W3D_ALTA float vTam;\n"
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
"  if(uPointSprite==1){ W3D_ALTA vec2 d=(gl_FragCoord.xy-vCentro)/vTam; uv=vec2(0.5+d.x,0.5-d.y); }\n"
"#endif\n"
"#endif\n"
"  vec4 col = vC;\n"   // sin textura: el color primario (la luz, o el color del vertice)
"#ifdef W3D_TEX\n"
"#ifdef W3D_ESPECIAL\n"   // (con un programa por draw, W3D_TEX ya es "hay textura")
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
"#ifdef W3D_ALFA_ABAJO\n"
"    if(uAlfaAbajo==1){\n"                      // video con alpha EMPAQUETADO. El frame se sube
"                                             \n"  // invertido en Y, asi que en la TEXTURA el color
"                                             \n"  // queda en la mitad de ABAJO y la mascara arriba.
"      tex.a = texture2D(uTex, vec2(uv.x, uv.y - 0.5)).r;\n"  // la mascara, media textura mas abajo
"    }\n"
"#endif\n"
"#if defined(W3D_ENV)\n"   // (el modo, constante del programa)
"#if W3D_ENV==1\n    col=tex;\n"
"#elif W3D_ENV==2\n    col=vec4(vec3(clamp(dot(tex.rgb*2.0-1.0,vC.rgb*2.0-1.0),0.0,1.0)),tex.a*vC.a);\n"
"#elif W3D_ENV==3\n    col=vec4(vC.rgb,tex.a*vC.a);\n"
"#else\n    col=vC*tex;\n#endif\n"
"#else\n"
"    if(uEnv==1) col=tex;\n"                                                         // GL_REPLACE
"    else if(uEnv==2) col=vec4(vec3(clamp(dot(tex.rgb*2.0-1.0,vC.rgb*2.0-1.0),0.0,1.0)),tex.a*vC.a);\n" // GL_DOT3_RGB
"    else if(uEnv==3) col=vec4(vC.rgb,tex.a*vC.a);\n"                              // solo alfa (por el del color)
"    else col=vC*tex;\n"                                                            // GL_MODULATE
"#endif\n"
"  }\n"
"#endif\n"
"#ifdef W3D_MEZCLA\n"
"#ifdef W3D_MEZCLA_MODO\n"
"  { vec4 b=texture2D(uTex1,vT1);\n"
"#if W3D_MEZCLA_MODO==0\n    col.rgb=mix(col.rgb,b.rgb,uMezclaK);\n"
"#elif W3D_MEZCLA_MODO==3\n    col.rgb=max(col.rgb-b.rgb,0.0);\n"
"#elif W3D_MEZCLA_MODO==2\n    col.rgb=min(col.rgb*b.rgb*2.0,1.0);\n"
"#else\n    col.rgb=min(col.rgb*b.rgb,1.0);\n#endif\n"
"  }\n"
"#else\n"
"  if(uMezcla>=0){ vec4 b=texture2D(uTex1,vT1);\n"   // la unidad 1 sobre lo que salio de la 0 (el alfa no cambia)
"    if(uMezcla==0) col.rgb=mix(col.rgb,b.rgb,uMezclaK);\n"
"    else if(uMezcla==3) col.rgb=max(col.rgb-b.rgb,0.0);\n"
"    else col.rgb=min(col.rgb*b.rgb*((uMezcla==2)?2.0:1.0),1.0); }\n"
"#endif\n"
"#endif\n"
"#ifdef W3D_NIEBLA\n"
"#ifdef W3D_NIEBLA_MODO\n"
"  { float f;\n"
"#if W3D_NIEBLA_MODO==1\n    f=exp(-uFogDens*vFogZ);\n"
"#elif W3D_NIEBLA_MODO==2\n    float e=uFogDens*vFogZ; f=exp(-e*e);\n"
"#else\n    f=(uFogEnd-vFogZ)/(uFogEnd-uFogStart);\n#endif\n"
"    col.rgb=mix(uFogColor.rgb,col.rgb,clamp(f,0.0,1.0)); }\n"
"#else\n"
"  if(uFogOn==1){ float f;\n"
"    if(uFogModo==1) f=exp(-uFogDens*vFogZ);\n"                                   // GL_EXP
"    else if(uFogModo==2){ float e=uFogDens*vFogZ; f=exp(-e*e); }\n"            // GL_EXP2
"    else f=(uFogEnd-vFogZ)/(uFogEnd-uFogStart);\n"                              // GL_LINEAR
"    col.rgb=mix(uFogColor.rgb,col.rgb,clamp(f,0.0,1.0)); }\n"
"#endif\n"
"#endif\n"
"#ifdef W3D_DESCARTE\n"
"#ifdef W3D_ESPECIAL\n"
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
static GLuint compile(GLenum tipo,const char* defs,const char* src){
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    const char* partes[2] = { defs, src };
    const int n = 2;
#else
    // PC (GL 2.1, -DW3D_PC_GLES2): GLSL 1.20 (gl_PointCoord) en vez del GLSL ES 1.00 implicito
    const char* partes[3] = { "#version 120\n", defs, src };
    const int n = 3;
#endif
    GLuint s=W3D_GLC(glCreateShader(tipo)); W3D_GLC(glShaderSource(s,n,partes,0)); W3D_GLC(glCompileShader(s));
    GLint ok=0; W3D_GLC(glGetShaderiv(s,GL_COMPILE_STATUS,&ok));
    // (al log del editor: en Android y en la web stderr no se ve en ningun lado)
    if(!ok){ char log[1024]; log[0]=0; W3D_GLC(glGetShaderInfoLog(s,1024,0,log)); w3dLogfE("[w3dGLES2] shader compile error: %s",log); }
    return s;
}
// ============================================================================
//  PROGRAMAS. Donde la GPU se banca el programa ENTERO (PC, web, el Android de siempre) hay uno solo, con todo, y cada
//  funcion se prende o apaga con uniforms. Donde no entra (la VideoCore IV de Android 2.3, ver buildProgram) cada draw
//  usa uno armado con lo que necesita: un draw de la UI sin luz ni niebla ni recorte tiene un shader chiquito.
//  La clave de un programa: un bit por funcion + la cantidad de luces (desde el bit PLuces)
// ============================================================================
enum { PLuz=1<<0, PTexGen=1<<1, PSprite=1<<2, PTex=1<<3, PBlur=1<<4, PAlfaAbajo=1<<5, PMezcla=1<<6, PNiebla=1<<7,
       PClip=1<<8, PDescarte=1<<9, PTodo=(1<<10)-1, PLuces=10 };
// Con un programa por draw la clave lleva ademas, desde el bit PEstado, lo que el driver de la VideoCore IV usa para
// traducir el programa: la primitiva, el blending, la mascara de color y como llega cada atributo que el programa lee
// (arreglo o constante, tipo, tamanio). El driver guarda pocas traducciones por programa: usado con mas combinaciones
// en un mismo cuadro, las iba pisando y re-traducia el programa en cada cuadro (~35 ms cada vez). Con la combinacion
// en la clave cada programa tiene una sola y se traduce una vez
typedef unsigned long long Clave;
static const int PEstado = 16;
static const char* kDefFuncion[10] = { "W3D_LUZ", "W3D_TEXGEN", "W3D_SPRITE", "W3D_TEX", "W3D_BLUR", "W3D_ALFA_ABAJO",
                                       "W3D_MEZCLA", "W3D_NIEBLA", "W3D_CLIP", "W3D_DESCARTE" };
// las luces que entran en un programa armado por draw (las demas no iluminan)
static const int kLucesPorDraw = 4;
// donde quedo cada uniform en un programa (-1: ese programa no lo tiene, y glUniform con -1 no hace nada)
#define W3D_UNIFORMS(X) X(uMVP) X(uMV) X(uNMat) X(uTexM) X(uTexGen) X(uUseTex) X(uTex) X(uTex1) X(uEnv) X(uLightOn) \
    X(uFogOn) X(uPointSize) X(uPointSprite) X(uColorMat) X(uBlurY) X(uAlphaRef) X(uAlfaAbajo) X(uMezcla) X(uMezclaK) \
    X(uViewport) X(uClipN) X(uMDiff) X(uMAmb) X(uMSpec) X(uMEmis) X(uShine) X(uAmbGlobal) X(uNLuces) X(uLPos) X(uLAmb) \
    X(uLDif) X(uLSpec) X(uLAtt) X(uLCono) X(uFogColor) X(uFogStart) X(uFogEnd) X(uFogModo) X(uFogDens)
// lo ultimo que se subio a un uniform (n = 0: nada todavia). Una llamada a GL que no cambia nada no se hace: en la
// VideoCore IV de Android 2.3 cada una es un mensaje al chip, y un draw subia ~40 uniforms
struct UCache { unsigned char b[64]; int n; };
struct Programa {
    GLuint id;
    Clave clave;
    unsigned verLuces;   // las luces que tiene subidas (ver SubirLuces)
#define W3D_U(n) GLint n; UCache c_##n;
    W3D_UNIFORMS(W3D_U)
#undef W3D_U
    GLint uClip[5]; UCache c_uClip[5];
};
static const int kProgramasMax = 96;
static Programa programas[kProgramasMax];
static int nProgramas = 0;
static Programa* progUso = 0;   // el del ultimo draw
static GLuint progActivo = 0;   // el que tiene GL (glUseProgram)

static bool porDraw = false;    // un programa por draw (el entero no entra en la GPU)
// los vertices de cada draw como arreglos del cliente (el driver los copia en el draw) y no subidos a un VBO: en la
// misma GPU (la VideoCore IV de Android 2.3) un glBufferData por draw al mismo VBO a veces deja a un draw con los datos
// del siguiente (los rotulos "X" "Y" "Z" de Properties salian "X" "Z" "Z"), y ademas cuesta mas
static bool sinVBO = false;
static unsigned verLuces = 1;   // sube cada vez que cambian las luces
// (W3D_PRUEBA_RELLENO=1, para medir: cada draw y cada limpieza solo en el pixel de abajo a la izquierda. La GPU casi no
// pinta y lo que queda es la geometria, la CPU y el swap: separa lo que cuestan los pixeles)
static bool sinRelleno = false;
static void SoloUnPixel(){ glEnable(GL_SCISSOR_TEST); glScissor(0,0,1,1); }

// los #define de un programa: sus funciones y LUCES = LUZ(0) LUZ(1) ... con sus luces
static void DefsPrograma(Clave clave, char* d, int tam){
    int n = 0;
    for(int b=0;b<PLuces;b++) if(clave&(1ull<<b)) n += snprintf(d+n, tam-n, "#define %s\n", kDefFuncion[b]);
    // UN PROGRAMA POR DRAW: los modos que el programa entero elige con uniforms van como constantes (ver ElegirPrograma).
    // El compilador de la VideoCore IV no saltea las ramas de un if por uniform: hacia las cuentas de todas (la niebla
    // con dos exp por pixel aunque fuera lineal). Medido en el Galaxy Ace: ~13 ms por cuadro de la GPU son los pixeles
    const Clave e = clave >> PEstado;
    if(e){
        n += snprintf(d+n, tam-n, "#define W3D_ESPECIAL\n");
        if((e & 3) == 0) n += snprintf(d+n, tam-n, "#define W3D_PUNTOS\n");
        if(clave & PTex){ n += snprintf(d+n, tam-n, "#define W3D_ENV %d\n", (int)((e >> 31) & 3));
                          if((e >> 33) & 1) n += snprintf(d+n, tam-n, "#define W3D_TEXM\n"); }
        if(clave & PNiebla) n += snprintf(d+n, tam-n, "#define W3D_NIEBLA_MODO %d\n", (int)((e >> 34) & 3));
        if(clave & PMezcla) n += snprintf(d+n, tam-n, "#define W3D_MEZCLA_MODO %d\n", (int)((e >> 36) & 3));
    }
    n += snprintf(d+n, tam-n, "#define LUCES");
    for(int i=0;i<(int)((clave>>PLuces)&15);i++) n += snprintf(d+n, tam-n, " LUZ(%d)", i);
    snprintf(d+n, tam-n, "\n");
}
// el programa, con defs delante de los dos shaders
static GLuint linkear(const char* defs){
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
static void ArmarPrograma(Programa& p, Clave clave){
    char defs[512]; DefsPrograma(clave, defs, (int)sizeof(defs));
    memset(&p, 0, sizeof(p));   // (los caches vacios)
    p.id = linkear(defs); p.clave = clave; p.verLuces = 0;
#define W3D_U(n) p.n=W3D_GLC(glGetUniformLocation(p.id,#n));
    W3D_UNIFORMS(W3D_U)
#undef W3D_U
    p.uClip[0]=W3D_GLC(glGetUniformLocation(p.id,"uClip0")); p.uClip[1]=W3D_GLC(glGetUniformLocation(p.id,"uClip1"));
    p.uClip[2]=W3D_GLC(glGetUniformLocation(p.id,"uClip2")); p.uClip[3]=W3D_GLC(glGetUniformLocation(p.id,"uClip3"));
    p.uClip[4]=W3D_GLC(glGetUniformLocation(p.id,"uClip4"));
}
// lo deja en uso: prog y los u* son los suyos
static void UsarPrograma(Programa& p){
    progUso = &p; prog = p.id;
#define W3D_U(n) n=p.n;
    W3D_UNIFORMS(W3D_U)
#undef W3D_U
    for(int i=0;i<5;i++) uClip[i]=p.uClip[i];
    if(progActivo != prog){ W3D_GLC(glUseProgram(prog)); progActivo = prog; }
}
static void BorrarPrograma(Programa& p){
    if(progActivo == p.id) progActivo = 0;   // (un id nuevo puede salir igual al borrado)
    W3D_GLC(glDeleteProgram(p.id));
}
// subir un uniform del programa en uso solo si cambio (nombre = el global u*, que tiene su c_* en el Programa)
static inline bool UNuevo(UCache& c, const void* v, int n){
    if(c.n == n && memcmp(c.b, v, (size_t)n) == 0) return false;
    memcpy(c.b, v, (size_t)n); c.n = n; return true;
}
#define U_1I(nom, val)  do{ GLint v_ = (GLint)(val);   if(nom>=0 && UNuevo(progUso->c_##nom, &v_, (int)sizeof(v_))) W3D_GLC(glUniform1i(nom, v_)); }while(0)
#define U_1F(nom, val)  do{ GLfloat v_ = (GLfloat)(val); if(nom>=0 && UNuevo(progUso->c_##nom, &v_, (int)sizeof(v_))) W3D_GLC(glUniform1f(nom, v_)); }while(0)
#define U_4F(nom, p)    do{ if(nom>=0 && UNuevo(progUso->c_##nom, (p), 16)) W3D_GLC(glUniform4fv(nom, 1, (p))); }while(0)
#define U_M3(nom, p)    do{ if(nom>=0 && UNuevo(progUso->c_##nom, (p), 36)) W3D_GLC(glUniformMatrix3fv(nom, 1, GL_FALSE, (p))); }while(0)
#define U_M4(nom, p)    do{ if(nom>=0 && UNuevo(progUso->c_##nom, (p), 64)) W3D_GLC(glUniformMatrix4fv(nom, 1, GL_FALSE, (p))); }while(0)
// el programa ENTRA en la GPU? El driver de Broadcom (VideoCore IV, Android 2.3) recien traduce los shaders al primer
// draw, y si no le entran ese draw y todos los que siguen fallan con GL_OUT_OF_MEMORY: la pantalla queda negra.
// Un triangulo de prueba sin escribir color ni profundidad (el atributo 0 en vboP)
static bool entra(GLuint p){
    static const float tri[9] = { -1,-1,0, 1,-1,0, 0,1,0 };
    for(int i=0;i<8 && glGetError()!=GL_NO_ERROR;i++){}
    W3D_GLC(glUseProgram(p)); progActivo = p;
    W3D_GLC(glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE)); W3D_GLC(glDepthMask(GL_FALSE));
    W3D_GLC(glBindBuffer(GL_ARRAY_BUFFER,vboP)); W3D_GLC(glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)sizeof(tri),tri,GL_DYNAMIC_DRAW));
    W3D_GLC(glEnableVertexAttribArray(0)); W3D_GLC(glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,(const void*)0));
    W3D_GLC(glDrawArrays(GL_TRIANGLES,0,3));
    const bool si = glGetError()!=GL_OUT_OF_MEMORY;
    W3D_GLC(glDisableVertexAttribArray(0));
    W3D_GLC(glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE)); W3D_GLC(glDepthMask(GL_TRUE));
    AttrOlvidar();
    return si;
}
// LA COPIA DE LA PANTALLA, DADA VUELTA? GL copia un rectangulo de la pantalla a una textura (glCopyTexSubImage2D: las
// capturas de los juegos) con la fila de ABAJO en la fila 0, y el que la muestra la da vuelta con las UV. En el Galaxy
// Ace (VideoCore IV, Android 2.3) el fundido de los cortes salia patas arriba. Se prueba una vez al armar el backend:
// abajo a la izquierda dos pixeles (el de abajo rojo, el de arriba azul) y arriba dos mas (verde y amarillo), copiados
// los dos de abajo a una textura de 2x2 que se lee con un FBO. Si la fila 0 sale azul, el driver la copio al reves y las
// UV de la captura se dan vuelta (CopiaPantallaDadaVuelta). Si sale amarillo (la de mas arriba), el driver cuenta las
// filas DESDE ARRIBA (el Ace: "fila 0 amarillo, fila 1 verde"): copia al reves y de otro lugar, asi que ademas se le
// pide la fila espejada (CopiaPantallaFila, con el alto de la ventana). En la PC no se prueba (sin FBO sin extensiones)
static bool copiaDadaVuelta = false, copiaDesdeArriba = false;
#if defined(__ANDROID__)
// el alto de la ventana (la superficie de EGL), para la copia de un driver que cuenta las filas desde arriba. Por dlsym:
// el editor y los juegos no linkean libEGL (la abre SDL)
static int AltoVentana(){
    typedef void* (*FnDisplay)();
    typedef void* (*FnSuperficie)(int);
    typedef unsigned (*FnConsultar)(void*, void*, int, int*);
    static FnDisplay display = 0; static FnSuperficie superficie = 0; static FnConsultar consultar = 0;
    static bool buscado = false;
    if(!buscado){
        buscado = true;
        void* lib = dlopen("libEGL.so", RTLD_NOW);
        if(lib){
            display = (FnDisplay)dlsym(lib, "eglGetCurrentDisplay");
            superficie = (FnSuperficie)dlsym(lib, "eglGetCurrentSurface");
            consultar = (FnConsultar)dlsym(lib, "eglQuerySurface");
        }
    }
    int alto = 0;
    if(display && superficie && consultar && consultar(display(), superficie(0x3059 /*EGL_DRAW*/), 0x3056 /*EGL_HEIGHT*/,
                                                       &alto)) return alto;
    return 0;
}
#endif
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
static const char* ColorDe(const unsigned char* p){
    const bool r = p[0] > 128, g = p[1] > 128, b = p[2] > 128;
    return (r && !g && !b) ? "rojo" : (!r && !g && b) ? "azul" : (!r && g && !b) ? "verde" : (r && g && !b) ? "amarillo"
                                                                                                        : "otro";
}
static void ProbarCopiaDePantalla(){
    GLint caja[4], vp[4]; W3D_GLC(glGetIntegerv(GL_SCISSOR_BOX,caja)); W3D_GLC(glGetIntegerv(GL_VIEWPORT,vp));
    const GLboolean recorte = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat fondo[4]; W3D_GLC(glGetFloatv(GL_COLOR_CLEAR_VALUE,fondo));
    const int alto = vp[3] > 4 ? vp[1] + vp[3] : 4;   // (al empezar, el viewport es la ventana)
    static const struct { int y; float r, g, b; } kPintar[4] = {
        {0, 1,0,0}, {1, 0,0,1}, {-2, 0,1,0}, {-1, 1,1,0} };   // (negativos: desde arriba)
    W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER,0));
    W3D_GLC(glEnable(GL_SCISSOR_TEST));
    for(int i=0;i<4;i++){
        W3D_GLC(glScissor(0,kPintar[i].y >= 0 ? kPintar[i].y : alto + kPintar[i].y,1,1));
        W3D_GLC(glClearColor(kPintar[i].r,kPintar[i].g,kPintar[i].b,1)); W3D_GLC(glClear(GL_COLOR_BUFFER_BIT));
    }
    unsigned char lectura[4] = {0,0,0,0};
    W3D_GLC(glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,lectura));
    GLuint t = 0, f = 0;
    W3D_GLC(glGenTextures(1,&t)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,t));
    W3D_GLC(glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,2,2,0,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,NULL));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST));
    W3D_GLC(glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,1,2));
    W3D_GLC(glGenFramebuffers(1,&f)); W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER,f));
    W3D_GLC(glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t,0));
    unsigned char copia[8] = {0,0,0,0, 0,0,0,0};
    const bool fbo = glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    if(fbo) W3D_GLC(glReadPixels(0,0,1,2,GL_RGBA,GL_UNSIGNED_BYTE,copia));
    W3D_GLC(glBindFramebuffer(GL_FRAMEBUFFER,0)); W3D_GLC(glDeleteFramebuffers(1,&f));
    W3D_GLC(glDeleteTextures(1,&t)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex));
    W3D_GLC(glScissor(caja[0],caja[1],caja[2],caja[3]));
    if(!recorte) W3D_GLC(glDisable(GL_SCISSOR_TEST));
    W3D_GLC(glClearColor(fondo[0],fondo[1],fondo[2],fondo[3]));
    for(int i=0;i<8 && glGetError()!=GL_NO_ERROR;i++){}
    if(!fbo){ w3dLogf("[w3dGLES2] la copia de la pantalla: sin FBO para probarla (se supone bien)"); return; }
    const char* c0 = ColorDe(copia); const char* c1 = ColorDe(copia + 4);
    copiaDesdeArriba = strcmp(c0,"amarillo")==0;
    copiaDadaVuelta = copiaDesdeArriba || strcmp(c0,"azul")==0;
    w3dLogf("[w3dGLES2] la copia de la pantalla: fila 0 %s, fila 1 %s (%s); la lectura de abajo: %s", c0, c1,
            copiaDesdeArriba ? "CUENTA LAS FILAS DESDE ARRIBA: se le pide la espejada y las UV se dan vuelta" :
            copiaDadaVuelta ? "DADA VUELTA: las UV de las capturas se dan vuelta" : "bien", ColorDe(lectura));
}
#endif
bool CopiaPantallaDadaVuelta(){ return copiaDadaVuelta; }
int CopiaPantallaFila(int y, int h){
#if defined(__ANDROID__)
    if(copiaDesdeArriba){ const int alto = AltoVentana(); if(alto > 0) return alto - y - h; }
#endif
    (void)h;
    return y;
}

static void buildProgram(){
    W3D_GLC(glGenBuffers(1,&vboP)); W3D_GLC(glGenBuffers(1,&vboN)); W3D_GLC(glGenBuffers(1,&vboT)); W3D_GLC(glGenBuffers(1,&vboC));
    W3D_GLC(glGenBuffers(1,&vboT1)); W3D_GLC(glGenBuffers(1,&ibo));
    // (de nuevo si el contexto GL se perdio y se volvio a armar: lo de antes ya no existe)
    progUso = 0; porDraw = false; sinVBO = false;
    { const char* r = getenv("W3D_PRUEBA_RELLENO"); sinRelleno = r && r[0] == '1'; }
    ArmarPrograma(programas[0], (Clave)(PTodo | (LucesMax << PLuces)));
    nProgramas = 1;
    // (W3D_GLES2_POR_DRAW=1: el camino de la VideoCore IV en cualquier GPU, para probarlo en la PC)
    const char* forzar = getenv("W3D_GLES2_POR_DRAW");
    if(!(forzar && forzar[0] == '1') && entra(programas[0].id)){
        UsarPrograma(programas[0]);
    } else {
        // (y no siempre: con los formatos de los atributos y el blending el driver le agrega instrucciones, y uno que
        // entra en la prueba puede no entrar en un draw)
        w3dLogfW("[w3dGLES2] el programa entero no entra en esta GPU: uno por draw, con hasta %d luces", kLucesPorDraw);
        BorrarPrograma(programas[0]);
        nProgramas = 0;
        porDraw = true;
        sinVBO = true;
    }
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    ProbarCopiaDePantalla();
#endif
    LucesDeFabrica();
    ready=true;
}

// ============================================================================
//  Capacidades (Enable/Disable/IsEnabled)
// ============================================================================
void Enable(Cap c){
    switch(c){
        case DepthTest: if(!(porDraw && cap_depth)){ W3D_GLC(glEnable(GL_DEPTH_TEST)); cap_depth=true; } break;
        case CullFace:  if(!(porDraw && cap_cull)){ W3D_GLC(glEnable(GL_CULL_FACE)); cap_cull=true; } break;
        case Blend:     if(!(porDraw && cap_blend)){ W3D_GLC(glEnable(GL_BLEND)); cap_blend=true; } break;
        case ScissorTest: if(!(porDraw && cap_scissor)){ W3D_GLC(glEnable(GL_SCISSOR_TEST)); cap_scissor=true; } break;
        case Texture2D: cap_tex=true;   break; // el shader decide (uUseTex)
        case Lighting:  cap_light=true; break;
        case Fog:       cap_fog=true;   break;
        case PolygonOffsetFill: if(!(porDraw && cap_offset)){ W3D_GLC(glEnable(GL_POLYGON_OFFSET_FILL)); cap_offset=true; } break; // pick de CARAS: empuja la malla de
                       // oclusion atras para que los triangulos-ID pasen el depth test (sino no se seleccionan)
        case ColorMaterial: cap_colormat=true; break; // el shader usa el vertex color como diffuse/ambient
        case Light0: if(!luces[0].on){ luces[0].on=true; lucesSucias=true; } break;
        case Normalize:
        case PointSprite: case Dither: case Multisample: break; // no aplican / el shader ya normaliza
    }
}
void Disable(Cap c){
    switch(c){
        case DepthTest: if(!(porDraw && !cap_depth)){ W3D_GLC(glDisable(GL_DEPTH_TEST)); cap_depth=false; } break;
        case CullFace:  if(!(porDraw && !cap_cull)){ W3D_GLC(glDisable(GL_CULL_FACE)); cap_cull=false; } break;
        case Blend:     if(!(porDraw && !cap_blend)){ W3D_GLC(glDisable(GL_BLEND)); cap_blend=false; } break;
        case ScissorTest: if(!(porDraw && !cap_scissor)){ W3D_GLC(glDisable(GL_SCISSOR_TEST)); cap_scissor=false; } break;
        case Texture2D: cap_tex=false;   break;
        case Lighting:  cap_light=false; break;
        case Fog:       cap_fog=false;   break;
        case ColorMaterial: cap_colormat=false; break;
        case Light0: if(luces[0].on){ luces[0].on=false; lucesSucias=true; } break;
        case PolygonOffsetFill: if(!(porDraw && !cap_offset)){ W3D_GLC(glDisable(GL_POLYGON_OFFSET_FILL)); cap_offset=false; } break;
        default: break;
    }
}
bool IsEnabled(Cap c){
    switch(c){
        case DepthTest: return cap_depth; case CullFace: return cap_cull; case Blend: return cap_blend;
        case ScissorTest: return cap_scissor; case Texture2D: return cap_tex; case Lighting: return cap_light;
        case Fog: return cap_fog; case ColorMaterial: return cap_colormat; case Light0: return luces[0].on;
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
void ClearColor(float r,float g,float b,float a){ W3D_GLC(glClearColor(r,g,b,a)); }
void Clear(int bits){ GLbitfield m=0; if(bits&ColorBuffer) m|=GL_COLOR_BUFFER_BIT; if(bits&DepthBuffer) m|=GL_DEPTH_BUFFER_BIT;
                      if(bits&StencilBuffer) m|=GL_STENCIL_BUFFER_BIT; if(sinRelleno) SoloUnPixel(); W3D_GLC(glClear(m)); }

// ---- planos de recorte: el pipeline fijo los hace en hardware (GL_CLIP_PLANEi);
//      aca es un discard en el shader. La ecuacion llega en el espacio de la
//      MODELVIEW ACTUAL (misma regla que glClipPlane) y se pasa a OJO con
//      (MV^-1)^T, que es lo que hace el pipeline fijo por dentro.
int PlanosRecorteMax(){ return 5; }
void PlanosRecorte(int n, const float* eq4){
    if(n<0) n=0; if(n>5) n=5;
    if(n>0){
        Matrix4 inv; if(!Matrix4::InvertirAfin(curMV, inv)) { n=0; }
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
int EstencilBits(){ GLint b=0; W3D_GLC(glGetIntegerv(GL_STENCIL_BITS,&b)); return b>0?(int)b:0; }
void EstencilLimpiar(int v){ W3D_GLC(glClearStencil(v)); W3D_GLC(glStencilMask(0xFFu)); W3D_GLC(glClear(GL_STENCIL_BUFFER_BIT)); }
void EstencilMarcar(int v){ W3D_GLC(glEnable(GL_STENCIL_TEST)); W3D_GLC(glStencilMask(0xFFu)); W3D_GLC(glStencilFunc(GL_ALWAYS,v,0xFFu)); W3D_GLC(glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE)); }
void EstencilDentro(int v){ W3D_GLC(glEnable(GL_STENCIL_TEST)); W3D_GLC(glStencilMask(0x00u)); W3D_GLC(glStencilFunc(GL_EQUAL,v,0xFFu)); W3D_GLC(glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP)); }
void EstencilApagar(){ W3D_GLC(glStencilMask(0xFFu)); W3D_GLC(glDisable(GL_STENCIL_TEST)); }
// LO QUE GL YA TIENE (con un programa por draw): no se vuelve a pedir. Ademas de la llamada, cada una dibujaria el lote
// juntado (ver LoteVaciar) y la UI repite el mismo scissor, blending o textura en cada widget. GLES2Init los olvida
static GLint scissorGL[4] = {-1,-1,-1,-1}, viewportGL[4] = {-1,-1,-1,-1};
static int depthMaskGL = -1, depthFuncGL = -1, frontFaceGL = -1, blendGL = -1, colorMaskGL = -1;
static float lineWidthGL = -1.0f;
struct TexParamGL { GLint minF, magF, wrap; };
static std::map<unsigned, TexParamGL> texParamGL;
static void EstadoGLOlvidar(){
    for(int i=0;i<4;i++){ scissorGL[i] = -1; viewportGL[i] = -1; }
    depthMaskGL = depthFuncGL = frontFaceGL = blendGL = colorMaskGL = -1; lineWidthGL = -1.0f;
    texParamGL.clear();
}
static inline bool MismoRect(const GLint* r, int x, int y, int w, int h){ return r[0]==x && r[1]==y && r[2]==w && r[3]==h; }
void Viewport(int x,int y,int w,int h){ if(w<0)w=0; if(h<0)h=0;
    if(!(porDraw && MismoRect(viewportGL,x,y,w,h))){ W3D_GLC(glViewport(x,y,w,h)); viewportGL[0]=x; viewportGL[1]=y; viewportGL[2]=w; viewportGL[3]=h; }
    vpActual[0]=(float)x; vpActual[1]=(float)y; vpActual[2]=(float)w; vpActual[3]=(float)h; }
// clamp de w/h a >=0: un ancho/alto negativo (viewport muy angosto - iconos/margenes en el outliner)
// es GL_INVALID_VALUE en WebGL y deja el scissor en mal estado. Clampeando no rompe el clipping.
void Scissor(int x,int y,int w,int h){ if(w<0)w=0; if(h<0)h=0;
    if(porDraw && MismoRect(scissorGL,x,y,w,h)) return;
    W3D_GLC(glScissor(x,y,w,h)); scissorGL[0]=x; scissorGL[1]=y; scissorGL[2]=w; scissorGL[3]=h; }
void GetViewport(int v[4]){ GLint g[4]; W3D_GLC(glGetIntegerv(GL_VIEWPORT,g)); for(int i=0;i<4;i++) v[i]=(int)g[i]; }
void GetScissor(int v[4]){ GLint g[4]; W3D_GLC(glGetIntegerv(GL_SCISSOR_BOX,g)); for(int i=0;i<4;i++) v[i]=(int)g[i]; }
bool ScissorActivo(){ return W3D_GLC(glIsEnabled(GL_SCISSOR_TEST))==GL_TRUE; }
void DepthFunc(DepthCmp c){ const GLenum f = c==DepthLEqual?GL_LEQUAL:(c==DepthEqual?GL_EQUAL:(c==DepthAlways?GL_ALWAYS:GL_LESS));
    if(porDraw && depthFuncGL==(int)f) return; W3D_GLC(glDepthFunc(f)); depthFuncGL=(int)f; }
void DepthMask(bool w){ if(porDraw && depthMaskGL==(w?1:0)) return; W3D_GLC(glDepthMask(w?GL_TRUE:GL_FALSE)); depthMaskGL=w?1:0; }
static unsigned mascaraColor = 15;   // (para la clave del programa, ver ElegirPrograma)
void ColorMask(bool r,bool g,bool b,bool a){
    const unsigned m = (r?1u:0u)|(g?2u:0u)|(b?4u:0u)|(a?8u:0u);
    if(porDraw && colorMaskGL==(int)m){ mascaraColor = m; return; }
    W3D_GLC(glColorMask(r,g,b,a)); mascaraColor = m; colorMaskGL=(int)m; }
void LineWidth(float px){ if(porDraw && lineWidthGL==px) return; W3D_GLC(glLineWidth(px)); lineWidthGL=px; }
void FrontFace(bool ccw){ if(porDraw && frontFaceGL==(ccw?1:0)) return; W3D_GLC(glFrontFace(ccw?GL_CCW:GL_CW)); frontFaceGL=ccw?1:0; }
void CullCarasFrontales(bool on){ W3D_GLC(glCullFace(on?GL_FRONT:GL_BACK)); }
void PolygonOffset(float factor,float units){ W3D_GLC(glPolygonOffset(factor,units)); }
static void AjustarProyeccion(float sesgo, float n, float f);
void DepthRange(float n,float f){ AjustarProyeccion(-1e30f, n, f); }   // (en la proyeccion: ver AjustarProyeccion)
void ReadPixelsBegin(int,int,int,int){}   // (lee el framebuffer de verdad)
void ReadPixelsRGBA(int x,int y,int w,int h,unsigned char* px){ W3D_GLC(glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px)); }
void BlurY(float amt){ blurY = (amt > 0.0f) ? amt : 0.0f; }   // blur vertical por shader
void AlphaTest(float ref){ alphaRef = (ref > 0.0f) ? ref : 0.0f; }  // descarte por alpha
void AlfaEmpaquetado(bool si){ alfaAbajo = si ? 1 : 0; }   // alpha en la mitad de abajo
// la ECUACION tambien (mismo porque que el backend fijo, ver w3dGraphics.cpp:
// tras un pase SUSTRACTIVO, reponer solo la func dejaba todo lo alpha-blended
// RESTANDO del framebuffer: silueta negra con el alpha bien).
// el modo de blending puesto, como numero chico (para la clave del programa, ver ElegirPrograma)
static unsigned modoBlend = 0;
enum { BAlpha, BOff, BAdd, BAddAlpha, BMultiplicar, BScreen, BPremult, BRestar };
// (con un programa por draw: el modo que GL ya tiene no se vuelve a poner)
static bool BlendYaPuesto(unsigned m){
    if(porDraw && blendGL==(int)m){ modoBlend = m; return true; }
    LoteVaciar();   // (lo juntado, con el modo que tenia)
    modoBlend = m; blendGL = (int)m; return false; }
void BlendAlpha(){ if(BlendYaPuesto(BAlpha)) return; W3D_GLC(glBlendEquation(GL_FUNC_ADD)); W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); }
void BlendMode(int modo){
    if(BlendYaPuesto(modo==1 ? BMultiplicar : modo==2 ? BAdd : BAlpha)) return;
    W3D_GLC(glBlendEquation(GL_FUNC_ADD));                                // capas multi-pass: siempre ADD
    if(modo==1)      W3D_GLC(glBlendFunc(GL_DST_COLOR, GL_ZERO));         // Multiply
    else if(modo==2) W3D_GLC(glBlendFunc(GL_ONE, GL_ONE));               // Add
    else             W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); // Mix
}
// modos de mezcla nombrados (ver enum Mezcla). GLES2/WebGL: glBlendEquation es nativo.
void SetMezcla(int m){
    if(BlendYaPuesto(m==MezclaOff ? BOff : m==MezclaAdd ? BAdd : m==MezclaAddAlpha ? BAddAlpha : m==MezclaMultiply ? BMultiplicar :
                     m==MezclaScreen ? BScreen : m==MezclaPremult ? BPremult : m==MezclaSubtract ? BRestar : BAlpha)) return;
    W3D_GLC(glBlendEquation(GL_FUNC_ADD)); // reset (por si venia de Substract)
    switch(m){
        case MezclaOff:      W3D_GLC(glBlendFunc(GL_ONE, GL_ZERO)); break;
        case MezclaAdd:      W3D_GLC(glBlendFunc(GL_ONE, GL_ONE)); break;
        case MezclaAddAlpha: W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE)); break;
        case MezclaMultiply: W3D_GLC(glBlendFunc(GL_DST_COLOR, GL_ZERO)); break;
        case MezclaScreen:   W3D_GLC(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR)); break;
        case MezclaPremult:  W3D_GLC(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)); break;
        case MezclaSubtract: W3D_GLC(glBlendEquation(GL_FUNC_REVERSE_SUBTRACT)); W3D_GLC(glBlendFunc(GL_ONE, GL_ONE)); break;
        case MezclaAlpha:
        default:             W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); break;
    }
}
void SmoothShading(bool){}   // ES2 siempre interpola (smooth); FLAT seria un shader aparte -> TODO
void FastPerspective(){}     // ES2 corrige perspectiva siempre
void Invalidate(){ texGenCache = -1; matcapCache = -1; }   // (solo los caches de la matriz de textura, como el fijo)
// LA PROYECCION AJUSTADA: el sesgo en metros (SesgoMetros) y el RANGO DE PROFUNDIDAD (DepthRange) se ponen en la matriz
// de proyeccion, sobre la del pase (la que estaba puesta mientras los dos eran neutros; EstadoBase los vuelve a neutros en
// cada cuadro). El rango no va con glDepthRangef: el driver de la VideoCore IV (Galaxy Ace, Android 2.3) dejaba mal la
// profundidad de lo que se dibujaba despues de un rango (1,1) -el cielo de Daytona- y el asfalto translucido tapaba la
// parte de abajo de los autos. En la matriz: z' = (f - n) z + (f + n - 1) w (y el lejano un pelo antes del plano de
// corte: con z = w justo, el redondeo lo recortaba)
static float gAjBase[16];
static float gAjSesgo = 0.0f, gAjN = 0.0f, gAjF = 1.0f;
static void AjustarProyeccion(float sesgo, float n, float f) {
    if (sesgo == -1e30f) sesgo = gAjSesgo;   // (DepthRange: el sesgo queda)
    if (n < 0.0f) n = 0.0f; if (f > 1.0f) f = 1.0f;
    if (sesgo == gAjSesgo && n == gAjN && f == gAjF) return;
    if (gAjSesgo == 0.0f && gAjN == 0.0f && gAjF == 1.0f) GetMatrix(Projection, gAjBase);
    float P[16]; for (int i = 0; i < 16; i++) P[i] = gAjBase[i];
    P[14] -= P[10] * sesgo;
    if (!(n == 0.0f && f == 1.0f)) {
        const float a = f - n, b = (f + n - 1.0f) * 0.99999f;
        for (int c = 0; c < 4; c++) P[c * 4 + 2] = a * P[c * 4 + 2] + b * P[c * 4 + 3];   // (fila z; la w: c*4+3)
    }
    MatrixMode(Projection); LoadMatrix(P); MatrixMode(ModelView);
    gAjSesgo = sesgo; gAjN = n; gAjF = f;
}
void SesgoMetros(float m){ AjustarProyeccion(m, gAjN, gAjF); }
void EstadoBase(){ AjustarProyeccion(0.0f, 0.0f, 1.0f); AlphaTest(0.0f); Disable(PolygonOffsetFill); Disable(Lighting); Disable(ColorMaterial); }
bool EstadoFotoSoportada(){ return false; }   // sin cache: el constructor de las rutinas corre en cada cuadro
void EstadoFotoTomar(){}
void EstadoFotoReponer(){}

// ============================================================================
//  Luz / material
// ============================================================================
// ---- luces: se guardan como las guarda GL (posicion y direccion del cono ya en OJO, con la modelview de AHORA)
static void EnOjoPunto(const float* v, float* out){   // posicion: w 0 = direccional (solo rota), 1 = puntual
    const float* m=curMV.m;
    for(int k=0;k<3;k++) out[k]=m[k]*v[0]+m[4+k]*v[1]+m[8+k]*v[2]+m[12+k]*v[3];
    out[3]=v[3];
}
void LightNfv(int i,LightFv p,const float* v){
    if(i<0 || i>=LucesMax) return;
    Luz& L=luces[i];
    if(p==LightPosition)     EnOjoPunto(v,L.pos);
    else if(p==LightDiffuse)  for(int k=0;k<4;k++) L.dif[k]=v[k];
    else if(p==LightAmbient)  for(int k=0;k<4;k++) L.amb[k]=v[k];
    else if(p==LightSpecular) for(int k=0;k<4;k++) L.spec[k]=v[k];
    lucesSucias=true;
}
void LightNf(int i,LightF p,float v){
    if(i<0 || i>=LucesMax) return;
    luces[i].att[p==LightConstantAtt ? 0 : (p==LightLinearAtt ? 1 : 2)]=v;
    lucesSucias=true;
}
void LightNSpot(int i,float corte,float exponente,const float* dir3){
    if(i<0 || i>=LucesMax) return;
    Luz& L=luces[i];
    const float* m=curMV.m;   // la direccion solo rota
    for(int k=0;k<3;k++) L.dir[k]=m[k]*dir3[0]+m[4+k]*dir3[1]+m[8+k]*dir3[2];
    L.corteCos=(corte>=180.0f) ? -1.0f : cosf(corte*0.01745329252f);
    L.expo=exponente;
    lucesSucias=true;
}
void LightN(int i,bool on){
    if(i<0 || i>=LucesMax || luces[i].on==on) return;
    luces[i].on=on; lucesSucias=true;
}
void Light0fv(LightFv p,const float* v){ LightNfv(0,p,v); }
void Light0f(LightF p,float v){ LightNf(0,p,v); }
void SetLightEnabled(unsigned int id,bool on){
#ifdef GL_LIGHT0
    const unsigned int l0=(unsigned int)GL_LIGHT0;
#else
    const unsigned int l0=0x4000u;
#endif
    if(id>=l0 && id<l0+(unsigned int)LucesMax) LightN((int)(id-l0),on);
}
void LightModelAmbient(const float* c){ for(int k=0;k<4;k++) ambGlobal[k]=c[k]; }
void Material(MatParam p,const float* rgba){
    if(p==MatDiffuse)       for(int i=0;i<4;i++) matDiff[i]=rgba[i];
    else if(p==MatAmbient)  for(int i=0;i<4;i++) matAmb[i]=rgba[i];
    else if(p==MatSpecular) for(int i=0;i<4;i++) matSpec[i]=rgba[i];
    else if(p==MatEmission) for(int i=0;i<4;i++) matEmis[i]=rgba[i];
}
void MaterialShininess(float s){ matShine=s; } // exponente specular (0..128)

// ============================================================================
//  Textura / texenv
// ============================================================================
void BindTexture(unsigned int id){ if(porDraw && id==boundTex) return; boundTex=id; W3D_GLC(glBindTexture(GL_TEXTURE_2D,id)); }
// el min-filter con la PIRAMIDE si la textura la subio y los mipmaps estan prendidos (como el backend fijo): sin esto lo
// lejano se veia con serrucho y titilaba en web/Android y no en el PC
static GLint FiltroMin(unsigned int id, bool linear){
    if(MipmapsGlobal() && TexTieneMips(id)) return linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    return linear ? GL_LINEAR : GL_NEAREST;
}
void TexFilter(bool linear){
    const GLint mn = FiltroMin(boundTex,linear), mg = linear?GL_LINEAR:GL_NEAREST;
    if(porDraw){ std::map<unsigned,TexParamGL>::iterator it = texParamGL.find(boundTex);
        if(it != texParamGL.end() && it->second.minF==mn && it->second.magF==mg) return;
        TexParamGL& t = texParamGL[boundTex]; if(it == texParamGL.end()) t.wrap = -1; t.minF = mn; t.magF = mg; }
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,mn)); W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,mg)); }
void TexWrap(bool repeat){
    const GLint w = repeat?GL_REPEAT:GL_CLAMP_TO_EDGE;
    if(porDraw){ std::map<unsigned,TexParamGL>::iterator it = texParamGL.find(boundTex);
        if(it != texParamGL.end() && it->second.wrap==w) return;
        TexParamGL& t = texParamGL[boundTex]; if(it == texParamGL.end()){ t.minF = -1; t.magF = -1; } t.wrap = w; }
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,w)); W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,w)); }
// el combiner de la unidad 0, como glTexEnv: apagar cualquiera de los modos vuelve a GL_MODULATE
void TexEnvDot3(bool on){ texEnv = on ? 2 : 0; }
void TexEnvReplace(bool on){ texEnv = on ? 1 : 0; }
void TexEnvAlphaOnly(bool on){ texEnv = on ? 3 : 0; }
// MEZCLA DE TEXTURAS: la unidad 1 del pipeline fijo (lightmaps, la textura B del material, la rampa del toon). En el
// shader es un segundo sampler sobre lo que salio de la unidad 0: interpolar con k, multiplicar (x1 / x2) o restar.
// Los parametros de filtro y repeticion son del OBJETO textura B (igual que en el backend fijo).
bool MezclaTexturasSoportada(){ return true; }
void MezclaTexturas(unsigned int idB, float k, bool filtrado, bool repetir, int modo){
    if(!idB){ MezclaTexturasOff(); return; }
    if(k<0.0f) k=0.0f; if(k>1.0f) k=1.0f;
    if(modo<0 || modo>4) modo=0;
    if(modo==4) modo=1;   // (multiplicar simple = multiplicar: en el shader es lo mismo)
    mezclaOn=true; mezclaTex=idB; mezclaK=k; mezclaModo=modo;
    texParamGL.erase(idB);   // (sus parametros se ponen aca, sin el cache)
    W3D_GLC(glActiveTexture(GL_TEXTURE1));
    W3D_GLC(glBindTexture(GL_TEXTURE_2D,idB));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,FiltroMin(idB,filtrado)));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,filtrado?GL_LINEAR:GL_NEAREST));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,repetir?GL_REPEAT:GL_CLAMP_TO_EDGE));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,repetir?GL_REPEAT:GL_CLAMP_TO_EDGE));
    W3D_GLC(glActiveTexture(GL_TEXTURE0));
}
void MezclaTexturasOff(){ mezclaOn=false; mezclaUV=0; mezclaUVVbo=0; }
bool MezclaTexturasActiva(){ return mezclaOn; }
void MezclaTexturasUV(const float* p){ mezclaUV=p; if(p) mezclaUVVbo=0; }
void MezclaTexturasUVVBO(unsigned int h){ mezclaUVVbo=h; if(h) mezclaUV=0; }
void MezclaTexturaBorrada(unsigned int id){ if(id && id==mezclaTex){ mezclaTex=0; mezclaOn=false; } texParamGL.erase(id); }
unsigned int ErrorGL(){ return (unsigned int)glGetError(); }
// SPHERE-MAP (GL_SPHERE_MAP): las UV salen del reflejo de la vista en la normal (en el vertex shader). Como el
// backend fijo, la matriz de textura da vuelta la V para que el reflejo no salga cabeza abajo.
void TexGenSphere(bool on){
    if(texGenCache == (on ? 1 : 0)) return;
    texGenCache = on ? 1 : 0;
    matcapCache = -1;
    texGenEsfera=on;
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
bool TieneTexGen(){ return true; }   // el sphere-map y el matcap van por el shader (como el texgen de GL)

// ============================================================================
//  Color uniforme (cuando NO hay array de color)
// ============================================================================
void Color4f(float r,float g,float b,float a){ curColor[0]=r; curColor[1]=g; curColor[2]=b; curColor[3]=a; }
void Color4fv(const float* c){ for(int i=0;i<4;i++) curColor[i]=c[i]; }
void Color4ub(unsigned char r,unsigned char g,unsigned char b,unsigned char a){ curColor[0]=r/255.0f; curColor[1]=g/255.0f; curColor[2]=b/255.0f; curColor[3]=a/255.0f; }

// ============================================================================
//  Fog
// ============================================================================
void FogMode(bool lineal){ fogModo = lineal ? 0 : 1; }
void FogModo(int m){ fogModo = (m < 0 || m > 2) ? 0 : m; }
void FogDensidad(float d){ fogDens = d; }
void FogStart(float z){ fogStart=z; }
void FogEnd(float z){ fogEnd=z; }
void FogColor(const float* rgba){ for(int i=0;i<4;i++) fogColor[i]=rgba[i]; }
void PointSize(float px){ pointSize=px; } // gl_PointSize en el VS (ES2 no tiene glPointSize)

// ============================================================================
//  Vertex arrays -> se suben a VBOs en el draw
// ============================================================================
void EnableArray(VArray a){ if(a==VertexArray)aPos.on=true; else if(a==NormalArray)aNrm.on=true; else if(a==TexCoordArray)aUV.on=true; else if(a==ColorArray)aCol.on=true; }
void DisableArray(VArray a){ if(a==VertexArray)aPos.on=false; else if(a==NormalArray)aNrm.on=false; else if(a==TexCoordArray)aUV.on=false; else if(a==ColorArray)aCol.on=false; }
int ArrayEstado(VArray a){ return (a==VertexArray ? aPos.on : a==NormalArray ? aNrm.on : a==TexCoordArray ? aUV.on : aCol.on) ? 1 : 0; }
void VertexPointer2f(int stride,const float* p){ aPos.size=2; aPos.type=GL_FLOAT; aPos.stride=stride; aPos.ptr=p; aPos.vbo=0; }
void VertexPointer3f(int stride,const float* p){ aPos.size=3; aPos.type=GL_FLOAT; aPos.stride=stride; aPos.ptr=p; aPos.vbo=0; }
void VertexPointer3s(int stride,const short* p){ aPos.size=3; aPos.type=GL_SHORT; aPos.stride=stride; aPos.ptr=p; aPos.vbo=0; }
void VertexPointer2s(int stride,const short* p){ aPos.size=2; aPos.type=GL_SHORT; aPos.stride=stride; aPos.ptr=p; aPos.vbo=0; }
void NormalPointer3b(const signed char* p){ aNrm.size=3; aNrm.type=GL_BYTE; aNrm.stride=0; aNrm.ptr=p; aNrm.vbo=0; }
void ColorPointer4ub(const unsigned char* p){ aCol.size=4; aCol.type=GL_UNSIGNED_BYTE; aCol.stride=0; aCol.ptr=p; aCol.vbo=0; }
void ColorPointer4f(int stride,const float* p){ aCol.size=4; aCol.type=GL_FLOAT; aCol.stride=stride; aCol.ptr=p; aCol.vbo=0; }
void TexCoordPointer2f(int stride,const float* p){ aUV.size=2; aUV.type=GL_FLOAT; aUV.stride=stride; aUV.ptr=p; aUV.vbo=0; }
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
        BindArray(sinVBO ? 0 : vbo);
        if(!sinVBO) W3D_GLC(glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)bytes,ar.ptr,GL_DYNAMIC_DRAW));
        // normal (byte -127..127 -> -1..1) y color (ubyte 0..255 -> 0..1) SI se normalizan.
        // la POSICION NO: un short es una coord ENTERA de pantalla/mundo (ej. 0..800), NO un [-1,1]
        // -> normalizarla la mandaba a ~0 (el borde del viewport / sprites 2D quedaban invisibles).
        GLboolean norm = (allowNorm && ar.type!=GL_FLOAT)?GL_TRUE:GL_FALSE;
        if(g.on != 1){ W3D_GLC(glEnableVertexAttribArray(loc)); g.on = 1; }
        // (con arreglos del cliente el driver lee los datos en el draw: el mismo puntero con otro contenido no hace
        // falta volver a darlo. Con el VBO se da siempre)
        if(!sinVBO || g.vbo!=0 || g.ptr!=ar.ptr || g.size!=ar.size || g.type!=ar.type || g.norm!=norm || g.stride!=ar.stride){
            W3D_GLC(glVertexAttribPointer(loc,ar.size,ar.type,norm,ar.stride,sinVBO ? ar.ptr : (const void*)0));
            g.vbo = sinVBO ? 0 : vbo; g.ptr = ar.ptr; g.size = ar.size; g.type = ar.type; g.norm = norm; g.stride = ar.stride;
        }
    } else {
        if(g.on != 0){ W3D_GLC(glDisableVertexAttribArray(loc)); g.on = 0; }
        if(!g.hayCte || memcmp(g.cte, def, sizeof(g.cte)) != 0){
            W3D_GLC(glVertexAttrib4fv(loc,def)); // constante (normal +Z, color = curColor)
            memcpy(g.cte, def, sizeof(g.cte)); g.hayCte = true;
        }
    }
}
// la matriz de las NORMALES: la inversa transpuesta de la modelview (3x3), como el pipeline fijo. Con una escala
// despareja la modelview sola tuerce las normales (y la luz). Columnas = productos cruz de las columnas / determinante.
static void MatrizNormal(float out[9]){
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
static void Prim(GLenum modo){ primDraw = (modo==GL_POINTS) ? 0 : (modo==GL_LINES || modo==GL_LINE_STRIP || modo==GL_LINE_LOOP) ? 1 : 2; }
// como llega un atributo: 0 = constante; si no, el tipo (1 float, 2 short, 3 byte, 4 ubyte) y el tamanio
static unsigned FirmaAttr(const Arr& a, int nV){
    if(!(a.on && (a.vbo || (a.ptr && nV>0)))) return 0;
    const unsigned t = (a.type==GL_FLOAT) ? 1u : (a.type==GL_SHORT) ? 2u : (a.type==GL_BYTE) ? 3u : 4u;
    return t | ((unsigned)(a.size-1) << 3);
}
// el programa de este draw, si hay uno por draw: el que tiene justo lo que se va a usar
static void ElegirPrograma(int nV){
    if(!porDraw) return;
    const bool luz = cap_light && !(cap_tex && texEnv==1);   // (como uLightOn, ver setupState)
    const bool mezcla = mezclaOn && mezclaTex;
    int nl = 0;
    if(luz) for(int i=0;i<LucesMax;i++) if(luces[i].on) nl++;
    if(nl > kLucesPorDraw) nl = kLucesPorDraw;
    const unsigned f = (luz ? PLuz : 0) | ((texGenEsfera && cap_tex) ? PTexGen : 0) | (pointSprite ? PSprite : 0)
                     | (cap_tex ? PTex : 0) | ((cap_tex && blurY > 0.0f) ? PBlur : 0) | ((cap_tex && alfaAbajo) ? PAlfaAbajo : 0)
                     | (mezcla ? PMezcla : 0) | (cap_fog ? PNiebla : 0) | (clipN > 0 ? PClip : 0)
                     | (alphaRef > 0.0f ? PDescarte : 0) | ((unsigned)nl << PLuces);
    // lo del driver (ver PEstado): solo los atributos que este programa lee
    const bool uv1 = mezcla && (mezclaUV || mezclaUVVbo || (aUV.on && aUV.type==GL_FLOAT && (aUV.vbo || (aUV.ptr && nV>0))));
    Clave e = (Clave)primDraw | ((Clave)(cap_blend ? 1u + modoBlend : 0u) << 2) | ((Clave)mascaraColor << 6)
            | ((Clave)FirmaAttr(aPos, nV) << 10) | ((Clave)FirmaAttr(aCol, nV) << 15);
    if(f & (PLuz|PTexGen)) e |= (Clave)FirmaAttr(aNrm, nV) << 20;
    if(f & PTex)           e |= (Clave)FirmaAttr(aUV, nV) << 25;
    if(uv1)                e |= (Clave)1 << 30;
    // (los modos, constantes del programa: ver DefsPrograma)
    if(f & PTex){ static const float kIdent[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                  e |= (Clave)(texEnv & 3) << 31;
                  if(memcmp(curTex.m, kIdent, sizeof(kIdent)) != 0) e |= (Clave)1 << 33; }
    if(f & PNiebla)        e |= (Clave)(fogModo & 3) << 34;
    if(f & PMezcla)        e |= (Clave)(mezclaModo & 3) << 36;
    const Clave clave = (Clave)f | (e << PEstado);
    if(progUso && progUso->clave == clave) return;
    for(int i=0;i<nProgramas;i++) if(programas[i].clave == clave){ UsarPrograma(programas[i]); return; }
    if(nProgramas == kProgramasMax){   // (lleno: se tiran todos y se vuelven a armar los que hagan falta)
        for(int i=0;i<nProgramas;i++) BorrarPrograma(programas[i]);
        nProgramas = 0;
    }
    ArmarPrograma(programas[nProgramas], clave);
    UsarPrograma(programas[nProgramas++]);
    // (cada uno nuevo le cuesta al driver ~35 ms la primera vez que dibuja: el log dice cuantos hay y cuando aparecen)
    w3dLogf("[w3dGLES2] programa %d (clave %08x%08x)", nProgramas, (unsigned)(clave >> 32), (unsigned)clave);
}
// las luces prendidas, juntas al principio de los arreglos del shader (solo cuando cambiaron o el programa todavia no
// las tiene: los uniforms quedan en cada programa)
static void SubirLuces(){
    if(lucesSucias){ verLuces++; lucesSucias=false; }
    if(progUso->verLuces == verLuces || uNLuces < 0) return;
    float pos[LucesMax*4], amb[LucesMax*4], dif[LucesMax*4], spec[LucesMax*4], att[LucesMax*4], cono[LucesMax*4];
    int n=0;
    for(int i=0;i<LucesMax;i++){
        const Luz& L=luces[i];
        if(!L.on) continue;
        for(int k=0;k<4;k++){ pos[n*4+k]=L.pos[k]; amb[n*4+k]=L.amb[k]; dif[n*4+k]=L.dif[k]; spec[n*4+k]=L.spec[k]; }
        att[n*4]=L.att[0]; att[n*4+1]=L.att[1]; att[n*4+2]=L.att[2]; att[n*4+3]=L.expo;
        cono[n*4]=L.dir[0]; cono[n*4+1]=L.dir[1]; cono[n*4+2]=L.dir[2]; cono[n*4+3]=L.corteCos;
        n++;
    }
    W3D_GLC(glUniform1i(uNLuces,n));
    if(n>0){
        W3D_GLC(glUniform4fv(uLPos,n,pos)); W3D_GLC(glUniform4fv(uLAmb,n,amb)); W3D_GLC(glUniform4fv(uLDif,n,dif));
        W3D_GLC(glUniform4fv(uLSpec,n,spec)); W3D_GLC(glUniform4fv(uLAtt,n,att)); W3D_GLC(glUniform4fv(uLCono,n,cono));
    }
    progUso->verLuces=verLuces;
}
static void setupState(int nV){
    LoteVaciar();
    gUltimoVerts = nV;   // (lo juntado sale ANTES de elegir nada para este draw: vaciarlo a mitad de camino pisaria el programa)
    Matrix4 mvp = curProj * curMV;
    float nmat[9]; MatrizNormal(nmat);
    ElegirPrograma(nV);
    if(progActivo != prog){ W3D_GLC(glUseProgram(prog)); progActivo = prog; }
    if(sinRelleno) SoloUnPixel();
    if(uClipN>=0){
        U_1I(uClipN, clipN);
        for(int i=0;i<clipN;i++) if(uClip[i]>=0 && UNuevo(progUso->c_uClip[i], clipEq[i], 16)) W3D_GLC(glUniform4fv(uClip[i],1,clipEq[i]));
    }
    U_M4(uMVP, mvp.m);
    U_M4(uMV, curMV.m);
    U_M3(uNMat, nmat);
    U_M4(uTexM, curTex.m);
    U_1I(uTexGen, texGenEsfera?1:0);
    U_1I(uUseTex, cap_tex?1:0);
    U_1I(uEnv, texEnv);
    // la luz no hace falta si la textura REEMPLAZA el color (el resultado es el mismo y ahorra el vertex shader)
    U_1I(uLightOn, (cap_light && !(cap_tex && texEnv==1))?1:0);
    U_1I(uColorMat, cap_colormat?1:0);
    U_1F(uBlurY, blurY);
    U_1F(uAlphaRef, alphaRef);
    U_1I(uAlfaAbajo, alfaAbajo);
    U_1I(uFogOn, cap_fog?1:0);
    U_1I(uTex, 0);
    U_1F(uPointSize, pointSize);
    U_1I(uPointSprite, pointSprite?1:0);
    U_4F(uViewport, vpActual);
    U_4F(uMDiff, matDiff); U_4F(uMAmb, matAmb);
    U_4F(uMSpec, matSpec); U_4F(uMEmis, matEmis); U_1F(uShine, matShine);
    U_4F(uAmbGlobal, ambGlobal);
    SubirLuces();
    U_4F(uFogColor, fogColor); U_1F(uFogStart, fogStart); U_1F(uFogEnd, fogEnd);
    U_1I(uFogModo, fogModo); U_1F(uFogDens, fogDens);
    // la unidad 1: la textura B en el sampler 1. Apagada, el sampler apunta a la unidad 0 (sin textura en la 1 el
    // navegador avisa en cada draw)
    const bool mezcla = mezclaOn && mezclaTex;
    U_1I(uMezcla, mezcla ? mezclaModo : -1);
    U_1I(uTex1, mezcla ? 1 : 0);
    if(mezcla){
        U_1F(uMezclaK, mezclaK);
        W3D_GLC(glActiveTexture(GL_TEXTURE1)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,mezclaTex));
        W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex));
    }
    // (sin la mezcla la unidad activa es siempre la 0 y su textura la deja BindTexture: no hace falta repetirla)
    static const float defNrm[4]={0,0,1,0};
    static const float defUV[4]={0,0,0,1};   // la texcoord de fabrica de GL
    // (con un programa por draw, los atributos que el programa no lee van apagados: si no, cambian la traduccion del
    // driver sin hacer falta, ver PEstado)
    static const Arr apagado={false,3,GL_FLOAT,0,0};
    const bool leeNrm = !porDraw || (progUso->clave & (PLuz|PTexGen));
    const bool leeUV  = !porDraw || (progUso->clave & PTex);
    bindAttr(aLpos,aPos,vboP,nV, defNrm,  false); // POSICION: NUNCA normalizar (short = coords enteras de pantalla/mundo)
    bindAttr(aLnrm,leeNrm ? aNrm : apagado,vboN,nV, defNrm,  true);  // normal: byte -> [-1,1]
    bindAttr(aLuv, leeUV ? aUV : apagado, vboT,nV, defUV,   false); // uv: float, o las normales en bytes del matcap (GL no las normaliza)
    bindAttr(aLcol,aCol,vboC,nV, curColor,true);  // color: ubyte -> [0,1]. sin array -> color uniforme (Color4*)
    if(aLuv1>=0){
        if(mezcla && (mezclaUV || mezclaUVVbo)){   // UV propias de la unidad 1
            Arr uv1={true,2,GL_FLOAT,0,mezclaUV,mezclaUVVbo};
            bindAttr(aLuv1,uv1,vboT1,nV,defUV,false);
        } else if(mezcla && aUV.on && aUV.type==GL_FLOAT && (aUV.vbo || (aUV.ptr && nV>0))){   // las mismas de la unidad 0 (ya subidas)
            Arr uv0={true,2,GL_FLOAT,aUV.stride,aUV.ptr,aUV.vbo};
            if(sinVBO || aUV.vbo) bindAttr(aLuv1,uv0,0,nV,defUV,false);
            else {
                BindArray(vboT);
                if(attrGL[aLuv1].on != 1){ W3D_GLC(glEnableVertexAttribArray(aLuv1)); attrGL[aLuv1].on = 1; }
                W3D_GLC(glVertexAttribPointer(aLuv1,2,GL_FLOAT,GL_FALSE,aUV.stride,(const void*)0));
            }
        } else {
            Arr nada={false,2,GL_FLOAT,0,0};
            bindAttr(aLuv1,nada,0,0,defUV,false);
        }
    }
}
// dibujo INDEXADO (indices ushort; convierte desde MeshIndex/ubyte)
static void drawIndexed(GLenum mode,int count,const unsigned short* idx16){
    gUltimoIdx = count; gUltimoVBO = 0;
    const GLint b = sinVBO ? 0 : (GLint)ibo;
    if(bufIndicesGL != b){ W3D_GLC(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,(GLuint)b)); bufIndicesGL = b; }
    if(!sinVBO) W3D_GLC(glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(count*2),idx16,GL_DYNAMIC_DRAW));
    g_statDrawTris++; g_statIndices += count; g_statDcCat[gStatCat]++;   // (un draw: lineas y puntos tambien)
    W3D_GLC(glDrawElements(mode,count,GL_UNSIGNED_SHORT,sinVBO ? (const void*)idx16 : (const void*)0));
}
static int maxIndex(const MeshIndex* ind,int count){ int n=0; for(int i=0;i<count;i++) if((int)ind[i]+1>n) n=(int)ind[i]+1; return n; }
static int maxIndexU16(const unsigned short* ind,int count){ int n=0; for(int i=0;i<count;i++) if((int)ind[i]+1>n) n=(int)ind[i]+1; return n; }

void DrawTriangles(int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    MedirDraw medir;
    Prim(GL_TRIANGLES); setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_TRIANGLES,count,&idx[0]);
}
void DrawElements(Primitiva p,int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    MedirDraw medir;
    static const GLenum kModo[7]={GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_LINE_LOOP,GL_TRIANGLES,GL_TRIANGLE_STRIP,GL_TRIANGLE_FAN};
    Prim(kModo[(int)p>=0&&(int)p<7?(int)p:4]); setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(kModo[(int)p>=0&&(int)p<7?(int)p:4],count,&idx[0]);
}
void DrawElements16(Primitiva p,int count,const unsigned short* indices){
    if(!ready || count<=0 || !indices) return;
    MedirDraw medir;
    static const GLenum kModo[7]={GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_LINE_LOOP,GL_TRIANGLES,GL_TRIANGLE_STRIP,GL_TRIANGLE_FAN};
    int nV=0; for(int i=0;i<count;i++) if((int)indices[i]+1>nV) nV=(int)indices[i]+1;
    Prim(kModo[(int)p>=0&&(int)p<7?(int)p:4]); setupState(nV);
    drawIndexed(kModo[(int)p>=0&&(int)p<7?(int)p:4],count,indices);
}
static void DrawDesdeIBO(GLenum modo, int count, int first);
void DrawElementsVBO(Primitiva p,int count,int firstIndex){
    static const GLenum kModo[7]={GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_LINE_LOOP,GL_TRIANGLES,GL_TRIANGLE_STRIP,GL_TRIANGLE_FAN};
    DrawDesdeIBO(kModo[(int)p>=0&&(int)p<7?(int)p:4],count,firstIndex);
}
void DrawTrianglesByte(int count,const unsigned char* indices){
    if(!ready || count<=0 || !indices) return;
    MedirDraw medir;
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
    MedirDraw medir;
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
int g_statMeshes = 0;
int g_statDrawTris = 0, g_statDrawVBO = 0, g_statIndices = 0, g_statTexBinds = 0, g_statStateChanges = 0, g_statOtras = 0;
int g_statUploadBytes = 0, g_statTrisOpacos = 0, g_statTrisBlend = 0;
int g_statDcCat[StatCatCount_] = { 0 };
void StatCategoria(StatCat c) { gStatCat = (int)c; }
StatCat StatCategoriaActual() { return (StatCat)gStatCat; }
void StatRaw(int n) { g_statLlamadas[gStatCat] += n; }
StatsDeCuadro g_statCuadro;
static int gCuadroDraws0 = 0, gCuadroIdx0 = 0;
// EL TIEMPO DE LOS DRAWS (g_medirDraws): cuanto tarda la CPU adentro de cada uno (elegir el programa, los uniforms, los
// atributos, la llamada de GL: con arreglos del cliente, el driver copia los vertices ahi) y el mas lento del cuadro
bool g_medirDraws = false;
static double gMsDraws = 0, gMsDrawMax = 0;
static int gDrawMaxIdx = 0, gDrawMaxVerts = 0, gDrawMaxVBO = 0;
static double RelojMs() {
#if defined(_WIN32)
    return 0.0;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec * 1e-6;
#endif
}
MedirDraw::MedirDraw() : t0(g_medirDraws ? RelojMs() : 0.0) {}
MedirDraw::~MedirDraw() {
    if (!g_medirDraws || t0 == 0.0) return;
    const double d = RelojMs() - t0;
    gMsDraws += d;
    if (d > gMsDrawMax) { gMsDrawMax = d; gDrawMaxIdx = gUltimoIdx; gDrawMaxVerts = gUltimoVerts; gDrawMaxVBO = gUltimoVBO; }
}
void StatsCuadro() {
    StatsDeCuadro& c = g_statCuadro;
    c.msDraws = (float)gMsDraws; c.msDrawMax = (float)gMsDrawMax;
    c.drawMaxIdx = gDrawMaxIdx; c.drawMaxVerts = gDrawMaxVerts; c.drawMaxVBO = gDrawMaxVBO;
    gMsDraws = gMsDrawMax = 0; gDrawMaxIdx = gDrawMaxVerts = gDrawMaxVBO = 0;
    c.total = 0;
    for (int i = 0; i < StatCatCount_; i++) { c.llamadas[i] = g_statLlamadas[i]; c.total += g_statLlamadas[i]; g_statLlamadas[i] = 0; }
    c.draws = g_statDrawTris - gCuadroDraws0; gCuadroDraws0 = g_statDrawTris;
    c.binds = 0; c.estados = 0;
    c.tris = (g_statIndices - gCuadroIdx0) / 3; gCuadroIdx0 = g_statIndices;
    c.cuadros++;
    gStatCat = StatCatEditor;
}
void StatsReset() {
    g_statDrawTris = 0; g_statDrawVBO = 0; g_statIndices = 0; g_statTexBinds = 0; g_statMeshes = 0;
    g_statStateChanges = 0; g_statUploadBytes = 0; g_statTrisOpacos = 0; g_statTrisBlend = 0; g_statOtras = 0;
    for (int i = 0; i < StatCatCount_; i++) { g_statDcCat[i] = 0; g_statLlamadas[i] = 0; }
    gCuadroDraws0 = gCuadroIdx0 = 0;
    gStatCat = StatCatEscena;
}
void Finish() { W3D_GLC(glFinish()); }
bool g_auditarEscena = false;
int  g_auditDesyncs  = 0;
int  AuditarEstado() { return 0; }      // en dispositivo no se audita (glGet = flush del tiler)
// MATRICES: en este backend las tres pilas ya viven en la CPU (curProj/curMV/curTex), asi que GetMatrix es una copia
// directa, sin glGet y sin una sombra aparte que pueda desincronizarse: la auditoria no tiene nada que comparar.
bool g_matrizAuditar = false;
int  g_matrizDesyncs = 0;
bool g_invalidarPorMalla = false;      // el Invalidate por malla es del editor de Symbian (GLES 1.1); aca no aplica
void GetMatrix(Matrix m, float* out16) {
    const Matrix4& M = (m == Projection) ? curProj : (m == TextureMatrix ? curTex : curMV);
    for (int i = 0; i < 16; i++) out16[i] = M.m[i];
}
void TexBaseLevel(int nivel) { (void)nivel; }   // ES2 no tiene GL_TEXTURE_BASE_LEVEL (PC solamente)
// BUFFER OBJECTS: la malla sube UNA vez a la GPU (ArrayBufferData / IndexBufferData) y cada draw solo da los punteros
// (VertexVBO y compania, desde el byte 0) en vez de mandar todos los vertices desde la RAM. Los indices de la malla son
// de 32 bits (MeshIndex) y GL ES 2 sin OES_element_index_uint solo dibuja de 16: se convierten al subirlos
struct BufInfo { long bytes; int verts; bool u32; };   // verts = el indice mas alto + 1 (los IBO)
static std::map<GLuint, BufInfo> buffers;
static GLuint iboVBO = 0;   // el IBO de BindIndexVBO
static int indicesU32 = -1; // el driver dibuja indices de 32 bits? (-1 = no se pregunto todavia)
bool VBOSoportado(){ return ready; }
unsigned int GenBuffer(){ GLuint h=0; W3D_GLC(glGenBuffers(1,&h)); return h; }
void DeleteBuffer(unsigned int h){
    if(!h) return;
    W3D_GLC(glDeleteBuffers(1,&h));
    buffers.erase(h);
    Arr* a[4]={&aPos,&aNrm,&aUV,&aCol};
    for(int i=0;i<4;i++) if(a[i]->vbo==h) a[i]->vbo=0;
    if(iboVBO==h) iboVBO=0;
    if(mezclaUVVbo==h) mezclaUVVbo=0;
    AttrOlvidar();   // (GL lo desbindeo de donde estuviera: el cache ya no sabe)
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
long VBOMemoria(){ long t=0; for(std::map<GLuint,BufInfo>::const_iterator it=buffers.begin(); it!=buffers.end(); ++it) t+=it->second.bytes; return t; }
// (los formatos son los de la malla: pos 3 float, normal 3 byte, color 4 ubyte, uv 2 float)
void VertexVBO(unsigned int h){ aPos.size=3; aPos.type=GL_FLOAT; aPos.stride=0; aPos.ptr=0; aPos.vbo=h; }
void NormalVBO(unsigned int h){ aNrm.size=3; aNrm.type=GL_BYTE; aNrm.stride=0; aNrm.ptr=0; aNrm.vbo=h; }
void ColorVBO(unsigned int h){ aCol.size=4; aCol.type=GL_UNSIGNED_BYTE; aCol.stride=0; aCol.ptr=0; aCol.vbo=h; }
void TexCoordVBO(unsigned int h){ aUV.size=2; aUV.type=GL_FLOAT; aUV.stride=0; aUV.ptr=0; aUV.vbo=h; }
void BindIndexVBO(unsigned int h){ iboVBO=h; }
// del IBO bindeado: 'first' indices adelante
static void DrawDesdeIBO(GLenum modo, int count, int first){
    if(!ready || count<=0 || !iboVBO) return;
    MedirDraw medir; gUltimoIdx = count; gUltimoVBO = 1;
    std::map<GLuint,BufInfo>::const_iterator it = buffers.find(iboVBO);
    const int nV = (it != buffers.end()) ? it->second.verts : 0;
    const bool u32 = (it != buffers.end()) && it->second.u32;
    Prim(modo); setupState(nV);
    if(bufIndicesGL != (GLint)iboVBO){ W3D_GLC(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,iboVBO)); bufIndicesGL = (GLint)iboVBO; }
    g_statDrawTris++; g_statDrawVBO++; g_statIndices += count; g_statDcCat[gStatCat]++;
    W3D_GLC(glDrawElements(modo,count,u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,(const void*)(size_t)(first * (u32 ? 4 : 2))));
}
void DrawTrianglesVBO(int count, int firstIndex){ DrawDesdeIBO(GL_TRIANGLES,count,firstIndex); }
void UnbindVBOs(){ aPos.vbo=aNrm.vbo=aUV.vbo=aCol.vbo=0; iboVBO=0; }
// la pila perezosa y el cache de punteros son del pipeline fijo: aca la pila ya vive en la CPU y va al shader por
// uniforms en cada draw (perezosa de por si) y los punteros se leen recien al dibujar. Las opciones existen para el harness
bool g_matricesPerezosas = true;
bool g_punterosCache = true;
void MatricesSincronizar() {}

// ============================================================================
//  LOTE (con un programa por draw: la VideoCore IV de Android 2.3). En ese driver cada draw cuesta ~0.15 ms y la UI hace
//  ~150 por cuadro (un texto, un fondo, un icono: un draw cada uno). Los draws de triangulos sin indices, sin luz,
//  niebla, recorte ni mezcla, se juntan -ya pasados por la modelview en la CPU, con su color- y salen en UN draw cuando
//  cambia algo de lo que no se junta (la textura, la proyeccion...). Cualquier llamada a GL dibuja antes lo juntado
//  (W3D_GLC): nada se adelanta a un draw pendiente. DibujarPendiente lo dibuja antes del swap
// ============================================================================
struct Lote { bool tex; int env; float alphaRef; unsigned textura; Matrix4 proj, texm; };
static Lote lote;
static std::vector<float> lotePos, loteUV, loteCol;
static const int kLoteMax = 6000;   // vertices
static void LoteVaciar(){
    if(!loteN) return;
    const int n = loteN; loteN = 0;   // (antes de cualquier llamada a GL: W3D_GLC volveria a entrar aca)
    // el estado de cuando se junto (lo que el lote no admite, apagado); el de ahora se repone al final
    const bool tex0=cap_tex, luz0=cap_light, fog0=cap_fog, spr0=pointSprite, tg0=texGenEsfera, mez0=mezclaOn;
    const int env0=texEnv, clip0=clipN, aa0=alfaAbajo, prim0=primDraw;
    const float ar0=alphaRef, blur0=blurY;
    const Matrix4 proj0=curProj, mv0=curMV, texm0=curTex;
    const Arr pos0=aPos, nrm0=aNrm, uv0=aUV, col0=aCol;
    cap_tex=lote.tex; cap_light=cap_fog=pointSprite=texGenEsfera=mezclaOn=false; texEnv=lote.env; clipN=0; alfaAbajo=0;
    alphaRef=lote.alphaRef; blurY=0.0f;
    curProj=lote.proj; curMV.Identity(); curTex=lote.texm;
    const Arr p={true,3,GL_FLOAT,0,&lotePos[0],0}, c={true,4,GL_FLOAT,0,&loteCol[0],0};
    const Arr u={lote.tex,2,GL_FLOAT,0,lote.tex ? &loteUV[0] : 0,0}, nada={false,3,GL_FLOAT,0,0,0};
    aPos=p; aCol=c; aUV=u; aNrm=nada;
    Prim(GL_TRIANGLES); setupState(n);
    g_statDrawTris++; g_statIndices += n; g_statDcCat[gStatCat]++;
    W3D_GLC(glDrawArrays(GL_TRIANGLES,0,n));
    cap_tex=tex0; cap_light=luz0; cap_fog=fog0; pointSprite=spr0; texGenEsfera=tg0; mezclaOn=mez0;
    texEnv=env0; clipN=clip0; alfaAbajo=aa0; primDraw=prim0; alphaRef=ar0; blurY=blur0;
    curProj=proj0; curMV=mv0; curTex=texm0;
    aPos=pos0; aNrm=nrm0; aUV=uv0; aCol=col0;
}
void DibujarPendiente(){ LoteVaciar(); }
// un draw de triangulos sin indices (vertices first .. first+n-1) al lote, si se puede; false = se dibuja solo
static bool AlLote(int first, int n){
    if(!porDraw || n > kLoteMax) return false;
    if(cap_light || cap_fog || pointSprite || (mezclaOn && mezclaTex) || clipN>0 || blurY>0.0f || alfaAbajo || (texGenEsfera && cap_tex)) return false;
    if(!aPos.on || !aPos.ptr || aPos.vbo || aPos.type!=GL_FLOAT || (aPos.size!=2 && aPos.size!=3)) return false;
    if(cap_tex && (!aUV.on || !aUV.ptr || aUV.vbo || aUV.type!=GL_FLOAT || aUV.size!=2)) return false;
    const bool colArr = aCol.on && (aCol.ptr || aCol.vbo);
    if(colArr && (aCol.vbo || aCol.size!=4 || (aCol.type!=GL_UNSIGNED_BYTE && aCol.type!=GL_FLOAT))) return false;
    const float* M = curMV.m;
    if(M[3]!=0.0f || M[7]!=0.0f || M[11]!=0.0f || M[15]!=1.0f) return false;   // (solo modelview afin)
    if(loteN){
        bool igual = lote.tex==cap_tex && lote.alphaRef==alphaRef && loteN + n <= kLoteMax && memcmp(lote.proj.m, curProj.m, sizeof(curProj.m))==0;
        if(igual && cap_tex) igual = lote.env==texEnv && lote.textura==boundTex && memcmp(lote.texm.m, curTex.m, sizeof(curTex.m))==0;
        if(!igual) LoteVaciar();
    }
    if(!loteN){ lote.tex=cap_tex; lote.env=texEnv; lote.alphaRef=alphaRef; lote.textura=boundTex; lote.proj=curProj; lote.texm=curTex; }
    lotePos.resize((size_t)(loteN+n)*3); loteCol.resize((size_t)(loteN+n)*4);
    if(cap_tex) loteUV.resize((size_t)(loteN+n)*2);
    // posiciones por la modelview
    const int sp = aPos.stride ? aPos.stride : aPos.size*4;
    const unsigned char* bp = (const unsigned char*)aPos.ptr + (size_t)first*sp;
    float* dp = &lotePos[(size_t)loteN*3];
    for(int i=0;i<n;i++, dp+=3){
        const float* v = (const float*)(bp + (size_t)i*sp);
        const float x=v[0], y=v[1], z=(aPos.size==3) ? v[2] : 0.0f;
        dp[0]=M[0]*x+M[4]*y+M[8]*z+M[12]; dp[1]=M[1]*x+M[5]*y+M[9]*z+M[13]; dp[2]=M[2]*x+M[6]*y+M[10]*z+M[14];
    }
    if(cap_tex){
        const int su = aUV.stride ? aUV.stride : 8;
        const unsigned char* bu = (const unsigned char*)aUV.ptr + (size_t)first*su;
        float* du = &loteUV[(size_t)loteN*2];
        for(int i=0;i<n;i++, du+=2){ const float* t=(const float*)(bu + (size_t)i*su); du[0]=t[0]; du[1]=t[1]; }
    }
    float* dc = &loteCol[(size_t)loteN*4];
    if(colArr && aCol.type==GL_UNSIGNED_BYTE){   // (GL los normaliza igual: c / 255)
        const int sc = aCol.stride ? aCol.stride : 4;
        const unsigned char* bc = (const unsigned char*)aCol.ptr + (size_t)first*sc;
        for(int i=0;i<n;i++, dc+=4){ const unsigned char* k=bc + (size_t)i*sc; for(int j=0;j<4;j++) dc[j]=k[j]/255.0f; }
    } else if(colArr){
        const int sc = aCol.stride ? aCol.stride : 16;
        const unsigned char* bc = (const unsigned char*)aCol.ptr + (size_t)first*sc;
        for(int i=0;i<n;i++, dc+=4){ const float* k=(const float*)(bc + (size_t)i*sc); for(int j=0;j<4;j++) dc[j]=k[j]; }
    } else for(int i=0;i<n;i++, dc+=4) for(int j=0;j<4;j++) dc[j]=curColor[j];
    loteN += n;
    return true;
}

void DrawTrianglesArray(int vertexCount){ if(!ready||vertexCount<=0) return; MedirDraw medir; gUltimoIdx = vertexCount; gUltimoVBO = 0; if(AlLote(0,vertexCount)) return; Prim(GL_TRIANGLES); setupState(vertexCount); g_statDrawTris++; g_statIndices += vertexCount; g_statDcCat[gStatCat]++; W3D_GLC(glDrawArrays(GL_TRIANGLES,0,vertexCount)); }
void DrawTrianglesArrayFrom(int first,int count){ if(!ready||count<=0) return; MedirDraw medir; gUltimoIdx = count; gUltimoVBO = 0; if(AlLote(first,count)) return; Prim(GL_TRIANGLES); setupState(first+count); g_statDrawTris++; g_statIndices += count; g_statDcCat[gStatCat]++; W3D_GLC(glDrawArrays(GL_TRIANGLES,first,count)); }
void DrawLines(int vertexCount){ if(!ready||vertexCount<=0) return; Prim(GL_LINES); setupState(vertexCount); W3D_GLC(glDrawArrays(GL_LINES,0,vertexCount)); }
void DrawLineStrip(int vertexCount){ if(!ready||vertexCount<=0) return; Prim(GL_LINE_STRIP); setupState(vertexCount); W3D_GLC(glDrawArrays(GL_LINE_STRIP,0,vertexCount)); }
void DrawPoints(int vertexCount){ if(!ready||vertexCount<=0) return; Prim(GL_POINTS); setupState(vertexCount); W3D_GLC(glDrawArrays(GL_POINTS,0,vertexCount)); }

void Wireframe(bool){}              // ES2 no tiene glPolygonMode: se dibuja relleno (como GLES1). Wireframe real = draw de aristas.
void PointSpriteCoordReplace(bool on){ pointSprite=on; } // samplear la textura con gl_PointCoord (point sprites)

// ============================================================================
//  Init: carga las funciones GL2.0 (desktop) y compila el shader
// ============================================================================
void GLES2Init(void* (*getProc)(const char*)){
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
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
    // GL de escritorio: el tamanio del punto lo da el shader y el sprite tiene gl_PointCoord (en ES2 ya es asi)
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
    glEnable(GL_POINT_SPRITE);
#else
    (void)getProc;
#endif
    curProj.Identity(); curMV.Identity(); curTex.Identity();
    // (un contexto nuevo arranca con todo lo de fabrica)
    cap_depth=cap_cull=cap_blend=cap_scissor=cap_offset=false; boundTex=0; EstadoGLOlvidar();
    buildProgram();
    { GLint v[4]={0,0,1,1}; glGetIntegerv(GL_VIEWPORT,v); for(int i=0;i<4;i++) vpActual[i]=(float)v[i]; }
}

} // namespace w3dEngine

#ifdef __ANDROID__
#include "w3dBase.h"
    void w3dSetColor(const ColorType c[4]) {
        w3dEngine::Color4f(c[0], c[1], c[2], c[3]);
    }
#endif