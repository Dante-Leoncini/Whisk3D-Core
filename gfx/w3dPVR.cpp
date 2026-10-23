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
const pvr_list_t kListas[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };
int gLista = -1;                 // la lista abierta
float gWCercano = 0.05f;
bool gBancos[64];                // los bancos de 16 colores de la paleta
Cuenta gCuenta;
matrix_t gMatriz __attribute__((aligned(32)));   // la cargada en XMTRX (para recortar hace falta otra vez)
uint32_t gSumado = 0;            // el color sumado de los vertices (ColorSumado)
size_t gTerminados = 0;          // los cuadros mandados (TerminarCuadro) desde Iniciar
size_t gBaseMostrados = 0;       // los que KallistiOS ya habia mostrado al Iniciar

// EL ENVIO RAPIDO (las tiras enteras, que son casi todo): el puntero de la store queue en un registro todo el bucle
// (pvr_dr_target lo lee y lo escribe en memoria por cada vertice) y las 8 palabras guardadas de atras para adelante
// con predecremento: el SH-4 no tiene fmov con desplazamiento y cada una costaba tres instrucciones. Despues, el pref
// que la manda a la PVR. 'sq' es la store queue de este vertice (alternan: sq ^= 32 antes de cada uno). (Sin
// "memory": solo escribe en la store queue, que nada lee; asi lo de alrededor sigue en registros)
inline void EnviarSQ(uint32_t sq, uint32_t flags, float x, float y, float z, uint32_t u, uint32_t v, uint32_t argb,
                     uint32_t oargb) {
    uint32_t p = sq + 32;
    __asm__ __volatile__(
        "mov.l   %1, @-%0\n\t"
        "mov.l   %2, @-%0\n\t"
        "mov.l   %3, @-%0\n\t"
        "mov.l   %4, @-%0\n\t"
        "fmov.s  %5, @-%0\n\t"
        "fmov.s  %6, @-%0\n\t"
        "fmov.s  %7, @-%0\n\t"
        "mov.l   %8, @-%0\n\t"
        "pref    @%0\n\t"
        : "+r"(p)
        : "r"(oargb), "r"(argb), "r"(v), "r"(u), "f"(z), "f"(y), "f"(x), "r"(flags));
}

// lo mismo con todo ya hecho (los vertices transformados: se copian como enteros)
inline void EnviarSQCopia(uint32_t sq, uint32_t flags, const uint32_t* xyz, const uint32_t* uv, uint32_t argb,
                          uint32_t oargb) {
    uint32_t p = sq + 32;
    __asm__ __volatile__(
        "mov.l   %1, @-%0\n\t"
        "mov.l   %2, @-%0\n\t"
        "mov.l   %3, @-%0\n\t"
        "mov.l   %4, @-%0\n\t"
        "mov.l   %5, @-%0\n\t"
        "mov.l   %6, @-%0\n\t"
        "mov.l   %7, @-%0\n\t"
        "mov.l   %8, @-%0\n\t"
        "pref    @%0\n\t"
        : "+r"(p)
        : "r"(oargb), "r"(argb), "r"(uv[1]), "r"(uv[0]), "r"(xyz[2]), "r"(xyz[1]), "r"(xyz[0]), "r"(flags));
}

inline void Vert(uint32_t flags, float x, float y, float z, float u, float v, uint32_t argb) {
    pvr_vertex_t* d = (pvr_vertex_t*)pvr_dr_target();
    d->flags = flags;
    d->x = x; d->y = y; d->z = z;
    d->u = u; d->v = v;
    d->argb = argb;
    d->oargb = gSumado;
    pvr_dr_commit(d);
}
}

// ---------------------------------------------------------------------------------------------------- el cuadro
bool Iniciar(int bytesVertices) {
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
    // el plano de fondo bien atras: lo que va con MatrizEscalada(m, FONDO) tiene que quedar delante de el
    pvr_set_zclip(1e-9f);
    pvr_set_pal_format(PVR_PAL_ARGB8888);
    PVR_SET(PVR_PT_ALPHA_REF, 128);   // el recorte por alfa: lo de mas de la mitad
    memset(gBancos, 0, sizeof(gBancos));
    mat_identity();
    pvr_stats_t st;
    pvr_get_stats(&st);
    gBaseMostrados = st.frame_count;
    gTerminados = 0;
    return true;
}

