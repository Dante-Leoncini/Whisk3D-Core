#ifndef SKELETAL_ANIMATION_H
#define SKELETAL_ANIMATION_H

#include <vector>
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include <string>
#include "animation/W3dCapaAnim.h" // capas del MIX
#include "animation/Animation.h"   // AnimProperty, keyFrame, enum AnimPosition/AnimRotation/AnimScale

// ============================================================================
//  ANIMACION DE ESQUELETO (per-hueso). Cada CLIP es un "take"/AnimStack de FBX:
//  un nombre + fps + un track por hueso. Cada track guarda las curvas de
//  Posicion / Rotacion / Escala reusando AnimProperty (keyframes con valor XYZ
//  e interpolacion). Mapea 1:1 a FBX:
//    SkeletalAnimation -> AnimationStack + AnimationLayer
//    BoneTrack         -> los 3 AnimCurveNode (Lcl Translation/Rotation/Scaling)
//    AnimProperty      -> un AnimCurveNode (con 3 AnimationCurve: X, Y, Z)
//    keyFrame          -> un Key (KeyTime = frame; KeyValueFloat = valueX/Y/Z)
//  Los pesos por-vertice (que hueso mueve que vertice) viven en la MALLA
//  (vertex groups); aca vive el MOVIMIENTO de los huesos en el tiempo.
// ============================================================================

// tracks de UN hueso: las curvas de Position / Rotation / Scale (AnimProperty).
class BoneTrack {
public:
    int bone;                              // indice en Armature::bones (-1 = sin asignar)
    std::vector<AnimProperty> Propertys;   // Position / Rotation / Scale
    BoneTrack() : bone(-1) {}
    // devuelve (creando si falta) la AnimProperty de una propiedad (AnimPosition/AnimRotation/AnimScale)
    // devuelve (creando si falta) la CURVA de un (propiedad, componente): (AnimPosition, AnimX) = "X Location".
    // Cada componente tiene sus PROPIOS keyframes -> se puede curvar/mover X sin tocar Y ni Z.
    AnimProperty& PropertyDe(int property, int component);
};

// un CLIP de animacion de esqueleto (= un AnimationStack de FBX).
class SkeletalAnimation {
public:
    std::string name;
    int FrameRate;                         // fps del clip (FBX: FrameRate del take)
    int startFrame;                        // rango del clip: arranca en 1
    int endFrame;                          // rango del clip: 0 por defecto (vacio; se setea al importar/editar)
    std::vector<BoneTrack> tracks;         // uno por hueso animado
    SkeletalAnimation(const std::string& n = "Animation") : name(n), FrameRate(24), startFrame(1), endFrame(0) {}
    // devuelve (creando si falta) el track de un hueso
    BoneTrack& TrackDe(int bone);
};

class Armature; // objects/Armature.h

// evalua la POSE del esqueleto en 'frame' con el clip ACTIVO (FK): llena bone.poseHead/poseTail.
// Si no hay clip activo (o el hueso no esta animado) la pose = rest. Lo llama el render del armature.
void EvaluarPoseEsqueleto(Armature* a, int frame);
// JUEGO (ActiveAnimKind 2): cada armature reproduce su clip activo con su propio cabezal (Armature::juegoFrame).
void W3dArmaturesJuegoTick(float dt);   // avanza todos los de la escena (lo llaman el Play del editor y el runtime)
bool W3dArmaturePlayClip(Armature* a, int clip, bool loop, bool reiniciar); // elige clip (reinicia si cambia)
int  W3dArmatureClipPorNombre(const Armature* a, const std::string& nombre); // -1 = no esta

