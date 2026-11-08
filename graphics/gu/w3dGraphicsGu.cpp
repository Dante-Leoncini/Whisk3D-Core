// ============================================================================
//  w3dGraphicsGu.cpp -- backend sceGu de w3dEngine: la GPU de la PSP (el GE, "Graphics Engine").
//
//  El GE tiene pipeline fijo por hardware (transformacion, 4 luces, niebla lineal, una textura) y este backend le pasa
//  el del Core (la MISMA API que w3dGraphics.cpp) casi derecho:
//    - las matrices: la pila por software (Matrix4) y en el GE la proyeccion y la modelview (como matriz de "modelo";
//      la de "vista" queda en identidad). El GE calcula la luz en el espacio del modelo transformado, que asi es el del
//      ojo, como GL: las luces se guardan en el ojo al ponerlas (con la modelview de ese momento)
//    - luces, material, niebla, mezclas, alpha test, estencil, scissor y z por hardware
//    - los arreglos de vertices de la RAM comun se leen en cada draw y se escriben ya entrelazados en el formato del GE
//      (UV, color, normal, posicion; todo float salvo el color) en un anillo de memoria que el GE lee
//  Lo que el GE no hace como GL:
//    - EL RECORTE: el GE no recorta contra los costados, y un triangulo con un vertice fuera de su espacio de 4096x4096
//      se descarta entero (la grilla del piso, que pasa por debajo de la camara, desaparecia). Cada vertice se pasa al
//      espacio de recorte en la CPU y lo que cruza el plano cercano, el lejano o el borde de ese espacio se recorta
//      ahi, interpolando lo del objeto (posicion, normal, UV, color): el GE lo transforma y lo ilumina igual
//    - las lineas de mas de un pixel y los puntos: se arman en la CPU en coordenadas de pantalla (rectangulos en el
//      modo "through" del GE, que no transforma)
//    - wireframe: los triangulos salen como sus tres aristas
//    - el viewport de GL tambien recorta: aca lo hace el scissor (el del GE es el rectangulo comun a los dos)
//    - el z es de 16 bits y al reves (65535 cerca, 0 lejos): los test de z se dan vuelta (LESS de GL = GREATER)
//    - el estencil es el alfa del framebuffer (8888). Para que no lo pise el alfa de lo que se dibuja, el test de
//      estencil queda siempre prendido (sin tocarlo: KEEP), salvo cuando se esta marcando
//  Cada cuadro es una lista de comandos que el GE ejecuta mientras se arma; al terminar el cuadro se espera a que la
//  termine, se espera el vsync y se dan vuelta los buffers.
// ============================================================================
#include "w3dGraphics.h"
#include "w3dlog.h"
#include "w3dGu.h"
#include "w3dGuInterno.h"
#include "math/Matrix4.h"
#include <pspgu.h>
#include <pspge.h>
#include <pspdisplay.h>
#include <psputils.h>
#include <malloc.h>
#include <vector>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

void W3dGuBindTextura(unsigned int id);   // (w3dGLgu.cpp: la textura de las funciones de GL)

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
struct Luz { bool on; float pos[4], amb[4], dif[4], spec[4], att[3], dir[3], corte, expo; };
static Luz luces[LucesMax];
static float ambGlobal[4] = { 0.2f, 0.2f, 0.2f, 1 };
static float matDiff[4] = { 0.8f, 0.8f, 0.8f, 1 }, matAmb[4] = { 0.2f, 0.2f, 0.2f, 1 };
static float matSpec[4] = { 0, 0, 0, 1 }, matEmis[4] = { 0, 0, 0, 1 };
static float matShine = 0.0f;
static bool  lucesSucias = true;      // (las luces o el material cambiaron: el proximo draw con luz los manda)
static float curColor[4] = { 1, 1, 1, 1 };
static float fogColor[4] = { 0, 0, 0, 1 };
static float fogStart = 0, fogEnd = 1, fogDens = 1.0f;
static int   fogModo = 0;
static int   texEnv = 0;              // 0 modulate, 1 replace, 2 DOT3 (aca: modulate), 3 solo alfa
static int   texGenCache = -1, matcapCache = -1;
static float uvMat[4] = { 0, 0, 1, 1 };
static bool  pointSprite = false;
static float pointSize = 1.0f, lineWidth = 1.0f;
static int   vpActual[4] = { 0, 0, GuAncho, GuAlto }, scActual[4] = { 0, 0, GuAncho, GuAlto };
static unsigned boundTex = 0;
static int   depthFunc = (int)DepthLess;
static bool  depthMask = true, colorMask = true;
static bool  frenteCCW = true, cullFrente = false;
static float alphaRef = 0.0f;
static float polyF = 0.0f, polyU = 0.0f;
static float depthN = 0.0f, depthF = 1.0f;
static float clearCol[4] = { 0, 0, 0, 1 };
static int   mezclaFirma = -1;        // la funcion de mezcla puesta (SetMezcla / BlendAlpha / BlendMode)
static int   estencil = 0, estencilValor = 0, estencilLimpio = 0;   // 0 apagado, 1 marcar, 2 dentro
static bool  wireframe = false, sombraSuave = true;
// los arreglos de cliente
enum { TipoFloat = 0, TipoShort, TipoByte, TipoUByte };
struct Arr { bool on; int tam; int tipo; int paso; const void* ptr; };
static Arr aPos = { false, 3, TipoFloat, 0, 0 }, aNrm = { false, 3, TipoByte, 0, 0 };
static Arr aUV = { false, 2, TipoFloat, 0, 0 }, aCol = { false, 4, TipoUByte, 0, 0 };

// ============================================================================
//  El GE: la VRAM (dos pantallas de 8888 y el z de 16 bits), la lista de comandos y el anillo de vertices
// ============================================================================
static const int kPaso = 512;                       // (pixeles por fila de la pantalla en la VRAM)
static const int kOffX = 2048 - GuAncho / 2, kOffY = 2048 - GuAlto / 2;   // (la pantalla en el medio de los 4096)
static const unsigned kBytesPantalla = (unsigned)kPaso * GuAlto * 4;
static void* const kFbA = (void*)0;                 // (direcciones relativas al comienzo de la VRAM)
static void* const kFbB = (void*)kBytesPantalla;
static void* const kZ   = (void*)(2 * kBytesPantalla);
static void* gFbDibujo = kFbA;                      // el que se esta dibujando (sceGuSwapBuffers los da vuelta)
static const int kListaBytes = 512 * 1024;
static unsigned int* gLista = NULL;
static bool gListaAbierta = false;
static bool gEnCuadro = false;
static const int kAnilloBytes = 2 * 1024 * 1024;
static unsigned char* gAnillo = NULL;
static int gAnilloUsado = 0;
static std::vector<void*> gLiberar;                 // (memoria que el GE puede estar leyendo: se suelta al cerrar la lista)
static bool gIniciado = false;

static void ListaAbrir() {
    if (gListaAbierta || !gIniciado) return;
    sceGuStart(GU_DIRECT, gLista);
    gListaAbierta = true;
    gAnilloUsado = 0;   // (cada lista se cierra esperando al GE: lo que habia en el anillo ya se dibujo)
}
// manda lo que falta y espera a que el GE termine
static void ListaCerrar() {
    if (!gListaAbierta) return;
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    gListaAbierta = false;
    for (size_t i = 0; i < gLiberar.size(); i++) free(gLiberar[i]);
    gLiberar.clear();
}
// a mitad del cuadro, la lista o el anillo se llenaron: lo que hay se dibuja y se sigue con otra (el estado del GE
// queda como estaba: son sus registros)
static void ListaVaciar() {
    ListaCerrar();
    ListaAbrir();
}
static void ListaEspacio() {
    if (!gListaAbierta) ListaAbrir();
    else if (sceGuCheckList() > kListaBytes - 32 * 1024) ListaVaciar();
}
static void* AnilloPedir(int bytes) {
    bytes = (bytes + 63) & ~63;   // (lineas de cache enteras)
    if (bytes > kAnilloBytes) return NULL;
    if (gAnilloUsado + bytes > kAnilloBytes) ListaVaciar();
    void* p = gAnillo + gAnilloUsado;
    gAnilloUsado += bytes;
    return p;
}

