#include "animation/SkeletalAnimation.h"
#include "W3dNames.h"   // LA regla de nombres unicos (compartida)
#include <algorithm>   // sort (frames del motion trail)
#include "objects/Armature.h"
#include "animation/W3dAnimSet.h"   // clips COMPARTIDOS: copy-on-write antes de editar (W3dOwnArmatureAnims)
#include "objects/Mesh.h"
#include "math/Matrix4.h"
#include <cstdio>
#include <stdio.h>  // sprintf GLOBAL (C99): en Symbian/STLport <cstdio> puede dejarlo solo en std::
#include <cmath>
#include <cstring>  // memcpy (los floats como enteros: SkinOrder)
#include <vector>
#include <map>
#include <utility>

// ===== FK: evaluar la pose del esqueleto en un frame =====
static const float TL_PI = 3.14159265358979f;

// Fast inverse sqrt (Quake III, 0x5f3759df) + 1 iteracion de Newton (~0.17% error: sobra para normalizar normales).
// Reemplaza sqrtf + division en el hot-path del skinning. Union para el type-punning (C++03-safe; GCC lo soporta).
union W3zFloatInt { float f; int i; };
static inline float FastInvSqrt(float x){
    W3zFloatInt u; u.f = x;
    u.i = 0x5f3759df - (u.i >> 1);
    float y = u.f;
    y = y * (1.5f - 0.5f * x * y * y);
    return y;
}

// valor de una AnimProperty en 'frame' (interp LINEAL entre keyframes; keyframes ordenados por frame)
// (la evaluacion de curvas vive en Animation.cpp: AnimProperty::Eval / EvalPropVec. Cada componente X/Y/Z es su
//  propia curva, asi que se evaluan por separado.)

static Matrix4 MatTrans(const Vector3& t){ Matrix4 m; m.Identity(); m.m[12]=t.x; m.m[13]=t.y; m.m[14]=t.z; return m; }
static Matrix4 MatScale(const Vector3& s){ Matrix4 m; m.Identity(); m.m[0]=s.x; m.m[5]=s.y; m.m[10]=s.z; return m; }
static Matrix4 RotX(float a){ Matrix4 m; m.Identity(); float c=cosf(a),s=sinf(a); m.m[5]=c; m.m[6]=s; m.m[9]=-s; m.m[10]=c; return m; }
static Matrix4 RotY(float a){ Matrix4 m; m.Identity(); float c=cosf(a),s=sinf(a); m.m[0]=c; m.m[2]=-s; m.m[8]=s; m.m[10]=c; return m; }
static Matrix4 RotZ(float a){ Matrix4 m; m.Identity(); float c=cosf(a),s=sinf(a); m.m[0]=c; m.m[1]=s; m.m[4]=-s; m.m[5]=c; return m; }
// rotacion euler (grados) segun el RotationOrder de FBX. IMPORTANTE (verificado con el rig del banana): FBX compone
// la matriz en orden INVERSO a las letras -> eEulerXYZ = Rz*Ry*Rx (no Rx*Ry*Rz). order: 0=XYZ 1=XZY 2=YZX 3=YXZ 4=ZXY 5=ZYX.
static Matrix4 MatRotEuler(const Vector3& deg, int order){
    float rx=deg.x*TL_PI/180.0f, ry=deg.y*TL_PI/180.0f, rz=deg.z*TL_PI/180.0f;
    Matrix4 X=RotX(rx), Y=RotY(ry), Z=RotZ(rz);
    switch (order){
        case 1: return Y * Z * X; // XZY
        case 2: return X * Z * Y; // YZX
        case 3: return Z * X * Y; // YXZ
        case 4: return Y * X * Z; // ZXY
        case 5: return X * Y * Z; // ZYX
        default: return Z * Y * X; // XYZ (eEulerXYZ) -> Rz*Ry*Rx
    }
}
// matriz LOCAL de un hueso = T * PreRot * R(order) * S (convencion FBX, con pivots/offsets = 0)
static Matrix4 LocalMat(const Vector3& T, const Vector3& R, const Vector3& S, const Vector3& preR, int order){
    return MatTrans(T) * MatRotEuler(preR, 0) * MatRotEuler(R, order) * MatScale(S);
}
// matriz MUNDO de un hueso: sube por la cadena de padres (no asume orden topologico)
static Matrix4 WorldMat(const std::vector<W3dBone>& bones, const std::vector<Matrix4>& local, int i){
    Matrix4 acc = local[i]; int p = bones[i].parent; int guard = 0;
    while (p >= 0 && p < (int)bones.size() && guard++ < (int)bones.size()){ acc = local[p] * acc; p = bones[p].parent; }
    return acc;
}
static Matrix4 LocalOf(const W3dBone& b){
    return b.hasRest ? LocalMat(b.restT, b.restR, b.restS, b.preRot, b.rotOrder) : MatTrans(b.head);
}

// ============================================================================
//  RETARGET POR TAMANO (ver SkeletalAnimation.h). El TAMANO de un esqueleto es la diagonal de
//  la caja de las cabezas de sus huesos (FK): no depende de que eje es "arriba" (los rigs FBX
//  trabajan en espacio nodo Z-up, los glTF en Y-up) ni de la orientacion del personaje.
// ============================================================================
static float LocalSize(const std::vector<W3dBone>& bones, const std::vector<Matrix4>& local){
    const size_t N = bones.size();
    if (!N || local.size() != N) return 0.0f;
    Vector3 mn(0, 0, 0), mx(0, 0, 0);
    for (size_t i = 0; i < N; i++){
        const Vector3 h = WorldMat(bones, local, (int)i) * Vector3(0, 0, 0);
        if (i == 0) { mn = h; mx = h; continue; }
        if (h.x < mn.x) mn.x = h.x;
        if (h.y < mn.y) mn.y = h.y;
        if (h.z < mn.z) mn.z = h.z;
        if (h.x > mx.x) mx.x = h.x;
        if (h.y > mx.y) mx.y = h.y;
        if (h.z > mx.z) mx.z = h.z;
    }
    const Vector3 d = mx - mn;
    return sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
}
float W3dArmatureRestSize(const Armature* a){
    if (!a || a->bones.empty() || !a->bones[0].hasRest) return 0.0f;
    // FK RECONSTRUIDO (biped): las traslaciones de rest del Lcl vienen DEGENERADAS (el FK de rest colapsa); el
    // esqueleto real es el de los TransformLink (tlNode, ya a la escala del FK reconstruido, ver PrepareSkin).
    // Es la misma escala en la que viaja su root motion (poseT - restT de la raiz), asi que se mide ahi.
    if (a->skinRebuildFK){
        Vector3 mn(0, 0, 0), mx(0, 0, 0);
        for (size_t i = 0; i < a->bones.size(); i++){
            const Matrix4& t = a->bones[i].tlNode;
            const Vector3 h(t.m[12], t.m[13], t.m[14]);
            if (i == 0) { mn = h; mx = h; continue; }
            if (h.x < mn.x) mn.x = h.x;
            if (h.y < mn.y) mn.y = h.y;
            if (h.z < mn.z) mn.z = h.z;
            if (h.x > mx.x) mx.x = h.x;
            if (h.y > mx.y) mx.y = h.y;
            if (h.z > mx.z) mx.z = h.z;
        }
        const Vector3 d = mx - mn;
        return sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    }
    std::vector<Matrix4> local(a->bones.size());
    for (size_t i = 0; i < a->bones.size(); i++) local[i] = LocalOf(a->bones[i]);
    return LocalSize(a->bones, local);
}
// el tamano del esqueleto que GRABO el clip: el que trae (heightRest) o, si no se sabe, el de sus
// propias traslaciones en su primer frame sobre la jerarquia del destino (una pista sin traslacion
// usa la de reposo del destino: en un clip horneado, que tiene todas, es el esqueleto de origen).
// Un biped no se puede deducir asi (sus traslaciones del Lcl son degeneradas): sin heightRest, 0.
static float OriginSizeClip(const Armature* a, const SkeletalAnimation* c){
    if (c->heightRest > 0.0f) return c->heightRest;
    if (!a->bones[0].hasRest || a->skinRebuildFK) return 0.0f;
    const size_t N = a->bones.size();
    std::vector<Matrix4> local(N);
    for (size_t i = 0; i < N; i++){
        const W3dBone& b = a->bones[i];
        Vector3 T = b.restT, R = b.restR, S = b.restS;
        for (size_t t = 0; t < c->tracks.size(); t++) if (c->tracks[t].bone == (int)i){
            const BoneTrack& tr = c->tracks[t];
            T = EvalPropVec(tr.Properties, AnimPosition, c->startFrame, b.restT);
            R = EvalPropVec(tr.Properties, AnimRotation, c->startFrame, b.restR);
            S = EvalPropVec(tr.Properties, AnimScale,    c->startFrame, b.restS);
            break;
        }
        local[i] = LocalMat(T, R, S, b.preRot, b.rotOrder);
    }
    return LocalSize(a->bones, local);
}
// la escala del movimiento de las raices para 'c' en 'a'
static float ScaleOf(const Armature* a, const SkeletalAnimation* c){
    const float o = OriginSizeClip(a, c), d = W3dArmatureRestSize(a);
    return (o > 1e-6f && d > 1e-6f) ? d / o : 1.0f;
}
// idem cacheada por clip (se recalcula al cambiarlo): la del clip activo en el armature, la de cada capa en la capa
static float ScaleRetarget(Armature* a, const SkeletalAnimation* c){
    if (a->retargetClip == (const void*)c) return a->retargetScale;
    a->retargetScale = ScaleOf(a, c);
    a->retargetClip = (const void*)c;
    return a->retargetScale;
}
static float ScaleRetargetLayer(const Armature* a, W3dLayerAnim& cp, const SkeletalAnimation* c){
    if (cp.retClip == (const void*)c) return cp.retScale;
    cp.retScale = ScaleOf(a, c);
    cp.retClip = (const void*)c;
    return cp.retScale;
}
void W3dArmatureRetarget(Armature* a, int mode){
    if (!a) return;
    a->retarget = (mode == W3D_RETARGET_ROTATIONS) ? W3D_RETARGET_ROTATIONS : W3D_RETARGET_COMPLETE;
    a->retargetClip = NULL;                          // (la escala se recalcula con el proximo clip)
    for (size_t k = 0; k < a->layers.size(); k++) a->layers[k].retClip = NULL;   // (idem la de cada capa)
    a->lastPoseFrame = -999999; a->lastPoseAnim = -999;   // re-evaluar la pose desde las curvas
    a->mixSignature = 0;                                 // (con capas, la firma del Mix no ve el cambio de modo)
}
// el componente 'comp' de la traslacion de una pista, SI tiene keyframes (una raiz sin curva en ese eje no
// se mueve: su reposo no se escala)
static bool TranslationChannel(const BoneTrack& tr, int comp, float fr, bool continuous, int frame, float& out){
    for (size_t p = 0; p < tr.Properties.size(); p++){
        const AnimProperty& ap = tr.Properties[p];
        if (ap.Property != AnimPosition || ap.component != comp || ap.keyframes.empty()) continue;
        out = continuous ? ap.EvalF(fr, 0.0f) : ap.Eval(frame, 0.0f);
        return true;
    }
    return false;
}

