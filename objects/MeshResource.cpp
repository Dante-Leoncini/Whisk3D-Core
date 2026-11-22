// ============================================================================
//  Whisk3DCore (engine) — MeshResource: la malla 3D como recurso compartido.
//  Ver el contrato en MeshResource.h. La contabilidad (id -> descriptor,
//  refcount, liberacion al llegar a 0) es del almacen de recursos
//  (io/W3dResources.h, tipo W3DREC_MESH, el mismo de MeshData); aca vive el
//  registro del proyecto, la carga de la entrada .w3db, el vinculo con cada
//  Mesh usuario y los VBO compartidos.
//  C++03. Motor generico: aca no hay nombres de ningun juego.
// ============================================================================
#include "MeshResource.h"
#include "MeshFlows.h"   // los flujos de indices derivados (se sueltan con el recurso)
#include "io/W3dResources.h"
#include "io/W3dMesh.h"         // W3dMeshInfo
#include "w3dFilesystem.h"      // la entrada del contenedor montado
#include "w3dGraphics.h"        // VBO compartidos
#include "W3dNames.h"           // LA regla de nombres unicos (Arbol -> Arbol.001)
#include "animation/Animation.h"        // CurrentFrame / ActiveAnimKind: el cuadro de la pose de la sesion
#include "animation/VertexAnimation.h"  // re-posar a los usuarios animados tras re-apuntarlos
#include "w3dlog.h"
#include <string.h>
#include <stdlib.h>             // getenv (W3D_VBO_LIMIT_KB, solo PC)

namespace gfx = w3dEngine;

const char* const W3D_RESOURCE_EDIT = "@recurso";
Material* (*W3dMeshMaterialByNameHook)(const std::string& name) = 0;
void (*W3dMeshBeforeLinkHook)(Mesh* m, MeshResource* r) = 0;
bool (*W3dMeshTextToBinHook)(const unsigned char* data, size_t n, const std::string& name, std::string& out) = 0;

// el registro del proyecto: dueno de los MeshResource (viven hasta cerrar el proyecto)
static std::vector<MeshResource*> gRegistry;
// las mallas de las LIBRERIAS externas (MeshResource::libreria): aparte del registro del proyecto (no se listan,
// no se guardan ni se purgan), vivas hasta que su libreria se cierra
static std::vector<MeshResource*> gLibMeshes;
// sube cada vez que un recurso sale de gRegistry o de gLibMeshes (ver W3dMeshResourceLive)
static unsigned gMeshesGen = 1;
// recursos que al cerrar el proyecto todavia tenian usuarios (no deberia pasar): se dejan
// vivos para que ningun puntero quede colgando
static std::vector<MeshResource*> gRetained;
static int gNextSerial = 1;
// las mallas vinculadas que las puertas al render marcaron como EDITADAS (ver el .h)
static std::vector<Mesh*> gEdited;

// entero -> texto sin sprintf de largo variable (C++03 / RVCT)
static std::string IntegerText(int v) {
    char b[16]; int i = 15; b[i] = '\0';
    const bool neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    do { b[--i] = (char)('0' + (u % 10u)); u /= 10u; } while (u && i > 1);
    if (neg && i > 0) b[--i] = '-';
    return std::string(b + i);
}

MeshResource::MeshResource()
    : modified(false), textBytes(false), format(-1), serial(gNextSerial++), refsUndo(0), keep(false), deleted(false),
      version(1), rec(0),
      smooth(false), primType(-1), primSize(2.0f), primSize2(0.0f), primHeight(0.0f), primVerts(8), primVerts2(8),
      extrasInResource(false), signatureShapes(0), signatureUVRest(0),
      activeGroup(-1), uvActiveGroup(-1), radius(0.0f), aabbOk(false), groupedVerts(0), statsNoEditable(false),
      flows(0), refsRoutine(0), vboPos(0), vboNor(0), vboCol(0), vboUV(0), vboIdx(0), vboShow(0), rejectedVbo(0),
      topoState(0),
      onlyGPU(false), ramFixed(false), releasePend(false), arraysHas(0), releasedBytes(0) {
    isResource = true;
    for (int k = 0; k < 3; k++) { aabbMin[k] = aabbMax[k] = center[k] = 0.0f; }
}

// los VBO del recurso (el contexto GL tiene que estar vivo, igual que en ~Mesh)
static void DeleteVBO(MeshResource* r) {
    gfx::DeleteBuffer(r->vboPos); gfx::DeleteBuffer(r->vboNor); gfx::DeleteBuffer(r->vboCol);
    gfx::DeleteBuffer(r->vboUV);  gfx::DeleteBuffer(r->vboIdx);
    for (size_t k = 0; k < r->vboUVx.size(); k++) gfx::DeleteBuffer(r->vboUVx[k]);
    r->vboUVx.clear();
    r->vboPos  = r->vboNor = r->vboCol = r->vboUV = r->vboIdx = 0;
    r->vboShow = 0;
}

static void RemovePendingOnlyGPU(MeshResource* r);
MeshResource::~MeshResource() {
    RemovePendingOnlyGPU(this);   // (anotada para soltar su RAM: la lista no puede quedar con un puntero muerto)
    W3dMeshFlowsDelete(this);
    DeleteVBO(this);
    // los arrays los libera ~MeshData
}

std::string MeshResource::IdStore() const {
    return entry.empty() ? ("*" + IntegerText(serial)) : entry;
}

// ============================================================================
//  REGISTRO
// ============================================================================
const std::vector<MeshResource*>& W3dMeshesRegistry() { return gRegistry; }

MeshResource* W3dMeshResourceByName(const std::string& name) {
    for (size_t i = 0; i < gRegistry.size(); i++) if (gRegistry[i]->name == name) return gRegistry[i];
    // (las de las librerias: su nombre lleva el prefijo de la libreria, "Personajes/Cuerpo")
    for (size_t i = 0; i < gLibMeshes.size(); i++) if (gLibMeshes[i]->name == name) return gLibMeshes[i];
    return 0;
}
MeshResource* W3dMeshResourceByEntry(const std::string& entry) {
    if (entry.empty()) return 0;
    for (size_t i = 0; i < gRegistry.size(); i++) if (gRegistry[i]->entry == entry) return gRegistry[i];
    for (size_t i = 0; i < gLibMeshes.size(); i++) if (gLibMeshes[i]->entry == entry) return gLibMeshes[i];
    return 0;
}
int W3dMeshResourceIndex(const MeshResource* r) {
    for (size_t i = 0; i < gRegistry.size(); i++) if (gRegistry[i] == r) return (int)i;
    return -1;
}
MeshResource* W3dMeshResourceBySerial(int serial) {
    for (size_t i = 0; i < gRegistry.size(); i++) if (gRegistry[i]->serial == serial) return gRegistry[i];
    return 0;
}

struct FreeNameCtx { const MeshResource* except; };
static bool MeshNameExists(const std::string& n, void* ctx) {
    const MeshResource* ex = ctx ? ((FreeNameCtx*)ctx)->except : 0;
    for (size_t i = 0; i < gRegistry.size(); i++)
        if (gRegistry[i] != ex && gRegistry[i]->name == n) return true;
    return false;
}
std::string W3dMeshFreeName(const std::string& base, const MeshResource* except) {
    FreeNameCtx c; c.except = except;
    return W3dUniqueName(base, "Mesh", MeshNameExists, &c);
}

std::string W3dMeshFolderNormalize(const std::string& c) {
    std::string out, segment;
    for (size_t i = 0; i <= c.size(); i++) {
        const char ch = (i < c.size()) ? c[i] : '/';
        if (ch != '/' && ch != '\\') { segment += ch; continue; }
        // fin de un tramo: sin espacios en los bordes; vacio = se descarta ("a//b" = "a/b")
        size_t a = 0, b = segment.size();
        while (a < b && (segment[a] == ' ' || segment[a] == '\t')) a++;
        while (b > a && (segment[b - 1] == ' ' || segment[b - 1] == '\t')) b--;
        if (b > a) {
            if (!out.empty()) out += '/';
            out.append(segment, a, b - a);
        }
        segment.clear();
    }
    return out;
}

MeshResource* W3dMeshNewResource(const std::string& name, const std::string& entry,
                                   const std::string& folder) {
    W3dMeshesEnsureOps();
    MeshResource* r = new MeshResource();
    r->name = W3dMeshFreeName(name);
    r->entry  = entry;
    r->folder = folder;
    gRegistry.push_back(r);
    return r;
}

// borra un recurso que se cierra (el del proyecto o el de una libreria). false = todavia lo usan: queda vivo
// (retenido) con un aviso, para que ningun puntero quede colgando
static bool CloseResource(MeshResource* r) {
    if (!r->users.empty()) {
        w3dLogfW("[mallas] cierre: el recurso '%s' todavia tiene %d usuario(s); queda vivo",
                 r->name.c_str(), (int)r->users.size());
        gRetained.push_back(r);
        return false;
    }
    if (r->rec) {
        if (!W3dResourceForget(W3DREC_MESH, r->rec->id)) {
            w3dLogfW("[mallas] cierre: el recurso '%s' todavia tiene referencias; queda vivo", r->name.c_str());
            gRetained.push_back(r);
            return false;
        }
        r->rec = 0;
    }
    delete r;
    return true;
}

MeshResource* W3dMeshResourceNewLib(const std::string& name, const std::string& entry,
                                      const std::string& lib) {
    W3dMeshesEnsureOps();
    for (size_t i = 0; i < gLibMeshes.size(); i++) if (gLibMeshes[i]->name == name) return gLibMeshes[i];
    MeshResource* r = new MeshResource();
    r->name = name;          // (con su prefijo: no compite con los nombres del proyecto)
    r->entry = entry;
    r->lib = lib;
    gLibMeshes.push_back(r);
    return r;
}
void W3dMeshesLibClear(const std::string& lib) {
    for (size_t i = gLibMeshes.size(); i-- > 0; ) {
        MeshResource* r = gLibMeshes[i];
        if (!lib.empty() && r->lib != lib) continue;
        gLibMeshes.erase(gLibMeshes.begin() + (long)i);
        gMeshesGen++;
        CloseResource(r);
    }
}
int W3dMeshesLibCount() { return (int)gLibMeshes.size(); }
const std::vector<MeshResource*>& W3dMeshesLib() { return gLibMeshes; }

