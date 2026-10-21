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
// User::TickCount), se registra con W3dSetReloj y el motor lo usa.
typedef unsigned int (*W3dRelojFn)();
void W3dSetReloj(W3dRelojFn fn);
unsigned int w3dGetTicks();
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>   // N95: OpenGL ES 1.1 (GLshort/GLbyte/GLfloat)
#else
    #include <GL/gl.h>     // PC: OpenGL de escritorio
#endif

#include "objects/Objects.h"

class W3dClipJer;   // animation/W3dAnimSet.h: un clip de jerarquia (la vista de edicion lo nombra)

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
extern bool AnimEsJuego;
// con esto prendido, PLAY desde un frame anterior NO borra lo grabado hacia adelante:
// lo REPRODUCE (util para revisar una partida sin perderla)
extern bool AnimConservarEstados;
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
//   AnimLightMisc: LUZ, GL light index=AnimX, direccional(0/1)=AnimY (interpolacion CONSTANTE)
//   AnimCamActiva: la pista "CAMARA ACTIVA" de una animacion de ESCENA (SceneAnimation::camPista). El valor de
//                  cada keyframe es un INDICE en SceneAnimation::camNombres; interpolacion CONSTANTE (un corte).
enum { AnimPosition, AnimRotation, AnimScale, AnimVisible, AnimRender, AnimVertex, AnimFov, AnimColor,
       AnimClip, AnimAmbient, AnimSpecular, AnimAtten, AnimSpot, AnimLightMisc, AnimCamActiva, AnimShape, AnimPaso };
//   AnimPaso: un PASO de una RUTINA (main/objects/Rutina.h); el COMPONENTE es id del paso * 8 + campo (0..4 = sus
//             numeros, 5 = on/off): mover el paso no lo cambia (el id es suyo). Lo aplica el gancho g_animPasoHook.
enum { AnimPasoCampos = 8, AnimPasoOn = 5 };
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
enum { KfConstant = 0, KfLinear = 1, KfBezier = 2 };

// CANALES de un "Insert Keyframe" (mascara de bits). La comparten los TRES caminos que keyframean un
// transform: InsertarKeyframeObjeto (objetos), InsertarKeyframeEsqueleto (pose 3D) e InsertarKeyframeArm2D
// (pose 2D). El menu desplegable de Insert Keyframe elige la mascara: Todos / Solo localizacion / Solo
// rotacion / Solo escala. 0 se trata como KfCanalTodos (el default historico: se keyeaba todo siempre).
// OJO: los keyframes de VERTICES (posiciones/normales/UV de una vertex anim) NO usan esta mascara: una
// pose de vertices es una sola cosa, no tiene canales -> ahi la 'i' inserta directo, sin menu.
enum { KfCanalLoc = 1, KfCanalRot = 2, KfCanalScl = 4, KfCanalTodos = 7 };