// precomputa lo del skinning. Verificado con metricas headless (skincheck) sobre 2 rigs reales:
//   NORMAL (banana):    la geometria esta modelada en la pose FK-rest, en espacio NODO
//                       -> skinPre = inv(restWorldNode)  (dev por-vertice ~0.06: rigido perfecto)
//   SEGMENTADO (LISA, estilo PS1): TransformLink ~identidad Y el hueso vive lejos del origen -> cada PIEZA esta
//                       modelada en el espacio LOCAL de su hueso -> skinPre = identidad (skin = animNode directo)
// El TransformLink solo se usa como DETECTOR del caso segmentado (usarlo como bind dispersaba el banana: los TL
// de exports convertidos -Sketchfab- suelen venir inconsistentes con los Lcl). g_skinFormula: 1 = auto (default),
// 0 = FK-rest puro incluso en huesos segmentados (debug A/B del harness).
int g_skinFormula = 1;
// Ny = NodeToYup como matriz (x,y,z)->(x,z,-y). Convierte el TransformLink (espacio ESCENA Y-up del FBX) al
// espacio NODO (Z-up) en el que trabaja el FK. (column-major: m[col*4+fila]).
static Matrix4 MatrixNodeToYup(){
    Matrix4 Ny; Ny.Identity();
    Ny.m[0]=1; Ny.m[4]=0; Ny.m[8]=0;
    Ny.m[1]=0; Ny.m[5]=0; Ny.m[9]=1;
    Ny.m[2]=0; Ny.m[6]=-1; Ny.m[10]=0;
    return Ny;
}
void PrepareSkin(Armature* a){
    if (!a) return;
    size_t N = a->bones.size();
    // glTF: modelo LIMPIO. La inverseBindMatrix ya viene dada por el importador (skinInvBind) y el bind global tambien.
    // FK estandar en Y-up -> skinMatrix = worldFK * inverseBindMatrix. Sin NyInv, sin reconstruccion, sin biped.
    if (a->skinGltf){
        for (size_t i = 0; i < N; i++){ W3dBone& b = a->bones[i];
            b.hasSkin = b.hasRest;
            if (!b.hasSkin){ b.skinA.Identity(); b.skinInvBind.Identity(); b.skinMatrix.Identity(); b.tlNode.Identity(); continue; }
            b.tlNode = b.bind;        // bind global (rest), por si algo lo consulta
            b.skinA = b.skinInvBind;  // g_skinFormula 1 usa skinA; ambos = inverseBindMatrix
            b.skinMatrix.Identity();
        }
        a->skinRebuildFK = false; a->skinUsesBind = true;
        return;
    }
    std::vector<Matrix4> local(N);
    for (size_t i = 0; i < N; i++) local[i] = LocalOf(a->bones[i]);
    Matrix4 Ny = MatrixNodeToYup(), NyInv = Ny.Inverse();
    int withBind = 0, totalSkin = 0;
    // bounding de los origenes del FK-rest (Lcl) y del TransformLink -> detectar el FK DEGENERADO (biped 3ds Max)
    Vector3 fkMn(1e9f,1e9f,1e9f), fkMx(-1e9f,-1e9f,-1e9f), tlMn(1e9f,1e9f,1e9f), tlMx(-1e9f,-1e9f,-1e9f);
    for (size_t i = 0; i < N; i++){
        W3dBone& b = a->bones[i];
        b.hasSkin = b.hasRest; // sin transforms de rest del FBX no hay FK -> no se skinnea con este hueso
        if (!b.hasSkin){ b.skinA.Identity(); b.skinInvBind.Identity(); b.skinMatrix.Identity(); b.tlNode.Identity(); continue; }
        Matrix4 restWorldNode = WorldMat(a->bones, local, (int)i);
        // inversa del bind = FK-rest (fallback universal). El bind "real" del FBX es el TransformLink (b.bind),
        // convertido al espacio nodo: tlNode. Se usa cuando es valido (rigs estandar); LISA lo tiene en cero.
        b.skinInvBind = restWorldNode.Inverse();
        b.skinA = b.skinInvBind;
        // bind del hueso en espacio NODO: el TransformLink (B->scene Y-up) convertido a B->node Z-up = NyInv * bind.
        // (ANTES tenia un '* Ny' de mas -> conjugacion -> giro de 90°X extra por hueso: el "rotado 90° sobre si mismo").
        b.tlNode = NyInv * b.bind;
        // TransformLink DEGENERADO (LISA: solo escala, traslacion 0) -> el inverse-bind real es el ESTANDAR FBX
        // inverse(TransformLink)*Transform (la 'Transform' del cluster codifica el swap de ejes/orientacion), en el
        // frame nodo del motor: skinA = inv(NyInv*bind) * (NyInv*clusterTransform). Reduce EXACTO a worldFK*inv(TL)*Transform.
        // El banana NO entra aca (su TransformLink tiene traslacion real -> path skinUsesBind/tlNode intacto).
        float tlN = Vector3(b.bind.m[12], b.bind.m[13], b.bind.m[14]).Length();
        float rnN = Vector3(restWorldNode.m[12], restWorldNode.m[13], restWorldNode.m[14]).Length();
        if (tlN <= 1.0f && rnN > 5.0f){
            // El TransformLink de LISA trae la ESCALA del armature (gigante, ~100) BAKEADA, pero mi FK trabaja en
            // espacio armature-LOCAL (escala 1; el 0.01 del armature se aplica al DIBUJAR). Si no la quito, el skin
            // se encoge 1/escala (piezas al 1%). Normalizo las columnas del bind (scale->1): queda rigido y ubicado.
            Matrix4 bindNorm = b.bind;
            for (int c = 0; c < 3; c++){ float l = sqrtf(bindNorm.m[c*4]*bindNorm.m[c*4] + bindNorm.m[c*4+1]*bindNorm.m[c*4+1] + bindNorm.m[c*4+2]*bindNorm.m[c*4+2]);
                if (l > 1e-6f){ bindNorm.m[c*4]/=l; bindNorm.m[c*4+1]/=l; bindNorm.m[c*4+2]/=l; } }
            Matrix4 tlNorm = NyInv * bindNorm;
            // D = diag(-1,1,-1) = 180° sobre Y (eje profundidad en espacio nodo). El TransformLink degenerado + el
            // swap de ejes del clusterTransform, al pasar por NyInv, dejan la pieza rotada 180° sobre si misma (la
            // "cabeza con la boca para arriba"). Verificado vs Blender (ground truth): skinMatrix
            // del motor = skin_Blender * D en los 21 huesos (diff 0.0001). Se corrige multiplicando skinA por D.
            Matrix4 D; D.Identity(); D.m[0] = -1.0f; D.m[10] = -1.0f;
            b.skinA = (tlNorm.Inverse() * (NyInv * b.clusterTransform)) * D;
        }
        b.skinMatrix.Identity();
        totalSkin++;
        // TransformLink usable como BIND solo si es CONSISTENTE con el FK-rest (tlNode ~= restWorldNode): la malla se
        // bindeo en esa pose y el FK la puede alcanzar. Antes se contaba solo "TL con traslacion != 0", pero eso aceptaba
        // TransformLinks BASURA que explotan el skinning: barney/nani traen la ESCALA del armature bakeada en el TL
        // (traslacion ~3952 vs FK-rest ~2), chicken lo trae en OTRO espacio de ejes (swap). En esos casos el TL NO sirve
        // de bind -> FK-rest (que siempre da identidad al rest). La banana SI es consistente (TL ~= FK-rest) -> usa TL.
        Vector3 tlt(b.tlNode.m[12], b.tlNode.m[13], b.tlNode.m[14]);
        Vector3 rwt(restWorldNode.m[12], restWorldNode.m[13], restWorldNode.m[14]);
        if ((tlt - rwt).Length() < 0.25f * rwt.Length() + 1.0f) withBind++;
        if(rwt.x<fkMn.x)fkMn.x=rwt.x; if(rwt.y<fkMn.y)fkMn.y=rwt.y; if(rwt.z<fkMn.z)fkMn.z=rwt.z;
        if(rwt.x>fkMx.x)fkMx.x=rwt.x; if(rwt.y>fkMx.y)fkMx.y=rwt.y; if(rwt.z>fkMx.z)fkMx.z=rwt.z;
        if(tlt.x<tlMn.x)tlMn.x=tlt.x; if(tlt.y<tlMn.y)tlMn.y=tlt.y; if(tlt.z<tlMn.z)tlMn.z=tlt.z;
        if(tlt.x>tlMx.x)tlMx.x=tlt.x; if(tlt.y>tlMx.y)tlMx.y=tlt.y; if(tlt.z>tlMx.z)tlMx.z=tlt.z;
    }
    // FK DEGENERADO (biped 3ds Max nani/barney): el esqueleto del Lcl-FK es MUCHO mas chico que el del TransformLink
    // (las Lcl translations vienen ~0 -> el FK se colapsa ~100x). El TL tiene el esqueleto REAL -> reconstruir el rest
    // desde el TL y animar por delta (ver EvaluatePoseSkeleton). Requiere un TL VALIDO (spread > 1).
    float fkDiag = (fkMx - fkMn).Length(), tlDiag = (tlMx - tlMn).Length();
    a->skinRebuildFK = (totalSkin > 0 && tlDiag > 1.0f && fkDiag < 0.25f * tlDiag);
    // usar el TL como bind si la MAYORIA de los huesos lo tienen CONSISTENTE con el FK-rest (banana), O si el FK es
    // degenerado (biped): en ambos casos el bind real es el TransformLink (barney/nani antes caian a FK-rest roto).
    a->skinUsesBind = (totalSkin > 0 && withBind * 2 > totalSkin) || a->skinRebuildFK;
    // el TransformLink del biped 3ds Max trae la ESCALA DEL FIGURE bakeada (~85-110x) tanto en las COLUMNAS (escCols)
    // como en la POSICION (traslacion ~6100 vs malla ~74). Se quita la escala uniforme del figure DIVIDIENDO todo por
    // la longitud de columna: las columnas quedan ortonormales (scale->1) Y la posicion baja al espacio de la malla.
    // Sin esto el esqueleto queda GIGANTE (~85x) y, como los huesos pivotean en ~6100 pero la malla esta en ~74, la
    // rotacion animada dispara los vertices lejos = deformacion rota. Con esto huesos y malla comparten escala.
    if (a->skinRebuildFK) for (size_t i = 0; i < N; i++){ Matrix4& m = a->bones[i].tlNode;
        float ls[3];
        for (int c = 0; c < 3; c++) ls[c] = sqrtf(m.m[c*4]*m.m[c*4] + m.m[c*4+1]*m.m[c*4+1] + m.m[c*4+2]*m.m[c*4+2]);
        float s = (ls[0] + ls[1] + ls[2]) / 3.0f; if (s < 1e-8f) s = 1.0f; // escala uniforme del figure
        if (i == 0) a->figureScale = s; // guardar la escala del figure (uniforme) para meter la traslacion animada
        for (int c = 0; c < 3; c++) if (ls[c] > 1e-8f){ m.m[c*4]/=ls[c]; m.m[c*4+1]/=ls[c]; m.m[c*4+2]/=ls[c]; }
        m.m[12]/=s; m.m[13]/=s; m.m[14]/=s; // la POSICION tambien: sino el esqueleto queda 85x mas grande que la malla
    }
    // BIND = TransformLink real (no el FK-rest): la malla fue skinneada en la pose del TransformLink, que puede
    // diferir de la Lcl-rest. skinMatrix = world_FK * inv(tlNode) -> malla PEGADA al hueso con el FK correcto.
    if (a->skinUsesBind){
        for (size_t i = 0; i < N; i++) if (a->bones[i].hasSkin){
            Matrix4 inv = a->bones[i].tlNode.Inverse();
            a->bones[i].skinInvBind = inv;
            a->bones[i].skinA = inv;
        }
    }
}

// ===== RIG AUTORADO (huesos creados en el editor): rest + matrices de skin minimas desde head/tail =====
// Modelo: FK ESTANDAR en espacio LOCAL del armature (Y-up del motor, el mismo espacio de head/tail).
//   local(rest) = T(head - head_padre)  (sin rotacion/escala de rest, preRot=0, orden XYZ)
//   world(rest) = T(head)  ->  bind = T(head), skinInvBind = skinA = T(-head)
//   skinMatrix(pose) = world(pose) * T(-head): en rest es IDENTIDAD (la malla queda en bind) y al posar
//   el hueso los vertices con peso rotan/trasladan/escalan alrededor de su HEAD.
// Se reusa el camino skinGltf de EvaluatePoseSkeleton (FK Y-up sin NodeToYup) -> cero rama nueva en el hot path.
void PrepareAuthoredSkin(Armature* a){
    if (!a || a->bones.empty()) return;
    if (!a->skinAuthored){
        // guarda: un rig IMPORTADO (algun hueso con rest de FBX/glTF) no se toca; esto es solo para
        // armatures creados a mano en el editor (todos los huesos con hasRest=false la primera vez).
        for (size_t i = 0; i < a->bones.size(); i++) if (a->bones[i].hasRest) return;
    }
    size_t N = a->bones.size();
    for (size_t i = 0; i < N; i++){
        W3dBone& b = a->bones[i];
        // pose actual como DELTA de la rest vieja: re-preparar (tras mover un head en Edit Mode) conserva
        // lo posado. La primera vez restT=0 y poseT=0 -> delta 0 -> pose = rest nueva.
        Vector3 dT = b.poseT - b.restT;
        Vector3 headParent = (b.parent >= 0 && b.parent < (int)N) ? a->bones[b.parent].head : Vector3(0,0,0);
        b.restT = b.head - headParent;
        b.restR = Vector3(0,0,0);
        b.restS = Vector3(1,1,1);
        b.preRot = Vector3(0,0,0); b.postRot = Vector3(0,0,0);
        b.rotPivot = Vector3(0,0,0); b.rotOffset = Vector3(0,0,0);
        b.sclPivot = Vector3(0,0,0); b.sclOffset = Vector3(0,0,0);
        b.rotOrder = 0;
        b.hasRest = true;
        b.poseT = b.restT + dT;
        if (!a->skinAuthored){ b.poseR = b.restR; b.poseS = b.restS; } // 1ra vez: pose limpia
        // bind global = T(head) (rest sin rotacion). clusterTransform queda identidad.
        b.bind.Identity(); b.bind.m[12] = b.head.x; b.bind.m[13] = b.head.y; b.bind.m[14] = b.head.z;
        b.tlNode = b.bind;
        Matrix4 inv; inv.Identity(); inv.m[12] = -b.head.x; inv.m[13] = -b.head.y; inv.m[14] = -b.head.z;
        b.skinInvBind = inv;
        b.skinA = inv;
        b.skinMatrix.Identity();
        b.hasSkin = true;
    }
    a->skinGltf = true;        // FK estandar Y-up (sin NodeToYup) + skinMatrix = world * skinA
    a->skinAuthored = true;
    a->skinUsesBind = true;
    a->skinRebuildFK = false;
    a->lastPoseFrame = -999999; a->lastPoseAnim = -999; a->poseDirty = false;
    a->poseSerial++;           // las mallas skinneadas re-deforman con el rest nuevo
}

bool g_skelAnimPreview = true; // ON: FK de la animacion (para rigs FBX; los armatures manuales se dibujan en bind)

// los transforms LOCALES del FBX vienen en espacio Z-up (nodo), pero el resto de la armature (bind head/tail,
// malla) esta en Y-up. Se convierte la salida del FK: (x,y,z) Z-up -> (x, z, -y) Y-up.
static Vector3 NodeToYup(const Vector3& v){ return Vector3(v.x, v.z, -v.y); }

// ===== helpers PUBLICOS para el transform interactivo de huesos (Pose Mode, main/ViewPorts/LayoutInput.cpp) =====
Matrix4 SkelNodeToYupMat(){ return MatrixNodeToYup(); }
Matrix4 SkelMatRotEuler(const Vector3& deg, int order){ return MatRotEuler(deg, order); }
// world (rot+trans) del hueso 'bone' en espacio NODO, con la POSE actual (poseT/R/S). Identidad si no hay hueso/padre.
Matrix4 SkelBoneWorldNode(Armature* a, int bone){
    Matrix4 I; I.Identity();
    if (!a || bone < 0 || bone >= (int)a->bones.size()) return I;
    size_t N = a->bones.size();
    std::vector<Matrix4> local(N);
    for (size_t i = 0; i < N; i++){ W3dBone& b = a->bones[i]; local[i] = LocalMat(b.poseT, b.poseR, b.poseS, b.preRot, b.rotOrder); }
    return WorldMat(a->bones, local, bone);
}
// extrae el euler (grados) de la parte rotacional de M en el orden FBX. Inversa exacta de MatRotEuler para order 0
// (Rz*Ry*Rx); para otros ordenes es una aproximacion XYZ (suficiente para el trackball de pose).
Vector3 SkelMatrixToEulerFBX(const Matrix4& M, int /*order*/){
    const float* m = M.m; // column-major m[col*4+fila]; R[fila][col] = m[col*4+fila]
    float sy = -m[2]; if (sy > 1.0f) sy = 1.0f; if (sy < -1.0f) sy = -1.0f; // R[2][0] = -sin(y)
    float y = asinf(sy), cy = cosf(y), x, z;
    if (fabsf(cy) > 1e-4f){ x = atan2f(m[6], m[10]); z = atan2f(m[1], m[0]); } // R[2][1]/R[2][2] , R[1][0]/R[0][0]
    else { x = atan2f(-m[9], m[5]); z = 0.0f; }                                // gimbal lock: fija z=0
    const float R2D = 180.0f / 3.14159265358979f;
    return Vector3(x * R2D, y * R2D, z * R2D);
}

