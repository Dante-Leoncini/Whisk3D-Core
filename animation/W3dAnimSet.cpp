#include "W3dAnimSet.h"
#include "SkeletalAnimation.h"
#include "objects/Armature.h"
#include "objects/Mesh.h"        // el tamano de una jerarquia cuenta las cajas de sus mallas
#include "math/Matrix4.h"
#include "W3dNombres.h"            // W3dNombreNormalizar: los nombres que la reparacion del editor tocaria
#include "io/W3dRecursos.h"     // el animset es un recurso del almacen (tipo ANIMSET)
#include "io/w3dFilesystem.h"   // la op Cargar lee la entrada por el Core (contenedor / pak / APK)
#include "base/w3dlog.h"

#include <map>
#include <set>
#include <cstring>
#include <math.h>    // sqrtf (el tamano de una jerarquia)
#include <stdlib.h>  // atoi (el "#k" de un tramo de ruta)
#include <stdio.h>   // sprintf GLOBAL: en Symbian/STLport <cstdio> puede dejarlo solo en std::

// ============================================================================
//  LOS BLOQUES QUE ESTA VERSION ENTIENDE (ver W3dAnimSet.h). Un fourcc son 4
//  letras: los de 3 se completan con un espacio ("REQ ").
// ============================================================================
static const char* kBloquesA[] = { "REQ ", "HUES", "CLIP", "JCLP", 0 };
// lo MINIMO que un lector tiene que entender para no romper el animset: van en REQ
static const char* kRequeridosA[] = { "HUES", "CLIP", 0 };

static bool BloqueConocido(const std::string& f) {
    for (int i = 0; kBloquesA[i]; i++) if (f == kBloquesA[i]) return true;
    return false;
}

void W3dAnimSetBloquesConocidos(std::vector<std::string>& conocidos, std::vector<std::string>* requeridos) {
    conocidos.clear();
    for (int i = 0; kBloquesA[i]; i++) conocidos.push_back(kBloquesA[i]);
    if (requeridos) {
        requeridos->clear();
        for (int i = 0; kRequeridosA[i]; i++) requeridos->push_back(kRequeridosA[i]);
    }
}

bool W3dAnimSetBinEs(const unsigned char* d, size_t n) {
    return d && n >= 4 && d[0] == 'W' && d[1] == '3' && d[2] == 'D' && d[3] == 'A';
}

// modos de los keyframes compactos (un byte cada uno, ver PonCurva)
enum { KF_FRAMES_TODOS = 0, KF_FRAMES_REGULARES = 1 };   // i32 por key | primero + paso
enum { KF_INT_TODOS = 0, KF_INT_UNO = 1 };               // i32 por key | uno para todos
enum { KF_HANDLES_CERO = 0, KF_HANDLES_TODOS = 1 };      // nada (todos 0) | f32 x4 por key

