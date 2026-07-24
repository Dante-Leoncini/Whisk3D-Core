#include "VertexAnimation.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cassert>
#include <cstring>


// Normal (-1..1) -> byte con signo (C++03: sin lambdas, el Core compila en Symbian).
static GLbyte NrmFloatToByte(float v) {
    v = ((v + 1.0f) * 0.5f) * 255.0f - 128.0f;
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (GLbyte)v;
}

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

VertexAnimation::VertexAnimation(Mesh* tgt, const std::string& animName, bool useNormals, float Speed, bool Repeat, int ProximaAnimacion):
      frameCount(0), proximaAnimacion(ProximaAnimacion), speed(Speed),
      padding(0), repeat(Repeat), target(tgt), UseNormals(useNormals) {
    name = animName;
}

void VertexAnimation::LiberarFrames() {
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i]) continue;
        delete[] frames[i]->positions;
        delete[] frames[i]->normals;
        delete frames[i];
    }
    frames.clear();
}

VertexAnimation::~VertexAnimation() { LiberarFrames(); }

bool VertexAnimation::LoadFrames() {
    if (!target || !target->vertex || target->vertexSize <= 0)
        return false;

    LiberarFrames();   // recargar sin fugar los frames anteriores

    for (int i = 1; i <= frameCount; ++i) {

        std::ostringstream path;
        path << basePath
             << std::setw(padding)
             << std::setfill('0')
             << i
             << ".obj";

        std::ifstream file(path.str().c_str());   // C++03: no acepta std::string
        if (!file.is_open()) {
            std::cerr << "[Anim] No se pudo abrir " << path.str() << "\n";
            return false;
        }

        std::vector<GLfloat> verts;
        std::vector<GLbyte>  tempNormals;
        std::vector<Face>    faces;

        std::string line;


        // ---------- PARSE OBJ ----------
        while (std::getline(file, line)) {

            if (line.rfind("v ", 0) == 0) {
                GLfloat x, y, z;
                sscanf(line.c_str(), "v %f %f %f", &x, &y, &z);
                verts.push_back(x);
                verts.push_back(y);
                verts.push_back(z);
            }
            else if (UseNormals && line.rfind("vn ", 0) == 0) {
                double nx, ny, nz;
                sscanf(line.c_str(), "vn %lf %lf %lf", &nx, &ny, &nz);

                tempNormals.push_back(NrmFloatToByte((float)nx));
                tempNormals.push_back(NrmFloatToByte((float)ny));
                tempNormals.push_back(NrmFloatToByte((float)nz));
            }
            else if (UseNormals && line.rfind("f ", 0) == 0) {
                Face f;
                ParseFace(line, f);
                faces.push_back(f);
            }
        }

        // ---------- VALIDACIÓN ----------
        if ((int)verts.size() != target->vertexSize * 3) {   // verts trae x,y,z por VERTICE
            std::cerr << "[Anim] Vertex count mismatch en "
                      << path.str() << "\n";
            return false;
        }

        // ---------- CONSTRUIR NORMALES ----------
        GLbyte* finalNormals = NULL;

        if (UseNormals) {
            size_t vcount = verts.size() / 3;
            finalNormals = BuildVertexNormals(vcount, tempNormals, faces);
        }

        // ---------- CREAR FRAME ----------
        VertexFrame* frame = new VertexFrame;

        GLfloat* pos = new GLfloat[verts.size()];
        std::memcpy(pos, verts.data(), verts.size() * sizeof(GLfloat));
        frame->positions = pos;

        frame->normals = finalNormals; // puede ser NULL

        frames.push_back(frame);
    }

    return true;
}

void ApplyVertexFrame(const VertexAnimation& anim, size_t frameIndex) {
    assert(anim.target);

    if (frameIndex >= anim.frames.size()) {
        frameIndex = anim.frames.size() - 1;
    }

    Mesh* mesh = anim.target;
    const VertexFrame* frame = anim.frames[frameIndex];

    // ---------- posiciones (3 floats por VERTICE) ----------
    const GLfloat* srcPos = frame->positions;
    const size_t nFloats = (size_t)mesh->vertexSize * 3;
    for (size_t i = 0; i < nFloats; ++i) {
        mesh->vertex[i] = srcPos[i];
    }

    // ---------- normales (opcional, 3 bytes por VERTICE) ----------
    if (anim.UseNormals && frame->normals && mesh->normals) {
        const GLbyte* srcNrm = frame->normals;
        for (size_t i = 0; i < nFloats; ++i) {
            mesh->normals[i] = srcNrm[i];
        }
    }
    mesh->skinGeomVersion++;   // re-subir el VBO (ver BlendVertexAnimations)
}

static inline float NrmByteToFloat(GLbyte v) {
    return (float(v) + 128.0f) / 255.0f * 2.0f - 1.0f;
}