bool GuEnCuadro() { return gEnCuadro; }
int  GuPasoPantalla() { return kPaso; }
const unsigned char* GuPantallaMostrada() {
    void* fb = (gFbDibujo == kFbA) ? kFbB : kFbA;
    return (const unsigned char*)(0x40000000u | ((unsigned)sceGeEdramGetAddr() + (unsigned)fb));
}

static void InicialLuces() {
    for (int i = 0; i < LucesMax; i++) {
        Luz& L = luces[i];
        L.on = false; L.pos[0] = 0; L.pos[1] = 0; L.pos[2] = 1; L.pos[3] = 0;
        for (int k = 0; k < 4; k++) { L.amb[k] = (k == 3) ? 1.0f : 0.0f; L.dif[k] = (i == 0 || k == 3) ? 1.0f : 0.0f; L.spec[k] = L.dif[k]; }
        L.att[0] = 1; L.att[1] = 0; L.att[2] = 0; L.dir[0] = 0; L.dir[1] = 0; L.dir[2] = -1; L.corte = 180.0f; L.expo = 0.0f;
    }
}
static void OlvidarPuesto();

bool GuIniciar() {
    if (gIniciado) return true;
    gLista = (unsigned int*)memalign(64, kListaBytes);
    gAnillo = (unsigned char*)memalign(64, kAnilloBytes);
    if (!gLista || !gAnillo) { w3dLogfE("[gu] sin memoria para la lista y el anillo de vertices"); return false; }
    sceGuInit();
    sceGuStart(GU_DIRECT, gLista);
    sceGuDrawBuffer(GU_PSM_8888, kFbA, kPaso);
    sceGuDispBuffer(GuAncho, GuAlto, kFbB, kPaso);
    sceGuDepthBuffer(kZ, kPaso);
    sceGuOffset(kOffX, kOffY);
    sceGuViewport(2048, 2048, GuAncho, GuAlto);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, GuAncho, GuAlto);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuDisable(GU_DITHER);
    sceGuShadeModel(GU_SMOOTH);
    sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuLightMode(GU_SINGLE_COLOR);
    {
        ScePspFMatrix4 id;
        memset(&id, 0, sizeof(id));
        id.x.x = id.y.y = id.z.z = id.w.w = 1.0f;
        sceGuSetMatrix(GU_VIEW, &id);
        sceGuSetMatrix(GU_TEXTURE, &id);
    }
    sceGuClearColor(0xFF000000u);
    sceGuClearDepth(0);
    sceGuClearStencil(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    gFbDibujo = kFbA;
    curProj.Identity(); curMV.Identity(); curTex.Identity();
    InicialLuces();
    OlvidarPuesto();
    gIniciado = true;
    w3dLogf("[gu] GE listo: 480x272 8888, z de 16 bits, lista %d KB, anillo %d KB", kListaBytes / 1024, kAnilloBytes / 1024);
    return true;
}
void GuFin() {
    if (!gIniciado) return;
    ListaCerrar();
    sceGuTerm();
    free(gLista); free(gAnillo);
    gLista = NULL; gAnillo = NULL;
    gIniciado = false;
}
void GuCuadroEmpezar() {
    ListaAbrir();
    gEnCuadro = true;
}
void GuCuadroTerminar() {
    gEnCuadro = false;
    if (!gListaAbierta) return;   // (no se dibujo nada: la pantalla queda como estaba)
    ListaCerrar();
    sceDisplayWaitVblankStart();
    gFbDibujo = sceGuSwapBuffers();
}

// ============================================================================
//  Helpers de matrices (column-major, convencion GL; el GE usa la misma disposicion)
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
// (cada cambio a una matriz sube su version: el draw manda al GE solo las que cambiaron)
static unsigned gVerMat[3] = { 1, 1, 1 };   // proyeccion, modelview, textura
static Matrix4& cur() {
    gVerMat[matMode == 0 ? 0 : (matMode == 2 ? 2 : 1)]++;
    return matMode == 0 ? curProj : (matMode == 2 ? curTex : curMV);
}

