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
    #include "GeometriaUI/GeometriaUI.h"
    #include "render/OpcionesRender.h"
#endif

class Light : public Object {
    public:
        GLenum LightID;
        GLfloat position[4];
        GLfloat ambient[4];
        GLfloat diffuse[4];
        GLfloat specular[4];
        // OpenGL fixed-function = UN tipo de luz, configurable: DIRECCIONAL (w=0, como el sol, sin posicion ni
        // atenuacion), PUNTUAL (w=1 + atenuacion) o SPOT (puntual + cono). Editables desde el panel de la luz.
        bool   direccional;     // true: w=0 (direccional); false: w=1 (puntual/spot, con atenuacion)
        GLfloat attConstant;    // atenuacion: 1/(C + L*d + Q*d^2). Solo luz puntual/spot.
        GLfloat attLinear;
        GLfloat attQuadratic;
        GLfloat spotCutoff;     // angulo del cono en grados: 180 = sin cono (punto); 1..90 = spotlight
        GLfloat spotExponent;   // concentracion del haz del spot (0 = parejo .. 128 = muy focalizado)

        static Light* Create(Object* parent = NULL, GLfloat x = 0, GLfloat y = 0, GLfloat z = 0);

        ObjectType getType() override;

        void SetDiffuse(GLfloat r = 1.0f, GLfloat g = 1.0f, GLfloat b = 1.0f);
        void SetLightID(GLenum ID);
        void RenderObject() override;

        ~Light();

    private:
        Light(Object* parent, GLfloat x, GLfloat y, GLfloat z);
};

// Contenedor global de luces
extern std::vector<Light*> Lights;

// Máximo de luces
const int MAX_LIGHTS = 8;

#endif