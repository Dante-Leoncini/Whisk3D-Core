#include "VertexAnimation.h"
#include "Animation.h"   // ActiveAnimKind/ActiveAnimMesh (kind 3: el playhead manda)
#include "io/w3dFilesystem.h"   // leer los cuadros por el Core (disco / pak / APK)

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cassert>
#include <cstring>
#include <cstdlib>  // strtod/strtol: parseo de los cuadros sin sscanf (reinicia el locale por linea en Symbian)
#include <iostream> // std::cout/cerr (en RVCT no llega transitivo)
#include "base/w3dlog.h" // log PROPIO: nunca std::cout (abria la consola blanca al cargar frames de vertex-anim)
#include "io/W3dResources.h"  // frames compartidos entre instancias (tipo ANIM)


// Normal (-1..1) -> byte con signo (C++03: sin lambdas, el Core compila en Symbian).
static GLbyte NrmFloatToByte(float v) {
    v = ((v + 1.0f) * 0.5f) * 255.0f - 128.0f;
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (GLbyte)v;
}

static void ParseFace(const std::string& line, Face& f) {
    // strtol manual (NO istringstream/sscanf por token: reinicializan el locale y en Symbian eran,
    // junto con el 'v' de abajo, el cuello de la carga de vertex-anim). Formato "f v/vt/vn ..." o
    // "v//vn"; solo interesan vertex (primer entero) y normal (entero tras la SEGUNDA barra).
    const char* p = line.c_str() + 1;   // saltar la 'f'
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char* e;
        long v = strtol(p, &e, 10);
        if (e == p) { p++; continue; }   // token no numerico: avanzar sin colgar
        const char* s = e;
        const char* b1 = 0; const char* b2 = 0;
        while (*s && *s != ' ' && *s != '\t') {
            if (*s == '/') { if (!b1) b1 = s; else b2 = s; }
            s++;
        }
        long n = b2 ? strtol(b2 + 1, NULL, 10) : 0;
        FaceCorner fc;
        fc.vertex = (int)v - 1;
        fc.normal = (int)n - 1;
        f.corners.push_back(fc);
        p = s;
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

                size_t v = (size_t)fc.vertex * 3;
                size_t n = (size_t)fc.normal * 3;
                // indice fuera de rango (OBJ roto): sin este chequeo se escribia/leia
                // fuera de los buffers (out tiene vertexCount verts; tempNormals, los vn leidos)
                if (v + 2 >= vertexCount * 3 || n + 2 >= tempNormals.size()) continue;

                out[v + 0] = tempNormals[n + 0];
                out[v + 1] = tempNormals[n + 1];
                out[v + 2] = tempNormals[n + 2];
            }
        }
    }

    return out;
}

VertexAnimation::VertexAnimation(Mesh* tgt, const std::string& animName, bool useNormals, float Speed, bool Repeat, int NextAnimation):
      frameCount(0), nextAnimation(NextAnimation), speed(Speed),
      padding(0), repeat(Repeat), target(tgt), UseNormals(useNormals), UseUV(false), vcount(0),
      notLoaded(false), startFrame(1), endFrame(250), fps(30) {
    name = animName;
    dataRec = 0;   // frames propios hasta que LoadFrames los comparta
}

// ===========================================================================
//  FRAMES COMPARTIDOS (io/W3dResources.h, tipo ANIM). El equivalente de
//  MeshData para las vertex anims: N objetos que declaran la MISMA
//  Animation { basePath } cargaban N juegos completos de frames (cada frame
//  es la malla entera: es el dato mas pesado del proyecto despues de las
//  texturas). Ahora la primera carga registra sus frames en el almacen y las
//  siguientes ADOPTAN los mismos punteros (+1 ref) sin tocar el disco.
//  La identidad incluye todo lo que cambia el resultado de LoadFrames:
//  basePath|frameCount|padding|startFrame|UseNormals.
// ===========================================================================
struct AnimData {
    std::vector<VertexFrame*> frames;   // duenio de los frames y sus buffers
    int vcount;
    // las capas que traen los frames (las adopta quien los comparte por CLAVE: VertexAnimAdoptKey)
    bool useNormals, useUV;
    AnimData() : vcount(0), useNormals(false), useUV(false) {}
    ~AnimData() {
        for (size_t i = 0; i < frames.size(); ++i) {
            if (!frames[i]) continue;
            delete[] frames[i]->positions;
            delete[] frames[i]->normals;
            delete[] frames[i]->uvs;
            delete frames[i];
        }
    }
    long Bytes() const {
        long b = 0;
        const long nF = (long)vcount * 3, nU = (long)vcount * 2;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (!frames[i]) continue;
            b += (long)sizeof(VertexFrame);
            if (frames[i]->positions) b += nF * (long)sizeof(GLfloat);
            if (frames[i]->normals)   b += nF;
            if (frames[i]->uvs)       b += nU * (long)sizeof(GLfloat);
        }
        return b;
    }
};

static bool gAnimCacheOn = true;
static int  gAnimDesinst = 0;
void ActiveAnimsCache(bool on) { gAnimCacheOn = on; }
bool AnimsCacheIsActive()    { return gAnimCacheOn; }
int  AnimsUninstantiated()    { return gAnimDesinst; }

static bool OpFreeAnim(void* data) { delete (AnimData*)data; return true; }
static void EnsureOpsAnim() {
    static bool ready = false;
    if (ready) return;
    W3dResourceOps ops;              // Cargar NULL: los frames los produce LoadFrames
    ops.Free = OpFreeAnim;
    W3dResourcesRegisterType(W3DREC_ANIM, ops);
    ready = true;
}

static std::string AnimIdShared(const VertexAnimation& a) {
    std::ostringstream id;
    id << a.basePath << "|" << a.frameCount << "|" << a.padding
       << "|" << a.startFrame << "|" << (a.UseNormals ? 1 : 0);
    return id.str();
}

void VertexAnimation::UninstantiateFrames() {
    if (!dataRec) return;
    // copia profunda de TODOS los frames (los compartidos no se tocan)
    const size_t nF = (size_t)(vcount > 0 ? vcount : 0) * 3;
    const size_t nU = (size_t)(vcount > 0 ? vcount : 0) * 2;
    std::vector<VertexFrame*> own;
    own.reserve(frames.size());
    for (size_t k = 0; k < frames.size(); ++k) {
        const VertexFrame* f = frames[k];
        if (!f) { own.push_back(0); continue; }
        VertexFrame* n = new VertexFrame();
        n->frame = f->frame; n->interp = f->interp; n->handleType = f->handleType;
        n->inDF = f->inDF; n->inDV = f->inDV; n->outDF = f->outDF; n->outDV = f->outDV;
        if (f->positions && nF) { GLfloat* p = new GLfloat[nF]; std::memcpy(p, f->positions, nF * sizeof(GLfloat)); n->positions = p; }
        if (f->normals   && nF) { GLbyte*  p = new GLbyte[nF];  std::memcpy(p, f->normals,   nF); n->normals = p; }
        if (f->uvs       && nU) { GLfloat* p = new GLfloat[nU]; std::memcpy(p, f->uvs, nU * sizeof(GLfloat)); n->uvs = p; }
        own.push_back(n);
    }
    frames.swap(own);
    W3dResourceRelease((W3dResource*)dataRec, W3DREC_PERMANENT);
    dataRec = 0;
    gAnimDesinst++;
}

// COPIA PROFUNDA de una vertex anim (para duplicar / separar una malla animada). Cada
// VertexFrame es DUENIO de sus tres buffers (~VertexAnimation los borra con delete[]), asi
// que una copia plana daria dos anims apuntando a la misma memoria y un doble free. Los
// buffers miden vcount*3 (posiciones/normales) y vcount*2 (uv), igual que en el serializador.
VertexAnimation* VertexAnimClone(const VertexAnimation* src, Mesh* newTarget) {
    if (!src) return NULL;
    VertexAnimation* d = new VertexAnimation(newTarget, src->name, src->UseNormals,
                                             src->speed, src->repeat, src->nextAnimation);
    d->basePath   = src->basePath;
    d->frameCount = src->frameCount;
    d->padding    = src->padding;
    d->startFrame = src->startFrame;
    d->endFrame   = src->endFrame;
    d->fps        = src->fps;
    d->UseUV      = src->UseUV;
    d->vcount     = src->vcount;
    d->notLoaded  = src->notLoaded;   // una anim rota se copia rota: la copia tampoco se guarda encima
    d->curves     = src->curves;
    const size_t nF = (size_t)(src->vcount > 0 ? src->vcount : 0) * 3;
    const size_t nU = (size_t)(src->vcount > 0 ? src->vcount : 0) * 2;
    for (size_t k = 0; k < src->frames.size(); k++) {
        const VertexFrame* f = src->frames[k];
        if (!f) continue;
        VertexFrame* n = new VertexFrame();
        n->frame = f->frame; n->interp = f->interp; n->handleType = f->handleType;
        n->inDF = f->inDF; n->inDV = f->inDV; n->outDF = f->outDF; n->outDV = f->outDV;
        if (f->positions && nF) { GLfloat* p = new GLfloat[nF]; std::memcpy(p, f->positions, nF * sizeof(GLfloat)); n->positions = p; }
        if (f->normals   && nF) { GLbyte*  p = new GLbyte[nF];  std::memcpy(p, f->normals,   nF); n->normals = p; }
        if (f->uvs       && nU) { GLfloat* p = new GLfloat[nU]; std::memcpy(p, f->uvs, nU * sizeof(GLfloat)); n->uvs = p; }
        d->frames.push_back(n);
    }
    return d;
}

// ===========================================================================
//  FRAMES COMPARTIDOS POR CLAVE / CON OTRA ANIM (ver el .h). Mismo modelo que los de
//  LoadFrames: el AnimData del almacen es el duenio, cada anim que los usa tiene UNA ref
//  y cualquier edicion de keyframes pasa antes por UninstantiateFrames (COW).
// ===========================================================================
// los frames PROPIOS de 'anim' pasan a ser el dato del almacen bajo 'id' (la anim se queda con la ref
// que deja el registro). false = no habia nada que registrar o el id ya existe
static bool RegisterFramesOwn(VertexAnimation& anim, const std::string& id) {
    if (anim.dataRec || anim.frames.empty() || id.empty()) return false;
    EnsureOpsAnim();
    if (W3dResourceFind(W3DREC_ANIM, id)) return false;
    AnimData* d = new AnimData();
    d->frames = anim.frames;
    d->vcount = anim.vcount;
    d->useNormals = anim.UseNormals;
    d->useUV = anim.UseUV;
    W3dResource* r = W3dResourceRegisterExternal(W3DREC_ANIM, id, d, d->Bytes());
    if (!r) { d->frames.clear(); delete d; return false; }
    anim.dataRec = r;
    return true;
}
// 'anim' toma los frames de 'r' (+1 ref): los mismos punteros, con sus capas y su conteo de vertices
static void AdoptFrom(VertexAnimation& anim, W3dResource* r) {
    AnimData* d = (AnimData*)r->data;
    anim.FreeFrames();
    anim.frames = d->frames;
    anim.vcount = d->vcount;
    anim.UseNormals = d->useNormals;
    anim.UseUV = d->useUV;
    anim.frameCount = (int)d->frames.size();
    anim.dataRec = r;
    W3dResourceRetain(r, W3DREC_PERMANENT);
}