void BlendVertexAnimations(
    const VertexAnimation& fromAnim,
    const VertexAnimation& toAnim,
    size_t fromFrame,
    size_t toFrame,
    float blendT,
    Mesh* mesh
) {
    assert(fromAnim.target);
    assert(fromAnim.target == toAnim.target);
    assert(fromFrame < fromAnim.frames.size());
    assert(toFrame < toAnim.frames.size());
    // (mismo target => misma cantidad de vertices; el campo vertexCount
    // no existe en VertexAnimation)
    assert(fromAnim.target->vertexSize == toAnim.target->vertexSize);

    // Clamp rápido
    if (blendT <= 0.0f) {
        ApplyVertexFrame(fromAnim, fromFrame);
        return;
    }
    if (blendT >= 1.0f) {
        ApplyVertexFrame(toAnim, toFrame);
        return;
    }

    const VertexFrame* A = fromAnim.frames[fromFrame];
    const VertexFrame* B = toAnim.frames[toFrame];

    // ---------- posiciones (3 floats por VERTICE) ----------
    const size_t nFloats = (size_t)mesh->vertexSize * 3;
    for (size_t i = 0; i < nFloats; ++i) {
        mesh->vertex[i] =
            A->positions[i] * (1.0f - blendT) +
            B->positions[i] * blendT;
    }

    // ---------- normales (solo si ambas animaciones las usan) ----------
    if (fromAnim.UseNormals && toAnim.UseNormals &&
        A->normals && B->normals && mesh->normals) {

        for (size_t i = 0; i < nFloats; ++i) {
            float na = NrmByteToFloat(A->normals[i]);
            float nb = NrmByteToFloat(B->normals[i]);

            float n = na * (1.0f - blendT) + nb * blendT;

            mesh->normals[i] = NrmFloatToByte(n);
        }
    }

    // el render por VBO solo RE-SUBE la geometria cuando cambia su version: sin
    // esto, la vertex animation escribia los vertices y la pantalla quedaba
    // CONGELADA en la pose vieja (bug del refactor de VBOs)
    mesh->skinGeomVersion++;
}

VertexAnimationActive::VertexAnimationActive(Mesh* mesh):
    meshToAnim(mesh), currentAnim(0), nextAnim(0), currentFrame(0),
    nextFrame(1), blendStep(0.0f) {
}

void VertexAnimationActive::UpdateAnimation(float dtSeg){
    if (!meshToAnim || meshToAnim->animations.empty()) return;

    VertexAnimation* fromAnim = meshToAnim->animations[currentAnim];
    VertexAnimation* toAnim   = meshToAnim->animations[nextAnim];
    
    if (!fromAnim || !toAnim) return;
    if (fromAnim->frames.empty() || toAnim->frames.empty()) return;

    // Avanza el blend por TIEMPO REAL: a cualquier framerate la animacion dura lo
    // mismo. El 'speed' del contenido se calibro con el loop viejo (~120 updates/seg
    // con el busy-spin): ESE es el tick de referencia.
    blendStep += dtSeg * 120.0f;

    float speed = fromAnim->speed;
    if (speed <= 0.0f) speed = 1.0f;

    float blendT = blendStep / speed;
    if (blendT > 1.0f) blendT = 1.0f;

    size_t fromFrame = static_cast<size_t>(currentFrame) % fromAnim->frames.size();
    size_t toFrame = (currentAnim == nextAnim)
        ? static_cast<size_t>(nextFrame) % fromAnim->frames.size()
        : 0;

    // Mezcla entre frame actual y el siguiente (puede ser otra anim)
    BlendVertexAnimations(
        *fromAnim,
        *toAnim,
        fromFrame,
        toFrame,
        blendT,
        meshToAnim
    );

    // Si terminó el blend
    if (blendStep >= speed) {
        blendStep = 0.0f;

        bool changedAnim = (currentAnim != nextAnim);

        // Consolidamos estado
        currentAnim = nextAnim;

        if (changedAnim) {
            currentFrame = 0;
            nextFrame = 1;
        } else {
            currentFrame = nextFrame;
            nextFrame++;
        }

        VertexAnimation* anim = meshToAnim->animations[currentAnim];
        if (anim && !anim->frames.empty()) {
            int lastFrame = (int)anim->frames.size() - 1;

            if (nextFrame > lastFrame){
                if (anim->proximaAnimacion >= 0){
                    nextFrame = 0;
                    nextAnim = anim->proximaAnimacion;
                }
                else if (anim->repeat) {
                    nextFrame = 0; // loop
                } else {
                    nextFrame = lastFrame; // HOLD último frame
                }
            }
        }
    }
}

std::vector<VertexAnimationActive*> VertexAnimationActives;

VertexAnimationActive* FindTargetAnim(Mesh* target) {
    if (!target)
        return NULL;

    for (std::vector<VertexAnimationActive*>::iterator it = VertexAnimationActives.begin();
         it != VertexAnimationActives.end(); ++it) {
        VertexAnimationActive* active = *it;
        if (!active) continue;

        if (active->meshToAnim == target) {
            return active;
        }
    }

    return NULL;
}

void NewActiveVertexAnimation(Mesh* mesh, VertexAnimation* anim){
    if (!mesh) return;

    mesh->animations.push_back(anim);

    // Un solo controlador activo por mesh; si ya existe, no crear otro.
    if (!FindTargetAnim(mesh)) {
        VertexAnimationActive* activeAnim = new VertexAnimationActive(mesh);
        VertexAnimationActives.push_back(activeAnim);
    }
};

void LoadVertexFrames(Mesh* mesh){
    if (!mesh) return;

    for (std::vector<VertexAnimation*>::iterator it = mesh->animations.begin();
         it != mesh->animations.end(); ++it) {
        VertexAnimation* anim = *it;
        anim->target = mesh;

        if (anim->frames.empty()) {
            anim->LoadFrames();
            std::cout << "Anim '"<< anim->name <<"' con " << anim->frames.size() << " frames, Speed: " << anim->speed << "\n";
            std::cout << "Animar Normals: "<< anim->UseNormals << "\n";
        }
    }
}

void UpdateAnimations(float dtSeg){
    for (size_t i = 0; i < VertexAnimationActives.size(); ++i) {
        VertexAnimationActives[i]->UpdateAnimation(dtSeg);
    }
}