// APLICAR TRANSFORM (Ctrl+A) sobre un ARMATURE: hornea la matriz B (= inv(M_arm_reseteado) * M_arm) en la REST de los
// huesos de modo que el FK mundial quede world_FK' = B * world_FK para TODOS. Como skinA/skinInvBind NO se tocan ->
// skinMatrix' = world_FK' * skinA = B * skinMatrix, y las mallas skinneadas HIJAS del armature (deformadas en espacio
// nodo, con el transform del objeto aplicado al dibujar) quedan IDENTICAS al resetear ese transform del objeto.
//   - Bake en la LOCAL rest de cada ROOT (parent<0): world[i] = local[root]*...*local[i] -> reemplazar local[root] por
//     B*local[root] escala/rota/traslada TODO el arbol. Se descompone a T/R/S (orden XYZ, preRot plegado en restR).
//   - Biped (skinRebuildFK, FK desde tlNode y NO desde restT/R/S): se pre-multiplican TODOS los tlNode por B (los
//     ratios inv(padre)*hijo quedan iguales, la raiz queda B*tlNode) -> mismo world' = B*world sin tocar la rest/delta.
// B ∈ {escala, rotacion, traslacion, o el M_arm completo} mantiene B*local como T*R*S limpio (sin shear) para rigs
// limpios. Ejemplo tipico (escala uniforme s): restT*=s, restS*=s en los roots (la posicion y escala de los huesos, no
// la rotacion) -> es lo que se espera al normalizar un rig que venia 100x.
void BakeTransformInBones(Armature* a, const Matrix4& B){
    if (!a) return;
    bool biped = a->skinRebuildFK;
    for (size_t i = 0; i < a->bones.size(); i++){
        W3dBone& b = a->bones[i];
        if (biped){
            b.tlNode = B * b.tlNode; // FK del biped sale del tlNode -> hornear en todos (raiz=B*tl, ratios hijos intactos)
        } else if (b.parent < 0){
            // ROOT de un rig normal/gltf: newLocal = B * local(rest); descomponer a T/R/S (XYZ, sin preRot)
            Matrix4 nl = B * LocalMat(b.restT, b.restR, b.restS, b.preRot, b.rotOrder);
            Vector3 cx(nl.m[0], nl.m[1], nl.m[2]);   // columnas (rotacion*escala)
            Vector3 cy(nl.m[4], nl.m[5], nl.m[6]);
            Vector3 cz(nl.m[8], nl.m[9], nl.m[10]);
            float sx = sqrtf(cx.x*cx.x + cx.y*cx.y + cx.z*cx.z);
            float sy = sqrtf(cy.x*cy.x + cy.y*cy.y + cy.z*cy.z);
            float sz = sqrtf(cz.x*cz.x + cz.y*cz.y + cz.z*cz.z);
            Matrix4 R; R.Identity();
            if (sx > 1e-8f){ R.m[0]=cx.x/sx; R.m[1]=cx.y/sx; R.m[2]=cx.z/sx; }
            if (sy > 1e-8f){ R.m[4]=cy.x/sy; R.m[5]=cy.y/sy; R.m[6]=cy.z/sy; }
            if (sz > 1e-8f){ R.m[8]=cz.x/sz; R.m[9]=cz.y/sz; R.m[10]=cz.z/sz; }
            b.restT = Vector3(nl.m[12], nl.m[13], nl.m[14]);
            b.restS = Vector3(sx, sy, sz);
            b.restR = SkelMatrixToEulerFBX(R, 0); // orden XYZ (inversa exacta de MatRotEuler order 0)
            b.preRot = Vector3(0, 0, 0);
            b.rotOrder = 0;
        }
        // head/tail (espacio nodo) para el foco/display fallback; en rigs FK poseHead se recalcula igual
        b.head = B * b.head;
        b.tail = B * b.tail;
        b.poseT = b.restT; b.poseR = b.restR; b.poseS = b.restS; // ver en rest tras aplicar
    }
    a->lastPoseFrame = -999999; a->lastPoseAnim = -999; a->poseDirty = false; a->poseSerial++; // re-evaluar FK + re-skinnear
}

// ============================================================================
//  EL MIX DE ANIMACIONES (capas). Estado global del modo Mix del editor + la evaluacion por hueso.
// ============================================================================
bool  g_animMix = false;
float g_mixStart = 1.0f, g_mixEnd = 250.0f;
std::vector<W3dLayerAnim> g_mixScenes;
int   g_mixActiveScene = -1;

int W3dLayerClip(const Armature* a, W3dLayerAnim& c){
    if (!a) return -1;
    const int n = (int)a->animations.size();
    if (c.clipCache >= 0 && c.clipCache < n && a->animations[c.clipCache] && a->animations[c.clipCache]->name == c.anim) return c.clipCache;
    c.clipCache = W3dArmatureClipByName(a, c.anim);
    return c.clipCache;
}
float W3dLayerFrameEditor(const W3dLayerAnim& c, int frameMix, int clipStart, int clipEnd){
    float length = (float)(clipEnd - clipStart + 1); if (length < 1.0f) length = 1.0f;
    float t = ((float)frameMix - g_mixStart - c.from) * c.vel;
    if (c.loop) { t = fmodf(t, length); if (t < 0.0f) t += length; }
    else { if (t < 0.0f) t = 0.0f; if (t > length - 1.0f) t = length - 1.0f; }
    return (float)clipStart + t;
}
static Quaternion QuatOfEuler(const Vector3& e, int order){ return Quaternion::FromMatrix(SkelMatRotEuler(e, order)); }
static Vector3 EulerOfQuat(const Quaternion& q, int order){ Matrix4 M; q.ToMatrix(M.m); return SkelMatrixToEulerFBX(M, order); }
// la pose de TODAS las capas visibles, mezcladas en orden, en poseT/R/S de cada hueso
static void MixLayers(Armature* a, int frameMix){
    const size_t N = a->bones.size();
    static std::vector<Vector3> T, S; static std::vector<Quaternion> Q, Qrest; static std::vector<int> trk; static std::vector<char> mask;
    T.resize(N); S.resize(N); Q.resize(N); Qrest.resize(N); trk.resize(N); mask.resize(N);
    for (size_t i = 0; i < N; i++){ const W3dBone& b = a->bones[i];
        T[i] = b.restT; S[i] = b.restS; Qrest[i] = QuatOfEuler(b.restR, b.rotOrder); Q[i] = Qrest[i]; }
    for (size_t k = 0; k < a->layers.size(); k++){
        W3dLayerAnim& c = a->layers[k];
        if (!c.visible || c.influence <= 0.0f) continue;
        int ci = W3dLayerClip(a, c); if (ci < 0) continue;
        SkeletalAnimation* clip = a->animations[ci]; if (!clip) continue;
        float w = c.influence * 0.01f; if (c.mode == 0 && w > 1.0f) w = 1.0f; if (w > 2.0f) w = 2.0f;
        int f = (ActiveAnimKind == 2) ? clip->startFrame + (int)c.gameFrame
                                      : (int)W3dLayerFrameEditor(c, frameMix, clip->startFrame, clip->endFrame);
        // mascara por hueso: el hueso raiz y sus descendientes
        int root = -1;
        if (!c.bone.empty()) for (size_t i = 0; i < N; i++) if (a->bones[i].name == c.bone) { root = (int)i; break; }
        for (size_t i = 0; i < N; i++){
            if (root < 0) { mask[i] = 1; continue; }
            int p = (int)i; mask[i] = 0;
            while (p >= 0) { if (p == root) { mask[i] = 1; break; } p = a->bones[p].parent; }
        }
        for (size_t i = 0; i < N; i++) trk[i] = -1;
        for (size_t t = 0; t < clip->tracks.size(); t++){ int bo = clip->tracks[t].bone; if (bo >= 0 && bo < (int)N) trk[bo] = (int)t; }
        // RETARGET "rotaciones" (el del armature, W3dArmatureRetarget): la capa aporta solo las rotaciones de su
        // clip; cada hueso con la traslacion y la escala de su reposo, salvo las raices (su movimiento escalado)
        const bool onlyRot = (a->retarget == W3D_RETARGET_ROTATIONS);
        const float rootScale = onlyRot ? ScaleRetargetLayer(a, c, clip) : 1.0f;
        for (size_t i = 0; i < N; i++){
            if (!mask[i]) continue;
            const W3dBone& b = a->bones[i];
            Vector3 Tl = b.restT, Rl = b.restR, Sl = b.restS;
            if (trk[i] >= 0){ BoneTrack& tr = clip->tracks[trk[i]];
                Tl = EvalPropVec(tr.Properties, AnimPosition, f, b.restT);
                Rl = EvalPropVec(tr.Properties, AnimRotation, f, b.restR);
                Sl = EvalPropVec(tr.Properties, AnimScale,    f, b.restS);
                if (onlyRot){
                    Tl = b.restT; Sl = b.restS;
                    float v = 0.0f;
                    if (b.parent < 0 || b.parent >= (int)N){
                        if (TranslationChannel(tr, AnimX, (float)f, false, f, v)) Tl.x = v * rootScale;
                        if (TranslationChannel(tr, AnimY, (float)f, false, f, v)) Tl.y = v * rootScale;
                        if (TranslationChannel(tr, AnimZ, (float)f, false, f, v)) Tl.z = v * rootScale;
                    }
                } }
            Quaternion Ql = (trk[i] >= 0) ? QuatOfEuler(Rl, b.rotOrder) : Qrest[i];
            if (c.mode == 0){
                T[i] = T[i] + (Tl - T[i]) * w;
                S[i] = S[i] + (Sl - S[i]) * w;
                Q[i] = (w >= 1.0f) ? Ql : Quaternion::Slerp(Q[i], Ql, w);
            } else {
                float sg = (c.mode == 2) ? -1.0f : 1.0f;
                T[i] = T[i] + (Tl - b.restT) * (w * sg);
                Quaternion d = Qrest[i].Inverted() * Ql;        // cuanto se aparta del reposo
                if (sg < 0.0f) d = d.Inverted();
                Q[i] = Q[i] * Quaternion::Slerp(Quaternion(), d, w);
                for (int e = 0; e < 3; e++){
                    float r0 = (e==0 ? b.restS.x : e==1 ? b.restS.y : b.restS.z), rl = (e==0 ? Sl.x : e==1 ? Sl.y : Sl.z);
                    float m = (r0 != 0.0f) ? rl / r0 : 1.0f; m = 1.0f + (m - 1.0f) * w * sg;
                    if (e==0) S[i].x *= m; else if (e==1) S[i].y *= m; else S[i].z *= m;
                }
            }
        }
    }
    // TRANSICION: fundir desde la pose congelada (w = lo que falta / el total; 1 al cambiar, 0 al terminar)
    if (a->remainingTrans > 0.0f && a->transTotal > 0.0f && a->transT.size() == N){
        float w = a->remainingTrans / a->transTotal; if (w > 1.0f) w = 1.0f;
        for (size_t i = 0; i < N; i++){
            T[i] = T[i] + (a->transT[i] - T[i]) * w;
            S[i] = S[i] + (a->transS[i] - S[i]) * w;
            Q[i] = Quaternion::Slerp(Q[i], a->transQ[i], w);
        }
    }
    for (size_t i = 0; i < N; i++){ W3dBone& b = a->bones[i];
        b.poseT = T[i]; b.poseS = S[i]; b.poseR = EulerOfQuat(Q[i], b.rotOrder); }
}

// congela la pose ACTUAL (la ultima calculada) para fundir desde ella durante 'segundos'
void W3dArmatureTransition(Armature* a, float seconds){
    if (!a) return;
    const size_t N = a->bones.size();
    a->transT.resize(N); a->transS.resize(N); a->transQ.resize(N);
    for (size_t i = 0; i < N; i++){ const W3dBone& b = a->bones[i];
        a->transT[i] = b.poseT; a->transS[i] = b.poseS; a->transQ[i] = QuatOfEuler(b.poseR, b.rotOrder); }
    a->transTotal = seconds > 0.0f ? seconds : 0.0f;
    a->remainingTrans = a->transTotal;
}
// firma del estado de las capas (para no recalcular la pose si nada cambio)
static unsigned SignatureLayers(Armature* a, int frameMix){
    unsigned h = 2166136261u;
    for (size_t k = 0; k < a->layers.size(); k++){ const W3dLayerAnim& c = a->layers[k];
        int ci = W3dLayerClip(a, a->layers[k]);
        int f = (ActiveAnimKind == 2) ? (int)c.gameFrame : (ci >= 0 ? (int)W3dLayerFrameEditor(c, frameMix, a->animations[ci]->startFrame, a->animations[ci]->endFrame) : 0);
        unsigned v[6] = { (unsigned)(ci + 7), (unsigned)f, (unsigned)(c.influence * 100.0f), (unsigned)c.mode, (unsigned)c.visible, (unsigned)c.bone.size() };
        for (int q = 0; q < 6; q++){ h ^= v[q]; h *= 16777619u; } }
    h ^= (unsigned)(a->remainingTrans * 10000.0f); h *= 16777619u;   // la transicion cambia la pose aunque no las capas
    return h;
}
static bool UsesLayers(const Armature* a){ return !a->layers.empty() && (g_animMix || ActiveAnimKind == 2); }

