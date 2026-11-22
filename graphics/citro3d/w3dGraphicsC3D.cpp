// ============================================================================
//  w3dGraphicsC3D.cpp -- backend citro3d de w3dEngine: la GPU de la Nintendo 3DS (PICA200).
//
//  La PICA no tiene pipeline fijo de transformacion: este backend EMULA el que usa el Core (la MISMA API que
//  w3dGraphics.cpp) como el de GLES2, pero con lo que la PICA trae por hardware:
//    - la transformacion corre en la GPU, en un vertex shader (w3d.v.pica), con la pila de matrices por software
//      (Matrix4) que sube como uniforms solo cuando cambia
//    - el combiner (modulate / replace / DOT3 / solo alfa y la mezcla de dos texturas) con los TexEnv de la PICA
//    - la luz con la iluminacion POR FRAGMENTO de la PICA (hasta 8 luces, especular con tabla)
//    - scissor, estencil, alpha test, mezclas y z por hardware
//    - los arreglos de vertices de la RAM comun se copian en cada draw a un anillo de memoria LINEAL (la que lee la
//      GPU), en su formato: la PICA lee float, short, byte y ubyte (el shader escala lo que GL normalizaria)
//  Lo que la PICA no tiene: lineas y puntos (se arman como triangulos en la CPU), planos de recorte y wireframe.
//
//  Las pantallas de la 3DS estan giradas un cuarto de vuelta y la PICA usa z de -1 a 0: cada proyeccion se
//  multiplica por kAjuste (ver abajo) y los rectangulos del viewport y el scissor se giran al ponerlos.
// ============================================================================
#include "w3dGraphics.h"
#include "w3dlog.h"
#include "w3dC3D.h"
#include "w3dC3DInternal.h"
#include "math/Matrix4.h"
#include <vector>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "w3d_shbin.h"   // el shader ensamblado (picasso + bin2s, ver platform/3ds/Makefile)

// estado de render compartido (lo setea el editor; el Core lo LEE). Los MISMOS globales que los otros backends
bool w3dRenderWireframe = false;
bool w3dRenderWireframeOnlyVisible = true;
bool w3dRenderSolid      = false;
bool w3dRenderNoLight    = false;
bool w3dRenderLights     = true;   // (el render: las luces de la escena. El editor pone su modo de vista)
bool w3dRenderNormalColor = false;
bool w3dRenderAlpha       = false;
bool w3dRenderOverlays    = true;
bool w3dShowSelection     = true;
bool g_xray               = false;

void W3dC3DBindTexture(unsigned int id);   // (w3dGLc3d.cpp: la textura de las funciones de GL)

namespace w3dEngine {

int g_statCalls[StatCatCount_] = { 0 };
static int gStatCat = StatCatEditor;
bool g_traceGL = false;
bool g_traceGLSec  = false;
float g_alphaColor = 1.0f;
void TraceGL(const char*) {}
void TraceGLVer() {}

// ============================================================================
//  Estado emulado del pipeline fijo
// ============================================================================
static bool cap_depth = false, cap_cull = false, cap_tex = false, cap_light = false, cap_blend = false, cap_fog = false;
static bool cap_scissor = false, cap_colormat = false, cap_offset = false;
static int  matMode = 1;   // 0 = Projection, 1 = ModelView, 2 = Texture
static Matrix4 curProj, curMV, curTex;
static std::vector<Matrix4> stkProj, stkMV, stkTex;
struct Light { bool on; float pos[4], amb[4], dif[4], spec[4], att[3], dir[3], cutCos, expo; };
static Light lights[LightsMax];
static float ambGlobal[4] = { 0.2f, 0.2f, 0.2f, 1 };
static bool  lightsDirty = true;
static float matDiff[4] = { 0.8f, 0.8f, 0.8f, 1 }, matAmb[4] = { 0.2f, 0.2f, 0.2f, 1 };
static float matSpec[4] = { 0, 0, 0, 1 }, matEmis[4] = { 0, 0, 0, 1 };
static float matShine = 0.0f;
static bool  dirtyMaterial = true;
static float curColor[4] = { 1, 1, 1, 1 };
static float fogColor[4] = { 0, 0, 0, 1 };
static float fogStart = 0, fogEnd = 1, fogDens = 1.0f;
static int   fogMode = 0;
static int   texEnv = 0;            // 0 modulate, 1 replace, 2 DOT3, 3 solo alfa
static int   texGenCache = -1, matcapCache = -1;
static float uvMat[4] = { 0, 0, 1, 1 };
static bool  pointSprite = false;
static float pointSize = 1.0f, lineWidth = 1.0f;
static int   vpCurrent[4] = { 0, 0, 320, 240 }, scCurrent[4] = { 0, 0, 320, 240 };
static unsigned boundTex  = 0;
static bool     mixOn     = false;
static unsigned mixTex = 0;
static int      texMixMode = 0;
static float    mixK = 1.0f;
static const float* mixUV = 0;
static int   depthFunc = (int)DepthLess;
static bool  depthMask = true, colorMask = true;
static bool  frontCCW  = true, cullFront = false;
static float alphaRef = 0.0f;
static float polyF = 0.0f, polyU = 0.0f;
static float clearCol[4]  = { 0, 0, 0, 1 };
static int   mixSignature = -1;      // la funcion de mezcla puesta (SetMix / BlendAlpha / BlendMode)
static int   stencil = 0, stencilValue = 0;   // 0 apagado, 1 marcar, 2 dentro
// los arreglos de cliente
enum { FloatType = 0, ShortType, ByteType, TypeUByte };
struct Arr { bool on; int size; int type; int step; const void* ptr; };
static Arr aPos = { false, 3, FloatType, 0, 0 }, aNrm = { false, 3, ByteType, 0, 0 };
static Arr aUV = { false, 2, FloatType, 0, 0 }, aCol = { false, 4, TypeUByte, 0, 0 };

// ============================================================================
//  citro3d: pantallas, shader, anillo de memoria lineal
// ============================================================================
static C3D_RenderTarget* gTarget[2] = { NULL, NULL };
static bool gClean[2] = { false, false };   // ya se limpio en este cuadro
static int  gScreen  = ScreenDown;
static bool gInFrame = false;
static DVLB_s* gDvlb = NULL;
static shaderProgram_s gProg;
static int uMvp = -1, uMv = -1, uNmat = -1, uTexm = -1, uScale = -1;
static C3D_LightEnv gLightEnv;
static C3D_Light    gLight[LightsMax];
static C3D_LightLut gLutEsp;
static float        gLutEspBrightness = -1.0f;
static bool         gLightPlacedEnv   = false;
struct Ring { u8* base; u32 size, used; };
static Ring gRing[2];
static int    gRingAct = 0;
static bool   gNotifiedRing = false;
static const u32 kRingBytes = 3u * 1024u * 1024u;

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static void* RingRequest(u32 bytes) {
    Ring& a = gRing[gRingAct];
    bytes = (bytes + 15u) & ~15u;
    if (!a.base || a.used + bytes > a.size) {
        if (!gNotifiedRing) { gNotifiedRing = true; printf("[c3d] el anillo de vertices se lleno (%u KB por cuadro)\n", (unsigned)(a.size / 1024)); }
        return NULL;
    }
    void* p = a.base + a.used;
    a.used += bytes;
    return p;
}

int  C3DScreenWidth(int p) { return p == ScreenUp ? 400 : 320; }
bool C3DInFrame() { return gInFrame; }

bool C3DInit() {
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    w3dLogf("[c3d] %s", n3ds ? "New 3DS" : "3DS (la vieja)");
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) return false;
    gTarget[ScreenUp]   = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    gTarget[ScreenDown] = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!gTarget[0] || !gTarget[1]) return false;
    C3D_RenderTargetSetOutput(gTarget[ScreenUp], GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(gTarget[ScreenDown], GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    gDvlb = DVLB_ParseFile((u32*)w3d_shbin, w3d_shbin_size);
    shaderProgramInit(&gProg);
    shaderProgramSetVsh(&gProg, &gDvlb->DVLE[0]);
    C3D_BindProgram(&gProg);
    uMvp   = shaderInstanceGetUniformLocation(gProg.vertexShader, "mvp");
    uMv    = shaderInstanceGetUniformLocation(gProg.vertexShader, "mv");
    uNmat  = shaderInstanceGetUniformLocation(gProg.vertexShader, "nmat");
    uTexm  = shaderInstanceGetUniformLocation(gProg.vertexShader, "texm");
    uScale = shaderInstanceGetUniformLocation(gProg.vertexShader, "scale");
    for (int i = 0; i < 2; i++) {
        gRing[i].base = (u8*)linearAlloc(kRingBytes);
        gRing[i].size = gRing[i].base ? kRingBytes : 0;
        gRing[i].used = 0;
    }
    for (int i = 0; i < 6; i++) { C3D_TexEnv* e = C3D_GetTexEnv(i); C3D_TexEnvInit(e); }
    C3D_LightEnvInit(&gLightEnv);
    for (int i = 0; i < LightsMax; i++) C3D_LightInit(&gLight[i], &gLightEnv);
    curProj.Identity(); curMV.Identity(); curTex.Identity();
    for (int i = 0; i < LightsMax; i++) {
        Light& L = lights[i];
        L.on = false; L.pos[0] = 0; L.pos[1] = 0; L.pos[2] = 1; L.pos[3] = 0;
        for (int k = 0; k < 4; k++) { L.amb[k] = (k == 3) ? 1.0f : 0.0f; L.dif[k] = (i == 0 || k == 3) ? 1.0f : 0.0f; L.spec[k] = L.dif[k]; }
        L.att[0] = 1; L.att[1] = 0; L.att[2] = 0; L.dir[0] = 0; L.dir[1] = 0; L.dir[2] = -1; L.cutCos = -1.0f; L.expo = 0.0f;
    }
    return true;
}
// al salir, ANTES de que SDL apague las pantallas (gfxExit): C3D_Fini espera a que la GPU termine lo pendiente, saca el
// hook de APT de citro3d y su interrupcion del vblank. Recien despues se libera lo que la GPU podia estar leyendo (el
// anillo de vertices de los dos cuadros, el shader)
void C3DEnd() {
    gInFrame = false;
    C3D_Fini();
    for (int i = 0; i < 2; i++) if (gRing[i].base) { linearFree(gRing[i].base); gRing[i].base = NULL; gRing[i].size = 0; }
    if (gDvlb) { shaderProgramFree(&gProg); DVLB_Free(gDvlb); gDvlb = NULL; }
}

// ============================================================================
//  EL VIGILANTE DE LA GPU: en la consola de verdad, un cuadro que la GPU no puede terminar la deja colgada (y a la
//  consola con ella); el emulador lo dibuja igual. Se anotan los draws de cada cuadro (que, cuantos vertices, con que
//  estado y, lo armado en la CPU, hasta donde llega en la pantalla). Si la GPU no termina un cuadro en 2 segundos, la
//  lista del cuadro colgado va al log (que en la 3DS queda en la SD) y se sigue esperando
// ============================================================================
struct RegistryDraw { char type; int n, mix, zFunc; unsigned combiner; float maxNdc, minW; };
enum { kRegMax = 128 };
static RegistryDraw gReg[kRegMax];
static int gRegN = 0, gRegTotal = 0;
static void ReportHungGPU() {
    w3dLogfE("[c3d] la GPU no termino el cuadro en 2 segundos (en la consola de verdad, colgada). Sus %d draws:", gRegTotal);
    for (int i = 0; i < gRegN; i++) {
        const RegistryDraw& r = gReg[i];
        if (r.maxNdc >= 0.0f)
            w3dLogfE("[c3d]   %c %d vertices, mezcla %d, z %d, combiner %x, |x/w| hasta %.1f, w desde %.4f",
                     r.type, r.n, r.mix, r.zFunc, r.combiner, r.maxNdc, r.minW);
        else w3dLogfE("[c3d]   %c %d vertices, mezcla %d, z %d, combiner %x", r.type, r.n, r.mix, r.zFunc, r.combiner);
    }
    if (gRegTotal > gRegN) w3dLogfE("[c3d]   (y %d draws mas)", gRegTotal - gRegN);
}
// C3D_FrameBegin, que espera a que la GPU termine lo anterior: si tarda mas de 2 segundos, avisa (una vez)
static void BeginWithWait(u8 flags) {
    if (C3D_FrameBegin(flags | C3D_FRAME_NONBLOCK)) return;
    const u64 t0  = svcGetSystemTick();
    bool notified = false;
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        svcSleepThread(1000000LL);   // 1 ms
        if (!notified && svcGetSystemTick() - t0 > 2000ULL * (u64)CPU_TICKS_PER_MSEC) { notified = true; ReportHungGPU(); }
    }
}

