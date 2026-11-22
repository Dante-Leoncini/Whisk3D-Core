#include "animation/Flipbook.h"

// ===========================================================================
//  Flipbook (asset compartido): grilla de celdas + 8 curvas de UV del quad.
// ===========================================================================
Flipbook::Flipbook()
    : cols(1), rows(1), frames(1), fps(30.0f), crossfade(false),
      stripWidth(1.0f), stripHeight(1.0f), stripU0(0.0f), stripV0(0.0f) {
    for (int k = 0; k < 8; k++) { uv[k].Property = AnimVertex; uv[k].component = k; }
}

void Flipbook::RectOfCell(int cell, float& u0, float& v0, float& u1, float& v1) const {
    int c = cols > 0 ? cols : 1;
    int f = rows > 0 ? rows : 1;
    if (cell < 0) cell = 0;
    int col = cell % c;
    int row = (cell / c) % f;
    // sub-tira: la grilla ocupa stripWidth x stripHeight de la textura (1,1 = entera)
    // y arranca en (stripU0, stripV0) (0,0 = entera; atlas unico = el rect de la tira)
    float du = stripWidth / (float)c, dv = stripHeight / (float)f;
    u0 = stripU0 + col * du;  u1 = u0 + du;
    v0 = stripV0 + row * dv; v1 = v0 + dv;   // fila 0 = ARRIBA del atlas (V crece hacia abajo, como Image2D)
}

void Flipbook::KeyCell(int frame, int cell) {
    float u0, v0, u1, v1;
    RectOfCell(cell, u0, v0, u1, v1);
    // esquinas en orden (0,0)(1,0)(1,1)(0,1); 2 canales por esquina
    float vals[8] = { u0, v0,  u1, v0,  u1, v1,  u0, v1 };
    for (int k = 0; k < 8; k++) {
        SetKeyCurve(uv[k], frame, vals[k]);
        // interp CONSTANTE: el flipbook clasico salta de celda (no interpola entre cuadros)
        for (size_t i = 0; i < uv[k].keyframes.size(); i++)
            if (uv[k].keyframes[i].frame == frame) {
                uv[k].keyframes[i].Interpolation = KfConstant;
                break;
            }
    }
}

void Flipbook::GenerateCells() {
    for (int k = 0; k < 8; k++) uv[k].keyframes.clear();
    int n = frames > 0 ? frames : 1;
    for (int i = 0; i < n; i++) KeyCell(i, i);
    for (int k = 0; k < 8; k++) uv[k].SortKeyFrames();
}

void Flipbook::ConfigureStrip(const std::string& atlasPath, int c, int f, int n, float rate,
                              float width, float height, float u0, float v0) {
    atlas = atlasPath;
    cols = c > 0 ? c : 1;
    rows = f > 0 ? f : 1;
    frames = n > 0 ? n : 1;
    fps = rate;
    stripWidth  = (width > 0.0f && width <= 1.0f) ? width : 1.0f;
    stripHeight = (height  > 0.0f && height  <= 1.0f) ? height  : 1.0f;
    stripU0 = (u0 >= 0.0f && u0 < 1.0f) ? u0 : 0.0f;
    stripV0 = (v0 >= 0.0f && v0 < 1.0f) ? v0 : 0.0f;
    GenerateCells();
}

// ===========================================================================
//  Registro global unico (funde gUVAnimMeshes + gImg2DAnim). Patron plano.
// ===========================================================================
static std::vector<FlipbookPlayer*> gFlipPlayers;

void FlipbookRegister(FlipbookPlayer* p) {
    if (!p) return;
    for (size_t i = 0; i < gFlipPlayers.size(); i++)
        if (gFlipPlayers[i] == p) return;
    gFlipPlayers.push_back(p);
}

void FlipbookRemove(FlipbookPlayer* p) {
    for (size_t i = 0; i < gFlipPlayers.size(); i++)
        if (gFlipPlayers[i] == p) { gFlipPlayers.erase(gFlipPlayers.begin() + i); return; }
}