// ============================================================================
//  EL RELOJ DE LOS ESQUELETOS EN EL JUEGO: avanza el cabezal de cada armature por su clip activo
//  (FrameRate del clip * gameVel), con loop o parando en el ultimo frame (gameEnded).
// ============================================================================
static void W3dArmGameTick(Armature* a, float dt){
    {
        if (a->remainingTrans > 0.0f) { a->remainingTrans -= dt; if (a->remainingTrans < 0.0f) a->remainingTrans = 0.0f; }
        // CAPAS: cada una avanza con su propio cabezal
        for (size_t k = 0; k < a->layers.size(); k++) {
            W3dLayerAnim& cp = a->layers[k];
            int ci = W3dLayerClip(a, cp); if (ci < 0) continue;
            SkeletalAnimation* c = a->animations[ci];
            const float length = (float)(c->endFrame - c->startFrame);
            if (!cp.gameEnded) cp.gameFrame += dt * (float)c->FrameRate * cp.vel;
            if (length <= 0.0f || cp.gameFrame != cp.gameFrame) cp.gameFrame = 0.0f;
            else if (cp.loop) cp.gameFrame = W3dCycleWrap(cp.gameFrame, length + 1.0f);   // (sin colgarse: ver Animation.h)
            else if (cp.gameFrame >= length) { cp.gameFrame = length; cp.gameEnded = true; }
        }
        if (a->activeAnim >= 0 && a->activeAnim < (int)a->animations.size()) {
            SkeletalAnimation* c = a->animations[a->activeAnim];
            const float length = (float)(c->endFrame - c->startFrame);   // el ultimo frame es startFrame + largo
            if (!a->gameEnded) a->gameFrame += dt * (float)c->FrameRate * a->gameVel;
            if (length <= 0.0f || a->gameFrame != a->gameFrame) a->gameFrame = 0.0f;
            else if (a->gameLoop) a->gameFrame = W3dCycleWrap(a->gameFrame, length + 1.0f);
            else if (a->gameFrame >= length) { a->gameFrame = length; a->gameEnded = true; }
        }
    }
}
// (los esqueletos de la escena salen de W3dLiveOfType: recorrer el arbol entero por cuadro eran miles de nodos)
void W3dArmaturesGameTick(float dt){
    if (SceneCollection) {
        const std::vector<Object*>& arms = W3dLiveOfType(ObjectType::armature);
        for (size_t i = 0; i < arms.size(); i++)
            if (W3dAttachedTo(arms[i], SceneCollection)) W3dArmGameTick((Armature*)arms[i], dt);
    }
    W3dMixScenesTick(dt);
}

// clip por nombre (o "#N" / indice) desde el juego: reinicia el cabezal salvo que ya sea el mismo clip
bool W3dArmaturePlayClip(Armature* a, int clip, bool loop, bool restart){
    if (!a || clip < 0 || clip >= (int)a->animations.size()) return false;
    if (clip != a->activeAnim || restart) { a->gameFrame = 0.0f; a->gameEnded = false; }
    a->activeAnim = clip;
    a->gameLoop = loop;
    if (loop) a->gameEnded = false;
    return true;
}
int W3dArmatureClipByName(const Armature* a, const std::string& name){
    if (!a) return -1;
    for (size_t i = 0; i < a->animations.size(); i++) if (a->animations[i] && a->animations[i]->name == name) return (int)i;
    return -1;
}

float g_poseFraction = 0.0f;   // (ver SkeletalAnimation.h)
void EvaluatePoseSkeleton(Armature* a, int frame){
    if (!a) return;
    // GATING de playback: este armature reproduce su clip SOLO si es la animacion ACTIVA (ActiveAnimKind 1 + este
    // armature). Con una ESCENA activa (kind 0) o con OTRO armature activo, se muestra en REST -> "solo se mueve lo
    // seleccionado", no todos a la vez. (Posar a mano igual anda: poseDirty corta antes de leer la curva.)
    int animPlay = (ActiveAnimKind == 1 && ActiveAnimArm == a &&
                    a->activeAnim >= 0 && a->activeAnim < (int)a->animations.size()) ? a->activeAnim : -1;
    // JUGANDO (kind 2): TODOS los esqueletos reproducen su clip activo, cada uno con SU cabezal (gameFrame, lo
    // avanza W3dArmaturesGameTick) -- no el frame global, que no sabe de clips ni de loops.
    // frameF: jugando, el frame CONTINUO del cabezal. La pose se INTERPOLA entre dos frames del clip: con el frame
    // truncado un clip de 30 fps dibujado a 45-55 fps avanzaba 1,1,0,1,0... (micro saltos en las cinematicas).
    float frameF = (float)frame;
    bool continuous = false;
    // UNA ESCENA con TIRAS de esqueleto (SceneAnimation::esqueletos): el clip que le toca a este armature, en el frame
    // de la escena (asi el timeline de la escena mueve a todos, como una pelicula)
    if (ActiveAnimKind == 0 && animPlay < 0 && ActiveSceneAnim >= 0 && ActiveSceneAnim < (int)SceneAnimations.size()) {
        const SceneAnimation* e = SceneAnimations[ActiveSceneAnim];
        if (e && !e->isClip)
            for (size_t t = 0; t < e->skeletons.size(); t++) {
                if (e->skeletons[t].armature != a->name) continue;
                const int c = W3dArmatureClipByName(a, e->skeletons[t].clip);
                if (c < 0) break;
                const SkeletalAnimation* cl = a->animations[c];
                int f = cl->startFrame + (frame - e->skeletons[t].frame);
                if (f < cl->startFrame) f = cl->startFrame;
                if (f > cl->endFrame) f = cl->endFrame;
                animPlay = c; frame = f; frameF = (float)f;
                break;
            }
    }
    // (el desenfoque trazado: el instante entre este cuadro y el siguiente)
    if (g_poseFraction > 0.0f && animPlay >= 0 && animPlay < (int)a->animations.size()) {
        frameF = (float)frame + g_poseFraction;
        if (frameF > (float)a->animations[animPlay]->endFrame) frameF = (float)a->animations[animPlay]->endFrame;
        continuous = true;
    }
    if (ActiveAnimKind == 2 && a->activeAnim >= 0 && a->activeAnim < (int)a->animations.size()) {
        animPlay = a->activeAnim;
        frame = a->animations[animPlay]->startFrame + (int)a->gameFrame;
        frameF = (float)a->animations[animPlay]->startFrame + a->gameFrame;
        continuous = true;
    }
    // CACHE: si el frame y el clip efectivo no cambiaron Y la pose no fue editada a mano, ya esta calculada (no
    // recalcular a 60fps). poseDirty (posando) fuerza re-FK sin refrescar poseT/R/S desde la curva.
    bool frameChanged = (a->lastPoseFrame != frame || a->lastPoseAnim != animPlay ||
                         (continuous && a->lastPoseFrameF != frameF));
    // MIX: la pose sale de las capas; el cache es la FIRMA del estado de todas (frames, influencias, ojos...)
    const bool mix = UsesLayers(a);
    const int frameMix = frame;
    if (mix) {
        unsigned signature = SignatureLayers(a, frameMix);
        frameChanged = (signature != a->mixSignature) || a->lastPoseAnim != -777;
        a->mixSignature = signature;
        animPlay = -777;
    }
    if (!frameChanged && !a->poseDirty) return;
    a->lastPoseFrame = frame; a->lastPoseAnim = animPlay; a->lastPoseFrameF = frameF;
    a->poseDirty = false;
    a->poseSerial++; // la pose se RECALCULA -> las mallas skinneadas re-deforman/re-suben el VBO aunque el frame no cambie
    // por defecto: rest (poseHead/poseTail = head/tail bind)
    for (size_t i = 0; i < a->bones.size(); i++){ a->bones[i].poseHead = a->bones[i].head; a->bones[i].poseTail = a->bones[i].tail;
                                                  a->bones[i].poseWorld = a->bones[i].bind; } // Child Of: el bind (rest global)
    // FK solo para rigs FBX (con transforms de rest). Los armatures MANUALES (hasRest=false) se muestran en bind.
    bool fbxRig = !a->bones.empty() && a->bones[0].hasRest;
    if (!g_skelAnimPreview || !fbxRig) return;
    SkeletalAnimation* clip = (animPlay >= 0) ? a->animations[animPlay] : NULL;

    size_t N = a->bones.size();
    // SCRATCH persistente (reusa la capacidad entre frames -> sin 5 allocs de heap por frame; los loops de abajo
    // sobreescriben TODOS los elementos, asi que resize alcanza). Single-thread, no re-entrante -> static seguro.
    static std::vector<Matrix4> local, world;
    local.resize(N); world.resize(N);
    // Al CAMBIAR de frame se refresca la POSE (poseT/R/S) de cada hueso desde la curva (o rest). Posando NO se
    // refresca (poseDirty): se respeta lo que el usuario esta editando hasta que cambie el frame o inserte keyframe.
    // RETARGET "rotaciones" (W3dArmatureRetarget): solo las rotaciones del clip; cada hueso con su traslacion
    // y su escala de REPOSO (las proporciones del destino) salvo las raices, cuyo movimiento se escala por
    // tamano. Las capas del Mix lo aplican capa por capa (MixLayers). En el FK RECONSTRUIDO (biped) la
    // traslacion de un hueso entra como DELTA contra su rest (el root motion): con T = restT los hijos no se
    // corren y la raiz avanza su delta escalado; la escala del biped ya no se usaba (solo rotacion + delta).
    const bool onlyRot = clip && a->retarget == W3D_RETARGET_ROTATIONS;
    const float rootScale = onlyRot ? ScaleRetarget(a, clip) : 1.0f;
    if (frameChanged && mix) MixLayers(a, frameMix);
    else if (frameChanged) for (size_t i = 0; i < N; i++){
        W3dBone& b = a->bones[i];
        Vector3 T = b.restT, R = b.restR, S = b.restS;
        if (clip) for (size_t t = 0; t < clip->tracks.size(); t++) if (clip->tracks[t].bone == (int)i){
            BoneTrack& tr = clip->tracks[t];
            // cada componente (X/Y/Z) tiene SU curva -> se evaluan por separado; el que no tenga keyframes
            // propios queda en su valor de rest.
            if (continuous) {
                T = EvalPropVecF(tr.Properties, AnimPosition, frameF, b.restT);
                R = EvalPropVecF(tr.Properties, AnimRotation, frameF, b.restR);
                S = EvalPropVecF(tr.Properties, AnimScale,    frameF, b.restS);
            } else {
                T = EvalPropVec(tr.Properties, AnimPosition, frame, b.restT);
                R = EvalPropVec(tr.Properties, AnimRotation, frame, b.restR);
                S = EvalPropVec(tr.Properties, AnimScale,    frame, b.restS);
            }
            if (onlyRot) {
                const bool root = (b.parent < 0 || b.parent >= (int)N);
                S = b.restS;
                T = b.restT;
                // la RAIZ: su movimiento (los ejes que el clip anima) escalado por tamano
                float v = 0.0f;
                if (root && TranslationChannel(tr, AnimX, frameF, continuous, frame, v)) T.x = v * rootScale;
                if (root && TranslationChannel(tr, AnimY, frameF, continuous, frame, v)) T.y = v * rootScale;
                if (root && TranslationChannel(tr, AnimZ, frameF, continuous, frame, v)) T.z = v * rootScale;
            }
            break;
        }
        b.poseT = T; b.poseR = R; b.poseS = S;
    }
    for (size_t i = 0; i < N; i++){
        W3dBone& b = a->bones[i];
        if (a->skinRebuildFK){
            // FK DEGENERADO (biped): el rest local se reconstruye del TransformLink (esqueleto real, sin colapsar) y la
            // animacion se aplica como DELTA de ROTACION del Lcl (restR->poseR). Solo la rotacion: las Lcl translations
            // y la escala del biped vienen degeneradas y contaminaban el delta (disparaban los huesos). El preRot se
            // cancela en el delta. En rest (poseR=restR) el delta es identidad -> local = Lcorrect -> FK reproduce el TL.
            // tlNode ya viene NORMALIZADO (escala 1) desde PrepareSkin -> Lcorrect y el delta trabajan en escala 1.
            Matrix4 Lcorrect = (b.parent >= 0 && b.parent < (int)N)
                             ? a->bones[b.parent].tlNode.Inverse() * b.tlNode : b.tlNode;
            // ROTACION: delta del Lcl (restR->poseR). PERO el bind (TL) y el rest-Lcl NO orientan igual el hueso: la
            // orientacion Q de Lcorrect (del TransformLink) difiere de MatRotEuler(restR) hasta ~20 deg en las EXTREMIDADES
            // (bind-pose != rest-pose, tipico de rigs de juego). El delta se APLICA en el frame del bind (Q) pero se CALCULA
            // en el frame Lcl (restR): si difieren, el eje del delta sale rotado -> negligible en rest/walk (rotacion chica)
            // pero CATASTROFICO en caidas/ataques (rotacion grande) -> el esqueleto se abre y la malla se desarma. Fix:
            // conjugar el delta por la correccion fija C = inv(Rrest)*Q -> delta expresado en el frame del bind. En rest
            // (poseR=restR) el delta es identidad -> local = Lcorrect (reproduce el TL exacto).
            Matrix4 deltaR = MatRotEuler(b.restR, b.rotOrder).Inverse() * MatRotEuler(b.poseR, b.rotOrder);
            Matrix4 Q; Q.Identity(); // orientacion pura del bind (columnas de Lcorrect normalizadas, sin traslacion)
            for (int c = 0; c < 3; c++){ float l = sqrtf(Lcorrect.m[c*4]*Lcorrect.m[c*4] + Lcorrect.m[c*4+1]*Lcorrect.m[c*4+1] + Lcorrect.m[c*4+2]*Lcorrect.m[c*4+2]);
                if (l < 1e-6f) l = 1.0f; Q.m[c*4] = Lcorrect.m[c*4]/l; Q.m[c*4+1] = Lcorrect.m[c*4+1]/l; Q.m[c*4+2] = Lcorrect.m[c*4+2]/l; }
            Matrix4 C = MatRotEuler(b.restR, b.rotOrder).Inverse() * Q; // correccion fija bind(TL) vs rest(Lcl)
            Matrix4 deltaRot = C.Inverse() * deltaR * C;
            // TRASLACION: la traslacion Lcl animada (poseT-restT) es el ROOT MOTION (la raiz avanza/baja al caminar).
            // La Lcl viene en la MISMA escala que el esqueleto reconstruido (el tlNode ya se dividio por figureScale en
            // PrepareSkin -> escala de la malla; la Lcl rest/anim tambien esta en esa escala, ~decenas). Se aplica
            // CRUDA: sin esto el personaje FLOTA (root motion aplastado ~100x). En rest (poseT=restT) el delta es 0; los
            // huesos rigidos del biped tienen poseT~restT -> solo la RAIZ (Bip01) traslada.
            Vector3 dt = b.poseT - b.restT;
            Matrix4 Tdelta; Tdelta.Identity(); Tdelta.m[12] = dt.x; Tdelta.m[13] = dt.y; Tdelta.m[14] = dt.z;
            local[i] = Tdelta * Lcorrect * deltaRot;
        } else {
            local[i] = LocalMat(b.poseT, b.poseR, b.poseS, b.preRot, b.rotOrder); // FK desde la POSE (editable)
        }
    }
    // MUNDO por hueso; head animado en espacio nodo -> Y-up (el Object de la armature aplica la escala 0.01 al dibujar)
    static std::vector<Vector3> headNode, tailNode; headNode.resize(N); tailNode.resize(N);
    for (size_t i = 0; i < N; i++){
        world[i] = WorldMat(a->bones, local, (int)i); // FK NORMAL: el MOVIMIENTO correcto del hueso
        headNode[i] = world[i] * Vector3(0,0,0);      // display = FK normal (el hueso rota/se mueve bien)
        // Child Of: el mundo del hueso en el MISMO espacio que el display (glTF ya es Y-up; FBX es nodo Z-up)
        a->bones[i].poseWorld = a->skinGltf ? world[i] : (MatrixNodeToYup() * world[i]);
        // SKINNING: skinMatrix = world_FK * inv(bind). El bind es el TransformLink REAL del FBX (skinInvBind ya
        // apunta a inv(tlNode) cuando skinUsesBind; ver PrepareSkin) -> la malla, authored a ese bind, queda PEGADA
        // al hueso a la vez que este se mueve con el FK correcto. LISA (sin TransformLink) usa el FK-rest/segmentado.
        if (a->bones[i].hasSkin)
            a->bones[i].skinMatrix = world[i] * (g_skinFormula == 1 ? a->bones[i].skinA : a->bones[i].skinInvBind);
    }
    // tails: en un rig AUTORADO el tail es DATA del usuario (Edit Mode de huesos) -> se transforma con el
    // hueso: tail(pose) = world * (tail - head) (el vector local del hueso; en rest da el tail autorado).
    // En rigs FBX el tail no es confiable -> se deriva: hueso con hijo = head del 1er hijo; hoja = extender.
    if (a->skinAuthored){
        for (size_t i = 0; i < N; i++)
            tailNode[i] = world[i] * (a->bones[i].tail - a->bones[i].head);
    } else {
    static std::vector<int> firstChild; firstChild.assign(N, -1);
    for (size_t i = 0; i < N; i++){ int pair = a->bones[i].parent; if (pair >= 0 && pair < (int)N && firstChild[pair] < 0) firstChild[pair] = (int)i; }
    for (size_t i = 0; i < N; i++){
        if (firstChild[i] >= 0) tailNode[i] = headNode[firstChild[i]];        // conectado: tail = head del hijo
        else { int pair = a->bones[i].parent;                                 // hoja: extender en la direccion parent->bone
               tailNode[i] = (pair >= 0 && pair < (int)N) ? headNode[i] + (headNode[i] - headNode[pair])
                                                        : headNode[i] + Vector3(0, 0, 5); }
    }
    }
    for (size_t i = 0; i < N; i++){
        // glTF ya viene Y-up (el FK trabaja en espacio escena) -> sin NodeToYup. FBX trabaja en espacio nodo Z-up -> convierte.
        a->bones[i].poseHead = a->skinGltf ? headNode[i] : NodeToYup(headNode[i]);
        a->bones[i].poseTail = a->skinGltf ? tailNode[i] : NodeToYup(tailNode[i]);
    }
}