// Tipo del par de handles de un keyframe bezier. Los tres ultimos se CALCULAN solos desde los vecinos (lo que
// haya guardado en los offsets se ignora); los dos primeros los pone el usuario.
//   HFree        los dos lados van por libre, cada uno donde lo dejes
//   HAligned     los dos lados quedan SIEMPRE en la misma recta (mover uno gira el otro; cada uno conserva su largo)
//   HVector      cada lado apunta al keyframe vecino -> el tramo sale recto
//   HAuto        pendiente suave que pasa por los vecinos (Catmull-Rom)
//   HAutoClamped como HAuto, pero se aplana en los picos: la curva NUNCA se pasa del valor de los keyframes
enum { HFree = 0, HAligned = 1, HVector = 2, HAuto = 3, HAutoClamped = 4 };

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
    // HandleEfectivo desde los vecinos.
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
//    - SIN expandir: size/empty, Frame/Valor/Interp/TipoH/Clave(i), PrimerFrame/UltimoFrame, la evaluacion
//      (AnimProperty::Eval/EvalF/HandleEfectivo: el juego y la reproduccion), el escritor del .w3da, copiar y
//      comparar.
//    - lo demas de la API de vector (operator[], at, begin/end, push_back, insert, erase...) la EXPANDE primero,
//      tambien en un acceso const: desde ahi es una curva comun (el editor, el dope sheet y el undo, como siempre).
//  La evaluacion es UNA sola funcion para las dos (lee con Frame/Valor/...): dan los mismos bits (lo compara en
//  todos los frames, enteros y fraccionarios, tools/pruebas/prueba_claves_compactas.w3s).
// ============================================================================
// el bloque de una curva compacta: esta cabecera y, en la MISMA reserva, sus n valores (y en una IRREGULAR,
// despues, sus n frames)
struct W3dClavesCompactas {
    int primero;     // frame de la clave 0
    int paso;        // frame de la clave i = primero + paso * i (paso > 0; el ultimo entra en un int).
                     // 0 = IRREGULAR: los frames de cada clave van despues de los valores (estrictamente crecientes)
    int interp;      // la interpolacion de TODAS
    int tipoH;       // el tipo de handle de todas (los guardados son cero: HFree/HAligned quedan en cero)
    unsigned n;      // cantidad de claves (>= 2)
    float*       Valores()       { return reinterpret_cast<float*>(this + 1); }
    const float* Valores() const { return reinterpret_cast<const float*>(this + 1); }
    bool Irregular() const { return paso == 0; }
    int*       Frames()       { return reinterpret_cast<int*>(Valores() + n); }        // (solo las irregulares)
    const int* Frames() const { return reinterpret_cast<const int*>(Valores() + n); }
    // frame de la clave i, en aritmetica sin signo: el mismo resultado que sumar el paso clave a clave (el lector)
    int Frame(unsigned i) const { return paso ? (int)((unsigned)primero + (unsigned)paso * i) : Frames()[i]; }
};

class W3dClaves {
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

    W3dClaves() : c_(0) {}
    W3dClaves(const W3dClaves& o);
    ~W3dClaves();
    W3dClaves& operator=(const W3dClaves& o);
    W3dClaves& operator=(const Vec& v);
#if defined(__cplusplus) && __cplusplus >= 201103L
    // (C++11 o mas: MOVER no copia las claves -un vector<AnimProperty> que crece las mueve, como cuando era un
    //  std::vector-; en C++03 se copian, como siempre)
    W3dClaves(W3dClaves&& o) noexcept : c_(0) { swap(o); }
    W3dClaves& operator=(W3dClaves&& o) noexcept { swap(o); return *this; }
#endif

    // ---- SIN EXPANDIR ----
    size_type size() const { return c_ ? (size_type)c_->n : v_.size(); }
    bool empty() const { return c_ ? false : v_.empty(); }        // (una compacta tiene 2 claves o mas)
    bool Compacta() const { return c_ != 0; }
    const W3dClavesCompactas* DatosCompactos() const { return c_; }   // NULL = curva comun
    int   Frame(size_type i) const { return c_ ? c_->Frame((unsigned)i) : v_[i].frame; }
    float Valor(size_type i) const { return c_ ? c_->Valores()[i] : v_[i].value; }
    int   Interp(size_type i) const { return c_ ? c_->interp : v_[i].Interpolation; }
    int   TipoH(size_type i) const { return c_ ? c_->tipoH : v_[i].handleType; }
    // el handle GUARDADO de la clave i (offset en frames / en valor), el de salida o el de entrada
    float HandleDF(size_type i, bool salida) const { return c_ ? 0.0f : (salida ? v_[i].outDF : v_[i].inDF); }
    float HandleDV(size_type i, bool salida) const { return c_ ? 0.0f : (salida ? v_[i].outDV : v_[i].inDV); }
    keyFrame Clave(size_type i) const;            // la clave i entera, POR VALOR (handles en cero si es compacta)
    int PrimerFrame() const { return Frame(0); }  // (con claves: !empty())
    int UltimoFrame() const { return Frame(size() - 1); }
    long BytesMemoria() const;                    // lo que ocupan sus claves en el heap (vector o bloque compacto)
    // igual clave a clave: frame, interpolacion y tipo de handle; valor y handles BIT a BIT (un -0.0 no es 0.0)
    bool MismosBits(const W3dClaves& o) const;
    // ---- el lector del .w3da ----
    // n claves con frames primero + paso * i se pueden guardar compactas? (2 o mas, paso > 0, el ultimo en un int)
    static bool CompactaPosible(size_type n, int primero, int paso);
    // la deja COMPACTA (descarta lo que tuviera) y devuelve donde van sus n valores. Antes: CompactaPosible.
    float* CompactaCrear(size_type n, int primero, int paso, int interp, int tipoH);
    // n claves con estos frames se pueden guardar compactas IRREGULARES? (2 o mas, estrictamente crecientes)
    static bool CompactaIrregularPosible(size_type n, const int* frames);
    // la deja COMPACTA IRREGULAR con esos frames (los copia) y devuelve donde van sus n valores. Antes:
    // CompactaIrregularPosible.
    float* CompactaCrearIrregular(size_type n, const int* frames, int interp, int tipoH);

