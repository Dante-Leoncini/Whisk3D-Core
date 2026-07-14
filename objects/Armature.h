#ifndef ARMATURE_H
#define ARMATURE_H

#include "objects/Objects.h"
#include "math/Matrix4.h"
#include <vector>
#include <string>

// ============================================================================
//  ARMATURE (esqueleto). Objeto INDEPENDIENTE que un mesh 3D usa como padre para
//  deformarse/animarse. En el CORE guarda lo MINIMO que necesita un esqueleto:
//  los huesos (nombre, jerarquia, y su transform de bind = rest pose). El editor
//  lo dibuja: cada hueso = una linea AZUL por encima de todo (ignora el z-buffer).
//  Los pesos por-vertice viven en la MALLA (vertex groups), no aca.
// ============================================================================

// un hueso: nombre + padre + su punta y cola en espacio LOCAL de la armature (rest pose).
struct W3dBone {
    std::string name;
    int parent;        // indice en bones[] (-1 = raiz)
    Vector3 head;      // origen del hueso (rest, espacio RAW del FBX)
    Vector3 tail;      // punta del hueso (para dibujarlo como palito)
    Vector3 poseHead;  // posicion ANIMADA (se calcula por FK al reproducir; = head en rest)
    Vector3 poseTail;
    // POSE actual EDITABLE (Pose Mode): T/R/S local del hueso en el frame actual. Se refresca desde la curva al
    // cambiar de frame; al posar (campos / G-R-S) se editan estos y se ve al toque, pero NO se guardan en la
    // animacion hasta hacer "Insert Keyframe" (i). Si no hay pose seteada, valen restT/R/S.
    Vector3 poseT, poseR, poseS;
    // transform LOCAL de rest (relativo al padre), para el FK. Viene de las props del Model FBX (Lcl Translation/
    // Rotation/Scaling + PreRotation + RotationOrder). Las curvas de animacion los REEMPLAZAN por canal.
    Vector3 restT;     // Lcl Translation (rest)
    Vector3 restR;     // Lcl Rotation (rest, euler grados)
    Vector3 restS;     // Lcl Scaling (rest)
    Vector3 preRot;    // PreRotation (euler grados)
    Vector3 postRot;   // PostRotation (euler grados)
    Vector3 rotPivot;  // RotationPivot   (los pivots/offsets del nodo FBX; Blender los aplica -> sin ellos el FK
    Vector3 rotOffset; // RotationOffset   no coincide con el TransformLink real y la malla queda corrida)
    Vector3 sclPivot;  // ScalingPivot
    Vector3 sclOffset; // ScalingOffset
    int     rotOrder;  // orden de rotacion euler (0 = XYZ, el mas comun)
    bool    hasRest;   // true si restT/R/S vienen del FBX (FK valido); false = armature manual (se dibuja bind)
    // SKINNING: matriz de skin del hueso en el frame actual = skinA * animWorldNode * skinInvBind. Deforma los
    // vertices (en espacio bind) a la pose animada. skinA/skinInvBind se precomputan al importar (ver PrepararSkin).
    Matrix4 bind;      // TransformLink (bind global del hueso, espacio escena Y-up del FBX)
    Matrix4 clusterTransform; // matriz 'Transform' del cluster FBX (transform de la geometria al bindear). En rigs
                              // normales (banana) es ~identidad; en otros (LISA) codifica el swap de ejes/orientacion.
    Matrix4 tlNode;    // TransformLink convertido al espacio NODO del FK (= NyInv * bind); bind real para el skinning
    Matrix4 skinA;     // = bind * inversa(restWorldNode)  (precomputada)
    Matrix4 skinInvBind; // = inversa(bind)                (precomputada)
    Matrix4 skinMatrix;  // resultado por-frame (identidad en rest)
    bool    hasSkin;   // true si bind/skinA/skinInvBind estan listos
    bool    select;    // seleccionado en Pose Mode (click en viewport o en la lista de huesos)
    W3dBone() : parent(-1), restS(1,1,1), poseS(1,1,1), rotOrder(0), hasRest(false), hasSkin(false), select(false) {
        bind.Identity(); clusterTransform.Identity(); skinA.Identity(); skinInvBind.Identity(); skinMatrix.Identity();
    }
};

class SkeletalAnimation; // animation/SkeletalAnimation.h (clips de animacion)

class Armature : public Object {
    public:
        std::vector<W3dBone> bones;
        // CLIPS de animacion de esqueleto (cada uno = un "take"/AnimStack de FBX). Se listan/crean/borran en el
        // panel de propiedades (pestania Animation). Las CURVAS (posicion/rotacion/escala por hueso) viven dentro.
        std::vector<SkeletalAnimation*> animations;
        int animActiva;    // clip activo (-1 = ninguno)
        bool skinUsaBind;  // true = usar el TransformLink real del FBX como bind (rigs estandar); false = FK-rest (LISA: TL en cero)
        bool skinReconstruirFK; // true = el Lcl-FK es DEGENERADO (restT ~0 -> se colapsa; biped 3ds Max nani/barney): el
                           // rest local se reconstruye del TransformLink (inv(tlNode_padre)*tlNode) y la animacion se
                           // aplica como DELTA del Lcl. Sin esto el FK sale ~100x chico y la malla se rompe al animar.
        bool skinGltf;     // true = esqueleto importado de glTF: FK ESTANDAR en Y-up con skinInvBind = inverseBindMatrix
                           // dada (sin NodeToYup, sin reconstruccion). El modelo de glTF es limpio -> skinMatrix = world*IBM.
        bool poseDirty;    // true = la pose fue editada a mano (posando) -> re-evaluar FK sin refrescar desde la curva
        int boneActivo;    // hueso seleccionado/activo en Pose Mode (-1 = ninguno); indice en bones[]
        int lastPoseFrame; // cache de pose: NO se recalcula la deformacion si el frame (y el clip) no cambiaron
        int lastPoseAnim;
        float figureScale; // (biped skinReconstruirFK) escala del FIGURE bakeada en el TL, con la que se NORMALIZO el
                           // tlNode en PrepararSkin (skeleton a escala de la malla). 1 = sin escala. Solo diagnostico.
        unsigned poseSerial; // sube cada vez que EvaluarPoseEsqueleto RECALCULA la pose (frame nuevo, clip nuevo, o posar
                           // a mano). La malla re-skinnea y re-sube su VBO cuando cambia -> se ve al toque aunque el FRAME
                           // no cambie (antes: cache por # de frame -> posar/elegir clip en el mismo frame no refrescaba).

        Armature(Object* parent = NULL, Vector3 pos = Vector3(0, 0, 0))
            : Object(parent, "Armature", pos), animActiva(-1), skinUsaBind(false), skinReconstruirFK(false), skinGltf(false), poseDirty(false), boneActivo(-1), lastPoseFrame(-999999), lastPoseAnim(-999), figureScale(1.0f), poseSerial(1) {}
        ~Armature() override; // libera los clips (animations)

        ObjectType getType() override { return ObjectType::armature; }
        void RenderObject() override; // huesos como lineas AZULES, siempre encima (sin z-test)
        // encuadre ('.'/frame selected): envuelve TODOS los huesos (poseHead/poseTail), no solo el origen
        Vector3 PuntoFoco() const override;
        float   RadioFoco() const override;
};

#endif // ARMATURE_H
