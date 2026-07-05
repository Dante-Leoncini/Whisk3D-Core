#ifndef MATERIALS_H
#define MATERIALS_H

#ifdef _WIN32
    #define NOMINMAX
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <vector>
#include <string>
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>
#else
    #include <GL/gl.h>
    #include "render/OpcionesRender.h"
#endif
#include "Textures.h"

// Declaración adelantada de Material
class Material;

// Vector global de materiales
extern std::vector<Material*> Materials;
extern Material* MaterialDefecto;

// ===================================================
// Clase Material
// ===================================================
// capa de textura EXTRA (encima de la textura base del material): su textura + como se MEZCLA con lo de
// abajo. Comparten las UV del modelo (UV por capa = TODO). Render por MULTI-PASS (GL 1.1, portable PC+N95).
class TexLayer {
    public:
        Texture* tex;
        int blend;  // 0 = Mix (alpha encima), 1 = Multiply, 2 = Add
        bool on;
        TexLayer() : tex(NULL), blend(0), on(true) {}
};

class Material {
    public:
        // defaults en el constructor (Materials.cpp): los inicializadores
        // en la clase son C++11 y esto compila tambien en RVCT (C++03)
        bool textureOn;   // prender/apagar la textura (checkbox de la UI)
        bool filtrado;    // filtrado de textura (lineal) o pixel perfect
        bool transparent;
        bool vertexColor;
        bool lighting;
        bool repeat;
        bool uv8bit;
        bool culling;
        bool depth_test;
        bool chrome;      // "Reflection": reflejo de entorno (env-map). On/off; el MODO lo elige reflectMode.
        int  reflectMode; // 0 = Matcap (normal-del-ojo, matriz de textura, HARDWARE en PC y N95; rapido)
                          // 1 = Sphere Map exacto (GL_SPHERE_MAP: HARDWARE en PC via texgen, SOFTWARE en N95)
                          // 2 = Equirectangular 360 (SOFTWARE, calidad). (VGP exacto por HW = futuro, falta API de IMG)
        bool normalMap;   // NORMAL MAPPING (DOT3): la textura 'normalTexture' perturba la normal por pixel.
                          // Multi-pass: base * (N.L). Excluyente con chrome (mismo combiner). Portable PC+N95.
        Texture* normalTexture; // el normal map (RGB = normal en tangent-space)
        int interpolacion;
        Texture* texture;
        std::vector<TexLayer> capas; // capas de textura EXTRA encima de 'texture' (multi-pass)
        float diffuse[4];
        float specular[4];
        float emission[4];
        float ambient[4]; // reflectancia de la luz ambiente del material
        float shininess;  // brillo especular (exponente)
        std::string name;

        Material(const std::string& nombre, bool MaterialDefecto = false, bool TieneVertexColor = false);
        ~Material();
};

// ===================================================
// Funciones auxiliares
// ===================================================
Material* BuscarMaterialPorNombre(const std::string& name);

class AnimatedMaterial {
    public:
        std::vector<Material*> targets;    // targets → materiales afectados
        std::vector<Texture*> frameTextures; // textures → cada frame de la animación
        std::vector<int> frameDurations;     // speeds → duración por frame (en ticks)

        int frameIndex;     // índice del frame actual (0)
        int tickCounter;    // tiempo acumulado en el frame actual (-1)

        AnimatedMaterial() : frameIndex(0), tickCounter(-1) {}
        void Update();
};

extern std::vector<AnimatedMaterial*> AnimatedMaterials;
extern void UpdateAnimatedMaterials();

// true si hay un material animado activo (animacion EN PLAY). Lo usa el render
// event-driven del loop (PC/Symbian) para seguir redibujando mientras algo se
// mueve sin input. Vive aca (Materials.cpp se compila en los 4 OS); el sistema
// de vertex-animation es solo de PC, asi que el loop de PC ademas chequea
// VertexAnimationActives por su lado.
bool HayAnimacionActiva();

#endif