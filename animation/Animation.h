#ifndef ANIMATION_H
#define ANIMATION_H

#ifdef _WIN32
    #define NOMINMAX
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <vector>
// reloj de milisegundos de la plataforma (lo provee el EDITOR; el core no
// depende de ninguna libreria de ventana). Ver w3dGetTicks en main.cpp.
// RELOJ del motor, en milisegundos. Lo DEFINE el Core (respaldo portable con clock()), asi que
// un proyecto puede usar el motor sin tener que adivinar que hay que definir un simbolo global
// con este nombre exacto. Si la plataforma tiene un reloj mejor (SDL_GetTicks, GetTickCount,
// User::TickCount), se registra con W3dSetClock y el motor lo usa.
typedef unsigned int (*W3dClockFn)();
void W3dSetClock(W3dClockFn fn);
unsigned int w3dGetTicks();
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>   // N95: OpenGL ES 1.1 (GLshort/GLbyte/GLfloat)
#else
    #include <GL/gl.h>     // PC: OpenGL de escritorio
#endif

#include "objects/Objects.h"

class W3dClipHier;   // animation/W3dAnimSet.h: un clip de jerarquia (la vista de edicion lo nombra)

// Variables globales
extern bool PlayAnimation;   // true = reproduciendo (avanza CurrentFrame en cada tick)
extern int AnimPlayDir;      // direccion del play: +1 adelante, -1 en reversa
extern int AnimFPS;          // fps de REPRODUCCION de las animaciones (editable en la pestania Render; default 30).
                             // La UI puede ir a 60 fps: el frame de animacion se repite y NO se recalcula la pose.
extern int StartFrame;
extern int EndFrame;
// MODO JUEGO (checkbox "Es un juego" de la tarjeta Animacion): la linea de tiempo es
// INFINITA (sin Fin, sin loop, sin Render Animation); el PLAY corre la simulacion de
// scripts y el editor graba el estado frame a frame (ver main/script/SimJuego).
extern bool AnimIsGame;
extern int CurrentFrame;

// avanza CurrentFrame un paso (si PlayAnimation) haciendo loop entre Start..End. Lo llama el main loop
// en cada tick de animacion. Respeta AnimPlayDir (play normal / reversa).
void AnimTick();

extern unsigned int lastAnimTime;
extern unsigned int lastRenderTime;

// Funciones de animación

// Constantes de animación
// AnimVisible / AnimRender: curvas 0/1 (se evalua >= 0.5). Componente: AnimX.
// AnimFov: campo de vision de la CAMARA (escalar, componente AnimX). AnimColor: color RGB de una LUZ
// o MATERIAL (3 curvas: R=AnimX, G=AnimY, B=AnimZ). Van AL FINAL: los .w3d viejos guardan el int del
// canal, agregar valores nuevos no corre los existentes.
//   AnimClip:      CAMARA, distancia de dibujado: near=AnimX, far=AnimY
//   AnimAmbient:   LUZ, color ambient RGB (X/Y/Z)
//   AnimSpecular:  LUZ, color specular RGB (X/Y/Z)
//   AnimAtten:     LUZ, atenuacion: constante=AnimX, lineal=AnimY, cuadratica=AnimZ
//   AnimSpot:      LUZ, spot: cutoff(grados)=AnimX, exponent=AnimY
//   AnimLightMisc: LUZ, GL light index=AnimX, directional(0/1)=AnimY (interpolacion CONSTANTE)
//   AnimCamActive: la pista "CAMARA ACTIVA" de una animacion de ESCENA (SceneAnimation::camTrack). El valor de
//                  cada keyframe es un INDICE en SceneAnimation::camNames; interpolacion CONSTANTE (un corte).
enum { AnimPosition, AnimRotation, AnimScale, AnimVisible, AnimRender, AnimVertex, AnimFov, AnimColor,
       AnimClip, AnimAmbient, AnimSpecular, AnimAtten, AnimSpot, AnimLightMisc, AnimCamActive, AnimShape, AnimStep,
       AnimSound, AnimLightIntens, AnimCamFocus, AnimBackground, AnimBackgroundRot, AnimBackgroundTex,
       AnimMatColor, AnimMatAlpha, AnimMatEmission, AnimMatEmitStrength, AnimMatSpecular, AnimMatRoughness,
       AnimMatMetallic, AnimMatTransmission, AnimMatLayerForce, AnimMatUVOffset, AnimMatUVScale, AnimMatUVRotation };
//   AnimMat*: los canales de un MATERIAL (sus curvas van en las pistas de material de una animacion de escena,
//             SceneAnimation::materials, no en las de un objeto; ver formato/material-animado.md). AnimColor y
//             AnimSpecular siguen siendo los de una LUZ:
//     AnimMatColor        el color base RGB (diffuse[0..2]: X/Y/Z = R/G/B)     AnimMatAlpha     el alfa (diffuse[3], X)
//     AnimMatEmission     el color de emision RGB (X/Y/Z)                       AnimMatEmitStrength la fuerza de la emision (X)
//     AnimMatSpecular     specular RGB (X/Y/Z)                                  AnimMatRoughness / AnimMatMetallic (X)
//     AnimMatTransmission la transmision (X)
//     AnimMatLayerForce   el factor de una CAPA de textura: el COMPONENTE es el indice de la capa (como AnimShape)
//     AnimMatUVOffset / AnimMatUVScale  la transformacion UV: X = U, Y = V     AnimMatUVRotation  en grados (X)
//   AnimBackground:    el COLOR del FONDO del render (X/Y/Z = R/G/B). Las curvas del fondo viven en la COLECCION de la escena
//   AnimBackgroundRot: la ROTACION de la textura 360 del fondo, en grados (X/Y/Z)
//   AnimBackgroundTex: QUE fondo (AnimX, interpolacion CONSTANTE): 0 = el color; k = la textura k de la lista del fondo
//   Los tres los aplica el gancho g_animBackgroundHook (el fondo es del render: el Core no lo conoce)
//   AnimLightIntens: LUZ, la intensidad (AnimX)
//   AnimCamFocus: CAMARA, la distancia de foco de la profundidad de campo (AnimX, m)
//   AnimSound: un SONIDO (main/objects/Sound.h): volumen=AnimX, tono (pitch)=AnimY, envio a la reverb=AnimZ. Lo
//               aplica el gancho g_animSoundHook (el Core no conoce la clase)
//   AnimStep: un PASO de una RUTINA (main/objects/Routine.h); el COMPONENTE es id del paso * 8 + campo (0..4 = sus
//             numeros, 5 = on/off): mover el paso no lo cambia (el id es suyo). Lo aplica el gancho g_animStepHook.
enum { AnimStepFields = 8, AnimStepOn = 5 };
//   AnimShape: PESO de una SHAPE KEY de una malla (Mesh::shapeKeys); el COMPONENTE es el indice de la forma (0..n-1,
//              no solo X/Y/Z). Ej: la cara que habla en una cinematica.
// AnimVertex: NO es una curva de canal (no vive en AnimProperty). Es el id que usa el
// dope sheet para la fila de KEYFRAMES DE VERTICES (los frames de la vertex anim). El
// dope lo trata como un canal seleccionable, pero las operaciones (mover/borrar/interp)
// se rutean por el prefijo "objvtx:" del ownerKey a los VertexFrame (su fuente de verdad).
// COMPONENTE de una propiedad. Cada uno es una CURVA INDEPENDIENTE: X puede tener keyframes en frames
// distintos que Y o Z (antes los tres compartian el mismo keyframe y no se les podia dar curva propia).
enum { AnimX = 0, AnimY = 1, AnimZ = 2 };
// Interpolacion del TRAMO que SALE de un keyframe (igual que el escalon: manda el keyframe IZQUIERDO).
//   KfConstant = escalon: se mantiene y cambia de golpe EXACTAMENTE en el frame del proximo keyframe
//   KfLinear   = recta
//   KfBezier   = curva; es la unica que tiene handles
// OJO: el default de keyFrame es KfLinear, NO Constant.
enum { KfConstant = 0, KfLinear = 1, KfBezier = 2,
       // EASING: curvas CALCULADAS, sin handles. El valor va de A a B segun una funcion f(t) del tiempo del tramo.
       //   por fuerza: Sinusoidal, Cuadratica (t^2), Cubica (t^3), Cuartica (t^4), Quintica (t^5), Exponencial, Circular
       //   efectos:    Back (se pasa un poco y vuelve), Bounce (rebota), Elastic (oscila como un resorte)
       KfSine = 3, KfQuad = 4, KfCubic = 5, KfQuart = 6, KfQuint = 7, KfExpo = 8, KfCirc = 9,
       KfBack = 10, KfBounce = 11, KfElastic = 12, KfInterpN = 13 };
