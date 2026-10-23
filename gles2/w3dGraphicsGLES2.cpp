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
#include "math/Matrix4.h"
#include <vector>
#include <math.h>
#include <string.h>
#include <stdio.h>

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
#define W3D_GLC(llamada) (g_statLlamadas[gStatCat]++, g_trazaGL ? TrazaGL(#llamada) : (void)0, (llamada))

// ============================================================================
//  Estado emulado del pipeline fijo
// ============================================================================
static bool cap_depth=false, cap_cull=false, cap_tex=false, cap_light=false, cap_blend=false, cap_fog=false, cap_scissor=false;
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
struct Arr { bool on; int size; unsigned type; int stride; const void* ptr; };
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
static GLint aLpos,aLnrm,aLuv,aLcol,aLuv1;
static bool ready=false;

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
"  vec4 eye=uMV*vec4(aPos,1.0); gl_Position=uMVP*vec4(aPos,1.0);\n"
"  vCentro=(gl_Position.xy/gl_Position.w*0.5+0.5)*uViewport.zw+uViewport.xy; vTam=uPointSize;\n"
"  vP=eye.xyz; vFogZ=-eye.z; gl_PointSize=uPointSize; vT1=aUV1;\n"
"  vec3 n=uNMat*aNrm; float nl=length(n); vec3 N=(nl>0.0)?n/nl:vec3(0.0,0.0,1.0);\n"   // GL_NORMALIZE
"  vec4 uv=aUV;\n"
"  if(uTexGen==1){\n"   // GL_SPHERE_MAP: el reflejo de la vista en la normal (en ojo)
"    vec3 r=reflect(normalize(eye.xyz),N); float m=2.0*sqrt(r.x*r.x+r.y*r.y+(r.z+1.0)*(r.z+1.0));\n"
"    uv=vec4(r.x/m+0.5,r.y/m+0.5,0.0,1.0);\n"
"  }\n"
"  vec4 t=uTexM*uv; vT=t.xy/t.w;\n"
"  vec4 c=aCol;\n"
"  if(uLightOn==1){\n"
"    vec4 md=(uColorMat==1)?aCol:uMDiff; vec4 ma=(uColorMat==1)?aCol:uMAmb;\n"   // COLOR_MATERIAL = AMBIENT_AND_DIFFUSE
"    vec3 acc=uMEmis.rgb+ma.rgb*uAmbGlobal.rgb;\n"
"    for(int i=0;i<8;i++){\n"
"      if(i>=uNLuces) break;\n"
"      vec3 L; float at=1.0;\n"
"      if(uLPos[i].w!=0.0){ vec3 d=uLPos[i].xyz/uLPos[i].w-eye.xyz; float dist=length(d); L=d/max(dist,1e-6);\n"
"        at=1.0/max(uLAtt[i].x+uLAtt[i].y*dist+uLAtt[i].z*dist*dist,1e-6); }\n"
"      else L=normalize(uLPos[i].xyz);\n"
"      if(uLCono[i].w>-0.5){ float sd=dot(-L,normalize(uLCono[i].xyz)); at*=(sd>=uLCono[i].w)?pow(max(sd,0.0),uLAtt[i].w):0.0; }\n"
"      float ndl=max(dot(N,L),0.0);\n"
"      vec3 col=ma.rgb*uLAmb[i].rgb+md.rgb*uLDif[i].rgb*ndl;\n"
"      if(ndl>0.0){ float nh=max(dot(N,normalize(L+vec3(0.0,0.0,1.0))),0.0);\n"
"        col+=uMSpec.rgb*uLSpec[i].rgb*((uShine>0.0)?pow(nh,uShine):1.0); }\n"
"      acc+=at*col;\n"
"    }\n"
"    c=vec4(acc,md.a);\n"
"  }\n"
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
"  vec4 Pe = vec4(vP,1.0);\n"   // recorte por planos (desenrollado: GLSL ES 1.00 no indexa uniforms por variable)
"  if(uClipN>0 && dot(Pe,uClip0)<0.0) discard;\n"
"  if(uClipN>1 && dot(Pe,uClip1)<0.0) discard;\n"
"  if(uClipN>2 && dot(Pe,uClip2)<0.0) discard;\n"
"  if(uClipN>3 && dot(Pe,uClip3)<0.0) discard;\n"
"  if(uClipN>4 && dot(Pe,uClip4)<0.0) discard;\n"
"  vec2 uv = vT;\n"
// POINT SPRITE (iconos: luz, origen, cursor 3D): la coordenada adentro del punto sale de la posicion del pixel respecto
// del centro del punto, no de gl_PointCoord: el driver iris de Mesa (Intel, Linux) la entrega en 0,0 -tambien a traves de
// Chrome- y el icono salia invisible (se leia siempre la esquina transparente). gl_FragCoord anda en todos los drivers.
"  if(uPointSprite==1){ W3D_ALTA vec2 d=(gl_FragCoord.xy-vCentro)/vTam; uv=vec2(0.5+d.x,0.5-d.y); }\n"
"  vec4 col = vC;\n"   // sin textura: el color primario (la luz, o el color del vertice)
"  if(uUseTex==1){\n"
"    vec4 tex;\n"
"    if(uBlurY > 0.0){\n"                       // 9 muestras a lo largo de V (motion blur)
"      vec4 acc = vec4(0.0);\n"
"      for(int i=-4;i<=4;i++){\n"
"        float vv = uv.y + float(i)*uBlurY*0.25;\n"
"        float m = step(0.0,vv)*step(vv,1.0);\n"   // fuera de la textura NO aporta: si no, el
"        acc += texture2D(uTex, vec2(uv.x, vv)) * m;\n" // CLAMP repite el borde y deja chorreones
"      }\n"
"      tex = acc * (1.0/9.0);\n"
"    } else tex = texture2D(uTex,uv);\n"
"    if(uAlfaAbajo==1){\n"                      // video con alpha EMPAQUETADO. El frame se sube
"                                             \n"  // invertido en Y, asi que en la TEXTURA el color
"                                             \n"  // queda en la mitad de ABAJO y la mascara arriba.
"      tex.a = texture2D(uTex, vec2(uv.x, uv.y - 0.5)).r;\n"  // la mascara, media textura mas abajo
"    }\n"
"    if(uEnv==1) col=tex;\n"                                                         // GL_REPLACE
"    else if(uEnv==2) col=vec4(vec3(clamp(dot(tex.rgb*2.0-1.0,vC.rgb*2.0-1.0),0.0,1.0)),tex.a*vC.a);\n" // GL_DOT3_RGB
"    else if(uEnv==3) col=vec4(vC.rgb,tex.a*vC.a);\n"                              // solo alfa (por el del color)
"    else col=vC*tex;\n"                                                            // GL_MODULATE
"  }\n"
"  if(uMezcla>=0){ vec4 b=texture2D(uTex1,vT1);\n"   // la unidad 1 sobre lo que salio de la 0 (el alfa no cambia)
"    if(uMezcla==0) col.rgb=mix(col.rgb,b.rgb,uMezclaK);\n"
"    else if(uMezcla==3) col.rgb=max(col.rgb-b.rgb,0.0);\n"
"    else col.rgb=min(col.rgb*b.rgb*((uMezcla==2)?2.0:1.0),1.0); }\n"
"  if(uFogOn==1){ float f;\n"
"    if(uFogModo==1) f=exp(-uFogDens*vFogZ);\n"                                   // GL_EXP
"    else if(uFogModo==2){ float e=uFogDens*vFogZ; f=exp(-e*e); }\n"            // GL_EXP2
"    else f=(uFogEnd-vFogZ)/(uFogEnd-uFogStart);\n"                              // GL_LINEAR
"    col.rgb=mix(uFogColor.rgb,col.rgb,clamp(f,0.0,1.0)); }\n"
"  if(uAlphaRef > 0.0 && col.a < uAlphaRef) discard;\n"   // no pinta NI escribe z-buffer
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
static GLuint compile(GLenum tipo,const char* src){
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    const char* partes[1] = { src };
    const int n = 1;
#else
    // PC (GL 2.1, -DW3D_PC_GLES2): GLSL 1.20 (gl_PointCoord) en vez del GLSL ES 1.00 implicito
    const char* partes[2] = { "#version 120\n", src };
    const int n = 2;
#endif
    GLuint s=W3D_GLC(glCreateShader(tipo)); W3D_GLC(glShaderSource(s,n,partes,0)); W3D_GLC(glCompileShader(s));
    GLint ok=0; W3D_GLC(glGetShaderiv(s,GL_COMPILE_STATUS,&ok));
    if(!ok){ char log[1024]; log[0]=0; W3D_GLC(glGetShaderInfoLog(s,1024,0,log)); fprintf(stderr,"[w3dGLES2] shader compile error: %s\n",log); }
    return s;
}
static void buildProgram(){
    GLuint vs=compile(GL_VERTEX_SHADER,VS), fs=compile(GL_FRAGMENT_SHADER,FS);
    prog=W3D_GLC(glCreateProgram()); W3D_GLC(glAttachShader(prog,vs)); W3D_GLC(glAttachShader(prog,fs));
    W3D_GLC(glBindAttribLocation(prog,0,"aPos"));   // la posicion en el atributo 0 (GL de escritorio lo pide)
    W3D_GLC(glLinkProgram(prog));
    GLint ok=0; W3D_GLC(glGetProgramiv(prog,GL_LINK_STATUS,&ok));
    if(!ok){ char log[1024]; log[0]=0; W3D_GLC(glGetProgramInfoLog(prog,1024,0,log)); fprintf(stderr,"[w3dGLES2] link error: %s\n",log); }
    W3D_GLC(glUseProgram(prog));
    uMVP=W3D_GLC(glGetUniformLocation(prog,"uMVP")); uMV=W3D_GLC(glGetUniformLocation(prog,"uMV")); uNMat=W3D_GLC(glGetUniformLocation(prog,"uNMat"));
    uTexM=W3D_GLC(glGetUniformLocation(prog,"uTexM")); uTexGen=W3D_GLC(glGetUniformLocation(prog,"uTexGen"));
    uUseTex=W3D_GLC(glGetUniformLocation(prog,"uUseTex")); uTex=W3D_GLC(glGetUniformLocation(prog,"uTex"));
    uTex1=W3D_GLC(glGetUniformLocation(prog,"uTex1")); uEnv=W3D_GLC(glGetUniformLocation(prog,"uEnv"));
    uMezcla=W3D_GLC(glGetUniformLocation(prog,"uMezcla")); uMezclaK=W3D_GLC(glGetUniformLocation(prog,"uMezclaK"));
    uViewport=W3D_GLC(glGetUniformLocation(prog,"uViewport"));
    uLightOn=W3D_GLC(glGetUniformLocation(prog,"uLightOn"));
    uColorMat=W3D_GLC(glGetUniformLocation(prog,"uColorMat"));
    uBlurY=W3D_GLC(glGetUniformLocation(prog,"uBlurY"));
    uAlphaRef=W3D_GLC(glGetUniformLocation(prog,"uAlphaRef"));
    uAlfaAbajo=W3D_GLC(glGetUniformLocation(prog,"uAlfaAbajo"));
    uClipN=W3D_GLC(glGetUniformLocation(prog,"uClipN"));
    uClip[0]=W3D_GLC(glGetUniformLocation(prog,"uClip0")); uClip[1]=W3D_GLC(glGetUniformLocation(prog,"uClip1"));
    uClip[2]=W3D_GLC(glGetUniformLocation(prog,"uClip2")); uClip[3]=W3D_GLC(glGetUniformLocation(prog,"uClip3"));
    uClip[4]=W3D_GLC(glGetUniformLocation(prog,"uClip4"));
    W3D_GLC(glUniform1i(uClipN, 0));
    uFogOn=W3D_GLC(glGetUniformLocation(prog,"uFogOn")); uPointSize=W3D_GLC(glGetUniformLocation(prog,"uPointSize"));
    uPointSprite=W3D_GLC(glGetUniformLocation(prog,"uPointSprite"));
    uMDiff=W3D_GLC(glGetUniformLocation(prog,"uMDiff")); uMAmb=W3D_GLC(glGetUniformLocation(prog,"uMAmb"));
    uMSpec=W3D_GLC(glGetUniformLocation(prog,"uMSpec")); uMEmis=W3D_GLC(glGetUniformLocation(prog,"uMEmis"));
    uShine=W3D_GLC(glGetUniformLocation(prog,"uShine")); uAmbGlobal=W3D_GLC(glGetUniformLocation(prog,"uAmbGlobal"));
    uNLuces=W3D_GLC(glGetUniformLocation(prog,"uNLuces"));
    uLPos=W3D_GLC(glGetUniformLocation(prog,"uLPos")); uLAmb=W3D_GLC(glGetUniformLocation(prog,"uLAmb"));
    uLDif=W3D_GLC(glGetUniformLocation(prog,"uLDif")); uLSpec=W3D_GLC(glGetUniformLocation(prog,"uLSpec"));
    uLAtt=W3D_GLC(glGetUniformLocation(prog,"uLAtt")); uLCono=W3D_GLC(glGetUniformLocation(prog,"uLCono"));
    uFogColor=W3D_GLC(glGetUniformLocation(prog,"uFogColor")); uFogStart=W3D_GLC(glGetUniformLocation(prog,"uFogStart")); uFogEnd=W3D_GLC(glGetUniformLocation(prog,"uFogEnd"));
    uFogModo=W3D_GLC(glGetUniformLocation(prog,"uFogModo")); uFogDens=W3D_GLC(glGetUniformLocation(prog,"uFogDens"));
    aLpos=W3D_GLC(glGetAttribLocation(prog,"aPos")); aLnrm=W3D_GLC(glGetAttribLocation(prog,"aNrm"));
    aLuv=W3D_GLC(glGetAttribLocation(prog,"aUV")); aLcol=W3D_GLC(glGetAttribLocation(prog,"aCol"));
    aLuv1=W3D_GLC(glGetAttribLocation(prog,"aUV1"));
    W3D_GLC(glGenBuffers(1,&vboP)); W3D_GLC(glGenBuffers(1,&vboN)); W3D_GLC(glGenBuffers(1,&vboT)); W3D_GLC(glGenBuffers(1,&vboC));
    W3D_GLC(glGenBuffers(1,&vboT1)); W3D_GLC(glGenBuffers(1,&ibo));
    LucesDeFabrica();
    ready=true;
}

