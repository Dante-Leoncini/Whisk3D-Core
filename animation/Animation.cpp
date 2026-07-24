#include "Animation.h"
#include <algorithm>   // std::sort (fuera el quicksort a mano)
#include "math/Vector3.h"        // EvalPropVec (las 3 curvas de una propiedad)
#include "SkeletalAnimation.h"   // clips de armature (startFrame/endFrame/FrameRate) para el rango por animacion
#include "objects/Armature.h"    // ActiveAnimArm->animations / animActiva
#include <stdio.h> // sprintf GLOBAL (nombre unico de escena); Symbian/STLport no tiene std::snprintf
#include <ctime>

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
int ActiveAnimKind = 0;          // 0 = escena, 1 = clip de armature
Armature* ActiveAnimArm = NULL;  // armature del clip activo (kind 1)

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

void SetEscenaActiva(int idx) {
    InitSceneAnimations();
    if (idx < 0 || idx >= (int)SceneAnimations.size() || idx == SceneAnimActiva) return;
    // guardar las curvas de la escena que estaba activa; cargar las de la nueva (swap: sin copias)
    SceneAnimations[SceneAnimActiva]->objetos.swap(AnimationObjects);
    SceneAnimActiva = idx;
    AnimationObjects.swap(SceneAnimations[SceneAnimActiva]->objetos);
}

int NuevaEscena() {
    InitSceneAnimations();
    // guardar las curvas de la escena activa (AnimationObjects queda vacio = curvas de la escena nueva)
    SceneAnimations[SceneAnimActiva]->objetos.swap(AnimationObjects);
    AnimationObjects.clear();
    // nombre unico "Scene N"
    std::string nombre;
    for (int n = 2; ; n++) {
        char buf[32]; sprintf(buf, "Scene %d", n); // sprintf GLOBAL: Symbian/STLport no tiene std::snprintf
        nombre = buf; bool usado = false;
        for (size_t i = 0; i < SceneAnimations.size(); i++) if (SceneAnimations[i]->name == nombre) { usado = true; break; }
        if (!usado) break;
    }
    SceneAnimations.push_back(new SceneAnimation(nombre));
    SceneAnimActiva = (int)SceneAnimations.size() - 1;
    return SceneAnimActiva;
}

void RenombrarEscenaActiva(const std::string& nombre) {
    InitSceneAnimations();
    if (!nombre.empty()) SceneAnimations[SceneAnimActiva]->name = nombre;
}

void BorrarEscenaActiva() {
    InitSceneAnimations();
    if (SceneAnimations.size() <= 1) {          // siempre queda "Scene": solo se vacian sus curvas
        AnimationObjects.clear();
        SceneAnimations[0]->objetos.clear();
        return;
    }
    delete SceneAnimations[SceneAnimActiva];    // su ->objetos esta vacio (las curvas vivas estan en AnimationObjects)
    SceneAnimations.erase(SceneAnimations.begin() + SceneAnimActiva);
    if (SceneAnimActiva >= (int)SceneAnimations.size()) SceneAnimActiva = (int)SceneAnimations.size() - 1;
    AnimationObjects.clear();                    // descartar las curvas de la escena borrada
    AnimationObjects.swap(SceneAnimations[SceneAnimActiva]->objetos); // cargar las de la escena que quedo activa
}

// ===== Start/End/FPS PROPIOS por animacion (escena o clip). Los comparten la tarjeta y el timeline. =====
static SkeletalAnimation* AnimClipActivo(){
    if (ActiveAnimKind == 1 && ActiveAnimArm &&
        ActiveAnimArm->animActiva >= 0 && ActiveAnimArm->animActiva < (int)ActiveAnimArm->animations.size())
        return ActiveAnimArm->animations[ActiveAnimArm->animActiva];
    return NULL;
}
void AnimCargarRangoActivo(){
    SkeletalAnimation* c = AnimClipActivo();
    if (c){ StartFrame = c->startFrame; EndFrame = c->endFrame; if (c->FrameRate > 0) AnimFPS = c->FrameRate; }
    else { InitSceneAnimations(); SceneAnimation* s = SceneAnimations[SceneAnimActiva];
           StartFrame = s->startFrame; EndFrame = s->endFrame; AnimFPS = s->fps; }
    if (CurrentFrame < StartFrame) CurrentFrame = StartFrame;
    if (CurrentFrame > EndFrame && EndFrame >= StartFrame) CurrentFrame = EndFrame;
}
void AnimSetStart(int v){ SkeletalAnimation* c = AnimClipActivo(); if (c) c->startFrame = v;
    else { InitSceneAnimations(); SceneAnimations[SceneAnimActiva]->startFrame = v; } StartFrame = v; }
