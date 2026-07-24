#ifndef VERTEXANIMATION_H
#define VERTEXANIMATION_H

#ifdef _WIN32
    #define NOMINMAX
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <vector>
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>
#else
    #include <GL/gl.h>
#endif
#include "objects/Mesh.h"
#include <string>

// Guarda SOLO posiciones de vértices para un frame
struct VertexFrame {
    // puntero a array de floats [x,y,z,x,y,z,...]
    const GLfloat* positions;
    const GLbyte* normals;
    VertexFrame() : positions(NULL), normals(NULL) {}
};

class VertexAnimation {
    public:
        // Nombre de la animación
        std::string name;

        // Declaración (desde .w3d)
        std::string basePath;
        // defaults en el constructor de abajo (C++03)
        int frameCount;
        int proximaAnimacion;
        float speed;
        int padding;
        bool repeat;

        // Runtime
        Mesh* target;

        bool UseNormals;

        // Frames de la animación (solo posiciones)
        std::vector<VertexFrame*> frames;

        VertexAnimation()
            : frameCount(0), proximaAnimacion(-1), speed(1.0f), padding(0),
              repeat(true), target(NULL), UseNormals(false) {}
        VertexAnimation(Mesh* tgt, const std::string& animName, bool useNormals = false, float Speed = 1, bool Repeat = true, int ProximaAnimacion = -1);
    
        ~VertexAnimation();          // libera los frames (posiciones/normales new[])
        void LiberarFrames();

        // Cargar animaciones desde archivos .obj
        bool LoadFrames();

        size_t FrameCount() const { return frames.size(); }
};

class VertexAnimationActive {
    public:
        // defaults en el constructor (VertexAnimation.cpp), C++03
        Mesh* meshToAnim;

        int currentAnim;   // animación activa (idle/run o la que sea)
        int nextAnim;      // animación a la que queremos ir

        int currentFrame;
        int nextFrame;

        float blendStep;    // en TICKS de 120 Hz (el speed del contenido se calibro con el loop viejo)

        VertexAnimationActive(Mesh* mesh);

        void UpdateAnimation(float dtSeg);   // dt REAL: la anim no depende de los fps
};

// Mezcla dos animaciones y escribe en la malla target
// - fromAnim: animación actual (ej: idle)
// - toAnim: animación destino (ej: run)
// - fromFrame: frame actual en fromAnim
// - blendT: 0..1 (0 = solo fromAnim, 1 = solo toAnim)
// - toFrame: frame en toAnim (usualmente 0 al comenzar la mezcla)
void BlendVertexAnimations(
    const VertexAnimation& fromAnim,
    const VertexAnimation& toAnim,
    size_t fromFrame,
    size_t toFrame,
    float blendT,
    Mesh* mesh
);

// Copia directa de un frame a la malla target (sin mezcla)
void ApplyVertexFrame(
    const VertexAnimation& anim,
    size_t frameIndex
);

void LoadVertexFrames(Mesh* mesh);

extern std::vector<VertexAnimationActive*> VertexAnimationActives;

VertexAnimationActive* FindTargetAnim(Mesh* target);

void UpdateAnimations(float dtSeg = 1.0f / 60.0f);
void NewActiveVertexAnimation(Mesh* mesh, VertexAnimation* anim);

#endif