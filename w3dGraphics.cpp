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
    #endif
    #include <GL/gl.h>
#endif
#include <math.h> // tanf (Perspective)

#ifndef GL_CLAMP_TO_EDGE
    #define GL_CLAMP_TO_EDGE 0x812F // GL 1.2+: por si el GL/gl.h del sistema es 1.1
#endif
#ifndef GL_POINT_SPRITE
    #define GL_POINT_SPRITE 0x8861 // GL 1.4/OES: el GL/gl.h del sistema (1.1) no lo trae
#endif
#ifndef GL_COORD_REPLACE
    #define GL_COORD_REPLACE 0x8862 // idem (point sprite coord replace)
#endif
#ifndef GL_MULTISAMPLE
    #define GL_MULTISAMPLE 0x809D // GL 1.3/ES1: apagar el MSAA durante el color-ID pick
#endif

namespace w3dEngine {

// ============================================================================
//  CACHE de estado: evita llamadas GL redundantes (cambiar a lo que ya esta
//  puesto no hace nada). Valido solo si TODO el estado pasa por el motor; tras
//  GL directo, Invalidate() resincroniza.
// ============================================================================
static const int kNumCaps = 14; // = cantidad de valores del enum Cap
static bool gCapOn[kNumCaps] = { false };    // estado que el motor cree puesto
static bool gCapKnown[kNumCaps] = { false }; // false = no lo conocemos -> forzar la llamada
static unsigned int gTexBound = 0;           // estado inicial de GL: sin textura

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

void DepthFunc(DepthCmp c) { 
    glDepthFunc(c == DepthEqual ? GL_EQUAL : (c == DepthLEqual ? GL_LEQUAL : GL_LESS)); 
}

void Enable(Cap c) {
    if (gCapKnown[c] && gCapOn[c]) { return; } // ya prendida y la conocemos -> nada
    gCapOn[c] = true; gCapKnown[c] = true;
    glEnable(CapGL(c));
}

void Disable(Cap c) {
    if (gCapKnown[c] && !gCapOn[c]) { return; } // ya apagada y la conocemos -> nada
    gCapOn[c] = false; gCapKnown[c] = true;
    glDisable(CapGL(c));
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
void EnableArray(VArray a)  { glEnableClientState(ArrayGL(a)); }
void DisableArray(VArray a) { glDisableClientState(ArrayGL(a)); }

// ---- buffers ----
void ClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
void Clear(int bits) {
    GLbitfield m = 0;
    if (bits & ColorBuffer) { m |= GL_COLOR_BUFFER_BIT; }
    if (bits & DepthBuffer) { m |= GL_DEPTH_BUFFER_BIT; }
    glClear(m);
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
    #ifdef W3D_SYMBIAN
        glOrthof(l, r, b, t, n, f);
    #else
        glOrtho(l, r, b, t, n, f);
    #endif
}

void Frustum(float l, float r, float b, float t, float n, float f) {
    #ifdef W3D_SYMBIAN
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
void DepthMask(bool escribir)  { glDepthMask(escribir ? GL_TRUE : GL_FALSE); }
void LineWidth(float px)       { glLineWidth(px); }
void SmoothShading(bool suave) { glShadeModel(suave ? GL_SMOOTH : GL_FLAT); }
void FastPerspective()         { glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST); }

// ---- textura activa (cacheada) ----
void BindTexture(unsigned int id) {
    if (id == gTexBound) { return; } // ya esta bindeada -> ahorramos el bind
    gTexBound = id;
    glBindTexture(GL_TEXTURE_2D, id);
}

unsigned int BoundTexture() { return gTexBound; }

// CHROME: genera las UV con sphere-map del pipeline fijo (PC). En GLES1 (Symbian) no hay glTexGen ->
// stub (calcular las UV del chrome por software a partir del normal y la camara).
void TexGenSphere(bool on) {
#ifdef W3D_SYMBIAN
    (void)on;
#else
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

// ---- color uniforme ----
void FrontFace(bool ccw) { glFrontFace(ccw ? GL_CCW : GL_CW); }
void ColorMask(bool r, bool g, bool b, bool a) { glColorMask(r, g, b, a); }
void PointSpriteCoordReplace(bool on) { glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, on ? GL_TRUE : GL_FALSE); }

void Color4f(float r, float g, float b, float a) { glColor4f(r, g, b, a); }
// glColor4fv NO existe en GLES1 (solo glColor4f/4ub/4x): expandir a glColor4f (los 2 OK)
void Color4fv(const float* c) { glColor4f(c[0], c[1], c[2], c[3]); }

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
void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { glColor4ub(r, g, b, a); }

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
#ifdef ANDROID
    GLfixed x[4]; // 16.16: float * 65536
    for (int i = 0; i < 4; i++) x[i] = (GLfixed)(rgba[i] * 65536.0f);
    glMaterialxv(GL_FRONT_AND_BACK, MatParamGL(p), x);
#else
    glMaterialfv(GL_FRONT_AND_BACK, MatParamGL(p), rgba);
#endif
}
void MaterialShininess(float s) {
#ifdef ANDROID
    glMaterialx(GL_FRONT_AND_BACK, GL_SHININESS, (GLfixed)(s * 65536.0f));
#else
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, s);
#endif
}