// ============================================================================
//  Capacidades (Enable/Disable/IsEnabled)
// ============================================================================
void Enable(Cap c){
    switch(c){
        case DepthTest: cap_depth=true;   W3D_GLC(glEnable(GL_DEPTH_TEST)); break;
        case CullFace:  cap_cull=true;    W3D_GLC(glEnable(GL_CULL_FACE));  break;
        case Blend:     cap_blend=true;   W3D_GLC(glEnable(GL_BLEND));      break;
        case ScissorTest: cap_scissor=true; W3D_GLC(glEnable(GL_SCISSOR_TEST)); break;
        case Texture2D: cap_tex=true;   break; // el shader decide (uUseTex)
        case Lighting:  cap_light=true; break;
        case Fog:       cap_fog=true;   break;
        case PolygonOffsetFill: W3D_GLC(glEnable(GL_POLYGON_OFFSET_FILL)); break; // pick de CARAS: empuja la malla de
                       // oclusion atras para que los triangulos-ID pasen el depth test (sino no se seleccionan)
        case ColorMaterial: cap_colormat=true; break; // el shader usa el vertex color como diffuse/ambient
        case Light0: if(!luces[0].on){ luces[0].on=true; lucesSucias=true; } break;
        case Normalize:
        case PointSprite: case Dither: case Multisample: break; // no aplican / el shader ya normaliza
    }
}
void Disable(Cap c){
    switch(c){
        case DepthTest: cap_depth=false;  W3D_GLC(glDisable(GL_DEPTH_TEST)); break;
        case CullFace:  cap_cull=false;   W3D_GLC(glDisable(GL_CULL_FACE));  break;
        case Blend:     cap_blend=false;  W3D_GLC(glDisable(GL_BLEND));      break;
        case ScissorTest: cap_scissor=false; W3D_GLC(glDisable(GL_SCISSOR_TEST)); break;
        case Texture2D: cap_tex=false;   break;
        case Lighting:  cap_light=false; break;
        case Fog:       cap_fog=false;   break;
        case ColorMaterial: cap_colormat=false; break;
        case Light0: if(luces[0].on){ luces[0].on=false; lucesSucias=true; } break;
        case PolygonOffsetFill: W3D_GLC(glDisable(GL_POLYGON_OFFSET_FILL)); break;
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
                      if(bits&StencilBuffer) m|=GL_STENCIL_BUFFER_BIT; W3D_GLC(glClear(m)); }

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
            if(uClip[i]>=0) W3D_GLC(glUniform4fv(uClip[i],1,e));
        }
    }
    if(uClipN>=0) W3D_GLC(glUniform1i(uClipN,n));
}