namespace {

// ===========================================================================
//  LITTLE-ENDIAN EXPLICITO, byte a byte: los mismos bytes en cualquier host y
//  ninguna lectura tipada desalineada (el ARM11 del N95 da SIGBUS con un float
//  desalineado; aca todo pasa por bytes).
// ===========================================================================
void PonU32(std::string& s, unsigned v) {
    char b[4];
    b[0] = (char)(v & 255u); b[1] = (char)((v >> 8) & 255u);
    b[2] = (char)((v >> 16) & 255u); b[3] = (char)((v >> 24) & 255u);
    s.append(b, 4);
}
void PonI32(std::string& s, int v) { PonU32(s, (unsigned)v); }
void PonF32(std::string& s, float f) { unsigned u; memcpy(&u, &f, 4); PonU32(s, u); }
void PonStr(std::string& s, const std::string& t) { PonU32(s, (unsigned)t.size()); s.append(t); }
unsigned LeeU32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
unsigned BitsF32(float f) { unsigned u; memcpy(&u, &f, 4); return u; }

// ---- BLOQUES: abrir escribe la cabecera con bytes=0; cerrar parchea el largo y
//      rellena con ceros hasta multiplo de 4 (sin basura: bytes deterministas) ----
size_t AbrirBloque(std::string& s, const char* id, unsigned dominio, unsigned cantidad) {
    s.append(id, 4);
    PonU32(s, dominio);
    PonU32(s, cantidad);
    const size_t pos = s.size();
    PonU32(s, 0);
    return pos;
}
void CerrarBloque(std::string& s, size_t posBytes) {
    const unsigned bytes = (unsigned)(s.size() - (posBytes + 4));
    s[posBytes]     = (char)(bytes & 255u);
    s[posBytes + 1] = (char)((bytes >> 8) & 255u);
    s[posBytes + 2] = (char)((bytes >> 16) & 255u);
    s[posBytes + 3] = (char)((bytes >> 24) & 255u);
    while (s.size() % 4) s += '\0';
}

// ---- LECTOR con cota: cada lectura mira lo que queda ANTES de tocar un byte ----
struct Cursor {
    const unsigned char* p;
    const unsigned char* fin;
    bool ok;
    Cursor(const unsigned char* a, size_t n) : p(a), fin(a + n), ok(true) {}
    size_t Quedan() const { return (size_t)(fin - p); }
    bool Hay(size_t n) { if (!ok || Quedan() < n) ok = false; return ok; }
    unsigned U32() { if (!Hay(4)) return 0; unsigned v = LeeU32(p); p += 4; return v; }
    int I32() { return (int)U32(); }
    float F32() { unsigned u = U32(); float f; memcpy(&f, &u, 4); return f; }
    unsigned U8() { if (!Hay(1)) return 0; return *p++; }
    std::string Str() {
        unsigned n = U32();
        if (!Hay(n)) return std::string();
        std::string s((const char*)p, (size_t)n); p += n;
        return s;
    }
};

// ---------------------------------------------------------------------------
//  UNA CURVA: propiedad, componente, nKeys, 4 bytes de modos y los datos.
//  Los modos se ELIGEN mirando los datos (deterministas: la misma curva da los
//  mismos bytes). El caso de las animaciones horneadas (un key por frame, todo
//  lineal, handles en cero) queda en 4 bytes por keyframe: solo el valor.
// ---------------------------------------------------------------------------
void PonCurva(std::string& s, const AnimProperty& ap) {
    const std::vector<keyFrame>& k = ap.keyframes;
    const size_t n = k.size();
    PonI32(s, ap.Property);
    PonI32(s, ap.component);
    PonU32(s, (unsigned)n);
    // frames regulares: cada diferencia igual a la primera (en double: sin overflow de int)
    bool regulares = n >= 2;
    int paso = 0;
    if (regulares) {
        const double d0 = (double)k[1].frame - (double)k[0].frame;
        if (d0 > 2147483647.0 || d0 < -2147483648.0) regulares = false;
        else paso = (int)d0;
        for (size_t i = 2; regulares && i < n; i++)
            if ((double)k[i].frame - (double)k[i - 1].frame != d0) regulares = false;
    }
    bool interpUno = n >= 1, tipoUno = n >= 1, handles = false;
    for (size_t i = 1; i < n; i++) {
        if (k[i].Interpolation != k[0].Interpolation) interpUno = false;
        if (k[i].handleType != k[0].handleType) tipoUno = false;
    }
    // handles: se comparan los BITS (un -0.0 no es "cero": tiene que volver igual)
    for (size_t i = 0; i < n && !handles; i++)
        if (BitsF32(k[i].inDF) || BitsF32(k[i].inDV) || BitsF32(k[i].outDF) || BitsF32(k[i].outDV)) handles = true;
    char modos[4];
    modos[0] = (char)(regulares ? KF_FRAMES_REGULARES : KF_FRAMES_TODOS);
    modos[1] = (char)(interpUno ? KF_INT_UNO : KF_INT_TODOS);
    modos[2] = (char)(tipoUno ? KF_INT_UNO : KF_INT_TODOS);
    modos[3] = (char)(handles ? KF_HANDLES_TODOS : KF_HANDLES_CERO);
    s.append(modos, 4);
    if (n == 0) return;
    if (regulares) { PonI32(s, k[0].frame); PonI32(s, paso); }
    else for (size_t i = 0; i < n; i++) PonI32(s, k[i].frame);
    for (size_t i = 0; i < n; i++) PonF32(s, k[i].value);
    if (interpUno) PonI32(s, k[0].Interpolation);
    else for (size_t i = 0; i < n; i++) PonI32(s, k[i].Interpolation);
    if (tipoUno) PonI32(s, k[0].handleType);
    else for (size_t i = 0; i < n; i++) PonI32(s, k[i].handleType);
    if (handles)
        for (size_t i = 0; i < n; i++) {
            PonF32(s, k[i].inDF); PonF32(s, k[i].inDV); PonF32(s, k[i].outDF); PonF32(s, k[i].outDV);
        }
}

bool LeerCurva(Cursor& c, AnimProperty& ap, std::string& motivo) {
    ap.Property = c.I32();
    ap.component = c.I32();
    const unsigned n = c.U32();
    const unsigned mF = c.U8(), mI = c.U8(), mT = c.U8(), mH = c.U8();
    if (!c.ok) { motivo = "curva cortada"; return false; }
    if (mF > 1 || mI > 1 || mT > 1 || mH > 1) { motivo = "curva con un modo desconocido"; return false; }
    if (n == 0) return true;
    // cota ANTES de reservar: cada keyframe ocupa por lo menos 4 bytes (su valor)
    if ((size_t)n > c.Quedan() / 4) { motivo = "curva con mas keyframes de los que entran en el bloque"; c.ok = false; return false; }
    // bytes exactos que ocupa lo que sigue (en size_t, sin overflow: n <= quedan/4)
    size_t nec = (size_t)n * 4;                                   // valores
    nec += (mF == KF_FRAMES_REGULARES) ? 8 : (size_t)n * 4;
    nec += (mI == KF_INT_UNO) ? 4 : (size_t)n * 4;
    nec += (mT == KF_INT_UNO) ? 4 : (size_t)n * 4;
    if (mH == KF_HANDLES_TODOS) nec += (size_t)n * 16;
    if (!c.Hay(nec)) { motivo = "curva cortada"; return false; }
    ap.keyframes.resize(n);
    std::vector<keyFrame>& k = ap.keyframes;
    if (mF == KF_FRAMES_REGULARES) {
        int f = c.I32(); const int paso = c.I32();
        for (unsigned i = 0; i < n; i++) { k[i].frame = f; if (i + 1 < n) f += paso; }
    } else for (unsigned i = 0; i < n; i++) k[i].frame = c.I32();
    for (unsigned i = 0; i < n; i++) k[i].value = c.F32();
    if (mI == KF_INT_UNO) { const int v = c.I32(); for (unsigned i = 0; i < n; i++) k[i].Interpolation = v; }
    else for (unsigned i = 0; i < n; i++) k[i].Interpolation = c.I32();
    if (mT == KF_INT_UNO) { const int v = c.I32(); for (unsigned i = 0; i < n; i++) k[i].handleType = v; }
    else for (unsigned i = 0; i < n; i++) k[i].handleType = c.I32();
    if (mH == KF_HANDLES_TODOS)
        for (unsigned i = 0; i < n; i++) {
            k[i].inDF = c.F32(); k[i].inDV = c.F32(); k[i].outDF = c.F32(); k[i].outDV = c.F32();
        }
    else for (unsigned i = 0; i < n; i++) { k[i].inDF = 0.0f; k[i].inDV = 0.0f; k[i].outDF = 0.0f; k[i].outDV = 0.0f; }
    return c.ok;
}

// un bloque ya ubicado en el archivo
struct BloqueA {
    std::string id;
    unsigned dominio, cantidad, bytes;
    const unsigned char* datos;
};

// recorre la cabecera y los bloques; false + motivo si la estructura no cierra
bool Ubicar(const unsigned char* d, size_t n, std::vector<BloqueA>& out, std::string& motivo) {
    out.clear();
    if (!W3dAnimSetBinEs(d, n)) { motivo = "no es un .w3da (falta la firma W3DA)"; return false; }
    if (n < 20) { motivo = "cabecera cortada"; return false; }
    const unsigned version = LeeU32(d + 4);
    if (version > (unsigned)W3DA_VERSION) {
        char b[96]; sprintf(b, "es de una version mas nueva del formato (%u; esta lee hasta %d)", version, (int)W3DA_VERSION);
        motivo = b; return false;
    }
    if (version == 0) { motivo = "version 0 invalida"; return false; }
    const unsigned nBloques = LeeU32(d + 12);
    size_t pos = 20;
    for (unsigned i = 0; i < nBloques; i++) {
        if (n - pos < 16) { motivo = "bloque cortado (cabecera)"; return false; }
        BloqueA b;
        b.id.assign((const char*)d + pos, 4);
        b.dominio = LeeU32(d + pos + 4);
        b.cantidad = LeeU32(d + pos + 8);
        b.bytes = LeeU32(d + pos + 12);
        pos += 16;
        if ((size_t)b.bytes > n - pos) { motivo = "bloque '" + b.id + "' cortado"; return false; }
        b.datos = d + pos;
        const size_t conRelleno = ((size_t)b.bytes + 3) & ~(size_t)3;
        pos += (conRelleno <= n - pos) ? conRelleno : (size_t)b.bytes;
        out.push_back(b);
    }
    if (pos != n) { motivo = "sobran bytes despues del ultimo bloque"; return false; }
    return true;
}

bool LeerClip(const BloqueA& b, int nHuesos, SkeletalAnimation*& out, std::string& motivo) {
    out = 0;
    Cursor c(b.datos, b.bytes);
    const std::string nombre = c.Str();
    const int fps = c.I32(), ini = c.I32(), fin = c.I32();
    if (!c.ok) { motivo = "clip cortado"; return false; }
    // cada pista ocupa por lo menos 8 bytes (hueso + nCurvas)
    if ((size_t)b.cantidad > c.Quedan() / 8) { motivo = "clip con mas pistas de las que entran en el bloque"; return false; }
    SkeletalAnimation* clip = new SkeletalAnimation(nombre);
    clip->FrameRate = fps; clip->startFrame = ini; clip->endFrame = fin;
    clip->tracks.resize(b.cantidad);
    for (unsigned t = 0; t < b.cantidad; t++) {
        BoneTrack& tr = clip->tracks[t];
        tr.bone = c.I32();
        const unsigned nCurvas = c.U32();
        if (!c.ok) { motivo = "pista cortada"; delete clip; return false; }
        if (tr.bone < -1 || tr.bone >= nHuesos) { motivo = "pista con un hueso fuera de la tabla"; delete clip; return false; }
        // cada curva ocupa por lo menos 16 bytes (propiedad, componente, nKeys, modos)
        if ((size_t)nCurvas > c.Quedan() / 16) { motivo = "pista con mas curvas de las que entran en el bloque"; delete clip; return false; }
        tr.Propertys.resize(nCurvas);
        for (unsigned k = 0; k < nCurvas; k++)
            if (!LeerCurva(c, tr.Propertys[k], motivo)) { delete clip; return false; }
    }
    // el tamano del esqueleto que lo grabo (opcional: un clip viejo termina en su ultima pista)
    if (c.ok && c.Quedan() == 4) clip->alturaReposo = c.F32();
    if (!c.ok || c.Quedan() != 0) { motivo = "el clip '" + nombre + "' no cierra con el largo de su bloque"; delete clip; return false; }
    out = clip;
    return true;
}

// un bloque JCLP (clip de jerarquia)
bool LeerClipJer(const BloqueA& b, W3dClipJer*& out, std::string& motivo) {
    out = 0;
    Cursor c(b.datos, b.bytes);
    const std::string nombre = c.Str();
    const int fps = c.I32(), ini = c.I32(), fin = c.I32();
    const float tam = c.F32();
    const unsigned ret = c.U32();
    if (!c.ok) { motivo = "clip de jerarquia cortado"; return false; }
    // cada pista ocupa por lo menos 8 bytes (largo de la ruta + nCurvas)
    if ((size_t)b.cantidad > c.Quedan() / 8) { motivo = "clip de jerarquia con mas pistas de las que entran en el bloque"; return false; }
    W3dClipJer* clip = new W3dClipJer();
    clip->nombre = nombre; clip->fps = fps; clip->inicio = ini; clip->fin = fin;
    clip->tamano = tam; clip->retarget = (ret == 1u) ? 1 : 0;
    clip->pistas.resize(b.cantidad);
    for (unsigned t = 0; t < b.cantidad; t++) {
        W3dPistaJer& p = clip->pistas[t];
        p.ruta = c.Str();
        const unsigned nCurvas = c.U32();
        if (!c.ok) { motivo = "pista de jerarquia cortada"; delete clip; return false; }
        if ((size_t)nCurvas > c.Quedan() / 16) { motivo = "pista con mas curvas de las que entran en el bloque"; delete clip; return false; }
        p.props.resize(nCurvas);
        for (unsigned k = 0; k < nCurvas; k++)
            if (!LeerCurva(c, p.props[k], motivo)) { delete clip; return false; }
    }
    if (!c.ok || c.Quedan() != 0) { motivo = "el clip de jerarquia '" + nombre + "' no cierra con el largo de su bloque"; delete clip; return false; }
    out = clip;
    return true;
}

} // namespace

// ============================================================================
//  ESCRITURA
// ============================================================================
void W3dAnimSetClipBloque(const SkeletalAnimation& c, const std::vector<int>& mapa, int nHuesos, std::string& out) {
    out.clear();
    const size_t pb = AbrirBloque(out, "CLIP", 0u, (unsigned)c.tracks.size());
    PonStr(out, c.name);
    PonI32(out, c.FrameRate);
    PonI32(out, c.startFrame);
    PonI32(out, c.endFrame);
    for (size_t t = 0; t < c.tracks.size(); t++) {
        const BoneTrack& tr = c.tracks[t];
        int h = -1;
        if (tr.bone >= 0 && tr.bone < (int)mapa.size()) h = mapa[tr.bone];
        if (h < 0 || h >= nHuesos) h = -1;
        PonI32(out, h);
        PonU32(out, (unsigned)tr.Propertys.size());
        for (size_t k = 0; k < tr.Propertys.size(); k++) PonCurva(out, tr.Propertys[k]);
    }
    // el tamano del esqueleto que lo grabo (retarget), SOLO si se sabe: un clip viejo sale igual que antes
    if (BitsF32(c.alturaReposo) != 0u) PonF32(out, c.alturaReposo);
    CerrarBloque(out, pb);
}

void W3dAnimSetClipJerBloque(const W3dClipJer& c, std::string& out) {
    out.clear();
    const size_t pb = AbrirBloque(out, "JCLP", 0u, (unsigned)c.pistas.size());
    PonStr(out, c.nombre);
    PonI32(out, c.fps);
    PonI32(out, c.inicio);
    PonI32(out, c.fin);
    PonF32(out, c.tamano);
    PonU32(out, (unsigned)c.retarget);
    for (size_t t = 0; t < c.pistas.size(); t++) {
        const W3dPistaJer& p = c.pistas[t];
        PonStr(out, p.ruta);
        PonU32(out, (unsigned)p.props.size());
        for (size_t k = 0; k < p.props.size(); k++) PonCurva(out, p.props[k]);
    }
    CerrarBloque(out, pb);
}

