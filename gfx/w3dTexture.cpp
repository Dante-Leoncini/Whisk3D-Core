// ============================================================================
//  Whisk3DCore (engine) — cargador de texturas UNIVERSAL
//  Ver w3dTexture.h y ARQUITECTURA.md.
// ============================================================================

#include "w3dTexture.h"
#include "w3dGraphics.h"   // rutear binds por el cache del motor (sino queda desincronizado)
#include "w3dlog.h"
#include <map> // id de textura -> (w,h) para el aspect ratio en el UV editor
#include <stdio.h>  // fopen/fwrite: escritura del PNG (PC/Web; Symbian = TODO via RFile)
#include <string.h> // memcpy en el encoder PNG
#include <errno.h>

// --- header de gráficos del backend ---
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>            // N95: OpenGL ES 1.1
#elif defined(__EMSCRIPTEN__)
    #include <GLES2/gl2.h>          // WebGL / OpenGL ES 2.0 (sin GLU)
    #ifdef W3D_STB_IMPL
        #define STB_IMAGE_IMPLEMENTATION
    #endif
    #include "stb/stb_image.h"
#else
    #ifdef _WIN32
        #define WIN32_LEAN_AND_MEAN
        #include <windows.h>        // requerido antes de GL/gl.h en MSVC
    #endif
    #include <GL/gl.h>              // PC: OpenGL de escritorio
    #ifndef __ANDROID__
        #include <GL/glu.h>         // gluBuild2DMipmaps (mipmaps en escritorio)
    #endif
    // stb_image: DECLARACIONES; la IMPLEMENTACION la compila el consumidor con
    // STB_IMAGE_IMPLEMENTATION (el editor en main.cpp; los ejemplos con -DW3D_STB_IMPL).
    #ifdef W3D_STB_IMPL
        #define STB_IMAGE_IMPLEMENTATION
    #endif
    #include "stb/stb_image.h"
#endif

#ifdef __ANDROID__
// Android: el shim GL/gl.h trae el header de ES 1.1 (tipos y enums del backend fixed-function del core),
// pero el contexto es ES 2.0 y se linkea libGLESv2, que exporta glGenerateMipmap (core en ES2). El header
// de ES1 no lo declara -> se declara aca. (Sin esto no compilaba: "use of undeclared identifier".)
extern "C" GL_API void GL_APIENTRY glGenerateMipmap(GLenum target);
#endif

// FILTRADO ANISOTROPICO (solo PC de escritorio, GL_EXT_texture_filter_anisotropic): con mipmaps el piso y los caminos
// vistos en angulo rasante se borroneaban (el trilineal promedia en cuadrado). Con 8x se ven nitidos casi gratis en
// cualquier GPU de PC. El N95 (ES 1.1), Android y la web quedan como estaban.
#if !defined(W3D_SYMBIAN) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
static void w3dAnisotropico() {
    static float maximo = -1.0f;
    if (maximo < 0.0f) {
        maximo = 0.0f;
        const char* ext = (const char*)glGetString(GL_EXTENSIONS);
        if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic")) {
            GLfloat m = 0.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &m);
            maximo = m < 8.0f ? m : 8.0f;
        }
    }
    if (maximo > 1.0f) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maximo);
}
#else
static void w3dAnisotropico() {}
#endif
#include "w3dFilesystem.h" // lectura de archivos UNIFICADA del Core (asset del APK o archivo real)
#include <vector>
#include <set>

// GL_CLAMP_TO_EDGE es GL 1.2+; el <GL/gl.h> 1.1 de Windows no lo declara.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
// GL_GENERATE_MIPMAP es GL 1.4+ / ES 1.1; el <GL/gl.h> 1.1 de Windows no lo declara.
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif
#ifndef GL_UNSIGNED_SHORT_5_5_5_1
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#endif
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif

namespace w3dEngine {

// PIXELADO GLOBAL de la escena 3D (ver w3dGraphics.h). Vive ACA y no en el
// backend porque los dos backends (pipeline fijo y GLES2) comparten este
// archivo: asi hay UNA sola definicion para todas las plataformas.
static bool g_pixeladoGlobal = false;
bool PixeladoGlobal()          { return g_pixeladoGlobal; }
void SetPixeladoGlobal(bool on){ g_pixeladoGlobal = on; }

// MIPMAPPING GLOBAL (pedido del dueno: "este proyecto tiene que tener mipmapping").
// Default true; el proyecto lo puede apagar con `mipmaps: false` en la cabecera del
// .w3d. Con pixelado la piramide va NEAREST_MIPMAP_LINEAR: conserva el look pixel
// perfect de cerca y de lejos usa los niveles chicos (menos ruido y menos ancho de
// banda de textura, clave en el N95). Cada piramide cuesta +33% de memoria.
static bool g_mipmapsGlobal = true;
bool MipmapsGlobal()          { return g_mipmapsGlobal; }
void SetMipmapsGlobal(bool on){ g_mipmapsGlobal = on; }

// que texturas subieron PIRAMIDE: pedirle un min-filter mipmap a una textura sin
// piramide la vuelve INCOMPLETA (negra), asi que TexFilter consulta aca antes.
// (por id en un vector: GL da ids chicos y seguidos y esto se pregunta en cada bind; un id enorme -raro- va al set)
static const unsigned int kTexIdsDirectos = 65536;
static std::vector<unsigned char> g_texConMipsVec;
static std::set<unsigned int> g_texConMips;
bool TexTieneMips(unsigned int id) {
    if (id < kTexIdsDirectos) return id < g_texConMipsVec.size() && g_texConMipsVec[id] != 0;
    return g_texConMips.count(id) != 0;
}
static void TexMipsRegistrar(unsigned int id, bool con) {
    if (id < kTexIdsDirectos) {
        if (id >= g_texConMipsVec.size()) { if (!con) return; g_texConMipsVec.resize(id + 1, 0); }
        g_texConMipsVec[id] = con ? 1 : 0;
        return;
    }
    if (con) g_texConMips.insert(id); else g_texConMips.erase(id);
}

// ---------------------------------------------------------------------------
//  CACHE de PARAMETROS por textura (min/mag filter + wrap). glTexParameter es
//  estado DEL OBJETO textura (persiste con la textura, no con el contexto):
//  re-mandar el mismo filtro/wrap en cada malla que la usa era churn puro de
//  driver (con 16 mallas del mismo material: 64 glTexParameter identicos por
//  frame). TexFilter/TexWrap (w3dGraphics.cpp) preguntan aca antes de llamar.
//  La 'firma' es un entero opaco que arma el caller (filtro: bit0 = linear,
//  bit1 = con piramide; wrap: 0/1 = clamp/repeat). -1 = desconocida.
//  DeleteTexture OLVIDA la entrada: GL recicla los ids y una textura nueva no
//  puede heredar la firma de la borrada.
// ---------------------------------------------------------------------------
struct TexParamEstado {
    signed char filtro, wrap;
    TexParamEstado() : filtro(-1), wrap(-1) {}
};
static std::vector<TexParamEstado> g_texParamsVec;           // (por id, como g_texConMipsVec)
static std::map<unsigned int, TexParamEstado> g_texParams;   // los ids enormes
static TexParamEstado& TexParams(unsigned int id) {
    if (id < kTexIdsDirectos) {
        if (id >= g_texParamsVec.size()) g_texParamsVec.resize(id + 1);
        return g_texParamsVec[id];
    }
    return g_texParams[id];
}

bool TexFiltroCambia(unsigned int id, int firma) {
    TexParamEstado& e = TexParams(id);
    if (e.filtro == (signed char)firma) return false;
    e.filtro = (signed char)firma;
    return true;
}
bool TexWrapCambia(unsigned int id, int firma) {
    TexParamEstado& e = TexParams(id);
    if (e.wrap == (signed char)firma) return false;
    e.wrap = (signed char)firma;
    return true;
}
void TexParamsRegistrar(unsigned int id, int firmaFiltro, int firmaWrap) {
    TexParamEstado& e = TexParams(id);
    e.filtro = (signed char)firmaFiltro;
    e.wrap = (signed char)firmaWrap;
}
void TexParamsOlvidar(unsigned int id) {
    if (id < kTexIdsDirectos) { if (id < g_texParamsVec.size()) g_texParamsVec[id] = TexParamEstado(); }
    else g_texParams.erase(id);
}

// dimensiones de cada textura subida (id GL -> w,h). Lo llena UploadRGBA, por la que pasan TODOS los
// uploads (PC stb + Symbian ICL + procedural) -> el UV editor lee el aspect ratio sin tocar la struct Texture.
static std::map<unsigned int, std::pair<int,int> > g_texSizes;
// la MEMORIA de cada textura subida (bytes con mips) y si es de 16 bits: TexturasMemoria
struct TexMem { long bytes, bytes32, bytesPot; bool de16; };
static std::map<unsigned int, TexMem> g_texMem;
static int PotSig(int v) { int p = 1; while (p < v && p < 1024) p <<= 1; return p; }
// 'mipsPot': si en el N95 llevaria piramide (ahi todas quedan potencia de dos: los materiales si, la UI 2D no)
static void TexMemAnotar(unsigned int id, int w, int h, int bpp, bool mips, bool mipsPot) {
    long b = (long)w * h * bpp, b32 = (long)w * h * 4;
    // como en el N95: cada textura remuestreada a la POTENCIA DE DOS siguiente (tope 1024)
    long bp = (long)PotSig(w) * PotSig(h) * bpp;
    if (mips) { b += b / 3; b32 += b32 / 3; }
    if (mipsPot) bp += bp / 3;
    TexMem m; m.bytes = b; m.bytes32 = b32; m.bytesPot = bp; m.de16 = (bpp == 2);
    g_texMem[id] = m;
}
bool TexturaMemoriaDe(unsigned int id, long& bytes, long& bytesPot, bool& de16) {
    std::map<unsigned int, TexMem>::const_iterator it = g_texMem.find(id);
    if (it == g_texMem.end()) return false;
    bytes = it->second.bytes; bytesPot = it->second.bytesPot; de16 = it->second.de16;
    return true;
}
MemoriaTexturas TexturasMemoria() {
    MemoriaTexturas r; r.bytes = 0; r.bytes32 = 0; r.bytesPot = 0; r.cantidad = 0; r.de16 = 0;
    for (std::map<unsigned int, TexMem>::const_iterator it = g_texMem.begin(); it != g_texMem.end(); ++it) {
        r.bytes += it->second.bytes; r.bytes32 += it->second.bytes32; r.bytesPot += it->second.bytesPot;
        r.cantidad++; if (it->second.de16) r.de16++;
    }
    return r;
}
// 16 BITS SIN PERDIDA VISIBLE: cada canal se cuantiza (5 o 6 bits) y se vuelve a expandir; si TODOS quedan a <= 2/255
// de su valor (la imagen ya era de pocos niveles: las texturas de la ROM, de 15 bits; la UI de colores planos) se
// empaca. Opaca -> 565; alfa de un bit (0 o 255) -> 5551 (el color de un texel transparente no se mira). Devuelve el
// tipo GL (0 = no entra: se sube RGBA de 8 bits) y el formato (GL_RGB / GL_RGBA)
static inline bool Canal(int v, int bits, int& q) {
    const int maxq = (1 << bits) - 1;
    q = (v * maxq + 127) / 255;
    const int back = (q * 255 + maxq / 2) / maxq;
    const int d = back - v;
    return d <= 2 && d >= -2;
}
static bool g_tex16Siempre = false;
bool Texturas16Siempre() { return g_tex16Siempre; }
void SetTexturas16Siempre(bool on) { g_tex16Siempre = on; }
static bool g_tex16Movil = false;
bool Texturas16Movil() { return g_tex16Movil; }
void SetTexturas16Movil(bool on) { g_tex16Movil = on; }

static int g_texMitadDesde = 0;
int TexturasMitadDesde() { return g_texMitadDesde; }
void SetTexturasMitadDesde(int lado) { g_texMitadDesde = lado > 0 ? lado : 0; }
// promedio de cada 2x2 (un lado impar repite su ultima fila/columna). El COLOR pesado por el alfa: un texel
// transparente (su color no se ve: suele ser negro) no oscurece el borde. Si el alfa era de un bit (0 / 255) sigue de
// un bit (corte en la mitad, como el alfa test): asi entra en 5551 y no pasa a 8 bits por los bordes promediados
unsigned char* ReducirMitadRGBA(const unsigned char* rgba, int w, int h, int& ow, int& oh) {
    if (!rgba || w <= 0 || h <= 0) return NULL;
    ow = (w + 1) / 2; oh = (h + 1) / 2;
    bool alfaBit = true;
    for (size_t i = 0, n = (size_t)w * h; i < n && alfaBit; i++) alfaBit = rgba[i * 4 + 3] == 0 || rgba[i * 4 + 3] == 255;
    unsigned char* out = new unsigned char[(size_t)ow * oh * 4];
    if (!out) return NULL;
    for (int y = 0; y < oh; y++) {
        const int y0 = 2 * y, y1 = (2 * y + 1 < h) ? 2 * y + 1 : 2 * y;
        for (int x = 0; x < ow; x++) {
            const int x0 = 2 * x, x1 = (2 * x + 1 < w) ? 2 * x + 1 : 2 * x;
            const unsigned char* q[4] = { rgba + ((size_t)y0 * w + x0) * 4, rgba + ((size_t)y0 * w + x1) * 4,
                                          rgba + ((size_t)y1 * w + x0) * 4, rgba + ((size_t)y1 * w + x1) * 4 };
            int a = 0, r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) { a += q[k][3]; r += q[k][0] * q[k][3]; g += q[k][1] * q[k][3]; b += q[k][2] * q[k][3]; }
            unsigned char* o = out + ((size_t)y * ow + x) * 4;
            if (a > 0) { o[0] = (unsigned char)(r / a); o[1] = (unsigned char)(g / a); o[2] = (unsigned char)(b / a); }
            else {
                o[0] = (unsigned char)((q[0][0] + q[1][0] + q[2][0] + q[3][0]) / 4);
                o[1] = (unsigned char)((q[0][1] + q[1][1] + q[2][1] + q[3][1]) / 4);
                o[2] = (unsigned char)((q[0][2] + q[1][2] + q[2][2] + q[3][2]) / 4);
            }
            o[3] = alfaBit ? (unsigned char)(a >= 2 * 255 ? 255 : 0) : (unsigned char)((a + 2) / 4);
        }
    }
    return out;
}
static GLenum Empacar16(const unsigned char* rgba, int w, int h, std::vector<unsigned short>& out, GLenum& formato) {
#ifdef W3D_SYMBIAN
    const bool siempre = g_tex16Siempre || g_tex16Movil;   // (el proyecto acepta el error de cuantizar)
#else
    const bool siempre = g_tex16Siempre;   // (el proyecto acepta el error de cuantizar)
#endif
    const size_t n = (size_t)w * h;
    bool opaca = true;
    for (size_t i = 0; i < n; i++) {
        const int a = rgba[i * 4 + 3];
        if (a != 255) { opaca = false; if (a != 0) return 0; }
    }
    out.resize(n);
    int r, g, b;
    for (size_t i = 0; i < n; i++) {
        const unsigned char* p = rgba + i * 4;
        if (opaca) {
            const bool ok = Canal(p[0], 5, r) & Canal(p[1], 6, g) & Canal(p[2], 5, b);
            if (!ok && !siempre) return 0;
            out[i] = (unsigned short)((r << 11) | (g << 5) | b);
        } else {
            const bool ve = p[3] != 0;
            const bool ok = Canal(p[0], 5, r) & Canal(p[1], 5, g) & Canal(p[2], 5, b);
            if (!ok && ve && !siempre) return 0;
            out[i] = (unsigned short)((r << 11) | (g << 6) | (b << 1) | (ve ? 1 : 0));
        }
    }
    formato = opaca ? GL_RGB : GL_RGBA;
    return opaca ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_SHORT_5_5_5_1;
}
bool TextureSize(unsigned int id, int& w, int& h) {
    std::map<unsigned int, std::pair<int,int> >::iterator it = g_texSizes.find(id);
    if (it == g_texSizes.end()) return false;
    w = it->second.first; h = it->second.second; return true;
}