// ---- estencil (GL ES 2.0 lo trae en el core) ----
int EstencilBits(){ GLint b=0; W3D_GLC(glGetIntegerv(GL_STENCIL_BITS,&b)); return b>0?(int)b:0; }
void EstencilLimpiar(int v){ W3D_GLC(glClearStencil(v)); W3D_GLC(glStencilMask(0xFFu)); W3D_GLC(glClear(GL_STENCIL_BUFFER_BIT)); }
void EstencilMarcar(int v){ W3D_GLC(glEnable(GL_STENCIL_TEST)); W3D_GLC(glStencilMask(0xFFu)); W3D_GLC(glStencilFunc(GL_ALWAYS,v,0xFFu)); W3D_GLC(glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE)); }
void EstencilDentro(int v){ W3D_GLC(glEnable(GL_STENCIL_TEST)); W3D_GLC(glStencilMask(0x00u)); W3D_GLC(glStencilFunc(GL_EQUAL,v,0xFFu)); W3D_GLC(glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP)); }
void EstencilApagar(){ W3D_GLC(glStencilMask(0xFFu)); W3D_GLC(glDisable(GL_STENCIL_TEST)); }
void Viewport(int x,int y,int w,int h){ if(w<0)w=0; if(h<0)h=0; W3D_GLC(glViewport(x,y,w,h)); vpActual[0]=(float)x; vpActual[1]=(float)y; vpActual[2]=(float)w; vpActual[3]=(float)h; }
// clamp de w/h a >=0: un ancho/alto negativo (viewport muy angosto - iconos/margenes en el outliner)
// es GL_INVALID_VALUE en WebGL y deja el scissor en mal estado. Clampeando no rompe el clipping.
void Scissor(int x,int y,int w,int h){ if(w<0)w=0; if(h<0)h=0; W3D_GLC(glScissor(x,y,w,h)); }
void GetViewport(int v[4]){ GLint g[4]; W3D_GLC(glGetIntegerv(GL_VIEWPORT,g)); for(int i=0;i<4;i++) v[i]=(int)g[i]; }
void GetScissor(int v[4]){ GLint g[4]; W3D_GLC(glGetIntegerv(GL_SCISSOR_BOX,g)); for(int i=0;i<4;i++) v[i]=(int)g[i]; }
bool ScissorActivo(){ return W3D_GLC(glIsEnabled(GL_SCISSOR_TEST))==GL_TRUE; }
void DepthFunc(DepthCmp c){ W3D_GLC(glDepthFunc(c==DepthLEqual?GL_LEQUAL:(c==DepthEqual?GL_EQUAL:(c==DepthAlways?GL_ALWAYS:GL_LESS)))); }
void DepthMask(bool w){ W3D_GLC(glDepthMask(w?GL_TRUE:GL_FALSE)); }
void ColorMask(bool r,bool g,bool b,bool a){ W3D_GLC(glColorMask(r,g,b,a)); }
void LineWidth(float px){ W3D_GLC(glLineWidth(px)); }
void FrontFace(bool ccw){ W3D_GLC(glFrontFace(ccw?GL_CCW:GL_CW)); }
void CullCarasFrontales(bool on){ W3D_GLC(glCullFace(on?GL_FRONT:GL_BACK)); }
void PolygonOffset(float factor,float units){ W3D_GLC(glPolygonOffset(factor,units)); }
void DepthRange(float n,float f){
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    W3D_GLC(glDepthRangef(n,f));
#else
    W3D_GLC(glDepthRange(n,f));
#endif
}
void ReadPixelsBegin(int,int,int,int){}   // (lee el framebuffer de verdad)
void ReadPixelsRGBA(int x,int y,int w,int h,unsigned char* px){ W3D_GLC(glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px)); }
void BlurY(float amt){ blurY = (amt > 0.0f) ? amt : 0.0f; }   // blur vertical por shader
void AlphaTest(float ref){ alphaRef = (ref > 0.0f) ? ref : 0.0f; }  // descarte por alpha
void AlfaEmpaquetado(bool si){ alfaAbajo = si ? 1 : 0; }   // alpha en la mitad de abajo
// la ECUACION tambien (mismo porque que el backend fijo, ver w3dGraphics.cpp:
// tras un pase SUSTRACTIVO, reponer solo la func dejaba todo lo alpha-blended
// RESTANDO del framebuffer: silueta negra con el alpha bien).
void BlendAlpha(){ W3D_GLC(glBlendEquation(GL_FUNC_ADD)); W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); }
void BlendMode(int modo){
    W3D_GLC(glBlendEquation(GL_FUNC_ADD));                                // capas multi-pass: siempre ADD
    if(modo==1)      W3D_GLC(glBlendFunc(GL_DST_COLOR, GL_ZERO));         // Multiply
    else if(modo==2) W3D_GLC(glBlendFunc(GL_ONE, GL_ONE));               // Add
    else             W3D_GLC(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)); // Mix
}
// modos de mezcla nombrados (ver enum Mezcla). GLES2/WebGL: glBlendEquation es nativo.
void SetMezcla(int m){
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
void SesgoMetros(float m){   // (sin cache de la proyeccion: EstadoBase la vuelve a 0 en cada cuadro)
    static float base[16]; static float puesto = 0.0f;
    if (m == puesto) return;
    if (puesto == 0.0f) GetMatrix(Projection, base);
    float P[16]; for (int i = 0; i < 16; i++) P[i] = base[i];
    P[14] -= P[10] * m;
    MatrixMode(Projection); LoadMatrix(P); MatrixMode(ModelView);
    puesto = m;
}
void EstadoBase(){ SesgoMetros(0.0f); AlphaTest(0.0f); Disable(PolygonOffsetFill); Disable(Lighting); Disable(ColorMaterial); }
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
void BindTexture(unsigned int id){ boundTex=id; W3D_GLC(glBindTexture(GL_TEXTURE_2D,id)); }
// el min-filter con la PIRAMIDE si la textura la subio y los mipmaps estan prendidos (como el backend fijo): sin esto lo
// lejano se veia con serrucho y titilaba en web/Android y no en el PC
static GLint FiltroMin(unsigned int id, bool linear){
    if(MipmapsGlobal() && TexTieneMips(id)) return linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    return linear ? GL_LINEAR : GL_NEAREST;
}
void TexFilter(bool linear){ W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,FiltroMin(boundTex,linear))); W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear?GL_LINEAR:GL_NEAREST)); }
void TexWrap(bool repeat){ W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE)); W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE)); }
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
    W3D_GLC(glActiveTexture(GL_TEXTURE1));
    W3D_GLC(glBindTexture(GL_TEXTURE_2D,idB));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,FiltroMin(idB,filtrado)));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,filtrado?GL_LINEAR:GL_NEAREST));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,repetir?GL_REPEAT:GL_CLAMP_TO_EDGE));
    W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,repetir?GL_REPEAT:GL_CLAMP_TO_EDGE));
    W3D_GLC(glActiveTexture(GL_TEXTURE0));
}
void MezclaTexturasOff(){ mezclaOn=false; mezclaUV=0; }
bool MezclaTexturasActiva(){ return mezclaOn; }
void MezclaTexturasUV(const float* p){ mezclaUV=p; }
void MezclaTexturasUVVBO(unsigned int){ mezclaUV=0; }   // (sin VBOs en este backend: las de la unidad 0)
void MezclaTexturaBorrada(unsigned int id){ if(id && id==mezclaTex){ mezclaTex=0; mezclaOn=false; } }
unsigned int ErrorGL(){ return 0u; }
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
void VertexPointer2f(int stride,const float* p){ aPos.size=2; aPos.type=GL_FLOAT; aPos.stride=stride; aPos.ptr=p; }
void VertexPointer3f(int stride,const float* p){ aPos.size=3; aPos.type=GL_FLOAT; aPos.stride=stride; aPos.ptr=p; }
void VertexPointer3s(int stride,const short* p){ aPos.size=3; aPos.type=GL_SHORT; aPos.stride=stride; aPos.ptr=p; }
void VertexPointer2s(int stride,const short* p){ aPos.size=2; aPos.type=GL_SHORT; aPos.stride=stride; aPos.ptr=p; }
void NormalPointer3b(const signed char* p){ aNrm.size=3; aNrm.type=GL_BYTE; aNrm.stride=0; aNrm.ptr=p; }
void ColorPointer4ub(const unsigned char* p){ aCol.size=4; aCol.type=GL_UNSIGNED_BYTE; aCol.stride=0; aCol.ptr=p; }
void TexCoordPointer2f(int stride,const float* p){ aUV.size=2; aUV.type=GL_FLOAT; aUV.stride=stride; aUV.ptr=p; }
void TexCoordPointer3b(const signed char* p, int count){ (void)count; aUV.size=3; aUV.type=GL_BYTE; aUV.stride=0; aUV.ptr=p; } // matcap: normales como UV (sin normalizar, como GL)

