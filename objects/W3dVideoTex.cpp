#include "W3dVideoTex.h"
#include "../io/w3dCompress.h"
#include "graphics/w3dTexture.h"
#include "../base/w3dlog.h"
#include <stdlib.h>
#include <string.h>

namespace w3dEngine {

namespace {
unsigned int ReadU16(const unsigned char* p) { return (unsigned int)p[0] | ((unsigned int)p[1] << 8); }
unsigned int ReadU32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
float ReadF32(const unsigned char* p) {
    const unsigned int u = ReadU32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
bool Pot(int n) { return n > 0 && (n & (n - 1)) == 0; }
}

W3dVideoTex::W3dVideoTex()
    : file(0), width(0), height(0), key(1), frames(0), fps(15.0f), table(0), px(0), tmp(0), read(0), readMax(0),
      decode(-1), current(-1), visible(0), nDecode(0), nUploaded(0), readBytes(0) {
    ids[0] = ids[1] = 0;
}

W3dVideoTex::~W3dVideoTex() { Close(); }

void W3dVideoTex::Close() {
    if (file) fclose(file);
    file = 0;
    for (int i = 0; i < 2; i++) {
        if (ids[i]) DeleteTexture(ids[i]);
        ids[i] = 0;
    }
    free(table);
    free(px);
    free(tmp);
    free(read);
    table = 0;
    px = tmp = 0;
    read = 0;
    readMax = 0;
    width  = height = frames = 0;
    key    = 1;
    decode = current = -1;
    visible = 0;
    nDecode = nUploaded = 0;
    readBytes = 0;
}

bool W3dVideoTex::Open(const char* path) {
    Close();
    if (!path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    unsigned char cab[32];
    if (fread(cab, 1, 32, f) != 32 || memcmp(cab, "W3DV", 4) != 0 || ReadU16(cab + 4) != 1 || ReadU16(cab + 6) != 1) {
        w3dLogf("W3dVideoTex: %s no es un video RGB565 (W3DV 1)", path);
        fclose(f);
        return false;
    }
    const int w = (int)ReadU16(cab + 8), h = (int)ReadU16(cab + 10);
    const int n = (int)ReadU32(cab + 20);
    if (!Pot(w) || !Pot(h) || w > 512 || h > 512 || n <= 0 || n > 1000000) {
        w3dLogf("W3dVideoTex: %s: %dx%d con %d cuadros (los lados van en potencias de dos)", path, w, h, n);
        fclose(f);
        return false;
    }
    unsigned char* t = (unsigned char*)malloc((size_t)(n + 1) * 4);
    table = (unsigned int*)malloc((size_t)(n + 1) * sizeof(unsigned int));
    px = (unsigned short*)malloc((size_t)w * h * 2);
    tmp = (unsigned short*)malloc((size_t)w * h * 2);
    if (!t || !table || !px || !tmp || fread(t, 4, (size_t)(n + 1), f) != (size_t)(n + 1)) {
        free(t);
        file = f;
        Close();
        w3dLogf("W3dVideoTex: %s cortado o sin memoria", path);
        return false;
    }
    for (int k = 0; k <= n; k++) table[k] = ReadU32(t + k * 4);
    free(t);
    file = f;
    width = w;
    height = h;
    key = (int)ReadU16(cab + 12);
    if (key < 1) key = 1;
    fps = ReadF32(cab + 16);
    if (!(fps > 0.0f)) fps = 15.0f;
    frames = n;
    // las dos texturas con el primer cuadro (la que se escribe despues tambien: si se ve antes de tiempo, no es basura)
    memset(px, 0, (size_t)w * h * 2);
    Decode(0);
    for (int i = 0; i < 2; i++) ids[i] = Reserve565(0, w, h, px);
    visible = 0;
    current = 0;
    if (!ids[0] || !ids[1]) {
        w3dLogf("W3dVideoTex: %s: no se pudieron crear las texturas", path);
        Close();
        return false;
    }
    return true;
}

bool W3dVideoTex::Decode(int k) {
    if (!file || k < 0 || k >= frames) return false;
    const unsigned int from = table[k], until = table[k + 1];
    if (until < from + 4) return false;
    const int length = (int)(until - from);
    if (length > readMax) {
        unsigned char* newValue = (unsigned char*)realloc(read, (size_t)length);
        if (!newValue) return false;
        read = newValue;
        readMax = length;
    }
    if (fseek(file, (long)from, SEEK_SET) != 0 || fread(read, 1, (size_t)length, file) != (size_t)length) return false;
    readBytes += length;
    const int type = read[0];
    const int bytes = width * height * 2;
    if (type == 2) {
        // (igual al anterior: si el anterior es el que esta, no hay nada que hacer)
        if (decode != k - 1) return false;
    } else if (type == 0) {
        if (!Inflate(read + 4, length - 4, (unsigned char*)px, bytes)) return false;
    } else if (type == 1) {
        if (decode != k - 1) return false;
        if (!Inflate(read + 4, length - 4, (unsigned char*)tmp, bytes)) return false;
        // (de a 32 bits: las filas son pares de pixeles)
        unsigned int* a = (unsigned int*)px;
        const unsigned int* b = (const unsigned int*)tmp;
        for (int i = bytes / 4; i > 0; i--) *a++ ^= *b++;
    } else {
        return false;
    }
    decode = k;
    nDecode++;
    return true;
}

unsigned int W3dVideoTex::GoFrame(int k) {
    if (!file) return 0;
    if (k < 0) k = 0;
    if (k >= frames) k = frames - 1;
    if (k == current) return ids[visible];
    // de donde se arranca: el que esta si viene antes y no hay un entero mas cerca, si no el entero de k
    const int integer = (k / key) * key;
    int from;
    if (decode >= 0 && decode <= k && decode >= integer) from = decode + 1;
    else from = integer;
    for (int i = from; i <= k; i++) {
        if (!Decode(i)) {
            // (algo fallo: se rearma desde el entero; si ese tampoco, queda lo que se ve)
            decode = -1;
            if (i == integer || !Decode(integer)) return ids[visible];
            for (int j = integer + 1; j <= k; j++)
                if (!Decode(j)) return ids[visible];
            break;
        }
    }
    if (decode != k) return ids[visible];
    const int other = 1 - visible;
    if (Update565(ids[other], width, height, px)) {
        visible = other;
        nUploaded++;
    }
    current = k;
    return ids[visible];
}

unsigned int W3dVideoTex::Go(float t) {
    if (!file) return 0;
    const int k = t <= 0.0f ? 0 : (int)(t * fps + 0.0001f);
    return GoFrame(k);
}

}