// ----------------------------------------------------------------------------
// UPLOAD: pixeles RGBA -> textura GL, sin mipmaps (LINEAR/NEAREST). Para las
// texturas de UI que ya vienen como pixeles (cursor, atlas, etc.). Comun a
// todos los backends GL/GLES.
// ----------------------------------------------------------------------------
// LAS TEXTURAS DE LA DREAMCAST (GLdc) Y DE LA 3DS (citro3d, ver c3d/w3dGLc3d.cpp): las dos GPU piden potencia de dos de
// 8 a 1024 por lado, toman RGBA de 8 bits (cada una lo pasa a su formato) y aca van sin mipmaps
#if defined(W3D_DREAMCAST) || defined(W3D_3DS)
#define W3D_TEX_POT_RGBA 1
#endif

unsigned int UploadRGBA(const unsigned char* rgba, int w, int h, bool filtrado, bool conMips, bool forzarPOT,
                        bool permitir16) {
    if (!rgba || w <= 0 || h <= 0) {
        return 0;
    }
#if defined(W3D_SYMBIAN) || defined(W3D_TEX_POT_RGBA)
#ifdef W3D_TEX_POT_RGBA
    forzarPOT = true;      // ni la PVR de la Dreamcast ni la PICA de la 3DS aceptan NPOT (ni mas de 1024): al POT siguiente
    permitir16 = false;    // el 5551/565 empacado del editor no: va en RGBA y cada backend lo pasa a 16 bits si conviene
    conMips = false;       // GLdc solo hace mipmaps en texturas twiddled (y en la 3DS todavia no se arman)
#endif
    // N95 (PowerVR MBX Lite, GLES 1.1): una textura NPOT se RECHAZA (glTexImage2D ->
    // INVALID_VALUE) y el quad sale SIN textura -- es el bug del fuego: sus atlas son NPOT
    // (960x24, 1440x48). El camino de MATERIAL ya remuestrea a POT (w3dtexload ReescalarPOT);
    // el de particulas/UI-2D (Textura2D -> aca) se lo saltaba. forzarPOT lo iguala: remuestreo
    // por VECINO al POT siguiente (tope 1024), igual que el material. Los UV 0..1 no cambian.
    unsigned char* potBuf = 0;
#ifdef W3D_TEX_POT_RGBA
    const int ladoMin = 8;   // la PVR no acepta texturas de menos de 8x8 (un icono de 2x8 se rechazaba)
#else
    const int ladoMin = 1;
#endif
    if (forzarPOT) {
        const bool yaPot = ((w & (w - 1)) == 0) && ((h & (h - 1)) == 0) && w <= 1024 && h <= 1024 && w >= ladoMin && h >= ladoMin;
        if (!yaPot) {
            int pw = ladoMin; while (pw < w && pw < 1024) { pw <<= 1; }
            int ph = ladoMin; while (ph < h && ph < 1024) { ph <<= 1; }
            potBuf = new unsigned char[(size_t)pw * ph * 4];
            if (potBuf) {
                for (int y = 0; y < ph; y++) {
                    const unsigned char* fila = rgba + (size_t)(y * h / ph) * w * 4;
                    unsigned char* out = potBuf + (size_t)y * pw * 4;
                    for (int x = 0; x < pw; x++) {
                        const unsigned char* p = fila + (size_t)(x * w / pw) * 4;
                        out[x*4] = p[0]; out[x*4+1] = p[1]; out[x*4+2] = p[2]; out[x*4+3] = p[3];
                    }
                }
                rgba = potBuf; w = pw; h = ph;
            }
        }
    }
#else
    (void)forzarPOT;
#endif
    GLuint id = 0;
    glGenTextures(1, &id);
    // OJO: bind por la ABSTRACCION, no glBindTexture crudo: el backend cachea la textura bindeada
    // y un bind por afuera lo deja mintiendo (la proxima BindTexture(id) se saltea y dibuja otra).
    w3dEngine::BindTexture(id);

    // wrap: REPEAT solo si ambos lados son potencia de 2. En WebGL1 (y GLES2 sin
    // extension) una textura NPOT con REPEAT es INCOMPLETA y samplea negro: un
    // icono de 24x24 se dibujaba como cuadrado negro en el navegador (funcionaba
    // en hardware que banca NPOT y por eso costo verlo). NPOT -> CLAMP_TO_EDGE.
    const bool pot = ((w & (w - 1)) == 0) && ((h & (h - 1)) == 0);

    // MIPMAPS (solo POT: NPOT + piramide es incompleta en ES1/WebGL1). Por aca pasan
    // TODAS las texturas del N95 (el ICL de Symbian decodifica y sube con UploadRGBA).
    const bool mips = conMips && pot && MipmapsGlobal();
    // 16 bits (la mitad de memoria) si la imagen entra sin perdida visible
    static std::vector<unsigned short> px16;
    GLenum formato = GL_RGBA, tipo = GL_UNSIGNED_BYTE;
    const void* datos = rgba;
    if (permitir16) {
        GLenum f16 = GL_RGBA;
        const GLenum t16 = Empacar16(rgba, w, h, px16, f16);
        if (t16) { formato = f16; tipo = t16; datos = &px16[0]; }
    }
    const bool de16 = (tipo != GL_UNSIGNED_BYTE);
    if (de16) glPixelStorei(GL_UNPACK_ALIGNMENT, 2);   // (filas de 2 bytes por texel)
#ifdef W3D_TEX_POT_RGBA
    // GLdc guarda GL_RGBA en ARGB4444 (4 bits por canal: bandas marcadas en una foto o en el trazado de rayos). Una
    // imagen sin transparencia va como GL_RGB, que GLdc guarda en RGB565 (y la 3DS tambien: c3d/w3dGLc3d.cpp). Los datos
    // siguen en RGBA de 8 bits: el backend los convierte, tambien en las actualizaciones parciales (UpdateRGBA)
    GLenum interno = formato;
    {
        bool opaca = true;
        for (size_t i = 3, n = (size_t)w * h * 4; i < n && opaca; i += 4) opaca = (rgba[i] == 255);
        if (opaca) interno = GL_RGB;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, interno, w, h, 0, formato, tipo, datos);
#elif defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0, formato, tipo, datos);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
#else
    // PC (GL 1.4+) y Symbian (ES 1.1): la piramide la genera el driver al subir
    if (mips) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0, formato, tipo, datos);
    if (mips) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_FALSE);