void W3dMeshesRegistryClear() {
    // las de las LIBRERIAS se cierran con el proyecto (el arbol que las usaba ya no existe)
    W3dMeshesLibClear(std::string());
    for (size_t i = 0; i < gRegistry.size(); i++) {
        MeshResource* r = gRegistry[i];
        if (!r->users.empty()) {
            // no deberia pasar: el arbol ya se destruyo. Borrarlo dejaria punteros colgando.
            w3dLogfW("[mallas] cierre: el recurso '%s' todavia tiene %d usuario(s); queda vivo",
                     r->name.c_str(), (int)r->users.size());
            gRetained.push_back(r);
            continue;
        }
        if (r->rec) {
            // cargado sin usuarios: su descriptor se olvida (los arrays se van con el recurso).
            // Si alguien todavia tiene una referencia (no deberia: las transitorias ya se
            // purgaron), el descriptor apunta aca y el recurso no se puede borrar.
            // (Por la clave del DESCRIPTOR: es la que vale en el almacen.)
            if (!W3dResourceForget(W3DREC_MESH, r->rec->id)) {
                w3dLogfW("[mallas] cierre: el recurso '%s' todavia tiene referencias; queda vivo",
                         r->name.c_str());
                gRetained.push_back(r);
                continue;
            }
            r->rec = 0;
        }
        delete r;
    }
    gRegistry.clear();
    gEdited.clear();
    gMeshesGen++;
}

void W3dMeshResourceUndoRetain(MeshResource* r) { if (r) r->refsUndo++; }
void W3dMeshResourceUndoRelease(MeshResource* r)  { if (r && r->refsUndo > 0) r->refsUndo--; }

int W3dMeshesPurgeUnused() {
    int n = 0;
    for (size_t i = 0; i < gRegistry.size(); ) {
        MeshResource* r = gRegistry[i];
        // usado (tambien por una malla detachada que un undo puede devolver) o nombrado por un paso de
        // undo: se queda. Del archivo (huerfano del proyecto) o un huerfano PEDIDO: se queda, salvo que
        // el usuario lo haya BORRADO (ya no es del proyecto: se va apenas nadie lo nombra)
        if (!r->users.empty() || r->refsUndo > 0 || r->refsRoutine > 0) { i++; continue; }
        if (!r->deleted && (!r->entry.empty() || r->keep)) { i++; continue; }
        if (r->rec) {
            // cargado sin usuarios: solo si nadie mas lo referencia (una ListaCarga no nombra
            // recursos sin entrada, pero el almacen es quien sabe)
            if (!W3dResourceForget(W3DREC_MESH, r->rec->id)) { i++; continue; }
            r->rec = 0;
        }
        gRegistry.erase(gRegistry.begin() + (long)i);
        gMeshesGen++;
        delete r;
        n++;
    }
    return n;
}

bool W3dMeshResourceDelete(MeshResource* r) {
    const int i = W3dMeshResourceIndex(r);
    if (i < 0) return false;
    // usado (tambien por una malla detachada que un undo puede devolver) o nombrado por el undo
    if (!r->users.empty() || r->refsUndo > 0) return false;
    if (r->rec) {
        // arrays cargados sin usuarios: solo si el almacen no tiene otra referencia (una
        // ListaCarga "malla:" que la pidio); el descriptor se olvida con la clave que tiene
        if (!W3dResourceForget(W3DREC_MESH, r->rec->id)) return false;
        r->rec = 0;
    }
    gRegistry.erase(gRegistry.begin() + (long)i);
    gMeshesGen++;
    delete r;
    return true;
}

// ============================================================================
//  DATOS
// ============================================================================
bool W3dMeshResourceReadBytes(const MeshResource* r, std::string& out) {
    out.clear();
    if (!r) return false;
    if (!r->bytes.empty()) { out = r->bytes; return true; }
    if (r->entry.empty()) return false;
    std::vector<unsigned char> d;
    if (!w3dFileSystem::ReadFileBytes(r->entry, d) || d.empty()) return false;
    if (!W3dMeshBinIs(&d[0], d.size())) {
        // UNA MALLA DEL REGISTRO GUARDADA EN TEXTO (.w3dm, "formatoMallas": "texto"): el recurso
        // vive en binario, asi que el texto se convierte UNA vez (lo hace el lector del proyecto)
        // y queda como cache en memoria hasta que se descarguen los arrays
        if (!W3dMeshTextToBinHook || !W3dMeshTextToBinHook(&d[0], d.size(), r->name, out) || out.empty()) {
            out.clear();
            return false;
        }
        MeshResource* rw = const_cast<MeshResource*>(r);
        rw->bytes = out;
        rw->textBytes = true;
        return true;
    }
    out.assign((const char*)&d[0], d.size());
    return true;
}

// resuelve el material de cada parte por su NOMBRE (el del archivo en la carga del proyecto). En una malla de
// LIBRERIA los nombres de sus partes son los de SU archivo: sus materiales viven con el prefijo de la libreria
// ("Personajes/Piel"), en su propio espacio de nombres
static Material* MaterialByName(const MeshResource* r, const std::string& n0) {
    if (n0.empty()) return 0;
    const std::string n = (r && !r->lib.empty()) ? r->lib + "/" + n0 : n0;
    if (W3dMeshMaterialByNameHook) return W3dMeshMaterialByNameHook(n);
    return FindMaterialByName(n);
}

// ============================================================================
//  LOS EXTRAS (ver el .h): la firma, soltarlos y conseguirlos de nuevo
// ============================================================================
// la firma de lo que el recurso tiene AHORA en sus listas (recien llenadas de los bytes)
static void SignExtras(MeshResource* r) {
    r->signatureGroups.clear(); r->signatureUVGroups.clear();
    for (size_t g = 0; g < r->groups.size(); g++) {
        MeshResource::SignatureGroup f; f.name = r->groups[g].name; f.n = r->groups[g].idx.size();
        r->signatureGroups.push_back(f);
    }
    for (size_t g = 0; g < r->uvGroups.size(); g++) {
        MeshResource::SignatureGroup f; f.name = r->uvGroups[g].name; f.n = r->uvGroups[g].idx.size();
        r->signatureUVGroups.push_back(f);
    }
    r->signatureShapes = r->shapes.size();
    r->signatureUVRest = r->uvRest.size();
    r->extrasInResource = true;
}
// el recurso ya no guarda los extras (un usuario limpio los tiene): se libera la memoria
static void ReleaseExtras(MeshResource* r) {
    if (!r->extrasInResource) return;
    std::vector<int>().swap(r->ctrl);
    std::vector<W3dMeshBinGroup>().swap(r->groups);
    std::vector<W3dMeshBinGroup>().swap(r->uvGroups);
    std::vector<W3dMeshBinShape>().swap(r->shapes);
    std::vector<float>().swap(r->uvRest);
    r->extrasInResource = false;
}
// la malla necesita la identidad de los puntos? (el skinning con grupos o formas la lee)
static bool WithRig(const MeshResource* r) { return !r->signatureGroups.empty() || r->signatureShapes > 0; }

// ============================================================================
//  LA TOPOLOGIA COMPARTIDA (ver el .h)
// ============================================================================
static void ThrowTopo(MeshResource* r) {
    std::vector<int>().swap(r->edges);
    std::vector<GLfloat>().swap(r->borders);
    std::vector<MeshFace>().swap(r->polyFaces);
    r->topoState = 0;
}
static bool EnsureTopo(MeshResource* r, bool withFaces) {
    if (r->topoState < 0) return false;
    if (r->topoState >= (withFaces ? 2 : 1)) return true;
    std::string b;
    std::vector<int> ar;
    std::vector<MeshFace> cs;
    if (!W3dMeshResourceReadBytes(r, b) || b.empty() ||
        !W3dMeshBinReadTopology((const unsigned char*)b.data(), b.size(), ar, withFaces ? &cs : 0)) {
        r->topoState = -1;   // una sola vez: no se reintenta en cada frame
        return false;
    }
    // indices fuera de los render-verts del recurso = no calza (no se dibuja nada raro)
    for (size_t i = 0; i < ar.size(); i++) if (ar[i] < 0 || ar[i] >= r->vertexSize) { r->topoState = -1; return false; }
    r->edges.swap(ar);
    if (withFaces) r->polyFaces.swap(cs);
    std::vector<GLfloat>().swap(r->borders);
    r->topoState = withFaces ? 2 : 1;
    return true;
}
// la malla se puede MIRAR con la topologia del recurso? (su edicion sigue ahi y mide lo mismo)
static bool TopoOfResource(const Mesh* m) {
    return m && m->mesh && m->mesh->rec && !m->noEditable &&
           m->pendingEdit == W3D_RESOURCE_EDIT && m->vertexSize == m->mesh->vertexSize;
}
const std::vector<int>* W3dMeshEdgesView(const Mesh* m) {
    if (!m) return 0;
    if (!m->edges.empty()) return &m->edges;
    if (!TopoOfResource(m) || !EnsureTopo(m->mesh, false) || m->mesh->edges.empty()) return 0;
    return &m->mesh->edges;
}
const std::vector<MeshFace>* W3dMeshFacesView(const Mesh* m) {
    if (!m) return 0;
    if (!m->faces3d.empty()) return &m->faces3d;
    if (!TopoOfResource(m) || !EnsureTopo(m->mesh, true) || m->mesh->polyFaces.empty()) return 0;
    return &m->mesh->polyFaces;
}
const std::vector<GLfloat>* W3dMeshBordersView(const Mesh* m) {
    if (!m) return 0;
    if (!m->bordersBuf.empty()) return &m->bordersBuf;
    if (!TopoOfResource(m) || !(m->dataComp & W3DMD_POS) || m->vertex != m->mesh->vertex) return 0;
    MeshResource* r = m->mesh;
    if (!EnsureTopo(r, false) || r->edges.empty() || !r->vertex) return 0;
    if (r->borders.empty()) {
        // lineal desde las aristas, como BuildBordersBuf del lector (una vez para todos los usuarios)
        r->borders.reserve(r->edges.size() * 3);
        for (size_t e = 0; e + 1 < r->edges.size(); e += 2) {
            const int a = r->edges[e], b = r->edges[e + 1];
            r->borders.push_back(r->vertex[a * 3]); r->borders.push_back(r->vertex[a * 3 + 1]); r->borders.push_back(r->vertex[a * 3 + 2]);
            r->borders.push_back(r->vertex[b * 3]); r->borders.push_back(r->vertex[b * 3 + 1]); r->borders.push_back(r->vertex[b * 3 + 2]);
        }
    }
    return &r->borders;
}