void ColorDeFondo(float r, float g, float b) {
    pvr_set_bg_color(r, g, b);
}

void EmpezarCuadro() {
    pvr_wait_ready();
    pvr_scene_begin();
    gLista = -1;
    memset(&gCuenta, 0, sizeof(gCuenta));
}

void EmpezarLista(Lista l) {
    if (gLista == (int)l) return;
    if (gLista >= 0) pvr_list_finish();
    pvr_list_begin(kListas[l]);
    gLista = (int)l;
}

void TerminarCuadro() {
    if (gLista >= 0) pvr_list_finish();
    gLista = -1;
    pvr_scene_finish();
    gTerminados++;
}

bool AnterioresDibujados() {
    // (KallistiOS cuenta cada cuadro cuando lo muestra: despues de dibujarlo)
    pvr_stats_t st;
    pvr_get_stats(&st);
    return st.frame_count - gBaseMostrados >= gTerminados;
}

Cuenta CuentaCuadro() { return gCuenta; }

// -------------------------------------------------------------------------------------------------- texturas
bool SubirTextura(Textura& t, int ancho, int alto, int formato, const void* datos, bool repetir, bool filtro) {
    memset(&t, 0, sizeof(t));
    const uint8_t* d = (const uint8_t*)datos;
    int bytes = ancho * alto * 2;
    if (formato == PAL4) {
        int b = 0;
        while (b < 64 && gBancos[b]) b++;
        if (b == 64) return false;
        for (int i = 0; i < 16; i++) {
            const uint8_t* c = d + i * 4;   // RGBA
            pvr_set_pal_entry(b * 16 + i, ((uint32_t)c[3] << 24) | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2]);
        }
        gBancos[b] = true;
        t.banco = (uint8_t)b;
        d += 64;
        bytes = ancho * alto / 2;
    }
    if (formato == PAL8) bytes = ancho * alto;   // (solo los indices: la paleta la pone PaletaPAL8)
    t.vram = pvr_mem_malloc(bytes);
    if (!t.vram) {
        if (formato == PAL4) gBancos[t.banco] = false;
        return false;
    }
    pvr_txr_load(d, t.vram, bytes);
    t.ancho = (uint16_t)ancho;
    t.alto = (uint16_t)alto;
    t.formato = (uint8_t)formato;
    t.repetir = repetir ? 1 : 0;
    t.filtro = filtro ? 1 : 0;
    return true;
}

void ActualizarTextura(Textura& t, const void* datos) {
    if (!t.vram || !datos) return;
    const int bytes = t.formato == PAL4 ? t.ancho * t.alto / 2 : (t.formato == PAL8 ? t.ancho * t.alto
                                                                                     : t.ancho * t.alto * 2);
    pvr_txr_load(datos, t.vram, bytes);
}

int ReservarPaleta8(int colores) {
    const int n = (colores + 15) / 16;
    if (n < 1 || n > 16) return -1;
    // (de atras para adelante: las PAL4 se reparten desde el banco 0)
    for (int b8 = 3; b8 >= 0; b8--) {
        bool libres = true;
        for (int k = 0; k < n && libres; k++) libres = !gBancos[b8 * 16 + k];
        if (!libres) continue;
        for (int k = 0; k < n; k++) gBancos[b8 * 16 + k] = true;
        return b8;
    }
    return -1;
}

void SoltarPaleta8(int banco, int colores) {
    if (banco < 0 || banco > 3) return;
    const int n = (colores + 15) / 16;
    for (int k = 0; k < n && k < 16; k++) gBancos[banco * 16 + k] = false;
}

void PaletaPAL8(int banco, const uint8_t* c, int colores) {
    if (banco < 0 || banco > 3) return;
    for (int i = 0; i < colores && i < 256; i++, c += 4)
        pvr_set_pal_entry(banco * 256 + i, ((uint32_t)c[3] << 24) | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2]);
}

bool CapturarPantalla(Textura& t) {
    const int fila = 640, filas = 480;
    if (!t.vram) {
        memset(&t, 0, sizeof(t));
        t.vram = pvr_mem_malloc(fila * filas * 2);
        if (!t.vram) return false;
        // (declarada de 1024 x 512: la PVR la lee con filas de 640 por el registro de stride)
        t.ancho = 1024;
        t.alto = 512;
        t.formato = PANTALLA;
        t.filtro = 1;
    }
    // la pantalla que se ve (vram_l: la lineal de 32 bits) a la textura (que la PVR lee por la de 64 bits)
    const uint32_t* o = vram_l;
    uint32_t* d = (uint32_t*)t.vram;
    for (int i = 0; i < fila * filas / 2; i++) d[i] = o[i];
    pvr_txr_set_stride(fila);
    return true;
}