#endif
    if (de16) glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    TexMipsRegistrar(id, mips);
    TexMemAnotar(id, w, h, de16 ? 2 : 4, mips, conMips && MipmapsGlobal());

    const GLint filtro = filtrado ? GL_LINEAR : GL_NEAREST;
    const GLint filtroMin = !mips ? filtro
                          : (filtrado ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtroMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtro);
    if (mips && filtrado) w3dAnisotropico();
    const GLint wrap = pot ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    // el cache de parametros arranca sabiendo lo que se acaba de poner (misma
    // firma que arma TexFilter/TexWrap): el primer frame ya no re-manda nada
    TexParamsRegistrar(id, (filtrado ? 1 : 0) | (mips ? 2 : 0), pot ? 1 : 0);

    w3dEngine::BindTexture(0);
    g_texSizes[id] = std::make_pair(w, h); // recordar el tamano para el aspect ratio del UV editor
#if defined(W3D_SYMBIAN) || defined(W3D_TEX_POT_RGBA)
    if (potBuf) delete[] potBuf;
#endif
    return id;
}

// ----------------------------------------------------------------------------
// CAPTURA DE PANTALLA: una textura RGB vacia y la copia del framebuffer a ella (w3dTexture.h)
// ----------------------------------------------------------------------------
unsigned int ReservarPantalla(unsigned int id, int w, int h) {
    if (w <= 0 || h <= 0) return id;
    GLuint t = id;
    if (!t) glGenTextures(1, &t);
    w3dEngine::BindTexture(t);
#ifdef W3D_SYMBIAN
    // (el framebuffer del N95 es de 16 bits: en 565 ocupa la mitad y no se pierde nada)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    TexMemAnotar(t, w, h, 2, false, false);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    TexMemAnotar(t, w, h, 3, false, false);
#endif
    TexMipsRegistrar(t, false);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    TexParamsRegistrar(t, 1, 0);
    w3dEngine::BindTexture(0);
    g_texSizes[t] = std::make_pair(w, h);
    return t;
}

void CopiarPantalla(unsigned int id, int x, int y, int w, int h) {
    if (!id || w <= 0 || h <= 0) return;
#ifdef W3D_TEX_POT_RGBA
    // la PVR arma el cuadro recien al cerrarlo: no hay pantalla para copiar a mitad de cuadro (y en GLdc
    // glCopyTexSubImage2D es un assert que corta el programa). La captura queda como estaba.
    (void)x; (void)y;
    return;
#endif
    w3dEngine::BindTexture(id);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, y, w, h);
}

// ----------------------------------------------------------------------------
// FREE: libera un buffer de DecodeImage. Comun (DecodeImage aloca con new[]).
// ----------------------------------------------------------------------------
void FreeImage(unsigned char* rgba) {
    delete[] rgba;
}

// borra una textura GL (las miniaturas del browser la usan para liberar al cambiar carpeta / cerrar).
void DeleteTexture(unsigned int id) {
    if (!id) return;
    GLuint g = (GLuint)id;
    glDeleteTextures(1, &g);
    w3dEngine::BindTexture(0);   // GL desbindea lo borrado; el cache del motor tiene que saberlo
    w3dEngine::MezclaTexturaBorrada(id);   // (idem la unidad 1)
    TexMipsRegistrar(id, false); // GL recicla ids: que el nuevo no herede "tiene mips"
    TexParamsOlvidar(id);        // idem su firma de filtro/wrap
    g_texSizes.erase(id);
    g_texMem.erase(id);
}