// llena los arrays y los extras del recurso desde un .w3db. Los arrays NUEVOS se alocan aca:
// el que llama ya aparto los viejos (los libera despues de re-apuntar a los usuarios).
static bool FillFromBytes(MeshResource* r, const unsigned char* d, size_t n) {
    W3dMeshRender R;
    W3dMeshInfo info;
    W3dForeign stamp;
    if (!W3dMeshBinReadRender(d, n, R, &info, &stamp)) {
        for (size_t k = 0; k < info.notices.size(); k++)
            w3dLogfE("[mallas] '%s': %s", r->name.c_str(), info.notices[k].c_str());
        return false;
    }
    const int nRV = R.nRV;
    r->vertexSize = nRV;
    r->facesSize = (int)R.idx.size();
    r->vertex = 0; r->normals = 0; r->uv = 0; r->vertexColor = 0; r->faces = 0;
    if (nRV > 0) {
        r->vertex      = new GLfloat[(size_t)nRV * 3];
        r->normals     = new GLbyte[(size_t)nRV * 3];
        r->uv          = new GLfloat[(size_t)nRV * 2];
        r->vertexColor = new GLubyte[(size_t)nRV * 4];
        memcpy(r->vertex, &R.pos[0], (size_t)nRV * 3 * sizeof(GLfloat));
        memcpy(r->normals, &R.nrm[0], (size_t)nRV * 3);
        memcpy(r->uv, &R.uv[0], (size_t)nRV * 2 * sizeof(GLfloat));
        memcpy(r->vertexColor, &R.col[0], (size_t)nRV * 4);
    }
    r->faces = new MeshIndex[r->facesSize > 0 ? r->facesSize : 1];
    if (r->facesSize > 0) memcpy(r->faces, &R.idx[0], (size_t)r->facesSize * sizeof(MeshIndex));
    r->uvExtra = R.uvExtra;
    r->smooth = R.smooth;
    r->primType = R.primType; r->primSize = R.primSize; r->primSize2 = R.primSize2; r->primHeight = R.primHeight;
    r->primVerts = R.primVerts; r->primVerts2 = R.primVerts2;
    r->parts.swap(R.parts);
    // el material de cada parte: se resuelve UNA vez (la primera carga). Recargar el mismo
    // recurso (se libero y se volvio a pedir) conserva los punteros: el nombre del archivo pudo
    // quedar viejo si el material se renombro en la sesion.
    if (r->materials.size() != r->parts.size()) {
        r->materials.assign(r->parts.size(), (Material*)0);
        for (size_t k = 0; k < r->parts.size(); k++) r->materials[k] = MaterialByName(r, r->parts[k].material);
    }
    r->ctrl.swap(R.ctrl);
    r->groups.swap(R.groups);
    r->uvGroups.swap(R.uvGroups);
    r->shapes.swap(R.shapes);
    r->uvRest.swap(R.uvRest);
    SignExtras(r);
    r->activeGroup = R.activeGroup; r->uvActiveGroup = R.uvActiveGroup;
    for (int k = 0; k < 3; k++) { r->aabbMin[k] = R.aabbMin[k]; r->aabbMax[k] = R.aabbMax[k]; r->center[k] = R.center[k]; }
    r->radius = R.radius; r->aabbOk = R.aabbOk; r->groupedVerts = R.groupedVerts;
    r->statsNoEditable = R.statsNoEditable;
    r->stamp = stamp;
    return true;
}

static void FreeArrays(MeshResource* r) {
    delete[] r->vertex; delete[] r->normals; delete[] r->uv; delete[] r->vertexColor; delete[] r->faces;
    r->vertex = 0; r->normals = 0; r->uv = 0; r->vertexColor = 0; r->faces = 0;
    r->uvExtra.clear();
    r->vertexSize = 0; r->facesSize = 0;
    // (una malla onlyGPU ya no tenia arrays: lo que sigue la deja sin geometria, como cualquier otra)
    r->onlyGPU = false; r->arraysHas = 0; r->layersUVHas.clear(); r->releasedBytes = 0;
}

void W3dMeshResourceSetBytes(MeshResource* r, const std::string& bytes) {
    if (!r) return;
    r->bytes = bytes;
    r->modified = true;
    r->textBytes = false;
    r->version++;
    g_w3dRoutinesGen++;
    ThrowTopo(r);
    // cargado sin usuarios (una referencia de ListaCarga): los arrays se rehacen de los bytes nuevos
    if (r->rec && r->users.empty() && !bytes.empty()) {
        FreeArrays(r);
        DeleteVBO(r);
        if (FillFromBytes(r, (const unsigned char*)bytes.data(), bytes.size())) r->rec->bytes = r->Bytes();
    }
}

bool W3dMeshResourceMaterials(MeshResource* r, std::vector<std::string>* inFile) {
    if (!r) return false;
    // CARGADO: las partes (con el nombre de material de los bytes) y sus materiales ya estan
    if (!r->parts.empty() && r->materials.size() == r->parts.size()) {
        if (inFile) {
            inFile->clear();
            for (size_t k = 0; k < r->parts.size(); k++) inFile->push_back(r->parts[k].material);
        }
        return true;
    }
    // SIN CARGAR (un huerfano que nadie pidio): los nombres de su PART, sin leer la geometria.
    // Se resuelven UNA vez (como en la primera carga, por el nombre del archivo) y quedan en el
    // recurso: FillFromBytes conserva los punteros si mide lo mismo.
    std::string b;
    std::vector<std::string> names;
    if (!W3dMeshResourceReadBytes(r, b) || !W3dMeshBinMaterials(b, names)) return false;
    if (r->materials.size() != names.size()) {
        r->materials.assign(names.size(), (Material*)0);
        for (size_t k = 0; k < names.size(); k++) r->materials[k] = MaterialByName(r, names[k]);
    }
    if (inFile) inFile->swap(names);
    return true;
}

// ============================================================================
//  EL TIPO EN EL ALMACEN: Cargar lee la entrada (o los bytes en memoria);
//  Liberar DESCARGA los arrays (el recurso sigue en el registro).
// ============================================================================
static MeshResource* ByIdStore(const std::string& id) {
    for (size_t i = 0; i < gRegistry.size(); i++) if (gRegistry[i]->IdStore() == id) return gRegistry[i];
    for (size_t i = 0; i < gLibMeshes.size(); i++) if (gLibMeshes[i]->IdStore() == id) return gLibMeshes[i];
    return 0;
}

// LOS BLOQUES DE RENDER de un .w3db, sin los de edicion (FACE, CNRM, UVLY... la mitad de los bytes de una malla de
// escenario): lo que el streaming necesita para dibujar. Lee la cabecera y la cabecera de cada bloque, y despues
// solo los tramos de render (los contiguos en UNA lectura), y arma un .w3db valido con esos bloques. false = no es un
// .w3db (o no se pudo): el que llama lee la entrada entera
static bool ReadOnlyRender(const std::string& entry, std::string& out) {
    out.clear();
    std::vector<unsigned char> cab;
    if (!w3dFileSystem::ReadFileRange(entry, 0, 20, cab) || cab.size() != 20 || memcmp(&cab[0], "W3DB", 4) != 0)
        return false;
    const unsigned nBlocks = cab[12] | (cab[13] << 8) | (cab[14] << 16) | ((unsigned)cab[15] << 24);
    if (nBlocks > 4096) return false;
    std::vector<std::pair<unsigned, unsigned> > segments;   // (desde, bytes) de lo que se queda
    unsigned p = 20, remain = 0, skipped = 0;
    std::vector<unsigned char> bh;
    for (unsigned k = 0; k < nBlocks; k++) {
        if (!w3dFileSystem::ReadFileRange(entry, p, 16, bh) || bh.size() != 16) return false;
        const unsigned bytes = bh[12] | (bh[13] << 8) | (bh[14] << 16) | ((unsigned)bh[15] << 24);
        const unsigned total = 16 + ((bytes + 3) & ~3u);
        static const char* const kEdit[] = { "FACE", "LOOS", "CNRM", "UVLY", "COLY", "SHRP", "SEAM", "EDGE", "AJEN" };
        bool edit = false;
        for (size_t e = 0; e < sizeof(kEdit) / sizeof(kEdit[0]) && !edit; e++)
            edit = memcmp(&bh[0], kEdit[e], 4) == 0;
        if (edit) skipped++;
        else {
            if (!segments.empty() && segments.back().first + segments.back().second == p) segments.back().second += total;
            else segments.push_back(std::make_pair(p, total));
            remain++;
        }
        p += total;
    }
    if (!skipped) return false;   // (nada que saltear: la lectura entera es una sola)
    out.reserve(20 + (p - 20));
    cab[12] = (unsigned char)(remain & 0xFF); cab[13] = (unsigned char)((remain >> 8) & 0xFF);
    cab[14] = (unsigned char)((remain >> 16) & 0xFF); cab[15] = (unsigned char)((remain >> 24) & 0xFF);
    out.append((const char*)&cab[0], 20);
    std::vector<unsigned char> t;
    for (size_t i = 0; i < segments.size(); i++) {
        if (!w3dFileSystem::ReadFileRange(entry, segments[i].first, segments[i].second, t) || t.size() != segments[i].second) {
            out.clear();
            return false;
        }
        out.append((const char*)&t[0], t.size());
    }
    return true;
}

