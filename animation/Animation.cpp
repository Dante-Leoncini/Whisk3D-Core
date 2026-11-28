#include "Animation.h"
#include "W3dNames.h"   // LA regla de nombres unicos (compartida)
#include "VertexAnimation.h"   // guardar el rango en la animacion propia (kind 3)
#include <algorithm>   // std::sort (fuera el quicksort a mano)
#include "math/Vector3.h"        // EvalPropVec (las 3 curvas de una propiedad)
#include "SkeletalAnimation.h"   // clips de armature (startFrame/endFrame/FrameRate) para el rango por animacion
#include "objects/Armature.h"    // ActiveAnimArm->animations / activeAnim
#include "W3dAnimSet.h"          // clips COMPARTIDOS: los setters copian antes de escribir (copy-on-write)
#include "io/W3dResources.h"     // el reproductor de clips de jerarquia retiene su biblioteca mientras suena
#include "objects/Mesh.h"        // shape keys (W3dApplyShapes)
#include "objects/Light.h"       // los colores de una luz (canales AnimColor/AnimAmbient/... en el juego)
#include "objects/Materials.h"   // las pistas de MATERIAL (canales AnimMat*)
#include "Armature2DAnimation.h" // clips del ARMATURE 2D (kind 4): rango/fps propios
#include "w3dlog.h"              // aviso del blindaje del kind 4 (el Core no tiene UI para notificar)
#include <stdio.h> // sprintf GLOBAL (nombre unico de escena); Symbian/STLport no tiene std::snprintf
#include <ctime>
#include <cstring>   // memcmp: comparar keyframes bit a bit (las vistas de los clips de jerarquia)
#include <math.h>    // fmod: un cabezal en loop que se paso MUCHAS vueltas (W3dCycleWrap)

// ---- RELOJ del motor (ver Animation.h) ---------------------------------------------------
// Antes esto era una funcion global que el USUARIO del motor tenia que definir con este nombre
// exacto, sin ningun aviso mas que un comentario: el editor la ponia en main.cpp y cualquier
// otro proyecto se comia un "undefined reference" sin saber por que.
static W3dClockFn gClock = 0;

void W3dSetClock(W3dClockFn fn) { gClock = fn; }

#ifndef W3D_SYMBIAN   // en Symbian la define w3dskelstubs.cpp (User::NTickCount): dos definiciones = error de link
unsigned int w3dGetTicks() {
    if (gClock) return gClock();
    // respaldo portable: sirve para animar, aunque mida tiempo de CPU y no de pared.
    return (unsigned int)((double)clock() * 1000.0 / (double)CLOCKS_PER_SEC);
}
#endif

// Variables globales
bool PlayAnimation = false;   // arranca PAUSADO (el timeline lo togglea con Play)
int AnimPlayDir = 1;          // +1 adelante, -1 reversa
int AnimFPS = 30;             // fps de reproduccion de animaciones (default 30)
int StartFrame = 1;
int EndFrame = 250;
bool AnimIsGame  = false;          // "Es un juego": timeline infinito (sin Fin)
int CurrentFrame = 1;

// === Animaciones de ESCENA + seleccion de animacion activa a nivel app ===
std::vector<SceneAnimation*> SceneAnimations;
int ActiveSceneAnim = 0;
int ActiveAnimKind = 0;          // 0 escena, 1 clip de armature, 2 juego (lo ponen el Play del editor y el runtime del juego, w3drun), 3 anim del objeto, 4 clip de armature 2D
Armature* ActiveAnimArm = NULL;  // armature del clip activo (kind 1)
Mesh* ActiveAnimMesh = 0;        // kind 3: la malla cuya vertex anim esta en el timeline

void InitSceneAnimations() {
    if (SceneAnimations.empty()) {
        SceneAnimations.push_back(new SceneAnimation("Scene"));
        ActiveSceneAnim = 0;
    }
    if (ActiveSceneAnim < 0 || ActiveSceneAnim >= (int)SceneAnimations.size()) ActiveSceneAnim = 0;
}

const char* ActiveSceneName() {
    InitSceneAnimations();
    return SceneAnimations[ActiveSceneAnim]->name.c_str();
}

// las curvas de objeto CAMBIARON por fuera del cabezal (ver Animation.h)
unsigned int g_animCurvesGen = 0;

void SetActiveScene(int idx) {
    InitSceneAnimations();
    if (idx < 0 || idx >= (int)SceneAnimations.size() || idx == ActiveSceneAnim) return;
    // la VISTA de un clip de jerarquia que se deja: sus curvas vuelven a su clip (compartido)
    if (W3dClipViewLive(ActiveSceneAnim)) W3dClipViewWrite(ActiveSceneAnim);
    // guardar las curvas de la escena que estaba activa; cargar las de la nueva (swap: sin copias)
    SceneAnimations[ActiveSceneAnim]->objects.swap(AnimationObjects);
    ActiveSceneAnim = idx;
    AnimationObjects.swap(SceneAnimations[ActiveSceneAnim]->objects);
    // ...y la que se elige: si su clip cambio mientras tanto (se edito desde otra raiz), se re-ata
    if (W3dClipViewLive(idx) && SceneAnimations[idx]->versionView != SceneAnimations[idx]->clipHier->version)
        W3dClipViewRead(idx);
    W3dAnimCurvesInvalidate();   // otras curvas: el editor vuelve a posar aunque el frame sea el mismo
}

int NewScene() {
    InitSceneAnimations();
    // (la VISTA de un clip de jerarquia que se deja vuelve a su clip, como en SetActiveScene)
    if (W3dClipViewLive(ActiveSceneAnim)) W3dClipViewWrite(ActiveSceneAnim);
    // guardar las curvas de la escena activa (AnimationObjects queda vacio = curvas de la escena nueva)
    SceneAnimations[ActiveSceneAnim]->objects.swap(AnimationObjects);
    AnimationObjects.clear();
    // nombre unico con LA regla comun: "Scene", "Scene.001", ... (antes era "Scene N",
    // otra convencion mas de las cinco que habia)
    SceneAnimations.push_back(new SceneAnimation(SceneAnimFreeName("Scene", -1)));
    ActiveSceneAnim = (int)SceneAnimations.size() - 1;
    W3dAnimCurvesInvalidate();
    return ActiveSceneAnim;
}

// nombre libre en el espacio GLOBAL de animaciones de escena ('excepto' conserva el suyo). Las VISTAS de
// los clips de jerarquia no cuentan: sus nombres son los de su biblioteca (W3dHierClipRename)
static bool SceneAnimExists(const std::string& n, void* ctx) {
    int except = *(int*)ctx;
    for (size_t i = 0; i < SceneAnimations.size(); i++)
        if ((int)i != except && SceneAnimations[i] && !SceneAnimations[i]->isClip && SceneAnimations[i]->name == n)
            return true;
    return false;
}
std::string SceneAnimFreeName(const std::string& base, int except) {
    InitSceneAnimations();
    return W3dUniqueName(base, "Scene", SceneAnimExists, &except);
}
// RENAME: antes no chequeaba NADA (dos escenas podian llamarse igual escribiendolo a mano).
// Devuelve el nombre que quedo.
std::string RenameActiveScene(const std::string& name) {
    InitSceneAnimations();
    SceneAnimation* e = SceneAnimations[ActiveSceneAnim];
    // la vista de un CLIP DE JERARQUIA renombra el clip (unico entre los de su biblioteca: lo ven todos)
    if (e->isClip) {
        if (W3dClipViewLive(ActiveSceneAnim)) {
            e->name = W3dHierClipRename(W3dHierAnimSet(e->owner), e->clipHier, name);
            e->versionView = e->clipHier->version;   // (la vista no quedo vieja: solo cambio el nombre)
        }
        return e->name;
    }
    e->name = SceneAnimFreeName(name, ActiveSceneAnim);
    return e->name;
}

void DeleteActiveScene() {
    InitSceneAnimations();
    // la VISTA de un clip de jerarquia: se borra el CLIP de su biblioteca (lo dejan de ver todos sus usuarios;
    // las otras vistas de ese clip quedan huerfanas: ni se listan ni se escriben)
    if (W3dClipViewLive(ActiveSceneAnim)) {
        SceneAnimation* v = SceneAnimations[ActiveSceneAnim];
        W3dHierClipDelete(v->owner, v->clipHier);
    }
    int pos = -1;
    SceneAnimation* e = W3dActiveSceneRemove(&pos);
    if (e) { delete e; return; }
    // siempre queda al menos UNA animacion de escena (los clips de objeto no cuentan): borrar la ultima
    // solo vacia sus curvas
    AnimationObjects.clear();
    SceneAnimations[ActiveSceneAnim]->objects.clear();
    W3dAnimCurvesInvalidate();
}

SceneAnimation* W3dActiveSceneRemove(int* pos) {
    InitSceneAnimations();
    int scenes = 0;
    for (size_t i = 0; i < SceneAnimations.size(); i++) if (!SceneAnimations[i]->isClip) scenes++;
    if (!SceneAnimations[ActiveSceneAnim]->isClip && scenes <= 1) return 0;
    const int deleted = ActiveSceneAnim;
    SceneAnimation* e = SceneAnimations[deleted];
    e->objects.clear();
    e->objects.swap(AnimationObjects);           // sus curvas (las vivas estaban en AnimationObjects) van con ella
    SceneAnimations.erase(SceneAnimations.begin() + deleted);
    // queda activa la que ocupa su lugar (la siguiente; si era la ultima, la anterior), como siempre, pero
    // salteando los clips de objeto: el descarte cae en una ANIMACION DE ESCENA
    int newValue = -1;
    for (int i = deleted; i < (int)SceneAnimations.size() && newValue < 0; i++) if (!SceneAnimations[(size_t)i]->isClip) newValue = i;
    for (int i = deleted - 1; i >= 0 && newValue < 0; i--) if (!SceneAnimations[(size_t)i]->isClip) newValue = i;
    ActiveSceneAnim = (newValue < 0) ? 0 : newValue;
    AnimationObjects.swap(SceneAnimations[ActiveSceneAnim]->objects); // cargar las de la escena que quedo activa
    W3dAnimCurvesInvalidate();
    if (pos) *pos = deleted;
    return e;
}

void W3dSceneReturn(SceneAnimation* e, int pos) {
    if (!e) return;
    InitSceneAnimations();
    if (pos < 0) pos = 0;
    if (pos > (int)SceneAnimations.size()) pos = (int)SceneAnimations.size();
    // (la vista que se esta editando vuelve a su clip antes de dejarla)
    if (W3dClipViewLive(ActiveSceneAnim)) W3dClipViewWrite(ActiveSceneAnim);
    SceneAnimations[ActiveSceneAnim]->objects.swap(AnimationObjects);   // la activa guarda sus curvas
    SceneAnimations.insert(SceneAnimations.begin() + pos, e);
    ActiveSceneAnim = pos;
    AnimationObjects.clear();
    AnimationObjects.swap(e->objects);                                  // ...y la devuelta carga las suyas
    if (W3dClipViewLive(pos) && e->versionView != e->clipHier->version) W3dClipViewRead(pos);
    W3dAnimCurvesInvalidate();
}

bool W3dClipsActiveViewCheck() {
    InitSceneAnimations();
    if (ActiveAnimKind != 0 || !W3dAnimIsClip(ActiveSceneAnim) || W3dClipViewLive(ActiveSceneAnim)) return false;
    // la animacion de escena mas cercana (siempre hay al menos una)
    int newValue = -1;
    for (int d = 1; d < (int)SceneAnimations.size() && newValue < 0; d++) {
        const int a = ActiveSceneAnim - d, b = ActiveSceneAnim + d;
        if (a >= 0 && !SceneAnimations[(size_t)a]->isClip) newValue = a;
        else if (b < (int)SceneAnimations.size() && !SceneAnimations[(size_t)b]->isClip) newValue = b;
    }
    if (newValue < 0) return false;
    SetActiveScene(newValue);
    AnimLoadActiveRange();
    return true;
}

// ===== Start/End/FPS PROPIOS por animacion (escena o clip). Los comparten la tarjeta y el timeline. =====
static SkeletalAnimation* ActiveAnimClip(){
    if (ActiveAnimKind == 1 && ActiveAnimArm &&
        ActiveAnimArm->activeAnim >= 0 && ActiveAnimArm->activeAnim < (int)ActiveAnimArm->animations.size())
        return ActiveAnimArm->animations[ActiveAnimArm->activeAnim];
    return NULL;
}
// el mismo clip, para ESCRIBIRLE el rango/fps: si es de un animset compartido, el armature pasa
// antes a clips propios (copy-on-write) y el resto de los que lo usan no se entera. Los setters
// lo piden SOLO si el valor cambia (confirmar un campo sin tocarlo no copia nada)
static SkeletalAnimation* ActiveAnimClipEditable(){
    if (ActiveAnimClip()) W3dOwnArmatureAnims(ActiveAnimArm);
    return ActiveAnimClip();
}
// el clip del ARMATURE 2D activo (kind 4), o NULL
static Armature2DAnimation* AnimClip2DActive(){
    if (ActiveAnimKind != 4 || !ActiveAnimMesh) return NULL;
    return Arm2DActiveClip(ActiveAnimMesh);
}
void AnimLoadActiveRange(){
    Armature2DAnimation* c2 = AnimClip2DActive();
    if (c2){ StartFrame = c2->startFrame; EndFrame = c2->endFrame; if (c2->fps > 0) AnimFPS = c2->fps;
             if (EndFrame < StartFrame) EndFrame = StartFrame;
             if (CurrentFrame < StartFrame) CurrentFrame = StartFrame;
             if (CurrentFrame > EndFrame && EndFrame >= StartFrame) CurrentFrame = EndFrame;
             return; }
    SkeletalAnimation* c = ActiveAnimClip();
    if (c){ StartFrame = c->startFrame; EndFrame = c->endFrame; if (c->FrameRate > 0) AnimFPS = c->FrameRate; }
    else { InitSceneAnimations(); SceneAnimation* s = SceneAnimations[ActiveSceneAnim];
           StartFrame = s->startFrame; EndFrame = s->endFrame; AnimFPS = s->fps; }
    if (CurrentFrame < StartFrame) CurrentFrame = StartFrame;
    if (CurrentFrame > EndFrame && EndFrame >= StartFrame) CurrentFrame = EndFrame;
}
// la animacion PROPIA activa del objeto (kind 3), o NULL
static VertexAnimation* OwnActiveAnimObj(){
    if (ActiveAnimKind != 3 || !ActiveAnimMesh) return NULL;
    VertexAnimationActive* va = FindTargetAnim(ActiveAnimMesh);
    int i = va ? va->currentAnim : -1;
    return (i>=0 && i<(int)ActiveAnimMesh->animations.size()) ? ActiveAnimMesh->animations[i] : NULL;
}
// BLINDAJE del kind 4 (clip del armature 2D): si el timeline dice "clip 2D" pero NO hay clip
// activo (el rig recien creado / el que quedo activo tras un undo no tiene ninguno), estos
// setters caian por la cadena de else hasta la ANIMACION DE ESCENA y le escribian el rango ahi
// -- una animacion que el usuario ni estaba mirando. En ese estado no hay nada que escribir:
// NO-OP + aviso al log (el Core no tiene notificaciones de UI). Cuando el rig tenga un clip,
// AnimClip2DActive deja de ser NULL y los campos vuelven a funcionar solos.
static bool AnimKind4NoClip(){
    if (ActiveAnimKind != 4) return false;
    if (AnimClip2DActive()) return false;
    w3dLogfW("Animation: kind 4 (clip de armature 2D) sin clip activo -> el rango no se escribe en ningun lado");
    return true;
}
// MIX (g_animMix, SkeletalAnimation.cpp): el rango del timeline es el del MIX, no el del clip de la capa elegida
extern bool g_animMix; extern float g_mixStart, g_mixEnd;
void AnimSetStart(int v){ if (g_animMix) { g_mixStart = (float)v; StartFrame = v; return; }
    if (AnimKind4NoClip()) return;
    Armature2DAnimation* c2 = AnimClip2DActive(); if (c2) c2->startFrame = v;
    else { VertexAnimation* oa = OwnActiveAnimObj(); if (oa) oa->startFrame = v;
    else { SkeletalAnimation* c = ActiveAnimClip(); if (c) { if (c->startFrame != v) ActiveAnimClipEditable()->startFrame = v; }
    else { InitSceneAnimations(); SceneAnimations[ActiveSceneAnim]->startFrame = v; } } } StartFrame = v; }
void AnimSetEnd(int v){ if (g_animMix) { g_mixEnd = (float)v; EndFrame = v; return; }
    if (AnimKind4NoClip()) return;
    Armature2DAnimation* c2 = AnimClip2DActive(); if (c2) c2->endFrame = v;
    else { VertexAnimation* oa = OwnActiveAnimObj(); if (oa) oa->endFrame = v;
    else { SkeletalAnimation* c = ActiveAnimClip(); if (c) { if (c->endFrame != v) ActiveAnimClipEditable()->endFrame = v; }
    else { InitSceneAnimations(); SceneAnimations[ActiveSceneAnim]->endFrame = v; } } } EndFrame = v; }
void AnimSetFps(int v){ if (AnimKind4NoClip()) return;
    Armature2DAnimation* c2 = AnimClip2DActive(); if (c2) c2->fps = v;
    else { VertexAnimation* oa = OwnActiveAnimObj(); if (oa) oa->fps = v;
    else { SkeletalAnimation* c = ActiveAnimClip(); if (c) { if (c->FrameRate != v) ActiveAnimClipEditable()->FrameRate = v; }
    else { InitSceneAnimations(); SceneAnimations[ActiveSceneAnim]->fps = v; } } } AnimFPS = v; }

// avanza un frame haciendo loop en [Start..End]; respeta la direccion del play
void AnimTick() {
    if (!PlayAnimation) return;
    if (EndFrame <= StartFrame) return; // rango vacio (ej: clip nuevo start=1/end=0): nada que reproducir
    CurrentFrame += AnimPlayDir;
    if (CurrentFrame > EndFrame)   CurrentFrame = StartFrame; // loop hacia adelante
    if (CurrentFrame < StartFrame) CurrentFrame = EndFrame;   // loop en reversa
}

