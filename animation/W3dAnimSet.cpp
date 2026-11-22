#include "W3dAnimSet.h"
#include "SkeletalAnimation.h"
#include "objects/Armature.h"
#include "objects/Mesh.h"        // el tamano de una jerarquia cuenta las cajas de sus mallas
#include "math/Matrix4.h"
#include "W3dNames.h"            // W3dNameNormalize: los nombres que la reparacion del editor tocaria
#include "io/W3dResources.h"    // el animset es un recurso del almacen (tipo ANIMSET)
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
static const char* kBlocksA[] = { "REQ ", "HUES", "CLIP", "JCLP", 0 };
// lo MINIMO que un lector tiene que entender para no romper el animset: van en REQ
static const char* kRequiredA[] = { "HUES", "CLIP", 0 };

static bool BlockKnown(const std::string& f) {
    for (int i = 0; kBlocksA[i]; i++) if (f == kBlocksA[i]) return true;
    return false;
}

void W3dAnimSetBlocksKnown(std::vector<std::string>& known, std::vector<std::string>* required) {
    known.clear();
    for (int i = 0; kBlocksA[i]; i++) known.push_back(kBlocksA[i]);
    if (required) {
        required->clear();
        for (int i = 0; kRequiredA[i]; i++) required->push_back(kRequiredA[i]);
    }
}

bool W3dAnimSetBinIs(const unsigned char* d, size_t n) {
    return d && n >= 4 && d[0] == 'W' && d[1] == '3' && d[2] == 'D' && d[3] == 'A';
}

// modos de los keyframes compactos (un byte cada uno, ver SetCurve)
enum { KF_FRAMES_ALL = 0, KF_FRAMES_REGULARS = 1 };   // i32 por key | primero + paso
enum { KF_INT_ALL = 0, KF_INT_ONE = 1 };              // i32 por key | uno para todos
enum { KF_HANDLES_ZERO = 0, KF_HANDLES_ALL = 1 };     // nada (todos 0) | f32 x4 por key

