// ============================================================================
//  w3dGraphicsGLES2.cpp — backend OpenGL ES 2.0 / WebGL de w3dEngine.
//
//  ES2/WebGL NO tiene pipeline fijo: todo es shaders. Este backend EMULA el pipeline
//  fijo que usa el Core (la MISMA API que w3dGraphics.cpp, el backend fijo) con:
//    - stack de matrices POR SOFTWARE (Matrix4) -> uniforms (MVP + modelview + normal)
//    - un uber-shader: transform + luz (ambient+diffuse) + textura + DOT3 + fog
//    - los vertex arrays client-side -> VBOs (WebGL no acepta punteros client-side)
//
//  Se elige en compile-time con W3D_WEBGL (en vez de w3dGraphics.cpp). En desktop las
//  funciones GL2.0 se cargan con GLES2Init(getProc = SDL_GL_GetProcAddress); en Emscripten
//  van directas. Mismo codigo -> se verifica en PC (GL 2.1) y compila a WebGL con emcc.
//
//  Lo que ES2 NO tiene fijo (matcap/texgen por HW, wireframe/glPolygonMode) queda como
//  stub honesto marcado TODO: son features de shader a completar, no rompen el link.
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
    static PFactiveTexture       glActiveTexture;
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
static Matrix4 curTex;     // matriz de TEXTURA propia: escribirla NO pisa la modelview (el shader aun no la usa)
static std::vector<Matrix4> stkProj, stkMV;
static float lightPos[4]  = {0,0,1,0};
static float lightDiff[4] = {1,1,1,1};
static float lightAmb[4]  = {0.2f,0.2f,0.2f,1};
static float matDiff[4]   = {0.8f,0.8f,0.8f,1};
static float matAmb[4]    = {0.2f,0.2f,0.2f,1};
static float matSpec[4]   = {0,0,0,1};   // specular del material (negro = apagado -> no se calcula el highlight)
static float matEmis[4]   = {0,0,0,1};   // emissive del material (negro = apagado)
static float matShine     = 0.0f;        // exponente specular (0 = sin highlight)
static float lightSpec[4] = {1,1,1,1};   // specular de la luz (GL_LIGHT0 default = blanco)
static float curColor[4]  = {1,1,1,1};   // color uniforme (Color4*) cuando NO hay array de color
static float fogColor[4]  = {0,0,0,1};
static float fogStart=0, fogEnd=1;
static int   fogModo=0;       // 0 lineal / 1 exp / 2 exp2 (el objeto Niebla), igual que GL_FOG_MODE
static float fogDens=1.0f;
static bool  dot3On=false, replaceOn=false;
static bool  pointSprite=false; // point sprite: samplear la textura con gl_PointCoord (iconos: cursor/origen/luz)
static float pointSize=1.0f;
static unsigned boundTex=0;
struct Arr { bool on; int size; unsigned type; int stride; const void* ptr; };
static Arr aPos={false,3,0,0,0}, aNrm={false,3,0,0,0}, aUV={false,2,0,0,0}, aCol={false,4,0,0,0};

// ---- objetos GL + locations ----
static GLuint prog=0, vboP=0, vboN=0, vboT=0, vboC=0, ibo=0;
static GLint uMVP,uMV,uNMat,uUseTex,uTex,uLightOn,uDot3On,uReplaceOn,uFogOn,uPointSize,uPointSprite,uColorMat,uBlurY,uAlphaRef,uAlfaAbajo;
static GLint uClipN,uClip[5];   // planos de recorte (ver PlanosRecorte): equivalente al GL_CLIP_PLANEi del pipeline fijo
static float blurY = 0.0f;
static float alphaRef = 0.0f;
static int   alfaAbajo = 0;
static GLint uLPos,uLDiff,uLAmb,uMDiff,uMAmb,uFogColor,uFogStart,uFogEnd,uFogModo,uFogDens;
static GLint uMSpec,uMEmis,uShine,uLSpec; // specular + emissive del material/luz
static GLint aLpos,aLnrm,aLuv,aLcol;
static bool ready=false;

