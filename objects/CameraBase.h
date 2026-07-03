#ifndef CAMERA_BASE_H
#define CAMERA_BASE_H

#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"

// ============================================================================
//  Camara BASE del engine (core). Es la VISTA que el render usa para dibujar la
//  escena: una posicion, una orientacion y los parametros de proyeccion. Minima
//  y REUSABLE: Cualquiera que reuse Whisk3DCore para renderizar una escena usa esta camara.
// ============================================================================
class CameraBase {
    public:
        Vector3    pos;     // posicion de la camara en el mundo
        Quaternion rot;     // orientacion de la camara
        float      fov;     // campo de vision vertical, en grados (perspectiva)
        float      nearZ;   // plano cercano
        float      farZ;    // plano lejano

        CameraBase();

        // Matriz de VISTA = inversa del transform de la camara (R * T): lleva el
        // mundo al espacio de la camara. Es lo que el render carga en ModelView.
        Matrix4 ViewMatrix() const;

        // Matriz de PROYECCION perspectiva (equivalente a gluPerspective con este
        // fov/near/far y el aspect dado). API del engine para el render/consumidores.
        Matrix4 ProjectionMatrix(float aspect) const;
};

// posicion (en MUNDO) de la camara del viewport que se esta renderizando. La setea el viewport ANTES de
// dibujar los objetos (ViewPort3D). El render del CHROME equirect la lee para calcular el vector de
// reflexion por vertice. Vive en el core para que el path por software ande igual en PC y N95.
extern Vector3 g_renderCamPos;

// base (ejes) de la camara en MUNDO: right/up/forward. Tambien la setea el viewport. El MATCAP por software
// (sphere-map del N95, sin glTexGen) la necesita para llevar pos/normal a espacio de OJO. Right=+X, Up=+Y,
// Forward=hacia la escena (el -Z del eye space).
extern Vector3 g_renderCamRight;
extern Vector3 g_renderCamUp;
extern Vector3 g_renderCamForward;

// POSICION (en MUNDO) de la luz principal de la escena: la setea el viewport antes de renderizar. La usa el
// NORMAL MAPPING (N.L por vertice) para que el relieve responda a la luz real (no a la camara). 0,0,0 = sin luz.
extern Vector3 g_renderLightPos;

// COLOR (diffuse rgb) de la luz principal: el normal map tiñe la base con esto (el N.L es luminancia -> sin esto
// el relieve sale BLANCO aunque la luz sea de color). Default blanco.
extern Vector3 g_renderLightColor;

#endif // CAMERA_BASE_H
