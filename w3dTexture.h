#pragma once
// ============================================================================
//  Whisk3DCore (engine) — cargador de texturas UNIVERSAL
//
//  Abrir archivos y cargar imagenes es lo basico de cualquier juego o programa, sin
//  importar el sistema operativo. por eso se incluye en el engine
//
//  Una textura se carga en dos pasos:
//    1) DECODE  archivo -> pixeles RGBA   (depende del sistema: stb en
//                                          PC/Android, ICL en Symbian)
//    2) UPLOAD  pixeles -> textura GL      (comun a todos)
//
//  Esta cabecera NO expone tipos de GL a proposito (el id es un unsigned int)
// ============================================================================

namespace w3dEngine {

    // Carga 'path' del disco y la sube como textura. Devuelve true y deja en
    // 'outId' el id de textura (!=0) si funciono. 'outW'/'outH' (opcionales)
    // reciben el tamano en pixeles. El decode lo resuelve cada plataforma; el
    // upload es comun (UploadRGBA).
    bool LoadTexture(const char* path, unsigned int& outId,
                     int* outW = 0, int* outH = 0);

    // Sube pixeles RGBA 8888 ya decodificados como textura 2D y devuelve su id
    // (0 si falla). 'filtrado' = LINEAR si true, NEAREST si false. Formato
    // interno GL_RGBA: valido tanto en GL de escritorio como en GLES 1.1.
    unsigned int UploadRGBA(const unsigned char* rgba, int w, int h,
                            bool filtrado = true);

    // dimensiones con que se subio una textura (para el aspect ratio). false si no se conoce.
    bool TextureSize(unsigned int id, int& w, int& h);

    // Decodifica una imagen del disco a pixeles RGBA 8888 en el heap, sin
    // subirla a GL. Util cuando se necesitan los PIXELES (no solo el id), p.ej.
    // para armar un sprite. El que llama libera con FreeImage. Devuelve true y
    // deja el buffer en *outRGBA (+ tamano en outW/outH) si funciono. El decode
    // es por plataforma (stb en PC/Android, ICL en Symbian).
    bool DecodeImage(const char* path, unsigned char** outRGBA,
                     int* outW, int* outH);

    // Libera un buffer devuelto por DecodeImage.
    void FreeImage(unsigned char* rgba);

    // Codifica pixeles RGBA 8888 a un buffer PNG RGB en el heap (new[]; liberar con delete[] o
    // FreeImage). Entra RGBA pero el PNG se guarda como RGB: el alpha se descarta (los renders son
    // solidos; un alpha del framebuffer haria "huecos" en materiales con transparencia como pelo/hojas).
    // 'outLen' recibe el tamano en bytes. 'flipY' invierte verticalmente (glReadPixels es bottom-left,
    // el PNG top-left). Portable (deflate stored, sin comprimir): lo usan SavePNG (PC/Web con stdio) y
    // el sink de Symbian (RFile). Devuelve 0 si falla.
    unsigned char* EncodePNG(const unsigned char* rgba, int w, int h, bool flipY, int* outLen);

    // Guarda pixeles RGBA 8888 como PNG RGB en 'path' (= EncodePNG + escribir a disco; el alpha se
    // descarta). Devuelve true si escribio. En PC/Web usa stdio; en Symbian usa RFile (w3dtexload.cpp).
    bool SavePNG(const char* path, const unsigned char* rgba, int w, int h, bool flipY = true);
}
