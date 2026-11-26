// Whisk3DCore — dibujo directo a la PVR de la Dreamcast (ver w3dPVR.h)
#ifdef W3D_DREAMCAST
#include "w3dPVR.h"
#include <kos.h>
#include <dc/pvr.h>
#include <dc/matrix.h>
#include <dc/video.h>
#include <string.h>

namespace w3dPVR {

namespace {
const pvr_list_t kLists[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };
int gList = -1;                 // la lista abierta
float gWNear = 0.05f;
bool gBanks[64];                // los bancos de 16 colores de la paleta
Count gCount;
matrix_t gMatrix __attribute__((aligned(32)));   // la cargada en XMTRX (para recortar hace falta otra vez)
uint32_t gAdded = 0;         // el color sumado de los vertices (AddedColor)
size_t gFinished = 0;        // los cuadros mandados (FinishFrame) desde Iniciar
size_t gShownBase = 0;       // los que KallistiOS ya habia mostrado al Iniciar

// EL ENVIO RAPIDO (las tiras enteras, que son casi todo): el puntero de la store queue en un registro todo el bucle
// (pvr_dr_target lo lee y lo escribe en memoria por cada vertice) y las 8 palabras guardadas de atras para adelante
// con predecremento: el SH-4 no tiene fmov con desplazamiento y cada una costaba tres instrucciones. Despues, el pref
// que la manda a la PVR. 'sq' es la store queue de este vertice (alternan: sq ^= 32 antes de cada uno). Lo del vertice
// va en registros de punto flotante (los enteros no alcanzan para el bucle y el compilador los mandaba a la pila).
// Lo del vertice se lee adentro, con post-incremento (fmov.s @Rm+: sin cuentas de direcciones): x, y, z de
// 'xyz' (o los ya calculados) y u, v de 'uv'. Lo que leen va declarado ("m"): con un "memory" el compilador tenia que
// dejar en la memoria todo lo del bucle (la store queue, los punteros) y releerlo en cada vertice
inline void SendSQOf(uint32_t sq, uint32_t flags, const float* xyz, const float* uv, uint32_t argb, uint32_t oargb) {
    uint32_t p = sq + 32;
    float a, b, c, d, e;
    __asm__ __volatile__(
        "fmov.s  @%[xyz]+, %[a]\n\t"
        "fmov.s  @%[xyz]+, %[b]\n\t"
        "fmov.s  @%[xyz], %[c]\n\t"
        "fmov.s  @%[uv]+, %[d]\n\t"
        "fmov.s  @%[uv], %[e]\n\t"
        "mov.l   %[oargb], @-%[p]\n\t"
        "mov.l   %[argb], @-%[p]\n\t"
        "fmov.s  %[e], @-%[p]\n\t"
        "fmov.s  %[d], @-%[p]\n\t"
        "fmov.s  %[c], @-%[p]\n\t"
        "fmov.s  %[b], @-%[p]\n\t"
        "fmov.s  %[a], @-%[p]\n\t"
        "mov.l   %[flags], @-%[p]\n\t"
        "pref    @%[p]\n\t"
        : [p] "+r"(p), [xyz] "+r"(xyz), [uv] "+r"(uv), [a] "=&f"(a), [b] "=&f"(b), [c] "=&f"(c), [d] "=&f"(d),
          [e] "=&f"(e)
        : [oargb] "r"(oargb), [argb] "r"(argb), [flags] "r"(flags), "m"(*(const float(*)[3])xyz),
          "m"(*(const float(*)[2])uv));
}

inline void SendSQUV(uint32_t sq, uint32_t flags, float x, float y, float z, const float* uv, uint32_t argb,
                       uint32_t oargb) {
    uint32_t p = sq + 32;
    float d, e;
    __asm__ __volatile__(
        "fmov.s  @%[uv]+, %[d]\n\t"
        "fmov.s  @%[uv], %[e]\n\t"
        "mov.l   %[oargb], @-%[p]\n\t"
        "mov.l   %[argb], @-%[p]\n\t"
        "fmov.s  %[e], @-%[p]\n\t"
        "fmov.s  %[d], @-%[p]\n\t"
        "fmov.s  %[z], @-%[p]\n\t"
        "fmov.s  %[y], @-%[p]\n\t"
        "fmov.s  %[x], @-%[p]\n\t"
        "mov.l   %[flags], @-%[p]\n\t"
        "pref    @%[p]\n\t"
        : [p] "+r"(p), [uv] "+r"(uv), [d] "=&f"(d), [e] "=&f"(e)
        : [oargb] "r"(oargb), [argb] "r"(argb), [x] "f"(x), [y] "f"(y), [z] "f"(z), [flags] "r"(flags),
          "m"(*(const float(*)[2])uv));
}

// x, y, z de 'p' (con w = 1) por la matriz cargada en XMTRX: lo proyectado (sin dividir) en x, y, w
inline void Project(const float* p, float& x, float& y, float& w) {
    register float f0 __asm__("fr0");
    register float f1 __asm__("fr1");
    register float f2 __asm__("fr2");
    register float f3 __asm__("fr3") = 1.0f;
    __asm__ __volatile__(
        "fmov.s  @%[p]+, fr0\n\t"
        "fmov.s  @%[p]+, fr1\n\t"
        "fmov.s  @%[p], fr2\n\t"
        "ftrv    xmtrx, fv0\n\t"
        : [p] "+r"(p), "=&f"(f0), "=&f"(f1), "=&f"(f2), "+f"(f3)
        : "m"(*(const float(*)[3])p));
    x = f0; y = f1; w = f3;
}

// las tiras de Bloques (StripsBlocks), el bucle entero aca: por indice (con END_STRIP, como SendModel) su Bloque
// (24 bytes: bandera, x, y, z, u, v) copiado de a pares (con fschg, fmov mueve 8 bytes), los colores y la bandera
// encima de la del Bloque. Devuelve la store queue del ultimo
inline uint32_t SendBlocks(uint32_t sq, const Block* b, const uint16_t* idx, int n, uint32_t argb, uint32_t oargb) {
    const uint32_t vertex = PVR_CMD_VERTEX, x32 = 32, twelve = 12;
    const int end = 28;
    uint32_t k, f, q;
    __asm__ __volatile__(
        "1:\n\t"
        "mov.w   @%[idx]+, %[k]\n\t"
        "shll    %[k]\n\t"
        "movt    %[f]\n\t"
        "extu.w  %[k], %[k]\n\t"
        "mulu.w  %[doce], %[k]\n\t"
        "shld    %[fin], %[f]\n\t"
        "or      %[vertice], %[f]\n\t"
        "xor     %[x32], %[sq]\n\t"
        "mov     %[sq], %[q]\n\t"
        "add     #32, %[q]\n\t"
        "sts     macl, %[k]\n\t"
        "add     %[b], %[k]\n\t"
        "fschg\n\t"
        "fmov    @%[k]+, dr0\n\t"
        "fmov    @%[k]+, dr2\n\t"
        "fmov    @%[k], dr4\n\t"
        "mov.l   %[oargb], @-%[q]\n\t"
        "mov.l   %[argb], @-%[q]\n\t"
        "fmov    dr4, @-%[q]\n\t"
        "fmov    dr2, @-%[q]\n\t"
        "fmov    dr0, @-%[q]\n\t"
        "fschg\n\t"
        "mov.l   %[f], @%[q]\n\t"
        "dt      %[n]\n\t"
        "bf.s    1b\n\t"
        "pref    @%[q]\n\t"
        : [idx] "+r"(idx), [n] "+r"(n), [sq] "+r"(sq), [k] "=&r"(k), [f] "=&r"(f), [q] "=&r"(q)
        : [b] "r"(b), [twelve] "r"(twelve), [end] "r"(end), [vertex] "r"(vertex), [x32] "r"(x32), [argb] "r"(argb),
          [oargb] "r"(oargb)
        : "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "macl", "t", "memory");
    return sq;
}

// las tiras de un modelo (StripsModel), el bucle entero aca: por indice, el registro de su vertice (k << 5), FTRV,
// 1/w con FSRRA y las 8 palabras a la store queue. La bandera sale del bit de arriba del indice (END_STRIP: shll lo
// deja en T). Devuelve la store queue del ultimo
inline uint32_t SendModel(uint32_t sq, const VertexModel* v, const uint16_t* idx, int n, uint32_t oargb) {
    const uint32_t vertex = PVR_CMD_VERTEX, x32 = 32;
    const int four = 4, end = 28;
    uint32_t k, b, q;
    __asm__ __volatile__(
        "1:\n\t"
        "mov.w   @%[idx]+, %[k]\n\t"
        "shll    %[k]\n\t"
        "movt    %[b]\n\t"
        "extu.w  %[k], %[k]\n\t"
        "shld    %[cuatro], %[k]\n\t"
        "add     %[v], %[k]\n\t"
        "fmov.s  @%[k]+, fr0\n\t"
        "fmov.s  @%[k]+, fr1\n\t"
        "fmov.s  @%[k]+, fr2\n\t"
        "fldi1   fr3\n\t"
        "ftrv    xmtrx, fv0\n\t"
        "fmov.s  @%[k]+, fr4\n\t"
        "fmov.s  @%[k]+, fr5\n\t"
        "mov.l   @%[k], %[k]\n\t"
        "shld    %[fin], %[b]\n\t"
        "or      %[vertice], %[b]\n\t"
        "xor     %[x32], %[sq]\n\t"
        "mov     %[sq], %[q]\n\t"
        "add     #32, %[q]\n\t"
        "fmul    fr3, fr3\n\t"
        "fsrra   fr3\n\t"
        "fmul    fr3, fr0\n\t"
        "fmul    fr3, fr1\n\t"
        "mov.l   %[oargb], @-%[q]\n\t"
        "mov.l   %[k], @-%[q]\n\t"
        "fmov.s  fr5, @-%[q]\n\t"
        "fmov.s  fr4, @-%[q]\n\t"
        "fmov.s  fr3, @-%[q]\n\t"
        "fmov.s  fr1, @-%[q]\n\t"
        "fmov.s  fr0, @-%[q]\n\t"
        "mov.l   %[b], @-%[q]\n\t"
        "dt      %[n]\n\t"
        "bf.s    1b\n\t"
        "pref    @%[q]\n\t"
        : [idx] "+r"(idx), [n] "+r"(n), [sq] "+r"(sq), [k] "=&r"(k), [b] "=&r"(b), [q] "=&r"(q)
        : [v] "r"(v), [four] "r"(four), [end] "r"(end), [vertex] "r"(vertex), [x32] "r"(x32),
          [oargb] "r"(oargb)
        : "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "t", "memory");
    return sq;
}

// recorre las tiras de 'idx' (separadas por CORTE) y por cada indice llama a one(k, flags): el ultimo de cada tira con
// su fin. El bucle de adentro no pregunta nada mas (cuantos van se cuenta por tira). Devuelve los que mando
template <class F> inline int ByStrips(const uint16_t* idx, int n, F one) {
    const uint16_t* p = idx;
    const uint16_t* const end = idx + n;
    int sent = 0;
    while (p < end) {
        if (*p == CUT) { p++; continue; }
        const uint16_t* q = p + 1;
        while (q < end && *q != CUT) q++;
        sent += (int)(q - p);
        const uint16_t* const last = q - 1;
        for (; p < last; p++) one(*p, PVR_CMD_VERTEX);
        one(*p, PVR_CMD_VERTEX_EOL);
        p = q;
    }
    return sent;
}

inline void Vert(uint32_t flags, float x, float y, float z, float u, float v, uint32_t argb) {
    pvr_vertex_t* d = (pvr_vertex_t*)pvr_dr_target();
    d->flags = flags;
    d->x = x; d->y = y; d->z = z;
    d->u = u; d->v = v;
    d->argb = argb;
    d->oargb = gAdded;
    pvr_dr_commit(d);
}
}

// ---------------------------------------------------------------------------------------------------- el cuadro
bool Start(int bytesVertices) {
    vid_set_mode(DM_640x480, PM_RGB565);
    pvr_init_params_t p;
    memset(&p, 0, sizeof(p));
    p.opb_sizes[PVR_LIST_OP_POLY] = PVR_BINSIZE_16;
    p.opb_sizes[PVR_LIST_TR_POLY] = PVR_BINSIZE_16;
    p.opb_sizes[PVR_LIST_PT_POLY] = PVR_BINSIZE_16;
    p.vertex_buf_size = bytesVertices;
    p.dma_enabled = 0;
    p.fsaa_enabled = 0;
    p.autosort_disabled = 1;         // la lista translucida en el orden de envio (como un z-buffer comun)
    p.opb_overflow_count = 3;
    if (pvr_init(&p) != 0) return false;
    // el plano de fondo bien atras: lo que va con ScaledMatrix(m, FONDO) tiene que quedar delante de el
    pvr_set_zclip(1e-9f);
    pvr_set_pal_format(PVR_PAL_ARGB8888);
    PVR_SET(PVR_PT_ALPHA_REF, 128);   // el recorte por alfa: lo de mas de la mitad
    memset(gBanks, 0, sizeof(gBanks));
    mat_identity();
    pvr_stats_t st;
    pvr_get_stats(&st);
    gShownBase = st.frame_count;
    gFinished = 0;
    return true;
}

void BackgroundColor(float r, float g, float b) {
    pvr_set_bg_color(r, g, b);
}

void BeginFrame() {
    pvr_wait_ready();
    pvr_scene_begin();
    gList = -1;
    memset(&gCount, 0, sizeof(gCount));
}

void BeginList(List l) {
    if (gList == (int)l) return;
    if (gList >= 0) pvr_list_finish();
    pvr_list_begin(kLists[l]);
    gList = (int)l;
}

void FinishFrame() {
    if (gList >= 0) pvr_list_finish();
    gList = -1;
    pvr_scene_finish();
    gFinished++;
}

bool PrevDrawn() {
    // (KallistiOS cuenta cada cuadro cuando lo muestra: despues de dibujarlo)
    pvr_stats_t st;
    pvr_get_stats(&st);
    return st.frame_count - gShownBase >= gFinished;
}

Count CountFrame() { return gCount; }

// -------------------------------------------------------------------------------------------------- texturas
bool UploadTexture(Texture& t, int width, int height, int format, const void* data, bool repeat, bool filter) {
    memset(&t, 0, sizeof(t));
    const uint8_t* d = (const uint8_t*)data;
    int bytes = width * height * 2;
    if (format == PAL4) {
        int b = 0;
        while (b < 64 && gBanks[b]) b++;
        if (b == 64) return false;
        for (int i = 0; i < 16; i++) {
            const uint8_t* c = d + i * 4;   // RGBA
            pvr_set_pal_entry(b * 16 + i, ((uint32_t)c[3] << 24) | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2]);
        }
        gBanks[b] = true;
        t.bank = (uint8_t)b;
        d += 64;
        bytes = width * height / 2;
    }
    if (format == PAL8) bytes = width * height;   // (solo los indices: la paleta la pone PalettePAL8)
    t.vram = pvr_mem_malloc(bytes);
    if (!t.vram) {
        if (format == PAL4) gBanks[t.bank] = false;
        return false;
    }
    pvr_txr_load(d, t.vram, bytes);
    t.width = (uint16_t)width;
    t.height = (uint16_t)height;
    t.format = (uint8_t)format;
    t.repeat = repeat ? 1 : 0;
    t.filter = filter ? 1 : 0;
    return true;
}

void UpdateTexture(Texture& t, const void* data) {
    if (!t.vram || !data) return;
    const int bytes = t.format == PAL4 ? t.width * t.height / 2 : (t.format == PAL8 ? t.width * t.height
                                                                                     : t.width * t.height * 2);
    pvr_txr_load(data, t.vram, bytes);
}

int ReservePalette8(int colors) {
    const int n = (colors + 15) / 16;
    if (n < 1 || n > 16) return -1;
    // (de atras para adelante: las PAL4 se reparten desde el banco 0)
    for (int b8 = 3; b8 >= 0; b8--) {
        bool free = true;
        for (int k = 0; k < n && free; k++) free = !gBanks[b8 * 16 + k];
        if (!free) continue;
        for (int k = 0; k < n; k++) gBanks[b8 * 16 + k] = true;
        return b8;
    }
    return -1;
}

void ReleasePalette8(int bank, int colors) {
    if (bank < 0 || bank > 3) return;
    const int n = (colors + 15) / 16;
    for (int k = 0; k < n && k < 16; k++) gBanks[bank * 16 + k] = false;
}

void PalettePAL8(int bank, const uint8_t* c, int colors) {
    if (bank < 0 || bank > 3) return;
    for (int i = 0; i < colors && i < 256; i++, c += 4)
        pvr_set_pal_entry(bank * 256 + i, ((uint32_t)c[3] << 24) | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2]);
}

bool CaptureScreen(Texture& t) {
    const int row = 640, rows = 480;
    if (!t.vram) {
        memset(&t, 0, sizeof(t));
        t.vram = pvr_mem_malloc(row * rows * 2);
        if (!t.vram) return false;
        // (declarada de 1024 x 512: la PVR la lee con filas de 640 por el registro de stride)
        t.width = 1024;
        t.height = 512;
        t.format = SCREEN;
        t.filter = 1;
    }
    // la pantalla que se ve (vram_l: la lineal de 32 bits) a la textura (que la PVR lee por la de 64 bits)
    const uint32_t* o = vram_l;
    uint32_t* d = (uint32_t*)t.vram;
    for (int i = 0; i < row * rows / 2; i++) d[i] = o[i];
    pvr_txr_set_stride(row);
    return true;
}

void ReleaseTexture(Texture& t) {
    if (t.vram) pvr_mem_free(t.vram);
    if (t.format == PAL4 && t.vram) gBanks[t.bank] = false;
    memset(&t, 0, sizeof(t));
}

int FreeVRAM() { return (int)pvr_mem_available(); }

int FreeBanks() {
    int n = 0;
    for (int b = 0; b < 64; b++) n += gBanks[b] ? 0 : 1;
    return n;
}

// ------------------------------------------------------------------------------------------------ el encabezado
void Header(const State& e) {
    pvr_poly_cxt_t c;
    const pvr_list_t list = kLists[gList < 0 ? 0 : gList];
    if (e.tex) {
        const Texture& t = *e.tex;
        int fmt = PVR_TXRFMT_TWIDDLED;
        switch (t.format) {
        case SCREEN: fmt = PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE; break;
        case PAL8:     fmt |= PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(t.bank); break;
        case RGB565:   fmt |= PVR_TXRFMT_RGB565; break;
        case ARGB1555: fmt |= PVR_TXRFMT_ARGB1555; break;
        case ARGB4444: fmt |= PVR_TXRFMT_ARGB4444; break;
        default:       fmt |= PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(t.bank); break;
        }
        pvr_poly_cxt_txr(&c, list, fmt, t.width, t.height, t.vram,
                         t.filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        c.txr.uv_clamp = t.repeat ? PVR_UVCLAMP_NONE : PVR_UVCLAMP_UV;
        c.txr.env = PVR_TXRENV_MODULATEALPHA;
    } else {
        pvr_poly_cxt_col(&c, list);
    }
    // (las de adelante, antihorarias como en GL, la PVR las ve antihorarias tambien: atras = horario)
    c.gen.culling = e.faces == 1 ? PVR_CULLING_CW : (e.faces == 2 ? PVR_CULLING_CCW : PVR_CULLING_NONE);
    c.gen.fog_type = e.fog ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
    c.gen.specular = e.sum ? PVR_SPECULAR_ENABLE : PVR_SPECULAR_DISABLE;
    // (ojo: KallistiOS pasa depth.write tal cual al bit de la PVR que APAGA la escritura: PVR_DEPTHWRITE_ENABLE es 0)
    c.depth.write = e.writesZ ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    c.depth.comparison = e.functionZ == ZGreater ? PVR_DEPTHCMP_GREATER
                       : e.functionZ == ZAlways ? PVR_DEPTHCMP_ALWAYS : PVR_DEPTHCMP_GEQUAL;
    switch (e.mix) {
    case Alpha:        c.blend.src = PVR_BLEND_SRCALPHA; c.blend.dst = PVR_BLEND_INVSRCALPHA; break;
    case Sum:        c.blend.src   = PVR_BLEND_SRCALPHA; c.blend.dst = PVR_BLEND_ONE; break;
    case Multiply: c.blend.src     = PVR_BLEND_DESTCOLOR; c.blend.dst = PVR_BLEND_ZERO; break;
    default:          c.blend.src  = PVR_BLEND_ONE; c.blend.dst = PVR_BLEND_ZERO; break;
    }
    pvr_poly_hdr_t h __attribute__((aligned(32)));
    pvr_poly_compile(&h, &c);
    pvr_prim(&h, sizeof(h));
    gCount.headers++;
}

void AddedColor(uint32_t argb) { gAdded = argb; }

void BufferVertices(int& last, int& max) {
    pvr_stats_t st;
    pvr_get_stats(&st);
    last = (int)st.vtx_buffer_used;
    max  = (int)st.vtx_buffer_used_max;
}

// ----------------------------------------------------------------------------------------------- las matrices
void ScreenFrom(const float* p, float* o) {
    // viewport de 640 x 480 con la y para abajo: x' = 320 x + 320 w, y' = -240 y + 240 w (por columnas: V * P)
    for (int c = 0; c < 4; c++) {
        const float x = p[c * 4], y = p[c * 4 + 1], z = p[c * 4 + 2], w = p[c * 4 + 3];
        o[c * 4] = 320.0f * x + 320.0f * w;
        o[c * 4 + 1] = -240.0f * y + 240.0f * w;
        o[c * 4 + 2] = z;
        o[c * 4 + 3] = w;
    }
}

void Matrix(const float* m) {
    memcpy(gMatrix, m, sizeof(gMatrix));   // (matrix_t de KallistiOS: float[4][4] por columnas, como GL)
    mat_load(&gMatrix);
}

void ScaledMatrix(const float* m, float k) {
    float* o = &gMatrix[0][0];
    for (int i = 0; i < 16; i++) o[i] = m[i] * k;
    mat_load(&gMatrix);
}

float WNear() { return gWNear; }
void SetWNear(float w) { gWNear = w; }

// ------------------------------------------------------------------------------------------------- dibujar
namespace {
struct Clip { float x, y, w, u, v; uint32_t c; };

inline void Transform(const float* p, Clip& o) {
    float x = p[0], y = p[1], z = p[2], w = 1.0f;
    mat_trans_nodiv(x, y, z, w);
    o.x = x; o.y = y; o.w = w;
}

inline uint32_t MixColor(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        const float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
        r |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return r;
}

// un triangulo que cruza el plano cercano: lo de adelante (3 o 4 vertices), como una tira chica
void CroppedTriangle(const Clip* t) {
    Clip p[4];
    int n = 0;
    const float wc = gWNear;
    for (int i = 0; i < 3; i++) {
        const Clip& a = t[i];
        const Clip& b = t[(i + 1) % 3];
        const bool ina = a.w >= wc, inb = b.w >= wc;
        if (ina) p[n++] = a;
        if (ina != inb) {
            const float k = (wc - a.w) / (b.w - a.w);
            Clip& q = p[n++];
            q.x = a.x + (b.x - a.x) * k; q.y = a.y + (b.y - a.y) * k; q.w = wc;
            q.u = a.u + (b.u - a.u) * k; q.v = a.v + (b.v - a.v) * k;
            q.c = MixColor(a.c, b.c, k);
        }
    }
    if (n < 3) return;
    // tira: 0 1 2 (y 0 1 3 2 si son 4: el poligono 0 1 2 3 es 0 1 3 / 1 3 2... en tira: 1 2 0 3)
    static const int kStrip3[3] = { 0, 1, 2 }, kStrip4[4] = { 1, 2, 0, 3 };
    const int* o = n == 3 ? kStrip3 : kStrip4;
    for (int i = 0; i < n; i++) {
        const Clip& q = p[o[i]];
        const float iw = Inverse(q.w);
        Vert(i == n - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, q.x * iw, q.y * iw, iw, q.u, q.v, q.c);
    }
    gCount.cropped++;
    gCount.vertices += n;
}
}

void Strips(const float* pos, const float* uv, const uint32_t* argb, uint32_t fixed, const uint16_t* idx, int n,
           bool crop) {
    if (n < 3) return;
    if (!crop && uv) {
        // lo de siempre: cada indice, FTRV, division y 8 palabras a la PVR. El color: el de cada vertice o el fijo (la
        // mascara deja el indice en 0)
        const uint32_t* colors = argb ? argb : &fixed;
        const uint32_t mask = argb ? 0xFFFFu : 0u;
        const uint32_t sum = gAdded;
        uint32_t sq = pvr_dr_addr;
        gCount.vertices += ByStrips(idx, n, [&](uint32_t k, uint32_t flags) {
            float x, y, w;
            Project(pos + k * 3, x, y, w);
            const float iw = Inverse(w);
            sq ^= 32;
            SendSQUV(sq, flags, x * iw, y * iw, iw, uv + k * 2, colors[k & mask], sum);
        });
        pvr_dr_addr = sq;
        return;
    }
    if (!crop) {
        // (sin UV: el camino comun)
        for (int i = 0; i < n; i++) {
            const int k = idx[i];
            if (k == CUT) continue;
            const float* p = pos + k * 3;
            float x = p[0], y = p[1], z = p[2], w = 1.0f;
            mat_trans_nodiv(x, y, z, w);
            const float iw = Inverse(w);
            const bool end = i == n - 1 || idx[i + 1] == CUT;
            Vert(end ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, x * iw, y * iw, iw, 0.0f, 0.0f, argb ? argb[k] : fixed);
            gCount.vertices++;
        }
        return;
    }
    // triangulo por triangulo: los de adelante enteros, los que cruzan el plano cercano recortados, los de atras no
    const float wc = gWNear;
    int start = 0;   // donde empieza la tira (el giro de cada triangulo depende de su lugar en ella)
    for (int i = 0; i + 2 < n; i++) {
        int a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a == CUT) { start = i + 1; continue; }
        if (b == CUT || c == CUT || a == b || b == c || a == c) continue;
        if ((i - start) & 1) { const int s = a; a = b; b = s; }   // (el giro de los impares de una tira)
        Clip t[3];
        const int ks[3] = { a, b, c };
        int inside = 0;
        for (int j = 0; j < 3; j++) {
            Transform(pos + ks[j] * 3, t[j]);
            t[j].u = uv ? uv[ks[j] * 2] : 0.0f;
            t[j].v = uv ? uv[ks[j] * 2 + 1] : 0.0f;
            t[j].c = argb ? argb[ks[j]] : fixed;
            inside += t[j].w >= wc ? 1 : 0;
        }
        if (inside == 0) continue;
        if (inside < 3) { CroppedTriangle(t); continue; }
        for (int j = 0; j < 3; j++) {
            const float iw = Inverse(t[j].w);
            Vert(j == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, t[j].x * iw, t[j].y * iw, iw, t[j].u, t[j].v, t[j].c);
        }
        gCount.vertices += 3;
    }
}

void StripsModel(const VertexModel* v, const uint16_t* ends, int n, bool crop) {
    if (n < 3) return;
    if (!crop) {
        pvr_dr_addr = SendModel(pvr_dr_addr, v, ends, n, gAdded);
        gCount.vertices += n;
        return;
    }
    // triangulo por triangulo (como Tiras): los de una misma tira (ni el primero ni el segundo son el fin de la suya)
    const float wc = gWNear;
    int start = 0;
    for (int i = 0; i + 2 < n; i++) {
        if (i > 0 && (ends[i - 1] & END_STRIP)) start = i;
        if ((ends[i] | ends[i + 1]) & END_STRIP) continue;
        int a = ends[i], b = ends[i + 1];
        const int c = ends[i + 2] & ~END_STRIP;
        if (a == b || b == c || a == c) continue;
        if ((i - start) & 1) { const int s = a; a = b; b = s; }
        Clip t[3];
        const int ks[3] = { a, b, c };
        int inside = 0;
        for (int j = 0; j < 3; j++) {
            const VertexModel& m = v[ks[j]];
            Transform(&m.x, t[j]);
            t[j].u = m.u;
            t[j].v = m.v;
            t[j].c = m.argb;
            inside += t[j].w >= wc ? 1 : 0;
        }
        if (inside == 0) continue;
        if (inside < 3) { CroppedTriangle(t); continue; }
        for (int j = 0; j < 3; j++) {
            const float iw = Inverse(t[j].w);
            Vert(j == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, t[j].x * iw, t[j].y * iw, iw, t[j].u, t[j].v, t[j].c);
        }
        gCount.vertices += 3;
    }
}

int StripsWithEnd(const uint16_t* idx, int n, uint16_t* output, uint32_t* where) {
    int o = 0, i = 0;
    while (i < n) {
        if (idx[i] == CUT) {
            if (where) where[i] = (uint32_t)o;
            i++;
            continue;
        }
        int j = i;
        while (j < n && idx[j] != CUT) j++;
        // (las de menos de 3 no dibujan nada: afuera)
        const bool serves = j - i >= 3;
        const int o0 = o;
        for (int k = i; k < j; k++) {
            if (idx[k] >= END_STRIP) return -1;
            if (where) where[k] = (uint32_t)(serves ? o0 + (k - i) : o0);
            if (serves) output[o++] = (uint16_t)(idx[k] | (k == j - 1 ? END_STRIP : 0));
        }
        i = j;
    }
    if (where) where[n] = (uint32_t)o;
    return o;
}

void TransformedStrips(const Vertex* v, const float* uv, uint32_t argb, const uint16_t* ends, int n,
                        bool forward) {
    if (n < 3) return;
    bool all = forward;
    if (!all) {
        all = true;
        for (int i = 0; i < n && all; i++) all = v[ends[i] & ~END_STRIP].w > 0.0f;
    }
    if (all) {
        const uint32_t sum = gAdded;
        uint32_t sq = pvr_dr_addr;
        for (int i = 0; i < n; i++) {
            const uint32_t k = ends[i] & ~END_STRIP;
            sq ^= 32;
            SendSQOf(sq, (ends[i] & END_STRIP) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, &v[k].x, uv + k * 2, argb,
                       sum);
        }
        pvr_dr_addr = sq;
        gCount.vertices += n;
        return;
    }
    // alguno detras del plano cercano: triangulo por triangulo, sin los que lo tocan (los de una misma tira: ni el
    // primero ni el segundo son el fin de la suya)
    int start = 0;
    for (int i = 0; i + 2 < n; i++) {
        if (i > 0 && (ends[i - 1] & END_STRIP)) start = i;
        if ((ends[i] | ends[i + 1]) & END_STRIP) continue;
        int a = ends[i], b = ends[i + 1];
        const int c = ends[i + 2] & ~END_STRIP;
        if (a == b || b == c || a == c) continue;
        if (v[a].w <= 0.0f || v[b].w <= 0.0f || v[c].w <= 0.0f) continue;
        if ((i - start) & 1) { const int s = a; a = b; b = s; }
        const int ks[3] = { a, b, c };
        for (int j = 0; j < 3; j++) {
            const int k = ks[j];
            Vert(j == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, v[k].x, v[k].y, v[k].z, uv[k * 2], uv[k * 2 + 1], argb);
        }
        gCount.vertices += 3;
    }
}

void StripsBlocks(const Block* b, uint32_t argb, const uint16_t* ends, int n) {
    if (n < 3) return;
    pvr_dr_addr = SendBlocks(pvr_dr_addr, b, ends, n, argb, gAdded);
    gCount.vertices += n;
}

void Frame2DCorners(const float* xy, const float* uv, uint32_t argb, float z) {
    for (int i = 0; i < 4; i++)
        Vert(i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, xy[i * 2], xy[i * 2 + 1], z, uv[i * 2], uv[i * 2 + 1], argb);
    gCount.vertices += 4;
}

void Frame2D(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t argb, float z) {
    const float xy[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    const float uv[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    Frame2DCorners(xy, uv, argb, z);
}

// ------------------------------------------------------------------------------------------------- la niebla
void ConstantFog(float alpha, float r, float g, float b) {
    static float table[129];
    for (int i = 0; i < 129; i++) table[i] = alpha;
    pvr_fog_table_color(1.0f, r, g, b);
    pvr_fog_table_custom(table);
}

}

#endif // W3D_DREAMCAST
