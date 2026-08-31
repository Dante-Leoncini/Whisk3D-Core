#include "animation/Flipbook.h"

// ===========================================================================
//  Flipbook (asset compartido): grilla de celdas + 8 curvas de UV del quad.
// ===========================================================================
Flipbook::Flipbook()
    : cols(1), filas(1), cuadros(1), fps(30.0f), crossfade(false),
      tiraAncho(1.0f), tiraAlto(1.0f), tiraU0(0.0f), tiraV0(0.0f) {
    for (int k = 0; k < 8; k++) { uv[k].Property = AnimVertex; uv[k].component = k; }
}

void Flipbook::RectDeCelda(int celda, float& u0, float& v0, float& u1, float& v1) const {
    int c = cols > 0 ? cols : 1;
    int f = filas > 0 ? filas : 1;
    if (celda < 0) celda = 0;
    int col = celda % c;
    int fila = (celda / c) % f;
    // sub-tira: la grilla ocupa tiraAncho x tiraAlto de la textura (1,1 = entera)
    // y arranca en (tiraU0, tiraV0) (0,0 = entera; atlas unico = el rect de la tira)
    float du = tiraAncho / (float)c, dv = tiraAlto / (float)f;
    u0 = tiraU0 + col * du;  u1 = u0 + du;
    v0 = tiraV0 + fila * dv; v1 = v0 + dv;   // fila 0 = ARRIBA del atlas (V crece hacia abajo, como Imagen2D)
}

void Flipbook::KeyCelda(int frame, int celda) {
    float u0, v0, u1, v1;
    RectDeCelda(celda, u0, v0, u1, v1);
    // esquinas en orden (0,0)(1,0)(1,1)(0,1); 2 canales por esquina
    float vals[8] = { u0, v0,  u1, v0,  u1, v1,  u0, v1 };
    for (int k = 0; k < 8; k++) {
        SetKeyCurva(uv[k], frame, vals[k]);
        // interp CONSTANTE: el flipbook clasico salta de celda (no interpola entre cuadros)
        for (size_t i = 0; i < uv[k].keyframes.size(); i++)
            if (uv[k].keyframes[i].frame == frame) {
                uv[k].keyframes[i].Interpolation = KfConstant;
                break;
            }
    }
}

void Flipbook::GenerarCeldas() {
    for (int k = 0; k < 8; k++) uv[k].keyframes.clear();
    int n = cuadros > 0 ? cuadros : 1;
    for (int i = 0; i < n; i++) KeyCelda(i, i);
    for (int k = 0; k < 8; k++) uv[k].SortKeyFrames();
}

void Flipbook::ConfigurarTira(const std::string& atlasRuta, int c, int f, int n, float rate,
                              float ancho, float alto, float u0, float v0) {
    atlas = atlasRuta;
    cols = c > 0 ? c : 1;
    filas = f > 0 ? f : 1;
    cuadros = n > 0 ? n : 1;
    fps = rate;
    tiraAncho = (ancho > 0.0f && ancho <= 1.0f) ? ancho : 1.0f;
    tiraAlto  = (alto  > 0.0f && alto  <= 1.0f) ? alto  : 1.0f;
    tiraU0 = (u0 >= 0.0f && u0 < 1.0f) ? u0 : 0.0f;
    tiraV0 = (v0 >= 0.0f && v0 < 1.0f) ? v0 : 0.0f;
    GenerarCeldas();
}

// ===========================================================================
//  Registro global unico (funde gUVAnimMeshes + gImg2DAnim). Patron plano.
// ===========================================================================
static std::vector<FlipbookPlayer*> gFlipPlayers;

void FlipbookRegistrar(FlipbookPlayer* p) {
    if (!p) return;
    for (size_t i = 0; i < gFlipPlayers.size(); i++)
        if (gFlipPlayers[i] == p) return;
    gFlipPlayers.push_back(p);
}

void FlipbookQuitar(FlipbookPlayer* p) {
    for (size_t i = 0; i < gFlipPlayers.size(); i++)
        if (gFlipPlayers[i] == p) { gFlipPlayers.erase(gFlipPlayers.begin() + i); return; }
}