// ============================================================================
//  Capacidades
// ============================================================================
void Enable(Cap c) {
    switch (c) {
        case DepthTest: cap_depth = true; break;
        case CullFace: cap_cull = true; break;
        case Blend: cap_blend = true; break;
        case ScissorTest: cap_scissor = true; break;
        case Texture2D: cap_tex = true; break;
        case Lighting: cap_light = true; break;
        case Fog: cap_fog = true; break;
        case ColorMaterial: cap_colormat = true; lucesSucias = true; break;
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
        case ScissorTest: cap_scissor = false; break;
        case Texture2D: cap_tex = false; break;
        case Lighting: cap_light = false; break;
        case Fog: cap_fog = false; break;
        case ColorMaterial: cap_colormat = false; lucesSucias = true; break;
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
//  Viewport / scissor (en coordenadas de GL: y desde abajo)
// ============================================================================
void Viewport(int x, int y, int w, int h) { vpActual[0] = x; vpActual[1] = y; vpActual[2] = w < 0 ? 0 : w; vpActual[3] = h < 0 ? 0 : h; }
void Scissor(int x, int y, int w, int h) { scActual[0] = x; scActual[1] = y; scActual[2] = w < 0 ? 0 : w; scActual[3] = h < 0 ? 0 : h; }
void GetViewport(int v[4]) { for (int i = 0; i < 4; i++) v[i] = vpActual[i]; }
void GetScissor(int v[4]) { for (int i = 0; i < 4; i++) v[i] = scActual[i]; }
bool ScissorActivo() { return cap_scissor; }

// ============================================================================
//  Estado suelto
// ============================================================================
void ClearColor(float r, float g, float b, float a) { clearCol[0] = r; clearCol[1] = g; clearCol[2] = b; clearCol[3] = a; }
static void Limpiar(int bits);
void Clear(int bits) { Limpiar(bits); }
int  PlanosRecorteMax() { return 0; }
void PlanosRecorte(int, const float*) {}
int  EstencilBits() { return 8; }
void EstencilLimpiar(int v) { estencilLimpio = v; Limpiar(StencilBuffer); }
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
void DepthRange(float n, float f) { depthN = n; depthF = f; }
void SmoothShading(bool suave) { sombraSuave = suave; }
void FastPerspective() {}
void Wireframe(bool on) { wireframe = on; }
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
void Finish() { if (gListaAbierta) ListaVaciar(); }
void Flush() {}
void DibujarPendiente() {}
bool g_auditarEscena = false;
int  g_auditDesyncs = 0;
int  AuditarEstado() { return 0; }
void TexBaseLevel(int) {}
void GLES2Init(void* (*)(const char*)) {}

// ============================================================================
//  Luz / material (las luces se guardan como GL: posicion y cono ya en el OJO, con la modelview de AHORA)
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
    L.corte = corte;
    L.expo = exponente;
    lucesSucias = true;
}
void LightN(int i, bool on) { if (i < 0 || i >= LucesMax || luces[i].on == on) return; luces[i].on = on; lucesSucias = true; }
void Light0fv(LightFv p, const float* v) { LightNfv(0, p, v); }
void Light0f(LightF p, float v) { LightNf(0, p, v); }
void SetLightEnabled(unsigned int id, bool on) { if (id >= 0x4000u && id < 0x4000u + (unsigned)LucesMax) LightN((int)(id - 0x4000u), on); }
void LightModelAmbient(const float* c) { for (int k = 0; k < 4; k++) ambGlobal[k] = c[k]; lucesSucias = true; }
void LightModelTwoSide(bool) {}   // (el GE ilumina una cara)
void Material(MatParam p, const float* rgba) {
    float* d = (p == MatDiffuse) ? matDiff : (p == MatAmbient) ? matAmb : (p == MatSpecular) ? matSpec : matEmis;
    if (memcmp(d, rgba, sizeof(float) * 4) == 0) return;
    for (int i = 0; i < 4; i++) d[i] = rgba[i];
    lucesSucias = true;
}
void MaterialShininess(float s) { if (s != matShine) { matShine = s; lucesSucias = true; } }

// ============================================================================
//  Textura / texenv / matriz de textura (la matriz se aplica en la CPU, al leer la UV de cada vertice)
// ============================================================================
void BindTexture(unsigned int id) { boundTex = id; ::W3dGuBindTextura(id); }
void SombreadoPorPixel(bool) {}                 // (pipeline fijo: la luz es por vertice, siempre)
bool SombreadoPorPixelSoportado() { return false; }
// (sin shaders: los pases van por la CPU y el postprocesado tambien; ver graphics/w3dGraphics.h)
bool PasesConShader() { return false; }
void PaseEspecial(int, const float*) {}
void AnteriorPointer3f(const float*) {}
bool PostEfecto(int, unsigned int, unsigned int, int, int, const float*, const float*) { return false; }
bool ShadersEscritos() { return false; }
int  ShaderEscritoCompilar(const char*, const char*, char* log, int logMax) { if (log && logMax > 0) log[0] = 0; return 0; }
void ShaderEscritoBorrar(int) {}
void ShaderEscritoUsar(int, const float*) {}
void TexEnvDot3(bool on) { texEnv = on ? 2 : 0; }
void TexEnvReplace(bool on) { texEnv = on ? 1 : 0; }
void TexEnvAlphaOnly(bool on) { texEnv = on ? 3 : 0; }
// (el GE tiene una sola unidad de textura: el editor mezcla en varias pasadas)
bool MezclaTexturasSoportada() { return false; }
void MezclaTexturas(unsigned int, float, bool, bool, int) {}
void MezclaTexturasOff() {}
bool MezclaTexturasActiva() { return false; }
void MezclaTexturasUV(const float*) {}
void MezclaTexturasUVVBO(unsigned int) {}
void MezclaTexturasUV2s(const short*) {}
void MezclaTexturasMatrizUV(float, float, float, float) {}
void MezclaTexturaBorrada(unsigned int) {}
void TexGenSphere(bool on) {
    if (texGenCache == (on ? 1 : 0)) return;
    texGenCache = on ? 1 : 0;
    matcapCache = -1;
    curTex.Identity(); gVerMat[2]++;
    if (on) { curTex.m[5] = -1.0f; curTex.m[13] = 1.0f; }
}
void TexMatrixMatcap(bool on) {
    if (!on && matcapCache == 0) return;
    matcapCache = on ? 1 : 0;
    curTex.Identity(); gVerMat[2]++;
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
    curTex.Identity(); gVerMat[2]++;
    curTex.m[0] = su; curTex.m[5] = sv; curTex.m[12] = du; curTex.m[13] = dv;
}
bool TieneTexGen() { return false; }   // (el sphere map va por software, como en la Dreamcast)

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
void VertexPointer4f(int s, const float* p) { aPos.tam = 4; aPos.tipo = TipoFloat; aPos.paso = s; aPos.ptr = p; }
void VertexPointer3s(int s, const short* p) { aPos.tam = 3; aPos.tipo = TipoShort; aPos.paso = s; aPos.ptr = p; }
void VertexPointer2s(int s, const short* p) { aPos.tam = 2; aPos.tipo = TipoShort; aPos.paso = s; aPos.ptr = p; }
void NormalPointer3b(const signed char* p) { aNrm.tam = 3; aNrm.tipo = TipoByte; aNrm.paso = 0; aNrm.ptr = p; }
void ColorPointer4ub(const unsigned char* p) { aCol.tam = 4; aCol.tipo = TipoUByte; aCol.paso = 0; aCol.ptr = p; }
void ColorPointer4f(int s, const float* p) { aCol.tam = 4; aCol.tipo = TipoFloat; aCol.paso = s; aCol.ptr = p; }
void TexCoordPointer2f(int s, const float* p) { aUV.tam = 2; aUV.tipo = TipoFloat; aUV.paso = s; aUV.ptr = p; }
void TexCoordPointer2s(int s, const short* p) { aUV.tam = 2; aUV.tipo = TipoShort; aUV.paso = s; aUV.ptr = p; }
void TexCoordPointer3b(const signed char* p, int) { aUV.tam = 3; aUV.tipo = TipoByte; aUV.paso = 0; aUV.ptr = p; }

// ============================================================================
//  EL ESTADO DEL GE ANTES DE CADA DRAW: se calcula a que tiene que quedar (sale del estado emulado de GL) y se manda
//  solo lo que cambio desde el draw anterior
// ============================================================================
static inline unsigned Byte(float f) { return f <= 0.0f ? 0u : f >= 1.0f ? 255u : (unsigned)(f * 255.0f + 0.5f); }
static inline unsigned ColorGE(const float* c) { return Byte(c[0]) | (Byte(c[1]) << 8) | (Byte(c[2]) << 16) | (Byte(c[3]) << 24); }
static inline unsigned ColorGE(const float* c, float a) { return Byte(c[0]) | (Byte(c[1]) << 8) | (Byte(c[2]) << 16) | (Byte(a) << 24); }

struct Puesto {
    int base;   // (lo que queda prendido siempre: el test de z y el de estencil)
    int luz, tex, mezcla, alfa, zFunc, zMask, cull, estencil, estencilValor, mascara, niebla, sombra, texFunc;
    unsigned texEnvColor;
    unsigned texId, texVer;
    float nieblaIni, nieblaFin; unsigned nieblaColor;
    unsigned verProj, verMV;
    int vp[4], sc[4];
    float zEscala, zCentro;
};
static Puesto gP;
static void OlvidarPuesto() {
    memset(&gP, 0xFF, sizeof(gP));   // (nada coincide: lo proximo se manda todo)
    gP.zEscala = gP.zCentro = -1e30f;
    gP.nieblaIni = gP.nieblaFin = -1e30f;
    lucesSucias = true;
}
// el GE: un cambio, con el valor que quedo puesto
#define W3D_GU_PONER(campo, valor, accion) do { if (gP.campo != (valor)) { gP.campo = (valor); accion; } } while (0)

static void PonerEnable(int cap, bool on) { if (on) sceGuEnable(cap); else sceGuDisable(cap); }

// el z de GL (de -1 a 1, con su DepthRange) al del GE (65535 cerca, 0 lejos); el polygon offset corre el centro.
// Lo usan tambien las lineas anchas y los puntos, que van armados en pantalla
// EL POLYGON OFFSET de GL es factor * pendiente + unidades * el minimo que distingue el z. Con un z de 16 bits, una unidad
// es un nivel del z; la pendiente (cuanto cambia el z de un pixel al de al lado) depende de cada triangulo y el GE no la
// calcula: va una tipica de las caras de una malla en el visor, 2 niveles. Mas que eso empujaba las caras tanto que las
// aristas y la grilla de ATRAS se veian a traves
static float gZEscala = -32767.5f, gZCentro = 32767.5f;
static const float kZPorFactor = 2.0f, kZPorUnidad = 1.0f;
static void CalcularZ() {
    gZEscala = -65535.0f * (depthF - depthN) * 0.5f;
    gZCentro = 65535.0f * (1.0f - (depthF + depthN) * 0.5f);
    if (cap_offset) gZCentro -= kZPorFactor * polyF + kZPorUnidad * polyU;
}
static void PonerZ() {
    CalcularZ();
    if (gP.zEscala != gZEscala || gP.zCentro != gZCentro) {
        gP.zEscala = gZEscala; gP.zCentro = gZCentro;
        sceGuSendCommandf(0x44, gZEscala);   // (VIEWPORT_Z_SCALE)
        sceGuSendCommandf(0x47, gZCentro);   // (VIEWPORT_Z_CENTER)
    }
}
static void PonerViewport() {
    if (gP.vp[0] == vpActual[0] && gP.vp[1] == vpActual[1] && gP.vp[2] == vpActual[2] && gP.vp[3] == vpActual[3]) return;
    for (int i = 0; i < 4; i++) gP.vp[i] = vpActual[i];
    const float w = (float)vpActual[2], h = (float)vpActual[3];
    sceGuSendCommandf(0x42, w * 0.5f);                                           // (VIEWPORT_X_SCALE)
    sceGuSendCommandf(0x43, -h * 0.5f);                                          // (VIEWPORT_Y_SCALE: la y del GE baja)
    sceGuSendCommandf(0x45, (float)kOffX + (float)vpActual[0] + w * 0.5f);      // (VIEWPORT_X_CENTER)
    sceGuSendCommandf(0x46, (float)kOffY + (float)(GuAlto - vpActual[1]) - h * 0.5f);   // (VIEWPORT_Y_CENTER)
}
// el scissor del GE: el de GL (si esta prendido) y, para dibujar, tambien el viewport (GL recorta ahi). false si no
// queda nada: no se dibuja
static bool PonerScissor(bool paraLimpiar) {
    int x0 = 0, y0 = 0, x1 = GuAncho, y1 = GuAlto;   // (en GL: y desde abajo)
    if (!paraLimpiar) {
        if (vpActual[0] > x0) x0 = vpActual[0];
        if (vpActual[1] > y0) y0 = vpActual[1];
        if (vpActual[0] + vpActual[2] < x1) x1 = vpActual[0] + vpActual[2];
        if (vpActual[1] + vpActual[3] < y1) y1 = vpActual[1] + vpActual[3];
    }
    if (cap_scissor) {
        if (scActual[0] > x0) x0 = scActual[0];
        if (scActual[1] > y0) y0 = scActual[1];
        if (scActual[0] + scActual[2] < x1) x1 = scActual[0] + scActual[2];
        if (scActual[1] + scActual[3] < y1) y1 = scActual[1] + scActual[3];
    }
    if (x1 <= x0 || y1 <= y0) return false;
    const int sc[4] = { x0, GuAlto - y1, x1 - x0, y1 - y0 };   // (en el GE: y desde arriba)
    if (gP.sc[0] != sc[0] || gP.sc[1] != sc[1] || gP.sc[2] != sc[2] || gP.sc[3] != sc[3]) {
        for (int i = 0; i < 4; i++) gP.sc[i] = sc[i];
        sceGuScissor(sc[0], sc[1], sc[2], sc[3]);
    }
    return true;
}
static int FuncZ() {
    switch (depthFunc) {
        case DepthLEqual: return GU_GEQUAL;
        case DepthEqual: return GU_EQUAL;
        case DepthAlways: return GU_ALWAYS;
        default: return GU_GREATER;
    }
}
static void PonerMezcla(int m) {
    if (m == MezclaOff) { sceGuDisable(GU_BLEND); return; }
    sceGuEnable(GU_BLEND);
    switch (m) {
        case MezclaAdd:      sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF); break;
        case MezclaAddAlpha: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFF); break;
        case MezclaMultiply: sceGuBlendFunc(GU_ADD, GU_OTHER_COLOR, GU_FIX, 0, 0); break;              // (DST_COLOR, ZERO)
        case MezclaScreen:   sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_OTHER_COLOR, 0xFFFFFF, 0); break;   // (ONE, 1 - SRC_COLOR)
        case MezclaPremult:  sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0xFFFFFF, 0); break;
        case MezclaSubtract: sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF); break;
        case 100:            sceGuBlendFunc(GU_ADD, GU_OTHER_COLOR, GU_OTHER_COLOR, 0, 0); break;     // (DST_COLOR, SRC_COLOR)
        default:             sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0); break;
    }
}
// las luces del GE (cuatro): las primeras prendidas de GL. El material con la cuenta del pipeline fijo: emisiva +
// ambiente global * ambiente + por luz (ambiente + difusa * N.L + especular); el alfa es el del ambiente (el del difuso
// en GL). Con COLOR_MATERIAL el ambiente y el difuso salen del color del vertice (que siempre esta: el actual si no
// hay arreglo de color)
static void PonerLuces() {
    sceGuAmbient(ColorGE(ambGlobal, 1.0f));
    sceGuColorMaterial(cap_colormat ? (GU_AMBIENT | GU_DIFFUSE) : 0);
    sceGuSendCommandi(0x54, (int)(ColorGE(matEmis) & 0xFFFFFF));   // (MATERIAL_EMISSIVE)
    sceGuMaterial(GU_AMBIENT, (int)ColorGE(matAmb, matDiff[3]));
    sceGuMaterial(GU_DIFFUSE, (int)ColorGE(matDiff));
    sceGuMaterial(GU_SPECULAR, (int)ColorGE(matSpec));
    sceGuSpecular(matShine > 0.0f ? matShine : 0.0f);
    const bool especular = matSpec[0] > 0.0f || matSpec[1] > 0.0f || matSpec[2] > 0.0f;
    int hw = 0;
    for (int i = 0; i < LucesMax && hw < 4; i++) {
        const Luz& L = luces[i];
        if (!L.on) continue;
        ScePspFVector3 p = { L.pos[0], L.pos[1], L.pos[2] };
        int tipo = GU_POINTLIGHT;
        if (L.pos[3] == 0.0f) {
            tipo = GU_DIRECTIONAL;
            const float l = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
            if (l > 1e-6f) { p.x /= l; p.y /= l; p.z /= l; }
        } else if (L.corte < 180.0f) tipo = GU_SPOTLIGHT;
        sceGuLight(hw, tipo, especular ? GU_DIFFUSE_AND_SPECULAR : GU_AMBIENT_AND_DIFFUSE, &p);
        sceGuLightColor(hw, GU_AMBIENT, ColorGE(L.amb) & 0xFFFFFF);
        sceGuLightColor(hw, GU_DIFFUSE, ColorGE(L.dif) & 0xFFFFFF);
        sceGuLightColor(hw, GU_SPECULAR, ColorGE(L.spec) & 0xFFFFFF);
        sceGuLightAtt(hw, L.att[0], L.att[1], L.att[2]);
        if (tipo == GU_SPOTLIGHT) {
            ScePspFVector3 d = { L.dir[0], L.dir[1], L.dir[2] };
            sceGuLightSpot(hw, &d, L.expo, cosf(L.corte * 0.01745329252f));
        }
        sceGuEnable(GU_LIGHT0 + hw);
        hw++;
    }
    for (; hw < 4; hw++) sceGuDisable(GU_LIGHT0 + hw);
    lucesSucias = false;
}
// la textura que va (NULL: sin textura)
static const w3dGu::Textura* TexturaActiva() {
    if (!cap_tex || !boundTex) return NULL;
    return w3dGu::TexturaDe(boundTex);
}
// todo el estado. 'en2D': lo armado en pantalla (lineas anchas, puntos): sin luz, niebla ni culling
static void PonerEstado(bool en2D, const w3dGu::Textura* tex) {
    PonerViewport();
    PonerZ();
    // (el test de z siempre prendido: el "sin test" de GL es ALWAYS sin escribir. El de estencil tambien: ver arriba)
    W3D_GU_PONER(base, 1, { sceGuEnable(GU_DEPTH_TEST); sceGuEnable(GU_STENCIL_TEST); });
    W3D_GU_PONER(zFunc, cap_depth ? FuncZ() : GU_ALWAYS, sceGuDepthFunc(gP.zFunc));
    W3D_GU_PONER(zMask, (cap_depth && depthMask) ? 0 : 1, sceGuDepthMask(gP.zMask));
    // culling: GL_BACK descarta las de atras (con el winding elegido). El GE mide el giro como GL (antes de dar vuelta
    // la y de la pantalla): el frente en contra del reloj de GL es GU_CCW ("no se descartan las que giran en contra")
    int cull = -1;
    if (cap_cull && !en2D) cull = (frenteCCW != cullFrente) ? GU_CCW : GU_CW;
    W3D_GU_PONER(cull, cull, { PonerEnable(GU_CULL_FACE, cull >= 0); if (cull >= 0) sceGuFrontFace(cull); });
    W3D_GU_PONER(mezcla, cap_blend ? mezclaFirma : (int)MezclaOff, PonerMezcla(gP.mezcla));
    const int alfa = alphaRef > 0.0f ? (int)Byte(alphaRef) : -1;
    W3D_GU_PONER(alfa, alfa, { PonerEnable(GU_ALPHA_TEST, alfa >= 0); if (alfa >= 0) sceGuAlphaFunc(GU_GEQUAL, alfa, 0xFF); });
    const int ev = estencil ? estencilValor : 0;
    if (gP.estencil != estencil || gP.estencilValor != ev) {
        gP.estencil = estencil; gP.estencilValor = ev;
        if (estencil == 1) { sceGuStencilFunc(GU_ALWAYS, ev, 0xFF); sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE); }
        else if (estencil == 2) { sceGuStencilFunc(GU_EQUAL, ev, 0xFF); sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP); }
        else { sceGuStencilFunc(GU_ALWAYS, 0, 0xFF); sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP); }
    }
    // (sin color: el alfa sigue escribible, que es el estencil)
    W3D_GU_PONER(mascara, colorMask ? 0 : 1, sceGuPixelMask(gP.mascara ? 0x00FFFFFFu : 0u));
    W3D_GU_PONER(sombra, sombraSuave ? GU_SMOOTH : GU_FLAT, sceGuShadeModel(gP.sombra));
    // textura
    W3D_GU_PONER(tex, tex ? 1 : 0, PonerEnable(GU_TEXTURE_2D, tex != NULL));
    if (tex) {
        if (gP.texId != boundTex || gP.texVer != tex->version) {
            gP.texId = boundTex; gP.texVer = tex->version;
            sceGuTexMode(tex->psm, 0, 0, GU_TRUE);
            sceGuTexImage(0, tex->w, tex->h, tex->w, tex->datos);
            sceGuTexFilter(tex->filtro ? GU_LINEAR : GU_NEAREST, tex->filtro ? GU_LINEAR : GU_NEAREST);
            sceGuTexWrap(tex->repetir ? GU_REPEAT : GU_CLAMP, tex->repetir ? GU_REPEAT : GU_CLAMP);
            sceGuTexFlush();
            g_statTexBinds++;
        }
        // solo alfa (el texto, las mascaras): el color es el del vertice. BLEND del GE: color = vertice * (1 - tex) +
        // constante * tex, con la constante = el color actual (el del vertice cuando no hay arreglo de color)
        const int tf = (texEnv == 1) ? GU_TFX_REPLACE : (texEnv == 3) ? GU_TFX_BLEND : GU_TFX_MODULATE;
        W3D_GU_PONER(texFunc, tf, sceGuTexFunc(tf, GU_TCC_RGBA));
        if (tf == GU_TFX_BLEND) W3D_GU_PONER(texEnvColor, ColorGE(curColor) & 0xFFFFFF, sceGuTexEnvColor(gP.texEnvColor));
    }
    // luz
    const bool luz = cap_light && !en2D && !(tex && texEnv == 1);
    W3D_GU_PONER(luz, luz ? 1 : 0, PonerEnable(GU_LIGHTING, luz));
    if (luz && lucesSucias) PonerLuces();
    // niebla (el GE solo la tiene lineal: la exponencial se aproxima con una lineal que llega al fondo donde la
    // exponencial deja un 5%)
    const bool niebla = cap_fog && !en2D;
    W3D_GU_PONER(niebla, niebla ? 1 : 0, PonerEnable(GU_FOG, niebla));
    if (niebla) {
        float ini = fogStart, fin = fogEnd;
        if (fogModo == 1) { ini = 0.0f; fin = 3.0f / (fogDens > 1e-6f ? fogDens : 1e-6f); }
        else if (fogModo == 2) { ini = 0.0f; fin = 1.73f / (fogDens > 1e-6f ? fogDens : 1e-6f); }
        const unsigned c = ColorGE(fogColor) & 0xFFFFFF;
        if (gP.nieblaIni != ini || gP.nieblaFin != fin || gP.nieblaColor != c) {
            gP.nieblaIni = ini; gP.nieblaFin = fin; gP.nieblaColor = c;
            sceGuFog(ini, fin, c);
        }
    }
    // matrices (las del modo 2D no se usan)
    if (!en2D) {
        if (gP.verProj != gVerMat[0]) { gP.verProj = gVerMat[0]; sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4*)curProj.m); }
        if (gP.verMV != gVerMat[1]) { gP.verMV = gVerMat[1]; sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4*)curMV.m); }
    }
    g_statStateChanges++;
}

