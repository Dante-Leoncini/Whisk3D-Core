#include "Animation.h"
#include "W3dNombres.h"   // LA regla de nombres unicos (compartida)
#include "VertexAnimation.h"   // guardar el rango en la animacion propia (kind 3)
#include <algorithm>   // std::sort (fuera el quicksort a mano)
#include "math/Vector3.h"        // EvalPropVec (las 3 curvas de una propiedad)
#include "SkeletalAnimation.h"   // clips de armature (startFrame/endFrame/FrameRate) para el rango por animacion
#include "objects/Armature.h"    // ActiveAnimArm->animations / animActiva
#include "W3dAnimSet.h"          // clips COMPARTIDOS: los setters copian antes de escribir (copy-on-write)
#include "io/W3dRecursos.h"      // el reproductor de clips de jerarquia retiene su biblioteca mientras suena
#include "objects/Mesh.h"        // shape keys (W3dAplicarFormas)
#include "objects/Light.h"       // los colores de una luz (canales AnimColor/AnimAmbient/... en el juego)
#include "Armature2DAnimation.h" // clips del ARMATURE 2D (kind 4): rango/fps propios
#include "w3dlog.h"              // aviso del blindaje del kind 4 (el Core no tiene UI para notificar)
#include <stdio.h> // sprintf GLOBAL (nombre unico de escena); Symbian/STLport no tiene std::snprintf
#include <ctime>
#include <cstring>   // memcmp: comparar keyframes bit a bit (las vistas de los clips de jerarquia)
#include <math.h>    // fmod: un cabezal en loop que se paso MUCHAS vueltas (W3dCicloEnvolver)

// ---- RELOJ del motor (ver Animation.h) ---------------------------------------------------
// Antes esto era una funcion global que el USUARIO del motor tenia que definir con este nombre
// exacto, sin ningun aviso mas que un comentario: el editor la ponia en main.cpp y cualquier
// otro proyecto se comia un "undefined reference" sin saber por que.
static W3dRelojFn gReloj = 0;

void W3dSetReloj(W3dRelojFn fn) { gReloj = fn; }