void W3dAnimSetArmar(const std::vector<std::string>& huesos, const std::vector<const std::string*>& clips,
                     const std::vector<W3dAnimSetBloqueAjeno>* ajenos, std::string& out,
                     const std::vector<const std::string*>* jer) {
    out.clear();
    const unsigned nAj = ajenos ? (unsigned)ajenos->size() : 0u;
    const unsigned nJer = jer ? (unsigned)jer->size() : 0u;
    out.append("W3DA", 4);
    PonU32(out, (unsigned)W3DA_VERSION);
    PonU32(out, 0u);                                  // banderas
    PonU32(out, 2u + (unsigned)clips.size() + nJer + nAj);   // REQ + HUES + clips + jerarquias + ajenos
    PonU32(out, 0u);                                  // reservado
    {
        // (JCLP solo si hay clips de jerarquia: un animset de esqueleto sale byte a byte como antes)
        unsigned nReq = 0; for (int i = 0; kRequeridosA[i]; i++) nReq++;
        const size_t pb = AbrirBloque(out, "REQ ", 0u, nReq + (nJer ? 1u : 0u));
        for (int i = 0; kRequeridosA[i]; i++) out.append(kRequeridosA[i], 4);
        if (nJer) out.append("JCLP", 4);
        CerrarBloque(out, pb);
    }
    {
        const size_t pb = AbrirBloque(out, "HUES", 0u, (unsigned)huesos.size());
        for (size_t i = 0; i < huesos.size(); i++) PonStr(out, huesos[i]);
        CerrarBloque(out, pb);
    }
    for (size_t i = 0; i < clips.size(); i++) if (clips[i]) out += *clips[i];
    for (unsigned i = 0; i < nJer; i++) if ((*jer)[i]) out += *(*jer)[i];
    for (unsigned i = 0; i < nAj; i++) {
        const W3dAnimSetBloqueAjeno& a = (*ajenos)[i];
        std::string id = a.id; id.resize(4, ' ');
        const size_t pb = AbrirBloque(out, id.c_str(), a.dominio, a.cantidad);
        out += a.datos;
        CerrarBloque(out, pb);
    }
}

// ============================================================================
//  LECTURA
// ============================================================================
W3dAnimSetDatos::~W3dAnimSetDatos() {
    for (size_t i = 0; i < clips.size(); i++) delete clips[i];
    for (size_t i = 0; i < jerarquias.size(); i++) delete jerarquias[i];
}

// ---- el clip de jerarquia ----
static unsigned gSerieJer = 0;
W3dClipJer::W3dClipJer() : nombre("Clip"), fps(30), inicio(1), fin(250), tamano(0.0f), retarget(0),
                           version(1), serie(++gSerieJer) {}
float W3dClipJer::Duracion() const {
    const int f = fps > 0 ? fps : 30;
    const int n = fin - inicio + 1;
    return (n > 0 ? (float)n : 1.0f) / (float)f;
}

bool W3dAnimSetLeer(const unsigned char* d, size_t n, W3dAnimSetDatos& out, std::string* motivo) {
    std::string m;
    for (size_t i = 0; i < out.clips.size(); i++) delete out.clips[i];
    for (size_t i = 0; i < out.jerarquias.size(); i++) delete out.jerarquias[i];
    out.clips.clear(); out.huesos.clear(); out.ajenos.clear(); out.jerarquias.clear();
    std::vector<BloqueA> bl;
    if (!Ubicar(d, n, bl, m)) { if (motivo) *motivo = m; return false; }
    // REQ: todo lo que pide tiene que ser conocido
    std::set<std::string> req;
    int nReq = 0, nHues = 0;
    for (size_t i = 0; i < bl.size(); i++) {
        if (bl[i].id == "REQ ") {
            nReq++;
            if ((size_t)bl[i].cantidad > bl[i].bytes / 4) { if (motivo) *motivo = "REQ cortado"; return false; }
            for (unsigned k = 0; k < bl[i].cantidad; k++) req.insert(std::string((const char*)bl[i].datos + k * 4, 4));
        } else if (bl[i].id == "HUES") nHues++;
    }
    if (nReq > 1 || nHues > 1) { if (motivo) *motivo = "bloque REQ o HUES repetido"; return false; }
    for (std::set<std::string>::iterator it = req.begin(); it != req.end(); ++it)
        if (!BloqueConocido(*it)) {
            if (motivo) *motivo = "pide el bloque '" + *it + "' de una version mas nueva de Whisk3D";
            return false;
        }
    // HUES primero: las pistas de los clips se validan contra la tabla
    for (size_t i = 0; i < bl.size(); i++) {
        if (bl[i].id != "HUES") continue;
        Cursor c(bl[i].datos, bl[i].bytes);
        if ((size_t)bl[i].cantidad > c.Quedan() / 4) { if (motivo) *motivo = "HUES con mas huesos de los que entran"; return false; }
        out.huesos.resize(bl[i].cantidad);
        for (unsigned k = 0; k < bl[i].cantidad; k++) out.huesos[k] = c.Str();
        if (!c.ok || c.Quedan() != 0) { out.huesos.clear(); if (motivo) *motivo = "HUES no cierra con el largo de su bloque"; return false; }
    }
    for (size_t i = 0; i < bl.size(); i++) {
        const BloqueA& b = bl[i];
        if (b.id == "REQ " || b.id == "HUES") continue;
        if (b.id == "CLIP") {
            SkeletalAnimation* clip = 0;
            if (!LeerClip(b, (int)out.huesos.size(), clip, m)) {
                for (size_t k = 0; k < out.clips.size(); k++) delete out.clips[k];
                out.clips.clear(); out.huesos.clear(); out.ajenos.clear();
                if (motivo) *motivo = m;
                return false;
            }
            out.clips.push_back(clip);
            continue;
        }
        if (b.id == "JCLP") {
            W3dClipJer* cj = 0;
            if (!LeerClipJer(b, cj, m)) {
                for (size_t k = 0; k < out.clips.size(); k++) delete out.clips[k];
                for (size_t k = 0; k < out.jerarquias.size(); k++) delete out.jerarquias[k];
                out.clips.clear(); out.huesos.clear(); out.ajenos.clear(); out.jerarquias.clear();
                if (motivo) *motivo = m;
                return false;
            }
            out.jerarquias.push_back(cj);
            continue;
        }
        // desconocido y no requerido: se PRESERVA tal cual
        W3dAnimSetBloqueAjeno a;
        a.id = b.id; a.dominio = b.dominio; a.cantidad = b.cantidad;
        a.datos.assign((const char*)b.datos, (size_t)b.bytes);
        out.ajenos.push_back(a);
    }
    return true;
}

bool W3dAnimSetListar(const unsigned char* d, size_t n, std::vector<std::string>& lineas) {
    lineas.clear();
    std::vector<BloqueA> bl;
    std::string m;
    if (!Ubicar(d, n, bl, m)) return false;
    for (size_t i = 0; i < bl.size(); i++) {
        char b[160];
        if (bl[i].id == "CLIP") {
            Cursor c(bl[i].datos, bl[i].bytes);
            std::string nom = c.Str();
            if (nom.size() > 60) nom.resize(60);
            sprintf(b, "CLIP '%s' %u pistas %u B", nom.c_str(), bl[i].cantidad, bl[i].bytes);
        } else if (bl[i].id == "JCLP") {
            Cursor c(bl[i].datos, bl[i].bytes);
            std::string nom = c.Str();
            if (nom.size() > 60) nom.resize(60);
            sprintf(b, "JCLP '%s' %u pistas %u B", nom.c_str(), bl[i].cantidad, bl[i].bytes);
        } else sprintf(b, "%s %u %u B", bl[i].id.c_str(), bl[i].cantidad, bl[i].bytes);
        lineas.push_back(b);
    }
    return true;
}

// ============================================================================
//  EL RECURSO
// ============================================================================
W3dAnimSetVariante::~W3dAnimSetVariante() {
    for (size_t i = 0; i < clips.size(); i++) delete clips[i];
}
W3dAnimSet::~W3dAnimSet() {
    for (size_t i = 0; i < variantes.size(); i++) delete variantes[i];
}

// FNV-1a de 32 bits + un segundo hash (djb2) sobre los mismos bytes: juntos, con el
// largo, alcanzan para decir "esta entrada es la misma que se leyo" (ver GuardarAnimSets)
void W3dAnimSetHuella(const unsigned char* d, size_t n, unsigned& h1, unsigned& h2) {
    h1 = 2166136261u; h2 = 5381u;
    for (size_t i = 0; i < n; i++) { h1 ^= d[i]; h1 *= 16777619u; h2 = h2 * 33u + d[i]; }
}

long W3dAnimSetClipBytes(const SkeletalAnimation* c, long* keys) {
    if (!c) return 0;
    long b = (long)sizeof(SkeletalAnimation) + (long)c->name.capacity();
    b += (long)(c->tracks.capacity() * sizeof(BoneTrack));
    long k = 0;
    for (size_t t = 0; t < c->tracks.size(); t++) {
        const BoneTrack& tr = c->tracks[t];
        b += (long)(tr.Propertys.capacity() * sizeof(AnimProperty));
        for (size_t p = 0; p < tr.Propertys.size(); p++) {
            b += (long)(tr.Propertys[p].keyframes.capacity() * sizeof(keyFrame));
            k += (long)tr.Propertys[p].keyframes.size();
        }
    }
    if (keys) *keys += k;
    return b;
}

// memoria aproximada de un clip de jerarquia (sus rutas, curvas y keyframes)
static long ClipJerBytes(const W3dClipJer* c, long* keys) {
    if (!c) return 0;
    long b = (long)sizeof(W3dClipJer) + (long)c->nombre.capacity() + (long)(c->pistas.capacity() * sizeof(W3dPistaJer));
    long k = 0;
    for (size_t t = 0; t < c->pistas.size(); t++) {
        const W3dPistaJer& p = c->pistas[t];
        b += (long)p.ruta.capacity() + (long)(p.props.capacity() * sizeof(AnimProperty));
        for (size_t q = 0; q < p.props.size(); q++) {
            b += (long)(p.props[q].keyframes.capacity() * sizeof(keyFrame));
            k += (long)p.props[q].keyframes.size();
        }
    }
    if (keys) *keys += k;
    return b;
}
static long MemoriaDe(const W3dAnimSet* s) {
    long b = (long)sizeof(W3dAnimSet);
    for (size_t i = 0; i < s->datos.clips.size(); i++) b += W3dAnimSetClipBytes(s->datos.clips[i], 0);
    for (size_t i = 0; i < s->datos.jerarquias.size(); i++) b += ClipJerBytes(s->datos.jerarquias[i], 0);
    return b;
}