void SoltarTextura(Textura& t) {
    if (t.vram) pvr_mem_free(t.vram);
    if (t.formato == PAL4 && t.vram) gBancos[t.banco] = false;
    memset(&t, 0, sizeof(t));
}

int VRAMLibre() { return (int)pvr_mem_available(); }

int BancosLibres() {
    int n = 0;
    for (int b = 0; b < 64; b++) n += gBancos[b] ? 0 : 1;
    return n;
}

// ------------------------------------------------------------------------------------------------ el encabezado
void Encabezado(const Estado& e) {
    pvr_poly_cxt_t c;
    const pvr_list_t lista = kListas[gLista < 0 ? 0 : gLista];
    if (e.tex) {
        const Textura& t = *e.tex;
        int fmt = PVR_TXRFMT_TWIDDLED;
        switch (t.formato) {
        case PANTALLA: fmt = PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE; break;
        case PAL8:     fmt |= PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(t.banco); break;
        case RGB565:   fmt |= PVR_TXRFMT_RGB565; break;
        case ARGB1555: fmt |= PVR_TXRFMT_ARGB1555; break;
        case ARGB4444: fmt |= PVR_TXRFMT_ARGB4444; break;
        default:       fmt |= PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(t.banco); break;
        }
        pvr_poly_cxt_txr(&c, lista, fmt, t.ancho, t.alto, t.vram,
                         t.filtro ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        c.txr.uv_clamp = t.repetir ? PVR_UVCLAMP_NONE : PVR_UVCLAMP_UV;
        c.txr.env = PVR_TXRENV_MODULATEALPHA;
    } else {
        pvr_poly_cxt_col(&c, lista);
    }
    // (las de adelante, antihorarias como en GL, la PVR las ve antihorarias tambien: atras = horario)
    c.gen.culling = e.caras == 1 ? PVR_CULLING_CW : (e.caras == 2 ? PVR_CULLING_CCW : PVR_CULLING_NONE);
    c.gen.fog_type = e.niebla ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
    c.gen.specular = e.suma ? PVR_SPECULAR_ENABLE : PVR_SPECULAR_DISABLE;
    // (ojo: KallistiOS pasa depth.write tal cual al bit de la PVR que APAGA la escritura: PVR_DEPTHWRITE_ENABLE es 0)
    c.depth.write = e.escribeZ ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    c.depth.comparison = e.funcionZ == ZMayor ? PVR_DEPTHCMP_GREATER
                       : e.funcionZ == ZSiempre ? PVR_DEPTHCMP_ALWAYS : PVR_DEPTHCMP_GEQUAL;
    switch (e.mezcla) {
    case Alfa:        c.blend.src = PVR_BLEND_SRCALPHA; c.blend.dst = PVR_BLEND_INVSRCALPHA; break;
    case Suma:        c.blend.src = PVR_BLEND_SRCALPHA; c.blend.dst = PVR_BLEND_ONE; break;
    case Multiplicar: c.blend.src = PVR_BLEND_DESTCOLOR; c.blend.dst = PVR_BLEND_ZERO; break;
    default:          c.blend.src = PVR_BLEND_ONE; c.blend.dst = PVR_BLEND_ZERO; break;
    }
    pvr_poly_hdr_t h __attribute__((aligned(32)));
    pvr_poly_compile(&h, &c);
    pvr_prim(&h, sizeof(h));
    gCuenta.encabezados++;
}

void ColorSumado(uint32_t argb) { gSumado = argb; }

void BufferVertices(int& ultimo, int& maximo) {
    pvr_stats_t st;
    pvr_get_stats(&st);
    ultimo = (int)st.vtx_buffer_used;
    maximo = (int)st.vtx_buffer_used_max;
}

// ----------------------------------------------------------------------------------------------- las matrices
void PantallaDesde(const float* p, float* o) {
    // viewport de 640 x 480 con la y para abajo: x' = 320 x + 320 w, y' = -240 y + 240 w (por columnas: V * P)
    for (int c = 0; c < 4; c++) {
        const float x = p[c * 4], y = p[c * 4 + 1], z = p[c * 4 + 2], w = p[c * 4 + 3];
        o[c * 4] = 320.0f * x + 320.0f * w;
        o[c * 4 + 1] = -240.0f * y + 240.0f * w;
        o[c * 4 + 2] = z;
        o[c * 4 + 3] = w;
    }
}

void Matriz(const float* m) {
    memcpy(gMatriz, m, sizeof(gMatriz));   // (matrix_t de KallistiOS: float[4][4] por columnas, como GL)
    mat_load(&gMatriz);
}

void MatrizEscalada(const float* m, float k) {
    float* o = &gMatriz[0][0];
    for (int i = 0; i < 16; i++) o[i] = m[i] * k;
    mat_load(&gMatriz);
}

float WCercano() { return gWCercano; }
void SetWCercano(float w) { gWCercano = w; }

// ------------------------------------------------------------------------------------------------- dibujar
namespace {
struct Clip { float x, y, w, u, v; uint32_t c; };

inline void Transformar(const float* p, Clip& o) {
    float x = p[0], y = p[1], z = p[2], w = 1.0f;
    mat_trans_nodiv(x, y, z, w);
    o.x = x; o.y = y; o.w = w;
}

inline uint32_t MezclarColor(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        const float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
        r |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return r;
}

// un triangulo que cruza el plano cercano: lo de adelante (3 o 4 vertices), como una tira chica
void TrianguloRecortado(const Clip* t) {
    Clip p[4];
    int n = 0;
    const float wc = gWCercano;
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
            q.c = MezclarColor(a.c, b.c, k);
        }
    }
    if (n < 3) return;
    // tira: 0 1 2 (y 0 1 3 2 si son 4: el poligono 0 1 2 3 es 0 1 3 / 1 3 2... en tira: 1 2 0 3)
    static const int kTira3[3] = { 0, 1, 2 }, kTira4[4] = { 1, 2, 0, 3 };
    const int* o = n == 3 ? kTira3 : kTira4;
    for (int i = 0; i < n; i++) {
        const Clip& q = p[o[i]];
        const float iw = Inversa(q.w);
        Vert(i == n - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, q.x * iw, q.y * iw, iw, q.u, q.v, q.c);
    }
    gCuenta.recortados++;
    gCuenta.vertices += n;
}
}