// El EASING de esas curvas (de que lado del tramo va el efecto) viaja en los bits 8-9 de la MISMA Interpolation:
// asi no cambia ningun formato (.w3d, .w3da, el undo, las curvas compactas guardan el int tal cual). Solo las de
// easing los usan: Constant / Linear / Bezier nunca los tienen (sus "== KfBezier" de siempre siguen valiendo).
//   KfEaseAuto = el habitual de cada curva: Ease In, salvo Back / Bounce / Elastic que son Ease Out
//   KfEaseIn   = el efecto al ARRANCAR el tramo     KfEaseOut = al LLEGAR   KfEaseInOut = en las dos puntas
enum { KfEaseAuto = 0, KfEaseIn = 1, KfEaseOut = 2, KfEaseInOut = 3 };
inline int  KfInterpBase(int interp)  { return interp & 0xFF; }
inline int  KfInterpEase(int interp)  { return (interp >> 8) & 3; }
inline int  KfInterpWith(int base, int ease) { return (base >= KfSine) ? (base | (ease << 8)) : base; }
inline bool KfIsEasing(int interp)    { return KfInterpBase(interp) >= KfSine && KfInterpBase(interp) < KfInterpN; }
// f(t) de una curva de easing: t en [0..1] del tramo -> fraccion del camino de A a B (0 en A, 1 en B; Back y
// Elastic se pasan un poco de 0..1). Para las que no son de easing devuelve t.
float KfEasing(int interp, float t);

// CANALES de un "Insert Keyframe" (mascara de bits). La comparten los TRES caminos que keyframean un
// transform: InsertKeyframeObject (objetos), InsertKeyframeSkeleton (pose 3D) e InsertKeyframeArm2D
// (pose 2D). El menu desplegable de Insert Keyframe elige la mascara: Todos / Solo localizacion / Solo
// rotacion / Solo escala. 0 se trata como KfChannelAll (el default historico: se keyeaba todo siempre).
// OJO: los keyframes de VERTICES (posiciones/normales/UV de una vertex anim) NO usan esta mascara: una
// pose de vertices es una sola cosa, no tiene canales -> ahi la 'i' inserta directo, sin menu.
enum { KfChannelLoc = 1, KfChannelRot = 2, KfChannelScl = 4, KfChannelAll = 7 };

// Tipo del par de handles de un keyframe bezier. Los tres ultimos se CALCULAN solos desde los vecinos (lo que
// haya guardado en los offsets se ignora); los dos primeros los pone el usuario.
//   HFree        los dos lados van por libre, cada uno donde lo dejes
//   HAligned     los dos lados quedan SIEMPRE en la misma recta (mover uno gira el otro; cada uno conserva su largo)
//   HVector      cada lado apunta al keyframe vecino -> el tramo sale recto
//   HAuto        pendiente suave que pasa por los vecinos (Catmull-Rom)
//   HAutoClamped como HAuto, pero se aplana en los picos: la curva NUNCA se pasa del valor de los keyframes
//   HPlane       los dos lados HORIZONTALES (un tercio del tramo): ease in/out en cada keyframe. Es el default de
//                un keyframe nuevo; calculado (no guarda offsets) -> las curvas siguen compactandose
enum { HFree = 0, HAligned = 1, HVector = 2, HAuto = 3, HAutoClamped = 4, HPlane = 5 };

// Keyframe de UNA curva: UN solo valor (el de su canal) + como sale de el.
class keyFrame {
public:
    int frame;
    GLfloat value;          // valor de ESTE canal (X, Y o Z segun la AnimProperty que lo contiene)
    int Interpolation;      // KfConstant / KfLinear / KfBezier
    int handleType;         // HFree / HAligned / HVector / HAuto / HAutoClamped
    // Handles guardados como OFFSET desde el keyframe, en (frames, valor). Es un PUNTO, no una pendiente: hace
    // falta para poder girarlos y para que escalarlos alargue la distancia. Con offsets (y no puntos absolutos)
    // mover el keyframe se lleva los handles solo. Solo valen para HFree/HAligned; los demas tipos los calcula
    // EffectiveHandle desde los vecinos.
    GLfloat inDF, inDV;     // handle de ENTRADA: dF < 0 (queda a la izquierda del keyframe)
    GLfloat outDF, outDV;   // handle de SALIDA:  dF > 0 (a la derecha)
    keyFrame() : frame(0), value(0.0f), Interpolation(KfLinear), handleType(HAuto),
                 inDF(0.0f), inDV(0.0f), outDF(0.0f), outDV(0.0f) {}
};