unsigned int lastAnimTime = w3dGetTicks();
unsigned int lastRenderTime = w3dGetTicks();


// ============================================================================
//  W3dKeys: las claves de una curva, comunes o COMPACTAS (ver Animation.h)
// ============================================================================
namespace {
long gKeysBytes = 0;           // bytes de claves vivos: capacidad de los vectores + bloques compactos
long gCompactKeys = 0;         // curvas compactas vivas, sus claves y sus bloques
long gCompactKeysN = 0;
long gCompactKeysBytes = 0;
long gKeysExpansions = 0;       // compacta -> comun, desde el arranque

// (una IRREGULAR lleva ademas un int por clave: su frame)
size_t BytesBlock(unsigned n, bool irregular) {
    return sizeof(W3dCompactKeys) + (size_t)n * (sizeof(float) + (irregular ? sizeof(int) : 0));
}
size_t BytesBlock(const W3dCompactKeys* c) { return BytesBlock(c->n, c->Irregular()); }
W3dCompactKeys* NewBlock(unsigned n, bool irregular) {
    W3dCompactKeys* c = static_cast<W3dCompactKeys*>(::operator new(BytesBlock(n, irregular)));
    c->first = 0; c->step = irregular ? 0 : 1; c->interp = KfLinear; c->typeH = HAuto; c->n = n;
    gCompactKeys++; gCompactKeysN += (long)n;
    gCompactKeysBytes += (long)BytesBlock(c); gKeysBytes += (long)BytesBlock(c);
    return c;
}
void BlockRelease(W3dCompactKeys* c) {
    gCompactKeys--; gCompactKeysN -= (long)c->n;
    gCompactKeysBytes -= (long)BytesBlock(c); gKeysBytes -= (long)BytesBlock(c);
    ::operator delete(c);
}
W3dCompactKeys* BlockCopy(const W3dCompactKeys* o) {
    W3dCompactKeys* c = NewBlock(o->n, o->Irregular());
    memcpy(c, o, BytesBlock(o));   // (cabecera + valores (+ frames): datos planos, los bits tal cual)
    return c;
}
long BytesVec(size_t capacity) { return (long)(capacity * sizeof(keyFrame)); }
} // namespace

void W3dKeysGetStats(W3dKeysStats& st) {
    st.bytes = gKeysBytes; st.compact = gCompactKeys; st.compactKeys = gCompactKeysN;
    st.compactBytes = gCompactKeysBytes; st.expansions = gKeysExpansions;
}

W3dKeys::W3dKeys(const W3dKeys& o) : v_(o.v_), c_(o.c_ ? BlockCopy(o.c_) : 0) {
    gKeysBytes += BytesVec(v_.capacity());
}
W3dKeys::~W3dKeys() {
    gKeysBytes -= BytesVec(v_.capacity());
    if (c_) BlockRelease(c_);
}
W3dKeys& W3dKeys::operator=(const W3dKeys& o) {
    if (this == &o) return *this;
    if (o.c_) {
        // una compacta se copia compacta, y el vector que hubiera se suelta (no queda reservado a 32 B por clave)
        W3dCompactKeys* c = BlockCopy(o.c_);
        ReleaseCompact();
        gKeysBytes -= BytesVec(v_.capacity());
        Vec().swap(v_);
        c_ = c;
    } else {
        ReleaseCompact();
        const size_type prev = v_.capacity();
        v_ = o.v_;
        CountCapacity(prev);
    }
    return *this;
}
W3dKeys& W3dKeys::operator=(const Vec& v) {
    ReleaseCompact();
    const size_type prev = v_.capacity();
    v_ = v;
    CountCapacity(prev);
    return *this;
}
void W3dKeys::ReleaseCompact() {
    if (c_) { BlockRelease(c_); c_ = 0; }
}
void W3dKeys::CountCapacity(size_type prev) {
    gKeysBytes += BytesVec(v_.capacity()) - BytesVec(prev);
}
// la compacta pasa a ser una curva comun con las MISMAS claves: lo que leeria el .w3da sin compactar (frames
// sumando el paso, la interpolacion y el tipo de handle de todas, handles 0.0f)
void W3dKeys::ExpandNow() const {
    const W3dCompactKeys* c = c_;
    Vec tmp(c->n);
    const float* val = c->Values();
    for (unsigned i = 0; i < c->n; i++) {
        keyFrame& k = tmp[i];
        k.frame = c->Frame(i); k.value = val[i]; k.Interpolation = c->interp; k.handleType = c->typeH;
    }
    const size_type prev = v_.capacity();   // (0: vacio mientras era compacta)
    v_.swap(tmp);
    gKeysBytes += BytesVec(v_.capacity()) - BytesVec(prev);
    BlockRelease(c_);
    c_ = 0;
    gKeysExpansions++;
}
keyFrame W3dKeys::Key(size_type i) const {
    if (!c_) return v_[i];
    keyFrame k;
    k.frame = c_->Frame((unsigned)i); k.value = c_->Values()[i];
    k.Interpolation = c_->interp; k.handleType = c_->typeH;
    return k;
}
long W3dKeys::BytesMemory() const {
    return c_ ? (long)BytesBlock(c_) : BytesVec(v_.capacity());
}
bool W3dKeys::SameBits(const W3dKeys& o) const {
    const size_type n = size();
    if (n != o.size()) return false;
    if (c_ && o.c_ && c_->Irregular() == o.c_->Irregular())
        // (las mismas claves = la misma cabecera -frames regulares o los mismos frames, interp y tipo de handle de
        //  todas- y los mismos valores)
        return c_->first == o.c_->first && c_->step == o.c_->step && c_->interp == o.c_->interp &&
               c_->typeH == o.c_->typeH && memcmp(c_->Values(), o.c_->Values(), n * sizeof(float)) == 0 &&
               (!c_->Irregular() || memcmp(c_->Frames(), o.c_->Frames(), n * sizeof(int)) == 0);
    for (size_type i = 0; i < n; i++) {
        const keyFrame x = Key(i), y = o.Key(i);
        if (x.frame != y.frame || x.Interpolation != y.Interpolation || x.handleType != y.handleType) return false;
        if (memcmp(&x.value, &y.value, sizeof(float)) || memcmp(&x.inDF, &y.inDF, sizeof(float)) ||
            memcmp(&x.inDV, &y.inDV, sizeof(float)) || memcmp(&x.outDF, &y.outDF, sizeof(float)) ||
            memcmp(&x.outDV, &y.outDV, sizeof(float))) return false;
    }
    return true;
}
bool W3dKeys::CompactPossible(size_type n, int first, int step) {
    // (el ultimo frame tiene que entrar en un int: es el que el lector obtiene sumando el paso)
    return n >= 2 && n <= 0xFFFFFFFFu && step > 0 &&
           (double)first + (double)step * (double)(n - 1) <= 2147483647.0;
}
float* W3dKeys::CompactCreate(size_type n, int first, int step, int interp, int typeH) {
    ReleaseCompact();
    gKeysBytes -= BytesVec(v_.capacity());
    Vec().swap(v_);
    c_ = NewBlock((unsigned)n, false);
    c_->first = first; c_->step = step; c_->interp = interp; c_->typeH = typeH;
    return c_->Values();
}
bool W3dKeys::CompactIrregularPossible(size_type n, const int* frames) {
    if (n < 2 || n > 0xFFFFFFFFu) return false;
    for (size_type i = 1; i < n; i++) if (frames[i] <= frames[i - 1]) return false;
    return true;
}
float* W3dKeys::CompactCreateIrregular(size_type n, const int* frames, int interp, int typeH) {
    ReleaseCompact();
    gKeysBytes -= BytesVec(v_.capacity());
    Vec().swap(v_);
    c_ = NewBlock((unsigned)n, true);
    c_->first = frames[0]; c_->interp = interp; c_->typeH = typeH;
    memcpy(c_->Frames(), frames, (size_t)n * sizeof(int));
    return c_->Values();
}
void W3dKeys::push_back(const keyFrame& k) {
    Expand(); const size_type prev = v_.capacity(); v_.push_back(k); CountCapacity(prev);
}
W3dKeys::iterator W3dKeys::insert(iterator pos, const keyFrame& k) {
    Expand(); const size_type prev = v_.capacity(); iterator r = v_.insert(pos, k); CountCapacity(prev);
    return r;
}
void W3dKeys::insert(iterator pos, size_type n, const keyFrame& k) {
    Expand(); const size_type prev = v_.capacity(); v_.insert(pos, n, k); CountCapacity(prev);
}
void W3dKeys::clear() { ReleaseCompact(); v_.clear(); }
void W3dKeys::resize(size_type n, const keyFrame& k) {
    Expand(); const size_type prev = v_.capacity(); v_.resize(n, k); CountCapacity(prev);
}
void W3dKeys::reserve(size_type n) {
    Expand(); const size_type prev = v_.capacity(); v_.reserve(n); CountCapacity(prev);
}
void W3dKeys::assign(size_type n, const keyFrame& k) {
    ReleaseCompact(); const size_type prev = v_.capacity(); v_.assign(n, k); CountCapacity(prev);
}
void W3dKeys::swap(W3dKeys& o) {   // (los bytes vivos no cambian: solo cambian de duenio)
    W3dCompactKeys* c = c_; c_ = o.c_; o.c_ = c;
    v_.swap(o.v_);
}
void W3dKeys::swap(Vec& v) {
    Expand(); const size_type prev = v_.capacity(); v_.swap(v); CountCapacity(prev);
}

// orden por numero de frame (para std::sort)
static bool compareKeyFrames(const keyFrame& a, const keyFrame& b) {
    return a.frame < b.frame;
}

// AnimProperty
void AnimProperty::SortKeyFrames() {
    if (keyframes.Compact()) return;   // (frames primero + paso * i con paso > 0: ya esta ordenada)
    std::sort(keyframes.begin(), keyframes.end(), compareKeyFrames);
}

// ============================================================================
//  EVALUACION de UNA curva (un canal). Es el UNICO lugar donde se interpola: el esqueleto y los objetos
//  llaman aca, asi que agregar bezier/easing se hace en un solo sitio (y el editor de curvas lo hereda).
// ============================================================================
// Handle EFECTIVO del keyframe i (offset dF/dV desde el). Ver el enum H* en Animation.h.
// (Lee las claves con Frame/Valor/TypeH/HandleDF/DV de W3dKeys: no expande una curva COMPACTA y es el MISMO
//  codigo para las dos, asi que dan los mismos bits)
void AnimProperty::EffectiveHandle(size_t i, bool output, float& dF, float& dV) const {
    const W3dKeys& K = keyframes;
    const size_t n = K.size();
    const int kFrame = K.Frame(i);
    const float kValue = K.Value(i);
    const int kType = K.TypeH(i);
    dF = 0.0f; dV = 0.0f;
    if (n < 2) return;

    // el vecino de ESTE lado (en las puntas se usa el unico que hay: el handle sale simetrico)
    size_t vec = output ? (i + 1 < n ? i + 1 : (i > 0 ? i - 1 : i))
                        : (i > 0 ? i - 1 : (i + 1 < n ? i + 1 : i));
    // largo por defecto: un tercio del tramo de este lado, con el signo del lado
    float length = (float)(K.Frame(vec) - kFrame) / 3.0f;
    if (output  && length <= 0.0f) length =  -length;   // en la punta derecha el vecino esta a la IZQUIERDA
    if (!output && length >= 0.0f) length =  -length;
    if (length == 0.0f) length = output ? 1.0f : -1.0f;

    if (kType == HFree || kType == HAligned){
        dF = K.HandleDF(i, output);
        dV = K.HandleDV(i, output);
        return;
    }
    if (kType == HPlane){ dF = length; dV = 0.0f; return; }   // horizontal: ease in/out
    if (kType == HVector){
        // apunta al vecino: el tramo de este lado sale RECTO
        int df = K.Frame(vec) - kFrame;
        if (df == 0){ dF = length; dV = 0.0f; return; }
        float t = length / (float)df;                        // fraccion hacia el vecino (1/3, con el signo del lado)
        dF = length;
        dV = (K.Value(vec) - kValue) * t;
        return;
    }
    // HAuto / HAutoClamped: pendiente suave (Catmull-Rom) por los dos vecinos
    size_t a = (i == 0) ? 0 : i - 1;
    size_t b = (i + 1 >= n) ? n - 1 : i + 1;
    int spanAB = K.Frame(b) - K.Frame(a);
    float m = (spanAB != 0) ? (K.Value(b) - K.Value(a)) / (float)spanAB : 0.0f;
    if (kType == HAutoClamped && i > 0 && i + 1 < n){
        // PICO: si los dos vecinos caen del mismo lado, este keyframe es un maximo/minimo local -> handle PLANO,
        // asi la curva no se pasa de largo (que es justo lo que arruina una animacion: un rebote que se hunde
        // por debajo del piso).
        float dPrev = K.Value(i-1) - kValue;
        float dNext = K.Value(i+1) - kValue;
        if ((dPrev > 0.0f && dNext > 0.0f) || (dPrev < 0.0f && dNext < 0.0f)) m = 0.0f;
    }
    dF = length;
    dV = m * length;
    if (kType == HAutoClamped){
        // ...y ademas el punto de control no puede irse mas alla del valor del VECINO. Aplanar solo los picos no
        // alcanza: el handle de un keyframe que NO es pico igual puede empujar la curva por encima del vecino
        // (una bezier vive dentro de la cascara de sus puntos de control, asi que acotando el control se acota la
        // curva). Esto es lo que hace que "clamped" de verdad no se pase.
        float vv = K.Value(vec);
        float lo = (kValue < vv) ? kValue : vv;
        float hi = (kValue > vv) ? kValue : vv;
        float cp = kValue + dV;
        if (cp < lo) dV = lo - kValue;
        else if (cp > hi) dV = hi - kValue;
    }
}

// ---- EASING (ver KfSine... en Animation.h). Cada curva se define por su Ease IN (lenta al arrancar); el Ease
// OUT es la misma dada vuelta (1 - in(1 - t)) y el In-Out pega la mitad de cada una.
static float EaseBounce(float t){   // Bounce OUT: cae y rebota cada vez mas bajo hasta quedarse en 1
    const float n = 7.5625f, d = 2.75f;
    if (t < 1.0f / d)   return n * t * t;
    if (t < 2.0f / d) { t -= 1.5f / d;   return n * t * t + 0.75f; }
    if (t < 2.5f / d) { t -= 2.25f / d;  return n * t * t + 0.9375f; }
    t -= 2.625f / d; return n * t * t + 0.984375f;
}
static float EaseInF(int base, float t){
    switch (base){
        case KfSine:  return 1.0f - cosf(t * 1.5707963f);
        case KfQuad:  return t * t;
        case KfCubic: return t * t * t;
        case KfQuart: return t * t * t * t;
        case KfQuint: return t * t * t * t * t;
        case KfExpo:  return (powf(2.0f, 10.0f * t) - 1.0f) / 1023.0f;   // (exacto en 0 y en 1)
        case KfCirc:  return 1.0f - sqrtf(1.0f - t * t);
        case KfBack:  { const float s = 1.70158f; return t * t * ((s + 1.0f) * t - s); }
        case KfBounce: return 1.0f - EaseBounce(1.0f - t);
        case KfElastic: {
            if (t <= 0.0f || t >= 1.0f) return t;
            const float p = 0.3f, s = p / 4.0f, u = t - 1.0f;
            return -powf(2.0f, 10.0f * u) * sinf((u - s) * 6.2831853f / p);
        }
    }
    return t;
}
float KfEasing(int interp, float t){
    if (!KfIsEasing(interp)) return t;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const int base = KfInterpBase(interp);
    int ease = KfInterpEase(interp);
    if (ease == KfEaseAuto) ease = (base == KfBack || base == KfBounce || base == KfElastic) ? KfEaseOut : KfEaseIn;
    if (ease == KfEaseIn)  return EaseInF(base, t);
    if (ease == KfEaseOut) return 1.0f - EaseInF(base, 1.0f - t);
    return (t < 0.5f) ? EaseInF(base, 2.0f * t) * 0.5f : 1.0f - EaseInF(base, 2.0f - 2.0f * t) * 0.5f;
}

float AnimProperty::Eval(int frame, float def) const { return EvalF((float)frame, def); }

// el primer i >= 1 cuyo frame (en float) es >= 'frame', con Frame(0) < frame < Frame(n-1) (los dos clamps de
// EvalF ya pasaron): el tramo [i-1, i] que se evalua. La curva comun la RECORRE (como siempre: no supone que
// este ordenada). La COMPACTA (frames primero + paso * i, crecientes) llega al MISMO i sin recorrer: lo estima
// con una division y lo corrige con las mismas comparaciones en float (un frame enorme no es exacto en float).
// La IRREGULAR (frames estrictamente crecientes: su (float) no decrece) lo busca por biseccion con la misma
// comparacion (con NaN ninguna da true y queda en el primero, como el recorrido).
static size_t SegmentOf(const W3dKeys& K, float frame) {
    const size_t n = K.size();
    const W3dCompactKeys* c = K.CompactData();
    if (!c) {
        size_t i = 1;
        while (i < n && (float)K.Frame(i) < frame) i++;
        return i;
    }
    if (c->Irregular()) {
        const int* fr = c->Frames();
        size_t lo = 1, hi = n - 1;
        while (lo < hi) {
            const size_t m = lo + (hi - lo) / 2;
            if ((float)fr[m] < frame) lo = m + 1; else hi = m;
        }
        return lo;
    }
    size_t i = 1;
    if (frame == frame) {   // (NaN: la curva comun se queda en el primer tramo; aca tambien)
        const double x = ceil(((double)frame - (double)c->first) / (double)c->step);
        if (x > 1.0) i = (x >= (double)(n - 1)) ? n - 1 : (size_t)x;
    }
    while (i > 1 && (float)K.Frame(i - 1) >= frame) i--;
    while (i < n - 1 && (float)K.Frame(i) < frame) i++;
    return i;
}