static void* OpLoadMesh(const std::string& id, long* bytes) {
    MeshResource* r = ByIdStore(id);
    if (!r) return 0;   // solo se cargan las del REGISTRO del proyecto (o las de una libreria)
    // YA CARGADO bajo OTRO descriptor (su clave quedo vieja): recargarlo liberaria los arrays que
    // sus usuarios estan apuntando (uso de memoria liberada). Un recurso tiene UN descriptor.
    if (r->rec && r->rec->id != id) {
        w3dLogfE("[mallas] la malla '%s' ya esta cargada como '%s': no la vuelvo a cargar como '%s'",
                 r->name.c_str(), r->rec->id.c_str(), id.c_str());
        return 0;
    }
    std::string b;
    // (lo que se carga aca son los arrays de DIBUJO: de un .w3db en el almacen alcanza con sus bloques de render; la
    //  edicion, si alguien la pide, se lee aparte -pendingEdit-)
    const bool onlyRender = r->bytes.empty() && !r->entry.empty() && ReadOnlyRender(r->entry, b);
    if (!onlyRender && !W3dMeshResourceReadBytes(r, b)) {
        w3dLogfE("[mallas] no pude leer la malla '%s' (%s)", r->name.c_str(), id.c_str());
        return 0;
    }
    FreeArrays(r);
    DeleteVBO(r);
    ThrowTopo(r);
    if (!FillFromBytes(r, (const unsigned char*)b.data(), b.size())) {
        w3dLogfE("[mallas] '%s' (%s) no es un .w3db que este Whisk3D pueda leer", r->name.c_str(), id.c_str());
        return 0;
    }
    if (bytes) *bytes = r->Bytes();
    // el descriptor ya existe (el almacen lo crea antes de llamar al op): el recurso lo anota,
    // lo haya pedido un objeto (W3dMeshLink) o una ListaCarga "malla:" (la bomba ASYNC)
    r->rec = W3dResourceFind(W3DREC_MESH, id);
    return (void*)(MeshData*)r;
}

static bool OpFreeMesh(void* data) {
    MeshData* d = (MeshData*)data;
    if (!d) return true;
    if (d->isResource) {
        // el recurso NO se borra (es del registro): se descargan sus arrays y sus VBO
        MeshResource* r = static_cast<MeshResource*>(d);
        RemovePendingOnlyGPU(r);
        r->ramFixed = false;   // (lo vuelve a pedir el paso que la lee de RAM, si la malla se vuelve a cargar)
        FreeArrays(r);
        DeleteVBO(r);
        ThrowTopo(r);
        ReleaseExtras(r);
        // la conversion de una entrada de texto es cache: se vuelve a hacer si se recarga
        if (r->textBytes) { std::string().swap(r->bytes); r->textBytes = false; }
        r->rec = 0;
        return true;
    }
    delete d;   // la geometria compartida de un .obj (MeshData.h)
    return true;
}

// el gancho con el que Mesh suelta una MeshData legacy (W3dMeshDataReleaseHook, Mesh.cpp)
extern void (*W3dMeshDataReleaseHook)(void*);
static void ReleaseRecMesh(void* rec) {
    W3dResourceRelease((W3dResource*)rec, W3DREC_PERMANENT);
}

void W3dMeshesEnsureOps() {
    static bool ready = false;
    if (ready) return;
    W3dResourceOps ops;
    ops.Load = OpLoadMesh;
    ops.Free = OpFreeMesh;
    W3dResourcesRegisterType(W3DREC_MESH, ops);
    W3dMeshDataReleaseHook = ReleaseRecMesh;
    ready = true;
}

// ============================================================================
//  USUARIOS
// ============================================================================
static void RemoveUser(MeshResource* r, Mesh* m) {
    for (size_t i = 0; i < r->users.size(); i++)
        if (r->users[i] == m) { r->users.erase(r->users.begin() + (long)i); return; }
}
static void RemoveEdited(Mesh* m) {
    for (size_t i = 0; i < gEdited.size(); i++)
        if (gEdited[i] == m) { gEdited.erase(gEdited.begin() + (long)i); return; }
}

// la edicion materializada de la malla (caras, capas, marcas, aristas): se tira
static void ClearEdit(Mesh* m) {
    m->InvalidateEdit();
    m->faces3d.clear();
    m->looseEdges.clear(); m->looseVerts.clear();
    m->cornerNormal.clear();
    for (size_t i = 0; i < m->uvMaps.size(); i++)      delete m->uvMaps[i];
    for (size_t i = 0; i < m->colorLayers.size(); i++) delete m->colorLayers[i];
    m->uvMaps.clear();      m->uvActiveMap = -1;
    m->colorLayers.clear(); m->activeColor = -1;
    m->sharpEdges.clear();  m->seamEdges.clear();
    m->posRep.clear();
    m->edges.clear();
    m->bordersBuf.clear();
    m->uvSelVert.clear();
    m->normFaceBuf.clear(); m->normCustomBuf.clear(); m->normVertBuf.clear();
    m->w3dmForeign.blocks.clear();
    m->w3dmForeign.requiresValue.clear();
}

// un usuario que tiene los EXTRAS tal cual el recurso: limpio (nada suyo pudo cambiar: su
// edicion sigue pendiente y sus listas miden lo que dice la firma) y con lo que haga falta
static const Mesh* ExtrasDonor(const MeshResource* r, const Mesh* except, bool withCtrl) {
    for (size_t i = 0; i < r->users.size(); i++) {
        const Mesh* u = r->users[i];
        if (!u || u == except || !W3dMeshClean(u)) continue;
        if (u->uv2dRest.size() != r->signatureUVRest) continue;          // un reposo 2D capturado en la sesion
        if (withCtrl && (int)u->vertCtrlPoint.size() != u->vertexSize) continue;
        return u;
    }
    return 0;
}
// los extras de unas listas (las del recurso o las releidas de los bytes) a la malla
static void ListExtras(Mesh* m, const std::vector<W3dMeshBinGroup>& groups,
                           const std::vector<W3dMeshBinGroup>& uvGroups, const std::vector<W3dMeshBinShape>& shapes,
                           const std::vector<float>& uvRest, const std::vector<int>& ctrl, bool withCtrl) {
    for (size_t g = 0; g < groups.size(); g++) {
        VertexGroup* vg = new VertexGroup(groups[g].name);
        vg->verts = groups[g].idx; vg->weights = groups[g].weights;
        m->vertexGroups.push_back(vg);
    }
    for (size_t g = 0; g < uvGroups.size(); g++) {
        UVGroup* ug = new UVGroup(uvGroups[g].name);
        ug->verts = uvGroups[g].idx; ug->weights = uvGroups[g].weights;
        m->uvGroups.push_back(ug);
    }
    for (size_t k = 0; k < shapes.size(); k++) {
        W3dShapeKey sk; sk.name = shapes[k].name; sk.idx = shapes[k].idx; sk.d = shapes[k].d;
        m->shapeKeys.push_back(sk);
    }
    m->uv2dRest = uvRest;
    if (withCtrl) m->vertCtrlPoint = ctrl;
    else m->vertCtrlPoint.clear();
}
// releer los extras de los bytes del recurso (nadie en memoria los tiene tal cual)
static bool RereadExtras(const MeshResource* r, W3dMeshRender& X) {
    std::string b;
    if (!W3dMeshResourceReadBytes(r, b) || b.empty()) return false;
    return W3dMeshBinReadExtras((const unsigned char*)b.data(), b.size(), X) && X.nRV == r->vertexSize;
}
// grupos, formas, reposo 2D e identidad de los puntos: CHICOS y por instancia (el skinning, el
// weight paint y el editor los leen de la malla). Salen del recurso si todavia los guarda, de
// otro usuario limpio (la copia que ya esta en memoria) o, si no hay, de los bytes.
static void ExtrasToMesh(Mesh* m, MeshResource* r, bool keepEdit) {
    for (size_t i = 0; i < m->vertexGroups.size(); i++) delete m->vertexGroups[i];
    for (size_t i = 0; i < m->uvGroups.size(); i++)     delete m->uvGroups[i];
    m->vertexGroups.clear(); m->uvGroups.clear();
    m->shapeKeys.clear();
    // LA IDENTIDAD DE LOS PUNTOS: la necesita el skinning (vertex groups / shape keys) y la
    // edicion (posRep sale de aca). Sin grupos ni formas se difiere a la materializacion: mil
    // arboles sin rig no pagan un int por vertice cada uno.
    const bool withCtrl = WithRig(r) || keepEdit;
    const Mesh* d = r->extrasInResource ? 0 : ExtrasDonor(r, m, withCtrl);
    if (r->extrasInResource) {
        ListExtras(m, r->groups, r->uvGroups, r->shapes, r->uvRest, r->ctrl, withCtrl);
    } else if (d) {
        for (size_t g = 0; g < d->vertexGroups.size(); g++)
            m->vertexGroups.push_back(d->vertexGroups[g] ? new VertexGroup(*d->vertexGroups[g]) : new VertexGroup(""));
        for (size_t g = 0; g < d->uvGroups.size(); g++)
            m->uvGroups.push_back(d->uvGroups[g] ? new UVGroup(*d->uvGroups[g]) : new UVGroup(""));
        m->shapeKeys = d->shapeKeys;
        m->uv2dRest = d->uv2dRest;
        if (withCtrl) m->vertCtrlPoint = d->vertCtrlPoint;
        else m->vertCtrlPoint.clear();
    } else {
        W3dMeshRender X;
        if (RereadExtras(r, X)) ListExtras(m, X.groups, X.uvGroups, X.shapes, X.uvRest, X.ctrl, withCtrl);
        else {
            w3dLogfW("[mallas] no pude releer los grupos/formas de la malla '%s' para '%s'",
                     r->name.c_str(), m->name.c_str());
            m->uv2dRest.clear();
            m->vertCtrlPoint.clear();
        }
    }
    m->activeGroup = r->activeGroup; m->uvActiveGroup = r->uvActiveGroup;
    m->shapeWeights.assign(m->shapeKeys.size(), 0.0f);
    // NO se reagrupa al cargar: seria gastar CPU del telefono en cada apertura. El agrupamiento de los puntos
    // de skinning viene HORNEADO en el .w3db (vertCtrlPoint compacto + grupos/formas por punto) cuando la malla
    // se guardo como NO-EDITABLE (ver W3dMeshBinWrite). Una malla editable vieja carga sin agrupar (mas lenta,
    // pero editable); convertila a no-editable y volve a guardar para hornear el agrupamiento.
}
// la identidad de los puntos del RECURSO (canonica) para el AABB: la de la malla si la tiene,
// la del recurso si todavia la guarda, o releida
static const int* ResourceCtrl(const MeshResource* r, const Mesh* m, std::vector<int>& tmp) {
    if (r->vertexSize <= 0) return 0;
    if ((int)m->vertCtrlPoint.size() == r->vertexSize) return &m->vertCtrlPoint[0];
    if ((int)r->ctrl.size() == r->vertexSize) return &r->ctrl[0];
    W3dMeshRender X;
    if (!RereadExtras(r, X) || (int)X.ctrl.size() != r->vertexSize) return 0;
    tmp.swap(X.ctrl);
    return &tmp[0];
}