// ============================================================================
//  LAS CLAVES DE UNA CURVA (AnimProperty::keyframes). Por fuera es un std::vector<keyFrame> (la misma API que
//  usa todo el editor); por dentro puede estar COMPACTA: una curva HORNEADA (frames regulares, la misma
//  interpolacion y el mismo tipo de handle en todas, handles guardados en cero: lo que trae un glTF, un keyframe
//  por frame; los handles automaticos se calculan de los vecinos) se guarda como primer frame + paso + UN float
//  por clave, ~4 B por clave en vez de los 32 de un keyFrame. Una curva con los frames IRREGULARES (crecientes)
//  pero lo demas igual (una animacion horneada y reducida: solo las claves que hacen falta) tambien: su frame y
//  su valor, ~8 B por clave (paso = 0), y la evaluacion busca el tramo por biseccion en vez de recorrerla. La crea
//  el lector del .w3da (W3dAnimSet.cpp) y se copia compacta (variantes, copy-on-write, undo).
//    - SIN expandir: size/empty, Frame/Valor/Interp/TypeH/Key(i), FirstFrame/LastFrame, la evaluacion
//      (AnimProperty::Eval/EvalF/EffectiveHandle: el juego y la reproduccion), el escritor del .w3da, copiar y
//      comparar.
//    - lo demas de la API de vector (operator[], at, begin/end, push_back, insert, erase...) la EXPANDE primero,
//      tambien en un acceso const: desde ahi es una curva comun (el editor, el dope sheet y el undo, como siempre).
//  La evaluacion es UNA sola funcion para las dos (lee con Frame/Valor/...): dan los mismos bits (lo compara en
//  todos los frames, enteros y fraccionarios, tools/pruebas/prueba_claves_compactas.w3s).
// ============================================================================
// el bloque de una curva compacta: esta cabecera y, en la MISMA reserva, sus n valores (y en una IRREGULAR,
// despues, sus n frames)
struct W3dCompactKeys {
    int first;     // frame de la clave 0
    int step;      // frame de la clave i = primero + paso * i (paso > 0; el ultimo entra en un int).
                     // 0 = IRREGULAR: los frames de cada clave van despues de los valores (estrictamente crecientes)
    int interp;      // la interpolacion de TODAS
    int typeH;       // el tipo de handle de todas (los guardados son cero: HFree/HAligned quedan en cero)
    unsigned n;      // cantidad de claves (>= 2)
    float*       Values()       { return reinterpret_cast<float*>(this + 1); }
    const float* Values() const { return reinterpret_cast<const float*>(this + 1); }
    bool Irregular() const { return step == 0; }
    int*       Frames()       { return reinterpret_cast<int*>(Values() + n); }        // (solo las irregulares)
    const int* Frames() const { return reinterpret_cast<const int*>(Values() + n); }
    // frame de la clave i, en aritmetica sin signo: el mismo resultado que sumar el paso clave a clave (el lector)
    int Frame(unsigned i) const { return step ? (int)((unsigned)first + (unsigned)step * i) : Frames()[i]; }
};

class W3dKeys {
public:
    typedef std::vector<keyFrame> Vec;
    typedef Vec::value_type value_type;
    typedef Vec::size_type size_type;
    typedef Vec::difference_type difference_type;
    typedef Vec::reference reference;
    typedef Vec::const_reference const_reference;
    typedef Vec::pointer pointer;
    typedef Vec::const_pointer const_pointer;
    typedef Vec::iterator iterator;
    typedef Vec::const_iterator const_iterator;
    typedef Vec::reverse_iterator reverse_iterator;
    typedef Vec::const_reverse_iterator const_reverse_iterator;

    W3dKeys() : c_(0) {}
    W3dKeys(const W3dKeys& o);
    ~W3dKeys();
    W3dKeys& operator=(const W3dKeys& o);
    W3dKeys& operator=(const Vec& v);
#if defined(__cplusplus) && __cplusplus >= 201103L
    // (C++11 o mas: MOVER no copia las claves -un vector<AnimProperty> que crece las mueve, como cuando era un
    //  std::vector-; en C++03 se copian, como siempre)
    W3dKeys(W3dKeys&& o) noexcept : c_(0) { swap(o); }
    W3dKeys& operator=(W3dKeys&& o) noexcept { swap(o); return *this; }
#endif

    // ---- SIN EXPANDIR ----
    size_type size() const { return c_ ? (size_type)c_->n : v_.size(); }
    bool empty() const { return c_ ? false : v_.empty(); }        // (una compacta tiene 2 claves o mas)
    bool Compact() const { return c_ != 0; }
    const W3dCompactKeys* CompactData() const { return c_; }   // NULL = curva comun
    int   Frame(size_type i) const { return c_ ? c_->Frame((unsigned)i) : v_[i].frame; }
    float Value(size_type i) const { return c_ ? c_->Values()[i] : v_[i].value; }
    int   Interp(size_type i) const { return c_ ? c_->interp : v_[i].Interpolation; }
    int   TypeH(size_type i) const { return c_ ? c_->typeH : v_[i].handleType; }
    // el handle GUARDADO de la clave i (offset en frames / en valor), el de salida o el de entrada
    float HandleDF(size_type i, bool output) const { return c_ ? 0.0f : (output ? v_[i].outDF : v_[i].inDF); }
    float HandleDV(size_type i, bool output) const { return c_ ? 0.0f : (output ? v_[i].outDV : v_[i].inDV); }
    keyFrame Key(size_type i) const;             // la clave i entera, POR VALOR (handles en cero si es compacta)
    int FirstFrame() const { return Frame(0); }  // (con claves: !empty())
    int LastFrame() const { return Frame(size() - 1); }
    long BytesMemory() const;                    // lo que ocupan sus claves en el heap (vector o bloque compacto)
    // igual clave a clave: frame, interpolacion y tipo de handle; valor y handles BIT a BIT (un -0.0 no es 0.0)
    bool SameBits(const W3dKeys& o) const;
    // ---- el lector del .w3da ----
    // n claves con frames primero + paso * i se pueden guardar compactas? (2 o mas, paso > 0, el ultimo en un int)
    static bool CompactPossible(size_type n, int first, int step);
    // la deja COMPACTA (descarta lo que tuviera) y devuelve donde van sus n valores. Antes: CompactPossible.
    float* CompactCreate(size_type n, int first, int step, int interp, int typeH);
    // n claves con estos frames se pueden guardar compactas IRREGULARES? (2 o mas, estrictamente crecientes)
    static bool CompactIrregularPossible(size_type n, const int* frames);
    // la deja COMPACTA IRREGULAR con esos frames (los copia) y devuelve donde van sus n valores. Antes:
    // CompactIrregularPossible.
    float* CompactCreateIrregular(size_type n, const int* frames, int interp, int typeH);