void BlendAlpha() { glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
void BlendMode(int modo) { // capa multi-pass sobre lo de abajo
    if (modo == 1)      glBlendFunc(GL_DST_COLOR, GL_ZERO);                 // Multiply (oscurece)
    else if (modo == 2) glBlendFunc(GL_ONE, GL_ONE);                       // Add (aclara)
    else                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Mix (alpha encima)
}

// ---- parametros de la textura activa ----
void TexFilter(bool linear) {
    GLint f = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}
void TexWrap(bool repeat) {
    GLfloat w = (GLfloat)(repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, w);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, w);
}

// ---- punteros de los arrays ----
void VertexPointer2f(int strideBytes, const float* p)   { glVertexPointer(2, GL_FLOAT, strideBytes, p); }
void VertexPointer3f(int strideBytes, const float* p)   { glVertexPointer(3, GL_FLOAT, strideBytes, p); }
void VertexPointer3s(int strideBytes, const short* p)   { glVertexPointer(3, GL_SHORT, strideBytes, p); }
void VertexPointer2s(int strideBytes, const short* p)   { glVertexPointer(2, GL_SHORT, strideBytes, p); }
void ColorPointer4ub(const unsigned char* p)            { glColorPointer(4, GL_UNSIGNED_BYTE, 0, p); }
void NormalPointer3b(const signed char* p)              { glNormalPointer(GL_BYTE, 0, p); }
void TexCoordPointer2f(int strideBytes, const float* p) { glTexCoordPointer(2, GL_FLOAT, strideBytes, p); }
void TexCoordPointer3b(const signed char* p)            { glTexCoordPointer(3, GL_BYTE, 0, p); } // normales como texcoords (matcap HW)

// ---- dibujo de triangulos indexados ----
void DrawTriangles(int count, const MeshIndex* indices) {
#ifdef W3D_SYMBIAN
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, indices); // GLES1.1: solo 16 bits
#else
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, indices);   // PC/Android/WebGL/N8: 32 bits
#endif
}
// triangulos NO indexados (glDrawArrays): la UI 2D arma sus verts en orden
void DrawTrianglesArray(int vertexCount) {
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
}
void DrawTrianglesArrayFrom(int first, int count) { // glDrawArrays con offset (chrome equirect por-corner)
    glDrawArrays(GL_TRIANGLES, first, count);
}
void DrawTrianglesByte(int count, const unsigned char* indices) {
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_BYTE, indices);
}

// lineas sueltas (overlay de normales): dibuja vertexCount vertices de a pares
void DrawLines(int vertexCount) {
    glDrawArrays(GL_LINES, 0, vertexCount);
}
void DrawLineStrip(int vertexCount) {
    glDrawArrays(GL_LINE_STRIP, 0, vertexCount);
}
void DrawLineStripIndexed(int count, const unsigned short* indices) {
    glDrawElements(GL_LINE_STRIP, count, GL_UNSIGNED_SHORT, indices);
}

// puntos (vertices en Edit Mode). glDrawArrays(GL_POINTS) anda en la fase 3D
// (no es la fase 2D rota del N95, y no usa glDrawElements)
void DrawPoints(int vertexCount) {
    glDrawArrays(GL_POINTS, 0, vertexCount);
}
void PointSize(float px) { glPointSize(px); }

// lineas INDEXADAS (bordes de edit mode con vertex color compartido): en la fase
// 3D glDrawElements funciona (la malla rellena ya lo usa para los triangulos).
void DrawLinesIndexed(int count, const unsigned short* indices) {
    glDrawElements(GL_LINES, count, GL_UNSIGNED_SHORT, indices);
}

// offset de profundidad del relleno (slope-aware). Negativo = lo tira hacia la
// camara; se usa para el contorno: los rellenos tapan las lineas internas.
void PolygonOffset(float factor, float units) { glPolygonOffset(factor, units); }

// empuja el rango de profundidad (contorno: las lineas internas quedan tapadas
// por la malla y solo se ve el borde/silueta)
void DepthRange(float n, float f) {
#ifdef W3D_SYMBIAN
    glDepthRangef(n, f);
#else
    glDepthRange(n, f);
#endif
}

// ---- wireframe (solo GL de escritorio tiene glPolygonMode) ----
void Wireframe(bool on) {
#if !defined(W3D_SYMBIAN) && !defined(ANDROID)
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
bool w3dRenderSolido    = false;
bool w3dRenderSinLuz    = false;
bool w3dRenderLuces     = false;

bool w3dRenderNormalColor = false;
bool w3dRenderOverlays    = true;