// Una sola funcion para la curva comun y la COMPACTA (W3dKeys: Frame/Valor/Interp no expanden): despues de
// elegir el tramo, la cuenta es el mismo codigo, asi que las dos dan los mismos bits (prueba_claves_compactas.w3s).
// REPETIR: fuera del rango de los keyframes el frame se lleva ADENTRO (una vuelta = del primero al ultimo) y, con
// offset, se le suma lo que avanza la curva por vuelta. Adentro del rango no cambia nada.
float AnimProperty::EvalF(float frame, float def) const {
    if (!mods.empty()) return EvalMods(frame, def, (int)mods.size());
    return EvalNoMods(frame, def);
}

// ============================================================================
// MODIFICADORES DE CURVA (Animation.h)
// ============================================================================
KfModifier::KfModifier(int t)
    : type(t), active(true), useInfluence(false), influence(1.0f),
      useRange(false), startRange(1.0f), endRange(100.0f), mixIn(0.0f), mixOut(0.0f),
      prevMode(KfCyclesRepeats), afterMode(KfCyclesRepeats), prevCount(0.0f), afterCount(0.0f),
      mix(KfNoiseReplaces), scale(1.0f), force(1.0f), offset(0.0f), phase(1.0f), depth(0.0f),
      noiseOld(false), lacunaridad(2.0f), roughness(0.5f),
      step(2.0f), stepOffset(0.0f), useStart(false), useEnd(false), start(0.0f), end(0.0f),
      sigma(0.33f), width(6.0f) {}
void KfModifier::AFloats(float* f) const {
    float v[NFloats] = { (float)type, active ? 1.0f : 0.0f, useInfluence ? 1.0f : 0.0f, influence,
        useRange ? 1.0f : 0.0f, startRange, endRange, mixIn, mixOut,
        (float)prevMode, (float)afterMode, prevCount, afterCount,
        (float)mix, scale, force, offset, phase, depth, noiseOld ? 1.0f : 0.0f, lacunaridad, roughness,
        step, stepOffset, useStart ? 1.0f : 0.0f, useEnd ? 1.0f : 0.0f, start, end,
        sigma, width, 0.0f, 0.0f };
    for (int i = 0; i < NFloats; i++) f[i] = v[i];
}
void KfModifier::FromFloats(const float* f, int n) {
    *this = KfModifier((n > 0) ? (int)f[0] : KfModCycles);
    float v[NFloats]; AFloats(v);                      // los que no vienen quedan en su default
    for (int i = 0; i < n && i < NFloats; i++) v[i] = f[i];
    type = (int)v[0]; if (type < 0 || type >= KfModTypes) type = KfModCycles;
    active = v[1] != 0.0f; useInfluence = v[2] != 0.0f; influence = v[3];
    useRange = v[4] != 0.0f; startRange = v[5]; endRange = v[6]; mixIn = v[7]; mixOut = v[8];
    prevMode = (int)v[9]; afterMode = (int)v[10]; prevCount = v[11]; afterCount = v[12];
    if (prevMode < 0 || prevMode > KfCyclesMirror) prevMode = KfCyclesNo;
    if (afterMode < 0 || afterMode > KfCyclesMirror) afterMode = KfCyclesNo;
    mix   = (int)v[13]; if (mix < 0 || mix > KfNoiseMultiplies) mix = KfNoiseReplaces;
    scale = v[14]; force = v[15]; offset = v[16]; phase = v[17]; depth = v[18]; noiseOld = v[19] != 0.0f;
    lacunaridad = v[20]; roughness = v[21];
    step = v[22]; stepOffset = v[23]; useStart = v[24] != 0.0f; useEnd = v[25] != 0.0f; start = v[26]; end = v[27];
    sigma = v[28]; width = v[29];
}
bool KfModifier::operator==(const KfModifier& o) const {
    float a[NFloats], b[NFloats]; AFloats(a); o.AFloats(b);
    return memcmp(a, b, sizeof(a)) == 0;
}
void AnimProperty::RepeatToModifier() {
    if (cyclePrev == KfCycleNo && cycleAfter == KfCycleNo) return;
    KfModifier m(KfModCycles);
    // KfCycleNo/Repite/Offset = KfCyclesNo/Repite/Offset (mismos numeros, a proposito)
    m.prevMode = cyclePrev; m.afterMode = cycleAfter;
    mods.insert(mods.begin(), m);
    cyclePrev = cycleAfter = KfCycleNo;
}
const char* KfModName(int type) {
    switch (type) { case KfModNoise: return "Noise"; case KfModStep: return "Stepped"; case KfModSmooth: return "Smooth"; default: return "Cycles"; }
}