static void SetViewport();
static void SetScissor();
static void BatchEmpty();
// (lo ultimo que se puso en citro3d del viewport y el scissor: si no cambia no se toca, y el lote sigue. C3DScreen
// los olvida)
static int gVpPlaced[4] = { -1, -1, -1, -1 }, gPlacedSc[5] = { -1, -1, -1, -1, -1 };
void C3DFrameBegin() {
    if (gInFrame) return;
    BeginWithWait(C3D_FRAME_SYNCDRAW);
    gRegN = 0; gRegTotal = 0;
    gInFrame = true;
    gClean[0] = gClean[1] = false;
    gRingAct ^= 1;
    gRing[gRingAct].used = 0;
    gNotifiedRing = false;
    C3DScreen(gScreen);
}
void C3DScreen(int p) {
    if (gInFrame) BatchEmpty();
    gScreen = (p == ScreenUp) ? ScreenUp : ScreenDown;
    if (!gInFrame || !gTarget[gScreen]) return;
    if (!gClean[gScreen]) {
        // (la primera vez en el cuadro: el clear de citro3d borra la pantalla entera, despues van los Clear del editor)
        const u32 c = ((u32)(clearCol[0] * 255.0f) << 24) | ((u32)(clearCol[1] * 255.0f) << 16) | ((u32)(clearCol[2] * 255.0f) << 8) | 0xFFu;
        C3D_RenderTargetClear(gTarget[gScreen], C3D_CLEAR_ALL, c, 0);
        gClean[gScreen] = true;
    }
    C3D_FrameDrawOn(gTarget[gScreen]);   // (pone el viewport de la pantalla entera y cambia el ancho)
    for (int i = 0; i < 4; i++) gVpPlaced[i] = -1;
    for (int i = 0; i < 5; i++) gPlacedSc[i] = -1;
    SetViewport();
    SetScissor();
}
void C3DFrameFinish() {
    if (!gInFrame) return;
    BatchEmpty();
    Ring& a = gRing[gRingAct];
    if (a.used) GSPGPU_FlushDataCache(a.base, a.used);   // (lo que la CPU copio tiene que estar en memoria para la GPU)
    // (GX_CMDLIST_FLUSH: sin eso citro3d vacia la cache de TODA la memoria lineal en cada cuadro; lo que la GPU lee ya
    // esta vaciado: el anillo aca y cada textura al subirla)
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    gInFrame = false;
}

// ============================================================================
//  Helpers de matrices (column-major, convencion GL)
// ============================================================================
static Matrix4 mTranslate(float x, float y, float z) { Matrix4 m; m.Identity(); m.m[12] = x; m.m[13] = y; m.m[14] = z; return m; }
static Matrix4 mScale(float x, float y, float z) { Matrix4 m; m.Identity(); m.m[0] = x; m.m[5] = y; m.m[10] = z; return m; }
static Matrix4 mRotate(float deg, float x, float y, float z) {
    float r = deg * 0.01745329252f, c = cosf(r), s = sinf(r);
    float len = sqrtf(x * x + y * y + z * z); if (len > 1e-6f) { x /= len; y /= len; z /= len; }
    float ic = 1.0f - c;
    Matrix4 m; m.Identity();
    m.m[0] = c + x * x * ic;     m.m[1] = y * x * ic + z * s; m.m[2] = z * x * ic - y * s;
    m.m[4] = x * y * ic - z * s; m.m[5] = c + y * y * ic;     m.m[6] = z * y * ic + x * s;
    m.m[8] = x * z * ic + y * s; m.m[9] = y * z * ic - x * s; m.m[10] = c + z * z * ic;
    return m;
}
static Matrix4 mFrustum(float l, float r, float b, float t, float n, float f) {
    Matrix4 m; for (int i = 0; i < 16; i++) m.m[i] = 0;
    m.m[0] = 2 * n / (r - l); m.m[5] = 2 * n / (t - b); m.m[8] = (r + l) / (r - l); m.m[9] = (t + b) / (t - b);
    m.m[10] = -(f + n) / (f - n); m.m[11] = -1; m.m[14] = -(2 * f * n) / (f - n);
    return m;
}
static Matrix4 mOrtho(float l, float r, float b, float t, float n, float f) {
    Matrix4 m; m.Identity();
    m.m[0] = 2 / (r - l); m.m[5] = 2 / (t - b); m.m[10] = -2 / (f - n);
    m.m[12] = -(r + l) / (r - l); m.m[13] = -(t + b) / (t - b); m.m[14] = -(f + n) / (f - n);
    return m;
}
// (cada cambio a una matriz sube su version: el draw sube a la GPU solo las que cambiaron desde la ultima vez)
static unsigned gShowMat[3] = { 0, 0, 0 };   // proyeccion, modelview, textura
static Matrix4& cur() {
    gShowMat[matMode == 0 ? 0 : (matMode == 2 ? 2 : 1)]++;
    return matMode == 0 ? curProj : (matMode == 2 ? curTex : curMV);
}

// EL AJUSTE A LA 3DS, despues de la proyeccion de GL: la pantalla girada un cuarto de vuelta (x' = y, y' = -x) y la z de
// GL (-1 cerca, 1 lejos) llevada a la de la PICA (-1 cerca, 0 lejos). En la PICA la profundidad guardada queda 1 cerca
// y 0 lejos (C3D_DepthMap de fabrica): por eso los test de z van al reves (LESS de GL = GREATER).
static const Matrix4& Fit() {
    static Matrix4 a;
    static bool ready = false;
    if (ready) return a;
    for (int i = 0; i < 16; i++) a.m[i] = 0;
    a.m[1 * 4 + 0] = 1.0f;     // x' = y
    a.m[0 * 4 + 1] = -1.0f;    // y' = -x
    a.m[2 * 4 + 2] = 0.5f;     // z' = z/2 - w/2
    a.m[3 * 4 + 2] = -0.5f;
    a.m[3 * 4 + 3] = 1.0f;
    ready = true;
    return a;
}
static void AMtx(const Matrix4& M, C3D_Mtx& o) {   // column-major de GL -> filas de citro3d
    for (int r = 0; r < 4; r++) { o.r[r].x = M.m[r]; o.r[r].y = M.m[4 + r]; o.r[r].z = M.m[8 + r]; o.r[r].w = M.m[12 + r]; }
}

// ============================================================================
//  Capacidades
// ============================================================================
void Enable(Cap c) {
    switch (c) {
        case DepthTest: cap_depth = true; break;
        case CullFace: cap_cull = true; break;
        case Blend: cap_blend = true; break;
        case ScissorTest: cap_scissor = true; SetScissor(); break;
        case Texture2D: cap_tex = true; break;
        case Lighting: cap_light = true; break;
        case Fog: cap_fog = true; break;
        case ColorMaterial: cap_colormat = true; dirtyMaterial = true; break;
        case PolygonOffsetFill: cap_offset = true; break;
        case Light0: if (!lights[0].on) { lights[0].on = true; lightsDirty = true; } break;
        default: break;
    }
}
void Disable(Cap c) {
    switch (c) {
        case DepthTest: cap_depth = false; break;
        case CullFace: cap_cull = false; break;
        case Blend: cap_blend = false; break;
        case ScissorTest: cap_scissor = false; SetScissor(); break;
        case Texture2D: cap_tex = false; break;
        case Lighting: cap_light = false; break;
        case Fog: cap_fog = false; break;
        case ColorMaterial: cap_colormat = false; dirtyMaterial = true; break;
        case PolygonOffsetFill: cap_offset = false; break;
        case Light0: if (lights[0].on) { lights[0].on = false; lightsDirty = true; } break;
        default: break;
    }
}
bool IsEnabled(Cap c) {
    switch (c) {
        case DepthTest: return cap_depth; case CullFace: return cap_cull; case Blend: return cap_blend;
        case ScissorTest: return cap_scissor; case Texture2D: return cap_tex; case Lighting: return cap_light;
        case Fog: return cap_fog; case ColorMaterial: return cap_colormat; case Light0: return lights[0].on;
        case PolygonOffsetFill: return cap_offset;
        default: return false;
    }
}

// ============================================================================
//  Matrices
// ============================================================================
void MatrixMode(Matrix m) { matMode = (m == Projection) ? 0 : (m == TextureMatrix ? 2 : 1); }
void LoadIdentity() { cur().Identity(); }
void PushMatrix() { if (matMode == 0) stkProj.push_back(curProj); else if (matMode == 2) stkTex.push_back(curTex); else stkMV.push_back(curMV); }
void PopMatrix() {
    gShowMat[matMode == 0 ? 0 : (matMode == 2 ? 2 : 1)]++;
    if (matMode == 0) { if (!stkProj.empty()) { curProj = stkProj.back(); stkProj.pop_back(); } }
    else if (matMode == 2) { if (!stkTex.empty()) { curTex = stkTex.back(); stkTex.pop_back(); } }
    else { if (!stkMV.empty()) { curMV = stkMV.back(); stkMV.pop_back(); } }
}
void Translatef(float x, float y, float z) { cur() = cur() * mTranslate(x, y, z); }
void Rotatef(float a, float x, float y, float z) { cur() = cur() * mRotate(a, x, y, z); }
void Scalef(float x, float y, float z) { cur() = cur() * mScale(x, y, z); }
void MultMatrix(const float* m16) { Matrix4 M; memcpy(M.m, m16, sizeof(float) * 16); cur() = cur() * M; }
void LoadMatrix(const float* m16) { memcpy(cur().m, m16, sizeof(float) * 16); }
void Ortho(float l, float r, float b, float t, float n, float f) { cur() = cur() * mOrtho(l, r, b, t, n, f); }
void Frustum(float l, float r, float b, float t, float n, float f) { cur() = cur() * mFrustum(l, r, b, t, n, f); }
void Perspective(float fovy, float aspect, float n, float f) {
    const float t = n * tanf(fovy * 0.5f * 0.01745329252f);
    Frustum(-t * aspect, t * aspect, -t, t, n, f);
}
void GetMatrix(Matrix m, float* out16) {
    const Matrix4& M = (m == Projection) ? curProj : (m == TextureMatrix ? curTex : curMV);
    for (int i = 0; i < 16; i++) out16[i] = M.m[i];
}
bool g_matrixAudit   = false;
int  g_matrixDesyncs = 0;
bool g_invalidatePerMesh = false;
bool g_matricesLazy      = true;
bool g_pointersCache = true;
void MatricesSync() {}
void BiasMeters(float m) {
    static float base[16]; static float placed = 0.0f;
    if (m == placed) return;
    if (placed == 0.0f) GetMatrix(Projection, base);
    float P[16]; for (int i = 0; i < 16; i++) P[i] = base[i];
    P[14] -= P[10] * m;
    MatrixMode(Projection); LoadMatrix(P); MatrixMode(ModelView);
    placed = m;
}