// LA POSE DE LA SESION vuelve en el acto: re-apuntar una malla a los datos del recurso la deja
// EN REPOSO (los arrays compartidos son el modelo). Si estaba posada por una vertex anim o tiene
// flipbook / rig 2D, esa pose se vuelve a aplicar YA, en el mismo cuadro: sin esto, publicar la
// edicion de OTRO usuario (o un Ctrl+Z del vinculo) la dejaba en reposo hasta el proximo cuadro
// de playback, y con el playback en pausa, para siempre.
static void RePose(Mesh* u, bool wasPosed) {
    if (wasPosed && !u->animations.empty()) {
        // el mismo cuadro que la poso: el del playhead si es la anim del timeline (kind 3), si no
        // el del controlador de reproduccion de la malla
        VertexAnimationActive* va = FindTargetAnim(u);
        const int ai = va ? va->currentAnim : -1;
        if (ai >= 0 && ai < (int)u->animations.size() && u->animations[(size_t)ai] &&
            !u->animations[(size_t)ai]->frames.empty()) {
            const float frame = (ActiveAnimKind == 3 && ActiveAnimMesh == u) ? (float)CurrentFrame : va->playFrame;
            EvalVertexAnim(*u->animations[(size_t)ai], u, frame);
        }
    }
    if (u->HasArm2D()) u->Armature2DApply();     // uv = f(reposo 2D, pose)
    if (u->flipbook) u->TickUVAnimStrip(0.0f);   // el cuadro actual del flipbook (sin avanzar)
}

// los datos del recurso -> la malla (los punteros de render APUNTAN ADENTRO del recurso)
static void ApplyToMesh(Mesh* m, MeshResource* r, bool keepEdit) {
    m->vertexSize = r->vertexSize;
    m->vertex = r->vertex; m->normals = r->normals; m->uv = r->uv; m->vertexColor = r->vertexColor;
    m->facesSize = r->facesSize; m->faces = r->faces;
    m->uvExtra = r->uvExtra;   // copia por usuario (no es un array compartido: ver Mesh.h)
    unsigned mask = 0;
    if (r->vertex)      mask |= W3DMD_POS;
    if (r->normals)     mask |= W3DMD_NOR;
    if (r->uv)          mask |= W3DMD_UV;
    if (r->vertexColor) mask |= W3DMD_COL;
    if (r->faces)       mask |= W3DMD_FACES;
    m->dataComp = (unsigned char)mask;
    m->posedByAnim = false;   // el recurso esta EN REPOSO (el playback la vuelve a posar)
    m->meshSmooth = r->smooth;
    if (r->primType >= 0) {
        m->meshType = r->primType; m->meshSize = r->primSize; m->meshSize2 = r->primSize2;
        m->meshDepth = r->primHeight; m->meshVerts = r->primVerts; m->meshVerts2 = r->primVerts2;
    } else m->meshType = -1;
    // mesh parts: rango del index buffer compartido + el material (dato de la malla)
    m->materialsGroup.clear();
    for (size_t k = 0; k < r->parts.size(); k++) {
        MaterialGroup mg;
        mg.name = r->parts[k].name;
        mg.startDrawn = r->parts[k].start;
        mg.indicesDrawnCount = r->parts[k].count;
        mg.material = k < r->materials.size() ? r->materials[k] : 0;
        m->materialsGroup.push_back(mg);
    }
    // grupos, formas, reposo 2D e identidad de los puntos (ver ExtrasToMesh)
    ExtrasToMesh(m, r, keepEdit);
    // AABB / centro / radio: horneados en el modo del guardado; el objeto puede pedir el otro
    if (m->noEditable == r->statsNoEditable) {
        m->aabbOk = r->aabbOk;
        m->aabbMin = Vector3(r->aabbMin[0], r->aabbMin[1], r->aabbMin[2]);
        m->aabbMax = Vector3(r->aabbMax[0], r->aabbMax[1], r->aabbMax[2]);
        m->centerGeom = Vector3(r->center[0], r->center[1], r->center[2]);
        m->radiusGeom = r->radius;
        m->groupedVerts = m->noEditable ? 0 : r->groupedVerts;
    } else {
        W3dGeomStats st;
        std::vector<int> tmp;
        const int* rep = m->noEditable ? 0 : ResourceCtrl(r, m, tmp);
        W3dComputeGeomStats(r->vertex, r->vertexSize, rep, st);
        m->aabbOk = st.ok;
        if (st.ok) { m->aabbMin = st.aabbMin; m->aabbMax = st.aabbMax; m->centerGeom = st.center; m->radiusGeom = st.radius; }
        m->groupedVerts = m->noEditable ? 0 : st.grouped;
    }
    // la preservacion del .w3db (sello + bloques que no se entienden + REQ)
    m->w3dmForeign.nPoints = r->stamp.nPoints;   m->w3dmForeign.nCorners = r->stamp.nCorners;
    m->w3dmForeign.nFaces = r->stamp.nFaces;     m->w3dmForeign.nEdges = r->stamp.nEdges;
    m->w3dmForeign.nRender = r->stamp.nRender;   m->w3dmForeign.nTriangles = r->stamp.nTriangles;
    m->w3dmForeign.blocksBin = r->stamp.blocksBin;
    m->w3dmForeign.requiresBin = r->stamp.requiresBin;
    m->w3dmForeign.truncated = false;
    m->w3dmForeign.notLoaded = false;
    // LA EDICION: pendiente (sale del recurso cuando alguien la pida) o la que la malla ya tiene
    if (!keepEdit) {
        ClearEdit(m);
        m->pendingEdit = W3D_RESOURCE_EDIT;
    } else {
        m->InvalidateEdit();   // la jaula de Edit Mode se rearma sobre los punteros nuevos
        m->pendingEdit.clear();
    }
    // TODO lo derivado de la geometria vieja se rehace (VBO propios, skinning, reflejos...)
    m->skinGeomVersion += 2;
    m->bordersGeomVersion = m->skinGeomVersion;
    m->overlayLcache = -1.0f;
    m->vboGeomVer = 0xFFFFFFFFu;
    m->lastSkinFrame = -999999;
    m->nextSkinFlat = 0;
    m->skinShapeSerial = 0xFFFFFFFFu;
    m->tangentsValid = false;
    m->chromeUVValid = false;
    m->genChromeValid = false;
    m->flipUvBase.clear(); m->flipApplied = -1;
}

// un usuario LIMPIO ya tiene los extras: el recurso suelta los suyos (el proximo usuario los
// copia de ese o los relee de los bytes). Se llama con TODOS los usuarios al dia (nunca en
// medio de un re-apuntado: uno todavia con los datos viejos pareceria limpio).
static void ReleaseExtrasIfExtra(MeshResource* r) {
    if (r->extrasInResource && ExtrasDonor(r, 0, WithRig(r))) ReleaseExtras(r);
}

// UNA REFERENCIA PERMANENTE sobre el recurso, cargandolo si hace falta. NULL = no se pudo, y en
// ese caso NO queda ninguna referencia sumada (el que llama no tiene nada que soltar).
static W3dResource* AcquireMesh(MeshResource* r) {
    W3dMeshesEnsureOps();
    // YA CARGADO: una referencia mas sobre SU descriptor, por puntero y no por clave (la clave
    // sigue a la entrada: buscarla podria dar OTRO descriptor, que lo volveria a cargar)
    if (r->rec && r->rec->state == W3DREC_READY && r->rec->data == (void*)(MeshData*)r) {
        W3dResourceRetain(r->rec, W3DREC_PERMANENT);
        return r->rec;
    }
    const std::string id = r->IdStore();
    // un FALLO viejo de esa clave que nadie referencia no se recuerda (el proyecto pudo arreglarse)
    W3dResource* old = W3dResourceFind(W3DREC_MESH, id);
    if (old && old->state == W3DREC_FAILED && old->refTotal <= 0) W3dResourceForget(W3DREC_MESH, id);
    // BLOQUEANTE: un descriptor EN_VUELO (una ListaCarga async sin bombear) se carga ahora
    W3dResource* d = W3dResourceAcquire(W3DREC_MESH, id, W3DREC_PERMANENT, W3DREC_BLOCKING, "mesh");
    if (d && d->state == W3DREC_READY && d->data == (void*)(MeshData*)r) return d;
    // no se pudo: la referencia que el almacen haya sumado se devuelve (un FALLO no suma) y el
    // fallo no se recuerda si nadie mas lo referencia
    if (d && d->state != W3DREC_FAILED) W3dResourceRelease(d, W3DREC_PERMANENT);
    else if (d && d->refTotal <= 0) W3dResourceForget(W3DREC_MESH, id);
    return 0;
}

