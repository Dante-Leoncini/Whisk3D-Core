#pragma once
// ============================================================================
//  Whisk3DCore — COMPRESION (zlib/deflate). Abstraccion PORTABLE (los 4 OS), SIN
//  dependencias externas: reusa el decoder de zlib de stb_image que ya vive con
//  el core. El editor NO toca los detalles: usa esta abstraccion.
//
//  La usa el importador FBX (sus arrays de vertices/uv/normales suelen venir
//  zlib-comprimidos) y va a ser la BASE del formato propio .w3d (archivo
//  comprimido). Aca solo el id de textura queda afuera: es puro bytes -> bytes.
// ============================================================================
namespace w3dEngine {

    // Descomprime datos ZLIB a un buffer de tamano CONOCIDO. 'out' debe medir outLen (el tamano descomprimido
    // EXACTO, que en FBX/.w3d viene en el header). Devuelve true si descomprimio EXACTAMENTE outLen bytes.
    bool Inflate(const unsigned char* in, int inLen, unsigned char* out, int outLen);

    // Descomprime ZLIB cuando NO se conoce el tamano de salida: aloca el buffer (con new[]; liberar con
    // FreeInflated) y deja su largo en *outLen. Devuelve el buffer, o 0 si falla. (Para el .w3d, si algun
    // bloque no guarda el tamano descomprimido.)
    unsigned char* InflateHeap(const unsigned char* in, int inLen, int* outLen);
    void FreeInflated(unsigned char* buf);

    // (DEFLATE / compresion: se agrega cuando haga falta escribir .w3d. Por ahora el core solo LEE comprimido.)
}