    // ---- la API de std::vector<keyFrame>: EXPANDEN (una compacta pasa a ser una curva comun) ----
    reference       operator[](size_type i)       { Expand(); return v_[i]; }
    const_reference operator[](size_type i) const { Expand(); return v_[i]; }
    reference       at(size_type i)       { Expand(); return v_.at(i); }
    const_reference at(size_type i) const { Expand(); return v_.at(i); }
    reference       front()       { Expand(); return v_.front(); }
    const_reference front() const { Expand(); return v_.front(); }
    reference       back()       { Expand(); return v_.back(); }
    const_reference back() const { Expand(); return v_.back(); }
    iterator       begin()       { Expand(); return v_.begin(); }
    const_iterator begin() const { Expand(); return v_.begin(); }
    iterator       end()       { Expand(); return v_.end(); }
    const_iterator end() const { Expand(); return v_.end(); }
    reverse_iterator       rbegin()       { Expand(); return v_.rbegin(); }
    const_reverse_iterator rbegin() const { Expand(); return v_.rbegin(); }
    reverse_iterator       rend()       { Expand(); return v_.rend(); }
    const_reverse_iterator rend() const { Expand(); return v_.rend(); }
    size_type capacity() const { Expand(); return v_.capacity(); }
    size_type max_size() const { return v_.max_size(); }
    void push_back(const keyFrame& k);
    void pop_back() { Expand(); v_.pop_back(); }
    iterator insert(iterator pos, const keyFrame& k);
    void insert(iterator pos, size_type n, const keyFrame& k);
    template <class It> void insert(iterator pos, It a, It b) {
        Expand(); const size_type prev = v_.capacity(); v_.insert(pos, a, b); CountCapacity(prev);
    }
    iterator erase(iterator pos) { Expand(); return v_.erase(pos); }
    iterator erase(iterator a, iterator b) { Expand(); return v_.erase(a, b); }
    void clear();                                 // (una compacta se suelta: queda una curva comun vacia)
    void resize(size_type n, const keyFrame& k = keyFrame());
    void reserve(size_type n);
    void assign(size_type n, const keyFrame& k);
    template <class It> void assign(It a, It b) {
        ReleaseCompact(); const size_type prev = v_.capacity(); v_.assign(a, b); CountCapacity(prev);
    }
    void swap(W3dKeys& o);
    void swap(Vec& v);
    // las claves como el vector de siempre (const: para leer). Expande.
    operator const Vec&() const { Expand(); return v_; }

private:
    mutable Vec v_;                // la curva comun (vacio mientras es compacta)
    mutable W3dCompactKeys* c_;    // la compacta (NULL = comun)
    void Expand() const { if (c_) ExpandNow(); }
    void ExpandNow() const;
    void ReleaseCompact();
    void CountCapacity(size_type prev);
};

// MEDICION (meminfo): las claves de TODAS las curvas vivas. 'bytes' cuenta los vectores (su capacidad) y los
// bloques compactos; 'expansiones', las curvas compactas que pasaron a comunes desde el arranque (jugar no
// expande ninguna: prueba_claves_compactas.w3s).
struct W3dKeysStats {
    long bytes;           // bytes de claves vivos (vectores + bloques compactos)
    long compact;         // curvas compactas vivas
    long compactKeys;     // sus claves
    long compactBytes;    // sus bloques
    long expansions;      // compacta -> comun, desde el arranque
    W3dKeysStats() : bytes(0), compact(0), compactKeys(0), compactBytes(0), expansions(0) {}
};
void W3dKeysGetStats(W3dKeysStats& st);

// Una CURVA de animacion = (Property, component). Ej: (AnimPosition, AnimX) = "X Location".
// Cada componente tiene SUS PROPIOS keyframes -> se puede mover/borrar/curvar X sin tocar Y ni Z.
// REPETIR (modificador de la curva): que pasa ANTES del primer keyframe y DESPUES del ultimo.
//   KfCycleNo     = se queda en el valor de la punta (como siempre)
//   KfCycleRepeats = la curva entera se repite, igual, para siempre
//   KfCycleOffset = se repite CORRIDA: cada vuelta arranca donde termino la anterior (un paso que avanza, una
//                   rueda que sigue girando)
enum { KfCycleNo = 0, KfCycleRepeats = 1, KfCycleOffset = 2 };

// ============================================================================
// MODIFICADORES DE CURVA (la pila de cada curva, como los de Blender): cambian lo que la curva DEVUELVE sin tocar
// sus keyframes. Se evaluan igual en el editor y en el juego (AnimProperty::EvalF).
//   KfModCycles   repite la curva antes y/o despues de sus keyframes: Repeat Motion / with Offset / Mirrored, con
//                 cuantas veces (0 = para siempre). Es el Repetir de siempre pero con mas modos y en la pila.
//   KfModNoise    le suma ruido (turbulencia): Replace / Add / Subtract / Multiply; escala, fuerza, offset, fase,
//                 profundidad (octavas), lacunaridad y rugosidad. "Legacy" = el de Blender viejo (2 y 0.5 fijos)
//   KfModStep  la curva salta de a 'paso' frames (animacion "a dos", stop motion), desde 'offset'
//   KfModSmooth    la suaviza con un filtro gaussiano de 'ancho' frames (sigma relativa al ancho)
// Cada uno tiene: prendido, influencia (opcional) y un rango de frames donde actua (opcional, con rampas).
// Los TIEMPOS (Ciclos, Escalon) se aplican del ultimo al primero y los VALORES (Ruido, Suave, el offset de los
// ciclos) del primero al ultimo, como en Blender. POD + floats: se guarda como una lista fija de numeros.
// ============================================================================
enum { KfModCycles = 0, KfModNoise = 1, KfModStep = 2, KfModSmooth = 3, KfModTypes = 4 };
enum { KfCyclesNo = 0, KfCyclesRepeats = 1, KfCyclesOffset = 2, KfCyclesMirror = 3 };
enum { KfNoiseReplaces = 0, KfNoiseSum = 1, KfNoiseSubtract = 2, KfNoiseMultiplies = 3 };
struct KfModifier {
    int   type;
    bool  active;
    bool  useInfluence; float influence;
    bool  useRange; float startRange, endRange, mixIn, mixOut;
    // Ciclos
    int   prevMode, afterMode; float prevCount, afterCount;
    // Ruido
    int   mix; float scale, force, offset, phase, depth; bool noiseOld; float lacunaridad, roughness;
    // Escalon
    float step, stepOffset; bool useStart, useEnd; float start, end;
    // Suave
    float sigma, width;
    KfModifier(int t = KfModCycles);
    enum { NFloats = 32 };              // los numeros que se guardan (el orden es el formato: solo se agrega al final)
    void AFloats(float* f) const;
    void FromFloats(const float* f, int n);
    bool operator==(const KfModifier& o) const;
    bool operator!=(const KfModifier& o) const { return !(*this == o); }
};
const char* KfModName(int type);   // "Cycles" / "Noise" / "Stepped" / "Smooth" (en ingles: T() al mostrarlo)

class AnimProperty {
public:
    int Property;   // AnimPosition / AnimRotation / AnimScale
    int component;  // AnimX / AnimY / AnimZ
    W3dKeys keyframes;   // por fuera, un std::vector<keyFrame>; por dentro puede estar compacta (W3dKeys)
    int cyclePrev, cycleAfter;   // KfCycleNo / KfCycleRepeats / KfCycleOffset (el modificador Repetir)
    std::vector<KfModifier> mods; // la pila de modificadores de la curva (vacia = la curva tal cual)