#ifndef W3D_SYMBIAN   // en Symbian la define w3dskelstubs.cpp (User::NTickCount): dos definiciones = error de link
unsigned int w3dGetTicks() {
    if (gReloj) return gReloj();
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
bool AnimEsJuego = false;          // "Es un juego": timeline infinito (sin Fin)
bool AnimConservarEstados = false; // play hacia adelante no pisa lo ya grabado
int CurrentFrame = 1;

// === Animaciones de ESCENA + seleccion de animacion activa a nivel app ===
std::vector<SceneAnimation*> SceneAnimations;
int SceneAnimActiva = 0;
int ActiveAnimKind = 0;          // 0 escena, 1 clip de armature, 2 juego (lo ponen el Play del editor y el runtime del juego, w3drun), 3 anim del objeto, 4 clip de armature 2D
Armature* ActiveAnimArm = NULL;  // armature del clip activo (kind 1)
Mesh* ActiveAnimMesh = 0;        // kind 3: la malla cuya vertex anim esta en el timeline

void InitSceneAnimations() {
    if (SceneAnimations.empty()) {
        SceneAnimations.push_back(new SceneAnimation("Scene"));
        SceneAnimActiva = 0;
    }
    if (SceneAnimActiva < 0 || SceneAnimActiva >= (int)SceneAnimations.size()) SceneAnimActiva = 0;
}

const char* NombreEscenaActiva() {
    InitSceneAnimations();
    return SceneAnimations[SceneAnimActiva]->name.c_str();
}

// las curvas de objeto CAMBIARON por fuera del cabezal (ver Animation.h)
unsigned int g_animCurvasGen = 0;

void SetEscenaActiva(int idx) {
    InitSceneAnimations();
    if (idx < 0 || idx >= (int)SceneAnimations.size() || idx == SceneAnimActiva) return;
    // la VISTA de un clip de jerarquia que se deja: sus curvas vuelven a su clip (compartido)
    if (W3dClipVistaViva(SceneAnimActiva)) W3dClipVistaEscribir(SceneAnimActiva);
    // guardar las curvas de la escena que estaba activa; cargar las de la nueva (swap: sin copias)
    SceneAnimations[SceneAnimActiva]->objetos.swap(AnimationObjects);
    SceneAnimActiva = idx;
    AnimationObjects.swap(SceneAnimations[SceneAnimActiva]->objetos);
    // ...y la que se elige: si su clip cambio mientras tanto (se edito desde otra raiz), se re-ata
    if (W3dClipVistaViva(idx) && SceneAnimations[idx]->versionVista != SceneAnimations[idx]->clipJer->version)
        W3dClipVistaLeer(idx);
    W3dAnimCurvasInvalidar();   // otras curvas: el editor vuelve a posar aunque el frame sea el mismo
}

int NuevaEscena() {
    InitSceneAnimations();
    // (la VISTA de un clip de jerarquia que se deja vuelve a su clip, como en SetEscenaActiva)
    if (W3dClipVistaViva(SceneAnimActiva)) W3dClipVistaEscribir(SceneAnimActiva);
    // guardar las curvas de la escena activa (AnimationObjects queda vacio = curvas de la escena nueva)
    SceneAnimations[SceneAnimActiva]->objetos.swap(AnimationObjects);
    AnimationObjects.clear();
    // nombre unico con LA regla comun: "Scene", "Scene.001", ... (antes era "Scene N",
    // otra convencion mas de las cinco que habia)
    SceneAnimations.push_back(new SceneAnimation(SceneAnimNombreLibre("Scene", -1)));
    SceneAnimActiva = (int)SceneAnimations.size() - 1;
    W3dAnimCurvasInvalidar();
    return SceneAnimActiva;
}

// nombre libre en el espacio GLOBAL de animaciones de escena ('excepto' conserva el suyo). Las VISTAS de
// los clips de jerarquia no cuentan: sus nombres son los de su biblioteca (W3dJerClipRenombrar)
static bool SceneAnimExiste(const std::string& n, void* ctx) {
    int excepto = *(int*)ctx;
    for (size_t i = 0; i < SceneAnimations.size(); i++)
        if ((int)i != excepto && SceneAnimations[i] && !SceneAnimations[i]->esClip && SceneAnimations[i]->name == n)
            return true;
    return false;
}
std::string SceneAnimNombreLibre(const std::string& base, int excepto) {
    InitSceneAnimations();
    return W3dNombreUnico(base, "Scene", SceneAnimExiste, &excepto);
}
// RENAME: antes no chequeaba NADA (dos escenas podian llamarse igual escribiendolo a mano).
// Devuelve el nombre que quedo.
std::string RenombrarEscenaActiva(const std::string& nombre) {
    InitSceneAnimations();
    SceneAnimation* e = SceneAnimations[SceneAnimActiva];
    // la vista de un CLIP DE JERARQUIA renombra el clip (unico entre los de su biblioteca: lo ven todos)
    if (e->esClip) {
        if (W3dClipVistaViva(SceneAnimActiva)) {
            e->name = W3dJerClipRenombrar(W3dJerAnimSet(e->duenio), e->clipJer, nombre);
            e->versionVista = e->clipJer->version;   // (la vista no quedo vieja: solo cambio el nombre)
        }
        return e->name;
    }
    e->name = SceneAnimNombreLibre(nombre, SceneAnimActiva);
    return e->name;
}

void BorrarEscenaActiva() {
    InitSceneAnimations();
    // la VISTA de un clip de jerarquia: se borra el CLIP de su biblioteca (lo dejan de ver todos sus usuarios;
    // las otras vistas de ese clip quedan huerfanas: ni se listan ni se escriben)
    if (W3dClipVistaViva(SceneAnimActiva)) {
        SceneAnimation* v = SceneAnimations[SceneAnimActiva];
        W3dJerClipBorrar(v->duenio, v->clipJer);
    }
    int pos = -1;
    SceneAnimation* e = W3dEscenaActivaSacar(&pos);
    if (e) { delete e; return; }
    // siempre queda al menos UNA animacion de escena (los clips de objeto no cuentan): borrar la ultima
    // solo vacia sus curvas
    AnimationObjects.clear();
    SceneAnimations[SceneAnimActiva]->objetos.clear();
    W3dAnimCurvasInvalidar();
}

SceneAnimation* W3dEscenaActivaSacar(int* pos) {
    InitSceneAnimations();
    int escenas = 0;
    for (size_t i = 0; i < SceneAnimations.size(); i++) if (!SceneAnimations[i]->esClip) escenas++;
    if (!SceneAnimations[SceneAnimActiva]->esClip && escenas <= 1) return 0;
    const int borrada = SceneAnimActiva;
    SceneAnimation* e = SceneAnimations[borrada];
    e->objetos.clear();
    e->objetos.swap(AnimationObjects);           // sus curvas (las vivas estaban en AnimationObjects) van con ella
    SceneAnimations.erase(SceneAnimations.begin() + borrada);
    // queda activa la que ocupa su lugar (la siguiente; si era la ultima, la anterior), como siempre, pero
    // salteando los clips de objeto: el descarte cae en una ANIMACION DE ESCENA
    int nueva = -1;
    for (int i = borrada; i < (int)SceneAnimations.size() && nueva < 0; i++) if (!SceneAnimations[(size_t)i]->esClip) nueva = i;
    for (int i = borrada - 1; i >= 0 && nueva < 0; i--) if (!SceneAnimations[(size_t)i]->esClip) nueva = i;
    SceneAnimActiva = (nueva < 0) ? 0 : nueva;
    AnimationObjects.swap(SceneAnimations[SceneAnimActiva]->objetos); // cargar las de la escena que quedo activa
    W3dAnimCurvasInvalidar();
    if (pos) *pos = borrada;
    return e;
}

void W3dEscenaDevolver(SceneAnimation* e, int pos) {
    if (!e) return;
    InitSceneAnimations();
    if (pos < 0) pos = 0;
    if (pos > (int)SceneAnimations.size()) pos = (int)SceneAnimations.size();
    // (la vista que se esta editando vuelve a su clip antes de dejarla)
    if (W3dClipVistaViva(SceneAnimActiva)) W3dClipVistaEscribir(SceneAnimActiva);
    SceneAnimations[SceneAnimActiva]->objetos.swap(AnimationObjects);   // la activa guarda sus curvas
    SceneAnimations.insert(SceneAnimations.begin() + pos, e);
    SceneAnimActiva = pos;
    AnimationObjects.clear();
    AnimationObjects.swap(e->objetos);                                  // ...y la devuelta carga las suyas
    if (W3dClipVistaViva(pos) && e->versionVista != e->clipJer->version) W3dClipVistaLeer(pos);
    W3dAnimCurvasInvalidar();
}

bool W3dClipsVistaActivaRevisar() {
    InitSceneAnimations();
    if (ActiveAnimKind != 0 || !W3dAnimEsClip(SceneAnimActiva) || W3dClipVistaViva(SceneAnimActiva)) return false;
    // la animacion de escena mas cercana (siempre hay al menos una)
    int nueva = -1;
    for (int d = 1; d < (int)SceneAnimations.size() && nueva < 0; d++) {
        const int a = SceneAnimActiva - d, b = SceneAnimActiva + d;
        if (a >= 0 && !SceneAnimations[(size_t)a]->esClip) nueva = a;
        else if (b < (int)SceneAnimations.size() && !SceneAnimations[(size_t)b]->esClip) nueva = b;
    }
    if (nueva < 0) return false;
    SetEscenaActiva(nueva);
    AnimCargarRangoActivo();
    return true;
}

// ===== Start/End/FPS PROPIOS por animacion (escena o clip). Los comparten la tarjeta y el timeline. =====
static SkeletalAnimation* AnimClipActivo(){
    if (ActiveAnimKind == 1 && ActiveAnimArm &&
        ActiveAnimArm->animActiva >= 0 && ActiveAnimArm->animActiva < (int)ActiveAnimArm->animations.size())
        return ActiveAnimArm->animations[ActiveAnimArm->animActiva];
    return NULL;
}
// el mismo clip, para ESCRIBIRLE el rango/fps: si es de un animset compartido, el armature pasa
// antes a clips propios (copy-on-write) y el resto de los que lo usan no se entera. Los setters
// lo piden SOLO si el valor cambia (confirmar un campo sin tocarlo no copia nada)
static SkeletalAnimation* AnimClipActivoEditable(){
    if (AnimClipActivo()) W3dArmatureAnimsPropias(ActiveAnimArm);
    return AnimClipActivo();
}
// el clip del ARMATURE 2D activo (kind 4), o NULL
static Armature2DAnimation* AnimClip2DActivo(){
    if (ActiveAnimKind != 4 || !ActiveAnimMesh) return NULL;
    return Arm2DClipActivo(ActiveAnimMesh);
}
void AnimCargarRangoActivo(){
    Armature2DAnimation* c2 = AnimClip2DActivo();
    if (c2){ StartFrame = c2->startFrame; EndFrame = c2->endFrame; if (c2->fps > 0) AnimFPS = c2->fps;
             if (EndFrame < StartFrame) EndFrame = StartFrame;
             if (CurrentFrame < StartFrame) CurrentFrame = StartFrame;
             if (CurrentFrame > EndFrame && EndFrame >= StartFrame) CurrentFrame = EndFrame;
             return; }
    SkeletalAnimation* c = AnimClipActivo();
    if (c){ StartFrame = c->startFrame; EndFrame = c->endFrame; if (c->FrameRate > 0) AnimFPS = c->FrameRate; }
    else { InitSceneAnimations(); SceneAnimation* s = SceneAnimations[SceneAnimActiva];
           StartFrame = s->startFrame; EndFrame = s->endFrame; AnimFPS = s->fps; }
    if (CurrentFrame < StartFrame) CurrentFrame = StartFrame;
    if (CurrentFrame > EndFrame && EndFrame >= StartFrame) CurrentFrame = EndFrame;
}
// la animacion PROPIA activa del objeto (kind 3), o NULL
static VertexAnimation* AnimObjPropiaActiva(){
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
// AnimClip2DActivo deja de ser NULL y los campos vuelven a funcionar solos.
static bool AnimKind4SinClip(){
    if (ActiveAnimKind != 4) return false;
    if (AnimClip2DActivo()) return false;
    w3dLogfW("Animation: kind 4 (clip de armature 2D) sin clip activo -> el rango no se escribe en ningun lado");
    return true;
}
// MIX (g_animMix, SkeletalAnimation.cpp): el rango del timeline es el del MIX, no el del clip de la capa elegida
extern bool g_animMix; extern float g_mixInicio, g_mixFin;
void AnimSetStart(int v){ if (g_animMix) { g_mixInicio = (float)v; StartFrame = v; return; }
    if (AnimKind4SinClip()) return;
    Armature2DAnimation* c2 = AnimClip2DActivo(); if (c2) c2->startFrame = v;
    else { VertexAnimation* oa = AnimObjPropiaActiva(); if (oa) oa->startFrame = v;
    else { SkeletalAnimation* c = AnimClipActivo(); if (c) { if (c->startFrame != v) AnimClipActivoEditable()->startFrame = v; }
    else { InitSceneAnimations(); SceneAnimations[SceneAnimActiva]->startFrame = v; } } } StartFrame = v; }
void AnimSetEnd(int v){ if (g_animMix) { g_mixFin = (float)v; EndFrame = v; return; }
    if (AnimKind4SinClip()) return;
    Armature2DAnimation* c2 = AnimClip2DActivo(); if (c2) c2->endFrame = v;
    else { VertexAnimation* oa = AnimObjPropiaActiva(); if (oa) oa->endFrame = v;
    else { SkeletalAnimation* c = AnimClipActivo(); if (c) { if (c->endFrame != v) AnimClipActivoEditable()->endFrame = v; }
    else { InitSceneAnimations(); SceneAnimations[SceneAnimActiva]->endFrame = v; } } } EndFrame = v; }
void AnimSetFps(int v){ if (AnimKind4SinClip()) return;
    Armature2DAnimation* c2 = AnimClip2DActivo(); if (c2) c2->fps = v;
    else { VertexAnimation* oa = AnimObjPropiaActiva(); if (oa) oa->fps = v;
    else { SkeletalAnimation* c = AnimClipActivo(); if (c) { if (c->FrameRate != v) AnimClipActivoEditable()->FrameRate = v; }
    else { InitSceneAnimations(); SceneAnimations[SceneAnimActiva]->fps = v; } } } AnimFPS = v; }

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
//  W3dClaves: las claves de una curva, comunes o COMPACTAS (ver Animation.h)
// ============================================================================
namespace {
long gClavesBytes = 0;             // bytes de claves vivos: capacidad de los vectores + bloques compactos
long gClavesCompactas = 0;         // curvas compactas vivas, sus claves y sus bloques
long gClavesCompactasN = 0;
long gClavesCompactasBytes = 0;
long gClavesExpansiones = 0;       // compacta -> comun, desde el arranque

size_t BytesBloque(unsigned n) { return sizeof(W3dClavesCompactas) + (size_t)n * sizeof(float); }
W3dClavesCompactas* BloqueNuevo(unsigned n) {
    W3dClavesCompactas* c = static_cast<W3dClavesCompactas*>(::operator new(BytesBloque(n)));
    c->primero = 0; c->paso = 1; c->interp = KfLinear; c->tipoH = HAuto; c->n = n;
    gClavesCompactas++; gClavesCompactasN += (long)n;
    gClavesCompactasBytes += (long)BytesBloque(n); gClavesBytes += (long)BytesBloque(n);
    return c;
}
void BloqueSoltar(W3dClavesCompactas* c) {
    gClavesCompactas--; gClavesCompactasN -= (long)c->n;
    gClavesCompactasBytes -= (long)BytesBloque(c->n); gClavesBytes -= (long)BytesBloque(c->n);
    ::operator delete(c);
}
W3dClavesCompactas* BloqueCopia(const W3dClavesCompactas* o) {
    W3dClavesCompactas* c = BloqueNuevo(o->n);
    memcpy(c, o, BytesBloque(o->n));   // (cabecera + valores: datos planos, los bits tal cual)
    return c;
}
long BytesVec(size_t capacidad) { return (long)(capacidad * sizeof(keyFrame)); }
} // namespace

void W3dClavesEstadisticas(W3dClavesStats& st) {
    st.bytes = gClavesBytes; st.compactas = gClavesCompactas; st.clavesCompactas = gClavesCompactasN;
    st.bytesCompactas = gClavesCompactasBytes; st.expansiones = gClavesExpansiones;
}

W3dClaves::W3dClaves(const W3dClaves& o) : v_(o.v_), c_(o.c_ ? BloqueCopia(o.c_) : 0) {
    gClavesBytes += BytesVec(v_.capacity());
}
W3dClaves::~W3dClaves() {
    gClavesBytes -= BytesVec(v_.capacity());
    if (c_) BloqueSoltar(c_);
}
W3dClaves& W3dClaves::operator=(const W3dClaves& o) {
    if (this == &o) return *this;
    if (o.c_) {
        // una compacta se copia compacta, y el vector que hubiera se suelta (no queda reservado a 32 B por clave)
        W3dClavesCompactas* c = BloqueCopia(o.c_);
        SoltarCompacta();
        gClavesBytes -= BytesVec(v_.capacity());
        Vec().swap(v_);
        c_ = c;
    } else {
        SoltarCompacta();
        const size_type antes = v_.capacity();
        v_ = o.v_;
        ContarCapacidad(antes);
    }
    return *this;
}
W3dClaves& W3dClaves::operator=(const Vec& v) {
    SoltarCompacta();
    const size_type antes = v_.capacity();
    v_ = v;
    ContarCapacidad(antes);
    return *this;
}
void W3dClaves::SoltarCompacta() {
    if (c_) { BloqueSoltar(c_); c_ = 0; }
}
void W3dClaves::ContarCapacidad(size_type antes) {
    gClavesBytes += BytesVec(v_.capacity()) - BytesVec(antes);
}
// la compacta pasa a ser una curva comun con las MISMAS claves: lo que leeria el .w3da sin compactar (frames
// sumando el paso, la interpolacion y el tipo de handle de todas, handles 0.0f)
void W3dClaves::ExpandirAhora() const {
    const W3dClavesCompactas* c = c_;
    Vec tmp(c->n);
    const float* val = c->Valores();
    for (unsigned i = 0; i < c->n; i++) {
        keyFrame& k = tmp[i];
        k.frame = c->Frame(i); k.value = val[i]; k.Interpolation = c->interp; k.handleType = c->tipoH;
    }
    const size_type antes = v_.capacity();   // (0: vacio mientras era compacta)
    v_.swap(tmp);
    gClavesBytes += BytesVec(v_.capacity()) - BytesVec(antes);
    BloqueSoltar(c_);
    c_ = 0;
    gClavesExpansiones++;
}
keyFrame W3dClaves::Clave(size_type i) const {
    if (!c_) return v_[i];
    keyFrame k;
    k.frame = c_->Frame((unsigned)i); k.value = c_->Valores()[i];
    k.Interpolation = c_->interp; k.handleType = c_->tipoH;
    return k;
}
long W3dClaves::BytesMemoria() const {
    return c_ ? (long)BytesBloque(c_->n) : BytesVec(v_.capacity());
}
bool W3dClaves::MismosBits(const W3dClaves& o) const {
    const size_type n = size();
    if (n != o.size()) return false;
    if (c_ && o.c_)   // (las mismas claves = la misma cabecera: frames regulares, interp y tipo de handle de todas)
        return c_->primero == o.c_->primero && c_->paso == o.c_->paso && c_->interp == o.c_->interp &&
               c_->tipoH == o.c_->tipoH && memcmp(c_->Valores(), o.c_->Valores(), n * sizeof(float)) == 0;
    for (size_type i = 0; i < n; i++) {
        const keyFrame x = Clave(i), y = o.Clave(i);
        if (x.frame != y.frame || x.Interpolation != y.Interpolation || x.handleType != y.handleType) return false;
        if (memcmp(&x.value, &y.value, sizeof(float)) || memcmp(&x.inDF, &y.inDF, sizeof(float)) ||
            memcmp(&x.inDV, &y.inDV, sizeof(float)) || memcmp(&x.outDF, &y.outDF, sizeof(float)) ||
            memcmp(&x.outDV, &y.outDV, sizeof(float))) return false;
    }
    return true;
}
bool W3dClaves::CompactaPosible(size_type n, int primero, int paso) {
    // (el ultimo frame tiene que entrar en un int: es el que el lector obtiene sumando el paso)
    return n >= 2 && n <= 0xFFFFFFFFu && paso > 0 &&
           (double)primero + (double)paso * (double)(n - 1) <= 2147483647.0;
}
float* W3dClaves::CompactaCrear(size_type n, int primero, int paso, int interp, int tipoH) {
    SoltarCompacta();
    gClavesBytes -= BytesVec(v_.capacity());
    Vec().swap(v_);
    c_ = BloqueNuevo((unsigned)n);
    c_->primero = primero; c_->paso = paso; c_->interp = interp; c_->tipoH = tipoH;
    return c_->Valores();
}
void W3dClaves::push_back(const keyFrame& k) {
    Expandir(); const size_type antes = v_.capacity(); v_.push_back(k); ContarCapacidad(antes);
}
W3dClaves::iterator W3dClaves::insert(iterator pos, const keyFrame& k) {
    Expandir(); const size_type antes = v_.capacity(); iterator r = v_.insert(pos, k); ContarCapacidad(antes);
    return r;
}
void W3dClaves::insert(iterator pos, size_type n, const keyFrame& k) {
    Expandir(); const size_type antes = v_.capacity(); v_.insert(pos, n, k); ContarCapacidad(antes);
}
void W3dClaves::clear() { SoltarCompacta(); v_.clear(); }
void W3dClaves::resize(size_type n, const keyFrame& k) {
    Expandir(); const size_type antes = v_.capacity(); v_.resize(n, k); ContarCapacidad(antes);
}
void W3dClaves::reserve(size_type n) {
    Expandir(); const size_type antes = v_.capacity(); v_.reserve(n); ContarCapacidad(antes);
}
void W3dClaves::assign(size_type n, const keyFrame& k) {
    SoltarCompacta(); const size_type antes = v_.capacity(); v_.assign(n, k); ContarCapacidad(antes);
}
void W3dClaves::swap(W3dClaves& o) {   // (los bytes vivos no cambian: solo cambian de duenio)
    W3dClavesCompactas* c = c_; c_ = o.c_; o.c_ = c;
    v_.swap(o.v_);
}
void W3dClaves::swap(Vec& v) {
    Expandir(); const size_type antes = v_.capacity(); v_.swap(v); ContarCapacidad(antes);
}

// orden por numero de frame (para std::sort)
static bool compareKeyFrames(const keyFrame& a, const keyFrame& b) {
    return a.frame < b.frame;
}

// AnimProperty
void AnimProperty::SortKeyFrames() {
    if (keyframes.Compacta()) return;   // (frames primero + paso * i con paso > 0: ya esta ordenada)
    std::sort(keyframes.begin(), keyframes.end(), compareKeyFrames);
}

// ============================================================================
//  EVALUACION de UNA curva (un canal). Es el UNICO lugar donde se interpola: el esqueleto y los objetos
//  llaman aca, asi que agregar bezier/easing se hace en un solo sitio (y el editor de curvas lo hereda).
// ============================================================================
// Handle EFECTIVO del keyframe i (offset dF/dV desde el). Ver el enum H* en Animation.h.
// (Lee las claves con Frame/Valor/TipoH/HandleDF/DV de W3dClaves: no expande una curva COMPACTA y es el MISMO
//  codigo para las dos, asi que dan los mismos bits)
void AnimProperty::HandleEfectivo(size_t i, bool salida, float& dF, float& dV) const {
    const W3dClaves& K = keyframes;
    const size_t n = K.size();
    const int kFrame = K.Frame(i);
    const float kValue = K.Valor(i);
    const int kTipo = K.TipoH(i);
    dF = 0.0f; dV = 0.0f;
    if (n < 2) return;

    // el vecino de ESTE lado (en las puntas se usa el unico que hay: el handle sale simetrico)
    size_t vec = salida ? (i + 1 < n ? i + 1 : (i > 0 ? i - 1 : i))
                        : (i > 0 ? i - 1 : (i + 1 < n ? i + 1 : i));
    // largo por defecto: un tercio del tramo de este lado, con el signo del lado
    float largo = (float)(K.Frame(vec) - kFrame) / 3.0f;
    if (salida  && largo <= 0.0f) largo =  -largo;   // en la punta derecha el vecino esta a la IZQUIERDA
    if (!salida && largo >= 0.0f) largo =  -largo;
    if (largo == 0.0f) largo = salida ? 1.0f : -1.0f;

    if (kTipo == HFree || kTipo == HAligned){
        dF = K.HandleDF(i, salida);
        dV = K.HandleDV(i, salida);
        return;
    }
    if (kTipo == HVector){
        // apunta al vecino: el tramo de este lado sale RECTO
        int df = K.Frame(vec) - kFrame;
        if (df == 0){ dF = largo; dV = 0.0f; return; }
        float t = largo / (float)df;                        // fraccion hacia el vecino (1/3, con el signo del lado)
        dF = largo;
        dV = (K.Valor(vec) - kValue) * t;
        return;
    }
    // HAuto / HAutoClamped: pendiente suave (Catmull-Rom) por los dos vecinos
    size_t a = (i == 0) ? 0 : i - 1;
    size_t b = (i + 1 >= n) ? n - 1 : i + 1;
    int spanAB = K.Frame(b) - K.Frame(a);
    float m = (spanAB != 0) ? (K.Valor(b) - K.Valor(a)) / (float)spanAB : 0.0f;
    if (kTipo == HAutoClamped && i > 0 && i + 1 < n){
        // PICO: si los dos vecinos caen del mismo lado, este keyframe es un maximo/minimo local -> handle PLANO,
        // asi la curva no se pasa de largo (que es justo lo que arruina una animacion: un rebote que se hunde
        // por debajo del piso).
        float dPrev = K.Valor(i-1) - kValue;
        float dNext = K.Valor(i+1) - kValue;
        if ((dPrev > 0.0f && dNext > 0.0f) || (dPrev < 0.0f && dNext < 0.0f)) m = 0.0f;
    }
    dF = largo;
    dV = m * largo;
    if (kTipo == HAutoClamped){
        // ...y ademas el punto de control no puede irse mas alla del valor del VECINO. Aplanar solo los picos no
        // alcanza: el handle de un keyframe que NO es pico igual puede empujar la curva por encima del vecino
        // (una bezier vive dentro de la cascara de sus puntos de control, asi que acotando el control se acota la
        // curva). Esto es lo que hace que "clamped" de verdad no se pase.
        float vv = K.Valor(vec);
        float lo = (kValue < vv) ? kValue : vv;
        float hi = (kValue > vv) ? kValue : vv;
        float cp = kValue + dV;
        if (cp < lo) dV = lo - kValue;
        else if (cp > hi) dV = hi - kValue;
    }
}

float AnimProperty::Eval(int frame, float def) const { return EvalF((float)frame, def); }

// el primer i >= 1 cuyo frame (en float) es >= 'frame', con Frame(0) < frame < Frame(n-1) (los dos clamps de
// EvalF ya pasaron): el tramo [i-1, i] que se evalua. La curva comun la RECORRE (como siempre: no supone que
// este ordenada). La COMPACTA (frames primero + paso * i, crecientes) llega al MISMO i sin recorrer: lo estima
// con una division y lo corrige con las mismas comparaciones en float (un frame enorme no es exacto en float).
static size_t TramoDe(const W3dClaves& K, float frame) {
    const size_t n = K.size();
    const W3dClavesCompactas* c = K.DatosCompactos();
    if (!c) {
        size_t i = 1;
        while (i < n && (float)K.Frame(i) < frame) i++;
        return i;
    }
    size_t i = 1;
    if (frame == frame) {   // (NaN: la curva comun se queda en el primer tramo; aca tambien)
        const double x = ceil(((double)frame - (double)c->primero) / (double)c->paso);
        if (x > 1.0) i = (x >= (double)(n - 1)) ? n - 1 : (size_t)x;
    }
    while (i > 1 && (float)K.Frame(i - 1) >= frame) i--;
    while (i < n - 1 && (float)K.Frame(i) < frame) i++;
    return i;
}

// Una sola funcion para la curva comun y la COMPACTA (W3dClaves: Frame/Valor/Interp no expanden): despues de
// elegir el tramo, la cuenta es el mismo codigo, asi que las dos dan los mismos bits (prueba_claves_compactas.w3s).
float AnimProperty::EvalF(float frame, float def) const {
    const W3dClaves& K = keyframes;
    const size_t n = K.size();
    if (n == 0) return def;                                         // canal sin keyframes -> su valor de reposo
    if (frame <= (float)K.Frame(0))     return K.Valor(0);          // antes del 1ro: clamp
    if (frame >= (float)K.Frame(n-1))   return K.Valor(n-1);        // despues del ultimo: clamp
    const size_t i = TramoDe(K, frame);
    if (i >= n) return K.Valor(n-1);                                // (curva desordenada: como siempre)
    const int interp = K.Interp(i-1);
    const int fa = K.Frame(i-1), fb = K.Frame(i);
    const float va = K.Valor(i-1), vb = K.Valor(i);
    // ESCALON: mantiene va hasta el proximo key... pero EN el frame de b ya vale vb. El tramo se elige con
    // frame de b >= frame, asi que aca 'frame' puede ser EXACTAMENTE fb: sin este chequeo el escalon caia un
    // frame tarde (y el editor lo dibuja en b, con lo cual lo que veias no era lo que corria).
    if (interp == KfConstant) return (frame >= (float)fb) ? vb : va;
    int span = fb - fa;
    if (span <= 0) return vb;
    float t = (frame - (float)fa) / (float)span;
    if (interp != KfBezier)
        return va + (vb - va) * t;                                  // lineal
    return EvalBezier(i, frame);
}

// Valor del tramo BEZIER [i-1, i] en 'frame'. Los handles son PUNTOS, asi que el tramo es una bezier cubica en
// (frame, valor): x(t) e y(t). Para saber el valor en un frame hay que despejar el t cuyo x(t) es ese frame — no
// alcanza con meter t = fraccion del tramo, porque los handles corren el x. Es lo mismo que hace una curva de
// easing: el eje del tiempo tambien esta curvado.
float AnimProperty::EvalBezier(size_t i, float frame) const {
    const W3dClaves& K = keyframes;   // (Frame/Valor: no expande una curva compacta)
    const float aValue = K.Valor(i-1), bValue = K.Valor(i);
    const float x0 = (float)K.Frame(i-1), x3 = (float)K.Frame(i);
    const float span = x3 - x0;
    if (span <= 0.0f) return bValue;

    float aDF, aDV, bDF, bDV;
    HandleEfectivo(i-1, true,  aDF, aDV);   // handle de SALIDA del izquierdo
    HandleEfectivo(i,   false, bDF, bDV);   // handle de ENTRADA del derecho
    float x1 = x0 + aDF, x2 = x3 + bDF;
    // x TIENE que ser monotono o la curva deja de ser una funcion del tiempo (se doblaria hacia atras y un frame
    // tendria dos valores). Los puntos de control se clampean al tramo: es lo mismo que hace cualquier editor.
    if (x1 < x0) x1 = x0; if (x1 > x3) x1 = x3;
    if (x2 < x0) x2 = x0; if (x2 > x3) x2 = x3;
    const float y0 = aValue, y1 = aValue + aDV, y2 = bValue + bDV, y3 = bValue;

    // despejar t / x(t) = frame, por biseccion. x(t) es monotona creciente (recien clampeada), asi que converge
    // siempre; 24 pasos dan 1/16M del tramo, de sobra para un frame.
    float lo = 0.0f, hi = 1.0f, t = 0.5f;
    for (int it = 0; it < 24; it++){
        t = (lo + hi) * 0.5f;
        float u = 1.0f - t;
        float x = u*u*u*x0 + 3.0f*u*u*t*x1 + 3.0f*u*t*t*x2 + t*t*t*x3;
        if (x < frame) lo = t; else hi = t;
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
// se leen aca: con handleType automatico, HandleEfectivo las RECALCULA desde los vecinos, y despues del erase los
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

void BorrarKeyframeManteniendoForma(AnimProperty& ap, int frame){
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
    ap.HandleEfectivo(i-1, true,  aDF, aDV);   // como SALE de A
    ap.HandleEfectivo(i+1, false, cDF, cDV);   // como ENTRA a C
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

AnimProperty& PropertyDeLista(std::vector<AnimProperty>& props, int property, int component) {
    for (size_t i = 0; i < props.size(); i++)
        if (props[i].Property == property && props[i].component == component) return props[i];
    AnimProperty p; p.Property = property; p.component = component;
    props.push_back(p);
    return props.back();
}

void SetKeyCurva(AnimProperty& ap, int frame, float value) {
    for (size_t i = 0; i < ap.keyframes.size(); i++)
        if (ap.keyframes[i].frame == frame) { ap.keyframes[i].value = value; return; } // ya hay uno: se actualiza
    keyFrame kf; kf.frame = frame; kf.value = value; kf.Interpolation = KfBezier; // default suave; los canales true/false lo pisan a KfConstant
    size_t pos = 0; while (pos < ap.keyframes.size() && ap.keyframes[pos].frame < frame) pos++;
    ap.keyframes.insert(ap.keyframes.begin() + pos, kf);   // insertado EN ORDEN (no hace falta ordenar)
}

// AnimationObject
void AnimationObject::UpdateFirstLastFrame() {
    FirstKeyFrame = 100000000;
    LastKeyFrame = 0;
    for(size_t pr = 0; pr < Propertys.size(); pr++) {
        const W3dClaves& k = Propertys[pr].keyframes;   // (Frame(i): mirar no expande una curva compacta)
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
// (invariante del swap de SetEscenaActiva; misma regla que el guardado)
static std::vector<AnimationObject>* ObjetosDeEscena(int i) {
    if (i < 0 || i >= (int)SceneAnimations.size() || !SceneAnimations[i]) return NULL;
    return (i == SceneAnimActiva) ? &AnimationObjects : &SceneAnimations[i]->objetos;
}

static int   gEscJuego     = -1;     // indice sonando (-1 = nada)
static float gEscJuegoF    = 0.0f;   // frame FLOTANTE (avanza por dt * fps)
static bool  gEscJuegoLoop = true;
static bool  gEscJuegoManual = false;  // el frame lo fija el script (animEscenaFrame): el tick no avanza su reloj

int W3dAnimEscenaIdx(const char* nombre) {
    if (!nombre) return -1;
    // (los CLIPS DE OBJETO no: esos se piden por su dueno, animObjeto)
    for (size_t i = 0; i < SceneAnimations.size(); i++)
        if (SceneAnimations[i] && !SceneAnimations[i]->esClip && SceneAnimations[i]->name == nombre) return (int)i;
    return -1;
}

float W3dAnimEscenaDur(int idx) {
    if (idx < 0 || idx >= (int)SceneAnimations.size() || !SceneAnimations[idx]) return 0.0f;
    const SceneAnimation* e = SceneAnimations[idx];
    int fps = (e->fps > 0) ? e->fps : 30;
    int n = e->endFrame - e->startFrame + 1;
    return (n > 0 ? (float)n : 1.0f) / (float)fps;
}

bool W3dAnimEscenaPlay(int idx, bool loop) {
    if (!ObjetosDeEscena(idx)) return false;
    gEscJuego = idx;
    gEscJuegoF = (float)SceneAnimations[idx]->startFrame;
    gEscJuegoLoop = loop;
    gEscJuegoManual = false;
    return true;
}

void W3dAnimEscenaReset() { gEscJuego = -1; gEscJuegoManual = false; }

void W3dAnimEscenaGuardarEstado(W3dAnimEscenaEstado* e) {
    if (!e) return;
    e->idx = gEscJuego; e->frame = gEscJuegoF; e->loop = gEscJuegoLoop; e->manual = gEscJuegoManual;
}
void W3dAnimEscenaReponerEstado(const W3dAnimEscenaEstado& e) {
    // (la lista de animaciones es la de la raiz que vuelve: el indice se valida contra ELLA)
    gEscJuego = (e.idx >= 0 && e.idx < (int)SceneAnimations.size()) ? e.idx : -1;
    gEscJuegoF = e.frame; gEscJuegoLoop = e.loop; gEscJuegoManual = e.manual;
}
bool W3dAnimEscenaTermino() {
    if (gEscJuego < 0 || gEscJuego >= (int)SceneAnimations.size() || !SceneAnimations[gEscJuego]) return true;
    return !gEscJuegoLoop && gEscJuegoF >= (float)SceneAnimations[gEscJuego]->endFrame;
}

void W3dAnimEscenaFijar(float frame) {
    if (gEscJuego < 0) return;
    gEscJuegoF = frame;
    gEscJuegoManual = true;
}

// ---- SHAPE KEYS animadas: canal AnimShape, componente = indice de la forma ----
void (*g_animPasoHook)(Object* rutina, int componente, float valor) = 0;
void W3dAplicarFormas(Object* o, std::vector<AnimProperty>& props, int frame) {
    if (!o) return;
    if (o->getType() == ObjectType::rutina) {   // los PASOS de una rutina (AnimPaso)
        if (g_animPasoHook)
            for (size_t p = 0; p < props.size(); p++)
                if (props[p].Property == AnimPaso && !props[p].keyframes.empty())
                    g_animPasoHook(o, props[p].component, props[p].Eval(frame, 0.0f));
        return;
    }
    if (o->getType() != ObjectType::mesh) return;
    Mesh* m = (Mesh*)o;
    for (size_t p = 0; p < props.size(); p++)
        if (props[p].Property == AnimShape && !props[p].keyframes.empty())
            m->SetShapePeso(props[p].component, props[p].Eval(frame, 0.0f));
}

// ---- PISTA "CAMARA ACTIVA" ----
void (*g_camaraActivaHook)(const char* nombre) = 0;
void (*g_animFovHook)(Object* camara, float fov) = 0;

static SceneAnimation* EscenaIdx(int i) {
    return (i >= 0 && i < (int)SceneAnimations.size()) ? SceneAnimations[i] : NULL;
}

AnimProperty* W3dCamPista(int escena, bool crear) {
    SceneAnimation* e = EscenaIdx(escena);
    if (!e) return NULL;
    if (e->camPista.empty()) {
        if (!crear) return NULL;
        AnimProperty ap; ap.Property = AnimCamActiva; ap.component = AnimX;
        e->camPista.push_back(ap);
    }
    return &e->camPista[0];
}

int W3dCamNombreIdx(int escena, const std::string& nombre) {
    SceneAnimation* e = EscenaIdx(escena);
    if (!e) return -1;
    for (size_t i = 0; i < e->camNombres.size(); i++) if (e->camNombres[i] == nombre) return (int)i;
    e->camNombres.push_back(nombre);
    return (int)e->camNombres.size() - 1;
}

void W3dCamKey(int escena, int frame, const std::string& nombre) {
    AnimProperty* ap = W3dCamPista(escena, true);
    if (!ap) return;
    SetKeyCurva(*ap, frame, (float)W3dCamNombreIdx(escena, nombre));
    for (size_t i = 0; i < ap->keyframes.size(); i++) ap->keyframes[i].Interpolation = KfConstant;   // cortes
}

const char* W3dCamEnFrame(int escena, int frame) {
    SceneAnimation* e = EscenaIdx(escena);
    if (!e || e->camPista.empty() || e->camPista[0].keyframes.empty()) return NULL;
    const W3dClaves& k = e->camPista[0].keyframes;   // (Frame/Valor: el juego no expande la pista)
    size_t j = 0;
    for (size_t i = 0; i < k.size(); i++) { if (k.Frame(i) <= frame) j = i; else break; }
    const int idx = (int)(k.Valor(j) + 0.5f);
    return (idx >= 0 && idx < (int)e->camNombres.size()) ? e->camNombres[idx].c_str() : NULL;
}

void W3dCamActivaAplicar(int escena, int frame) {
    if (!g_camaraActivaHook) return;
    const char* n = W3dCamEnFrame(escena, frame);
    if (n) g_camaraActivaHook(n);
}

// los canales de OBJETO de una lista de curvas, en 'frame' (la parte que el
// juego necesita de W3dAplicarCurvasEnFrame del editor)
// 'fr' CONTINUO (con fraccion): transformacion y fov se interpolan entre frames (la camara de una cinematica se mueve
// suave igual que los cuerpos, ver EvaluarPoseEsqueleto); visibilidad y formas van por el frame entero.
// 'transform' = false: solo los canales que NO se mezclan (visible/render, formas, fov, luz): lo que una capa
// de escena a peso entero aplica ademas de su transformacion mezclada (W3dMixEscenasAplicar)
// 'transform': mascara de los canales de transformacion que se aplican (kCanal*): el reproductor de clips
// de jerarquia con retarget "rotaciones" deja a los descendientes solo con su rotacion
enum { kCanalPos = 1, kCanalRot = 2, kCanalEsc = 4, kCanalTodos = 7 };
static void AplicarCurvasJuegoCanales(Object* o, std::vector<AnimProperty>& props, float fr, int transform) {
    const int frame = (int)fr;
    bool hayP = false, hayR = false, hayS = false, hayV = false, hayRnd = false;
    bool hayCol = false, hayAmb = false, haySpec = false, hayAtt = false, haySpot = false;
    for (size_t p = 0; p < props.size(); p++) {
        if (props[p].keyframes.empty()) continue;
        int P = props[p].Property;
        if      (P == AnimPosition) hayP = true;
        else if (P == AnimRotation) hayR = true;
        else if (P == AnimScale)    hayS = true;
        else if (P == AnimVisible)  hayV = true;
        else if (P == AnimRender)   hayRnd = true;
        else if (P == AnimColor)    hayCol = true;
        else if (P == AnimAmbient)  hayAmb = true;
        else if (P == AnimSpecular) haySpec = true;
        else if (P == AnimAtten)    hayAtt = true;
        else if (P == AnimSpot)     haySpot = true;
    }
    if (hayP && (transform & kCanalPos)) o->pos   = EvalPropVecF(props, AnimPosition, fr, o->pos);
    if (hayS && (transform & kCanalEsc)) o->scale = EvalPropVecF(props, AnimScale,    fr, o->scale);
    if (hayR && (transform & kCanalRot)) { Vector3 e = EvalPropVecF(props, AnimRotation, fr, o->rotEuler); o->SetRotEuler(e); }
    if (hayV)   o->visible      = PropertyDeLista(props, AnimVisible, AnimX).Eval(frame, o->visible ? 1.0f : 0.0f) >= 0.5f;
    if (hayRnd) o->renderizable = PropertyDeLista(props, AnimRender,  AnimX).Eval(frame, o->renderizable ? 1.0f : 0.0f) >= 0.5f;
    // (ver W3dAplicarFormas)
    W3dAplicarFormas(o, props, frame);
    // FOV de camara (cinematicas: el zoom de cada corte). El Core no conoce Camera: lo aplica el hook de main/
    if (g_animFovHook)
        for (size_t p = 0; p < props.size(); p++)
            if (props[p].Property == AnimFov && !props[p].keyframes.empty()) {
                g_animFovHook(o, props[p].EvalF(fr, 45.0f));
                break;
            }
    // LUZ: los colores, la atenuacion y el spot (el cambio de GL_LIGHTn / direccional es del editor)
    if (o->getType() == ObjectType::light && (hayCol || hayAmb || haySpec || hayAtt || haySpot)) {
        Light* l = (Light*)o;
        if (hayCol)  { Vector3 d(l->diffuse[0], l->diffuse[1], l->diffuse[2]);
                       Vector3 v = EvalPropVecF(props, AnimColor, fr, d);    l->diffuse[0] = v.x; l->diffuse[1] = v.y; l->diffuse[2] = v.z; }
        if (hayAmb)  { Vector3 d(l->ambient[0], l->ambient[1], l->ambient[2]);
                       Vector3 v = EvalPropVecF(props, AnimAmbient, fr, d);  l->ambient[0] = v.x; l->ambient[1] = v.y; l->ambient[2] = v.z; }
        if (haySpec) { Vector3 d(l->specular[0], l->specular[1], l->specular[2]);
                       Vector3 v = EvalPropVecF(props, AnimSpecular, fr, d); l->specular[0] = v.x; l->specular[1] = v.y; l->specular[2] = v.z; }
        if (hayAtt)  { l->attConstant  = PropertyDeLista(props, AnimAtten, AnimX).EvalF(fr, l->attConstant);
                       l->attLinear    = PropertyDeLista(props, AnimAtten, AnimY).EvalF(fr, l->attLinear);
                       l->attQuadratic = PropertyDeLista(props, AnimAtten, AnimZ).EvalF(fr, l->attQuadratic); }
        if (haySpot) { l->spotCutoff   = PropertyDeLista(props, AnimSpot, AnimX).EvalF(fr, l->spotCutoff);
                       l->spotExponent = PropertyDeLista(props, AnimSpot, AnimY).EvalF(fr, l->spotExponent); }
    }
}
void W3dAplicarCurvasJuego(Object* o, std::vector<AnimProperty>& props, float fr) {
    if (o) AplicarCurvasJuegoCanales(o, props, fr, kCanalTodos);
}
static void AplicarCurvasJuego(Object* o, std::vector<AnimProperty>& props, float fr) {
    AplicarCurvasJuegoCanales(o, props, fr, kCanalTodos);
}

// ---- LOS CABEZALES EN LOOP (ver W3dCicloEnvolver en Animation.h) ----
static bool FrameRaro(float f) { return f != f || f > 3.0e38f || f < -3.0e38f; }   // NaN o infinito
float W3dCicloEnvolver(float f, float periodo) {
    if (!(periodo > 0.0f) || FrameRaro(f)) return 0.0f;
    if (f >= periodo * 64.0f) f = (float)fmod((double)f, (double)periodo);   // (un salto enorme: de una vez)
    while (f >= periodo) f -= periodo;   // (las pocas vueltas de siempre: lo mismo que antes)
    return f;
}
// un cabezal de [ini, fin] (largo = fin - ini + 1) que se paso por ARRIBA: las vueltas que hagan falta y, si
// cae antes del inicio, el inicio (la regla de siempre de los reproductores). Infinito o NaN = el inicio.
static float EnvolverArriba(float f, float ini, float fin, float largo) {
    if (FrameRaro(f)) return ini;
    if ((double)f - (double)fin > (double)largo * 64.0)
        f = (float)((double)fin + fmod((double)f - (double)fin, (double)largo));   // (de una vez: queda a menos de una vuelta)
    while (f > fin) f -= largo;
    if (f < ini) f = ini;
    return f;
}
// ...y por ABAJO (velocidad negativa): si cae despues del fin, el fin
static float EnvolverAbajo(float f, float ini, float fin, float largo) {
    if (FrameRaro(f)) return ini;
    if ((double)ini - (double)f > (double)largo * 64.0)
        f = (float)((double)ini - fmod((double)ini - (double)f, (double)largo));
    while (f < ini) f += largo;
    if (f > fin) f = fin;
    return f;
}

void W3dAnimEscenaTick(float dt) {
    if (gEscJuego < 0) return;
    std::vector<AnimationObject>* objs = ObjetosDeEscena(gEscJuego);
    if (!objs) { gEscJuego = -1; return; }
    const SceneAnimation* e = SceneAnimations[gEscJuego];
    const int fps = (e->fps > 0) ? e->fps : 30;
    if (!gEscJuegoManual) gEscJuegoF += dt * (float)fps;   // con animEscenaFrame manda el script (un solo reloj)
    const float ini = (float)e->startFrame, fin = (float)e->endFrame;
    if (gEscJuegoF > fin) {
        if (gEscJuegoLoop) {
            float largo = fin - ini + 1.0f;
            if (largo < 1.0f) largo = 1.0f;
            gEscJuegoF = EnvolverArriba(gEscJuegoF, ini, fin, largo);
        } else {
            gEscJuegoF = fin;   // one-shot: queda clavada en la ultima pose
        }
    }
    const int f = (int)gEscJuegoF;
    for (size_t i = 0; i < objs->size(); i++) {
        AnimationObject& ao = (*objs)[i];
        if (ao.obj) AplicarCurvasJuego(ao.obj, ao.Propertys, gEscJuegoF);
    }
    W3dCamActivaAplicar(gEscJuego, f);   // los CORTES de camara de la cinematica
}

int W3dAnimEscenaFrame() { return gEscJuego < 0 ? -1 : (int)gEscJuegoF; }
int W3dAnimEscenaSonando() { return gEscJuego; }

// ============================================================================
//  VISTAS DE EDICION DE LOS CLIPS DE JERARQUIA (ver SceneAnimation::esClip en Animation.h y
//  W3dAnimSet.h). El clip vive COMPARTIDO en su biblioteca, con sus pistas por RUTA; el editor
//  lo edita como una animacion de escena atada a los objetos de UNA raiz (la vista), y la vista
//  se escribe de vuelta en el clip cuando alguien lo va a leer (W3dClipsVistasSincronizar).
// ============================================================================
bool W3dAnimEsClip(int idx) {
    return idx >= 0 && idx < (int)SceneAnimations.size() && SceneAnimations[idx] && SceneAnimations[idx]->esClip;
}

bool W3dClipVistaViva(int idx) {
    if (!W3dAnimEsClip(idx)) return false;
    const SceneAnimation* e = SceneAnimations[idx];
    // (una raiz borrada con undo queda DETACHADA, viva pero fuera de todo arbol: su vista no se edita ni se
    //  escribe. W3dRaizDe no alcanza: la raiz detachada conserva su Parent -el Ctrl+Z la cuelga ahi- y
    //  subiendo por Parent llega a SceneCollection; escribir la vista entonces descartaba la pista "." del
    //  clip COMPARTIDO -sus curvas se fueron con el borrado- para todas las raices que lo usan)
    return e->duenio && e->clipJer && W3dRaizDeColgado(e->duenio) &&
           W3dJerClipVivo(W3dJerRecurso(e->duenio), e->clipJer, e->serieJer);
}

// ---- la RAIZ es relativa: lo que se le suma a sus curvas de transformacion al atarla ----
static const AnimProperty* CurvaCon(const std::vector<AnimProperty>& props, int prop, int comp) {
    for (size_t i = 0; i < props.size(); i++)
        if (props[i].Property == prop && props[i].component == comp && !props[i].keyframes.empty()) return &props[i];
    return 0;
}
static float CompDe(const Vector3& v, int c) { return c == AnimX ? v.x : (c == AnimY ? v.y : v.z); }
static void CompPon(Vector3& v, int c, float f) { if (c == AnimX) v.x = f; else if (c == AnimY) v.y = f; else v.z = f; }
// el desplazamiento de cada componente (base de la raiz - valor del clip en su primer frame); 0 donde no hay curva
static Vector3 OffsetRaiz(const std::vector<AnimProperty>& props, int prop, int inicio, const Vector3& base) {
    Vector3 off(0, 0, 0);
    for (int c = AnimX; c <= AnimZ; c++) {
        const AnimProperty* ap = CurvaCon(props, prop, c);
        if (ap) CompPon(off, c, CompDe(base, c) - ap->Eval(inicio, 0.0f));
    }
    return off;
}
// suma (signo +1) o resta (-1) el desplazamiento a los valores de una curva de transformacion de la raiz
static void CorrerCurva(AnimProperty& ap, const SceneAnimation* e, int signo) {
    const Vector3* off = ap.Property == AnimPosition ? &e->raizOffPos : ap.Property == AnimRotation ? &e->raizOffRot
                       : ap.Property == AnimScale ? &e->raizOffEsc : 0;
    if (!off || ap.component < AnimX || ap.component > AnimZ) return;
    const float d = CompDe(*off, ap.component);
    if (d == 0.0f) return;
    for (size_t k = 0; k < ap.keyframes.size(); k++) ap.keyframes[k].value = signo > 0 ? ap.keyframes[k].value + d : ap.keyframes[k].value - d;
}
// (clave a clave, valores y handles bit a bit; sin expandir una curva compacta: W3dClaves::MismosBits)
static bool KeysIguales(const W3dClaves& a, const W3dClaves& b) { return a.MismosBits(b); }
static bool PropsIguales(const std::vector<AnimProperty>& a, const std::vector<AnimProperty>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].Property != b[i].Property || a[i].component != b[i].component || !KeysIguales(a[i].keyframes, b[i].keyframes))
            return false;
    return true;
}

// el REPOSO de la raiz de una vista: la base a la que se le suma su pista "." al atarla. NO es su transform de
// hoy: la raiz puede estar POSADA -por esta misma vista a mitad del clip (renombrar el clip que se edita la
// vuelve a leer), por la vista de otro clip suyo o por una animacion de escena- y ese cuadro no es su reposo
// (atarla ahi corria toda la curva y el guardado escribia la raiz corrida como su transform). Es lo mismo que
// escribe el guardado (ReposoAnimObjetos, main/io/GuardarW3D.cpp): la curva de quien la poso, en SU cuadro de
// inicio. Por componente y en este orden: la vista que se lee (sus curvas de antes), las vistas de los otros
// clips de la raiz (atadas al mismo reposo) y, si la raiz esta posada, la animacion de escena activa y las
// demas; lo que nadie anima queda con el valor que tiene.
static void ReposoRaizVista(int idx, Object* raiz, Vector3& pos, Vector3& rot, Vector3& esc) {
    pos = raiz->pos; rot = raiz->rotEuler; esc = raiz->scale;
    unsigned hecho = 0;   // bits: 0-2 posicion x/y/z, 3-5 rotacion, 6-8 escala
    for (int pasada = 0; pasada < 4; pasada++)
        for (size_t i = 0; i < SceneAnimations.size(); i++) {
            const SceneAnimation* e = SceneAnimations[i];
            if (!e) continue;
            const bool vistaSuya = e->esClip && e->duenio == raiz;
            if (pasada == 0 && (int)i != idx) continue;
            if (pasada == 1 && ((int)i == idx || !vistaSuya)) continue;
            if (pasada >= 2 && (e->esClip || !raiz->posadoPorCurvas)) continue;
            if (pasada == 2 && (int)i != SceneAnimActiva) continue;
            if (pasada == 3 && (int)i == SceneAnimActiva) continue;
            const std::vector<AnimationObject>* lst = ObjetosDeEscena((int)i);
            if (!lst) continue;
            for (size_t k = 0; k < lst->size(); k++) {
                if ((*lst)[k].obj != raiz) continue;
                const std::vector<AnimProperty>& props = (*lst)[k].Propertys;
                for (int c = AnimX; c <= AnimZ; c++) {
                    const unsigned b = 1u << (c - AnimX);
                    const AnimProperty* p = CurvaCon(props, AnimPosition, c);
                    if (p && !(hecho & b)) { CompPon(pos, c, p->Eval(e->startFrame, CompDe(pos, c))); hecho |= b; }
                    const AnimProperty* r = CurvaCon(props, AnimRotation, c);
                    if (r && !(hecho & (b << 3))) { CompPon(rot, c, r->Eval(e->startFrame, CompDe(rot, c))); hecho |= b << 3; }
                    const AnimProperty* q = CurvaCon(props, AnimScale, c);
                    if (q && !(hecho & (b << 6))) { CompPon(esc, c, q->Eval(e->startFrame, CompDe(esc, c))); hecho |= b << 6; }
                }
                break;
            }
        }
}

void W3dClipVistaLeer(int idx) {
    if (!W3dClipVistaViva(idx)) return;
    SceneAnimation* e = SceneAnimations[idx];
    std::vector<AnimationObject>* lst = ObjetosDeEscena(idx);
    if (!lst) return;
    const W3dClipJer* c = e->clipJer;
    Object* raiz = e->duenio;
    Vector3 basePos, baseRot, baseEsc;
    ReposoRaizVista(idx, raiz, basePos, baseRot, baseEsc);   // (ANTES de vaciarla: sus curvas de antes lo dicen)
    lst->clear();
    e->name = c->nombre;
    e->startFrame = c->inicio; e->endFrame = c->fin; e->fps = c->fps > 0 ? c->fps : 30;
    e->raizOffPos = e->raizOffRot = e->raizOffEsc = Vector3(0, 0, 0);
    for (size_t p = 0; p < c->pistas.size(); p++) {
        Object* n = W3dJerNodo(raiz, c->pistas[p].ruta);
        if (!n) continue;   // (esta jerarquia no lo tiene: la pista queda en el clip tal cual)
        bool ya = false;    // (dos rutas que caen en el mismo nodo: vale la primera)
        for (size_t k = 0; k < lst->size() && !ya; k++) if ((*lst)[k].obj == n) ya = true;
        if (ya) continue;
        AnimationObject ao;
        ao.obj = n; ao.FirstKeyFrame = 0; ao.LastKeyFrame = 0;
        ao.Propertys = c->pistas[p].props;
        if (n == raiz) {
            e->raizOffPos = OffsetRaiz(ao.Propertys, AnimPosition, c->inicio, basePos);
            e->raizOffRot = OffsetRaiz(ao.Propertys, AnimRotation, c->inicio, baseRot);
            e->raizOffEsc = OffsetRaiz(ao.Propertys, AnimScale,    c->inicio, baseEsc);
            for (size_t k = 0; k < ao.Propertys.size(); k++) CorrerCurva(ao.Propertys[k], e, +1);
        }
        ao.UpdateFirstLastFrame();
        lst->push_back(ao);
    }
    e->versionVista = c->version;
    W3dAnimCurvasInvalidar();
}

bool W3dClipVistaEscribir(int idx) {
    if (!W3dClipVistaViva(idx)) return false;
    SceneAnimation* e = SceneAnimations[idx];
    std::vector<AnimationObject>* lst = ObjetosDeEscena(idx);
    if (!lst) return false;
    W3dClipJer* c = e->clipJer;
    Object* raiz = e->duenio;
    std::vector<char> usado(lst->size(), 0);
    std::vector<W3dPistaJer> nuevas;
    // las pistas que ya tenia, en su orden y con su ruta de siempre: las de los nodos que esta jerarquia
    // no tiene quedan tal cual; las demas pasan a ser lo que dice la vista (sin keyframes = se borro)
    for (size_t p = 0; p < c->pistas.size(); p++) {
        const W3dPistaJer& vieja = c->pistas[p];
        Object* n = W3dJerNodo(raiz, vieja.ruta);
        if (!n) { nuevas.push_back(vieja); continue; }
        int k = -1;
        for (size_t i = 0; i < lst->size() && k < 0; i++) if (!usado[i] && (*lst)[i].obj == n) k = (int)i;
        if (k < 0) continue;
        usado[(size_t)k] = 1;
        W3dPistaJer np;
        np.ruta = vieja.ruta;
        const std::vector<AnimProperty>& vp = (*lst)[(size_t)k].Propertys;
        for (size_t q = 0; q < vp.size(); q++) {
            if (vp[q].keyframes.empty()) continue;
            AnimProperty ap = vp[q];
            if (n == raiz) {
                // la curva que no se toco vuelve con los valores que tenia (sin el ida y vuelta del desplazamiento)
                const AnimProperty* antes = 0;
                for (size_t r = 0; r < vieja.props.size() && !antes; r++)
                    if (vieja.props[r].Property == ap.Property && vieja.props[r].component == ap.component) antes = &vieja.props[r];
                AnimProperty esperada;
                if (antes) { esperada = *antes; CorrerCurva(esperada, e, +1); }
                if (antes && KeysIguales(esperada.keyframes, ap.keyframes)) ap = *antes;
                else CorrerCurva(ap, e, -1);
            }
            np.props.push_back(ap);
        }
        if (!np.props.empty()) nuevas.push_back(np);
    }
    // lo que se keyeo NUEVO en la vista (un objeto de la jerarquia que el clip no tenia)
    for (size_t i = 0; i < lst->size(); i++) {
        if (usado[i] || !(*lst)[i].obj) continue;
        const std::string ruta = W3dJerRuta(raiz, (*lst)[i].obj);
        if (ruta.empty()) continue;   // (fuera de la jerarquia: W3dAnimObjetoPermitido no deberia dejarlo entrar)
        W3dPistaJer np;
        np.ruta = ruta;
        for (size_t q = 0; q < (*lst)[i].Propertys.size(); q++) {
            if ((*lst)[i].Propertys[q].keyframes.empty()) continue;
            AnimProperty ap = (*lst)[i].Propertys[q];
            if ((*lst)[i].obj == raiz) CorrerCurva(ap, e, -1);
            np.props.push_back(ap);
        }
        if (!np.props.empty()) nuevas.push_back(np);
    }
    // el nombre (el Rename de la tarjeta escribe el de la vista): unico entre los clips de su biblioteca
    if (e->name != c->nombre) e->name = W3dJerClipRenombrar(W3dJerAnimSet(raiz), c, e->name);
    bool cambio = (nuevas.size() != c->pistas.size());
    for (size_t p = 0; p < nuevas.size() && !cambio; p++)
        if (nuevas[p].ruta != c->pistas[p].ruta || !PropsIguales(nuevas[p].props, c->pistas[p].props)) cambio = true;
    if (c->inicio != e->startFrame || c->fin != e->endFrame || c->fps != e->fps) cambio = true;
    if (cambio) {
        c->pistas.swap(nuevas);
        c->inicio = e->startFrame; c->fin = e->endFrame; c->fps = e->fps;
        c->version++;
    }
    e->versionVista = c->version;
    return cambio;
}

void W3dClipsVistasSincronizar() {
    if (W3dClipVistaViva(SceneAnimActiva)) W3dClipVistaEscribir(SceneAnimActiva);
}

int W3dClipVista(Object* raiz, W3dClipJer* c) {
    if (!raiz || !c) return -1;
    InitSceneAnimations();   // (la animacion de escena de siempre existe antes que cualquier vista)
    for (size_t i = 0; i < SceneAnimations.size(); i++) {
        SceneAnimation* e = SceneAnimations[i];
        if (e && e->esClip && e->duenio == raiz && e->clipJer == c && e->serieJer == c->serie) {
            if (e->versionVista != c->version && (int)i != SceneAnimActiva) W3dClipVistaLeer((int)i);
            return (int)i;
        }
    }
    SceneAnimation* e = new SceneAnimation(c->nombre);
    e->esClip = true;
    e->duenio = raiz;
    e->clipJer = c;
    e->serieJer = c->serie;
    SceneAnimations.push_back(e);
    const int idx = (int)SceneAnimations.size() - 1;
    W3dClipVistaLeer(idx);
    return idx;
}

void W3dClipVistaActivaReleer(const W3dClipJer* c) {
    if (!c || !W3dClipVistaViva(SceneAnimActiva) || SceneAnimations[SceneAnimActiva]->clipJer != c) return;
    W3dClipVistaLeer(SceneAnimActiva);
}

void W3dClipsVistasPurgar() {
    for (size_t i = SceneAnimations.size(); i-- > 0; ) {
        if ((int)i == SceneAnimActiva || !W3dAnimEsClip((int)i) || W3dClipVistaViva((int)i)) continue;
        delete SceneAnimations[i];
        SceneAnimations.erase(SceneAnimations.begin() + (long)i);
        if ((int)i < SceneAnimActiva) SceneAnimActiva--;
    }
}

void W3dClipsVistasRecambiar(std::vector<SceneAnimation*>& lista, const Object* duenio,
                             const std::vector<W3dClipJer*>& viejos, const std::vector<W3dClipJer*>& nuevos) {
    for (size_t i = 0; i < lista.size(); i++) {
        SceneAnimation* e = lista[i];
        if (!e || !e->esClip || e->duenio != duenio || !e->clipJer) continue;
        for (size_t k = 0; k < viejos.size() && k < nuevos.size(); k++) {
            if (viejos[k] != e->clipJer || viejos[k]->serie != e->serieJer || !nuevos[k]) continue;
            // (al dia con el viejo = al dia con el nuevo: son iguales; si estaba vieja se sigue re-leyendo)
            const bool alDia = (e->versionVista == viejos[k]->version);
            e->clipJer = nuevos[k];
            e->serieJer = nuevos[k]->serie;
            e->versionVista = alDia ? nuevos[k]->version : nuevos[k]->version - 1u;
            break;
        }
    }
}

std::string W3dAnimEscenaEtiqueta(int idx) {
    if (idx < 0 || idx >= (int)SceneAnimations.size() || !SceneAnimations[idx]) return std::string();
    const SceneAnimation* e = SceneAnimations[idx];
    if (!e->esClip) return e->name;
    return (e->duenio ? e->duenio->name : std::string("?")) + ": " + e->name;
}

bool W3dAnimObjetoPermitido(const Object* o) {
    if (!o || ActiveAnimKind != 0 || !W3dAnimEsClip(SceneAnimActiva)) return true;
    const SceneAnimation* e = SceneAnimations[SceneAnimActiva];
    return e->duenio && !W3dJerRuta(e->duenio, o).empty();
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
//  Una raiz con CAPAS (W3dJerRaiz::capas) la posa el Mix, despues de este reproductor: el Mix manda.
// ============================================================================
namespace {
struct W3dAnimObjetoSonando {
    Object* o;                     // la raiz que lo reproduce
    W3dRecurso* rec;               // la biblioteca del clip (una referencia PERMANENTE mientras suena)
    const W3dClipJer* clip;
    unsigned serie, version;       // el clip (su serie) y la version con la que se resolvieron los nodos
    float frame, vel;
    bool loop, termino;
    bool mover;                    // (solo dentro del tick) su cabezal avanzo en este tick: se posa
    std::vector<Object*> nodos;    // por pista (NULL = esta jerarquia no lo tiene)
    Vector3 basePos, baseRot, baseEsc;   // la raiz al arrancar (su pista es relativa); en los canales que le
                                         // anima un ancestro, la ultima pose que ese ancestro le dio
    int retarget;                  // el modo con el que suena (W3D_RETARGET_*)
    float escala;                  // retarget: tamano del destino / tamano del clip
    W3dAnimObjetoSonando() : o(0), rec(0), clip(0), serie(0), version(0), frame(0.0f), vel(1.0f), loop(true),
                             termino(false), mover(false), basePos(0, 0, 0), baseRot(0, 0, 0), baseEsc(1, 1, 1),
                             retarget(0), escala(1.0f) {}
};
// los canales de transformacion (bits: 0-2 posicion x/y/z, 3-5 rotacion, 6-8 escala) que en ESTE tick el
// clip de un ancestro le poso a cada raiz que suena. Solo se lleva con dos o mas raices sonando.
struct W3dCanalesPosados {
    std::vector<Object*> raices;                          // las raices que suenan, ordenadas (binary_search)
    std::vector<std::pair<Object*, unsigned> > canales;   // (raiz anidada, lo que le posaron); casi siempre vacio
};
}
static std::vector<W3dAnimObjetoSonando> gAnimObjetos;
// true si la raiz tiene CAPAS de clips de jerarquia (el Mix manda: ver W3dJerRaiz::capas)
static bool ConCapas(const Object* o) { return o && o->clipsJer && !o->clipsJer->capas.empty(); }

static W3dAnimObjetoSonando* SonandoDe(const Object* o) {
    for (size_t i = 0; i < gAnimObjetos.size(); i++) if (gAnimObjetos[i].o == o) return &gAnimObjetos[i];
    return NULL;
}
static void SoltarSonando(W3dAnimObjetoSonando& s) {
    if (s.rec) { W3dRecurso* r = s.rec; s.rec = 0; W3dRecursoSoltar(r, W3DREC_PERMANENTE); }
}
static void ResolverNodos(W3dAnimObjetoSonando& s) {
    s.nodos.assign(s.clip->pistas.size(), (Object*)0);
    for (size_t p = 0; p < s.clip->pistas.size(); p++) s.nodos[p] = W3dJerNodo(s.o, s.clip->pistas[p].ruta);
    s.version = s.clip->version;
}
// el modo de retarget de la raiz (el suyo, si se lo dieron; si no, el del clip) y su escala
static void ElegirRetarget(W3dAnimObjetoSonando& s) {
    const int propio = s.o->clipsJer ? s.o->clipsJer->retarget : -1;
    s.retarget = (propio >= 0) ? propio : s.clip->retarget;
    s.escala = 1.0f;
    if (s.retarget == W3D_RETARGET_ROTACIONES && s.clip->tamano > 1e-6f) {
        const float d = W3dJerTamano(s.o);
        if (d > 1e-6f) s.escala = d / s.clip->tamano;
    }
}
// la pista de la RAIZ: el delta desde el primer frame del clip, sumado a donde estaba al arrancar.
// 'seguir' (bits de W3dCanalesPosados): los canales que un ANCESTRO que suena le acaba de posar en este tick;
// en esos la base pasa a ser esa pose (la raiz anidada se mueve SOBRE lo que le hace su ancestro)
static void AplicarRaiz(W3dAnimObjetoSonando& s, std::vector<AnimProperty>& props, float fr, unsigned seguir) {
    Object* o = s.o;
    AplicarCurvasJuegoCanales(o, props, fr, 0);   // lo que no es transformacion (visible, fov, luz, formas)
    const float ini = (float)s.clip->inicio;
    bool rot = false;
    Vector3 e = o->rotEuler;
    for (int c = AnimX; c <= AnimZ; c++) {
        const unsigned b = 1u << (c - AnimX);
        const AnimProperty* p = CurvaCon(props, AnimPosition, c);
        if (p) {
            if (seguir & b) CompPon(s.basePos, c, CompDe(o->pos, c));
            float d = p->EvalF(fr, 0.0f) - p->EvalF(ini, 0.0f);
            if (s.retarget == W3D_RETARGET_ROTACIONES) d *= s.escala;   // el movimiento, a escala del destino
            CompPon(o->pos, c, CompDe(s.basePos, c) + d);
        }
        const AnimProperty* r = CurvaCon(props, AnimRotation, c);
        if (r) {
            if (seguir & (b << 3)) CompPon(s.baseRot, c, CompDe(e, c));
            CompPon(e, c, CompDe(s.baseRot, c) + r->EvalF(fr, 0.0f) - r->EvalF(ini, 0.0f));
            rot = true;
        }
        const AnimProperty* k = CurvaCon(props, AnimScale, c);
        if (k) {
            if (seguir & (b << 6)) CompPon(s.baseEsc, c, CompDe(o->scale, c));
            CompPon(o->scale, c, CompDe(s.baseEsc, c) + k->EvalF(fr, 0.0f) - k->EvalF(ini, 0.0f));
        }
    }
    if (rot) o->SetRotEuler(e);
}
// los canales de transformacion (bits de W3dCanalesPosados) que tienen curva en 'props', de los que 'canales'
// (kCanal*) deja aplicar
static unsigned CanalesConCurva(const std::vector<AnimProperty>& props, int canales) {
    unsigned m = 0;
    for (size_t i = 0; i < props.size(); i++) {
        const AnimProperty& p = props[i];
        if (p.keyframes.empty() || p.component < AnimX || p.component > AnimZ) continue;
        const unsigned b = 1u << (p.component - AnimX);
        if      (p.Property == AnimPosition && (canales & kCanalPos)) m |= b;
        else if (p.Property == AnimRotation && (canales & kCanalRot)) m |= b << 3;
        else if (p.Property == AnimScale    && (canales & kCanalEsc)) m |= b << 6;
    }
    return m;
}
static unsigned CanalesPosadosDe(const W3dCanalesPosados& v, const Object* o) {
    for (size_t i = 0; i < v.canales.size(); i++) if (v.canales[i].first == o) return v.canales[i].second;
    return 0;
}
// anota lo que se le poso al nodo 'o' SOLO si es la raiz de otro clip que suena (lo unico que se consulta)
static void CanalesPosadosSumar(W3dCanalesPosados& v, Object* o, const std::vector<AnimProperty>& props, int canales) {
    if (!std::binary_search(v.raices.begin(), v.raices.end(), o)) return;
    const unsigned m = CanalesConCurva(props, canales);
    if (!m) return;
    for (size_t i = 0; i < v.canales.size(); i++) if (v.canales[i].first == o) { v.canales[i].second |= m; return; }
    v.canales.push_back(std::make_pair(o, m));
}
// aplica el clip en su frame: la raiz (relativa) y los nodos de su jerarquia que tienen curvas en el.
// 'seguir': ver AplicarRaiz (0 fuera del tick). 'posados' (opcional): anota lo que se le poso a cada nodo.
static void AplicarClip(W3dAnimObjetoSonando& s, unsigned seguir = 0, W3dCanalesPosados* posados = 0) {
    W3dClipJer* c = (W3dClipJer*)s.clip;
    // "rotaciones": los descendientes conservan su traslacion y su escala (las proporciones del destino)
    const int canales = s.retarget == W3D_RETARGET_ROTACIONES ? kCanalRot : kCanalTodos;
    for (size_t p = 0; p < c->pistas.size() && p < s.nodos.size(); p++) {
        Object* n = s.nodos[p];
        if (!n) continue;
        if (n == s.o) { AplicarRaiz(s, c->pistas[p].props, s.frame, seguir); continue; }
        AplicarCurvasJuegoCanales(n, c->pistas[p].props, s.frame, canales);
        if (posados) CanalesPosadosSumar(*posados, n, c->pistas[p].props, canales);
    }
}
// cuantos padres tiene 'o' hasta el tope de su arbol (los ancestros se posan antes)
static int ProfundidadDe(const Object* o) {
    int d = 0;
    for (const Object* p = o ? o->Parent : 0; p; p = p->Parent) d++;
    return d;
}
// posa las raices que suenan en la raiz activa, ANCESTROS PRIMERO (ver "RAICES ANIDADAS" arriba): las que
// avanzaron en este tick y las que, quietas (una pasada que ya termino), tienen un ancestro que les sigue
// moviendo los canales que su pista "." anima (su ultima pose se suma a la de el)
static void PosarSonando() {
    std::vector<std::pair<int, int> > orden;   // (profundidad, indice): el indice desempata = determinista
    for (size_t k = 0; k < gAnimObjetos.size(); k++)
        if (W3dRaizDe(gAnimObjetos[k].o) == SceneCollection)
            orden.push_back(std::make_pair(ProfundidadDe(gAnimObjetos[k].o), (int)k));
    W3dCanalesPosados posados;
    W3dCanalesPosados* anotar = 0;   // (una sola raiz: nada que anidar)
    if (orden.size() > 1) {
        std::sort(orden.begin(), orden.end());
        for (size_t i = 0; i < orden.size(); i++) posados.raices.push_back(gAnimObjetos[(size_t)orden[i].second].o);
        std::sort(posados.raices.begin(), posados.raices.end());
        anotar = &posados;
    }
    for (size_t i = 0; i < orden.size(); i++) {
        W3dAnimObjetoSonando& s = gAnimObjetos[(size_t)orden[i].second];
        // (una raiz con CAPAS muestra la mezcla, como un armature: su clip unico avanza pero no la posa)
        if (ConCapas(s.o)) continue;
        const unsigned seguir = CanalesPosadosDe(posados, s.o);
        if (!s.mover && !seguir) continue;
        AplicarClip(s, seguir, anotar);
    }
}
float W3dAnimObjetoPlay(Object* o, const std::string& nombre, bool loop) {
    if (!o) return -1.0f;
    W3dRecurso* rec = 0;
    W3dClipJer* c = W3dJerClip(o, nombre);
    if (c) { rec = W3dJerRecurso(o); W3dRecursoRetener(rec, W3DREC_PERMANENTE); }
    else c = W3dJerClipBuscar(nombre, &rec);   // "Biblioteca/clip" (o el unico con ese nombre): con referencia
    if (!c || !rec) return -1.0f;
    W3dAnimObjetoSonando* s = SonandoDe(o);
    if (!s) { gAnimObjetos.push_back(W3dAnimObjetoSonando()); s = &gAnimObjetos.back(); s->o = o; }
    SoltarSonando(*s);
    s->rec = rec;
    s->clip = c;
    s->serie = c->serie;
    s->frame = (float)c->inicio;
    s->vel = 1.0f;
    s->loop = loop;
    s->termino = false;
    s->basePos = o->pos; s->baseRot = o->rotEuler; s->baseEsc = o->scale;
    ResolverNodos(*s);
    ElegirRetarget(*s);
    if (!ConCapas(o)) AplicarClip(*s);   // el cuadro de inicio ya posado en este mismo tick
    return c->Duracion();
}
void W3dAnimObjetosTick(float dt) {
    // 1) los cabezales
    for (size_t k = 0; k < gAnimObjetos.size(); ) {
        W3dAnimObjetoSonando& s = gAnimObjetos[k];
        s.mover = false;
        // una raiz de OTRA raiz del proyecto (el juego mientras suena una cinematica en su escena): en pausa,
        // se conserva tal cual
        if (W3dRaizDe(s.o) != SceneCollection) { k++; continue; }
        // el clip ya no esta (se borro de su biblioteca en el editor): la raiz deja de sonar
        if (!W3dJerClipVivo(s.rec, s.clip, s.serie)) { SoltarSonando(s); gAnimObjetos.erase(gAnimObjetos.begin() + (long)k); continue; }
        if (s.version != s.clip->version) ResolverNodos(s);   // (lo cambio el editor: rutas nuevas)
        if (!s.termino) {
            const float ini = (float)s.clip->inicio, fin = (float)s.clip->fin;
            float largo = fin - ini + 1.0f;
            if (largo < 1.0f) largo = 1.0f;
            s.frame += dt * (float)(s.clip->fps > 0 ? s.clip->fps : 30) * s.vel;
            if (s.frame > fin) {
                if (s.loop) s.frame = EnvolverArriba(s.frame, ini, fin, largo);
                else { s.frame = fin; s.termino = true; }    // una pasada: queda en su ultima pose
            } else if (s.frame < ini) {   // (velocidad negativa)
                if (s.loop) s.frame = EnvolverAbajo(s.frame, ini, fin, largo);
                else { s.frame = ini; s.termino = true; }
            } else if (FrameRaro(s.frame)) s.frame = ini;   // (NaN: una velocidad NaN desde lua)
            s.mover = true;   // (el tick en que una pasada llega al final tambien la posa: su ultima pose)
        }
        k++;
    }
    // 2) las poses: los ancestros primero (raices anidadas)
    PosarSonando();
}
float W3dAnimObjetoFrame(const Object* o) {
    const W3dAnimObjetoSonando* s = SonandoDe(o);
    return s ? s->frame : -1.0f;
}
void W3dAnimObjetoFijarFrame(Object* o, float f) {
    W3dAnimObjetoSonando* s = SonandoDe(o);
    if (!s || !W3dJerClipVivo(s->rec, s->clip, s->serie)) return;
    const float ini = (float)s->clip->inicio, fin = (float)s->clip->fin;
    s->frame = f < ini ? ini : (f > fin ? fin : f);
    s->termino = false;
    if (!ConCapas(o)) AplicarClip(*s);
}
bool W3dAnimObjetoTermino(const Object* o) {
    const W3dAnimObjetoSonando* s = SonandoDe(o);
    return !s || s->termino;
}
std::string W3dAnimObjetoClip(const Object* o) {
    const W3dAnimObjetoSonando* s = SonandoDe(o);
    return (s && W3dJerClipVivo(s->rec, s->clip, s->serie)) ? s->clip->nombre : std::string();
}
void W3dAnimObjetoParar(Object* o) {
    for (size_t i = 0; i < gAnimObjetos.size(); i++)
        if (gAnimObjetos[i].o == o) { SoltarSonando(gAnimObjetos[i]); gAnimObjetos.erase(gAnimObjetos.begin() + (long)i); return; }
}
bool W3dAnimObjetoVelocidad(Object* o, float v) {
    W3dAnimObjetoSonando* s = SonandoDe(o);
    if (!s) return false;
    s->vel = v;
    if (v != 0.0f) s->termino = false;
    return true;
}
void W3dAnimObjetoRetarget(Object* o, int modo) {
    if (!o) return;
    if (!o->clipsJer) o->clipsJer = new W3dJerRaiz();
    o->clipsJer->retarget = (modo < 0) ? -1 : (modo == W3D_RETARGET_ROTACIONES ? W3D_RETARGET_ROTACIONES : W3D_RETARGET_COMPLETO);
    W3dAnimObjetoSonando* s = SonandoDe(o);
    if (s && W3dJerClipVivo(s->rec, s->clip, s->serie)) { ElegirRetarget(*s); if (!ConCapas(o)) AplicarClip(*s); }
    // las capas recalculan su escala con el modo nuevo
    if (o->clipsJer) for (size_t k = 0; k < o->clipsJer->capas.size(); k++) o->clipsJer->capas[k].retClip = 0;
}
void W3dAnimObjetosReset() {
    for (size_t i = 0; i < gAnimObjetos.size(); i++) SoltarSonando(gAnimObjetos[i]);
    gAnimObjetos.clear();
}
int  W3dAnimObjetosSonando() { return (int)gAnimObjetos.size(); }

// ~Object: una raiz que se borra deja de sonar (suelta su biblioteca), un nodo que se borra deja de
// posarse, y las vistas de edicion de esa raiz quedan HUERFANAS (no se listan ni se escriben; las de una
// raiz inactiva las suelta main/W3dRaices)
static std::vector<Object*> gJerConCapas;   // las raices con CAPAS de clips de jerarquia (ver mas abajo)
static void MixBasesOlvidarObjeto(Object* o);   // (el Mix, mas abajo)
static void AnimObjetosDesvincular(Object* o) {
    for (size_t i = gJerConCapas.size(); i-- > 0; ) if (gJerConCapas[i] == o) gJerConCapas.erase(gJerConCapas.begin() + (long)i);
    MixBasesOlvidarObjeto(o);
    for (size_t i = gAnimObjetos.size(); i-- > 0; ) {
        W3dAnimObjetoSonando& s = gAnimObjetos[i];
        if (s.o == o) { SoltarSonando(s); gAnimObjetos.erase(gAnimObjetos.begin() + (long)i); continue; }
        for (size_t n = 0; n < s.nodos.size(); n++) if (s.nodos[n] == o) s.nodos[n] = 0;
    }
    for (size_t i = 0; i < SceneAnimations.size(); i++)
        if (SceneAnimations[i] && SceneAnimations[i]->esClip && SceneAnimations[i]->duenio == o) SceneAnimations[i]->duenio = 0;
}
namespace {
struct RegistrarAnimObjetos { RegistrarAnimObjetos() { W3dDesvincularRegistrar(AnimObjetosDesvincular); } } gRegistrarAnimObjetos;
}


// ============================================================================
//  MIX DE ESCENAS (capas de animaciones de ESCENA, W3dCapaAnim / g_mixEscenas): cada objeto que tocan las
//  capas visibles recibe la MEZCLA (mezclar = lerp, sumar/restar = lo que la capa se aparta de la BASE). Sirve
//  para personajes hechos de OBJETOS sueltos (GTA3: cada parte es un objeto; correr + disparar + ...).
//  La BASE es la transform que el objeto tenia la primera vez que el mix lo toco; W3dMixEscenasSoltar lo
//  devuelve ahi (al salir del Mix en el editor). En el juego cada capa avanza con su cabezal (juegoFrame).
// ============================================================================
#include "animation/SkeletalAnimation.h"   // g_mixEscenas, W3dCapaFrameEditor
#include <map>
struct W3dMixBase { Vector3 p, r, s; };
static std::map<Object*, W3dMixBase> gMixBase;
void W3dMixEscenasSoltar(){
    for (std::map<Object*, W3dMixBase>::iterator it = gMixBase.begin(); it != gMixBase.end(); ++it){
        Object* o = it->first; if (!o) continue;
        o->pos = it->second.p; o->SetRotEuler(it->second.r); o->scale = it->second.s; }
    gMixBase.clear();
}
// el PLAY del editor: las bases se guardan al darle Play y se reponen en Stop SIN mover nada (las
// posiciones las repone el snapshot de SimJuego). Sin esto, un objeto que el mix toco por PRIMERA vez
// jugando se quedaba con la base de la partida y el Mix del editor lo mezclaba (y lo soltaba) desde ahi.
static std::map<Object*, W3dMixBase> gMixBaseEditor;
void W3dMixEscenasBasesGuardar(){ gMixBaseEditor = gMixBase; }
void W3dMixEscenasBasesReponer(){ gMixBase = gMixBaseEditor; gMixBaseEditor.clear(); }
// ReiniciarEscena (abrir / nuevo proyecto): los objetos de las bases se van a BORRAR -> se olvidan SIN
// moverlos. Sin esto, salir del Mix despues de abrir otro proyecto les escribia encima (ya liberados).
void W3dMixEscenasOlvidar(){ gMixBase.clear(); gMixBaseEditor.clear(); }
// ~Object: su base se olvida (salir del Mix o guardar le escribian encima a un objeto ya liberado)
static void MixBasesOlvidarObjeto(Object* o){ gMixBase.erase(o); gMixBaseEditor.erase(o); }
void W3dMixEscenasBases(std::vector<Object*>& objs, std::vector<Vector3>& pos, std::vector<Vector3>& rot,
                        std::vector<Vector3>& esc){
    objs.clear(); pos.clear(); rot.clear(); esc.clear();
    for (std::map<Object*, W3dMixBase>::const_iterator it = gMixBase.begin(); it != gMixBase.end(); ++it){
        if (!it->first) continue;
        objs.push_back(it->first); pos.push_back(it->second.p); rot.push_back(it->second.r); esc.push_back(it->second.s);
    }
}
// ---- LAS CAPAS DE CLIPS DE JERARQUIA (W3dJerRaiz::capas): el Mix de una raiz, que se suma al de escena ----
void W3dJerCapasCambiaron(Object* o) {
    if (!o) return;
    const bool tiene = ConCapas(o);
    std::vector<Object*>::iterator it = std::find(gJerConCapas.begin(), gJerConCapas.end(), o);
    if (tiene && it == gJerConCapas.end()) gJerConCapas.push_back(o);
    else if (!tiene && it != gJerConCapas.end()) gJerConCapas.erase(it);
    if (o->clipsJer) for (size_t k = 0; k < o->clipsJer->capas.size(); k++) o->clipsJer->capas[k].retClip = 0;
}
// las raices con capas de la raiz ACTIVA (las de una escena que no se esta mirando quedan quietas), los
// ANCESTROS primero: una raiz anidada (con sus propias capas) mezcla sobre lo que le dejaron las de su
// ancestro, sin importar en que orden se les pusieron las capas (ver "RAICES ANIDADAS" arriba)
static void RaicesConCapas(std::vector<Object*>& out) {
    out.clear();
    std::vector<std::pair<int, int> > orden;   // (profundidad, indice)
    for (size_t i = 0; i < gJerConCapas.size(); i++)
        if (ConCapas(gJerConCapas[i]) && W3dRaizDe(gJerConCapas[i]) == SceneCollection)
            orden.push_back(std::make_pair(ProfundidadDe(gJerConCapas[i]), (int)i));
    if (orden.size() > 1) std::sort(orden.begin(), orden.end());
    for (size_t i = 0; i < orden.size(); i++) out.push_back(gJerConCapas[(size_t)orden[i].second]);
}
void W3dJerCapasRaices(std::vector<Object*>& out) { RaicesConCapas(out); }
// el frame de la capa sobre su clip: jugando, su cabezal; en el editor, el del Mix
static float FrameCapaJer(const W3dCapaAnim& c, const W3dClipJer* clip, int frameMix, bool juego) {
    return juego ? (float)clip->inicio + c.juegoFrame : W3dCapaFrameEditor(c, frameMix, clip->inicio, clip->fin);
}
// el retarget de la capa (el de la raiz si lo tiene, si no el del clip) y su escala, cacheada por clip
static int RetargetCapaJer(Object* o, W3dCapaAnim& c, const W3dClipJer* clip, float* escala) {
    const int propio = o->clipsJer ? o->clipsJer->retarget : -1;
    const int modo = (propio >= 0) ? propio : clip->retarget;
    *escala = 1.0f;
    if (modo != W3D_RETARGET_ROTACIONES) return modo;
    if (c.retClip != (const void*)clip) {
        c.retEscala = 1.0f;
        const float d = W3dJerTamano(o);
        if (clip->tamano > 1e-6f && d > 1e-6f) c.retEscala = d / clip->tamano;
        c.retClip = (const void*)clip;
    }
    *escala = c.retEscala;
    return modo;
}
bool W3dMixEscenasAplicar(int frameMix, bool juego){
    std::vector<Object*> raicesCapas;
    RaicesConCapas(raicesCapas);
    if (g_mixEscenas.empty() && raicesCapas.empty()) return false;
    InitSceneAnimations();
    std::map<Object*, W3dMixBase> acc;
    for (size_t k = 0; k < g_mixEscenas.size(); k++){
        W3dCapaAnim& c = g_mixEscenas[k];
        if (!c.visible || c.influencia <= 0.0f) continue;
        int idx = W3dAnimEscenaIdx(c.anim.c_str()); if (idx < 0) continue;
        SceneAnimation* sa = SceneAnimations[idx];
        std::vector<AnimationObject>& objs = (idx == SceneAnimActiva) ? AnimationObjects : sa->objetos;
        int f = juego ? sa->startFrame + (int)c.juegoFrame : (int)W3dCapaFrameEditor(c, frameMix, sa->startFrame, sa->endFrame);
        float w = c.influencia * 0.01f; if (c.modo == 0 && w > 1.0f) w = 1.0f; if (w > 2.0f) w = 2.0f;
        float sg = (c.modo == 2) ? -1.0f : 1.0f;
        for (size_t i = 0; i < objs.size(); i++){
            Object* o = objs[i].obj; if (!o) continue;
            if (gMixBase.find(o) == gMixBase.end()){ W3dMixBase b; b.p = o->pos; b.r = o->rotEuler; b.s = o->scale; gMixBase[o] = b; }
            const W3dMixBase& base = gMixBase[o];
            if (acc.find(o) == acc.end()) acc[o] = base;
            W3dMixBase& A = acc[o];
            std::vector<AnimProperty>& pr = objs[i].Propertys;
            Vector3 P = EvalPropVec(pr, AnimPosition, f, base.p);
            Vector3 R = EvalPropVec(pr, AnimRotation, f, base.r);
            Vector3 S = EvalPropVec(pr, AnimScale,    f, base.s);
            if (c.modo == 0){ A.p = A.p + (P - A.p) * w; A.r = A.r + (R - A.r) * w; A.s = A.s + (S - A.s) * w; }
            else { A.p = A.p + (P - base.p) * (w * sg); A.r = A.r + (R - base.r) * (w * sg); A.s = A.s + (S - base.s) * (w * sg); }
        }
    }
    // las CAPAS DE CLIPS DE JERARQUIA de cada raiz, en su orden, sobre la misma acumulacion: cada nodo por su RUTA.
    // La pista de la raiz es RELATIVA (el delta desde el primer frame del clip, sumado a su base); con retarget
    // "rotaciones" los descendientes solo toman la rotacion y el movimiento de la raiz se escala por tamano.
    // Las raices van ANCESTROS PRIMERO (RaicesConCapas): la de una raiz ANIDADA (un nodo que ya mezclaron las
    // capas de su ancestro) se mueve SOBRE esa mezcla, como en el reproductor de clips (ver "RAICES ANIDADAS").
    std::vector<Object*> tocadosJer;   // las raices con capas que ya mezclaron las capas de una raiz anterior
    std::vector<Object*> conCapas;     // las raices con capas, ordenadas (binary_search): solo esas se anotan
    if (raicesCapas.size() > 1){ conCapas = raicesCapas; std::sort(conCapas.begin(), conCapas.end()); }
    for (size_t r = 0; r < raicesCapas.size(); r++){
        Object* raiz = raicesCapas[r];
        const bool anidada = std::find(tocadosJer.begin(), tocadosJer.end(), raiz) != tocadosJer.end();
        W3dMixBase baseAnidada;
        if (anidada) baseAnidada = acc[raiz];   // (lo que le dejaron las capas de antes: su base relativa)
        std::vector<W3dCapaAnim>& capas = raiz->clipsJer->capas;
        for (size_t k = 0; k < capas.size(); k++){
            W3dCapaAnim& c = capas[k];
            if (!c.visible || c.influencia <= 0.0f) continue;
            const W3dClipJer* clip = W3dJerCapaClip(raiz, c, 0);
            if (!clip) continue;
            const float f = FrameCapaJer(c, clip, frameMix, juego);
            const float ini = (float)clip->inicio;
            float esc = 1.0f;
            const bool soloRot = RetargetCapaJer(raiz, c, clip, &esc) == W3D_RETARGET_ROTACIONES;
            float w = c.influencia * 0.01f; if (c.modo == 0 && w > 1.0f) w = 1.0f; if (w > 2.0f) w = 2.0f;
            const float sg = (c.modo == 2) ? -1.0f : 1.0f;
            for (size_t p = 0; p < clip->pistas.size(); p++){
                Object* n = W3dJerNodo(raiz, clip->pistas[p].ruta);
                if (!n) continue;   // (esta jerarquia no lo tiene)
                const std::vector<AnimProperty>& pr = clip->pistas[p].props;
                if (gMixBase.find(n) == gMixBase.end()){ W3dMixBase b; b.p = n->pos; b.r = n->rotEuler; b.s = n->scale; gMixBase[n] = b; }
                const W3dMixBase& base = (n == raiz && anidada) ? baseAnidada : gMixBase[n];
                if (acc.find(n) == acc.end()) acc[n] = base;
                if (n != raiz && !conCapas.empty() && std::binary_search(conCapas.begin(), conCapas.end(), n)) tocadosJer.push_back(n);
                W3dMixBase& A = acc[n];
                Vector3 P = base.p, R = base.r, S = base.s;
                if (n == raiz){
                    for (int q = AnimX; q <= AnimZ; q++){
                        const AnimProperty* cp = CurvaCon(pr, AnimPosition, q);
                        if (cp) CompPon(P, q, CompDe(base.p, q) + (cp->EvalF(f, 0.0f) - cp->EvalF(ini, 0.0f)) * (soloRot ? esc : 1.0f));
                        const AnimProperty* cr = CurvaCon(pr, AnimRotation, q);
                        if (cr) CompPon(R, q, CompDe(base.r, q) + cr->EvalF(f, 0.0f) - cr->EvalF(ini, 0.0f));
                        const AnimProperty* ck = CurvaCon(pr, AnimScale, q);
                        if (ck) CompPon(S, q, CompDe(base.s, q) + ck->EvalF(f, 0.0f) - ck->EvalF(ini, 0.0f));
                    }
                } else {
                    std::vector<AnimProperty>& prm = const_cast<std::vector<AnimProperty>&>(pr);
                    R = EvalPropVecF(prm, AnimRotation, f, base.r);
                    if (!soloRot){ P = EvalPropVecF(prm, AnimPosition, f, base.p); S = EvalPropVecF(prm, AnimScale, f, base.s); }
                }
                if (c.modo == 0){ A.p = A.p + (P - A.p) * w; A.r = A.r + (R - A.r) * w; A.s = A.s + (S - A.s) * w; }
                else { A.p = A.p + (P - base.p) * (w * sg); A.r = A.r + (R - base.r) * (w * sg); A.s = A.s + (S - base.s) * (w * sg); }
            }
        }
    }
    for (std::map<Object*, W3dMixBase>::iterator it = acc.begin(); it != acc.end(); ++it){
        Object* o = it->first;
        o->pos = it->second.p; o->SetRotEuler(it->second.r); o->scale = it->second.s;
        o->posadoPorCurvas = true;
    }
    // JUEGO: varias animaciones de escena A LA VEZ (una por capa: la cinematica de la puerta, la de las
    // luces, la del cielo...). Una capa "mezclar" a peso ENTERO es la animacion tal cual, asi que ademas
    // de la transformacion aplica lo que no se mezcla: visibilidad, formas, fov, colores de luz y sus
    // cortes de camara (el de la ultima capa que tenga). Las capas parciales solo mezclan transformacion.
    if (juego){
        int corteEsc = -1, corteFrame = 0;
        for (size_t k = 0; k < g_mixEscenas.size(); k++){
            W3dCapaAnim& c = g_mixEscenas[k];
            if (!c.visible || c.modo != 0 || c.influencia < 99.95f) continue;
            int idx = W3dAnimEscenaIdx(c.anim.c_str()); if (idx < 0) continue;
            SceneAnimation* sa = SceneAnimations[idx];
            std::vector<AnimationObject>& objs = (idx == SceneAnimActiva) ? AnimationObjects : sa->objetos;
            const float f = (float)sa->startFrame + c.juegoFrame;
            for (size_t i = 0; i < objs.size(); i++)
                if (objs[i].obj) AplicarCurvasJuegoCanales(objs[i].obj, objs[i].Propertys, f, 0);
            if (!sa->camPista.empty() && !sa->camPista[0].keyframes.empty()) { corteEsc = idx; corteFrame = (int)f; }
        }
        if (corteEsc >= 0) W3dCamActivaAplicar(corteEsc, corteFrame);
        // idem las capas de jerarquia "mezclar" a peso entero: visible, formas, fov y luz de sus nodos
        for (size_t r = 0; r < raicesCapas.size(); r++){
            Object* raiz = raicesCapas[r];
            std::vector<W3dCapaAnim>& capas = raiz->clipsJer->capas;
            for (size_t k = 0; k < capas.size(); k++){
                const W3dCapaAnim& c = capas[k];
                if (!c.visible || c.modo != 0 || c.influencia < 99.95f) continue;
                const W3dClipJer* clip = W3dJerCapaClip(raiz, c, 0);
                if (!clip) continue;
                const float f = (float)clip->inicio + c.juegoFrame;
                for (size_t p = 0; p < clip->pistas.size(); p++){
                    Object* n = W3dJerNodo(raiz, clip->pistas[p].ruta);
                    if (n) AplicarCurvasJuegoCanales(n, const_cast<std::vector<AnimProperty>&>(clip->pistas[p].props), f, 0);
                }
            }
        }
    }
    return true;
}
// JUEGO: avanza el cabezal de cada capa de escena (fps de SU escena) y aplica la mezcla
void W3dMixEscenasTick(float dt){
    std::vector<Object*> raicesCapas;
    RaicesConCapas(raicesCapas);
    if (g_mixEscenas.empty() && raicesCapas.empty()) return;
    // las capas de clips de jerarquia: cada una con su cabezal (fps de SU clip)
    for (size_t r = 0; r < raicesCapas.size(); r++){
        std::vector<W3dCapaAnim>& capas = raicesCapas[r]->clipsJer->capas;
        for (size_t k = 0; k < capas.size(); k++){
            W3dCapaAnim& c = capas[k];
            const W3dClipJer* clip = W3dJerCapaClip(raicesCapas[r], c, 0);
            if (!clip) continue;
            const float largo = (float)(clip->fin - clip->inicio);
            if (!c.juegoTermino) c.juegoFrame += dt * (float)(clip->fps > 0 ? clip->fps : 30) * c.vel;
            if (largo <= 0.0f || FrameRaro(c.juegoFrame)) c.juegoFrame = 0.0f;
            else if (c.loop) c.juegoFrame = W3dCicloEnvolver(c.juegoFrame, largo + 1.0f);
            else if (c.juegoFrame >= largo) { c.juegoFrame = largo; c.juegoTermino = true; }
        }
    }
    InitSceneAnimations();
    for (size_t k = 0; k < g_mixEscenas.size(); k++){
        W3dCapaAnim& c = g_mixEscenas[k];
        int idx = W3dAnimEscenaIdx(c.anim.c_str()); if (idx < 0) continue;
        SceneAnimation* sa = SceneAnimations[idx];
        const float largo = (float)(sa->endFrame - sa->startFrame);
        if (!c.juegoTermino) c.juegoFrame += dt * (float)(sa->fps > 0 ? sa->fps : 30) * c.vel;
        if (largo <= 0.0f || FrameRaro(c.juegoFrame)) c.juegoFrame = 0.0f;
        else if (c.loop) c.juegoFrame = W3dCicloEnvolver(c.juegoFrame, largo + 1.0f);
        else if (c.juegoFrame >= largo) { c.juegoFrame = largo; c.juegoTermino = true; }
    }
    W3dMixEscenasAplicar(0, true);
}