// ============================================================================
//  LOS VERTICES: cada uno se lee de los arreglos de GL al formato del GE (UV, color, normal y posicion del objeto) y
//  se pasa al espacio de recorte para ver de que lado de cada borde queda
// ============================================================================
struct VertGE { float u, v; unsigned int c; float nx, ny, nz; float x, y, z; };
static const int kTipoGE = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
struct Vert2D { float u, v; unsigned int c; float x, y, z; };
static const int kTipo2D = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D;

static std::vector<VertGE> gV;          // los vertices que usa el draw
static std::vector<float> gC;           // su posicion en el espacio de recorte (x, y, z, w)
static std::vector<unsigned char> gOC;  // de que bordes quedan afuera (Codigo)
static std::vector<int> gIdx;           // los vertices del draw, en orden, como posiciones en gV
static std::vector<VertGE> gTri, gLin;  // lo armado: triangulos y lineas de un pixel (3D)
static std::vector<Vert2D> gTri2D, gSpr; // lo armado en pantalla: lineas anchas (triangulos) y puntos (sprites)

// EL BORDE DEL ESPACIO DEL GE: el viewport en el medio de 4096x4096; un vertice que sale de ahi descarta su triangulo.
// En NDC, el borde queda a gBx (gBy) veces la mitad del viewport (con margen)
static float gBx = 7.0f, gBy = 7.0f;
static float PM[16];                    // proyeccion * modelview (lo que hace el GE con la posicion)
static unsigned gPMProj = 0, gPMMV = 0;
static void PrepararRecorte() {
    if (gPMProj != gVerMat[0] || gPMMV != gVerMat[1]) {
        Matrix4 m = curProj * curMV;
        memcpy(PM, m.m, sizeof(PM));
        gPMProj = gVerMat[0]; gPMMV = gVerMat[1];
    }
    const float w = vpActual[2] > 1 ? (float)vpActual[2] : 1.0f, h = vpActual[3] > 1 ? (float)vpActual[3] : 1.0f;
    gBx = 3400.0f / w; gBy = 3400.0f / h;
}
enum { BordeCerca = 1, BordeLejos = 2, BordeDer = 4, BordeIzq = 8, BordeArr = 16, BordeAba = 32 };
static inline unsigned char Codigo(const float* c) {
    unsigned char k = 0;
    const float w = c[3];
    if (c[2] < -w) k |= BordeCerca;
    if (c[2] > w) k |= BordeLejos;
    if (c[0] > gBx * w) k |= BordeDer;
    if (c[0] < -gBx * w) k |= BordeIzq;
    if (c[1] > gBy * w) k |= BordeArr;
    if (c[1] < -gBy * w) k |= BordeAba;
    return k;
}
// la distancia firmada al borde (adentro >= 0)
static inline float Distancia(int borde, const float* c) {
    switch (borde) {
        case BordeCerca: return c[2] + c[3];
        case BordeLejos: return c[3] - c[2];
        case BordeDer:   return gBx * c[3] - c[0];
        case BordeIzq:   return gBx * c[3] + c[0];
        case BordeArr:   return gBy * c[3] - c[1];
        default:         return gBy * c[3] + c[1];
    }
}