// el ultimo motivo por el que la op Cargar no pudo (para el aviso del que la pidio)
static std::string gMotivoCarga;

static void* OpCargarAnimSet(const std::string& id, long* bytes) {
    std::vector<unsigned char> d;
    if (!w3dFileSystem::ReadFileBytes(id, d) || d.empty()) {
        gMotivoCarga = "no encuentro la entrada '" + id + "'";
        w3dLogfE("[W3D] animset: %s", gMotivoCarga.c_str());
        return 0;
    }
    W3dAnimSet* s = new W3dAnimSet();
    std::string motivo;
    if (!W3dAnimSetLeer(&d[0], d.size(), s->datos, &motivo)) {
        gMotivoCarga = "'" + id + "': " + motivo;
        w3dLogfE("[W3D] animset %s", gMotivoCarga.c_str());
        delete s;
        return 0;
    }
    W3dAnimSetHuella(&d[0], d.size(), s->huella1, s->huella2);
    s->tamArchivo = d.size();
    if (bytes) *bytes = MemoriaDe(s);
    return s;
}
static bool OpLiberarAnimSet(void* dato) { delete (W3dAnimSet*)dato; return true; }

void W3dAnimSetsAsegurarOps() {
    static bool listo = false;
    if (listo) return;
    W3dRecursoOps ops;
    ops.Cargar = OpCargarAnimSet;
    ops.Liberar = OpLiberarAnimSet;
    W3dRecursosRegistrarTipo(W3DREC_ANIMSET, ops);
    listo = true;
}

// ============================================================================
//  EL REGISTRO DEL PROYECTO
// ============================================================================
static std::vector<W3dAnimSetFila> gRegistro;

void W3dAnimSetsRegistroLimpiar() { gRegistro.clear(); }
void W3dAnimSetsRegistroAgregar(const std::string& nombre, const std::string& entrada, const std::string& carpeta) {
    for (size_t i = 0; i < gRegistro.size(); i++)
        if (gRegistro[i].nombre == nombre) { gRegistro[i].entrada = entrada; gRegistro[i].carpeta = carpeta; return; }
    W3dAnimSetFila f; f.nombre = nombre; f.entrada = entrada; f.carpeta = carpeta;
    gRegistro.push_back(f);
}
const std::vector<W3dAnimSetFila>& W3dAnimSetsRegistro() { return gRegistro; }
void W3dAnimSetsRegistroFijar(const std::vector<W3dAnimSetFila>& filas) { gRegistro = filas; }
// los de las LIBRERIAS externas (ver el .h): nombre con su prefijo -> entrada con su "lib:"
static std::vector<W3dAnimSetFila> gRegistroLib;
void W3dAnimSetsLibAgregar(const std::string& nombre, const std::string& entrada) {
    for (size_t i = 0; i < gRegistroLib.size(); i++)
        if (gRegistroLib[i].nombre == nombre) { gRegistroLib[i].entrada = entrada; return; }
    W3dAnimSetFila f; f.nombre = nombre; f.entrada = entrada;
    gRegistroLib.push_back(f);
}
void W3dAnimSetsLibLimpiar(const std::string& libreria) {
    if (libreria.empty()) { gRegistroLib.clear(); return; }
    const std::string pre = "lib:" + libreria + "/";
    for (size_t i = gRegistroLib.size(); i-- > 0; )
        if (gRegistroLib[i].entrada.compare(0, pre.size(), pre) == 0) gRegistroLib.erase(gRegistroLib.begin() + (long)i);
}
bool W3dAnimSetEsDeLibreria(const std::string& nombre) {
    for (size_t i = 0; i < gRegistroLib.size(); i++) if (gRegistroLib[i].nombre == nombre) return true;
    return false;
}
std::string W3dAnimSetsEntradaDe(const std::string& nombre) {
    for (size_t i = 0; i < gRegistro.size(); i++) if (gRegistro[i].nombre == nombre) return gRegistro[i].entrada;
    for (size_t i = 0; i < gRegistroLib.size(); i++) if (gRegistroLib[i].nombre == nombre) return gRegistroLib[i].entrada;
    return std::string();
}

std::string W3dAnimSetIdEntrada(const std::string& id) {
    if (id.empty() || id[0] != '#') return id;
    const size_t p = id.find(':');
    return (p == std::string::npos) ? std::string() : id.substr(p + 1);
}

bool W3dAnimSetDesenganchar(W3dRecurso* r, const char* motivo, int n) {
    if (!r) return false;
    // SIN REFERENCIAS y sin dato (un FALLO recordado, uno que nunca se cargo): se OLVIDA. Correrlo
    // de clave no alcanzaba: quedaba para siempre en el almacen (un descriptor zombie por cada
    // proyecto con un .w3da roto que se cerraba) y el cierre siguiente le volvia a agregar otro
    // prefijo (los ids crecian sin tope). Es el mismo criterio que las mallas (W3dRecursoOlvidar).
    if (r->refTotal <= 0 && r->estado != W3DREC_LISTO) return W3dRecursoOlvidar(W3DREC_ANIMSET, r->id);
    // con usuarios: a una clave que no es una entrada, armada sobre la ENTRADA (sin los prefijos
    // de un desenganche anterior). Cuando el ultimo usuario lo suelte, el almacen lo libera.
    char b[64]; sprintf(b, "#%s%d:", motivo ? motivo : "", n);
    W3dRecursoRenombrar(W3DREC_ANIMSET, r->id, std::string(b) + W3dAnimSetIdEntrada(r->id));
    return false;
}

void W3dAnimSetsCerrarProyecto() {
    gRegistro.clear();
    gRegistroLib.clear();   // (las librerias se cierran con el proyecto)
    // los descriptores que NO estan listos (un FALLO recordado, uno que nunca se cargo) dejan
    // libre la clave de su entrada: si el proyecto siguiente tiene una entrada homonima, se
    // tiene que cargar de nuevo (sin esto el FALLO la bloqueaba para siempre). Los que nadie
    // referencia se BORRAN; los que todavia tienen usuarios (una ListaCarga en vuelo) se corren
    // a una clave que no es una entrada, UNA vez (el que ya esta corrido no ocupa ninguna).
    // Los LISTO no se tocan: si alguno quedo vivo es porque algo todavia lo usa.
    static int nOlvidados = 0;
    std::vector<W3dRecurso*> todos;
    W3dRecursosListar(W3DREC_ANIMSET, todos);
    for (size_t i = 0; i < todos.size(); i++) {
        W3dRecurso* r = todos[i];
        if (!r || r->estado == W3DREC_LISTO) continue;
        if (r->refTotal > 0 && !r->id.empty() && r->id[0] == '#') continue;   // ya desenganchado
        W3dAnimSetDesenganchar(r, "olvidado", ++nOlvidados);
    }
}

// ============================================================================
//  ARMATURES
// ============================================================================
void W3dArmatureNombresHuesos(const Armature* a, std::vector<std::string>& out) {
    out.clear();
    if (!a) return;
    out.reserve(a->bones.size());
    for (size_t i = 0; i < a->bones.size(); i++) out.push_back(a->bones[i].name);
}

// tabla -> armature: la entrada s de la tabla es el k-esimo hueso con ese nombre (k = cuantas
// veces aparecio antes en la tabla); en el armature se busca el k-esimo con el mismo nombre
static void MapaTablaAArmature(const std::vector<std::string>& huesos, const Armature* a, std::vector<int>& mapa) {
    mapa.assign(huesos.size(), -1);
    if (!a) return;
    std::map<std::string, std::vector<int> > porNombre;
    for (size_t i = 0; i < a->bones.size(); i++) porNombre[a->bones[i].name].push_back((int)i);
    std::map<std::string, int> vistos;
    for (size_t s = 0; s < huesos.size(); s++) {
        const int k = vistos[huesos[s]]++;
        std::map<std::string, std::vector<int> >::const_iterator it = porNombre.find(huesos[s]);
        if (it != porNombre.end() && k < (int)it->second.size()) mapa[s] = it->second[k];
    }
}

void W3dAnimSetMapaDesdeArmature(const std::vector<std::string>& huesos, const Armature* a, std::vector<int>& out) {
    out.clear();
    if (!a) return;
    out.assign(a->bones.size(), -1);
    // camino comun: el mismo esqueleto (mismos nombres en el mismo orden) = identidad
    bool igual = a->bones.size() >= huesos.size();
    for (size_t i = 0; igual && i < huesos.size(); i++) if (a->bones[i].name != huesos[i]) igual = false;
    if (igual) { for (size_t i = 0; i < huesos.size(); i++) out[i] = (int)i; return; }
    std::vector<int> m;
    MapaTablaAArmature(huesos, a, m);
    for (size_t s = 0; s < m.size(); s++) if (m[s] >= 0 && out[m[s]] < 0) out[m[s]] = (int)s;
}

// copia del clip canonico k con los huesos traducidos al armature (una variante)
static SkeletalAnimation* ClipDeVariante(W3dAnimSet* set, W3dAnimSetVariante* v, size_t k) {
    if (k >= set->datos.clips.size()) return 0;
    if (v->clips.size() != set->datos.clips.size()) v->clips.resize(set->datos.clips.size(), (SkeletalAnimation*)0);
    if (!v->clips[k]) {
        const SkeletalAnimation* src = set->datos.clips[k];
        if (!src) return 0;
        SkeletalAnimation* c = new SkeletalAnimation(*src);
        for (size_t t = 0; t < c->tracks.size(); t++) {
            const int b = c->tracks[t].bone;
            c->tracks[t].bone = (b >= 0 && b < (int)v->mapa.size()) ? v->mapa[b] : -1;
        }
        v->clips[k] = c;
    }
    return v->clips[k];
}

