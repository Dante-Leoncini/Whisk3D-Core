// ============================================================================
//  Whisk3DCore (engine) — cargador de texturas UNIVERSAL
//  Ver w3dTexture.h y ARQUITECTURA.md.
// ============================================================================

#include "w3dTexture.h"
#include <map> // id de textura -> (w,h) para el aspect ratio en el UV editor
#include <stdio.h>  // fopen/fwrite: escritura del PNG (PC/Web; Symbian = TODO via RFile)
#include <string.h> // memcpy en el encoder PNG

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

namespace w3dEngine {

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
unsigned int UploadRGBA(const unsigned char* rgba, int w, int h, bool filtrado) {
    if (!rgba || w <= 0 || h <= 0) {
        return 0;
    }
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    const GLint filtro = filtrado ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtro);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtro);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBindTexture(GL_TEXTURE_2D, 0);
    g_texSizes[id] = std::make_pair(w, h); // recordar el tamaño para el aspect ratio del UV editor
    return id;
}

// ----------------------------------------------------------------------------
// FREE: libera un buffer de DecodeImage. Comun (DecodeImage aloca con new[]).
// ----------------------------------------------------------------------------
void FreeImage(unsigned char* rgba) {
    delete[] rgba;
}

// ----------------------------------------------------------------------------
// DECODE: imagen de disco -> pixeles RGBA en el heap (sin subir a GL).
//   - PC / Android: stb_image (aca mismo, forzando RGBA).
//   - Symbian: lo implementa platform/symbian/src/w3dtexload.cpp con ICL.
// ----------------------------------------------------------------------------
#ifndef W3D_SYMBIAN
bool DecodeImage(const char* path, unsigned char** outRGBA, int* outW, int* outH) {
    if (!outRGBA) {
        return false;
    }
    int w = 0, h = 0, canales = 0;
    stbi_uc* data = stbi_load(path, &w, &h, &canales, STBI_rgb_alpha);
    if (!data) {
        return false;
    }
    const int n = w * h * 4;
    unsigned char* buf = new unsigned char[n];
    for (int i = 0; i < n; i++) {
        buf[i] = data[i];
    }
    stbi_image_free(data);
    *outRGBA = buf;
    if (outW) { *outW = w; }
    if (outH) { *outH = h; }
    return true;
}
#endif

// ----------------------------------------------------------------------------
// LOAD: decode + upload de una imagen de disco (texturas de material).
//   - Escritorio (PC): stb + gluBuild2DMipmaps (con mipmaps, como siempre).
//   - Android: stb + glTexImage2D (GLES1, sin glu).
//   - Symbian: NO se compila aca; lo implementa platform/symbian/w3dtexload.cpp
//     con ICL (CImageDecoder) y termina llamando a UploadRGBA.
// ----------------------------------------------------------------------------
#ifndef W3D_SYMBIAN
bool LoadTexture(const char* path, unsigned int& outId, int* outW, int* outH) {
    int w = 0, h = 0, canales = 0;
    stbi_uc* data = stbi_load(path, &w, &h, &canales, 0);
    if (!data) {
        return false;
    }
    const GLenum formato = (canales == 4) ? GL_RGBA : GL_RGB;

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);

#ifdef __ANDROID__
    glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0,
                 formato, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#elif defined(__EMSCRIPTEN__)
    // WebGL: sin GLU. Sin mipmaps + CLAMP -> anda con cualquier tamano (WebGL1 pide
    // POT para mipmaps/REPEAT; asi evitamos esa restriccion).
    glTexImage2D(GL_TEXTURE_2D, 0, formato, w, h, 0,
                 formato, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gluBuild2DMipmaps(GL_TEXTURE_2D, formato, w, h,
                      formato, GL_UNSIGNED_BYTE, data);
#endif

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);

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
    if (!f) { delete[] png; return false; }
    fwrite(png, 1, len, f);
    fclose(f);
    delete[] png;
    return true;
}
#endif

} // namespace w3dEngine
