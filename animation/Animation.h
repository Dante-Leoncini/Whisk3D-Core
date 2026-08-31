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
enum { AnimPosition, AnimRotation, AnimScale, AnimVisible, AnimRender, AnimVertex, AnimFov, AnimColor,
       AnimClip, AnimAmbient, AnimSpecular, AnimAtten, AnimSpot, AnimLightMisc };
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

// Una CURVA de animacion = (Property, component). Ej: (AnimPosition, AnimX) = "X Location".
// Cada componente tiene SUS PROPIOS keyframes -> se puede mover/borrar/curvar X sin tocar Y ni Z.
class AnimProperty {
public:
    int Property;   // AnimPosition / AnimRotation / AnimScale
    int component;  // AnimX / AnimY / AnimZ
    std::vector<keyFrame> keyframes;

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
    SceneAnimation(const std::string& n) : name(n), startFrame(1), endFrame(250), fps(30) {}
};
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

// Start/End/FPS PROPIOS de la animacion activa (escena o clip de armature). La comparten la tarjeta y el timeline.
void AnimCargarRangoActivo();      // StartFrame/EndFrame/AnimFPS <- animacion activa (al seleccionarla)
void AnimSetStart(int v);          // animacion activa.start = v; StartFrame = v
void AnimSetEnd(int v);            // animacion activa.end   = v; EndFrame   = v
void AnimSetFps(int v);            // animacion activa.fps   = v; AnimFPS    = v

#endif