// ============================================================================
//  Draw: setup compartido de uniforms + atributos, y las variantes
// ============================================================================
static int bytesOf(unsigned type){ return (type==GL_FLOAT)?4:((type==GL_SHORT)?2:1); }
static void bindAttr(GLint loc,const Arr& ar,GLuint vbo,int nVerts,const float* def,bool allowNorm){
    if(loc<0) return;
    if(ar.on && ar.ptr && nVerts>0){
        int elem = ar.size * bytesOf(ar.type);
        // el ultimo vertice solo aporta 'elem' bytes: con stride>elem no hay que leer el relleno final
        int bytes = ar.stride ? ar.stride*(nVerts-1)+elem : elem*nVerts;
        W3D_GLC(glBindBuffer(GL_ARRAY_BUFFER,vbo));
        W3D_GLC(glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)bytes,ar.ptr,GL_DYNAMIC_DRAW));
        // normal (byte -127..127 -> -1..1) y color (ubyte 0..255 -> 0..1) SI se normalizan.
        // la POSICION NO: un short es una coord ENTERA de pantalla/mundo (ej. 0..800), NO un [-1,1]
        // -> normalizarla la mandaba a ~0 (el borde del viewport / sprites 2D quedaban invisibles).
        GLboolean norm = (allowNorm && ar.type!=GL_FLOAT)?GL_TRUE:GL_FALSE;
        W3D_GLC(glEnableVertexAttribArray(loc));
        W3D_GLC(glVertexAttribPointer(loc,ar.size,ar.type,norm,ar.stride,(const void*)0));
    } else {
        W3D_GLC(glDisableVertexAttribArray(loc));
        W3D_GLC(glVertexAttrib4fv(loc,def)); // constante (normal +Z, color = curColor)
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
// las luces prendidas, juntas al principio de los arreglos del shader (solo cuando cambiaron: los uniforms quedan)
static void SubirLuces(){
    if(!lucesSucias) return;
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
    lucesSucias=false;
}
static void setupState(int nV){
    Matrix4 mvp = curProj * curMV;
    float nmat[9]; MatrizNormal(nmat);
    W3D_GLC(glUseProgram(prog));
    W3D_GLC(glUniformMatrix4fv(uMVP,1,GL_FALSE,mvp.m));
    W3D_GLC(glUniformMatrix4fv(uMV,1,GL_FALSE,curMV.m));
    W3D_GLC(glUniformMatrix3fv(uNMat,1,GL_FALSE,nmat));
    W3D_GLC(glUniformMatrix4fv(uTexM,1,GL_FALSE,curTex.m));
    W3D_GLC(glUniform1i(uTexGen,texGenEsfera?1:0));
    W3D_GLC(glUniform1i(uUseTex,cap_tex?1:0));
    W3D_GLC(glUniform1i(uEnv,texEnv));
    // la luz no hace falta si la textura REEMPLAZA el color (el resultado es el mismo y ahorra el vertex shader)
    W3D_GLC(glUniform1i(uLightOn,(cap_light && !(cap_tex && texEnv==1))?1:0));
    W3D_GLC(glUniform1i(uColorMat,cap_colormat?1:0));
    W3D_GLC(glUniform1f(uBlurY, blurY));
    W3D_GLC(glUniform1f(uAlphaRef, alphaRef));
    W3D_GLC(glUniform1i(uAlfaAbajo, alfaAbajo));
    W3D_GLC(glUniform1i(uFogOn,cap_fog?1:0));
    W3D_GLC(glUniform1i(uTex,0));
    W3D_GLC(glUniform1f(uPointSize,pointSize));
    W3D_GLC(glUniform1i(uPointSprite,pointSprite?1:0));
    W3D_GLC(glUniform4fv(uViewport,1,vpActual));
    W3D_GLC(glUniform4fv(uMDiff,1,matDiff)); W3D_GLC(glUniform4fv(uMAmb,1,matAmb));
    W3D_GLC(glUniform4fv(uMSpec,1,matSpec)); W3D_GLC(glUniform4fv(uMEmis,1,matEmis)); W3D_GLC(glUniform1f(uShine,matShine));
    W3D_GLC(glUniform4fv(uAmbGlobal,1,ambGlobal));
    SubirLuces();
    W3D_GLC(glUniform4fv(uFogColor,1,fogColor)); W3D_GLC(glUniform1f(uFogStart,fogStart)); W3D_GLC(glUniform1f(uFogEnd,fogEnd));
    W3D_GLC(glUniform1i(uFogModo,fogModo)); W3D_GLC(glUniform1f(uFogDens,fogDens));
    // la unidad 1: la textura B en el sampler 1. Apagada, el sampler apunta a la unidad 0 (sin textura en la 1 el
    // navegador avisa en cada draw)
    const bool mezcla = mezclaOn && mezclaTex;
    W3D_GLC(glUniform1i(uMezcla, mezcla ? mezclaModo : -1));
    W3D_GLC(glUniform1i(uTex1, mezcla ? 1 : 0));
    if(mezcla){
        W3D_GLC(glUniform1f(uMezclaK, mezclaK));
        W3D_GLC(glActiveTexture(GL_TEXTURE1)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,mezclaTex));
    }
    if(cap_tex || mezcla){ W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex)); }
    static const float defNrm[4]={0,0,1,0};
    static const float defUV[4]={0,0,0,1};   // la texcoord de fabrica de GL
    bindAttr(aLpos,aPos,vboP,nV, defNrm,  false); // POSICION: NUNCA normalizar (short = coords enteras de pantalla/mundo)
    bindAttr(aLnrm,aNrm,vboN,nV, defNrm,  true);  // normal: byte -> [-1,1]
    bindAttr(aLuv, aUV, vboT,nV, defUV,   false); // uv: float, o las normales en bytes del matcap (GL no las normaliza)
    bindAttr(aLcol,aCol,vboC,nV, curColor,true);  // color: ubyte -> [0,1]. sin array -> color uniforme (Color4*)
    if(aLuv1>=0){
        if(mezcla && mezclaUV){   // UV propias de la unidad 1
            Arr uv1={true,2,GL_FLOAT,0,mezclaUV};
            bindAttr(aLuv1,uv1,vboT1,nV,defUV,false);
        } else if(mezcla && aUV.on && aUV.ptr && aUV.type==GL_FLOAT && nV>0){   // las mismas de la unidad 0 (ya subidas)
            W3D_GLC(glBindBuffer(GL_ARRAY_BUFFER,vboT));
            W3D_GLC(glEnableVertexAttribArray(aLuv1));
            W3D_GLC(glVertexAttribPointer(aLuv1,2,GL_FLOAT,GL_FALSE,aUV.stride,(const void*)0));
        } else {
            W3D_GLC(glDisableVertexAttribArray(aLuv1));
            W3D_GLC(glVertexAttrib4fv(aLuv1,defUV));
        }
    }
}
// dibujo INDEXADO (indices ushort; convierte desde MeshIndex/ubyte)
static void drawIndexed(GLenum mode,int count,const unsigned short* idx16){
    W3D_GLC(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ibo));
    W3D_GLC(glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(count*2),idx16,GL_DYNAMIC_DRAW));
    g_statDrawTris++; g_statIndices += count; g_statDcCat[gStatCat]++;   // (un draw: lineas y puntos tambien)
    W3D_GLC(glDrawElements(mode,count,GL_UNSIGNED_SHORT,(const void*)0));
}
static int maxIndex(const MeshIndex* ind,int count){ int n=0; for(int i=0;i<count;i++) if((int)ind[i]+1>n) n=(int)ind[i]+1; return n; }
static int maxIndexU16(const unsigned short* ind,int count){ int n=0; for(int i=0;i<count;i++) if((int)ind[i]+1>n) n=(int)ind[i]+1; return n; }

