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
bool w3dRenderSolido    = false;
bool w3dRenderSinLuz    = false;
bool w3dRenderLuces     = false;
bool w3dRenderNormalColor = false;
bool w3dRenderAlpha       = false; // pase ALPHA (matte): blanco unlit + solo el alpha de la textura
bool w3dRenderOverlays    = true;

namespace w3dEngine {

// ============================================================================
//  Estado emulado del pipeline fijo
// ============================================================================
static bool cap_depth=false, cap_cull=false, cap_tex=false, cap_light=false, cap_blend=false, cap_fog=false, cap_scissor=false;
static int  matMode = 1; // 0=Projection, 1=ModelView
static Matrix4 curProj, curMV;
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
static bool  dot3On=false, replaceOn=false;
static bool  pointSprite=false; // point sprite: samplear la textura con gl_PointCoord (iconos: cursor/origen/luz)
static float pointSize=1.0f;
static unsigned boundTex=0;
struct Arr { bool on; int size; unsigned type; int stride; const void* ptr; };
static Arr aPos={false,3,0,0,0}, aNrm={false,3,0,0,0}, aUV={false,2,0,0,0}, aCol={false,4,0,0,0};

// ---- objetos GL + locations ----
static GLuint prog=0, vboP=0, vboN=0, vboT=0, vboC=0, ibo=0;
static GLint uMVP,uMV,uNMat,uUseTex,uTex,uLightOn,uDot3On,uReplaceOn,uFogOn,uPointSize,uPointSprite;
static GLint uLPos,uLDiff,uLAmb,uMDiff,uMAmb,uFogColor,uFogStart,uFogEnd;
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
"uniform int uPointSprite;\n"
"uniform sampler2D uTex;\n"
"uniform vec4 uLPos; uniform vec4 uLDiff; uniform vec4 uLAmb; uniform vec4 uMDiff; uniform vec4 uMAmb;\n"
"uniform vec4 uMSpec; uniform vec4 uMEmis; uniform vec4 uLSpec; uniform float uShine;\n"
"uniform vec4 uFogColor; uniform float uFogStart; uniform float uFogEnd;\n"
"varying vec3 vN; varying vec3 vP; varying vec2 vT; varying vec4 vC; varying float vFogZ;\n"
"void main(){\n"
"  vec2 uv = (uPointSprite==1) ? gl_PointCoord : vT;\n" // point sprite: coord por-fragmento (iconos)
"  vec4 tex = (uUseTex==1) ? texture2D(uTex,uv) : vec4(1.0);\n"
"  vec4 col;\n"
"  if(uDot3On==1){ vec3 N=normalize(tex.rgb*2.0-1.0); vec3 L=normalize(vC.rgb*2.0-1.0); col=vec4(vec3(max(dot(N,L),0.0)),1.0); }\n"
"  else if(uReplaceOn==1){ col=tex; }\n"
"  else if(uLightOn==1){ vec3 N=normalize(vN); vec3 L=normalize(uLPos.xyz-vP); float ndl=max(dot(N,L),0.0);\n"
"    col=uMAmb*uLAmb + uMDiff*uLDiff*ndl; col*=tex; col.a=uMDiff.a*tex.a;\n"
"    if(uShine>0.0 && ndl>0.0){ vec3 V=normalize(-vP); vec3 H=normalize(L+V);\n" // specular Blinn-Phong (solo si hay brillo)
"      col.rgb += uMSpec.rgb*uLSpec.rgb*pow(max(dot(N,H),0.0),uShine); }\n"
"    col.rgb += uMEmis.rgb; }\n"                                                  // emissive (negro = no suma nada)
"  else { col=vC*tex; }\n"
"  if(uFogOn==1){ float f=clamp((uFogEnd-vFogZ)/(uFogEnd-uFogStart),0.0,1.0); col.rgb=mix(uFogColor.rgb,col.rgb,f); }\n"
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
static Matrix4& cur(){ return matMode==0 ? curProj : curMV; }

// ============================================================================
//  Compilar / linkear el programa
// ============================================================================
static GLuint compile(GLenum tipo,const char* src){
    GLuint s=glCreateShader(tipo); glShaderSource(s,1,&src,0); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[512]; log[0]=0; glGetShaderInfoLog(s,512,0,log); fprintf(stderr,"[w3dGLES2] shader compile error: %s\n",log); }
    return s;
}
static void buildProgram(){
    GLuint vs=compile(GL_VERTEX_SHADER,VS), fs=compile(GL_FRAGMENT_SHADER,FS);
    prog=glCreateProgram(); glAttachShader(prog,vs); glAttachShader(prog,fs); glLinkProgram(prog);
    GLint ok=0; glGetProgramiv(prog,GL_LINK_STATUS,&ok);
    if(!ok){ char log[512]; log[0]=0; glGetProgramInfoLog(prog,512,0,log); fprintf(stderr,"[w3dGLES2] link error: %s\n",log); }
    uMVP=glGetUniformLocation(prog,"uMVP"); uMV=glGetUniformLocation(prog,"uMV"); uNMat=glGetUniformLocation(prog,"uNMat");
    uUseTex=glGetUniformLocation(prog,"uUseTex"); uTex=glGetUniformLocation(prog,"uTex");
    uLightOn=glGetUniformLocation(prog,"uLightOn"); uDot3On=glGetUniformLocation(prog,"uDot3On"); uReplaceOn=glGetUniformLocation(prog,"uReplaceOn");
    uFogOn=glGetUniformLocation(prog,"uFogOn"); uPointSize=glGetUniformLocation(prog,"uPointSize");
    uPointSprite=glGetUniformLocation(prog,"uPointSprite");
    uLPos=glGetUniformLocation(prog,"uLPos"); uLDiff=glGetUniformLocation(prog,"uLDiff"); uLAmb=glGetUniformLocation(prog,"uLAmb");
    uMDiff=glGetUniformLocation(prog,"uMDiff"); uMAmb=glGetUniformLocation(prog,"uMAmb");
    uMSpec=glGetUniformLocation(prog,"uMSpec"); uMEmis=glGetUniformLocation(prog,"uMEmis");
    uLSpec=glGetUniformLocation(prog,"uLSpec"); uShine=glGetUniformLocation(prog,"uShine");
    uFogColor=glGetUniformLocation(prog,"uFogColor"); uFogStart=glGetUniformLocation(prog,"uFogStart"); uFogEnd=glGetUniformLocation(prog,"uFogEnd");
    aLpos=glGetAttribLocation(prog,"aPos"); aLnrm=glGetAttribLocation(prog,"aNrm");
    aLuv=glGetAttribLocation(prog,"aUV"); aLcol=glGetAttribLocation(prog,"aCol");
    glGenBuffers(1,&vboP); glGenBuffers(1,&vboN); glGenBuffers(1,&vboT); glGenBuffers(1,&vboC); glGenBuffers(1,&ibo);
    ready=true;
}