void AnimSetEnd(int v){ SkeletalAnimation* c = AnimClipActivo(); if (c) c->endFrame = v;
    else { InitSceneAnimations(); SceneAnimations[SceneAnimActiva]->endFrame = v; } EndFrame = v; }
void AnimSetFps(int v){ SkeletalAnimation* c = AnimClipActivo(); if (c) c->FrameRate = v;
    else { InitSceneAnimations(); SceneAnimations[SceneAnimActiva]->fps = v; } AnimFPS = v; }

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


// orden por numero de frame (para std::sort)
static bool compareKeyFrames(const keyFrame& a, const keyFrame& b) {
    return a.frame < b.frame;
}

// AnimProperty
void AnimProperty::SortKeyFrames() {
    std::sort(keyframes.begin(), keyframes.end(), compareKeyFrames);
}

// ============================================================================
//  EVALUACION de UNA curva (un canal). Es el UNICO lugar donde se interpola: el esqueleto y los objetos
//  llaman aca, asi que agregar bezier/easing se hace en un solo sitio (y el editor de curvas lo hereda).
// ============================================================================
// Handle EFECTIVO del keyframe i (offset dF/dV desde el). Ver el enum H* en Animation.h.
void AnimProperty::HandleEfectivo(size_t i, bool salida, float& dF, float& dV) const {
    const size_t n = keyframes.size();
    const keyFrame& k = keyframes[i];
    dF = 0.0f; dV = 0.0f;
    if (n < 2) return;

    // el vecino de ESTE lado (en las puntas se usa el unico que hay: el handle sale simetrico)
    size_t vec = salida ? (i + 1 < n ? i + 1 : (i > 0 ? i - 1 : i))
                        : (i > 0 ? i - 1 : (i + 1 < n ? i + 1 : i));
    // largo por defecto: un tercio del tramo de este lado, con el signo del lado
    float largo = (float)(keyframes[vec].frame - k.frame) / 3.0f;
    if (salida  && largo <= 0.0f) largo =  -largo;   // en la punta derecha el vecino esta a la IZQUIERDA
    if (!salida && largo >= 0.0f) largo =  -largo;
    if (largo == 0.0f) largo = salida ? 1.0f : -1.0f;

    if (k.handleType == HFree || k.handleType == HAligned){
        dF = salida ? k.outDF : k.inDF;
        dV = salida ? k.outDV : k.inDV;
        return;
    }
    if (k.handleType == HVector){
        // apunta al vecino: el tramo de este lado sale RECTO
        int df = keyframes[vec].frame - k.frame;
        if (df == 0){ dF = largo; dV = 0.0f; return; }
        float t = largo / (float)df;                        // fraccion hacia el vecino (1/3, con el signo del lado)
        dF = largo;
        dV = (keyframes[vec].value - k.value) * t;
        return;
    }
    // HAuto / HAutoClamped: pendiente suave (Catmull-Rom) por los dos vecinos
    size_t a = (i == 0) ? 0 : i - 1;
    size_t b = (i + 1 >= n) ? n - 1 : i + 1;
    int spanAB = keyframes[b].frame - keyframes[a].frame;
    float m = (spanAB != 0) ? (keyframes[b].value - keyframes[a].value) / (float)spanAB : 0.0f;
    if (k.handleType == HAutoClamped && i > 0 && i + 1 < n){
        // PICO: si los dos vecinos caen del mismo lado, este keyframe es un maximo/minimo local -> handle PLANO,
        // asi la curva no se pasa de largo (que es justo lo que arruina una animacion: un rebote que se hunde
        // por debajo del piso).
        float dPrev = keyframes[i-1].value - k.value;
        float dNext = keyframes[i+1].value - k.value;
        if ((dPrev > 0.0f && dNext > 0.0f) || (dPrev < 0.0f && dNext < 0.0f)) m = 0.0f;
    }
    dF = largo;
    dV = m * largo;
    if (k.handleType == HAutoClamped){
        // ...y ademas el punto de control no puede irse mas alla del valor del VECINO. Aplanar solo los picos no
        // alcanza: el handle de un keyframe que NO es pico igual puede empujar la curva por encima del vecino
        // (una bezier vive dentro de la cascara de sus puntos de control, asi que acotando el control se acota la
        // curva). Esto es lo que hace que "clamped" de verdad no se pase.
        float vv = keyframes[vec].value;
        float lo = (k.value < vv) ? k.value : vv;
        float hi = (k.value > vv) ? k.value : vv;
        float cp = k.value + dV;
        if (cp < lo) dV = lo - k.value;
        else if (cp > hi) dV = hi - k.value;
    }
}