void DrawTriangles(int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_TRIANGLES,count,&idx[0]);
}
void DrawElements(Primitiva p,int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    static const GLenum kModo[7]={GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_LINE_LOOP,GL_TRIANGLES,GL_TRIANGLE_STRIP,GL_TRIANGLE_FAN};
    setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(kModo[(int)p>=0&&(int)p<7?(int)p:4],count,&idx[0]);
}
void DrawElementsVBO(Primitiva,int,int){}   // (sin VBO en este backend: VBOSoportado() = false)
void DrawTrianglesByte(int count,const unsigned char* indices){
    if(!ready || count<=0 || !indices) return;
    int nV=0; for(int i=0;i<count;i++) if((int)indices[i]+1>nV) nV=(int)indices[i]+1;   // bytes: sin overload
    setupState(nV);
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_TRIANGLES,count,&idx[0]);
}
void DrawLinesIndexed(int count,const unsigned short* indices){
    if(!ready || count<=0 || !indices) return;
    setupState(maxIndexU16(indices,count)); drawIndexed(GL_LINES,count,indices);
}
void DrawLineStripIndexed(int count,const unsigned short* indices){
    if(!ready || count<=0 || !indices) return;
    setupState(maxIndexU16(indices,count)); drawIndexed(GL_LINE_STRIP,count,indices);
}

