#include "animation/SkeletalAnimation.h"
#include "objects/Armature.h"
#include "objects/Mesh.h"
#include "math/Matrix4.h"
#include <cstdio>
#include <stdio.h>  // sprintf GLOBAL (C99): en Symbian/STLport <cstdio> puede dejarlo solo en std::
#include <cmath>
#include <vector>
#include <map>
#include <utility>
#include <new>      // new (std::nothrow): reservar el cache de animacion SIN crashear si el telefono se queda sin RAM

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
static Vector3 KfVal(const keyFrame& k){ return Vector3(k.valueX, k.valueY, k.valueZ); }

// valor de una AnimProperty en 'frame' (interp LINEAL entre keyframes; keyframes ordenados por frame)
static Vector3 EvalProp(const AnimProperty& ap, int frame, const Vector3& def){
    const std::vector<keyFrame>& k = ap.keyframes;
    if (k.empty()) return def;
    if (frame <= k.front().frame) return KfVal(k.front());
    if (frame >= k.back().frame)  return KfVal(k.back());
    for (size_t i = 1; i < k.size(); i++) if (k[i].frame >= frame){
        int f0 = k[i-1].frame, f1 = k[i].frame;
        float t = (f1 == f0) ? 0.0f : (float)(frame - f0) / (float)(f1 - f0);
        Vector3 a = KfVal(k[i-1]), b = KfVal(k[i]);
        return a + (b - a) * t;
    }
    return KfVal(k.back());
}

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
static Matrix4 LocalDe(const W3dBone& b){
    return b.hasRest ? LocalMat(b.restT, b.restR, b.restS, b.preRot, b.rotOrder) : MatTrans(b.head);
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
static Matrix4 MatrizNodeToYup(){
    Matrix4 Ny; Ny.Identity();
    Ny.m[0]=1; Ny.m[4]=0; Ny.m[8]=0;
    Ny.m[1]=0; Ny.m[5]=0; Ny.m[9]=1;
    Ny.m[2]=0; Ny.m[6]=-1; Ny.m[10]=0;
    return Ny;
}
void PrepararSkin(Armature* a){
    if (!a) return;
    size_t N = a->bones.size();
    std::vector<Matrix4> local(N);
    for (size_t i = 0; i < N; i++) local[i] = LocalDe(a->bones[i]);
    Matrix4 Ny = MatrizNodeToYup(), NyInv = Ny.Inverse();
    int conBind = 0, totalSkin = 0;
    for (size_t i = 0; i < N; i++){
        W3dBone& b = a->bones[i];
        b.hasSkin = b.hasRest; // sin transforms de rest del FBX no hay FK -> no se skinnea con este hueso
        if (!b.hasSkin){ b.skinA.Identity(); b.skinInvBind.Identity(); b.skinMatrix.Identity(); b.tlNode.Identity(); continue; }
        Matrix4 restWorldNode = WorldMat(a->bones, local, (int)i);
        // inversa del bind = FK-rest (fallback universal). El bind "real" del FBX es el TransformLink (b.bind),
        // convertido al espacio nodo: tlNode. Se usa cuando es valido (rigs estandar); LISA lo tiene en cero.
        b.skinInvBind = restWorldNode.Inverse();
        b.skinA = b.skinInvBind;
        // bind del hueso en espacio NODO: el TransformLink (B->escena Y-up) convertido a B->nodo Z-up = NyInv * bind.
        // (ANTES tenia un '* Ny' de mas -> conjugacion -> giro de 90°X extra por hueso: el "rotado 90° sobre si mismo").
        b.tlNode = NyInv * b.bind;
        // TransformLink DEGENERADO (LISA: solo escala, traslacion 0) -> el inverse-bind real es el ESTANDAR FBX
        // inverse(TransformLink)*Transform (la 'Transform' del cluster codifica el swap de ejes/orientacion), en el
        // frame nodo del motor: skinA = inv(NyInv*bind) * (NyInv*clusterTransform). Reduce EXACTO a worldFK*inv(TL)*Transform.
        // El banana NO entra aca (su TransformLink tiene traslacion real -> path skinUsaBind/tlNode intacto).
        float tlN = Vector3(b.bind.m[12], b.bind.m[13], b.bind.m[14]).Length();
        float rnN = Vector3(restWorldNode.m[12], restWorldNode.m[13], restWorldNode.m[14]).Length();
        if (tlN <= 1.0f && rnN > 5.0f){
            // El TransformLink de LISA trae la ESCALA del armature (gigante, ~100) BAKEADA, pero mi FK trabaja en
            // espacio armature-LOCAL (escala 1; el 0.01 del armature se aplica al DIBUJAR). Si no la quito, el skin
            // se encoge 1/escala (piezas al 1%). Normalizo las columnas del bind (escala->1): queda rigido y ubicado.
            Matrix4 bindNorm = b.bind;
            for (int c = 0; c < 3; c++){ float l = sqrtf(bindNorm.m[c*4]*bindNorm.m[c*4] + bindNorm.m[c*4+1]*bindNorm.m[c*4+1] + bindNorm.m[c*4+2]*bindNorm.m[c*4+2]);
                if (l > 1e-6f){ bindNorm.m[c*4]/=l; bindNorm.m[c*4+1]/=l; bindNorm.m[c*4+2]/=l; } }
            Matrix4 tlNorm = NyInv * bindNorm;
            // D = diag(-1,1,-1) = 180° sobre Y (eje profundidad en espacio nodo). El TransformLink degenerado + el
            // swap de ejes del clusterTransform, al pasar por NyInv, dejan la pieza rotada 180° sobre si misma (la
            // "cabeza con la boca para arriba" que reportaba Dante). Verificado vs Blender (ground truth): skinMatrix
            // del motor = skin_Blender * D en los 21 huesos (diff 0.0001). Se corrige multiplicando skinA por D.
            Matrix4 D; D.Identity(); D.m[0] = -1.0f; D.m[10] = -1.0f;
            b.skinA = (tlNorm.Inverse() * (NyInv * b.clusterTransform)) * D;
        }
        b.skinMatrix.Identity();
        totalSkin++;
        // TransformLink valido = tiene traslacion real (no cero). LISA: casi todos en cero -> invalido -> FK-rest.
        if (Vector3(b.bind.m[12], b.bind.m[13], b.bind.m[14]).Length() > 1.0f) conBind++;
    }
    // usar el bind real si la MAYORIA de los huesos tienen TransformLink con datos (banana si; LISA no)
    a->skinUsaBind = (totalSkin > 0 && conBind * 2 > totalSkin);
    // BIND = TransformLink real (no el FK-rest): la malla fue skinneada en la pose del TransformLink, que puede
    // diferir de la Lcl-rest. skinMatrix = world_FK * inv(tlNode) -> malla PEGADA al hueso con el FK correcto.
    if (a->skinUsaBind){
        for (size_t i = 0; i < N; i++) if (a->bones[i].hasSkin){
            Matrix4 inv = a->bones[i].tlNode.Inverse();
            a->bones[i].skinInvBind = inv;
            a->bones[i].skinA = inv;
        }
    }
}

bool g_skelAnimPreview = true; // ON: FK de la animacion (para rigs FBX; los armatures manuales se dibujan en bind)

// los transforms LOCALES del FBX vienen en espacio Z-up (nodo), pero el resto de la armature (bind head/tail,
// malla) esta en Y-up. Se convierte la salida del FK: (x,y,z) Z-up -> (x, z, -y) Y-up.
static Vector3 NodeToYup(const Vector3& v){ return Vector3(v.x, v.z, -v.y); }

// ===== helpers PUBLICOS para el transform interactivo de huesos (Pose Mode, main/ViewPorts/LayoutInput.cpp) =====
Matrix4 SkelNodeToYupMat(){ return MatrizNodeToYup(); }
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
Vector3 SkelMatrizAEulerFBX(const Matrix4& M, int /*order*/){
    const float* m = M.m; // column-major m[col*4+fila]; R[fila][col] = m[col*4+fila]
    float sy = -m[2]; if (sy > 1.0f) sy = 1.0f; if (sy < -1.0f) sy = -1.0f; // R[2][0] = -sin(y)
    float y = asinf(sy), cy = cosf(y), x, z;
    if (fabsf(cy) > 1e-4f){ x = atan2f(m[6], m[10]); z = atan2f(m[1], m[0]); } // R[2][1]/R[2][2] , R[1][0]/R[0][0]
    else { x = atan2f(-m[9], m[5]); z = 0.0f; }                                // gimbal lock: fija z=0
    const float R2D = 180.0f / 3.14159265358979f;
    return Vector3(x * R2D, y * R2D, z * R2D);
}

void EvaluarPoseEsqueleto(Armature* a, int frame){
    if (!a) return;
    // CACHE: si el frame y el clip no cambiaron Y la pose no fue editada a mano, ya esta calculada (no recalcular a
    // 60fps). poseDirty (posando) fuerza re-FK sin refrescar poseT/R/S desde la curva.
    bool frameChanged = (a->lastPoseFrame != frame || a->lastPoseAnim != a->animActiva);
    if (!frameChanged && !a->poseDirty) return;
    a->lastPoseFrame = frame; a->lastPoseAnim = a->animActiva;
    a->poseDirty = false;
    // por defecto: rest (poseHead/poseTail = head/tail bind)
    for (size_t i = 0; i < a->bones.size(); i++){ a->bones[i].poseHead = a->bones[i].head; a->bones[i].poseTail = a->bones[i].tail; }
    // FK solo para rigs FBX (con transforms de rest). Los armatures MANUALES (hasRest=false) se muestran en bind.
    bool fbxRig = !a->bones.empty() && a->bones[0].hasRest;
    if (!g_skelAnimPreview || !fbxRig) return;
    SkeletalAnimation* clip = (a->animActiva >= 0 && a->animActiva < (int)a->animations.size()) ? a->animations[a->animActiva] : NULL;

    size_t N = a->bones.size();
    // SCRATCH persistente (reusa la capacidad entre frames -> sin 5 allocs de heap por frame; los loops de abajo
    // sobreescriben TODOS los elementos, asi que resize alcanza). Single-thread, no re-entrante -> static seguro.
    static std::vector<Matrix4> local, world;
    local.resize(N); world.resize(N);
    // Al CAMBIAR de frame se refresca la POSE (poseT/R/S) de cada hueso desde la curva (o rest). Posando NO se
    // refresca (poseDirty): se respeta lo que el usuario esta editando hasta que cambie el frame o inserte keyframe.
    if (frameChanged) for (size_t i = 0; i < N; i++){
        W3dBone& b = a->bones[i];
        Vector3 T = b.restT, R = b.restR, S = b.restS;
        if (clip) for (size_t t = 0; t < clip->tracks.size(); t++) if (clip->tracks[t].bone == (int)i){
            BoneTrack& tr = clip->tracks[t];
            for (size_t p = 0; p < tr.Propertys.size(); p++){
                if      (tr.Propertys[p].Property == AnimPosition) T = EvalProp(tr.Propertys[p], frame, b.restT);
                else if (tr.Propertys[p].Property == AnimRotation) R = EvalProp(tr.Propertys[p], frame, b.restR);
                else if (tr.Propertys[p].Property == AnimScale)    S = EvalProp(tr.Propertys[p], frame, b.restS);
            }
            break;
        }
        b.poseT = T; b.poseR = R; b.poseS = S;
    }
    for (size_t i = 0; i < N; i++){
        W3dBone& b = a->bones[i];
        local[i] = LocalMat(b.poseT, b.poseR, b.poseS, b.preRot, b.rotOrder); // FK desde la POSE (editable)
    }
    // MUNDO por hueso; head animado en espacio nodo -> Y-up (el Object de la armature aplica la escala 0.01 al dibujar)
    static std::vector<Vector3> headNode, tailNode; headNode.resize(N); tailNode.resize(N);
    for (size_t i = 0; i < N; i++){
        world[i] = WorldMat(a->bones, local, (int)i); // FK NORMAL: el MOVIMIENTO correcto del hueso
        headNode[i] = world[i] * Vector3(0,0,0);      // display = FK normal (el hueso rota/se mueve bien)
        // SKINNING: skinMatrix = world_FK * inv(bind). El bind es el TransformLink REAL del FBX (skinInvBind ya
        // apunta a inv(tlNode) cuando skinUsaBind; ver PrepararSkin) -> la malla, authored a ese bind, queda PEGADA
        // al hueso a la vez que este se mueve con el FK correcto. LISA (sin TransformLink) usa el FK-rest/segmentado.
        if (a->bones[i].hasSkin)
            a->bones[i].skinMatrix = world[i] * (g_skinFormula == 1 ? a->bones[i].skinA : a->bones[i].skinInvBind);
    }
    // tails: hueso con hijo -> tail = head del 1er hijo (conectados); hoja -> extender en la direccion del hueso
    static std::vector<int> primerHijo; primerHijo.assign(N, -1);
    for (size_t i = 0; i < N; i++){ int par = a->bones[i].parent; if (par >= 0 && par < (int)N && primerHijo[par] < 0) primerHijo[par] = (int)i; }
    for (size_t i = 0; i < N; i++){
        if (primerHijo[i] >= 0) tailNode[i] = headNode[primerHijo[i]];        // conectado: tail = head del hijo
        else { int par = a->bones[i].parent;                                  // hoja: extender en la direccion padre->hueso
               tailNode[i] = (par >= 0 && par < (int)N) ? headNode[i] + (headNode[i] - headNode[par])
                                                        : headNode[i] + Vector3(0, 0, 5); }
    }
    for (size_t i = 0; i < N; i++){
        a->bones[i].poseHead = NodeToYup(headNode[i]);
        a->bones[i].poseTail = NodeToYup(tailNode[i]);
    }
}



// devuelve (creando si falta) la curva de una propiedad (Position/Rotation/Scale)
AnimProperty& BoneTrack::PropertyDe(int property){
    for (size_t i = 0; i < Propertys.size(); i++)
        if (Propertys[i].Property == property) return Propertys[i];
    AnimProperty p;
    p.Property = property;
    p.firstFrameIndex = 0;
    p.lastFrameIndex = 0;
    Propertys.push_back(p);
    return Propertys.back();
}

// devuelve (creando si falta) el track de un hueso
BoneTrack& SkeletalAnimation::TrackDe(int bone){
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
// ===== CACHE de vertex-animation (bake del skinning) =====
// Reproduce snapshots por-frame en vez de recomputar el skinning. LAZY: la 1ra vuelta skinnea y guarda cada frame;
// despues copia/interpola. Un SOLO clip cacheado a la vez: la firma incluye animActiva -> al cambiar de animacion se
// libera el cache viejo y se re-dimensiona para el rango del clip nuevo (memoria bounded, distintos clips distinto
// tamaño). OOM-SAFE: si new falla (telefono sin RAM), no se guarda ese frame (se re-skinnea, sin crash).
static unsigned SkinCacheFirma(Mesh* m){
    Armature* a = m->skinArmature;
    unsigned s = 2166136261u; // FNV-ish
    s = (s ^ m->skinGeomVersion)                 * 16777619u; // geometria (regen -> invalida)
    s = (s ^ (unsigned)(a ? a->animActiva + 1 : 0)) * 16777619u; // CLIP activo (cambiar de animacion invalida)
    s = (s ^ (unsigned)(size_t)a)                * 16777619u; // rig
    s = (s ^ (unsigned)StartFrame)               * 16777619u; // rango del clip
    s = (s ^ (unsigned)EndFrame)                 * 16777619u;
    s = (s ^ (unsigned)m->skinCacheSkip)         * 16777619u; // decimacion
    s = (s ^ (unsigned)(m->skinConLuz ? 1 : 0))  * 16777619u; // se cachean normales?
    return s ? s : 1u; // 0 reservado para "sin cache"
}
// dimensiona el cache para el clip/rango ACTUAL; si la firma cambio libera el anterior y crea slots vacios (16B c/u;
// las posiciones/normales se allocan lazy). false si el rango es invalido. Aca es donde se "reserva al seleccionar".
static bool SkinCacheValidar(Mesh* m){
    unsigned sig = SkinCacheFirma(m);
    if (m->skinCacheSig == sig && !m->skinCache.empty()) return true; // ya valido para este clip
    m->LiberarSkinCache();                                            // libera el clip anterior (otro tamaño de memoria)
    if (EndFrame < StartFrame) return false;
    int stride = m->skinCacheSkip + 1; if (stride < 1) stride = 1;
    int slots = (EndFrame - StartFrame) / stride + 1;
    if (slots < 1 || slots > 100000) return false;                   // guarda de sanidad
    Mesh::SkinSnapshot vacio; vacio.pos = NULL; vacio.nor = NULL;
    m->skinCache.assign((size_t)slots, vacio);                       // slots vacios (se llenan al reproducir 1 vez)
    m->skinCacheStart = StartFrame; m->skinCacheEnd = EndFrame;
    m->skinCacheConLuz = m->skinConLuz; m->skinCacheSig = sig;
    return true;
}
// copia/interpola el snapshot de CurrentFrame a skinVertex(+skinNormals). true si el slot estaba bakeado.
static bool SkinCacheReproducir(Mesh* m){
    int idx = CurrentFrame - m->skinCacheStart;
    if (idx < 0 || CurrentFrame > m->skinCacheEnd) return false;
    int stride = m->skinCacheSkip + 1; if (stride < 1) stride = 1;
    int q = idx / stride, r = idx % stride;
    if (q < 0 || q >= (int)m->skinCache.size() || !m->skinCache[q].pos) return false; // slot todavia sin bakear
    int fc = m->vertexSize * 3;
    if (m->skinVertex && m->skinVertexCap != fc){ delete[] m->skinVertex; m->skinVertex = NULL; if (m->skinNormals){ delete[] m->skinNormals; m->skinNormals = NULL; } }
    if (!m->skinVertex){ m->skinVertex = new (std::nothrow) GLfloat[fc + 16]; if (!m->skinVertex){ m->skinVertexCap = 0; return false; } m->skinVertexCap = fc; }
    const Mesh::SkinSnapshot& A = m->skinCache[q];
    bool doN = m->skinCacheConLuz && A.nor;
    if (doN && !m->skinNormals){ m->skinNormals = new (std::nothrow) GLbyte[fc + 32]; if (!m->skinNormals) doN = false; }
    bool interp = (r != 0) && (q + 1 < (int)m->skinCache.size()) && m->skinCache[q+1].pos;
    if (!interp){
        for (int i = 0; i < fc; i++) m->skinVertex[i] = A.pos[i];                       // frame exacto -> copia directa
        if (doN) for (int i = 0; i < fc; i++) m->skinNormals[i] = A.nor[i];
    } else {
        const Mesh::SkinSnapshot& B = m->skinCache[q+1];                                 // intermedio -> lerp con el vecino
        float t = (float)r / (float)stride;
        for (int i = 0; i < fc; i++) m->skinVertex[i] = A.pos[i] + (B.pos[i] - A.pos[i]) * t;
        if (doN && B.nor) for (int i = 0; i < fc; i++){ float av=(float)A.nor[i], bv=(float)B.nor[i]; m->skinNormals[i] = (GLbyte)(av + (bv-av)*t); }
        else if (doN) for (int i = 0; i < fc; i++) m->skinNormals[i] = A.nor[i];
    }
    return true;
}
// guarda el resultado ACTUAL de skinVertex/skinNormals como snapshot del slot q. OOM-safe.
static void SkinCacheGuardar(Mesh* m, int q){
    if (q < 0 || q >= (int)m->skinCache.size() || m->skinCache[q].pos || !m->skinVertex) return;
    int fc = m->vertexSize * 3;
    GLfloat* p = new (std::nothrow) GLfloat[fc]; if (!p) return;      // sin RAM -> no cachear (se re-skinnea, sin crash)
    for (int i = 0; i < fc; i++) p[i] = m->skinVertex[i];
    GLbyte* nb = NULL;
    if (m->skinCacheConLuz && m->skinNormals){ nb = new (std::nothrow) GLbyte[fc]; if (nb) for (int i=0;i<fc;i++) nb[i] = m->skinNormals[i]; }
    m->skinCache[q].pos = p; m->skinCache[q].nor = nb;
}

void SkinearMesh(Mesh* m){
    if (!m || !m->skinArmature || !m->vertex || m->vertexSize <= 0) return;
    Armature* a = m->skinArmature;
    EvaluarPoseEsqueleto(a, CurrentFrame); // asegura skinMatrix de cada hueso al frame actual (cacheado)
    if (m->lastSkinFrame == CurrentFrame && m->skinVertex) return; // ya skinneado este frame
    // ---- CACHE de vertex-animation: reproducir el snapshot si esta bakeado; si no, skinnear y guardarlo (lazy) ----
    int slotAGuardar = -1;
    if (m->skinCacheOn){
        if (SkinCacheValidar(m)){
            if (SkinCacheReproducir(m)){ m->lastSkinFrame = CurrentFrame; return; } // HIT -> sin recomputar skinning
            int idx = CurrentFrame - m->skinCacheStart, stride = m->skinCacheSkip + 1; if (stride < 1) stride = 1;
            if (idx >= 0 && CurrentFrame <= m->skinCacheEnd && (idx % stride) == 0) slotAGuardar = idx / stride; // keyframe slot -> guardar tras skinnear
        }
    } else if (!m->skinCache.empty()) {
        m->LiberarSkinCache(); // el cache se apago -> liberar la memoria
    }
    m->lastSkinFrame = CurrentFrame;
    // OJO: vertexSize = cantidad de VERTICES (no de floats). Los arrays vertex/normals/skin* son vertexSize*3.
    int nv = m->vertexSize;      // vertices
    int fc = m->vertexSize * 3;  // floats/bytes de los arrays (3 por vertice)
    // REALLOC si el mesh cambio de tamaño (edit/apply modificador): sino skinVertex queda chico -> overflow -> crash
    if (m->skinVertex && m->skinVertexCap != fc) { delete[] m->skinVertex; m->skinVertex = NULL;
        if (m->skinNormals){ delete[] m->skinNormals; m->skinNormals = NULL; } }
    // PADDING: el driver GL (mesa) sube los client arrays con copias vectorizadas (AVX, 32 bytes/vez) y SOBRE-LEE el
    // final del array; +16 floats / +32 bytes de holgura para que el over-read no se salga del heap (cazado con ASan).
    if (!m->skinVertex) { m->skinVertex = new GLfloat[fc + 16]; m->skinVertexCap = fc; }
    for (int i = 0; i < fc; i++) m->skinVertex[i] = m->vertex[i]; // default = bind (verts sin peso quedan)
    // NORMALES: rotarlas por los huesos (iluminacion correcta al doblar). Solo si hay luz (skinConLuz) y normales de bind.
    bool doN = m->skinConLuz && m->normals != NULL;
    if (doN){ if (!m->skinNormals) m->skinNormals = new GLbyte[fc + 32];
        for (int i = 0; i < fc; i++) m->skinNormals[i] = m->normals[i]; } // default = bind
    if ((int)m->vertCtrlPoint.size() < nv) return; // sin mapeo render-vert -> control-point: no skinnear (no romper)

    // ---- CACHE CSR de pesos por CONTROL-POINT (no por render-vert). Se arma UNA sola vez y se reusa cada frame. Un
    //      asset de juego duplica cada control-point en varios render-verts por seams de UV/normal/material (banana:
    //      42840 render pero 7186 CP = 6x). Como los render-verts de un mismo CP comparten bind + pesos, el blend caro
    //      de matrices se hace 1 vez POR CP y se escalea (6x menos matematica). Invalida por firma de topologia
    //      (skinFlatSig): vertexSize, armature, #huesos, #grupos + puntero/tamaño de cada grupo, #control-points. ----
    unsigned sig = (unsigned)nv * 2654435761u;
    sig ^= (unsigned)(size_t)a; sig = sig*31u + (unsigned)a->bones.size();
    sig = sig*31u + (unsigned)m->vertexGroups.size() + (unsigned)m->vertCtrlPoint.size()*131u;
    sig = sig*2654435761u + m->skinGeomVersion; // regenerar geometria (GenerarRender/CalcularBordes) invalida aunque nV no cambie;
                                                // como el mapeo render->CP (y por ende nCtrl) se rehace ahi, cubre el conteo de CP.
    // los NOMBRES mapean grupo->hueso (boneDe.find(vg->nombre)); si se renombra un grupo o un hueso, el mapeo cambia
    // pero punteros/tamaños no -> hay que hashear los nombres (y hasSkin) sino el CSR queda stale. Barato (~cientos de chars).
    for (size_t b = 0; b < a->bones.size(); b++){ const std::string& nm=a->bones[b].name;
        for (size_t c=0;c<nm.size();c++) sig = sig*31u + (unsigned char)nm[c];
        sig = sig*2u + (a->bones[b].hasSkin?1u:0u); }
    for (size_t g = 0; g < m->vertexGroups.size(); g++){
        sig ^= (unsigned)(size_t)m->vertexGroups[g];
        sig = sig*31u + (unsigned)m->vertexGroups[g]->verts.size();
        const std::string& gn=m->vertexGroups[g]->nombre;
        for (size_t c=0;c<gn.size();c++) sig = sig*31u + (unsigned char)gn[c];
    }
    if (m->skinFlatSig != sig || m->skinCpOff.empty()){
        // nCtrl (cantidad de control-points) SOLO se recomputa al reconstruir (cambia con la geometria, ya en el sig via
        // skinGeomVersion). Fuera de aca el hot-loop usa m->skinNCtrl cacheado -> sin loop O(nv) por frame.
        int nCtrl = 0;
        for (int ri = 0; ri < nv; ri++){ int c = m->vertCtrlPoint[ri]; if (c + 1 > nCtrl) nCtrl = c + 1; }
        m->skinNCtrl = nCtrl;
        std::map<std::string,int> boneDe;                         // nombre de hueso -> indice (solo al reconstruir)
        for (size_t b = 0; b < a->bones.size(); b++) boneDe[a->bones[b].name] = (int)b;
        // control-point -> lista de (hueso, peso), solo huesos con skin. Los vertex groups vienen por control-point del FBX.
        std::map<int, std::vector<std::pair<int,float> > > cpW;
        for (size_t g = 0; g < m->vertexGroups.size(); g++){
            VertexGroup* vg = m->vertexGroups[g];
            std::map<std::string,int>::iterator it = boneDe.find(vg->nombre);
            if (it == boneDe.end()) continue;
            int b = it->second; if (b < 0 || b >= (int)a->bones.size() || !a->bones[b].hasSkin) continue;
            for (size_t j = 0; j < vg->verts.size() && j < vg->pesos.size(); j++)
                cpW[vg->verts[j]].push_back(std::make_pair(b, vg->pesos[j]));
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
        m->skinCpMat.assign((size_t)(nCtrl>0?nCtrl:0)*12, 0.0f); // buffer temp por-frame (matriz mezclada por CP; reusado)
        m->skinFlatSig = sig;
    }

    // ---- PASE 1: por CONTROL-POINT con peso, mezcla la matriz de skin (4x3) UNA vez -> skinCpMat[c]. Este es el blend
    //      CARO (leer + acumular cada skinMatrix de hueso) y ahora corre nCtrl veces (7186) en vez de nv (42840): 6x
    //      menos. skinMatrix es el delta en espacio NODO = espacio de la geometria -> se aplica DIRECTO. Los pesos ya
    //      suman 1. Como los render-verts de un CP comparten pesos+huesos, comparten la MISMA matriz mezclada. ----
    const int    nCtrl = m->skinNCtrl;   // cacheado en el ultimo rebuild (no se recomputa por frame)
    const int    nb    = (int)a->bones.size();
    const int*   cpOff = &m->skinCpOff[0];
    const int*   cpBn  = m->skinCpBone.empty() ? NULL : &m->skinCpBone[0];
    const float* cpWt  = m->skinCpW.empty()    ? NULL : &m->skinCpW[0];
    float*       cpMat = m->skinCpMat.empty()  ? NULL : &m->skinCpMat[0];
    for (int c = 0; c < nCtrl; c++){
        int s = cpOff[c], e = cpOff[c+1];
        if (s == e) continue;                       // CP sin peso -> sus render-verts quedan en bind
        float* M = &cpMat[c*12];
        M[0]=M[1]=M[2]=M[3]=M[4]=M[5]=M[6]=M[7]=M[8]=M[9]=M[10]=M[11]=0.0f;
        for (int k = s; k < e; k++){
            int bi = cpBn[k]; if (bi<0||bi>=nb) continue; // defensa si el rig encogio (la firma del CSR igual lo invalidaria)
            const float* mm = a->bones[bi].skinMatrix.m; float w = cpWt[k];
            M[0]+=mm[0]*w;  M[1]+=mm[1]*w;  M[2]+=mm[2]*w;    // columna X (rotacion)
            M[3]+=mm[4]*w;  M[4]+=mm[5]*w;  M[5]+=mm[6]*w;    // columna Y
            M[6]+=mm[8]*w;  M[7]+=mm[9]*w;  M[8]+=mm[10]*w;   // columna Z
            M[9]+=mm[12]*w; M[10]+=mm[13]*w; M[11]+=mm[14]*w; // traslacion
        }
    }
    // ---- PASE 2: por RENDER-vert, aplica la matriz mezclada de su CP a SU PROPIO bind (posicion) y normal. No usa
    //      un representante ni asume que los render-verts de un CP compartan bind position: cada uno usa vertex[ri] y
    //      normals[ri] -> resultado identico al loop por render-vert anterior (LBS lineal), para CUALQUIER malla.
    //      Barato: aca NO hay loop de influencias (esa parte, la cara, ya se hizo 1 vez por CP en el Pase 1). ----
    for (int ri = 0; ri < nv; ri++){
        int c = m->vertCtrlPoint[ri];
        if (c < 0 || c >= nCtrl) continue;          // sin CP -> queda en bind
        if (cpOff[c] == cpOff[c+1]) continue;       // CP sin peso -> queda en bind
        const float* M = &cpMat[c*12];
        float vx=m->vertex[ri*3], vy=m->vertex[ri*3+1], vz=m->vertex[ri*3+2];
        float px = M[0]*vx + M[3]*vy + M[6]*vz + M[9];
        float py = M[1]*vx + M[4]*vy + M[7]*vz + M[10];
        float pz = M[2]*vx + M[5]*vy + M[8]*vz + M[11];
        // POSICION: guard NaN/inf (una matriz degenerada no debe mandar el vert al infinito -> queda en bind)
        if (px==px && py==py && pz==pz && px<1e6f&&px>-1e6f && py<1e6f&&py>-1e6f && pz<1e6f&&pz>-1e6f){
            m->skinVertex[ri*3]=px; m->skinVertex[ri*3+1]=py; m->skinVertex[ri*3+2]=pz; }
        // NORMAL: la 3x3 mezclada del CP * la normal de bind del render-vert (distinta por seam), FAST INVERSE SQRT
        if (doN){
            float nx=m->normals[ri*3]/127.0f, ny=m->normals[ri*3+1]/127.0f, nz=m->normals[ri*3+2]/127.0f;
            float anx = M[0]*nx + M[3]*ny + M[6]*nz;
            float any = M[1]*nx + M[4]*ny + M[7]*nz;
            float anz = M[2]*nx + M[5]*ny + M[8]*nz;
            float l2 = anx*anx + any*any + anz*anz;
            if (l2 > 1e-12f && l2==l2){ float sc = 127.0f * FastInvSqrt(l2);
                m->skinNormals[ri*3]=(GLbyte)(anx*sc); m->skinNormals[ri*3+1]=(GLbyte)(any*sc); m->skinNormals[ri*3+2]=(GLbyte)(anz*sc); }
        }
    }
    // CACHE: si este frame era un keyframe slot no bakeado, guardar el resultado del skinning (lazy: se paga 1 vez)
    if (slotAGuardar >= 0) SkinCacheGuardar(m, slotAGuardar);
}

// ===== gestion de clips (crear/borrar/mover el activo), igual que los vertex groups =====
void CrearAnimacion(Armature* a){
    if (!a) return;
    // nombre unico "Animation" / "Animation.NNN"
    std::string base = "Animation", nombre = base; int suf = 0;
    for (;;){ bool choca = false;
        for (size_t i = 0; i < a->animations.size(); i++)
            if (a->animations[i]->name == nombre){ choca = true; break; }
        if (!choca) break;
        ++suf; char b[16]; sprintf(b, ".%03d", suf); nombre = base + b; }
    a->animations.push_back(new SkeletalAnimation(nombre));
    a->animActiva = (int)a->animations.size() - 1;
}

// pone (o actualiza) un keyframe en 'frame' con valor v, manteniendo la lista ordenada por frame.
static void SetKey(AnimProperty& ap, int frame, const Vector3& v){
    for (size_t i = 0; i < ap.keyframes.size(); i++) if (ap.keyframes[i].frame == frame){
        ap.keyframes[i].valueX = v.x; ap.keyframes[i].valueY = v.y; ap.keyframes[i].valueZ = v.z; return; }
    keyFrame kf; kf.frame = frame; kf.valueX = v.x; kf.valueY = v.y; kf.valueZ = v.z;
    size_t pos = 0; while (pos < ap.keyframes.size() && ap.keyframes[pos].frame < frame) pos++;
    ap.keyframes.insert(ap.keyframes.begin() + pos, kf);
}
// INSERT KEYFRAME (i): guarda la POSE actual (poseT/R/S) de los huesos SELECCIONADOS en la curva del clip activo,
// en CurrentFrame. Es lo que hace permanente la pose (antes de esto se ve pero no se guarda). Crea un clip si no hay.
void InsertarKeyframeEsqueleto(Armature* a){
    if (!a || a->bones.empty()) return;
    if (a->animActiva < 0 || a->animActiva >= (int)a->animations.size()) CrearAnimacion(a); // asegurar clip activo
    if (a->animActiva < 0 || a->animActiva >= (int)a->animations.size()) return;
    SkeletalAnimation* clip = a->animations[a->animActiva];
    int nSel = 0;
    for (size_t i = 0; i < a->bones.size(); i++){
        W3dBone& b = a->bones[i];
        // huesos seleccionados; si no hay ninguno seleccionado, el activo
        if (!b.select && (int)i != a->boneActivo) continue;
        if (!b.select && a->boneActivo < 0) continue;
        BoneTrack& tr = clip->TrackDe((int)i);
        SetKey(tr.PropertyDe(AnimPosition), CurrentFrame, b.poseT);
        SetKey(tr.PropertyDe(AnimRotation), CurrentFrame, b.poseR);
        SetKey(tr.PropertyDe(AnimScale),    CurrentFrame, b.poseS);
        nSel++;
    }
    if (nSel == 0) return;
    if (CurrentFrame > clip->endFrame) clip->endFrame = CurrentFrame; // extender el rango del clip
    if (CurrentFrame < clip->startFrame || clip->startFrame < 1) clip->startFrame = CurrentFrame < 1 ? 1 : CurrentFrame;
    a->poseDirty = false; a->lastPoseFrame = -999999; // re-evaluar desde la curva (ya con el keyframe nuevo)
}

void BorrarAnimacionActiva(Armature* a){
    if (!a) return;
    int i = a->animActiva;
    if (i < 0 || i >= (int)a->animations.size()) return;
    delete a->animations[i];
    a->animations.erase(a->animations.begin() + i);
    if (a->animActiva >= (int)a->animations.size()) a->animActiva = (int)a->animations.size() - 1;
}

void MoverAnimacionActiva(Armature* a, int dir){
    if (!a) return;
    int i = a->animActiva, j = i + dir;
    if (i < 0 || i >= (int)a->animations.size() || j < 0 || j >= (int)a->animations.size()) return;
    SkeletalAnimation* t = a->animations[i];
    a->animations[i] = a->animations[j];
    a->animations[j] = t;
    a->animActiva = j;
}