    AnimProperty() : Property(AnimPosition), component(AnimX), cyclePrev(KfCycleNo), cycleAfter(KfCycleNo) {}
    void SortKeyFrames();
    // evalua la curva en 'frame' (devuelve def si no tiene keyframes). Clampea fuera del rango.
    float Eval(int frame, float def) const;
    // igual que Eval pero en frame CONTINUO. La animacion solo pisa frames enteros, asi que Eval(int) es este
    // mismo con t entero; el editor lo usa para dibujar.
    float EvalF(float frame, float def) const;
    // EvalF sin el modificador Repetir (fuera del rango de los keyframes: el valor de la punta)
    float EvalRange(float frame, float def) const;
    // la curva con el Repetir pero SIN la pila de modificadores
    float EvalNoMods(float frame, float def) const;
    // la curva con los primeros 'hasta' modificadores de la pila (el Suave mira la curva de antes de el)
    float EvalMods(float frame, float def, int until) const;
    // el REPETIR viejo (cyclePrev / cycleAfter, de los archivos de antes) pasa a ser un modificador CICLOS al
    // principio de la pila: es la misma cosa, y el editor solo muestra la pila. Lo llaman los lectores.
    void RepeatToModifier();
    // Handle EFECTIVO del keyframe i, como offset (dF, dV) desde el. 'salida' = el de la derecha. Segun handleType
    // devuelve el guardado (HFree/HAligned) o lo calcula desde los vecinos (HVector/HAuto/HAutoClamped). Es el
    // UNICO lugar donde se decide donde cae un handle: lo usan la evaluacion y el editor, asi que lo que ves es
    // lo que corre.
    void EffectiveHandle(size_t i, bool output, float& dF, float& dV) const;
    // valor del tramo BEZIER [i-1, i] en 'frame' (despeja t de la bezier cubica: los handles curvan tambien el
    // eje del tiempo, asi que t NO es la fraccion del tramo)
    float EvalBezier(size_t i, float frame) const;
};

class Vector3; // math/Vector3.h (lo incluye el .cpp)
// evalua las 3 curvas (X/Y/Z) de una propiedad en 'frame'. Cada componente se evalua POR SEPARADO: si Y no
// tiene keyframes propios, ese componente queda en su 'def'.
Vector3 EvalPropVec(const std::vector<AnimProperty>& props, int property, int frame, const Vector3& def);
// igual en frame CONTINUO (EvalF): el juego interpola entre dos frames del clip (sin tirones si el render no va
// a multiplo de los fps de la animacion)
Vector3 EvalPropVecF(const std::vector<AnimProperty>& props, int property, float frame, const Vector3& def);
// devuelve (creando si falta) la curva (property, component) de una lista de propiedades
AnimProperty& ListProperty(std::vector<AnimProperty>& props, int property, int component);
// pone/actualiza un keyframe en 'frame' con 'value', manteniendo la lista ordenada por frame. Uno NUEVO sale bezier
// con handles 'typeH': HAuto para lo que trae datos (importar, hornear: una clave por frame, sin frenar en cada una);
// HPlane (ease in/out) para lo que inserta el USUARIO (la 'i', el auto key)
void SetKeyCurve(AnimProperty& ap, int frame, float value, int typeH = HAuto);
// EASE de un tramo BEZIER comun (a -> b, valores va -> vb), con sus handles (sin curvas calculadas):
//   Ease In  = sale despacio de a (handle de salida de a PLANO) y llega derecho a b (handle de entrada apuntando a a)
//   Ease Out = sale derecho y llega despacio     Ease In and Out = los dos planos     Automatic = handles automaticos
// Los handles de a y b tienen que estar CONCRETOS (Free/Aligned) antes de llamar, salvo con Automatic.
template <class K> void KfEaseBezierSegment(K& a, K& b, float va, float vb, int ease) {
    const float span = (float)(b.frame - a.frame);
    if (span <= 0.0f) return;
    if (ease == KfEaseAuto) { a.handleType = HAuto; b.handleType = HAuto; return; }
    const bool planeA = (ease == KfEaseIn || ease == KfEaseInOut);
    const bool planeB = (ease == KfEaseOut || ease == KfEaseInOut);
    a.outDF = span / 3.0f;  a.outDV = planeA ? 0.0f : (vb - va) / 3.0f;
    b.inDF = -span / 3.0f;  b.inDV  = planeB ? 0.0f : (va - vb) / 3.0f;
    // alineado solo si los dos lados quedaron en la misma recta (planos los dos); si no, cada lado por libre
    a.handleType = (a.inDV == 0.0f && a.outDV == 0.0f) ? HAligned : HFree;
    b.handleType = (b.inDV == 0.0f && b.outDV == 0.0f) ? HAligned : HFree;
}
// el tramo que SALE del keyframe i de una curva (si es bezier y hay siguiente). Congela antes los handles
// calculados de los dos keyframes (lo que se veia) para no perder el otro lado.
void KfEaseBezier(AnimProperty& ap, size_t i, int ease);
// de una curva de EASING (Sine...Circ, Back) a BEZIER comun CONSERVANDO LA FORMA: los handles del tramo que sale
// del keyframe i quedan en los de la bezier que mas se parece a esa curva (Free). Bounce y Elastic no entran en una
// sola bezier: ahi (y si el tramo no era de easing) devuelve false y no toca nada. 'oldInterp' = la de antes.
bool KfBezierFromEasing(AnimProperty& ap, size_t i, int oldInterp);
// Borra el keyframe de 'frame' tratando de MANTENER LA FORMA de la curva: ajusta los handles de los vecinos por
// minimos cuadrados contra la curva original (conserva las direcciones, ajusta los largos). Es una simplificacion:
// un tramo no siempre puede reproducir dos, pero los extremos y las tangentes quedan iguales.
// Si el tramo no es bezier (o el keyframe es la punta), es un borrado comun: no hay forma que mantener.
void DeleteKeyframeKeepingShape(AnimProperty& ap, int frame);