// cuanto actua el modificador en este frame: el rango (con sus rampas) y la influencia
static float KfModInfluence(const KfModifier& m, float t) {
    float inf = 1.0f;
    if (m.useRange) {
        if (t < m.startRange || t > m.endRange) return 0.0f;
        if (m.mixIn > 0.0f && t < m.startRange + m.mixIn) inf = (t - m.startRange) / m.mixIn;
        if (m.mixOut > 0.0f && t > m.endRange - m.mixOut) { const float o = (m.endRange - t) / m.mixOut; if (o < inf) inf = o; }
        if (inf < 0.0f) inf = 0.0f;
    }
    if (m.useInfluence) inf *= m.influence;
    return inf;
}
// RUIDO: ruido de gradiente 2D propio (determinista: el mismo en todas las plataformas), en [0..1]
static float KfNoiseHash(int x, int y) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
    return (float)(h & 0xFFFFu) / 65535.0f;
}
static float KfNoiseGrad(int x, int y, float dx, float dy) {
    const float a = KfNoiseHash(x, y) * 6.2831853f;
    return cosf(a) * dx + sinf(a) * dy;
}
static float KfNoise2(float x, float y) {
    const float fx = floorf(x), fy = floorf(y);
    const int ix = (int)fx, iy = (int)fy;
    const float dx = x - fx, dy = y - fy;
    const float ux = dx * dx * (3.0f - 2.0f * dx), uy = dy * dy * (3.0f - 2.0f * dy);
    const float a = KfNoiseGrad(ix, iy, dx, dy),         b = KfNoiseGrad(ix + 1, iy, dx - 1.0f, dy);
    const float c = KfNoiseGrad(ix, iy + 1, dx, dy - 1.0f), d = KfNoiseGrad(ix + 1, iy + 1, dx - 1.0f, dy - 1.0f);
    const float v = (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;   // ~[-0.7, 0.7]
    float r = 0.5f + v * 0.7f;
    if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    return r;
}
// turbulencia: la octava base + 'profundidad' octavas (cada una lacunaridad veces mas fina y rugosidad veces mas suave)
static float KfTurbulence(const KfModifier& m, float t) {
    const float esc = (fabsf(m.scale) > 1e-6f) ? m.scale : 1e-6f;
    const float lac = m.noiseOld ? 2.0f : m.lacunaridad, rug = m.noiseOld ? 0.5f : m.roughness;
    float x = (t - m.offset) / esc, y = m.phase;
    float s = KfNoise2(x, y), amp = 1.0f, div = 1.0f;
    int oct = (int)(m.depth + 0.5f); if (oct > 16) oct = 16;
    for (int i = 0; i < oct; i++) { amp *= rug; x *= lac; y = y * lac + 3.7f; s += amp * KfNoise2(x, y); div += amp; }
    return (div > 0.0f) ? s / div : s;
}
// CICLOS sobre el TIEMPO (fcm_cycles_time de Blender): el frame equivalente adentro de los keyframes. ofsV = cuanto
// hay que correr el valor (Repeat with Offset). false = este frame no lo toca (adentro del rango, o pasaron las vueltas)
static bool KfCyclesTime(const KfModifier& m, const W3dKeys& K, float& t, float& ofsV) {
    ofsV = 0.0f;
    const size_t n = K.size();
    if (n < 2) return false;
    const float f0 = (float)K.Frame(0), f1 = (float)K.Frame(n - 1);
    const float v0 = K.Value(0), v1 = K.Value(n - 1);
    int side = 0, mode = KfCyclesNo; float count = 0.0f, ofs = 0.0f;
    if (t < f0 && m.prevMode != KfCyclesNo) { side = -1; mode = m.prevMode; count = m.prevCount; ofs = f0; }
    else if (t > f1 && m.afterMode != KfCyclesNo) { side = 1; mode = m.afterMode; count = m.afterCount; ofs = f1; }
    if (side == 0) return false;
    const float dx = f1 - f0, dy = v1 - v0;
    if (dx <= 0.0f) return false;
    const float cycle = (float)side * (t - ofs) / dx;
    const float ct = fmodf(t - ofs, dx);
    if (count >= 1.0f && cycle > count) return false;      // pasaron las vueltas pedidas: queda la punta
    if (mode == KfCyclesOffset) ofsV = ((side < 0) ? floorf((t - ofs) / dx) : ceilf((t - ofs) / dx)) * dy;
    float nt;
    if (ct == 0.0f) {
        nt = (side == 1) ? f1 : f0;
        if (mode == KfCyclesMirror && ((int)cycle % 2)) nt = (side == 1) ? f0 : f1;
    } else if (mode == KfCyclesMirror && ((int)(cycle + 1.0f) % 2)) {
        nt = (side < 0) ? f0 - ct : f1 - ct;               // vuelta impar: al reves
    } else nt = f0 + ct;
    if (nt < f0) nt += dx;
    t = nt;
    return true;
}
float AnimProperty::EvalMods(float frame, float def, int until) const {
    if (until > (int)mods.size()) until = (int)mods.size();
    if (until <= 0) return EvalNoMods(frame, def);
    // 1) los TIEMPOS, del ultimo al primero
    float t = frame;
    float ofsCycles[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = until - 1; i >= 0; i--) {
        const KfModifier& m = mods[(size_t)i];
        if (!m.active) continue;
        const float inf = KfModInfluence(m, frame);
        if (inf <= 0.0f) continue;
        float nt = t;
        if (m.type == KfModCycles) {
            float ofs = 0.0f;
            if (!KfCyclesTime(m, keyframes, nt, ofs)) continue;
            if (i < 8) ofsCycles[i] = ofs;
        } else if (m.type == KfModStep) {
            if (m.useStart && t < m.start) continue;
            if (m.useEnd && t > m.end) continue;
            if (fabsf(m.step) < 1e-4f) continue;
            nt = floorf((t - m.stepOffset) / m.step) * m.step + m.stepOffset;
        } else continue;
        t = t + (nt - t) * inf;
    }
    float v = EvalNoMods(t, def);
    // 2) los VALORES, del primero al ultimo
    for (int i = 0; i < until; i++) {
        const KfModifier& m = mods[(size_t)i];
        if (!m.active) continue;
        const float inf = KfModInfluence(m, frame);
        if (inf <= 0.0f) continue;
        float nv = v;
        if (m.type == KfModCycles) { if (i < 8) nv = v + ofsCycles[i]; }
        else if (m.type == KfModNoise) {
            const float r = KfTurbulence(m, frame);
            switch (m.mix) {
                case KfNoiseSum:       nv     = v + r * m.force; break;
                case KfNoiseSubtract:      nv = v - r * m.force; break;
                case KfNoiseMultiplies: nv    = v * r * m.force; break;
                default:                nv    = v + (r - 0.5f) * m.force; break;
            }
        } else if (m.type == KfModSmooth) {
            // gaussiano de 'ancho' frames a cada lado, sobre la curva de ANTES de este modificador
            int w = (int)(m.width + 0.5f); if (w < 1) continue; if (w > 64) w = 64;
            const float sg = (m.sigma > 1e-3f) ? m.sigma : 1e-3f;
            float sum = 0.0f, weights = 0.0f;
            for (int k = -w; k <= w; k++) {
                const float u = (float)k / (float)w;
                const float p = expf(-(u * u) / (2.0f * sg * sg));
                sum += p * ((k == 0) ? v : EvalMods(frame + (float)k, def, i));
                weights += p;
            }
            if (weights > 0.0f) nv = sum / weights;
        }
        v = v + (nv - v) * inf;
    }
    return v;
}

float AnimProperty::EvalNoMods(float frame, float def) const {
    if (cyclePrev == KfCycleNo && cycleAfter == KfCycleNo) return EvalRange(frame, def);
    const W3dKeys& K = keyframes;
    const size_t n = K.size();
    if (n < 2) return EvalRange(frame, def);
    const float f0 = (float)K.Frame(0), f1 = (float)K.Frame(n-1), period = f1 - f0;
    if (period <= 0.0f) return EvalRange(frame, def);
    int mode = KfCycleNo;
    if (frame > f1) mode = cycleAfter; else if (frame < f0) mode = cyclePrev;
    if (mode == KfCycleNo) return EvalRange(frame, def);
    const float turns = floorf((frame - f0) / period);           // cuantas vueltas enteras (negativo antes)
    float local = frame - turns * period;                            // el frame equivalente adentro del rango
    if (local < f0) local = f0; if (local > f1) local = f1;          // (redondeo de float en frames enormes)
    float v = EvalRange(local, def);
    if (mode == KfCycleOffset) v += turns * (K.Value(n-1) - K.Value(0));
    return v;
}
float AnimProperty::EvalRange(float frame, float def) const {
    const W3dKeys& K = keyframes;
    const size_t n = K.size();
    if (n == 0) return def;                                         // canal sin keyframes -> su valor de reposo
    if (frame <= (float)K.Frame(0))     return K.Value(0);          // antes del 1ro: clamp
    if (frame >= (float)K.Frame(n-1))   return K.Value(n-1);        // despues del ultimo: clamp
    const size_t i = SegmentOf(K, frame);
    if (i >= n) return K.Value(n-1);                                // (curva desordenada: como siempre)
    const int interp = K.Interp(i-1);
    const int fa = K.Frame(i-1), fb = K.Frame(i);
    const float va = K.Value(i-1), vb = K.Value(i);
    // ESCALON: mantiene va hasta el proximo key... pero EN el frame de b ya vale vb. El tramo se elige con
    // frame de b >= frame, asi que aca 'frame' puede ser EXACTAMENTE fb: sin este chequeo el escalon caia un
    // frame tarde (y el editor lo dibuja en b, con lo cual lo que veias no era lo que corria).
    if (interp == KfConstant) return (frame >= (float)fb) ? vb : va;
    int span = fb - fa;
    if (span <= 0) return vb;
    float t = (frame - (float)fa) / (float)span;
    if (KfIsEasing(interp)) return va + (vb - va) * KfEasing(interp, t);   // easing: curva calculada
    if (interp != KfBezier)
        return va + (vb - va) * t;                                  // lineal
    return EvalBezier(i, frame);
}

// Valor del tramo BEZIER [i-1, i] en 'frame'. Los handles son PUNTOS, asi que el tramo es una bezier cubica en
// (frame, valor): x(t) e y(t). Para saber el valor en un frame hay que despejar el t cuyo x(t) es ese frame — no
// alcanza con meter t = fraccion del tramo, porque los handles corren el x. Es lo mismo que hace una curva de
// easing: el eje del tiempo tambien esta curvado.
float AnimProperty::EvalBezier(size_t i, float frame) const {
    const W3dKeys& K   = keyframes;   // (Frame/Valor: no expande una curva compacta)
    const float aValue = K.Value(i-1), bValue = K.Value(i);
    const float x0 = (float)K.Frame(i-1), x3 = (float)K.Frame(i);
    const float span = x3 - x0;
    if (span <= 0.0f) return bValue;
    // en las PUNTAS, el valor exacto del keyframe: con un handle pegado a la punta (x2 == x3) x(t) llega plana y
    // en float la biseccion se frena un poco antes (el keyframe daba 9.998 en vez de 10)
    if (frame >= x3) return bValue;
    if (frame <= x0) return aValue;

    float aDF, aDV, bDF, bDV;
    EffectiveHandle(i-1, true,  aDF, aDV);   // handle de SALIDA del izquierdo
    EffectiveHandle(i,   false, bDF, bDV);   // handle de ENTRADA del derecho
    float x1 = x0 + aDF, x2 = x3 + bDF;
    // x TIENE que ser monotono o la curva deja de ser una funcion del tiempo (se doblaria hacia atras y un frame
    // tendria dos valores). Los puntos de control se clampean al tramo: es lo mismo que hace cualquier editor.
    if (x1 < x0) x1 = x0; if (x1 > x3) x1 = x3;
    if (x2 < x0) x2 = x0; if (x2 > x3) x2 = x3;
    const float y0 = aValue, y1 = aValue + aDV, y2 = bValue + bDV, y3 = bValue;

    // despejar t / x(t) = frame. Con los handles a UN TERCIO del tramo en x (los automaticos y los vector: todo lo
    // horneado, una clave por frame) x(t) es una RECTA: t sale directo. Si no, por biseccion: x(t) es monotona
    // creciente (recien clampeada), asi que converge siempre; 24 pasos dan 1/16M del tramo, de sobra para un frame.
    // (La biseccion era lo mas caro de posar un esqueleto: 24 cubicas por curva por cuadro.)
    float t;
    if (aDF == span / 3.0f && bDF == -span / 3.0f) {
        t = (frame - x0) / span;
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    } else {
        float lo = 0.0f, hi = 1.0f;
        t = 0.5f;
        for (int it = 0; it < 24; it++){
            t = (lo + hi) * 0.5f;
            float u = 1.0f - t;
            float x = u*u*u*x0 + 3.0f*u*u*t*x1 + 3.0f*u*t*t*x2 + t*t*t*x3;
            if (x < frame) lo = t; else hi = t;
        }
    }
    float u = 1.0f - t;
    return u*u*u*y0 + 3.0f*u*u*t*y1 + 3.0f*u*t*t*y2 + t*t*t*y3;
}

// ============================================================================
//  BORRAR UN KEYFRAME MANTENIENDO LA FORMA de la curva ("simplificacion").
//  Al sacar B de A-B-C, el tramo A->C tiene que parecerse lo mas posible a lo que hacian A->B->C.
//  Se conservan las DIRECCIONES de los handles en A y en C (la curva sale y entra igual que antes) y se ajustan
//  sus LARGOS por MINIMOS CUADRADOS contra la curva original, muestreada frame a frame. Es el ajuste clasico de
//  bezier con tangentes fijas: quedan 2 incognitas (un largo por lado) y un sistema de 2x2.
//  Solo tiene sentido si el tramo es BEZIER: en lineal/constante no hay forma que mantener (borrar deja la recta).
// ============================================================================
// aDF/aDV y cDF/cDV son las direcciones de los handles TAL COMO ESTABAN ANTES de borrar. Se pasan de afuera y no
// se leen aca: con handleType automatico, EffectiveHandle las RECALCULA desde los vecinos, y despues del erase los
// vecinos ya son otros -> se obtenia la direccion NUEVA y el ajuste no hacia nada (quedaba igual que borrar crudo).
static void FitHandles(AnimProperty& ap, size_t iA, size_t iC, const std::vector<float>& fs,
                       const std::vector<float>& vs,
                       float aDF, float aDV, float cDF, float cDV){
    keyFrame& A = ap.keyframes[iA];
    keyFrame& C = ap.keyframes[iC];
    const float x0 = (float)A.frame, y0 = A.value, x3 = (float)C.frame, y3 = C.value;
    const float span = x3 - x0;
    if (span <= 0.0f || fs.size() < 2) return;

    float l0 = sqrtf(aDF*aDF + aDV*aDV), l1 = sqrtf(cDF*cDF + cDV*cDV);
    if (l0 < 1e-9f || l1 < 1e-9f) return;
    const float d0x = aDF/l0, d0y = aDV/l0;   // sale de A hacia adelante
    const float d1x = cDF/l1, d1y = cDV/l1;   // entra a C desde atras (dF < 0)

    // minimos cuadrados sobre las muestras. u = fraccion del tramo (parametrizacion uniforme: alcanza porque los
    // extremos y las tangentes ya estan fijos, y lo que se ajusta es cuanto "panzea").
    double c00=0, c01=0, c11=0, r0=0, r1=0;
    for (size_t k = 0; k < fs.size(); k++){
        float u = (fs[k] - x0) / span;
        if (u <= 0.0f || u >= 1.0f) continue;
        float t = 1.0f - u;
        float B0 = t*t*t, B1 = 3.0f*t*t*u, B2 = 3.0f*t*u*u, B3 = u*u*u;
        // lo que falta cubrir con los handles (el resto lo ponen los extremos)
        float rx = (fs[k] - (B0+B1)*x0 - (B2+B3)*x3);
        float ry = (vs[k]  - (B0+B1)*y0 - (B2+B3)*y3);
        float a1x = B1*d0x, a1y = B1*d0y;     // aporte del handle de A (por unidad de largo)
        float a2x = B2*d1x, a2y = B2*d1y;     // aporte del handle de C
        c00 += a1x*a1x + a1y*a1y;
        c01 += a1x*a2x + a1y*a2y;
        c11 += a2x*a2x + a2y*a2y;
        r0  += a1x*rx + a1y*ry;
        r1  += a2x*rx + a2y*ry;
    }
    double det = c00*c11 - c01*c01;
    float n0, n1;
    if (det > 1e-12 || det < -1e-12){
        n0 = (float)((r0*c11 - r1*c01) / det);
        n1 = (float)((c00*r1 - c01*r0) / det);
    } else {
        n0 = n1 = span / 3.0f;                // degenerado: el largo por defecto
    }
    // los handles no pueden cruzar al otro lado ni salirse del tramo: si lo hicieran, x dejaria de ser monotono y
    // un frame tendria dos valores
    const float maxL = span;
    if (n0 < 0.0f) n0 = 0.0f; if (n0 > maxL) n0 = maxL;
    if (n1 < 0.0f) n1 = 0.0f; if (n1 > maxL) n1 = maxL;

    A.handleType = HFree; C.handleType = HFree;   // los largos son propios: ya no los puede recalcular nadie
    A.outDF = d0x*n0; A.outDV = d0y*n0;
    C.inDF  = d1x*n1; C.inDV  = d1y*n1;
    A.Interpolation = KfBezier;                   // el tramo A->C es el que queda
}

void DeleteKeyframeKeepingShape(AnimProperty& ap, int frame){
    size_t n = ap.keyframes.size();
    size_t i = n;
    // (Frame(k): una curva compacta que no tiene clave en 'frame' no se expande para nada)
    for (size_t k = 0; k < n; k++) if (ap.keyframes.Frame(k) == frame){ i = k; break; }
    if (i >= n) return;
    // sin vecinos de los dos lados, o el tramo no es bezier -> borrado comun (no hay forma que mantener)
    bool bez = (i > 0 && i + 1 < n) &&
               (ap.keyframes[i-1].Interpolation == KfBezier || ap.keyframes[i].Interpolation == KfBezier);
    if (!bez){ ap.keyframes.erase(ap.keyframes.begin() + i); return; }
    // las direcciones de los handles que se conservan: se capturan ANTES de borrar (despues, los automaticos se
    // recalculan desde los vecinos nuevos y ya no son las de la curva original)
    float aDF, aDV, cDF, cDV;
    ap.EffectiveHandle(i-1, true,  aDF, aDV);   // como SALE de A
    ap.EffectiveHandle(i+1, false, cDF, cDV);   // como ENTRA a C
    // muestrear la curva ORIGINAL entre los vecinos, frame a frame (que es lo que la animacion pisa)
    int fA = ap.keyframes[i-1].frame, fC = ap.keyframes[i+1].frame;
    std::vector<float> fs, vs;
    for (int f = fA; f <= fC; f++){ fs.push_back((float)f); vs.push_back(ap.EvalF((float)f, 0.0f)); }
    ap.keyframes.erase(ap.keyframes.begin() + i);
    FitHandles(ap, i-1, i, fs, vs, aDF, aDV, cDF, cDV);   // tras el erase, C quedo en el indice i
}

// las 3 curvas (X/Y/Z) de una propiedad: cada componente se evalua POR SEPARADO (puede tener sus propios frames)
Vector3 EvalPropVec(const std::vector<AnimProperty>& props, int property, int frame, const Vector3& def) {
    Vector3 r = def;
    for (size_t p = 0; p < props.size(); p++) {
        if (props[p].Property != property) continue;
        float d = (props[p].component == AnimX) ? def.x : (props[p].component == AnimY) ? def.y : def.z;
        float v = props[p].Eval(frame, d);
        if      (props[p].component == AnimX) r.x = v;
        else if (props[p].component == AnimY) r.y = v;
        else                                  r.z = v;
    }
    return r;
}

Vector3 EvalPropVecF(const std::vector<AnimProperty>& props, int property, float frame, const Vector3& def) {
    Vector3 r = def;
    for (size_t p = 0; p < props.size(); p++) {
        if (props[p].Property != property) continue;
        float d = (props[p].component == AnimX) ? def.x : (props[p].component == AnimY) ? def.y : def.z;
        float v = props[p].EvalF(frame, d);
        if      (props[p].component == AnimX) r.x = v;
        else if (props[p].component == AnimY) r.y = v;
        else                                  r.z = v;
    }
    return r;
}

AnimProperty& ListProperty(std::vector<AnimProperty>& props, int property, int component) {
    for (size_t i = 0; i < props.size(); i++)
        if (props[i].Property == property && props[i].component == component) return props[i];
    AnimProperty p; p.Property = property; p.component = component;
    props.push_back(p);
    return props.back();
}

void SetKeyCurve(AnimProperty& ap, int frame, float value, int typeH) {
    for (size_t i = 0; i < ap.keyframes.size(); i++)
        if (ap.keyframes[i].frame == frame) { ap.keyframes[i].value = value; return; } // ya hay uno: se actualiza
    keyFrame kf; kf.frame = frame; kf.value = value; kf.Interpolation = KfBezier; kf.handleType = typeH; // los canales true/false lo pisan a KfConstant
    size_t pos = 0; while (pos < ap.keyframes.size() && ap.keyframes[pos].frame < frame) pos++;
    ap.keyframes.insert(ap.keyframes.begin() + pos, kf);   // insertado EN ORDEN (no hace falta ordenar)
}

void KfEaseBezier(AnimProperty& ap, size_t i, int ease) {
    if (i + 1 >= ap.keyframes.size() || KfInterpBase(ap.keyframes[i].Interpolation) != KfBezier) return;
    if (ease != KfEaseAuto) {
        for (size_t j = i; j <= i + 1; j++) {
            const int t = ap.keyframes[j].handleType;
            if (t == HFree || t == HAligned) continue;
            float idf, idv, odf, odv;
            ap.EffectiveHandle(j, false, idf, idv); ap.EffectiveHandle(j, true, odf, odv);
            keyFrame& k = ap.keyframes[j];
            k.inDF = idf; k.inDV = idv; k.outDF = odf; k.outDV = odv; k.handleType = HFree;
        }
    }
    KfEaseBezierSegment(ap.keyframes[i], ap.keyframes[i + 1], ap.keyframes[i].value, ap.keyframes[i + 1].value, ease);
}

// La bezier (0,0) (x1,y1) (x2,y2) (1,1) que mas se parece a cada curva de easing, en fracciones del tramo (x =
// tiempo, y = camino de A a B). Una fila por curva desde KfSine: primero Ease In, despues Ease In and Out (simetrica:
// el segundo handle es el primero dado vuelta); el Ease Out es el Ease In dado vuelta (punto por punto: (1-x, 1-y),
// y los dos handles cambian de lado). Ajustadas minimizando el error MAXIMO contra KfEasing (con el mismo despeje
// de x que EvalBezier): el peor caso es Circ In and Out, 1.9% del tramo; Cubic In y Back In son exactas.
static const float kEaseBez[][2][4] = {
    { { 0.1987f, 0.0182f, 0.4107f,  0.0462f }, { 0.3628f, -0.0016f, 0.6372f, 1.0016f } },   // Sine
    { { 0.1706f, 0.0022f, 0.5361f,  0.0701f }, { 0.4843f,  0.0420f, 0.5157f, 0.9580f } },   // Quad
    { { 0.3333f, 0.0000f, 0.6667f,  0.0000f }, { 0.6249f, -0.0405f, 0.3751f, 1.0405f } },   // Cubic (t^3 ES una cubica)
    { { 0.4244f, 0.0074f, 0.7302f, -0.0815f }, { 0.7145f, -0.0895f, 0.2855f, 1.0895f } },   // Quart
    { { 0.5399f, 0.0043f, 0.7785f, -0.0916f }, { 0.7742f, -0.1232f, 0.2258f, 1.1232f } },   // Quint
    { { 0.6455f, 0.0145f, 0.8467f, -0.0457f }, { 0.8424f, -0.1194f, 0.1576f, 1.1194f } },   // Expo
    { { 0.5536f, 0.0002f, 0.9995f,  0.4489f }, { 0.9384f,  0.2088f, 0.0616f, 0.7912f } },   // Circ
    { { 0.3333f, 0.0000f, 0.6667f, -0.5671f }, { 0.7205f, -0.3339f, 0.2795f, 1.3339f } },   // Back (el retroceso SI entra)
};
bool KfBezierFromEasing(AnimProperty& ap, size_t i, int oldInterp) {
    if (i + 1 >= ap.keyframes.size() || !KfIsEasing(oldInterp)) return false;
    const int base = KfInterpBase(oldInterp);
    if (base > KfBack) return false;                          // Bounce / Elastic: no hay una bezier que se parezca
    int ease = KfInterpEase(oldInterp);
    if (ease == KfEaseAuto) ease = (base == KfBack) ? KfEaseOut : KfEaseIn;   // (el mismo default que KfEasing)
    const float* e = kEaseBez[base - KfSine][ease == KfEaseInOut ? 1 : 0];
    float x1 = e[0], y1 = e[1], x2 = e[2], y2 = e[3];
    if (ease == KfEaseOut) { x1 = 1.0f - e[2]; y1 = 1.0f - e[3]; x2 = 1.0f - e[0]; y2 = 1.0f - e[1]; }
    // los OTROS lados de los dos keyframes (los tramos vecinos) se congelan como se veian
    for (size_t j = i; j <= i + 1; j++) {
        const int t = ap.keyframes[j].handleType;
        if (t == HFree || t == HAligned) continue;
        float idf, idv, odf, odv;
        ap.EffectiveHandle(j, false, idf, idv); ap.EffectiveHandle(j, true, odf, odv);
        keyFrame& k = ap.keyframes[j];
        k.inDF = idf; k.inDV = idv; k.outDF = odf; k.outDV = odv;
    }
    keyFrame& a = ap.keyframes[i]; keyFrame& b = ap.keyframes[i + 1];
    const float span = (float)(b.frame - a.frame), dv = b.value - a.value;
    a.outDF = x1 * span;          a.outDV = y1 * dv;
    b.inDF  = (x2 - 1.0f) * span; b.inDV  = (y2 - 1.0f) * dv;
    a.handleType = HFree; b.handleType = HFree;
    return true;
}

// AnimationObject
void AnimationObject::UpdateFirstLastFrame() {
    FirstKeyFrame = 100000000;
    LastKeyFrame = 0;
    for(size_t pr = 0; pr < Properties.size(); pr++) {
        const W3dKeys& k = Properties[pr].keyframes;   // (Frame(i): mirar no expande una curva compacta)
        for(size_t kf = 0; kf < k.size(); kf++) {
            const int f = k.Frame(kf);
            if (f > LastKeyFrame)  LastKeyFrame = f;
            if (f < FirstKeyFrame) FirstKeyFrame = f;
        }
    }
}

// Vector global de objetos animados
std::vector<AnimationObject> AnimationObjects;

// ============================================================================
//  REPRODUCTOR de animaciones de ESCENA para el MODO JUEGO (ver Animation.h).
//  El editor solo evalua estas curvas con kind 0; un juego (kind 2) las dispara
//  desde Lua con animEscena() y este tick las avanza y APLICA. Se aplican los
//  canales de OBJETO (pos/rot/escala/visible); los de camara/luz son del editor.
// ============================================================================

// las curvas de la escena i: la ACTIVA vive en el global AnimationObjects
// (invariante del swap de SetActiveScene; misma regla que el guardado)
static std::vector<AnimationObject>* SceneObjects(int i) {
    if (i < 0 || i >= (int)SceneAnimations.size() || !SceneAnimations[i]) return NULL;
    return (i == ActiveSceneAnim) ? &AnimationObjects : &SceneAnimations[i]->objects;
}

static int   gEscGame     = -1;     // indice sonando (-1 = nada)
static float gEscGameF    = 0.0f;   // frame FLOTANTE (avanza por dt * fps)
static bool  gEscGameLoop = true;
static bool  gEscGameManual = false;  // el frame lo fija el script (animEscenaFrame): el tick no avanza su reloj

int W3dAnimSceneIdx(const char* name) {
    if (!name) return -1;
    // (los CLIPS DE OBJETO no: esos se piden por su dueno, animObjeto)
    for (size_t i = 0; i < SceneAnimations.size(); i++)
        if (SceneAnimations[i] && !SceneAnimations[i]->isClip && SceneAnimations[i]->name == name) return (int)i;
    return -1;
}

float W3dAnimSceneDur(int idx) {
    if (idx < 0 || idx >= (int)SceneAnimations.size() || !SceneAnimations[idx]) return 0.0f;
    const SceneAnimation* e = SceneAnimations[idx];
    int fps = (e->fps > 0) ? e->fps : 30;
    int n = e->endFrame - e->startFrame + 1;
    return (n > 0 ? (float)n : 1.0f) / (float)fps;
}

bool W3dAnimScenePlay(int idx, bool loop) {
    if (!SceneObjects(idx)) return false;
    gEscGame = idx;
    gEscGameF = (float)SceneAnimations[idx]->startFrame;
    gEscGameLoop = loop;
    gEscGameManual = false;
    return true;
}

void W3dAnimSceneReset() { gEscGame = -1; gEscGameManual = false; }

void W3dAnimSceneSaveState(W3dAnimSceneState* e) {
    if (!e) return;
    e->idx = gEscGame; e->frame = gEscGameF; e->loop = gEscGameLoop; e->manual = gEscGameManual;
}
void W3dAnimSceneRestoreState(const W3dAnimSceneState& e) {
    // (la lista de animaciones es la de la raiz que vuelve: el indice se valida contra ELLA)
    gEscGame = (e.idx >= 0 && e.idx < (int)SceneAnimations.size()) ? e.idx : -1;
    gEscGameF = e.frame; gEscGameLoop = e.loop; gEscGameManual = e.manual;
}
bool W3dAnimSceneEnded() {
    if (gEscGame < 0 || gEscGame >= (int)SceneAnimations.size() || !SceneAnimations[gEscGame]) return true;
    return !gEscGameLoop && gEscGameF >= (float)SceneAnimations[gEscGame]->endFrame;
}

void W3dAnimSceneSet(float frame) {
    if (gEscGame < 0) return;
    gEscGameF = frame;
    gEscGameManual = true;
}

// ---- SHAPE KEYS animadas: canal AnimShape, componente = indice de la forma ----
void (*g_animStepHook)(Object* routine, int component, float value) = 0;
void (*g_animSoundHook)(Object* sound, int component, float value) = 0;
void (*g_animBackgroundHook)(int property, int component, float value) = 0;
void W3dApplyShapes(Object* o, std::vector<AnimProperty>& props, int frame) {
    if (!o) return;
    // el FONDO del render (color, rotacion de la textura 360, que fondo): sus curvas viven en la coleccion de la escena
    if (g_animBackgroundHook)
        for (size_t p = 0; p < props.size(); p++) {
            const int P = props[p].Property;
            if ((P == AnimBackground || P == AnimBackgroundRot || P == AnimBackgroundTex) && !props[p].keyframes.empty())
                g_animBackgroundHook(P, props[p].component, props[p].Eval(frame, 0.0f));
        }
    if (o->getType() == ObjectType::sound) {   // el VOLUMEN / el tono / la reverb de un sonido (AnimSound)
        if (g_animSoundHook)
            for (size_t p = 0; p < props.size(); p++)
                if (props[p].Property == AnimSound && !props[p].keyframes.empty())
                    g_animSoundHook(o, props[p].component, props[p].Eval(frame, 0.0f));
        return;
    }
    if (o->getType() == ObjectType::routine) {   // los PASOS de una rutina (AnimStep)
        if (g_animStepHook)
            for (size_t p = 0; p < props.size(); p++)
                if (props[p].Property == AnimStep && !props[p].keyframes.empty())
                    g_animStepHook(o, props[p].component, props[p].Eval(frame, 0.0f));
        return;
    }
    if (o->getType() != ObjectType::mesh) return;
    Mesh* m = (Mesh*)o;
    for (size_t p = 0; p < props.size(); p++)
        if (props[p].Property == AnimShape && !props[p].keyframes.empty())
            m->SetShapeWeight(props[p].component, props[p].Eval(frame, 0.0f));
}

// ---- PISTA "CAMARA ACTIVA" ----
void (*g_activeCameraHook)(const char* name) = 0;
void (*g_animFovHook)(Object* camera, float fov) = 0;

static SceneAnimation* SceneIdx(int i) {
    return (i >= 0 && i < (int)SceneAnimations.size()) ? SceneAnimations[i] : NULL;
}

AnimProperty* W3dCamTrack(int scene, bool create) {
    SceneAnimation* e = SceneIdx(scene);
    if (!e) return NULL;
    if (e->camTrack.empty()) {
        if (!create) return NULL;
        AnimProperty ap; ap.Property = AnimCamActive; ap.component = AnimX;
        e->camTrack.push_back(ap);
    }
    return &e->camTrack[0];
}

int W3dCamNameIdx(int scene, const std::string& name) {
    SceneAnimation* e = SceneIdx(scene);
    if (!e) return -1;
    for (size_t i = 0; i < e->camNames.size(); i++) if (e->camNames[i] == name) return (int)i;
    e->camNames.push_back(name);
    return (int)e->camNames.size() - 1;
}

void W3dCamKey(int scene, int frame, const std::string& name) {
    AnimProperty* ap = W3dCamTrack(scene, true);
    if (!ap) return;
    SetKeyCurve(*ap, frame, (float)W3dCamNameIdx(scene, name));
    for (size_t i = 0; i < ap->keyframes.size(); i++) ap->keyframes[i].Interpolation = KfConstant;   // cortes
}

const char* W3dCamInFrame(int scene, int frame) {
    SceneAnimation* e = SceneIdx(scene);
    if (!e || e->camTrack.empty() || e->camTrack[0].keyframes.empty()) return NULL;
    const W3dKeys& k = e->camTrack[0].keyframes;   // (Frame/Valor: el juego no expande la pista)
    size_t j = 0;
    for (size_t i = 0; i < k.size(); i++) { if (k.Frame(i) <= frame) j = i; else break; }
    const int idx = (int)(k.Value(j) + 0.5f);
    return (idx >= 0 && idx < (int)e->camNames.size()) ? e->camNames[idx].c_str() : NULL;
}

void W3dActiveCamApply(int scene, int frame) {
    if (!g_activeCameraHook) return;
    const char* n = W3dCamInFrame(scene, frame);
    if (n) g_activeCameraHook(n);
}

// ============================================================================
//  LAS PISTAS DE MATERIAL (ver AnimationMaterial en Animation.h y formato/material-animado.md)
// ============================================================================
// el campo del material que mueve un canal (NULL = ese canal / componente no existe en el material)
static float* MatChannelPtr(Material* m, int prop, int comp) {
    if (!m) return NULL;
    const bool rgb = comp >= AnimX && comp <= AnimZ, one = comp == AnimX;
    switch (prop) {
        case AnimMatColor:        return rgb ? &m->diffuse[comp] : NULL;
        case AnimMatAlpha:        return one ? &m->diffuse[3] : NULL;
        case AnimMatEmission:     return rgb ? &m->emission[comp] : NULL;
        case AnimMatEmitStrength: return one ? &m->emissionStrength : NULL;
        case AnimMatSpecular:     return rgb ? &m->specular[comp] : NULL;
        case AnimMatRoughness:    return one ? &m->rtRoughness : NULL;
        case AnimMatMetallic:     return one ? &m->rtMetallic : NULL;
        case AnimMatTransmission: return one ? &m->transmission : NULL;
        // (la capa que no esta -una dormida que todavia no desperto, una que se borro- no se toca)
        case AnimMatLayerForce:   return (comp >= 0 && comp < (int)m->layers.size()) ? &m->layers[(size_t)comp].force : NULL;
        default:                  return NULL;
    }
}
bool W3dIsMatChannel(int prop) { return prop >= AnimMatColor && prop <= AnimMatLayerForce; }
float W3dMatChannelGet(const Material* m, int prop, int comp) {
    const float* f = MatChannelPtr(const_cast<Material*>(m), prop, comp);
    return f ? *f : 0.0f;
}
void W3dMatChannelSet(Material* m, int prop, int comp, float v) {
    float* f = MatChannelPtr(m, prop, comp);
    if (f) *f = v;
}
int W3dMatChannelComps(int prop, int comp, int* out) {
    if (prop == AnimMatColor || prop == AnimMatEmission || prop == AnimMatSpecular) {
        out[0] = AnimX; out[1] = AnimY; out[2] = AnimZ; return 3;
    }
    out[0] = comp; return 1;
}
const char* W3dMatChannelBase(int prop) {
    switch (prop) {
        case AnimMatColor:        return "Base Color";
        case AnimMatAlpha:        return "Alpha";
        case AnimMatEmission:     return "Emission";
        case AnimMatEmitStrength: return "Emission Strength";
        case AnimMatSpecular:     return "Specular";
        case AnimMatRoughness:    return "Roughness";
        case AnimMatMetallic:     return "Metallic";
        case AnimMatTransmission: return "Transmission";
        case AnimMatLayerForce:   return "Layer";
        default:                  return NULL;
    }
}
const char* W3dMatChannelSuffix(int prop, int comp) {
    static const char* const kRGB[3] = { "R", "G", "B" };
    if ((prop == AnimMatColor || prop == AnimMatEmission || prop == AnimMatSpecular) && comp >= 0 && comp < 3) return kRGB[comp];
    return "";
}

AnimationMaterial* W3dMatTrack(int scene, Material* m, bool create) {
    SceneAnimation* e = SceneIdx(scene);
    if (!e || !m) return NULL;
    for (size_t i = 0; i < e->materials.size(); i++) if (e->materials[i].mat == m) return &e->materials[i];
    if (!create) return NULL;
    AnimationMaterial am; am.mat = m;
    e->materials.push_back(am);
    return &e->materials.back();
}

bool W3dApplyMaterialCurves(Material* m, const std::vector<AnimProperty>& props, float frame) {
    if (!m) return false;
    bool wrote = false;
    for (size_t p = 0; p < props.size(); p++) {
        const AnimProperty& ap = props[p];
        if (ap.keyframes.empty()) continue;
        float* f = MatChannelPtr(m, ap.Property, ap.component);
        if (!f) continue;
        *f = ap.EvalF(frame, *f);
        wrote = true;
    }
    // (lo lee el reposo del guardado, como Object::posedByCurves: solo se devuelve lo que el cabezal poso)
    if (wrote) m->posedByCurves = true;
    return wrote;
}

void W3dApplyMaterialTracks(int scene, float frame) {
    SceneAnimation* e = SceneIdx(scene);
    if (!e || e->materials.empty()) return;   // (lo comun: ni un material animado, no cuesta nada)
    for (size_t i = 0; i < e->materials.size(); i++)
        W3dApplyMaterialCurves(e->materials[i].mat, e->materials[i].Properties, frame);
}

// ---- EL REPOSO de los materiales posados (guardar sin hornear el cabezal) ----
void W3dMatRest::Read(Material* mt) {
    m = mt;
    for (int k = 0; k < 4; k++) dif[k] = mt->diffuse[k];
    for (int k = 0; k < 3; k++) { emi[k] = mt->emission[k]; spe[k] = mt->specular[k]; }
    emiK = mt->emissionStrength; rug = mt->rtRoughness; met = mt->rtMetallic; trans = mt->transmission;
    layers.resize(mt->layers.size());
    for (size_t c = 0; c < mt->layers.size(); c++) layers[c] = mt->layers[c].force;
}
void W3dMatRest::Write() const {
    if (!m) return;
    for (int k = 0; k < 4; k++) m->diffuse[k] = dif[k];
    for (int k = 0; k < 3; k++) { m->emission[k] = emi[k]; m->specular[k] = spe[k]; }
    m->emissionStrength = emiK; m->rtRoughness = rug; m->rtMetallic = met; m->transmission = trans;
    for (size_t c = 0; c < layers.size() && c < m->layers.size(); c++) m->layers[c].force = layers[c];
}
void W3dMaterialsRestBegin(std::vector<W3dMatRest>& saved, const Material* only) {
    saved.clear();
    // primero las animaciones de escena que NO son la activa y la activa al final: si dos animan el mismo canal, gana la
    // que el cabezal esta aplicando (el mismo orden que RestAnimObjects con los objetos)
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < SceneAnimations.size(); i++) {
            if (((int)i == ActiveSceneAnim) != (pass == 1)) continue;
            SceneAnimation* e = SceneAnimations[i];
            if (!e || e->isClip) continue;
            for (size_t t = 0; t < e->materials.size(); t++) {
                AnimationMaterial& am = e->materials[t];
                Material* m = am.mat;
                if (!m || (only && m != only) || !m->posedByCurves) continue;
                bool has = false;
                for (size_t p = 0; p < am.Properties.size() && !has; p++) if (!am.Properties[p].keyframes.empty()) has = true;
                if (!has) continue;   // (sin keyframes no hay pose que deshacer)
                bool seen = false;
                for (size_t k = 0; k < saved.size() && !seen; k++) seen = (saved[k].m == m);
                if (!seen) { W3dMatRest r; r.Read(m); saved.push_back(r); }
                W3dApplyMaterialCurves(m, am.Properties, (float)e->startFrame);
            }
        }
}
void W3dMaterialsRestEnd(std::vector<W3dMatRest>& saved) {
    for (size_t i = saved.size(); i-- > 0; ) saved[i].Write();   // (al reves: gana lo que habia en la pantalla)
    saved.clear();
}