void Tiras(const float* pos, const float* uv, const uint32_t* argb, uint32_t fijo, const uint16_t* idx, int n,
           bool recortar) {
    if (n < 3) return;
    if (!recortar && uv) {
        // lo de siempre: cada indice, FTRV, division y 8 palabras a la PVR (el ultimo de cada tira, con su fin). El
        // color: el de cada vertice o el fijo (la mascara deja el indice en 0)
        const uint32_t* colores = argb ? argb : &fijo;
        const uint32_t mascara = argb ? 0xFFFFu : 0u;
        const uint32_t* uvs = (const uint32_t*)uv;
        const uint32_t suma = gSumado;
        uint32_t sq = pvr_dr_addr;
        int enviados = 0;
        for (int i = 0; i < n; i++) {
            const uint32_t k = idx[i];
            if (k == CORTE) continue;
            const float* p = pos + k * 3;
            float x = p[0], y = p[1], z = p[2], w = 1.0f;
            mat_trans_nodiv(x, y, z, w);
            const float iw = Inversa(w);
            const uint32_t flags = (i == n - 1 || idx[i + 1] == CORTE) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            sq ^= 32;
            EnviarSQ(sq, flags, x * iw, y * iw, iw, uvs[k * 2], uvs[k * 2 + 1], colores[k & mascara], suma);
            enviados++;
        }
        pvr_dr_addr = sq;
        gCuenta.vertices += enviados;
        return;
    }
    if (!recortar) {
        // (sin UV: el camino comun)
        for (int i = 0; i < n; i++) {
            const int k = idx[i];
            if (k == CORTE) continue;
            const float* p = pos + k * 3;
            float x = p[0], y = p[1], z = p[2], w = 1.0f;
            mat_trans_nodiv(x, y, z, w);
            const float iw = Inversa(w);
            const bool fin = i == n - 1 || idx[i + 1] == CORTE;
            Vert(fin ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, x * iw, y * iw, iw, 0.0f, 0.0f, argb ? argb[k] : fijo);
            gCuenta.vertices++;
        }
        return;
    }
    // triangulo por triangulo: los de adelante enteros, los que cruzan el plano cercano recortados, los de atras no
    const float wc = gWCercano;
    int inicio = 0;   // donde empieza la tira (el giro de cada triangulo depende de su lugar en ella)
    for (int i = 0; i + 2 < n; i++) {
        int a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a == CORTE) { inicio = i + 1; continue; }
        if (b == CORTE || c == CORTE || a == b || b == c || a == c) continue;
        if ((i - inicio) & 1) { const int s = a; a = b; b = s; }   // (el giro de los impares de una tira)
        Clip t[3];
        const int ks[3] = { a, b, c };
        int adentro = 0;
        for (int j = 0; j < 3; j++) {
            Transformar(pos + ks[j] * 3, t[j]);
            t[j].u = uv ? uv[ks[j] * 2] : 0.0f;
            t[j].v = uv ? uv[ks[j] * 2 + 1] : 0.0f;
            t[j].c = argb ? argb[ks[j]] : fijo;
            adentro += t[j].w >= wc ? 1 : 0;
        }
        if (adentro == 0) continue;
        if (adentro < 3) { TrianguloRecortado(t); continue; }
        for (int j = 0; j < 3; j++) {
            const float iw = Inversa(t[j].w);
            Vert(j == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, t[j].x * iw, t[j].y * iw, iw, t[j].u, t[j].v, t[j].c);
        }
        gCuenta.vertices += 3;
    }
}

