#ifndef LIGHT_H
#define LIGHT_H

#ifdef _WIN32
    #define NOMINMAX
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <vector>
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
        bool   direccional;     // true: w=0 (direccional); false: w=1 (puntual/spot, con atenuacion)
        float attConstant;    // atenuacion: 1/(C + L*d + Q*d^2). Solo luz puntual/spot.
        float attLinear;
        float attQuadratic;
        float spotCutoff;     // angulo del cono en grados: 180 = sin cono (punto); 1..90 = spotlight
        float spotExponent;   // concentracion del haz del spot (0 = parejo .. 128 = muy focalizado)

        static Light* Create(Object* parent = NULL, float x = 0, float y = 0, float z = 0);

        ObjectType getType() override;

        void SetDiffuse(float r = 1.0f, float g = 1.0f, float b = 1.0f);
        void SetLightID(GLenum ID);
        void RenderObject() override;

        ~Light();

    private:
        Light(Object* parent, float x, float y, float z);
};

// HOOK del EDITOR: Light::RenderObject lo llama (en PC) tras montar la luz para dibujar su GIZMO
// (linea + color de seleccion). NULL por defecto -> una app/juego sin editor no dibuja gizmos.
extern void (*g_lightOverlayHook)(Light*);

// Contenedor global de luces
extern std::vector<Light*> Lights;

// Máximo de luces
const int MAX_LIGHTS = 8;

#endif