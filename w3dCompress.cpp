#include "w3dCompress.h"
#include <cstdlib> // free (stb aloca con su STBI_MALLOC = malloc)

// El decoder de zlib de stb_image esta IMPLEMENTADO en w3dTexture.cpp (STB_IMAGE_IMPLEMENTATION). Aca solo lo
// declaramos para reusarlo sin arrastrar todo stb_image.h. OJO: stb envuelve su API en extern "C" (linkage C).
extern "C" char* stbi_zlib_decode_malloc(const char* buffer, int len, int* outlen);
extern "C" int   stbi_zlib_decode_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

namespace w3dEngine {

    bool Inflate(const unsigned char* in, int inLen, unsigned char* out, int outLen) {
        if (!in || !out || inLen <= 0 || outLen <= 0) return false;
        int got = stbi_zlib_decode_buffer((char*)out, outLen, (const char*)in, inLen);
        return got == outLen;
    }

    unsigned char* InflateHeap(const unsigned char* in, int inLen, int* outLen) {
        if (outLen) *outLen = 0;
        if (!in || inLen <= 0) return 0;
        int n = 0;
        char* raw = stbi_zlib_decode_malloc((const char*)in, inLen, &n); // aloca con el malloc de stb
        if (!raw || n <= 0) { return 0; }
        unsigned char* out = new unsigned char[n]; // devolver siempre un buffer new[] (liberar con FreeInflated)
        for (int i = 0; i < n; i++) out[i] = (unsigned char)raw[i];
        free(raw); // stb alocó con malloc
        if (outLen) *outLen = n;
        return out;
    }

    void FreeInflated(unsigned char* buf) { delete[] buf; }
}
