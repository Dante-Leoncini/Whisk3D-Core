#include "objects/Armature.h"
#include "animation/SkeletalAnimation.h" // tipo completo para liberar los clips en el destructor
#include "animation/W3dAnimSet.h"        // clips de un animset COMPARTIDO: se sueltan, no se borran

// El Armature en el CORE es SOLO datos (huesos + jerarquia). El dibujo del esqueleto (lineas azules por encima de
// todo) es un EXTRA del editor: se hace en una pasada aparte DESPUES de renderizar la escena, para que las lineas
// queden encima del mesh hijo (si se dibujara aca, durante el recorrido del arbol, el mesh hijo -que se renderiza
// despues- las taparia). Ver RenderArmaturesOnTop() en el editor.
void Armature::RenderObject() {}

Armature::~Armature() {
    // si este armature era la animacion activa a nivel app, volver a "Scene" (evita puntero colgante)
    extern int ActiveAnimKind; extern Armature* ActiveAnimArm;
    if (ActiveAnimArm == this) { ActiveAnimArm = NULL; ActiveAnimKind = 0; }
    // clips de un ANIMSET compartido: son del recurso (los usan otros armatures) -> solo se suelta la referencia
    if (animsRec) { W3dArmatureAnimsRelease(this); return; }
    for (size_t i = 0; i < animations.size(); i++) delete animations[i];
}

// bbox LOCAL de los huesos (poseHead/poseTail = lo que se dibuja). Devuelve false si no hay huesos.
static bool BonesBBoxLocal(const std::vector<W3dBone>& bones, Vector3& mn, Vector3& mx){
    if (bones.empty()) return false;
    mn = mx = bones[0].poseHead;
    for (size_t i = 0; i < bones.size(); i++){
        const Vector3 pts[2] = { bones[i].poseHead, bones[i].poseTail };
        for (int k = 0; k < 2; k++){ const Vector3& p = pts[k];
            if (p.x<mn.x)mn.x=p.x; if (p.y<mn.y)mn.y=p.y; if (p.z<mn.z)mn.z=p.z;
            if (p.x>mx.x)mx.x=p.x; if (p.y>mx.y)mx.y=p.y; if (p.z>mx.z)mx.z=p.z; }
    }
    return true;
}
Vector3 Armature::PointFocus() const {
    Vector3 mn, mx;
    if (!BonesBBoxLocal(bones, mn, mx)) return GetGlobalPosition();
    Matrix4 W; GetWorldMatrix(W);
    return W * ((mn + mx) * 0.5f); // centro de los huesos en MUNDO
}
float Armature::FocusRadius() const {
    Vector3 mn, mx;
    if (!BonesBBoxLocal(bones, mn, mx)) return 0.0f;
    Matrix4 W; GetWorldMatrix(W);
    Vector3 cW = W * ((mn + mx) * 0.5f);
    // esquinas de la bbox a mundo -> radio = max distancia al centro
    float r = 0.0f;
    for (int i = 0; i < 8; i++){
        Vector3 c((i&1)?mx.x:mn.x, (i&2)?mx.y:mn.y, (i&4)?mx.z:mn.z);
        float d = (Vector3(W*c) - cW).Length(); if (d > r) r = d;
    }
    return r;
}

// ============================================================================
//  CONSTRAINTS DE HUESO: la puerta de refs y los accesores del panel (contrato en Armature.h)
//
//  Los slots de OwnRef son 2 por constraint (0 = target, 1 = pole) recorriendo huesos -> stack, en
//  orden. Se recorre en cada consulta (sin indice cacheado): el stack cambia con Add/Remove/Move y con
//  el undo de huesos, y un indice cacheado que quede viejo es exactamente la corrupcion muda que la
//  puerta existe para evitar. Son pocos constraints: lineal alcanza.
// ============================================================================
W3dConstraint* Armature::BoneConstraintOfRef(int i, int* which) const {
    if (i < 0) return NULL;
    int k = i;
    for (size_t b = 0; b < bones.size(); b++) {
        const int n = (int)bones[b].constraints.list.size();
        if (k < 2 * n) { if (which) *which = k & 1; return bones[b].constraints.list[k >> 1]; }
        k -= 2 * n;
    }
    return NULL;
}
int Armature::OwnRefs() const {
    int n = 0;
    for (size_t b = 0; b < bones.size(); b++) n += 2 * (int)bones[b].constraints.list.size();
    return n;
}
Object* Armature::OwnRef(int i) const {
    int w = 0; W3dConstraint* c = BoneConstraintOfRef(i, &w);
    if (!c) return NULL;
    return w ? c->poleObj : c->sourceObj;
}
void Armature::SetOwnRef(int i, Object* o){
    int w = 0; W3dConstraint* c = BoneConstraintOfRef(i, &w);
    if (!c) return;
    if (w) { c->poleObj = o; c->poleBoneCache = -1; } else { c->sourceObj = o; c->boneCache = -1; }
    poseDirty = true;   // el target cambio (se fue o volvio): la pose se recalcula
}
std::string* Armature::RefOwnName(int i){
    int w = 0; W3dConstraint* c = BoneConstraintOfRef(i, &w);
    if (!c) return NULL;
    return w ? &c->poleName : &c->sourceName;
}
W3dConstraint* Armature::BoneConstraintBySerial(unsigned int s) const {
    if (!s) return NULL;
    for (size_t b = 0; b < bones.size(); b++)
        for (size_t i = 0; i < bones[b].constraints.list.size(); i++)
            if (bones[b].constraints.list[i] && bones[b].constraints.list[i]->serial.v == s) return bones[b].constraints.list[i];
    return NULL;
}
bool Armature::HasBoneConstraints() const {
    for (size_t b = 0; b < bones.size(); b++) if (!bones[b].constraints.list.empty()) return true;
    return false;
}
int Armature::BoneConstraintsCount() const {
    if (activeBone < 0 || activeBone >= (int)bones.size()) return 0;
    return (int)bones[activeBone].constraints.list.size();
}
std::string Armature::BoneConstraintName(int i) const {
    if (activeBone < 0 || activeBone >= (int)bones.size()) return std::string();
    const std::vector<W3dConstraint*>& L = bones[activeBone].constraints.list;
    return (i >= 0 && i < (int)L.size() && L[i]) ? L[i]->name : std::string();
}
void Armature::SetActiveBoneConstraint(int i){
    if (activeBone < 0 || activeBone >= (int)bones.size()) return;
    W3dBoneConstraints& bc = bones[activeBone].constraints;
    bc.active = (i >= 0 && i < (int)bc.list.size()) ? i : -1;
}
int Armature::ActiveBoneConstraint() const {
    if (activeBone < 0 || activeBone >= (int)bones.size()) return -1;
    const W3dBoneConstraints& bc = bones[activeBone].constraints;
    return (bc.active >= 0 && bc.active < (int)bc.list.size()) ? bc.active : -1;
}
