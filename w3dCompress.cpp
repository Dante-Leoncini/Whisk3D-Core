#include "w3dCompress.h"
#include <string.h> // memset, memcpy
#include <limits.h> // UINT_MAX

// ============================================================================
//  Whisk3DCore — inflate DEFLATE/zlib SELF-CONTAINED. Dominio publico, basado en
//  el decoder de zlib de stb_image (Sean Barrett, v0.2). Reescrito SIN depender de
//  stb_image: typedefs propios y buffers new[]/delete[] (nada de malloc/realloc ni
//  libs externas) -> compila igual en los 4 OS, incluido Symbian (C++03). Es la
//  base para leer los arrays comprimidos del FBX y, a futuro, el .w3d (zip).
// ============================================================================

namespace {

typedef unsigned char  w3z_u8;
typedef unsigned short w3z_u16;
typedef unsigned int   w3z_u32;

#define W3Z_ZFAST_BITS 9
#define W3Z_ZFAST_MASK ((1 << W3Z_ZFAST_BITS) - 1)
#define W3Z_ZNSYMS     288 // simbolos del alfabeto literal/length

struct w3z_huff {
    w3z_u16 fast[1 << W3Z_ZFAST_BITS];
    w3z_u16 firstcode[16];
    int     maxcode[17];
    w3z_u16 firstsymbol[16];
    w3z_u8  size[W3Z_ZNSYMS];
    w3z_u16 value[W3Z_ZNSYMS];
};

struct w3z_buf {
    w3z_u8 *zbuffer, *zbuffer_end;
    int     num_bits;
    int     hit_zeof_once;
    w3z_u32 code_buffer;
    char   *zout, *zout_start, *zout_end;
    int     z_expandable;
    w3z_huff z_length, z_distance;
};

static int w3z_bitreverse16(int n) {
    n = ((n & 0xAAAA) >> 1) | ((n & 0x5555) << 1);
    n = ((n & 0xCCCC) >> 2) | ((n & 0x3333) << 2);
    n = ((n & 0xF0F0) >> 4) | ((n & 0x0F0F) << 4);
    n = ((n & 0xFF00) >> 8) | ((n & 0x00FF) << 8);
    return n;
}
static int w3z_bit_reverse(int v, int bits) { return w3z_bitreverse16(v) >> (16 - bits); }

static int w3z_build_huffman(w3z_huff *z, const w3z_u8 *sizelist, int num) {
    int i, k = 0;
    int code, next_code[16], sizes[17];
    memset(sizes, 0, sizeof(sizes));
    memset(z->fast, 0, sizeof(z->fast));
    for (i = 0; i < num; ++i) ++sizes[sizelist[i]];
    sizes[0] = 0;
    for (i = 1; i < 16; ++i) if (sizes[i] > (1 << i)) return 0;
    code = 0;
    for (i = 1; i < 16; ++i) {
        next_code[i] = code;
        z->firstcode[i]   = (w3z_u16)code;
        z->firstsymbol[i] = (w3z_u16)k;
        code = (code + sizes[i]);
        if (sizes[i]) if (code - 1 >= (1 << i)) return 0;
        z->maxcode[i] = code << (16 - i); // preshift para el loop interno
        code <<= 1;
        k += sizes[i];
    }
    z->maxcode[16] = 0x10000; // centinela
    for (i = 0; i < num; ++i) {
        int s = sizelist[i];
        if (s) {
            int c = next_code[s] - z->firstcode[s] + z->firstsymbol[s];
            w3z_u16 fastv = (w3z_u16)((s << 9) | i);
            z->size[c]  = (w3z_u8)s;
            z->value[c] = (w3z_u16)i;
            if (s <= W3Z_ZFAST_BITS) {
                int j = w3z_bit_reverse(next_code[s], s);
                while (j < (1 << W3Z_ZFAST_BITS)) { z->fast[j] = fastv; j += (1 << s); }
            }
            ++next_code[s];
        }
    }
    return 1;
}

static int w3z_zeof(w3z_buf *z) { return (z->zbuffer >= z->zbuffer_end); }
static w3z_u8 w3z_zget8(w3z_buf *z) { return w3z_zeof(z) ? 0 : *z->zbuffer++; }

static void w3z_fill_bits(w3z_buf *z) {
    do {
        if (z->code_buffer >= (1U << z->num_bits)) { z->zbuffer = z->zbuffer_end; return; } // tratar como EOF
        z->code_buffer |= (unsigned int)w3z_zget8(z) << z->num_bits;
        z->num_bits += 8;
    } while (z->num_bits <= 24);
}

static unsigned int w3z_zreceive(w3z_buf *z, int n) {
    unsigned int k;
    if (z->num_bits < n) w3z_fill_bits(z);
    k = z->code_buffer & ((1 << n) - 1);
    z->code_buffer >>= n;
    z->num_bits -= n;
    return k;
}

static int w3z_huffman_decode_slowpath(w3z_buf *a, w3z_huff *z) {
    int b, s, k;
    k = w3z_bit_reverse(a->code_buffer, 16);
    for (s = W3Z_ZFAST_BITS + 1; ; ++s) if (k < z->maxcode[s]) break;
    if (s >= 16) return -1; // codigo invalido
    b = (k >> (16 - s)) - z->firstcode[s] + z->firstsymbol[s];
    if (b >= W3Z_ZNSYMS) return -1;
    if (z->size[b] != s) return -1;
    a->code_buffer >>= s;
    a->num_bits -= s;
    return z->value[b];
}

static int w3z_huffman_decode(w3z_buf *a, w3z_huff *z) {
    int b, s;
    if (a->num_bits < 16) {
        if (w3z_zeof(a)) {
            if (!a->hit_zeof_once) { a->hit_zeof_once = 1; a->num_bits += 16; } // 16 bits cero implicitos
            else return -1; // stream terminado antes de tiempo
        } else {
            w3z_fill_bits(a);
        }
    }
    b = z->fast[a->code_buffer & W3Z_ZFAST_MASK];
    if (b) { s = b >> 9; a->code_buffer >>= s; a->num_bits -= s; return b & 511; }
    return w3z_huffman_decode_slowpath(a, z);
}

// crece el buffer de salida (solo si z_expandable): new[] mas grande, copia lo decodificado, borra el viejo.
static int w3z_expand(w3z_buf *z, char *zout, int n) {
    char *q;
    unsigned int cur, limit;
    z->zout = zout;
    if (!z->z_expandable) return 0;
    cur   = (unsigned int)(z->zout - z->zout_start);
    limit = (unsigned int)(z->zout_end - z->zout_start);
    if (limit == 0) limit = 1;
    if (UINT_MAX - cur < (unsigned)n) return 0;
    while (cur + (unsigned)n > limit) { if (limit > UINT_MAX / 2) return 0; limit *= 2; }
    q = new char[limit];
    if (q == 0) return 0;
    memcpy(q, z->zout_start, cur);
    delete[] z->zout_start;
    z->zout_start = q;
    z->zout       = q + cur;
    z->zout_end   = q + limit;
    return 1;
}

static const int w3z_length_base[31] = {
    3,4,5,6,7,8,9,10,11,13, 15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258,0,0 };
static const int w3z_length_extra[31] =
    { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0,0,0 };
static const int w3z_dist_base[32] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577,0,0 };
static const int w3z_dist_extra[32] =
    { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int w3z_parse_huffman_block(w3z_buf *a) {
    char *zout = a->zout;
    for (;;) {
        int z = w3z_huffman_decode(a, &a->z_length);
        if (z < 256) {
            if (z < 0) return 0;
            if (zout >= a->zout_end) { if (!w3z_expand(a, zout, 1)) return 0; zout = a->zout; }
            *zout++ = (char)z;
        } else {
            w3z_u8 *p;
            int len, dist;
            if (z == 256) {
                a->zout = zout;
                if (a->hit_zeof_once && a->num_bits < 16) return 0; // consumio el padding -> stream mal formado
                return 1;
            }
            if (z >= 286) return 0;
            z -= 257;
            len = w3z_length_base[z];
            if (w3z_length_extra[z]) len += w3z_zreceive(a, w3z_length_extra[z]);
            z = w3z_huffman_decode(a, &a->z_distance);
            if (z < 0 || z >= 30) return 0;
            dist = w3z_dist_base[z];
            if (w3z_dist_extra[z]) dist += w3z_zreceive(a, w3z_dist_extra[z]);
            if (zout - a->zout_start < dist) return 0;
            if (len > a->zout_end - zout) { if (!w3z_expand(a, zout, len)) return 0; zout = a->zout; }
            p = (w3z_u8 *)(zout - dist);
            if (dist == 1) { w3z_u8 v = *p; if (len) { do *zout++ = v; while (--len); } }
            else           { if (len) { do *zout++ = *p++; while (--len); } }
        }
    }
}

static int w3z_compute_huffman_codes(w3z_buf *a) {
    static const w3z_u8 length_dezigzag[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    w3z_huff z_codelength;
    w3z_u8 lencodes[286 + 32 + 137]; // padding para el maximo op
    w3z_u8 codelength_sizes[19];
    int i, n;
    int hlit  = w3z_zreceive(a, 5) + 257;
    int hdist = w3z_zreceive(a, 5) + 1;
    int hclen = w3z_zreceive(a, 4) + 4;
    int ntot  = hlit + hdist;
    memset(codelength_sizes, 0, sizeof(codelength_sizes));
    for (i = 0; i < hclen; ++i) {
        int s = w3z_zreceive(a, 3);
        codelength_sizes[length_dezigzag[i]] = (w3z_u8)s;
    }
    if (!w3z_build_huffman(&z_codelength, codelength_sizes, 19)) return 0;
    n = 0;
    while (n < ntot) {
        int c = w3z_huffman_decode(a, &z_codelength);
        if (c < 0 || c >= 19) return 0;
        if (c < 16) lencodes[n++] = (w3z_u8)c;
        else {
            w3z_u8 fill = 0;
            if (c == 16)      { c = w3z_zreceive(a, 2) + 3; if (n == 0) return 0; fill = lencodes[n - 1]; }
            else if (c == 17) { c = w3z_zreceive(a, 3) + 3; }
            else if (c == 18) { c = w3z_zreceive(a, 7) + 11; }
            else return 0;
            if (ntot - n < c) return 0;
            memset(lencodes + n, fill, c);
            n += c;
        }
    }
    if (n != ntot) return 0;
    if (!w3z_build_huffman(&a->z_length,   lencodes, hlit)) return 0;
    if (!w3z_build_huffman(&a->z_distance, lencodes + hlit, hdist)) return 0;
    return 1;
}

static int w3z_parse_uncompressed_block(w3z_buf *a) {
    w3z_u8 header[4];
    int len, nlen, k;
    if (a->num_bits & 7) w3z_zreceive(a, a->num_bits & 7); // descartar hasta byte
    k = 0;
    while (a->num_bits > 0) {
        header[k++] = (w3z_u8)(a->code_buffer & 255);
        a->code_buffer >>= 8;
        a->num_bits -= 8;
    }
    if (a->num_bits < 0) return 0;
    while (k < 4) header[k++] = w3z_zget8(a);
    len  = header[1] * 256 + header[0];
    nlen = header[3] * 256 + header[2];
    if (nlen != (len ^ 0xffff)) return 0;
    if (a->zbuffer + len > a->zbuffer_end) return 0;
    if (a->zout + len > a->zout_end) if (!w3z_expand(a, a->zout, len)) return 0;
    memcpy(a->zout, a->zbuffer, len);
    a->zbuffer += len;
    a->zout += len;
    return 1;
}

static int w3z_parse_zlib_header(w3z_buf *a) {
    int cmf = w3z_zget8(a);
    int cm  = cmf & 15;
    int flg = w3z_zget8(a);
    if (w3z_zeof(a)) return 0;
    if ((cmf * 256 + flg) % 31 != 0) return 0; // spec zlib
    if (flg & 32) return 0;                    // preset dict no permitido
    if (cm != 8)  return 0;                     // DEFLATE
    return 1;
}

static const w3z_u8 w3z_default_length[W3Z_ZNSYMS] = {
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8 };
static const w3z_u8 w3z_default_distance[32] = {
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5 };

static int w3z_parse_zlib(w3z_buf *a, int parse_header) {
    int final, type;
    if (parse_header) if (!w3z_parse_zlib_header(a)) return 0;
    a->num_bits = 0;
    a->code_buffer = 0;
    a->hit_zeof_once = 0;
    do {
        final = w3z_zreceive(a, 1);
        type  = w3z_zreceive(a, 2);
        if (type == 0) { if (!w3z_parse_uncompressed_block(a)) return 0; }
        else if (type == 3) { return 0; }
        else {
            if (type == 1) {
                if (!w3z_build_huffman(&a->z_length,   w3z_default_length,   W3Z_ZNSYMS)) return 0;
                if (!w3z_build_huffman(&a->z_distance, w3z_default_distance, 32)) return 0;
            } else {
                if (!w3z_compute_huffman_codes(a)) return 0;
            }
            if (!w3z_parse_huffman_block(a)) return 0;
        }
    } while (!final);
    return 1;
}

static int w3z_do_zlib(w3z_buf *a, char *obuf, int olen, int exp, int parse_header) {
    a->zout_start   = obuf;
    a->zout         = obuf;
    a->zout_end     = obuf + olen;
    a->z_expandable = exp;
    return w3z_parse_zlib(a, parse_header);
}

// buffer de tamano CONOCIDO (sin expandir). Devuelve bytes decodificados, o -1.
static int w3z_decode_buffer(char *obuffer, int olen, const char *ibuffer, int ilen) {
    w3z_buf a;
    a.zbuffer     = (w3z_u8 *)ibuffer;
    a.zbuffer_end = (w3z_u8 *)ibuffer + ilen;
    if (w3z_do_zlib(&a, obuffer, olen, 0, 1)) return (int)(a.zout - a.zout_start);
    return -1;
}

// tamano DESCONOCIDO: aloca (new char[], crece solo) y devuelve el buffer + su largo. 0 si falla.
static char *w3z_decode_malloc(const char *buffer, int len, int *outlen) {
    w3z_buf a;
    const int initial = 16384;
    char *p = new char[initial];
    if (p == 0) return 0;
    a.zbuffer     = (w3z_u8 *)buffer;
    a.zbuffer_end = (w3z_u8 *)buffer + len;
    if (w3z_do_zlib(&a, p, initial, 1, 1)) {
        if (outlen) *outlen = (int)(a.zout - a.zout_start);
        return a.zout_start;
    }
    delete[] a.zout_start;
    return 0;
}

} // namespace anonimo

namespace w3dEngine {

    bool Inflate(const unsigned char* in, int inLen, unsigned char* out, int outLen) {
        if (!in || !out || inLen <= 0 || outLen <= 0) return false;
        int got = w3z_decode_buffer((char*)out, outLen, (const char*)in, inLen);
        return got == outLen;
    }

    unsigned char* InflateHeap(const unsigned char* in, int inLen, int* outLen) {
        if (outLen) *outLen = 0;
        if (!in || inLen <= 0) return 0;
        int n = 0;
        char* raw = w3z_decode_malloc((const char*)in, inLen, &n); // ya es new char[]
        if (!raw || n <= 0) { if (raw) delete[] raw; return 0; }
        // copiar a un new unsigned char[] EXACTO (contrato: FreeInflated hace delete[] sobre unsigned char*)
        unsigned char* out = new unsigned char[n];
        for (int i = 0; i < n; i++) out[i] = (unsigned char)raw[i];
        delete[] raw;
        if (outLen) *outLen = n;
        return out;
    }

    void FreeInflated(unsigned char* buf) { delete[] buf; }
}