bool W3dArmatureAsignarAnimSet(Armature* a, const std::string& nombre, const std::vector<int>* indices,
                               std::string* motivo) {
    if (!a) return false;
    W3dAnimSetsAsegurarOps();
    // lo que tuviera antes se va (propio: se borra; compartido: se suelta)
    if (a->animsRec) W3dArmatureAnimsSoltar(a);
    else { for (size_t i = 0; i < a->animations.size(); i++) delete a->animations[i]; a->animations.clear(); }
    const std::string entrada = W3dAnimSetsEntradaDe(nombre);
    if (entrada.empty()) {
        if (motivo) *motivo = "el animset '" + nombre + "' no esta en el registro del proyecto (\"animsets\")";
        return false;
    }
    gMotivoCarga.clear();
    W3dRecurso* r = W3dRecursoAdquirir(W3DREC_ANIMSET, entrada, W3DREC_PERMANENTE, W3DREC_BLOQUEANTE, a->name.c_str());
    // uno EN VUELO (lo pidio una lista de carga async): el armature lo necesita YA
    for (int i = 0; r && r->estado == W3DREC_EN_VUELO && i < 100000; i++) W3dRecursosPump();
    if (!r || r->estado != W3DREC_LISTO || !r->dato) {
        // Adquirir no suma la ref si la carga fallo; si quedo EN VUELO si la sumo
        if (r && r->estado == W3DREC_EN_VUELO) W3dRecursoSoltar(r, W3DREC_PERMANENTE);
        if (motivo) *motivo = gMotivoCarga.empty() ? ("no se pudo cargar '" + entrada + "'") : gMotivoCarga;
        return false;
    }
    W3dAnimSet* set = (W3dAnimSet*)r->dato;
    if (set->nombre.empty()) set->nombre = nombre;
    const size_t nClips = set->datos.clips.size();
    if (indices)
        for (size_t i = 0; i < indices->size(); i++)
            if ((*indices)[i] < 0 || (*indices)[i] >= (int)nClips) {
                char b[160];
                sprintf(b, "el clip %d no existe en el animset (tiene %d)", (*indices)[i], (int)nClips);
                if (motivo) *motivo = b;
                W3dRecursoSoltar(r, W3DREC_PERMANENTE);
                return false;
            }
    // ---- el esqueleto: calza tal cual (identidad) o hace falta una variante ----
    std::vector<int> mapa;
    MapaTablaAArmature(set->datos.huesos, a, mapa);
    bool identidad = true;
    for (size_t s = 0; s < mapa.size(); s++) if (mapa[s] != (int)s) { identidad = false; break; }
    W3dAnimSetVariante* var = 0;
    if (!identidad) {
        for (size_t v = 0; v < set->variantes.size() && !var; v++) if (set->variantes[v]->mapa == mapa) var = set->variantes[v];
        if (!var) { var = new W3dAnimSetVariante(); var->mapa = mapa; set->variantes.push_back(var); }
    }
    std::vector<SkeletalAnimation*> nuevos;
    if (indices) {
        nuevos.reserve(indices->size());
        for (size_t i = 0; i < indices->size(); i++) {
            const size_t k = (size_t)(*indices)[i];
            nuevos.push_back(var ? ClipDeVariante(set, var, k) : set->datos.clips[k]);
        }
    } else {
        nuevos.reserve(nClips);
        for (size_t k = 0; k < nClips; k++) nuevos.push_back(var ? ClipDeVariante(set, var, k) : set->datos.clips[k]);
    }
    // NOMBRES que la reparacion del editor tocaria: REPETIDOS dentro del armature (el mismo clip dos
    // veces, o dos clips homonimos) o SIN NORMALIZAR (vacio, con blancos en las puntas). El editor los
    // repara al abrir (W3dNombresRepararEscena, espacio CLIP "Anim" de cada armature) escribiendo el
    // nombre A TRAVES DEL PUNTERO. Con los clips compartidos eso renombraria el clip de TODOS: este
    // armature pasa a clips propios ya mismo (caso raro: un archivo editado a mano o de antes).
    std::set<std::string> vistos;
    bool repetidos = false;
    for (size_t i = 0; i < nuevos.size() && !repetidos; i++) {
        if (!nuevos[i]) continue;
        const std::string& n = nuevos[i]->name;
        if (W3dNombreNormalizar(n, "Anim") != n || !vistos.insert(n).second) repetidos = true;
    }
    a->animations.swap(nuevos);
    a->animsRec = r;
    a->animsVar = var;
    a->animSetNombre = nombre;
    a->animSetNoCargo.clear();
    a->lastPoseFrame = -999999; a->lastPoseAnim = -999; a->poseDirty = true;
    if (repetidos) W3dArmatureAnimsPropias(a);
    return true;
}

// saca 'c' del recurso (su casillero queda en NULL: ya no lo libera el recurso)
static bool QuitarDelRecurso(W3dAnimSet* set, SkeletalAnimation* c) {
    for (size_t k = 0; k < set->datos.clips.size(); k++)
        if (set->datos.clips[k] == c) { set->datos.clips[k] = 0; return true; }
    for (size_t v = 0; v < set->variantes.size(); v++) {
        std::vector<SkeletalAnimation*>& vc = set->variantes[v]->clips;
        for (size_t k = 0; k < vc.size(); k++) if (vc[k] == c) { vc[k] = 0; return true; }
    }
    return false;
}

static int gCopias = 0;
int W3dAnimSetsCopias() { return gCopias; }

void W3dArmatureAnimsPropias(Armature* a) {
    if (!a || !a->animsRec) return;
    W3dRecurso* r = (W3dRecurso*)a->animsRec;
    W3dAnimSet* set = (r->estado == W3DREC_LISTO) ? (W3dAnimSet*)r->dato : 0;
    // UNICO usuario: los clips se le PASAN (sin copiar); el recurso muere enseguida con los
    // casilleros vacios. Si no, cada casillero es una copia propia (tambien los repetidos:
    // un clip que el armature usa dos veces pasa a ser dos clips independientes)
    const bool solo = set && r->refTotal <= 1;
    std::set<SkeletalAnimation*> pasados;
    for (size_t i = 0; i < a->animations.size(); i++) {
        SkeletalAnimation* c = a->animations[i];
        if (!c) continue;
        if (solo && !pasados.count(c) && QuitarDelRecurso(set, c)) { pasados.insert(c); continue; }
        a->animations[i] = new SkeletalAnimation(*c);
    }
    a->animsRec = 0;
    a->animsVar = 0;
    gCopias++;
    W3dRecursoSoltar(r, W3DREC_PERMANENTE);
}

void W3dArmatureAnimsSoltar(Armature* a) {
    if (!a || !a->animsRec) return;
    W3dRecurso* r = (W3dRecurso*)a->animsRec;
    a->animations.clear();
    a->animsRec = 0;
    a->animsVar = 0;
    W3dRecursoSoltar(r, W3DREC_PERMANENTE);
}

// ============================================================================
//  DUPLICAR UN ARMATURE (Shift+D / Alt+D; ver W3dAnimSet.h)
// ============================================================================
// lo que el armature tuviera se va (propio: se borra; compartido: se suelta)
static void VaciarClips(Armature* a) {
    if (a->animsRec) { W3dArmatureAnimsSoltar(a); return; }
    for (size_t i = 0; i < a->animations.size(); i++) delete a->animations[i];
    a->animations.clear();
}

void W3dArmatureAnimsCopiar(Armature* dst, const Armature* src) {
    if (!dst || !src || dst == src) return;
    VaciarClips(dst);
    // cada casillero es un clip NUEVO (tambien los repetidos: un clip que src usa dos veces pasa a
    // ser dos clips independientes, igual que en el copy-on-write)
    dst->animations.reserve(src->animations.size());
    for (size_t i = 0; i < src->animations.size(); i++)
        dst->animations.push_back(src->animations[i] ? new SkeletalAnimation(*src->animations[i]) : (SkeletalAnimation*)0);
    dst->animSetNombre = src->animSetNombre;
    dst->animSetNoCargo = src->animSetNoCargo;
    dst->lastPoseFrame = -999999; dst->lastPoseAnim = -999;
}

// los clips PROPIOS de 'a' pasan a ser un animset EN MEMORIA, sin copiar: el recurso se queda con
// esos MISMOS objetos (sus pistas ya estan en el espacio de la tabla, que es el esqueleto de 'a').
// Su id no es una entrada ("#memoriaN:<nombre>"); el guardado le da una y, si lo que escribe es
// igual a esto (la HUELLA de abajo), el recurso pasa a llamarse como ella: queda igual que uno
// leido del disco (GuardarAnimSetsConfirmar).
static int gEnMemoria = 0;
static bool AnimSetEnMemoria(Armature* a) {
    if (!a || a->animsRec || a->animations.empty()) return false;
    // un mismo clip dos veces o un nombre que la reparacion del editor tocaria: compartidos, eso
    // escribiria el clip de TODOS (mismo criterio que W3dArmatureAsignarAnimSet)
    std::set<const SkeletalAnimation*> vistos;
    std::set<std::string> nombres;
    for (size_t i = 0; i < a->animations.size(); i++) {
        const SkeletalAnimation* c = a->animations[i];
        if (!c || !vistos.insert(c).second) return false;
        if (W3dNombreNormalizar(c->name, "Anim") != c->name || !nombres.insert(c->name).second) return false;
    }
    W3dAnimSetsAsegurarOps();
    W3dAnimSet* set = new W3dAnimSet();
    set->nombre = a->animSetNombre.empty() ? a->name : a->animSetNombre;
    W3dArmatureNombresHuesos(a, set->datos.huesos);
    // la HUELLA de lo que escribiria el guardado con esta biblioteca tal cual (mapa identidad: la
    // tabla es el esqueleto), con el mismo escritor que usa el guardado
    std::vector<int> identidad(set->datos.huesos.size());
    for (size_t h = 0; h < identidad.size(); h++) identidad[h] = (int)h;
    std::vector<std::string> bloques(a->animations.size());
    std::vector<const std::string*> ptrs;
    for (size_t i = 0; i < a->animations.size(); i++) {
        W3dAnimSetClipBloque(*a->animations[i], identidad, (int)identidad.size(), bloques[i]);
        ptrs.push_back(&bloques[i]);
    }
    std::string bytes;
    W3dAnimSetArmar(set->datos.huesos, ptrs, &set->datos.ajenos, bytes);
    W3dAnimSetHuella((const unsigned char*)bytes.data(), bytes.size(), set->huella1, set->huella2);
    set->tamArchivo = bytes.size();
    set->datos.clips = a->animations;
    char b[48]; sprintf(b, "#memoria%d:", ++gEnMemoria);
    W3dRecurso* r = W3dRecursoRegistrarExterno(W3DREC_ANIMSET, std::string(b) + set->nombre, set, MemoriaDe(set));
    if (!r) { set->datos.clips.clear(); delete set; return false; }   // (los clips siguen siendo de 'a')
    a->animsRec = r;   // RegistrarExterno deja refTotal = 1: la de 'a'
    a->animsVar = 0;
    a->animSetNombre = set->nombre;
    return true;
}

