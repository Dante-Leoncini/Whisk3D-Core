// ============================================================================
//  w3dGLgu.cpp -- las pocas funciones de OpenGL que el Core todavia llama directo (w3dTexture.cpp sube las texturas
//  con glTexImage2D; Light.cpp pone las luces con glLightfv), hechas para el GE de la PSP. El resto del motor pasa por
//  w3dEngine (w3dGraphicsGu.cpp). Los tipos y enums salen del GL/gl.h de platform/psp/include.
//
//  TEXTURAS (ver graphics/gu/w3dGuInterno.h): se convierten desde lo que manda GL (RGBA de 8 bits casi siempre) a 8888, o a 565
//  si GL las pidio sin alfa (UploadRGBA lo hace con las opacas: la mitad de memoria). Van "swizzled" y en potencia de
//  dos de 8 a 512: una mas grande se achica a la mitad las veces que haga falta (promediando), una que no es potencia
//  de dos se lleva a la siguiente. Las filas quedan en el orden de GL (la primera de los datos en v = 0).
// ============================================================================
#include <GL/gl.h>
#include "w3dGuInterno.h"
#include "w3dGraphics.h"
#include <pspgu.h>
#include <psputils.h>
#include <malloc.h>
#include <map>
#include <vector>
#include <string.h>
#include <stdlib.h>

#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif
#ifndef GL_UNSIGNED_SHORT_5_5_5_1
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace {
typedef unsigned char u8;
typedef unsigned short u16;
const int kLadoMax = 512, kLadoMin = 8;

struct Tex {
    w3dGu::Textura t;
    int origW, origH;   // (el tamano que pidio GL: las actualizaciones parciales vienen en esas coordenadas)
    bool magLin, minLin, repS, repT;
    Tex() : origW(0), origH(0), magLin(true), minLin(true), repS(true), repT(true) {
        t.datos = NULL; t.w = 0; t.h = 0; t.psm = GU_PSM_8888; t.filtro = true; t.repetir = true; t.version = 0;
    }
};
std::map<GLuint, Tex*> gTex;
GLuint gTexSig = 1;
GLuint gTexBind = 0;
unsigned gVersion = 1;

Tex* Actual() {
    std::map<GLuint, Tex*>::iterator it = gTex.find(gTexBind);
    return it == gTex.end() ? NULL : it->second;
}

// un pixel de los datos de GL a RGBA de 8 bits
bool LeerPixel(const u8* src, GLenum format, GLenum type, int i, u8* rgba) {
    if (type == GL_UNSIGNED_BYTE) {
        if (format == GL_RGBA) { memcpy(rgba, src + i * 4, 4); return true; }
        if (format == GL_RGB) { const u8* p = src + i * 3; rgba[0] = p[0]; rgba[1] = p[1]; rgba[2] = p[2]; rgba[3] = 255; return true; }
        if (format == GL_LUMINANCE) { rgba[0] = rgba[1] = rgba[2] = src[i]; rgba[3] = 255; return true; }
        if (format == GL_ALPHA) { rgba[0] = rgba[1] = rgba[2] = 255; rgba[3] = src[i]; return true; }
        if (format == GL_LUMINANCE_ALPHA) { rgba[0] = rgba[1] = rgba[2] = src[i * 2]; rgba[3] = src[i * 2 + 1]; return true; }
        return false;
    }
    const u16 v = (u16)(src[i * 2] | (src[i * 2 + 1] << 8));
    if (type == GL_UNSIGNED_SHORT_5_6_5) {
        rgba[0] = (u8)(((v >> 11) & 31) * 255 / 31); rgba[1] = (u8)(((v >> 5) & 63) * 255 / 63); rgba[2] = (u8)((v & 31) * 255 / 31); rgba[3] = 255;
        return true;
    }
    if (type == GL_UNSIGNED_SHORT_5_5_5_1) {
        rgba[0] = (u8)(((v >> 11) & 31) * 255 / 31); rgba[1] = (u8)(((v >> 6) & 31) * 255 / 31); rgba[2] = (u8)(((v >> 1) & 31) * 255 / 31); rgba[3] = (v & 1) ? 255 : 0;
        return true;
    }
    if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
        rgba[0] = (u8)(((v >> 12) & 15) * 17); rgba[1] = (u8)(((v >> 8) & 15) * 17); rgba[2] = (u8)(((v >> 4) & 15) * 17); rgba[3] = (u8)((v & 15) * 17);
        return true;
    }
    return false;
}
int BytesPsm(int psm) { return psm == GU_PSM_8888 ? 4 : 2; }
// el texel (x, y) en la textura "swizzled": bloques de 16 bytes x 8 filas, uno detras del otro
inline size_t Desplazamiento(int x, int y, int anchoBytes, int bpp) {
    const int bx = x * bpp;
    return ((size_t)(y >> 3) * (size_t)(anchoBytes >> 4) + (size_t)(bx >> 4)) * 128u + (size_t)(y & 7) * 16u + (size_t)(bx & 15);
}
inline void EscribirTexel(Tex& t, int x, int y, const u8* rgba) {
    const int bpp = BytesPsm(t.t.psm);
    u8* d = t.t.datos + Desplazamiento(x, y, t.t.w * bpp, bpp);
    if (t.t.psm == GU_PSM_8888) { d[0] = rgba[0]; d[1] = rgba[1]; d[2] = rgba[2]; d[3] = rgba[3]; }
    else {   // (565 del GE: el rojo en los bits de abajo)
        const u16 v = (u16)((rgba[0] >> 3) | ((rgba[1] >> 2) << 5) | ((rgba[2] >> 3) << 11));
        d[0] = (u8)(v & 0xFF); d[1] = (u8)(v >> 8);
    }
}
int PotSig(int v) { int p = kLadoMin; while (p < v && p < kLadoMax) p <<= 1; return p; }
// la imagen de GL (RGBA 8 bits, w x h) llevada al tamano que acepta el GE: a la mitad (promediando) mientras pase de
// 512 y despues, si no es potencia de dos, a la siguiente (por vecino)
void AjustarTamano(std::vector<u8>& img, int& w, int& h) {
    while (w > kLadoMax || h > kLadoMax) {
        const int nw = w > kLadoMax ? w / 2 : w, nh = h > kLadoMax ? h / 2 : h;
        const int fx = w / nw, fy = h / nh;
        std::vector<u8> o((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int k = 0; k < 4; k++) {
                    int s = 0;
                    for (int j = 0; j < fy; j++) for (int i = 0; i < fx; i++) s += img[((size_t)(y * fy + j) * w + (x * fx + i)) * 4 + k];
                    o[((size_t)y * nw + x) * 4 + k] = (u8)(s / (fx * fy));
                }
        img.swap(o); w = nw; h = nh;
    }
    const int pw = PotSig(w), ph = PotSig(h);
    if (pw == w && ph == h) return;
    std::vector<u8> o((size_t)pw * ph * 4);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++)
            memcpy(&o[((size_t)y * pw + x) * 4], &img[((size_t)(y * h / ph) * w + (x * w / pw)) * 4], 4);
    img.swap(o); w = pw; h = ph;
}
void Tocada(Tex& t) { t.t.version = ++gVersion; }
void PonerParametros(Tex& t) {
    t.t.filtro = t.magLin;
    t.t.repetir = t.repS;
    Tocada(t);
}
} // namespace