// devuelve (creando si falta) la CURVA de un (propiedad, componente). Ej: (AnimPosition, AnimX) = "X Location".
AnimProperty& BoneTrack::PropertyOf(int property, int component){
    return ListProperty(Properties, property, component);
}

// devuelve (creando si falta) el track de un hueso
BoneTrack& SkeletalAnimation::TrackOf(int bone){
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i].bone == bone) return tracks[i];
    BoneTrack t;
    t.bone = bone;
    tracks.push_back(t);
    return tracks.back();
}

// ===== SKINNING: deforma m->skinVertex a la pose del esqueleto (linear blend por vertex groups) =====
// Liviano (N95): solo posiciones; y solo recalcula si cambio el frame. Conjuga la skinMatrix (espacio escena) al
// espacio LOCAL de la malla (MLi * skin * ML) para que el render aplique el transform del objeto como siempre.
// SKINNING INCREMENTAL (interruptor A/B del harness 'skininc'): recalcular SOLO los vertices cuyos
// huesos o formas se MOVIERON de verdad desde su ultima aplicacion. Default ON. Ver SkinMesh.
bool g_skinIncremental = true;
// diagnostico del harness (skinverify): que camino tomo el ULTIMO SkinMesh de cada malla.
// 0=early-out(frame/pose/shape iguales) 2=nada-se-movio 3=salpicado 4=completo 5=sin-mapeo
int g_skinLastPath = -1;
// PERF EN EL DEVICE: ms acumulados skinneando (los mide el caller del render, Mesh::RenderObject,
// con este reloj; NULL = sin medicion, costo cero). La plataforma instala su reloj de ms (el N95
// User::NTickCount) y el log PERF /30f del contenedor imprime y resetea los contadores: dice si
// el cuello del frame es el skinning (y si el incremental esta cortando o cae al camino completo).
unsigned int (*g_skinClockMs)() = 0;
int g_skinPerfMs = 0;       // ms acumulados en SkinMesh (quien loguea resetea)
int g_skinPerfSkins = 0;    // llamadas que RECALCULARON algo (camino >= 2)
int g_completeSkinPerf = 0; // de esas, las que cayeron al camino COMPLETO (4)

// suma TODAS las formas activas al delta denso por control-point (shapeCpDelta). Lo usa el pase
// COMPLETO (de cero); el incremental lo mantiene por DIFERENCIA de pesos. NULL = sin formas.
static const float* SkinCompleteShapes(Mesh* m, int nv, bool shapes){
    if (!shapes){ m->shapeCpDelta.clear(); return NULL; }   // sin formas: el delta persistente queda VACIO (coherente con el incremental)
    int nCp = 0;
    for (int ri = 0; ri < nv && ri < (int)m->vertCtrlPoint.size(); ri++) { int c = m->vertCtrlPoint[ri]; if (c + 1 > nCp) nCp = c + 1; }
    m->shapeCpDelta.assign((size_t)nCp * 3, 0.0f);
    for (size_t k = 0; k < m->shapeKeys.size() && k < m->shapeWeights.size(); k++) {
        const float w = m->shapeWeights[k]; if (w == 0.0f) continue;
        const W3dShapeKey& sk = m->shapeKeys[k];
        for (size_t j = 0; j < sk.idx.size() && j * 3 + 2 < sk.d.size(); j++) {
            const int c = sk.idx[j]; if (c < 0 || c >= nCp) continue;
            m->shapeCpDelta[(size_t)c*3]   += w * sk.d[j*3];
            m->shapeCpDelta[(size_t)c*3+1] += w * sk.d[j*3+1];
            m->shapeCpDelta[(size_t)c*3+2] += w * sk.d[j*3+2];
        }
    }
    return m->shapeCpDelta.empty() ? NULL : &m->shapeCpDelta[0];
}

// AABB LOCAL de la pose recien skinneada (para el autocull por frustum de RenderObject): un pase
// barato sobre skinVertex. Corre SOLO cuando se (re)calculo la pose, no por frame.
// (comparando los floats como ENTEROS: los bits de un float positivo ordenan igual que el numero, y los de uno negativo
// al reves; con el xor quedan en orden. En el ARM11 cada comparacion de floats vacia el pipeline del VFP: ~10 ciclos,
// seis por vertice. Las posiciones son finitas: SkinPoint deja en el bind lo de un hueso roto)
static inline int SkinOrder(GLfloat f){ int i; memcpy(&i, &f, 4); return i >= 0 ? i : (int)(i ^ 0x7fffffff); }
static inline float SkinDisorder(int i){ if (i < 0) i = (int)(i ^ 0x7fffffff); float f; memcpy(&f, &i, 4); return f; }
static void SkinUpdateAabb(Mesh* m){
    const int nv = m->vertexSize;
    if (!m->skinVertex || nv <= 0){ m->skinAabbOk = false; return; }
    const GLfloat* v = m->skinVertex;
    int mnx = SkinOrder(v[0]), mny = SkinOrder(v[1]), mnz = SkinOrder(v[2]);
    int mxx = mnx, mxy = mny, mxz = mnz;
    for (int i = 1; i < nv; i++){
        const int x = SkinOrder(v[i*3]), y = SkinOrder(v[i*3+1]), z = SkinOrder(v[i*3+2]);
        if (x < mnx) mnx = x; if (x > mxx) mxx = x;
        if (y < mny) mny = y; if (y > mxy) mxy = y;
        if (z < mnz) mnz = z; if (z > mxz) mxz = z;
    }
    m->skinAabbMin = Vector3(SkinDisorder(mnx), SkinDisorder(mny), SkinDisorder(mnz));
    m->skinAabbMax = Vector3(SkinDisorder(mxx), SkinDisorder(mxy), SkinDisorder(mxz));
    m->skinAabbOk = true;
}

// LAS MATRICES DE LOS HUESOS JUNTAS: los 12 numeros que usa el skinning (3x3 + traslacion) de cada hueso seguidos, y
// si el hueso esta sano (todo finito y no enorme). Se arman una vez por SkinMesh: el bucle por punto ya no salta por
// el W3dBone entero ni compara cada vertice (en el ARM11 cada comparacion de floats vacia el pipeline del VFP; medido en
// el Galaxy Ace: el skinning de Goku y Freezer bajo de 10.6 a 3.5 ms con SkinPoint)
// (los punteros del bucle no se pisan entre si: el compilador puede dejar los valores en registros. Solo donde el
// compilador lo entiende: el de Symbian para el emulador no)
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER) || defined(__ARMCC_VERSION)
#define W3D_NO_ALIAS __restrict
#else
#define W3D_NO_ALIAS
#endif
static std::vector<float> gSkinMats;
static std::vector<unsigned char> gSaneSkin;
static void SkinCollectBones(Armature* a){
    const size_t nb = a->bones.size();
    gSkinMats.resize(nb * 12 + 12); gSaneSkin.resize(nb + 1);
    for (size_t b = 0; b < nb; b++){
        const float* mm = a->bones[b].skinMatrix.m; float* d = &gSkinMats[b * 12];
        d[0]=mm[0]; d[1]=mm[1]; d[2]=mm[2]; d[3]=mm[4]; d[4]=mm[5]; d[5]=mm[6];
        d[6]=mm[8]; d[7]=mm[9]; d[8]=mm[10]; d[9]=mm[12]; d[10]=mm[13]; d[11]=mm[14];
        bool sane = true;
        for (int k = 0; k < 12; k++) if (!(d[k] < 1e6f && d[k] > -1e6f)) sane = false;   // (un NaN tampoco pasa)
        gSaneSkin[b] = sane ? 1 : 0;
    }
}
// UN PUNTO de control: su matriz mezclada (en registros) aplicada a su bind (+ las formas) y a la normal de cada
// render-vert suyo. Un punto sin peso, o con un hueso roto (NaN, infinito), queda en el bind: el vertice no se va al
// infinito. La normal no se divide por 127: se normaliza igual y la escala no cambia el resultado
static inline void SkinPoint(Mesh* m, int c, int nb, const float* cpd, bool doN){
    const int* W3D_NO_ALIAS rv = &m->skinCpRv[0];
    const int rs = m->skinCpRvOff[(size_t)c], re = m->skinCpRvOff[(size_t)c + 1];
    if (rs >= re) return;   // punto sin render-verts (huerfano)
    const float* W3D_NO_ALIAS vert = m->vertex;
    const GLbyte* W3D_NO_ALIAS nor = m->normals;
    GLfloat* W3D_NO_ALIAS sv = m->skinVertex;
    GLbyte* W3D_NO_ALIAS sn = m->skinNormals;
    const float* W3D_NO_ALIAS mats = &gSkinMats[0];
    const unsigned char* W3D_NO_ALIAS sane = &gSaneSkin[0];
    const int s = m->skinCpOff[(size_t)c], e = m->skinCpOff[(size_t)c + 1];
    float M0=0,M1=0,M2=0,M3=0,M4=0,M5=0,M6=0,M7=0,M8=0,M9=0,M10=0,M11=0;
    bool weighs = s < e;
    if (weighs && e - s == 1){   // un hueso (la mayoria): su matriz tal cual (el peso normalizado es 1)
        const int b = m->skinCpBone[(size_t)s];
        if (b < 0 || b >= nb || !sane[b]) weighs = false;
        else { const float* mm = &mats[b * 12];
            M0=mm[0]; M1=mm[1]; M2=mm[2]; M3=mm[3]; M4=mm[4]; M5=mm[5];
            M6=mm[6]; M7=mm[7]; M8=mm[8]; M9=mm[9]; M10=mm[10]; M11=mm[11]; }
    } else if (weighs){
        const int* W3D_NO_ALIAS bn = &m->skinCpBone[0];
        const float* W3D_NO_ALIAS wt = &m->skinCpW[0];
        for (int k = s; k < e; k++){
            const int b = bn[k];
            if (b < 0 || b >= nb) continue;   // (defensa si el rig encogio: la firma del CSR igual lo invalida)
            if (!sane[b]){ weighs = false; break; }
            const float* mm = &mats[b * 12]; const float w = wt[k];
            M0+=mm[0]*w; M1+=mm[1]*w; M2+=mm[2]*w; M3+=mm[3]*w; M4+=mm[4]*w; M5+=mm[5]*w;
            M6+=mm[6]*w; M7+=mm[7]*w; M8+=mm[8]*w; M9+=mm[9]*w; M10+=mm[10]*w; M11+=mm[11]*w;
        }
    }
    // la POSICION es la misma para todos los render-verts del punto (comparten bind): 1 mat*vec
    const int ri0 = rv[rs];
    float vx = vert[ri0*3], vy = vert[ri0*3+1], vz = vert[ri0*3+2];
    if (cpd){ vx += cpd[(size_t)c*3]; vy += cpd[(size_t)c*3+1]; vz += cpd[(size_t)c*3+2]; }   // la base con las formas
    float px = vx, py = vy, pz = vz;
    if (weighs){
        px = M0*vx + M3*vy + M6*vz + M9;
        py = M1*vx + M4*vy + M7*vz + M10;
        pz = M2*vx + M5*vy + M8*vz + M11;
    }
    for (int t = rs; t < re; t++){
        const int ri = rv[t];
        sv[ri*3] = px; sv[ri*3+1] = py; sv[ri*3+2] = pz;
        if (!doN) continue;
        // NORMAL: la 3x3 mezclada del punto * la normal de bind del render-vert (distinta por seam)
        const float nx = nor[ri*3], ny = nor[ri*3+1], nz = nor[ri*3+2];
        const float anx = M0*nx + M3*ny + M6*nz;
        const float any = M1*nx + M4*ny + M7*nz;
        const float anz = M2*nx + M5*ny + M8*nz;
        const float l2 = anx*anx + any*any + anz*anz;
        if (weighs && l2 > 1.6e-8f){ const float sc = 127.0f * FastInvSqrt(l2);   // (1e-12 de la normal de largo 1, por 127^2)
            sn[ri*3]=(GLbyte)(anx*sc); sn[ri*3+1]=(GLbyte)(any*sc); sn[ri*3+2]=(GLbyte)(anz*sc); }
        else { sn[ri*3]=nor[ri*3]; sn[ri*3+1]=nor[ri*3+1]; sn[ri*3+2]=nor[ri*3+2]; }
    }
}

