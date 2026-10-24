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
#include "c3d/w3dC3D.h"
#include "c3d/w3dC3DInterno.h"
#include "math/Matrix4.h"
#include <vector>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "w3d_shbin.h"   // el shader ensamblado (picasso + bin2s, ver platform/3ds/Makefile)

// estado de render compartido (lo setea el editor; el Core lo LEE). Los MISMOS globales que los otros backends
bool w3dRenderWireframe = false;
bool w3dRenderWireframeSoloVisible = true;
bool w3dRenderSolido    = false;
bool w3dRenderSinLuz    = false;
bool w3dRenderLuces     = false;
bool w3dRenderNormalColor = false;
bool w3dRenderAlpha       = false;
bool w3dRenderOverlays    = true;
bool w3dVerSeleccion      = true;
bool g_xray               = false;

void W3dC3DBindTextura(unsigned int id);   // (w3dGLc3d.cpp: la textura de las funciones de GL)

namespace w3dEngine {

int g_statLlamadas[StatCatCount_] = { 0 };
static int gStatCat = StatCatEditor;
bool g_trazaGL = false;
bool g_trazaGLSec = false;
void TrazaGL(const char*) {}
void TrazaGLVer() {}

// ============================================================================
//  Estado emulado del pipeline fijo
// ============================================================================
static bool cap_depth = false, cap_cull = false, cap_tex = false, cap_light = false, cap_blend = false, cap_fog = false;
static bool cap_scissor = false, cap_colormat = false, cap_offset = false;
static int  matMode = 1;   // 0 = Projection, 1 = ModelView, 2 = Texture
static Matrix4 curProj, curMV, curTex;
static std::vector<Matrix4> stkProj, stkMV, stkTex;
struct Luz { bool on; float pos[4], amb[4], dif[4], spec[4], att[3], dir[3], corteCos, expo; };
static Luz luces[LucesMax];
static float ambGlobal[4] = { 0.2f, 0.2f, 0.2f, 1 };
static bool  lucesSucias = true;
static float matDiff[4] = { 0.8f, 0.8f, 0.8f, 1 }, matAmb[4] = { 0.2f, 0.2f, 0.2f, 1 };
static float matSpec[4] = { 0, 0, 0, 1 }, matEmis[4] = { 0, 0, 0, 1 };
static float matShine = 0.0f;
static bool  materialSucio = true;
static float curColor[4] = { 1, 1, 1, 1 };
static float fogColor[4] = { 0, 0, 0, 1 };
static float fogStart = 0, fogEnd = 1, fogDens = 1.0f;
static int   fogModo = 0;
static int   texEnv = 0;            // 0 modulate, 1 replace, 2 DOT3, 3 solo alfa
static int   texGenCache = -1, matcapCache = -1;
static float uvMat[4] = { 0, 0, 1, 1 };
static bool  pointSprite = false;
static float pointSize = 1.0f, lineWidth = 1.0f;
static int   vpActual[4] = { 0, 0, 320, 240 }, scActual[4] = { 0, 0, 320, 240 };
static unsigned boundTex = 0;
static bool     mezclaOn = false;
static unsigned mezclaTex = 0;
static int      mezclaModo = 0;
static float    mezclaK = 1.0f;
static const float* mezclaUV = 0;
static int   depthFunc = (int)DepthLess;
static bool  depthMask = true, colorMask = true;
static bool  frenteCCW = true, cullFrente = false;
static float alphaRef = 0.0f;
static float polyF = 0.0f, polyU = 0.0f;
static float clearCol[4] = { 0, 0, 0, 1 };
static int   mezclaFirma = -1;      // la funcion de mezcla puesta (SetMezcla / BlendAlpha / BlendMode)
static int   estencil = 0, estencilValor = 0;   // 0 apagado, 1 marcar, 2 dentro
// los arreglos de cliente
enum { TipoFloat = 0, TipoShort, TipoByte, TipoUByte };
struct Arr { bool on; int tam; int tipo; int paso; const void* ptr; };
static Arr aPos = { false, 3, TipoFloat, 0, 0 }, aNrm = { false, 3, TipoByte, 0, 0 };
static Arr aUV = { false, 2, TipoFloat, 0, 0 }, aCol = { false, 4, TipoUByte, 0, 0 };

// ============================================================================
//  citro3d: pantallas, shader, anillo de memoria lineal
// ============================================================================
static C3D_RenderTarget* gTarget[2] = { NULL, NULL };
static bool gLimpia[2] = { false, false };   // ya se limpio en este cuadro
static int  gPantalla = PantallaAbajo;
static bool gEnCuadro = false;
static DVLB_s* gDvlb = NULL;
static shaderProgram_s gProg;
static int uMvp = -1, uMv = -1, uNmat = -1, uTexm = -1, uEscala = -1;
static C3D_LightEnv gLuzEnv;
static C3D_Light    gLuz[LucesMax];
static C3D_LightLut gLutEsp;
static float        gLutEspBrillo = -1.0f;
static bool         gLuzEnvPuesto = false;
struct Anillo { u8* base; u32 tam, usado; };
static Anillo gAnillo[2];
static int    gAnilloAct = 0;
static bool   gAnilloAvisado = false;
static const u32 kAnilloBytes = 3u * 1024u * 1024u;

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static void* AnilloPedir(u32 bytes) {
    Anillo& a = gAnillo[gAnilloAct];
    bytes = (bytes + 15u) & ~15u;
    if (!a.base || a.usado + bytes > a.tam) {
        if (!gAnilloAvisado) { gAnilloAvisado = true; printf("[c3d] el anillo de vertices se lleno (%u KB por cuadro)\n", (unsigned)(a.tam / 1024)); }
        return NULL;
    }
    void* p = a.base + a.usado;
    a.usado += bytes;
    return p;
}

int  C3DAnchoPantalla(int p) { return p == PantallaArriba ? 400 : 320; }
bool C3DEnCuadro() { return gEnCuadro; }

bool C3DInit() {
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    w3dLogf("[c3d] %s", n3ds ? "New 3DS" : "3DS (la vieja)");
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) return false;
    gTarget[PantallaArriba] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    gTarget[PantallaAbajo]  = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!gTarget[0] || !gTarget[1]) return false;
    C3D_RenderTargetSetOutput(gTarget[PantallaArriba], GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(gTarget[PantallaAbajo], GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    gDvlb = DVLB_ParseFile((u32*)w3d_shbin, w3d_shbin_size);
    shaderProgramInit(&gProg);
    shaderProgramSetVsh(&gProg, &gDvlb->DVLE[0]);
    C3D_BindProgram(&gProg);
    uMvp    = shaderInstanceGetUniformLocation(gProg.vertexShader, "mvp");
    uMv     = shaderInstanceGetUniformLocation(gProg.vertexShader, "mv");
    uNmat   = shaderInstanceGetUniformLocation(gProg.vertexShader, "nmat");
    uTexm   = shaderInstanceGetUniformLocation(gProg.vertexShader, "texm");
    uEscala = shaderInstanceGetUniformLocation(gProg.vertexShader, "escala");
    for (int i = 0; i < 2; i++) {
        gAnillo[i].base = (u8*)linearAlloc(kAnilloBytes);
        gAnillo[i].tam = gAnillo[i].base ? kAnilloBytes : 0;
        gAnillo[i].usado = 0;
    }
    for (int i = 0; i < 6; i++) { C3D_TexEnv* e = C3D_GetTexEnv(i); C3D_TexEnvInit(e); }
    C3D_LightEnvInit(&gLuzEnv);
    for (int i = 0; i < LucesMax; i++) C3D_LightInit(&gLuz[i], &gLuzEnv);
    curProj.Identity(); curMV.Identity(); curTex.Identity();
    for (int i = 0; i < LucesMax; i++) {
        Luz& L = luces[i];
        L.on = false; L.pos[0] = 0; L.pos[1] = 0; L.pos[2] = 1; L.pos[3] = 0;
        for (int k = 0; k < 4; k++) { L.amb[k] = (k == 3) ? 1.0f : 0.0f; L.dif[k] = (i == 0 || k == 3) ? 1.0f : 0.0f; L.spec[k] = L.dif[k]; }
        L.att[0] = 1; L.att[1] = 0; L.att[2] = 0; L.dir[0] = 0; L.dir[1] = 0; L.dir[2] = -1; L.corteCos = -1.0f; L.expo = 0.0f;
    }
    return true;
}
void C3DFin() {
    for (int i = 0; i < 2; i++) if (gAnillo[i].base) { linearFree(gAnillo[i].base); gAnillo[i].base = NULL; }
    if (gDvlb) { shaderProgramFree(&gProg); DVLB_Free(gDvlb); gDvlb = NULL; }
    C3D_Fini();
}

// ============================================================================
//  EL VIGILANTE DE LA GPU: en la consola de verdad, un cuadro que la GPU no puede terminar la deja colgada (y a la
//  consola con ella); el emulador lo dibuja igual. Se anotan los draws de cada cuadro (que, cuantos vertices, con que
//  estado y, lo armado en la CPU, hasta donde llega en la pantalla). Si la GPU no termina un cuadro en 2 segundos, la
//  lista del cuadro colgado va al log (que en la 3DS queda en la SD) y se sigue esperando
// ============================================================================
struct RegistroDraw { char tipo; int n, mezcla, zFunc; unsigned combiner; float maxNdc, minW; };
enum { kRegMax = 128 };
static RegistroDraw gReg[kRegMax];
static int gRegN = 0, gRegTotal = 0;
static void ReportarGPUColgada() {
    w3dLogfE("[c3d] la GPU no termino el cuadro en 2 segundos (en la consola de verdad, colgada). Sus %d draws:", gRegTotal);
    for (int i = 0; i < gRegN; i++) {
        const RegistroDraw& r = gReg[i];
        if (r.maxNdc >= 0.0f)
            w3dLogfE("[c3d]   %c %d vertices, mezcla %d, z %d, combiner %x, |x/w| hasta %.1f, w desde %.4f",
                     r.tipo, r.n, r.mezcla, r.zFunc, r.combiner, r.maxNdc, r.minW);
        else w3dLogfE("[c3d]   %c %d vertices, mezcla %d, z %d, combiner %x", r.tipo, r.n, r.mezcla, r.zFunc, r.combiner);
    }
    if (gRegTotal > gRegN) w3dLogfE("[c3d]   (y %d draws mas)", gRegTotal - gRegN);
}
// C3D_FrameBegin, que espera a que la GPU termine lo anterior: si tarda mas de 2 segundos, avisa (una vez)
static void EmpezarConEspera(u8 flags) {
    if (C3D_FrameBegin(flags | C3D_FRAME_NONBLOCK)) return;
    const u64 t0 = svcGetSystemTick();
    bool avisado = false;
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        svcSleepThread(1000000LL);   // 1 ms
        if (!avisado && svcGetSystemTick() - t0 > 2000ULL * (u64)CPU_TICKS_PER_MSEC) { avisado = true; ReportarGPUColgada(); }
    }
}