// los canales de OBJETO de una lista de curvas, en 'frame' (la parte que el
// juego necesita de W3dApplyCurvesInFrame del editor)
// 'fr' CONTINUO (con fraccion): transformacion y fov se interpolan entre frames (la camara de una cinematica se mueve
// suave igual que los cuerpos, ver EvaluatePoseSkeleton); visibilidad y formas van por el frame entero.
// 'transform' = false: solo los canales que NO se mezclan (visible/render, formas, fov, luz): lo que una capa
// de escena a peso entero aplica ademas de su transformacion mezclada (W3dMixScenesApply)
// 'transform': mascara de los canales de transformacion que se aplican (kCanal*): el reproductor de clips
// de jerarquia con retarget "rotaciones" deja a los descendientes solo con su rotacion
enum { kChannelPos = 1, kChannelRot = 2, kChannelEsc = 4, kChannelAll = 7 };
static void ApplyCurvesGameChannels(Object* o, std::vector<AnimProperty>& props, float fr, int transform) {
    const int frame = (int)fr;
    bool hasP = false, hasR = false, hasS = false, hasV = false, hasRnd = false;
    bool hasCol = false, hasAmb = false, hasSpec = false, hasAtt = false, hasSpot = false;
    for (size_t p = 0; p < props.size(); p++) {
        if (props[p].keyframes.empty()) continue;
        int P = props[p].Property;
        if      (P == AnimPosition) hasP = true;
        else if (P == AnimRotation) hasR = true;
        else if (P == AnimScale)    hasS = true;
        else if (P == AnimVisible)  hasV = true;
        else if (P == AnimRender)   hasRnd = true;
        else if (P == AnimColor)    hasCol = true;
        else if (P == AnimAmbient)  hasAmb = true;
        else if (P == AnimSpecular) hasSpec = true;
        else if (P == AnimAtten)    hasAtt = true;
        else if (P == AnimSpot)     hasSpot = true;
    }
    // la VISIBILIDAD primero: un nodo que su clip apaga no se dibuja, asi que no se evalua lo demas (los pedazos de una
    // explosion: de cientos, se ven unos pocos a la vez). Se pone al dia el cuadro en que vuelve a prenderse
    if (hasV) {
        o->visible = ListProperty(props, AnimVisible, AnimX).Eval(frame, o->visible ? 1.0f : 0.0f) >= 0.5f;
        // (solo mallas y vacios: una camara o una luz ocultas pueden seguir en uso)
        if (!o->visible && (o->getType() == ObjectType::mesh || o->getType() == ObjectType::empty)) return;
    }
    if ((hasP && (transform & kChannelPos)) || (hasS && (transform & kChannelEsc)) || (hasR && (transform & kChannelRot)) || hasRnd)
        o->posedByClip = true;   // (lo que escribe un clip no es un cambio del usuario: ver Object::posedByClip)
    if (hasP && (transform & kChannelPos)) o->pos   = EvalPropVecF(props, AnimPosition, fr, o->pos);
    if (hasS && (transform & kChannelEsc)) o->scale = EvalPropVecF(props, AnimScale,    fr, o->scale);
    if (hasR && (transform & kChannelRot)) { Vector3 e = EvalPropVecF(props, AnimRotation, fr, o->rotEuler); o->SetRotEuler(e); }
    if (hasRnd) o->renderable = ListProperty(props, AnimRender,  AnimX).Eval(frame, o->renderable ? 1.0f : 0.0f) >= 0.5f;
    // (ver W3dApplyShapes)
    W3dApplyShapes(o, props, frame);
    // FOV de camara (cinematicas: el zoom de cada corte). El Core no conoce Camera: lo aplica el hook de main/
    if (g_animFovHook)
        for (size_t p = 0; p < props.size(); p++)
            if (props[p].Property == AnimFov && !props[p].keyframes.empty()) {
                g_animFovHook(o, props[p].EvalF(fr, 45.0f));
                break;
            }
    // LUZ: los colores, la atenuacion y el spot (el cambio de GL_LIGHTn / direccional es del editor)
    if (o->getType() == ObjectType::light)
        for (size_t p = 0; p < props.size(); p++)
            if (props[p].Property == AnimLightIntens && !props[p].keyframes.empty()) { ((Light*)o)->intensity = props[p].EvalF(fr, 1.0f); break; }
    if (o->getType() == ObjectType::light && (hasCol || hasAmb || hasSpec || hasAtt || hasSpot)) {
        Light* l = (Light*)o;
        if (hasCol)  { Vector3 d(l->diffuse[0], l->diffuse[1], l->diffuse[2]);
                       Vector3 v = EvalPropVecF(props, AnimColor, fr, d);    l->diffuse[0] = v.x; l->diffuse[1] = v.y; l->diffuse[2] = v.z; }
        if (hasAmb)  { Vector3 d(l->ambient[0], l->ambient[1], l->ambient[2]);
                       Vector3 v = EvalPropVecF(props, AnimAmbient, fr, d);  l->ambient[0] = v.x; l->ambient[1] = v.y; l->ambient[2] = v.z; }
        if (hasSpec) { Vector3 d(l->specular[0], l->specular[1], l->specular[2]);
                       Vector3 v = EvalPropVecF(props, AnimSpecular, fr, d); l->specular[0] = v.x; l->specular[1] = v.y; l->specular[2] = v.z; }
        if (hasAtt)  { l->attConstant  = ListProperty(props, AnimAtten, AnimX).EvalF(fr, l->attConstant);
                       l->attLinear    = ListProperty(props, AnimAtten, AnimY).EvalF(fr, l->attLinear);
                       l->attQuadratic = ListProperty(props, AnimAtten, AnimZ).EvalF(fr, l->attQuadratic); }
        if (hasSpot) { l->spotCutoff   = ListProperty(props, AnimSpot, AnimX).EvalF(fr, l->spotCutoff);
                       l->spotExponent = ListProperty(props, AnimSpot, AnimY).EvalF(fr, l->spotExponent); }
    }
}
void W3dApplyCurvesGame(Object* o, std::vector<AnimProperty>& props, float fr) {
    if (o) ApplyCurvesGameChannels(o, props, fr, kChannelAll);
}
static void ApplyCurvesGame(Object* o, std::vector<AnimProperty>& props, float fr) {
    ApplyCurvesGameChannels(o, props, fr, kChannelAll);
}

// ---- LOS CABEZALES EN LOOP (ver W3dCycleWrap en Animation.h) ----
static bool FrameRare(float f) { return f != f || f > 3.0e38f || f < -3.0e38f; }   // NaN o infinito
float W3dCycleWrap(float f, float period) {
    if (!(period > 0.0f) || FrameRare(f)) return 0.0f;
    if (f >= period * 64.0f) f = (float)fmod((double)f, (double)period);   // (un salto enorme: de una vez)
    while (f >= period) f -= period;   // (las pocas vueltas de siempre: lo mismo que antes)
    return f;
}
// un cabezal de [ini, fin] (largo = fin - ini + 1) que se paso por ARRIBA: las vueltas que hagan falta y, si
// cae antes del inicio, el inicio (la regla de siempre de los reproductores). Infinito o NaN = el inicio.
static float WrapUp(float f, float start, float end, float length) {
    if (FrameRare(f)) return start;
    if ((double)f - (double)end > (double)length * 64.0)
        f = (float)((double)end + fmod((double)f - (double)end, (double)length));   // (de una vez: queda a menos de una vuelta)
    while (f > end) f -= length;
    if (f < start) f = start;
    return f;
}
// ...y por ABAJO (velocidad negativa): si cae despues del fin, el fin
static float WrapDown(float f, float start, float end, float length) {
    if (FrameRare(f)) return start;
    if ((double)start - (double)f > (double)length * 64.0)
        f = (float)((double)start - fmod((double)start - (double)f, (double)length));
    while (f < start) f += length;
    if (f > end) f = end;
    return f;
}