// de donde sale cada atributo en este draw
struct Fuente {
    const unsigned char* pos; int pasoPos, tamPos, tipoPos;
    const signed char* nrm;
    const unsigned char* uv; int pasoUV, tamUV, tipoUV;
    const unsigned char* col; int pasoCol, tipoCol;
    unsigned colorFijo;
    bool texMat;
};
static Fuente gF;
static int Bytes(int tipo) { return tipo == TipoFloat ? 4 : (tipo == TipoShort ? 2 : 1); }
static bool PrepararFuente() {
    if (!aPos.on || !aPos.ptr) return false;
    Fuente& f = gF;
    f.pos = (const unsigned char*)aPos.ptr; f.tamPos = aPos.tam; f.tipoPos = aPos.tipo;
    f.pasoPos = aPos.paso ? aPos.paso : aPos.tam * Bytes(aPos.tipo);
    f.nrm = (aNrm.on && aNrm.ptr) ? (const signed char*)aNrm.ptr : NULL;
    f.uv = (aUV.on && aUV.ptr) ? (const unsigned char*)aUV.ptr : NULL;
    f.tamUV = aUV.tam; f.tipoUV = aUV.tipo; f.pasoUV = aUV.paso ? aUV.paso : aUV.tam * Bytes(aUV.tipo);
    f.col = (aCol.on && aCol.ptr) ? (const unsigned char*)aCol.ptr : NULL;
    f.tipoCol = aCol.tipo; f.pasoCol = aCol.paso ? aCol.paso : (aCol.tipo == TipoFloat ? 16 : 4);
    f.colorFijo = ColorGE(curColor);
    f.texMat = false;
    for (int i = 0; i < 16 && !f.texMat; i++) f.texMat = curTex.m[i] != ((i % 5) == 0 ? 1.0f : 0.0f);
    PrepararRecorte();
    return true;
}
// el vertice i de los arreglos: al formato del GE y al espacio de recorte
static inline void Leer(int i, VertGE& o, float* c) {
    const Fuente& f = gF;
    const unsigned char* p = f.pos + (size_t)i * f.pasoPos;
    float x, y, z = 0.0f;
    if (f.tipoPos == TipoShort) { const short* q = (const short*)p; x = q[0]; y = q[1]; if (f.tamPos > 2) z = q[2]; }
    else {
        const float* q = (const float*)p; x = q[0]; y = q[1];
        if (f.tamPos > 2) z = q[2];
        if (f.tamPos > 3 && q[3] != 0.0f && q[3] != 1.0f) { const float iw = 1.0f / q[3]; x *= iw; y *= iw; z *= iw; }
    }
    o.x = x; o.y = y; o.z = z;
    if (f.nrm) { const signed char* n = f.nrm + (size_t)i * 3; o.nx = n[0] * (1.0f / 127.0f); o.ny = n[1] * (1.0f / 127.0f); o.nz = n[2] * (1.0f / 127.0f); }
    else { o.nx = 0.0f; o.ny = 0.0f; o.nz = 1.0f; }
    if (f.uv) {
        const unsigned char* t = f.uv + (size_t)i * f.pasoUV;
        float s, tt, r = 0.0f;
        if (f.tipoUV == TipoFloat) { const float* q = (const float*)t; s = q[0]; tt = q[1]; }
        else if (f.tipoUV == TipoShort) { const short* q = (const short*)t; s = q[0]; tt = q[1]; }
        else { const signed char* q = (const signed char*)t; s = q[0]; tt = q[1]; if (f.tamUV > 2) r = q[2]; }
        if (f.texMat) {
            const float* T = curTex.m;
            o.u = T[0] * s + T[4] * tt + T[8] * r + T[12];
            o.v = T[1] * s + T[5] * tt + T[9] * r + T[13];
        } else { o.u = s; o.v = tt; }
    } else { o.u = 0.0f; o.v = 0.0f; }
    if (f.col) {
        const unsigned char* q = f.col + (size_t)i * f.pasoCol;
        if (f.tipoCol == TipoUByte) o.c = (unsigned)q[0] | ((unsigned)q[1] << 8) | ((unsigned)q[2] << 16) | ((unsigned)q[3] << 24);
        else o.c = ColorGE((const float*)q);
    } else o.c = f.colorFijo;
    c[0] = PM[0] * x + PM[4] * y + PM[8] * z + PM[12];
    c[1] = PM[1] * x + PM[5] * y + PM[9] * z + PM[13];
    c[2] = PM[2] * x + PM[6] * y + PM[10] * z + PM[14];
    c[3] = PM[3] * x + PM[7] * y + PM[11] * z + PM[15];
}
// los vertices del draw: 'idx' (o desde 'primero' si no hay indices), n. Si los indices caen en un rango chico se lee
// cada vertice una vez; si no (unos pocos indices de una malla grande), cada indice por su lado
template <typename T> static bool LeerDraw(const T* idx, int primero, int n) {
    if (n <= 0 || !PrepararFuente()) return false;
    gIdx.resize((size_t)n);
    int lo = primero, cant = n;
    bool porIndice = false;
    if (idx) {
        int a = 0x7fffffff, b = -1;
        for (int i = 0; i < n; i++) { const int k = (int)idx[i]; if (k < a) a = k; if (k > b) b = k; }
        if (b < 0) return false;
        lo = a; cant = b - a + 1;
        if (cant > 2 * n + 64) { porIndice = true; cant = n; }
    }
    gV.resize((size_t)cant); gC.resize((size_t)cant * 4); gOC.resize((size_t)cant);
    for (int j = 0; j < cant; j++) {
        const int i = porIndice ? (int)idx[j] : lo + j;
        Leer(i, gV[j], &gC[(size_t)j * 4]);
        gOC[j] = Codigo(&gC[(size_t)j * 4]);
    }
    for (int j = 0; j < n; j++) gIdx[j] = porIndice ? j : (idx ? (int)idx[j] - lo : j);
    return true;
}