static void PonerViewport();
static void PonerScissor();
static void LoteVaciar();
// (lo ultimo que se puso en citro3d del viewport y el scissor: si no cambia no se toca, y el lote sigue. C3DPantalla
// los olvida)
static int gVpPuesto[4] = { -1, -1, -1, -1 }, gScPuesto[5] = { -1, -1, -1, -1, -1 };
void C3DCuadroEmpezar() {
    if (gEnCuadro) return;
    EmpezarConEspera(C3D_FRAME_SYNCDRAW);
    gRegN = 0; gRegTotal = 0;
    gEnCuadro = true;
    gLimpia[0] = gLimpia[1] = false;
    gAnilloAct ^= 1;
    gAnillo[gAnilloAct].usado = 0;
    gAnilloAvisado = false;
    C3DPantalla(gPantalla);
}
void C3DPantalla(int p) {
    if (gEnCuadro) LoteVaciar();
    gPantalla = (p == PantallaArriba) ? PantallaArriba : PantallaAbajo;
    if (!gEnCuadro || !gTarget[gPantalla]) return;
    if (!gLimpia[gPantalla]) {
        // (la primera vez en el cuadro: el clear de citro3d borra la pantalla entera, despues van los Clear del editor)
        const u32 c = ((u32)(clearCol[0] * 255.0f) << 24) | ((u32)(clearCol[1] * 255.0f) << 16) | ((u32)(clearCol[2] * 255.0f) << 8) | 0xFFu;
        C3D_RenderTargetClear(gTarget[gPantalla], C3D_CLEAR_ALL, c, 0);
        gLimpia[gPantalla] = true;
    }
    C3D_FrameDrawOn(gTarget[gPantalla]);   // (pone el viewport de la pantalla entera y cambia el ancho)
    for (int i = 0; i < 4; i++) gVpPuesto[i] = -1;
    for (int i = 0; i < 5; i++) gScPuesto[i] = -1;
    PonerViewport();
    PonerScissor();
}
void C3DCuadroTerminar() {
    if (!gEnCuadro) return;
    LoteVaciar();
    Anillo& a = gAnillo[gAnilloAct];
    if (a.usado) GSPGPU_FlushDataCache(a.base, a.usado);   // (lo que la CPU copio tiene que estar en memoria para la GPU)
    // (GX_CMDLIST_FLUSH: sin eso citro3d vacia la cache de TODA la memoria lineal en cada cuadro; lo que la GPU lee ya
    // esta vaciado: el anillo aca y cada textura al subirla)
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    gEnCuadro = false;
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
static unsigned gVerMat[3] = { 0, 0, 0 };   // proyeccion, modelview, textura
static Matrix4& cur() {
    gVerMat[matMode == 0 ? 0 : (matMode == 2 ? 2 : 1)]++;
    return matMode == 0 ? curProj : (matMode == 2 ? curTex : curMV);
}

// EL AJUSTE A LA 3DS, despues de la proyeccion de GL: la pantalla girada un cuarto de vuelta (x' = y, y' = -x) y la z de
// GL (-1 cerca, 1 lejos) llevada a la de la PICA (-1 cerca, 0 lejos). En la PICA la profundidad guardada queda 1 cerca
// y 0 lejos (C3D_DepthMap de fabrica): por eso los test de z van al reves (LESS de GL = GREATER).
static const Matrix4& Ajuste() {
    static Matrix4 a;
    static bool listo = false;
    if (listo) return a;
    for (int i = 0; i < 16; i++) a.m[i] = 0;
    a.m[1 * 4 + 0] = 1.0f;     // x' = y
    a.m[0 * 4 + 1] = -1.0f;    // y' = -x
    a.m[2 * 4 + 2] = 0.5f;     // z' = z/2 - w/2
    a.m[3 * 4 + 2] = -0.5f;
    a.m[3 * 4 + 3] = 1.0f;
    listo = true;
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
        case ScissorTest: cap_scissor = true; PonerScissor(); break;
        case Texture2D: cap_tex = true; break;
        case Lighting: cap_light = true; break;
        case Fog: cap_fog = true; break;
        case ColorMaterial: cap_colormat = true; materialSucio = true; break;
        case PolygonOffsetFill: cap_offset = true; break;
        case Light0: if (!luces[0].on) { luces[0].on = true; lucesSucias = true; } break;
        default: break;
    }
}
void Disable(Cap c) {
    switch (c) {
        case DepthTest: cap_depth = false; break;
        case CullFace: cap_cull = false; break;
        case Blend: cap_blend = false; break;
        case ScissorTest: cap_scissor = false; PonerScissor(); break;
        case Texture2D: cap_tex = false; break;
        case Lighting: cap_light = false; break;
        case Fog: cap_fog = false; break;
        case ColorMaterial: cap_colormat = false; materialSucio = true; break;
        case PolygonOffsetFill: cap_offset = false; break;
        case Light0: if (luces[0].on) { luces[0].on = false; lucesSucias = true; } break;
        default: break;
    }
}
bool IsEnabled(Cap c) {
    switch (c) {
        case DepthTest: return cap_depth; case CullFace: return cap_cull; case Blend: return cap_blend;
        case ScissorTest: return cap_scissor; case Texture2D: return cap_tex; case Lighting: return cap_light;
        case Fog: return cap_fog; case ColorMaterial: return cap_colormat; case Light0: return luces[0].on;
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
    gVerMat[matMode == 0 ? 0 : (matMode == 2 ? 2 : 1)]++;
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
bool g_matrizAuditar = false;
int  g_matrizDesyncs = 0;
bool g_invalidarPorMalla = false;
bool g_matricesPerezosas = true;
bool g_punterosCache = true;
void MatricesSincronizar() {}
void SesgoMetros(float m) {
    static float base[16]; static float puesto = 0.0f;
    if (m == puesto) return;
    if (puesto == 0.0f) GetMatrix(Projection, base);
    float P[16]; for (int i = 0; i < 16; i++) P[i] = base[i];
    P[14] -= P[10] * m;
    MatrixMode(Projection); LoadMatrix(P); MatrixMode(ModelView);
    puesto = m;
}

// ============================================================================
//  Viewport / scissor (en coordenadas de GL de la pantalla apaisada: se giran al framebuffer de la PICA)
// ============================================================================
static void PonerViewport() {
    if (!gEnCuadro) return;
    const int W = C3DAnchoPantalla(gPantalla);
    int x = vpActual[1], y = W - vpActual[0] - vpActual[2], w = vpActual[3], h = vpActual[2];
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x == gVpPuesto[0] && y == gVpPuesto[1] && w == gVpPuesto[2] && h == gVpPuesto[3]) return;
    LoteVaciar();
    gVpPuesto[0] = x; gVpPuesto[1] = y; gVpPuesto[2] = w; gVpPuesto[3] = h;
    C3D_SetViewport((u32)x, (u32)y, (u32)w, (u32)h);
}
static void PonerScissor() {
    if (!gEnCuadro) return;
    int modo = GPU_SCISSOR_DISABLE, l = 0, t = 0, r = 0, b = 0;
    if (cap_scissor) {
        const int W = C3DAnchoPantalla(gPantalla);
        modo = GPU_SCISSOR_NORMAL;
        l = scActual[1]; r = scActual[1] + scActual[3]; t = W - scActual[0] - scActual[2]; b = W - scActual[0];
        if (l < 0) l = 0; if (t < 0) t = 0; if (r > 240) r = 240; if (b > W) b = W;
        if (r <= l || b <= t) { l = 0; t = 0; r = 1; b = 1; }   // (vacio: un pixel, para no dibujar nada visible)
    }
    if (modo == gScPuesto[0] && l == gScPuesto[1] && t == gScPuesto[2] && r == gScPuesto[3] && b == gScPuesto[4]) return;
    LoteVaciar();
    gScPuesto[0] = modo; gScPuesto[1] = l; gScPuesto[2] = t; gScPuesto[3] = r; gScPuesto[4] = b;
    C3D_SetScissor((GPU_SCISSORMODE)modo, (u32)l, (u32)t, (u32)r, (u32)b);
}
void Viewport(int x, int y, int w, int h) { vpActual[0] = x; vpActual[1] = y; vpActual[2] = w < 0 ? 0 : w; vpActual[3] = h < 0 ? 0 : h; PonerViewport(); }
void Scissor(int x, int y, int w, int h) { scActual[0] = x; scActual[1] = y; scActual[2] = w < 0 ? 0 : w; scActual[3] = h < 0 ? 0 : h; PonerScissor(); }
void GetViewport(int v[4]) { for (int i = 0; i < 4; i++) v[i] = vpActual[i]; }
void GetScissor(int v[4]) { for (int i = 0; i < 4; i++) v[i] = scActual[i]; }
bool ScissorActivo() { return cap_scissor; }

// ============================================================================
//  Estado suelto
// ============================================================================
void ClearColor(float r, float g, float b, float a) { clearCol[0] = r; clearCol[1] = g; clearCol[2] = b; clearCol[3] = a; }
static void LimpiarConRectangulo(int bits);
void Clear(int bits) { LimpiarConRectangulo(bits); }
int  PlanosRecorteMax() { return 0; }
void PlanosRecorte(int, const float*) {}
int  EstencilBits() { return 8; }
void EstencilLimpiar(int v) { estencilValor = v; LimpiarConRectangulo(StencilBuffer); }
void EstencilMarcar(int v) { estencil = 1; estencilValor = v; }
void EstencilDentro(int v) { estencil = 2; estencilValor = v; }
void EstencilApagar() { estencil = 0; }
void DepthFunc(DepthCmp c) { depthFunc = (int)c; }
void DepthMask(bool w) { depthMask = w; }
void ColorMask(bool r, bool g, bool b, bool a) { (void)a; colorMask = r || g || b; }
void LineWidth(float px) { lineWidth = px; }
void FrontFace(bool ccw) { frenteCCW = ccw; }
void CullCarasFrontales(bool on) { cullFrente = on; }
void PolygonOffset(float factor, float units) { polyF = factor; polyU = units; }
void DepthRange(float, float) {}
void SmoothShading(bool) {}
void FastPerspective() {}
void Wireframe(bool) {}
void PointSize(float px) { pointSize = px; }
void PointSpriteCoordReplace(bool on) { pointSprite = on; }
void BlurY(float) {}
void AlphaTest(float ref) { alphaRef = ref > 0.0f ? ref : 0.0f; }
void AlfaEmpaquetado(bool) {}
void BlendAlpha() { mezclaFirma = MezclaAlpha; }
void SetMezcla(int m) { mezclaFirma = m; }
void BlendMode(int modo) {
    mezclaFirma = (modo == 1) ? MezclaMultiply : (modo == 2) ? MezclaAdd : (modo == 3) ? 100 : (modo == 4) ? MezclaSubtract
                : (modo == 5) ? MezclaScreen : MezclaAlpha;
}
void Invalidate() { texGenCache = -1; matcapCache = -1; }
void EstadoBase() { SesgoMetros(0.0f); AlphaTest(0.0f); Disable(PolygonOffsetFill); Disable(Lighting); Disable(ColorMaterial); }
bool EstadoFotoSoportada() { return false; }
void EstadoFotoTomar() {}
void EstadoFotoReponer() {}
unsigned int ErrorGL() { return 0u; }
void Finish() {}
void DibujarPendiente() {}
bool g_auditarEscena = false;
int  g_auditDesyncs = 0;
int  AuditarEstado() { return 0; }
void TexBaseLevel(int) {}
void GLES2Init(void* (*)(const char*)) {}

// ============================================================================
//  Luz / material (las luces se guardan como GL: posicion y cono ya en OJO, con la modelview de AHORA)
// ============================================================================
static void EnOjoPunto(const float* v, float* out) {
    const float* m = curMV.m;
    for (int k = 0; k < 3; k++) out[k] = m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k] * v[3];
    out[3] = v[3];
}
void LightNfv(int i, LightFv p, const float* v) {
    if (i < 0 || i >= LucesMax) return;
    Luz& L = luces[i];
    if (p == LightPosition) EnOjoPunto(v, L.pos);
    else if (p == LightDiffuse) for (int k = 0; k < 4; k++) L.dif[k] = v[k];
    else if (p == LightAmbient) for (int k = 0; k < 4; k++) L.amb[k] = v[k];
    else if (p == LightSpecular) for (int k = 0; k < 4; k++) L.spec[k] = v[k];
    lucesSucias = true;
}
void LightNf(int i, LightF p, float v) {
    if (i < 0 || i >= LucesMax) return;
    luces[i].att[p == LightConstantAtt ? 0 : (p == LightLinearAtt ? 1 : 2)] = v;
    lucesSucias = true;
}
void LightNSpot(int i, float corte, float exponente, const float* dir3) {
    if (i < 0 || i >= LucesMax) return;
    Luz& L = luces[i];
    const float* m = curMV.m;
    for (int k = 0; k < 3; k++) L.dir[k] = m[k] * dir3[0] + m[4 + k] * dir3[1] + m[8 + k] * dir3[2];
    L.corteCos = (corte >= 180.0f) ? -1.0f : cosf(corte * 0.01745329252f);
    L.expo = exponente;
    lucesSucias = true;
}
void LightN(int i, bool on) { if (i < 0 || i >= LucesMax || luces[i].on == on) return; luces[i].on = on; lucesSucias = true; }
void Light0fv(LightFv p, const float* v) { LightNfv(0, p, v); }
void Light0f(LightF p, float v) { LightNf(0, p, v); }
void SetLightEnabled(unsigned int id, bool on) { if (id >= 0x4000u && id < 0x4000u + (unsigned)LucesMax) LightN((int)(id - 0x4000u), on); }
void LightModelAmbient(const float* c) { for (int k = 0; k < 4; k++) ambGlobal[k] = c[k]; materialSucio = true; }
void Material(MatParam p, const float* rgba) {
    float* d = (p == MatDiffuse) ? matDiff : (p == MatAmbient) ? matAmb : (p == MatSpecular) ? matSpec : matEmis;
    if (memcmp(d, rgba, sizeof(float) * 4) == 0) return;
    for (int i = 0; i < 4; i++) d[i] = rgba[i];
    materialSucio = true;
}
void MaterialShininess(float s) { if (s != matShine) { matShine = s; materialSucio = true; } }

