#ifndef SKELETAL_ANIMATION_H
#define SKELETAL_ANIMATION_H

#include <vector>
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include <string>
#include "animation/W3dLayerAnim.h" // capas del MIX
#include "animation/Animation.h"    // AnimProperty, keyFrame, enum AnimPosition/AnimRotation/AnimScale

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
    int bone;                               // indice en Armature::bones (-1 = sin asignar)
    std::vector<AnimProperty> Properties;   // Position / Rotation / Scale
    BoneTrack() : bone(-1) {}
    // devuelve (creando si falta) la AnimProperty de una propiedad (AnimPosition/AnimRotation/AnimScale)
    // devuelve (creando si falta) la CURVA de un (propiedad, componente): (AnimPosition, AnimX) = "X Location".
    // Cada componente tiene sus PROPIOS keyframes -> se puede curvar/mover X sin tocar Y ni Z.
    AnimProperty& PropertyOf(int property, int component);
};

// un CLIP de animacion de esqueleto (= un AnimationStack de FBX).
class SkeletalAnimation {
public:
    std::string name;
    int FrameRate;                         // fps del clip (FBX: FrameRate del take)
    int startFrame;                        // rango del clip: arranca en 1
    int endFrame;                          // rango del clip: 0 por defecto (vacio; se setea al importar/editar)
    std::vector<BoneTrack> tracks;         // uno por hueso animado
    // RETARGET (W3dArmatureRetarget): el TAMANO del esqueleto con el que se grabo el clip (la diagonal de la
    // caja de sus cabezas en rest, W3dArmatureRestSize). 0 = no se sabe (un clip importado o de un archivo
    // viejo): el reproductor lo deduce de las traslaciones del propio clip en su primer frame. Viaja en el
    // .w3da solo si se sabe (un animset viejo se re-guarda con los mismos bytes).
    float heightRest;
    SkeletalAnimation(const std::string& n = "Animation") : name(n), FrameRate(24), startFrame(1), endFrame(0),
                                                           heightRest(0.0f) {}
    // devuelve (creando si falta) el track de un hueso
    BoneTrack& TrackOf(int bone);
};

class Armature; // objects/Armature.h

// evalua la POSE del esqueleto en 'frame' con el clip ACTIVO (FK): llena bone.poseHead/poseTail.
// Si no hay clip activo (o el hueso no esta animado) la pose = rest. Lo llama el render del armature.
void EvaluatePoseSkeleton(Armature* a, int frame);
// UNA FRACCION DE CUADRO para la pose (0..1, 0 = el cuadro entero de siempre): el desenfoque de movimiento trazado evalua
// el esqueleto en el instante exacto de cada pase (con las rotaciones interpoladas: un giro rapido no corta camino por
// el centro como pasaba interpolando los vertices). Quien la pone la vuelve a 0 y descarta el cache de la pose
extern float g_poseFraction;
// RETARGET POR TAMANO (animRetarget de lua / W3dArmatureRetarget): un personaje mas alto o mas chico con la MISMA
// jerarquia de nombres de huesos reproduce el mismo clip sin deformarse.
//   W3D_RETARGET_COMPLETE   (0, el de siempre) el clip tal cual: traslaciones, rotaciones y escalas.
//   W3D_RETARGET_ROTATIONS (1) solo las ROTACIONES del clip: la traslacion y la escala de cada hueso son las
//                           de REPOSO del destino (sus proporciones), salvo las RAICES (huesos sin padre), cuyo
//                           movimiento se ESCALA por tamDestino / tamOrigen (el del clip, heightRest; si no
//                           se sabe, el de sus traslaciones en su primer frame).
// Solo cambia la reproduccion de ESTE armature (su pose): los clips (compartidos) no se tocan. Vale igual para las
// capas del Mix (cada capa con la escala de SU clip) y para los rigs de FK reconstruido (biped: el tamano se mide
// en sus TransformLink y la traslacion de cada hueso entra como delta contra su rest, asi que los hijos no se
// corren y la raiz avanza su delta escalado). Se guarda en el nodo del armature ("retarget": "rotaciones").
enum { W3D_RETARGET_COMPLETE = 0, W3D_RETARGET_ROTATIONS = 1 };
void  W3dArmatureRetarget(Armature* a, int mode);
// el TAMANO del esqueleto en REPOSO: la diagonal de la caja de las cabezas de los huesos por FK con los
// transforms de rest (espacio del nodo; misma medida que heightRest). En un biped (FK reconstruido), la de las
// cabezas de sus TransformLink. 0 si no tiene huesos con rest. Los importadores FBX/glTF y CreateAnimation la
// anotan en cada clip (heightRest).
float W3dArmatureRestSize(const Armature* a);
// JUEGO (ActiveAnimKind 2): cada armature reproduce su clip activo con su propio cabezal (Armature::gameFrame).
void W3dArmaturesGameTick(float dt);   // avanza todos los de la escena (lo llaman el Play del editor y el runtime)
bool W3dArmaturePlayClip(Armature* a, int clip, bool loop, bool restart); // elige clip (reinicia si cambia)
int  W3dArmatureClipByName(const Armature* a, const std::string& name); // -1 = no esta

