#include "CameraBase.h"
#include <math.h> // C puro: compila en RVCT (Symbian) y PC por igual

float g_renderMarco[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
Vector3 g_renderCamPos(0, 0, 0); // la setea el viewport antes de renderizar (chrome equirect)
Vector3 g_renderCamRight(1, 0, 0);   // base de la camara en mundo (matcap por software / sphere-map)
Vector3 g_renderCamUp(0, 1, 0);
Vector3 g_renderCamForward(0, 0, -1);
Quaternion g_renderCamRot;           // identidad por su propio ctor: la vista sin girar
float g_renderCamFov    = 45.0f;     // lente de la vista bindeada (la publica el viewport
float g_renderCamNear   = 0.1f;      // junto con W3dVistaBind; ver el .h). Defaults =
float g_renderCamFar    = 1000.0f;   // los de CameraBase, para el que pregunta antes
float g_renderCamAspect = 1.0f;      // del primer frame.
bool  g_renderCamOrto   = false;     // vista ortografica: el Culling no corta (ver el .h)
bool g_vistaBindeada = false;        // se prende en el primer W3dVistaBind y no se apaga mas
Vector3 g_renderLightPos(4, 5, 4); // luz principal en mundo (normal mapping); el viewport la actualiza por frame
Vector3 g_renderLightColor(1, 1, 1); // color (diffuse) de la luz principal -> tiñe el normal map

// publica la vista que se va a dibujar (ver el comentario del .h). Son las mismas 4 cuentas que hacia a
// mano ViewPort3D: right/up/forward son los ejes de la camara LLEVADOS A MUNDO por su rotacion, con
// forward = el -Z del eye space, o sea "hacia la escena".
void W3dVistaBind(const CameraBase& cam) {
    g_renderCamPos     = cam.pos;
    g_renderCamRot     = cam.rot;
    g_renderCamRight   = cam.rot * Vector3(1, 0, 0);
    g_renderCamUp      = cam.rot * Vector3(0, 1, 0);
    g_renderCamForward = cam.rot * Vector3(0, 0, -1);
    g_vistaBindeada    = true;
}

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