namespace w3dGu {
const Textura* TexturaDe(unsigned int id) {
    std::map<GLuint, Tex*>::iterator it = gTex.find(id);
    return (it == gTex.end() || !it->second->t.datos) ? NULL : &it->second->t;
}
}

// el bind del motor (w3dEngine::BindTexture) tambien elige la textura de estas funciones
void W3dGuBindTextura(unsigned int id) { gTexBind = id; }

// el filtro y la repeticion de la textura activa (w3dGraphics.h), como glTexParameter
namespace w3dEngine {
void TexFilter(bool linear) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
}
void TexWrap(bool repeat) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}
}

extern "C" {

void glGenTextures(GLsizei n, GLuint* textures) {
    for (GLsizei i = 0; i < n; i++) { textures[i] = gTexSig++; gTex[textures[i]] = new Tex(); }
}
void glDeleteTextures(GLsizei n, const GLuint* textures) {
    for (GLsizei i = 0; i < n; i++) {
        std::map<GLuint, Tex*>::iterator it = gTex.find(textures[i]);
        if (it == gTex.end()) continue;
        w3dGu::LiberarDespues(it->second->t.datos);
        delete it->second;
        gTex.erase(it);
        if (gTexBind == textures[i]) gTexBind = 0;
    }
}
void glBindTexture(GLenum, GLuint texture) { gTexBind = texture; }

void glTexImage2D(GLenum, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint,
                  GLenum format, GLenum type, const GLvoid* pixels) {
    if (level != 0) return;   // (sin piramide)
    Tex* t = Actual();
    if (!t || width <= 0 || height <= 0) return;
    // a RGBA de 8 bits y al tamano del GE
    std::vector<u8> img((size_t)width * height * 4, 0);
    if (pixels) {
        const u8* src = (const u8*)pixels;
        for (int i = 0; i < width * height; i++) if (!LeerPixel(src, format, type, i, &img[(size_t)i * 4])) img[(size_t)i * 4 + 3] = 255;
    }
    int w = width, h = height;
    AjustarTamano(img, w, h);
    const int psm = (internalformat == GL_RGB || internalformat == GL_RGB8 || internalformat == 3) ? GU_PSM_5650 : GU_PSM_8888;
    const size_t bytes = (size_t)w * h * BytesPsm(psm);
    if (t->t.datos && (t->t.w != w || t->t.h != h || t->t.psm != psm)) { w3dGu::LiberarDespues(t->t.datos); t->t.datos = NULL; }
    if (!t->t.datos) {
        t->t.datos = (u8*)memalign(16, bytes);
        if (!t->t.datos) return;
        t->t.w = w; t->t.h = h; t->t.psm = psm;
    }
    t->origW = width; t->origH = height;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) EscribirTexel(*t, x, y, &img[((size_t)y * w + x) * 4]);
    sceKernelDcacheWritebackRange(t->t.datos, (unsigned)bytes);   // (el GE lee la memoria, no el cache de la CPU)
    w3dEngine::g_statUploadBytes += (int)bytes;
    PonerParametros(*t);
}
void glTexSubImage2D(GLenum, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const GLvoid* pixels) {
    if (level != 0 || !pixels) return;
    Tex* t = Actual();
    if (!t || !t->t.datos || t->origW <= 0 || t->origH <= 0) return;
    const u8* src = (const u8*)pixels;
    u8 rgba[4];
    // (si la textura se achico o se agrando, cada pixel de GL cae en el texel que le toca)
    const bool igual = t->origW == t->t.w && t->origH == t->t.h;
    for (int y = 0; y < height; y++) {
        const int gy = yoffset + y;
        if (gy < 0 || gy >= t->origH) continue;
        const int ty = igual ? gy : gy * t->t.h / t->origH;
        for (int x = 0; x < width; x++) {
            const int gx = xoffset + x;
            if (gx < 0 || gx >= t->origW) continue;
            const int tx = igual ? gx : gx * t->t.w / t->origW;
            if (LeerPixel(src, format, type, y * width + x, rgba)) EscribirTexel(*t, tx, ty, rgba);
        }
    }
    sceKernelDcacheWritebackRange(t->t.datos, (unsigned)((size_t)t->t.w * t->t.h * BytesPsm(t->t.psm)));
    Tocada(*t);
}
void glTexParameteri(GLenum, GLenum pname, GLint param) {
    Tex* t = Actual();
    if (!t) return;
    switch (pname) {
        case GL_TEXTURE_MAG_FILTER: t->magLin = (param != GL_NEAREST); break;
        case GL_TEXTURE_MIN_FILTER: t->minLin = !(param == GL_NEAREST || param == GL_NEAREST_MIPMAP_NEAREST || param == GL_NEAREST_MIPMAP_LINEAR); break;
        case GL_TEXTURE_WRAP_S: t->repS = (param == GL_REPEAT); break;
        case GL_TEXTURE_WRAP_T: t->repT = (param == GL_REPEAT); break;
        default: return;
    }
    PonerParametros(*t);
}
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) { glTexParameteri(target, pname, (GLint)param); }
void glPixelStorei(GLenum, GLint) {}
void glCopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) {}   // (las capturas de pantalla de los juegos: todavia no)
const GLubyte* glGetString(GLenum) { return (const GLubyte*)"sceGu (GE de la PSP)"; }
void glGetFloatv(GLenum, GLfloat* params) { if (params) params[0] = 0.0f; }
void glGetIntegerv(GLenum, GLint* params) { if (params) params[0] = 0; }
GLenum glGetError(void) { return 0; }
void glGenerateMipmap(GLenum) {}