namespace {

// ===========================================================================
//  LITTLE-ENDIAN EXPLICITO, byte a byte: los mismos bytes en cualquier host y
//  ninguna lectura tipada desalineada (el ARM11 del N95 da SIGBUS con un float
//  desalineado; aca todo pasa por bytes).
// ===========================================================================
void SetU32(std::string& s, unsigned v) {
    char b[4];
    b[0] = (char)(v & 255u); b[1] = (char)((v >> 8) & 255u);
    b[2] = (char)((v >> 16) & 255u); b[3] = (char)((v >> 24) & 255u);
    s.append(b, 4);
}
void SetI32(std::string& s, int v) { SetU32(s, (unsigned)v); }
void SetF32(std::string& s, float f) { unsigned u; memcpy(&u, &f, 4); SetU32(s, u); }
void SetStr(std::string& s, const std::string& t) { SetU32(s, (unsigned)t.size()); s.append(t); }
unsigned LeeU32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
unsigned BitsF32(float f) { unsigned u; memcpy(&u, &f, 4); return u; }

// ---- BLOQUES: abrir escribe la cabecera con bytes=0; cerrar parchea el largo y
//      rellena con ceros hasta multiplo de 4 (sin basura: bytes deterministas) ----
size_t OpenBlock(std::string& s, const char* id, unsigned domain, unsigned count) {
    s.append(id, 4);
    SetU32(s, domain);
    SetU32(s, count);
    const size_t pos = s.size();
    SetU32(s, 0);
    return pos;
}
void CloseBlock(std::string& s, size_t posBytes) {
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
    const unsigned char* end;
    bool ok;
    Cursor(const unsigned char* a, size_t n) : p(a), end(a + n), ok(true) {}
    size_t Remain() const { return (size_t)(end - p); }
    bool Has(size_t n) { if (!ok || Remain() < n) ok = false; return ok; }
    unsigned U32() { if (!Has(4)) return 0; unsigned v = LeeU32(p); p += 4; return v; }
    int I32() { return (int)U32(); }
    float F32() { unsigned u = U32(); float f; memcpy(&f, &u, 4); return f; }
    unsigned U8() { if (!Has(1)) return 0; return *p++; }
    std::string Str() {
        unsigned n = U32();
        if (!Has(n)) return std::string();
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
// Una curva COMPACTA en memoria (W3dKeys, Animation.h) se escribe SIN expandirla y con los mismos bytes que
// daria expandida: sus claves son justo las de los modos regulares + uno para todos + handles en cero (n >= 2,
// paso > 0 y el ultimo frame entra en un int, asi que las diferencias que mira el camino de abajo son el paso).
// Una IRREGULAR va con sus frames, salvo que resulten regulares (como las mira el camino de abajo: el lector nunca
// las deja asi, pero los bytes no dependen de eso). Lo prueba prueba_claves_compactas.w3s (guardar sin editar da
// el archivo identico).
// el modificador REPETIR va en los bits 2-5 del byte de modo de los frames (antes: 2-3, despues: 4-5). Sin
// modificador son 0: una curva sin el da los MISMOS bytes que antes.
static unsigned CycleBits(const AnimProperty& ap) {
    return ((unsigned)(ap.cyclePrev & 3) << 2) | ((unsigned)(ap.cycleAfter & 3) << 4) | (ap.mods.empty() ? 0u : 0x40u);
}
// la PILA DE MODIFICADORES (Animation.h, KfModifier): el bit 6 del modo de los frames dice que viene, justo despues
// de los 4 bytes de modos: u32 cantidad + por cada uno u32 cuantos numeros + esos f32 (el orden de AFloats). Sin
// modificadores el bit es 0 y no se escribe nada: los mismos bytes de siempre.
void SetModsCurve(std::string& s, const AnimProperty& ap) {
    if (ap.mods.empty()) return;
    SetU32(s, (unsigned)ap.mods.size());
    for (size_t i = 0; i < ap.mods.size(); i++) {
        float f[KfModifier::NFloats]; ap.mods[i].AFloats(f);
        SetU32(s, (unsigned)KfModifier::NFloats);
        for (int q = 0; q < KfModifier::NFloats; q++) SetF32(s, f[q]);
    }
}
void SetCompactCurve(std::string& s, const AnimProperty& ap, const W3dCompactKeys& c) {
    SetI32(s, ap.Property);
    SetI32(s, ap.component);
    SetU32(s, c.n);
    bool regulars = !c.Irregular();
    if (!regulars) {
        const double d0 = (double)c.Frame(1) - (double)c.Frame(0);
        regulars = d0 <= 2147483647.0;
        for (unsigned i = 2; regulars && i < c.n; i++)
            if ((double)c.Frame(i) - (double)c.Frame(i - 1) != d0) regulars = false;
    }
    char modes[4];
    modes[0] = (char)((regulars ? KF_FRAMES_REGULARS : KF_FRAMES_ALL) | CycleBits(ap));
    modes[1] = (char)KF_INT_ONE;
    modes[2] = (char)KF_INT_ONE;
    modes[3] = (char)KF_HANDLES_ZERO;
    s.append(modes, 4);
    SetModsCurve(s, ap);
    if (!c.Irregular()) { SetI32(s, c.first); SetI32(s, c.step); }
    else if (regulars) { SetI32(s, c.Frame(0)); SetI32(s, c.Frame(1) - c.Frame(0)); }
    else for (unsigned i = 0; i < c.n; i++) SetI32(s, c.Frame(i));
    const float* v = c.Values();
    for (unsigned i = 0; i < c.n; i++) SetF32(s, v[i]);
    SetI32(s, c.interp);
    SetI32(s, c.typeH);
}

void SetCurve(std::string& s, const AnimProperty& ap) {
    if (const W3dCompactKeys* cc = ap.keyframes.CompactData()) { SetCompactCurve(s, ap, *cc); return; }
    const std::vector<keyFrame>& k = ap.keyframes;
    const size_t n = k.size();
    SetI32(s, ap.Property);
    SetI32(s, ap.component);
    SetU32(s, (unsigned)n);
    // frames regulares: cada diferencia igual a la primera (en double: sin overflow de int)
    bool regulars = n >= 2;
    int step = 0;
    if (regulars) {
        const double d0 = (double)k[1].frame - (double)k[0].frame;
        if (d0 > 2147483647.0 || d0 < -2147483648.0) regulars = false;
        else step = (int)d0;
        for (size_t i = 2; regulars && i < n; i++)
            if ((double)k[i].frame - (double)k[i - 1].frame != d0) regulars = false;
    }
    bool interpOne = n >= 1, oneType = n >= 1, handles = false;
    for (size_t i = 1; i < n; i++) {
        if (k[i].Interpolation != k[0].Interpolation) interpOne = false;
        if (k[i].handleType != k[0].handleType) oneType = false;
    }
    // handles: se comparan los BITS (un -0.0 no es "cero": tiene que volver igual)
    for (size_t i = 0; i < n && !handles; i++)
        if (BitsF32(k[i].inDF) || BitsF32(k[i].inDV) || BitsF32(k[i].outDF) || BitsF32(k[i].outDV)) handles = true;
    char modes[4];
    modes[0] = (char)((regulars ? KF_FRAMES_REGULARS : KF_FRAMES_ALL) | CycleBits(ap));
    modes[1] = (char)(interpOne ? KF_INT_ONE : KF_INT_ALL);
    modes[2] = (char)(oneType ? KF_INT_ONE : KF_INT_ALL);
    modes[3] = (char)(handles ? KF_HANDLES_ALL : KF_HANDLES_ZERO);
    s.append(modes, 4);
    SetModsCurve(s, ap);
    if (n == 0) return;
    if (regulars) { SetI32(s, k[0].frame); SetI32(s, step); }
    else for (size_t i = 0; i < n; i++) SetI32(s, k[i].frame);
    for (size_t i = 0; i < n; i++) SetF32(s, k[i].value);
    if (interpOne) SetI32(s, k[0].Interpolation);
    else for (size_t i = 0; i < n; i++) SetI32(s, k[i].Interpolation);
    if (oneType) SetI32(s, k[0].handleType);
    else for (size_t i = 0; i < n; i++) SetI32(s, k[i].handleType);
    if (handles)
        for (size_t i = 0; i < n; i++) {
            SetF32(s, k[i].inDF); SetF32(s, k[i].inDV); SetF32(s, k[i].outDF); SetF32(s, k[i].outDV);
        }
}

bool ReadCurve(Cursor& c, AnimProperty& ap, std::string& reason) {
    ap.Property = c.I32();
    ap.component = c.I32();
    const unsigned n = c.U32();
    unsigned mF = c.U8();
    const unsigned mI = c.U8(), mT = c.U8(), mH = c.U8();
    if (!c.ok) { reason = "curva cortada"; return false; }
    // el modificador REPETIR (bits 2-5 del modo de los frames, ver CycleBits)
    const bool hasMods = ((mF >> 6) & 1) != 0;
    { const unsigned ca = (mF >> 2) & 3, cd = (mF >> 4) & 3;
      if (ca > KfCycleOffset || cd > KfCycleOffset || (mF >> 7)) { reason = "curva con un modo desconocido"; return false; }
      ap.cyclePrev = (int)ca; ap.cycleAfter = (int)cd; mF &= 3; }
    ap.mods.clear();
    if (hasMods) {   // la pila de modificadores (ver SetModsCurve)
        const unsigned nm = c.U32();
        if (!c.ok || nm > 64) { reason = "curva con demasiados modificadores"; c.ok = false; return false; }
        for (unsigned i = 0; i < nm; i++) {
            const unsigned nf = c.U32();
            if (!c.ok || nf > 256 || !c.Has((size_t)nf * 4)) { reason = "modificador de curva cortado"; c.ok = false; return false; }
            float f[KfModifier::NFloats];
            for (int q = 0; q < KfModifier::NFloats; q++) f[q] = 0.0f;
            for (unsigned q = 0; q < nf; q++) { const float v = c.F32(); if (q < (unsigned)KfModifier::NFloats) f[q] = v; }
            KfModifier m; m.FromFloats(f, nf < (unsigned)KfModifier::NFloats ? (int)nf : (int)KfModifier::NFloats);
            ap.mods.push_back(m);
        }
    }
    ap.RepeatToModifier();   // (un animset de antes con el Repetir viejo: queda como modificador Ciclos)
    if (mF > 1 || mI > 1 || mT > 1 || mH > 1) { reason = "curva con un modo desconocido"; return false; }
    if (n == 0) return true;
    // cota ANTES de reservar: cada keyframe ocupa por lo menos 4 bytes (su valor)
    if ((size_t)n > c.Remain() / 4) { reason = "curva con mas keyframes de los que entran en el bloque"; c.ok = false; return false; }
    // bytes exactos que ocupa lo que sigue (en size_t, sin overflow: n <= quedan/4)
    size_t nec = (size_t)n * 4;                                   // valores
    nec += (mF == KF_FRAMES_REGULARS) ? 8 : (size_t)n * 4;
    nec += (mI == KF_INT_ONE) ? 4 : (size_t)n * 4;
    nec += (mT == KF_INT_ONE) ? 4 : (size_t)n * 4;
    if (mH == KF_HANDLES_ALL) nec += (size_t)n * 16;
    if (!c.Has(nec)) { reason = "curva cortada"; return false; }
    // HORNEADA (frames regulares, una interpolacion y un tipo de handle para todas, handles en cero): queda
    // COMPACTA en memoria, solo los valores (W3dKeys, Animation.h). La interpolacion y el tipo de handle vienen
    // DESPUES de los valores: se miran antes (ya se sabe que estan: c.Has(nec)) para leer los valores directo.
    if (mF == KF_FRAMES_REGULARS && mI == KF_INT_ONE && mT == KF_INT_ONE && mH == KF_HANDLES_ZERO) {
        const int f0 = (int)LeeU32(c.p), step = (int)LeeU32(c.p + 4);
        const int interp = (int)LeeU32(c.p + 8 + (size_t)n * 4), typeH = (int)LeeU32(c.p + 12 + (size_t)n * 4);
        if (W3dKeys::CompactPossible(n, f0, step)) {
            c.I32(); c.I32();
            float* v = ap.keyframes.CompactCreate(n, f0, step, interp, typeH);
            for (unsigned i = 0; i < n; i++) v[i] = c.F32();
            c.I32(); c.I32();
            return c.ok;
        }
    }
    // REDUCIDA (frames irregulares crecientes, lo demas igual): compacta IRREGULAR, su frame y su valor por clave
    if (mF == KF_FRAMES_ALL && mI == KF_INT_ONE && mT == KF_INT_ONE && mH == KF_HANDLES_ZERO && n >= 2) {
        std::vector<int> fr(n);
        for (unsigned i = 0; i < n; i++) fr[i] = (int)LeeU32(c.p + (size_t)i * 4);
        if (W3dKeys::CompactIrregularPossible(n, &fr[0])) {
            const int interp = (int)LeeU32(c.p + (size_t)n * 8), typeH = (int)LeeU32(c.p + 4 + (size_t)n * 8);
            for (unsigned i = 0; i < n; i++) c.I32();
            float* v = ap.keyframes.CompactCreateIrregular(n, &fr[0], interp, typeH);
            for (unsigned i = 0; i < n; i++) v[i] = c.F32();
            c.I32(); c.I32();
            return c.ok;
        }
    }
    std::vector<keyFrame> k(n);
    if (mF == KF_FRAMES_REGULARS) {
        int f = c.I32(); const int step = c.I32();
        for (unsigned i = 0; i < n; i++) { k[i].frame = f; if (i + 1 < n) f += step; }
    } else for (unsigned i = 0; i < n; i++) k[i].frame = c.I32();
    for (unsigned i = 0; i < n; i++) k[i].value = c.F32();
    if (mI == KF_INT_ONE) { const int v = c.I32(); for (unsigned i = 0; i < n; i++) k[i].Interpolation = v; }
    else for (unsigned i = 0; i < n; i++) k[i].Interpolation = c.I32();
    if (mT == KF_INT_ONE) { const int v = c.I32(); for (unsigned i = 0; i < n; i++) k[i].handleType = v; }
    else for (unsigned i = 0; i < n; i++) k[i].handleType = c.I32();
    if (mH == KF_HANDLES_ALL)
        for (unsigned i = 0; i < n; i++) {
            k[i].inDF = c.F32(); k[i].inDV = c.F32(); k[i].outDF = c.F32(); k[i].outDV = c.F32();
        }
    else for (unsigned i = 0; i < n; i++) { k[i].inDF = 0.0f; k[i].inDV = 0.0f; k[i].outDF = 0.0f; k[i].outDV = 0.0f; }
    ap.keyframes.swap(k);
    return c.ok;
}

// un bloque ya ubicado en el archivo
struct BlockA {
    std::string id;
    unsigned domain, count, bytes;
    const unsigned char* data;
};

// recorre la cabecera y los bloques; false + motivo si la estructura no cierra
bool Locate(const unsigned char* d, size_t n, std::vector<BlockA>& out, std::string& reason) {
    out.clear();
    if (!W3dAnimSetBinIs(d, n)) { reason = "no es un .w3da (falta la firma W3DA)"; return false; }
    if (n < 20) { reason = "cabecera cortada"; return false; }
    const unsigned version = LeeU32(d + 4);
    if (version > (unsigned)W3DA_VERSION) {
        char b[96]; sprintf(b, "es de una version mas nueva del formato (%u; esta lee hasta %d)", version, (int)W3DA_VERSION);
        reason = b; return false;
    }
    if (version == 0) { reason = "version 0 invalida"; return false; }
    const unsigned nBlocks = LeeU32(d + 12);
    size_t pos = 20;
    for (unsigned i = 0; i < nBlocks; i++) {
        if (n - pos < 16) { reason = "bloque cortado (cabecera)"; return false; }
        BlockA b;
        b.id.assign((const char*)d + pos, 4);
        b.domain = LeeU32(d + pos + 4);
        b.count = LeeU32(d + pos + 8);
        b.bytes = LeeU32(d + pos + 12);
        pos += 16;
        if ((size_t)b.bytes > n - pos) { reason = "bloque '" + b.id + "' cortado"; return false; }
        b.data = d + pos;
        const size_t withFill = ((size_t)b.bytes + 3) & ~(size_t)3;
        pos += (withFill <= n - pos) ? withFill : (size_t)b.bytes;
        out.push_back(b);
    }
    if (pos != n) { reason = "sobran bytes despues del ultimo bloque"; return false; }
    return true;
}

bool ReadClip(const BlockA& b, int nBones, SkeletalAnimation*& out, std::string& reason) {
    out = 0;
    Cursor c(b.data, b.bytes);
    const std::string name = c.Str();
    const int fps = c.I32(), start = c.I32(), end = c.I32();
    if (!c.ok) { reason = "clip cortado"; return false; }
    // cada pista ocupa por lo menos 8 bytes (hueso + nCurves)
    if ((size_t)b.count > c.Remain() / 8) { reason = "clip con mas pistas de las que entran en el bloque"; return false; }
    SkeletalAnimation* clip = new SkeletalAnimation(name);
    clip->FrameRate = fps; clip->startFrame = start; clip->endFrame = end;
    clip->tracks.resize(b.count);
    for (unsigned t = 0; t < b.count; t++) {
        BoneTrack& tr = clip->tracks[t];
        tr.bone = c.I32();
        const unsigned nCurves = c.U32();
        if (!c.ok) { reason = "pista cortada"; delete clip; return false; }
        if (tr.bone < -1 || tr.bone >= nBones) { reason = "pista con un hueso fuera de la tabla"; delete clip; return false; }
        // cada curva ocupa por lo menos 16 bytes (propiedad, componente, nKeys, modos)
        if ((size_t)nCurves > c.Remain() / 16) { reason = "pista con mas curvas de las que entran en el bloque"; delete clip; return false; }
        tr.Properties.resize(nCurves);
        for (unsigned k = 0; k < nCurves; k++)
            if (!ReadCurve(c, tr.Properties[k], reason)) { delete clip; return false; }
    }
    // el tamano del esqueleto que lo grabo (opcional: un clip viejo termina en su ultima pista)
    if (c.ok && c.Remain() == 4) clip->heightRest = c.F32();
    if (!c.ok || c.Remain() != 0) { reason = "el clip '" + name + "' no cierra con el largo de su bloque"; delete clip; return false; }
    out = clip;
    return true;
}

// un bloque JCLP (clip de jerarquia)
bool ReadClipHier(const BlockA& b, W3dClipHier*& out, std::string& reason) {
    out = 0;
    Cursor c(b.data, b.bytes);
    const std::string name = c.Str();
    const int fps = c.I32(), start = c.I32(), end = c.I32();
    const float size = c.F32();
    const unsigned ret = c.U32();
    if (!c.ok) { reason = "clip de jerarquia cortado"; return false; }
    // cada pista ocupa por lo menos 8 bytes (largo de la ruta + nCurves)
    if ((size_t)b.count > c.Remain() / 8) { reason = "clip de jerarquia con mas pistas de las que entran en el bloque"; return false; }
    W3dClipHier* clip = new W3dClipHier();
    clip->name = name; clip->fps = fps; clip->start = start; clip->end = end;
    clip->size = size; clip->retarget = (ret == 1u) ? 1 : 0;
    clip->tracks.resize(b.count);
    for (unsigned t = 0; t < b.count; t++) {
        W3dHierTrack& p = clip->tracks[t];
        p.path = c.Str();
        const unsigned nCurves = c.U32();
        if (!c.ok) { reason = "pista de jerarquia cortada"; delete clip; return false; }
        if ((size_t)nCurves > c.Remain() / 16) { reason = "pista con mas curvas de las que entran en el bloque"; delete clip; return false; }
        p.props.resize(nCurves);
        for (unsigned k = 0; k < nCurves; k++)
            if (!ReadCurve(c, p.props[k], reason)) { delete clip; return false; }
    }
    if (!c.ok || c.Remain() != 0) { reason = "el clip de jerarquia '" + name + "' no cierra con el largo de su bloque"; delete clip; return false; }
    out = clip;
    return true;
}

} // namespace

// ============================================================================
//  ESCRITURA
// ============================================================================
void W3dAnimSetClipBlock(const SkeletalAnimation& c, const std::vector<int>& map, int nBones, std::string& out) {
    out.clear();
    const size_t pb = OpenBlock(out, "CLIP", 0u, (unsigned)c.tracks.size());
    SetStr(out, c.name);
    SetI32(out, c.FrameRate);
    SetI32(out, c.startFrame);
    SetI32(out, c.endFrame);
    for (size_t t = 0; t < c.tracks.size(); t++) {
        const BoneTrack& tr = c.tracks[t];
        int h = -1;
        if (tr.bone >= 0 && tr.bone < (int)map.size()) h = map[tr.bone];
        if (h < 0 || h >= nBones) h = -1;
        SetI32(out, h);
        SetU32(out, (unsigned)tr.Properties.size());
        for (size_t k = 0; k < tr.Properties.size(); k++) SetCurve(out, tr.Properties[k]);
    }
    // el tamano del esqueleto que lo grabo (retarget), SOLO si se sabe: un clip viejo sale igual que antes
    if (BitsF32(c.heightRest) != 0u) SetF32(out, c.heightRest);
    CloseBlock(out, pb);
}

void W3dAnimSetClipHierBlock(const W3dClipHier& c, std::string& out) {
    out.clear();
    const size_t pb = OpenBlock(out, "JCLP", 0u, (unsigned)c.tracks.size());
    SetStr(out, c.name);
    SetI32(out, c.fps);
    SetI32(out, c.start);
    SetI32(out, c.end);
    SetF32(out, c.size);
    SetU32(out, (unsigned)c.retarget);
    for (size_t t = 0; t < c.tracks.size(); t++) {
        const W3dHierTrack& p = c.tracks[t];
        SetStr(out, p.path);
        SetU32(out, (unsigned)p.props.size());
        for (size_t k = 0; k < p.props.size(); k++) SetCurve(out, p.props[k]);
    }
    CloseBlock(out, pb);
}

void W3dAnimSetBuild(const std::vector<std::string>& bones, const std::vector<const std::string*>& clips,
                     const std::vector<W3dAnimSetForeignBlock>* foreign, std::string& out,
                     const std::vector<const std::string*>* hier) {
    out.clear();
    const unsigned nSettings = foreign ? (unsigned)foreign->size() : 0u;
    const unsigned nHier = hier ? (unsigned)hier->size() : 0u;
    out.append("W3DA", 4);
    SetU32(out, (unsigned)W3DA_VERSION);
    SetU32(out, 0u);                                  // banderas
    SetU32(out, 2u + (unsigned)clips.size() + nHier + nSettings);   // REQ + HUES + clips + jerarquias + ajenos
    SetU32(out, 0u);                                  // reservado
    {
        // (JCLP solo si hay clips de jerarquia: un animset de esqueleto sale byte a byte como antes)
        unsigned nReq = 0; for (int i = 0; kRequiredA[i]; i++) nReq++;
        const size_t pb = OpenBlock(out, "REQ ", 0u, nReq + (nHier ? 1u : 0u));
        for (int i = 0; kRequiredA[i]; i++) out.append(kRequiredA[i], 4);
        if (nHier) out.append("JCLP", 4);
        CloseBlock(out, pb);
    }
    {
        const size_t pb = OpenBlock(out, "HUES", 0u, (unsigned)bones.size());
        for (size_t i = 0; i < bones.size(); i++) SetStr(out, bones[i]);
        CloseBlock(out, pb);
    }
    for (size_t i = 0; i < clips.size(); i++) if (clips[i]) out += *clips[i];
    for (unsigned i = 0; i < nHier; i++) if ((*hier)[i]) out += *(*hier)[i];
    for (unsigned i = 0; i < nSettings; i++) {
        const W3dAnimSetForeignBlock& a = (*foreign)[i];
        std::string id = a.id; id.resize(4, ' ');
        const size_t pb = OpenBlock(out, id.c_str(), a.domain, a.count);
        out += a.data;
        CloseBlock(out, pb);
    }
}

// ============================================================================
//  LECTURA
// ============================================================================
W3dAnimSetData::~W3dAnimSetData() {
    for (size_t i = 0; i < clips.size(); i++) delete clips[i];
    for (size_t i = 0; i < hierarchies.size(); i++) delete hierarchies[i];
}

// ---- el clip de jerarquia ----
static unsigned gSeriesHier = 0;
W3dClipHier::W3dClipHier() : name("Clip"), fps(30), start(1), end(250), size(0.0f), retarget(0),
                           version(1), series(++gSeriesHier) {}
float W3dClipHier::Duration() const {
    const int f = fps > 0 ? fps : 30;
    const int n = end - start + 1;
    return (n > 0 ? (float)n : 1.0f) / (float)f;
}

bool W3dAnimSetRead(const unsigned char* d, size_t n, W3dAnimSetData& out, std::string* reason) {
    std::string m;
    for (size_t i = 0; i < out.clips.size(); i++) delete out.clips[i];
    for (size_t i = 0; i < out.hierarchies.size(); i++) delete out.hierarchies[i];
    out.clips.clear(); out.bones.clear(); out.foreign.clear(); out.hierarchies.clear();
    std::vector<BlockA> bl;
    if (!Locate(d, n, bl, m)) { if (reason) *reason = m; return false; }
    // REQ: todo lo que pide tiene que ser conocido
    std::set<std::string> req;
    int nReq = 0, nHues = 0;
    for (size_t i = 0; i < bl.size(); i++) {
        if (bl[i].id == "REQ ") {
            nReq++;
            if ((size_t)bl[i].count > bl[i].bytes / 4) { if (reason) *reason = "REQ cortado"; return false; }
            for (unsigned k = 0; k < bl[i].count; k++) req.insert(std::string((const char*)bl[i].data + k * 4, 4));
        } else if (bl[i].id == "HUES") nHues++;
    }
    if (nReq > 1 || nHues > 1) { if (reason) *reason = "bloque REQ o HUES repetido"; return false; }
    for (std::set<std::string>::iterator it = req.begin(); it != req.end(); ++it)
        if (!BlockKnown(*it)) {
            if (reason) *reason = "pide el bloque '" + *it + "' de una version mas nueva de Whisk3D";
            return false;
        }
    // HUES primero: las pistas de los clips se validan contra la tabla
    for (size_t i = 0; i < bl.size(); i++) {
        if (bl[i].id != "HUES") continue;
        Cursor c(bl[i].data, bl[i].bytes);
        if ((size_t)bl[i].count > c.Remain() / 4) { if (reason) *reason = "HUES con mas huesos de los que entran"; return false; }
        out.bones.resize(bl[i].count);
        for (unsigned k = 0; k < bl[i].count; k++) out.bones[k] = c.Str();
        if (!c.ok || c.Remain() != 0) { out.bones.clear(); if (reason) *reason = "HUES no cierra con el largo de su bloque"; return false; }
    }
    for (size_t i = 0; i < bl.size(); i++) {
        const BlockA& b = bl[i];
        if (b.id == "REQ " || b.id == "HUES") continue;
        if (b.id == "CLIP") {
            SkeletalAnimation* clip = 0;
            if (!ReadClip(b, (int)out.bones.size(), clip, m)) {
                for (size_t k = 0; k < out.clips.size(); k++) delete out.clips[k];
                out.clips.clear(); out.bones.clear(); out.foreign.clear();
                if (reason) *reason = m;
                return false;
            }
            out.clips.push_back(clip);
            continue;
        }
        if (b.id == "JCLP") {
            W3dClipHier* cj = 0;
            if (!ReadClipHier(b, cj, m)) {
                for (size_t k = 0; k < out.clips.size(); k++) delete out.clips[k];
                for (size_t k = 0; k < out.hierarchies.size(); k++) delete out.hierarchies[k];
                out.clips.clear(); out.bones.clear(); out.foreign.clear(); out.hierarchies.clear();
                if (reason) *reason = m;
                return false;
            }
            out.hierarchies.push_back(cj);
            continue;
        }
        // desconocido y no requerido: se PRESERVA tal cual
        W3dAnimSetForeignBlock a;
        a.id = b.id; a.domain = b.domain; a.count = b.count;
        a.data.assign((const char*)b.data, (size_t)b.bytes);
        out.foreign.push_back(a);
    }
    return true;
}

bool W3dAnimSetList(const unsigned char* d, size_t n, std::vector<std::string>& lines) {
    lines.clear();
    std::vector<BlockA> bl;
    std::string m;
    if (!Locate(d, n, bl, m)) return false;
    for (size_t i = 0; i < bl.size(); i++) {
        char b[160];
        if (bl[i].id == "CLIP") {
            Cursor c(bl[i].data, bl[i].bytes);
            std::string name = c.Str();
            if (name.size() > 60) name.resize(60);
            sprintf(b, "CLIP '%s' %u pistas %u B", name.c_str(), bl[i].count, bl[i].bytes);
        } else if (bl[i].id == "JCLP") {
            Cursor c(bl[i].data, bl[i].bytes);
            std::string name = c.Str();
            if (name.size() > 60) name.resize(60);
            sprintf(b, "JCLP '%s' %u pistas %u B", name.c_str(), bl[i].count, bl[i].bytes);
        } else sprintf(b, "%s %u %u B", bl[i].id.c_str(), bl[i].count, bl[i].bytes);
        lines.push_back(b);
    }
    return true;
}

// ============================================================================
//  EL RECURSO
// ============================================================================
W3dAnimSetVariant::~W3dAnimSetVariant() {
    for (size_t i = 0; i < clips.size(); i++) delete clips[i];
}
W3dAnimSet::~W3dAnimSet() {
    for (size_t i = 0; i < variants.size(); i++) delete variants[i];
}

// FNV-1a de 32 bits + un segundo hash (djb2) sobre los mismos bytes: juntos, con el
// largo, alcanzan para decir "esta entrada es la misma que se leyo" (ver GuardarAnimSets)
void W3dAnimSetFootprint(const unsigned char* d, size_t n, unsigned& h1, unsigned& h2) {
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
        b += (long)(tr.Properties.capacity() * sizeof(AnimProperty));
        for (size_t p = 0; p < tr.Properties.size(); p++) {
            b += tr.Properties[p].keyframes.BytesMemory();   // (vector o bloque compacto: medir no expande)
            k += (long)tr.Properties[p].keyframes.size();
        }
    }
    if (keys) *keys += k;
    return b;
}

// memoria aproximada de un clip de jerarquia (sus rutas, curvas y keyframes)
static long ClipHierBytes(const W3dClipHier* c, long* keys) {
    if (!c) return 0;
    long b = (long)sizeof(W3dClipHier) + (long)c->name.capacity() + (long)(c->tracks.capacity() * sizeof(W3dHierTrack));
    long k = 0;
    for (size_t t = 0; t < c->tracks.size(); t++) {
        const W3dHierTrack& p = c->tracks[t];
        b += (long)p.path.capacity() + (long)(p.props.capacity() * sizeof(AnimProperty));
        for (size_t q = 0; q < p.props.size(); q++) {
            b += p.props[q].keyframes.BytesMemory();
            k += (long)p.props[q].keyframes.size();
        }
    }
    if (keys) *keys += k;
    return b;
}
static long MemoryOf(const W3dAnimSet* s) {
    long b = (long)sizeof(W3dAnimSet);
    for (size_t i = 0; i < s->data.clips.size(); i++) b += W3dAnimSetClipBytes(s->data.clips[i], 0);
    for (size_t i = 0; i < s->data.hierarchies.size(); i++) b += ClipHierBytes(s->data.hierarchies[i], 0);
    return b;
}

// el ultimo motivo por el que la op Cargar no pudo (para el aviso del que la pidio)
static std::string gReasonLoad;

static void* OpLoadAnimSet(const std::string& id, long* bytes) {
    { const std::string milestone = "cargando el animset " + id; w3dLogMemory(milestone.c_str()); }
    std::vector<unsigned char> d;
    if (!w3dFileSystem::ReadFileBytes(id, d) || d.empty()) {
        gReasonLoad = "no encuentro la entrada '" + id + "'";
        w3dLogfE("[W3D] animset: %s", gReasonLoad.c_str());
        return 0;
    }
    W3dAnimSet* s = new W3dAnimSet();
    std::string reason;
    if (!W3dAnimSetRead(&d[0], d.size(), s->data, &reason)) {
        gReasonLoad = "'" + id + "': " + reason;
        w3dLogfE("[W3D] animset %s", gReasonLoad.c_str());
        delete s;
        return 0;
    }
    W3dAnimSetFootprint(&d[0], d.size(), s->footprint1, s->footprint2);
    s->fileSize = d.size();
    if (bytes) *bytes = MemoryOf(s);
    return s;
}
static bool OpFreeAnimSet(void* data) { delete (W3dAnimSet*)data; return true; }

void W3dAnimSetsEnsureOps() {
    static bool ready = false;
    if (ready) return;
    W3dResourceOps ops;
    ops.Load = OpLoadAnimSet;
    ops.Free = OpFreeAnimSet;
    W3dResourcesRegisterType(W3DREC_ANIMSET, ops);
    ready = true;
}

// ============================================================================
//  EL REGISTRO DEL PROYECTO
// ============================================================================
static std::vector<W3dAnimSetRow> gRegistry;

void W3dAnimSetsRegistryClear() { gRegistry.clear(); }
void W3dAnimSetsRegistryAdd(const std::string& name, const std::string& entry, const std::string& folder) {
    for (size_t i = 0; i < gRegistry.size(); i++)
        if (gRegistry[i].name == name) { gRegistry[i].entry = entry; gRegistry[i].folder = folder; return; }
    W3dAnimSetRow f; f.name = name; f.entry = entry; f.folder = folder;
    gRegistry.push_back(f);
}
const std::vector<W3dAnimSetRow>& W3dAnimSetsRegistry() { return gRegistry; }
void W3dAnimSetsRegistrySet(const std::vector<W3dAnimSetRow>& rows) { gRegistry = rows; }
// los de las LIBRERIAS externas (ver el .h): nombre con su prefijo -> entrada con su "lib:"
static std::vector<W3dAnimSetRow> gRegistryLib;
void W3dAnimSetsLibAdd(const std::string& name, const std::string& entry) {
    for (size_t i = 0; i < gRegistryLib.size(); i++)
        if (gRegistryLib[i].name == name) { gRegistryLib[i].entry = entry; return; }
    W3dAnimSetRow f; f.name = name; f.entry = entry;
    gRegistryLib.push_back(f);
}
void W3dAnimSetsLibClear(const std::string& lib) {
    if (lib.empty()) { gRegistryLib.clear(); return; }
    const std::string pre = "lib:" + lib + "/";
    for (size_t i = gRegistryLib.size(); i-- > 0; )
        if (gRegistryLib[i].entry.compare(0, pre.size(), pre) == 0) gRegistryLib.erase(gRegistryLib.begin() + (long)i);
}
bool W3dAnimSetIsFromLib(const std::string& name) {
    for (size_t i = 0; i < gRegistryLib.size(); i++) if (gRegistryLib[i].name == name) return true;
    return false;
}
std::string W3dAnimSetsEntryOf(const std::string& name) {
    for (size_t i = 0; i < gRegistry.size(); i++) if (gRegistry[i].name == name) return gRegistry[i].entry;
    for (size_t i = 0; i < gRegistryLib.size(); i++) if (gRegistryLib[i].name == name) return gRegistryLib[i].entry;
    return std::string();
}

std::string W3dAnimSetIdEntry(const std::string& id) {
    if (id.empty() || id[0] != '#') return id;
    const size_t p = id.find(':');
    return (p == std::string::npos) ? std::string() : id.substr(p + 1);
}

bool W3dAnimSetUnhook(W3dResource* r, const char* reason, int n) {
    if (!r) return false;
    // SIN REFERENCIAS y sin dato (un FALLO recordado, uno que nunca se cargo): se OLVIDA. Correrlo
    // de clave no alcanzaba: quedaba para siempre en el almacen (un descriptor zombie por cada
    // proyecto con un .w3da roto que se cerraba) y el cierre siguiente le volvia a agregar otro
    // prefijo (los ids crecian sin tope). Es el mismo criterio que las mallas (W3dResourceForget).
    if (r->refTotal <= 0 && r->state != W3DREC_READY) return W3dResourceForget(W3DREC_ANIMSET, r->id);
    // con usuarios: a una clave que no es una entrada, armada sobre la ENTRADA (sin los prefijos
    // de un desenganche anterior). Cuando el ultimo usuario lo suelte, el almacen lo libera.
    char b[64]; sprintf(b, "#%s%d:", reason ? reason : "", n);
    W3dResourceRename(W3DREC_ANIMSET, r->id, std::string(b) + W3dAnimSetIdEntry(r->id));
    return false;
}

void W3dAnimSetsCloseProject() {
    gRegistry.clear();
    gRegistryLib.clear();   // (las librerias se cierran con el proyecto)
    // los descriptores que NO estan listos (un FALLO recordado, uno que nunca se cargo) dejan
    // libre la clave de su entrada: si el proyecto siguiente tiene una entrada homonima, se
    // tiene que cargar de nuevo (sin esto el FALLO la bloqueaba para siempre). Los que nadie
    // referencia se BORRAN; los que todavia tienen usuarios (una ListaCarga en vuelo) se corren
    // a una clave que no es una entrada, UNA vez (el que ya esta corrido no ocupa ninguna).
    // Los LISTO no se tocan: si alguno quedo vivo es porque algo todavia lo usa.
    static int nForgotten = 0;
    std::vector<W3dResource*> all;
    W3dResourcesList(W3DREC_ANIMSET, all);
    for (size_t i = 0; i < all.size(); i++) {
        W3dResource* r = all[i];
        if (!r || r->state == W3DREC_READY) continue;
        if (r->refTotal > 0 && !r->id.empty() && r->id[0] == '#') continue;   // ya desenganchado
        W3dAnimSetUnhook(r, "forgotten", ++nForgotten);
    }
}

// ============================================================================
//  ARMATURES
// ============================================================================
void W3dArmatureBoneNames(const Armature* a, std::vector<std::string>& out) {
    out.clear();
    if (!a) return;
    out.reserve(a->bones.size());
    for (size_t i = 0; i < a->bones.size(); i++) out.push_back(a->bones[i].name);
}

// tabla -> armature: la entrada s de la tabla es el k-esimo hueso con ese nombre (k = cuantas
// veces aparecio antes en la tabla); en el armature se busca el k-esimo con el mismo nombre
static void MapTableToArmature(const std::vector<std::string>& bones, const Armature* a, std::vector<int>& map) {
    map.assign(bones.size(), -1);
    if (!a) return;
    std::map<std::string, std::vector<int> > byName;
    for (size_t i = 0; i < a->bones.size(); i++) byName[a->bones[i].name].push_back((int)i);
    std::map<std::string, int> seen;
    for (size_t s = 0; s < bones.size(); s++) {
        const int k = seen[bones[s]]++;
        std::map<std::string, std::vector<int> >::const_iterator it = byName.find(bones[s]);
        if (it != byName.end() && k < (int)it->second.size()) map[s] = it->second[k];
    }
}

void W3dAnimSetMapFromArmature(const std::vector<std::string>& bones, const Armature* a, std::vector<int>& out) {
    out.clear();
    if (!a) return;
    out.assign(a->bones.size(), -1);
    // camino comun: el mismo esqueleto (mismos nombres en el mismo orden) = identidad
    bool equal = a->bones.size() >= bones.size();
    for (size_t i = 0; equal && i < bones.size(); i++) if (a->bones[i].name != bones[i]) equal = false;
    if (equal) { for (size_t i = 0; i < bones.size(); i++) out[i] = (int)i; return; }
    std::vector<int> m;
    MapTableToArmature(bones, a, m);
    for (size_t s = 0; s < m.size(); s++) if (m[s] >= 0 && out[m[s]] < 0) out[m[s]] = (int)s;
}

// copia del clip canonico k con los huesos traducidos al armature (una variante)
static SkeletalAnimation* VariantClip(W3dAnimSet* set, W3dAnimSetVariant* v, size_t k) {
    if (k >= set->data.clips.size()) return 0;
    if (v->clips.size() != set->data.clips.size()) v->clips.resize(set->data.clips.size(), (SkeletalAnimation*)0);
    if (!v->clips[k]) {
        const SkeletalAnimation* src = set->data.clips[k];
        if (!src) return 0;
        SkeletalAnimation* c = new SkeletalAnimation(*src);
        for (size_t t = 0; t < c->tracks.size(); t++) {
            const int b = c->tracks[t].bone;
            c->tracks[t].bone = (b >= 0 && b < (int)v->map.size()) ? v->map[b] : -1;
        }
        v->clips[k] = c;
    }
    return v->clips[k];
}

bool W3dArmatureAssignAnimSet(Armature* a, const std::string& name, const std::vector<int>* indices,
                               std::string* reason) {
    if (!a) return false;
    W3dAnimSetsEnsureOps();
    // lo que tuviera antes se va (propio: se borra; compartido: se suelta)
    if (a->animsRec) W3dArmatureAnimsRelease(a);
    else { for (size_t i = 0; i < a->animations.size(); i++) delete a->animations[i]; a->animations.clear(); }
    const std::string entry = W3dAnimSetsEntryOf(name);
    if (entry.empty()) {
        if (reason) *reason = "el animset '" + name + "' no esta en el registro del proyecto (\"animsets\")";
        return false;
    }
    gReasonLoad.clear();
    W3dResource* r = W3dResourceAcquire(W3DREC_ANIMSET, entry, W3DREC_PERMANENT, W3DREC_BLOCKING, a->name.c_str());
    // uno EN VUELO (lo pidio una lista de carga async): el armature lo necesita YA
    for (int i = 0; r && r->state == W3DREC_IN_FLIGHT && i < 100000; i++) W3dResourcesPump();
    if (!r || r->state != W3DREC_READY || !r->data) {
        // Adquirir no suma la ref si la carga fallo; si quedo EN VUELO si la sumo
        if (r && r->state == W3DREC_IN_FLIGHT) W3dResourceRelease(r, W3DREC_PERMANENT);
        if (reason) *reason = gReasonLoad.empty() ? ("no se pudo cargar '" + entry + "'") : gReasonLoad;
        return false;
    }
    W3dAnimSet* set = (W3dAnimSet*)r->data;
    if (set->name.empty()) set->name = name;
    const size_t nClips = set->data.clips.size();
    if (indices)
        for (size_t i = 0; i < indices->size(); i++)
            if ((*indices)[i] < 0 || (*indices)[i] >= (int)nClips) {
                char b[160];
                sprintf(b, "el clip %d no existe en el animset (tiene %d)", (*indices)[i], (int)nClips);
                if (reason) *reason = b;
                W3dResourceRelease(r, W3DREC_PERMANENT);
                return false;
            }
    // ---- el esqueleto: calza tal cual (identidad) o hace falta una variante ----
    std::vector<int> map;
    MapTableToArmature(set->data.bones, a, map);
    bool identity = true;
    for (size_t s = 0; s < map.size(); s++) if (map[s] != (int)s) { identity = false; break; }
    W3dAnimSetVariant* var = 0;
    if (!identity) {
        for (size_t v = 0; v < set->variants.size() && !var; v++) if (set->variants[v]->map == map) var = set->variants[v];
        if (!var) { var = new W3dAnimSetVariant(); var->map = map; set->variants.push_back(var); }
    }
    std::vector<SkeletalAnimation*> newValue;
    if (indices) {
        newValue.reserve(indices->size());
        for (size_t i = 0; i < indices->size(); i++) {
            const size_t k = (size_t)(*indices)[i];
            newValue.push_back(var ? VariantClip(set, var, k) : set->data.clips[k]);
        }
    } else {
        newValue.reserve(nClips);
        for (size_t k = 0; k < nClips; k++) newValue.push_back(var ? VariantClip(set, var, k) : set->data.clips[k]);
    }
    // NOMBRES que la reparacion del editor tocaria: REPETIDOS dentro del armature (el mismo clip dos
    // veces, o dos clips homonimos) o SIN NORMALIZAR (vacio, con blancos en las puntas). El editor los
    // repara al abrir (W3dNamesRepairScene, espacio CLIP "Anim" de cada armature) escribiendo el
    // nombre A TRAVES DEL PUNTERO. Con los clips compartidos eso renombraria el clip de TODOS: este
    // armature pasa a clips propios ya mismo (caso raro: un archivo editado a mano o de antes).
    std::set<std::string> seen;
    bool repeated = false;
    for (size_t i = 0; i < newValue.size() && !repeated; i++) {
        if (!newValue[i]) continue;
        const std::string& n = newValue[i]->name;
        if (W3dNameNormalize(n, "Anim") != n || !seen.insert(n).second) repeated = true;
    }
    a->animations.swap(newValue);
    a->animsRec = r;
    a->animsVar = var;
    a->animSetName = name;
    a->animSetNotLoaded.clear();
    a->lastPoseFrame = -999999; a->lastPoseAnim = -999; a->poseDirty = true;
    if (repeated) W3dOwnArmatureAnims(a);
    return true;
}

// saca 'c' del recurso (su casillero queda en NULL: ya no lo libera el recurso)
static bool RemoveFromResource(W3dAnimSet* set, SkeletalAnimation* c) {
    for (size_t k = 0; k < set->data.clips.size(); k++)
        if (set->data.clips[k] == c) { set->data.clips[k] = 0; return true; }
    for (size_t v = 0; v < set->variants.size(); v++) {
        std::vector<SkeletalAnimation*>& vc = set->variants[v]->clips;
        for (size_t k = 0; k < vc.size(); k++) if (vc[k] == c) { vc[k] = 0; return true; }
    }
    return false;
}

static int gCopies = 0;
int W3dAnimSetsCopies() { return gCopies; }

void W3dOwnArmatureAnims(Armature* a) {
    if (!a || !a->animsRec) return;
    W3dResource* r = (W3dResource*)a->animsRec;
    W3dAnimSet* set = (r->state == W3DREC_READY) ? (W3dAnimSet*)r->data : 0;
    // UNICO usuario: los clips se le PASAN (sin copiar); el recurso muere enseguida con los
    // casilleros vacios. Si no, cada casillero es una copia propia (tambien los repetidos:
    // un clip que el armature usa dos veces pasa a ser dos clips independientes)
    const bool only = set && r->refTotal <= 1;
    std::set<SkeletalAnimation*> past;
    for (size_t i = 0; i < a->animations.size(); i++) {
        SkeletalAnimation* c = a->animations[i];
        if (!c) continue;
        if (only && !past.count(c) && RemoveFromResource(set, c)) { past.insert(c); continue; }
        a->animations[i] = new SkeletalAnimation(*c);
    }
    a->animsRec = 0;
    a->animsVar = 0;
    gCopies++;
    W3dResourceRelease(r, W3DREC_PERMANENT);
}

void W3dArmatureAnimsRelease(Armature* a) {
    if (!a || !a->animsRec) return;
    W3dResource* r = (W3dResource*)a->animsRec;
    a->animations.clear();
    a->animsRec = 0;
    a->animsVar = 0;
    W3dResourceRelease(r, W3DREC_PERMANENT);
}

// ============================================================================
//  DUPLICAR UN ARMATURE (Shift+D / Alt+D; ver W3dAnimSet.h)
// ============================================================================
// lo que el armature tuviera se va (propio: se borra; compartido: se suelta)
static void EmptyClips(Armature* a) {
    if (a->animsRec) { W3dArmatureAnimsRelease(a); return; }
    for (size_t i = 0; i < a->animations.size(); i++) delete a->animations[i];
    a->animations.clear();
}

void W3dArmatureAnimsCopy(Armature* dst, const Armature* src) {
    if (!dst || !src || dst == src) return;
    EmptyClips(dst);
    // cada casillero es un clip NUEVO (tambien los repetidos: un clip que src usa dos veces pasa a
    // ser dos clips independientes, igual que en el copy-on-write)
    dst->animations.reserve(src->animations.size());
    for (size_t i = 0; i < src->animations.size(); i++)
        dst->animations.push_back(src->animations[i] ? new SkeletalAnimation(*src->animations[i]) : (SkeletalAnimation*)0);
    dst->animSetName = src->animSetName;
    dst->animSetNotLoaded = src->animSetNotLoaded;
    dst->lastPoseFrame = -999999; dst->lastPoseAnim = -999;
}

// los clips PROPIOS de 'a' pasan a ser un animset EN MEMORIA, sin copiar: el recurso se queda con
// esos MISMOS objetos (sus pistas ya estan en el espacio de la tabla, que es el esqueleto de 'a').
// Su id no es una entrada ("#memoriaN:<nombre>"); el guardado le da una y, si lo que escribe es
// igual a esto (la HUELLA de abajo), el recurso pasa a llamarse como ella: queda igual que uno
// leido del disco (SaveAnimSetsConfirm).
static int gInMemory = 0;
static bool AnimSetInMemory(Armature* a) {
    if (!a || a->animsRec || a->animations.empty()) return false;
    // un mismo clip dos veces o un nombre que la reparacion del editor tocaria: compartidos, eso
    // escribiria el clip de TODOS (mismo criterio que W3dArmatureAssignAnimSet)
    std::set<const SkeletalAnimation*> seen;
    std::set<std::string> names;
    for (size_t i = 0; i < a->animations.size(); i++) {
        const SkeletalAnimation* c = a->animations[i];
        if (!c || !seen.insert(c).second) return false;
        if (W3dNameNormalize(c->name, "Anim") != c->name || !names.insert(c->name).second) return false;
    }
    W3dAnimSetsEnsureOps();
    W3dAnimSet* set = new W3dAnimSet();
    set->name = a->animSetName.empty() ? a->name : a->animSetName;
    W3dArmatureBoneNames(a, set->data.bones);
    // la HUELLA de lo que escribiria el guardado con esta biblioteca tal cual (mapa identidad: la
    // tabla es el esqueleto), con el mismo escritor que usa el guardado
    std::vector<int> identity(set->data.bones.size());
    for (size_t h = 0; h < identity.size(); h++) identity[h] = (int)h;
    std::vector<std::string> blocks(a->animations.size());
    std::vector<const std::string*> ptrs;
    for (size_t i = 0; i < a->animations.size(); i++) {
        W3dAnimSetClipBlock(*a->animations[i], identity, (int)identity.size(), blocks[i]);
        ptrs.push_back(&blocks[i]);
    }
    std::string bytes;
    W3dAnimSetBuild(set->data.bones, ptrs, &set->data.foreign, bytes);
    W3dAnimSetFootprint((const unsigned char*)bytes.data(), bytes.size(), set->footprint1, set->footprint2);
    set->fileSize = bytes.size();
    set->data.clips = a->animations;
    char b[48]; sprintf(b, "#memoria%d:", ++gInMemory);
    W3dResource* r = W3dResourceRegisterExternal(W3DREC_ANIMSET, std::string(b) + set->name, set, MemoryOf(set));
    if (!r) { set->data.clips.clear(); delete set; return false; }   // (los clips siguen siendo de 'a')
    a->animsRec = r;   // RegisterExternal deja refTotal = 1: la de 'a'
    a->animsVar = 0;
    a->animSetName = set->name;
    return true;
}

bool W3dArmatureAnimsLink(Armature* dst, Armature* src) {
    if (!dst || !src || dst == src) return false;
    EmptyClips(dst);
    dst->animSetNotLoaded = src->animSetNotLoaded;
    dst->lastPoseFrame = -999999; dst->lastPoseAnim = -999;
    if (src->animations.empty()) { dst->animSetName = src->animSetName; return true; }   // nada que compartir
    if (!src->animsRec && !AnimSetInMemory(src)) { W3dArmatureAnimsCopy(dst, src); return false; }
    W3dResource* r = (W3dResource*)src->animsRec;
    W3dResourceRetain(r, W3DREC_PERMANENT);
    dst->animations = src->animations;
    dst->animsRec = r;
    dst->animsVar = src->animsVar;   // el mismo esqueleto: el mismo mapeo (la misma variante, si hay)
    dst->animSetName = src->animSetName;
    return true;
}

bool W3dArmatureAnimsRemove(Armature* a, int i) {
    if (!a || i < 0 || i >= (int)a->animations.size()) return false;
    // el UNICO usuario: pasarle los clips no copia nada y el borrado se libera de verdad (si
    // quedaba colgado, el recurso seguia reteniendo un clip que ya no usa nadie). Con una
    // VARIANTE no: sus clips tienen en -1 las pistas de los huesos que le faltan y, propios, el
    // guardado ya no podria escribir los canonicos (el animset perderia esas pistas)
    const W3dResource* r = (const W3dResource*)a->animsRec;
    if (r && r->refTotal <= 1 && !a->animsVar) W3dOwnArmatureAnims(a);
    if (a->animsRec) {
        // COMPARTIDOS: el clip es del animset (lo siguen usando los demas): solo se suelta el
        // puntero. El armature sigue colgado con los que le quedan, sin copiar ninguno.
        a->animations.erase(a->animations.begin() + i);
        if (a->animations.empty()) W3dArmatureAnimsRelease(a);   // sin clips ya no lo usa
        return true;
    }
    delete a->animations[i];
    a->animations.erase(a->animations.begin() + i);
    return true;
}

bool W3dArmatureAnimsShared(const Armature* a) { return a && a->animsRec; }

W3dResource* W3dArmatureAnimSetResource(const Armature* a) { return a ? (W3dResource*)a->animsRec : 0; }

bool W3dArmatureAnimSetFits(const Armature* a) {
    const W3dAnimSet* set = W3dArmatureAnimSet(a);
    if (!set) return false;
    std::vector<int> map;
    MapTableToArmature(set->data.bones, a, map);
    const W3dAnimSetVariant* var = (const W3dAnimSetVariant*)a->animsVar;
    if (var) return map == var->map;
    for (size_t s = 0; s < map.size(); s++) if (map[s] != (int)s) return false;
    return true;
}

W3dAnimSet* W3dArmatureAnimSet(const Armature* a) {
    W3dResource* r = W3dArmatureAnimSetResource(a);
    return (r && r->state == W3DREC_READY) ? (W3dAnimSet*)r->data : 0;
}

bool W3dArmatureAnimsIndices(const Armature* a, std::vector<int>& out) {
    out.clear();
    W3dAnimSet* set = W3dArmatureAnimSet(a);
    if (!set) return false;
    const W3dAnimSetVariant* var = (const W3dAnimSetVariant*)a->animsVar;
    const std::vector<SkeletalAnimation*>& list = var ? var->clips : set->data.clips;
    std::map<const SkeletalAnimation*, int> idx;
    for (size_t k = 0; k < list.size(); k++) if (list[k]) idx[list[k]] = (int)k;
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
void W3dAnimSetsGetStats(W3dAnimSetsStats& st) {
    st = W3dAnimSetsStats();
    std::vector<W3dResource*> all;
    W3dResourcesList(W3DREC_ANIMSET, all);
    for (size_t i = 0; i < all.size(); i++) {
        W3dResource* r = all[i];
        if (!r || r->state != W3DREC_READY || !r->data) continue;
        const W3dAnimSet* s = (const W3dAnimSet*)r->data;
        st.live++;
        st.refs += r->refTotal;
        st.bytes += (long)sizeof(W3dAnimSet);
        for (size_t k = 0; k < s->data.clips.size(); k++) {
            if (!s->data.clips[k]) continue;
            st.clips++;
            st.bytes += W3dAnimSetClipBytes(s->data.clips[k], &st.keys);
        }
        for (size_t k = 0; k < s->data.hierarchies.size(); k++) {
            if (!s->data.hierarchies[k]) continue;
            st.clipsHier++;
            st.bytes += ClipHierBytes(s->data.hierarchies[k], &st.keysHier);
        }
        for (size_t v = 0; v < s->variants.size(); v++) {
            st.variants++;
            for (size_t k = 0; k < s->variants[v]->clips.size(); k++) {
                if (!s->variants[v]->clips[k]) continue;
                st.clipsVariant++;
                st.bytes += W3dAnimSetClipBytes(s->variants[v]->clips[k], &st.keys);
            }
        }
    }
}

// ============================================================================
//  OBJETOS RAIZ DE CLIPS DE JERARQUIA (ver W3dAnimSet.h). La biblioteca de un objeto es
//  un recurso ANIMSET como cualquier otro: el objeto le toma una referencia PERMANENTE
//  (la suelta al cambiarla o al morir) y los clips viven UNA vez en el recurso.
// ============================================================================
W3dResource* W3dHierResource(const Object* o) { return (o && o->clipsHier) ? o->clipsHier->rec : 0; }
W3dAnimSet* W3dHierAnimSet(const Object* o) {
    W3dResource* r = W3dHierResource(o);
    return (r && r->state == W3DREC_READY) ? (W3dAnimSet*)r->data : 0;
}

W3dResource* W3dHierLoadedLibrary(const std::string& name) {
    if (name.empty()) return 0;
    std::vector<W3dResource*> all;
    W3dResourcesList(W3DREC_ANIMSET, all);
    for (size_t i = 0; i < all.size(); i++)
        if (all[i] && all[i]->state == W3DREC_READY && all[i]->data &&
            ((W3dAnimSet*)all[i]->data)->name == name) return all[i];
    return 0;
}

std::string W3dHierFreeLibraryName(const std::string& base) {
    std::vector<std::string> taken;
    const std::vector<W3dAnimSetRow>& reg = W3dAnimSetsRegistry();
    for (size_t i = 0; i < reg.size(); i++) taken.push_back(reg[i].name);
    std::vector<W3dResource*> all;
    W3dResourcesList(W3DREC_ANIMSET, all);
    for (size_t i = 0; i < all.size(); i++)
        if (all[i] && all[i]->state == W3DREC_READY && all[i]->data && !((W3dAnimSet*)all[i]->data)->name.empty())
            taken.push_back(((W3dAnimSet*)all[i]->data)->name);
    return W3dUniqueNameInValues(W3dNameNormalize(base, "Clips"), "Clips", taken, -1);
}

static W3dHierRoot* HierOf(Object* o) {
    if (!o->clipsHier) o->clipsHier = new W3dHierRoot();
    return o->clipsHier;
}
static void HierReleaseRec(W3dHierRoot* j) {
    if (j && j->rec) { W3dResource* r = j->rec; j->rec = 0; W3dResourceRelease(r, W3DREC_PERMANENT); }
}

// la biblioteca 'nombre' CON una referencia PERMANENTE (la cargada con ese nombre o la del registro,
// leida de su entrada). NULL + motivo si no se puede.
static W3dResource* HierAcquireLibrary(const std::string& name, const char* who, std::string* reason) {
    W3dAnimSetsEnsureOps();
    W3dResource* r = W3dHierLoadedLibrary(name);
    if (r) { W3dResourceRetain(r, W3DREC_PERMANENT); return r; }
    const std::string entry = W3dAnimSetsEntryOf(name);
    if (entry.empty()) {
        if (reason) *reason = "la biblioteca de clips '" + name + "' no esta en el registro del proyecto (\"animsets\")";
        return 0;
    }
    gReasonLoad.clear();
    r = W3dResourceAcquire(W3DREC_ANIMSET, entry, W3DREC_PERMANENT, W3DREC_BLOCKING, who ? who : "");
    for (int i = 0; r && r->state == W3DREC_IN_FLIGHT && i < 100000; i++) W3dResourcesPump();
    if (!r || r->state != W3DREC_READY || !r->data) {
        if (r && r->state == W3DREC_IN_FLIGHT) W3dResourceRelease(r, W3DREC_PERMANENT);
        if (reason) *reason = gReasonLoad.empty() ? ("no se pudo cargar '" + entry + "'") : gReasonLoad;
        return 0;
    }
    W3dAnimSet* set = (W3dAnimSet*)r->data;
    if (set->name.empty()) set->name = name;
    return r;
}

W3dResource* W3dHierLibraryTake(const std::string& name, const char* who, std::string* reason) {
    if (name.empty()) { if (reason) *reason = "sin nombre"; return 0; }
    return HierAcquireLibrary(name, who, reason);
}

bool W3dHierAssign(Object* o, const std::string& name, std::string* reason) {
    if (!o) return false;
    if (name.empty()) { W3dHierRelease(o); return true; }
    std::string m;
    W3dResource* r = HierAcquireLibrary(name, o->name.c_str(), &m);
    W3dHierRoot* j = HierOf(o);
    HierReleaseRec(j);   // (despues de tomar la nueva: si es la misma, no se libera en el medio)
    j->animset = name;
    if (!r) {
        j->notLoaded = name;
        if (reason) *reason = m;
        w3dLogfE("[W3D] '%s': %s", o->name.c_str(), m.c_str());
        return false;
    }
    j->rec = r;
    j->notLoaded.clear();
    return true;
}

void W3dHierRelease(Object* o) {
    if (!o || !o->clipsHier) return;
    W3dHierRoot* j = o->clipsHier;
    HierReleaseRec(j);
    j->animset.clear();
    j->notLoaded.clear();
    // (sin biblioteca, ni retarget propio, ni capas no hay nada que recordar)
    if (j->retarget < 0 && j->layers.empty()) { delete j; o->clipsHier = 0; }
}

void W3dHierRestore(Object* o, W3dResource* r, const std::string& animset, const std::string& notLoaded) {
    if (!o) return;
    if (!r && animset.empty() && notLoaded.empty()) { W3dHierRelease(o); return; }
    if (r) W3dResourceRetain(r, W3DREC_PERMANENT);   // (antes de soltar la actual: puede ser la misma)
    W3dHierRoot* j = HierOf(o);
    HierReleaseRec(j);
    j->rec = r;
    j->animset   = animset;
    j->notLoaded = notLoaded;
}

void W3dHierLink(Object* dst, const Object* src) {
    if (!dst || dst == src) return;
    W3dHierRelease(dst);
    if (!src || !src->clipsHier) return;
    W3dHierRoot* j = HierOf(dst);
    j->animset   = src->clipsHier->animset;
    j->notLoaded = src->clipsHier->notLoaded;
    j->retarget = src->clipsHier->retarget;
    if (src->clipsHier->rec) { W3dResourceRetain(src->clipsHier->rec, W3DREC_PERMANENT); j->rec = src->clipsHier->rec; }
    // sus CAPAS del Mix (los clips se nombran: la copia mezcla los mismos, cada una con su cabezal)
    j->layers = src->clipsHier->layers;
    j->activeLayer = src->clipsHier->activeLayer;
    W3dHierLayersChanged(dst);
}

bool W3dHierNewLibrary(Object* o, const std::string& name) {
    if (!o) return false;
    W3dAnimSetsEnsureOps();
    W3dAnimSet* set = new W3dAnimSet();
    set->name = W3dHierFreeLibraryName(name);
    char b[48]; sprintf(b, "#memoria%d:", ++gInMemory);
    W3dResource* r = W3dResourceRegisterExternal(W3DREC_ANIMSET, std::string(b) + set->name, set, MemoryOf(set));
    if (!r) { delete set; return false; }
    W3dHierRoot* j = HierOf(o);
    HierReleaseRec(j);
    j->rec = r;   // RegisterExternal deja refTotal = 1: la de 'o'
    j->animset = set->name;
    j->notLoaded.clear();
    w3dLogf("[W3D] '%s': biblioteca de clips nueva '%s' (en memoria)", o->name.c_str(), set->name.c_str());
    return true;
}

W3dClipHier* W3dHierClip(const Object* o, const std::string& name) {
    const W3dAnimSet* set = W3dHierAnimSet(o);
    if (!set) return 0;
    for (size_t i = 0; i < set->data.hierarchies.size(); i++)
        if (set->data.hierarchies[i] && set->data.hierarchies[i]->name == name) return set->data.hierarchies[i];
    return 0;
}

// nombre libre entre los clips de jerarquia de 'set' ('excepto' conserva el suyo)
static std::string ClipHierFreeName(const W3dAnimSet* set, const std::string& base, const W3dClipHier* except) {
    std::vector<std::string> taken;
    for (size_t i = 0; set && i < set->data.hierarchies.size(); i++)
        if (set->data.hierarchies[i] && set->data.hierarchies[i] != except) taken.push_back(set->data.hierarchies[i]->name);
    return W3dUniqueNameInValues(W3dNameNormalize(base, "Clip"), "Clip", taken, -1);
}

W3dClipHier* W3dHierNewClip(Object* o, const std::string& name) {
    if (!o) return 0;
    if (!W3dHierAnimSet(o) && !W3dHierNewLibrary(o, o->name)) return 0;
    W3dAnimSet* set = W3dHierAnimSet(o);
    if (!set) return 0;
    W3dClipHier* c = new W3dClipHier();
    c->name = ClipHierFreeName(set, name, 0);
    c->size = W3dHierSize(o);   // (el retarget de otra jerarquia se mide contra esta)
    set->data.hierarchies.push_back(c);
    return c;
}

bool W3dHierClipDelete(Object* o, W3dClipHier* c) {
    if (W3dHierClipRemove(W3dHierResource(o), c) < 0) return false;
    delete c;
    return true;
}

W3dAnimSet* W3dHierLibrary(W3dResource* r) {
    return (r && r->type == W3DREC_ANIMSET && r->state == W3DREC_READY) ? (W3dAnimSet*)r->data : 0;
}

int W3dHierClipRemove(W3dResource* r, W3dClipHier* c) {
    W3dAnimSet* set = W3dHierLibrary(r);
    if (!set || !c) return -1;
    for (size_t i = 0; i < set->data.hierarchies.size(); i++)
        if (set->data.hierarchies[i] == c) {
            set->data.hierarchies.erase(set->data.hierarchies.begin() + (long)i);
            return (int)i;
        }
    return -1;
}

bool W3dHierClipReturn(W3dResource* r, W3dClipHier* c, int pos) {
    W3dAnimSet* set = W3dHierLibrary(r);
    if (!set || !c) return false;
    for (size_t i = 0; i < set->data.hierarchies.size(); i++) if (set->data.hierarchies[i] == c) return false;
    if (pos < 0) pos = 0;
    if (pos > (int)set->data.hierarchies.size()) pos = (int)set->data.hierarchies.size();
    // (el nombre: unico entre los de su biblioteca, por si mientras estuvo afuera se creo otro igual)
    c->name = ClipHierFreeName(set, c->name, c);
    set->data.hierarchies.insert(set->data.hierarchies.begin() + (long)pos, c);
    c->version++;   // (las vistas que lo editaban se re-atan: pudo cambiar el nombre)
    return true;
}

const W3dClipHier* W3dHierLayerClip(const Object* o, const W3dLayerAnim& cp, W3dResource** recOut) {
    if (recOut) *recOut = 0;
    if (!o || cp.anim.empty()) return 0;
    // la biblioteca de la raiz
    W3dClipHier* c = W3dHierClip(o, cp.anim);
    if (c) { if (recOut) *recOut = W3dHierResource(o); return c; }
    // "Biblioteca/clip": solo entre las CARGADAS (una capa no carga ni retiene nada)
    const size_t bar = cp.anim.find('/');
    if (bar != std::string::npos && bar > 0 && bar + 1 < cp.anim.size()) {
        W3dResource* r = W3dHierLoadedLibrary(cp.anim.substr(0, bar));
        const W3dAnimSet* set = W3dHierLibrary(r);
        if (!set) return 0;
        const std::string name = cp.anim.substr(bar + 1);
        for (size_t i = 0; i < set->data.hierarchies.size(); i++)
            if (set->data.hierarchies[i] && set->data.hierarchies[i]->name == name) {
                if (recOut) *recOut = r;
                return set->data.hierarchies[i];
            }
        return 0;
    }
    // suelto: el UNICO con ese nombre entre las cargadas
    const W3dClipHier* found = 0;
    W3dResource* of = 0;
    int n = 0;
    std::vector<W3dResource*> all;
    W3dResourcesList(W3DREC_ANIMSET, all);
    for (size_t i = 0; i < all.size(); i++) {
        const W3dAnimSet* set = W3dHierLibrary(all[i]);
        if (!set) continue;
        for (size_t k = 0; k < set->data.hierarchies.size(); k++)
            if (set->data.hierarchies[k] && set->data.hierarchies[k]->name == cp.anim) { found = set->data.hierarchies[k]; of = all[i]; n++; }
    }
    if (n != 1) return 0;
    if (recOut) *recOut = of;
    return found;
}

std::string W3dHierClipRename(W3dAnimSet* set, W3dClipHier* c, const std::string& newValue) {
    if (!c) return std::string();
    const std::string n = ClipHierFreeName(set, newValue, c);
    if (n != c->name) { c->name = n; c->version++; }
    return c->name;
}

bool W3dHierClipLive(const W3dResource* r, const W3dClipHier* c, unsigned series) {
    if (!r || !c || r->state != W3DREC_READY || !r->data) return false;
    const W3dAnimSet* set = (const W3dAnimSet*)r->data;
    for (size_t i = 0; i < set->data.hierarchies.size(); i++)
        if (set->data.hierarchies[i] == c) return c->series == series;
    return false;
}

W3dClipHier* W3dHierClipFind(const std::string& name, W3dResource** recOut) {
    if (recOut) *recOut = 0;
    const size_t bar = name.find('/');
    if (bar != std::string::npos && bar > 0 && bar + 1 < name.size()) {
        // "Biblioteca/clip": esa biblioteca (se carga de su entrada si hace falta)
        W3dResource* r = HierAcquireLibrary(name.substr(0, bar), "animObject", 0);
        if (!r) return 0;
        const W3dAnimSet* set = (const W3dAnimSet*)r->data;
        const std::string clip = name.substr(bar + 1);
        for (size_t i = 0; i < set->data.hierarchies.size(); i++)
            if (set->data.hierarchies[i] && set->data.hierarchies[i]->name == clip) {
                if (recOut) *recOut = r; else W3dResourceRelease(r, W3DREC_PERMANENT);
                return set->data.hierarchies[i];
            }
        W3dResourceRelease(r, W3DREC_PERMANENT);
        return 0;
    }
    // suelto: el UNICO clip con ese nombre entre las bibliotecas cargadas
    W3dClipHier* found = 0;
    W3dResource* of = 0;
    int n = 0;
    std::vector<W3dResource*> all;
    W3dResourcesList(W3DREC_ANIMSET, all);
    for (size_t i = 0; i < all.size(); i++) {
        if (!all[i] || all[i]->state != W3DREC_READY || !all[i]->data) continue;
        const W3dAnimSet* set = (const W3dAnimSet*)all[i]->data;
        for (size_t k = 0; k < set->data.hierarchies.size(); k++)
            if (set->data.hierarchies[k] && set->data.hierarchies[k]->name == name) { found = set->data.hierarchies[k]; of = all[i]; n++; }
    }
    if (n != 1) return 0;   // (ninguno, o ambiguo: hay que calificarlo "Biblioteca/clip")
    W3dResourceRetain(of, W3DREC_PERMANENT);
    if (recOut) *recOut = of; else W3dResourceRelease(of, W3DREC_PERMANENT);
    return found;
}

// ---- LAS RUTAS ----
// el nombre BASE (sin el ".NNN" canonico de los nombres unicos)
static std::string BaseName(const std::string& n) {
    std::string root; int num = 0;
    W3dNameSeparateSuffix(n, root, num);
    return root;
}
// el tramo de 'x' dentro de su padre 'p': su nombre base; si otro hermano tiene la misma base,
// base + "#k" (k = su orden entre los de esa base)
static std::string Segment(const Object* p, const Object* x) {
    const std::string base = BaseName(x->name);
    int k = 0, n = 0;
    for (size_t i = 0; i < p->Childrens.size(); i++) {
        const Object* h = p->Childrens[i];
        if (!h || BaseName(h->name) != base) continue;
        if (h == x) k = n;
        n++;
    }
    if (n <= 1) return base;
    char b[24]; sprintf(b, "#%d", k);
    return base + b;
}
std::string W3dHierPath(const Object* root, const Object* n) {
    if (!root || !n) return std::string();
    if (n == root) return ".";
    std::vector<std::string> segments;
    const Object* x = n;
    while (x && x != root) {
        const Object* p = x->Parent;
        if (!p) return std::string();   // no cuelga de 'raiz'
        segments.push_back(Segment(p, x));
        x = p;
    }
    if (x != root) return std::string();
    std::string r;
    for (size_t i = segments.size(); i-- > 0; ) { if (!r.empty()) r += '/'; r += segments[i]; }
    return r;
}
Object* W3dHierNode(Object* root, const std::string& path) {
    if (!root) return 0;
    if (path.empty() || path == ".") return root;
    Object* x = root;
    size_t a = 0;
    while (x && a <= path.size()) {
        size_t b = path.find('/', a);
        if (b == std::string::npos) b = path.size();
        std::string t = path.substr(a, b - a);
        a = b + 1;
        if (t.empty() || t == ".") { if (b >= path.size()) break; continue; }
        // "base#k": el k-esimo hijo con esa base
        int k = -1;
        const size_t h = t.rfind('#');
        if (h != std::string::npos && h > 0 && h + 1 < t.size()) {
            bool dig = true;
            for (size_t i = h + 1; i < t.size(); i++) if (t[i] < '0' || t[i] > '9') dig = false;
            if (dig) { k = atoi(t.c_str() + h + 1); t = t.substr(0, h); }
        }
        Object* next = 0;
        if (k < 0)   // el nombre exacto primero
            for (size_t i = 0; i < x->Childrens.size() && !next; i++)
                if (x->Childrens[i] && x->Childrens[i]->name == t) next = x->Childrens[i];
        if (!next) {  // despues por la base (una copia renombrada ".001")
            const std::string base = BaseName(t);
            int n = 0;
            for (size_t i = 0; i < x->Childrens.size() && !next; i++) {
                Object* c = x->Childrens[i];
                if (!c || BaseName(c->name) != base) continue;
                if (k < 0 || n == k) next = c;
                n++;
            }
        }
        x = next;
        if (b >= path.size()) break;
    }
    return x;
}

// ---- EL TAMANO de una jerarquia (retarget) ----
// Se mide SIN rotaciones (cada nodo = su traslacion y su escala sobre las de su padre): el tamano de una
// jerarquia no cambia porque un clip le haya girado la puerta o la grua (con retarget "rotaciones" lo unico
// que el clip le cambia a los descendientes es eso), asi que da lo mismo medirla en reposo o a mitad de un
// clip. 'org'/'esc': donde queda el origen del nodo y su escala acumulada, en el espacio del padre de la raiz.
static void SizeAccumulate(Object* o, const Vector3& orgParent, const Vector3& escParent, Vector3& mn, Vector3& mx, bool& has) {
    const Vector3 org(orgParent.x + escParent.x * o->pos.x, orgParent.y + escParent.y * o->pos.y, orgParent.z + escParent.z * o->pos.z);
    const Vector3 esc(escParent.x * o->scale.x, escParent.y * o->scale.y, escParent.z * o->scale.z);
    std::vector<Vector3> pts;
    pts.push_back(Vector3(0, 0, 0));
    if (o->getType() == ObjectType::mesh) {
        const Mesh* me = (const Mesh*)o;
        // su caja LOCAL: la calculada (aabbOk) o, si no la tiene (una copia recien duplicada, una malla que
        // nunca paso por ComputeBorders), la de sus posiciones de render
        Vector3 a = me->aabbMin, b = me->aabbMax;
        bool hasBox = me->aabbOk;
        if (!hasBox && me->vertex && me->vertexSize > 0) {
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
            hasBox = true;
        }
        if (hasBox) { pts.push_back(a); pts.push_back(b); }
    }
    for (size_t i = 0; i < pts.size(); i++) {
        const Vector3 p(org.x + esc.x * pts[i].x, org.y + esc.y * pts[i].y, org.z + esc.z * pts[i].z);
        if (!has) { mn = p; mx = p; has = true; continue; }
        if (p.x < mn.x) mn.x = p.x;
        if (p.y < mn.y) mn.y = p.y;
        if (p.z < mn.z) mn.z = p.z;
        if (p.x > mx.x) mx.x = p.x;
        if (p.y > mx.y) mx.y = p.y;
        if (p.z > mx.z) mx.z = p.z;
    }
    for (size_t i = 0; i < o->Childrens.size(); i++)
        if (o->Childrens[i]) SizeAccumulate(o->Childrens[i], org, esc, mn, mx, has);
}
float W3dHierSize(Object* root) {
    if (!root) return 0.0f;
    Vector3 mn(0, 0, 0), mx(0, 0, 0);
    bool has = false;
    SizeAccumulate(root, Vector3(0, 0, 0), Vector3(1, 1, 1), mn, mx, has);
    const Vector3 d = mx - mn;
    const float t = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    return t > 1e-6f ? t : 0.0f;
}
