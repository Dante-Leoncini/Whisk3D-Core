#ifndef ARMATURE2D_ANIMATION_H
#define ARMATURE2D_ANIMATION_H

#include <vector>
#include <string>
#include "animation/Animation.h"   // AnimProperty, keyFrame, enum AnimPosition/AnimRotation/AnimScale, KfCanal*

// ============================================================================
//  ANIMACION DEL ARMATURE 2D (huesos-linea en espacio UV, Mesh::armatures2d: la malla puede
//  tener VARIOS rigs 2D y cada uno tiene SUS clips).
//
//  CALCADO del armature 3D (SkeletalAnimation / BoneTrack / AnimProperty): mismo modelo de
//  clips con nombre + fps + rango + un track por hueso, y cada track con SUS curvas por canal
//  (cada componente con keyframes propios, bezier con handles). Se serializa con los MISMOS
//  helpers del .w3d (WriteCurve / LoadCurveJson).
//
//  POR QUE existe (antes no habia): la pose 2D se HORNEABA en la capa uv de la vertex anim
//  (VertexFrame::uvs guarda el array uv ENTERO por keyframe). Eso costaba vertexSize*2 floats
//  por keyframe en vez de 5 por hueso movido, no dejaba curva por hueso (el dope sheet solo
//  mostraba una fila plana "UV") y obligaba a una vertex anim completa por cada animacion del
//  mismo rig 2D. Con clips propios cada malla tiene su lista de clips y su clip activo: dos
//  personajes 2D en la escena = dos rigs con curvas separadas.
//
//  CANALES: se mapean sobre el enum EXISTENTE (no se agregan valores nuevos: los .w3d viejos
//  guardan el int del canal y correrlos rompería lo ya guardado):
//     posU = (AnimPosition, AnimX)   posV  = (AnimPosition, AnimY)
//     rot  = (AnimRotation, AnimX)   -> GRADOS, alrededor del head del hueso
//     sclX = (AnimScale,    AnimX)   sclY  = (AnimScale,    AnimY)
//  El componente Z no existe en 2D: no se escribe ni se muestra.
//
//  CONVIVENCIA con la capa uv horneada (retrocompat, ver Armature2DEvaluate): un armature SIN
//  clips propios (Armature2D::anims vacio) se comporta EXACTAMENTE como antes (pose estatica +
//  capa uv de la vertex anim).
// ============================================================================

// tracks de UN hueso 2D: las curvas de posicion (U/V), rotacion y escala (X/Y).
class Bone2DTrack {
public:
    int bone;                              // indice en los huesos del armature DUEÑO del clip (-1 = sin asignar)
    std::vector<AnimProperty> Properties;
    Bone2DTrack() : bone(-1) {}
    // devuelve (creando si falta) la CURVA de un (propiedad, componente). SOLO para INSERTAR
    // keyframes: al evaluar usar PropertyFind (esta MUTA el clip).
    AnimProperty& PropertyOf(int property, int component);
    // la curva de un (propiedad, componente) SIN crearla: NULL si el hueso no anima ese canal.
    const AnimProperty* PropertyFind(int property, int component) const;
};

// un CLIP de animacion del armature 2D (equivalente de SkeletalAnimation)
class Armature2DAnimation {
public:
    std::string name;
    int fps;
    int startFrame;
    int endFrame;
    std::vector<Bone2DTrack> tracks;
    Armature2DAnimation(const std::string& n = "Anim2D") : name(n), fps(30), startFrame(1), endFrame(0) {}
    Bone2DTrack& TrackOf(int bone);        // devuelve (creando si falta) el track de un hueso
};

class Mesh;      // objects/Mesh.h (los clips viven en Armature2D::anims, uno por rig)
struct Armature2D; // objects/Mesh.h (UN rig 2D: sus huesos + SUS clips + su activo)