// ============================================================================
//  Capacidades (Enable/Disable/IsEnabled)
// ============================================================================
void Enable(Cap c){
    switch(c){
        case DepthTest: cap_depth=true;   glEnable(GL_DEPTH_TEST); break;
        case CullFace:  cap_cull=true;    glEnable(GL_CULL_FACE);  break;
        case Blend:     cap_blend=true;   glEnable(GL_BLEND);      break;
        case ScissorTest: cap_scissor=true; glEnable(GL_SCISSOR_TEST); break;
        case Texture2D: cap_tex=true;   break; // el shader decide (uUseTex)
        case Lighting:  cap_light=true; break;
        case Fog:       cap_fog=true;   break;
        case PolygonOffsetFill: glEnable(GL_POLYGON_OFFSET_FILL); break; // pick de CARAS: empuja la malla de
                       // oclusion atras para que los triangulos-ID pasen el depth test (sino no se seleccionan)
        case Light0: case Normalize: case ColorMaterial:
        case PointSprite: case Dither: case Multisample: break; // no aplican / el shader ya normaliza
    }
}
void Disable(Cap c){
    switch(c){
        case DepthTest: cap_depth=false;  glDisable(GL_DEPTH_TEST); break;
        case CullFace:  cap_cull=false;   glDisable(GL_CULL_FACE);  break;
        case Blend:     cap_blend=false;  glDisable(GL_BLEND);      break;
        case ScissorTest: cap_scissor=false; glDisable(GL_SCISSOR_TEST); break;
        case Texture2D: cap_tex=false;   break;
        case Lighting:  cap_light=false; break;
        case Fog:       cap_fog=false;   break;
        case PolygonOffsetFill: glDisable(GL_POLYGON_OFFSET_FILL); break;
        default: break;
    }
}
bool IsEnabled(Cap c){
    switch(c){
        case DepthTest: return cap_depth; case CullFace: return cap_cull; case Blend: return cap_blend;
        case ScissorTest: return cap_scissor; case Texture2D: return cap_tex; case Lighting: return cap_light;
        case Fog: return cap_fog; default: return false;
    }
}

