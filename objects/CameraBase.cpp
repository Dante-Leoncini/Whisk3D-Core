#include "CameraBase.h"
#include <math.h> // C puro: compila en RVCT (Symbian) y PC por igual

Vector3 g_renderCamPos(0, 0, 0); // la setea el viewport antes de renderizar (chrome equirect)
Vector3 g_renderCamRight(1, 0, 0);   // base de la camara en mundo (matcap por software / sphere-map)
Vector3 g_renderCamUp(0, 1, 0);
Vector3 g_renderCamForward(0, 0, -1);
Vector3 g_renderLightPos(4, 5, 4); // luz principal en mundo (normal mapping); el viewport la actualiza por frame
Vector3 g_renderLightColor(1, 1, 1); // color (diffuse) de la luz principal -> tiñe el normal map

CameraBase::CameraBase() {
    // (inicializadores en el ctor: dialecto C++03, sin in-class init)
    pos   = Vector3(0, 0, 0);
    fov   = 45.0f;
    nearZ = 0.1f;
    farZ  = 1000.0f;
    // rot queda en la identidad por su propio constructor por defecto
}

// VISTA: inversa del transform de la camara. Para rotacion pura la inversa es el
// conjugado (rot.Inverted()); la traslacion lleva el mundo a -pos. view = R * T.
Matrix4 CameraBase::ViewMatrix() const {
    Matrix4 R = rot.Inverted().ToMatrix();
    Matrix4 T;
    T.Identity();
    T.m[12] = -pos.x;
    T.m[13] = -pos.y;
    T.m[14] = -pos.z;
    return R * T;
}

// PROYECCION perspectiva (column-major, igual que gluPerspective).
Matrix4 CameraBase::ProjectionMatrix(float aspect) const {
    Matrix4 P;
    for (int i = 0; i < 16; i++) P.m[i] = 0.0f;
    float f = 1.0f / tanf(fov * 3.14159265f / 180.0f * 0.5f);
    P.m[0]  = f / aspect;
    P.m[5]  = f;
    P.m[10] = (farZ + nearZ) / (nearZ - farZ);
    P.m[11] = -1.0f;
    P.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
    return P;
}
