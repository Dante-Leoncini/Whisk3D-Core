#include "animation/SkeletalAnimation.h"
#include "objects/Armature.h"
#include "objects/Mesh.h"
#include "math/Matrix4.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <map>
#include <utility>

// ===== FK: evaluar la pose del esqueleto en un frame =====
static const float TL_PI = 3.14159265358979f;
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
        // FALLBACK segmentado (solo para el path FK-rest, ej LISA sin TransformLink): si la pieza esta en el espacio
        // LOCAL del hueso (bind ~0 pero el hueso lejos del origen), skinA=identidad -> skinMatrix=world[bone] la ubica.
        float tlN = Vector3(b.bind.m[12], b.bind.m[13], b.bind.m[14]).Length();
        float rnN = Vector3(restWorldNode.m[12], restWorldNode.m[13], restWorldNode.m[14]).Length();
        if (tlN <= 1.0f && rnN > 5.0f) b.skinA.Identity();
        b.tlNode = NyInv * b.bind * Ny;
        b.skinMatrix.Identity();
        totalSkin++;
        // TransformLink valido = tiene traslacion real (no cero). LISA: casi todos en cero -> invalido -> FK-rest.
        if (Vector3(b.bind.m[12], b.bind.m[13], b.bind.m[14]).Length() > 1.0f) conBind++;
    }
    // usar el bind real si la MAYORIA de los huesos tienen TransformLink con datos (banana si; LISA no)
    a->skinUsaBind = (totalSkin > 0 && conBind * 2 > totalSkin);
}

bool g_skelAnimPreview = true; // ON: FK de la animacion (para rigs FBX; los armatures manuales se dibujan en bind)

// los transforms LOCALES del FBX vienen en espacio Z-up (nodo), pero el resto de la armature (bind head/tail,
// malla) esta en Y-up. Se convierte la salida del FK: (x,y,z) Z-up -> (x, z, -y) Y-up.
static Vector3 NodeToYup(const Vector3& v){ return Vector3(v.x, v.z, -v.y); }