void W3dAnimSceneTick(float dt) {
    if (gEscGame < 0) return;
    std::vector<AnimationObject>* objs = SceneObjects(gEscGame);
    if (!objs) { gEscGame = -1; return; }
    const SceneAnimation* e = SceneAnimations[gEscGame];
    const int fps = (e->fps > 0) ? e->fps : 30;
    if (!gEscGameManual) gEscGameF += dt * (float)fps;   // con animEscenaFrame manda el script (un solo reloj)
    const float start = (float)e->startFrame, end = (float)e->endFrame;
    if (gEscGameF > end) {
        if (gEscGameLoop) {
            float length = end - start + 1.0f;
            if (length < 1.0f) length = 1.0f;
            gEscGameF = WrapUp(gEscGameF, start, end, length);
        } else {
            gEscGameF = end;   // one-shot: queda clavada en la ultima pose
        }
    }
    const int f = (int)gEscGameF;
    for (size_t i = 0; i < objs->size(); i++) {
        AnimationObject& ao = (*objs)[i];
        if (ao.obj) ApplyCurvesGame(ao.obj, ao.Properties, gEscGameF);
    }
    W3dActiveCamApply(gEscGame, f);   // los CORTES de camara de la cinematica
    W3dApplyMaterialTracks(gEscGame, gEscGameF);   // los MATERIALES animados (frame continuo, como los objetos)
}

int W3dAnimSceneFrame() { return gEscGame < 0 ? -1 : (int)gEscGameF; }
int W3dAnimScenePlaying() { return gEscGame; }

// ============================================================================
//  VISTAS DE EDICION DE LOS CLIPS DE JERARQUIA (ver SceneAnimation::isClip en Animation.h y
//  W3dAnimSet.h). El clip vive COMPARTIDO en su biblioteca, con sus pistas por RUTA; el editor
//  lo edita como una animacion de escena atada a los objetos de UNA raiz (la vista), y la vista
//  se escribe de vuelta en el clip cuando alguien lo va a leer (W3dClipsViewsSync).
// ============================================================================
bool W3dAnimIsClip(int idx) {
    return idx >= 0 && idx < (int)SceneAnimations.size() && SceneAnimations[idx] && SceneAnimations[idx]->isClip;
}

bool W3dClipViewLive(int idx) {
    if (!W3dAnimIsClip(idx)) return false;
    const SceneAnimation* e = SceneAnimations[idx];
    // (una raiz borrada con undo queda DETACHADA, viva pero fuera de todo arbol: su vista no se edita ni se
    //  escribe. W3dRootOf no alcanza: la raiz detachada conserva su Parent -el Ctrl+Z la cuelga ahi- y
    //  subiendo por Parent llega a SceneCollection; escribir la vista entonces descartaba la pista "." del
    //  clip COMPARTIDO -sus curvas se fueron con el borrado- para todas las raices que lo usan)
    return e->owner && e->clipHier && W3dAttachedRoot(e->owner) &&
           W3dHierClipLive(W3dHierResource(e->owner), e->clipHier, e->seriesHier);
}

// ---- la RAIZ es relativa: lo que se le suma a sus curvas de transformacion al atarla ----
static const AnimProperty* CurveWith(const std::vector<AnimProperty>& props, int prop, int comp) {
    for (size_t i = 0; i < props.size(); i++)
        if (props[i].Property == prop && props[i].component == comp && !props[i].keyframes.empty()) return &props[i];
    return 0;
}
static float CompOf(const Vector3& v, int c) { return c == AnimX ? v.x : (c == AnimY ? v.y : v.z); }
static void CompSet(Vector3& v, int c, float f) { if (c == AnimX) v.x = f; else if (c == AnimY) v.y = f; else v.z = f; }
// el desplazamiento de cada componente (base de la raiz - valor del clip en su primer frame); 0 donde no hay curva
static Vector3 OffsetRoot(const std::vector<AnimProperty>& props, int prop, int start, const Vector3& base) {
    Vector3 off(0, 0, 0);
    for (int c = AnimX; c <= AnimZ; c++) {
        const AnimProperty* ap = CurveWith(props, prop, c);
        if (ap) CompSet(off, c, CompOf(base, c) - ap->Eval(start, 0.0f));
    }
    return off;
}
// suma (signo +1) o resta (-1) el desplazamiento a los valores de una curva de transformacion de la raiz
static void RunCurve(AnimProperty& ap, const SceneAnimation* e, int sign) {
    const Vector3* off = ap.Property == AnimPosition ? &e->rootOffPos : ap.Property == AnimRotation ? &e->rootOffRot
                       : ap.Property == AnimScale ? &e->rootOffEsc : 0;
    if (!off || ap.component < AnimX || ap.component > AnimZ) return;
    const float d = CompOf(*off, ap.component);
    if (d == 0.0f) return;
    for (size_t k = 0; k < ap.keyframes.size(); k++) ap.keyframes[k].value = sign > 0 ? ap.keyframes[k].value + d : ap.keyframes[k].value - d;
}
// (clave a clave, valores y handles bit a bit; sin expandir una curva compacta: W3dKeys::SameBits)
static bool KeysEqual(const W3dKeys& a, const W3dKeys& b) { return a.SameBits(b); }
static bool PropsEqual(const std::vector<AnimProperty>& a, const std::vector<AnimProperty>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].Property != b[i].Property || a[i].component != b[i].component || !KeysEqual(a[i].keyframes, b[i].keyframes))
            return false;
    return true;
}

// el REPOSO de la raiz de una vista: la base a la que se le suma su pista "." al atarla. NO es su transform de
// hoy: la raiz puede estar POSADA -por esta misma vista a mitad del clip (renombrar el clip que se edita la
// vuelve a leer), por la vista de otro clip suyo o por una animacion de escena- y ese cuadro no es su reposo
// (atarla ahi corria toda la curva y el guardado escribia la raiz corrida como su transform). Es lo mismo que
// escribe el guardado (RestAnimObjects, main/io/SaveW3D.cpp): la curva de quien la poso, en SU cuadro de
// inicio. Por componente y en este orden: la vista que se lee (sus curvas de antes), las vistas de los otros
// clips de la raiz (atadas al mismo reposo) y, si la raiz esta posada, la animacion de escena activa y las
// demas; lo que nadie anima queda con el valor que tiene.
static void RestRootView(int idx, Object* root, Vector3& pos, Vector3& rot, Vector3& esc) {
    pos = root->pos; rot = root->rotEuler; esc = root->scale;
    unsigned done = 0;   // bits: 0-2 posicion x/y/z, 3-5 rotacion, 6-8 escala
    for (int pass = 0; pass < 4; pass++)
        for (size_t i = 0; i < SceneAnimations.size(); i++) {
            const SceneAnimation* e = SceneAnimations[i];
            if (!e) continue;
            const bool viewOwn = e->isClip && e->owner == root;
            if (pass == 0 && (int)i != idx) continue;
            if (pass == 1 && ((int)i == idx || !viewOwn)) continue;
            if (pass >= 2 && (e->isClip || !root->posedByCurves)) continue;
            if (pass == 2 && (int)i != ActiveSceneAnim) continue;
            if (pass == 3 && (int)i == ActiveSceneAnim) continue;
            const std::vector<AnimationObject>* lst = SceneObjects((int)i);
            if (!lst) continue;
            for (size_t k = 0; k < lst->size(); k++) {
                if ((*lst)[k].obj != root) continue;
                const std::vector<AnimProperty>& props = (*lst)[k].Properties;
                for (int c = AnimX; c <= AnimZ; c++) {
                    const unsigned b = 1u << (c - AnimX);
                    const AnimProperty* p = CurveWith(props, AnimPosition, c);
                    if (p && !(done & b)) { CompSet(pos, c, p->Eval(e->startFrame, CompOf(pos, c))); done |= b; }
                    const AnimProperty* r = CurveWith(props, AnimRotation, c);
                    if (r && !(done & (b << 3))) { CompSet(rot, c, r->Eval(e->startFrame, CompOf(rot, c))); done |= b << 3; }
                    const AnimProperty* q = CurveWith(props, AnimScale, c);
                    if (q && !(done & (b << 6))) { CompSet(esc, c, q->Eval(e->startFrame, CompOf(esc, c))); done |= b << 6; }
                }
                break;
            }
        }
}

void W3dClipViewRead(int idx) {
    if (!W3dClipViewLive(idx)) return;
    SceneAnimation* e = SceneAnimations[idx];
    std::vector<AnimationObject>* lst = SceneObjects(idx);
    if (!lst) return;
    const W3dClipHier* c = e->clipHier;
    Object* root = e->owner;
    Vector3 basePos, baseRot, baseEsc;
    RestRootView(idx, root, basePos, baseRot, baseEsc);   // (ANTES de vaciarla: sus curvas de antes lo dicen)
    lst->clear();
    e->name = c->name;
    e->startFrame = c->start; e->endFrame = c->end; e->fps = c->fps > 0 ? c->fps : 30;
    e->rootOffPos = e->rootOffRot = e->rootOffEsc = Vector3(0, 0, 0);
    for (size_t p = 0; p < c->tracks.size(); p++) {
        Object* n = W3dHierNode(root, c->tracks[p].path);
        if (!n) continue;       // (esta jerarquia no lo tiene: la pista queda en el clip tal cual)
        bool already = false;   // (dos rutas que caen en el mismo nodo: vale la primera)
        for (size_t k = 0; k < lst->size() && !already; k++) if ((*lst)[k].obj == n) already = true;
        if (already) continue;
        AnimationObject ao;
        ao.obj = n; ao.FirstKeyFrame = 0; ao.LastKeyFrame = 0;
        ao.Properties = c->tracks[p].props;
        if (n == root) {
            e->rootOffPos = OffsetRoot(ao.Properties, AnimPosition, c->start, basePos);
            e->rootOffRot = OffsetRoot(ao.Properties, AnimRotation, c->start, baseRot);
            e->rootOffEsc = OffsetRoot(ao.Properties, AnimScale,    c->start, baseEsc);
            for (size_t k = 0; k < ao.Properties.size(); k++) RunCurve(ao.Properties[k], e, +1);
        }
        ao.UpdateFirstLastFrame();
        lst->push_back(ao);
    }
    e->versionView = c->version;
    W3dAnimCurvesInvalidate();
}

bool W3dClipViewWrite(int idx) {
    if (!W3dClipViewLive(idx)) return false;
    SceneAnimation* e = SceneAnimations[idx];
    std::vector<AnimationObject>* lst = SceneObjects(idx);
    if (!lst) return false;
    W3dClipHier* c = e->clipHier;
    Object* root = e->owner;
    std::vector<char> used(lst->size(), 0);
    std::vector<W3dHierTrack> newValue;
    // las pistas que ya tenia, en su orden y con su ruta de siempre: las de los nodos que esta jerarquia
    // no tiene quedan tal cual; las demas pasan a ser lo que dice la vista (sin keyframes = se borro)
    for (size_t p = 0; p < c->tracks.size(); p++) {
        const W3dHierTrack& old = c->tracks[p];
        Object* n = W3dHierNode(root, old.path);
        if (!n) { newValue.push_back(old); continue; }
        int k = -1;
        for (size_t i = 0; i < lst->size() && k < 0; i++) if (!used[i] && (*lst)[i].obj == n) k = (int)i;
        if (k < 0) continue;
        used[(size_t)k] = 1;
        W3dHierTrack np;
        np.path = old.path;
        const std::vector<AnimProperty>& vp = (*lst)[(size_t)k].Properties;
        for (size_t q = 0; q < vp.size(); q++) {
            if (vp[q].keyframes.empty()) continue;
            AnimProperty ap = vp[q];
            if (n == root) {
                // la curva que no se toco vuelve con los valores que tenia (sin el ida y vuelta del desplazamiento)
                const AnimProperty* prev = 0;
                for (size_t r = 0; r < old.props.size() && !prev; r++)
                    if (old.props[r].Property == ap.Property && old.props[r].component == ap.component) prev = &old.props[r];
                AnimProperty expected;
                if (prev) { expected = *prev; RunCurve(expected, e, +1); }
                if (prev && KeysEqual(expected.keyframes, ap.keyframes)) ap = *prev;
                else RunCurve(ap, e, -1);
            }
            np.props.push_back(ap);
        }
        if (!np.props.empty()) newValue.push_back(np);
    }
    // lo que se keyeo NUEVO en la vista (un objeto de la jerarquia que el clip no tenia)
    for (size_t i = 0; i < lst->size(); i++) {
        if (used[i] || !(*lst)[i].obj) continue;
        const std::string path = W3dHierPath(root, (*lst)[i].obj);
        if (path.empty()) continue;   // (fuera de la jerarquia: W3dAnimObjectAllowed no deberia dejarlo entrar)
        W3dHierTrack np;
        np.path = path;
        for (size_t q = 0; q < (*lst)[i].Properties.size(); q++) {
            if ((*lst)[i].Properties[q].keyframes.empty()) continue;
            AnimProperty ap = (*lst)[i].Properties[q];
            if ((*lst)[i].obj == root) RunCurve(ap, e, -1);
            np.props.push_back(ap);
        }
        if (!np.props.empty()) newValue.push_back(np);
    }
    // el nombre (el Rename de la tarjeta escribe el de la vista): unico entre los clips de su biblioteca
    if (e->name != c->name) e->name = W3dHierClipRename(W3dHierAnimSet(root), c, e->name);
    bool change = (newValue.size() != c->tracks.size());
    for (size_t p = 0; p < newValue.size() && !change; p++)
        if (newValue[p].path != c->tracks[p].path || !PropsEqual(newValue[p].props, c->tracks[p].props)) change = true;
    if (c->start != e->startFrame || c->end != e->endFrame || c->fps != e->fps) change = true;
    if (change) {
        c->tracks.swap(newValue);
        c->start = e->startFrame; c->end = e->endFrame; c->fps = e->fps;
        c->version++;
    }
    e->versionView = c->version;
    return change;
}

void W3dClipsViewsSync() {
    if (W3dClipViewLive(ActiveSceneAnim)) W3dClipViewWrite(ActiveSceneAnim);
}

int W3dClipView(Object* root, W3dClipHier* c) {
    if (!root || !c) return -1;
    InitSceneAnimations();   // (la animacion de escena de siempre existe antes que cualquier vista)
    for (size_t i = 0; i < SceneAnimations.size(); i++) {
        SceneAnimation* e = SceneAnimations[i];
        if (e && e->isClip && e->owner == root && e->clipHier == c && e->seriesHier == c->series) {
            if (e->versionView != c->version && (int)i != ActiveSceneAnim) W3dClipViewRead((int)i);
            return (int)i;
        }
    }
    SceneAnimation* e = new SceneAnimation(c->name);
    e->isClip = true;
    e->owner  = root;
    e->clipHier = c;
    e->seriesHier = c->series;
    SceneAnimations.push_back(e);
    const int idx = (int)SceneAnimations.size() - 1;
    W3dClipViewRead(idx);
    return idx;
}

void W3dClipActiveViewReread(const W3dClipHier* c) {
    if (!c || !W3dClipViewLive(ActiveSceneAnim) || SceneAnimations[ActiveSceneAnim]->clipHier != c) return;
    W3dClipViewRead(ActiveSceneAnim);
}

void W3dClipsViewsPurge() {
    for (size_t i = SceneAnimations.size(); i-- > 0; ) {
        if ((int)i == ActiveSceneAnim || !W3dAnimIsClip((int)i) || W3dClipViewLive((int)i)) continue;
        delete SceneAnimations[i];
        SceneAnimations.erase(SceneAnimations.begin() + (long)i);
        if ((int)i < ActiveSceneAnim) ActiveSceneAnim--;
    }
}

void W3dClipsViewsSwap(std::vector<SceneAnimation*>& list, const Object* owner,
                             const std::vector<W3dClipHier*>& old, const std::vector<W3dClipHier*>& newValue) {
    for (size_t i = 0; i < list.size(); i++) {
        SceneAnimation* e = list[i];
        if (!e || !e->isClip || e->owner != owner || !e->clipHier) continue;
        for (size_t k = 0; k < old.size() && k < newValue.size(); k++) {
            if (old[k] != e->clipHier || old[k]->series != e->seriesHier || !newValue[k]) continue;
            // (al dia con el viejo = al dia con el nuevo: son iguales; si estaba vieja se sigue re-leyendo)
            const bool upToDate = (e->versionView == old[k]->version);
            e->clipHier = newValue[k];
            e->seriesHier = newValue[k]->series;
            e->versionView = upToDate ? newValue[k]->version : newValue[k]->version - 1u;
            break;
        }
    }
}

std::string W3dAnimSceneLabel(int idx) {
    if (idx < 0 || idx >= (int)SceneAnimations.size() || !SceneAnimations[idx]) return std::string();
    const SceneAnimation* e = SceneAnimations[idx];
    if (!e->isClip) return e->name;
    return (e->owner ? e->owner->name : std::string("?")) + ": " + e->name;
}

bool W3dAnimObjectAllowed(const Object* o) {
    if (!o || ActiveAnimKind != 0 || !W3dAnimIsClip(ActiveSceneAnim)) return true;
    const SceneAnimation* e = SceneAnimations[ActiveSceneAnim];
    return e->owner && !W3dHierPath(e->owner, o).empty();
}