// ---- LAS PISTAS DE MATERIAL ("se tendria que poder animar los materiales") ----
// Un material NO es un objeto: vive en la lista global Materials, lo comparten muchas mallas y lo nombran por su nombre.
// Sus curvas (canales AnimMat*) van en una pista por material dentro de cada animacion de ESCENA
// (SceneAnimation::materials). La pista nombra al material por PUNTERO (renombrarlo no la pierde; el .w3d la guarda
// por nombre). Las curvas son las de siempre: interpolaciones, easing, handles y modificadores andan igual.
// VALOR BASE Y VALOR ANIMADO: como un objeto o una luz, el campo del material es lo que se ve. El cabezal le aplica las
// curvas (canales sin curva no se tocan); editarlo sin keyframe es temporal (el proximo cambio de frame lo vuelve a la
// curva). Lo que se GUARDA es su valor en el frame de inicio de la animacion que lo poso (W3dMaterialsRestBegin).
class Material;   // objects/Materials.h
class AnimationMaterial {
public:
    Material* mat;
    std::vector<AnimProperty> Properties;
    AnimationMaterial() : mat(0) {}
};
bool  W3dIsMatChannel(int prop);                      // AnimMatColor .. AnimMatUVRotation
// el valor del canal (prop, comp) en el material (0 si no existe: una capa que no esta) / ponerlo
float W3dMatChannelGet(const Material* m, int prop, int comp);
void  W3dMatChannelSet(Material* m, int prop, int comp, float v);
// las curvas que keyea JUNTAS el rombo de esa fila (un color: R/G/B; lo demas, la suya). Devuelve cuantas (en 'out')
int   W3dMatChannelComps(int prop, int comp, int* out);
// el nombre del canal (en ingles: T() al mostrarlo) y el sufijo de la componente ("R", "U"...; "" = ninguno)
const char* W3dMatChannelBase(int prop);
const char* W3dMatChannelSuffix(int prop, int comp);
// aplica las curvas de UN material en 'frame' (continuo). true = escribio algun canal (y lo marca posado)
bool  W3dApplyMaterialCurves(Material* m, const std::vector<AnimProperty>& props, float frame);
// EL REPOSO (guardar sin hornear el cabezal, como RestAnimObjects con los objetos): los materiales que el cabezal poso
// vuelven al valor de sus curvas en el frame de INICIO de su animacion de escena (la activa manda); End los devuelve a
// lo que habia en la pantalla. 'solo' = solo ese material (NULL = todos)
struct W3dMatRest {
    Material* m;
    float dif[4], emi[3], spe[3], emiK, rug, met, trans, uvOff[2], uvScl[2], uvRot;
    std::vector<float> layers;
    void Read(Material* mt);
    void Write() const;
};
void W3dMaterialsRestBegin(std::vector<W3dMatRest>& saved, const Material* only);
void W3dMaterialsRestEnd(std::vector<W3dMatRest>& saved);

// Animación de objeto
class AnimationObject { 
public:
    Object* obj; 
    int FirstKeyFrame;
    int LastKeyFrame;
    std::vector<AnimProperty> Properties;

    void UpdateFirstLastFrame();
};

// Variables globales de objetos animados
extern std::vector<AnimationObject> AnimationObjects;

// === Animacion de ESCENA: contenedor con nombre que agrupa las curvas de transform de objetos (mallas, luces,
// camaras) de la escena. Por defecto hay una llamada "Scene"; pueden crearse mas. Las curvas de la escena ACTIVA
// viven en el global AnimationObjects (arriba); al cambiar de escena se hace swap con la copia guardada aca. ===
class SceneAnimation {
public:
    std::string name;
    int startFrame;                       // rango + fps PROPIOS de esta escena (se cargan a los globales al activarla)
    int endFrame;
    int fps;
    std::vector<AnimationObject> objects; // curvas guardadas (vacio mientras esta es la escena activa)
    // PISTA "CAMARA ACTIVA" (los cortes de una cinematica): 0 o 1 curva (Property AnimCamActive, componente X) cuyos
    // keyframes valen un INDICE en camNames. Va en un vector (y no suelta) para que el dope sheet y el undo la
    // traten como cualquier curva (mover / borrar / escalar / Ctrl+Z). A DIFERENCIA de 'objetos' NO se hace swap
    // con un global al activar la escena: vive siempre aca. Las camaras van por NOMBRE (sobreviven al guardado).
    std::vector<AnimProperty> camTrack;
    std::vector<std::string>  camNames;
    // LAS PISTAS DE MATERIAL de esta escena (ver AnimationMaterial arriba). Como camTrack, NO se swapean con un global
    // al activarla: viven siempre aca (el editor aplica las de la activa; el juego, las de la que suena)
    std::vector<AnimationMaterial> materials;
    // LAS TIRAS DE ESQUELETO de la escena (una cinematica o una pelicula, no un juego): el armature 'armature' toca
    // su clip 'clip' desde 'frame' (el frame 'frame' de la escena es el primero del clip) mientras se reproduce el
    // timeline, y se acomoda solo al mover la linea de tiempo; fuera del clip queda en su primera / ultima pose.
    // (El AUDIO de una escena es un objeto Sonido: main/objects/Sound.h)
    struct SkeletonTrack { std::string armature, clip; int frame; };
    std::vector<SkeletonTrack> skeletons;
    // VISTA DE EDICION de un CLIP DE JERARQUIA (animation/W3dAnimSet.h: los clips que un objeto raiz usa
    // para animarse a si mismo y a sus hijos por ruta de nombres; viven COMPARTIDOS en una biblioteca). El
    // editor los edita como una animacion de escena mas -keyframes, dope sheet, curvas, Auto Key, undo- sobre
    // SU raiz: la vista es una COPIA de las curvas del clip atada a los objetos de esa raiz, que se escribe de
    // vuelta en el clip (W3dClipViewWrite) al dejar de editarla, al guardar, al dar Play... No se guarda
    // (el clip vive en su biblioteca) y animEscena() / escenaCapa() / la cinematica de una escena no la ven.
    //   - su nombre es el del clip (unico entre los de su biblioteca);
    //   - la pista de la RAIZ es relativa (ver W3dAnimSet.h): al atarla se le suma lo que hace falta para que
    //     el primer frame caiga donde esta la raiz (raizOff*), y al escribirla se le resta.
    bool isClip;             // true = vista de un clip de jerarquia
    Object* owner;           // su raiz (NULL con isClip = la raiz se borro: la vista queda huerfana)
    W3dClipHier* clipHier;   // el clip que edita (se valida con su serie: W3dClipViewLive)
    unsigned seriesHier;     // la serie del clip al atarla
    unsigned versionView;    // la version del clip que tiene copiada (distinta = la vista quedo vieja)
    Vector3 rootOffPos, rootOffRot, rootOffEsc;   // lo que se sumo a las curvas de la raiz al atarla
    SceneAnimation(const std::string& n) : name(n), startFrame(1), endFrame(250), fps(30), isClip(false), owner(0),
                                           clipHier(0), seriesHier(0), versionView(0),
                                           rootOffPos(0, 0, 0), rootOffRot(0, 0, 0), rootOffEsc(0, 0, 0) {}
};