// MEDICION FINA (el medidor del juego compilado en modo debug): con un reloj de alta resolucion instalado, cuanto
// tardan en total los SkinMesh del cuadro y, de eso, evaluar la pose del esqueleto (las curvas y las matrices)
double (*g_skinClockHeight)() = 0;
double g_skinTotalMs = 0, g_skinPoseMs = 0;
namespace { struct MeasureSkin { double t0; MeasureSkin() : t0(g_skinClockHeight ? g_skinClockHeight() : 0) {}
    ~MeasureSkin() { if (g_skinClockHeight) g_skinTotalMs += g_skinClockHeight() - t0; } }; }

void SkinMesh(Mesh* m){
    if (!m || !m->skinArmature || !m->vertex || m->vertexSize <= 0) return;
    MeasureSkin measure;
    Armature* a = m->skinArmature;
    const double tPose = g_skinClockHeight ? g_skinClockHeight() : 0;
    EvaluatePoseSkeleton(a, CurrentFrame); // asegura skinMatrix de cada hueso al frame actual (cacheado)
    if (g_skinClockHeight) g_skinPoseMs += g_skinClockHeight() - tPose;
    // ya skinneado para ESTA pose? cache por (frame, poseSerial): si la pose se recalculo (posar/elegir clip en el mismo
    // frame) el serial cambio -> re-skinnea aunque el frame sea el mismo (antes solo miraba el frame -> quedaba stale).
    if (m->lastSkinFrame == CurrentFrame && m->skinPoseSerial == a->poseSerial && m->skinVertex &&
        m->skinShapeSerial == m->shapeSerial) { g_skinLastPath = 0; return; }
    m->skinPoseSerial = a->poseSerial;
    m->skinShapeSerial = m->shapeSerial;
    g_skinLastPath     = 4;   // default: completo (los otros caminos lo pisan al salir)
    // SHAPE KEYS: con alguna forma prendida la base del skinning es vertex + sum(peso * forma), por punto de control.
    const bool shapes = m->HasShapesActive() && !m->vertCtrlPoint.empty();
    m->lastSkinFrame = CurrentFrame;
    // OJO: vertexSize = cantidad de VERTICES (no de floats). Los arrays vertex/normals/skin* son vertexSize*3.
    int nv = m->vertexSize;      // vertices
    int fc = m->vertexSize * 3;  // floats/bytes de los arrays (3 por vertice)
    // REALLOC si el mesh cambio de tamaño (edit/apply modificador): sino skinVertex queda chico -> overflow -> crash
    if (m->skinVertex && m->skinVertexCap != fc) { delete[] m->skinVertex; m->skinVertex = NULL;
        if (m->skinNormals){ delete[] m->skinNormals; m->skinNormals = NULL; } m->skinIncOk = false; }
    // PADDING: el driver GL (mesa) sube los client arrays con copias vectorizadas (AVX, 32 bytes/vez) y SOBRE-LEE el
    // final del array; +16 floats / +32 bytes de holgura para que el over-read no se salga del heap (cazado con ASan).
    if (!m->skinVertex) { m->skinVertex = new GLfloat[fc + 16]; m->skinVertexCap = fc; m->skinIncOk = false; }
    // NORMALES: rotarlas por los huesos (iluminacion correcta al doblar). Solo si hay luz (skinWithLight) y normales de bind.
    bool doN = m->skinWithLight && m->normals != NULL;
    if (doN && !m->skinNormals){ m->skinNormals = new GLbyte[fc + 32]; m->skinIncOk = false; }
    if ((int)m->vertCtrlPoint.size() < nv) {   // sin mapeo render-vert -> control-point: queda bind + formas (no romper)
        const float* cpd0 = SkinCompleteShapes(m, nv, shapes);
        for (int i = 0; i < fc; i++) m->skinVertex[i] = m->vertex[i];
        if (cpd0) for (int ri = 0; ri < nv && ri < (int)m->vertCtrlPoint.size(); ri++) {
            const int c = m->vertCtrlPoint[ri]; if (c < 0) continue;
            m->skinVertex[ri*3] += cpd0[c*3]; m->skinVertex[ri*3+1] += cpd0[c*3+1]; m->skinVertex[ri*3+2] += cpd0[c*3+2];
        }
        if (doN) for (int i = 0; i < fc; i++) m->skinNormals[i] = m->normals[i];
        m->skinIncOk = false;
        SkinUpdateAabb(m); return;
    }

    // ---- CACHE CSR de pesos por CONTROL-POINT (no por render-vert). Se arma UNA sola vez y se reusa cada frame. Un
    //      asset de juego duplica cada control-point en varios render-verts por seams de UV/normal/material (banana:
    //      42840 render pero 7186 CP = 6x). Como los render-verts de un mismo CP comparten bind + pesos, el blend caro
    //      de matrices se hace 1 vez POR CP y se escalea (6x menos matematica). Invalida por firma de topologia
    //      (nextSkinFlat): vertexSize, armature, #huesos, #grupos + puntero/tamaño de cada grupo, #control-points. ----
    unsigned next = (unsigned)nv * 2654435761u;
    next ^= (unsigned)(size_t)a; next = next*31u + (unsigned)a->bones.size();
    next = next*31u + (unsigned)m->vertexGroups.size() + (unsigned)m->vertCtrlPoint.size()*131u;
    next = next*2654435761u + m->skinGeomVersion; // regenerar geometria (GenerateRender/ComputeBorders) invalida aunque nV no cambie;
                                                // como el mapeo render->CP (y por ende nCtrl) se rehace ahi, cubre el conteo de CP.
    // los NOMBRES mapean group->bone (boneOf.find(vg->name)); si se renombra un grupo o un hueso, el mapeo cambia
    // pero punteros/tamaños no -> hay que hashear los nombres (y hasSkin) sino el CSR queda stale. Barato (~cientos de chars).
    for (size_t b = 0; b < a->bones.size(); b++){ const std::string& nm=a->bones[b].name;
        for (size_t c=0;c<nm.size();c++) next = next*31u + (unsigned char)nm[c];
        next = next*2u + (a->bones[b].hasSkin?1u:0u); }
    for (size_t g = 0; g < m->vertexGroups.size(); g++){
        next ^= (unsigned)(size_t)m->vertexGroups[g];
        next = next*31u + (unsigned)m->vertexGroups[g]->verts.size();
        const std::string& gn=m->vertexGroups[g]->name;
        for (size_t c=0;c<gn.size();c++) next = next*31u + (unsigned char)gn[c];
    }
    if (m->nextSkinFlat != next || m->skinCpOff.empty()){
        // nCtrl (cantidad de control-points) SOLO se recomputa al reconstruir (cambia con la geometria, ya en el sig via
        // skinGeomVersion). Fuera de aca el hot-loop usa m->skinNCtrl cacheado -> sin loop O(nv) por frame.
        int nCtrl = 0;
        for (int ri = 0; ri < nv; ri++){ int c = m->vertCtrlPoint[ri]; if (c + 1 > nCtrl) nCtrl = c + 1; }
        m->skinNCtrl = nCtrl;
        std::map<std::string,int> boneOf;                         // nombre de hueso -> indice (solo al reconstruir)
        for (size_t b = 0; b < a->bones.size(); b++) boneOf[a->bones[b].name] = (int)b;
        // control-point -> lista de (hueso, peso), solo huesos con skin. Los vertex groups vienen por control-point del FBX.
        std::map<int, std::vector<std::pair<int,float> > > cpW;
        for (size_t g = 0; g < m->vertexGroups.size(); g++){
            VertexGroup* vg = m->vertexGroups[g];
            std::map<std::string,int>::iterator it = boneOf.find(vg->name);
            if (it == boneOf.end()) continue;
            int b = it->second; if (b < 0 || b >= (int)a->bones.size() || !a->bones[b].hasSkin) continue;
            for (size_t j = 0; j < vg->verts.size() && j < vg->weights.size(); j++)
                cpW[vg->verts[j]].push_back(std::make_pair(b, vg->weights[j]));
        }
        // PRE-NORMALIZAR los pesos de cada control-point a suma 1 (aca, UNA sola vez) -> el hot-loop por frame ya NO
        // divide. Los cp con peso total ~0 se DESCARTAN (el vertex queda en bind, como antes hacia el wsum<=0.0001).
        for (std::map<int, std::vector<std::pair<int,float> > >::iterator it = cpW.begin(); it != cpW.end(); ){
            std::vector<std::pair<int,float> >& lst = it->second;
            float sum = 0.0f; for (size_t k = 0; k < lst.size(); k++) sum += lst[k].second;
            if (sum <= 1e-6f){ std::map<int, std::vector<std::pair<int,float> > >::iterator er = it++; cpW.erase(er); continue; }
            float inv = 1.0f/sum; for (size_t k = 0; k < lst.size(); k++) lst[k].second *= inv;
            ++it;
        }
        // aplanar a CSR por CONTROL-POINT (offset[c]..offset[c+1] = rango de (bone,weight) del CP c)
        m->skinCpOff.assign(nCtrl+1, 0);
        for (int c = 0; c < nCtrl; c++){
            std::map<int, std::vector<std::pair<int,float> > >::iterator it = cpW.find(c);
            m->skinCpOff[c+1] = (it==cpW.end()) ? 0 : (int)it->second.size();
        }
        for (int c = 0; c < nCtrl; c++) m->skinCpOff[c+1] += m->skinCpOff[c]; // prefix sum
        int total = m->skinCpOff[nCtrl];
        m->skinCpBone.assign(total > 0 ? total : 0, 0); m->skinCpW.assign(total > 0 ? total : 0, 0.0f);
        for (int c = 0; c < nCtrl; c++){
            std::map<int, std::vector<std::pair<int,float> > >::iterator it = cpW.find(c);
            if (it == cpW.end()) continue;
            int base = m->skinCpOff[c];
            for (size_t k = 0; k < it->second.size(); k++){ m->skinCpBone[base+(int)k]=it->second[k].first; m->skinCpW[base+(int)k]=it->second[k].second; }
        }
        // ---- INVERSOS para el skinning INCREMENTAL: hueso -> CPs que influye, y CP -> render-verts.
        //      Con esto "se movio el hueso B / la forma K" se traduce directo a la lista EXACTA de
        //      vertices a recalcular, sin recorrer la malla entera. Mismo ciclo de vida que el CSR. ----
        m->skinBoneCpOff.assign(a->bones.size() + 1, 0);
        for (int c = 0; c < nCtrl; c++)
            for (int k = m->skinCpOff[c]; k < m->skinCpOff[c+1]; k++) m->skinBoneCpOff[(size_t)m->skinCpBone[k] + 1]++;
        for (size_t b = 0; b < a->bones.size(); b++) m->skinBoneCpOff[b+1] += m->skinBoneCpOff[b];
        m->skinBoneCp.assign(m->skinCpBone.size(), 0);
        { std::vector<int> cur(m->skinBoneCpOff.begin(), m->skinBoneCpOff.end());
          for (int c = 0; c < nCtrl; c++)
              for (int k = m->skinCpOff[c]; k < m->skinCpOff[c+1]; k++) m->skinBoneCp[(size_t)cur[m->skinCpBone[k]]++] = c; }
        m->skinCpRvOff.assign((size_t)nCtrl + 1, 0);
        for (int ri = 0; ri < nv; ri++){ int c = m->vertCtrlPoint[ri]; if (c >= 0 && c < nCtrl) m->skinCpRvOff[(size_t)c + 1]++; }
        for (int c = 0; c < nCtrl; c++) m->skinCpRvOff[(size_t)c + 1] += m->skinCpRvOff[c];
        m->skinCpRv.assign((size_t)m->skinCpRvOff[nCtrl], 0);
        { std::vector<int> cur(m->skinCpRvOff.begin(), m->skinCpRvOff.end());
          for (int ri = 0; ri < nv; ri++){ int c = m->vertCtrlPoint[ri]; if (c >= 0 && c < nCtrl) m->skinCpRv[(size_t)cur[c]++] = ri; } }
        m->skinCpDirty.assign((size_t)(nCtrl > 0 ? nCtrl : 1), 0);
        // UMBRAL SUB-VISIBLE escalado a la malla: un hueso que se movio menos que esto no mueve ningun
        // vertex mas que una fraccion de pixel. Los bakes por-frame del juego (clip .w3da) traen RUIDO float
        // en TODOS los huesos cada frame -> medido en la intro HL2 (G-Man hablando): el 85% de los deltas de
        // matriz caen por debajo de 1e-4 (jitter de cuantizacion), solo el 1% supera 1e-3 (movimiento real).
        // Con el umbral viejo (5e-5) ese jitter PASABA -> los 70 huesos "cambiaban" cada frame -> 100% de CPs
        // sucios -> el incremental caia SIEMPRE al pase completo (en el N95: skin ~200 ms/frame = 4 fps). El
        // umbral 2e-4 filtra la banda del jitter y deja pasar el movimiento real; el error maximo por vertex
        // es umbral * brazo_de_palanca (~0.2 mm en el cuerpo, ~0.03 mm en la cara: sub-pixel a 240p) y NO
        // deriva (la referencia solo avanza para los huesos aplicados; el ruido oscila sin acumular).
        { float mnx=1e30f,mny=1e30f,mnz=1e30f,mxx=-1e30f,mxy=-1e30f,mxz=-1e30f;
          for (int i = 0; i < nv; i++){ const GLfloat* p = &m->vertex[i*3];
              if(p[0]<mnx)mnx=p[0]; if(p[0]>mxx)mxx=p[0];
              if(p[1]<mny)mny=p[1]; if(p[1]>mxy)mxy=p[1];
              if(p[2]<mnz)mnz=p[2]; if(p[2]>mxz)mxz=p[2]; }
          float dx=mxx-mnx, dy=mxy-mny, dz=mxz-mnz;
          float diag = (float)sqrt((double)(dx*dx + dy*dy + dz*dz)); if (diag <= 0.0f) diag = 1.0f;
          m->skinEpsPos = diag * 2e-4f;   // traslacion: ~1/5000 del tamaño de la malla
          m->skinEpsRot = 2e-4f; }        // rotacion (adimensional): filtra el jitter del bake (85% < 1e-4)
        m->skinIncOk = false;   // el proximo pase COMPLETO deja la referencia del incremental
        m->nextSkinFlat = next;
    }

    // ================= SKINNING INCREMENTAL =================
    // Pedido del dueno, textual: "hay que mover/recalcular los vertices minimos y necesarios. nada mas.
    // si el motor recalcula vertices de mas, esta mal". Con una referencia valida (un pase completo
    // previo con este layout), cada frame se recalculan SOLO los CPs cuyos huesos se movieron MAS que
    // el umbral sub-visible (contra la matriz con la que se APLICARON por ultima vez: el arrastre
    // lento acumula contra la referencia y entra solo -> sin deriva) y los CPs de las shape keys cuyo
    // peso cambio (el delta denso se mantiene por DIFERENCIA: un flex del labio solo toca el labio).
    // Si lo sucio cubre >60% de los CPs, el pase completo es mas barato y se cae a el.
    {
        extern bool g_skinIncremental;
        const int nCtrlI = m->skinNCtrl;
        const int nbI = (int)a->bones.size();
        if (g_skinIncremental && m->skinIncOk && nCtrlI > 0 && m->skinVertex &&
            doN == m->skinPrevWithLight && (int)m->skinPrevMat.size() == nbI * 12 &&
            m->skinPrevWeights.size() == m->shapeWeights.size() &&
            (int)m->skinCpDirty.size() >= nCtrlI && !m->skinBoneCpOff.empty()) {
            // 1) huesos movidos por encima del umbral (rotacion y traslacion por separado)
            std::vector<int> bch;
            for (int b = 0; b < nbI; b++){
                const float* mm = a->bones[b].skinMatrix.m;
                const float* pv = &m->skinPrevMat[(size_t)b*12];
                bool ch = false;
                for (int k = 0; k < 9 && !ch; k++){ float d = mm[(k/3)*4 + (k%3)] - pv[k]; if (d < 0) d = -d; if (d > m->skinEpsRot) ch = true; }
                if (!ch) for (int k = 0; k < 3 && !ch; k++){ float d = mm[12+k] - pv[9+k]; if (d < 0) d = -d; if (d > m->skinEpsPos) ch = true; }
                if (ch) bch.push_back(b);
            }
            // 2) shape keys con peso distinto al aplicado (umbral chico sobre el peso 0..1)
            std::vector<int> kch;
            for (size_t k = 0; k < m->shapeWeights.size() && k < m->shapeKeys.size(); k++){
                float d = m->shapeWeights[k] - m->skinPrevWeights[k]; if (d < 0) d = -d;
                if (d > 0.0005f) kch.push_back((int)k);
            }
            if (bch.empty() && kch.empty()){   // NADA se movio de verdad: cero trabajo este frame
                g_skinLastPath = 2;
                return;
            }
            // 3) marcar los CPs sucios UNICOS (bitmap persistente) -> 'sucios'. El presupuesto se decide por el
            //    conteo UNICO (sucios.size()), no por la suma de CPs-por-hueso + indices-de-forma: varias shape
            //    keys de la boca pisan los MISMOS CPs (15 visemas sobre ~1500 CPs del labio), y esa suma con
            //    solapamiento disparaba el fallback al pase completo aunque los CPs reales sucios fueran pocos.
            std::vector<int> dirty;
            for (size_t q = 0; q < bch.size(); q++){ int b = bch[q];
                for (int t = m->skinBoneCpOff[(size_t)b]; t < m->skinBoneCpOff[(size_t)b+1]; t++){
                    int c = m->skinBoneCp[(size_t)t];
                    if (!m->skinCpDirty[(size_t)c]){ m->skinCpDirty[(size_t)c] = 1; dirty.push_back(c); } } }
            for (size_t q = 0; q < kch.size(); q++){ int k = kch[q];
                const W3dShapeKey& sk = m->shapeKeys[(size_t)k];
                for (size_t j = 0; j < sk.idx.size() && j*3+2 < sk.d.size(); j++){
                    int c = sk.idx[j]; if (c < 0 || c >= nCtrlI) continue;
                    if (!m->skinCpDirty[(size_t)c]){ m->skinCpDirty[(size_t)c] = 1; dirty.push_back(c); } } }
            // PRESUPUESTO al 95%: el pase completo hace trabajo REDUNDANTE que el incremental evita (copia el
            // array entero bind->skinVertex, reconstruye el delta denso de TODAS las formas, y recorre nv + nCtrl).
            // En estas mallas nCtrl ~= nv (sin seams colapsados: cada vertex es su CP), asi que el incremental es
            // mas barato incluso con 85-90% sucio (procesa solo lo sucio y se saltea copia + formas-de-cero). El
            // completo solo gana cuando casi TODO esta sucio Y hay mucha deduplicacion (nCtrl << nv): ahi su pase1
            // por-CP es chico. Umbral 95%: el habla con gestos (60-90% sucio) ahora va por el incremental.
            if ((long)dirty.size() <= (long)nCtrlI * 19 / 20) {
                // 4) formas: delta persistente por DIFERENCIA (sus CPs ya quedaron marcados arriba)
                for (size_t q = 0; q < kch.size(); q++){ int k = kch[q];
                    float dw = m->shapeWeights[(size_t)k] - m->skinPrevWeights[(size_t)k];
                    const W3dShapeKey& sk = m->shapeKeys[(size_t)k];
                    if (m->shapeCpDelta.size() < (size_t)nCtrlI * 3) m->shapeCpDelta.assign((size_t)nCtrlI * 3, 0.0f);
                    for (size_t j = 0; j < sk.idx.size() && j*3+2 < sk.d.size(); j++){
                        int c = sk.idx[j]; if (c < 0 || c >= nCtrlI) continue;
                        m->shapeCpDelta[(size_t)c*3]   += dw * sk.d[j*3];
                        m->shapeCpDelta[(size_t)c*3+1] += dw * sk.d[j*3+1];
                        m->shapeCpDelta[(size_t)c*3+2] += dw * sk.d[j*3+2];
                    }
                    m->skinPrevWeights[(size_t)k] = m->shapeWeights[(size_t)k];
                }
                const float* cpdI = m->shapeCpDelta.empty() ? NULL : &m->shapeCpDelta[0];
                // 5) SOLO los CPs sucios: re-mezclar su matriz y re-aplicarla a SUS render-verts
                SkinCollectBones(a);
                const bool doNI = doN && m->skinNormals;
                for (size_t q = 0; q < dirty.size(); q++){
                    const int c = dirty[q]; m->skinCpDirty[(size_t)c] = 0;
                    SkinPoint(m, c, nbI, cpdI, doNI);
                }
                // 6) la referencia avanza SOLO para los huesos aplicados (anti-deriva del resto)
                for (size_t q = 0; q < bch.size(); q++){ int b = bch[q];
                    const float* mm = a->bones[(size_t)b].skinMatrix.m; float* pv = &m->skinPrevMat[(size_t)b*12];
                    pv[0]=mm[0]; pv[1]=mm[1]; pv[2]=mm[2];  pv[3]=mm[4];  pv[4]=mm[5];  pv[5]=mm[6];
                    pv[6]=mm[8]; pv[7]=mm[9]; pv[8]=mm[10]; pv[9]=mm[12]; pv[10]=mm[13]; pv[11]=mm[14]; }
                SkinUpdateAabb(m);
                g_skinLastPath = 3;
                return;
            }
            // sobre presupuesto (>60% sucio): el pase completo es mas barato. Hay que DESMARCAR el bitmap
            // que se ensucio arriba, sino esos CPs quedan en 1 y el proximo frame incremental los saltea
            // (el guard '!skinCpDirty[c]') -> no se recalcularian nunca -> verts stale.
            for (size_t q = 0; q < dirty.size(); q++) m->skinCpDirty[(size_t)dirty[q]] = 0;
        }
    }
    // ---- pase COMPLETO: cada punto de control, con la matriz de sus huesos mezclada UNA vez (los render-verts de un
    //      punto comparten bind y pesos: W3dMeshComputePoints agrupa por posicion exacta), ver SkinPoint ----
    const float* cpd = SkinCompleteShapes(m, nv, shapes);
    const int nCtrl = m->skinNCtrl;   // cacheado en el ultimo rebuild (no se recomputa por frame)
    // los render-verts que no son de ningun punto no estan en el CSR inverso: quedan en el bind (en una malla comun no hay)
    if (m->skinCpRvOff.size() <= (size_t)nCtrl || m->skinCpRvOff[(size_t)nCtrl] < nv) {
        for (int i = 0; i < fc; i++) m->skinVertex[i] = m->vertex[i];
        if (doN) for (int i = 0; i < fc; i++) m->skinNormals[i] = m->normals[i];
    }
    SkinCollectBones(a);
    const int nb = (int)a->bones.size();
    for (int c = 0; c < nCtrl; c++) SkinPoint(m, c, nb, cpd, doN);
    // REFERENCIA del skinning incremental: la pose/formas que ESTE pase completo dejo aplicadas
    {
        const int nbF = (int)a->bones.size();
        m->skinPrevMat.resize((size_t)nbF * 12);
        for (int b = 0; b < nbF; b++){
            const float* mm = a->bones[(size_t)b].skinMatrix.m; float* pv = &m->skinPrevMat[(size_t)b*12];
            pv[0]=mm[0]; pv[1]=mm[1]; pv[2]=mm[2];  pv[3]=mm[4];  pv[4]=mm[5];  pv[5]=mm[6];
            pv[6]=mm[8]; pv[7]=mm[9]; pv[8]=mm[10]; pv[9]=mm[12]; pv[10]=mm[13]; pv[11]=mm[14];
        }
        m->skinPrevWeights.assign(m->shapeWeights.begin(), m->shapeWeights.end());
        m->skinPrevWithLight = doN;
        m->skinIncOk = true;
    }
    SkinUpdateAabb(m);   // AABB de ESTA pose (lo usa el autocull por frustum de RenderObject)
}