// ============================================================================
//  EL REPRODUCTOR DE CLIPS DE JERARQUIA (un cabezal por raiz). Los nodos se resuelven UNA vez
//  por ruta al arrancar (y otra vez si el clip cambia en el editor); el clip es de su biblioteca,
//  a la que la entrada le toma una referencia mientras suena.
//
//  RAICES ANIDADAS: un nodo del clip de una raiz puede ser a su vez la raiz de OTRO clip que suena (el
//  auto abre la puerta con su clip y la puerta, ademas, tiembla con el suyo). El resultado no depende del
//  orden en que lua los arranco: en cada tick se posan primero los ANCESTROS y despues los descendientes.
//    - las pistas de un nodo que no es la raiz del clip van tal cual: si dos clips que suenan lo animan,
//      gana el de la raiz MAS CERCANA (la mas profunda);
//    - la pista "." (relativa) de la raiz anidada se SUMA a lo que le dejo el ancestro: en los canales que
//      el clip del ancestro le anima, su base es la pose que el ancestro le acaba de dar (y la conserva si
//      el del ancestro termina); en los demas, donde estaba al arrancar.
//  Una raiz con CAPAS (W3dHierRoot::capas) la posa el Mix, despues de este reproductor: el Mix manda.
// ============================================================================
namespace {
struct W3dAnimObjectPlaying {
    Object* o;                      // la raiz que lo reproduce
    W3dResource* rec;               // la biblioteca del clip (una referencia PERMANENTE mientras suena)
    const W3dClipHier* clip;
    unsigned series, version;       // el clip (su serie) y la version con la que se resolvieron los nodos
    float frame, vel;
    bool loop, ended;
    bool move;                     // (solo dentro del tick) su cabezal avanzo en este tick: se posa
    std::vector<Object*> nodes;    // por pista (NULL = esta jerarquia no lo tiene)
    Vector3 basePos, baseRot, baseEsc;   // la raiz al arrancar (su pista es relativa); en los canales que le
                                         // anima un ancestro, la ultima pose que ese ancestro le dio
    int retarget;                  // el modo con el que suena (W3D_RETARGET_*)
    float scale;                   // retarget: tamano del destino / tamano del clip
    W3dAnimObjectPlaying() : o(0), rec(0), clip(0), series(0), version(0), frame(0.0f), vel(1.0f), loop(true),
                             ended(false), move(false), basePos(0, 0, 0), baseRot(0, 0, 0), baseEsc(1, 1, 1),
                             retarget(0), scale(1.0f) {}
};
// los canales de transformacion (bits: 0-2 posicion x/y/z, 3-5 rotacion, 6-8 escala) que en ESTE tick el
// clip de un ancestro le poso a cada raiz que suena. Solo se lleva con dos o mas raices sonando.
struct W3dPosedChannels {
    std::vector<Object*> roots;                            // las raices que suenan, ordenadas (binary_search)
    std::vector<std::pair<Object*, unsigned> > channels;   // (raiz anidada, lo que le posaron); casi siempre vacio
};
}
static std::vector<W3dAnimObjectPlaying> gAnimObjects;
// true si la raiz tiene CAPAS de clips de jerarquia (el Mix manda: ver W3dHierRoot::capas)
static bool WithLayers(const Object* o) { return o && o->clipsHier && !o->clipsHier->layers.empty(); }

static W3dAnimObjectPlaying* PlayingFrom(const Object* o) {
    for (size_t i = 0; i < gAnimObjects.size(); i++) if (gAnimObjects[i].o == o) return &gAnimObjects[i];
    return NULL;
}
static void ReleasePlaying(W3dAnimObjectPlaying& s) {
    if (s.rec) { W3dResource* r = s.rec; s.rec = 0; W3dResourceRelease(r, W3DREC_PERMANENT); }
}
static void ResolveNodes(W3dAnimObjectPlaying& s) {
    s.nodes.assign(s.clip->tracks.size(), (Object*)0);
    for (size_t p = 0; p < s.clip->tracks.size(); p++) s.nodes[p] = W3dHierNode(s.o, s.clip->tracks[p].path);
    s.version = s.clip->version;
}
// el modo de retarget de la raiz (el suyo, si se lo dieron; si no, el del clip) y su escala
static void PickRetarget(W3dAnimObjectPlaying& s) {
    const int own = s.o->clipsHier ? s.o->clipsHier->retarget : -1;
    s.retarget = (own >= 0) ? own : s.clip->retarget;
    s.scale = 1.0f;
    if (s.retarget == W3D_RETARGET_ROTATIONS && s.clip->size > 1e-6f) {
        const float d = W3dHierSize(s.o);
        if (d > 1e-6f) s.scale = d / s.clip->size;
    }
}
// la pista de la RAIZ: el delta desde el primer frame del clip, sumado a donde estaba al arrancar.
// 'seguir' (bits de W3dPosedChannels): los canales que un ANCESTRO que suena le acaba de posar en este tick;
// en esos la base pasa a ser esa pose (la raiz anidada se mueve SOBRE lo que le hace su ancestro)
static void ApplyRoot(W3dAnimObjectPlaying& s, std::vector<AnimProperty>& props, float fr, unsigned keepGoing) {
    Object* o = s.o;
    ApplyCurvesGameChannels(o, props, fr, 0);   // lo que no es transformacion (visible, fov, luz, formas)
    const float start = (float)s.clip->start;
    bool rot = false;
    Vector3 e = o->rotEuler;
    for (int c = AnimX; c <= AnimZ; c++) {
        const unsigned b = 1u << (c - AnimX);
        const AnimProperty* p = CurveWith(props, AnimPosition, c);
        if (p) {
            if (keepGoing & b) CompSet(s.basePos, c, CompOf(o->pos, c));
            float d = p->EvalF(fr, 0.0f) - p->EvalF(start, 0.0f);
            if (s.retarget == W3D_RETARGET_ROTATIONS) d *= s.scale;   // el movimiento, a escala del destino
            CompSet(o->pos, c, CompOf(s.basePos, c) + d);
        }
        const AnimProperty* r = CurveWith(props, AnimRotation, c);
        if (r) {
            if (keepGoing & (b << 3)) CompSet(s.baseRot, c, CompOf(e, c));
            CompSet(e, c, CompOf(s.baseRot, c) + r->EvalF(fr, 0.0f) - r->EvalF(start, 0.0f));
            rot = true;
        }
        const AnimProperty* k = CurveWith(props, AnimScale, c);
        if (k) {
            if (keepGoing & (b << 6)) CompSet(s.baseEsc, c, CompOf(o->scale, c));
            CompSet(o->scale, c, CompOf(s.baseEsc, c) + k->EvalF(fr, 0.0f) - k->EvalF(start, 0.0f));
        }
    }
    if (rot) o->SetRotEuler(e);
    o->posedByClip = true;   // (ver Object::posedByClip)
}
// los canales de transformacion (bits de W3dPosedChannels) que tienen curva en 'props', de los que 'canales'
// (kCanal*) deja aplicar
static unsigned ChannelsWithCurve(const std::vector<AnimProperty>& props, int channels) {
    unsigned m = 0;
    for (size_t i = 0; i < props.size(); i++) {
        const AnimProperty& p = props[i];
        if (p.keyframes.empty() || p.component < AnimX || p.component > AnimZ) continue;
        const unsigned b = 1u << (p.component - AnimX);
        if      (p.Property == AnimPosition && (channels & kChannelPos)) m |= b;
        else if (p.Property == AnimRotation && (channels & kChannelRot)) m |= b << 3;
        else if (p.Property == AnimScale    && (channels & kChannelEsc)) m |= b << 6;
    }
    return m;
}
static unsigned PosedChannelsOf(const W3dPosedChannels& v, const Object* o) {
    for (size_t i = 0; i < v.channels.size(); i++) if (v.channels[i].first == o) return v.channels[i].second;
    return 0;
}
// anota lo que se le poso al nodo 'o' SOLO si es la raiz de otro clip que suena (lo unico que se consulta)
static void PosedChannelsAdd(W3dPosedChannels& v, Object* o, const std::vector<AnimProperty>& props, int channels) {
    if (!std::binary_search(v.roots.begin(), v.roots.end(), o)) return;
    const unsigned m = ChannelsWithCurve(props, channels);
    if (!m) return;
    for (size_t i = 0; i < v.channels.size(); i++) if (v.channels[i].first == o) { v.channels[i].second |= m; return; }
    v.channels.push_back(std::make_pair(o, m));
}
// aplica el clip en su frame: la raiz (relativa) y los nodos de su jerarquia que tienen curvas en el.
// 'seguir': ver ApplyRoot (0 fuera del tick). 'posados' (opcional): anota lo que se le poso a cada nodo.
static void ApplyClip(W3dAnimObjectPlaying& s, unsigned keepGoing = 0, W3dPosedChannels* posed = 0) {
    W3dClipHier* c = (W3dClipHier*)s.clip;
    // "rotaciones": los descendientes conservan su traslacion y su escala (las proporciones del destino)
    const int channels = s.retarget == W3D_RETARGET_ROTATIONS ? kChannelRot : kChannelAll;
    for (size_t p = 0; p < c->tracks.size() && p < s.nodes.size(); p++) {
        Object* n = s.nodes[p];
        if (!n) continue;
        if (n == s.o) { ApplyRoot(s, c->tracks[p].props, s.frame, keepGoing); continue; }
        ApplyCurvesGameChannels(n, c->tracks[p].props, s.frame, channels);
        if (posed) PosedChannelsAdd(*posed, n, c->tracks[p].props, channels);
    }
}
// cuantos padres tiene 'o' hasta el tope de su arbol (los ancestros se posan antes)
static int DepthOf(const Object* o) {
    int d = 0;
    for (const Object* p = o ? o->Parent : 0; p; p = p->Parent) d++;
    return d;
}
// posa las raices que suenan en la raiz activa, ANCESTROS PRIMERO (ver "RAICES ANIDADAS" arriba): las que
// avanzaron en este tick y las que, quietas (una pasada que ya termino), tienen un ancestro que les sigue
// moviendo los canales que su pista "." anima (su ultima pose se suma a la de el)
static void PosePlaying() {
    std::vector<std::pair<int, int> > order;   // (profundidad, indice): el indice desempata = determinista
    for (size_t k = 0; k < gAnimObjects.size(); k++)
        if (W3dRootOf(gAnimObjects[k].o) == SceneCollection)
            order.push_back(std::make_pair(DepthOf(gAnimObjects[k].o), (int)k));
    W3dPosedChannels posed;
    W3dPosedChannels* annotate = 0;   // (una sola raiz: nada que anidar)
    if (order.size() > 1) {
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i < order.size(); i++) posed.roots.push_back(gAnimObjects[(size_t)order[i].second].o);
        std::sort(posed.roots.begin(), posed.roots.end());
        annotate = &posed;
    }
    for (size_t i = 0; i < order.size(); i++) {
        W3dAnimObjectPlaying& s = gAnimObjects[(size_t)order[i].second];
        // (una raiz con CAPAS muestra la mezcla, como un armature: su clip unico avanza pero no la posa)
        if (WithLayers(s.o)) continue;
        const unsigned keepGoing = PosedChannelsOf(posed, s.o);
        if (!s.move && !keepGoing) continue;
        ApplyClip(s, keepGoing, annotate);
    }
}
float W3dAnimObjectPlay(Object* o, const std::string& name, bool loop) {
    if (!o) return -1.0f;
    W3dResource* rec = 0;
    W3dClipHier* c = W3dHierClip(o, name);
    if (c) { rec = W3dHierResource(o); W3dResourceRetain(rec, W3DREC_PERMANENT); }
    else c = W3dHierClipFind(name, &rec);   // "Biblioteca/clip" (o el unico con ese nombre): con referencia
    if (!c || !rec) return -1.0f;
    W3dAnimObjectPlaying* s = PlayingFrom(o);
    if (!s) { gAnimObjects.push_back(W3dAnimObjectPlaying()); s = &gAnimObjects.back(); s->o = o; }
    ReleasePlaying(*s);
    s->rec = rec;
    s->clip = c;
    s->series = c->series;
    s->frame  = (float)c->start;
    s->vel = 1.0f;
    s->loop = loop;
    s->ended   = false;
    s->basePos = o->pos; s->baseRot = o->rotEuler; s->baseEsc = o->scale;
    ResolveNodes(*s);
    PickRetarget(*s);
    if (!WithLayers(o)) ApplyClip(*s);   // el cuadro de inicio ya posado en este mismo tick
    return c->Duration();
}
void W3dAnimObjectsTick(float dt) {
    // 1) los cabezales
    for (size_t k = 0; k < gAnimObjects.size(); ) {
        W3dAnimObjectPlaying& s = gAnimObjects[k];
        s.move = false;
        // una raiz de OTRA raiz del proyecto (el juego mientras suena una cinematica en su escena): en pausa,
        // se conserva tal cual
        if (W3dRootOf(s.o) != SceneCollection) { k++; continue; }
        // el clip ya no esta (se borro de su biblioteca en el editor): la raiz deja de sonar
        if (!W3dHierClipLive(s.rec, s.clip, s.series)) { ReleasePlaying(s); gAnimObjects.erase(gAnimObjects.begin() + (long)k); continue; }
        if (s.version != s.clip->version) ResolveNodes(s);   // (lo cambio el editor: rutas nuevas)
        if (!s.ended) {
            const float start = (float)s.clip->start, end = (float)s.clip->end;
            float length = end - start + 1.0f;
            if (length < 1.0f) length = 1.0f;
            s.frame += dt * (float)(s.clip->fps > 0 ? s.clip->fps : 30) * s.vel;
            if (s.frame > end) {
                if (s.loop) s.frame = WrapUp(s.frame, start, end, length);
                else { s.frame = end; s.ended = true; }    // una pasada: queda en su ultima pose
            } else if (s.frame < start) {   // (velocidad negativa)
                if (s.loop) s.frame = WrapDown(s.frame, start, end, length);
                else { s.frame = start; s.ended = true; }
            } else if (FrameRare(s.frame)) s.frame = start;   // (NaN: una velocidad NaN desde lua)
            // (el tick en que una pasada llega al final tambien la posa: su ultima pose). Con velocidad 0 el cabezal no
            // se mueve: la pose ya esta (la puso animObjeto o animObjetoFrame, que posan en el momento)
            s.move = s.vel != 0.0f;
        }
        k++;
    }
    // 2) las poses: los ancestros primero (raices anidadas)
    PosePlaying();
}
float W3dAnimObjectFrame(const Object* o) {
    const W3dAnimObjectPlaying* s = PlayingFrom(o);
    return s ? s->frame : -1.0f;
}
void W3dAnimObjectSetFrame(Object* o, float f) {
    W3dAnimObjectPlaying* s = PlayingFrom(o);
    if (!s || !W3dHierClipLive(s->rec, s->clip, s->series)) return;
    const float start = (float)s->clip->start, end = (float)s->clip->end;
    s->frame = f < start ? start : (f > end ? end : f);
    s->ended = false;
    if (!WithLayers(o)) ApplyClip(*s);
}
bool W3dAnimObjectEnded(const Object* o) {
    const W3dAnimObjectPlaying* s = PlayingFrom(o);
    return !s || s->ended;
}
std::string W3dAnimObjectClip(const Object* o) {
    const W3dAnimObjectPlaying* s = PlayingFrom(o);
    return (s && W3dHierClipLive(s->rec, s->clip, s->series)) ? s->clip->name : std::string();
}
void W3dAnimObjectStop(Object* o) {
    for (size_t i = 0; i < gAnimObjects.size(); i++)
        if (gAnimObjects[i].o == o) { ReleasePlaying(gAnimObjects[i]); gAnimObjects.erase(gAnimObjects.begin() + (long)i); return; }
}
bool W3dAnimObjectVelocity(Object* o, float v) {
    W3dAnimObjectPlaying* s = PlayingFrom(o);
    if (!s) return false;
    s->vel = v;
    if (v != 0.0f) s->ended = false;
    return true;
}
void W3dAnimObjectRetarget(Object* o, int mode) {
    if (!o) return;
    if (!o->clipsHier) o->clipsHier = new W3dHierRoot();
    o->clipsHier->retarget = (mode < 0) ? -1 : (mode == W3D_RETARGET_ROTATIONS ? W3D_RETARGET_ROTATIONS : W3D_RETARGET_COMPLETE);
    W3dAnimObjectPlaying* s = PlayingFrom(o);
    if (s && W3dHierClipLive(s->rec, s->clip, s->series)) { PickRetarget(*s); if (!WithLayers(o)) ApplyClip(*s); }
    // las capas recalculan su escala con el modo nuevo
    if (o->clipsHier) for (size_t k = 0; k < o->clipsHier->layers.size(); k++) o->clipsHier->layers[k].retClip = 0;
}
void W3dAnimObjectsReset() {
    for (size_t i = 0; i < gAnimObjects.size(); i++) ReleasePlaying(gAnimObjects[i]);
    gAnimObjects.clear();
}
int  W3dAnimObjectsPlaying() { return (int)gAnimObjects.size(); }

// ~Object: una raiz que se borra deja de sonar (suelta su biblioteca), un nodo que se borra deja de
// posarse, y las vistas de edicion de esa raiz quedan HUERFANAS (no se listan ni se escriben; las de una
// raiz inactiva las suelta main/W3dRoots)
static std::vector<Object*> gHierWithLayers;   // las raices con CAPAS de clips de jerarquia (ver mas abajo)
static void MixBasesForgetObject(Object* o);   // (el Mix, mas abajo)
static void AnimObjectsUnlink(Object* o) {
    for (size_t i = gHierWithLayers.size(); i-- > 0; ) if (gHierWithLayers[i] == o) gHierWithLayers.erase(gHierWithLayers.begin() + (long)i);
    MixBasesForgetObject(o);
    for (size_t i = gAnimObjects.size(); i-- > 0; ) {
        W3dAnimObjectPlaying& s = gAnimObjects[i];
        if (s.o == o) { ReleasePlaying(s); gAnimObjects.erase(gAnimObjects.begin() + (long)i); continue; }
        for (size_t n = 0; n < s.nodes.size(); n++) if (s.nodes[n] == o) s.nodes[n] = 0;
    }
    for (size_t i = 0; i < SceneAnimations.size(); i++)
        if (SceneAnimations[i] && SceneAnimations[i]->isClip && SceneAnimations[i]->owner == o) SceneAnimations[i]->owner = 0;
}
namespace {
struct RegisterAnimObjects { RegisterAnimObjects() { W3dUnlinkRegister(AnimObjectsUnlink); } } gRegisterAnimObjects;
}


