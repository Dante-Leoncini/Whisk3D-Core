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
#if !defined(_WIN32) && !defined(W3D_SYMBIAN)
#include <GL/glext.h>
#endif

#ifndef W3D_SYMBIAN
#endif
#include "objects/Mesh.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cassert>
#include <cstring>

#ifndef W3D_SYMBIAN
#include "importers/import_obj.h" // (llega a Symbian en la Fase 4)
#endif

static void ParseFace(const std::string& line, Face& f) {
    std::istringstream ss(line.substr(2));
    std::string tok;

    while (ss >> tok) {
        FaceCorner fc;

        int v = -1, t = -1, n = -1;

        if (tok.find("//") != std::string::npos) {
            sscanf(tok.c_str(), "%d//%d", &v, &n);
        } else {
            sscanf(tok.c_str(), "%d/%d/%d", &v, &t, &n);
        }

        fc.vertex = v - 1;
        fc.normal = n - 1;

        f.corners.push_back(fc);
    }
}

static GLbyte* BuildVertexNormals(
    size_t vertexCount,
    const std::vector<GLbyte>& tempNormals,
    const std::vector<Face>& faces
) {
    GLbyte* out = new GLbyte[vertexCount * 3];

    // default
    for (size_t i = 0; i < vertexCount * 3; i++)
        out[i] = 127;

    for (size_t fi = 0; fi < faces.size(); fi++) {
        const Face& f = faces[fi];
        if (f.corners.size() < 3) continue;

        for (size_t i = 1; i < f.corners.size() - 1; i++) {
            const FaceCorner tri[3] = {
                f.corners[0],
                f.corners[i],
                f.corners[i + 1]
            };

            for (int k = 0; k < 3; k++) {
                const FaceCorner& fc = tri[k];
                if (fc.vertex < 0 || fc.normal < 0) continue;

                size_t v = fc.vertex * 3;
                size_t n = fc.normal * 3;

                out[v + 0] = tempNormals[n + 0];
                out[v + 1] = tempNormals[n + 1];
                out[v + 2] = tempNormals[n + 2];
            }
        }
    }

    return out;
}

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

        int blendStep;

        VertexAnimationActive(Mesh* mesh);

        void UpdateAnimation();
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

void UpdateAnimations();
void NewActiveVertexAnimation(Mesh* mesh, VertexAnimation* anim);

#endif