bool UpdateFlipbooks(float dtSeg) {
    bool change = false;
    for (size_t i = 0; i < gFlipPlayers.size(); i++)
        if (gFlipPlayers[i] && gFlipPlayers[i]->Tick(dtSeg)) change = true;
    return change;
}

bool HasFlipbooks() { return !gFlipPlayers.empty(); }

// ===========================================================================
//  Flipbooks CON NOMBRE de la escena (assets compartidos).
// ===========================================================================
std::vector<Flipbook*> SceneFlipbooks;

Flipbook* FlipbookByName(const std::string& n) {
    for (size_t i = 0; i < SceneFlipbooks.size(); i++)
        if (SceneFlipbooks[i] && SceneFlipbooks[i]->name == n) return SceneFlipbooks[i];
    return 0;
}

static std::string _NumStr(int v) {   // int->string sin sprintf (C++03/Symbian)
    if (v == 0) return "0";
    std::string s; bool neg = v < 0; unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    while (u) { s = (char)('0' + (u % 10)) + s; u /= 10; }
    return neg ? "-" + s : s;
}

Flipbook* NewFlipbook(const std::string& base) {
    std::string root = base.empty() ? std::string("Flipbook") : base;
    std::string n = root;
    for (int k = 1; FlipbookByName(n); k++) n = root + "." + _NumStr(k);   // nombre unico
    Flipbook* f = new Flipbook();
    f->name = n;
    SceneFlipbooks.push_back(f);
    return f;
}

void FlipbookDelete(Flipbook* f) {
    if (!f) return;
    for (size_t i = 0; i < SceneFlipbooks.size(); i++)
        if (SceneFlipbooks[i] == f) { SceneFlipbooks.erase(SceneFlipbooks.begin() + i); break; }
    delete f;
}

void SceneFlipbooksClear() {
    for (size_t i = 0; i < SceneFlipbooks.size(); i++) delete SceneFlipbooks[i];
    SceneFlipbooks.clear();
}

// ===========================================================================
//  FlipbookPlayer (estado por consumidor).
// ===========================================================================
FlipbookPlayer::FlipbookPlayer()
    : fb(0), offset(0), accum(0.0f), currentFrame(-1) {
    // UV de reposo = quad entero (0,0)(1,0)(1,1)(0,1)
    uvCurrent[0] = 0; uvCurrent[1] = 0; uvCurrent[2] = 1; uvCurrent[3] = 0;
    uvCurrent[4] = 1; uvCurrent[5] = 1; uvCurrent[6] = 0; uvCurrent[7] = 1;
}

FlipbookPlayer::~FlipbookPlayer() { FlipbookRemove(this); }

void FlipbookPlayer::Set(Flipbook* f, int d, bool doRegister) {
    fb = f; offset = d; accum = 0.0f; currentFrame = -1;
    if (f) {
        if (doRegister) FlipbookRegister(this);   // Image2D/particulas: lo tickea UpdateFlipbooks
        else           FlipbookRemove(this);      // Mesh: se tickea solo -> fuera del registro global
        Tick(0.0f);
    } else {
        FlipbookRemove(this);
    }
}

bool FlipbookPlayer::Tick(float dtSeg) {
    if (!fb || fb->frames < 1) return false;
    if (dtSeg > 0.0f) accum += dtSeg;
    // posicion en cuadros logicos, con loop al rango [0, cuadros)
    float fp = accum * fb->fps + (float)offset;
    float cycle = (float)fb->frames;
    if (cycle > 0.0f) {
        fp = fp - (float)((long)(fp / cycle)) * cycle;   // fmod (sin floorf: (long) trunca)
        if (fp < 0.0f) fp += cycle;
    }
    int cel = (int)fp;
    if (cel < 0) cel = 0;
    if (cel >= fb->frames) cel = fb->frames - 1;
    bool change = (cel != currentFrame);
    currentFrame = cel;
    for (int k = 0; k < 8; k++) {
        float nv = fb->uv[k].EvalF(fp, uvCurrent[k]);
        if (nv != uvCurrent[k]) { uvCurrent[k] = nv; change = true; }
    }
    return change;
}
