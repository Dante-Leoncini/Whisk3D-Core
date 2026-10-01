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

// el MARCO del render adentro del viewport que se esta dibujando, en NDC del viewport: centro (x, y) y
// medias-extensiones (x, y). {0, 0, 1, 1} = todo el viewport. Mirando por una camara con encuadre (passepartout /
// barras) es el cuadro de la IMAGEN: lo que miden en fracciones los objetos que dibujan en una parte de la pantalla
// (Recorte) para coincidir con la UI, que se encaja a ese mismo cuadro. Lo publica la proyeccion de la escena.
extern float g_renderMarco[4];

// base (ejes) de la camara en MUNDO: right/up/forward. Tambien la setea el viewport. El MATCAP por software
// (sphere-map del N95, sin glTexGen) la necesita para llevar pos/normal a espacio de OJO. Right=+X, Up=+Y,
// Forward=hacia la escena (el -Z del eye space).
extern Vector3 g_renderCamRight;
extern Vector3 g_renderCamUp;
extern Vector3 g_renderCamForward;

// ORIENTACION (en MUNDO) de la vista que se esta renderizando. Es el mismo dato que right/up/forward
// pero sin descomponer: lo necesita el que tiene que MEZCLAR contra la orientacion de la vista (slerp),
// no solo proyectar sobre sus ejes.
extern Quaternion g_renderCamRot;

// LENTE de la vista que se esta renderizando (fov vertical en grados, near/far y aspect
// del frustum REAL con el que se dibuja). La setea el viewport ANTES de recorrer la
// escena, junto con W3dVistaBind. La necesita el que arma el frustum en CPU (el objeto
// Culling): pos/rot solos no alcanzan para los 6 planos. Defaults = los de CameraBase.
extern float g_renderCamFov;
extern float g_renderCamNear;
extern float g_renderCamFar;
extern float g_renderCamAspect;
// true = la vista se dibuja en ORTOGRAFICA: el fov de arriba no describe el volumen
// visible. El Culling en ese caso NO corta (dibuja todo): mentir un frustum en
// perspectiva cortaria cosas que SI se ven.
extern bool g_renderCamOrto;

// false = todavia nadie llamo a W3dVistaBind en este proceso, o sea que los globales de arriba son el
// default y NO la vista de nadie. El que dependa de la vista tiene que poder distinguir "la camara mira
// para alla" de "no hay camara": con esto avisa UNA vez y usa el default en vez de dibujar cualquier cosa.
extern bool g_vistaBindeada;

// UNICO camino para publicar la vista al Core: setea pos/rot/right/up/forward y prende g_vistaBindeada.
// TODO el que va a dibujar (o a preguntar "como se ve desde aca") bindea SU vista antes; sino lee la del
// viewport que dibujo ultimo. Sale de las 4 asignaciones a mano que tenia el viewport del editor, que
// eran la unica copia de esta cuenta y no las veia ni el runtime del juego.
// NO normaliza cam.rot: la vista viene de un quaternion UNITARIO (viewRot se normaliza en cada orbita) y
// asi la base que le llega al chrome/matcap es BIT A BIT la de siempre. Con un rot sin normalizar la base
// deja de ser ortonormal y el que la use lo va a notar.
void W3dVistaBind(const CameraBase& cam);

// POSICION (en MUNDO) de la luz principal de la escena: la setea el viewport antes de renderizar. La usa el
// NORMAL MAPPING (N.L por vertice) para que el relieve responda a la luz real (no a la camara). 0,0,0 = sin luz.
extern Vector3 g_renderLightPos;

// COLOR (diffuse rgb) de la luz principal: el normal map tiñe la base con esto (el N.L es luminancia -> sin esto
// el relieve sale BLANCO aunque la luz sea de color). Default blanco.
extern Vector3 g_renderLightColor;

#endif // CAMERA_BASE_H
