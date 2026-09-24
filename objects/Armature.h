#ifndef ARMATURE_H
#define ARMATURE_H

#include "objects/Objects.h"
#include "crossplatform.h"   // W3D_OVERRIDE (antes llegaba de rebote por Objects.h)
#include "math/Matrix4.h"
#include "math/Quaternion.h"
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
    // MUNDO del hueso en la pose actual, en el espacio del OBJETO armature (el mismo en que se dibujan
    // poseHead/poseTail: Y-up de escena, ya con NodeToYup en los rigs FBX). Lo llena EvaluarPoseEsqueleto
    // por frame; lo lee el constraint Child Of para colgar un objeto rigido (arma, cuchillo) de un hueso.
    Matrix4 poseWorld;
    bool    hasSkin;  // true si bind/skinA/skinInvBind estan listos
    bool    select;    // seleccionado en Pose Mode (click en viewport o en la lista de huesos)
    // EDIT MODE (editor): seleccion POR PUNTA. El hueso ENTERO seleccionado = select
    // (con ambas puntas prendidas); una punta sola = solo su flag. Una punta COMPARTIDA (head del
    // hijo conectado == tail del padre) se selecciona/mueve como UNA sola (lo sincroniza BoneEdit).
    bool    selHead;   // punta head seleccionada (Edit Mode)
    bool    selTail;   // punta tail seleccionada (Edit Mode)
    // CONECTADO al padre: con el flag prendido Y las puntas coincidentes (head == tail del padre)
    // la punta compartida es UNA sola (soldada). "Disconnect Bone" (Alt+P) lo apaga SIN tocar el
    // padre: el hueso sigue emparentado (linea punteada al separarlo) pero la punta queda libre.
    // Se guarda en el .w3d ("suelto": true solo cuando esta apagado con padre) -> los archivos
    // viejos (sin el campo) cargan con el comportamiento posicional de siempre.
    bool    conectado;
    W3dBone() : parent(-1), restS(1,1,1), poseS(1,1,1), rotOrder(0), hasRest(false), hasSkin(false), select(false), selHead(false), selTail(false), conectado(true) {
        bind.Identity(); clusterTransform.Identity(); skinA.Identity(); skinInvBind.Identity(); skinMatrix.Identity();
        poseWorld.Identity();
    }
};

class SkeletalAnimation; // animation/SkeletalAnimation.h (clips de animacion)

#include "animation/W3dCapaAnim.h"  // una capa del MIX de animaciones (Armature::capas)

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
        bool skinAutorado; // true = rig AUTORADO en el editor (PrepararSkinAutorado): el rest sale de head/tail
                           // (restT = head - head del padre, sin rotacion de rest), FK estandar Y-up (skinGltf=true)
                           // y bind = T(head). El TAIL es DATA del usuario -> la pose lo transforma con el hueso
                           // (no se deriva del primer hijo como en FBX). Un armature "2D" es uno autorado con Z=0.
        bool poseDirty;    // true = la pose fue editada a mano (posando) -> re-evaluar FK sin refrescar desde la curva
        int boneActivo;    // hueso seleccionado/activo en Pose Mode (-1 = ninguno); indice en bones[]
        int lastPoseFrame; // cache de pose: NO se recalcula la deformacion si el frame (y el clip) no cambiaron
        int lastPoseAnim;
        float figureScale; // (biped skinReconstruirFK) escala del FIGURE bakeada en el TL, con la que se NORMALIZO el
                           // tlNode en PrepararSkin (skeleton a escala de la malla). 1 = sin escala. Solo diagnostico.
        unsigned poseSerial; // sube cada vez que EvaluarPoseEsqueleto RECALCULA la pose (frame nuevo, clip nuevo, o posar
                           // a mano). La malla re-skinnea y re-sube su VBO cuando cambia -> se ve al toque aunque el FRAME
                           // no cambie (antes: cache por # de frame -> posar/elegir clip en el mismo frame no refrescaba).

        // ---- REPRODUCCION EN EL JUEGO (ActiveAnimKind 2): cada armature tiene SU cabezal ----
        // En el editor manda el timeline global (un solo armature activo). Jugando, cada esqueleto reproduce
        // su clip 'animActiva' con su propio reloj: lo avanza W3dArmaturesJuegoTick y lo manejan los scripts
        // (animClip / animFrame / animTermino). Runtime puro: no se guarda en el .w3d.
        // CAPAS DEL MIX (ver W3dCapaAnim). Vacio = el armature reproduce su clip activo como siempre.
        std::vector<W3dCapaAnim> capas;
        int capaActiva;        // la capa elegida en la lista del Mix (-1 = ninguna)
        unsigned mixFirma;     // cache: firma del estado de las capas de la ultima pose calculada
        // TRANSICION (el "hokan" de los juegos de consola): al cambiar de animacion la pose NUEVA se funde desde la
        // pose CONGELADA del momento del cambio durante transTotal segundos (animTransicion de lua). Runtime.
        float transRestante, transTotal;
        std::vector<Vector3> transT, transS;
        std::vector<Quaternion> transQ;
        float juegoFrame;  // frames transcurridos desde el inicio del clip (0 = startFrame)
        float juegoVel;    // multiplicador de velocidad (1 = el FrameRate del clip)
        bool  juegoLoop;   // true = vuelve a empezar al terminar; false = se queda en el ultimo frame
        bool  juegoTermino;// el clip sin loop llego al final (lo lee animTermino)

        Armature(Object* parent = NULL, Vector3 pos = Vector3(0, 0, 0))
            : Object(parent, "Armature", pos), animActiva(-1), skinUsaBind(false), skinReconstruirFK(false), skinGltf(false), skinAutorado(false), poseDirty(false), boneActivo(-1), lastPoseFrame(-999999), lastPoseAnim(-999), figureScale(1.0f), poseSerial(1),
              capaActiva(-1), mixFirma(0), transRestante(0.0f), transTotal(0.0f), juegoFrame(0.0f), juegoVel(1.0f), juegoLoop(true), juegoTermino(false) {}
        ~Armature() W3D_OVERRIDE; // libera los clips (animations)

        ObjectType getType() W3D_OVERRIDE { return ObjectType::armature; }
        void RenderObject() W3D_OVERRIDE; // huesos como lineas AZULES, siempre encima (sin z-test)
        // encuadre ('.'/frame selected): envuelve TODOS los huesos (poseHead/poseTail), no solo el origen
        Vector3 PuntoFoco() const W3D_OVERRIDE;
        float   RadioFoco() const W3D_OVERRIDE;
};

#endif // ARMATURE_H