// ============================================================================
//  .w3dt — TEXTURA LISTA PARA LA GPU (sin decodificar nada al cargar)
//
//  Un PNG se descomprime (inflate), se desfiltra, se convierte a RGBA, se reescala a potencia de dos, se empaca a
//  16 bits y el driver le arma la piramide: en el N95 eso es lo que traba el streaming. Un .w3dt trae los pixeles
//  YA en el formato que recibe glTexImage2D, con TODOS los niveles de mipmap hechos: cargarlo es leer y subir.
//
//      cabecera (16 B): "W3DT" | u16 version (1) | u16 formato | u16 ancho | u16 alto | u8 niveles | u8 0 | u16 0
//      niveles:         el 0 (ancho x alto) y cada mitad hasta 1x1, filas apretadas, cada nivel arranca alineado a 4
//      formato:         1 RGB565 | 2 RGBA5551 | 3 RGBA4444 | 4 RGB888 | 5 RGBA8888   (little-endian, como GL)
//
//  Ancho y alto potencia de dos (lo que acepta el N95). En el telefono, con texturasMitadDesde, se SALTEA el nivel 0
//  (la mitad ya esta hecha: es el nivel 1).
// ============================================================================
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#endif
namespace {
struct W3dtCab { int formato, w, h, niveles; };
inline unsigned short W3dtU16(const unsigned char* p) { return (unsigned short)(p[0] | (p[1] << 8)); }
int W3dtBpp(int formato) { return formato <= 3 ? 2 : (formato == 4 ? 3 : 4); }
bool W3dtCabecera(const std::vector<unsigned char>& d, W3dtCab& c) {
    if (d.size() < 16 || memcmp(&d[0], "W3DT", 4) != 0 || W3dtU16(&d[4]) != 1) return false;
    c.formato = W3dtU16(&d[6]); c.w = W3dtU16(&d[8]); c.h = W3dtU16(&d[10]); c.niveles = d[12];
    return c.formato >= 1 && c.formato <= 5 && c.w > 0 && c.h > 0 && c.niveles >= 1;
}
// el offset de cada nivel (y su tamano); false si el archivo no los trae enteros
bool W3dtNiveles(const std::vector<unsigned char>& d, const W3dtCab& c, std::vector<size_t>& off) {
    off.clear();
    size_t p = 16;
    int w = c.w, h = c.h;
    for (int k = 0; k < c.niveles; k++) {
        const size_t n = (size_t)w * h * W3dtBpp(c.formato);
        if (p > d.size() || d.size() - p < n) return false;
        off.push_back(p);
        p = (p + n + 3) & ~(size_t)3;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    return true;
}
// un nivel a RGBA 8888 (para el editor: miniaturas, el pintor de texturas, lo que pide pixeles)
void W3dtARGBA(const unsigned char* s, int formato, int n, unsigned char* o) {
    for (int i = 0; i < n; i++, o += 4) {
        if (formato <= 3) {
            const unsigned v = s[i * 2] | (s[i * 2 + 1] << 8);
            if (formato == 1) { o[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31); o[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
                                o[2] = (unsigned char)((v & 31) * 255 / 31); o[3] = 255; }
            else if (formato == 2) { o[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31); o[1] = (unsigned char)(((v >> 6) & 31) * 255 / 31);
                                     o[2] = (unsigned char)(((v >> 1) & 31) * 255 / 31); o[3] = (v & 1) ? 255 : 0; }
            else { o[0] = (unsigned char)(((v >> 12) & 15) * 17); o[1] = (unsigned char)(((v >> 8) & 15) * 17);
                   o[2] = (unsigned char)(((v >> 4) & 15) * 17); o[3] = (unsigned char)((v & 15) * 17); }
        } else if (formato == 4) { o[0] = s[i * 3]; o[1] = s[i * 3 + 1]; o[2] = s[i * 3 + 2]; o[3] = 255; }
        else { o[0] = s[i * 4]; o[1] = s[i * 4 + 1]; o[2] = s[i * 4 + 2]; o[3] = s[i * 4 + 3]; }
    }
}
} // namespace

bool EsW3dt(const char* path) {
    if (!path) return false;
    const size_t n = strlen(path);
    if (n < 5) return false;
    const char* e = path + n - 5;
    return e[0] == '.' && (e[1] | 32) == 'w' && e[2] == '3' && (e[3] | 32) == 'd' && (e[4] | 32) == 't';
}

bool LoadTextureW3dt(const char* path, unsigned int& outId, int* outW, int* outH) {
#ifdef W3D_TEX_POT_RGBA
    {   // GLdc no toma los 16 bits del archivo (5551/4444 en ese orden): se decodifica a RGBA y sube como cualquier otra
        unsigned char* rgba = 0; int w = 0, h = 0;
        if (!DecodeW3dt(path, 1024, &rgba, &w, &h) || !rgba) return false;
        outId = UploadRGBA(rgba, w, h, !PixeladoGlobal(), false, true, false);
        delete[] rgba;
        if (outW) *outW = w;
        if (outH) *outH = h;
        return outId != 0;
    }
#endif
    std::vector<unsigned char> d;
    W3dtCab c;
    std::vector<size_t> off;
    if (!w3dFileSystem::ReadFileBytes(path ? path : "", d) || !W3dtCabecera(d, c) || !W3dtNiveles(d, c, off)) {
        w3dLogfE("[textura] %s no es un .w3dt que este Whisk3D pueda leer", path ? path : "");
        return false;
    }
    const bool pot = ((c.w & (c.w - 1)) == 0) && ((c.h & (c.h - 1)) == 0);
    int primero = 0;
#ifdef W3D_SYMBIAN
    if (!pot || c.w > 1024 || c.h > 1024) {
        w3dLogfE("[textura] %s: %dx%d no es potencia de dos (el N95 no la acepta)", path, c.w, c.h);
        return false;
    }
    // la MITAD en el telefono (texturasMitadDesde): el nivel 1 ya es la mitad
    const int mitad = TexturasMitadDesde();
    while (mitad > 0 && primero + 1 < c.niveles && ((c.w >> primero) > mitad || (c.h >> primero) > mitad)) primero++;
#endif
    GLenum formato = GL_RGBA, tipo = GL_UNSIGNED_BYTE;
    switch (c.formato) {
        case 1: formato = GL_RGB;  tipo = GL_UNSIGNED_SHORT_5_6_5; break;
        case 2: formato = GL_RGBA; tipo = GL_UNSIGNED_SHORT_5_5_5_1; break;
        case 3: formato = GL_RGBA; tipo = GL_UNSIGNED_SHORT_4_4_4_4; break;
        case 4: formato = GL_RGB;  tipo = GL_UNSIGNED_BYTE; break;
        default: break;
    }
    const int bpp = W3dtBpp(c.formato);
    // la piramide del archivo, si el proyecto quiere mipmaps (y la textura es POT: NPOT + piramide es incompleta)
    const bool mips = pot && MipmapsGlobal() && c.niveles - primero > 1;
    GLuint id = 0;
    glGenTextures(1, &id);
    BindTexture(id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, bpp == 4 ? 4 : (bpp == 2 ? 2 : 1));
    const int hasta = mips ? c.niveles : primero + 1;
    for (int k = primero; k < hasta; k++) {
        const int w = c.w >> k ? c.w >> k : 1, h = c.h >> k ? c.h >> k : 1;
        glTexImage2D(GL_TEXTURE_2D, k - primero, formato, w, h, 0, formato, tipo, &d[off[k]]);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const bool pix = PixeladoGlobal();
    const GLint filtro = pix ? GL_NEAREST : GL_LINEAR;
    const GLint filtroMin = !mips ? filtro : (pix ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtroMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtro);
    if (mips && !pix) w3dAnisotropico();
    const GLint wrap = pot ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    TexParamsRegistrar(id, (pix ? 0 : 1) | (mips ? 2 : 0), pot ? 1 : 0);
    TexMipsRegistrar(id, mips);
    BindTexture(0);
    const int w0 = c.w >> primero ? c.w >> primero : 1, h0 = c.h >> primero ? c.h >> primero : 1;
    g_texSizes[id] = std::make_pair(w0, h0);
    TexMemAnotar(id, w0, h0, bpp, mips, mips);
    outId = id;
    if (outW) *outW = c.w;
    if (outH) *outH = c.h;
    return true;
}

bool DecodeW3dt(const char* path, int maxLado, unsigned char** outRGBA, int* outW, int* outH) {
    std::vector<unsigned char> d;
    W3dtCab c;
    std::vector<size_t> off;
    if (!outRGBA || !w3dFileSystem::ReadFileBytes(path ? path : "", d) || !W3dtCabecera(d, c) || !W3dtNiveles(d, c, off))
        return false;
    // (una miniatura sale del nivel que ya entra: la piramide esta hecha)
    int k = 0;
    while (maxLado > 0 && k + 1 < c.niveles && ((c.w >> k) > maxLado || (c.h >> k) > maxLado)) k++;
    const int w = c.w >> k ? c.w >> k : 1, h = c.h >> k ? c.h >> k : 1;
    unsigned char* buf = new unsigned char[(size_t)w * h * 4];
    W3dtARGBA(&d[off[k]], c.formato, w * h, buf);
    *outRGBA = buf;
    if (outW) *outW = w;
    if (outH) *outH = h;
    return true;
}

// stb + su consumidor (CargarPixeles/DecodeImage/DecodeThumbnail) NO existe en Symbian: el N95 decodifica
// con ICL en platform/symbian/src/w3dtexload.cpp. Ademas su toolchain es C++03 (sin nullptr / vector::data).
#ifndef W3D_SYMBIAN
// Decodifica una imagen a pixeles con stb. La LECTURA la hace la abstraccion del
// Core (w3dFileSystem::ReadFileBytes): un SOLO camino para todas las plataformas
// (en Android resuelve solo si es asset del APK o archivo real del /storage). SIN SDL.
static stbi_uc* CargarPixeles(const char* path, int* w, int* h, int* canales, int reqComp) {
    std::vector<unsigned char> bytes;
    if (!w3dFileSystem::ReadFileBytes(path ? path : "", bytes) || bytes.empty()) return 0;
    return stbi_load_from_memory(&bytes[0], (int)bytes.size(), w, h, canales, reqComp);
}

// ----------------------------------------------------------------------------
// DECODE: imagen de disco -> pixeles RGBA en el heap (sin subir a GL).
//   - PC / Android / Web: stb_image (aca mismo, forzando RGBA).
//   - Symbian: lo implementa platform/symbian/src/w3dtexload.cpp con ICL.
// ----------------------------------------------------------------------------
bool DecodeImage(const char* path, unsigned char** outRGBA, int* outW, int* outH) {
    if (!outRGBA) {
        return false;
    }
    if (EsW3dt(path)) return DecodeW3dt(path, 0, outRGBA, outW, outH);
    int w = 0, h = 0, canales = 0;
    stbi_uc* data = CargarPixeles(path, &w, &h, &canales, STBI_rgb_alpha);
    if (!data) {
        return false;
    }
    const int n = w * h * 4;
    unsigned char* buf = new unsigned char[n];
    memcpy(buf, data, n);
    stbi_image_free(data);
    *outRGBA = buf;
    if (outW) { *outW = w; }
    if (outH) { *outH = h; }
    return true;
}

// MINIATURA: decode + reduccion box a <= maxSize (mantiene aspecto). El buffer full es transitorio (se libera aca);
// solo queda la mini. Asi una foto de 5MP no se queda entera en RAM: clave para no reventar la memoria (N95).
bool DecodeThumbnail(const char* path, int maxSize, unsigned char** outRGBA, int* outW, int* outH) {
    if (!outRGBA || maxSize < 1) return false;
    if (EsW3dt(path)) return DecodeW3dt(path, maxSize, outRGBA, outW, outH);   // (el nivel de mipmap que entra)
    int w = 0, h = 0, canales = 0;
    stbi_uc* full = CargarPixeles(path, &w, &h, &canales, STBI_rgb_alpha);
    if (!full) return false;
    if (w < 1 || h < 1) { stbi_image_free(full); return false; }
    int tw, th; // destino con aspecto, lado mayor = maxSize
    if (w >= h) { tw = maxSize; th = (int)((long long)h * maxSize / w); }
    else        { th = maxSize; tw = (int)((long long)w * maxSize / h); }
    if (tw < 1) tw = 1; if (th < 1) th = 1;
    unsigned char* thumb = new unsigned char[(size_t)tw * th * 4];
    for (int y = 0; y < th; y++) {
        int sy0 = (int)((long long)y * h / th), sy1 = (int)((long long)(y + 1) * h / th);
        if (sy1 <= sy0) sy1 = sy0 + 1; if (sy1 > h) sy1 = h;
        for (int x = 0; x < tw; x++) {
            int sx0 = (int)((long long)x * w / tw), sx1 = (int)((long long)(x + 1) * w / tw);
            if (sx1 <= sx0) sx1 = sx0 + 1; if (sx1 > w) sx1 = w;
            unsigned int r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) { const stbi_uc* row = full + (size_t)sy * w * 4;
                for (int sx = sx0; sx < sx1; sx++) { const stbi_uc* p = row + (size_t)sx * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++; } }
            unsigned char* d = thumb + ((size_t)y * tw + x) * 4;
            if (n) { d[0] = (unsigned char)(r/n); d[1] = (unsigned char)(g/n); d[2] = (unsigned char)(b/n); d[3] = (unsigned char)(a/n); }
            else   { d[0] = d[1] = d[2] = 0; d[3] = 255; }
        }
    }
    stbi_image_free(full);
    *outRGBA = thumb; if (outW) *outW = tw; if (outH) *outH = th;
    return true;
}
#endif

// ----------------------------------------------------------------------------
// LOAD: decode + upload de una imagen de disco (texturas de material).
//   - Escritorio (PC): stb + gluBuild2DMipmaps (con mipmaps, como siempre).
//   - Android: stb (assets del APK via AAssetManager) + glTexImage2D (GLES2, sin glu).
//   - Web: stb + glTexImage2D (GLES2/WebGL, sin glu).
//   - Symbian: NO se compila aca; lo implementa platform/symbian/w3dtexload.cpp
//     con ICL (CImageDecoder) y termina llamando a UploadRGBA.
// ----------------------------------------------------------------------------
#ifndef W3D_SYMBIAN
bool LoadTexture(const char* path, unsigned int& outId, int* outW, int* outH) {
    if (EsW3dt(path)) return LoadTextureW3dt(path, outId, outW, outH);   // (lista para la GPU: nada que decodificar)
    int w = 0, h = 0, canales = 0;
    // SIEMPRE a RGBA: con canales reales (req=0) una imagen en escala de grises entrega 1-2 bytes
    // por pixel y subirla como GL_RGB sobre-lee el heap. Pedir 4 canales lo resuelve de raiz.
    stbi_uc* data = CargarPixeles(path, &w, &h, &canales, 4);
    if (!data) {
        return false;
    }
#ifdef W3D_TEX_POT_RGBA
    {   // la PVR: potencia de dos, RGBA (GLdc la pasa a 16 bits) y sin mipmaps: lo resuelve UploadRGBA
        outId = UploadRGBA(data, w, h, !w3dEngine::PixeladoGlobal(), false, true, false);
        stbi_image_free(data);
        if (outW) *outW = w;
        if (outH) *outH = h;
        return outId != 0;
    }
#endif
    // PNG SIN canal alpha (canales <= 3, lo reporta stb aunque se fuerce RGBA): se
    // RE-EMPACA el buffer ya-RGBA a RGB apretado y se sube GL_RGB -> 25% menos de
    // memoria de GPU en las texturas opacas (idea del dueno: atlas opaco 8,8,8).
    // OJO: NO pedir reqComp=0 (una imagen gris entrega 1-2 bpp y GL_RGB sobre-lee).
    GLenum formato = GL_RGBA, tipo = GL_UNSIGNED_BYTE;
    // 16 BITS si entra sin perdida visible (lo mismo que sube el N95 por UploadRGBA: el editor ve lo que vera el
    // telefono y la memoria se mide igual)
    static std::vector<unsigned short> px16;
    {
        GLenum f16 = GL_RGBA;
        const GLenum t16 = Empacar16(data, w, h, px16, f16);
        if (t16) { formato = f16; tipo = t16; }
    }
    if (tipo == GL_UNSIGNED_BYTE && canales > 0 && canales <= 3) {
        stbi_uc* p = data;                              // in-place: RGB queda adelante
        for (int i = 0; i < w * h; i++) {
            p[i*3]   = p[i*4];
            p[i*3+1] = p[i*4+1];
            p[i*3+2] = p[i*4+2];
        }
        formato = GL_RGB;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);          // filas RGB no-multiplo-de-4
    }

    GLuint id = 0;
    glGenTextures(1, &id);
    w3dEngine::BindTexture(id);   // por la abstraccion: el cache del backend queda al dia

    // PIXELADO GLOBAL (pedido del dueno): NEAREST en min y mag. MIPMAPS: piramide
    // segun MipmapsGlobal() (default ON; el .w3d lo apaga con `mipmaps: false`).
    // Con pixelado + mips el min-filter va NEAREST_MIPMAP_LINEAR: pixel perfect de
    // cerca, niveles chicos de lejos (TexFilter respeta esto en cada material).
    const bool pix = w3dEngine::PixeladoGlobal();
    const bool pot = ((w & (w - 1)) == 0) && ((h & (h - 1)) == 0);
    const bool mips = pot && w3dEngine::MipmapsGlobal();
    const GLint filtroPix = pix ? GL_NEAREST : GL_LINEAR;
    const GLint filtroMin = !mips ? filtroPix
                          : (pix ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
#ifdef __ANDROID__
    if (tipo != GL_UNSIGNED_BYTE) glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0,
                 formato, tipo, tipo != GL_UNSIGNED_BYTE ? (const void*)&px16[0] : (const void*)data);
    if (tipo != GL_UNSIGNED_BYTE) glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtroMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtroPix);
#elif defined(__EMSCRIPTEN__)
    // WebGL: POT -> piramide con glGenerateMipmap; NPOT -> sin mips + CLAMP (WebGL1
    // pide POT para mipmaps/REPEAT).
    if (tipo != GL_UNSIGNED_BYTE) glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0,
                 formato, tipo, tipo != GL_UNSIGNED_BYTE ? (const void*)&px16[0] : (const void*)data);
    if (tipo != GL_UNSIGNED_BYTE) glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtroMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtroPix);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtroMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtroPix);
    if (mips && !pix) w3dAnisotropico();
    if (tipo != GL_UNSIGNED_BYTE) {
        // 16 bits: la piramide la arma el driver (GL 1.4+), como en UploadRGBA
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        if (mips) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0, formato, tipo, &px16[0]);
        if (mips) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_FALSE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    } else if (mips) {
        gluBuild2DMipmaps(GL_TEXTURE_2D, formato, w, h,
                          formato, GL_UNSIGNED_BYTE, data);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0,
                     formato, GL_UNSIGNED_BYTE, data);
    }