    // ---- la API de std::vector<keyFrame>: EXPANDEN (una compacta pasa a ser una curva comun) ----
    reference       operator[](size_type i)       { Expandir(); return v_[i]; }
    const_reference operator[](size_type i) const { Expandir(); return v_[i]; }
    reference       at(size_type i)       { Expandir(); return v_.at(i); }
    const_reference at(size_type i) const { Expandir(); return v_.at(i); }
    reference       front()       { Expandir(); return v_.front(); }
    const_reference front() const { Expandir(); return v_.front(); }
    reference       back()       { Expandir(); return v_.back(); }
    const_reference back() const { Expandir(); return v_.back(); }
    iterator       begin()       { Expandir(); return v_.begin(); }
    const_iterator begin() const { Expandir(); return v_.begin(); }
    iterator       end()       { Expandir(); return v_.end(); }
    const_iterator end() const { Expandir(); return v_.end(); }
    reverse_iterator       rbegin()       { Expandir(); return v_.rbegin(); }
    const_reverse_iterator rbegin() const { Expandir(); return v_.rbegin(); }
    reverse_iterator       rend()       { Expandir(); return v_.rend(); }
    const_reverse_iterator rend() const { Expandir(); return v_.rend(); }
    size_type capacity() const { Expandir(); return v_.capacity(); }
    size_type max_size() const { return v_.max_size(); }
    void push_back(const keyFrame& k);
    void pop_back() { Expandir(); v_.pop_back(); }
    iterator insert(iterator pos, const keyFrame& k);
    void insert(iterator pos, size_type n, const keyFrame& k);
    template <class It> void insert(iterator pos, It a, It b) {
        Expandir(); const size_type antes = v_.capacity(); v_.insert(pos, a, b); ContarCapacidad(antes);
    }
    iterator erase(iterator pos) { Expandir(); return v_.erase(pos); }
    iterator erase(iterator a, iterator b) { Expandir(); return v_.erase(a, b); }
    void clear();                                 // (una compacta se suelta: queda una curva comun vacia)
    void resize(size_type n, const keyFrame& k = keyFrame());
    void reserve(size_type n);
    void assign(size_type n, const keyFrame& k);
    template <class It> void assign(It a, It b) {
        SoltarCompacta(); const size_type antes = v_.capacity(); v_.assign(a, b); ContarCapacidad(antes);
    }
    void swap(W3dClaves& o);
    void swap(Vec& v);
    // las claves como el vector de siempre (const: para leer). Expande.
    operator const Vec&() const { Expandir(); return v_; }

private:
    mutable Vec v_;                    // la curva comun (vacio mientras es compacta)
    mutable W3dClavesCompactas* c_;    // la compacta (NULL = comun)
    void Expandir() const { if (c_) ExpandirAhora(); }
    void ExpandirAhora() const;
    void SoltarCompacta();
    void ContarCapacidad(size_type antes);
};