// ============================================================================
//  Viewport / scissor (en coordenadas de GL de la pantalla apaisada: se giran al framebuffer de la PICA)
// ============================================================================
static void SetViewport() {
    if (!gInFrame) return;
    const int W = C3DScreenWidth(gScreen);
    int x = vpCurrent[1], y = W - vpCurrent[0] - vpCurrent[2], w = vpCurrent[3], h = vpCurrent[2];
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x == gVpPlaced[0] && y == gVpPlaced[1] && w == gVpPlaced[2] && h == gVpPlaced[3]) return;
    BatchEmpty();
    gVpPlaced[0] = x; gVpPlaced[1] = y; gVpPlaced[2] = w; gVpPlaced[3] = h;
    C3D_SetViewport((u32)x, (u32)y, (u32)w, (u32)h);
}
static void SetScissor() {
    if (!gInFrame) return;
    int mode = GPU_SCISSOR_DISABLE, l = 0, t = 0, r = 0, b = 0;
    if (cap_scissor) {
        const int W = C3DScreenWidth(gScreen);
        mode = GPU_SCISSOR_NORMAL;
        l = scCurrent[1]; r = scCurrent[1] + scCurrent[3]; t = W - scCurrent[0] - scCurrent[2]; b = W - scCurrent[0];
        if (l < 0) l = 0; if (t < 0) t = 0; if (r > 240) r = 240; if (b > W) b = W;
        if (r <= l || b <= t) { l = 0; t = 0; r = 1; b = 1; }   // (vacio: un pixel, para no dibujar nada visible)
    }
    if (mode == gPlacedSc[0] && l == gPlacedSc[1] && t == gPlacedSc[2] && r == gPlacedSc[3] && b == gPlacedSc[4]) return;
    BatchEmpty();
    gPlacedSc[0] = mode; gPlacedSc[1] = l; gPlacedSc[2] = t; gPlacedSc[3] = r; gPlacedSc[4] = b;
    C3D_SetScissor((GPU_SCISSORMODE)mode, (u32)l, (u32)t, (u32)r, (u32)b);
}
void Viewport(int x, int y, int w, int h) { vpCurrent[0] = x; vpCurrent[1] = y; vpCurrent[2] = w < 0 ? 0 : w; vpCurrent[3] = h < 0 ? 0 : h; SetViewport(); }
void Scissor(int x, int y, int w, int h) { scCurrent[0] = x; scCurrent[1] = y; scCurrent[2] = w < 0 ? 0 : w; scCurrent[3] = h < 0 ? 0 : h; SetScissor(); }
void GetViewport(int v[4]) { for (int i = 0; i < 4; i++) v[i] = vpCurrent[i]; }
void GetScissor(int v[4]) { for (int i = 0; i < 4; i++) v[i] = scCurrent[i]; }
bool ScissorActive() { return cap_scissor; }

// ============================================================================
//  Estado suelto
// ============================================================================
void ClearColor(float r, float g, float b, float a) { clearCol[0] = r; clearCol[1] = g; clearCol[2] = b; clearCol[3] = a; }
static void ClearWithRect(int bits);
void Clear(int bits) { ClearWithRect(bits); }
int  PlanesClipMax() { return 0; }
void PlanesClip(int, const float*) {}
int  StencilBits() { return 8; }
void StencilClear(int v) { stencilValue = v; ClearWithRect(StencilBuffer); }
void StencilMark(int v) { stencil = 1; stencilValue = v; }
void StencilInside(int v) { stencil = 2; stencilValue = v; }
void StencilTurnOff() { stencil = 0; }
void DepthFunc(DepthCmp c) { depthFunc = (int)c; }
void DepthMask(bool w) { depthMask = w; }
void ColorMask(bool r, bool g, bool b, bool a) { (void)a; colorMask = r || g || b; }
void LineWidth(float px) { lineWidth = px; }
void FrontFace(bool ccw) { frontCCW = ccw; }
void CullFrontalFaces(bool on) { cullFront = on; }
void PolygonOffset(float factor, float units) { polyF = factor; polyU = units; }
void DepthRange(float, float) {}
void SmoothShading(bool) {}
void FastPerspective() {}
void Wireframe(bool) {}
void PointSize(float px) { pointSize = px; }
void PointSpriteCoordReplace(bool on) { pointSprite = on; }
void BlurY(float) {}
void AlphaTest(float ref) { alphaRef = ref > 0.0f ? ref : 0.0f; }
void AlphaPacked(bool) {}
void BlendAlpha() { mixSignature = MixAlpha; }
void SetMix(int m) { mixSignature = m; }
void BlendMode(int mode) {
    mixSignature = (mode == 1) ? MixMultiply : (mode == 2) ? MixAdd : (mode == 3) ? 100 : (mode == 4) ? MixSubtract
                : (mode == 5) ? MixScreen : (mode == 6) ? 101 : MixAlpha;   // (101: Multiply con su factor)
}
void Invalidate() { texGenCache = -1; matcapCache = -1; }
void BaseState() {   // (lo mismo que el backend fijo: el texenv de un constructor no le llega a la interfaz)
    BiasMeters(0.0f); AlphaTest(0.0f); Disable(PolygonOffsetFill);
    TexMatrixMatcap(false); TexGenSphere(false); TexEnvReplace(false); MixTexturesOff();
    SmoothShading(true); Disable(Lighting); Disable(ColorMaterial);
}
bool SnapshotStateSupported() { return false; }
void SnapshotStateTake() {}
void SnapshotStateRestore() {}
unsigned int ErrorGL() { return 0u; }
void Finish() {}
void Flush() {}
void DrawPending() {}
bool g_auditScene = false;
int  g_auditDesyncs = 0;
int  AuditState() { return 0; }
void TexBaseLevel(int) {}
void GLES2Init(void* (*)(const char*)) {}

// ============================================================================
//  Luz / material (las luces se guardan como GL: posicion y cono ya en OJO, con la modelview de AHORA)
// ============================================================================
static void InEyePoint(const float* v, float* out) {
    const float* m = curMV.m;
    for (int k = 0; k < 3; k++) out[k] = m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k] * v[3];
    out[3] = v[3];
}
void LightNfv(int i, LightFv p, const float* v) {
    if (i < 0 || i >= LightsMax) return;
    Light& L = lights[i];
    if (p == LightPosition) InEyePoint(v, L.pos);
    else if (p == LightDiffuse) for (int k = 0; k < 4; k++) L.dif[k] = v[k];
    else if (p == LightAmbient) for (int k = 0; k < 4; k++) L.amb[k] = v[k];
    else if (p == LightSpecular) for (int k = 0; k < 4; k++) L.spec[k] = v[k];
    lightsDirty = true;
}
void LightNf(int i, LightF p, float v) {
    if (i < 0 || i >= LightsMax) return;
    lights[i].att[p == LightConstantAtt ? 0 : (p == LightLinearAtt ? 1 : 2)] = v;
    lightsDirty = true;
}
void LightNSpot(int i, float cut, float exponent, const float* dir3) {
    if (i < 0 || i >= LightsMax) return;
    Light& L = lights[i];
    const float* m = curMV.m;
    for (int k = 0; k < 3; k++) L.dir[k] = m[k] * dir3[0] + m[4 + k] * dir3[1] + m[8 + k] * dir3[2];
    L.cutCos = (cut >= 180.0f) ? -1.0f : cosf(cut * 0.01745329252f);
    L.expo = exponent;
    lightsDirty = true;
}
void LightN(int i, bool on) { if (i < 0 || i >= LightsMax || lights[i].on == on) return; lights[i].on = on; lightsDirty = true; }
void Light0fv(LightFv p, const float* v) { LightNfv(0, p, v); }
void Light0f(LightF p, float v) { LightNf(0, p, v); }
void SetLightEnabled(unsigned int id, bool on) { if (id >= 0x4000u && id < 0x4000u + (unsigned)LightsMax) LightN((int)(id - 0x4000u), on); }
void LightModelAmbient(const float* c) { for (int k = 0; k < 4; k++) ambGlobal[k] = c[k]; dirtyMaterial = true; }
void LightModelTwoSide(bool) {}   // (el citro3d ilumina una cara)
void Material(MatParam p, const float* rgba) {
    float* d = (p == MatDiffuse) ? matDiff : (p == MatAmbient) ? matAmb : (p == MatSpecular) ? matSpec : matEmis;
    if (memcmp(d, rgba, sizeof(float) * 4) == 0) return;
    for (int i = 0; i < 4; i++) d[i] = rgba[i];
    dirtyMaterial = true;
}
void MaterialShininess(float s) { if (s != matShine) { matShine = s; dirtyMaterial = true; } }
void MaterialPBR(float, float, float, float, bool) {}   // (la luz fija de la 3DS: sin luz PBR)
bool MaterialMaps(unsigned, unsigned, unsigned, unsigned, unsigned) { return false; }   // (sin shaders)
void AmbientSH(const float*, const float*) {}
bool ShadowsSupported() { return false; }   // (sin shaders: las sombras van por la CPU, RealTimeShadows.cpp)
bool ShadowMapBegin(int, int) { return false; }
void ShadowMapBox(int, int, int, int) {}
void ShadowMapFinish() {}
void ShadowReceptors(const float*) {}
void ShadowReceive(int) {}
bool CaptureBegin(int, int) { return false; }   // (sin la imagen fuera de pantalla: la sonda de tiempo real no se dibuja)
void CaptureFinish() {}
void Environment(unsigned int, unsigned int, const float*) {}
void SceneCopy(bool) {}
bool GpuLimited() { return false; }
unsigned int UploadCube(const unsigned char* const*, int) { return 0; }   // (sin shaders: el entorno no se usa)
void EnvironmentCube(unsigned int, unsigned int, const float*) {}