// ============================================================================
//  Matrices
// ============================================================================
void MatrixMode(Matrix m){ matMode = (m==Projection)?0:1; } // TextureMatrix: TODO (matcap)
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
void ClearColor(float r,float g,float b,float a){ glClearColor(r,g,b,a); }
void Clear(int bits){ GLbitfield m=0; if(bits&ColorBuffer) m|=GL_COLOR_BUFFER_BIT; if(bits&DepthBuffer) m|=GL_DEPTH_BUFFER_BIT; glClear(m); }
void Viewport(int x,int y,int w,int h){ if(w<0)w=0; if(h<0)h=0; glViewport(x,y,w,h); }
// clamp de w/h a >=0: un ancho/alto negativo (viewport muy angosto - iconos/margenes en el outliner)
// es GL_INVALID_VALUE en WebGL y deja el scissor en mal estado. Clampeando no rompe el clipping.
void Scissor(int x,int y,int w,int h){ if(w<0)w=0; if(h<0)h=0; glScissor(x,y,w,h); }
void DepthFunc(DepthCmp c){ glDepthFunc(c==DepthLEqual?GL_LEQUAL:(c==DepthEqual?GL_EQUAL:GL_LESS)); }
void DepthMask(bool w){ glDepthMask(w?GL_TRUE:GL_FALSE); }
void ColorMask(bool r,bool g,bool b,bool a){ glColorMask(r,g,b,a); }
void LineWidth(float px){ glLineWidth(px); }
void FrontFace(bool ccw){ glFrontFace(ccw?GL_CCW:GL_CW); }
void PolygonOffset(float factor,float units){ glPolygonOffset(factor,units); }
void DepthRange(float n,float f){
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    glDepthRangef(n,f);
#else
    glDepthRange(n,f);
#endif
}
void ReadPixelsRGBA(int x,int y,int w,int h,unsigned char* px){ glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px); }
void BlendAlpha(){ glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
void BlendMode(int modo){
    if(modo==1)      glBlendFunc(GL_DST_COLOR, GL_ZERO);         // Multiply
    else if(modo==2) glBlendFunc(GL_ONE, GL_ONE);               // Add
    else             glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Mix
}
void SmoothShading(bool){}   // ES2 siempre interpola (smooth); FLAT seria un shader aparte -> TODO
void FastPerspective(){}     // ES2 corrige perspectiva siempre
void Invalidate(){}          // este backend no cachea estado GL

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
void BindTexture(unsigned int id){ boundTex=id; glBindTexture(GL_TEXTURE_2D,id); }
unsigned int BoundTexture(){ return boundTex; }
void TexFilter(bool linear){ glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,linear?GL_LINEAR:GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear?GL_LINEAR:GL_NEAREST); }
void TexWrap(bool repeat){ glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,repeat?GL_REPEAT:GL_CLAMP_TO_EDGE); }
void TexEnvDot3(bool on){ dot3On=on; }
void TexEnvReplace(bool on){ replaceOn=on; }
void TexEnvAlphaOnly(bool){}   // TODO ES2: modo alpha-only en el uber-shader (pase alpha en WebGL)
void TexGenSphere(bool){}      // TODO: matcap por shader (UV = reflejo del normal en eye-space)
void TexMatrixMatcap(bool){}   // TODO: idem, via matriz de textura -> UV del normal
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
void FogMode(bool){}           // solo LINEAR por ahora (el shader es lineal)
void FogStart(float z){ fogStart=z; }
void FogEnd(float z){ fogEnd=z; }
void FogColor(const float* rgba){ for(int i=0;i<4;i++) fogColor[i]=rgba[i]; }
void PointSize(float px){ pointSize=px; } // gl_PointSize en el VS (ES2 no tiene glPointSize)

