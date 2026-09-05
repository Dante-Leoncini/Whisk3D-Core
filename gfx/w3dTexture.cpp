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
#include "w3dFilesystem.h" // lectura de archivos UNIFICADA del Core (asset del APK o archivo real)
#include <vector>
#include <set>

// GL_CLAMP_TO_EDGE es GL 1.2+; el <GL/gl.h> 1.1 de Windows no lo declara.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
// GL_GENERATE_MIPMAP es GL 1.4+ / ES 1.1; el <GL/gl.h> 1.1 de Windows no lo declara.
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
static std::set<unsigned int> g_texConMips;
bool TexTieneMips(unsigned int id) { return g_texConMips.count(id) != 0; }
static void TexMipsRegistrar(unsigned int id, bool con) {
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
static std::map<unsigned int, TexParamEstado> g_texParams;

bool TexFiltroCambia(unsigned int id, int firma) {
    TexParamEstado& e = g_texParams[id];
    if (e.filtro == (signed char)firma) return false;
    e.filtro = (signed char)firma;
    return true;
}
bool TexWrapCambia(unsigned int id, int firma) {
    TexParamEstado& e = g_texParams[id];
    if (e.wrap == (signed char)firma) return false;
    e.wrap = (signed char)firma;
    return true;
}
void TexParamsRegistrar(unsigned int id, int firmaFiltro, int firmaWrap) {
    TexParamEstado& e = g_texParams[id];
    e.filtro = (signed char)firmaFiltro;
    e.wrap = (signed char)firmaWrap;
}
void TexParamsOlvidar(unsigned int id) { g_texParams.erase(id); }

// dimensiones de cada textura subida (id GL -> w,h). Lo llena UploadRGBA, por la que pasan TODOS los
// uploads (PC stb + Symbian ICL + procedural) -> el UV editor lee el aspect ratio sin tocar la struct Texture.
static std::map<unsigned int, std::pair<int,int> > g_texSizes;
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
unsigned int UploadRGBA(const unsigned char* rgba, int w, int h, bool filtrado, bool conMips) {
    if (!rgba || w <= 0 || h <= 0) {
        return 0;
    }
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
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
#else
    // PC (GL 1.4+) y Symbian (ES 1.1): la piramide la genera el driver al subir
    if (mips) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (mips) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_FALSE);
#endif
    TexMipsRegistrar(id, mips);

    const GLint filtro = filtrado ? GL_LINEAR : GL_NEAREST;
    const GLint filtroMin = !mips ? filtro
                          : (filtrado ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtroMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtro);
    const GLint wrap = pot ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    // el cache de parametros arranca sabiendo lo que se acaba de poner (misma
    // firma que arma TexFilter/TexWrap): el primer frame ya no re-manda nada
    TexParamsRegistrar(id, (filtrado ? 1 : 0) | (mips ? 2 : 0), pot ? 1 : 0);

    w3dEngine::BindTexture(0);
    g_texSizes[id] = std::make_pair(w, h); // recordar el tamano para el aspect ratio del UV editor
    return id;
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
    TexMipsRegistrar(id, false); // GL recicla ids: que el nuevo no herede "tiene mips"
    TexParamsOlvidar(id);        // idem su firma de filtro/wrap
    g_texSizes.erase(id);
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
    int w = 0, h = 0, canales = 0;
    // SIEMPRE a RGBA: con canales reales (req=0) una imagen en escala de grises entrega 1-2 bytes
    // por pixel y subirla como GL_RGB sobre-lee el heap. Pedir 4 canales lo resuelve de raiz.
    stbi_uc* data = CargarPixeles(path, &w, &h, &canales, 4);
    if (!data) {
        return false;
    }
    // PNG SIN canal alpha (canales <= 3, lo reporta stb aunque se fuerce RGBA): se
    // RE-EMPACA el buffer ya-RGBA a RGB apretado y se sube GL_RGB -> 25% menos de
    // memoria de GPU en las texturas opacas (idea del dueno: atlas opaco 8,8,8).
    // OJO: NO pedir reqComp=0 (una imagen gris entrega 1-2 bpp y GL_RGB sobre-lee).
    GLenum formato = GL_RGBA;
    if (canales > 0 && canales <= 3) {
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
    glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0,
                 formato, GL_UNSIGNED_BYTE, data);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtroMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtroPix);
#elif defined(__EMSCRIPTEN__)
    // WebGL: POT -> piramide con glGenerateMipmap; NPOT -> sin mips + CLAMP (WebGL1
    // pide POT para mipmaps/REPEAT).
    glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0,
                 formato, GL_UNSIGNED_BYTE, data);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtroMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtroPix);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtroMin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtroPix);
    if (mips) {
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
unsigned char* EncodePNG(const unsigned char* rgba, int w, int h, bool flipY, int* outLen){
    if (outLen) *outLen = 0;
    if (!rgba || w <= 0 || h <= 0) return 0;

    // El render se exporta como RGB (sin alpha): un material con alpha (pelo, hojas) ya queda
    // compuesto sobre el fondo en el COLOR; guardar el alpha del framebuffer hace "huecos" en el
    // visor donde deberia ser solido. Entrada = RGBA (glReadPixels), salida = RGB (se descarta
    // el 4to byte de cada pixel). scanlines: 1 byte de filtro (0 = None) + RGB de la fila.
    const int srcRow   = w * 4; // fila de la entrada (RGBA)
    const int rowbytes = w * 3; // fila del PNG (RGB)
    const int rawlen = h * (1 + rowbytes);
    unsigned char* raw = new unsigned char[rawlen];
    for (int y = 0; y < h; y++){
        int src = flipY ? (h - 1 - y) : y;
        const unsigned char* s = rgba + (size_t)src * srcRow;
        unsigned char* dst = raw + y * (1 + rowbytes);
        dst[0] = 0; // filtro None
        unsigned char* d = dst + 1;
        for (int x = 0; x < w; x++){
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; // RGB (se saltea s[3] = alpha)
            d += 3; s += 4;
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
    ihdr[8]=8; ihdr[9]=2; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0; // 8bit, RGB, sin interlace
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