// ---- el recorte en la CPU ----
struct VertRec { VertGE v; float c[4]; };
static inline void Mezclar(const VertRec& a, const VertRec& b, float t, VertRec& o) {
    o.v.u = a.v.u + (b.v.u - a.v.u) * t; o.v.v = a.v.v + (b.v.v - a.v.v) * t;
    o.v.nx = a.v.nx + (b.v.nx - a.v.nx) * t; o.v.ny = a.v.ny + (b.v.ny - a.v.ny) * t; o.v.nz = a.v.nz + (b.v.nz - a.v.nz) * t;
    o.v.x = a.v.x + (b.v.x - a.v.x) * t; o.v.y = a.v.y + (b.v.y - a.v.y) * t; o.v.z = a.v.z + (b.v.z - a.v.z) * t;
    unsigned c = 0;
    for (int k = 0; k < 32; k += 8) {
        const float ca = (float)((a.v.c >> k) & 0xFF), cb = (float)((b.v.c >> k) & 0xFF);
        int r = (int)(ca + (cb - ca) * t + 0.5f); if (r < 0) r = 0; if (r > 255) r = 255;
        c |= (unsigned)r << k;
    }
    o.v.c = c;
    for (int k = 0; k < 4; k++) o.c[k] = a.c[k] + (b.c[k] - a.c[k]) * t;
}
static inline void Rec(int j, VertRec& o) { o.v = gV[j]; memcpy(o.c, &gC[(size_t)j * 4], sizeof(o.c)); }
// un poligono contra los bordes de 'mascara' (Sutherland-Hodgman). Devuelve cuantos vertices quedaron
static int RecortarPoligono(VertRec* p, int n, unsigned mascara) {
    VertRec tmp[16];
    for (int borde = 1; borde <= BordeAba; borde <<= 1) {
        if (!(mascara & (unsigned)borde) || n < 3) continue;
        int m = 0;
        for (int i = 0; i < n && m < 15; i++) {
            const VertRec& a = p[i];
            const VertRec& b = p[(i + 1) % n];
            const float da = Distancia(borde, a.c), db = Distancia(borde, b.c);
            if (da >= 0.0f) tmp[m++] = a;
            if ((da >= 0.0f) != (db >= 0.0f) && m < 15) Mezclar(a, b, da / (da - db), tmp[m++]);
        }
        for (int i = 0; i < m; i++) p[i] = tmp[i];
        n = m;
    }
    return n;
}