// ============================================================================
//  Textura / texenv / matriz de textura
// ============================================================================
void BindTexture(unsigned int id) { boundTex = id; ::W3dC3DBindTexture(id); }
void ShadingPerPixel(bool){}                 // (pipeline fijo: la luz es por vertice, siempre)
bool ShadingPerPixelSupported(){ return false; }
// (sin shaders: los pases van por la CPU y el postprocesado tambien; ver graphics/w3dGraphics.h)
bool PassesWithShader(){ return false; }
void SpecialPass(int, const float*){}
void PrevPointer3f(const float*){}
bool PostEffect(int, unsigned int, unsigned int, int, int, const float*, const float*){ return false; }
bool ShadersWritten(){ return false; }
int  WrittenShaderCompile(const char*, const char*, char* log, int logMax){ if(log && logMax > 0) log[0] = 0; return 0; }
void WrittenShaderDelete(int){}
void WrittenShaderUse(int, const float*){}
void TexEnvDot3(bool on) { texEnv = on ? 2 : 0; }
void TexEnvReplace(bool on) { texEnv = on ? 1 : 0; }
void TexEnvAlphaOnly(bool on) { texEnv = on ? 3 : 0; }
bool MixTexturesSupported() { return true; }
void MixTextures(unsigned int idB, float k, bool filtered, bool repeat, int mode) {
    if (!idB) { MixTexturesOff(); return; }
    if (k < 0.0f) k = 0.0f; if (k > 1.0f) k = 1.0f;
    if (mode < 0 || mode > 4) mode = 0;
    if (mode == 4) mode = 1;
    mixOn = true; mixTex = idB; mixK = k; texMixMode = mode;
    C3D_Tex* t = w3dC3D::Texture(idB);
    if (t) {
        C3D_TexSetFilter(t, filtered ? GPU_LINEAR : GPU_NEAREST, filtered ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetWrap(t, repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE, repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
    }
}
void MixTexturesOff() { mixOn = false; mixUV = 0; }
bool MixTexturesActive() { return mixOn; }
void MixTexturesUV(const float* p) { mixUV = p; }
void MixTexturesUVVBO(unsigned int) { mixUV = 0; }
void MixTexturesUV2s(const short*) { mixUV = 0; }
void MixTexturesMatrixUV(float, float, float, float) {}   // (sin matriz propia en la unidad 1: queda quieta)
void MixDeletedTexture(unsigned int id) { if (id && id == mixTex) { mixTex = 0; mixOn = false; } }
void TexGenSphere(bool on) {
    if (texGenCache == (on ? 1 : 0)) return;
    texGenCache = on ? 1 : 0;
    matcapCache = -1;
    curTex.Identity();
    if (on) { curTex.m[5] = -1.0f; curTex.m[13] = 1.0f; }
}
void TexMatrixMatcap(bool on) {
    if (!on && matcapCache == 0) return;
    matcapCache = on ? 1 : 0;
    curTex.Identity();
    if (!on) return;
    const float* mv = curMV.m;
    const float s = 0.5f / 127.0f;
    float* T = curTex.m;
    T[0]     = s * mv[0];  T[1] = -s * mv[1]; T[2] = 0.0f;  T[3] = 0.0f;
    T[4]     = s * mv[4];  T[5] = -s * mv[5]; T[6] = 0.0f;  T[7] = 0.0f;
    T[8] = s * mv[8];  T[9] = -s * mv[9]; T[10] = 0.0f; T[11] = 0.0f;
    T[12] = 0.5f;      T[13] = 0.5f;      T[14] = 0.0f; T[15] = 1.0f;
}
void TexMatrixUV(float du, float dv, float su, float sv) {
    const bool ident = du == 0.0f && dv == 0.0f && su == 1.0f && sv == 1.0f;
    if (ident ? matcapCache == 0 : (matcapCache == 2 && du == uvMat[0] && dv == uvMat[1] && su == uvMat[2] && sv == uvMat[3])) return;
    matcapCache = ident ? 0 : 2;
    uvMat[0] = du; uvMat[1] = dv; uvMat[2] = su; uvMat[3] = sv;
    curTex.Identity();
    curTex.m[0] = su; curTex.m[5] = sv; curTex.m[12] = du; curTex.m[13] = dv;
}
bool HasTexGen() { return false; }   // (el sphere-map por hardware todavia no: cae al de software)

// ============================================================================
//  Color / niebla / arreglos
// ============================================================================
// (el alfa por g_alphaColor: la UI translucida, ver w3dGraphics.h)
void Color4f(float r, float g, float b, float a) { curColor[0] = r; curColor[1] = g; curColor[2] = b; curColor[3] = a * g_alphaColor; }
void Color4fv(const float* c) { for (int i = 0; i < 4; i++) curColor[i] = c[i]; curColor[3] *= g_alphaColor; }
void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { curColor[0] = r / 255.0f; curColor[1] = g / 255.0f; curColor[2] = b / 255.0f; curColor[3] = a / 255.0f * g_alphaColor; }
void FogMode(int m) { fogMode = (m < 0 || m > 2) ? 0 : m; }
void FogDensity(float d) { fogDens = d; }
void FogStart(float z) { fogStart = z; }
void FogEnd(float z) { fogEnd = z; }
void FogColor(const float* rgba) { for (int i = 0; i < 4; i++) fogColor[i] = rgba[i]; }
void EnableArray(VArray a) { if (a == VertexArray) aPos.on = true; else if (a == NormalArray) aNrm.on = true; else if (a == TexCoordArray) aUV.on = true; else aCol.on = true; }
void DisableArray(VArray a) { if (a == VertexArray) aPos.on = false; else if (a == NormalArray) aNrm.on = false; else if (a == TexCoordArray) aUV.on = false; else aCol.on = false; }
int  ArrayState(VArray a) { return (a == VertexArray ? aPos.on : a == NormalArray ? aNrm.on : a == TexCoordArray ? aUV.on : aCol.on) ? 1 : 0; }
void VertexPointer2f(int s, const float* p) { aPos.size = 2; aPos.type = FloatType; aPos.step = s; aPos.ptr = p; }
void VertexPointer3f(int s, const float* p) { aPos.size = 3; aPos.type = FloatType; aPos.step = s; aPos.ptr = p; }
void VertexPointer4f(int s, const float* p) { aPos.size = 4; aPos.type = FloatType; aPos.step = s; aPos.ptr = p; }
void VertexPointer3s(int s, const short* p) { aPos.size = 3; aPos.type = ShortType; aPos.step = s; aPos.ptr = p; }
void VertexPointer2s(int s, const short* p) { aPos.size = 2; aPos.type = ShortType; aPos.step = s; aPos.ptr = p; }
void NormalPointer3b(const signed char* p) { aNrm.size = 3; aNrm.type = ByteType; aNrm.step = 0; aNrm.ptr = p; }
void ColorPointer4ub(const unsigned char* p) { aCol.size = 4; aCol.type = TypeUByte; aCol.step = 0; aCol.ptr = p; }
void ColorPointer4f(int s, const float* p) { aCol.size = 4; aCol.type = FloatType; aCol.step = s; aCol.ptr = p; }
void TexCoordPointer2f(int s, const float* p) { aUV.size = 2; aUV.type = FloatType; aUV.step = s; aUV.ptr = p; }
void TexCoordPointer2s(int s, const short* p) { aUV.size = 2; aUV.type = ShortType; aUV.step = s; aUV.ptr = p; }
void TexCoordPointer3b(const signed char* p, int) { aUV.size = 3; aUV.type = ByteType; aUV.step = 0; aUV.ptr = p; }

// ============================================================================
//  El estado de la PICA antes de cada draw
// ============================================================================
static int Bytes(int type) { return type == FloatType ? 4 : (type == ShortType ? 2 : 1); }
static GPU_FORMATS FormatGPU(int type) { return type == FloatType ? GPU_FLOAT : type == ShortType ? GPU_SHORT : type == ByteType ? GPU_BYTE : GPU_UNSIGNED_BYTE; }
static GPU_TESTFUNC FuncZ() {
    switch (depthFunc) {
        case DepthLEqual: return GPU_GEQUAL;
        case DepthEqual: return GPU_EQUAL;
        case DepthAlways: return GPU_ALWAYS;
        default: return GPU_GREATER;
    }
}
static void SetMix() {
    if (!cap_blend) { C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO); return; }
    switch (mixSignature) {
        case MixOff:      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO); break;
        case MixAdd:      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE); break;
        case MixAddAlpha: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE); break;
        case MixMultiply: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ZERO, GPU_DST_ALPHA, GPU_ZERO); break;
        case MixScreen:   C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_COLOR, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA); break;
        case MixPremult:  C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA); break;
        case MixSubtract: C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE); break;
        case 100:            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_SRC_COLOR, GPU_DST_ALPHA, GPU_SRC_ALPHA); break;
        case 101:            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_SRC_ALPHA, GPU_DST_ALPHA, GPU_SRC_ALPHA); break;
        default:             C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA); break;
    }
}
// la luz por fragmento de la PICA con la cuenta del pipeline fijo: emision + ambiente global * ambiente del material
// + por luz (ambiente de la luz * ambiente + N.L * difusa + (N.H)^brillo * especular). Con COLOR_MATERIAL el material
// queda blanco y el combiner multiplica por el color del vertice
static void SetLights() {
    if (dirtyMaterial) {
        C3D_Material m;
        for (int k = 0; k < 3; k++) {
            m.ambient[k] = cap_colormat ? 1.0f : matAmb[k];
            m.diffuse[k] = cap_colormat ? 1.0f : matDiff[k];
            m.specular0[k] = matSpec[k];
            m.specular1[k] = 0.0f;
            m.emission[k] = matEmis[k] + ambGlobal[k] * (cap_colormat ? 1.0f : matAmb[k]);
        }
        C3D_LightEnvMaterial(&gLightEnv, &m);
        if (matShine != gLutEspBrightness) {
            LightLut_Phong(&gLutEsp, matShine > 0.0f ? matShine : 0.001f);
            C3D_LightEnvLut(&gLightEnv, GPU_LUT_D0, GPU_LUTINPUT_NH, false, &gLutEsp);
            gLutEspBrightness = matShine;
        }
        dirtyMaterial = false;
    }
    if (lightsDirty) {
        for (int i = 0; i < LightsMax; i++) {
            const Light& L = lights[i];
            C3D_LightEnable(&gLight[i], L.on);
            if (!L.on) continue;
            C3D_FVec p = FVec4_New(L.pos[0], L.pos[1], L.pos[2], L.pos[3] != 0.0f ? 1.0f : 0.0f);
            C3D_LightPosition(&gLight[i], &p);
            C3D_LightDiffuse(&gLight[i], L.dif[0], L.dif[1], L.dif[2]);
            C3D_LightAmbient(&gLight[i], L.amb[0], L.amb[1], L.amb[2]);
            C3D_LightSpecular0(&gLight[i], L.spec[0], L.spec[1], L.spec[2]);
        }
        lightsDirty = false;
    }
}
static void SetCombiner(bool withTex, bool withLight) {
    C3D_TexEnv* e0 = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e0);
    // el color "primario": la luz por fragmento (por el color del vertice si hay COLOR_MATERIAL) o el del vertice
    const GPU_TEVSRC prim = withLight ? GPU_FRAGMENT_PRIMARY_COLOR : GPU_PRIMARY_COLOR;
    if (!withTex) {
        C3D_TexEnvSrc(e0, C3D_Both, prim, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e0, C3D_Both, (withLight && cap_colormat) ? GPU_MODULATE : GPU_REPLACE);
        if (withLight && !cap_colormat) {
            // (el alfa de la luz: el del difuso del material)
            C3D_TexEnvSrc(e0, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
            C3D_TexEnvColor(e0, (u32)(matDiff[3] * 255.0f) << 24);
        }
    } else if (texEnv == 1) {
        C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
        C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
    } else if (texEnv == 3) {
        C3D_TexEnvSrc(e0, C3D_RGB, prim, prim, prim);
        C3D_TexEnvFunc(e0, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
    } else if (texEnv == 2) {
        C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e0, C3D_RGB, GPU_DOT3_RGB);
        C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e0, C3D_Alpha, GPU_MODULATE);
    } else {
        C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, prim, prim);
        C3D_TexEnvFunc(e0, C3D_Both, GPU_MODULATE);
    }
    // etapa 1: la especular de la luz (se suma) o nada
    C3D_TexEnv* e1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(e1);
    if (withLight && texEnv != 1) {
        C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_FRAGMENT_SECONDARY_COLOR, GPU_PREVIOUS);
        C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD);
    }
    // etapa 2: COLOR_MATERIAL con textura (la luz ya modulo la textura: falta el color del vertice)
    C3D_TexEnv* e2 = C3D_GetTexEnv(2);
    C3D_TexEnvInit(e2);
    if (withLight && withTex && cap_colormat && texEnv == 0) {
        C3D_TexEnvSrc(e2, C3D_Both, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PREVIOUS);
        C3D_TexEnvFunc(e2, C3D_Both, GPU_MODULATE);
    }
    // etapa 3: la segunda textura (mezcla) sobre lo anterior
    C3D_TexEnv* e3 = C3D_GetTexEnv(3);
    C3D_TexEnvInit(e3);
    if (mixOn && mixTex) {
        if (texMixMode == 0) {
            C3D_TexEnvSrc(e3, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_CONSTANT);
            C3D_TexEnvOpRgb(e3, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
            C3D_TexEnvFunc(e3, C3D_RGB, GPU_INTERPOLATE);
            C3D_TexEnvColor(e3, (u32)(mixK * 255.0f) << 24);
        } else if (texMixMode == 3) {
            C3D_TexEnvSrc(e3, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PREVIOUS);
            C3D_TexEnvFunc(e3, C3D_RGB, GPU_SUBTRACT);
        } else {
            C3D_TexEnvSrc(e3, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PREVIOUS);
            C3D_TexEnvFunc(e3, C3D_RGB, GPU_MODULATE);
            if (texMixMode == 2) C3D_TexEnvScale(e3, C3D_RGB, GPU_TEVSCALE_2);
        }
    }
}
// LO QUE YA ESTA PUESTO EN LA PICA: citro3d vuelve a mandar a la GPU todo lo que se toca aunque quede igual (el combiner
// entero, la textura y el vaciado de su cache, las matrices...), y eso era lo que mas tardaba de cada draw. Cada draw
// compara con lo ultimo que se mando y toca solo lo que cambio
struct Placed {
    GPU_TESTFUNC zFunc; int zWrites; float zOffset;
    GPU_CULLMODE cull;
    int mix, alpha, stencil, stencilValue;
    unsigned combiner;
    C3D_Tex* tex[2];
    unsigned showProj, showMV, showTex; bool clip;
    float scale[4];
    float fixed[5][4];
};
static Placed gPlaced;
static bool   gPlacedValid = false;   // false: lo proximo se manda todo
// la textura cambio (sus datos, el filtro o se borro): el proximo draw la vuelve a atar (y la GPU vacia su cache)
void C3DTouchedTexture(C3D_Tex* t) {
    BatchEmpty();   // (lo que ya esta en el lote se dibuja con la textura como estaba atada)
    for (int i = 0; i < 2; i++) if (gPlaced.tex[i] == t) gPlaced.tex[i] = NULL;
}
static void SetFixed(int i, float x, float y, float z, float w) {
    float* f = gPlaced.fixed[i];
    if (gPlacedValid && f[0] == x && f[1] == y && f[2] == z && f[3] == w) return;
    f[0] = x; f[1] = y; f[2] = z; f[3] = w;
    C3D_FixedAttribSet(i, x, y, z, w);
}
static void SetScale(float x, float y, float z, float w) {
    float* e = gPlaced.scale;
    if (gPlacedValid && e[0] == x && e[1] == y && e[2] == z && e[3] == w) return;
    e[0] = x; e[1] = y; e[2] = z; e[3] = w;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, uScale, x, y, z, w);
}
// la clave del combiner: todo lo que decide como quedan las etapas en SetCombiner
static unsigned KeyCombiner(bool withTex, bool withLight) {
    const bool mix = mixOn && mixTex;
    unsigned k = (withTex ? 1u : 0u) | (withLight ? 2u : 0u) | ((unsigned)texEnv << 2) | (cap_colormat ? 16u : 0u) | (mix ? 32u : 0u);
    if (mix) k |= ((unsigned)texMixMode << 6) | ((unsigned)(mixK * 255.0f) << 8);
    if (withLight && !cap_colormat && !withTex) k |= (unsigned)(matDiff[3] * 255.0f) << 16;
    return k;
}
static void UploadMatrices(bool alreadyInClip) {
    C3D_Mtx m;
    if (alreadyInClip) {
        if (gPlacedValid && gPlaced.clip) return;
        C3D_Mtx ident;
        Mtx_Identity(&ident);
        AMtx(Fit(), m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp, &m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMv, &ident);
        C3D_FVUnifMtx3x4(GPU_VERTEX_SHADER, uNmat, &ident);
        gPlaced.clip = true;
        return;
    }
    const bool changeMV = !gPlacedValid || gPlaced.clip || gPlaced.showMV != gShowMat[1];
    const bool changeP = !gPlacedValid || gPlaced.clip || gPlaced.showProj != gShowMat[0];
    if (changeMV || changeP) {
        AMtx(Fit() * curProj * curMV, m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp, &m);
    }
    if (changeMV) {
        AMtx(curMV, m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMv, &m);
        // la matriz de las normales: la inversa transpuesta de la modelview (3x3)
        const float* q = curMV.m;
        const float c0[3] = { q[0], q[1], q[2] }, c1[3] = { q[4], q[5], q[6] }, c2[3] = { q[8], q[9], q[10] };
        const float n0[3] = { c1[1] * c2[2] - c1[2] * c2[1], c1[2] * c2[0] - c1[0] * c2[2], c1[0] * c2[1] - c1[1] * c2[0] };
        const float n1[3] = { c2[1] * c0[2] - c2[2] * c0[1], c2[2] * c0[0] - c2[0] * c0[2], c2[0] * c0[1] - c2[1] * c0[0] };
        const float n2[3] = { c0[1] * c1[2] - c0[2] * c1[1], c0[2] * c1[0] - c0[0] * c1[2], c0[0] * c1[1] - c0[1] * c1[0] };
        const float det = c0[0] * n0[0] + c0[1] * n0[1] + c0[2] * n0[2];
        const float id = fabsf(det) > 1e-12f ? 1.0f / det : 1.0f;
        C3D_Mtx nm;
        for (int k = 0; k < 4; k++) { nm.r[k].x = 0; nm.r[k].y = 0; nm.r[k].z = 0; nm.r[k].w = 0; }
        // fila k de la matriz normal = componente k de las columnas n0, n1, n2 (ya traspuesta)
        for (int k = 0; k < 3; k++) { nm.r[k].x = n0[k] * id; nm.r[k].y = n1[k] * id; nm.r[k].z = n2[k] * id; }
        C3D_FVUnifMtx3x4(GPU_VERTEX_SHADER, uNmat, &nm);
    }
    gPlaced.showProj = gShowMat[0]; gPlaced.showMV = gShowMat[1]; gPlaced.clip = false;
}
// lo comun a todos los draws: z, culling, mezcla, alpha test, estencil, texturas, combiner, luz y las matrices.
// Primero se calcula a que tiene que quedar (StateDraw, sale del estado emulado de GL) y despues se pone (solo lo que
// cambio). 'alreadyInClip' = la posicion ya viene en el espacio de recorte de GL (armada en la CPU: lineas, puntos y el
// lote): ahi no hay luz ni culling y la matriz es la de ajuste sola
struct StateDraw {
    GPU_TESTFUNC zFunc; int zWrites; float zOffset;
    GPU_CULLMODE cull;
    int mix, alpha, stencil, stencilValue;
    C3D_Tex* tex[2];
    bool withLight;
    unsigned combiner, showTex;
};
static void ComputeState(bool alreadyInClip, StateDraw& e) {
    memset(&e, 0, sizeof(e));   // (se comparan con memcmp: el relleno tambien en cero)
    const int colorBits = colorMask ? GPU_WRITE_COLOR : 0;
    e.zFunc = cap_depth ? FuncZ() : GPU_ALWAYS;   // (sin test, GL tampoco escribe la z)
    e.zWrites = colorBits | ((cap_depth && depthMask) ? GPU_WRITE_DEPTH : 0);
    e.zOffset = cap_offset ? -0.00002f * (polyU + polyF) : 0.0f;
    // GL_BACK descarta las de atras; GL_FRONT, las de adelante (con el winding que se haya elegido)
    e.cull = (!cap_cull || alreadyInClip) ? GPU_CULL_NONE : ((frontCCW == cullFront) ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW);
    e.mix = cap_blend ? mixSignature : MixOff;
    e.alpha = alphaRef > 0.0f ? (int)(alphaRef * 255.0f) : -1;
    e.stencil = stencil;
    e.stencilValue = stencil ? stencilValue : 0;
    e.tex[0]    = (cap_tex && boundTex) ? w3dC3D::Texture(boundTex) : NULL;
    e.tex[1]    = (mixOn && mixTex) ? w3dC3D::Texture(mixTex) : NULL;
    e.withLight = cap_light && !alreadyInClip && !(e.tex[0] && texEnv == 1);
    e.combiner = KeyCombiner(e.tex[0] != NULL, e.withLight);
    e.showTex = gShowMat[2];
}
static void ApplyState(const StateDraw& e, bool alreadyInClip) {
    const bool all = !gPlacedValid;
    if (all || e.zFunc != gPlaced.zFunc || e.zWrites != gPlaced.zWrites) {
        C3D_DepthTest(true, e.zFunc, (GPU_WRITEMASK)e.zWrites);
        gPlaced.zFunc = e.zFunc; gPlaced.zWrites = e.zWrites;
    }
    if (all || e.zOffset != gPlaced.zOffset) { C3D_DepthMap(true, -1.0f, e.zOffset); gPlaced.zOffset = e.zOffset; }
    if (all || e.cull != gPlaced.cull) { C3D_CullFace(e.cull); gPlaced.cull = e.cull; }
    if (all || e.mix != gPlaced.mix) { SetMix(); gPlaced.mix = e.mix; }
    if (all || e.alpha != gPlaced.alpha) { C3D_AlphaTest(e.alpha >= 0, GPU_GEQUAL, e.alpha >= 0 ? e.alpha : 0); gPlaced.alpha = e.alpha; }
    if (all || e.stencil != gPlaced.stencil || e.stencilValue != gPlaced.stencilValue) {
        if (e.stencil == 1) { C3D_StencilTest(true, GPU_ALWAYS, e.stencilValue, 0xFF, 0xFF); C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE); }
        else if (e.stencil == 2) { C3D_StencilTest(true, GPU_EQUAL, e.stencilValue, 0xFF, 0x00); C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP); }
        else C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0x00);
        gPlaced.stencil = e.stencil; gPlaced.stencilValue = e.stencilValue;
    }
    // texturas (la unidad sin textura queda con la anterior: el combiner no la lee)
    for (int u = 0; u < 2; u++)
        if (e.tex[u] && (all || e.tex[u] != gPlaced.tex[u])) { C3D_TexBind(u, e.tex[u]); gPlaced.tex[u] = e.tex[u]; }
    if (e.withLight) {
        if (!gLightPlacedEnv) { C3D_LightEnvBind(&gLightEnv); gLightPlacedEnv = true; }
        SetLights();
    } else if (gLightPlacedEnv) { C3D_LightEnvBind(NULL); gLightPlacedEnv = false; }
    if (all || e.combiner != gPlaced.combiner) { SetCombiner(e.tex[0] != NULL, e.withLight); gPlaced.combiner = e.combiner; }
    // matrices
    UploadMatrices(alreadyInClip);
    if (all || gPlaced.showTex != e.showTex) {
        C3D_Mtx m;
        AMtx(curTex, m);
        C3D_FVUnifMtx2x4(GPU_VERTEX_SHADER, uTexm, &m);
        gPlaced.showTex = e.showTex;
    }
    gPlacedValid = true;
}
static void SetState(bool alreadyInClip) {
    StateDraw e;
    ComputeState(alreadyInClip, e);
    ApplyState(e, alreadyInClip);
}