bool W3dArmatureAnimsVincular(Armature* dst, Armature* src) {
    if (!dst || !src || dst == src) return false;
    VaciarClips(dst);
    dst->animSetNoCargo = src->animSetNoCargo;
    dst->lastPoseFrame = -999999; dst->lastPoseAnim = -999;
    if (src->animations.empty()) { dst->animSetNombre = src->animSetNombre; return true; }   // nada que compartir
    if (!src->animsRec && !AnimSetEnMemoria(src)) { W3dArmatureAnimsCopiar(dst, src); return false; }
    W3dRecurso* r = (W3dRecurso*)src->animsRec;
    W3dRecursoRetener(r, W3DREC_PERMANENTE);
    dst->animations = src->animations;
    dst->animsRec = r;
    dst->animsVar = src->animsVar;   // el mismo esqueleto: el mismo mapeo (la misma variante, si hay)
    dst->animSetNombre = src->animSetNombre;
    return true;
}

bool W3dArmatureAnimsQuitar(Armature* a, int i) {
    if (!a || i < 0 || i >= (int)a->animations.size()) return false;
    // el UNICO usuario: pasarle los clips no copia nada y el borrado se libera de verdad (si
    // quedaba colgado, el recurso seguia reteniendo un clip que ya no usa nadie). Con una
    // VARIANTE no: sus clips tienen en -1 las pistas de los huesos que le faltan y, propios, el
    // guardado ya no podria escribir los canonicos (el animset perderia esas pistas)
    const W3dRecurso* r = (const W3dRecurso*)a->animsRec;
    if (r && r->refTotal <= 1 && !a->animsVar) W3dArmatureAnimsPropias(a);
    if (a->animsRec) {
        // COMPARTIDOS: el clip es del animset (lo siguen usando los demas): solo se suelta el
        // puntero. El armature sigue colgado con los que le quedan, sin copiar ninguno.
        a->animations.erase(a->animations.begin() + i);
        if (a->animations.empty()) W3dArmatureAnimsSoltar(a);   // sin clips ya no lo usa
        return true;
    }
    delete a->animations[i];
    a->animations.erase(a->animations.begin() + i);
    return true;
}

bool W3dArmatureAnimsCompartidas(const Armature* a) { return a && a->animsRec; }

W3dRecurso* W3dArmatureAnimSetRecurso(const Armature* a) { return a ? (W3dRecurso*)a->animsRec : 0; }

bool W3dArmatureAnimSetCalza(const Armature* a) {
    const W3dAnimSet* set = W3dArmatureAnimSet(a);
    if (!set) return false;
    std::vector<int> mapa;
    MapaTablaAArmature(set->datos.huesos, a, mapa);
    const W3dAnimSetVariante* var = (const W3dAnimSetVariante*)a->animsVar;
    if (var) return mapa == var->mapa;
    for (size_t s = 0; s < mapa.size(); s++) if (mapa[s] != (int)s) return false;
    return true;
}

W3dAnimSet* W3dArmatureAnimSet(const Armature* a) {
    W3dRecurso* r = W3dArmatureAnimSetRecurso(a);
    return (r && r->estado == W3DREC_LISTO) ? (W3dAnimSet*)r->dato : 0;
}

bool W3dArmatureAnimsIndices(const Armature* a, std::vector<int>& out) {
    out.clear();
    W3dAnimSet* set = W3dArmatureAnimSet(a);
    if (!set) return false;
    const W3dAnimSetVariante* var = (const W3dAnimSetVariante*)a->animsVar;
    const std::vector<SkeletalAnimation*>& lista = var ? var->clips : set->datos.clips;
    std::map<const SkeletalAnimation*, int> idx;
    for (size_t k = 0; k < lista.size(); k++) if (lista[k]) idx[lista[k]] = (int)k;
    bool ok = true;
    for (size_t i = 0; i < a->animations.size(); i++) {
        std::map<const SkeletalAnimation*, int>::const_iterator it = idx.find(a->animations[i]);
        if (it == idx.end()) { out.push_back(-1); ok = false; }
        else out.push_back(it->second);
    }
    return ok;
}

// ============================================================================
//  MEDICION
// ============================================================================
void W3dAnimSetsEstadisticas(W3dAnimSetsStats& st) {
    st = W3dAnimSetsStats();
    std::vector<W3dRecurso*> todos;
    W3dRecursosListar(W3DREC_ANIMSET, todos);
    for (size_t i = 0; i < todos.size(); i++) {
        W3dRecurso* r = todos[i];
        if (!r || r->estado != W3DREC_LISTO || !r->dato) continue;
        const W3dAnimSet* s = (const W3dAnimSet*)r->dato;
        st.vivos++;
        st.refs += r->refTotal;
        st.bytes += (long)sizeof(W3dAnimSet);
        for (size_t k = 0; k < s->datos.clips.size(); k++) {
            if (!s->datos.clips[k]) continue;
            st.clips++;
            st.bytes += W3dAnimSetClipBytes(s->datos.clips[k], &st.keys);
        }
        for (size_t k = 0; k < s->datos.jerarquias.size(); k++) {
            if (!s->datos.jerarquias[k]) continue;
            st.clipsJer++;
            st.bytes += ClipJerBytes(s->datos.jerarquias[k], &st.keysJer);
        }
        for (size_t v = 0; v < s->variantes.size(); v++) {
            st.variantes++;
            for (size_t k = 0; k < s->variantes[v]->clips.size(); k++) {
                if (!s->variantes[v]->clips[k]) continue;
                st.clipsVariante++;
                st.bytes += W3dAnimSetClipBytes(s->variantes[v]->clips[k], &st.keys);
            }
        }
    }
}

// ============================================================================
//  OBJETOS RAIZ DE CLIPS DE JERARQUIA (ver W3dAnimSet.h). La biblioteca de un objeto es
//  un recurso ANIMSET como cualquier otro: el objeto le toma una referencia PERMANENTE
//  (la suelta al cambiarla o al morir) y los clips viven UNA vez en el recurso.
// ============================================================================
W3dRecurso* W3dJerRecurso(const Object* o) { return (o && o->clipsJer) ? o->clipsJer->rec : 0; }
W3dAnimSet* W3dJerAnimSet(const Object* o) {
    W3dRecurso* r = W3dJerRecurso(o);
    return (r && r->estado == W3DREC_LISTO) ? (W3dAnimSet*)r->dato : 0;
}

W3dRecurso* W3dJerBibliotecaCargada(const std::string& nombre) {
    if (nombre.empty()) return 0;
    std::vector<W3dRecurso*> todos;
    W3dRecursosListar(W3DREC_ANIMSET, todos);
    for (size_t i = 0; i < todos.size(); i++)
        if (todos[i] && todos[i]->estado == W3DREC_LISTO && todos[i]->dato &&
            ((W3dAnimSet*)todos[i]->dato)->nombre == nombre) return todos[i];
    return 0;
}

std::string W3dJerNombreBibliotecaLibre(const std::string& base) {
    std::vector<std::string> tomados;
    const std::vector<W3dAnimSetFila>& reg = W3dAnimSetsRegistro();
    for (size_t i = 0; i < reg.size(); i++) tomados.push_back(reg[i].nombre);
    std::vector<W3dRecurso*> todos;
    W3dRecursosListar(W3DREC_ANIMSET, todos);
    for (size_t i = 0; i < todos.size(); i++)
        if (todos[i] && todos[i]->estado == W3DREC_LISTO && todos[i]->dato && !((W3dAnimSet*)todos[i]->dato)->nombre.empty())
            tomados.push_back(((W3dAnimSet*)todos[i]->dato)->nombre);
    return W3dNombreUnicoEnValores(W3dNombreNormalizar(base, "Clips"), "Clips", tomados, -1);
}

static W3dJerRaiz* JerDe(Object* o) {
    if (!o->clipsJer) o->clipsJer = new W3dJerRaiz();
    return o->clipsJer;
}
static void JerSoltarRec(W3dJerRaiz* j) {
    if (j && j->rec) { W3dRecurso* r = j->rec; j->rec = 0; W3dRecursoSoltar(r, W3DREC_PERMANENTE); }
}