// ===== MIX de animaciones (capas; ver W3dCapaAnim en objects/Armature.h) =====
// g_animMix: el editor esta en modo Mix (el selector del timeline dice "Mix"). Los armatures con capas muestran
// la mezcla; el timeline/dope sheet siguen editando la capa ELEGIDA (ActiveAnimArm + su animActiva).
extern bool  g_animMix;
extern float g_mixInicio, g_mixFin;                  // el rango del timeline en el modo Mix
extern std::vector<W3dCapaAnim> g_mixEscenas;        // capas de ANIMACIONES DE ESCENA (curvas de objetos)
extern int   g_mixEscenaActiva;                      // la capa de escena elegida en la lista (-1 = ninguna)
// indice del clip de una capa (resuelve el nombre; -1 = no esta)
int   W3dCapaClip(const Armature* a, W3dCapaAnim& c);
// frame (del clip) en que suena una capa: jugando el cabezal de la capa; en el editor el del mix
float W3dCapaFrameEditor(const W3dCapaAnim& c, int frameMix, int clipIni, int clipFin);
// TRANSICION: congela la pose actual y la funde hacia la nueva durante 'segundos' (el hokan del juego)
void  W3dArmatureTransicion(Armature* a, float segundos);
// MIX DE ESCENAS (Animation.cpp): aplica las capas de escena (editor: el frame del mix; juego: su cabezal)
bool  W3dMixEscenasAplicar(int frameMix, bool juego);
void  W3dMixEscenasTick(float dt);        // juego: avanza las capas de escena y aplica la mezcla
void  W3dMixEscenasSoltar();              // devuelve los objetos a su base (al salir del Mix)
// precomputa las matrices de skinning de cada hueso (skinA/skinInvBind). Llamar UNA vez tras importar el esqueleto.
void PrepararSkin(Armature* a);
// RIG AUTORADO en el editor (huesos creados a mano, sin datos de FBX): arma el rest y las matrices de skin
// MINIMAS desde head/tail -> restT = head - head(padre), restR=0, restS=1, bind = T(head), hasSkin/hasRest = true,
// y marca el armature como FK estandar Y-up (skinGltf + skinAutorado). Con esto SkinearMesh acepta los huesos
// (antes hasSkin=false los descartaba) y posar un hueso rota/traslada/escala la malla alrededor de su HEAD.
// Llamar al asignar el modificador Armature o al salir del Edit Mode de huesos (y tras cada edicion de huesos).
// NO toca rigs importados (si algun hueso ya trae rest de FBX/glTF y el armature no es autorado, es no-op).
void PrepararSkinAutorado(Armature* a);
// seleccion de la formula de skinning (para A/B testing headless con 'skinformula' del harness):
//   0 = FK-rest puro:      skinMatrix = animNode * inv(restNode)          (no usa TransformLink)
//   1 = bind cuando valido: skinMatrix = animNode * inv(TL) * Rhat        (TL = TransformLink; Rhat = TL_ref*inv(restNode_ref))
// La 1 corrige los huesos cuyo rest Lcl NO coincide con el bind (twist/helpers); cae a la 0 si el TL es invalido (LISA).
extern int g_skinFormula;
// deforma m->skinVertex a la pose del esqueleto (m->skinArmature) en CurrentFrame. Liviano; no-op si no hay skinning.
class Mesh; void SkinearMesh(Mesh* m);

// FK de la pose ACTIVADO (default true): reproduce la animacion moviendo los huesos. El FK usa los transforms
// LOCALES del FBX (Lcl Translation/Rotation/Scaling) y convierte la salida Z-up(nodo)->Y-up. Los armatures MANUALES
// (sin datos de rest del FBX) se muestran en bind (sin FK).
extern bool g_skelAnimPreview;