// ===== MIX de animaciones (capas; ver W3dLayerAnim en objects/Armature.h) =====
// g_animMix: el editor esta en modo Mix (el selector del timeline dice "Mix"). Los armatures con capas muestran
// la mezcla; el timeline/dope sheet siguen editando la capa ELEGIDA (ActiveAnimArm + su activeAnim).
extern bool  g_animMix;
extern float g_mixStart, g_mixEnd;                   // el rango del timeline en el modo Mix
extern std::vector<W3dLayerAnim> g_mixScenes;        // capas de ANIMACIONES DE ESCENA (curvas de objetos)
extern int   g_mixActiveScene;                       // la capa de escena elegida en la lista (-1 = ninguna)
// indice del clip de una capa (resuelve el nombre; -1 = no esta)
int   W3dLayerClip(const Armature* a, W3dLayerAnim& c);
// frame (del clip) en que suena una capa: jugando el cabezal de la capa; en el editor el del mix
float W3dLayerFrameEditor(const W3dLayerAnim& c, int frameMix, int clipStart, int clipEnd);
// TRANSICION: congela la pose actual y la funde hacia la nueva durante 'segundos' (crossfade de pose)
void  W3dArmatureTransition(Armature* a, float seconds);
// MIX DE OBJETOS (Animation.cpp): aplica las capas de escena y las CAPAS DE CLIPS DE JERARQUIA de cada raiz de la
// escena activa (W3dHierRoot::capas, animation/W3dAnimSet.h), sobre la misma mezcla (editor: el frame del mix;
// juego: el cabezal de cada capa). false = no hay ninguna capa.
bool  W3dMixScenesApply(int frameMix, bool game);
void  W3dMixScenesTick(float dt);        // juego: avanza las capas (de escena y de jerarquia) y aplica la mezcla
void  W3dMixScenesRelease();             // devuelve los objetos a su base (al salir del Mix)
void  W3dMixScenesBasesSave();           // Play del editor: guarda las bases (Stop las repone,
void  W3dMixScenesBasesRestore();        // sin mover objetos: eso lo hace el snapshot del Play)
void  W3dMixScenesForget();              // abrir otro proyecto: olvida las bases sin mover (van a borrarse)
// las BASES del Mix: cada objeto que el Mix poso y la transformacion que tenia antes (a la que lo devuelve
// W3dMixScenesRelease). El GUARDADO escribe esa y no la mezcla del cabezal: una raiz que solo anima el Mix (sus
// capas de clips de jerarquia) no esta en ninguna lista de curvas y se guardaba con la pose del frame del Mix
void  W3dMixScenesBases(std::vector<Object*>& objs, std::vector<Vector3>& pos, std::vector<Vector3>& rot,
                         std::vector<Vector3>& esc);
// precomputa las matrices de skinning de cada hueso (skinA/skinInvBind). Llamar UNA vez tras importar el esqueleto.
void PrepareSkin(Armature* a);
// RIG AUTORADO en el editor (huesos creados a mano, sin datos de FBX): arma el rest y las matrices de skin
// MINIMAS desde head/tail -> restT = head - head(padre), restR=0, restS=1, bind = T(head), hasSkin/hasRest = true,
// y marca el armature como FK estandar Y-up (skinGltf + skinAuthored). Con esto SkinMesh acepta los huesos
// (antes hasSkin=false los descartaba) y posar un hueso rota/traslada/escala la malla alrededor de su HEAD.
// Llamar al asignar el modificador Armature o al salir del Edit Mode de huesos (y tras cada edicion de huesos).
// NO toca rigs importados (si algun hueso ya trae rest de FBX/glTF y el armature no es autorado, es no-op).
void PrepareAuthoredSkin(Armature* a);
// seleccion de la formula de skinning (para A/B testing headless con 'skinformula' del harness):
//   0 = FK-rest puro:      skinMatrix = animNode * inv(restNode)          (no usa TransformLink)
//   1 = bind cuando valido: skinMatrix = animNode * inv(TL) * Rhat        (TL = TransformLink; Rhat = TL_ref*inv(restNode_ref))
// La 1 corrige los huesos cuyo rest Lcl NO coincide con el bind (twist/helpers); cae a la 0 si el TL es invalido (LISA).
extern int g_skinFormula;
// deforma m->skinVertex a la pose del esqueleto (m->skinArmature) en CurrentFrame. Liviano; no-op si no hay skinning.
class Mesh; void SkinMesh(Mesh* m);
// PIEL RIGIDA: si la malla sigue ENTERA a un solo hueso, su indice (RenderObject la dibuja con la matriz de skin de ese
// hueso, sin deformar vertices); si no, -1. Deja la pose evaluada al cuadro actual. Ver SkeletalAnimation.cpp
int SkinRigidBone(Mesh* m);

