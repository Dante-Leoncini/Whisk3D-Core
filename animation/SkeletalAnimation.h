#ifndef SKELETAL_ANIMATION_H
#define SKELETAL_ANIMATION_H

#include <vector>
#include <string>
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
    AnimProperty& PropertyDe(int property);
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
// precomputa las matrices de skinning de cada hueso (skinA/skinInvBind). Llamar UNA vez tras importar el esqueleto.
void PrepararSkin(Armature* a);
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
void CrearAnimacion(Armature* a);                // crea un clip vacio (nombre unico) y lo deja activo
void BorrarAnimacionActiva(Armature* a);         // borra el clip activo (puede quedar 0)
void MoverAnimacionActiva(Armature* a, int dir); // reordena el clip activo (-1 arriba / +1 abajo)

#endif // SKELETAL_ANIMATION_H