// un arreglo de cliente al anillo: los vertices [desde, hasta). Devuelve el puntero en memoria lineal (o NULL)
// En el anillo cada arreglo va empaquetado y lo que se declara de cada vertice ocupa el paso entero: lo de 3 bytes
// (las normales de las mallas) va como 4 componentes, con la cuarta en cero (el shader no la lee). Con 3 bytes y paso
// 4 el emulador dibujaba bien, pero en la consola el cubo salia sombreado como con las normales soldadas: la PICA de
// verdad no parece saltear los bytes que no estan descriptos (picaGL tambien agrega relleno hasta completar el paso).
// 'compsOut' = cuantas componentes declarar
static const void* CopyArray(const Arr& a, int from, int until, int& stepOut, int& compsOut) {
    const int b = Bytes(a.type);
    const int elem = a.size * b;
    const int step = a.step ? a.step : elem;
    const int n = until - from;
    if (n <= 0 || !a.ptr) return NULL;
    const int comps = (b == 1 && a.size == 3) ? 4 : a.size;
    const int elemDst = comps * b;
    u8* dst = (u8*)RingRequest((u32)(elemDst * n));
    if (!dst) return NULL;
    const u8* src = (const u8*)a.ptr + (size_t)from * step;
    if (step == elemDst && elem == elemDst) memcpy(dst, src, (size_t)elemDst * n);
    else for (int i = 0; i < n; i++) {
        u8* d = dst + (size_t)i * elemDst;
        memcpy(d, src + (size_t)i * step, (size_t)elem);
        if (elemDst > elem) memset(d + elem, 0, (size_t)(elemDst - elem));
    }
    stepOut = elemDst;
    compsOut = comps;
    return dst;
}
// los atributos del draw: los cinco del shader en orden (posicion, normal, UV, color, UV de la unidad 1), cada uno
// leido del anillo o fijo. En el anillo queda solo el rango [desde, hasta): el draw cuenta los vertices desde 'desde'.
// Devuelve false si no hay posiciones o no entra en el anillo
static bool SetAttributes(int from, int until) {
    if (!aPos.on || !aPos.ptr) return false;
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    float esc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const Arr* arr[4] = { &aPos, &aNrm, &aUV, &aCol };
    for (int i = 0; i < 4; i++) {
        const Arr& a = *arr[i];
        if (a.on && a.ptr) {
            int step = 0, comps = 0;
            const void* p = CopyArray(a, from, until, step, comps);
            if (!p) return false;
            const int id = AttrInfo_AddLoader(ai, i, FormatGPU(a.type), comps);
            BufInfo_Add(bi, p, step, 1, (u64)id);
            if (i == 1 && a.type == ByteType) esc[1] = 1.0f / 127.0f;
            if (i == 3 && a.type == TypeUByte) esc[3] = 1.0f / 255.0f;
        } else {
            AttrInfo_AddFixed(ai, i);
            if (i == 1) SetFixed(i, 0.0f, 0.0f, 1.0f, 0.0f);
            else if (i == 2) SetFixed(i, 0.0f, 0.0f, 0.0f, 1.0f);
            else if (i == 3) SetFixed(i, curColor[0], curColor[1], curColor[2], curColor[3]);
        }
    }
    // la UV de la unidad 1: la propia (2 floats) o la misma de la unidad 0
    if (mixOn && mixTex && (mixUV || (aUV.on && aUV.ptr))) {
        Arr u1 = mixUV ? Arr{ true, 2, FloatType, 0, mixUV } : aUV;
        int step = 0, comps = 0;
        const void* p = CopyArray(u1, from, until, step, comps);
        if (!p) return false;
        const int id = AttrInfo_AddLoader(ai, 4, FormatGPU(u1.type), comps);
        BufInfo_Add(bi, p, step, 1, (u64)id);
    } else {
        AttrInfo_AddFixed(ai, 4);
        SetFixed(4, 0.0f, 0.0f, 0.0f, 1.0f);
    }
    SetScale(esc[0], esc[1], esc[2], esc[3]);
    return true;
}