#endif
    TexMipsRegistrar(id, mips);

    w3dEngine::BindTexture(0);
    stbi_image_free(data);

    // TAMANO: hasta hoy solo lo anotaba UploadRGBA, asi que TextureSize() mentia
    // (devolvia false) para TODA textura de material en PC -> no habia forma de
    // contar la memoria de texturas del proyecto. Ahora tambien pasa por aca.
    g_texSizes[id] = std::make_pair(w, h);
    TexMemAnotar(id, w, h, tipo != GL_UNSIGNED_BYTE ? 2 : (formato == GL_RGB ? 3 : 4), mips, w3dEngine::MipmapsGlobal());

    outId = id;
    if (outW) { *outW = w; }
    if (outH) { *outH = h; }
    return true;
}
#endif

// ============================================================================
//  ENCODER PNG propio, C puro y portable (PC/N95). Sin dependencias: deflate
//  "stored" (sin comprimir) + CRC32 + Adler32. El archivo pesa mas que un PNG
//  comprimido, pero es correcto y compila igual en todos lados; la compresion
//  real se puede sumar despues. Entrada = RGBA8. glReadPixels entrega bottom-left,
//  asi que flipY invierte las filas para dejar el PNG top-left.
//  EncodePNG arma el PNG en un buffer de memoria (portable); SavePNG lo ESCRIBE:
//  en PC/Web con stdio (aca), en Symbian con RFile (platform/symbian/w3dtexload.cpp).
// ============================================================================