// ---- luces (Light.cpp): van a la API del motor. El cono: el corte y el exponente llegan antes que la direccion ----
static float gCorte[8] = { 180, 180, 180, 180, 180, 180, 180, 180 }, gExpo[8] = { 0 };
void glLightfv(GLenum light, GLenum pname, const GLfloat* params) {
    const int i = (int)light - (int)GL_LIGHT0;
    if (i < 0 || i >= 8 || !params) return;
    switch (pname) {
        case GL_POSITION: w3dEngine::LightNfv(i, w3dEngine::LightPosition, params); break;
        case GL_DIFFUSE:  w3dEngine::LightNfv(i, w3dEngine::LightDiffuse, params); break;
        case GL_AMBIENT:  w3dEngine::LightNfv(i, w3dEngine::LightAmbient, params); break;
        case GL_SPECULAR: w3dEngine::LightNfv(i, w3dEngine::LightSpecular, params); break;
        case GL_SPOT_DIRECTION: w3dEngine::LightNSpot(i, gCorte[i], gExpo[i], params); break;
        default: break;
    }
}
void glLightf(GLenum light, GLenum pname, GLfloat param) {
    const int i = (int)light - (int)GL_LIGHT0;
    if (i < 0 || i >= 8) return;
    switch (pname) {
        case GL_CONSTANT_ATTENUATION:  w3dEngine::LightNf(i, w3dEngine::LightConstantAtt, param); break;
        case GL_LINEAR_ATTENUATION:    w3dEngine::LightNf(i, w3dEngine::LightLinearAtt, param); break;
        case GL_QUADRATIC_ATTENUATION: w3dEngine::LightNf(i, w3dEngine::LightQuadraticAtt, param); break;
        case GL_SPOT_CUTOFF:   gCorte[i] = param; break;
        case GL_SPOT_EXPONENT: gExpo[i] = param; break;
        default: break;
    }
}
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) { w3dEngine::Rotatef(angle, x, y, z); }

} // extern "C"

// GLU: el camino de escritorio de LoadTexture la nombra; en la PSP las texturas van por UploadRGBA
extern "C" int gluBuild2DMipmaps(GLenum, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) { return 1; }
