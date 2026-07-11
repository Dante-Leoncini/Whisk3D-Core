#include "objects/Armature.h"
#include "animation/SkeletalAnimation.h" // tipo completo para liberar los clips en el destructor

// El Armature en el CORE es SOLO datos (huesos + jerarquia). El dibujo del esqueleto (lineas azules por encima de
// todo) es un EXTRA del editor: se hace en una pasada aparte DESPUES de renderizar la escena, para que las lineas
// queden encima del mesh hijo (si se dibujara aca, durante el recorrido del arbol, el mesh hijo -que se renderiza
// despues- las taparia). Ver RenderArmaturasEncima() en el editor.
void Armature::RenderObject() {}

Armature::~Armature() {
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
Vector3 Armature::PuntoFoco() const {
    Vector3 mn, mx;
    if (!BonesBBoxLocal(bones, mn, mx)) return GetGlobalPosition();
    Matrix4 W; GetWorldMatrix(W);
    return W * ((mn + mx) * 0.5f); // centro de los huesos en MUNDO
}
float Armature::RadioFoco() const {
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