// ============================================================================
//  Textura / texenv / matriz de textura
// ============================================================================
void BindTexture(unsigned int id) { boundTex = id; ::W3dC3DBindTextura(id); }
void TexEnvDot3(bool on) { texEnv = on ? 2 : 0; }
void TexEnvReplace(bool on) { texEnv = on ? 1 : 0; }
void TexEnvAlphaOnly(bool on) { texEnv = on ? 3 : 0; }
bool MezclaTexturasSoportada() { return true; }
void MezclaTexturas(unsigned int idB, float k, bool filtrado, bool repetir, int modo) {
    if (!idB) { MezclaTexturasOff(); return; }
    if (k < 0.0f) k = 0.0f; if (k > 1.0f) k = 1.0f;
    if (modo < 0 || modo > 4) modo = 0;
    if (modo == 4) modo = 1;
    mezclaOn = true; mezclaTex = idB; mezclaK = k; mezclaModo = modo;
    C3D_Tex* t = w3dC3D::Textura(idB);
    if (t) {
        C3D_TexSetFilter(t, filtrado ? GPU_LINEAR : GPU_NEAREST, filtrado ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetWrap(t, repetir ? GPU_REPEAT : GPU_CLAMP_TO_EDGE, repetir ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
    }
}
void MezclaTexturasOff() { mezclaOn = false; mezclaUV = 0; }
bool MezclaTexturasActiva() { return mezclaOn; }
void MezclaTexturasUV(const float* p) { mezclaUV = p; }
void MezclaTexturasUVVBO(unsigned int) { mezclaUV = 0; }
void MezclaTexturaBorrada(unsigned int id) { if (id && id == mezclaTex) { mezclaTex = 0; mezclaOn = false; } }
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
    T[0] = s * mv[0];  T[1] = -s * mv[1]; T[2] = 0.0f;  T[3] = 0.0f;
    T[4] = s * mv[4];  T[5] = -s * mv[5]; T[6] = 0.0f;  T[7] = 0.0f;
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
bool TieneTexGen() { return false; }   // (el sphere-map por hardware todavia no: cae al de software)

// ============================================================================
//  Color / niebla / arreglos
// ============================================================================
void Color4f(float r, float g, float b, float a) { curColor[0] = r; curColor[1] = g; curColor[2] = b; curColor[3] = a; }
void Color4fv(const float* c) { for (int i = 0; i < 4; i++) curColor[i] = c[i]; }
void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { curColor[0] = r / 255.0f; curColor[1] = g / 255.0f; curColor[2] = b / 255.0f; curColor[3] = a / 255.0f; }
void FogMode(bool lineal) { fogModo = lineal ? 0 : 1; }
void FogModo(int m) { fogModo = (m < 0 || m > 2) ? 0 : m; }
void FogDensidad(float d) { fogDens = d; }
void FogStart(float z) { fogStart = z; }
void FogEnd(float z) { fogEnd = z; }
void FogColor(const float* rgba) { for (int i = 0; i < 4; i++) fogColor[i] = rgba[i]; }
void EnableArray(VArray a) { if (a == VertexArray) aPos.on = true; else if (a == NormalArray) aNrm.on = true; else if (a == TexCoordArray) aUV.on = true; else aCol.on = true; }
void DisableArray(VArray a) { if (a == VertexArray) aPos.on = false; else if (a == NormalArray) aNrm.on = false; else if (a == TexCoordArray) aUV.on = false; else aCol.on = false; }
int  ArrayEstado(VArray a) { return (a == VertexArray ? aPos.on : a == NormalArray ? aNrm.on : a == TexCoordArray ? aUV.on : aCol.on) ? 1 : 0; }
void VertexPointer2f(int s, const float* p) { aPos.tam = 2; aPos.tipo = TipoFloat; aPos.paso = s; aPos.ptr = p; }
void VertexPointer3f(int s, const float* p) { aPos.tam = 3; aPos.tipo = TipoFloat; aPos.paso = s; aPos.ptr = p; }
void VertexPointer3s(int s, const short* p) { aPos.tam = 3; aPos.tipo = TipoShort; aPos.paso = s; aPos.ptr = p; }
void VertexPointer2s(int s, const short* p) { aPos.tam = 2; aPos.tipo = TipoShort; aPos.paso = s; aPos.ptr = p; }
void NormalPointer3b(const signed char* p) { aNrm.tam = 3; aNrm.tipo = TipoByte; aNrm.paso = 0; aNrm.ptr = p; }
void ColorPointer4ub(const unsigned char* p) { aCol.tam = 4; aCol.tipo = TipoUByte; aCol.paso = 0; aCol.ptr = p; }
void ColorPointer4f(int s, const float* p) { aCol.tam = 4; aCol.tipo = TipoFloat; aCol.paso = s; aCol.ptr = p; }
void TexCoordPointer2f(int s, const float* p) { aUV.tam = 2; aUV.tipo = TipoFloat; aUV.paso = s; aUV.ptr = p; }
void TexCoordPointer3b(const signed char* p, int) { aUV.tam = 3; aUV.tipo = TipoByte; aUV.paso = 0; aUV.ptr = p; }

// ============================================================================
//  El estado de la PICA antes de cada draw
// ============================================================================
static int Bytes(int tipo) { return tipo == TipoFloat ? 4 : (tipo == TipoShort ? 2 : 1); }
static GPU_FORMATS FormatoGPU(int tipo) { return tipo == TipoFloat ? GPU_FLOAT : tipo == TipoShort ? GPU_SHORT : tipo == TipoByte ? GPU_BYTE : GPU_UNSIGNED_BYTE; }
static GPU_TESTFUNC FuncZ() {
    switch (depthFunc) {
        case DepthLEqual: return GPU_GEQUAL;
        case DepthEqual: return GPU_EQUAL;
        case DepthAlways: return GPU_ALWAYS;
        default: return GPU_GREATER;
    }
}
static void PonerMezcla() {
    if (!cap_blend) { C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO); return; }
    switch (mezclaFirma) {
        case MezclaOff:      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO); break;
        case MezclaAdd:      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE); break;
        case MezclaAddAlpha: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE); break;
        case MezclaMultiply: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ZERO, GPU_DST_ALPHA, GPU_ZERO); break;
        case MezclaScreen:   C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_COLOR, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA); break;
        case MezclaPremult:  C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA); break;
        case MezclaSubtract: C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE); break;
        case 100:            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_SRC_COLOR, GPU_DST_ALPHA, GPU_SRC_ALPHA); break;
        default:             C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA); break;
    }
}
// la luz por fragmento de la PICA con la cuenta del pipeline fijo: emision + ambiente global * ambiente del material
// + por luz (ambiente de la luz * ambiente + N.L * difusa + (N.H)^brillo * especular). Con COLOR_MATERIAL el material
// queda blanco y el combiner multiplica por el color del vertice
static void PonerLuces() {
    if (materialSucio) {
        C3D_Material m;
        for (int k = 0; k < 3; k++) {
            m.ambient[k] = cap_colormat ? 1.0f : matAmb[k];
            m.diffuse[k] = cap_colormat ? 1.0f : matDiff[k];
            m.specular0[k] = matSpec[k];
            m.specular1[k] = 0.0f;
            m.emission[k] = matEmis[k] + ambGlobal[k] * (cap_colormat ? 1.0f : matAmb[k]);
        }
        C3D_LightEnvMaterial(&gLuzEnv, &m);
        if (matShine != gLutEspBrillo) {
            LightLut_Phong(&gLutEsp, matShine > 0.0f ? matShine : 0.001f);
            C3D_LightEnvLut(&gLuzEnv, GPU_LUT_D0, GPU_LUTINPUT_NH, false, &gLutEsp);
            gLutEspBrillo = matShine;
        }
        materialSucio = false;
    }
    if (lucesSucias) {
        for (int i = 0; i < LucesMax; i++) {
            const Luz& L = luces[i];
            C3D_LightEnable(&gLuz[i], L.on);
            if (!L.on) continue;
            C3D_FVec p = FVec4_New(L.pos[0], L.pos[1], L.pos[2], L.pos[3] != 0.0f ? 1.0f : 0.0f);
            C3D_LightPosition(&gLuz[i], &p);
            C3D_LightDiffuse(&gLuz[i], L.dif[0], L.dif[1], L.dif[2]);
            C3D_LightAmbient(&gLuz[i], L.amb[0], L.amb[1], L.amb[2]);
            C3D_LightSpecular0(&gLuz[i], L.spec[0], L.spec[1], L.spec[2]);
        }
        lucesSucias = false;
    }
}
static void PonerCombiner(bool conTex, bool conLuz) {
    C3D_TexEnv* e0 = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e0);
    // el color "primario": la luz por fragmento (por el color del vertice si hay COLOR_MATERIAL) o el del vertice
    const GPU_TEVSRC prim = conLuz ? GPU_FRAGMENT_PRIMARY_COLOR : GPU_PRIMARY_COLOR;
    if (!conTex) {
        C3D_TexEnvSrc(e0, C3D_Both, prim, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e0, C3D_Both, (conLuz && cap_colormat) ? GPU_MODULATE : GPU_REPLACE);
        if (conLuz && !cap_colormat) {
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
    if (conLuz && texEnv != 1) {
        C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_FRAGMENT_SECONDARY_COLOR, GPU_PREVIOUS);
        C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD);
    }
    // etapa 2: COLOR_MATERIAL con textura (la luz ya modulo la textura: falta el color del vertice)
    C3D_TexEnv* e2 = C3D_GetTexEnv(2);
    C3D_TexEnvInit(e2);
    if (conLuz && conTex && cap_colormat && texEnv == 0) {
        C3D_TexEnvSrc(e2, C3D_Both, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PREVIOUS);
        C3D_TexEnvFunc(e2, C3D_Both, GPU_MODULATE);
    }
    // etapa 3: la segunda textura (mezcla) sobre lo anterior
    C3D_TexEnv* e3 = C3D_GetTexEnv(3);
    C3D_TexEnvInit(e3);
    if (mezclaOn && mezclaTex) {
        if (mezclaModo == 0) {
            C3D_TexEnvSrc(e3, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_CONSTANT);
            C3D_TexEnvOpRgb(e3, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
            C3D_TexEnvFunc(e3, C3D_RGB, GPU_INTERPOLATE);
            C3D_TexEnvColor(e3, (u32)(mezclaK * 255.0f) << 24);
        } else if (mezclaModo == 3) {
            C3D_TexEnvSrc(e3, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PREVIOUS);
            C3D_TexEnvFunc(e3, C3D_RGB, GPU_SUBTRACT);
        } else {
            C3D_TexEnvSrc(e3, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PREVIOUS);
            C3D_TexEnvFunc(e3, C3D_RGB, GPU_MODULATE);
            if (mezclaModo == 2) C3D_TexEnvScale(e3, C3D_RGB, GPU_TEVSCALE_2);
        }
    }
}
// LO QUE YA ESTA PUESTO EN LA PICA: citro3d vuelve a mandar a la GPU todo lo que se toca aunque quede igual (el combiner
// entero, la textura y el vaciado de su cache, las matrices...), y eso era lo que mas tardaba de cada draw. Cada draw
// compara con lo ultimo que se mando y toca solo lo que cambio
struct Puesto {
    GPU_TESTFUNC zFunc; int zEscribe; float zOffset;
    GPU_CULLMODE cull;
    int mezcla, alfa, estencil, estencilValor;
    unsigned combiner;
    C3D_Tex* tex[2];
    unsigned verProj, verMV, verTex; bool recorte;
    float escala[4];
    float fijo[5][4];
};
static Puesto gPuesto;
static bool   gPuestoValido = false;   // false: lo proximo se manda todo
// la textura cambio (sus datos, el filtro o se borro): el proximo draw la vuelve a atar (y la GPU vacia su cache)
void C3DTexturaTocada(C3D_Tex* t) {
    LoteVaciar();   // (lo que ya esta en el lote se dibuja con la textura como estaba atada)
    for (int i = 0; i < 2; i++) if (gPuesto.tex[i] == t) gPuesto.tex[i] = NULL;
}
static void PonerFijo(int i, float x, float y, float z, float w) {
    float* f = gPuesto.fijo[i];
    if (gPuestoValido && f[0] == x && f[1] == y && f[2] == z && f[3] == w) return;
    f[0] = x; f[1] = y; f[2] = z; f[3] = w;
    C3D_FixedAttribSet(i, x, y, z, w);
}
static void PonerEscala(float x, float y, float z, float w) {
    float* e = gPuesto.escala;
    if (gPuestoValido && e[0] == x && e[1] == y && e[2] == z && e[3] == w) return;
    e[0] = x; e[1] = y; e[2] = z; e[3] = w;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, uEscala, x, y, z, w);
}
// la clave del combiner: todo lo que decide como quedan las etapas en PonerCombiner
static unsigned ClaveCombiner(bool conTex, bool conLuz) {
    const bool mezcla = mezclaOn && mezclaTex;
    unsigned k = (conTex ? 1u : 0u) | (conLuz ? 2u : 0u) | ((unsigned)texEnv << 2) | (cap_colormat ? 16u : 0u) | (mezcla ? 32u : 0u);
    if (mezcla) k |= ((unsigned)mezclaModo << 6) | ((unsigned)(mezclaK * 255.0f) << 8);
    if (conLuz && !cap_colormat && !conTex) k |= (unsigned)(matDiff[3] * 255.0f) << 16;
    return k;
}
static void SubirMatrices(bool yaEnRecorte) {
    C3D_Mtx m;
    if (yaEnRecorte) {
        if (gPuestoValido && gPuesto.recorte) return;
        C3D_Mtx ident;
        Mtx_Identity(&ident);
        AMtx(Ajuste(), m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp, &m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMv, &ident);
        C3D_FVUnifMtx3x4(GPU_VERTEX_SHADER, uNmat, &ident);
        gPuesto.recorte = true;
        return;
    }
    const bool cambioMV = !gPuestoValido || gPuesto.recorte || gPuesto.verMV != gVerMat[1];
    const bool cambioP = !gPuestoValido || gPuesto.recorte || gPuesto.verProj != gVerMat[0];
    if (cambioMV || cambioP) {
        AMtx(Ajuste() * curProj * curMV, m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp, &m);
    }
    if (cambioMV) {
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
    gPuesto.verProj = gVerMat[0]; gPuesto.verMV = gVerMat[1]; gPuesto.recorte = false;
}
// lo comun a todos los draws: z, culling, mezcla, alpha test, estencil, texturas, combiner, luz y las matrices.
// Primero se calcula a que tiene que quedar (EstadoDraw, sale del estado emulado de GL) y despues se pone (solo lo que
// cambio). 'yaEnRecorte' = la posicion ya viene en el espacio de recorte de GL (armada en la CPU: lineas, puntos y el
// lote): ahi no hay luz ni culling y la matriz es la de ajuste sola
struct EstadoDraw {
    GPU_TESTFUNC zFunc; int zEscribe; float zOffset;
    GPU_CULLMODE cull;
    int mezcla, alfa, estencil, estencilValor;
    C3D_Tex* tex[2];
    bool conLuz;
    unsigned combiner, verTex;
};
static void CalcularEstado(bool yaEnRecorte, EstadoDraw& e) {
    memset(&e, 0, sizeof(e));   // (se comparan con memcmp: el relleno tambien en cero)
    const int colorBits = colorMask ? GPU_WRITE_COLOR : 0;
    e.zFunc = cap_depth ? FuncZ() : GPU_ALWAYS;   // (sin test, GL tampoco escribe la z)
    e.zEscribe = colorBits | ((cap_depth && depthMask) ? GPU_WRITE_DEPTH : 0);
    e.zOffset = cap_offset ? -0.00002f * (polyU + polyF) : 0.0f;
    // GL_BACK descarta las de atras; GL_FRONT, las de adelante (con el winding que se haya elegido)
    e.cull = (!cap_cull || yaEnRecorte) ? GPU_CULL_NONE : ((frenteCCW == cullFrente) ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW);
    e.mezcla = cap_blend ? mezclaFirma : MezclaOff;
    e.alfa = alphaRef > 0.0f ? (int)(alphaRef * 255.0f) : -1;
    e.estencil = estencil;
    e.estencilValor = estencil ? estencilValor : 0;
    e.tex[0] = (cap_tex && boundTex) ? w3dC3D::Textura(boundTex) : NULL;
    e.tex[1] = (mezclaOn && mezclaTex) ? w3dC3D::Textura(mezclaTex) : NULL;
    e.conLuz = cap_light && !yaEnRecorte && !(e.tex[0] && texEnv == 1);
    e.combiner = ClaveCombiner(e.tex[0] != NULL, e.conLuz);
    e.verTex = gVerMat[2];
}
static void AplicarEstado(const EstadoDraw& e, bool yaEnRecorte) {
    const bool todo = !gPuestoValido;
    if (todo || e.zFunc != gPuesto.zFunc || e.zEscribe != gPuesto.zEscribe) {
        C3D_DepthTest(true, e.zFunc, (GPU_WRITEMASK)e.zEscribe);
        gPuesto.zFunc = e.zFunc; gPuesto.zEscribe = e.zEscribe;
    }
    if (todo || e.zOffset != gPuesto.zOffset) { C3D_DepthMap(true, -1.0f, e.zOffset); gPuesto.zOffset = e.zOffset; }
    if (todo || e.cull != gPuesto.cull) { C3D_CullFace(e.cull); gPuesto.cull = e.cull; }
    if (todo || e.mezcla != gPuesto.mezcla) { PonerMezcla(); gPuesto.mezcla = e.mezcla; }
    if (todo || e.alfa != gPuesto.alfa) { C3D_AlphaTest(e.alfa >= 0, GPU_GEQUAL, e.alfa >= 0 ? e.alfa : 0); gPuesto.alfa = e.alfa; }
    if (todo || e.estencil != gPuesto.estencil || e.estencilValor != gPuesto.estencilValor) {
        if (e.estencil == 1) { C3D_StencilTest(true, GPU_ALWAYS, e.estencilValor, 0xFF, 0xFF); C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE); }
        else if (e.estencil == 2) { C3D_StencilTest(true, GPU_EQUAL, e.estencilValor, 0xFF, 0x00); C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP); }
        else C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0x00);
        gPuesto.estencil = e.estencil; gPuesto.estencilValor = e.estencilValor;
    }
    // texturas (la unidad sin textura queda con la anterior: el combiner no la lee)
    for (int u = 0; u < 2; u++)
        if (e.tex[u] && (todo || e.tex[u] != gPuesto.tex[u])) { C3D_TexBind(u, e.tex[u]); gPuesto.tex[u] = e.tex[u]; }
    if (e.conLuz) {
        if (!gLuzEnvPuesto) { C3D_LightEnvBind(&gLuzEnv); gLuzEnvPuesto = true; }
        PonerLuces();
    } else if (gLuzEnvPuesto) { C3D_LightEnvBind(NULL); gLuzEnvPuesto = false; }
    if (todo || e.combiner != gPuesto.combiner) { PonerCombiner(e.tex[0] != NULL, e.conLuz); gPuesto.combiner = e.combiner; }
    // matrices
    SubirMatrices(yaEnRecorte);
    if (todo || gPuesto.verTex != e.verTex) {
        C3D_Mtx m;
        AMtx(curTex, m);
        C3D_FVUnifMtx2x4(GPU_VERTEX_SHADER, uTexm, &m);
        gPuesto.verTex = e.verTex;
    }
    gPuestoValido = true;
}
static void PonerEstado(bool yaEnRecorte) {
    EstadoDraw e;
    CalcularEstado(yaEnRecorte, e);
    AplicarEstado(e, yaEnRecorte);
}