void TirasTransformadas(const Vertice* v, const float* uv, uint32_t argb, const uint16_t* idx, int n, bool adelante) {
    if (n < 3) return;
    bool todos = adelante;
    if (!todos) {
        todos = true;
        for (int i = 0; i < n && todos; i++) todos = idx[i] == CORTE || v[idx[i]].w > 0.0f;
    }
    if (todos) {
        const uint32_t* uvs = (const uint32_t*)uv;
        const uint32_t suma = gSumado;
        uint32_t sq = pvr_dr_addr;
        int enviados = 0;
        for (int i = 0; i < n; i++) {
            const uint32_t k = idx[i];
            if (k == CORTE) continue;
            const uint32_t flags = (i == n - 1 || idx[i + 1] == CORTE) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            sq ^= 32;
            EnviarSQCopia(sq, flags, (const uint32_t*)&v[k], uvs + k * 2, argb, suma);
            enviados++;
        }
        pvr_dr_addr = sq;
        gCuenta.vertices += enviados;
        return;
    }
    // alguno detras del plano cercano: triangulo por triangulo, sin los que lo tocan
    int inicio = 0;
    for (int i = 0; i + 2 < n; i++) {
        int a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a == CORTE) { inicio = i + 1; continue; }
        if (b == CORTE || c == CORTE || a == b || b == c || a == c) continue;
        if (v[a].w <= 0.0f || v[b].w <= 0.0f || v[c].w <= 0.0f) continue;
        if ((i - inicio) & 1) { const int s = a; a = b; b = s; }
        const int ks[3] = { a, b, c };
        for (int j = 0; j < 3; j++) {
            const int k = ks[j];
            Vert(j == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, v[k].x, v[k].y, v[k].z, uv[k * 2], uv[k * 2 + 1], argb);
        }
        gCuenta.vertices += 3;
    }
}

void Cuadro2DEsquinas(const float* xy, const float* uv, uint32_t argb, float z) {
    for (int i = 0; i < 4; i++)
        Vert(i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, xy[i * 2], xy[i * 2 + 1], z, uv[i * 2], uv[i * 2 + 1], argb);
    gCuenta.vertices += 4;
}

void Cuadro2D(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t argb, float z) {
    const float xy[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    const float uv[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    Cuadro2DEsquinas(xy, uv, argb, z);
}

// ------------------------------------------------------------------------------------------------- la niebla
void NieblaConstante(float alfa, float r, float g, float b) {
    static float tabla[129];
    for (int i = 0; i < 129; i++) tabla[i] = alfa;
    pvr_fog_table_color(1.0f, r, g, b);
    pvr_fog_table_custom(tabla);
}

}

#endif // W3D_DREAMCAST