// ---- armar: triangulos, lineas y puntos ----
static void ArmarLinea(int a, int b);
static void ArmarTriangulo(int a, int b, int c) {
    if (wireframe) { ArmarLinea(a, b); ArmarLinea(b, c); ArmarLinea(c, a); return; }
    const unsigned oa = gOC[a], ob = gOC[b], oc = gOC[c];
    if (!(oa | ob | oc)) { gTri.push_back(gV[a]); gTri.push_back(gV[b]); gTri.push_back(gV[c]); return; }
    if (oa & ob & oc) return;   // (entero afuera de un mismo borde)
    VertRec p[16];
    Rec(a, p[0]); Rec(b, p[1]); Rec(c, p[2]);
    const int n = RecortarPoligono(p, 3, oa | ob | oc);
    for (int i = 1; i + 1 < n; i++) { gTri.push_back(p[0].v); gTri.push_back(p[i].v); gTri.push_back(p[i + 1].v); }
}
// de clip a pantalla (pixeles del GE, y hacia abajo) y al z del GE
static inline void APantalla(const float* c, float& sx, float& sy, float& sz) {
    const float iw = 1.0f / c[3];
    sx = (float)vpActual[0] + (c[0] * iw + 1.0f) * 0.5f * (float)vpActual[2];
    sy = (float)GuAlto - ((float)vpActual[1] + (c[1] * iw + 1.0f) * 0.5f * (float)vpActual[3]);
    sz = gZCentro + gZEscala * c[2] * iw;
    if (sz < 0.0f) sz = 0.0f; if (sz > 65535.0f) sz = 65535.0f;
}
static float gTexW = 1.0f, gTexH = 1.0f;   // (el modo 2D toma la UV en texeles)
static void ArmarLinea(int a, int b) {
    const unsigned oa = gOC[a], ob = gOC[b];
    if (oa & ob) return;
    VertRec p[2];
    Rec(a, p[0]); Rec(b, p[1]);
    if (oa | ob) {
        float t0 = 0.0f, t1 = 1.0f;
        for (int borde = 1; borde <= BordeAba; borde <<= 1) {
            if (!((oa | ob) & (unsigned)borde)) continue;
            const float da = Distancia(borde, p[0].c), db = Distancia(borde, p[1].c);
            if (da < 0.0f && db < 0.0f) return;
            if (da < 0.0f) { const float t = da / (da - db); if (t > t0) t0 = t; }
            else if (db < 0.0f) { const float t = da / (da - db); if (t < t1) t1 = t; }
        }
        if (t0 > t1) return;
        VertRec q0 = p[0], q1 = p[1];
        if (t0 > 0.0f) Mezclar(p[0], p[1], t0, q0);
        if (t1 < 1.0f) Mezclar(p[0], p[1], t1, q1);
        p[0] = q0; p[1] = q1;
    }
    if (lineWidth <= 1.0f) { gLin.push_back(p[0].v); gLin.push_back(p[1].v); return; }
    // ANCHA: un rectangulo en pantalla, del ancho de la linea
    float x0, y0, z0, x1, y1, z1;
    APantalla(p[0].c, x0, y0, z0); APantalla(p[1].c, x1, y1, z1);
    float dx = x1 - x0, dy = y1 - y0;
    const float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-4f) return;
    const float h = lineWidth * 0.5f;
    const float nx = -dy / l * h, ny = dx / l * h;
    Vert2D q[4];
    for (int k = 0; k < 4; k++) {
        const VertRec& r = p[k < 2 ? 0 : 1];
        const float s = (k & 1) ? -1.0f : 1.0f;
        q[k].u = r.v.u * gTexW; q[k].v = r.v.v * gTexH; q[k].c = r.v.c;
        q[k].x = (k < 2 ? x0 : x1) + nx * s; q[k].y = (k < 2 ? y0 : y1) + ny * s; q[k].z = k < 2 ? z0 : z1;
    }
    gTri2D.push_back(q[0]); gTri2D.push_back(q[1]); gTri2D.push_back(q[2]);
    gTri2D.push_back(q[1]); gTri2D.push_back(q[3]); gTri2D.push_back(q[2]);
}
static void ArmarPunto(int a) {
    if (gOC[a]) return;   // (el punto afuera no se ve: GL descarta el punto entero)
    float sx, sy, sz;
    APantalla(&gC[(size_t)a * 4], sx, sy, sz);
    const float h = (pointSize > 1.0f ? pointSize : 1.0f) * 0.5f;
    const VertGE& v = gV[a];
    Vert2D q[2];
    q[0].x = sx - h; q[0].y = sy - h; q[1].x = sx + h; q[1].y = sy + h;
    q[0].z = q[1].z = sz;
    q[0].c = q[1].c = v.c;
    if (pointSprite) { q[0].u = 0.0f; q[0].v = 0.0f; q[1].u = gTexW; q[1].v = gTexH; }   // (la textura entera, de arriba abajo)
    else { q[0].u = q[1].u = v.u * gTexW; q[0].v = q[1].v = v.v * gTexH; }
    gSpr.push_back(q[0]); gSpr.push_back(q[1]);
}

// ---- lo armado, al GE ----
template <typename V> static void Mandar(int prim, int tipo, std::vector<V>& v, int porPrim) {
    if (v.empty()) return;
    const int maxPorDraw = (65535 / porPrim) * porPrim;
    const int total = (int)v.size();
    for (int i = 0; i < total; i += maxPorDraw) {
        const int n = (total - i) < maxPorDraw ? (total - i) : maxPorDraw;
        const int bytes = n * (int)sizeof(V);
        void* d = AnilloPedir(bytes);
        if (!d) return;
        memcpy(d, &v[(size_t)i], (size_t)bytes);
        sceKernelDcacheWritebackRange(d, (unsigned)bytes);   // (el GE lee la memoria, no el cache de la CPU)
        sceGuDrawArray(prim, tipo, n, NULL, d);
        g_statDrawTris++; g_statDcCat[gStatCat]++;
    }
    v.clear();
}
static void MandarTodo() {
    if (gTri.empty() && gLin.empty() && gTri2D.empty() && gSpr.empty()) return;
    ListaEspacio();
    if (!PonerScissor(false)) { gTri.clear(); gLin.clear(); gTri2D.clear(); gSpr.clear(); return; }
    const w3dGu::Textura* tex = TexturaActiva();
    if (!gTri.empty() || !gLin.empty()) {
        PonerEstado(false, tex);
        const int nt = (int)gTri.size() / 3;
        if (cap_blend) g_statTrisBlend += nt; else g_statTrisOpacos += nt;
        Mandar(GU_TRIANGLES, kTipoGE, gTri, 3);
        Mandar(GU_LINES, kTipoGE, gLin, 2);
    }
    if (!gTri2D.empty() || !gSpr.empty()) {
        PonerEstado(true, tex);
        Mandar(GU_TRIANGLES, kTipo2D, gTri2D, 3);
        Mandar(GU_SPRITES, kTipo2D, gSpr, 2);
    }
}
static void PrepararTexturaPantalla() {
    const w3dGu::Textura* t = TexturaActiva();
    gTexW = t ? (float)t->w : 1.0f; gTexH = t ? (float)t->h : 1.0f;
    CalcularZ();
}

