// ============================================================================
//  w3dGLc3d.cpp -- las pocas funciones de OpenGL que el Core todavia llama directo (w3dTexture.cpp sube las texturas
//  con glTexImage2D; Light.cpp pone las luces con glLightfv), hechas sobre citro3d para la Nintendo 3DS. El resto del
//  motor pasa por w3dEngine (w3dGraphicsC3D.cpp). Los tipos y enums salen del GL/gl.h de platform/3ds/include.
//
//  TEXTURAS: la PICA las lee en bloques de 8x8 pixeles en orden Morton (z-order), en potencia de dos de 8 a 1024 por
//  lado. Aca se convierten desde lo que manda GL (RGBA de 8 bits casi siempre): a RGBA8, o a RGB565 si GL las pidio sin
//  alfa (UploadRGBA lo hace con las opacas: la mitad de memoria, sin perder nada visible en una imagen opaca).
// ============================================================================
#include <GL/gl.h>
#include "c3d/w3dC3DInterno.h"
#include "w3dGraphics.h"
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
struct Tex {
    C3D_Tex tex;
    bool conImagen;
    int w, h;
    GPU_TEXCOLOR fmt;
    GPU_TEXTURE_FILTER_PARAM magF, minF;
    GPU_TEXTURE_WRAP_PARAM wrapS, wrapT;
    Tex() : conImagen(false), w(0), h(0), fmt(GPU_RGBA8), magF(GPU_LINEAR), minF(GPU_LINEAR), wrapS(GPU_REPEAT), wrapT(GPU_REPEAT) {}
};
std::map<GLuint, Tex*> gTex;
GLuint gTexSig = 1;
GLuint gTexBind = 0;

Tex* Actual() {
    std::map<GLuint, Tex*>::iterator it = gTex.find(gTexBind);
    return it == gTex.end() ? NULL : it->second;
}

// el texel (x, y) de GL (y = fila de los datos, de abajo hacia arriba) adentro de la textura de la PICA
inline u32 Morton(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}
inline u32 IndiceTexel(int x, int y, int w, int h) {
    const int fy = h - 1 - y;   // (la PICA guarda las filas de arriba hacia abajo)
    return (u32)((fy >> 3) * (w >> 3) + (x >> 3)) * 64u + Morton((u32)(x & 7), (u32)(fy & 7));
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
int BytesPixel(GLenum format, GLenum type) {
    if (type != GL_UNSIGNED_BYTE) return 2;
    if (format == GL_RGBA) return 4;
    if (format == GL_RGB) return 3;
    if (format == GL_LUMINANCE_ALPHA) return 2;
    return 1;
}
void EscribirTexel(Tex& t, int x, int y, const u8* rgba) {
    u8* d = (u8*)t.tex.data;
    const u32 i = IndiceTexel(x, y, t.w, t.h);
    if (t.fmt == GPU_RGB565) {
        const u16 v = (u16)(((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) | (rgba[2] >> 3));
        ((u16*)d)[i] = v;
    } else {
        u8* p = d + i * 4;   // la PICA lee un u32 0xRRGGBBAA: en memoria A, B, G, R
        p[0] = rgba[3]; p[1] = rgba[2]; p[2] = rgba[1]; p[3] = rgba[0];
    }
}
bool EsPot(int v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; }
} // namespace

namespace w3dC3D {
C3D_Tex* Textura(unsigned int id) {
    std::map<GLuint, Tex*>::iterator it = gTex.find(id);
    return (it == gTex.end() || !it->second->conImagen) ? NULL : &it->second->tex;
}
}

// el bind del motor (w3dEngine::BindTexture) tambien elige la textura de estas funciones
void W3dC3DBindTextura(unsigned int id) { gTexBind = id; }

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
        if (it->second->conImagen) C3D_TexDelete(&it->second->tex);
        w3dEngine::C3DTexturaTocada(&it->second->tex);
        delete it->second;
        gTex.erase(it);
        if (gTexBind == textures[i]) gTexBind = 0;
    }
}
void glBindTexture(GLenum, GLuint texture) { gTexBind = texture; }