// ---- PISTA "CAMARA ACTIVA" de una animacion de escena ----
// la curva de la escena 'escena' (NULL si no tiene y !crear)
AnimProperty* W3dCamTrack(int scene, bool create);
// indice del nombre en camNames (lo agrega si falta)
int  W3dCamNameIdx(int scene, const std::string& name);
// pone (o reemplaza) el corte en 'frame' -> esa camara pasa a ser la activa desde ese frame (interpolacion constante)
void W3dCamKey(int scene, int frame, const std::string& name);
// la camara que manda en 'frame' (NULL = la escena no tiene pista). Antes del primer corte manda el primero.
const char* W3dCamInFrame(int scene, int frame);
// aplica la pista en 'frame': llama g_activeCameraHook con el nombre (si hay pista). Lo llaman el editor (al mover
// el playhead de una animacion de escena) y el juego (W3dAnimSceneTick, animEscena de lua).
void W3dActiveCamApply(int scene, int frame);
// ---- LAS PISTAS DE MATERIAL de la animacion de escena 'scene' (ver AnimationMaterial) ----
// la pista de 'm' (NULL si no tiene y !create). OJO: crear una puede mover las demas (es un vector)
AnimationMaterial* W3dMatTrack(int scene, Material* m, bool create);
// aplica todas sus pistas en 'frame' (continuo). Sin pistas no cuesta nada. Lo llaman el editor (ApplyAnimationObjects)
// y el juego (W3dAnimSceneTick: la animacion de escena que suena)
void  W3dApplyMaterialTracks(int scene, float frame);
// los registra main/: el Core no conoce la clase Camera ni CameraActive
extern void (*g_activeCameraHook)(const char* name);
extern void (*g_animFovHook)(Object* camera, float fov);   // curva AnimFov en el JUEGO (cinematicas)
// aplica las curvas INDEXADAS de una lista: AnimShape a la malla (los pesos de sus shape keys) y AnimStep a la rutina
// (g_animStepHook: el Core no conoce la clase Rutina). Lo usan el juego y el editor.
void W3dApplyShapes(Object* o, std::vector<AnimProperty>& props, int frame);
extern void (*g_animStepHook)(Object* routine, int component, float value);
extern void (*g_animSoundHook)(Object* sound, int component, float value);
extern void (*g_animBackgroundHook)(int property, int component, float value);   // el FONDO del render (AnimBackground*)
extern std::vector<SceneAnimation*> SceneAnimations; // lista global de animaciones de escena
extern int ActiveSceneAnim;                          // indice de la escena activa (sus curvas estan en AnimationObjects)

// Seleccion de animacion ACTIVA a nivel APP (la comparten la tarjeta Animation y el Timeline; no depende del objeto
// seleccionado, asi clickear un armature NO cambia la animacion activa):
//   ActiveAnimKind 0 = una animacion de ESCENA (SceneAnimations[ActiveSceneAnim])
//   ActiveAnimKind 1 = un CLIP de un armature (ActiveAnimArm y su activeAnim)
class Armature; // tipo completo en objects/Armature.h
//   ActiveAnimKind 2 = modo JUEGO (timeline infinito; lo pone main/ con "Es un juego")
//   ActiveAnimKind 3 = una ANIMACION DEL OBJETO de una malla (ActiveAnimMesh): el timeline
//                      muestra/edita sus frames (el mesh sigue al playhead). Es un CONTENEDOR:
//                      capas de vertices / normales / UV + curvas de transform propias.
//   ActiveAnimKind 4 = un CLIP DEL ARMATURE 2D de una malla. Que clip es sale de DOS indices de la
//                      malla (ActiveAnimMesh): el armature ACTIVO (Mesh::armature2dActive) y, dentro
//                      de el, su clip activo (Armature2D::activeAnim) -- es lo que resuelve
//                      Arm2DActiveClip(). Curvas por hueso 2D (posU/posV/rot/escalaX/escalaY) que el
//                      playhead evalua con Armature2DEvaluate. A DIFERENCIA del kind 3, NO congela
//                      UpdateAnimations de la escena: un rig 2D animado convive con el resto del mundo.
//                      OJO: cambiar de armature activo CAMBIA el clip del timeline -> hay que recargar
//                      el rango (Arm2DSetActive / Arm2DSyncRange, en Armature2DAnimation.h).
extern int ActiveAnimKind;
extern Armature* ActiveAnimArm;
class Mesh;
extern Mesh* ActiveAnimMesh;

void InitSceneAnimations();     // crea "Scene" si la lista esta vacia (idempotente)
const char* ActiveSceneName();  // nombre de la escena activa
void SetActiveScene(int idx);   // hace activa la escena idx (swap de curvas con AnimationObjects)
int  NewScene();                // crea una escena nueva, la deja activa, devuelve su indice
// RENAME de la animacion de escena activa (nombre UNICO); devuelve el nombre que quedo
std::string RenameActiveScene(const std::string& name);
// nombre libre en el espacio GLOBAL de animaciones de escena ('excepto' = indice que conserva el suyo)
std::string SceneAnimFreeName(const std::string& base, int except);
void DeleteActiveScene();         // borra la escena activa (siempre queda al menos "Scene")
// el BORRADO DESHACIBLE del editor (main/undo: UndoDeleteActiveScene) en dos mitades:
//  - SACA la animacion ACTIVA de la lista SIN liberarla: sus curvas pasan a su ->objects y queda activa la de
//    al lado (la misma regla que DeleteActiveScene). 'pos' = donde estaba. NULL = no se saca: es la UNICA
//    animacion de escena (esa no se borra, se vacia). La VISTA de un clip de jerarquia sale igual (su CLIP lo
//    saca de su biblioteca quien la borra, ANTES: la vista ya no se escribe).
SceneAnimation* W3dActiveSceneRemove(int* pos);
//  - la DEVUELVE en 'pos' (acotada) y la deja ACTIVA (con sus curvas)
void W3dSceneReturn(SceneAnimation* e, int pos);
// si lo que se edita es la VISTA HUERFANA de un clip de jerarquia (su clip salio de la biblioteca, la raiz cambio
// de biblioteca...), pasa a la animacion de escena mas cercana: una vista muerta no se edita. true = cambio.
bool W3dClipsActiveViewCheck();

// ---- REPRODUCTOR de animaciones de ESCENA para el MODO JUEGO (kind 2) ----
// El editor evalua las curvas de escena SOLO con kind 0 (ApplyAnimationObjects,
// main/); en un juego las dispara el script Lua: animEscena("walk_player", loop).
// UN clip activo por vez (alcanza para el personaje del demo; instancias en
// paralelo para NPCs es el paso siguiente). El tick lo llama la simulacion
// (TickReal de SimJuego) una vez por frame: avanza el reloj y APLICA las curvas
// pos/rot/escala/visible sobre los objetos referenciados.
int   W3dAnimSceneIdx(const char* name);     // indice en SceneAnimations (-1 si no esta)
float W3dAnimSceneDur(int idx);              // duracion en segundos (0 si invalido)
bool  W3dAnimScenePlay(int idx, bool loop);  // arranca desde su startFrame
void  W3dAnimSceneTick(float dt);            // avanzar + aplicar (1 vez por tick de sim)
void  W3dAnimSceneReset();                   // al STOP del juego: nada sonando
int   W3dAnimSceneFrame();                   // frame (entero) de la que suena; -1 = ninguna
// fija el frame de la que suena y APAGA su reloj propio: el script manda (animEscenaFrame de lua). Para sincronizar
// una cinematica con UN solo reloj: camaras, caras y clips de los personajes en el MISMO frame (sin micro saltos).
void  W3dAnimSceneSet(float frame);
int   W3dAnimScenePlaying();                 // indice de la que suena; -1 = ninguna
// el estado del reproductor, para dejarlo en pausa y retomarlo tal cual (una CINEMATICA que se juega en otra
// raiz: al volver, la animacion de escena que sonaba en el juego sigue donde iba, ver main/W3dRoots.h)
struct W3dAnimSceneState {
    int idx; float frame; bool loop, manual;
    W3dAnimSceneState() : idx(-1), frame(0.0f), loop(true), manual(false) {}
};
void  W3dAnimSceneSaveState(W3dAnimSceneState* e);
void  W3dAnimSceneRestoreState(const W3dAnimSceneState& e);
// true si la que suena es de UNA pasada y ya llego a su ultimo frame (una cinematica que termino)
bool  W3dAnimSceneEnded();