// ---- gestion de clips (mismo patron que CreateAnimation/DeleteActiveAnimation del 3D) ----
void Arm2DCreateAnimation(Mesh* m);
// nombre libre de CLIP dentro del armature 2D activo ('excepto' = indice que conserva el suyo)
std::string Arm2DAnimFreeName(Mesh* m, const std::string& base, int except);          // crea un clip vacio (nombre unico) y lo deja activo
// OJO (EDITOR): CORRE LOS INDICES de la lista de clips y los destinos de undo de un rename van
// por (malla, armature, INDICE) -> desde el editor va por UndoDeleteClip2D (main/undo/Undo.h),
// que remapea los destinos ya capturados. Cruda aca porque el Core compila para el runtime.
void Arm2DDeleteActiveAnimation(Mesh* m);    // borra el clip activo (puede quedar 0)
void Arm2DDuplicateActiveAnimation(Mesh* m); // duplica el clip activo y lo deja activo
Armature2DAnimation* Arm2DActiveClip(Mesh* m); // el clip activo (del armature ACTIVO), o NULL

// ---- gestion del ARMATURE ACTIVO de la malla (Mesh::armature2dActive) ----
// UNICA puerta para cambiar de rig 2D activo. NO alcanza con escribir el indice: el timeline en
// kind 4 muestra el clip del armature ACTIVO (Arm2DActiveClip lo sigue) y su rango/fps viven en
// ESE clip, asi que cambiar de rig sin recargar dejaba Start/End/FPS del clip ANTERIOR -> el Play
// usaba el rango equivocado y tocar el campo "End" ESCRIBIA ese valor en el clip del rig nuevo.
void Arm2DSetActive(Mesh* m, int idx);
// re-sincroniza Start/End/FPS del timeline con el clip activo del armature activo de 'm'. Es lo
// que llama Arm2DSetActive; se expone para los caminos que cambian el activo por aritmetica (el
// undo del alta/baja de armatures, que reacomoda el indice) en vez de elegirlo.
void Arm2DSyncRange(Mesh* m);

// CLON PROFUNDO de un armature 2D (huesos + clips). Armature2D es NO COPIABLE a proposito (es
// dueño de los punteros de Armature2D::anims), asi que duplicar una malla rigueada NO puede usar
// el copy-ctor: pasa por aca. Devuelve NULL si src es NULL.
Armature2D* Arm2DClone(const Armature2D* src);

// ---- evaluacion ----
// escribe poseTU/poseTV/poseRot/poseSX/poseSY de cada hueso desde las curvas del clip activo
// (el hueso sin curva queda en IDENTIDAD, que es el rol que cumple la rest en el 3D) y aplica
// el skinning 2D (Mesh::Armature2DApply). Con cache lastFrame/lastAnim/dirty como el 3D.
// NO MUTA el clip: lee las curvas con PropertyFind (con PropertyOf, mover el playhead creaba
// hasta 5 AnimProperty vacias por hueso y despues se serializaban).
// GATING: lee la curva si esta malla es la animacion ACTIVA del timeline (ActiveAnimKind==4 y
// ActiveAnimMesh==m) o si estamos en MODO JUEGO (ActiveAnimKind==2: no hay "animacion activa",
// TODA malla reproduce su clip). Si no, la pose queda como esta (posar a mano sigue andando).
void Armature2DEvaluate(Mesh* m, int frame);
// MODO JUEGO: recorre la escena (SceneCollection) y evalua el clip 2D de TODA malla que tenga
// clips. Es lo que hace que los rigs 2D se muevan con el Play / en el juego compilado (el kind 4
// solo mueve la malla que estas editando en el timeline).
void Armature2DEvaluateScene(int frame);

// ---- insercion de keyframes ----
// Insert Keyframe de la POSE 2D: guarda la pose de los huesos SELECCIONADOS (mas el activo) en
// CurrentFrame. Crea el clip si no hay ninguno (igual que InsertKeyframeSkeleton: insertar un
// keyframe NUNCA es un error por "no elegiste una animacion"). 'canales' = mascara KfCanal*.
void InsertKeyframeArm2D(Mesh* m, int channels = 0);
// AUTO KEY por canal (mismo trio que el 3D): Prep una vez, AutoKeyBone2D por hueso, Fin una vez.
bool AutoKeyArm2DPrep(Mesh* m);
int  AutoKeyBone2D(Mesh* m, int i, float tu0, float tv0, float rot0, float sx0, float sy0);
void AutoKeyArm2DEnd(Mesh* m);

#endif // ARMATURE2D_ANIMATION_H