bool W3dMeshResourceRetain(MeshResource* r) {
    if (!r) return false;
    W3dResource* d = AcquireMesh(r);
    if (!d) return false;
    r->rec = d;
    r->refsRoutine++;
    return true;
}
void W3dMeshResourceRelease(MeshResource* r) {
    if (!r || r->refsRoutine <= 0) return;
    r->refsRoutine--;
    if (r->rec) W3dResourceRelease(r->rec, W3DREC_PERMANENT);   // la ultima -> OpFreeMesh descarga
}
// (con una cache como W3dObjectLive: la validacion de las rutinas pregunta por cada paso y recorrer los registros
// era lineal. Una entrada dice "r estaba registrado en la generacion g"; sacar cualquiera del registro la sube)
unsigned W3dMeshesGen() { return gMeshesGen; }
unsigned g_w3dRoutinesGen = 1;
struct LiveMeshCache { const MeshResource* r; unsigned gen; };
static LiveMeshCache gLiveMesh[64];
bool W3dMeshResourceLive(const MeshResource* r) {
    if (!r) return false;
    LiveMeshCache& c = gLiveMesh[((unsigned)(size_t)r * 2654435761u) >> 26];
    if (c.r == r && c.gen == gMeshesGen) return !r->deleted;
    bool is = false;
    for (size_t i = 0; i < gRegistry.size() && !is; i++) is = gRegistry[i] == r;
    for (size_t i = 0; i < gLibMeshes.size() && !is; i++) is = gLibMeshes[i] == r;
    if (!is) return false;
    c.r = r; c.gen = gMeshesGen;
    return !r->deleted;
}

bool W3dMeshLink(Mesh* m, MeshResource* r, bool keepEdit) {
    if (!m || !r) return false;
    if (W3dMeshBeforeLinkHook) W3dMeshBeforeLinkHook(m, r);
    // la referencia NUEVA antes de soltar la vieja (si es el mismo recurso, no se descarga en el medio)
    W3dResource* d = AcquireMesh(r);
    if (!d) return false;
    r->rec = d;
    // una malla que solto su RAM (onlyGPU) la recupera de sus bytes: un objeto apunta ADENTRO de sus arrays
    if (r->onlyGPU && !W3dMeshResourceEnsureRAM(r)) { W3dResourceRelease(d, W3DREC_PERMANENT); return false; }
    const bool posed = m->posedByAnim;   // la pose de la sesion vuelve al final (RePose)
    // lo que la malla tenia: lo COMPARTIDO se suelta (queda NULL) y lo PROPIO se libera
    m->ReleaseDataShared();
    delete[] m->vertex;      m->vertex = 0;
    delete[] m->normals;     m->normals = 0;
    delete[] m->uv;          m->uv = 0;
    delete[] m->vertexColor; m->vertexColor = 0;
    delete[] m->faces;       m->faces = 0;
    m->mesh = r;
    r->users.push_back(m);
    ApplyToMesh(m, r, keepEdit);
    if (posed || m->flipbook || m->HasArm2D()) RePose(m, posed);
    ReleaseExtrasIfExtra(r);
    return true;
}

bool W3dMeshMove(Mesh* m, MeshResource* r) {
    if (!m || !r) return false;
    if (W3dMeshBeforeLinkHook) W3dMeshBeforeLinkHook(m, r);
    if (m->mesh == r) return true;
    W3dResource* d = AcquireMesh(r);
    if (!d) return false;
    r->rec = d;
    // (un usuario: el recurso vuelve a RAM, como en W3dMeshLink; lo leen W3dMeshClean y el guardado)
    if (r->onlyGPU && !W3dMeshResourceEnsureRAM(r)) { W3dResourceRelease(d, W3DREC_PERMANENT); return false; }
    m->UninstantiateData(W3DMD_ALL, true);   // lo que compartia con el recurso viejo, propio
    const std::string pend = m->pendingEdit;
    if (m->mesh) W3dMeshUnlink(m);
    m->pendingEdit = pend;
    m->mesh = r;
    r->users.push_back(m);
    return true;
}

void W3dMeshReleaseEdit(Mesh* m) {
    if (!m || !m->mesh || !m->pendingEdit.empty()) return;
    if (m->edit) return;   // la jaula de Edit Mode la esta usando
    const MeshResource* r = m->mesh;
    unsigned mask = 0;
    if (r->vertex)      mask |= W3DMD_POS;
    if (r->normals)     mask |= W3DMD_NOR;
    if (r->uv)          mask |= W3DMD_UV;
    if (r->vertexColor) mask |= W3DMD_COL;
    if (r->faces)       mask |= W3DMD_FACES;
    if ((unsigned)m->dataComp != mask) return;   // algo propio: la edicion puede ser distinta
    ClearEdit(m);
    if (!WithRig(r)) m->vertCtrlPoint.clear();
    m->pendingEdit = W3D_RESOURCE_EDIT;
}

static unsigned gUnlinked = 0;
unsigned W3dMeshesUnlinked() { return gUnlinked; }
void W3dMeshUnlink(Mesh* m) {
    if (!m || !m->mesh) return;
    gUnlinked++;   // (una malla quedo sin recurso: el editor le da uno, ver W3dMeshesUnlinked)
    MeshResource* r = m->mesh;
    // las capas compartidas no son de la malla: NULL (ningun delete[] posterior las toca)
    if (m->dataComp & W3DMD_POS)   m->vertex = 0;
    if (m->dataComp & W3DMD_NOR)   m->normals = 0;
    if (m->dataComp & W3DMD_UV)    m->uv = 0;
    if (m->dataComp & W3DMD_COL)   m->vertexColor = 0;
    if (m->dataComp & W3DMD_FACES) m->faces = 0;
    m->dataComp = 0;
    RemoveUser(r, m);
    RemoveEdited(m);
    m->mesh = 0;
    // sin recurso, la edicion pendiente ya no tiene de donde salir
    if (m->pendingEdit == W3D_RESOURCE_EDIT) m->pendingEdit.clear();
    if (r->rec) W3dResourceRelease(r->rec, W3DREC_PERMANENT);   // la ultima -> OpFreeMesh descarga
}

static bool InTree(const Object* root, const MeshResource* r, int& n) {
    for (size_t i = 0; i < root->Childrens.size(); i++) {
        Object* o = root->Childrens[i];
        if (!o) continue;
        if (o->getType() == ObjectType::mesh && ((Mesh*)o)->mesh == r) n++;
        InTree(o, r, n);
    }
    return true;
}
int W3dMeshResourceUsersInScene(const MeshResource* r, const Object* root) {
    if (!r) return 0;
    int n = 0;
    if (root) { InTree(root, r, n); return n; }
    // sin raiz: TODAS las raices cargadas del proyecto (la activa y las escenas/prefabs que no se
    // estan mirando). Una malla que solo usa otra escena NO es huerfana ni de un solo usuario.
    std::vector<Object*> roots;
    W3dLiveRoots(roots);
    for (size_t i = 0; i < roots.size(); i++) InTree(roots[i], r, n);
    return n;
}

bool W3dMeshClean(const Mesh* m) {
    if (!m || !m->mesh) return false;
    const MeshResource* r = m->mesh;
    // la edicion tiene que seguir en el recurso: una materializada se pudo tocar (marcas, capas)
    if (m->pendingEdit != W3D_RESOURCE_EDIT) return false;
    unsigned mask = 0;
    if (r->vertex)      mask |= W3DMD_POS;
    if (r->normals)     mask |= W3DMD_NOR;
    if (r->uv)          mask |= W3DMD_UV;
    if (r->vertexColor) mask |= W3DMD_COL;
    if (r->faces)       mask |= W3DMD_FACES;
    if (!r->rec || (unsigned)m->dataComp != mask) return false;
    // lo que se copia por instancia tambien tiene que seguir igual (un rename de mesh part o de
    // grupo en la tarjeta, un grupo nuevo, otra cantidad de formas)
    return W3dMeshExtrasAsResource(m);
}

bool W3dMeshExtrasAsResource(const Mesh* m) {
    if (!m || !m->mesh) return false;
    const MeshResource* r = m->mesh;
    if (m->materialsGroup.size() != r->parts.size()) return false;
    for (size_t k = 0; k < r->parts.size(); k++) {
        if (m->materialsGroup[k].name != r->parts[k].name) return false;
        if (k < r->materials.size() && m->materialsGroup[k].material != r->materials[k]) return false;
    }
    // (contra la FIRMA: el recurso pudo soltar los datos, ver ExtrasToMesh)
    if (m->vertexGroups.size() != r->signatureGroups.size() || m->uvGroups.size() != r->signatureUVGroups.size() ||
        m->shapeKeys.size() != r->signatureShapes) return false;
    for (size_t g = 0; g < r->signatureGroups.size(); g++)
        if (!m->vertexGroups[g] || m->vertexGroups[g]->name != r->signatureGroups[g].name ||
            m->vertexGroups[g]->verts.size() != r->signatureGroups[g].n) return false;
    for (size_t g = 0; g < r->signatureUVGroups.size(); g++)
        if (!m->uvGroups[g] || m->uvGroups[g]->name != r->signatureUVGroups[g].name ||
            m->uvGroups[g]->verts.size() != r->signatureUVGroups[g].n) return false;
    if (m->meshSmooth != r->smooth) return false;
    return true;
}