bool VertexAnimAdoptKey(VertexAnimation& anim, const std::string& key, int vcountMesh) {
    if (!gAnimCacheOn || key.empty()) return false;
    EnsureOpsAnim();
    W3dResource* r = W3dResourceFind(W3DREC_ANIM, key);
    if (!r || r->state != W3DREC_READY || !r->data) return false;
    const AnimData* d = (const AnimData*)r->data;
    if (d->frames.empty() || d->vcount != vcountMesh) return false;   // (armados para otra malla: que lea los suyos)
    AdoptFrom(anim, r);
    return true;
}

void VertexAnimRegisterKey(VertexAnimation& anim, const std::string& key) {
    if (!gAnimCacheOn) return;
    RegisterFramesOwn(anim, key);
}

VertexAnimation* VertexAnimShare(VertexAnimation* src, Mesh* newTarget) {
    if (!src) return NULL;
    if (!gAnimCacheOn || src->frames.empty()) return VertexAnimClone(src, newTarget);
    if (!src->dataRec) {
        // sus frames eran PROPIOS (una anim sin guardar): pasan al almacen con un id de memoria, unico
        static unsigned sNext = 1;
        std::ostringstream id;
        id << "mem:" << sNext++;
        if (!RegisterFramesOwn(*src, id.str())) return VertexAnimClone(src, newTarget);
    }
    // la copia: todo lo de la anim como VertexAnimClone, pero los frames son los MISMOS
    VertexAnimation* d = new VertexAnimation(newTarget, src->name, src->UseNormals,
                                             src->speed, src->repeat, src->nextAnimation);
    d->basePath   = src->basePath;
    d->padding    = src->padding;
    d->startFrame = src->startFrame;
    d->endFrame   = src->endFrame;
    d->fps        = src->fps;
    d->notLoaded  = src->notLoaded;
    d->curves     = src->curves;
    AdoptFrom(*d, (W3dResource*)src->dataRec);
    // (lo que dice la anim de origen manda: el dato de LoadFrames no anota sus capas)
    d->frameCount = src->frameCount;
    d->vcount     = src->vcount;
    d->UseNormals = src->UseNormals;
    d->UseUV      = src->UseUV;
    return d;
}

void VertexAnimation::FreeFrames() {
    if (dataRec) {   // los frames son del dato compartido: solo se va la ref
        frames.clear();
        W3dResourceRelease((W3dResource*)dataRec, W3DREC_PERMANENT);
        dataRec = 0;
        return;
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i]) continue;
        delete[] frames[i]->positions;
        delete[] frames[i]->normals;
        delete[] frames[i]->uvs;
        delete frames[i];
    }
    frames.clear();
}

VertexAnimation::~VertexAnimation() { FreeFrames(); }