float AnimProperty::Eval(int frame, float def) const { return EvalF((float)frame, def); }

float AnimProperty::EvalF(float frame, float def) const {
    const size_t n = keyframes.size();
    if (n == 0) return def;                                  // canal sin keyframes -> su valor de reposo
    if (frame <= (float)keyframes[0].frame)     return keyframes[0].value;      // antes del 1ro: clamp
    if (frame >= (float)keyframes[n-1].frame)   return keyframes[n-1].value;    // despues del ultimo: clamp
    for (size_t i = 1; i < n; i++) {
        if ((float)keyframes[i].frame < frame) continue;
        const keyFrame& a = keyframes[i-1];
        const keyFrame& b = keyframes[i];
        // ESCALON: mantiene a.value hasta el proximo key... pero EN el frame de b ya vale b.value. El bucle corta
        // con keyframes[i].frame >= frame, asi que aca 'frame' puede ser EXACTAMENTE b.frame: sin este chequeo el
        // escalon caia un frame tarde (y el editor lo dibuja en b, con lo cual lo que veias no era lo que corria).
        if (a.Interpolation == KfConstant) return (frame >= (float)b.frame) ? b.value : a.value;
        int span = b.frame - a.frame;
        if (span <= 0) return b.value;
        float t = (frame - (float)a.frame) / (float)span;
        if (a.Interpolation != KfBezier)
            return a.value + (b.value - a.value) * t;         // lineal
        return EvalBezier(i, frame);
    }
    return keyframes[n-1].value;
}

// Valor del tramo BEZIER [i-1, i] en 'frame'. Los handles son PUNTOS, asi que el tramo es una bezier cubica en
// (frame, valor): x(t) e y(t). Para saber el valor en un frame hay que despejar el t cuyo x(t) es ese frame — no
// alcanza con meter t = fraccion del tramo, porque los handles corren el x. Es lo mismo que hace una curva de
// easing: el eje del tiempo tambien esta curvado.
float AnimProperty::EvalBezier(size_t i, float frame) const {
    const keyFrame& a = keyframes[i-1];
    const keyFrame& b = keyframes[i];
    const float x0 = (float)a.frame, x3 = (float)b.frame;
    const float span = x3 - x0;
    if (span <= 0.0f) return b.value;

    float aDF, aDV, bDF, bDV;
    HandleEfectivo(i-1, true,  aDF, aDV);   // handle de SALIDA del izquierdo
    HandleEfectivo(i,   false, bDF, bDV);   // handle de ENTRADA del derecho
    float x1 = x0 + aDF, x2 = x3 + bDF;
    // x TIENE que ser monotono o la curva deja de ser una funcion del tiempo (se doblaria hacia atras y un frame
    // tendria dos valores). Los puntos de control se clampean al tramo: es lo mismo que hace cualquier editor.
    if (x1 < x0) x1 = x0; if (x1 > x3) x1 = x3;
    if (x2 < x0) x2 = x0; if (x2 > x3) x2 = x3;
    const float y0 = a.value, y1 = a.value + aDV, y2 = b.value + bDV, y3 = b.value;

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
    for (size_t k = 0; k < n; k++) if (ap.keyframes[k].frame == frame){ i = k; break; }
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
    keyFrame kf; kf.frame = frame; kf.value = value; kf.Interpolation = KfLinear;
    size_t pos = 0; while (pos < ap.keyframes.size() && ap.keyframes[pos].frame < frame) pos++;
    ap.keyframes.insert(ap.keyframes.begin() + pos, kf);   // insertado EN ORDEN (no hace falta ordenar)
}

// AnimationObject
void AnimationObject::UpdateFirstLastFrame() {
    FirstKeyFrame = 100000000;
    LastKeyFrame = 0;
    for(size_t pr = 0; pr < Propertys.size(); pr++) {    
        for(size_t kf = 0; kf < Propertys[pr].keyframes.size(); kf++) {    
            if (Propertys[pr].keyframes[kf].frame > LastKeyFrame){
                LastKeyFrame = Propertys[pr].keyframes[kf].frame;    
            }
            if (Propertys[pr].keyframes[kf].frame < FirstKeyFrame){
                FirstKeyFrame = Propertys[pr].keyframes[kf].frame;    
            }
        }    
    }
}

// Vector global de objetos animados
std::vector<AnimationObject> AnimationObjects;