// ============================================================================
//  Vertex arrays -> se suben a VBOs en el draw
// ============================================================================
void EnableArray(VArray a){ if(a==VertexArray)aPos.on=true; else if(a==NormalArray)aNrm.on=true; else if(a==TexCoordArray)aUV.on=true; else if(a==ColorArray)aCol.on=true; }
void DisableArray(VArray a){ if(a==VertexArray)aPos.on=false; else if(a==NormalArray)aNrm.on=false; else if(a==TexCoordArray)aUV.on=false; else if(a==ColorArray)aCol.on=false; }
void VertexPointer2f(int stride,const float* p){ aPos.size=2; aPos.type=GL_FLOAT; aPos.stride=stride; aPos.ptr=p; }
void VertexPointer3f(int stride,const float* p){ aPos.size=3; aPos.type=GL_FLOAT; aPos.stride=stride; aPos.ptr=p; }
void VertexPointer3s(int stride,const short* p){ aPos.size=3; aPos.type=GL_SHORT; aPos.stride=stride; aPos.ptr=p; }
void VertexPointer2s(int stride,const short* p){ aPos.size=2; aPos.type=GL_SHORT; aPos.stride=stride; aPos.ptr=p; }
void NormalPointer3b(const signed char* p){ aNrm.size=3; aNrm.type=GL_BYTE; aNrm.stride=0; aNrm.ptr=p; }
void ColorPointer4ub(const unsigned char* p){ aCol.size=4; aCol.type=GL_UNSIGNED_BYTE; aCol.stride=0; aCol.ptr=p; }
void TexCoordPointer2f(int stride,const float* p){ aUV.size=2; aUV.type=GL_FLOAT; aUV.stride=stride; aUV.ptr=p; }
void TexCoordPointer3b(const signed char* p){ aUV.size=3; aUV.type=GL_BYTE; aUV.stride=0; aUV.ptr=p; } // matcap (normales como UV): TODO en el shader

// ============================================================================
//  Draw: setup compartido de uniforms + atributos, y las variantes
// ============================================================================
static int bytesOf(unsigned type){ return (type==GL_FLOAT)?4:((type==GL_SHORT)?2:1); }
static void bindAttr(GLint loc,const Arr& ar,GLuint vbo,int nVerts,const float* def,bool allowNorm){
    if(loc<0) return;
    if(ar.on && ar.ptr && nVerts>0){
        int elem = ar.size * bytesOf(ar.type);
        int bytes = (ar.stride?ar.stride:elem) * nVerts;
        glBindBuffer(GL_ARRAY_BUFFER,vbo);
        glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)bytes,ar.ptr,GL_DYNAMIC_DRAW);
        // normal (byte -127..127 -> -1..1) y color (ubyte 0..255 -> 0..1) SI se normalizan.
        // la POSICION NO: un short es una coord ENTERA de pantalla/mundo (ej. 0..800), NO un [-1,1]
        // -> normalizarla la mandaba a ~0 (el borde del viewport / sprites 2D quedaban invisibles).
        GLboolean norm = (allowNorm && ar.type!=GL_FLOAT)?GL_TRUE:GL_FALSE;
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc,ar.size,ar.type,norm,ar.stride,(const void*)0);
    } else {
        glDisableVertexAttribArray(loc);
        glVertexAttrib4fv(loc,def); // constante (normal +Z, color = curColor)
    }
}
static void setupState(int nV){
    Matrix4 mvp = curProj * curMV;
    float nmat[9] = { curMV.m[0],curMV.m[1],curMV.m[2], curMV.m[4],curMV.m[5],curMV.m[6], curMV.m[8],curMV.m[9],curMV.m[10] };
    glUseProgram(prog);
    glUniformMatrix4fv(uMVP,1,GL_FALSE,mvp.m);
    glUniformMatrix4fv(uMV,1,GL_FALSE,curMV.m);
    glUniformMatrix3fv(uNMat,1,GL_FALSE,nmat);
    glUniform1i(uUseTex,cap_tex?1:0);
    glUniform1i(uLightOn,(cap_light && !dot3On && !replaceOn)?1:0);
    glUniform1i(uDot3On,dot3On?1:0);
    glUniform1i(uReplaceOn,replaceOn?1:0);
    glUniform1i(uFogOn,cap_fog?1:0);
    glUniform1i(uTex,0);
    glUniform1f(uPointSize,pointSize);
    glUniform1i(uPointSprite,pointSprite?1:0);
    glUniform4fv(uLPos,1,lightPos); glUniform4fv(uLDiff,1,lightDiff); glUniform4fv(uLAmb,1,lightAmb);
    glUniform4fv(uMDiff,1,matDiff); glUniform4fv(uMAmb,1,matAmb);
    glUniform4fv(uMSpec,1,matSpec); glUniform4fv(uMEmis,1,matEmis); glUniform4fv(uLSpec,1,lightSpec); glUniform1f(uShine,matShine);
    glUniform4fv(uFogColor,1,fogColor); glUniform1f(uFogStart,fogStart); glUniform1f(uFogEnd,fogEnd);
    if(cap_tex){ glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,boundTex); }
    static const float defNrm[4]={0,0,1,0};
    bindAttr(aLpos,aPos,vboP,nV, defNrm,  false); // POSICION: NUNCA normalizar (short = coords enteras de pantalla/mundo)
    bindAttr(aLnrm,aNrm,vboN,nV, defNrm,  true);  // normal: byte -> [-1,1]
    bindAttr(aLuv, aUV, vboT,nV, defNrm,  true);  // uv: float (no toca); byte matcap seria TODO
    bindAttr(aLcol,aCol,vboC,nV, curColor,true);  // color: ubyte -> [0,1]. sin array -> color uniforme (Color4*)
}
// dibujo INDEXADO (indices ushort; convierte desde MeshIndex/ubyte)
static void drawIndexed(GLenum mode,int count,const unsigned short* idx16){
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(count*2),idx16,GL_DYNAMIC_DRAW);
    glDrawElements(mode,count,GL_UNSIGNED_SHORT,(const void*)0);
}
static int maxIndex(const MeshIndex* ind,int count){ int n=0; for(int i=0;i<count;i++) if((int)ind[i]+1>n) n=(int)ind[i]+1; return n; }