// la biblioteca 'nombre' CON una referencia PERMANENTE (la cargada con ese nombre o la del registro,
// leida de su entrada). NULL + motivo si no se puede.
static W3dRecurso* JerAdquirirBiblioteca(const std::string& nombre, const char* quien, std::string* motivo) {
    W3dAnimSetsAsegurarOps();
    W3dRecurso* r = W3dJerBibliotecaCargada(nombre);
    if (r) { W3dRecursoRetener(r, W3DREC_PERMANENTE); return r; }
    const std::string entrada = W3dAnimSetsEntradaDe(nombre);
    if (entrada.empty()) {
        if (motivo) *motivo = "la biblioteca de clips '" + nombre + "' no esta en el registro del proyecto (\"animsets\")";
        return 0;
    }
    gMotivoCarga.clear();
    r = W3dRecursoAdquirir(W3DREC_ANIMSET, entrada, W3DREC_PERMANENTE, W3DREC_BLOQUEANTE, quien ? quien : "");
    for (int i = 0; r && r->estado == W3DREC_EN_VUELO && i < 100000; i++) W3dRecursosPump();
    if (!r || r->estado != W3DREC_LISTO || !r->dato) {
        if (r && r->estado == W3DREC_EN_VUELO) W3dRecursoSoltar(r, W3DREC_PERMANENTE);
        if (motivo) *motivo = gMotivoCarga.empty() ? ("no se pudo cargar '" + entrada + "'") : gMotivoCarga;
        return 0;
    }
    W3dAnimSet* set = (W3dAnimSet*)r->dato;
    if (set->nombre.empty()) set->nombre = nombre;
    return r;
}

W3dRecurso* W3dJerBibliotecaTomar(const std::string& nombre, const char* quien, std::string* motivo) {
    if (nombre.empty()) { if (motivo) *motivo = "sin nombre"; return 0; }
    return JerAdquirirBiblioteca(nombre, quien, motivo);
}

bool W3dJerAsignar(Object* o, const std::string& nombre, std::string* motivo) {
    if (!o) return false;
    if (nombre.empty()) { W3dJerSoltar(o); return true; }
    std::string m;
    W3dRecurso* r = JerAdquirirBiblioteca(nombre, o->name.c_str(), &m);
    W3dJerRaiz* j = JerDe(o);
    JerSoltarRec(j);   // (despues de tomar la nueva: si es la misma, no se libera en el medio)
    j->animset = nombre;
    if (!r) {
        j->noCargo = nombre;
        if (motivo) *motivo = m;
        w3dLogfE("[W3D] '%s': %s", o->name.c_str(), m.c_str());
        return false;
    }
    j->rec = r;
    j->noCargo.clear();
    return true;
}

void W3dJerSoltar(Object* o) {
    if (!o || !o->clipsJer) return;
    W3dJerRaiz* j = o->clipsJer;
    JerSoltarRec(j);
    j->animset.clear();
    j->noCargo.clear();
    // (sin biblioteca, ni retarget propio, ni capas no hay nada que recordar)
    if (j->retarget < 0 && j->capas.empty()) { delete j; o->clipsJer = 0; }
}

void W3dJerRestaurar(Object* o, W3dRecurso* r, const std::string& animset, const std::string& noCargo) {
    if (!o) return;
    if (!r && animset.empty() && noCargo.empty()) { W3dJerSoltar(o); return; }
    if (r) W3dRecursoRetener(r, W3DREC_PERMANENTE);   // (antes de soltar la actual: puede ser la misma)
    W3dJerRaiz* j = JerDe(o);
    JerSoltarRec(j);
    j->rec = r;
    j->animset = animset;
    j->noCargo = noCargo;
}

void W3dJerVincular(Object* dst, const Object* src) {
    if (!dst || dst == src) return;
    W3dJerSoltar(dst);
    if (!src || !src->clipsJer) return;
    W3dJerRaiz* j = JerDe(dst);
    j->animset = src->clipsJer->animset;
    j->noCargo = src->clipsJer->noCargo;
    j->retarget = src->clipsJer->retarget;
    if (src->clipsJer->rec) { W3dRecursoRetener(src->clipsJer->rec, W3DREC_PERMANENTE); j->rec = src->clipsJer->rec; }
    // sus CAPAS del Mix (los clips se nombran: la copia mezcla los mismos, cada una con su cabezal)
    j->capas = src->clipsJer->capas;
    j->capaActiva = src->clipsJer->capaActiva;
    W3dJerCapasCambiaron(dst);
}

bool W3dJerBibliotecaNueva(Object* o, const std::string& nombre) {
    if (!o) return false;
    W3dAnimSetsAsegurarOps();
    W3dAnimSet* set = new W3dAnimSet();
    set->nombre = W3dJerNombreBibliotecaLibre(nombre);
    char b[48]; sprintf(b, "#memoria%d:", ++gEnMemoria);
    W3dRecurso* r = W3dRecursoRegistrarExterno(W3DREC_ANIMSET, std::string(b) + set->nombre, set, MemoriaDe(set));
    if (!r) { delete set; return false; }
    W3dJerRaiz* j = JerDe(o);
    JerSoltarRec(j);
    j->rec = r;   // RegistrarExterno deja refTotal = 1: la de 'o'
    j->animset = set->nombre;
    j->noCargo.clear();
    w3dLogf("[W3D] '%s': biblioteca de clips nueva '%s' (en memoria)", o->name.c_str(), set->nombre.c_str());
    return true;
}

W3dClipJer* W3dJerClip(const Object* o, const std::string& nombre) {
    const W3dAnimSet* set = W3dJerAnimSet(o);
    if (!set) return 0;
    for (size_t i = 0; i < set->datos.jerarquias.size(); i++)
        if (set->datos.jerarquias[i] && set->datos.jerarquias[i]->nombre == nombre) return set->datos.jerarquias[i];
    return 0;
}

// nombre libre entre los clips de jerarquia de 'set' ('excepto' conserva el suyo)
static std::string ClipJerNombreLibre(const W3dAnimSet* set, const std::string& base, const W3dClipJer* excepto) {
    std::vector<std::string> tomados;
    for (size_t i = 0; set && i < set->datos.jerarquias.size(); i++)
        if (set->datos.jerarquias[i] && set->datos.jerarquias[i] != excepto) tomados.push_back(set->datos.jerarquias[i]->nombre);
    return W3dNombreUnicoEnValores(W3dNombreNormalizar(base, "Clip"), "Clip", tomados, -1);
}

W3dClipJer* W3dJerClipNuevo(Object* o, const std::string& nombre) {
    if (!o) return 0;
    if (!W3dJerAnimSet(o) && !W3dJerBibliotecaNueva(o, o->name)) return 0;
    W3dAnimSet* set = W3dJerAnimSet(o);
    if (!set) return 0;
    W3dClipJer* c = new W3dClipJer();
    c->nombre = ClipJerNombreLibre(set, nombre, 0);
    c->tamano = W3dJerTamano(o);   // (el retarget de otra jerarquia se mide contra esta)
    set->datos.jerarquias.push_back(c);
    return c;
}

bool W3dJerClipBorrar(Object* o, W3dClipJer* c) {
    if (W3dJerClipSacar(W3dJerRecurso(o), c) < 0) return false;
    delete c;
    return true;
}

W3dAnimSet* W3dJerBiblioteca(W3dRecurso* r) {
    return (r && r->tipo == W3DREC_ANIMSET && r->estado == W3DREC_LISTO) ? (W3dAnimSet*)r->dato : 0;
}

int W3dJerClipSacar(W3dRecurso* r, W3dClipJer* c) {
    W3dAnimSet* set = W3dJerBiblioteca(r);
    if (!set || !c) return -1;
    for (size_t i = 0; i < set->datos.jerarquias.size(); i++)
        if (set->datos.jerarquias[i] == c) {
            set->datos.jerarquias.erase(set->datos.jerarquias.begin() + (long)i);
            return (int)i;
        }
    return -1;
}

bool W3dJerClipDevolver(W3dRecurso* r, W3dClipJer* c, int pos) {
    W3dAnimSet* set = W3dJerBiblioteca(r);
    if (!set || !c) return false;
    for (size_t i = 0; i < set->datos.jerarquias.size(); i++) if (set->datos.jerarquias[i] == c) return false;
    if (pos < 0) pos = 0;
    if (pos > (int)set->datos.jerarquias.size()) pos = (int)set->datos.jerarquias.size();
    // (el nombre: unico entre los de su biblioteca, por si mientras estuvo afuera se creo otro igual)
    c->nombre = ClipJerNombreLibre(set, c->nombre, c);
    set->datos.jerarquias.insert(set->datos.jerarquias.begin() + (long)pos, c);
    c->version++;   // (las vistas que lo editaban se re-atan: pudo cambiar el nombre)
    return true;
}

const W3dClipJer* W3dJerCapaClip(const Object* o, const W3dCapaAnim& cp, W3dRecurso** recOut) {
    if (recOut) *recOut = 0;
    if (!o || cp.anim.empty()) return 0;
    // la biblioteca de la raiz
    W3dClipJer* c = W3dJerClip(o, cp.anim);
    if (c) { if (recOut) *recOut = W3dJerRecurso(o); return c; }
    // "Biblioteca/clip": solo entre las CARGADAS (una capa no carga ni retiene nada)
    const size_t barra = cp.anim.find('/');
    if (barra != std::string::npos && barra > 0 && barra + 1 < cp.anim.size()) {
        W3dRecurso* r = W3dJerBibliotecaCargada(cp.anim.substr(0, barra));
        const W3dAnimSet* set = W3dJerBiblioteca(r);
        if (!set) return 0;
        const std::string nom = cp.anim.substr(barra + 1);
        for (size_t i = 0; i < set->datos.jerarquias.size(); i++)
            if (set->datos.jerarquias[i] && set->datos.jerarquias[i]->nombre == nom) {
                if (recOut) *recOut = r;
                return set->datos.jerarquias[i];
            }
        return 0;
    }
    // suelto: el UNICO con ese nombre entre las cargadas
    const W3dClipJer* hallado = 0;
    W3dRecurso* de = 0;
    int n = 0;
    std::vector<W3dRecurso*> todos;
    W3dRecursosListar(W3DREC_ANIMSET, todos);
    for (size_t i = 0; i < todos.size(); i++) {
        const W3dAnimSet* set = W3dJerBiblioteca(todos[i]);
        if (!set) continue;
        for (size_t k = 0; k < set->datos.jerarquias.size(); k++)
            if (set->datos.jerarquias[k] && set->datos.jerarquias[k]->nombre == cp.anim) { hallado = set->datos.jerarquias[k]; de = todos[i]; n++; }
    }
    if (n != 1) return 0;
    if (recOut) *recOut = de;
    return hallado;
}

