#include "Light.h"
#include "w3dGraphics.h" // flags de estado de render (w3dRenderLuces)
#include <cstdio>

// Inicialización de variables estáticas
std::vector<Light*> Lights;

// hook del editor para el GIZMO de la luz (ver Light.h). NULL = sin editor -> no se dibuja.
void (*g_lightOverlayHook)(Light*) = NULL;

// Constructor privado
Light::Light(Object* parent, float x, float y, float z)
: Object(parent, "Light", Vector3(x, y, z)), LightID(GL_LIGHT0) {

    position[0] = 0;
    position[1] = 0;
    position[2] = 0;
    position[3] = 1.0f;

    ambient[0] = ambient[1] = ambient[2] = 0.0f; ambient[3] = 1.0f;
    diffuse[0] = diffuse[1] = diffuse[2] = 1.0f; diffuse[3] = 1.0f;
    specular[0] = specular[1] = specular[2] = 0.0f; specular[3] = 1.0f;
    direccional = false;                       // por defecto luz PUNTUAL (w=1) con atenuacion suave
    attConstant = 0.5f; attLinear = 0.1f; attQuadratic = 0.0f;
    spotCutoff = 180.0f;                       // 180 = sin cono (punto). <90 -> spotlight
    spotExponent = 0.0f;
}

// Método Create
Light* Light::Create(Object* parent, float x, float y, float z) {
    if (Lights.size() >= MAX_LIGHTS) {
        printf("WARNING: Máximo de luces alcanzado.\n");
        return NULL;
    }

    Light* l = new Light(parent, x, y, z);
    // cada luz NUEVA es unica por defecto: toma el GL_LIGHT libre MAS BAJO
    // (antes era GL_LIGHT0 + Lights.size(), que colisionaba al borrar una del medio).
    // Varias luces PUEDEN compartir un GL light si se setea a mano.
    bool usado[MAX_LIGHTS];
    for (int i = 0; i < MAX_LIGHTS; i++) usado[i] = false;
    for (size_t i = 0; i < Lights.size(); i++) {
        int idx = (int)(Lights[i]->LightID - GL_LIGHT0);
        if (idx >= 0 && idx < MAX_LIGHTS) usado[idx] = true;
    }
    int libre = 0;
    while (libre < MAX_LIGHTS && usado[libre]) libre++;
    if (libre >= MAX_LIGHTS) libre = 0; // (no deberia pasar por el limite)
    l->SetLightID(GL_LIGHT0 + (GLenum)libre);
    Lights.push_back(l);
    return l;
}

// Destructor
Light::~Light() {
    // CRITICO: sacarse del vector global. Si no, queda un puntero COLGADO y
    // ReloadLights (recorre Lights y hace Lights[l]->LightID) crashea al borrar una luz.
    GLenum id = LightID;
    for (size_t i = 0; i < Lights.size(); i++) {
        if (Lights[i] == this) { Lights.erase(Lights.begin() + i); break; }
    }
    // si NINGUNA otra luz usa este GL light, apagarlo (sino lo comparten)
    bool compartido = false;
    for (size_t i = 0; i < Lights.size(); i++)
        if (Lights[i]->LightID == id) { compartido = true; break; }
    if (!compartido) glDisable(id);
}

// getType
ObjectType Light::getType() {
    return ObjectType::light;
}

// SetDiffuse
void Light::SetDiffuse(float r, float g, float b) {
    diffuse[0] = r;
    diffuse[1] = g;
    diffuse[2] = b;
}

// SetLightID
void Light::SetLightID(GLenum ID) {
    LightID = ID;
}

// RenderObject
void Light::RenderObject() {
    // Las luces de la ESCENA se aplican SOLO en RENDER preview (esto es algo mas del editor 3d puede quitarse en el futuro)
    bool aplicar = w3dRenderLuces;
#ifdef W3D_SYMBIAN
    // misma logica de luz que PC: la luz de ESCENA se enciende aca (bajo la matriz del objeto: position local =
    // origen del objeto). El icono lo dibuja render.cpp (RenderIcons3D).
    if (aplicar) {
        glEnable(LightID);
        float pos[4];
        if (direccional) {                                  // DIRECCIONAL: la direccion la da la ROTACION de la
            // lampara (no su posicion). Local (0,1,0)="arriba" -> la matriz del objeto (su rotacion) lo orienta:
            // a 0° la luz viene de ARRIBA = la lampara apunta ABAJO; al rotarla, la luz rota con ella.
            pos[0] = 0.0f; pos[1] = 1.0f; pos[2] = 0.0f; pos[3] = 0.0f;
        } else { pos[0] = position[0]; pos[1] = position[1]; pos[2] = position[2]; pos[3] = 1.0f; }
        glLightfv(LightID, GL_POSITION, pos);
        glLightfv(LightID, GL_DIFFUSE,  diffuse);
        glLightfv(LightID, GL_AMBIENT,  ambient);
        glLightfv(LightID, GL_SPECULAR, specular);
        glLightf(LightID, GL_CONSTANT_ATTENUATION, attConstant);
        glLightf(LightID, GL_LINEAR_ATTENUATION,   attLinear);
        // QUADRATIC + SPOT: SOLO en PC. El GLES1 del N95 nunca los ejecuto (el bloque viejo no los tenia) y
        // parecen colgar el driver -> los dejamos afuera de Symbian (la luz queda puntual/direccional, sin cono).
    }
    return;
#else

    if (aplicar){
        glEnable(LightID);
        float pos[4];
        if (direccional) {                                  
            // DIRECCIONAL (sol): la direccion la da la ROTACION de la
            // lampara. Local (0,1,0)="arriba" -> la matriz del objeto (su rotacion) lo orienta: a 0° la luz viene
            // de ARRIBA = la lampara apunta ABAJO; al rotar la lampara, la luz rota con ella.
            pos[0] = 0.0f; pos[1] = 1.0f; pos[2] = 0.0f; pos[3] = 0.0f;
        } else { pos[0] = position[0]; pos[1] = position[1]; pos[2] = position[2]; pos[3] = 1.0f; }
        glLightfv(LightID, GL_POSITION, pos);
        glLightfv(LightID, GL_DIFFUSE,  diffuse);
        glLightfv(LightID, GL_AMBIENT,  ambient);
        glLightfv(LightID, GL_SPECULAR, specular);
        // atenuacion editable (solo afecta a la puntual/spot; la direccional la ignora OpenGL)
        glLightf(LightID, GL_CONSTANT_ATTENUATION,  attConstant);
        glLightf(LightID, GL_LINEAR_ATTENUATION,    attLinear);
        glLightf(LightID, GL_QUADRATIC_ATTENUATION, attQuadratic);
        // SPOT: cono + concentracion + direccion = el "forward" local del objeto (0,0,-1 bajo su matriz)
        glLightf(LightID, GL_SPOT_CUTOFF,   direccional ? 180.0f : spotCutoff); // 180 = sin cono (punto)
        glLightf(LightID, GL_SPOT_EXPONENT, spotExponent);
        float sdir[3] = {0.0f, 0.0f, -1.0f};
        glLightfv(LightID, GL_SPOT_DIRECTION, sdir);
    }

    // GIZMO de la luz (linea + color de seleccion): overlay del EDITOR, lo dibuja el hook (render.cpp),
    // NO el Core. En Symbian el path de arriba retorna sin llamarlo (el icono lo hace RenderIcons3D).
    if (g_lightOverlayHook) g_lightOverlayHook(this);
#endif // !W3D_SYMBIAN
}
