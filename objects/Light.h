#ifndef LIGHT_H
#define LIGHT_H

#ifdef _WIN32
    #define NOMINMAX
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <vector>
#include "crossplatform.h"   // W3D_OVERRIDE (antes llegaba de rebote por Objects.h)
#include "objects/Objects.h"
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>
#else
    #include <GL/gl.h>
#endif

class Light : public Object {
    public:
        GLenum LightID;
        float position[4];
        float ambient[4];
        float diffuse[4];
        float specular[4];
        // OpenGL fixed-function = UN tipo de luz, configurable: DIRECCIONAL (w=0, como el sol, sin posicion ni
        // atenuacion), PUNTUAL (w=1 + atenuacion) o SPOT (puntual + cono). Editables desde el panel de la luz.
        bool   directional;     // true: w=0 (direccional); false: w=1 (puntual/spot, con atenuacion)
        // IGNORAR EL ORDEN DEL ARBOL (default true): la luz se enciende en el PRE-PASE, antes de toda la geometria,
        // y alumbra la escena entera sin importar donde este colgada. En false vuelve el comportamiento de GL fijo:
        // solo alumbra lo que se dibuja DESPUES de ella en el arbol (sirve para iluminar ciertas cosas con ciertas
        // luces, o para REUSAR uno de los 8 GL lights pisando una luz anterior mas abajo en el arbol).
        bool   ignoreOrder;
        float attConstant;    // atenuacion: 1/(C + L*d + Q*d^2). Solo luz puntual/spot.
        float attLinear;
        float attQuadratic;
        float spotCutoff;     // angulo del cono en grados: 180 = sin cono (punto); 1..90 = spotlight
        float spotExponent;   // concentracion del haz del spot (0 = parejo .. 128 = muy focalizado)
        // TRAZADO DE RAYOS (editor, render/RayTracer): el TAMANO de la lampara y cuantos rayos de sombra manda.
        //   rtRadius: 0 = puntual (sombra dura); >0 = esfera de ese radio (puntual) o cono de esos GRADOS (solar)
        //            -> penumbra, mas suave cuanto mas grande.
        //   rtRays: rayos de sombra por impacto hacia esta lampara; 0 = usar el global de la pestania Render.
        float rtRadius;
        float rtRays;
        // INTENSIDAD: multiplica el color (difuso y especular) de la luz, en GL y en el trazado de rayos. 1 = el color
        // tal cual; mas = mas fuerte (el color queda en 0..1 como siempre y esto la sube). Animable (AnimLightIntens)
        float intensity;
        // DISTANCIA en metros: hasta donde alumbra una puntual/foco (el largo del cono que se dibuja). 0 = sin limite.
        // En el trazado de rayos la luz se apaga suave hasta llegar a 0 ahi; el GL fijo no la corta (solo atenua)
        float distance;
        // LUZ DE AREA (tipo 3, la "Area" de Blender): una SUPERFICIE que alumbra de UN solo lado y pareja hacia todos lados
        // de ese lado (lambertiana: de costado da menos luz, por el coseno). Esta en el plano XY local de la lampara (la
        // escala del objeto la agranda, como en Blender) y alumbra hacia su -Z local, como el foco.
        //   areaShape: W3D_AREA_SQUARE / RECTANGLE / DISK / ELLIPSE (las de Blender; el cuadrado y el disco usan solo X)
        //   areaSizeX / areaSizeY: el ancho y el alto en metros (el diametro en el disco y la elipse)
        // LA INTENSIDAD de la de area es la de una puntual de caida 1 / d^2 (atenuacion 0, 0, 1) vista de LEJOS: a d metros
        // sobre su eje una superficie de frente recibe color * intensidad / d^2 (y de costado, por el coseno de cada lado),
        // sea del tamanio que sea. Agrandarla NO la hace mas fuerte: reparte esa luz en mas superficie (la sombra se ablanda
        // y de cerca no quema), como la potencia de Blender. De cerca vale la cuenta de la superficie entera: lo maximo, con
        // la lampara tapando medio cielo, es pi * intensidad / superficie (no explota como 1 / d^2): ese es su brillo (la
        // radiancia) en las unidades de las otras luces. DE BLENDER: una de area de P vatios (radiancia P / (pi * S) en
        // Cycles) es intensidad = P / pi^2 (por la exposicion del conversor), con el color, la forma y el tamanio por la
        // escala del objeto tal cual
        // La atenuacion C/L/Q y el cono no se usan (el foco no se pierde: si vuelve a ser foco, es el mismo); la distancia si
        // (en el trazado de rayos la apaga suave hasta ahi). Donde no hay cuenta de area (el GL fijo, GLES 1.1, PSP, 3DS,
        // la luz por vertice) se dibuja como un foco: ver W3dLightAreaAsSpot
        bool  area;
        int   areaShape;
        float areaSizeX, areaSizeY;