// CRC-32 (poly 0xEDB88320) incremental, sin tabla. crc arranca en 0xFFFFFFFF.
static unsigned int w3dCrcUpdate(unsigned int crc, const unsigned char* d, int n){
    for (int i = 0; i < n; i++){
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}
// Adler-32 (checksum del stream zlib).
static unsigned int w3dAdler32(const unsigned char* d, int n){
    unsigned int a = 1, b = 0;
    for (int i = 0; i < n; i++){ a = (a + d[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}
// escribe un chunk PNG en el buffer 'out' avanzando p: len(BE) + tipo(4) + datos + crc(BE).
static void w3dPngChunkMem(unsigned char* out, int& p, const char* tipo, const unsigned char* data, int len){
    out[p++]=(len>>24)&255; out[p++]=(len>>16)&255; out[p++]=(len>>8)&255; out[p++]=len&255;
    out[p++]=(unsigned char)tipo[0]; out[p++]=(unsigned char)tipo[1];
    out[p++]=(unsigned char)tipo[2]; out[p++]=(unsigned char)tipo[3];
    for (int i = 0; i < len; i++) out[p++] = data[i];
    unsigned int crc = 0xFFFFFFFFu;
    crc = w3dCrcUpdate(crc, (const unsigned char*)tipo, 4);
    if (len) crc = w3dCrcUpdate(crc, data, len);
    crc ^= 0xFFFFFFFFu;
    out[p++]=(crc>>24)&255; out[p++]=(crc>>16)&255; out[p++]=(crc>>8)&255; out[p++]=crc&255;
}

// arma el PNG en un buffer del heap (new[]). El que llama libera con delete[] (o FreeImage).
static unsigned char* w3dEncodePNGImpl(const unsigned char* rgba, int w, int h, bool flipY, int* outLen, bool conAlpha){
    if (outLen) *outLen = 0;
    if (!rgba || w <= 0 || h <= 0) return 0;
    const int bpp = conAlpha ? 4 : 3;   // conAlpha: PNG RGBA (texturas generadas con canal alpha)

    // El render se exporta como RGB (sin alpha): un material con alpha (pelo, hojas) ya queda
    // compuesto sobre el fondo en el COLOR; guardar el alpha del framebuffer hace "huecos" en el
    // visor donde deberia ser solido. Entrada = RGBA (glReadPixels), salida = RGB (se descarta
    // el 4to byte de cada pixel). scanlines: 1 byte de filtro (0 = None) + RGB de la fila.
    const int srcRow   = w * 4;   // fila de la entrada (RGBA)
    const int rowbytes = w * bpp; // fila del PNG (RGB, o RGBA si conAlpha)
    const int rawlen = h * (1 + rowbytes);
    unsigned char* raw = new unsigned char[rawlen];
    for (int y = 0; y < h; y++){
        int src = flipY ? (h - 1 - y) : y;
        const unsigned char* s = rgba + (size_t)src * srcRow;
        unsigned char* dst = raw + y * (1 + rowbytes);
        dst[0] = 0; // filtro None
        unsigned char* d = dst + 1;
        for (int x = 0; x < w; x++){
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; // RGB (se saltea s[3] = alpha)...
            if (conAlpha) d[3] = s[3];             // ...salvo que se pida el alpha
            d += bpp; s += 4;
        }
    }

    // IDAT = zlib con bloques "stored" (BTYPE=00): header 0x78 0x01 + bloques + adler32
    int nbloques = (rawlen + 65534) / 65535; if (nbloques < 1) nbloques = 1;
    int idatlen = 2 + rawlen + nbloques * 5 + 4;
    unsigned char* idat = new unsigned char[idatlen];
    int ip = 0;
    idat[ip++] = 0x78; idat[ip++] = 0x01;             // header zlib
    int off = 0;
    while (off < rawlen){
        int n = rawlen - off; if (n > 65535) n = 65535;
        int ultimo = (off + n >= rawlen) ? 1 : 0;
        idat[ip++] = (unsigned char)ultimo;           // BFINAL + BTYPE=00 (stored)
        idat[ip++] = n & 255; idat[ip++] = (n >> 8) & 255;         // LEN (LE)
        unsigned int nn = (~(unsigned int)n) & 0xFFFF;
        idat[ip++] = nn & 255; idat[ip++] = (nn >> 8) & 255;       // NLEN (LE)
        memcpy(idat + ip, raw + off, n); ip += n; off += n;
    }
    unsigned int ad = w3dAdler32(raw, rawlen);
    idat[ip++]=(ad>>24)&255; idat[ip++]=(ad>>16)&255; idat[ip++]=(ad>>8)&255; idat[ip++]=ad&255;
    delete[] raw;

    // ensamblar el PNG: firma + IHDR + IDAT + IEND
    unsigned char ihdr[13];
    ihdr[0]=(w>>24)&255; ihdr[1]=(w>>16)&255; ihdr[2]=(w>>8)&255; ihdr[3]=w&255;
    ihdr[4]=(h>>24)&255; ihdr[5]=(h>>16)&255; ihdr[6]=(h>>8)&255; ihdr[7]=h&255;
    ihdr[8]=8; ihdr[9]=(unsigned char)(conAlpha ? 6 : 2); ihdr[10]=0; ihdr[11]=0; ihdr[12]=0; // 8bit, RGB (6 = RGBA), sin interlace
    int total = 8 + (12 + 13) + (12 + idatlen) + 12;
    unsigned char* png = new unsigned char[total];
    int p = 0;
    static const unsigned char firma[8] = {137,80,78,71,13,10,26,10};
    for (int i = 0; i < 8; i++) png[p++] = firma[i];
    w3dPngChunkMem(png, p, "IHDR", ihdr, 13);
    w3dPngChunkMem(png, p, "IDAT", idat, idatlen);
    w3dPngChunkMem(png, p, "IEND", 0, 0);
    delete[] idat;

    if (outLen) *outLen = p; // = total
    return png;
}
// actualiza un RECTANGULO de una textura ya subida (pintura en vivo): 'rgbaRect' = w*h pixeles del rect
bool UpdateRGBA(unsigned int id, int x, int y, int w, int h, const unsigned char* rgbaRect, bool regenerarMips) {
    if (!id || !rgbaRect || w <= 0 || h <= 0) return false;
    w3dEngine::BindTexture(id);
    const bool mips = regenerarMips && TexTieneMips(id);
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgbaRect);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
#else
    if (mips) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgbaRect);
    if (mips) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_FALSE);
#endif
    w3dEngine::BindTexture(0);
    return true;
}
void RegenerarMips(unsigned int id, int x, int y, const unsigned char* px1) {
    if (!id || !px1 || !TexTieneMips(id)) return;
    w3dEngine::BindTexture(id);
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    (void)x; (void)y;
    glGenerateMipmap(GL_TEXTURE_2D);
#else
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px1);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_FALSE);
#endif
    w3dEngine::BindTexture(0);
}
unsigned char* EncodePNG(const unsigned char* rgba, int w, int h, bool flipY, int* outLen){ return w3dEncodePNGImpl(rgba, w, h, flipY, outLen, false); }
unsigned char* EncodePNGRGBA(const unsigned char* rgba, int w, int h, bool flipY, int* outLen){ return w3dEncodePNGImpl(rgba, w, h, flipY, outLen, true); }

#ifndef W3D_SYMBIAN
// PC/Web: encode + escritura con stdio. En Symbian SavePNG lo implementa
// platform/symbian/w3dtexload.cpp (mismo EncodePNG + RFile).
bool SavePNG(const char* path, const unsigned char* rgba, int w, int h, bool flipY){
    if (!path) return false;
    int len = 0;
    unsigned char* png = EncodePNG(rgba, w, h, flipY, &len);
    if (!png) return false;
    FILE* f = fopen(path, "wb");
    if (!f) { w3dLogfE("SavePNG: fopen FALLO path='%s' errno=%d (%s)", path, errno, strerror(errno)); delete[] png; return false; }
    size_t wr = fwrite(png, 1, len, f);
    bool cerro = (fclose(f) == 0);   // el flush pasa ACA: sin chequearlo, un PNG chico "guardaba bien"
    delete[] png;
    if (!cerro || wr != (size_t)len) { w3dLogfE("SavePNG: escritura corta en '%s' (disco lleno?)", path); return false; }
    return true;
}
#endif

} // namespace w3dEngine