bool UpdateFlipbooks(float dtSeg) {
    bool cambio = false;
    for (size_t i = 0; i < gFlipPlayers.size(); i++)
        if (gFlipPlayers[i] && gFlipPlayers[i]->Tick(dtSeg)) cambio = true;
    return cambio;
}

bool HayFlipbooks() { return !gFlipPlayers.empty(); }

// ===========================================================================
//  Flipbooks CON NOMBRE de la escena (assets compartidos).
// ===========================================================================
std::vector<Flipbook*> SceneFlipbooks;

Flipbook* FlipbookPorNombre(const std::string& n) {
    for (size_t i = 0; i < SceneFlipbooks.size(); i++)
        if (SceneFlipbooks[i] && SceneFlipbooks[i]->nombre == n) return SceneFlipbooks[i];
    return 0;
}

static std::string _NumStr(int v) {   // int->string sin sprintf (C++03/Symbian)
    if (v == 0) return "0";
    std::string s; bool neg = v < 0; unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    while (u) { s = (char)('0' + (u % 10)) + s; u /= 10; }
    return neg ? "-" + s : s;
}

Flipbook* FlipbookNuevo(const std::string& base) {
    std::string raiz = base.empty() ? std::string("Flipbook") : base;
    std::string n = raiz;
    for (int k = 1; FlipbookPorNombre(n); k++) n = raiz + "." + _NumStr(k);   // nombre unico
    Flipbook* f = new Flipbook();
    f->nombre = n;
    SceneFlipbooks.push_back(f);
    return f;
}

void FlipbookBorrar(Flipbook* f) {
    if (!f) return;
    for (size_t i = 0; i < SceneFlipbooks.size(); i++)
        if (SceneFlipbooks[i] == f) { SceneFlipbooks.erase(SceneFlipbooks.begin() + i); break; }
    delete f;
}

void SceneFlipbooksLimpiar() {
    for (size_t i = 0; i < SceneFlipbooks.size(); i++) delete SceneFlipbooks[i];
    SceneFlipbooks.clear();
}

// ===========================================================================
//  FlipbookPlayer (estado por consumidor).
// ===========================================================================
FlipbookPlayer::FlipbookPlayer()
    : fb(0), desfase(0), acum(0.0f), cuadroActual(-1) {
    // UV de reposo = quad entero (0,0)(1,0)(1,1)(0,1)
    uvActual[0] = 0; uvActual[1] = 0; uvActual[2] = 1; uvActual[3] = 0;
    uvActual[4] = 1; uvActual[5] = 1; uvActual[6] = 0; uvActual[7] = 1;
}

FlipbookPlayer::~FlipbookPlayer() { FlipbookQuitar(this); }

void FlipbookPlayer::Set(Flipbook* f, int d, bool registrar) {
    fb = f; desfase = d; acum = 0.0f; cuadroActual = -1;
    if (f) {
        if (registrar) FlipbookRegistrar(this);   // Imagen2D/particulas: lo tickea UpdateFlipbooks
        else           FlipbookQuitar(this);      // Mesh: se tickea solo -> fuera del registro global
        Tick(0.0f);
    } else {
        FlipbookQuitar(this);
    }
}

bool FlipbookPlayer::Tick(float dtSeg) {
    if (!fb || fb->cuadros < 1) return false;
    if (dtSeg > 0.0f) acum += dtSeg;
    // posicion en cuadros logicos, con loop al rango [0, cuadros)
    float fp = acum * fb->fps + (float)desfase;
    float ciclo = (float)fb->cuadros;
    if (ciclo > 0.0f) {
        fp = fp - (float)((long)(fp / ciclo)) * ciclo;   // fmod (sin floorf: (long) trunca)
        if (fp < 0.0f) fp += ciclo;
    }
    int cel = (int)fp;
    if (cel < 0) cel = 0;
    if (cel >= fb->cuadros) cel = fb->cuadros - 1;
    bool cambio = (cel != cuadroActual);
    cuadroActual = cel;
    for (int k = 0; k < 8; k++) {
        float nv = fb->uv[k].EvalF(fp, uvActual[k]);
        if (nv != uvActual[k]) { uvActual[k] = nv; cambio = true; }
    }
    return cambio;
}