void EvaluarPoseEsqueleto(Armature* a, int frame){
    if (!a) return;
    // CACHE: si el frame y el clip no cambiaron, la pose ya esta calculada (no recalcular en frames repetidos).
    // "solo recalcula cuando ocurre el salto" (pedido Dante): la UI a 60fps repite el frame de animacion sin recomputar.
    if (a->lastPoseFrame == frame && a->lastPoseAnim == a->animActiva) return;
    a->lastPoseFrame = frame; a->lastPoseAnim = a->animActiva;
    // por defecto: rest (poseHead/poseTail = head/tail bind)
    for (size_t i = 0; i < a->bones.size(); i++){ a->bones[i].poseHead = a->bones[i].head; a->bones[i].poseTail = a->bones[i].tail; }
    // FK solo para rigs FBX (con transforms de rest). Los armatures MANUALES (hasRest=false) se muestran en bind.
    bool fbxRig = !a->bones.empty() && a->bones[0].hasRest;
    if (!g_skelAnimPreview || !fbxRig) return;
    SkeletalAnimation* clip = (a->animActiva >= 0 && a->animActiva < (int)a->animations.size()) ? a->animations[a->animActiva] : NULL;

    size_t N = a->bones.size();
    std::vector<Matrix4> local(N), world(N);
    // matriz LOCAL de cada hueso: T/R/S = valor de la CURVA (si el canal esta animado) o el rest Lcl del hueso
    for (size_t i = 0; i < N; i++){
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
        local[i] = LocalMat(T, R, S, b.preRot, b.rotOrder);
    }
    // MUNDO por hueso; head animado en espacio nodo -> Y-up (el Object de la armature aplica la escala 0.01 al dibujar)
    std::vector<Vector3> headNode(N), tailNode(N);
    for (size_t i = 0; i < N; i++){
        world[i] = WorldMat(a->bones, local, (int)i); // FK NORMAL: el MOVIMIENTO correcto (no se toca)
        // SKINNING: delta rigido rest->anim (identidad en rest). Es lo que mueve la malla; se deja IGUAL que siempre.
        if (a->bones[i].hasSkin)
            a->bones[i].skinMatrix = world[i] * (g_skinFormula == 1 ? a->bones[i].skinA : a->bones[i].skinInvBind);
        // DISPLAY del hueso: si el TransformLink es valido, se dibuja anclado al bind REAL del FBX (D * tlNode) para que
        // el esqueleto COINCIDA con la malla (que esta authored a ese bind). El MOVIMIENTO es el mismo delta D que la
        // malla -> se mueven juntos. Sin TransformLink (LISA), FK normal.
        if (a->skinUsaBind && a->bones[i].hasSkin){
            Matrix4 D = world[i] * a->bones[i].skinInvBind;  // delta rigido rest->anim
            Matrix4 wDisp = D * a->bones[i].tlNode;          // aplicado al bind real (TransformLink en espacio nodo)
            headNode[i] = wDisp * Vector3(0,0,0);
        } else {
            headNode[i] = world[i] * Vector3(0,0,0);
        }
    }
    // tails: hueso con hijo -> tail = head del 1er hijo (conectados); hoja -> extender en la direccion del hueso
    std::vector<int> primerHijo(N, -1);
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
    // nombre de hueso -> indice
    std::map<std::string,int> boneDe;
    for (size_t b = 0; b < a->bones.size(); b++) boneDe[a->bones[b].name] = (int)b;
    // skinMatrix es el delta en espacio NODO = espacio de la geometria del mesh -> se aplica DIRECTO (sin conjugar)
    std::vector<Matrix4> SL(a->bones.size());
    std::vector<char> hasSL(a->bones.size(), 0);
    for (size_t b = 0; b < a->bones.size(); b++) if (a->bones[b].hasSkin){ SL[b] = a->bones[b].skinMatrix; hasSL[b] = 1; }
    // CONTROL-POINT -> lista de (hueso, peso). Los vertex groups vienen indexados por control-point del FBX.
    std::map<int, std::vector<std::pair<int,float> > > cpW;
    for (size_t g = 0; g < m->vertexGroups.size(); g++){
        VertexGroup* vg = m->vertexGroups[g];
        std::map<std::string,int>::iterator it = boneDe.find(vg->nombre);
        if (it == boneDe.end()) continue;
        int b = it->second; if (!hasSL[b]) continue;
        for (size_t j = 0; j < vg->verts.size() && j < vg->pesos.size(); j++)
            cpW[vg->verts[j]].push_back(std::make_pair(b, vg->pesos[j]));
    }
    // por RENDER-vert: su control-point -> blend de los huesos que lo pesan
    for (int ri = 0; ri < nv; ri++){
        int cp = m->vertCtrlPoint[ri];
        std::map<int, std::vector<std::pair<int,float> > >::iterator it = cpW.find(cp);
        if (it == cpW.end()) continue; // sin peso -> queda en bind
        Vector3 v(m->vertex[ri*3], m->vertex[ri*3+1], m->vertex[ri*3+2]);
        Vector3 acc(0,0,0); float wsum = 0.0f;
        for (size_t k = 0; k < it->second.size(); k++){ int b = it->second[k].first; float w = it->second[k].second;
            acc += (SL[b] * v) * w; wsum += w; }
        if (wsum > 0.0001f){ acc = acc * (1.0f/wsum);
            // GUARD: si una matriz degenerada mandara el vert a NaN/infinito, dejar el original (no romper el modelo)
            bool ok = (acc.x==acc.x && acc.y==acc.y && acc.z==acc.z) && // no NaN
                      (acc.x<1e6f&&acc.x>-1e6f && acc.y<1e6f&&acc.y>-1e6f && acc.z<1e6f&&acc.z>-1e6f);
            if (ok){ m->skinVertex[ri*3]=acc.x; m->skinVertex[ri*3+1]=acc.y; m->skinVertex[ri*3+2]=acc.z; } }
        // NORMAL: rotar la normal de bind por los MISMOS huesos/pesos (solo la parte rotacional 3x3, sin traslacion).
        if (doN){
            Vector3 n(m->normals[ri*3]/127.0f, m->normals[ri*3+1]/127.0f, m->normals[ri*3+2]/127.0f);
            Vector3 nAcc(0,0,0);
            for (size_t k = 0; k < it->second.size(); k++){ int b = it->second[k].first; float w = it->second[k].second;
                const float* mm = SL[b].m; // column-major: rotar direccion = 3x3 superior (sin m[12..14])
                nAcc.x += (mm[0]*n.x + mm[4]*n.y + mm[8]*n.z) * w;
                nAcc.y += (mm[1]*n.x + mm[5]*n.y + mm[9]*n.z) * w;
                nAcc.z += (mm[2]*n.x + mm[6]*n.y + mm[10]*n.z) * w; }
            float ln = sqrtf(nAcc.x*nAcc.x + nAcc.y*nAcc.y + nAcc.z*nAcc.z);
            if (ln > 1e-6f && ln==ln){ float s = 127.0f/ln;
                m->skinNormals[ri*3]=(GLbyte)(nAcc.x*s); m->skinNormals[ri*3+1]=(GLbyte)(nAcc.y*s); m->skinNormals[ri*3+2]=(GLbyte)(nAcc.z*s); }
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