bool VertexAnimation::LoadFrames() {
    if (!target || !target->vertex || target->vertexSize <= 0)
        return false;

    FreeFrames();   // recargar sin fugar los frames anteriores

    // FRAMES COMPARTIDOS: si otra instancia ya cargo este mismo basePath,
    // adoptar sus frames (+1 ref) sin tocar el disco.
    if (gAnimCacheOn && frameCount > 0 && !basePath.empty()) {
        EnsureOpsAnim();
        W3dResource* r = W3dResourceFind(W3DREC_ANIM, AnimIdShared(*this));
        if (r && r->state == W3DREC_READY && r->data) {
            AnimData* d = (AnimData*)r->data;
            if (d->vcount == target->vertexSize && !d->frames.empty()) {
                frames = d->frames;                 // mismos punteros
                vcount = d->vcount;
                endFrame = d->frames.back()->frame; // mismo cierre que la carga
                dataRec  = r;
                W3dResourceRetain(r, W3DREC_PERMANENT);
                return true;
            }
        }
    }

    for (int i = 1; i <= frameCount; ++i) {

        std::ostringstream path;
        path << basePath
             << std::setw(padding)
             << std::setfill('0')
             << i
             << ".obj";

        // POR LA ABSTRACCION DEL CORE (no ifstream): los cuadros de la animacion
        // pueden venir del pak embebido o de ADENTRO DEL APK, donde no son
        // archivos reales. Con ifstream, en Android TODA la vertex animation
        // (el personaje, los enemigos) se quedaba sin cargar.
        bool okFrm = false;
        const std::string dataFrm = w3dFileSystem::ReadTextFile(path.str(), &okFrm);
        if (!okFrm) {
            std::cerr << "[Anim] No se pudo abrir " << path.str() << "\n";
            return false;
        }
        std::istringstream file(dataFrm);

        std::vector<GLfloat> verts;
        std::vector<GLbyte>  tempNormals;
        std::vector<Face>    faces;

        std::string line;


        // ---------- PARSE OBJ ----------
        while (std::getline(file, line)) {

            if (line.rfind("v ", 0) == 0) {
                // strtod (NO sscanf: en Symbian sscanf de floats reinicializa el locale por linea ->
                // era EL cuello de la carga de vertex-anim, ~137 s por miles de frames en el N95).
                char* e; const char* q = line.c_str() + 2;
                double x = strtod(q, &e); double y = strtod(e, &e); double z = strtod(e, &e);
                verts.push_back((GLfloat)x);
                verts.push_back((GLfloat)y);
                verts.push_back((GLfloat)z);
            }
            else if (UseNormals && line.rfind("vn ", 0) == 0) {
                char* e; const char* q = line.c_str() + 3;
                double nx = strtod(q, &e); double ny = strtod(e, &e); double nz = strtod(e, &e);
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

        // ---------- VALIDACION ----------
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
        std::memcpy(pos, &verts[0], verts.size() * sizeof(GLfloat));   // &verts[0]: vector::data() es C++11 (RVCT no lo tiene)
        frame->positions = pos;

        frame->normals = finalNormals; // puede ser NULL

        // MIGRACION al modelo por keyframes: la anim vieja era CONTIGUA (frame 1,2,3...)
        // y LINEAL. Cada .obj importado pasa a ser un KEYFRAME en un cuadro consecutivo
        // (startFrame + i) con interpolacion LINEAL -> reproduce IGUAL que antes, pero
        // ahora se pueden mover/borrar keyframes y dejar huecos con curva.
        frame->frame = startFrame + (i - 1);
        frame->interp = KfLinear;

        frames.push_back(frame);
    }

    // el rango de la anim importada = el span real de sus keyframes (no el 250 default)
    if (!frames.empty()) endFrame = frames.back()->frame;
    vcount = target->vertexSize;   // los frames (.obj) estan armados para este conteo

    // FRAMES COMPARTIDOS: registrar esta carga como el dato del id (AnimData
    // adopta los frames; esta instancia queda con la ref que deja el registro).
    if (gAnimCacheOn && !frames.empty() && !basePath.empty()) {
        EnsureOpsAnim();
        const std::string id = AnimIdShared(*this);
        if (!W3dResourceFind(W3DREC_ANIM, id)) {
            AnimData* d = new AnimData();
            d->frames = frames;
            d->vcount = vcount;
            d->useNormals = UseNormals;
            d->useUV = UseUV;
            W3dResource* nr = W3dResourceRegisterExternal(W3DREC_ANIM, id, d, d->Bytes());
            if (nr) dataRec = nr;
            else { d->frames.clear(); delete d; }
        }
    }

    return true;
}

void ApplyVertexFrame(const VertexAnimation& anim, size_t frameIndex) {
    assert(anim.target);
    if (anim.frames.empty()) return;   // sin frames el clamp de abajo daria (size_t)-1 -> lectura fuera de rango
    // COW (MeshData.h): el playback escribe vertex[] (+normales/uv si las usa).
    // La POSE es POR INSTANCIA: la geometria compartida se desinstancia aca.
    anim.target->UninstantiateData(W3DMD_POS | (anim.UseNormals ? W3DMD_NOR : 0)
                                              | (anim.UseUV ? W3DMD_UV : 0));

    if (frameIndex >= anim.frames.size()) {
        frameIndex = anim.frames.size() - 1;
    }

    Mesh* mesh = anim.target;
    const VertexFrame* frame = anim.frames[frameIndex];

    // ---------- posiciones (3 floats por VERTICE; el frame puede ser solo-UV) ----------
    const size_t nFloats = (size_t)mesh->vertexSize * 3;
    if (frame->positions) {
        const GLfloat* srcPos = frame->positions;
        for (size_t i = 0; i < nFloats; ++i) {
            mesh->vertex[i] = srcPos[i];
        }
        // ESTADO REAL DE POSE (ver Mesh::posedByAnim): en reposo solo si lo que se
        // copio es el KEYFRAME BASE (el primero con posiciones de esta anim).
        const VertexFrame* base = NULL;
        for (size_t k = 0; k < anim.frames.size() && !base; ++k)
            if (anim.frames[k] && anim.frames[k]->positions) base = anim.frames[k];
        mesh->posedByAnim = (frame != base);
    }

    // ---------- normales (capa propia, opcional, 3 bytes por VERTICE) ----------
    if (frame->normals && mesh->normals) {
        const GLbyte* srcNrm = frame->normals;
        for (size_t i = 0; i < nFloats; ++i) {
            mesh->normals[i] = srcNrm[i];
        }
    }
    // ---------- UV (opcional, 2 floats por VERTICE) ----------
    if (anim.UseUV && frame->uvs && mesh->uv) {
        const size_t nUV = (size_t)mesh->vertexSize * 2;
        for (size_t i = 0; i < nUV; ++i) mesh->uv[i] = frame->uvs[i];
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

    // COW (MeshData.h): la mezcla escribe vertex[] (+normales/uv si las usan)
    if (mesh) mesh->UninstantiateData(W3DMD_POS
                | ((fromAnim.UseNormals && toAnim.UseNormals) ? W3DMD_NOR : 0)
                | ((fromAnim.UseUV && toAnim.UseUV) ? W3DMD_UV : 0));

    // Clamp rapido
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

    // ---------- posiciones (3 floats por VERTICE; frames solo-UV no aportan) ----------
    const size_t nFloats = (size_t)mesh->vertexSize * 3;
    if (A->positions && B->positions) {
    for (size_t i = 0; i < nFloats; ++i) {
        mesh->vertex[i] =
            A->positions[i] * (1.0f - blendT) +
            B->positions[i] * blendT;
    }
    // ESTADO REAL DE POSE (ver Mesh::posedByAnim): una MEZCLA de dos clips nunca
    // es el keyframe base (blendT ya vino clampeado a (0,1) mas arriba: los
    // extremos se fueron por ApplyVertexFrame, que decide el flag por su cuenta).
    mesh->posedByAnim = true;
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

    // ---------- UV (solo si ambas animaciones las animan) ----------
    if (fromAnim.UseUV && toAnim.UseUV && A->uvs && B->uvs && mesh->uv) {
        const size_t nUV = (size_t)mesh->vertexSize * 2;
        for (size_t i = 0; i < nUV; ++i)
            mesh->uv[i] = A->uvs[i] * (1.0f - blendT) + B->uvs[i] * blendT;
    }

    // el render por VBO solo RE-SUBE la geometria cuando cambia su version: sin
    // esto, la vertex animation escribia los vertices y la pantalla quedaba
    // CONGELADA en la pose vieja (bug del refactor de VBOs)
    mesh->skinGeomVersion++;
}

// ============================================================================
//  EVALUADOR COMUN del EASE (SIN overshoot) — ver el contrato en VertexAnimation.h.
//  La reproduccion (EvalVertexAnim) y el editor de curvas (VertexChannelTemp en
//  Timeline.cpp) usan ESTAS funciones: la curva dibujada = la curva real.
// ============================================================================
void VertexAnimClampHandle(bool output, float spanFrames, float& dF, float& dV) {
    if (spanFrames < 0.0f) spanFrames = 0.0f;
    if (output) {
        // handle derecho del key IZQUIERDO: tiempo hacia adelante dentro del tramo,
        // punto de control de valor entre 0 (su key) y 1 (el otro)
        if (dF < 0.0f) dF = 0.0f;               if (dF > spanFrames) dF = spanFrames;
        if (dV < 0.0f) dV = 0.0f;               if (dV > 1.0f) dV = 1.0f;
    } else {
        // handle izquierdo del key DERECHO: tiempo hacia atras, control entre 0 y 1
        if (dF > 0.0f) dF = 0.0f;               if (dF < -spanFrames) dF = -spanFrames;
        if (dV > 0.0f) dV = 0.0f;               if (dV < -1.0f) dV = -1.0f;
    }
}

// el tramo A->B como curva de 2 keyframes (valor 0 -> 1), con los datos del VertexFrame
static void VertexAnimSegBase(const VertexFrame& A, const VertexFrame& B, AnimProperty& seg) {
    keyFrame ka; ka.frame = A.frame; ka.value = 0.0f; ka.Interpolation = A.interp;
    ka.handleType = A.handleType; ka.inDF = A.inDF; ka.inDV = A.inDV; ka.outDF = A.outDF; ka.outDV = A.outDV;
    keyFrame kb; kb.frame = B.frame; kb.value = 1.0f; kb.Interpolation = B.interp;
    kb.handleType = B.handleType; kb.inDF = B.inDF; kb.inDV = B.inDV; kb.outDF = B.outDF; kb.outDV = B.outDV;
    seg.keyframes.clear();
    seg.keyframes.push_back(ka); seg.keyframes.push_back(kb);
}

void VertexAnimHandlesSegment(const VertexFrame& A, const VertexFrame& B,
                            float& outDF, float& outDV, float& inDF, float& inDV) {
    AnimProperty seg; VertexAnimSegBase(A, B, seg);
    seg.EffectiveHandle(0, true,  outDF, outDV);  // resuelve tambien los CALCULADOS (Vector/Auto/...)
    seg.EffectiveHandle(1, false, inDF,  inDV);
    const float span = (float)(B.frame - A.frame);
    VertexAnimClampHandle(true,  span, outDF, outDV);
    VertexAnimClampHandle(false, span, inDF,  inDV);
}

float EvalCurveVertexAnim(const VertexFrame& A, const VertexFrame& B, float frame) {
    const int span = B.frame - A.frame;
    if (span <= 0) return 1.0f;                       // keys pisados: manda el derecho
    if (frame <= (float)A.frame) return 0.0f;
    if (frame >= (float)B.frame) return 1.0f;
    if (A.interp == KfConstant) return 0.0f;          // escalon: A hasta llegar a B
    const float t = (frame - (float)A.frame) / (float)span;
    if (KfIsEasing(A.interp)) return KfEasing(A.interp, t);   // easing: la misma curva que las de transform
    if (A.interp != KfBezier) return t;               // lineal
    // BEZIER con los handles efectivos CLAMPEADOS al rectangulo del tramo (los MISMOS
    // que dibuja/agarra el editor): los 4 puntos de control quedan en la caja
    // [A.frame..B.frame] x [0..1] -> la cubica es monotona y no se sale de 0..1.
    float aDF, aDV, bDF, bDV;
    VertexAnimHandlesSegment(A, B, aDF, aDV, bDF, bDV);
    AnimProperty seg; VertexAnimSegBase(A, B, seg);
    seg.keyframes[0].handleType = HFree; seg.keyframes[0].outDF = aDF; seg.keyframes[0].outDV = aDV;
    seg.keyframes[1].handleType = HFree; seg.keyframes[1].inDF  = bDF; seg.keyframes[1].inDV  = bDV;
    float v = seg.EvalF(frame, 0.0f);
    if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;  // cinturon (la caja ya lo garantiza)
    return v;
}

// que CAPA trae un frame? (0 = posiciones, 1 = uv, 2 = normales). El criterio unico de
// "capa presente" que comparten la evaluacion, la busqueda de tramos y el move selectivo.
static bool VertexFrameBrings(const VertexFrame* f, int layer) {
    if (!f) return false;
    if (layer == 0) return f->positions != NULL;
    if (layer == 1) return f->uvs != NULL;
    return f->normals != NULL;
}

// ---- aplicar UNA capa a la malla (t=0 -> A, t=1 -> B; B NULL = HOLD A) ----
static void ApplyLayerPos(const VertexFrame* A, const VertexFrame* B, float t, Mesh* mesh) {
    if (!A || !A->positions || !mesh->vertex) return;
    const size_t nF = (size_t)mesh->vertexSize * 3;
    const VertexFrame* S = NULL;                       // copia directa de un solo frame?
    if (!B || !B->positions || t <= 0.0f) S = A;
    else if (t >= 1.0f) S = B;
    if (S) {
        for (size_t i = 0; i < nF; ++i) mesh->vertex[i] = S->positions[i];
        return;
    }
    for (size_t i = 0; i < nF; ++i)
        mesh->vertex[i] = A->positions[i] * (1.0f - t) + B->positions[i] * t;
}
// capa NORMALES: independiente de las posiciones (frames propios; si la anim no tiene
// ningun frame con normales, EvalVertexAnim ni entra aca y la malla conserva las suyas)
static void ApplyLayerNrm(const VertexFrame* A, const VertexFrame* B, float t, Mesh* mesh) {
    if (!A || !A->normals || !mesh->normals) return;
    const size_t nF = (size_t)mesh->vertexSize * 3;
    const VertexFrame* S = NULL;
    if (!B || !B->normals || t <= 0.0f) S = A;
    else if (t >= 1.0f) S = B;
    if (S) { for (size_t i = 0; i < nF; ++i) mesh->normals[i] = S->normals[i]; return; }
    for (size_t i = 0; i < nF; ++i) {
        float na = NrmByteToFloat(A->normals[i]);
        float nb = NrmByteToFloat(B->normals[i]);
        mesh->normals[i] = NrmFloatToByte(na * (1.0f - t) + nb * t);
    }
}
static void ApplyLayerUV(const VertexFrame* A, const VertexFrame* B, float t, Mesh* mesh) {
    if (!A || !A->uvs || !mesh->uv) return;
    const size_t nU = (size_t)mesh->vertexSize * 2;
    const VertexFrame* S = NULL;
    if (!B || !B->uvs || t <= 0.0f) S = A;
    else if (t >= 1.0f) S = B;
    if (S) { for (size_t i = 0; i < nU; ++i) mesh->uv[i] = S->uvs[i]; return; }
    // UV morph: recta entre el mapeo de A y el de B (mismo criterio que las posiciones)
    for (size_t i = 0; i < nU; ++i)
        mesh->uv[i] = A->uvs[i] * (1.0f - t) + B->uvs[i] * t;
}

void EvalVertexAnim(const VertexAnimation& anim, Mesh* mesh, float frame) {
    if (!mesh) return;
    // GUARD anti-crash: los frames son paralelos a mesh->vertex. Si la topologia cambio y
    // los frames NO se remapearon (vcount != vertexSize), evaluar leeria fuera de rango.
    // Mejor no aplicar la anim que reventar. Normalmente RemapVertexAnims los mantiene en sync.
    if (anim.vcount != 0 && anim.vcount != mesh->vertexSize) return;
    const std::vector<VertexFrame*>& F = anim.frames;
    if (F.empty()) return;
    // COW (MeshData.h): la evaluacion escribe vertex[] (+normales/uv segun capas).
    mesh->UninstantiateData(W3DMD_POS | (anim.UseNormals ? W3DMD_NOR : 0)
                                       | (anim.UseUV ? W3DMD_UV : 0));

    // CADA CAPA se evalua POR SU CUENTA (curvas separadas): las posiciones interpolan
    // entre los frames que traen positions, las UV entre los que traen uvs y las
    // NORMALES entre los que traen normals. Un frame solo-UV NO corta el tramo de
    // posiciones (y al reves). Sin ningun frame con normales (capa borrada), la pasa
    // de normales no encuentra tramo y se saltea entera: la malla conserva las suyas.
    bool something = false;
    for (int layer = 0; layer < 3; ++layer) {
        // el par [A,B] de ESTA capa que rodea 'frame' (los frames van ordenados)
        const VertexFrame* A = NULL; const VertexFrame* B = NULL;
        const VertexFrame* first = NULL;            // primer keyframe de la capa (el BASE, capa 0)
        for (size_t i = 0; i < F.size(); ++i) {
            const VertexFrame* f = F[i]; if (!f) continue;
            if (!VertexFrameBrings(f, layer)) continue;
            if (!first) first = f;                    // el 1ro de la capa: siempre antes que A y que B
            if ((float)f->frame <= frame) A = f;      // el ultimo a la izquierda (o el mismo cuadro)
            else { B = f; break; }                    // el primero a la derecha
        }
        if (!A && !B) continue;                       // la capa no tiene keyframes
        float t = 0.0f;
        if (!A) { A = B; B = NULL; }                  // antes del primero: HOLD el primero
        else if (B) t = EvalCurveVertexAnim(*A, *B, frame);  // el ease COMUN (mismo que dibuja el editor)
        // despues del ultimo: B quedo NULL -> HOLD A (igual que las curvas de canal)
        if (layer == 0) {
            ApplyLayerPos(A, B, t, mesh);
            // ESTADO REAL DE POSE (ver Mesh::posedByAnim): la malla queda EN REPOSO
            // solo si lo que se acaba de escribir es EXACTAMENTE el keyframe BASE, o
            // sea A == el primer keyframe con posiciones y sin mezcla (t == 0: la
            // interpolacion con t=0 copia A tal cual, bit a bit). Cualquier otro
            // cuadro la deja POSADA. Nadie mas escribe este flag: una edicion sin
            // autokey mueve vertex[] y NO la posa, que era la regresion.
            mesh->posedByAnim = !(A == first && t == 0.0f);
        }
        else if (layer == 1) ApplyLayerUV(A, B, t, mesh);
        else                ApplyLayerNrm(A, B, t, mesh);
        something = true;
    }
    if (something) mesh->skinGeomVersion++;   // re-subir el VBO (ver BlendVertexAnimations)
}

int VertexAnimSetKey(VertexAnimation& anim, int frame,
                     const GLfloat* positions, const GLbyte* normals,
                     const GLfloat* uvs, int interp) {
    if (!anim.target || anim.target->vertexSize <= 0 || (!positions && !uvs && !normals)) return -1;
    anim.UninstantiateFrames();   // COW: se van a mutar los keyframes
    const size_t nFloats = (size_t)anim.target->vertexSize * 3;
    const size_t nUV = (size_t)anim.target->vertexSize * 2;
    anim.vcount = anim.target->vertexSize;   // los frames quedan armados para este conteo
    if (uvs) anim.UseUV = true;              // primer keyframe con UV -> la anim anima UV
    if (normals) anim.UseNormals = true;     // idem para la capa de normales

    // copias propias (el llamador conserva sus buffers). Cada capa por su cuenta:
    // insertar keyframe desde el 3D pasa posiciones+normales (siguen normalmente
    // juntas), pero la capa de normales tambien puede venir SOLA.
    GLfloat* pos = NULL; GLbyte* nrm = NULL; GLfloat* tuv = NULL;
    if (positions) {
        pos = new GLfloat[nFloats];
        std::memcpy(pos, positions, nFloats * sizeof(GLfloat));
    }
    if (normals) { nrm = new GLbyte[nFloats]; std::memcpy(nrm, normals, nFloats * sizeof(GLbyte)); }
    if (uvs) { tuv = new GLfloat[nUV]; std::memcpy(tuv, uvs, nUV * sizeof(GLfloat)); }

    // reemplazar si ya hay un keyframe EXACTO en ese cuadro: SOLO pisa las capas que
    // vienen; las otras capas del frame se CONSERVAN (insertar un keyframe de vertices
    // no borra el de UV del mismo cuadro, y al reves - son curvas separadas)
    for (size_t k = 0; k < anim.frames.size(); ++k) {
        if (anim.frames[k]->frame != frame) continue;
        VertexFrame* f = anim.frames[k];
        if (pos) {
            delete[] (GLfloat*)f->positions;
            f->positions = pos;
        }
        if (nrm) {
            delete[] (GLbyte*)f->normals;
            f->normals = nrm;
        }
        if (tuv) {
            delete[] (GLfloat*)f->uvs;
            f->uvs = tuv;
        }
        f->interp = interp;
        anim.frameCount = (int)anim.frames.size();
        return (int)k;
    }

    // insertar ORDENADO por frame
    VertexFrame* nf = new VertexFrame();
    nf->positions = pos; nf->normals = nrm; nf->uvs = tuv; nf->frame = frame; nf->interp = interp;
    if (interp == KfBezier) nf->handleType = HPlane;   // ease in/out (igual que SetKeyCurve)
    size_t ins = 0;
    while (ins < anim.frames.size() && anim.frames[ins]->frame < frame) ++ins;
    anim.frames.insert(anim.frames.begin() + ins, nf);
    anim.frameCount = (int)anim.frames.size();
    return (int)ins;
}

bool VertexAnimDeleteFrame(VertexAnimation& anim, int frame) {
    anim.UninstantiateFrames();   // COW: se borran keyframes
    for (size_t k = 0; k < anim.frames.size(); ++k) {
        if (anim.frames[k]->frame != frame) continue;
        VertexFrame* f = anim.frames[k];
        delete[] (GLfloat*)f->positions;
        delete[] (GLbyte*)f->normals;
        delete[] (GLfloat*)f->uvs;
        delete f;
        anim.frames.erase(anim.frames.begin() + k);
        anim.frameCount = (int)anim.frames.size();
        return true;
    }
    return false;
}

bool VertexAnimDeleteLayer(VertexAnimation& anim, int frame, int layer) {
    anim.UninstantiateFrames();   // COW: se mutan los keyframes
    for (size_t k = 0; k < anim.frames.size(); ++k) {
        VertexFrame* f = anim.frames[k];
        if (!f || f->frame != frame) continue;
        if (layer == 0) {
            if (!f->positions) return false;             // este frame no keyframea posiciones
            delete[] (GLfloat*)f->positions; f->positions = NULL;
        } else if (layer == 1) {
            if (!f->uvs) return false;                   // este frame no keyframea UV
            delete[] (GLfloat*)f->uvs; f->uvs = NULL;
        } else {
            if (!f->normals) return false;               // este frame no keyframea normales
            delete[] (GLbyte*)f->normals; f->normals = NULL;
        }
        if (!f->positions && !f->uvs && !f->normals) {   // sin capas: el frame ya no existe
            delete f;
            anim.frames.erase(anim.frames.begin() + k);
            anim.frameCount = (int)anim.frames.size();
        }
        if (layer == 1) {                                 // el espejo UseUV sigue a los datos
            bool has = false;
            for (size_t i = 0; i < anim.frames.size(); ++i)
                if (anim.frames[i] && anim.frames[i]->uvs) { has = true; break; }
            anim.UseUV = has;
        }
        if (layer == 2) {                                 // idem el espejo UseNormals
            bool has = false;
            for (size_t i = 0; i < anim.frames.size(); ++i)
                if (anim.frames[i] && anim.frames[i]->normals) { has = true; break; }
            anim.UseNormals = has;
        }
        return true;
    }
    return false;
}

bool VertexAnimDeleteLayerNormals(VertexAnimation& anim) {
    anim.UninstantiateFrames();   // COW: se borran capas de los keyframes
    bool any = false;
    for (size_t k = anim.frames.size(); k-- > 0; ) {
        VertexFrame* f = anim.frames[k];
        if (!f || !f->normals) continue;
        delete[] (GLbyte*)f->normals; f->normals = NULL;
        any = true;
        if (!f->positions && !f->uvs) {                  // era solo-normales: se va entero
            delete f;
            anim.frames.erase(anim.frames.begin() + k);
        }
    }
    anim.frameCount = (int)anim.frames.size();
    anim.UseNormals = false;   // EvalVertexAnim ya no encuentra tramo de normales: se la saltea
    return any;
}

void VertexAnimSetInterp(VertexAnimation& anim, int frame, int interp) {
    for (size_t k = 0; k < anim.frames.size(); ++k)
        if (anim.frames[k]->frame == frame) { anim.frames[k]->interp = interp; return; }
}

void VertexAnimSort(VertexAnimation& anim) {
    // insertion sort por 'frame' (C++03: sin lambdas; ademas son pocos keyframes). NO
    // borra colisiones: los punteros siguen todos vivos (el arrastre los referencia).
    std::vector<VertexFrame*>& F = anim.frames;
    for (size_t i = 1; i < F.size(); ++i) {
        VertexFrame* cur = F[i];
        size_t j = i;
        while (j > 0 && F[j-1]->frame > cur->frame) { F[j] = F[j-1]; --j; }
        F[j] = cur;
    }
}

// ============================================================================
//  MOVE SELECTIVO por MASCARA de vertices - el nucleo del toggle "Solo vertices
//  seleccionados" del dope sheet / editor de curvas. Contrato en VertexAnimation.h.
// ============================================================================
// el par [A,B] de la capa que rodea 'frame' (mismo criterio que EvalVertexAnim). 'ignorar' saca UN keyframe de
// la cuenta: sirve para preguntar "cuanto valdria esta capa aca SI este keyframe no existiera" (lo usa el move
// selectivo para sacarle al origen la contribucion de los vertices seleccionados).
static void VertexAnimLayerAB(const std::vector<VertexFrame*>& F, int layer, float frame,
                             const VertexFrame** Aout, const VertexFrame** Bout,
                             const VertexFrame* ignore = NULL) {
    const VertexFrame* A = NULL; const VertexFrame* B = NULL;
    for (size_t i = 0; i < F.size(); ++i) {
        const VertexFrame* f = F[i]; if (f == ignore) continue;
        if (!VertexFrameBrings(f, layer)) continue;
        if ((float)f->frame <= frame) A = f;
        else { B = f; break; }
    }
    if (!A) { A = B; B = NULL; }                     // antes del primero: HOLD el primero
    *Aout = A; *Bout = B;
}
// evalua UNA capa de la anim en 'frame' a un BUFFER (no toca la malla): outF para
// posiciones (vc*3) / uv (vc*2), outB para normales (vc*3). Mismo interpolador que la
// reproduccion (EvalCurveVertexAnim). Si la capa no tiene keyframes no escribe nada.
// Devuelve false si la capa no tenia NINGUN keyframe (sin 'ignorar'): el buffer queda como estaba.
static bool VertexAnimEvalLayerBuffer(const VertexAnimation& anim, int layer, float frame,
                                     int vc, GLfloat* outF, GLbyte* outB,
                                     const VertexFrame* ignore = NULL) {
    const VertexFrame* A; const VertexFrame* B;
    VertexAnimLayerAB(anim.frames, layer, frame, &A, &B, ignore);
    if (!A) return false;
    float t = B ? EvalCurveVertexAnim(*A, *B, frame) : 0.0f;
    if (layer == 0 && outF) {
        const size_t n = (size_t)vc * 3;
        if (!B || t <= 0.0f) { for (size_t i = 0; i < n; ++i) outF[i] = A->positions[i]; }
        else if (t >= 1.0f)  { for (size_t i = 0; i < n; ++i) outF[i] = B->positions[i]; }
        else for (size_t i = 0; i < n; ++i) outF[i] = A->positions[i]*(1.0f-t) + B->positions[i]*t;
    } else if (layer == 1 && outF) {
        const size_t n = (size_t)vc * 2;
        if (!B || t <= 0.0f) { for (size_t i = 0; i < n; ++i) outF[i] = A->uvs[i]; }
        else if (t >= 1.0f)  { for (size_t i = 0; i < n; ++i) outF[i] = B->uvs[i]; }
        else for (size_t i = 0; i < n; ++i) outF[i] = A->uvs[i]*(1.0f-t) + B->uvs[i]*t;
    } else if (layer == 2 && outB) {
        const size_t n = (size_t)vc * 3;
        if (!B || t <= 0.0f) { for (size_t i = 0; i < n; ++i) outB[i] = A->normals[i]; }
        else if (t >= 1.0f)  { for (size_t i = 0; i < n; ++i) outB[i] = B->normals[i]; }
        else for (size_t i = 0; i < n; ++i)
            outB[i] = NrmFloatToByte(NrmByteToFloat(A->normals[i])*(1.0f-t) + NrmByteToFloat(B->normals[i])*t);
    }
    return true;
}
// frame vivo en 'frame' (o NULL). 'salvo' = punteros a IGNORAR (los que se estan moviendo)
static VertexFrame* VertexAnimFrameIn(VertexAnimation& anim, int frame,
                                      const std::vector<VertexFrame*>* except) {
    for (size_t k = 0; k < anim.frames.size(); ++k) {
        VertexFrame* f = anim.frames[k];
        if (!f || f->frame != frame) continue;
        if (except) { bool isMoved = false;
            for (size_t i = 0; i < except->size(); ++i) if ((*except)[i] == f) { isMoved = true; break; }
            if (isMoved) continue; }
        return f;
    }
    return NULL;
}

// un keyframe APORTA algo (para los vertices de 'mask') si sus valores en esa capa difieren de lo que la capa
// evaluaria en ese mismo cuadro SIN el. Es la definicion de "este cuadro le anima algo a estos vertices": un key
// que cae justo sobre la recta/curva de sus vecinos no cambia nada, es un cuadro de paso.
// mask NULL = todos los vertices. Tolerancias: posiciones/UV en float, normales en bytes (+-1 = redondeo).
static bool VertexLayerContributes(const VertexAnimation& anim, const VertexFrame* f, int layer, int vc,
                             const unsigned char* mask) {
    if (!VertexFrameBrings(f, layer)) return false;
    const size_t n = (layer == 1) ? (size_t)vc*2 : (size_t)vc*3;
    if (layer == 2) {
        std::vector<GLbyte> b(n, 0);
        if (!VertexAnimEvalLayerBuffer(anim, layer, (float)f->frame, vc, NULL, &b[0], f)) return true; // sin vecinos: aporta
        for (int v = 0; v < vc; ++v) { if (mask && !mask[v]) continue;
            for (int c = 0; c < 3; ++c){ int d = (int)f->normals[v*3+c] - (int)b[v*3+c]; if (d < 0) d = -d; if (d > 1) return true; } }
        return false;
    }
    std::vector<GLfloat> b(n, 0.0f);
    if (!VertexAnimEvalLayerBuffer(anim, layer, (float)f->frame, vc, &b[0], NULL, f)) return true;
    const int cn = (layer == 1) ? 2 : 3;
    const float tol = (layer == 1) ? 1e-6f : 1e-5f;
    const GLfloat* src = (layer == 1) ? f->uvs : f->positions;
    for (int v = 0; v < vc; ++v) { if (mask && !mask[v]) continue;
        for (int c = 0; c < cn; ++c) if (fabsf(src[v*cn+c] - b[v*cn+c]) > tol) return true; }
    return false;
}
bool VertexAnimKeyContributesSel(const VertexAnimation& anim, int layer, int frame, const unsigned char* mask) {
    const int vc = (anim.vcount > 0) ? anim.vcount : (anim.target ? anim.target->vertexSize : 0);
    if (vc <= 0) return true;
    for (size_t k = 0; k < anim.frames.size(); ++k)
        if (anim.frames[k] && anim.frames[k]->frame == frame)
            return VertexLayerContributes(anim, anim.frames[k], layer, vc, mask);
    return false;
}

bool VertexAnimMoveKeysSelective(VertexAnimation& anim,
                                  const std::vector<int>& origin,
                                  const std::vector<int>& destination,
                                  const unsigned char* mask) {
    if (!mask || origin.empty() || origin.size() != destination.size()) return false;
    anim.UninstantiateFrames();   // COW: move selectivo escribe los frames
    const int vc = (anim.vcount > 0) ? anim.vcount : (anim.target ? anim.target->vertexSize : 0);
    if (vc <= 0) return false;
    const size_t nF = (size_t)vc * 3, nU = (size_t)vc * 2;

    // los frames FUENTE (se descartan pares sin frame o con origen == destino)
    std::vector<VertexFrame*> src; std::vector<int> dst;
    for (size_t i = 0; i < origin.size(); ++i) {
        if (origin[i] == destination[i]) continue;
        VertexFrame* f = VertexAnimFrameIn(anim, origin[i], NULL);
        if (!f) continue;
        src.push_back(f); dst.push_back(destination[i]);
    }
    if (src.empty()) return false;

    // ---- PASADA 1 (sin mutar NADA: todo se mide contra el estado PREVIO al move) ----
    // Por cada keyframe movido:
    //  (a) COPIA de lo que se mueve (mov*): el destino se escribe con esto, no con el frame origen (que para
    //      entonces ya puede estar reescrito).
    //  (b) buffer COMPLETO del destino (prep*), solo si en el destino no hay un frame QUIETO con esa capa:
    //      mascara = el valor del movido, resto = lo EVALUADO ahi con la interpolacion VIGENTE (no saltan).
    // (Lo del ORIGEN va DESPUES de escribir el destino: los vertices seleccionados tienen que caer sobre la
    //  interpolacion NUEVA -la que dejo el move-, no sobre la vieja, o el cuadro de origen les seguiria animando
    //  un pico raro y no desapareceria de la fila filtrada.)
    std::vector<GLfloat*> movPos(src.size(), (GLfloat*)NULL), prepPos(src.size(), (GLfloat*)NULL), noPos(src.size(), (GLfloat*)NULL);
    std::vector<GLbyte*>  movNrm(src.size(), (GLbyte*)NULL),  prepNrm(src.size(), (GLbyte*)NULL),  noNrm(src.size(), (GLbyte*)NULL);
    std::vector<GLfloat*> movUV (src.size(), (GLfloat*)NULL), prepUV (src.size(), (GLfloat*)NULL), noUV (src.size(), (GLfloat*)NULL);
    std::vector<VertexFrame> ease(src.size());   // interp/handles del movido (para el frame nuevo del destino)
    for (size_t i = 0; i < src.size(); ++i) {
        ease[i] = *src[i]; ease[i].positions = NULL; ease[i].normals = NULL; ease[i].uvs = NULL;
        VertexFrame* still = VertexAnimFrameIn(anim, dst[i], &src);   // el que NO se mueve
        if (src[i]->positions) {
            GLfloat* mv = new GLfloat[nF];
            for (size_t j = 0; j < nF; ++j) mv[j] = src[i]->positions[j];
            movPos[i] = mv;
            if (!(still && still->positions)) {
                GLfloat* b = new GLfloat[nF];
                for (size_t j = 0; j < nF; ++j) b[j] = mv[j];               // fallback: la pose del movido
                VertexAnimEvalLayerBuffer(anim, 0, (float)dst[i], vc, b, NULL);
                for (int v = 0; v < vc; ++v) if (mask[v]) {                 // los seleccionados: el movido
                    b[v*3] = mv[v*3]; b[v*3+1] = mv[v*3+1]; b[v*3+2] = mv[v*3+2]; }
                prepPos[i] = b;
            }
        }
        if (src[i]->normals) {
            GLbyte* mv = new GLbyte[nF];
            for (size_t j = 0; j < nF; ++j) mv[j] = src[i]->normals[j];
            movNrm[i] = mv;
            if (!(still && still->normals)) {
                GLbyte* b = new GLbyte[nF];
                for (size_t j = 0; j < nF; ++j) b[j] = mv[j];
                VertexAnimEvalLayerBuffer(anim, 2, (float)dst[i], vc, NULL, b);
                for (int v = 0; v < vc; ++v) if (mask[v]) {
                    b[v*3] = mv[v*3]; b[v*3+1] = mv[v*3+1]; b[v*3+2] = mv[v*3+2]; }
                prepNrm[i] = b;
            }
        }
        if (src[i]->uvs) {
            GLfloat* mv = new GLfloat[nU];
            for (size_t j = 0; j < nU; ++j) mv[j] = src[i]->uvs[j];
            movUV[i] = mv;
            if (!(still && still->uvs)) {
                GLfloat* b = new GLfloat[nU];
                for (size_t j = 0; j < nU; ++j) b[j] = mv[j];
                VertexAnimEvalLayerBuffer(anim, 1, (float)dst[i], vc, b, NULL);
                for (int v = 0; v < vc; ++v) if (mask[v]) { b[v*2] = mv[v*2]; b[v*2+1] = mv[v*2+1]; }
                prepUV[i] = b;
            }
        }
    }

    // ---- PASADA 2: escribir los DESTINOS ----
    // Si en el destino hay un frame se hace MERGE selectivo: SOLO los verts de la mascara se pisan con los del
    // movido; el resto del keyframe destino (y su interp/handles) queda como estaba. Si no hay nada, nace un
    // keyframe COMPLETO con el buffer preparado en la pasada 1 (los no seleccionados con lo que la anim ya
    // evaluaba ahi: no saltan). Va ANTES que el origen: el origen se resuelve contra el resultado.
    std::vector<VertexFrame*> recF; std::vector<int> recM;   // frames que RECIBIERON un destino + capas escritas
    for (size_t i = 0; i < src.size(); ++i) {
        VertexFrame* q = VertexAnimFrameIn(anim, dst[i], NULL);
        if (!q) {
            q = new VertexFrame();
            q->frame = dst[i];
            q->interp = ease[i].interp;             // un frame nuevo hereda el ease del movido
            q->handleType = ease[i].handleType;
            q->inDF = ease[i].inDF; q->inDV = ease[i].inDV;
            q->outDF = ease[i].outDF; q->outDV = ease[i].outDV;
            size_t ins = 0;                          // insertar ORDENADO por frame
            while (ins < anim.frames.size() && anim.frames[ins]->frame < dst[i]) ++ins;
            anim.frames.insert(anim.frames.begin() + ins, q);
        }
        int written = 0;
        if (movPos[i]) {
            if (q->positions) {
                GLfloat* qp = (GLfloat*)q->positions;
                for (int v = 0; v < vc; ++v) if (mask[v]) {
                    qp[v*3] = movPos[i][v*3]; qp[v*3+1] = movPos[i][v*3+1]; qp[v*3+2] = movPos[i][v*3+2]; }
            } else if (prepPos[i]) { q->positions = prepPos[i]; prepPos[i] = NULL; }
            written |= 1;
        }
        if (movNrm[i]) {
            if (q->normals) {
                GLbyte* qn = (GLbyte*)q->normals;
                for (int v = 0; v < vc; ++v) if (mask[v]) {
                    qn[v*3] = movNrm[i][v*3]; qn[v*3+1] = movNrm[i][v*3+1]; qn[v*3+2] = movNrm[i][v*3+2]; }
            } else if (prepNrm[i]) { q->normals = prepNrm[i]; prepNrm[i] = NULL; }
            written |= 4;
        }
        if (movUV[i]) {
            if (q->uvs) {
                GLfloat* qu = (GLfloat*)q->uvs;
                for (int v = 0; v < vc; ++v) if (mask[v]) { qu[v*2] = movUV[i][v*2]; qu[v*2+1] = movUV[i][v*2+1]; }
            } else if (prepUV[i]) { q->uvs = prepUV[i]; prepUV[i] = NULL; }
            written |= 2;
        }
        size_t r = 0; for (; r < recF.size(); ++r) if (recF[r] == q) break;
        if (r == recF.size()){ recF.push_back(q); recM.push_back(0); }
        recM[r] |= written;
    }

    // ---- PASADA 3: el ORIGEN pierde la contribucion de los vertices SELECCIONADOS ----
    // Capa por capa: los verts de la mascara pasan a valer lo que valdrian ahi SIN este keyframe (la
    // interpolacion de los vecinos, ya con el destino escrito) -> dejan de estar keyframeados en ese cuadro sin
    // que su animacion pegue un salto. Si con eso la capa entera queda REDUNDANTE (TODOS sus verts caen sobre esa
    // interpolacion: el cuadro ya no aporta nada distinto) se borra la capa; el frame se va cuando se queda sin
    // ninguna. Con la mascara COMPLETA eso siempre termina borrando el frame -> es exactamente el move de
    // siempre. Con un subconjunto, los vertices NO seleccionados conservan SU keyframe alla (que es lo que
    // promete el toggle: no tocar lo que no elegiste).
    // Un frame que ademas RECIBIO un destino en esa capa no se toca: manda el keyframe que acaba de llegar.
    for (size_t i = 0; i < src.size(); ++i) {
        VertexFrame* f = src[i]; if (!f) continue;
        int received = 0;
        for (size_t r = 0; r < recF.size(); ++r) if (recF[r] == f){ received = recM[r]; break; }
        if (f->positions && !(received & 1)) {
            GLfloat* s = new GLfloat[nF];
            if (VertexAnimEvalLayerBuffer(anim, 0, (float)f->frame, vc, s, NULL, f)) noPos[i] = s; else delete[] s;
        }
        if (f->normals && !(received & 4)) {
            GLbyte* s = new GLbyte[nF];
            if (VertexAnimEvalLayerBuffer(anim, 2, (float)f->frame, vc, NULL, s, f)) noNrm[i] = s; else delete[] s;
        }
        if (f->uvs && !(received & 2)) {
            GLfloat* s = new GLfloat[nU];
            if (VertexAnimEvalLayerBuffer(anim, 1, (float)f->frame, vc, s, NULL, f)) noUV[i] = s; else delete[] s;
        }
    }
    for (size_t i = 0; i < src.size(); ++i) {     // aplicar (en un paso aparte: ningun origen ve el de otro)
        VertexFrame* f = src[i]; if (!f) continue;
        int received = 0;
        for (size_t r = 0; r < recF.size(); ++r) if (recF[r] == f){ received = recM[r]; break; }
        if (f->positions && !(received & 1)) {
            if (!noPos[i]) { delete[] (GLfloat*)f->positions; f->positions = NULL; }  // sin vecinos: nada que sostener
            else { GLfloat* p = (GLfloat*)f->positions;
                   for (int v = 0; v < vc; ++v) if (mask[v]) {
                       p[v*3] = noPos[i][v*3]; p[v*3+1] = noPos[i][v*3+1]; p[v*3+2] = noPos[i][v*3+2]; } }
        }
        if (f->normals && !(received & 4)) {
            if (!noNrm[i]) { delete[] (GLbyte*)f->normals; f->normals = NULL; }
            else { GLbyte* p = (GLbyte*)f->normals;
                   for (int v = 0; v < vc; ++v) if (mask[v]) {
                       p[v*3] = noNrm[i][v*3]; p[v*3+1] = noNrm[i][v*3+1]; p[v*3+2] = noNrm[i][v*3+2]; } }
        }
        if (f->uvs && !(received & 2)) {
            if (!noUV[i]) { delete[] (GLfloat*)f->uvs; f->uvs = NULL; }
            else { GLfloat* p = (GLfloat*)f->uvs;
                   for (int v = 0; v < vc; ++v) if (mask[v]) { p[v*2] = noUV[i][v*2]; p[v*2+1] = noUV[i][v*2+1]; } }
        }
    }
    for (size_t i = 0; i < src.size(); ++i) {     // ...y recien ahora, que capas quedaron REDUNDANTES
        VertexFrame* f = src[i]; if (!f) continue;
        int received = 0;
        for (size_t r = 0; r < recF.size(); ++r) if (recF[r] == f){ received = recM[r]; break; }
        if (f->positions && !(received & 1) && !VertexLayerContributes(anim, f, 0, vc, NULL)){ delete[] (GLfloat*)f->positions; f->positions = NULL; }
        if (f->normals   && !(received & 4) && !VertexLayerContributes(anim, f, 2, vc, NULL)){ delete[] (GLbyte*)f->normals;   f->normals   = NULL; }
        if (f->uvs       && !(received & 2) && !VertexLayerContributes(anim, f, 1, vc, NULL)){ delete[] (GLfloat*)f->uvs;      f->uvs       = NULL; }
    }
    // los frames de origen que quedaron SIN NINGUNA capa se van
    for (size_t k = anim.frames.size(); k-- > 0; ) {
        VertexFrame* f = anim.frames[k];
        if (!f) { anim.frames.erase(anim.frames.begin()+k); continue; }
        if (f->positions || f->normals || f->uvs) continue;
        bool isSrc = false; for (size_t i = 0; i < src.size(); ++i) if (src[i] == f){ isSrc = true; break; }
        if (!isSrc) continue;                      // uno vacio que no venia de aca: no es asunto nuestro
        anim.frames.erase(anim.frames.begin()+k);
        for (size_t i = 0; i < src.size(); ++i) if (src[i] == f) src[i] = NULL;   // olvidarlo ANTES de liberarlo
        delete f;
    }

    // liberar los temporales (los prep* que se adjuntaron ya quedaron en NULL)
    for (size_t i = 0; i < src.size(); ++i) {
        delete[] movPos[i]; delete[] prepPos[i]; delete[] noPos[i];
        delete[] movNrm[i]; delete[] prepNrm[i]; delete[] noNrm[i];
        delete[] movUV[i];  delete[] prepUV[i];  delete[] noUV[i];
    }
    VertexAnimSort(anim);
    anim.frameCount = (int)anim.frames.size();
    // los espejos de capa pueden haber cambiado (una capa entera pudo desaparecer)
    { bool hasN = false, hasU = false;
      for (size_t k = 0; k < anim.frames.size(); ++k){ if (!anim.frames[k]) continue;
          if (anim.frames[k]->normals) hasN = true;
          if (anim.frames[k]->uvs)     hasU = true; }
      anim.UseNormals = hasN; anim.UseUV = hasU; }
    return true;
}

bool VertexAnimMoveKeySelective(VertexAnimation& anim, int origin, int destination,
                                 const unsigned char* mask) {
    std::vector<int> o(1, origin), d(1, destination);
    return VertexAnimMoveKeysSelective(anim, o, d, mask);
}

// helpers de escritura/lectura LE (little-endian explicito: NO depender del orden nativo,
// asi un .w3d guardado en PC abre igual en el N95). C++03: sin templates fancy.
static void PutU32(std::vector<unsigned char>& o, unsigned v){
    o.push_back((unsigned char)(v & 0xFF)); o.push_back((unsigned char)((v>>8)&0xFF));
    o.push_back((unsigned char)((v>>16)&0xFF)); o.push_back((unsigned char)((v>>24)&0xFF));
}
static void PutI32(std::vector<unsigned char>& o, int v){ PutU32(o, (unsigned)v); }
static void PutF32(std::vector<unsigned char>& o, float f){
    unsigned u; std::memcpy(&u, &f, 4); PutU32(o, u);   // el patron de bits IEEE-754
}
static unsigned GetU32(const unsigned char* p){
    return (unsigned)p[0] | ((unsigned)p[1]<<8) | ((unsigned)p[2]<<16) | ((unsigned)p[3]<<24);
}
static float GetF32(const unsigned char* p){ unsigned u=GetU32(p); float f; std::memcpy(&f,&u,4); return f; }

void VertexAnimSerialize(const VertexAnimation& anim, std::vector<unsigned char>& out){
    out.clear();
    int vc = anim.target ? anim.target->vertexSize : 0;
    // si no hay target, deducir vc del primer frame no sirve (no guardamos vc por frame);
    // exigir target valido. Sin vertices no hay nada que guardar.
    if (vc <= 0) { return; }
    // useN/useU reflejan los DATOS (algun frame trae esa capa?), no los espejos: si
    // quedaron desincronizados, manda lo que realmente hay guardado
    unsigned useN = 0, useU = 0;
    for (size_t k=0;k<anim.frames.size();k++){
        if (!anim.frames[k]) continue;
        if (anim.frames[k]->normals) useN = 1;
        if (anim.frames[k]->uvs)     useU = 1;
    }
    out.push_back('W'); out.push_back('3'); out.push_back('D'); out.push_back('V');
    PutU32(out, 5);                          // version 5: capa de NORMALES propia (bit2 de la mascara)
    PutU32(out, (unsigned)vc);               // vertexCount
    PutU32(out, useN);                       // useNormals: hay frames con capa de normales
    PutU32(out, useU);                       // useUV (v3+): hay frames con capa UV
    PutU32(out, (unsigned)anim.frames.size());
    const size_t nF = (size_t)vc * 3;
    const size_t nU = (size_t)vc * 2;
    for (size_t k=0;k<anim.frames.size();k++){
        const VertexFrame* f = anim.frames[k];
        PutI32(out, f->frame);
        PutI32(out, f->interp);
        PutI32(out, f->handleType);          // handles del ease (v2)
        PutF32(out, f->inDF); PutF32(out, f->inDV); PutF32(out, f->outDF); PutF32(out, f->outDV);
        // v5: que CAPAS trae este frame (bit0 = posiciones, bit1 = uv, bit2 = normales).
        // Solo se escriben los buffers presentes, en orden posiciones / normales / uv.
        unsigned layers = (f->positions ? 1u : 0u) | (f->uvs ? 2u : 0u) | (f->normals ? 4u : 0u);
        PutU32(out, layers);
        if (layers & 1u) for (size_t i=0;i<nF;i++) PutF32(out, f->positions[i]);
        if (layers & 4u) for (size_t i=0;i<nF;i++) out.push_back((unsigned char)f->normals[i]);
        if (layers & 2u) for (size_t i=0;i<nU;i++) PutF32(out, f->uvs[i]);
    }
}

bool VertexAnimDeserialize(VertexAnimation& anim, const unsigned char* in, size_t n){
    if (!in || n < 20) return false;
    if (in[0]!='W'||in[1]!='3'||in[2]!='D'||in[3]!='V') return false;
    size_t p = 4;
    unsigned show = GetU32(in+p); p+=4;
    if (show >= 3 && n < 24) return false;    // v3+ mete un u32 extra (useUV) en el header (24 bytes)
    unsigned vc  = GetU32(in+p); p+=4;
    unsigned useN= GetU32(in+p); p+=4;
    const bool withUV = (show >= 3);           // v3+: header trae useUV
    unsigned useU = 0;
    if (withUV){ useU = GetU32(in+p); p+=4; }
    unsigned nfr = GetU32(in+p); p+=4;
    if (vc == 0) return false;
    const size_t nF = (size_t)vc * 3;
    const size_t nU = (size_t)vc * 2;
    const bool withHandles = (show >= 2);   // v1: sin handles (default); v2: con handles del bezier
    const bool withLayers  = (show >= 4);   // v4: mascara de capas POR FRAME (frames de tamano variable)
    const bool nrmOwn      = (show >= 5);   // v5: la capa de NORMALES tiene bit propio (bit2); antes
                                          // viajaba PEGADA a las posiciones (si useN)
    anim.FreeFrames();
    anim.UseNormals = (useN != 0);
    anim.UseUV = (useU != 0);
    anim.vcount = (int)vc;   // los frames del blob estan armados para este conteo de vertices
    for (unsigned k=0;k<nfr;k++){
        // v4+ tiene frames de TAMANO VARIABLE -> se valida el resto del blob antes de cada
        // bloque (v1..v3 pasan por los mismos chequeos: son un caso de tamano fijo)
        size_t need = 8 + (withHandles ? 20u : 0u) + (withLayers ? 4u : 0u);
        if (n < p + need){ anim.FreeFrames(); anim.frameCount = 0; return false; }
        VertexFrame* f = new VertexFrame();
        f->frame  = (int)GetU32(in+p); p+=4;
        f->interp = (int)GetU32(in+p); p+=4;
        if (withHandles){
            f->handleType = (int)GetU32(in+p); p+=4;
            f->inDF = GetF32(in+p); p+=4; f->inDV = GetF32(in+p); p+=4;
            f->outDF = GetF32(in+p); p+=4; f->outDV = GetF32(in+p); p+=4;
        }
        // capas del frame: v4+ la trae explicita; v1..v3 era el layout viejo (posiciones
        // siempre; uv en TODOS los frames si useU) -> retrocompat
        unsigned layers = 1u | (useU ? 2u : 0u);
        if (withLayers){ layers = GetU32(in+p); p+=4; }
        anim.frames.push_back(f);   // ya en la lista: si falla un bloque, FreeFrames lo limpia
        if (layers & 1u){
            // v1..v4: las normales acompanaban a las posiciones (useN) -> se leen juntas
            size_t needPos = nF*4 + ((!nrmOwn && useN) ? nF : 0);
            if (n < p + needPos){ anim.FreeFrames(); anim.frameCount = 0; return false; }
            GLfloat* pos = new GLfloat[nF];
            for (size_t i=0;i<nF;i++){ pos[i]=GetF32(in+p); p+=4; }
            f->positions = pos;
            if (!nrmOwn && useN){ GLbyte* nr = new GLbyte[nF]; for (size_t i=0;i<nF;i++){ nr[i]=(GLbyte)in[p]; p++; } f->normals = nr; }
        }
        if (nrmOwn && (layers & 4u)){   // v5: bloque de normales propio (puede venir sin posiciones)
            if (n < p + nF){ anim.FreeFrames(); anim.frameCount = 0; return false; }
            GLbyte* nr = new GLbyte[nF];
            for (size_t i=0;i<nF;i++){ nr[i]=(GLbyte)in[p]; p++; }
            f->normals = nr;
        }
        if (layers & 2u){
            if (n < p + nU*4){ anim.FreeFrames(); anim.frameCount = 0; return false; }
            GLfloat* uv = new GLfloat[nU];
            for (size_t i=0;i<nU;i++){ uv[i]=GetF32(in+p); p+=4; }
            f->uvs = uv;
        }
    }
    anim.frameCount = (int)anim.frames.size();
    return true;
}

VertexAnimationActive::VertexAnimationActive(Mesh* mesh):
    meshToAnim(mesh), currentAnim(0), nextAnim(0), currentFrame(0),
    nextFrame(1), blendStep(0.0f), playFrame(0.0f) {
}

// ver el comentario del .h: cortar AHORA al clip 'clip' y caer en el cuadro 'frame'.
void VertexAnimationActive::ChangeNow(int clip, float frame) {
    if (!meshToAnim) return;
    if (clip < 0 || clip >= (int)meshToAnim->animations.size()) return;
    VertexAnimation* an = meshToAnim->animations[clip];
    if (!an || an->frames.empty()) return;
    // el rango REAL del clip (mismo criterio que UpdateAnimation: si el start/end
    // declarado no cubre los keyframes, manda el span de estos)
    int startF = an->startFrame, endF = an->endFrame;
    if (endF <= startF) { startF = an->frames.front()->frame; endF = an->frames.back()->frame; }
    if (endF < startF) endF = startF;
    if (frame < (float)startF) frame = (float)startF;
    if (frame > (float)endF)   frame = (float)endF;
    currentAnim = nextAnim = clip;
    playFrame = frame;
    an->target = meshToAnim;         // las anims que vienen del .w3d llegan con target NULL
    EvalVertexAnim(*an, meshToAnim, playFrame);
}

void VertexAnimationActive::UpdateAnimation(float dtSeg){
    if (!meshToAnim || meshToAnim->animations.empty()) return;
    if (currentAnim < 0 || currentAnim >= (int)meshToAnim->animations.size()) return;

    VertexAnimation* anim = meshToAnim->animations[currentAnim];
    if (!anim || anim->frames.empty()) return;

    // rango de reproduccion: el PROPIO de la anim (start..end @ fps). Si no cubre
    // los keyframes (anim vieja / mal configurada), usar el span real de estos.
    int startF = anim->startFrame;
    int endF   = anim->endFrame;
    int firstKF = anim->frames.front()->frame;
    int lastKF  = anim->frames.back()->frame;
    if (endF <= startF) { startF = firstKF; endF = lastKF; }
    if (endF < startF) endF = startF;

    int fps = (anim->fps > 0) ? anim->fps : 30;
    float speed = (anim->speed > 0.0f) ? anim->speed : 1.0f;

    // avance por TIEMPO REAL: la anim corre por el timeline a 'fps' cuadros/seg
    // (a cualquier framerate de render dura lo mismo). 'speed' la acelera/frena.
    if (playFrame < (float)startF) playFrame = (float)startF;
    playFrame += dtSeg * (float)fps * speed;

    if (playFrame > (float)endF) {
        // un script pidio cambiar de clip (lua animar()): saltar al pedido
        if (nextAnim != currentAnim && nextAnim >= 0 &&
            nextAnim < (int)meshToAnim->animations.size()) {
            currentAnim = nextAnim;
            VertexAnimation* nx = meshToAnim->animations[currentAnim];
            playFrame = (float)(nx ? nx->startFrame : startF);
            anim = nx;
        } else if (anim->nextAnimation >= 0 &&
                   anim->nextAnimation < (int)meshToAnim->animations.size()) {
            currentAnim = nextAnim = anim->nextAnimation;
            VertexAnimation* nx = meshToAnim->animations[currentAnim];
            playFrame = (float)(nx ? nx->startFrame : startF);
            anim = nx;
        } else if (anim->repeat) {
            float span = (float)(endF - startF);
            if (span <= 0.0f) playFrame = (float)startF;
            else { while (playFrame > (float)endF) playFrame -= span; } // loop conservando el sobrante
        } else {
            playFrame = (float)endF; // HOLD el ultimo
        }
    }

    if (!anim || anim->frames.empty()) return;
    // la dibuja una rutina: el tiempo y los cambios de clip avanzan, la pose se calcula al apuntarla (W3dUpToDatePose)
    if (meshToAnim->onlyRoutine) { meshToAnim->posePending = true; return; }
    EvalVertexAnim(*anim, meshToAnim, playFrame);
}

void W3dUpToDatePose(Mesh* m) {
    if (!m || !m->posePending) return;
    m->posePending = false;
    VertexAnimationActive* a = FindTargetAnim(m);
    if (!a || a->currentAnim < 0 || a->currentAnim >= (int)m->animations.size()) return;
    VertexAnimation* anim = m->animations[a->currentAnim];
    if (anim && !anim->frames.empty()) EvalVertexAnim(*anim, m, a->playFrame);
}

std::vector<VertexAnimationActive*> VertexAnimationActives;
void (*OnSelectAnimVertex)(Mesh* m, int idx) = NULL;
int  (*ActiveAnimVertexOf)(Mesh* m) = NULL;

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

void RemapVertexAnims(Mesh* mesh, const int* newToOld, int newVcount, int oldVcount){
    if (!mesh || newVcount <= 0 || !newToOld || !mesh->vertex) return;
    (void)oldVcount;
    const size_t nF = (size_t)newVcount * 3;
    for (size_t a = 0; a < mesh->animations.size(); ++a){
        VertexAnimation* an = mesh->animations[a];
        if (!an) continue;
        an->UninstantiateFrames();   // COW: el remap reescribe los frames
        const int avc = an->vcount;   // conteo con el que estan armados los frames de ESTA anim
        // GUARDA anti-desalineado: las RECETAS de corte asumen que los verts ORIGINALES del
        // layout pre-op ocupan [0, vtxAnimRecipeBase) = el layout de los frames (avc). Si no
        // coincide (ej: un restaurar-y-reaplicar que no restauro los frames), remapear leeria
        // los keyframes CORRIDOS y la malla animada se rompe. Fallback documentado: esa anim
        // queda ESTATICA en la pose actual (pierde la anim, pero JAMAS buffers desalineados).
        const bool aligned = (avc > 0) &&
            (mesh->vtxAnimRecipes.empty() || mesh->vtxAnimRecipeBase == avc);
        for (size_t k = 0; k < an->frames.size(); ++k){
            VertexFrame* f = an->frames[k];
            // se remapean SOLO las capas que el frame trae (un frame solo-UV no gana
            // posiciones, y al reves: la distincion de capas sobrevive al remapeo).
            // Las normales son capa PROPIA: se remapean si el frame las trae, tenga o
            // no posiciones.
            GLfloat* np = f->positions ? new GLfloat[nF] : NULL;
            GLbyte*  nn = f->normals ? new GLbyte[nF] : NULL;
            GLfloat* nu = f->uvs ? new GLfloat[(size_t)newVcount*2] : NULL;   // UV remapeadas (2/vert)
            for (int gi = 0; gi < newVcount; ++gi){
                int ov = newToOld[gi];
                // que ES este vert nuevo: 1 = EXISTENTE (sigue la anim), 2 = con RECETA
                // (corte: lerp de 2 viejos en TODAS las capas; o receta onlyUV de un
                // duplicar/extrude: SOLO la capa UV copia la del origen, posiciones y
                // normales estaticas), 0 = NUEVO sin receta (estatico en la pose actual)
                int mode = 0; const Mesh::VtxRecipe* rec = NULL;
                if (aligned && ov >= 0 && ov < avc) mode = 1;
                else {
                    int ri = ov - mesh->vtxAnimRecipeBase;
                    if (aligned && ov >= mesh->vtxAnimRecipeBase && ri >= 0 && ri < (int)mesh->vtxAnimRecipes.size()){
                        const Mesh::VtxRecipe& r = mesh->vtxAnimRecipes[ri];
                        if (r.a >= 0 && r.a < avc && r.b >= 0 && r.b < avc){ mode = 2; rec = &r; }
                    }
                }
                // receta onlyUV: posiciones/normales van por el camino ESTATICO (como modo 0)
                const bool recOnlyUV = (mode == 2 && rec->onlyUV);
                if (np){   // capa POSICIONES (np != NULL implica f->positions != NULL)
                    if (mode == 1){
                        np[gi*3]=f->positions[ov*3]; np[gi*3+1]=f->positions[ov*3+1]; np[gi*3+2]=f->positions[ov*3+2];
                    } else if (mode == 2 && !recOnlyUV){
                        float u = 1.0f - rec->t;
                        np[gi*3]  =f->positions[rec->a*3]  *u+f->positions[rec->b*3]  *rec->t;
                        np[gi*3+1]=f->positions[rec->a*3+1]*u+f->positions[rec->b*3+1]*rec->t;
                        np[gi*3+2]=f->positions[rec->a*3+2]*u+f->positions[rec->b*3+2]*rec->t;
                    } else {   // modo 0, o receta onlyUV: estatico en la pose actual
                        np[gi*3]=mesh->vertex[gi*3]; np[gi*3+1]=mesh->vertex[gi*3+1]; np[gi*3+2]=mesh->vertex[gi*3+2];
                    }
                }
                if (nn){   // capa NORMALES (nn != NULL implica f->normals != NULL)
                    if (mode == 1){
                        nn[gi*3]=f->normals[ov*3]; nn[gi*3+1]=f->normals[ov*3+1]; nn[gi*3+2]=f->normals[ov*3+2];
                    } else if (mode == 2 && !recOnlyUV){
                        float u = 1.0f - rec->t;
                        nn[gi*3]  =(GLbyte)(f->normals[rec->a*3]  *u+f->normals[rec->b*3]  *rec->t);
                        nn[gi*3+1]=(GLbyte)(f->normals[rec->a*3+1]*u+f->normals[rec->b*3+1]*rec->t);
                        nn[gi*3+2]=(GLbyte)(f->normals[rec->a*3+2]*u+f->normals[rec->b*3+2]*rec->t);
                    } else if ((mode == 0 || recOnlyUV) && mesh->normals){
                        nn[gi*3]=mesh->normals[gi*3]; nn[gi*3+1]=mesh->normals[gi*3+1]; nn[gi*3+2]=mesh->normals[gi*3+2];
                    } else { nn[gi*3]=0; nn[gi*3+1]=127; nn[gi*3+2]=0; }
                }
                if (nu){   // capa UV (nu != NULL implica f->uvs != NULL)
                    if (mode == 1){
                        nu[gi*2]=f->uvs[ov*2]; nu[gi*2+1]=f->uvs[ov*2+1];
                    } else if (mode == 2){
                        float u = 1.0f - rec->t;
                        nu[gi*2]  =f->uvs[rec->a*2]  *u+f->uvs[rec->b*2]  *rec->t;
                        nu[gi*2+1]=f->uvs[rec->a*2+1]*u+f->uvs[rec->b*2+1]*rec->t;
                    } else if (mesh->uv){
                        nu[gi*2]=mesh->uv[gi*2]; nu[gi*2+1]=mesh->uv[gi*2+1];
                    } else { nu[gi*2]=0.0f; nu[gi*2+1]=0.0f; }
                }
            }
            delete[] (GLfloat*)f->positions; f->positions = np;
            delete[] (GLbyte*)f->normals;   f->normals   = nn;
            delete[] (GLfloat*)f->uvs;      f->uvs        = nu;
        }
        an->vcount = newVcount;   // ahora los frames estan armados para el conteo NUEVO
    }
}

void VertexAnimSnapshot(Mesh* mesh, VtxAnimSnap& out){
    out.anims.clear(); out.has = false;
    if (!mesh) return;
    out.has = true;
    out.anims.resize(mesh->animations.size());
    for (size_t a = 0; a < mesh->animations.size(); ++a){
        VertexAnimation* an = mesh->animations[a];
        VtxAnimSnap::Anim& sa = out.anims[a];
        if (!an) continue;
        // los frames estan armados para an->vcount verts (0 = legacy: asumen vertexSize)
        const int n = (an->vcount > 0) ? an->vcount : mesh->vertexSize;
        if (n <= 0) continue;
        sa.vcount = n;
        sa.frames.resize(an->frames.size());
        for (size_t k = 0; k < an->frames.size(); ++k){
            const VertexFrame* f = an->frames[k];
            VtxAnimSnap::Frame& sf = sa.frames[k];
            if (!f) continue;
            if (f->positions) sf.pos.assign(f->positions, f->positions + (size_t)n*3);
            if (f->normals)   sf.nrm.assign(f->normals,   f->normals   + (size_t)n*3);
            if (f->uvs)       sf.uv.assign (f->uvs,       f->uvs       + (size_t)n*2);
        }
    }
}

void VertexAnimRestoreSnap(Mesh* mesh, const VtxAnimSnap& snap){
    if (!mesh || !snap.has) return;
    for (size_t a = 0; a < mesh->animations.size() && a < snap.anims.size(); ++a){
        VertexAnimation* an = mesh->animations[a];
        const VtxAnimSnap::Anim& sa = snap.anims[a];
        if (!an || sa.vcount <= 0) continue;
        // solo si la ESTRUCTURA no cambio (mismos keyframes): un op de topologia no
        // agrega/borra keyframes, asi que en los flujos snapshot/restore siempre coincide
        if (an->frames.size() != sa.frames.size()) continue;
        for (size_t k = 0; k < an->frames.size(); ++k){
            VertexFrame* f = an->frames[k];
            const VtxAnimSnap::Frame& sf = sa.frames[k];
            if (!f) continue;
            delete[] (GLfloat*)f->positions; f->positions = NULL;
            delete[] (GLbyte*) f->normals;   f->normals   = NULL;
            delete[] (GLfloat*)f->uvs;       f->uvs       = NULL;
            if (!sf.pos.empty()){ GLfloat* p = new GLfloat[sf.pos.size()]; std::memcpy(p, &sf.pos[0], sf.pos.size()*sizeof(GLfloat)); f->positions = p; }
            if (!sf.nrm.empty()){ GLbyte*  p = new GLbyte [sf.nrm.size()]; std::memcpy(p, &sf.nrm[0], sf.nrm.size()*sizeof(GLbyte));  f->normals   = p; }
            if (!sf.uv.empty()) { GLfloat* p = new GLfloat[sf.uv.size()];  std::memcpy(p, &sf.uv[0],  sf.uv.size()*sizeof(GLfloat));  f->uvs       = p; }
        }
        an->vcount = sa.vcount;   // los frames vuelven a estar armados para el layout del snapshot
    }
}

void VertexAnimRemapByPosition(VertexAnimation& anim, Mesh* mesh){
    if (!mesh || !mesh->vertex || mesh->vertexSize<=0 || anim.frames.empty()) return;
    const int N = mesh->vertexSize;         // verts de la malla (del GLB, re-splitteado)
    const int M = anim.vcount;              // verts del blob de la anim
    if (M<=0 || N==M) { anim.vcount = N; return; }
    // pose base (el GLB se exporto en esta pose): el PRIMER frame con capa de posiciones
    const VertexFrame* base = NULL;
    for (size_t k=0;k<anim.frames.size();k++)
        if (anim.frames[k] && anim.frames[k]->positions){ base = anim.frames[k]; break; }
    if (!base) return;   // anim solo-UV: sin posiciones no hay contra que matchear
    // cada vert de la malla -> el vert del blob mas cercano EN LA POSE BASE
    std::vector<int> map(N, -1);
    for (int gi=0; gi<N; gi++){
        float best=1e30f; int bj=0;
        for (int j=0;j<M;j++){
            float dx=mesh->vertex[gi*3]-base->positions[j*3];
            float dy=mesh->vertex[gi*3+1]-base->positions[j*3+1];
            float dz=mesh->vertex[gi*3+2]-base->positions[j*3+2];
            float d=dx*dx+dy*dy+dz*dz; if (d<best){ best=d; bj=j; }
        }
        map[gi]=bj;
    }
    const size_t nF = (size_t)N*3;
    for (size_t k=0;k<anim.frames.size();k++){
        VertexFrame* f = anim.frames[k];
        // solo las capas que el frame TRAE (un frame solo-UV conserva su distincion;
        // las normales son capa propia: van si el frame las trae)
        GLfloat* np = f->positions ? new GLfloat[nF] : NULL;
        GLbyte*  nn = f->normals ? new GLbyte[nF] : NULL;
        GLfloat* nu = f->uvs ? new GLfloat[(size_t)N*2] : NULL;   // UV por el mismo match de posicion
        for (int gi=0; gi<N; gi++){ int j=map[gi];
            if (np){ np[gi*3]=f->positions[j*3]; np[gi*3+1]=f->positions[j*3+1]; np[gi*3+2]=f->positions[j*3+2]; }
            if (nn){ nn[gi*3]=f->normals[j*3]; nn[gi*3+1]=f->normals[j*3+1]; nn[gi*3+2]=f->normals[j*3+2]; }
            if (nu){ nu[gi*2]=f->uvs[j*2]; nu[gi*2+1]=f->uvs[j*2+1]; }
        }
        delete[] (GLfloat*)f->positions; f->positions=np;
        delete[] (GLbyte*)f->normals;   f->normals=nn;
        delete[] (GLfloat*)f->uvs;      f->uvs=nu;
    }
    anim.vcount = N;
}

void VertexAnimFreeController(Mesh* mesh){
    for (size_t i = 0; i < VertexAnimationActives.size(); ){
        if (VertexAnimationActives[i] && VertexAnimationActives[i]->meshToAnim == mesh){
            delete VertexAnimationActives[i];
            VertexAnimationActives.erase(VertexAnimationActives.begin() + i);
        } else ++i;
    }
}

void NewActiveVertexAnimation(Mesh* mesh, VertexAnimation* anim){
    if (!mesh) return;

    mesh->animations.push_back(anim);

    // Un solo controlador activo por mesh; si ya existe, no crear otro.
    if (!FindTargetAnim(mesh)) {
        VertexAnimationActive* activeAnim = new VertexAnimationActive(mesh);
        VertexAnimationActives.push_back(activeAnim);
    }
}

void LoadVertexFrames(Mesh* mesh){
    if (!mesh) return;

    for (std::vector<VertexAnimation*>::iterator it = mesh->animations.begin();
         it != mesh->animations.end(); ++it) {
        VertexAnimation* anim = *it;
        anim->target = mesh;

        if (anim->frames.empty()) {
            anim->LoadFrames();
            w3dLogf("Anim '%s' con %d frames, Speed: %g (Normals: %d)",
                    anim->name.c_str(), (int)anim->frames.size(), (double)anim->speed, (int)anim->UseNormals);
        }
    }
}

void UpdateAnimations(float dtSeg){
    for (size_t i = 0; i < VertexAnimationActives.size(); ++i) {
        // la malla cuya vertex anim esta ACTIVA en el timeline (kind 3) no se
        // auto-avanza: el PLAYHEAD manda (el editor aplica el frame del timeline)
        if (ActiveAnimKind == 3 && ActiveAnimMesh &&
            VertexAnimationActives[i]->meshToAnim == ActiveAnimMesh) continue;
        VertexAnimationActives[i]->UpdateAnimation(dtSeg);
    }
}

// Posa la malla con vertex-anim ACTIVA en el timeline (ActiveAnimKind==3) al frame del PLAYHEAD (CurrentFrame).
// Es el mismo bloque que corre en PC (main.cpp): se llama por frame; el cache interno (serial/anim/frame) solo
// re-evalua al cambiar. El loop de Symbian FALTABA llamarlo -> al dar Play/scrubbear un clip (ej "correr") la
// timeline avanzaba pero la malla vertex-animada no cambiaba (UpdateAnimations la saltea a proposito: kind 3).
void ApplyVertexAnimTimeline(){
    extern int CurrentFrame;
    if (ActiveAnimKind == 3 && ActiveAnimMesh) {
        VertexAnimationActive* va = FindTargetAnim(ActiveAnimMesh);
        int ai = va ? va->currentAnim : -1;
        if (ai >= 0 && ai < (int)ActiveAnimMesh->animations.size() && ActiveAnimMesh->animations[ai] &&
            !ActiveAnimMesh->animations[ai]->frames.empty()) {
            static unsigned int lastSerial = 0, lastGen = 0; static int lastAnim = -1, lastFrame = -999999;
            if (ActiveAnimMesh->serial != lastSerial || ai != lastAnim || CurrentFrame != lastFrame || lastGen != g_animCurvesGen) {
                lastSerial = ActiveAnimMesh->serial; lastAnim = ai; lastFrame = CurrentFrame; lastGen = g_animCurvesGen;   // (o editaron la curva)
                VertexAnimation* an = ActiveAnimMesh->animations[ai];
                an->target = ActiveAnimMesh;
                EvalVertexAnim(*an, ActiveAnimMesh, (float)CurrentFrame);
            }
        }
    }
}

// ============================================================================
//  W3dRestVertexAnim -- ver el contrato completo en VertexAnimation.h
// ============================================================================
W3dRestVertexAnim::W3dRestVertexAnim(Mesh* m) : mesh(m), restore(false) {
    if (!mesh || !mesh->vertex || mesh->vertexSize <= 0) return;
    if (!mesh->PosedByVertexAnim()) return;    // EN REPOSO: no hay nada que hacer (caso normal)
    // la anim (y el cuadro) del KEYFRAME BASE. Si ninguna sirve -los frames no
    // alinean con la malla, una op de topologia a medio camino- no hay reposo al
    // que volver: se deja como esta (lado conservador, igual que la poda).
    const VertexAnimation* an = NULL; int frameBase = 0;
    for (size_t a = 0; a < mesh->animations.size() && !an; ++a) {
        const VertexAnimation* c = mesh->animations[a];
        if (!c) continue;
        if (c->vcount != 0 && c->vcount != mesh->vertexSize) continue;
        for (size_t k = 0; k < c->frames.size(); ++k)
            if (c->frames[k] && c->frames[k]->positions) { an = c; frameBase = c->frames[k]->frame; break; }
    }
    if (!an) return;
    const size_t n3 = (size_t)mesh->vertexSize * 3;
    const size_t n2 = (size_t)mesh->vertexSize * 2;
    pos.assign(mesh->vertex, mesh->vertex + n3);
    if (mesh->normals) nrm.assign(mesh->normals, mesh->normals + n3);
    if (mesh->uv)      uvs.assign(mesh->uv, mesh->uv + n2);
    restore = true;
    // el mismo evaluador del playback en el cuadro base: deja las TRES capas
    // (posiciones, normales, UV) como en reposo y apaga solo el flag de pose.
    EvalVertexAnim(*an, mesh, (float)frameBase);
}

W3dRestVertexAnim::~W3dRestVertexAnim() {
    if (!restore || !mesh || !mesh->vertex) return;
    const size_t n3 = (size_t)mesh->vertexSize * 3;
    const size_t n2 = (size_t)mesh->vertexSize * 2;
    // LA TOPOLOGIA CAMBIO ADENTRO DEL SCOPE (el scope no era solo "guardar": alguien
    // separo/corto/rebuildeo la malla): no hay pose que devolver, las posiciones que
    // quedaron son las del CUADRO BASE. Entonces la malla se queda EN REPOSO **y con
    // el flag en false**, que es el estado que corresponde a esas posiciones. Antes
    // salteaba el memcpy pero igual escribia posedByAnim=true -> malla en reposo
    // diciendo "estoy posada" (el gate apagado sobre geometria de modelo).
    if (pos.size() != n3) return;
    std::memcpy(mesh->vertex, &pos[0], n3 * sizeof(GLfloat));
    if (mesh->normals && nrm.size() == n3) std::memcpy(mesh->normals, &nrm[0], n3 * sizeof(GLbyte));
    if (mesh->uv && uvs.size() == n2)      std::memcpy(mesh->uv, &uvs[0], n2 * sizeof(GLfloat));
    mesh->posedByAnim = true;   // vuelve a estar POSADA: la pose es la que habia
    mesh->skinGeomVersion++;    // re-subir el VBO (la pantalla tiene que seguir mostrando la pose)
}