// ============================================================================
//  Shaders (GLSL ES 1.00)
// ============================================================================
static const char* VS =
"uniform mat4 uMVP; uniform mat4 uMV; uniform mat3 uNMat; uniform float uPointSize;\n"
"attribute vec3 aPos; attribute vec3 aNrm; attribute vec2 aUV; attribute vec4 aCol;\n"
"varying vec3 vN; varying vec3 vP; varying vec2 vT; varying vec4 vC; varying float vFogZ;\n"
"void main(){ vec4 eye=uMV*vec4(aPos,1.0); gl_Position=uMVP*vec4(aPos,1.0);\n"
"  vN=uNMat*aNrm; vP=eye.xyz; vT=aUV; vC=aCol; vFogZ=-eye.z; gl_PointSize=uPointSize; }\n";
static const char* FS =
"precision mediump float;\n"
"precision mediump int;\n"
"uniform int uUseTex; uniform int uLightOn; uniform int uDot3On; uniform int uReplaceOn; uniform int uFogOn;\n"
"uniform float uBlurY;\n"   // blur vertical (0 = apagado)
"uniform float uAlphaRef;\n" // descarte por alpha (0 = apagado)
"uniform int uAlfaAbajo;\n"  // 1 = el ALPHA viene en la mitad de ABAJO de la textura
"uniform int uColorMat;\n" // COLOR_MATERIAL: usar el color del vertice (vC) como diffuse/ambient del material
"uniform int uPointSprite;\n"
"uniform sampler2D uTex;\n"
"uniform vec4 uLPos; uniform vec4 uLDiff; uniform vec4 uLAmb; uniform vec4 uMDiff; uniform vec4 uMAmb;\n"
"uniform vec4 uMSpec; uniform vec4 uMEmis; uniform vec4 uLSpec; uniform float uShine;\n"
"uniform vec4 uFogColor; uniform float uFogStart; uniform float uFogEnd; uniform int uFogModo; uniform float uFogDens;\n"
"uniform int uClipN; uniform vec4 uClip0,uClip1,uClip2,uClip3,uClip4;\n"   // planos de recorte, en OJO
"varying vec3 vN; varying vec3 vP; varying vec2 vT; varying vec4 vC; varying float vFogZ;\n"
"void main(){\n"
"  vec4 Pe = vec4(vP,1.0);\n"   // recorte por planos (desenrollado: GLSL ES 1.00 no indexa uniforms por variable)
"  if(uClipN>0 && dot(Pe,uClip0)<0.0) discard;\n"
"  if(uClipN>1 && dot(Pe,uClip1)<0.0) discard;\n"
"  if(uClipN>2 && dot(Pe,uClip2)<0.0) discard;\n"
"  if(uClipN>3 && dot(Pe,uClip3)<0.0) discard;\n"
"  if(uClipN>4 && dot(Pe,uClip4)<0.0) discard;\n"
"  vec2 uv = (uPointSprite==1) ? gl_PointCoord : vT;\n" // point sprite: coord por-fragmento (iconos)
"  vec4 tex = vec4(1.0);\n"
"  if(uUseTex==1){\n"
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
"  }\n"
"  vec4 col;\n"
"  if(uDot3On==1){ vec3 N=normalize(tex.rgb*2.0-1.0); vec3 L=normalize(vC.rgb*2.0-1.0); col=vec4(vec3(max(dot(N,L),0.0)),1.0); }\n"
"  else if(uReplaceOn==1){ col=tex; }\n"
"  else if(uLightOn==1){ vec3 N=normalize(vN); vec3 L=normalize(uLPos.xyz-vP); float ndl=max(dot(N,L),0.0);\n"
"    vec4 md=(uColorMat==1)?vC:uMDiff; vec4 ma=(uColorMat==1)?vC:uMAmb;\n" // COLOR_MATERIAL: el vertex color reemplaza diffuse+ambient
"    col=ma*uLAmb + md*uLDiff*ndl; col*=tex; col.a=md.a*tex.a;\n"
"    if(uShine>0.0 && ndl>0.0){ vec3 V=normalize(-vP); vec3 H=normalize(L+V);\n" // specular Blinn-Phong (solo si hay brillo)
"      col.rgb += uMSpec.rgb*uLSpec.rgb*pow(max(dot(N,H),0.0),uShine); }\n"
"    col.rgb += uMEmis.rgb; }\n"                                                  // emissive (negro = no suma nada)
"  else { col=vC*tex; }\n"
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
    GLuint s=W3D_GLC(glCreateShader(tipo)); W3D_GLC(glShaderSource(s,1,&src,0)); W3D_GLC(glCompileShader(s));
    GLint ok=0; W3D_GLC(glGetShaderiv(s,GL_COMPILE_STATUS,&ok));
    if(!ok){ char log[512]; log[0]=0; W3D_GLC(glGetShaderInfoLog(s,512,0,log)); fprintf(stderr,"[w3dGLES2] shader compile error: %s\n",log); }
    return s;
}
static void buildProgram(){
    GLuint vs=compile(GL_VERTEX_SHADER,VS), fs=compile(GL_FRAGMENT_SHADER,FS);
    prog=W3D_GLC(glCreateProgram()); W3D_GLC(glAttachShader(prog,vs)); W3D_GLC(glAttachShader(prog,fs)); W3D_GLC(glLinkProgram(prog));
    GLint ok=0; W3D_GLC(glGetProgramiv(prog,GL_LINK_STATUS,&ok));
    if(!ok){ char log[512]; log[0]=0; W3D_GLC(glGetProgramInfoLog(prog,512,0,log)); fprintf(stderr,"[w3dGLES2] link error: %s\n",log); }
    uMVP=W3D_GLC(glGetUniformLocation(prog,"uMVP")); uMV=W3D_GLC(glGetUniformLocation(prog,"uMV")); uNMat=W3D_GLC(glGetUniformLocation(prog,"uNMat"));
    uUseTex=W3D_GLC(glGetUniformLocation(prog,"uUseTex")); uTex=W3D_GLC(glGetUniformLocation(prog,"uTex"));
    uLightOn=W3D_GLC(glGetUniformLocation(prog,"uLightOn")); uDot3On=W3D_GLC(glGetUniformLocation(prog,"uDot3On")); uReplaceOn=W3D_GLC(glGetUniformLocation(prog,"uReplaceOn"));
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
    uLPos=W3D_GLC(glGetUniformLocation(prog,"uLPos")); uLDiff=W3D_GLC(glGetUniformLocation(prog,"uLDiff")); uLAmb=W3D_GLC(glGetUniformLocation(prog,"uLAmb"));
    uMDiff=W3D_GLC(glGetUniformLocation(prog,"uMDiff")); uMAmb=W3D_GLC(glGetUniformLocation(prog,"uMAmb"));
    uMSpec=W3D_GLC(glGetUniformLocation(prog,"uMSpec")); uMEmis=W3D_GLC(glGetUniformLocation(prog,"uMEmis"));
    uLSpec=W3D_GLC(glGetUniformLocation(prog,"uLSpec")); uShine=W3D_GLC(glGetUniformLocation(prog,"uShine"));
    uFogColor=W3D_GLC(glGetUniformLocation(prog,"uFogColor")); uFogStart=W3D_GLC(glGetUniformLocation(prog,"uFogStart")); uFogEnd=W3D_GLC(glGetUniformLocation(prog,"uFogEnd"));
    uFogModo=W3D_GLC(glGetUniformLocation(prog,"uFogModo")); uFogDens=W3D_GLC(glGetUniformLocation(prog,"uFogDens"));
    aLpos=W3D_GLC(glGetAttribLocation(prog,"aPos")); aLnrm=W3D_GLC(glGetAttribLocation(prog,"aNrm"));
    aLuv=W3D_GLC(glGetAttribLocation(prog,"aUV")); aLcol=W3D_GLC(glGetAttribLocation(prog,"aCol"));
    W3D_GLC(glGenBuffers(1,&vboP)); W3D_GLC(glGenBuffers(1,&vboN)); W3D_GLC(glGenBuffers(1,&vboT)); W3D_GLC(glGenBuffers(1,&vboC)); W3D_GLC(glGenBuffers(1,&ibo));
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
        case Light0: case Normalize:
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
        case PolygonOffsetFill: W3D_GLC(glDisable(GL_POLYGON_OFFSET_FILL)); break;
        default: break;
    }
}
bool IsEnabled(Cap c){
    switch(c){
        case DepthTest: return cap_depth; case CullFace: return cap_cull; case Blend: return cap_blend;
        case ScissorTest: return cap_scissor; case Texture2D: return cap_tex; case Lighting: return cap_light;
        case Fog: return cap_fog; case ColorMaterial: return cap_colormat;
        default: return false;
    }
}