// ============================================================================
//  Los draws
// ============================================================================
enum { ModoLista = 0, ModoTira, ModoAbanico };
template <typename T> static void Triangulos(const T* idx, int primero, int n, int modo) {
    if (!gIniciado || n < 3 || !LeerDraw(idx, primero, n)) return;
    PrepararTexturaPantalla();
    const int* I = &gIdx[0];
    if (modo == ModoLista) for (int i = 0; i + 2 < n; i += 3) ArmarTriangulo(I[i], I[i + 1], I[i + 2]);
    else if (modo == ModoTira) for (int i = 0; i + 2 < n; i++) { if (i & 1) ArmarTriangulo(I[i + 1], I[i], I[i + 2]); else ArmarTriangulo(I[i], I[i + 1], I[i + 2]); }
    else for (int i = 1; i + 1 < n; i++) ArmarTriangulo(I[0], I[i], I[i + 1]);
    g_statIndices += n;
    MandarTodo();
}
template <typename T> static void Lineas(const T* idx, int primero, int n, bool tira, bool lazo) {
    if (!gIniciado || n < 2 || !LeerDraw(idx, primero, n)) return;
    PrepararTexturaPantalla();
    const int* I = &gIdx[0];
    if (!tira) for (int i = 0; i + 1 < n; i += 2) ArmarLinea(I[i], I[i + 1]);
    else {
        for (int i = 0; i + 1 < n; i++) ArmarLinea(I[i], I[i + 1]);
        if (lazo) ArmarLinea(I[n - 1], I[0]);
    }
    g_statIndices += n;
    MandarTodo();
}
template <typename T> static void Puntos(const T* idx, int primero, int n) {
    if (!gIniciado || n < 1 || !LeerDraw(idx, primero, n)) return;
    PrepararTexturaPantalla();
    for (int i = 0; i < n; i++) ArmarPunto(gIdx[i]);
    g_statIndices += n;
    MandarTodo();
}
static const int* const kSinIndices = NULL;

void DrawTriangles(int count, const MeshIndex* indices) { Triangulos(indices, 0, count, ModoLista); }
void DrawTrianglesClientIdx(int count, const MeshIndex* indices) { Triangulos(indices, 0, count, ModoLista); }
void DrawLinesClientIdx(int count, const MeshIndex* indices) { Lineas(indices, 0, count, false, false); }
void DrawTrianglesByte(int count, const unsigned char* indices) { Triangulos(indices, 0, count, ModoLista); }
void DrawTrianglesArray(int n) { Triangulos(kSinIndices, 0, n, ModoLista); }
void DrawTrianglesArrayFrom(int first, int n) { Triangulos(kSinIndices, first, n, ModoLista); }
void DrawLinesIndexed(int count, const unsigned short* indices) { Lineas(indices, 0, count, false, false); }
void DrawLineStripIndexed(int count, const unsigned short* indices) { Lineas(indices, 0, count, true, false); }
void DrawLines(int n) { Lineas(kSinIndices, 0, n, false, false); }
void DrawLineStrip(int n) { Lineas(kSinIndices, 0, n, true, false); }
void DrawPoints(int n) { Puntos(kSinIndices, 0, n); }
template <typename T> static void Elementos(Primitiva p, int count, const T* indices) {
    if (!indices) return;
    switch (p) {
        case PrimPuntos: Puntos(indices, 0, count); return;
        case PrimLineas: Lineas(indices, 0, count, false, false); return;
        case PrimTiraLineas: Lineas(indices, 0, count, true, false); return;
        case PrimLazoLineas: Lineas(indices, 0, count, true, true); return;
        case PrimTiraTriangulos: Triangulos(indices, 0, count, ModoTira); return;
        case PrimAbanico: Triangulos(indices, 0, count, ModoAbanico); return;
        default: Triangulos(indices, 0, count, ModoLista); return;
    }
}
void DrawElements(Primitiva p, int count, const MeshIndex* indices) { Elementos(p, count, indices); }
void DrawElements16(Primitiva p, int count, const unsigned short* indices) { Elementos(p, count, indices); }
void DrawElementsVBO(Primitiva, int, int) {}

// EL CLEAR: el del GE es un rectangulo en "modo limpiar" que respeta el scissor (como GL: el scissor si, el viewport
// no). El z se limpia a 0 (lo mas lejos) y el estencil (el alfa) al valor de EstencilLimpiar
static void Limpiar(int bits) {
    if (!gIniciado || !bits) return;
    ListaEspacio();
    if (!PonerScissor(true)) return;
    int f = 0;
    if (bits & ColorBuffer) f |= GU_COLOR_BUFFER_BIT;
    if (bits & DepthBuffer) f |= GU_DEPTH_BUFFER_BIT;
    if (bits & StencilBuffer) f |= GU_STENCIL_BUFFER_BIT;
    sceGuClearColor(ColorGE(clearCol) & 0xFFFFFF);
    sceGuClearDepth(0);
    sceGuClearStencil(estencilLimpio & 0xFF);
    sceGuClear(f);
}

// ============================================================================
//  LA SELECCION POR COLOR (Pick3D): se dibuja en el buffer de atras (fuera de un cuadro no se ve: el proximo cuadro lo
//  pisa entero) y ReadPixelsRGBA espera al GE y lee los pixeles de la VRAM, sin cache. Si habia un cuadro a medio
//  dibujar, sigue con otra lista (lo de antes queda en el buffer, como en la PC)
// ============================================================================
void ReadPixelsBegin(int, int, int, int) { ListaEspacio(); }
bool CopiaPantallaDadaVuelta() { return false; }
int CopiaPantallaFila(int y, int) { return y; }
void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* p) {
    if (!p || w <= 0 || h <= 0) return;
    if (!gIniciado) { memset(p, 0, (size_t)w * h * 4); return; }
    ListaCerrar();
    const unsigned char* fb = (const unsigned char*)(0x40000000u | ((unsigned)sceGeEdramGetAddr() + (unsigned)gFbDibujo));
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            unsigned char* o = p + ((size_t)j * w + i) * 4;
            const int gx = x + i, gy = GuAlto - 1 - (y + j);   // (GL cuenta las filas desde abajo)
            if (gx < 0 || gx >= GuAncho || gy < 0 || gy >= GuAlto) { o[0] = o[1] = o[2] = 0; o[3] = 255; continue; }
            const unsigned char* q = fb + ((size_t)gy * kPaso + gx) * 4;   // (RGBA en memoria; el alfa es el estencil)
            o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = 255;
        }
    if (gEnCuadro) ListaAbrir();
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
void TexCoordVBO2s(unsigned int) {}
void VertexVBO3s(unsigned int) {}
void BindIndexVBO(unsigned int) {}
void DrawTrianglesVBO(int, int) {}
void UnbindVBOs() {}
int BuffersVivos() { return 0; }

// ============================================================================
//  Estadisticas (como el backend de citro3d)
// ============================================================================
int g_statMeshes = 0;
int g_statDrawTris = 0, g_statDrawVBO = 0, g_statIndices = 0, g_statTexBinds = 0, g_statStateChanges = 0, g_statOtras = 0;
int g_statUploadBytes = 0, g_statTrisOpacos = 0, g_statTrisBlend = 0;
int g_statDcCat[StatCatCount_] = { 0 };
void StatCategoria(StatCat c) { gStatCat = (int)c; }
StatCat StatCategoriaActual() { return (StatCat)gStatCat; }
void StatRaw(int n) { g_statLlamadas[gStatCat] += n; }
StatsDeCuadro g_statCuadro;
bool g_medirDraws = false;
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

// la memoria de una textura que se borra: si el GE puede estar leyendola (hay una lista abierta), al cerrarla
void w3dGu::LiberarDespues(void* p) {
    if (!p) return;
    if (w3dEngine::gListaAbierta) w3dEngine::gLiberar.push_back(p);
    else free(p);
}

// w3dSetColor (w3dBase.h): por la abstraccion
#include "w3dBase.h"
void w3dSetColor(const ColorType c[4]) { w3dEngine::Color4f(c[0], c[1], c[2], c[3]); }
