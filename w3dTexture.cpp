// ============================================================================
//  Whisk3DCore (engine) — cargador de texturas UNIVERSAL
//  Ver w3dTexture.h y ARQUITECTURA.md.
// ============================================================================

#include "w3dTexture.h"
#include <map> // id de textura -> (w,h) para el aspect ratio en el UV editor

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

} // namespace w3dEngine