// FK de la pose ACTIVADO (default true): reproduce la animacion moviendo los huesos. El FK usa los transforms
// LOCALES del FBX (Lcl Translation/Rotation/Scaling) y convierte la salida Z-up(nodo)->Y-up. Los armatures MANUALES
// (sin datos de rest del FBX) se muestran en bind (sin FK).
extern bool g_skelAnimPreview;

// gestion de clips (MISMO patron que los vertex groups del mesh: crear/borrar/mover el activo)
void CreateAnimation(Armature* a);
// nombre libre de CLIP dentro del armature ('excepto' = indice que conserva el suyo, -1 = ninguno)
std::string AnimFreeName(Armature* a, const std::string& base, int except);                // crea un clip vacio (nombre unico) y lo deja activo
// Insert Keyframe (i): guarda la pose de los huesos seleccionados en CurrentFrame. 'canales' = mascara
// KfChannelLoc/Rot/Scl (Animation.h) del menu desplegable; 0/omitido = los tres (como siempre).
void InsertKeyframeSkeleton(Armature* a, int channels = 0);
// AUTO KEY de la pose: guarda SOLO los canales del hueso que cambiaron respecto de (T0,R0,S0) = su pose de ANTES
// del transform. Uso: Prep una vez, AutoKeyBone por hueso, Fin una vez (si se guardo algo).
// MOTION TRAIL de un HUESO: el camino (SOLO POSICION) de su cabeza en espacio NODO, frame a frame. Reusa el FK
// real (EvaluatePoseSkeleton) y deja el esqueleto como estaba. 'keys' = los frames con keyframe de posicion.
bool MotionTrailBoneNode(Armature* a, int bone, std::vector<Vector3>& pts, std::vector<int>& keys,
                          int& from, int& until);

bool AutoKeySkeletonPrep(Armature* a);
int  AutoKeyBone(Armature* a, int i, const Vector3& T0, const Vector3& R0, const Vector3& S0);
void AutoKeySkeletonEnd(Armature* a);
// helpers para el transform interactivo de huesos (Pose Mode): conversion rotacion-mundo <-> euler LOCAL del hueso.
Matrix4 SkelNodeToYupMat();                       // matriz NodeToYup (nodo Z-up -> escena Y-up)
Matrix4 SkelMatRotEuler(const Vector3& deg, int order); // rotacion euler (grados) en el orden FBX
Matrix4 SkelBoneWorldNode(Armature* a, int bone); // world del hueso en espacio nodo (pose actual)
Vector3 SkelMatrixToEulerFBX(const Matrix4& M, int order); // rotacion (matriz) -> euler (grados) orden FBX
// Apply Transform (Ctrl+A) sobre un armature: hornea B (= inv(M_arm_reseteado)*M_arm) en la rest de los huesos de modo
// que world_FK'=B*world_FK -> skinMatrix'=B*skinMatrix (skinA intacto) y las mallas skinneadas hijas quedan identicas
// al resetear el transform del objeto. El editor llama a esto y luego resetea el pos/rot/scale del objeto armature.
void BakeTransformInBones(Armature* a, const Matrix4& B);
void DuplicateActiveAnimation(Armature* a);       // duplica el clip activo (nombre+fps+rango+keyframes) y lo deja activo
// HOOK del editor: al elegir un clip desde la LISTA de animaciones (PropList modo 5, tab Armature) hay que sincronizar
// la seleccion APP-WIDE (ActiveAnimKind/ActiveAnimArm) + cargar Start/End/FPS, cosa que vive en el editor. La lista
// (WhiskUI) llama a este hook si esta seteado. Sin esto la lista cambiaba arm->activeAnim pero el timeline no se enteraba.
extern void (*OnSelectAnimClip)(Armature* a, int clipIdx);
// OJO (EDITOR): estas dos CORREN LOS INDICES de animations[] y los destinos de undo de un
// rename van por (armature, INDICE). Desde el editor NO se llaman directo: van por
// UndoDeleteClipArm / UndoMoveClipArm (main/undo/Undo.h), que remapean los destinos ya
// capturados (y el mover ademas deja el paso de Ctrl+Z). Sin eso, renombrar un clip y despues
// borrar/mover otro hacia que el Ctrl+Z escribiera el nombre viejo ENCIMA del clip de al lado.
// Aca quedan crudas porque el Core compila tambien para el RUNTIME de los juegos, que no tiene
// undo (ni stacks que remapear).
void DeleteActiveAnimation(Armature* a);        // borra el clip activo (puede quedar 0)
void MoveActiveAnimation(Armature* a, int dir); // reordena el clip activo (-1 arriba / +1 abajo)

#endif // SKELETAL_ANIMATION_H
