#include "W3dPack.h"
#include <string.h>
#include <stdio.h>

namespace w3dEngine {

// ---------------------------------------------------------------------------
//  ChaCha20 (D. Bernstein). Cifrador de flujo: cifrar == descifrar (XOR con el
//  keystream). Portable C++03 (compila en el N95 con RVCT, en desktop y en wasm).
//  Sin tablas (a diferencia de AES): codigo chico y sin datos estaticos grandes.
// ---------------------------------------------------------------------------
typedef unsigned int u32;

static u32 rd32le(const unsigned char* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void wr32le(unsigned char* p, u32 v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static u32 rotl32(u32 x, int c) { return (x << c) | (x >> (32 - c)); }

#define QR(a,b,c,d) a+=b; d^=a; d=rotl32(d,16); c+=d; b^=c; b=rotl32(b,12); a+=b; d^=a; d=rotl32(d,8); c+=d; b^=c; b=rotl32(b,7);

static void chachaBlock(unsigned char out[64], const u32 in[16]) {
    u32 x[16]; int i;
    for (i = 0; i < 16; i++) x[i] = in[i];
    for (i = 0; i < 10; i++) { // 20 rounds = 10 double-rounds
        QR(x[0], x[4], x[8],  x[12]) QR(x[1], x[5], x[9],  x[13])
        QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12])
        QR(x[2], x[7], x[8],  x[13]) QR(x[3], x[4], x[9],  x[14])
    }
    for (i = 0; i < 16; i++) wr32le(out + i * 4, x[i] + in[i]);
}

void W3dChaCha20(const unsigned char key[32], unsigned int counter,
                 const unsigned char* in, unsigned char* out, size_t n) {
    u32 st[16];
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574; // "expand 32-byte k"
    for (int i = 0; i < 8; i++) st[4 + i] = rd32le(key + i * 4);
    st[12] = counter; st[13] = 0; st[14] = 0; st[15] = 0; // nonce 0: la clave es UNICA por pack
    unsigned char ks[64];
    size_t off = 0;
    while (off < n) {
        chachaBlock(ks, st);
        st[12]++; // siguiente bloque
        size_t chunk = n - off; if (chunk > 64) chunk = 64;
        for (size_t j = 0; j < chunk; j++) out[off + j] = in[off + j] ^ ks[j];
        off += chunk;
    }
}

// ---------------------------------------------------------------------------
//  W3dPack
// ---------------------------------------------------------------------------
W3dPack::W3dPack() : entries(0), count(0), blob(0), blobLen(0) {}
W3dPack::~W3dPack() { delete[] entries; delete[] blob; }

int W3dPack::Count() const { return count; }

bool W3dPack::OpenMemory(const void* packBytes, size_t packLen, const unsigned char key[32]) {
    delete[] entries; entries = 0; count = 0;
    delete[] blob; blob = 0; blobLen = 0;

    const unsigned char* p = (const unsigned char*)packBytes;
    if (packLen < 12 || memcmp(p, "W3DPACK1", 8) != 0) return false;
    u32 cnt = rd32le(p + 8);
    size_t headerLen = 12 + (size_t)cnt * 40; // por entrada: name(32)+offset(4)+length(4)
    if (packLen < headerLen) return false;

    entries = new Entry[cnt ? cnt : 1];
    count = (int)cnt;
    for (u32 i = 0; i < cnt; i++) {
        const unsigned char* e = p + 12 + (size_t)i * 40;
        memcpy(entries[i].name, e, 32);
        entries[i].offset = rd32le(e + 32);
        entries[i].length = rd32le(e + 36);
    }

    blobLen = packLen - headerLen;
    blob = new unsigned char[blobLen ? blobLen : 1];
    W3dChaCha20(key, 0, p + headerLen, blob, blobLen); // descifra a nuestro buffer

    for (u32 i = 0; i < cnt; i++) { // los offsets deben caer dentro del blob
        if ((size_t)entries[i].offset + entries[i].length > blobLen) {
            delete[] entries; entries = 0; count = 0;
            delete[] blob; blob = 0; blobLen = 0;
            return false;
        }
    }
    return true;
}

bool W3dPack::OpenFile(const char* path, const unsigned char key[32]) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    unsigned char* buf = new unsigned char[n];
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    bool ok = (rd == (size_t)n) && OpenMemory(buf, (size_t)n, key);
    delete[] buf;
    return ok;
}

const unsigned char* W3dPack::Get(const char* name, size_t* outLen) const {
    for (int i = 0; i < count; i++) {
        if (strncmp(entries[i].name, name, 32) == 0) {
            if (outLen) *outLen = entries[i].length;
            return blob + entries[i].offset;
        }
    }
    if (outLen) *outLen = 0;
    return 0;
}

bool W3dPackBuild(const char* outPath, const char* const* names,
                  const unsigned char* const* datas, const size_t* lens,
                  int count, const unsigned char key[32]) {
    size_t blobLen = 0;
    for (int i = 0; i < count; i++) blobLen += lens[i];

    unsigned char* plain = new unsigned char[blobLen ? blobLen : 1];
    u32* offs = new u32[count ? count : 1];
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        offs[i] = (u32)off;
        memcpy(plain + off, datas[i], lens[i]);
        off += lens[i];
    }
    unsigned char* enc = new unsigned char[blobLen ? blobLen : 1];
    W3dChaCha20(key, 0, plain, enc, blobLen);

    FILE* f = fopen(outPath, "wb");
    bool ok = false;
    if (f) {
        unsigned char b4[4];
        fwrite("W3DPACK1", 1, 8, f);
        wr32le(b4, (u32)count); fwrite(b4, 1, 4, f);
        for (int i = 0; i < count; i++) {
            char nm[32]; memset(nm, 0, 32);
            strncpy(nm, names[i], 31); // nombre < 32 chars
            fwrite(nm, 1, 32, f);
            wr32le(b4, offs[i]);         fwrite(b4, 1, 4, f);
            wr32le(b4, (u32)lens[i]);    fwrite(b4, 1, 4, f);
        }
        fwrite(enc, 1, blobLen, f);
        fclose(f);
        ok = true;
    }
    delete[] plain; delete[] offs; delete[] enc;
    return ok;
}

} // namespace w3dEngine