// un arreglo de cliente al anillo: los vertices [desde, hasta). Devuelve el puntero en memoria lineal (o NULL)
// En el anillo cada arreglo va empaquetado y lo que se declara de cada vertice ocupa el paso entero: lo de 3 bytes
// (las normales de las mallas) va como 4 componentes, con la cuarta en cero (el shader no la lee). Con 3 bytes y paso
// 4 el emulador dibujaba bien, pero en la consola el cubo salia sombreado como con las normales soldadas: la PICA de
// verdad no parece saltear los bytes que no estan descriptos (picaGL tambien agrega relleno hasta completar el paso).
// 'compsOut' = cuantas componentes declarar
static const void* CopiarArreglo(const Arr& a, int desde, int hasta, int& pasoOut, int& compsOut) {
    const int b = Bytes(a.tipo);
    const int elem = a.tam * b;
    const int paso = a.paso ? a.paso : elem;
    const int n = hasta - desde;
    if (n <= 0 || !a.ptr) return NULL;
    const int comps = (b == 1 && a.tam == 3) ? 4 : a.tam;
    const int elemDst = comps * b;
    u8* dst = (u8*)AnilloPedir((u32)(elemDst * n));
    if (!dst) return NULL;
    const u8* src = (const u8*)a.ptr + (size_t)desde * paso;
    if (paso == elemDst && elem == elemDst) memcpy(dst, src, (size_t)elemDst * n);
    else for (int i = 0; i < n; i++) {
        u8* d = dst + (size_t)i * elemDst;
        memcpy(d, src + (size_t)i * paso, (size_t)elem);
        if (elemDst > elem) memset(d + elem, 0, (size_t)(elemDst - elem));
    }
    pasoOut = elemDst;
    compsOut = comps;
    return dst;
}
// los atributos del draw: los cinco del shader en orden (posicion, normal, UV, color, UV de la unidad 1), cada uno
// leido del anillo o fijo. En el anillo queda solo el rango [desde, hasta): el draw cuenta los vertices desde 'desde'.
// Devuelve false si no hay posiciones o no entra en el anillo
static bool PonerAtributos(int desde, int hasta) {
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
            int paso = 0, comps = 0;
            const void* p = CopiarArreglo(a, desde, hasta, paso, comps);
            if (!p) return false;
            const int id = AttrInfo_AddLoader(ai, i, FormatoGPU(a.tipo), comps);
            BufInfo_Add(bi, p, paso, 1, (u64)id);
            if (i == 1 && a.tipo == TipoByte) esc[1] = 1.0f / 127.0f;
            if (i == 3 && a.tipo == TipoUByte) esc[3] = 1.0f / 255.0f;
        } else {
            AttrInfo_AddFixed(ai, i);
            if (i == 1) PonerFijo(i, 0.0f, 0.0f, 1.0f, 0.0f);
            else if (i == 2) PonerFijo(i, 0.0f, 0.0f, 0.0f, 1.0f);
            else if (i == 3) PonerFijo(i, curColor[0], curColor[1], curColor[2], curColor[3]);
        }
    }
    // la UV de la unidad 1: la propia (2 floats) o la misma de la unidad 0
    if (mezclaOn && mezclaTex && (mezclaUV || (aUV.on && aUV.ptr))) {
        Arr u1 = mezclaUV ? Arr{ true, 2, TipoFloat, 0, mezclaUV } : aUV;
        int paso = 0, comps = 0;
        const void* p = CopiarArreglo(u1, desde, hasta, paso, comps);
        if (!p) return false;
        const int id = AttrInfo_AddLoader(ai, 4, FormatoGPU(u1.tipo), comps);
        BufInfo_Add(bi, p, paso, 1, (u64)id);
    } else {
        AttrInfo_AddFixed(ai, 4);
        PonerFijo(4, 0.0f, 0.0f, 0.0f, 1.0f);
    }
    PonerEscala(esc[0], esc[1], esc[2], esc[3]);
    return true;
}