// ===== gestion de clips (crear/borrar/mover el activo), igual que los vertex groups =====
// nombre libre de clip dentro del armature ('excepto' = indice que conserva el suyo)
struct AnimNameCtx { Armature* a; int except; };
static bool AnimNameExists(const std::string& n, void* ctx){
    AnimNameCtx* c = (AnimNameCtx*)ctx;
    for (size_t i = 0; i < c->a->animations.size(); i++)
        if ((int)i != c->except && c->a->animations[i] && c->a->animations[i]->name == n) return true;
    return false;
}
std::string AnimFreeName(Armature* a, const std::string& base, int except){
    if (!a) return base;
    AnimNameCtx c; c.a = a; c.except = except;
    return W3dUniqueName(base, "Animation", AnimNameExists, &c);
}

void CreateAnimation(Armature* a){
    if (!a) return;
    W3dOwnArmatureAnims(a);   // la lista pasa a ser del armature (clips de un animset compartido: se copian)
    a->animations.push_back(new SkeletalAnimation(AnimFreeName(a, "Animation", -1)));
    a->activeAnim = (int)a->animations.size() - 1;
    // el TAMANO del esqueleto que lo graba (retarget: otro personaje con la misma jerarquia lo escala)
    a->animations.back()->heightRest = W3dArmatureRestSize(a);
    // arrancar la animacion NUEVA desde la pose RESET del esqueleto (sin esto quedaba la pose del clip anterior).
    for (size_t i = 0; i < a->bones.size(); i++){ a->bones[i].poseT = a->bones[i].restT; a->bones[i].poseR = a->bones[i].restR; a->bones[i].poseS = a->bones[i].restS; }
    a->poseDirty = false; a->lastPoseFrame = -999999; a->lastPoseAnim = -999; // forzar re-eval -> clip vacio = rest
}

// DUPLICA el clip activo (copia nombre+fps+rango+tracks/keyframes) y deja la copia como activa. (Duplicate del card;
// solo tiene sentido si hay clips.) Los tracks son vectores por valor -> la asignacion hace copia profunda.
// hook editor: lo setea Properties.cpp; la lista de animaciones (PropList modo 5) lo llama al seleccionar un clip.
void (*OnSelectAnimClip)(Armature* a, int clipIdx) = NULL;

void DuplicateActiveAnimation(Armature* a){
    if (!a || a->activeAnim < 0 || a->activeAnim >= (int)a->animations.size()) return;
    W3dOwnArmatureAnims(a);   // la copia nueva es PROPIA: la lista entera pasa a ser del armature
    SkeletalAnimation* src = a->animations[a->activeAnim]; if (!src) return;
    // el " copy" era OTRA convencion de sufijo: se unifica a la regla .NNN comun
    SkeletalAnimation* dup = new SkeletalAnimation(AnimFreeName(a, src->name, -1));
    dup->FrameRate = src->FrameRate; dup->startFrame = src->startFrame; dup->endFrame = src->endFrame;
    dup->heightRest = src->heightRest;
    dup->tracks = src->tracks; // copia profunda (BoneTrack/AnimProperty/keyFrame son por valor)
    a->animations.push_back(dup);
    a->activeAnim = (int)a->animations.size() - 1;
}