// ============================================================================
//  Matrices
// ============================================================================
void MatrixMode(Matrix m){ matMode = (m==Projection)?0:(m==TextureMatrix?2:1); }
void LoadIdentity(){ cur().Identity(); }
void PushMatrix(){ if(matMode==0) stkProj.push_back(curProj); else stkMV.push_back(curMV); }
void PopMatrix(){ if(matMode==0){ if(!stkProj.empty()){ curProj=stkProj.back(); stkProj.pop_back(); } }
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
void Viewport(int x,int y,int w,int h){ if(w<0)w=0; if(h<0)h=0; W3D_GLC(glViewport(x,y,w,h)); }
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
void PolygonOffset(float factor,float units){ W3D_GLC(glPolygonOffset(factor,units)); }
void DepthRange(float n,float f){
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    W3D_GLC(glDepthRangef(n,f));
#else
    W3D_GLC(glDepthRange(n,f));
#endif
}
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
void Invalidate(){}          // este backend no cachea estado GL
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
void Light0fv(LightFv p,const float* v){
    if(p==LightPosition){ // se guarda en EYE space (como glLightfv): x la modelview actual
        if(v[3]!=0.0f){ Vector3 e=curMV*Vector3(v[0],v[1],v[2]); lightPos[0]=e.x; lightPos[1]=e.y; lightPos[2]=e.z; lightPos[3]=1; }
        else { // direccional: solo rota (sin traslacion)
            lightPos[0]=curMV.m[0]*v[0]+curMV.m[4]*v[1]+curMV.m[8]*v[2];
            lightPos[1]=curMV.m[1]*v[0]+curMV.m[5]*v[1]+curMV.m[9]*v[2];
            lightPos[2]=curMV.m[2]*v[0]+curMV.m[6]*v[1]+curMV.m[10]*v[2]; lightPos[3]=0;
        }
    }
    else if(p==LightDiffuse)  for(int i=0;i<4;i++) lightDiff[i]=v[i];
    else if(p==LightAmbient)  for(int i=0;i<4;i++) lightAmb[i]=v[i];
    else if(p==LightSpecular) for(int i=0;i<4;i++) lightSpec[i]=v[i];
}
void Light0f(LightF,float){}          // atenuacion: TODO en el shader
void SetLightEnabled(unsigned int,bool){} // 1 sola luz en el shader; multi-luz: TODO
void LightNfv(int i,LightFv p,const float* v){ if(i==0) Light0fv(p,v); }   // el shader tiene UNA luz
void LightN(int,bool){}                       // (como SetLightEnabled: la luz del shader no se apaga sola)
void LightModelAmbient(const float*){}        // sin ambiente global en el shader: TODO
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
void TexFilter(bool linear){ W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,linear?GL_LINEAR:GL_NEAREST)); W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear?GL_LINEAR:GL_NEAREST)); }
void TexWrap(bool repeat){ W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE)); W3D_GLC(glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE)); }
void TexEnvDot3(bool on){ dot3On=on; }
void TexEnvReplace(bool on){ replaceOn=on; }
void TexEnvAlphaOnly(bool){}   // TODO ES2: modo alpha-only en el uber-shader (pase alpha en WebGL)
// MEZCLA DE TEXTURAS (unidad 1 con INTERPOLATE en GL1): TODO ES2: segunda textura + k en el uber-shader. Mientras
// tanto no hay mezcla: se dibuja la textura base sola (el material queda igual que sin texturaB)
bool MezclaTexturasSoportada(){ return false; }
void MezclaTexturas(unsigned int, float, bool, bool, int){}
void MezclaTexturasOff(){}
bool MezclaTexturasActiva(){ return false; }
void MezclaTexturasUV(const float*){}
void MezclaTexturasUVVBO(unsigned int){}
void MezclaTexturaBorrada(unsigned int){}
void TexGenSphere(bool){}      // TODO: matcap por shader (UV = reflejo del normal en eye-space)
void TexMatrixMatcap(bool){}   // TODO: idem, via matriz de textura -> UV del normal
void TexMatrixUV(float,float,float,float){}   // TODO: uniform de la matriz de textura en el uber-shader
bool TieneTexGen(){ return false; } // ES2 no tiene glTexGen fijo -> el matcap va por shader

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
void TexCoordPointer3b(const signed char* p, int count){ (void)count; aUV.size=3; aUV.type=GL_BYTE; aUV.stride=0; aUV.ptr=p; } // matcap (normales como UV): TODO en el shader

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
static void setupState(int nV){
    Matrix4 mvp = curProj * curMV;
    float nmat[9] = { curMV.m[0],curMV.m[1],curMV.m[2], curMV.m[4],curMV.m[5],curMV.m[6], curMV.m[8],curMV.m[9],curMV.m[10] };
    W3D_GLC(glUseProgram(prog));
    W3D_GLC(glUniformMatrix4fv(uMVP,1,GL_FALSE,mvp.m));
    W3D_GLC(glUniformMatrix4fv(uMV,1,GL_FALSE,curMV.m));
    W3D_GLC(glUniformMatrix3fv(uNMat,1,GL_FALSE,nmat));
    W3D_GLC(glUniform1i(uUseTex,cap_tex?1:0));
    W3D_GLC(glUniform1i(uLightOn,(cap_light && !dot3On && !replaceOn)?1:0));
    W3D_GLC(glUniform1i(uColorMat,cap_colormat?1:0));
    W3D_GLC(glUniform1f(uBlurY, blurY));
    W3D_GLC(glUniform1f(uAlphaRef, alphaRef));
    W3D_GLC(glUniform1i(uAlfaAbajo, alfaAbajo)); // vertex color como material (COLOR_MATERIAL) con luz activa
    W3D_GLC(glUniform1i(uDot3On,dot3On?1:0));
    W3D_GLC(glUniform1i(uReplaceOn,replaceOn?1:0));
    W3D_GLC(glUniform1i(uFogOn,cap_fog?1:0));
    W3D_GLC(glUniform1i(uTex,0));
    W3D_GLC(glUniform1f(uPointSize,pointSize));
    W3D_GLC(glUniform1i(uPointSprite,pointSprite?1:0));
    W3D_GLC(glUniform4fv(uLPos,1,lightPos)); W3D_GLC(glUniform4fv(uLDiff,1,lightDiff)); W3D_GLC(glUniform4fv(uLAmb,1,lightAmb));
    W3D_GLC(glUniform4fv(uMDiff,1,matDiff)); W3D_GLC(glUniform4fv(uMAmb,1,matAmb));
    W3D_GLC(glUniform4fv(uMSpec,1,matSpec)); W3D_GLC(glUniform4fv(uMEmis,1,matEmis)); W3D_GLC(glUniform4fv(uLSpec,1,lightSpec)); W3D_GLC(glUniform1f(uShine,matShine));
    W3D_GLC(glUniform4fv(uFogColor,1,fogColor)); W3D_GLC(glUniform1f(uFogStart,fogStart)); W3D_GLC(glUniform1f(uFogEnd,fogEnd));
    W3D_GLC(glUniform1i(uFogModo,fogModo)); W3D_GLC(glUniform1f(uFogDens,fogDens));
    if(cap_tex){ W3D_GLC(glActiveTexture(GL_TEXTURE0)); W3D_GLC(glBindTexture(GL_TEXTURE_2D,boundTex)); }
    static const float defNrm[4]={0,0,1,0};
    bindAttr(aLpos,aPos,vboP,nV, defNrm,  false); // POSICION: NUNCA normalizar (short = coords enteras de pantalla/mundo)
    bindAttr(aLnrm,aNrm,vboN,nV, defNrm,  true);  // normal: byte -> [-1,1]
    bindAttr(aLuv, aUV, vboT,nV, defNrm,  true);  // uv: float (no toca); byte matcap seria TODO
    bindAttr(aLcol,aCol,vboC,nV, curColor,true);  // color: ubyte -> [0,1]. sin array -> color uniforme (Color4*)
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
#else
    (void)getProc;
#endif
    curProj.Identity(); curMV.Identity();
    buildProgram();
}

} // namespace w3dEngine

#ifdef __ANDROID__
#include "w3dBase.h"
    void w3dSetColor(const ColorType c[4]) {
        w3dEngine::Color4f(c[0], c[1], c[2], c[3]);
    }
#endif