// ============================================================================
//  Lineas y puntos: la PICA no los tiene. Se arman como triangulos en la CPU, en el espacio de recorte de GL (con el
//  ancho en pixeles de pantalla), y se dibujan con la matriz de ajuste sola
// ============================================================================
struct VertClip { float x, y, z, w, u, v; unsigned char c[4]; };
static void TransformToClip(int i, const float* PM, float* o) {
    const Arr& a = aPos;
    const int b = Bytes(a.type), step = a.step ? a.step : a.size * b;
    const u8* p = (const u8*)a.ptr + (size_t)i * step;
    float x, y, z = 0.0f;
    if (a.type == ShortType) { const short* q = (const short*)p; x = q[0]; y = q[1]; if (a.size > 2) z = q[2]; }
    else { const float* q = (const float*)p; x = q[0]; y = q[1]; if (a.size > 2) z = q[2]; }
    for (int k = 0; k < 4; k++) o[k] = PM[k] * x + PM[4 + k] * y + PM[8 + k] * z + PM[12 + k];
}
static void ColorOf(int i, unsigned char* c) {
    if (aCol.on && aCol.ptr) {
        if (aCol.type == TypeUByte) { const u8* p = (const u8*)aCol.ptr + (size_t)i * (aCol.step ? aCol.step : 4); memcpy(c, p, 4); return; }
        const float* p = (const float*)((const u8*)aCol.ptr + (size_t)i * (aCol.step ? aCol.step : 16));
        for (int k = 0; k < 4; k++) c[k] = (unsigned char)(p[k] <= 0 ? 0 : p[k] >= 1 ? 255 : p[k] * 255.0f + 0.5f);
        return;
    }
    for (int k = 0; k < 4; k++) c[k] = (unsigned char)(curColor[k] <= 0 ? 0 : curColor[k] >= 1 ? 255 : curColor[k] * 255.0f + 0.5f);
}
// ============================================================================
//  EL LOTE: la UI son muchos draws chiquitos (un rectangulo, un icono, un texto) y a la PICA cada draw le cuesta lo
//  mismo tenga 6 vertices o 600. Lo armado en la CPU (lineas, puntos, el clear) y los draws chicos sin luz (que se
//  pasan al espacio de recorte en la CPU) se juntan: los seguidos con el mismo estado salen a la GPU en un solo draw.
//  El estado del lote se pone en citro3d cuando el lote empieza y queda ahi hasta que sale: lo que cambie algo que
//  el lote no lleva adentro (el viewport, el scissor, la pantalla, una textura, un draw que no entra al lote) lo
//  vacia antes
// ============================================================================
static const int kBatchMaxVerts = 96;       // un draw con mas vertices va directo (la GPU los transforma mas rapido)
static const int kBatchLimit = 8192;        // (lo que no entra sale en el lote siguiente)
static VertClip* gBatch = NULL;             // (memoria comun: al anillo se copia cuando el lote sale)
static int gBatchN = 0;
static StateDraw gBatchState;
static std::vector<VertClip> gBuilt;   // (donde se arman las lineas, los puntos y el clear)
// (el vigilante: este draw, con el estado que quedo puesto. Lo armado en la CPU dice hasta donde llega en la pantalla;
// la interfaz, con w = 1, sin dividir)
static void Register(char type, int n, const VertClip* v) {
    gRegTotal++;
    if (gRegN >= kRegMax) return;
    RegistryDraw& r = gReg[gRegN++];
    r.type = type; r.n = n; r.mix = gPlaced.mix; r.zFunc = (int)gPlaced.zFunc; r.combiner = gPlaced.combiner;
    r.maxNdc = -1.0f; r.minW = -1.0f;
    if (!v) return;
    float m = 0.0f, mw = 1e30f;
    for (int i = 0; i < n; i++) {
        const float w = v[i].w;
        float ax = fabsf(v[i].x), ay = fabsf(v[i].y);
        if (w != 1.0f) { const float inv = (w > 0.0f) ? 1.0f / w : 0.0f; ax *= inv; ay *= inv; }
        if (ax > m) m = ax;
        if (ay > m) m = ay;
        if (w < mw) mw = w;
    }
    r.maxNdc = m; r.minW = mw;
}
// n vertices armados en la CPU a la GPU, en un draw propio con el estado que ya esta puesto
static void DrawClip(const VertClip* v, int n) {
    VertClip* d = (VertClip*)RingRequest((u32)(n * sizeof(VertClip)));
    if (!d) return;
    memcpy(d, v, (size_t)n * sizeof(VertClip));
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);          // posicion (recorte)
    AttrInfo_AddFixed(ai, 1);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);          // UV
    AttrInfo_AddLoader(ai, 3, GPU_UNSIGNED_BYTE, 4);  // color
    AttrInfo_AddFixed(ai, 4);
    SetFixed(1, 0.0f, 0.0f, 1.0f, 0.0f);
    SetFixed(4, 0.0f, 0.0f, 0.0f, 1.0f);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, d, sizeof(VertClip), 3, 0x320);  // (los atributos 0, 2 y 3 entrelazados)
    SetScale(1.0f, 1.0f, 1.0f, 1.0f / 255.0f);
    C3D_DrawArrays(GPU_TRIANGLES, 0, n);
    Register('R', n, v);
    g_statDrawTris++; g_statDcCat[gStatCat]++;
}
static void BatchEmpty() {
    if (gBatchN <= 0) return;
    DrawClip(gBatch, gBatchN);
    gBatchN = 0;
}
// lugar en el lote para n vertices con el estado actual (si el estado cambio, el lote anterior sale antes). NULL si
// son demasiados para el lote: el que llama los dibuja aparte
static VertClip* BatchPlace(int n) {
    if (!gBatch) gBatch = (VertClip*)malloc(sizeof(VertClip) * kBatchLimit);
    StateDraw e;
    ComputeState(true, e);
    if (gBatchN == 0 || memcmp(&e, &gBatchState, sizeof(e)) != 0 || gBatchN + n > kBatchLimit) {
        BatchEmpty();
        ApplyState(e, true);
        gBatchState = e;
    }
    g_statIndices += n;
    if (!gBatch || n > kBatchLimit) return NULL;
    VertClip* p = gBatch + gBatchN;
    gBatchN += n;
    return p;
}
static void DrawBuilt(const VertClip* v, int n) {
    if (n <= 0 || !gInFrame) return;
    VertClip* d = BatchPlace(n);
    if (d) memcpy(d, v, (size_t)n * sizeof(VertClip));
    else DrawClip(v, n);   // (el estado ya quedo puesto)
}
// un draw de triangulos va al lote si es chico, sin luz ni culling (en el recorte no hay) y con lo que el lote lleva:
// una sola textura con UV de dos float
static bool Loteable(GPU_Primitive_t prim, int n) {
    if (prim != GPU_TRIANGLES || n > kBatchMaxVerts) return false;
    if (cap_light || cap_cull || cap_fog || (mixOn && mixTex)) return false;
    if (aPos.type != FloatType && aPos.type != ShortType) return false;
    if (aUV.on && aUV.ptr && (aUV.type != FloatType || aUV.size != 2)) return false;
    return true;
}
// los vertices del draw (los indices, o desde 'primero') al espacio de recorte, al lote. Es la cuenta del vertex
// shader hecha en la CPU: tiene que ser barata (la UI en 2D: x, y sin z; el color casi siempre fijo)
template <typename T> static void ToBatch(const T* idx, int first, int n) {
    VertClip* o = BatchPlace(n);
    if (!o) return;
    float PM[16];
    Matrix4 m = curProj * curMV;
    memcpy(PM, m.m, sizeof(PM));
    const u8* pos = (const u8*)aPos.ptr;
    const int stepPos = aPos.step ? aPos.step : aPos.size * Bytes(aPos.type);
    const bool pos2f = aPos.type == FloatType && aPos.size == 2;
    const u8* uv = (aUV.on && aUV.ptr) ? (const u8*)aUV.ptr : NULL;
    const int stepUV = aUV.step ? aUV.step : 8;
    unsigned char cFixed[4];
    const bool fixedColor = !(aCol.on && aCol.ptr);
    if (fixedColor) ColorOf(0, cFixed);
    for (int j = 0; j < n; j++, o++) {
        const int i = idx ? (int)idx[j] : first + j;
        if (pos2f) {
            const float* q = (const float*)(pos + (size_t)i * stepPos);
            const float x = q[0], y = q[1];
            o->x = PM[0] * x + PM[4] * y + PM[12];
            o->y = PM[1] * x + PM[5] * y + PM[13];
            o->z = PM[2] * x + PM[6] * y + PM[14];
            o->w = PM[3] * x + PM[7] * y + PM[15];
        } else {
            float p[4];
            TransformToClip(i, PM, p);
            o->x = p[0]; o->y = p[1]; o->z = p[2]; o->w = p[3];
        }
        if (uv) { const float* t = (const float*)(uv + (size_t)i * stepUV); o->u = t[0]; o->v = t[1]; }
        else { o->u = 0.0f; o->v = 0.0f; }
        if (fixedColor) memcpy(o->c, cFixed, 4);
        else ColorOf(i, o->c);
    }
}
static void Corner(std::vector<VertClip>& v, const float* p, float dx, float dy, float u, float t, const unsigned char* c) {
    VertClip q; q.x = p[0] + dx * p[3]; q.y = p[1] + dy * p[3]; q.z = p[2]; q.w = p[3]; q.u = u; q.v = t; memcpy(q.c, c, 4);
    v.push_back(q);
}
static void Crop(float* a, const float* b) {   // la punta detras del plano cercano, al plano (z + w >= 0)
    const float da = a[2] + a[3], db = b[2] + b[3], t = da / (da - db);
    for (int k = 0; k < 4; k++) a[k] = a[k] + (b[k] - a[k]) * t;
}
// EL MARGEN: lo armado en la CPU no pasa de 2 veces la pantalla para cada lado. La PICA recorta solo contra el plano
// cercano: una linea que lo cruza (la grilla debajo de la camara, con el plano cercano a 0,01) llegaba con la punta a
// miles de pantallas de distancia (|x/w| de mas de 3000: cientos de miles de pixeles). Afuera del margen no se ve nada
static const float kBand = 2.0f;
// el tramo [a, b] (ya delante del plano cercano) recortado a |x| <= kBand w, |y| <= kBand w. false si queda afuera
static bool CropBand(float* a, float* b) {
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 4; k++) {
        const int axis = k >> 1;
        const float s  = (k & 1) ? -1.0f : 1.0f;   // (x <= kBand w, x >= -kBand w, lo mismo con y)
        const float da = kBand * a[3] - s * a[axis], db = kBand * b[3] - s * b[axis];
        if (da < 0.0f && db < 0.0f) return false;
        if (da < 0.0f) { const float t = da / (da - db); if (t > t0) t0 = t; }
        else if (db < 0.0f) { const float t = da / (da - db); if (t < t1) t1 = t; }
    }
    if (t0 > t1) return false;
    if (t0 > 0.0f || t1 < 1.0f) {
        float na[4], nb[4];
        for (int k = 0; k < 4; k++) { na[k] = a[k] + (b[k] - a[k]) * t0; nb[k] = a[k] + (b[k] - a[k]) * t1; }
        memcpy(a, na, sizeof(na)); memcpy(b, nb, sizeof(nb));
    }
    return true;
}
static void BuildLines(const int* idx, int n, bool strip, std::vector<VertClip>& v) {
    float PM[16]; Matrix4 m = curProj * curMV; memcpy(PM, m.m, sizeof(PM));
    const float hx = (lineWidth > 1.0f ? lineWidth : 1.0f) / (float)(vpCurrent[2] > 0 ? vpCurrent[2] : 320);
    const float hy = (lineWidth > 1.0f ? lineWidth : 1.0f) / (float)(vpCurrent[3] > 0 ? vpCurrent[3] : 240);
    const int step = strip ? 1 : 2;
    for (int i = 0; i + 1 < n; i += step) {
        float a[4], b[4]; unsigned char ca[4], cb[4];
        TransformToClip(idx[i], PM, a); TransformToClip(idx[i + 1], PM, b);
        ColorOf(idx[i], ca); ColorOf(idx[i + 1], cb);
        const bool fa = a[2] < -a[3], fb = b[2] < -b[3];
        if (fa && fb) continue;
        if (fa) Crop(a, b); else if (fb) Crop(b, a);
        if (!CropBand(a, b)) continue;
        float dx = b[0] / b[3] - a[0] / a[3], dy = b[1] / b[3] - a[1] / a[3];
        // (la direccion en pixeles, para que el ancho sea parejo en las dos direcciones)
        float px = dx / hx, py = dy / hy, l = sqrtf(px * px + py * py);
        if (l < 1e-6f) continue;
        const float nx = -py / l * hx, ny = px / l * hy;
        Corner(v, a, nx, ny, 0, 0, ca); Corner(v, b, nx, ny, 1, 0, cb); Corner(v, a, -nx, -ny, 0, 1, ca);
        Corner(v, a, -nx, -ny, 0, 1, ca); Corner(v, b, nx, ny, 1, 0, cb); Corner(v, b, -nx, -ny, 1, 1, cb);
    }
}
static void BuildPoints(const int* idx, int n, std::vector<VertClip>& v) {
    float PM[16]; Matrix4 m = curProj * curMV; memcpy(PM, m.m, sizeof(PM));
    const float hx = pointSize / (float)(vpCurrent[2] > 0 ? vpCurrent[2] : 320);
    const float hy = pointSize / (float)(vpCurrent[3] > 0 ? vpCurrent[3] : 240);
    for (int j = 0; j < n; j++) {
        const int i = idx ? idx[j] : j;
        float p[4]; unsigned char c[4];
        TransformToClip(i, PM, p);
        if (p[3] <= 0.0f || p[2] < -p[3]) continue;
        if (fabsf(p[0]) > kBand * p[3] || fabsf(p[1]) > kBand * p[3]) continue;   // (afuera del margen: no se ve)
        ColorOf(i, c);
        // (UV de 0 a 1 en las esquinas: los iconos son point sprites; la v de GL va de abajo a arriba)
        Corner(v, p, -hx, hy, 0, 1, c); Corner(v, p, -hx, -hy, 0, 0, c); Corner(v, p, hx, -hy, 1, 0, c);
        Corner(v, p, -hx, hy, 0, 1, c); Corner(v, p, hx, -hy, 1, 0, c); Corner(v, p, hx, hy, 1, 1, c);
    }
}