        static Light* Create(Object* parent = NULL, float x = 0, float y = 0, float z = 0);

        ObjectType getType() W3D_OVERRIDE;

        void SetDiffuse(float r = 1.0f, float g = 1.0f, float b = 1.0f);
        void SetLightID(GLenum ID);
        void RenderObject() W3D_OVERRIDE;

        ~Light();

    private:
        Light(Object* parent, float x, float y, float z);
};

// HOOK del EDITOR: Light::RenderObject lo llama (en PC) tras montar la luz para dibujar su GIZMO
// (linea + color de seleccion). NULL por defecto -> una app/juego sin editor no dibuja gizmos.
extern void (*g_lightOverlayHook)(Light*);

// Contenedor global de luces
extern std::vector<Light*> Lights;
// los CANALES de GL (bit i = GL_LIGHT0 + i) que prendieron las luces de la ESCENA al dibujarse. El pase de Render con
// Constructor los apaga al empezar (W3dScene3DLights) y las que siguen en la escena los vuelven a prender: una luz
// BORRADA (viva para el Ctrl+Z) u oculta no deja su canal prendido iluminando
extern unsigned g_w3dLightsOnScene;

// Máximo de luces
const int MAX_LIGHTS = 8;     // las de GL (GL_LIGHT0..7): las que el pipeline fijo / los shaders prenden a la vez
// las de la ESCENA: pasadas las 8, las nuevas COMPARTEN un GL light (en OpenGL se ven 8 a la vez; el trazado de rayos
// las usa todas: un escenario con muchos focos)
const int MAX_LIGHTS_SCENE = 256;

// PRE-PASE DE LUCES (Light.cpp): encender TODAS las luces visibles antes de dibujar la geometria del pase
void W3dLightsPrePass(Object* root);
void W3dLightsPrePassEnd();
// EL TIPO de una luz: 0 Punto (para todos lados), 1 Sol (direccional), 2 Foco (un cono de 45 grados si no tenia),
// 3 Area (una superficie: Light::area)
void W3dLightSetType(Light* l, int type);
// LA LUZ DE AREA (Light::area): sus formas (Light::areaShape, las de Blender en su orden)
enum { W3D_AREA_SQUARE = 0, W3D_AREA_RECTANGLE = 1, W3D_AREA_DISK = 2, W3D_AREA_ELLIPSE = 3 };
bool W3dLightAreaRound(const Light* l);                          // disco o elipse
void W3dLightAreaHalf(const Light* l, float& hx, float& hy);     // los MEDIOS lados en su espacio local (sin la escala)
// LA DE AREA COMO FOCO, para lo que no tiene su cuenta (el GL fijo, GLES 1.1, PSP, 3DS, la luz por vertice de los shaders,
// la luz por vertice de la CPU): un foco de 90 grados con caida coseno (exponente 1) desde el centro, con atenuacion
// 1 / (S/pi + d^2) (att = S/pi, 0, 1), S = la superficie en el mundo (sx, sy: la escala del objeto en X e Y). De lejos es
// la misma luz; de cerca no pasa de la de la superficie entera (en el eje de un disco es la cuenta exacta)
void W3dLightAreaAsSpot(const Light* l, float sx, float sy, float att[3]);

#endif