void glTexImage2D(GLenum, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint,
                  GLenum format, GLenum type, const GLvoid* pixels) {
    if (level != 0) return;   // (sin piramide: la PICA solo filtra bien las twiddled y aca no se usan mipmaps)
    Tex* t = Actual();
    if (!t || width <= 0 || height <= 0) return;
    if (!EsPot(width) || !EsPot(height)) return;   // (UploadRGBA ya las lleva a potencia de dos de 8 a 1024)
    const GPU_TEXCOLOR fmt = (internalformat == GL_RGB || internalformat == GL_RGB8 || internalformat == 3) ? GPU_RGB565 : GPU_RGBA8;
    if (t->conImagen && (t->w != width || t->h != height || t->fmt != fmt)) { C3D_TexDelete(&t->tex); t->conImagen = false; }
    if (!t->conImagen) {
        if (!C3D_TexInit(&t->tex, (u16)width, (u16)height, fmt)) return;
        t->conImagen = true; t->w = width; t->h = height; t->fmt = fmt;
        C3D_TexSetFilter(&t->tex, t->magF, t->minF);
        C3D_TexSetWrap(&t->tex, t->wrapS, t->wrapT);
    }
    w3dEngine::C3DTexturaTocada(&t->tex);
    if (!pixels) { memset(t->tex.data, 0, t->tex.size); C3D_TexFlush(&t->tex); return; }
    const u8* src = (const u8*)pixels;
    u8 rgba[4];
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            if (LeerPixel(src, format, type, y * width + x, rgba)) EscribirTexel(*t, x, y, rgba);
    C3D_TexFlush(&t->tex);
}
void glTexSubImage2D(GLenum, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const GLvoid* pixels) {
    if (level != 0 || !pixels) return;
    Tex* t = Actual();
    if (!t || !t->conImagen) return;
    const u8* src = (const u8*)pixels;
    u8 rgba[4];
    for (int y = 0; y < height; y++) {
        const int ty = yoffset + y;
        if (ty < 0 || ty >= t->h) continue;
        for (int x = 0; x < width; x++) {
            const int tx = xoffset + x;
            if (tx < 0 || tx >= t->w) continue;
            if (LeerPixel(src, format, type, y * width + x, rgba)) EscribirTexel(*t, tx, ty, rgba);
        }
    }
    (void)BytesPixel;
    C3D_TexFlush(&t->tex);
    w3dEngine::C3DTexturaTocada(&t->tex);
}
void glTexParameteri(GLenum, GLenum pname, GLint param) {
    Tex* t = Actual();
    if (!t) return;
    switch (pname) {
        case GL_TEXTURE_MAG_FILTER: t->magF = (param == GL_NEAREST) ? GPU_NEAREST : GPU_LINEAR; break;
        case GL_TEXTURE_MIN_FILTER: t->minF = (param == GL_NEAREST || param == GL_NEAREST_MIPMAP_NEAREST || param == GL_NEAREST_MIPMAP_LINEAR) ? GPU_NEAREST : GPU_LINEAR; break;
        case GL_TEXTURE_WRAP_S: t->wrapS = (param == GL_REPEAT) ? GPU_REPEAT : GPU_CLAMP_TO_EDGE; break;
        case GL_TEXTURE_WRAP_T: t->wrapT = (param == GL_REPEAT) ? GPU_REPEAT : GPU_CLAMP_TO_EDGE; break;
        default: return;
    }
    if (t->conImagen) {
        C3D_TexSetFilter(&t->tex, t->magF, t->minF);
        C3D_TexSetWrap(&t->tex, t->wrapS, t->wrapT);
        w3dEngine::C3DTexturaTocada(&t->tex);
    }
}
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) { glTexParameteri(target, pname, (GLint)param); }
void glPixelStorei(GLenum, GLint) {}
void glCopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) {}   // (las capturas de pantalla de los juegos: todavia no)
const GLubyte* glGetString(GLenum) { return (const GLubyte*)"citro3d (PICA200)"; }
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

// GLU: el camino de escritorio de LoadTexture la nombra; en la 3DS las texturas van por UploadRGBA
extern "C" int gluBuild2DMipmaps(GLenum, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) { return 1; }
