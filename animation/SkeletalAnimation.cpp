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
void SkinearMesh(Mesh* m){
    if (!m || !m->skinArmature || !m->vertex || m->vertexSize <= 0) return;
    Armature* a = m->skinArmature;
    EvaluarPoseEsqueleto(a, CurrentFrame); // asegura skinMatrix de cada hueso al frame actual (cacheado)
    if (m->lastSkinFrame == CurrentFrame && m->skinVertex) return; // ya skinneado este frame
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

    // ---- CACHE CSR de pesos (bone,weight) por RENDER-vert. Se arma UNA sola vez y se reusa cada frame. Antes se
    //      reconstruia un std::map por frame sobre TODOS los verts (banana=42840) -> ~5ms/frame de allocs. Con el CSR
    //      el costo por frame es solo la matematica (matriz*vertice). Invalida por firma de topologia (skinFlatSig):
    //      vertexSize, armature, #huesos, #grupos + puntero/tamaño de cada grupo, #control-points. Los pesos NO se
    //      editan in-place en el editor (weight paint es solo visual; import/duplicar/undo cambian punteros o tamaños). ----
    unsigned sig = (unsigned)nv * 2654435761u;
    sig ^= (unsigned)(size_t)a; sig = sig*31u + (unsigned)a->bones.size();
    sig = sig*31u + (unsigned)m->vertexGroups.size() + (unsigned)m->vertCtrlPoint.size()*131u;
    sig = sig*2654435761u + m->skinGeomVersion; // regenerar geometria (GenerarRender/CalcularBordes) invalida aunque nV no cambie
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
    if (m->skinFlatSig != sig || (int)m->skinFlatOff.size() != nv+1){
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
        // divide por vertice. Los cp con peso total ~0 se DESCARTAN (el vertex queda en bind, como antes hacia el
        // wsum<=0.0001 de SkinearMesh). Mismo resultado, menos matematica por frame.
        for (std::map<int, std::vector<std::pair<int,float> > >::iterator it = cpW.begin(); it != cpW.end(); ){
            std::vector<std::pair<int,float> >& lst = it->second;
            float sum = 0.0f; for (size_t k = 0; k < lst.size(); k++) sum += lst[k].second;
            if (sum <= 1e-6f){ std::map<int, std::vector<std::pair<int,float> > >::iterator er = it++; cpW.erase(er); continue; }
            float inv = 1.0f/sum; for (size_t k = 0; k < lst.size(); k++) lst[k].second *= inv;
            ++it;
        }
        // aplanar a CSR por render-vert (offset[ri]..offset[ri+1] = rango de (bone,weight) del vert ri)
        m->skinFlatOff.assign(nv+1, 0);
        for (int ri = 0; ri < nv; ri++){
            std::map<int, std::vector<std::pair<int,float> > >::iterator it = cpW.find(m->vertCtrlPoint[ri]);
            m->skinFlatOff[ri+1] = (it==cpW.end()) ? 0 : (int)it->second.size();
        }
        for (int ri = 0; ri < nv; ri++) m->skinFlatOff[ri+1] += m->skinFlatOff[ri]; // prefix sum
        int total = m->skinFlatOff[nv];
        m->skinFlatBone.assign(total > 0 ? total : 0, 0); m->skinFlatW.assign(total > 0 ? total : 0, 0.0f);
        for (int ri = 0; ri < nv; ri++){
            std::map<int, std::vector<std::pair<int,float> > >::iterator it = cpW.find(m->vertCtrlPoint[ri]);
            if (it == cpW.end()) continue;
            int base = m->skinFlatOff[ri];
            for (size_t k = 0; k < it->second.size(); k++){ m->skinFlatBone[base+(int)k]=it->second[k].first; m->skinFlatW[base+(int)k]=it->second[k].second; }
        }
        m->skinFlatSig = sig;
    }

    // ---- por RENDER-vert: blend de los huesos que lo pesan (CSR, sin mapas ni allocs por frame). skinMatrix es el
    //      delta en espacio NODO = espacio de la geometria del mesh -> se aplica DIRECTO (sin conjugar). ----
    const int*   off = &m->skinFlatOff[0];
    const int*   fb  = m->skinFlatBone.empty() ? NULL : &m->skinFlatBone[0];
    const float* fw  = m->skinFlatW.empty()    ? NULL : &m->skinFlatW[0];
    const int nb = (int)a->bones.size();
    for (int ri = 0; ri < nv; ri++){
        int s = off[ri], e = off[ri+1];
        if (s == e) continue; // sin peso -> queda en bind
        float vx=m->vertex[ri*3], vy=m->vertex[ri*3+1], vz=m->vertex[ri*3+2];
        float nx=0,ny=0,nz=0; if (doN){ nx=m->normals[ri*3]/127.0f; ny=m->normals[ri*3+1]/127.0f; nz=m->normals[ri*3+2]/127.0f; }
        float ax=0,ay=0,az=0, anx=0,any=0,anz=0;
        // pesos YA normalizados a suma 1 (en el build del CSR) -> sin wsum ni division. UN solo loop de influencias:
        // posicion (M*v, inline sin temporales Vector3) + normal (3x3) juntas, leyendo skinMatrix y el peso 1 sola vez.
        for (int k = s; k < e; k++){
            int bi=fb[k]; if (bi<0||bi>=nb) continue; // defensa si el rig encogio (la firma del CSR igual lo invalidaria)
            const float* mm = a->bones[bi].skinMatrix.m; float w = fw[k];
            ax += (mm[0]*vx + mm[4]*vy + mm[8]*vz + mm[12]) * w;
            ay += (mm[1]*vx + mm[5]*vy + mm[9]*vz + mm[13]) * w;
            az += (mm[2]*vx + mm[6]*vy + mm[10]*vz + mm[14]) * w;
            if (doN){
                anx += (mm[0]*nx + mm[4]*ny + mm[8]*nz) * w;
                any += (mm[1]*nx + mm[5]*ny + mm[9]*nz) * w;
                anz += (mm[2]*nx + mm[6]*ny + mm[10]*nz) * w;
            }
        }
        // POSICION: guard NaN/inf (una matriz degenerada no debe mandar el vert al infinito -> queda en bind)
        if (ax==ax && ay==ay && az==az && ax<1e6f&&ax>-1e6f && ay<1e6f&&ay>-1e6f && az<1e6f&&az>-1e6f){
            m->skinVertex[ri*3]=ax; m->skinVertex[ri*3+1]=ay; m->skinVertex[ri*3+2]=az; }
        // NORMAL: normalizar a 127 con FAST INVERSE SQRT (evita el sqrtf + la division por vertice)
        if (doN){
            float l2 = anx*anx + any*any + anz*anz;
            if (l2 > 1e-12f && l2==l2){ float sc = 127.0f * FastInvSqrt(l2);
                m->skinNormals[ri*3]=(GLbyte)(anx*sc); m->skinNormals[ri*3+1]=(GLbyte)(any*sc); m->skinNormals[ri*3+2]=(GLbyte)(anz*sc); }
        }
    }
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