// gestion de clips (MISMO patron que los vertex groups del mesh: crear/borrar/mover el activo)
void CrearAnimacion(Armature* a);
// nombre libre de CLIP dentro del armature ('excepto' = indice que conserva el suyo, -1 = ninguno)
std::string AnimNombreLibre(Armature* a, const std::string& base, int excepto);                // crea un clip vacio (nombre unico) y lo deja activo
// Insert Keyframe (i): guarda la pose de los huesos seleccionados en CurrentFrame. 'canales' = mascara
// KfCanalLoc/Rot/Scl (Animation.h) del menu desplegable; 0/omitido = los tres (como siempre).
void InsertarKeyframeEsqueleto(Armature* a, int canales = 0);
// AUTO KEY de la pose: guarda SOLO los canales del hueso que cambiaron respecto de (T0,R0,S0) = su pose de ANTES
// del transform. Uso: Prep una vez, AutoKeyHueso por hueso, Fin una vez (si se guardo algo).
// MOTION TRAIL de un HUESO: el camino (SOLO POSICION) de su cabeza en espacio NODO, frame a frame. Reusa el FK
// real (EvaluarPoseEsqueleto) y deja el esqueleto como estaba. 'keys' = los frames con keyframe de posicion.
bool MotionTrailHuesoNodo(Armature* a, int bone, std::vector<Vector3>& pts, std::vector<int>& keys,
                          int& desde, int& hasta);

bool AutoKeyEsqueletoPrep(Armature* a);
int  AutoKeyHueso(Armature* a, int i, const Vector3& T0, const Vector3& R0, const Vector3& S0);
void AutoKeyEsqueletoFin(Armature* a);
// helpers para el transform interactivo de huesos (Pose Mode): conversion rotacion-mundo <-> euler LOCAL del hueso.
Matrix4 SkelNodeToYupMat();                       // matriz NodeToYup (nodo Z-up -> escena Y-up)
Matrix4 SkelMatRotEuler(const Vector3& deg, int order); // rotacion euler (grados) en el orden FBX
Matrix4 SkelBoneWorldNode(Armature* a, int bone); // world del hueso en espacio nodo (pose actual)
Vector3 SkelMatrizAEulerFBX(const Matrix4& M, int order); // rotacion (matriz) -> euler (grados) orden FBX
// Apply Transform (Ctrl+A) sobre un armature: hornea B (= inv(M_arm_reseteado)*M_arm) en la rest de los huesos de modo
// que world_FK'=B*world_FK -> skinMatrix'=B*skinMatrix (skinA intacto) y las mallas skinneadas hijas quedan identicas
// al resetear el transform del objeto. El editor llama a esto y luego resetea el pos/rot/scale del objeto armature.
void HornearTransformEnHuesos(Armature* a, const Matrix4& B);
void DuplicarAnimacionActiva(Armature* a);       // duplica el clip activo (nombre+fps+rango+keyframes) y lo deja activo
// HOOK del editor: al elegir un clip desde la LISTA de animaciones (PropList modo 5, tab Armature) hay que sincronizar
// la seleccion APP-WIDE (ActiveAnimKind/ActiveAnimArm) + cargar Start/End/FPS, cosa que vive en el editor. La lista
// (WhiskUI) llama a este hook si esta seteado. Sin esto la lista cambiaba arm->animActiva pero el timeline no se enteraba.
extern void (*OnSeleccionarAnimClip)(Armature* a, int clipIdx);
// OJO (EDITOR): estas dos CORREN LOS INDICES de animations[] y los destinos de undo de un
// rename van por (armature, INDICE). Desde el editor NO se llaman directo: van por
// UndoBorrarClipArm / UndoMoverClipArm (main/undo/Undo.h), que remapean los destinos ya
// capturados (y el mover ademas deja el paso de Ctrl+Z). Sin eso, renombrar un clip y despues
// borrar/mover otro hacia que el Ctrl+Z escribiera el nombre viejo ENCIMA del clip de al lado.
// Aca quedan crudas porque el Core compila tambien para el RUNTIME de los juegos, que no tiene
// undo (ni stacks que remapear).
void BorrarAnimacionActiva(Armature* a);         // borra el clip activo (puede quedar 0)
void MoverAnimacionActiva(Armature* a, int dir); // reordena el clip activo (-1 arriba / +1 abajo)

#endif // SKELETAL_ANIMATION_H