// ============================================================================
//  Los draws
// ============================================================================
// (la medicion de la prueba de rendimiento: solo el draw de afuera cuenta, los que llama adentro ya estan incluidos)
static bool gMeasure = false;
static int  gMeasureProf = 0;
static u64  gMeasureTicks = 0;
struct Stopwatch {
    u64 t0;
    Stopwatch() : t0(0) { if (gMeasure && gMeasureProf++ == 0) t0 = svcGetSystemTick(); }
    ~Stopwatch() { if (gMeasure && --gMeasureProf == 0) gMeasureTicks += svcGetSystemTick() - t0; }
};
void   C3DMeasure(bool on) { gMeasure = on; gMeasureProf = 0; gMeasureTicks = 0; }
double C3DTimeDrawsMs() { const double ms = (double)gMeasureTicks / CPU_TICKS_PER_MSEC; gMeasureTicks = 0; return ms; }

static void DrawArray(GPU_Primitive_t prim, int first, int n) {
    Stopwatch timer;
    if (!gInFrame || n <= 0) return;
    if (Loteable(prim, n)) { ToBatch((const int*)NULL, first, n); return; }
    BatchEmpty();
    SetState(false);
    if (!SetAttributes(first, first + n)) return;
    C3D_DrawArrays(prim, 0, n);
    Register('A', n, NULL);
    g_statDrawTris++; g_statIndices += n; g_statDcCat[gStatCat]++;
    if (cap_blend) g_statTrisBlend += n / 3; else g_opaqueStatTris += n / 3;
}
template <typename T> static void DrawIndices(GPU_Primitive_t prim, int n, const T* idx) {
    Stopwatch timer;
    if (!gInFrame || n <= 0 || !idx) return;
    if (Loteable(prim, n)) { ToBatch(idx, 0, n); return; }
    BatchEmpty();
    int lo = 0x7fffffff, hi = -1;
    for (int i = 0; i < n; i++) { const int k = (int)idx[i]; if (k < lo) lo = k; if (k > hi) hi = k; }
    if (hi < 0) return;
    u16* d = (u16*)RingRequest((u32)(n * 2));
    if (!d) return;
    for (int i = 0; i < n; i++) d[i] = (u16)((int)idx[i] - lo);
    SetState(false);
    if (!SetAttributes(lo, hi + 1)) return;
    C3D_DrawElements(prim, n, C3D_UNSIGNED_SHORT, d);   // (los indices, relativos a lo)
    Register('I', n, NULL);
    g_statDrawTris++; g_statIndices += n; g_statDcCat[gStatCat]++;
    if (cap_blend) g_statTrisBlend += n / 3; else g_opaqueStatTris += n / 3;
}
static std::vector<int> gIndices;   // (los indices de lineas y puntos, a int)
template <typename T> static void IndexedLines(int n, const T* idx, bool strip) {
    Stopwatch timer;
    if (!gInFrame || n < 2 || !idx || !aPos.on || !aPos.ptr) return;
    gIndices.resize((size_t)n);
    for (int i = 0; i < n; i++) gIndices[i] = (int)idx[i];
    gBuilt.clear();
    BuildLines(&gIndices[0], n, strip, gBuilt);
    if (!gBuilt.empty()) DrawBuilt(&gBuilt[0], (int)gBuilt.size());
}

void DrawTriangles(int count, const MeshIndex* indices) { DrawIndices(GPU_TRIANGLES, count, indices); }
void DrawTrianglesClientIdx(int count, const MeshIndex* indices) { DrawIndices(GPU_TRIANGLES, count, indices); }
void DrawLinesClientIdx(int count, const MeshIndex* indices) { IndexedLines(count, indices, false); }
void DrawTrianglesByte(int count, const unsigned char* indices) { DrawIndices(GPU_TRIANGLES, count, indices); }
void DrawTrianglesArray(int n) { DrawArray(GPU_TRIANGLES, 0, n); }
void DrawTrianglesArrayFrom(int first, int n) { DrawArray(GPU_TRIANGLES, first, n); }
void DrawLinesIndexed(int count, const unsigned short* indices) { IndexedLines(count, indices, false); }
void DrawLineStripIndexed(int count, const unsigned short* indices) { IndexedLines(count, indices, true); }
static void LinesArray(int n, bool strip) {
    Stopwatch timer;
    if (!gInFrame || n < 2 || !aPos.on || !aPos.ptr) return;
    gIndices.resize((size_t)n);
    for (int i = 0; i < n; i++) gIndices[i] = i;
    gBuilt.clear();
    BuildLines(&gIndices[0], n, strip, gBuilt);
    if (!gBuilt.empty()) DrawBuilt(&gBuilt[0], (int)gBuilt.size());
}
void DrawLines(int n) { LinesArray(n, false); }
void DrawLineStrip(int n) { LinesArray(n, true); }
void DrawPoints(int n) {
    Stopwatch timer;
    if (!gInFrame || n <= 0 || !aPos.on || !aPos.ptr) return;
    gBuilt.clear();
    BuildPoints(NULL, n, gBuilt);
    if (!gBuilt.empty()) DrawBuilt(&gBuilt[0], (int)gBuilt.size());
}
template <typename T> static void Elements(Primitive p, int count, const T* indices) {
    Stopwatch timer;
    switch (p) {
        case PrimPoints: {
            if (!gInFrame || count <= 0 || !aPos.on || !aPos.ptr) return;
            gIndices.resize((size_t)count);
            for (int i = 0; i < count; i++) gIndices[i] = (int)indices[i];
            gBuilt.clear();
            BuildPoints(&gIndices[0], count, gBuilt);
            if (!gBuilt.empty()) DrawBuilt(&gBuilt[0], (int)gBuilt.size());
            return;
        }
        case PrimLines: IndexedLines(count, indices, false); return;
        case PrimStripLines: IndexedLines(count, indices, true); return;
        case PrimLoopLines: {
            std::vector<T> l(indices, indices + count);
            if (count > 0) l.push_back(indices[0]);
            IndexedLines((int)l.size(), &l[0], true);
            return;
        }
        case PrimStripTriangles: DrawIndices(GPU_TRIANGLE_STRIP, count, indices); return;
        case PrimFan: DrawIndices(GPU_TRIANGLE_FAN, count, indices); return;
        default: DrawIndices(GPU_TRIANGLES, count, indices); return;
    }
}
void DrawElements(Primitive p, int count, const MeshIndex* indices) { Elements(p, count, indices); }
void DrawElements16(Primitive p, int count, const unsigned short* indices) { Elements(p, count, indices); }
void DrawElementsVBO(Primitive, int, int) {}