// ---------------------------------------------------------------------------
//  INDICES DE CLIENTE sobre los atributos bindeados (ver w3dGraphics.h). En el
//  backend de escritorio existen para dibujar SOLO la lista de triangulos visible
//  (PVS) sin re-subir la geometria; en ES2 este backend ya sube los atributos a un
//  VBO dinamico por draw (VBOSoportado()=false), asi que son el mismo camino que
//  DrawTriangles/DrawLinesIndexed. Faltaban: sin ellas el APK de un juego 3D NO
//  LINKEABA (el pase 2D nunca las usaba, por eso no se habia notado).
// ---------------------------------------------------------------------------
void DrawTrianglesClientIdx(int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_TRIANGLES,count,&idx[0]);
}
void DrawLinesClientIdx(int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    setupState(maxIndex(indices,count));
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
void StatsCuadro() {
    StatsDeCuadro& c = g_statCuadro;
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
// BUFFER OBJECTS: por ahora STUBS -> VBOSoportado()=false, asi Mesh cae al camino client-side de este backend (que
// ya sube sus arrays a un VBO dinamico por draw). Los VBOs PERSISTENTES en el pipeline de atributos (glVertexAttrib
// desde un VBO por-malla) son una optimizacion futura de este backend; la abstraccion ya esta lista.
bool VBOSoportado(){ return false; }
unsigned int GenBuffer(){ return 0; }
void DeleteBuffer(unsigned int){}
void ArrayBufferData(unsigned int, const void*, int){}
void IndexBufferData(unsigned int, const void*, int){}
void VertexVBO(unsigned int){}
void NormalVBO(unsigned int){}
void ColorVBO(unsigned int){}
void TexCoordVBO(unsigned int){}
void BindIndexVBO(unsigned int){}
void DrawTrianglesVBO(int, int){}
void UnbindVBOs(){}
// la pila perezosa y el cache de punteros son del pipeline fijo: aca la pila ya vive en la CPU y va al shader por
// uniforms en cada draw (perezosa de por si) y los punteros se leen recien al dibujar. Las opciones existen para el harness
bool g_matricesPerezosas = true;
bool g_punterosCache = true;
void MatricesSincronizar() {}

void DrawTrianglesArray(int vertexCount){ if(!ready||vertexCount<=0) return; setupState(vertexCount); g_statDrawTris++; g_statIndices += vertexCount; g_statDcCat[gStatCat]++; W3D_GLC(glDrawArrays(GL_TRIANGLES,0,vertexCount)); }
void DrawTrianglesArrayFrom(int first,int count){ if(!ready||count<=0) return; setupState(first+count); g_statDrawTris++; g_statIndices += count; g_statDcCat[gStatCat]++; W3D_GLC(glDrawArrays(GL_TRIANGLES,first,count)); }
void DrawLines(int vertexCount){ if(!ready||vertexCount<=0) return; setupState(vertexCount); W3D_GLC(glDrawArrays(GL_LINES,0,vertexCount)); }
void DrawLineStrip(int vertexCount){ if(!ready||vertexCount<=0) return; setupState(vertexCount); W3D_GLC(glDrawArrays(GL_LINE_STRIP,0,vertexCount)); }
void DrawPoints(int vertexCount){ if(!ready||vertexCount<=0) return; setupState(vertexCount); W3D_GLC(glDrawArrays(GL_POINTS,0,vertexCount)); }

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
    // GL de escritorio: el tamanio del punto lo da el shader y el sprite tiene gl_PointCoord (en ES2 ya es asi)
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
    glEnable(GL_POINT_SPRITE);
#else
    (void)getProc;
#endif
    curProj.Identity(); curMV.Identity(); curTex.Identity();
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