// ============================================================================
//  Lineas y puntos: la PICA no los tiene. Se arman como triangulos en la CPU, en el espacio de recorte de GL (con el
//  ancho en pixeles de pantalla), y se dibujan con la matriz de ajuste sola
// ============================================================================
struct VertRecorte { float x, y, z, w, u, v; unsigned char c[4]; };
static void TransformarAlRecorte(int i, const float* PM, float* o) {
    const Arr& a = aPos;
    const int b = Bytes(a.tipo), paso = a.paso ? a.paso : a.tam * b;
    const u8* p = (const u8*)a.ptr + (size_t)i * paso;
    float x, y, z = 0.0f;
    if (a.tipo == TipoShort) { const short* q = (const short*)p; x = q[0]; y = q[1]; if (a.tam > 2) z = q[2]; }
    else { const float* q = (const float*)p; x = q[0]; y = q[1]; if (a.tam > 2) z = q[2]; }
    for (int k = 0; k < 4; k++) o[k] = PM[k] * x + PM[4 + k] * y + PM[8 + k] * z + PM[12 + k];
}
static void ColorDe(int i, unsigned char* c) {
    if (aCol.on && aCol.ptr) {
        if (aCol.tipo == TipoUByte) { const u8* p = (const u8*)aCol.ptr + (size_t)i * (aCol.paso ? aCol.paso : 4); memcpy(c, p, 4); return; }
        const float* p = (const float*)((const u8*)aCol.ptr + (size_t)i * (aCol.paso ? aCol.paso : 16));
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
static const int kLoteMaxVerts = 96;       // un draw con mas vertices va directo (la GPU los transforma mas rapido)
static const int kLoteTope = 8192;         // (lo que no entra sale en el lote siguiente)
static VertRecorte* gLote = NULL;          // (memoria comun: al anillo se copia cuando el lote sale)
static int gLoteN = 0;
static EstadoDraw gLoteEstado;
static std::vector<VertRecorte> gArmado;   // (donde se arman las lineas, los puntos y el clear)
// (el vigilante: este draw, con el estado que quedo puesto. Lo armado en la CPU dice hasta donde llega en la pantalla;
// la interfaz, con w = 1, sin dividir)
static void Registrar(char tipo, int n, const VertRecorte* v) {
    gRegTotal++;
    if (gRegN >= kRegMax) return;
    RegistroDraw& r = gReg[gRegN++];
    r.tipo = tipo; r.n = n; r.mezcla = gPuesto.mezcla; r.zFunc = (int)gPuesto.zFunc; r.combiner = gPuesto.combiner;
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
static void DibujarRecorte(const VertRecorte* v, int n) {
    VertRecorte* d = (VertRecorte*)AnilloPedir((u32)(n * sizeof(VertRecorte)));
    if (!d) return;
    memcpy(d, v, (size_t)n * sizeof(VertRecorte));
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);          // posicion (recorte)
    AttrInfo_AddFixed(ai, 1);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);          // UV
    AttrInfo_AddLoader(ai, 3, GPU_UNSIGNED_BYTE, 4);  // color
    AttrInfo_AddFixed(ai, 4);
    PonerFijo(1, 0.0f, 0.0f, 1.0f, 0.0f);
    PonerFijo(4, 0.0f, 0.0f, 0.0f, 1.0f);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, d, sizeof(VertRecorte), 3, 0x320);  // (los atributos 0, 2 y 3 entrelazados)
    PonerEscala(1.0f, 1.0f, 1.0f, 1.0f / 255.0f);
    C3D_DrawArrays(GPU_TRIANGLES, 0, n);
    Registrar('R', n, v);
    g_statDrawTris++; g_statDcCat[gStatCat]++;
}
static void LoteVaciar() {
    if (gLoteN <= 0) return;
    DibujarRecorte(gLote, gLoteN);
    gLoteN = 0;
}
// lugar en el lote para n vertices con el estado actual (si el estado cambio, el lote anterior sale antes). NULL si
// son demasiados para el lote: el que llama los dibuja aparte
static VertRecorte* LoteLugar(int n) {
    if (!gLote) gLote = (VertRecorte*)malloc(sizeof(VertRecorte) * kLoteTope);
    EstadoDraw e;
    CalcularEstado(true, e);
    if (gLoteN == 0 || memcmp(&e, &gLoteEstado, sizeof(e)) != 0 || gLoteN + n > kLoteTope) {
        LoteVaciar();
        AplicarEstado(e, true);
        gLoteEstado = e;
    }
    g_statIndices += n;
    if (!gLote || n > kLoteTope) return NULL;
    VertRecorte* p = gLote + gLoteN;
    gLoteN += n;
    return p;
}
static void DibujarArmado(const VertRecorte* v, int n) {
    if (n <= 0 || !gEnCuadro) return;
    VertRecorte* d = LoteLugar(n);
    if (d) memcpy(d, v, (size_t)n * sizeof(VertRecorte));
    else DibujarRecorte(v, n);   // (el estado ya quedo puesto)
}
// un draw de triangulos va al lote si es chico, sin luz ni culling (en el recorte no hay) y con lo que el lote lleva:
// una sola textura con UV de dos float
static bool Loteable(GPU_Primitive_t prim, int n) {
    if (prim != GPU_TRIANGLES || n > kLoteMaxVerts) return false;
    if (cap_light || cap_cull || cap_fog || (mezclaOn && mezclaTex)) return false;
    if (aPos.tipo != TipoFloat && aPos.tipo != TipoShort) return false;
    if (aUV.on && aUV.ptr && (aUV.tipo != TipoFloat || aUV.tam != 2)) return false;
    return true;
}
// los vertices del draw (los indices, o desde 'primero') al espacio de recorte, al lote. Es la cuenta del vertex
// shader hecha en la CPU: tiene que ser barata (la UI en 2D: x, y sin z; el color casi siempre fijo)
template <typename T> static void AlLote(const T* idx, int primero, int n) {
    VertRecorte* o = LoteLugar(n);
    if (!o) return;
    float PM[16];
    Matrix4 m = curProj * curMV;
    memcpy(PM, m.m, sizeof(PM));
    const u8* pos = (const u8*)aPos.ptr;
    const int pasoPos = aPos.paso ? aPos.paso : aPos.tam * Bytes(aPos.tipo);
    const bool pos2f = aPos.tipo == TipoFloat && aPos.tam == 2;
    const u8* uv = (aUV.on && aUV.ptr) ? (const u8*)aUV.ptr : NULL;
    const int pasoUV = aUV.paso ? aUV.paso : 8;
    unsigned char cFijo[4];
    const bool colorFijo = !(aCol.on && aCol.ptr);
    if (colorFijo) ColorDe(0, cFijo);
    for (int j = 0; j < n; j++, o++) {
        const int i = idx ? (int)idx[j] : primero + j;
        if (pos2f) {
            const float* q = (const float*)(pos + (size_t)i * pasoPos);
            const float x = q[0], y = q[1];
            o->x = PM[0] * x + PM[4] * y + PM[12];
            o->y = PM[1] * x + PM[5] * y + PM[13];
            o->z = PM[2] * x + PM[6] * y + PM[14];
            o->w = PM[3] * x + PM[7] * y + PM[15];
        } else {
            float p[4];
            TransformarAlRecorte(i, PM, p);
            o->x = p[0]; o->y = p[1]; o->z = p[2]; o->w = p[3];
        }
        if (uv) { const float* t = (const float*)(uv + (size_t)i * pasoUV); o->u = t[0]; o->v = t[1]; }
        else { o->u = 0.0f; o->v = 0.0f; }
        if (colorFijo) memcpy(o->c, cFijo, 4);
        else ColorDe(i, o->c);
    }
}
static void Esquina(std::vector<VertRecorte>& v, const float* p, float dx, float dy, float u, float t, const unsigned char* c) {
    VertRecorte q; q.x = p[0] + dx * p[3]; q.y = p[1] + dy * p[3]; q.z = p[2]; q.w = p[3]; q.u = u; q.v = t; memcpy(q.c, c, 4);
    v.push_back(q);
}
static void Recortar(float* a, const float* b) {   // la punta detras del plano cercano, al plano (z + w >= 0)
    const float da = a[2] + a[3], db = b[2] + b[3], t = da / (da - db);
    for (int k = 0; k < 4; k++) a[k] = a[k] + (b[k] - a[k]) * t;
}
// EL MARGEN: lo armado en la CPU no pasa de 2 veces la pantalla para cada lado. La PICA recorta solo contra el plano
// cercano: una linea que lo cruza (la grilla debajo de la camara, con el plano cercano a 0,01) llegaba con la punta a
// miles de pantallas de distancia (|x/w| de mas de 3000: cientos de miles de pixeles). Afuera del margen no se ve nada
static const float kBanda = 2.0f;
// el tramo [a, b] (ya delante del plano cercano) recortado a |x| <= kBanda w, |y| <= kBanda w. false si queda afuera
static bool RecortarBanda(float* a, float* b) {
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 4; k++) {
        const int eje = k >> 1;
        const float s = (k & 1) ? -1.0f : 1.0f;   // (x <= kBanda w, x >= -kBanda w, lo mismo con y)
        const float da = kBanda * a[3] - s * a[eje], db = kBanda * b[3] - s * b[eje];
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
static void ArmarLineas(const int* idx, int n, bool tira, std::vector<VertRecorte>& v) {
    float PM[16]; Matrix4 m = curProj * curMV; memcpy(PM, m.m, sizeof(PM));
    const float hx = (lineWidth > 1.0f ? lineWidth : 1.0f) / (float)(vpActual[2] > 0 ? vpActual[2] : 320);
    const float hy = (lineWidth > 1.0f ? lineWidth : 1.0f) / (float)(vpActual[3] > 0 ? vpActual[3] : 240);
    const int paso = tira ? 1 : 2;
    for (int i = 0; i + 1 < n; i += paso) {
        float a[4], b[4]; unsigned char ca[4], cb[4];
        TransformarAlRecorte(idx[i], PM, a); TransformarAlRecorte(idx[i + 1], PM, b);
        ColorDe(idx[i], ca); ColorDe(idx[i + 1], cb);
        const bool fa = a[2] < -a[3], fb = b[2] < -b[3];
        if (fa && fb) continue;
        if (fa) Recortar(a, b); else if (fb) Recortar(b, a);
        if (!RecortarBanda(a, b)) continue;
        float dx = b[0] / b[3] - a[0] / a[3], dy = b[1] / b[3] - a[1] / a[3];
        // (la direccion en pixeles, para que el ancho sea parejo en las dos direcciones)
        float px = dx / hx, py = dy / hy, l = sqrtf(px * px + py * py);
        if (l < 1e-6f) continue;
        const float nx = -py / l * hx, ny = px / l * hy;
        Esquina(v, a, nx, ny, 0, 0, ca); Esquina(v, b, nx, ny, 1, 0, cb); Esquina(v, a, -nx, -ny, 0, 1, ca);
        Esquina(v, a, -nx, -ny, 0, 1, ca); Esquina(v, b, nx, ny, 1, 0, cb); Esquina(v, b, -nx, -ny, 1, 1, cb);
    }
}
static void ArmarPuntos(const int* idx, int n, std::vector<VertRecorte>& v) {
    float PM[16]; Matrix4 m = curProj * curMV; memcpy(PM, m.m, sizeof(PM));
    const float hx = pointSize / (float)(vpActual[2] > 0 ? vpActual[2] : 320);
    const float hy = pointSize / (float)(vpActual[3] > 0 ? vpActual[3] : 240);
    for (int j = 0; j < n; j++) {
        const int i = idx ? idx[j] : j;
        float p[4]; unsigned char c[4];
        TransformarAlRecorte(i, PM, p);
        if (p[3] <= 0.0f || p[2] < -p[3]) continue;
        if (fabsf(p[0]) > kBanda * p[3] || fabsf(p[1]) > kBanda * p[3]) continue;   // (afuera del margen: no se ve)
        ColorDe(i, c);
        // (UV de 0 a 1 en las esquinas: los iconos son point sprites; la v de GL va de abajo a arriba)
        Esquina(v, p, -hx, hy, 0, 1, c); Esquina(v, p, -hx, -hy, 0, 0, c); Esquina(v, p, hx, -hy, 1, 0, c);
        Esquina(v, p, -hx, hy, 0, 1, c); Esquina(v, p, hx, -hy, 1, 0, c); Esquina(v, p, hx, hy, 1, 1, c);
    }
}

// ============================================================================
//  Los draws
// ============================================================================
// (la medicion de la prueba de rendimiento: solo el draw de afuera cuenta, los que llama adentro ya estan incluidos)
static bool gMedir = false;
static int  gMedirProf = 0;
static u64  gMedirTicks = 0;
struct Cronometro {
    u64 t0;
    Cronometro() : t0(0) { if (gMedir && gMedirProf++ == 0) t0 = svcGetSystemTick(); }
    ~Cronometro() { if (gMedir && --gMedirProf == 0) gMedirTicks += svcGetSystemTick() - t0; }
};
void   C3DMedir(bool on) { gMedir = on; gMedirProf = 0; gMedirTicks = 0; }
double C3DTiempoDrawsMs() { const double ms = (double)gMedirTicks / CPU_TICKS_PER_MSEC; gMedirTicks = 0; return ms; }

static void DibujarArray(GPU_Primitive_t prim, int primero, int n) {
    Cronometro crono;
    if (!gEnCuadro || n <= 0) return;
    if (Loteable(prim, n)) { AlLote((const int*)NULL, primero, n); return; }
    LoteVaciar();
    PonerEstado(false);
    if (!PonerAtributos(primero, primero + n)) return;
    C3D_DrawArrays(prim, 0, n);
    Registrar('A', n, NULL);
    g_statDrawTris++; g_statIndices += n; g_statDcCat[gStatCat]++;
    if (cap_blend) g_statTrisBlend += n / 3; else g_statTrisOpacos += n / 3;
}
template <typename T> static void DibujarIndices(GPU_Primitive_t prim, int n, const T* idx) {
    Cronometro crono;
    if (!gEnCuadro || n <= 0 || !idx) return;
    if (Loteable(prim, n)) { AlLote(idx, 0, n); return; }
    LoteVaciar();
    int lo = 0x7fffffff, hi = -1;
    for (int i = 0; i < n; i++) { const int k = (int)idx[i]; if (k < lo) lo = k; if (k > hi) hi = k; }
    if (hi < 0) return;
    u16* d = (u16*)AnilloPedir((u32)(n * 2));
    if (!d) return;
    for (int i = 0; i < n; i++) d[i] = (u16)((int)idx[i] - lo);
    PonerEstado(false);
    if (!PonerAtributos(lo, hi + 1)) return;
    C3D_DrawElements(prim, n, C3D_UNSIGNED_SHORT, d);   // (los indices, relativos a lo)
    Registrar('I', n, NULL);
    g_statDrawTris++; g_statIndices += n; g_statDcCat[gStatCat]++;
    if (cap_blend) g_statTrisBlend += n / 3; else g_statTrisOpacos += n / 3;
}
static std::vector<int> gIndices;   // (los indices de lineas y puntos, a int)
template <typename T> static void LineasIndexadas(int n, const T* idx, bool tira) {
    Cronometro crono;
    if (!gEnCuadro || n < 2 || !idx || !aPos.on || !aPos.ptr) return;
    gIndices.resize((size_t)n);
    for (int i = 0; i < n; i++) gIndices[i] = (int)idx[i];
    gArmado.clear();
    ArmarLineas(&gIndices[0], n, tira, gArmado);
    if (!gArmado.empty()) DibujarArmado(&gArmado[0], (int)gArmado.size());
}

void DrawTriangles(int count, const MeshIndex* indices) { DibujarIndices(GPU_TRIANGLES, count, indices); }
void DrawTrianglesClientIdx(int count, const MeshIndex* indices) { DibujarIndices(GPU_TRIANGLES, count, indices); }
void DrawLinesClientIdx(int count, const MeshIndex* indices) { LineasIndexadas(count, indices, false); }
void DrawTrianglesByte(int count, const unsigned char* indices) { DibujarIndices(GPU_TRIANGLES, count, indices); }
void DrawTrianglesArray(int n) { DibujarArray(GPU_TRIANGLES, 0, n); }
void DrawTrianglesArrayFrom(int first, int n) { DibujarArray(GPU_TRIANGLES, first, n); }
void DrawLinesIndexed(int count, const unsigned short* indices) { LineasIndexadas(count, indices, false); }
void DrawLineStripIndexed(int count, const unsigned short* indices) { LineasIndexadas(count, indices, true); }
static void LineasArray(int n, bool tira) {
    Cronometro crono;
    if (!gEnCuadro || n < 2 || !aPos.on || !aPos.ptr) return;
    gIndices.resize((size_t)n);
    for (int i = 0; i < n; i++) gIndices[i] = i;
    gArmado.clear();
    ArmarLineas(&gIndices[0], n, tira, gArmado);
    if (!gArmado.empty()) DibujarArmado(&gArmado[0], (int)gArmado.size());
}
void DrawLines(int n) { LineasArray(n, false); }
void DrawLineStrip(int n) { LineasArray(n, true); }
void DrawPoints(int n) {
    Cronometro crono;
    if (!gEnCuadro || n <= 0 || !aPos.on || !aPos.ptr) return;
    gArmado.clear();
    ArmarPuntos(NULL, n, gArmado);
    if (!gArmado.empty()) DibujarArmado(&gArmado[0], (int)gArmado.size());
}
template <typename T> static void Elementos(Primitiva p, int count, const T* indices) {
    Cronometro crono;
    switch (p) {
        case PrimPuntos: {
            if (!gEnCuadro || count <= 0 || !aPos.on || !aPos.ptr) return;
            gIndices.resize((size_t)count);
            for (int i = 0; i < count; i++) gIndices[i] = (int)indices[i];
            gArmado.clear();
            ArmarPuntos(&gIndices[0], count, gArmado);
            if (!gArmado.empty()) DibujarArmado(&gArmado[0], (int)gArmado.size());
            return;
        }
        case PrimLineas: LineasIndexadas(count, indices, false); return;
        case PrimTiraLineas: LineasIndexadas(count, indices, true); return;
        case PrimLazoLineas: {
            std::vector<T> l(indices, indices + count);
            if (count > 0) l.push_back(indices[0]);
            LineasIndexadas((int)l.size(), &l[0], true);
            return;
        }
        case PrimTiraTriangulos: DibujarIndices(GPU_TRIANGLE_STRIP, count, indices); return;
        case PrimAbanico: DibujarIndices(GPU_TRIANGLE_FAN, count, indices); return;
        default: DibujarIndices(GPU_TRIANGLES, count, indices); return;
    }
}
void DrawElements(Primitiva p, int count, const MeshIndex* indices) { Elementos(p, count, indices); }
void DrawElements16(Primitiva p, int count, const unsigned short* indices) { Elementos(p, count, indices); }
void DrawElementsVBO(Primitiva, int, int) {}

// EL CLEAR: el de citro3d borra la pantalla entera (y en el momento, no en orden con lo dibujado). GL respeta el
// scissor: es un rectangulo con z ALWAYS, del color de fondo y en la z de lo mas lejano
static void LimpiarConRectangulo(int bits) {
    Cronometro crono;
    if (!gEnCuadro || !bits) return;
    const int W = C3DAnchoPantalla(gPantalla);
    int r[4] = { 0, 0, W, 240 };
    if (cap_scissor) for (int i = 0; i < 4; i++) r[i] = scActual[i];
    // el rectangulo en NDC del viewport de la pantalla entera
    const int vp0[4] = { vpActual[0], vpActual[1], vpActual[2], vpActual[3] };
    vpActual[0] = 0; vpActual[1] = 0; vpActual[2] = W; vpActual[3] = 240; PonerViewport();
    const float x0 = r[0] * 2.0f / W - 1.0f, x1 = (r[0] + r[2]) * 2.0f / W - 1.0f;
    const float y0 = r[1] * 2.0f / 240.0f - 1.0f, y1 = (r[1] + r[3]) * 2.0f / 240.0f - 1.0f;
    unsigned char c[4];
    for (int k = 0; k < 4; k++) c[k] = (unsigned char)(clearCol[k] <= 0 ? 0 : clearCol[k] >= 1 ? 255 : clearCol[k] * 255.0f + 0.5f);
    std::vector<VertRecorte>& v = gArmado;
    v.clear();
    const float p00[4] = { x0, y0, 1.0f, 1.0f }, p10[4] = { x1, y0, 1.0f, 1.0f }, p01[4] = { x0, y1, 1.0f, 1.0f }, p11[4] = { x1, y1, 1.0f, 1.0f };
    Esquina(v, p00, 0, 0, 0, 0, c); Esquina(v, p10, 0, 0, 0, 0, c); Esquina(v, p11, 0, 0, 0, 0, c);
    Esquina(v, p00, 0, 0, 0, 0, c); Esquina(v, p11, 0, 0, 0, 0, c); Esquina(v, p01, 0, 0, 0, 0, c);
    // estado del clear: sin textura, sin luz, sin mezcla, z ALWAYS
    const bool t = cap_tex, d = cap_depth, b = cap_blend, l = cap_light, cm = colorMask, dm = depthMask, sc = cap_scissor;
    const int df = depthFunc, es = estencil; const float ar = alphaRef;
    cap_tex = false; cap_blend = false; cap_light = false; cap_depth = true; depthFunc = DepthAlways; alphaRef = 0.0f;
    colorMask = (bits & ColorBuffer) != 0; depthMask = (bits & DepthBuffer) != 0;
    if (bits & StencilBuffer) { estencil = 1; } else estencil = 0;
    DibujarArmado(&v[0], (int)v.size());
    cap_tex = t; cap_depth = d; cap_blend = b; cap_light = l; colorMask = cm; depthMask = dm; cap_scissor = sc;
    depthFunc = df; estencil = es; alphaRef = ar;
    for (int i = 0; i < 4; i++) vpActual[i] = vp0[i];
    PonerViewport();
}

// ============================================================================
//  LA SELECCION POR COLOR (Pick3D): ReadPixelsBegin abre una pasada aparte, en una superficie del tamano de la
//  pantalla de abajo que no se ve; ReadPixelsRGBA la manda a la GPU, la copia de la VRAM a memoria lineal con el motor
//  de transferencias (el mismo que lleva cada cuadro a la pantalla) y lee ahi esos pixeles. La CPU no lee la VRAM
//  directo: en el emulador andaba, en la consola de verdad no es seguro. Si habia un cuadro a medio dibujar, sigue
//  despues como estaba. Fuera de una pasada abierta la lectura da negro (la pantalla real no se lee)
// ============================================================================
static C3D_RenderTarget* gTargetSel = NULL;
static bool gSelActiva = false, gSelEnCuadro = false;
static int  gSelPantalla = PantallaAbajo;
static void OlvidarVpSc() { for (int i = 0; i < 4; i++) gVpPuesto[i] = -1; for (int i = 0; i < 5; i++) gScPuesto[i] = -1; }
void ReadPixelsBegin(int, int, int, int) {
    if (gSelActiva) return;
    if (!gTargetSel) gTargetSel = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!gTargetSel) return;
    gSelEnCuadro = gEnCuadro;
    if (gEnCuadro) LoteVaciar();
    else {
        EmpezarConEspera(0);   // (espera a que la GPU termine el cuadro anterior: su anillo queda libre)
        gEnCuadro = true;
    }
    gSelActiva = true;
    gSelPantalla = gPantalla;
    gPantalla = PantallaAbajo;   // (las cuentas del viewport con el ancho de la de abajo: la superficie es de su tamano)
    C3D_FrameDrawOn(gTargetSel);
    OlvidarVpSc();
    PonerViewport();
    PonerScissor();
}
static u8* gSelCopia = NULL;   // (la superficie copiada a memoria lineal: 240 x 320 RGBA8, sin bloques)
void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* p) {
    if (!p || w <= 0 || h <= 0) return;
    if (!gSelActiva) { memset(p, 0, (size_t)w * h * 4); return; }
    LoteVaciar();
    Anillo& a = gAnillo[gAnilloAct];
    if (a.usado) GSPGPU_FlushDataCache(a.base, a.usado);
    // las pantallas que ya tenian algo de este cuadro no salen todavia (citro3d las manda al terminar el cuadro)
    const bool usada0 = gTarget[0] && gTarget[0]->used, usada1 = gTarget[1] && gTarget[1]->used;
    if (gTarget[0]) gTarget[0]->used = false;
    if (gTarget[1]) gTarget[1]->used = false;
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    // esperar a que la GPU termine (con el vigilante) y salir del cuadro: la copia sincronica va fuera de un cuadro
    EmpezarConEspera(0);
    C3D_FrameEnd(GX_CMDLIST_FLUSH);   // (vacio: no dibuja ni manda nada a las pantallas)
    const u32 bytes = 240u * 320u * 4u;
    if (!gSelCopia) gSelCopia = (u8*)linearAlloc(bytes);
    if (gSelCopia) {
        // de bloques a lineal, como la salida a la pantalla: la fila gx de la copia es la columna gx de la pantalla
        // y adentro, de abajo hacia arriba
        C3D_SyncDisplayTransfer((u32*)gTargetSel->frameBuf.colorBuf, GX_BUFFER_DIM(240, 320), (u32*)gSelCopia,
                                GX_BUFFER_DIM(240, 320), DISPLAY_TRANSFER_FLAGS);
        GSPGPU_InvalidateDataCache(gSelCopia, bytes);   // (lo que la CPU tuviera en cache de la copia anterior)
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            unsigned char* o = p + ((size_t)j * w + i) * 4;
            const int gx = x + i, gy = y + j;
            if (!gSelCopia || gx < 0 || gx >= 320 || gy < 0 || gy >= 240) { o[0] = o[1] = o[2] = 0; o[3] = 255; continue; }
            const u8* q = gSelCopia + ((size_t)gx * 240 + (size_t)gy) * 4;   // (RGBA8 guardado como A, B, G, R)
            o[0] = q[3]; o[1] = q[2]; o[2] = q[1]; o[3] = q[0];
        }
    gSelActiva = false;
    gPantalla = gSelPantalla;
    if (gSelEnCuadro) {
        // el cuadro que se estaba dibujando sigue (sus pantallas, con lo que ya tenian, salen al terminarlo)
        EmpezarConEspera(0);
        if (gTarget[0]) gTarget[0]->used = usada0;
        if (gTarget[1]) gTarget[1]->used = usada1;
        C3D_FrameDrawOn(gTarget[gPantalla]);
        OlvidarVpSc();
        PonerViewport();
        PonerScissor();
    } else gEnCuadro = false;
}