// ============================================================================
//  MIX DE ESCENAS (capas de animaciones de ESCENA, W3dLayerAnim / g_mixScenes): cada objeto que tocan las
//  capas visibles recibe la MEZCLA (mezclar = lerp, sumar/restar = lo que la capa se aparta de la BASE). Sirve
//  para personajes hechos de OBJETOS sueltos (GTA3: cada parte es un objeto; correr + disparar + ...).
//  La BASE es la transform que el objeto tenia la primera vez que el mix lo toco; W3dMixScenesRelease lo
//  devuelve ahi (al salir del Mix en el editor). En el juego cada capa avanza con su cabezal (gameFrame).
// ============================================================================
#include "animation/SkeletalAnimation.h"   // g_mixScenes, W3dLayerFrameEditor
#include <map>
struct W3dMixBase { Vector3 p, r, s; };
static std::map<Object*, W3dMixBase> gMixBase;
void W3dMixScenesRelease(){
    for (std::map<Object*, W3dMixBase>::iterator it = gMixBase.begin(); it != gMixBase.end(); ++it){
        Object* o = it->first; if (!o) continue;
        o->pos = it->second.p; o->SetRotEuler(it->second.r); o->scale = it->second.s; }
    gMixBase.clear();
}
// el PLAY del editor: las bases se guardan al darle Play y se reponen en Stop SIN mover nada (las
// posiciones las repone el snapshot de SimJuego). Sin esto, un objeto que el mix toco por PRIMERA vez
// jugando se quedaba con la base de la partida y el Mix del editor lo mezclaba (y lo soltaba) desde ahi.
static std::map<Object*, W3dMixBase> gMixBaseEditor;
void W3dMixScenesBasesSave(){ gMixBaseEditor = gMixBase; }
void W3dMixScenesBasesRestore(){ gMixBase = gMixBaseEditor; gMixBaseEditor.clear(); }
// RestartScene (abrir / nuevo proyecto): los objetos de las bases se van a BORRAR -> se olvidan SIN
// moverlos. Sin esto, salir del Mix despues de abrir otro proyecto les escribia encima (ya liberados).
void W3dMixScenesForget(){ gMixBase.clear(); gMixBaseEditor.clear(); }
// ~Object: su base se olvida (salir del Mix o guardar le escribian encima a un objeto ya liberado)
static void MixBasesForgetObject(Object* o){ gMixBase.erase(o); gMixBaseEditor.erase(o); }
void W3dMixScenesBases(std::vector<Object*>& objs, std::vector<Vector3>& pos, std::vector<Vector3>& rot,
                        std::vector<Vector3>& esc){
    objs.clear(); pos.clear(); rot.clear(); esc.clear();
    for (std::map<Object*, W3dMixBase>::const_iterator it = gMixBase.begin(); it != gMixBase.end(); ++it){
        if (!it->first) continue;
        objs.push_back(it->first); pos.push_back(it->second.p); rot.push_back(it->second.r); esc.push_back(it->second.s);
    }
}
// ---- LAS CAPAS DE CLIPS DE JERARQUIA (W3dHierRoot::capas): el Mix de una raiz, que se suma al de escena ----
void W3dHierLayersChanged(Object* o) {
    if (!o) return;
    const bool has = WithLayers(o);
    std::vector<Object*>::iterator it = std::find(gHierWithLayers.begin(), gHierWithLayers.end(), o);
    if (has && it == gHierWithLayers.end()) gHierWithLayers.push_back(o);
    else if (!has && it != gHierWithLayers.end()) gHierWithLayers.erase(it);
    if (o->clipsHier) for (size_t k = 0; k < o->clipsHier->layers.size(); k++) o->clipsHier->layers[k].retClip = 0;
}
// las raices con capas de la raiz ACTIVA (las de una escena que no se esta mirando quedan quietas), los
// ANCESTROS primero: una raiz anidada (con sus propias capas) mezcla sobre lo que le dejaron las de su
// ancestro, sin importar en que orden se les pusieron las capas (ver "RAICES ANIDADAS" arriba)
static void RootsWithLayers(std::vector<Object*>& out) {
    out.clear();
    std::vector<std::pair<int, int> > order;   // (profundidad, indice)
    for (size_t i = 0; i < gHierWithLayers.size(); i++)
        if (WithLayers(gHierWithLayers[i]) && W3dRootOf(gHierWithLayers[i]) == SceneCollection)
            order.push_back(std::make_pair(DepthOf(gHierWithLayers[i]), (int)i));
    if (order.size() > 1) std::sort(order.begin(), order.end());
    for (size_t i = 0; i < order.size(); i++) out.push_back(gHierWithLayers[(size_t)order[i].second]);
}
void W3dHierLayersRoots(std::vector<Object*>& out) { RootsWithLayers(out); }
// el frame de la capa sobre su clip: jugando, su cabezal; en el editor, el del Mix
static float FrameLayerHier(const W3dLayerAnim& c, const W3dClipHier* clip, int frameMix, bool game) {
    return game ? (float)clip->start + c.gameFrame : W3dLayerFrameEditor(c, frameMix, clip->start, clip->end);
}
// el retarget de la capa (el de la raiz si lo tiene, si no el del clip) y su escala, cacheada por clip
static int RetargetLayerHier(Object* o, W3dLayerAnim& c, const W3dClipHier* clip, float* scale) {
    const int own = o->clipsHier ? o->clipsHier->retarget : -1;
    const int mode = (own >= 0) ? own : clip->retarget;
    *scale = 1.0f;
    if (mode != W3D_RETARGET_ROTATIONS) return mode;
    if (c.retClip != (const void*)clip) {
        c.retScale = 1.0f;
        const float d = W3dHierSize(o);
        if (clip->size > 1e-6f && d > 1e-6f) c.retScale = d / clip->size;
        c.retClip = (const void*)clip;
    }
    *scale = c.retScale;
    return mode;
}
bool W3dMixScenesApply(int frameMix, bool game){
    std::vector<Object*> rootsLayers;
    RootsWithLayers(rootsLayers);
    if (g_mixScenes.empty() && rootsLayers.empty()) return false;
    InitSceneAnimations();
    std::map<Object*, W3dMixBase> acc;
    for (size_t k = 0; k < g_mixScenes.size(); k++){
        W3dLayerAnim& c = g_mixScenes[k];
        if (!c.visible || c.influence <= 0.0f) continue;
        int idx = W3dAnimSceneIdx(c.anim.c_str()); if (idx < 0) continue;
        SceneAnimation* sa = SceneAnimations[idx];
        std::vector<AnimationObject>& objs = (idx == ActiveSceneAnim) ? AnimationObjects : sa->objects;
        int f = game ? sa->startFrame + (int)c.gameFrame : (int)W3dLayerFrameEditor(c, frameMix, sa->startFrame, sa->endFrame);
        float w = c.influence * 0.01f; if (c.mode == 0 && w > 1.0f) w = 1.0f; if (w > 2.0f) w = 2.0f;
        float sg = (c.mode == 2) ? -1.0f : 1.0f;
        for (size_t i = 0; i < objs.size(); i++){
            Object* o = objs[i].obj; if (!o) continue;
            if (gMixBase.find(o) == gMixBase.end()){ W3dMixBase b; b.p = o->pos; b.r = o->rotEuler; b.s = o->scale; gMixBase[o] = b; }
            const W3dMixBase& base = gMixBase[o];
            if (acc.find(o) == acc.end()) acc[o] = base;
            W3dMixBase& A = acc[o];
            std::vector<AnimProperty>& pr = objs[i].Properties;
            Vector3 P = EvalPropVec(pr, AnimPosition, f, base.p);
            Vector3 R = EvalPropVec(pr, AnimRotation, f, base.r);
            Vector3 S = EvalPropVec(pr, AnimScale,    f, base.s);
            if (c.mode == 0){ A.p = A.p + (P - A.p) * w; A.r = A.r + (R - A.r) * w; A.s = A.s + (S - A.s) * w; }
            else { A.p = A.p + (P - base.p) * (w * sg); A.r = A.r + (R - base.r) * (w * sg); A.s = A.s + (S - base.s) * (w * sg); }
        }
    }
    // las CAPAS DE CLIPS DE JERARQUIA de cada raiz, en su orden, sobre la misma acumulacion: cada nodo por su RUTA.
    // La pista de la raiz es RELATIVA (el delta desde el primer frame del clip, sumado a su base); con retarget
    // "rotaciones" los descendientes solo toman la rotacion y el movimiento de la raiz se escala por tamano.
    // Las raices van ANCESTROS PRIMERO (RootsWithLayers): la de una raiz ANIDADA (un nodo que ya mezclaron las
    // capas de su ancestro) se mueve SOBRE esa mezcla, como en el reproductor de clips (ver "RAICES ANIDADAS").
    std::vector<Object*> touchedHier;   // las raices con capas que ya mezclaron las capas de una raiz anterior
    std::vector<Object*> withLayers;    // las raices con capas, ordenadas (binary_search): solo esas se anotan
    if (rootsLayers.size() > 1){ withLayers = rootsLayers; std::sort(withLayers.begin(), withLayers.end()); }
    for (size_t r = 0; r < rootsLayers.size(); r++){
        Object* root = rootsLayers[r];
        // JUGANDO, una raiz OCULTA (ella o un ancestro) no se posa: no se dibuja, y su pose cuesta lo mismo que la de
        // una visible (un peaton lejano que el juego escondio). Su cabezal sigue avanzando: al mostrarse, esta en fase
        if (game) {
            bool show = true;
            for (const Object* q = root; q && show; q = q->Parent) show = q->visible;
            if (!show) continue;
        }
        const bool nested = std::find(touchedHier.begin(), touchedHier.end(), root) != touchedHier.end();
        W3dMixBase nestedBase;
        if (nested) nestedBase = acc[root];   // (lo que le dejaron las capas de antes: su base relativa)
        std::vector<W3dLayerAnim>& layers = root->clipsHier->layers;
        for (size_t k = 0; k < layers.size(); k++){
            W3dLayerAnim& c = layers[k];
            if (!c.visible || c.influence <= 0.0f) continue;
            const W3dClipHier* clip = W3dHierLayerClip(root, c, 0);
            if (!clip) continue;
            const float f = FrameLayerHier(c, clip, frameMix, game);
            const float start = (float)clip->start;
            float esc = 1.0f;
            const bool onlyRot = RetargetLayerHier(root, c, clip, &esc) == W3D_RETARGET_ROTATIONS;
            float w = c.influence * 0.01f; if (c.mode == 0 && w > 1.0f) w = 1.0f; if (w > 2.0f) w = 2.0f;
            const float sg = (c.mode == 2) ? -1.0f : 1.0f;
            for (size_t p = 0; p < clip->tracks.size(); p++){
                Object* n = W3dHierNode(root, clip->tracks[p].path);
                if (!n) continue;   // (esta jerarquia no lo tiene)
                const std::vector<AnimProperty>& pr = clip->tracks[p].props;
                if (gMixBase.find(n) == gMixBase.end()){ W3dMixBase b; b.p = n->pos; b.r = n->rotEuler; b.s = n->scale; gMixBase[n] = b; }
                const W3dMixBase& base = (n == root && nested) ? nestedBase : gMixBase[n];
                if (acc.find(n) == acc.end()) acc[n] = base;
                if (n != root && !withLayers.empty() && std::binary_search(withLayers.begin(), withLayers.end(), n)) touchedHier.push_back(n);
                W3dMixBase& A = acc[n];
                Vector3 P = base.p, R = base.r, S = base.s;
                if (n == root){
                    for (int q = AnimX; q <= AnimZ; q++){
                        const AnimProperty* cp = CurveWith(pr, AnimPosition, q);
                        if (cp) CompSet(P, q, CompOf(base.p, q) + (cp->EvalF(f, 0.0f) - cp->EvalF(start, 0.0f)) * (onlyRot ? esc : 1.0f));
                        const AnimProperty* cr = CurveWith(pr, AnimRotation, q);
                        if (cr) CompSet(R, q, CompOf(base.r, q) + cr->EvalF(f, 0.0f) - cr->EvalF(start, 0.0f));
                        const AnimProperty* ck = CurveWith(pr, AnimScale, q);
                        if (ck) CompSet(S, q, CompOf(base.s, q) + ck->EvalF(f, 0.0f) - ck->EvalF(start, 0.0f));
                    }
                } else {
                    std::vector<AnimProperty>& prm = const_cast<std::vector<AnimProperty>&>(pr);
                    R = EvalPropVecF(prm, AnimRotation, f, base.r);
                    if (!onlyRot){ P = EvalPropVecF(prm, AnimPosition, f, base.p); S = EvalPropVecF(prm, AnimScale, f, base.s); }
                }
                if (c.mode == 0){ A.p = A.p + (P - A.p) * w; A.r = A.r + (R - A.r) * w; A.s = A.s + (S - A.s) * w; }
                else { A.p = A.p + (P - base.p) * (w * sg); A.r = A.r + (R - base.r) * (w * sg); A.s = A.s + (S - base.s) * (w * sg); }
            }
        }
    }
    for (std::map<Object*, W3dMixBase>::iterator it = acc.begin(); it != acc.end(); ++it){
        Object* o = it->first;
        o->pos = it->second.p; o->SetRotEuler(it->second.r); o->scale = it->second.s;
        o->posedByCurves = true;
    }
    // JUEGO: varias animaciones de escena A LA VEZ (una por capa: la cinematica de la puerta, la de las
    // luces, la del cielo...). Una capa "mezclar" a peso ENTERO es la animacion tal cual, asi que ademas
    // de la transformacion aplica lo que no se mezcla: visibilidad, formas, fov, colores de luz y sus
    // cortes de camara (el de la ultima capa que tenga). Las capas parciales solo mezclan transformacion.
    if (game){
        int cutEsc = -1, cutFrame = 0;
        for (size_t k = 0; k < g_mixScenes.size(); k++){
            W3dLayerAnim& c = g_mixScenes[k];
            if (!c.visible || c.mode != 0 || c.influence < 99.95f) continue;
            int idx = W3dAnimSceneIdx(c.anim.c_str()); if (idx < 0) continue;
            SceneAnimation* sa = SceneAnimations[idx];
            std::vector<AnimationObject>& objs = (idx == ActiveSceneAnim) ? AnimationObjects : sa->objects;
            const float f = (float)sa->startFrame + c.gameFrame;
            for (size_t i = 0; i < objs.size(); i++)
                if (objs[i].obj) ApplyCurvesGameChannels(objs[i].obj, objs[i].Properties, f, 0);
            if (!sa->camTrack.empty() && !sa->camTrack[0].keyframes.empty()) { cutEsc = idx; cutFrame = (int)f; }
        }
        if (cutEsc >= 0) W3dActiveCamApply(cutEsc, cutFrame);
        // idem las capas de jerarquia "mezclar" a peso entero: visible, formas, fov y luz de sus nodos
        for (size_t r = 0; r < rootsLayers.size(); r++){
            Object* root = rootsLayers[r];
            std::vector<W3dLayerAnim>& layers = root->clipsHier->layers;
            for (size_t k = 0; k < layers.size(); k++){
                const W3dLayerAnim& c = layers[k];
                if (!c.visible || c.mode != 0 || c.influence < 99.95f) continue;
                const W3dClipHier* clip = W3dHierLayerClip(root, c, 0);
                if (!clip) continue;
                const float f = (float)clip->start + c.gameFrame;
                for (size_t p = 0; p < clip->tracks.size(); p++){
                    Object* n = W3dHierNode(root, clip->tracks[p].path);
                    if (n) ApplyCurvesGameChannels(n, const_cast<std::vector<AnimProperty>&>(clip->tracks[p].props), f, 0);
                }
            }
        }
    }
    return true;
}
// JUEGO: avanza el cabezal de cada capa de escena (fps de SU escena) y aplica la mezcla
void W3dMixScenesTick(float dt){
    std::vector<Object*> rootsLayers;
    RootsWithLayers(rootsLayers);
    if (g_mixScenes.empty() && rootsLayers.empty()) return;
    // las capas de clips de jerarquia: cada una con su cabezal (fps de SU clip)
    for (size_t r = 0; r < rootsLayers.size(); r++){
        std::vector<W3dLayerAnim>& layers = rootsLayers[r]->clipsHier->layers;
        for (size_t k = 0; k < layers.size(); k++){
            W3dLayerAnim& c = layers[k];
            const W3dClipHier* clip = W3dHierLayerClip(rootsLayers[r], c, 0);
            if (!clip) continue;
            const float length = (float)(clip->end - clip->start);
            if (!c.gameEnded) c.gameFrame += dt * (float)(clip->fps > 0 ? clip->fps : 30) * c.vel;
            if (length <= 0.0f || FrameRare(c.gameFrame)) c.gameFrame = 0.0f;
            else if (c.loop) c.gameFrame = W3dCycleWrap(c.gameFrame, length + 1.0f);
            else if (c.gameFrame >= length) { c.gameFrame = length; c.gameEnded = true; }
        }
    }
    InitSceneAnimations();
    for (size_t k = 0; k < g_mixScenes.size(); k++){
        W3dLayerAnim& c = g_mixScenes[k];
        int idx = W3dAnimSceneIdx(c.anim.c_str()); if (idx < 0) continue;
        SceneAnimation* sa = SceneAnimations[idx];
        const float length = (float)(sa->endFrame - sa->startFrame);
        if (!c.gameEnded) c.gameFrame += dt * (float)(sa->fps > 0 ? sa->fps : 30) * c.vel;
        if (length <= 0.0f || FrameRare(c.gameFrame)) c.gameFrame = 0.0f;
        else if (c.loop) c.gameFrame = W3dCycleWrap(c.gameFrame, length + 1.0f);
        else if (c.gameFrame >= length) { c.gameFrame = length; c.gameEnded = true; }
    }
    W3dMixScenesApply(0, true);
}