bool W3dMeshResourceApplyBytes(MeshResource* r, const std::string& bytes, Mesh* except,
                                 const std::vector<Material*>* mats) {
    std::vector<Mesh*> ex;
    if (except) ex.push_back(except);
    return W3dMeshResourceApplyBytes(r, bytes, ex, mats);
}

static bool IsExcept(const std::vector<Mesh*>& ex, const Mesh* u) {
    for (size_t i = 0; i < ex.size(); i++) if (ex[i] == u) return true;
    return false;
}

bool W3dMeshResourceApplyBytes(MeshResource* r, const std::string& bytes, const std::vector<Mesh*>& exceptions,
                                 const std::vector<Material*>* mats) {
    if (!r || bytes.empty()) return false;
    // los EXCEPTUADOS (el que publica, la malla en Edit Mode) se quedan con LO SUYO: una capa que
    // todavia compartieran apunta a los arrays que se van a liberar -> se copia antes (tiene el
    // mismo contenido que ven hoy)
    for (size_t i = 0; i < exceptions.size(); i++)
        if (exceptions[i] && exceptions[i]->mesh == r) exceptions[i]->UninstantiateData(W3DMD_ALL, true);
    // los arrays viejos se apartan: los usuarios los siguen apuntando hasta re-vincularse
    GLfloat* vVie = r->vertex; GLbyte* nVie = r->normals; GLfloat* uVie = r->uv;
    GLubyte* cVie = r->vertexColor; MeshIndex* fVie = r->faces;
    const int vsVie = r->vertexSize, fsVie = r->facesSize;
    if (!FillFromBytes(r, (const unsigned char*)bytes.data(), bytes.size())) {
        r->vertex = vVie; r->normals = nVie; r->uv = uVie; r->vertexColor = cVie; r->faces = fVie;
        r->vertexSize = vsVie; r->facesSize = fsVie;
        return false;
    }
    if (mats && mats->size() == r->parts.size()) r->materials = *mats;
    // (una malla onlyGPU tenia los arrays en NULL: ahora tiene los nuevos, como cualquier otra)
    r->onlyGPU = false; r->arraysHas = 0; r->layersUVHas.clear(); r->releasedBytes = 0;
    r->bytes = bytes;
    r->modified = true;
    r->textBytes = false;
    r->version++;
    g_w3dRoutinesGen++;
    DeleteVBO(r);   // se vuelven a subir con los arrays nuevos (en el proximo dibujo)
    ThrowTopo(r);   // las aristas/caras compartidas eran de la malla vieja
    // TODOS los usuarios (tambien los detachados por el undo) pasan a los arrays nuevos
    std::vector<Mesh*> us = r->users;
    for (size_t i = 0; i < us.size(); i++) {
        Mesh* u = us[i];
        if (!u || IsExcept(exceptions, u)) continue;
        const bool posed = u->posedByAnim;   // la pose de la sesion vuelve enseguida (RePose)
        // lo PROPIO de la malla (una capa copiada por una vertex anim, el flipbook...) es del
        // contenido viejo: se libera; lo compartido apuntaba a los arrays viejos
        if (!(u->dataComp & W3DMD_POS))   delete[] u->vertex;
        if (!(u->dataComp & W3DMD_NOR))   delete[] u->normals;
        if (!(u->dataComp & W3DMD_UV))    delete[] u->uv;
        if (!(u->dataComp & W3DMD_COL))   delete[] u->vertexColor;
        if (!(u->dataComp & W3DMD_FACES)) delete[] u->faces;
        u->vertex = 0; u->normals = 0; u->uv = 0; u->vertexColor = 0; u->faces = 0;
        ApplyToMesh(u, r, false);
        if (posed || u->flipbook || u->HasArm2D()) RePose(u, posed);
    }
    ReleaseExtrasIfExtra(r);
    delete[] vVie; delete[] nVie; delete[] uVie; delete[] cVie; delete[] fVie;
    if (r->rec) r->rec->bytes = r->Bytes();
    return true;
}

void W3dMeshResourceChangeMaterial(MeshResource* r, int k, Material* mat) {
    if (!r || k < 0) return;
    if ((size_t)k >= r->materials.size()) {
        if ((size_t)k >= r->parts.size()) return;
        r->materials.resize(r->parts.size(), (Material*)0);
    }
    r->materials[(size_t)k] = mat;
    for (size_t i = 0; i < r->users.size(); i++) {
        Mesh* u = r->users[i];
        if (u && (size_t)k < u->materialsGroup.size()) u->materialsGroup[(size_t)k].material = mat;
    }
}

// ============================================================================
//  VBO COMPARTIDOS
// ============================================================================
static void AnnotateDroppable(MeshResource* r);
bool W3dMeshResourceEnsureVBO(MeshResource* r) {
    if (!r || !gfx::VBOSupported() || (!r->vertex && !r->onlyGPU) || r->vertexSize <= 0) return false;
    if (r->vboPos && r->vboShow == r->version) { AnnotateDroppable(r); return true; }
    // (el driver ya rechazo los VBO de esta version: va desde RAM, sin volver a probar cada cuadro)
    if (r->rejectedVbo == r->version + 1) return false;
    // una malla onlyGPU con el VBO perdido (contexto GL nuevo): sus arrays de vuelta para subirlos
    if (!r->vertex && !W3dMeshResourceEnsureRAM(r)) return false;
    const int nv = r->vertexSize;
    gfx::ErrorGL();   // (vaciar lo que hubiera)
    if (!r->vboPos) r->vboPos = gfx::GenBuffer();
    gfx::ArrayBufferData(r->vboPos, r->vertex, nv * 3 * (int)sizeof(GLfloat));
    if (r->normals)     { if (!r->vboNor) r->vboNor = gfx::GenBuffer(); gfx::ArrayBufferData(r->vboNor, r->normals, nv * 3 * (int)sizeof(GLbyte)); }
    if (r->vertexColor) { if (!r->vboCol) r->vboCol = gfx::GenBuffer(); gfx::ArrayBufferData(r->vboCol, r->vertexColor, nv * 4 * (int)sizeof(GLubyte)); }
    if (r->uv)          { if (!r->vboUV)  r->vboUV  = gfx::GenBuffer(); gfx::ArrayBufferData(r->vboUV,  r->uv, nv * 2 * (int)sizeof(GLfloat)); }
    // las capas UV extra (el puntero de UV de una rutina puede apuntar a una: Routine.cpp, Puntero)
    if (r->vboUVx.size() < r->uvExtra.size()) r->vboUVx.resize(r->uvExtra.size(), 0u);
    for (size_t k = 1; k < r->uvExtra.size(); k++) {
        if (r->uvExtra[k].size() < (size_t)nv * 2) continue;
        if (!r->vboUVx[k]) r->vboUVx[k] = gfx::GenBuffer();
        gfx::ArrayBufferData(r->vboUVx[k], &r->uvExtra[k][0], nv * 2 * (int)sizeof(GLfloat));
    }
    if (r->faces && r->facesSize > 0) {
        if (!r->vboIdx) r->vboIdx = gfx::GenBuffer();
        gfx::IndexBufferData(r->vboIdx, r->faces, r->facesSize * (int)sizeof(MeshIndex));
    }
    // un glBufferData que no entra (el MBX del N95 con la memoria de VBO llena) falla EN SILENCIO: sin preguntar, la
    // malla soltaba su RAM (onlyGPU) y el dibujo leia un VBO vacio (el driver se caia: KERN-EXEC 3). Queda en RAM,
    // mas lenta pero se ve, y no la suelta
    unsigned err = gfx::ErrorGL();
#ifndef W3D_SYMBIAN
    {   // (pruebas en PC: W3D_VBO_LIMIT_KB simula un driver que, pasado ese total, rechaza lo que se sube)
        static const char* limit = getenv("W3D_VBO_LIMIT_KB");
        static long uploaded = 0;
        const long bytes = (long)nv * (3 * 4 + (r->normals ? 3 : 0) + (r->vertexColor ? 4 : 0) + (r->uv ? 8 : 0)) +
                           (long)r->facesSize * (long)sizeof(MeshIndex);
        if (limit && limit[0] && err == 0) {
            if (uploaded + bytes > atol(limit) * 1024L) err = 0x0505;   // GL_OUT_OF_MEMORY
            else uploaded += bytes;
        }
    }
#endif
    if (err != 0) {
        w3dLogfW("[mallas] '%s' (%d vertices, %d indices): el driver no acepto sus VBO (glError 0x%x) -> se dibuja desde RAM",
                 r->name.c_str(), nv, r->facesSize, err);
        DeleteVBO(r);
        r->rejectedVbo = r->version + 1;
        r->ramFixed = true;
        return false;
    }
    r->vboShow = r->version;
    AnnotateDroppable(r);
    return true;
}

// ============================================================================
//  SOLO EN GPU (ver el .h)
// ============================================================================
static bool gOnlyGPU = false;
static std::vector<MeshResource*> gReleasePend;   // anotadas por AsegurarVBO; se sueltan al cerrar la corrida
static int gOnlyGPUReloads = 0;

static void RemovePendingOnlyGPU(MeshResource* r) {
    if (!r || !r->releasePend) return;
    r->releasePend = false;
    for (size_t i = 0; i < gReleasePend.size(); i++)
        if (gReleasePend[i] == r) { gReleasePend.erase(gReleasePend.begin() + (long)i); return; }
}
// la malla puede soltar su RAM al cerrar la corrida? (lo barato primero: es por cada AsegurarVBO)
static void AnnotateDroppable(MeshResource* r) {
    if (!gOnlyGPU || r->onlyGPU || r->releasePend || r->ramFixed || r->refsRoutine <= 0 || !r->users.empty()) return;
    r->releasePend = true;
    gReleasePend.push_back(r);
}