// EL CLEAR: el de citro3d borra la pantalla entera (y en el momento, no en orden con lo dibujado). GL respeta el
// scissor: es un rectangulo con z ALWAYS, del color de fondo y en la z de lo mas lejano
static void ClearWithRect(int bits) {
    Stopwatch timer;
    if (!gInFrame || !bits) return;
    const int W = C3DScreenWidth(gScreen);
    int r[4] = { 0, 0, W, 240 };
    if (cap_scissor) for (int i = 0; i < 4; i++) r[i] = scCurrent[i];
    // el rectangulo en NDC del viewport de la pantalla entera
    const int vp0[4] = { vpCurrent[0], vpCurrent[1], vpCurrent[2], vpCurrent[3] };
    vpCurrent[0] = 0; vpCurrent[1] = 0; vpCurrent[2] = W; vpCurrent[3] = 240; SetViewport();
    const float x0 = r[0] * 2.0f / W - 1.0f, x1 = (r[0] + r[2]) * 2.0f / W - 1.0f;
    const float y0 = r[1] * 2.0f / 240.0f - 1.0f, y1 = (r[1] + r[3]) * 2.0f / 240.0f - 1.0f;
    unsigned char c[4];
    for (int k = 0; k < 4; k++) c[k] = (unsigned char)(clearCol[k] <= 0 ? 0 : clearCol[k] >= 1 ? 255 : clearCol[k] * 255.0f + 0.5f);
    std::vector<VertClip>& v = gBuilt;
    v.clear();
    const float p00[4] = { x0, y0, 1.0f, 1.0f }, p10[4] = { x1, y0, 1.0f, 1.0f }, p01[4] = { x0, y1, 1.0f, 1.0f }, p11[4] = { x1, y1, 1.0f, 1.0f };
    Corner(v, p00, 0, 0, 0, 0, c); Corner(v, p10, 0, 0, 0, 0, c); Corner(v, p11, 0, 0, 0, 0, c);
    Corner(v, p00, 0, 0, 0, 0, c); Corner(v, p11, 0, 0, 0, 0, c); Corner(v, p01, 0, 0, 0, 0, c);
    // estado del clear: sin textura, sin luz, sin mezcla, z ALWAYS
    const bool t = cap_tex, d = cap_depth, b = cap_blend, l = cap_light, cm = colorMask, dm = depthMask, sc = cap_scissor;
    const int df = depthFunc, is = stencil; const float ar = alphaRef;
    cap_tex = false; cap_blend = false; cap_light = false; cap_depth = true; depthFunc = DepthAlways; alphaRef = 0.0f;
    colorMask = (bits & ColorBuffer) != 0; depthMask = (bits & DepthBuffer) != 0;
    if (bits & StencilBuffer) { stencil = 1; } else stencil = 0;
    DrawBuilt(&v[0], (int)v.size());
    cap_tex = t; cap_depth = d; cap_blend = b; cap_light = l; colorMask = cm; depthMask = dm; cap_scissor = sc;
    depthFunc = df; stencil = is; alphaRef = ar;
    for (int i = 0; i < 4; i++) vpCurrent[i] = vp0[i];
    SetViewport();
}

// ============================================================================
//  LA SELECCION POR COLOR (Pick3D): ReadPixelsBegin abre una pasada aparte, en una superficie del tamano de la
//  pantalla de abajo que no se ve; ReadPixelsRGBA la manda a la GPU, la copia de la VRAM a memoria lineal con el motor
//  de transferencias (el mismo que lleva cada cuadro a la pantalla) y lee ahi esos pixeles. La CPU no lee la VRAM
//  directo: en el emulador andaba, en la consola de verdad no es seguro. Si habia un cuadro a medio dibujar, sigue
//  despues como estaba. Fuera de una pasada abierta la lectura da negro (la pantalla real no se lee)
// ============================================================================
static C3D_RenderTarget* gTargetSel = NULL;
static bool gActiveSel = false, gSelInFrame = false;
static int  gSelScreen = ScreenDown;
static void ForgetVpSc() { for (int i = 0; i < 4; i++) gVpPlaced[i] = -1; for (int i = 0; i < 5; i++) gPlacedSc[i] = -1; }
void ReadPixelsBegin(int, int, int, int) {
    if (gActiveSel) return;
    if (!gTargetSel) gTargetSel = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!gTargetSel) return;
    gSelInFrame = gInFrame;
    if (gInFrame) BatchEmpty();
    else {
        BeginWithWait(0);   // (espera a que la GPU termine el cuadro anterior: su anillo queda libre)
        gInFrame = true;
    }
    gActiveSel = true;
    gSelScreen = gScreen;
    gScreen = ScreenDown;   // (las cuentas del viewport con el ancho de la de abajo: la superficie es de su tamano)
    C3D_FrameDrawOn(gTargetSel);
    ForgetVpSc();
    SetViewport();
    SetScissor();
}
bool CopyScreenFlipped() { return false; }   // (las capturas de los juegos todavia no: ver w3dGLc3d.cpp)
int CopyScreenRow(int y, int) { return y; }
static u8* gSelCopy = NULL;   // (la superficie copiada a memoria lineal: 240 x 320 RGBA8, sin bloques)
void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* p) {
    if (!p || w <= 0 || h <= 0) return;
    if (!gActiveSel) { memset(p, 0, (size_t)w * h * 4); return; }
    BatchEmpty();
    Ring& a = gRing[gRingAct];
    if (a.used) GSPGPU_FlushDataCache(a.base, a.used);
    // las pantallas que ya tenian algo de este cuadro no salen todavia (citro3d las manda al terminar el cuadro)
    const bool used0 = gTarget[0] && gTarget[0]->used, used1 = gTarget[1] && gTarget[1]->used;
    if (gTarget[0]) gTarget[0]->used = false;
    if (gTarget[1]) gTarget[1]->used = false;
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    // esperar a que la GPU termine (con el vigilante) y salir del cuadro: la copia sincronica va fuera de un cuadro
    BeginWithWait(0);
    C3D_FrameEnd(GX_CMDLIST_FLUSH);   // (vacio: no dibuja ni manda nada a las pantallas)
    const u32 bytes = 240u * 320u * 4u;
    if (!gSelCopy) gSelCopy = (u8*)linearAlloc(bytes);
    if (gSelCopy) {
        // de bloques a lineal, como la salida a la pantalla: la fila gx de la copia es la columna gx de la pantalla
        // y adentro, de abajo hacia arriba
        C3D_SyncDisplayTransfer((u32*)gTargetSel->frameBuf.colorBuf, GX_BUFFER_DIM(240, 320), (u32*)gSelCopy,
                                GX_BUFFER_DIM(240, 320), DISPLAY_TRANSFER_FLAGS);
        GSPGPU_InvalidateDataCache(gSelCopy, bytes);   // (lo que la CPU tuviera en cache de la copia anterior)
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            unsigned char* o = p + ((size_t)j * w + i) * 4;
            const int gx = x + i, gy = y + j;
            if (!gSelCopy || gx < 0 || gx >= 320 || gy < 0 || gy >= 240) { o[0] = o[1] = o[2] = 0; o[3] = 255; continue; }
            const u8* q = gSelCopy + ((size_t)gx * 240 + (size_t)gy) * 4;   // (RGBA8 guardado como A, B, G, R)
            o[0] = q[3]; o[1] = q[2]; o[2] = q[1]; o[3] = q[0];
        }
    gActiveSel = false;
    gScreen = gSelScreen;
    if (gSelInFrame) {
        // el cuadro que se estaba dibujando sigue (sus pantallas, con lo que ya tenian, salen al terminarlo)
        BeginWithWait(0);
        if (gTarget[0]) gTarget[0]->used = used0;
        if (gTarget[1]) gTarget[1]->used = used1;
        C3D_FrameDrawOn(gTarget[gScreen]);
        ForgetVpSc();
        SetViewport();
        SetScissor();
    } else gInFrame = false;
}

// ============================================================================
//  Buffer objects: no (las mallas van por el camino de arreglos de cliente)
// ============================================================================
bool VBOSupported() { return false; }
unsigned int GenBuffer() { return 0; }
void DeleteBuffer(unsigned int) {}
void ArrayBufferData(unsigned int, const void*, int) {}
void IndexBufferData(unsigned int, const void*, int) {}
long VBOMemory() { return 0; }
void VertexVBO(unsigned int) {}
void NormalVBO(unsigned int) {}
void ColorVBO(unsigned int) {}
void TexCoordVBO(unsigned int) {}
void TexCoordVBO2s(unsigned int) {}
void VertexVBO3s(unsigned int) {}
void BindIndexVBO(unsigned int) {}
void DrawTrianglesVBO(int, int) {}
void UnbindVBOs() {}

// ============================================================================
//  Estadisticas (como el backend de GLES2)
// ============================================================================
int g_statMeshes = 0;
int g_statDrawTris = 0, g_statDrawVBO = 0, g_statIndices = 0, g_statTexBinds = 0, g_statStateChanges = 0, g_otherStat = 0;
int g_statUploadBytes = 0, g_opaqueStatTris = 0, g_statTrisBlend = 0;
int g_statDcCat[StatCatCount_] = { 0 };
void StatCategory(StatCat c) { gStatCat = (int)c; }
StatCat StatCurrentCategory() { return (StatCat)gStatCat; }
void StatRaw(int n) { g_statCalls[gStatCat] += n; }
FrameStats g_statFrame;
bool g_measureDraws = false;   // (solo el backend de GLES2 mide los draws)
static int gFrameDraws0 = 0, gFrameIdx0 = 0;
void StatsFrame() {
    FrameStats& c = g_statFrame;
    c.total = 0;
    for (int i = 0; i < StatCatCount_; i++) { c.calls[i] = g_statCalls[i]; c.total += g_statCalls[i]; g_statCalls[i] = 0; }
    c.draws = g_statDrawTris - gFrameDraws0; gFrameDraws0 = g_statDrawTris;
    c.binds = 0; c.states = 0;
    c.tris = (g_statIndices - gFrameIdx0) / 3; gFrameIdx0 = g_statIndices;
    c.frames++;
    gStatCat = StatCatEditor;
}
void StatsReset() {
    g_statDrawTris = 0; g_statDrawVBO = 0; g_statIndices = 0; g_statTexBinds = 0; g_statMeshes = 0;
    g_statStateChanges = 0; g_statUploadBytes = 0; g_opaqueStatTris = 0; g_statTrisBlend = 0; g_otherStat = 0;
    for (int i = 0; i < StatCatCount_; i++) { g_statDcCat[i] = 0; g_statCalls[i] = 0; }
    gFrameDraws0 = gFrameIdx0 = 0;
    gStatCat = StatCatScene;
}

} // namespace w3dEngine