// MEDICION (meminfo): las claves de TODAS las curvas vivas. 'bytes' cuenta los vectores (su capacidad) y los
// bloques compactos; 'expansiones', las curvas compactas que pasaron a comunes desde el arranque (jugar no
// expande ninguna: prueba_claves_compactas.w3s).
struct W3dClavesStats {
    long bytes;              // bytes de claves vivos (vectores + bloques compactos)
    long compactas;          // curvas compactas vivas
    long clavesCompactas;    // sus claves
    long bytesCompactas;     // sus bloques
    long expansiones;        // compacta -> comun, desde el arranque
    W3dClavesStats() : bytes(0), compactas(0), clavesCompactas(0), bytesCompactas(0), expansiones(0) {}
};
void W3dClavesEstadisticas(W3dClavesStats& st);

// Una CURVA de animacion = (Property, component). Ej: (AnimPosition, AnimX) = "X Location".
// Cada componente tiene SUS PROPIOS keyframes -> se puede mover/borrar/curvar X sin tocar Y ni Z.
class AnimProperty {
public:
    int Property;   // AnimPosition / AnimRotation / AnimScale
    int component;  // AnimX / AnimY / AnimZ
    W3dClaves keyframes;   // por fuera, un std::vector<keyFrame>; por dentro puede estar compacta (W3dClaves)