// pone (o actualiza) un keyframe en 'frame' en las TRES curvas (X/Y/Z) de una propiedad del hueso.
// Cada componente es una curva independiente; keyar la pose las toca a las 3 en el mismo frame, pero despues
// se pueden mover/borrar/curvar por separado desde el dope sheet.
static void SetKey3(BoneTrack& tr, int prop, int frame, const Vector3& v){
    SetKeyCurve(tr.PropertyOf(prop, AnimX), frame, v.x, HPlane);
    SetKeyCurve(tr.PropertyOf(prop, AnimY), frame, v.y, HPlane);
    SetKeyCurve(tr.PropertyOf(prop, AnimZ), frame, v.z, HPlane);
}
// ============================================================================
//  MOTION TRAIL de un HUESO: por donde pasa su cabeza (poseHead), en espacio NODO, frame a frame.
//  Reusa EvaluatePoseSkeleton en vez de recalcular el FK: ese FK tiene toda la correccion del biped
//  (delta de rotacion conjugado, root motion, tlNode...) y duplicarlo seria garantizar que se desincronicen.
//  Como EvaluatePoseSkeleton PISA la pose viva, se guarda y se restaura: al salir, el esqueleto queda igual que
//  como estaba (incluida la pose que el usuario este editando a mano).
//  Es SOLO POSICION, igual que el trail de objetos.
// ============================================================================
bool MotionTrailBoneNode(Armature* a, int bone, std::vector<Vector3>& pts, std::vector<int>& keys,
                          int& from, int& until){
    pts.clear(); keys.clear(); from = 0; until = -1;
    if (!a || bone < 0 || bone >= (int)a->bones.size()) return false;
    if (a->activeAnim < 0 || a->activeAnim >= (int)a->animations.size()) return false;
    SkeletalAnimation* clip = a->animations[a->activeAnim];
    // el rango + los frames con keyframe salen de las curvas de POSICION de ESTE hueso. Si no tiene ninguna, el
    // hueso no traslada por si mismo... pero igual se puede mover porque lo arrastra un PADRE que si rota. Por eso
    // el rango cae al del clip: el trail muestra el camino real, venga de donde venga.
    int mn = 0x7fffffff, mx = -0x7fffffff;
    for (size_t t = 0; t < clip->tracks.size(); t++){
        if (clip->tracks[t].bone != bone) continue;
        const std::vector<AnimProperty>& P = clip->tracks[t].Properties;
        for (size_t p2 = 0; p2 < P.size(); p2++){
            if (P[p2].Property != AnimPosition) continue;
            for (size_t k = 0; k < P[p2].keyframes.size(); k++){
                int f = P[p2].keyframes.Frame(k);   // (Frame: mirar el trail no expande una curva compacta)
                if (f < mn) mn = f; if (f > mx) mx = f;
                bool already = false; for (size_t j = 0; j < keys.size(); j++) if (keys[j] == f){ already = true; break; }
                if (!already) keys.push_back(f);
            }
        }
        break;
    }
    if (mn > mx){                       // sin curvas de posicion propias: el rango del clip (lo arrastra el padre)
        mn = clip->startFrame; mx = clip->endFrame;
        if (mn > mx) return false;
    }
    std::sort(keys.begin(), keys.end());
    from = mn; until = mx;

    // --- guardar la pose viva + el estado del cache ---
    size_t N = a->bones.size();
    static std::vector<Vector3> sT, sR, sS;
    sT.resize(N); sR.resize(N); sS.resize(N);
    for (size_t i = 0; i < N; i++){ sT[i] = a->bones[i].poseT; sR[i] = a->bones[i].poseR; sS[i] = a->bones[i].poseS; }
    int lpf = a->lastPoseFrame, lpa = a->lastPoseAnim; bool pd = a->poseDirty;
    unsigned int ps = a->poseSerial;
    // ...y el gating: EvaluatePoseSkeleton solo lee la curva si ESTE armature es la animacion activa
    int kind0 = ActiveAnimKind; Armature* arm0 = ActiveAnimArm;
    ActiveAnimKind = 1; ActiveAnimArm = a;

    for (int f = mn; f <= mx; f++){
        a->lastPoseFrame = -999999; a->poseDirty = false;   // forzar re-lectura de la curva en ESTE frame
        EvaluatePoseSkeleton(a, f);
        pts.push_back(a->bones[bone].poseHead);
    }

    // --- restaurar TODO: el esqueleto queda como estaba ---
    for (size_t i = 0; i < N; i++){ a->bones[i].poseT = sT[i]; a->bones[i].poseR = sR[i]; a->bones[i].poseS = sS[i]; }
    // ...y rehacer el FK para que poseHead/poseTail vuelvan a matchear esa pose. OJO: NO con lastPoseFrame en
    // -999999. Eso da frameChanged=true y EvaluatePoseSkeleton REFRESCA poseT/R/S DESDE LA CURVA -> pisaria la
    // pose que se acaba de restaurar (y con ella la que el usuario esta editando a mano). La forma de re-correr
    // SOLO el FK es frameChanged=false + poseDirty=true (mismo criterio que InsertKeyframeSkeleton).
    int fLive = (lpf == -999999) ? CurrentFrame : lpf;
    a->lastPoseFrame = fLive; a->lastPoseAnim = a->activeAnim; a->poseDirty = true;
    EvaluatePoseSkeleton(a, fLive);
    ActiveAnimKind = kind0; ActiveAnimArm = arm0;
    a->lastPoseFrame = lpf; a->lastPoseAnim = lpa; a->poseDirty = pd; a->poseSerial = ps;
    return !pts.empty();
}

// AUTO KEY de UN hueso: guarda solo los canales que CAMBIARON respecto de (T0,R0,S0), que es su pose de ANTES
// del transform. Es por CANAL: si solo rotaste en X, se guarda X Euler Rotation y nada mas.
// El clip lo asegura el llamador (AutoKeySkeletonPrep). Devuelve cuantos canales guardo.
int AutoKeyBone(Armature* a, int i, const Vector3& T0, const Vector3& R0, const Vector3& S0){
    if (!a || i < 0 || i >= (int)a->bones.size()) return 0;
    W3dOwnArmatureAnims(a);   // escribe en el clip: nunca en uno compartido
    if (a->activeAnim < 0 || a->activeAnim >= (int)a->animations.size()) return 0;
    SkeletalAnimation* clip = a->animations[a->activeAnim];
    W3dBone& b = a->bones[i];
    BoneTrack& tr = clip->TrackOf(i);
    int n = 0;
    // tolerancia RELATIVA: la pose pasa por matrices y trigonometria, un canal que no se toco vuelve con basura
    // en el ultimo bit -> con == se guardarian los 9 canales siempre y el "solo lo que cambio" no serviria de nada
    struct C { static bool change(float x, float y){
        float d = x-y; if (d<0) d=-d;
        float m = (x<0?-x:x), mb = (y<0?-y:y); if (mb>m) m=mb;
        return d > 1e-5f * (1.0f + m); } };
    const int props[3] = { AnimPosition, AnimRotation, AnimScale };
    const Vector3 newValue[3] = { b.poseT, b.poseR, b.poseS };
    const Vector3 old[3]      = { T0, R0, S0 };
    for (int p2 = 0; p2 < 3; p2++){
        for (int c = 0; c < 3; c++){
            float vn = (c==0)?newValue[p2].x : (c==1)?newValue[p2].y : newValue[p2].z;
            float vv = (c==0)?old[p2].x : (c==1)?old[p2].y : old[p2].z;
            if (!C::change(vn, vv)) continue;
            SetKeyCurve(tr.PropertyOf(props[p2], (c==0)?AnimX:(c==1)?AnimY:AnimZ), CurrentFrame, vn, HPlane);
            n++;
        }
    }
    return n;
}
// Prepara el clip para auto-keyear (lo crea si no hay) y devuelve si se puede. Va antes de AutoKeyBone.
bool AutoKeySkeletonPrep(Armature* a){
    if (!a || a->bones.empty()) return false;
    W3dOwnArmatureAnims(a);   // el auto key va a escribir en el clip activo
    if (a->activeAnim < 0 || a->activeAnim >= (int)a->animations.size()) CreateAnimation(a);
    return (a->activeAnim >= 0 && a->activeAnim < (int)a->animations.size());
}
// Cierra el auto key: extiende el rango del clip y deja la pose visible sin refrescarla desde la curva (mismo
// criterio que InsertKeyframeSkeleton: refrescar reseteaba a rest los huesos sin keyframe).
void AutoKeySkeletonEnd(Armature* a){
    if (!a || a->activeAnim < 0 || a->activeAnim >= (int)a->animations.size()) return;
    W3dOwnArmatureAnims(a);   // extiende el rango del clip
    SkeletalAnimation* clip = a->animations[a->activeAnim];
    if (CurrentFrame > clip->endFrame) clip->endFrame = CurrentFrame;
    if (CurrentFrame < clip->startFrame || clip->startFrame < 1) clip->startFrame = CurrentFrame < 1 ? 1 : CurrentFrame;
    int animPlay = (ActiveAnimKind == 1 && ActiveAnimArm == a) ? a->activeAnim : -1;
    a->lastPoseFrame = CurrentFrame; a->lastPoseAnim = animPlay; a->poseDirty = true;
}

// INSERT KEYFRAME (i): guarda la POSE actual (poseT/R/S) de los huesos SELECCIONADOS en la curva del clip activo,
// en CurrentFrame. Es lo que hace permanente la pose (antes de esto se ve pero no se guarda). Crea un clip si no hay.
void InsertKeyframeSkeleton(Armature* a, int channels){
    if (!a || a->bones.empty()) return;
    if (channels == 0) channels = KfChannelAll;   // menu desplegable: Todos / Loc / Rot / Escala
    W3dOwnArmatureAnims(a);   // keyear escribe en el clip: nunca en uno compartido
    if (a->activeAnim < 0 || a->activeAnim >= (int)a->animations.size()) CreateAnimation(a); // asegurar clip activo
    if (a->activeAnim < 0 || a->activeAnim >= (int)a->animations.size()) return;
    SkeletalAnimation* clip = a->animations[a->activeAnim];
    int nSel = 0;
    for (size_t i = 0; i < a->bones.size(); i++){
        W3dBone& b = a->bones[i];
        // se keyean los huesos SELECCIONADOS y ademas el ACTIVO (aunque no este seleccionado).
        // (habia una segunda condicion "!b.select && activeBone < 0" que era inalcanzable:
        // con activeBone < 0 ningun i la pasa sin cortar antes en esta misma linea)
        if (!b.select && (int)i != a->activeBone) continue;
        BoneTrack& tr = clip->TrackOf((int)i);
        if (channels & KfChannelLoc) SetKey3(tr, AnimPosition, CurrentFrame, b.poseT);
        if (channels & KfChannelRot) SetKey3(tr, AnimRotation, CurrentFrame, b.poseR);
        if (channels & KfChannelScl) SetKey3(tr, AnimScale,    CurrentFrame, b.poseS);
        nSel++;
    }
    if (nSel == 0) return;
    if (CurrentFrame > clip->endFrame) clip->endFrame = CurrentFrame; // extender el rango del clip
    if (CurrentFrame < clip->startFrame || clip->startFrame < 1) clip->startFrame = CurrentFrame < 1 ? 1 : CurrentFrame;
    // NO forzar re-lectura de la curva: eso pulia poseT/R/S de TODOS los huesos desde el clip, y los que NO tienen
    // keyframe (EvalProp devuelve rest) se RESETEABAN -> se perdia la pose que el usuario hizo a mano en los demas
    // huesos al keyar uno solo. En cambio: dejar el frame como "ya evaluado" (lastPoseFrame=CurrentFrame,
    // frameChanged=false) + poseDirty=true -> EvaluatePoseSkeleton re-corre el FK con la pose ACTUAL sin refrescar
    // desde la curva -> se conserva la pose visible de todos los huesos (los sin key vuelven a rest recien al scrollear)
    // El hueso keyado ya coincide con su key.
    int animPlay = (ActiveAnimKind == 1 && ActiveAnimArm == a && a->activeAnim >= 0 && a->activeAnim < (int)a->animations.size()) ? a->activeAnim : -1;
    a->lastPoseFrame = CurrentFrame; a->lastPoseAnim = animPlay; a->poseDirty = true;
}

// BORRAR / MOVER el clip activo: las dos CORREN LOS INDICES de animations[]. Desde el EDITOR no
// se llaman directo (UndoDeleteClipArm / UndoMoveClipArm remapean los destinos de rename, que
// van por (armature, indice)); aca quedan crudas para el runtime. Ver SkeletalAnimation.h.
void DeleteActiveAnimation(Armature* a){
    if (!a) return;
    int i = a->activeAnim;
    if (i < 0 || i >= (int)a->animations.size()) return;
    // borrar no escribe en ningun clip: con clips de un animset compartido solo se suelta el
    // puntero (el clip sigue siendo del animset) y NO se copian los demas (W3dAnimSet.h)
    W3dArmatureAnimsRemove(a, i);
    if (a->activeAnim >= (int)a->animations.size()) a->activeAnim = (int)a->animations.size() - 1;
    // el cache de pose se saltea el trabajo comparando (frame, INDICE de clip) -lastPoseFrame /
    // lastPoseAnim, ver EvaluatePoseSkeleton-. Borrar CORRE los indices: el mismo numero de
    // activeAnim ya es OTRO clip (o ninguno), asi que sin invalidar el viewport se queda
    // mostrando la pose del clip BORRADO hasta que cambie el frame. Reordenar ya lo invalidaba
    // a proposito (ClipSwapInArm, main/undo/Undo.cpp) y el 2D tambien (Armature2DAnimation.cpp).
    a->lastPoseFrame = -999999; a->lastPoseAnim = -999; a->poseDirty = true;
}

void MoveActiveAnimation(Armature* a, int dir){
    if (!a) return;
    int i = a->activeAnim, j = i + dir;
    if (i < 0 || i >= (int)a->animations.size() || j < 0 || j >= (int)a->animations.size()) return;
    SkeletalAnimation* t = a->animations[i];
    a->animations[i] = a->animations[j];
    a->animations[j] = t;
    a->activeAnim = j;
}