std::string W3dJerClipRenombrar(W3dAnimSet* set, W3dClipJer* c, const std::string& nuevo) {
    if (!c) return std::string();
    const std::string n = ClipJerNombreLibre(set, nuevo, c);
    if (n != c->nombre) { c->nombre = n; c->version++; }
    return c->nombre;
}

bool W3dJerClipVivo(const W3dRecurso* r, const W3dClipJer* c, unsigned serie) {
    if (!r || !c || r->estado != W3DREC_LISTO || !r->dato) return false;
    const W3dAnimSet* set = (const W3dAnimSet*)r->dato;
    for (size_t i = 0; i < set->datos.jerarquias.size(); i++)
        if (set->datos.jerarquias[i] == c) return c->serie == serie;
    return false;
}

W3dClipJer* W3dJerClipBuscar(const std::string& nombre, W3dRecurso** recOut) {
    if (recOut) *recOut = 0;
    const size_t barra = nombre.find('/');
    if (barra != std::string::npos && barra > 0 && barra + 1 < nombre.size()) {
        // "Biblioteca/clip": esa biblioteca (se carga de su entrada si hace falta)
        W3dRecurso* r = JerAdquirirBiblioteca(nombre.substr(0, barra), "animObjeto", 0);
        if (!r) return 0;
        const W3dAnimSet* set = (const W3dAnimSet*)r->dato;
        const std::string clip = nombre.substr(barra + 1);
        for (size_t i = 0; i < set->datos.jerarquias.size(); i++)
            if (set->datos.jerarquias[i] && set->datos.jerarquias[i]->nombre == clip) {
                if (recOut) *recOut = r; else W3dRecursoSoltar(r, W3DREC_PERMANENTE);
                return set->datos.jerarquias[i];
            }
        W3dRecursoSoltar(r, W3DREC_PERMANENTE);
        return 0;
    }
    // suelto: el UNICO clip con ese nombre entre las bibliotecas cargadas
    W3dClipJer* hallado = 0;
    W3dRecurso* de = 0;
    int n = 0;
    std::vector<W3dRecurso*> todos;
    W3dRecursosListar(W3DREC_ANIMSET, todos);
    for (size_t i = 0; i < todos.size(); i++) {
        if (!todos[i] || todos[i]->estado != W3DREC_LISTO || !todos[i]->dato) continue;
        const W3dAnimSet* set = (const W3dAnimSet*)todos[i]->dato;
        for (size_t k = 0; k < set->datos.jerarquias.size(); k++)
            if (set->datos.jerarquias[k] && set->datos.jerarquias[k]->nombre == nombre) { hallado = set->datos.jerarquias[k]; de = todos[i]; n++; }
    }
    if (n != 1) return 0;   // (ninguno, o ambiguo: hay que calificarlo "Biblioteca/clip")
    W3dRecursoRetener(de, W3DREC_PERMANENTE);
    if (recOut) *recOut = de; else W3dRecursoSoltar(de, W3DREC_PERMANENTE);
    return hallado;
}

// ---- LAS RUTAS ----
// el nombre BASE (sin el ".NNN" canonico de los nombres unicos)
static std::string NombreBase(const std::string& n) {
    std::string raiz; int num = 0;
    W3dNombreSepararSufijo(n, raiz, num);
    return raiz;
}
// el tramo de 'x' dentro de su padre 'p': su nombre base; si otro hermano tiene la misma base,
// base + "#k" (k = su orden entre los de esa base)
static std::string Tramo(const Object* p, const Object* x) {
    const std::string base = NombreBase(x->name);
    int k = 0, n = 0;
    for (size_t i = 0; i < p->Childrens.size(); i++) {
        const Object* h = p->Childrens[i];
        if (!h || NombreBase(h->name) != base) continue;
        if (h == x) k = n;
        n++;
    }
    if (n <= 1) return base;
    char b[24]; sprintf(b, "#%d", k);
    return base + b;
}
std::string W3dJerRuta(const Object* raiz, const Object* n) {
    if (!raiz || !n) return std::string();
    if (n == raiz) return ".";
    std::vector<std::string> tramos;
    const Object* x = n;
    while (x && x != raiz) {
        const Object* p = x->Parent;
        if (!p) return std::string();   // no cuelga de 'raiz'
        tramos.push_back(Tramo(p, x));
        x = p;
    }
    if (x != raiz) return std::string();
    std::string r;
    for (size_t i = tramos.size(); i-- > 0; ) { if (!r.empty()) r += '/'; r += tramos[i]; }
    return r;
}
Object* W3dJerNodo(Object* raiz, const std::string& ruta) {
    if (!raiz) return 0;
    if (ruta.empty() || ruta == ".") return raiz;
    Object* x = raiz;
    size_t a = 0;
    while (x && a <= ruta.size()) {
        size_t b = ruta.find('/', a);
        if (b == std::string::npos) b = ruta.size();
        std::string t = ruta.substr(a, b - a);
        a = b + 1;
        if (t.empty() || t == ".") { if (b >= ruta.size()) break; continue; }
        // "base#k": el k-esimo hijo con esa base
        int k = -1;
        const size_t h = t.rfind('#');
        if (h != std::string::npos && h > 0 && h + 1 < t.size()) {
            bool dig = true;
            for (size_t i = h + 1; i < t.size(); i++) if (t[i] < '0' || t[i] > '9') dig = false;
            if (dig) { k = atoi(t.c_str() + h + 1); t = t.substr(0, h); }
        }
        Object* sig = 0;
        if (k < 0)   // el nombre exacto primero
            for (size_t i = 0; i < x->Childrens.size() && !sig; i++)
                if (x->Childrens[i] && x->Childrens[i]->name == t) sig = x->Childrens[i];
        if (!sig) {  // despues por la base (una copia renombrada ".001")
            const std::string base = NombreBase(t);
            int n = 0;
            for (size_t i = 0; i < x->Childrens.size() && !sig; i++) {
                Object* c = x->Childrens[i];
                if (!c || NombreBase(c->name) != base) continue;
                if (k < 0 || n == k) sig = c;
                n++;
            }
        }
        x = sig;
        if (b >= ruta.size()) break;
    }
    return x;
}

// ---- EL TAMANO de una jerarquia (retarget) ----
// Se mide SIN rotaciones (cada nodo = su traslacion y su escala sobre las de su padre): el tamano de una
// jerarquia no cambia porque un clip le haya girado la puerta o la grua (con retarget "rotaciones" lo unico
// que el clip le cambia a los descendientes es eso), asi que da lo mismo medirla en reposo o a mitad de un
// clip. 'org'/'esc': donde queda el origen del nodo y su escala acumulada, en el espacio del padre de la raiz.
static void TamanoAcumular(Object* o, const Vector3& orgPadre, const Vector3& escPadre, Vector3& mn, Vector3& mx, bool& hay) {
    const Vector3 org(orgPadre.x + escPadre.x * o->pos.x, orgPadre.y + escPadre.y * o->pos.y, orgPadre.z + escPadre.z * o->pos.z);
    const Vector3 esc(escPadre.x * o->scale.x, escPadre.y * o->scale.y, escPadre.z * o->scale.z);
    std::vector<Vector3> pts;
    pts.push_back(Vector3(0, 0, 0));
    if (o->getType() == ObjectType::mesh) {
        const Mesh* me = (const Mesh*)o;
        // su caja LOCAL: la calculada (aabbOk) o, si no la tiene (una copia recien duplicada, una malla que
        // nunca paso por CalcularBordes), la de sus posiciones de render
        Vector3 a = me->aabbMin, b = me->aabbMax;
        bool hayCaja = me->aabbOk;
        if (!hayCaja && me->vertex && me->vertexSize > 0) {
            a = b = Vector3(me->vertex[0], me->vertex[1], me->vertex[2]);
            for (int i = 1; i < me->vertexSize; i++) {
                const GLfloat* v = me->vertex + i * 3;
                if (v[0] < a.x) a.x = v[0];
                if (v[1] < a.y) a.y = v[1];
                if (v[2] < a.z) a.z = v[2];
                if (v[0] > b.x) b.x = v[0];
                if (v[1] > b.y) b.y = v[1];
                if (v[2] > b.z) b.z = v[2];
            }
            hayCaja = true;
        }
        if (hayCaja) { pts.push_back(a); pts.push_back(b); }
    }
    for (size_t i = 0; i < pts.size(); i++) {
        const Vector3 p(org.x + esc.x * pts[i].x, org.y + esc.y * pts[i].y, org.z + esc.z * pts[i].z);
        if (!hay) { mn = p; mx = p; hay = true; continue; }
        if (p.x < mn.x) mn.x = p.x;
        if (p.y < mn.y) mn.y = p.y;
        if (p.z < mn.z) mn.z = p.z;
        if (p.x > mx.x) mx.x = p.x;
        if (p.y > mx.y) mx.y = p.y;
        if (p.z > mx.z) mx.z = p.z;
    }
    for (size_t i = 0; i < o->Childrens.size(); i++)
        if (o->Childrens[i]) TamanoAcumular(o->Childrens[i], org, esc, mn, mx, hay);
}
float W3dJerTamano(Object* raiz) {
    if (!raiz) return 0.0f;
    Vector3 mn(0, 0, 0), mx(0, 0, 0);
    bool hay = false;
    TamanoAcumular(raiz, Vector3(0, 0, 0), Vector3(1, 1, 1), mn, mx, hay);
    const Vector3 d = mx - mn;
    const float t = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    return t > 1e-6f ? t : 0.0f;
}