// ============================================================================
//  Buffer objects: no (las mallas van por el camino de arreglos de cliente)
// ============================================================================
bool VBOSoportado() { return false; }
unsigned int GenBuffer() { return 0; }
void DeleteBuffer(unsigned int) {}
void ArrayBufferData(unsigned int, const void*, int) {}
void IndexBufferData(unsigned int, const void*, int) {}
long VBOMemoria() { return 0; }
void VertexVBO(unsigned int) {}
void NormalVBO(unsigned int) {}
void ColorVBO(unsigned int) {}
void TexCoordVBO(unsigned int) {}
void BindIndexVBO(unsigned int) {}
void DrawTrianglesVBO(int, int) {}
void UnbindVBOs() {}

// ============================================================================
//  Estadisticas (como el backend de GLES2)
// ============================================================================
int g_statMeshes = 0;
int g_statDrawTris = 0, g_statDrawVBO = 0, g_statIndices = 0, g_statTexBinds = 0, g_statStateChanges = 0, g_statOtras = 0;
int g_statUploadBytes = 0, g_statTrisOpacos = 0, g_statTrisBlend = 0;
int g_statDcCat[StatCatCount_] = { 0 };
void StatCategoria(StatCat c) { gStatCat = (int)c; }
StatCat StatCategoriaActual() { return (StatCat)gStatCat; }
void StatRaw(int n) { g_statLlamadas[gStatCat] += n; }
StatsDeCuadro g_statCuadro;
static int gCuadroDraws0 = 0, gCuadroIdx0 = 0;
void StatsCuadro() {
    StatsDeCuadro& c = g_statCuadro;
    c.total = 0;
    for (int i = 0; i < StatCatCount_; i++) { c.llamadas[i] = g_statLlamadas[i]; c.total += g_statLlamadas[i]; g_statLlamadas[i] = 0; }
    c.draws = g_statDrawTris - gCuadroDraws0; gCuadroDraws0 = g_statDrawTris;
    c.binds = 0; c.estados = 0;
    c.tris = (g_statIndices - gCuadroIdx0) / 3; gCuadroIdx0 = g_statIndices;
    c.cuadros++;
    gStatCat = StatCatEditor;
}
void StatsReset() {
    g_statDrawTris = 0; g_statDrawVBO = 0; g_statIndices = 0; g_statTexBinds = 0; g_statMeshes = 0;
    g_statStateChanges = 0; g_statUploadBytes = 0; g_statTrisOpacos = 0; g_statTrisBlend = 0; g_statOtras = 0;
    for (int i = 0; i < StatCatCount_; i++) { g_statDcCat[i] = 0; g_statLlamadas[i] = 0; }
    gCuadroDraws0 = gCuadroIdx0 = 0;
    gStatCat = StatCatEscena;
}

} // namespace w3dEngine