// un cabezal en LOOP (en [0, periodo), periodo > 0) que se paso: vuelve adentro de UNA vez. El "while (f >=
// periodo) f -= periodo" de siempre colgaba el tick con una velocidad o un frame ENORMES desde lua
// (animObjetoVelocidad, objetoCapaFrame, animClipVelocidad...): en float, restarle una vuelta a un numero muy
// grande no lo cambia. Las pocas vueltas de siempre dan lo mismo que antes; un negativo queda igual (como
// antes); infinito o NaN vuelve a 0.
float W3dCycleWrap(float f, float period);

// ---- VISTAS DE EDICION DE LOS CLIPS DE JERARQUIA (SceneAnimation::isClip, ver arriba) ----
bool  W3dAnimIsClip(int idx);                 // SceneAnimations[idx] es la vista de un clip (viva o huerfana)
// la vista de 'c' (un clip de la biblioteca de 'raiz') en la lista de animaciones de la raiz ACTIVA: la que
// ya habia o una nueva (atada ya: sus curvas son las del clip). NO la activa. -1 = no se pudo.
int   W3dClipView(Object* root, W3dClipHier* c);
// la vista sigue editando un clip que existe (su raiz vive colgada de un arbol -no detachada por un borrado con
// undo-, sigue con esa biblioteca y el clip sigue en ella)
bool  W3dClipViewLive(int idx);
// clip -> vista (la re-ata: sus curvas vuelven a ser las del clip) / vista -> clip (true = el clip cambio)
void  W3dClipViewRead(int idx);
bool  W3dClipViewWrite(int idx);
// escribe en su clip la vista que se esta editando (si lo que esta activo es una). Lo llaman quienes van a
// LEER los clips: el guardado, el Play, el cambio de raiz, el harness.
void  W3dClipsViewsSync();
// saca de la lista las vistas HUERFANAS (su clip ya no existe o su raiz se borro) que no son la activa
void  W3dClipsViewsPurge();
// el clip 'c' cambio POR FUERA de su vista (renombrado desde el outliner, un undo): si lo que se edita es una
// vista de 'c', se re-ata (toma su nombre y sus curvas). Las demas vistas lo notan solas por su version.
void  W3dClipActiveViewReread(const W3dClipHier* c);
// la biblioteca de 'duenio' se cambio por OTRA con los MISMOS clips en el mismo orden (el guardado junta dos
// bibliotecas iguales en una: main/io/SaveAnimSets.cpp): sus vistas en 'lista' pasan a editar el clip
// equivalente de la nueva, sin re-atarse (el contenido es el mismo: sus curvas siguen valiendo). Sin esto la
// vista que se editaba quedaba HUERFANA y lo que se le keyeaba no llegaba a ningun clip.
void  W3dClipsViewsSwap(std::vector<SceneAnimation*>& list, const Object* owner,
                              const std::vector<W3dClipHier*>& old, const std::vector<W3dClipHier*>& newValue);
// la etiqueta de una animacion de escena para la UI ("Puerta: abrir" si es la vista de un clip; su nombre si no)
std::string W3dAnimSceneLabel(int idx);
// EDITOR: se le pueden insertar keyframes a 'o' en la animacion activa? (en la vista de un clip de jerarquia,
// solo a su raiz y a sus descendientes: el clip no puede nombrar lo que no es de su jerarquia)
bool  W3dAnimObjectAllowed(const Object* o);

// ---- REPRODUCTOR DE CLIPS DE JERARQUIA (el juego: animObjeto de lua) ----
// Un cabezal POR RAIZ, independiente del de animEscena(), de los clips de los armatures y de las demas raices:
// varias animaciones a la vez (la puerta que se abre, el ascensor que sube, el auto hecho de objetos...). El
// clip se busca en la biblioteca de la raiz y, si no esta, por nombre calificado "Biblioteca/clip" (o suelto,
// si es el unico con ese nombre): cualquier jerarquia con los mismos nombres lo puede reproducir. Lo avanza
// W3dAnimObjectsTick una vez por tick del juego (Play del editor y runtime, junto a W3dAnimSceneTick).
// arranca el clip en 'o' desde su inicio. Devuelve su duracion en segundos (< 0 = no hay un clip con ese nombre)
float W3dAnimObjectPlay(Object* o, const std::string& clip, bool loop);
void  W3dAnimObjectsTick(float dt);
float W3dAnimObjectFrame(const Object* o);        // frame del clip que suena en 'o' (-1 = ninguno)
void  W3dAnimObjectSetFrame(Object* o, float f);  // mueve su cabezal (el clip sigue sonando desde ahi)
bool  W3dAnimObjectEnded(const Object* o);        // true: nada suena en 'o' o su clip de una pasada termino
std::string W3dAnimObjectClip(const Object* o);   // el nombre del clip que suena ("" = ninguno)
void  W3dAnimObjectStop(Object* o);               // lo deja quieto donde esta
// velocidad del clip que suena (1 = sus fps; 2 = el doble; negativa = hacia atras). false = no suena nada
bool  W3dAnimObjectVelocity(Object* o, float v);
// el RETARGET de la raiz 'o' (W3D_RETARGET_*, SkeletalAnimation.h; -1 = el de cada clip). Lo recuerda para
// los clips que arranque despues y lo aplica al que suena.
void  W3dAnimObjectRetarget(Object* o, int mode);
void  W3dAnimObjectsReset();                        // al STOP del juego: nada suena
int   W3dAnimObjectsPlaying();                      // cuantas raices tienen un clip sonando

// las curvas de UN objeto en el JUEGO, en el frame continuo 'fr': transformacion, visible/render, formas,
// fov de camara y colores/atenuacion/spot de una luz. Lo usan animEscena, los clips de jerarquia y las capas
// de escena (el editor usa W3dApplyCurvesInFrame, que ademas marca lo posado para el guardado)
void  W3dApplyCurvesGame(Object* o, std::vector<AnimProperty>& props, float fr);

// las curvas de objeto CAMBIARON por fuera del cabezal (otra raiz, otro clip, el estado base de un juego):
// el editor tiene que volver a posar aunque el frame sea el mismo (ApplyAnimationObjects compara esto)
extern unsigned int g_animCurvesGen;
inline void W3dAnimCurvesInvalidate() { g_animCurvesGen++; }

// Start/End/FPS PROPIOS de la animacion activa (escena o clip de armature). La comparten la tarjeta y el timeline.
void AnimLoadActiveRange();      // StartFrame/EndFrame/AnimFPS <- animacion activa (al seleccionarla)
void AnimSetStart(int v);        // animacion activa.start = v; StartFrame = v
void AnimSetEnd(int v);          // animacion activa.end   = v; EndFrame   = v
void AnimSetFps(int v);          // animacion activa.fps   = v; AnimFPS    = v

#endif