    AnimProperty() : Property(AnimPosition), component(AnimX) {}
    void SortKeyFrames();
    // evalua la curva en 'frame' (devuelve def si no tiene keyframes). Clampea fuera del rango.
    float Eval(int frame, float def) const;
    // igual que Eval pero en frame CONTINUO. La animacion solo pisa frames enteros, asi que Eval(int) es este
    // mismo con t entero; el editor lo usa para dibujar.
    float EvalF(float frame, float def) const;
    // Handle EFECTIVO del keyframe i, como offset (dF, dV) desde el. 'salida' = el de la derecha. Segun handleType
    // devuelve el guardado (HFree/HAligned) o lo calcula desde los vecinos (HVector/HAuto/HAutoClamped). Es el
    // UNICO lugar donde se decide donde cae un handle: lo usan la evaluacion y el editor, asi que lo que ves es
    // lo que corre.
    void HandleEfectivo(size_t i, bool salida, float& dF, float& dV) const;
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
AnimProperty& PropertyDeLista(std::vector<AnimProperty>& props, int property, int component);
// pone/actualiza un keyframe en 'frame' con 'value', manteniendo la lista ordenada por frame
void SetKeyCurva(AnimProperty& ap, int frame, float value);
// Borra el keyframe de 'frame' tratando de MANTENER LA FORMA de la curva: ajusta los handles de los vecinos por
// minimos cuadrados contra la curva original (conserva las direcciones, ajusta los largos). Es una simplificacion:
// un tramo no siempre puede reproducir dos, pero los extremos y las tangentes quedan iguales.
// Si el tramo no es bezier (o el keyframe es la punta), es un borrado comun: no hay forma que mantener.
void BorrarKeyframeManteniendoForma(AnimProperty& ap, int frame);

// Animación de objeto
class AnimationObject { 
public:
    Object* obj; 
    int FirstKeyFrame;
    int LastKeyFrame;
    std::vector<AnimProperty> Propertys;

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
    std::vector<AnimationObject> objetos; // curvas guardadas (vacio mientras esta es la escena activa)
    // PISTA "CAMARA ACTIVA" (los cortes de una cinematica): 0 o 1 curva (Property AnimCamActiva, componente X) cuyos
    // keyframes valen un INDICE en camNombres. Va en un vector (y no suelta) para que el dope sheet y el undo la
    // traten como cualquier curva (mover / borrar / escalar / Ctrl+Z). A DIFERENCIA de 'objetos' NO se hace swap
    // con un global al activar la escena: vive siempre aca. Las camaras van por NOMBRE (sobreviven al guardado).
    std::vector<AnimProperty> camPista;
    std::vector<std::string>  camNombres;
    // VISTA DE EDICION de un CLIP DE JERARQUIA (animation/W3dAnimSet.h: los clips que un objeto raiz usa
    // para animarse a si mismo y a sus hijos por ruta de nombres; viven COMPARTIDOS en una biblioteca). El
    // editor los edita como una animacion de escena mas -keyframes, dope sheet, curvas, Auto Key, undo- sobre
    // SU raiz: la vista es una COPIA de las curvas del clip atada a los objetos de esa raiz, que se escribe de
    // vuelta en el clip (W3dClipVistaEscribir) al dejar de editarla, al guardar, al dar Play... No se guarda
    // (el clip vive en su biblioteca) y animEscena() / escenaCapa() / la cinematica de una escena no la ven.
    //   - su nombre es el del clip (unico entre los de su biblioteca);
    //   - la pista de la RAIZ es relativa (ver W3dAnimSet.h): al atarla se le suma lo que hace falta para que
    //     el primer frame caiga donde esta la raiz (raizOff*), y al escribirla se le resta.
    bool esClip;             // true = vista de un clip de jerarquia
    Object* duenio;          // su raiz (NULL con esClip = la raiz se borro: la vista queda huerfana)
    W3dClipJer* clipJer;     // el clip que edita (se valida con su serie: W3dClipVistaViva)
    unsigned serieJer;       // la serie del clip al atarla
    unsigned versionVista;   // la version del clip que tiene copiada (distinta = la vista quedo vieja)
    Vector3 raizOffPos, raizOffRot, raizOffEsc;   // lo que se sumo a las curvas de la raiz al atarla
    SceneAnimation(const std::string& n) : name(n), startFrame(1), endFrame(250), fps(30), esClip(false), duenio(0),
                                           clipJer(0), serieJer(0), versionVista(0),
                                           raizOffPos(0, 0, 0), raizOffRot(0, 0, 0), raizOffEsc(0, 0, 0) {}
};

// ---- PISTA "CAMARA ACTIVA" de una animacion de escena ----
// la curva de la escena 'escena' (NULL si no tiene y !crear)
AnimProperty* W3dCamPista(int escena, bool crear);
// indice del nombre en camNombres (lo agrega si falta)
int  W3dCamNombreIdx(int escena, const std::string& nombre);
// pone (o reemplaza) el corte en 'frame' -> esa camara pasa a ser la activa desde ese frame (interpolacion constante)
void W3dCamKey(int escena, int frame, const std::string& nombre);
// la camara que manda en 'frame' (NULL = la escena no tiene pista). Antes del primer corte manda el primero.
const char* W3dCamEnFrame(int escena, int frame);
// aplica la pista en 'frame': llama g_camaraActivaHook con el nombre (si hay pista). Lo llaman el editor (al mover
// el playhead de una animacion de escena) y el juego (W3dAnimEscenaTick, animEscena de lua).
void W3dCamActivaAplicar(int escena, int frame);
// los registra main/: el Core no conoce la clase Camera ni CameraActive
extern void (*g_camaraActivaHook)(const char* nombre);
extern void (*g_animFovHook)(Object* camara, float fov);   // curva AnimFov en el JUEGO (cinematicas)
// aplica las curvas INDEXADAS de una lista: AnimShape a la malla (los pesos de sus shape keys) y AnimPaso a la rutina
// (g_animPasoHook: el Core no conoce la clase Rutina). Lo usan el juego y el editor.
void W3dAplicarFormas(Object* o, std::vector<AnimProperty>& props, int frame);
extern void (*g_animPasoHook)(Object* rutina, int componente, float valor);
extern std::vector<SceneAnimation*> SceneAnimations; // lista global de animaciones de escena
extern int SceneAnimActiva;                          // indice de la escena activa (sus curvas estan en AnimationObjects)

// Seleccion de animacion ACTIVA a nivel APP (la comparten la tarjeta Animation y el Timeline; no depende del objeto
// seleccionado, asi clickear un armature NO cambia la animacion activa):
//   ActiveAnimKind 0 = una animacion de ESCENA (SceneAnimations[SceneAnimActiva])
//   ActiveAnimKind 1 = un CLIP de un armature (ActiveAnimArm y su animActiva)
class Armature; // tipo completo en objects/Armature.h
//   ActiveAnimKind 2 = modo JUEGO (timeline infinito; lo pone main/ con "Es un juego")
//   ActiveAnimKind 3 = una ANIMACION DEL OBJETO de una malla (ActiveAnimMesh): el timeline
//                      muestra/edita sus frames (el mesh sigue al playhead). Es un CONTENEDOR:
//                      capas de vertices / normales / UV + curvas de transform propias.
//   ActiveAnimKind 4 = un CLIP DEL ARMATURE 2D de una malla. Que clip es sale de DOS indices de la
//                      malla (ActiveAnimMesh): el armature ACTIVO (Mesh::armature2dActivo) y, dentro
//                      de el, su clip activo (Armature2D::animActiva) -- es lo que resuelve
//                      Arm2DClipActivo(). Curvas por hueso 2D (posU/posV/rot/escalaX/escalaY) que el
//                      playhead evalua con Armature2DEvaluar. A DIFERENCIA del kind 3, NO congela
//                      UpdateAnimations de la escena: un rig 2D animado convive con el resto del mundo.
//                      OJO: cambiar de armature activo CAMBIA el clip del timeline -> hay que recargar
//                      el rango (Arm2DSetActivo / Arm2DSincronizarRango, en Armature2DAnimation.h).
extern int ActiveAnimKind;
extern Armature* ActiveAnimArm;
class Mesh;
extern Mesh* ActiveAnimMesh;

void InitSceneAnimations();        // crea "Scene" si la lista esta vacia (idempotente)
const char* NombreEscenaActiva();  // nombre de la escena activa
void SetEscenaActiva(int idx);     // hace activa la escena idx (swap de curvas con AnimationObjects)
int  NuevaEscena();                // crea una escena nueva, la deja activa, devuelve su indice
// RENAME de la animacion de escena activa (nombre UNICO); devuelve el nombre que quedo
std::string RenombrarEscenaActiva(const std::string& nombre);
// nombre libre en el espacio GLOBAL de animaciones de escena ('excepto' = indice que conserva el suyo)
std::string SceneAnimNombreLibre(const std::string& base, int excepto);
void BorrarEscenaActiva();         // borra la escena activa (siempre queda al menos "Scene")
// el BORRADO DESHACIBLE del editor (main/undo: UndoBorrarEscenaActiva) en dos mitades:
//  - SACA la animacion ACTIVA de la lista SIN liberarla: sus curvas pasan a su ->objetos y queda activa la de
//    al lado (la misma regla que BorrarEscenaActiva). 'pos' = donde estaba. NULL = no se saca: es la UNICA
//    animacion de escena (esa no se borra, se vacia). La VISTA de un clip de jerarquia sale igual (su CLIP lo
//    saca de su biblioteca quien la borra, ANTES: la vista ya no se escribe).
SceneAnimation* W3dEscenaActivaSacar(int* pos);
//  - la DEVUELVE en 'pos' (acotada) y la deja ACTIVA (con sus curvas)
void W3dEscenaDevolver(SceneAnimation* e, int pos);
// si lo que se edita es la VISTA HUERFANA de un clip de jerarquia (su clip salio de la biblioteca, la raiz cambio
// de biblioteca...), pasa a la animacion de escena mas cercana: una vista muerta no se edita. true = cambio.
bool W3dClipsVistaActivaRevisar();

// ---- REPRODUCTOR de animaciones de ESCENA para el MODO JUEGO (kind 2) ----
// El editor evalua las curvas de escena SOLO con kind 0 (AplicarAnimacionObjetos,
// main/); en un juego las dispara el script Lua: animEscena("walk_player", loop).
// UN clip activo por vez (alcanza para el personaje del demo; instancias en
// paralelo para NPCs es el paso siguiente). El tick lo llama la simulacion
// (TickReal de SimJuego) una vez por frame: avanza el reloj y APLICA las curvas
// pos/rot/escala/visible sobre los objetos referenciados.
int   W3dAnimEscenaIdx(const char* nombre);   // indice en SceneAnimations (-1 si no esta)
float W3dAnimEscenaDur(int idx);              // duracion en segundos (0 si invalido)
bool  W3dAnimEscenaPlay(int idx, bool loop);  // arranca desde su startFrame
void  W3dAnimEscenaTick(float dt);            // avanzar + aplicar (1 vez por tick de sim)
void  W3dAnimEscenaReset();                   // al STOP del juego: nada sonando
int   W3dAnimEscenaFrame();                   // frame (entero) de la que suena; -1 = ninguna
// fija el frame de la que suena y APAGA su reloj propio: el script manda (animEscenaFrame de lua). Para sincronizar
// una cinematica con UN solo reloj: camaras, caras y clips de los personajes en el MISMO frame (sin micro saltos).
void  W3dAnimEscenaFijar(float frame);
int   W3dAnimEscenaSonando();                 // indice de la que suena; -1 = ninguna
// el estado del reproductor, para dejarlo en pausa y retomarlo tal cual (una CINEMATICA que se juega en otra
// raiz: al volver, la animacion de escena que sonaba en el juego sigue donde iba, ver main/W3dRaices.h)
struct W3dAnimEscenaEstado {
    int idx; float frame; bool loop, manual;
    W3dAnimEscenaEstado() : idx(-1), frame(0.0f), loop(true), manual(false) {}
};
void  W3dAnimEscenaGuardarEstado(W3dAnimEscenaEstado* e);
void  W3dAnimEscenaReponerEstado(const W3dAnimEscenaEstado& e);
// true si la que suena es de UNA pasada y ya llego a su ultimo frame (una cinematica que termino)
bool  W3dAnimEscenaTermino();

// un cabezal en LOOP (en [0, periodo), periodo > 0) que se paso: vuelve adentro de UNA vez. El "while (f >=
// periodo) f -= periodo" de siempre colgaba el tick con una velocidad o un frame ENORMES desde lua
// (animObjetoVelocidad, objetoCapaFrame, animClipVelocidad...): en float, restarle una vuelta a un numero muy
// grande no lo cambia. Las pocas vueltas de siempre dan lo mismo que antes; un negativo queda igual (como
// antes); infinito o NaN vuelve a 0.
float W3dCicloEnvolver(float f, float periodo);

// ---- VISTAS DE EDICION DE LOS CLIPS DE JERARQUIA (SceneAnimation::esClip, ver arriba) ----
bool  W3dAnimEsClip(int idx);                 // SceneAnimations[idx] es la vista de un clip (viva o huerfana)
// la vista de 'c' (un clip de la biblioteca de 'raiz') en la lista de animaciones de la raiz ACTIVA: la que
// ya habia o una nueva (atada ya: sus curvas son las del clip). NO la activa. -1 = no se pudo.
int   W3dClipVista(Object* raiz, W3dClipJer* c);
// la vista sigue editando un clip que existe (su raiz vive colgada de un arbol -no detachada por un borrado con
// undo-, sigue con esa biblioteca y el clip sigue en ella)
bool  W3dClipVistaViva(int idx);
// clip -> vista (la re-ata: sus curvas vuelven a ser las del clip) / vista -> clip (true = el clip cambio)
void  W3dClipVistaLeer(int idx);
bool  W3dClipVistaEscribir(int idx);
// escribe en su clip la vista que se esta editando (si lo que esta activo es una). Lo llaman quienes van a
// LEER los clips: el guardado, el Play, el cambio de raiz, el harness.
void  W3dClipsVistasSincronizar();
// saca de la lista las vistas HUERFANAS (su clip ya no existe o su raiz se borro) que no son la activa
void  W3dClipsVistasPurgar();
// el clip 'c' cambio POR FUERA de su vista (renombrado desde el outliner, un undo): si lo que se edita es una
// vista de 'c', se re-ata (toma su nombre y sus curvas). Las demas vistas lo notan solas por su version.
void  W3dClipVistaActivaReleer(const W3dClipJer* c);
// la biblioteca de 'duenio' se cambio por OTRA con los MISMOS clips en el mismo orden (el guardado junta dos
// bibliotecas iguales en una: main/io/GuardarAnimSets.cpp): sus vistas en 'lista' pasan a editar el clip
// equivalente de la nueva, sin re-atarse (el contenido es el mismo: sus curvas siguen valiendo). Sin esto la
// vista que se editaba quedaba HUERFANA y lo que se le keyeaba no llegaba a ningun clip.
void  W3dClipsVistasRecambiar(std::vector<SceneAnimation*>& lista, const Object* duenio,
                              const std::vector<W3dClipJer*>& viejos, const std::vector<W3dClipJer*>& nuevos);
// la etiqueta de una animacion de escena para la UI ("Puerta: abrir" si es la vista de un clip; su nombre si no)
std::string W3dAnimEscenaEtiqueta(int idx);
// EDITOR: se le pueden insertar keyframes a 'o' en la animacion activa? (en la vista de un clip de jerarquia,
// solo a su raiz y a sus descendientes: el clip no puede nombrar lo que no es de su jerarquia)
bool  W3dAnimObjetoPermitido(const Object* o);

// ---- REPRODUCTOR DE CLIPS DE JERARQUIA (el juego: animObjeto de lua) ----
// Un cabezal POR RAIZ, independiente del de animEscena(), de los clips de los armatures y de las demas raices:
// varias animaciones a la vez (la puerta que se abre, el ascensor que sube, el auto hecho de objetos...). El
// clip se busca en la biblioteca de la raiz y, si no esta, por nombre calificado "Biblioteca/clip" (o suelto,
// si es el unico con ese nombre): cualquier jerarquia con los mismos nombres lo puede reproducir. Lo avanza
// W3dAnimObjetosTick una vez por tick del juego (Play del editor y runtime, junto a W3dAnimEscenaTick).
// arranca el clip en 'o' desde su inicio. Devuelve su duracion en segundos (< 0 = no hay un clip con ese nombre)
float W3dAnimObjetoPlay(Object* o, const std::string& clip, bool loop);
void  W3dAnimObjetosTick(float dt);
float W3dAnimObjetoFrame(const Object* o);          // frame del clip que suena en 'o' (-1 = ninguno)
void  W3dAnimObjetoFijarFrame(Object* o, float f);  // mueve su cabezal (el clip sigue sonando desde ahi)
bool  W3dAnimObjetoTermino(const Object* o);        // true: nada suena en 'o' o su clip de una pasada termino
std::string W3dAnimObjetoClip(const Object* o);     // el nombre del clip que suena ("" = ninguno)
void  W3dAnimObjetoParar(Object* o);                // lo deja quieto donde esta
// velocidad del clip que suena (1 = sus fps; 2 = el doble; negativa = hacia atras). false = no suena nada
bool  W3dAnimObjetoVelocidad(Object* o, float v);
// el RETARGET de la raiz 'o' (W3D_RETARGET_*, SkeletalAnimation.h; -1 = el de cada clip). Lo recuerda para
// los clips que arranque despues y lo aplica al que suena.
void  W3dAnimObjetoRetarget(Object* o, int modo);
void  W3dAnimObjetosReset();                        // al STOP del juego: nada suena
int   W3dAnimObjetosSonando();                      // cuantas raices tienen un clip sonando

// las curvas de UN objeto en el JUEGO, en el frame continuo 'fr': transformacion, visible/render, formas,
// fov de camara y colores/atenuacion/spot de una luz. Lo usan animEscena, los clips de jerarquia y las capas
// de escena (el editor usa W3dAplicarCurvasEnFrame, que ademas marca lo posado para el guardado)
void  W3dAplicarCurvasJuego(Object* o, std::vector<AnimProperty>& props, float fr);

// las curvas de objeto CAMBIARON por fuera del cabezal (otra raiz, otro clip, el estado base de un juego):
// el editor tiene que volver a posar aunque el frame sea el mismo (AplicarAnimacionObjetos compara esto)
extern unsigned int g_animCurvasGen;
inline void W3dAnimCurvasInvalidar() { g_animCurvasGen++; }

// Start/End/FPS PROPIOS de la animacion activa (escena o clip de armature). La comparten la tarjeta y el timeline.
void AnimCargarRangoActivo();      // StartFrame/EndFrame/AnimFPS <- animacion activa (al seleccionarla)
void AnimSetStart(int v);          // animacion activa.start = v; StartFrame = v
void AnimSetEnd(int v);            // animacion activa.end   = v; EndFrame   = v
void AnimSetFps(int v);            // animacion activa.fps   = v; AnimFPS    = v

#endif