void W3dMeshesOnlyGPUActivate(bool on) {
    if (gOnlyGPU == on) return;
    gOnlyGPU = on;
    if (on) return;   // (las que puedan se sueltan en la proxima corrida de rutinas, al asegurar su VBO)
    // apagada: todo como siempre (las sueltas vuelven a RAM; las anotadas ya no se sueltan)
    for (size_t i = 0; i < gReleasePend.size(); i++) gReleasePend[i]->releasePend = false;
    gReleasePend.clear();
    for (int l = 0; l < 2; l++) {
        const std::vector<MeshResource*>& v = l ? gLibMeshes : gRegistry;
        for (size_t i = 0; i < v.size(); i++) if (v[i]->onlyGPU) W3dMeshResourceEnsureRAM(v[i]);
    }
}
bool W3dMeshesOnlyGPUActive() { return gOnlyGPU; }

bool W3dMeshResourceHasArray(const MeshResource* r, unsigned mask) {
    if (!r) return false;
    if (r->onlyGPU) return (r->arraysHas & mask) == mask;
    return (!(mask & W3DMD_POS) || r->vertex) && (!(mask & W3DMD_NOR) || r->normals) &&
           (!(mask & W3DMD_UV) || r->uv) && (!(mask & W3DMD_COL) || r->vertexColor) &&
           (!(mask & W3DMD_FACES) || r->faces);
}
bool W3dMeshResourceHasLayerUV(const MeshResource* r, int layer) {
    if (!r || layer <= 0) return false;
    if (r->onlyGPU) return layer < (int)r->layersUVHas.size() && r->layersUVHas[(size_t)layer] != 0;
    return layer < (int)r->uvExtra.size() && r->uvExtra[(size_t)layer].size() >= (size_t)r->vertexSize * 2;
}

bool W3dMeshResourceReleaseRAM(MeshResource* r) {
    if (!r || !gOnlyGPU || r->onlyGPU || r->ramFixed || !r->rec || r->refsRoutine <= 0 || !r->users.empty()) return false;
    const size_t nv = r->vertexSize > 0 ? (size_t)r->vertexSize : 0;
    if (!r->vertex || !nv || !r->vboPos || r->vboShow != r->version || !gfx::VBOSupported()) return false;
    // TODO lo que tiene tiene que estar en un VBO (un GenBuffer que fallo: se queda en RAM). Una capa UV que no
    // llega a una UV por vertice no se sube (el puntero la tomaria de RAM): con una asi, tambien se queda
    if ((r->normals && !r->vboNor) || (r->vertexColor && !r->vboCol) || (r->uv && !r->vboUV) ||
        (r->faces && r->facesSize > 0 && !r->vboIdx)) return false;
    for (size_t k = 1; k < r->uvExtra.size(); k++)
        if (!r->uvExtra[k].empty() && (k >= r->vboUVx.size() || !r->vboUVx[k] || r->uvExtra[k].size() < nv * 2)) return false;
    // lo que despues se leeria de RAM: la caja y el indice maximo de cada parte y de toda la malla (el rango de
    // cada parte, vertexSize y facesSize se quedan en el recurso)
    W3dMeshFlowsPrecompute(r);
    unsigned has = W3DMD_POS;
    if (r->normals)     has |= W3DMD_NOR;
    if (r->uv)          has |= W3DMD_UV;
    if (r->vertexColor) has |= W3DMD_COL;
    if (r->faces)       has |= W3DMD_FACES;
    r->arraysHas = (unsigned char)has;
    r->layersUVHas.assign(r->uvExtra.size(), (unsigned char)0);
    long b = r->Bytes();
    for (size_t k = 0; k < r->uvExtra.size(); k++) {
        b += (long)(r->uvExtra[k].size() * sizeof(GLfloat));
        if (k > 0 && r->uvExtra[k].size() >= nv * 2) r->layersUVHas[k] = 1;
    }
    if (r->extrasInResource) {
        b += (long)(r->ctrl.size() * sizeof(int) + r->uvRest.size() * sizeof(float));
        for (size_t g = 0; g < r->groups.size(); g++)
            b += (long)(r->groups[g].idx.size() * sizeof(int) + r->groups[g].weights.size() * sizeof(float));
        for (size_t g = 0; g < r->uvGroups.size(); g++)
            b += (long)(r->uvGroups[g].idx.size() * sizeof(int) + r->uvGroups[g].weights.size() * sizeof(float));
    }
    if (r->textBytes) b += (long)r->bytes.size();
    // los arrays se van; vertexSize y facesSize se quedan (son lo que dibujan los VBO)
    delete[] r->vertex; delete[] r->normals; delete[] r->uv; delete[] r->vertexColor; delete[] r->faces;
    r->vertex = 0; r->normals = 0; r->uv = 0; r->vertexColor = 0; r->faces = 0;
    std::vector< std::vector<GLfloat> >().swap(r->uvExtra);
    ReleaseExtras(r);   // (sin usuarios: el proximo que se vincule los relee con la malla)
    ThrowTopo(r);
    // la conversion de una entrada de TEXTO es cache (se rehace al releer); los bytes EDITADOS son la malla: se quedan
    if (r->textBytes) { std::string().swap(r->bytes); r->textBytes = false; }
    r->onlyGPU = true;
    r->releasedBytes = b;
    // (el cache de cajas y flujos sigue valiendo: mismo contenido; prueba_mallas_solo_gpu.w3s: mismo cuadro y llamadas GL)
    W3dMeshFlowsRepoint(r);
    r->rec->bytes = r->Bytes();
    return true;
}

void W3dMeshesOnlyGPUReleasePending() {
    if (gReleasePend.empty()) return;
    std::vector<MeshResource*> v;
    v.swap(gReleasePend);
    for (size_t i = 0; i < v.size(); i++) {
        v[i]->releasePend = false;
        W3dMeshResourceReleaseRAM(v[i]);
    }
}

bool W3dMeshResourceEnsureRAM(MeshResource* r) {
    if (!r) return false;
    if (!r->onlyGPU) return true;
    const int nv = r->vertexSize, ni = r->facesSize;
    std::string b;
    if (!W3dMeshResourceReadBytes(r, b) || b.empty() ||
        !FillFromBytes(r, (const unsigned char*)b.data(), b.size())) {
        w3dLogfE("[mallas] '%s' estaba solo en la GPU y no pude releerla de sus bytes", r->name.c_str());
        return false;
    }
    r->onlyGPU = false; r->arraysHas = 0; r->layersUVHas.clear(); r->releasedBytes = 0;
    // (los bytes son los mismos con los que se subio el VBO; si no midieran lo mismo, se vuelve a subir)
    if (r->vertexSize != nv || r->facesSize != ni) r->vboShow = 0;
    if (r->rec) r->rec->bytes = r->Bytes();
    W3dMeshFlowsRepoint(r);
    gOnlyGPUReloads++;
    w3dLogf("[mallas] '%s' vuelve a RAM (estaba solo en la GPU)", r->name.c_str());
    return true;
}

void W3dMeshesVBOLost() {
    for (int l = 0; l < 2; l++) {
        const std::vector<MeshResource*>& v = l ? gLibMeshes : gRegistry;
        for (size_t i = 0; i < v.size(); i++) {
            MeshResource* r = v[i];
            // (sin glDelete: esos nombres eran del contexto que ya no existe)
            r->vboPos = r->vboNor = r->vboCol = r->vboUV = r->vboIdx = 0;
            r->vboUVx.clear();
            r->vboShow = 0;
            W3dMeshFlowsGLLost(r);
        }
    }
}

int W3dMeshesOnlyGPUCount(long* savedBytes) {
    int n = 0; long b = 0;
    for (int l = 0; l < 2; l++) {
        const std::vector<MeshResource*>& v = l ? gLibMeshes : gRegistry;
        for (size_t i = 0; i < v.size(); i++) if (v[i]->onlyGPU) { n++; b += v[i]->releasedBytes; }
    }
    if (savedBytes) *savedBytes = b;
    return n;
}
int W3dMeshesOnlyGPUReloads() { return gOnlyGPUReloads; }

// ============================================================================
//  MALLAS EDITADAS
// ============================================================================
void W3dMeshMarkEdited(Mesh* m) {
    if (!m || !m->mesh) return;
    for (size_t i = 0; i < gEdited.size(); i++) if (gEdited[i] == m) return;
    gEdited.push_back(m);
}
void W3dMeshesTakeEdited(std::vector<Mesh*>& out) {
    out.clear();
    out.swap(gEdited);
}

// ============================================================================
//  MEDICION
// ============================================================================
int W3dMeshesResourcesLoaded() {
    int n = 0;
    for (size_t i = 0; i < gRegistry.size(); i++) if (gRegistry[i]->rec) n++;
    return n;
}
int W3dMeshesVBOShared() {
    int n = 0;
    for (size_t i = 0; i < gRegistry.size(); i++) if (gRegistry[i]->vboPos) n++;
    return n;
}
long W3dMeshesVBOBytes() {
    long b = 0;
    for (size_t i = 0; i < gRegistry.size(); i++) {
        const MeshResource* r = gRegistry[i];
        if (!r->vboPos) continue;
        const long nv = (long)r->vertexSize;
        b += nv * 3L * (long)sizeof(GLfloat);
        if (r->vboNor) b += nv * 3L;
        if (r->vboCol) b += nv * 4L;
        if (r->vboUV)  b += nv * 2L * (long)sizeof(GLfloat);
        for (size_t k = 0; k < r->vboUVx.size(); k++) if (r->vboUVx[k]) b += nv * 2L * (long)sizeof(GLfloat);
        if (r->vboIdx) b += (long)r->facesSize * (long)sizeof(MeshIndex);
    }
    return b;
}