void DrawTriangles(int count,const MeshIndex* indices){
    if(!ready || count<=0 || !indices) return;
    setupState(maxIndex(indices,count));
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_TRIANGLES,count,&idx[0]);
}
void DrawTrianglesByte(int count,const unsigned char* indices){
    if(!ready || count<=0 || !indices) return;
    int nV=0; for(int i=0;i<count;i++) if((int)indices[i]+1>nV) nV=(int)indices[i]+1;
    setupState(nV);
    static std::vector<unsigned short> idx; idx.resize(count);
    for(int i=0;i<count;i++) idx[i]=(unsigned short)indices[i];
    drawIndexed(GL_TRIANGLES,count,&idx[0]);
}
void DrawLinesIndexed(int count,const unsigned short* indices){
    if(!ready || count<=0 || !indices) return;
    int nV=0; for(int i=0;i<count;i++) if((int)indices[i]+1>nV) nV=(int)indices[i]+1;
    setupState(nV); drawIndexed(GL_LINES,count,indices);
}
void DrawLineStripIndexed(int count,const unsigned short* indices){
    if(!ready || count<=0 || !indices) return;
    int nV=0; for(int i=0;i<count;i++) if((int)indices[i]+1>nV) nV=(int)indices[i]+1;
    setupState(nV); drawIndexed(GL_LINE_STRIP,count,indices);
}
void DrawTrianglesArray(int vertexCount){ if(!ready||vertexCount<=0) return; setupState(vertexCount); glDrawArrays(GL_TRIANGLES,0,vertexCount); }
void DrawTrianglesArrayFrom(int first,int count){ if(!ready||count<=0) return; setupState(first+count); glDrawArrays(GL_TRIANGLES,first,count); }
void DrawLines(int vertexCount){ if(!ready||vertexCount<=0) return; setupState(vertexCount); glDrawArrays(GL_LINES,0,vertexCount); }
void DrawLineStrip(int vertexCount){ if(!ready||vertexCount<=0) return; setupState(vertexCount); glDrawArrays(GL_LINE_STRIP,0,vertexCount); }
void DrawPoints(int vertexCount){ if(!ready||vertexCount<=0) return; setupState(vertexCount); glDrawArrays(GL_POINTS,0,vertexCount); }

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