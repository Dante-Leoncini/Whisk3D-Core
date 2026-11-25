#include "Mesh.h"
#include "animation/VertexAnimation.h" // ~Mesh: liberar el controlador de vertex anim (o queda colgando)
#include "animation/Armature2DAnimation.h" // ~Mesh: liberar los clips del armature 2D
#include "objects/Armature.h" // skinArmature->poseSerial (gate del VBO skinneado por pose, no por frame)
#include "w3dGraphics.h"    // abstraccion de graficos del engine (sin GL)
#include "W3dNames.h"       // LA regla de nombres unicos (compartida por todo el editor)
#include "CameraBase.h"     // g_renderCamPos (camara del render, para el chrome equirect)
#include "RenderColors.h"   // paleta de render del CORE (sin depender de la UI)
#include "MeshResource.h"   // la malla 3D como recurso: vinculo, VBO compartidos
#include "objects/Light.h"  // Lights: la luz que sombrea el toon (ToonTowardsLight)
#include "WrittenShader.h"   // el shader escrito de un material (Material::shaderOn)
#include "ShaderCPU.h"       // su @vertex corrido en la CPU (backends sin shaders: GLES 1.1, GL fijo)
#include <math.h> // C puro: compila en RVCT y PC por igual
#include <algorithm> // sort/unique: dedup de aristas del material con `lineas`
#include <map>    // Armature2DApply: peso por control-point de cada hueso 2D
#include <string.h> // memcmp: comparar los BYTES de una posicion (ReanchorMarksPos)
#include <stdio.h>  // aviso de COW (UninstantiateData) en consola

// NOTA IMPORTANTE: mucho de este codigo NO va aca y se va a borrar/simplificar.....
// es que hay codigo que va en el EDITOR 3d de Whisk3D y que no deberia estar en el codigo base del "core".
// esta parte necesita una reescritura... pero necesito pensarla mejor

// ===================================================
// Constructor
// ===================================================
Mesh::Mesh(Object* parent, Vector3 pos)
    : Object(parent, "Mesh", pos),
      vertexSize(0), vertex(NULL), vertexColor(NULL), normals(NULL),
      uv(NULL), facesSize(0), faces(NULL)
{
    //MUCHAS de estas definiciones se van a borrar. ya que NO son la base y son cosas mas relacionadas al editor 3d....
    //ejemplo: "edit" o "activeModifier" etc... eso es del Editor de Whisk3D

    layerUnit1 = -1;
    skinArmature = NULL; skinVertex = NULL; skinNormals = NULL; skinVertexCap = 0; skinWithLight = true; lastSkinFrame = -999999; skinPoseSerial = 0; lastSkinBordersFrame = -999999; skinBordersPoseSerial = 0; nextSkinFlat = 0; skinNCtrl = 0; skinGeomVersion = 1; bordersGeomVersion = 0; vtxAnimRecipeBase = 0; // skinning apagado por defecto
    skinAabbMin = Vector3(0,0,0); skinAabbMax = Vector3(0,0,0); skinAabbOk = false; consecutiveSkinCull = 0; // autocull
    skinEpsPos = 0.0f; skinEpsRot = 0.0f; skinIncOk = false; skinPrevWithLight = false; // skinning incremental
    shapeSerial = 0; skinShapeSerial = 0xFFFFFFFFu;   // shape keys: ninguna forma, nada calculado
    meshType = -1;       // no es una primitiva regenerable por defecto
    indirectLightSig = 0;   // (la luz indirecta por vertice: nada calculado)
    meshSize = 2.0f;
    meshSize2 = 0.0f;
    meshDepth = 2.0f;
    meshVerts = 8;
    meshVerts2 = 8;
    meshSmooth = false; // flat por defecto (cada cara su normal)
    moveOpenVerts = 0;     // ningun guard W3dMoveVerts abierto (ver PruneMarksNoEdge)
    posedByAnim   = false; // nace EN REPOSO: solo el playback la posa (ver PosedByVertexAnim)
    overlayLcache = -1.0f; // los buffers de normales se calculan al primer uso
    groupedVerts = 0;
    centerGeom = Vector3(0, 0, 0);
    radiusGeom = 0.0f;
    aabbMin = Vector3(0, 0, 0); aabbMax = Vector3(0, 0, 0); aabbOk = false; // sin AABB hasta ComputeBorders
    edit = NULL; // la malla de edicion se crea on-demand al entrar a Edit Mode
    uvActiveMap = -1; activeColor = -1; activeGroup = -1; uvActiveGroup = -1; // sin capas hasta PopulateLayers
    armature2dActive = -1; // armatures 2D del mesh: la lista arranca vacia
    flipbook = NULL; flipbookOwn = true; flipApplied = -1; flipGeomVer = 0; // sin flipbook (SetUVAnimStrip lo crea)
    last2dFrame = -999999; last2dAnim = -999; pose2dDirty = false; // cache de evaluacion de los clips 2D
    geoVersion = 1;        // la geometria editable arranca en la version 1 (0 = "nunca vista")
    weightPaintOn = false; // se prende en modo Weight Paint
    vertexPaintOn = false; // idem en modo Vertex Paint (dibuja el vertex color sin luz)
    activeModifier = -1; // stack de modificadores vacio (lo gestiona el editor)
    genVertex = NULL; genNormals = NULL; genUV = NULL; genColor = NULL; genFaces = NULL;
    genVertexSize = 0; genFacesSize = 0; genValid = false; // sin malla generada hasta que haya modificadores
    genVersion = 0; vboGenPos = vboGenNor = vboGenCol = vboGenUV = vboGenIdx = 0; vboGenVer = 0;
    chromeExpPos = NULL; chromeExpUV = NULL; chromeExpCount = 0; chromeUVValid = false; chromeCacheEq = true; // reflejo (lazy)
    genChromeExpPos = NULL; genChromeExpUV = NULL; genChromeCount = 0; genChromeValid = false; // reflejo de la malla generada (lazy)
    tangents = NULL; nmColors = NULL; tangentsValid = false; // normal mapping (lazy)
    vboPos = vboNor = vboCol = vboUV = vboIdx = 0; vboGeomVer = 0; vboSkinFrame = -999999; vboSkinFramePrev = -999999; vboPoseSerial = 0; vboPoseSerialPrev = 0; vboVertN = 0; vboIdxN = 0; vboIdxGeomVer = 0; vboRenderActive = false; vboPoseSkinned = false; rawVBO = false; // VBOs (lazy)
    pvsFaces = NULL; pvsFacesSize = 0; pvsVersion = 0; vboPvsVer = 0; pvsSorted = false; // sin override PVS (malla completa; vboPvsVer retirado por P3, ver Mesh.h)
    inStaticBatch = 0; // (P4) sin sello: la malla se dibuja sola
    uvOffsetU = uvOffsetV = 0.0f; // desplazarUV: sin base capturada (uvOffsetBase vacia)
    noEditable = false; onlyRoutine = false; lockNormals = false; posePending = false; drawnPass = 0; inkNoEditable = false; // malla editable (el import con noEditable / el boton de la card Mesh lo prenden)
    tint[0] = tint[1] = tint[2] = 1.0f;
    dataRec = NULL; dataComp = 0; // geometria propia hasta que el importador la comparta (MeshData.h)
    mesh = NULL; sharedVbo = 0; vboUVInUse = 0; // sin recurso (MeshResource.h) hasta que el lector la vincule
    dedupByContent = false; // una malla NUEVA no se junta con las iguales al guardar (ver Mesh.h)
}

// libera el override de indices del PVS (vuelve a dibujar la malla completa).
// pvsVersion sube para que el fast-path VBO re-suba el IBO completo.
void Mesh::PvsClear() {
    if (!pvsFaces && pvsGroups.empty()) return;
    delete[] pvsFaces; pvsFaces = NULL; pvsFacesSize = 0;
    pvsGroups.clear();
    pvsRuns.clear();
    pvsSorted = false;   // sin override no hay lista ordenada que respetar
    pvsVersion++;
}

// ===================================================
// Destructor
// ===================================================
Mesh::~Mesh() {
    // GEOMETRIA COMPARTIDA (MeshData.h): soltar la referencia ANTES que nada.
    // Deja en NULL las capas que no eran nuestras; el dato compartido muere
    // solo cuando su refcount llega a 0 (almacen de recursos).
    ReleaseDataShared();
    // vertex anims: sacar el controlador de reproduccion (o queda COLGANDO en
    // VertexAnimationActives -> UpdateAnimations crashea) y liberar las anims que
    // posee esta malla (sus frames se van con ~VertexAnimation). El borrado por la
    // lista (Properties) ya las saca del vector, asi que aca solo quedan las vivas.
    VertexAnimFreeController(this);
    for (size_t i = 0; i < animations.size(); i++) delete animations[i];
    animations.clear();
    // ARMATURES 2D: los posee la malla (y cada uno posee SUS clips, ver ~Armature2D)
    for (size_t i = 0; i < armatures2d.size(); i++) delete armatures2d[i];
    armatures2d.clear(); armature2dActive = -1;
    RemoveUVAnimStrip();   // saca la malla del registro de UpdateUVAnims (o quedaria colgando)
    FreeLayers();
    // UV GROUPS (pesos por corner del editor UV): los posee la malla. FreeLayers ya los
    // libera en el EDITOR, pero en el RUNTIME del juego esa funcion es un stub vacio -> el
    // borrado va aca (el vector ya vino vaciado si FreeLayers hizo su trabajo).
    for (size_t i = 0; i < uvGroups.size(); i++) delete uvGroups[i];
    uvGroups.clear(); uvActiveGroup = -1;
    InvalidateEdit();
    FreeModifiers(); // definida en el editor (como InvalidateEdit): libera el stack de modificadores
    FreeMeshModified(); // libera las gen buffers
    delete[] skinVertex; delete[] skinNormals; // buffers de skinning (deform por esqueleto)
    delete[] pvsFaces; pvsFaces = NULL; // override de indices del PVS (modificador Culling)
    // VBOs de render (buffer objects en GPU)
    namespace gfx = w3dEngine;
    gfx::DeleteBuffer(vboPos); gfx::DeleteBuffer(vboNor); gfx::DeleteBuffer(vboCol); gfx::DeleteBuffer(vboUV); gfx::DeleteBuffer(vboIdx);
    gfx::DeleteBuffer(vboGenPos); gfx::DeleteBuffer(vboGenNor); gfx::DeleteBuffer(vboGenCol); gfx::DeleteBuffer(vboGenUV);
    gfx::DeleteBuffer(vboGenIdx);
    // LA GEOMETRIA DE RENDER (y sus caches perezosas). El FreeMemory de arriba estaba
    // comentado y nada mas la liberaba: cada malla borrada fugaba vertex/normals/uv/color/
    // faces enteros. Cerrar y reabrir el mismo proyecto sumaba toda su geometria al heap
    // cada vez (1000 arboles iguales: +27 MB por apertura, medido con cargabench leakKB).
    // Va AL FINAL: lo de arriba (anims, capas, edit, modificadores) todavia puede mirarla.
    // Lo COMPARTIDO ya no esta: ReleaseDataShared (primera linea) dejo en NULL las
    // capas del almacen, asi que lo que queda aca es todo propio.
    delete[] vertex;      vertex = NULL;
    delete[] normals;     normals = NULL;
    delete[] uv;          uv = NULL;
    delete[] vertexColor; vertexColor = NULL;
    delete[] faces;       faces = NULL;
    delete[] chromeExpPos; chromeExpPos = NULL;   // reflejo por corner (si se calculo)
    delete[] chromeExpUV;  chromeExpUV = NULL;
    delete[] tangents;     tangents = NULL;       // normal mapping (si se calculo)
    delete[] nmColors;     nmColors = NULL;
}

// ===================================================
// Tipo de objeto
// ===================================================
ObjectType Mesh::getType() {
    return ObjectType::mesh;
}

// ===========================================================================
//  ANIMACION UV de la malla = FLIPBOOK del Core (ver Mesh.h). Registro plano de
//  mallas: UpdateUVAnims tickea el player de cada una Y escribe su uv[] (por eso NO
//  van en el registro global de UpdateFlipbooks, que solo lee uvCurrent sin tocar geometria).
// ===========================================================================
static std::vector<Mesh*> gUVAnimMeshes;

void Mesh::SetUVAnimStrip(int frames, float fps, int axis, int offset, float width, float height) {
    if (frames < 1) frames = 1;
    if (fps < 0.0f) fps = 0.0f;
    if (axis != 1) axis = 0;
    if (offset < 0) offset = 0;
    if (flipbook && !flipbookOwn) RemoveUVAnimStrip();   // suelta el compartido antes de crear el propio
    if (!flipbook) {
        flipbook = new Flipbook(); flipbookOwn = true;
        gUVAnimMeshes.push_back(this);
    }
    // eje u = tira horizontal (cols=frames, filas=1); eje v = vertical (cols=1, filas=frames).
    // ancho/alto (default 1) = fraccion de la textura que ocupa la tira: con la tira
    // metida en el atlas unico el paso por celda es ancho/frames, no 1/frames.
    flipbook->ConfigureStrip("", axis ? 1 : frames, axis ? frames : 1, frames, fps, width, height);
    flipPlay.Set(flipbook, offset % frames, false);   // false: la malla lo tickea (escribe uv[])
    // reconfigurar resetea la aplicacion (la base se recaptura al primer tick)
    flipApplied = -1; flipGeomVer = 0; flipUvBase.clear();
}

// usa un flipbook COMPARTIDO con nombre (SceneFlipbooks): NO lo posee, no lo libera al quitar.
void Mesh::UseFlipbook(Flipbook* fb, int offset) {
    if (!fb) { RemoveUVAnimStrip(); return; }
    if (flipbook && flipbookOwn) delete flipbook;   // soltar el propio anterior si lo habia
    bool reg = false;
    for (size_t i = 0; i < gUVAnimMeshes.size(); i++) if (gUVAnimMeshes[i] == this) { reg = true; break; }
    if (!reg) gUVAnimMeshes.push_back(this);
    flipbook = fb; flipbookOwn = false;
    flipPlay.Set(fb, offset, false);
    flipApplied = -1; flipGeomVer = 0; flipUvBase.clear();
}

void Mesh::RemoveUVAnimStrip() {
    if (!flipbook) return;
    flipPlay.Set(0, 0, false);
    if (flipbookOwn) delete flipbook;   // el compartido vive en SceneFlipbooks: NO se libera aca
    flipbook = NULL; flipbookOwn = true;
    flipUvBase.clear(); flipApplied = -1;
    for (size_t i = 0; i < gUVAnimMeshes.size(); i++)
        if (gUVAnimMeshes[i] == this) { gUVAnimMeshes.erase(gUVAnimMeshes.begin() + i); break; }
}

bool Mesh::TickUVAnimStrip(float dtSeg) {
    if (!flipbook || !uv || vertexSize < 1 || flipbook->frames < 1) return false;
    flipPlay.Tick(dtSeg);                 // avanza el player (no esta en el registro global de UpdateFlipbooks)
    int frame = flipPlay.currentFrame;
    const size_t n = (size_t)vertexSize * 2;
    // offset APLICADO (para el chequeo de 'intacto'): corner0 de la celda ya aplicada
    float apU = 0.0f, apV = 0.0f;
    if (flipApplied >= 0) {
        float u0, v0, u1, v1; flipbook->RectOfCell(flipApplied, u0, v0, u1, v1); apU = u0; apV = v0;
    }
    // (re)captura de la base: primera vez, cambio de topologia, o un rebuild AJENO que regenero
    // uv[]. Una VERTEX ANIM en la misma malla sube skinGeomVersion sin tocar uv[]; recapturar a
    // ciegas arrastraria el offset aplicado (deriva) -> antes se verifica que uv[] siga siendo base+offset.
    if (flipUvBase.size() != n) {
        flipUvBase.assign(uv, uv + n);
        flipApplied = -1;   // uv[] esta en base: nada aplicado
    } else if (flipGeomVer != skinGeomVersion) {
        bool intact = (flipApplied >= 0);
        for (size_t i = 0; i < n && intact; i++) {
            const float esp = flipUvBase[i] + (((i & 1) == 0) ? apU : apV);
            if (uv[i] != esp) intact = false;
        }
        if (!intact) { flipUvBase.assign(uv, uv + n); flipApplied = -1; }
        flipGeomVer = skinGeomVersion;
    }
    if (frame == flipApplied) return false;
    UninstantiateData(W3DMD_UV);         // COW: escribimos uv[] (geometria compartida)
    const float offU = flipPlay.uvCurrent[0], offV = flipPlay.uvCurrent[1];   // corner0 de la celda actual
    for (size_t i = 0; i < n; i += 2) {
        uv[i]     = flipUvBase[i]     + offU;
        uv[i + 1] = flipUvBase[i + 1] + offV;   // offV = 0 en una tira horizontal (no toca v)
    }
    flipApplied = frame;
    skinGeomVersion++;                    // re-subir el VBO (mismo camino que EvalVertexAnim)
    flipGeomVer = skinGeomVersion;        // nuestra propia subida no invalida la base
    return true;
}

void Mesh::RestUVAnimStrip() {
    if (!flipbook || !uv || flipApplied < 0) return;
    const size_t n = (size_t)vertexSize * 2;
    if (flipUvBase.size() != n) return;   // base invalida: uv[] ya es lo mejor que hay
    for (size_t i = 0; i < n; i++) uv[i] = flipUvBase[i];
    flipApplied = -1;   // el proximo tick reaplica el cuadro actual (y re-sube el VBO)
}

bool UpdateUVAnims(float dtSeg) {
    bool change = false;
    for (size_t i = 0; i < gUVAnimMeshes.size(); i++)
        if (gUVAnimMeshes[i] && gUVAnimMeshes[i]->TickUVAnimStrip(dtSeg)) change = true;
    return change;
}

// hay alguna malla animando sus UV? (lo consulta HasAnimationActive, Materials.cpp)
bool HasUVAnims() {
    return !gUVAnimMeshes.empty();
}

// re-arma bordersBuf (contorno/wireframe) desde vertex[] con las aristas ACTUALES, sin
// recalcular topologia. Lo usa RenderBorders cuando una vertex anim movio vertex[] (el
// contorno estatico quedaria en la pose vieja). Marca bordersGeomVersion = skinGeomVersion.
void Mesh::RefreshBordersFromVertex() {
    bordersGeomVersion = skinGeomVersion;
    // las aristas de la malla, o las COMPARTIDAS de su recurso si su edicion sigue pendiente
    // (una malla de un recurso posada por su vertex anim: su contorno es suyo, las aristas no)
    const std::vector<int>* E = W3dMeshEdgesView(this);
    if (!vertex || !E || E->empty()) return;
    const std::vector<int>& ed = *E;
    const int nv = vertexSize;
    bordersBuf.clear(); bordersBuf.reserve(ed.size()*3);
    for (size_t e = 0; e + 1 < ed.size(); e += 2) {
        int a = ed[e], b = ed[e+1];
        if (a<0||a>=nv||b<0||b>=nv) continue;
        bordersBuf.push_back(vertex[a*3]); bordersBuf.push_back(vertex[a*3+1]); bordersBuf.push_back(vertex[a*3+2]);
        bordersBuf.push_back(vertex[b*3]); bordersBuf.push_back(vertex[b*3+1]); bordersBuf.push_back(vertex[b*3+2]);
    }
}

// ===================================================
// Liberar memoria
// ===================================================
void Mesh::FreeMemory() {
    ReleaseDataShared(); // capas compartidas: quedan NULL y se va la ref (MeshData.h)
    delete[] vertex;
    delete[] vertexColor;
    delete[] normals;
    delete[] uv;

    FreeLayers(); // uv maps / color layers / vertex groups

    delete[] faces;
    materialsGroup.clear();
}

// ===========================================================================
//  GEOMETRIA COMPARTIDA (MeshData.h) — el lado del Mesh, compilado en TODAS
//  las plataformas (este .cpp esta en el .mmp). La liberacion de la referencia
//  pasa por un HOOK que MeshData.cpp setea al registrarse: si nadie comparte
//  (Symbian, o `mallacache off` desde el arranque) el hook queda NULL y este
//  archivo no depende del almacen de recursos.
// ===========================================================================
void (*W3dMeshDataReleaseHook)(void*) = 0;   // lo apunta MeshData.cpp
int W3dUninstantiatedMesh = 0;               // contador COW (lo muestra meminfo)

// ---- AUTOCULL por malla (frustum): el test "este AABB de MUNDO se ve?" lo instala el EDITOR
//      (Culling.cpp -> W3dAabbVisible) via el hook; NULL (un juego compilado sin editor) = sin cull.
//      g_meshAutocull es el interruptor A/B (harness 'autocull'). Ver Mesh::RenderObject.
bool (*g_meshVisibleHook)(const Vector3&, const Vector3&) = NULL;
bool g_meshAutocull = true;

// GRABADOR de visibilidad para preparar demos (harness 'cullcamaras'): acumula los NOMBRES de las
// mallas que pasaron el autocull y se dibujaron a lo largo de una corrida -> lo que las camaras VEN.
// Vive en el Core para que linkee igual en Symbian (ahi nunca se prende). Costo cero con g_cullRecord=false.
#include <set>
bool g_cullRecord = false;
static std::set<std::string> g_cullViews;
void W3dCullViewRegister(const char* n) { if (n && *n) g_cullViews.insert(n); }
void W3dCullViewClear() { g_cullViews.clear(); }
bool W3dCullViewHas(const char* n) { return n && g_cullViews.count(n) > 0; }
int  W3dCullViewCount() { return (int)g_cullViews.size(); }

void Mesh::ReleaseDataShared() {
    if (!dataRec && !mesh) return;
    // los punteros compartidos no son nuestros: NULLearlos para que ningun
    // delete[] posterior (FreeMemory, editor) toque memoria ajena
    if (dataComp & W3DMD_POS)   vertex = NULL;
    if (dataComp & W3DMD_NOR)   normals = NULL;
    if (dataComp & W3DMD_UV)    uv = NULL;
    if (dataComp & W3DMD_COL)   vertexColor = NULL;
    if (dataComp & W3DMD_FACES) faces = NULL;
    dataComp = 0;
    if (dataRec) {
        if (W3dMeshDataReleaseHook) W3dMeshDataReleaseHook(dataRec);
        dataRec = NULL;
    }
    // la malla con nombre (MeshResource.h): sale de sus usuarios y suelta la referencia
    if (mesh) W3dMeshUnlink(this);
}

static std::string KeyUVPersonal(const Material* m, int map) {
    return (m ? m->name : std::string()) + "\x1f" + W3dMatMapKey(map);
}
std::string Mesh::UVCustomized(const Material* m, int map) const {
    if (uvPersonal.empty()) return std::string();
    std::map<std::string, std::string>::const_iterator it = uvPersonal.find(KeyUVPersonal(m, map));
    return it == uvPersonal.end() ? std::string() : it->second;
}
void Mesh::SetUVCustomized(const Material* m, int map, const std::string& uvName) {
    if (uvName.empty()) uvPersonal.erase(KeyUVPersonal(m, map));
    else uvPersonal[KeyUVPersonal(m, map)] = uvName;
}
int Mesh::UVPersonalIndex(const Material* m, int map) const {
    if (uvPersonalIdx.empty()) return -1;
    std::map<std::string, int>::const_iterator it = uvPersonalIdx.find(KeyUVPersonal(m, map));
    return it == uvPersonalIdx.end() ? -1 : it->second;
}
void Mesh::SetUVPersonalIndex(const Material* m, int map, int index) {
    if (index < 0) uvPersonalIdx.erase(KeyUVPersonal(m, map));
    else uvPersonalIdx[KeyUVPersonal(m, map)] = index;
}
const GLfloat* Mesh::UVOfMap(const Material* m, int map) const {
    const std::string mapName = UVCustomized(m, map);
    if (!mapName.empty())
        for (size_t j = 0; j < uvMaps.size(); j++)
            if (uvMaps[j] && uvMaps[j]->name == mapName) { const GLfloat* p = UVOfLayer((int)j); if (p) return p; }
    const int own = UVPersonalIndex(m, map);   // (el indice propio de este objeto)
    if (own > 0) return UVOfLayer(own);
    if (own == 0) return 0;
    if (m && map >= 0 && map < W3D_MAPS && m->uvMap[map] > 0) return UVOfLayer(m->uvMap[map]);
    return 0;
}
const GLfloat* Mesh::UVOfLayer(int i) const {
    if (i <= 0 && uvActiveMap <= 0) return uv;
    const int active = (uvActiveMap >= 0) ? uvActiveMap : 0;
    if (i == active) return uv;
    if (i >= 0 && i < (int)uvExtra.size() && vertexSize > 0 && uvExtra[(size_t)i].size() >= (size_t)vertexSize * 2)
        return &uvExtra[(size_t)i][0];
    return 0;
}

void Mesh::UninstantiateData(unsigned mask, bool copy) {
    if (!dataRec && !mesh) return;
    unsigned affected = (unsigned)dataComp & mask;
    if (!affected) return;
    if (copy) {
        // COW de verdad: copiar la capa a memoria propia y recien ahi escribir
        if ((affected & W3DMD_POS) && vertex) {
            GLfloat* nv = new GLfloat[(size_t)vertexSize * 3];
            memcpy(nv, vertex, (size_t)vertexSize * 3 * sizeof(GLfloat));
            vertex = nv;
        }
        if ((affected & W3DMD_NOR) && normals) {
            GLbyte* nn = new GLbyte[(size_t)vertexSize * 3];
            memcpy(nn, normals, (size_t)vertexSize * 3 * sizeof(GLbyte));
            normals = nn;
        }
        if ((affected & W3DMD_UV) && uv) {
            GLfloat* nu = new GLfloat[(size_t)vertexSize * 2];
            memcpy(nu, uv, (size_t)vertexSize * 2 * sizeof(GLfloat));
            uv = nu;
        }
        if ((affected & W3DMD_COL) && vertexColor) {
            GLubyte* nc = new GLubyte[(size_t)vertexSize * 4];
            memcpy(nc, vertexColor, (size_t)vertexSize * 4 * sizeof(GLubyte));
            vertexColor = nc;
        }
        if ((affected & W3DMD_FACES) && faces) {
            MeshIndex* nf = new MeshIndex[facesSize > 0 ? facesSize : 1];
            memcpy(nf, faces, (size_t)(facesSize > 0 ? facesSize : 0) * sizeof(MeshIndex));
            faces = nf;
        }
    } else {
        // abandono: el caller va a REGENERAR la capa (la habria borrado con
        // delete[]) -> queda NULL, sin copiar nada
        if (affected & W3DMD_POS)   vertex = NULL;
        if (affected & W3DMD_NOR)   normals = NULL;
        if (affected & W3DMD_UV)    uv = NULL;
        if (affected & W3DMD_COL)   vertexColor = NULL;
        if (affected & W3DMD_FACES) faces = NULL;
    }
    dataComp = (unsigned char)(dataComp & ~mask);
    W3dUninstantiatedMesh++;
    // la geometria de un .obj se suelta cuando no queda nada compartido; la de un RECURSO no:
    // el objeto sigue siendo su usuario (las capas propias son solo de esta instancia)
    if (!dataComp && dataRec) {
        if (W3dMeshDataReleaseHook) W3dMeshDataReleaseHook(dataRec);
        dataRec = NULL;
    }
}

// ===================================================
// Renderizado
// ===================================================
// --- NORMAL MAPPING (DOT3) ---------------------------------------------------------------------------------
// Base tangente POR VERTICE de render (T en .xyz, handedness en .w). Acumula por triangulo desde pos+UV,
// ortonormaliza contra la normal y guarda. CACHE: solo recalcula si cambio la geometria/UV (tangentsValid).
void Mesh::ComputeTangents() {
    if (tangentsValid && tangents) return;
    if (!vertex || !uv || !normals || !faces || vertexSize <= 0 || facesSize < 3) return;
    delete[] tangents; tangents = new GLfloat[vertexSize * 4];
    std::vector<Vector3> tanAcc(vertexSize, Vector3(0,0,0));
    std::vector<Vector3> bitAcc(vertexSize, Vector3(0,0,0));
    for (int i = 0; i + 2 < facesSize; i += 3) {
        int a = faces[i], b = faces[i+1], c = faces[i+2];
        Vector3 p0(vertex[a*3], vertex[a*3+1], vertex[a*3+2]);
        Vector3 p1(vertex[b*3], vertex[b*3+1], vertex[b*3+2]);
        Vector3 p2(vertex[c*3], vertex[c*3+1], vertex[c*3+2]);
        float u0=uv[a*2], v0=uv[a*2+1], u1=uv[b*2], v1=uv[b*2+1], u2=uv[c*2], v2=uv[c*2+1];
        Vector3 e1 = p1 - p0, e2 = p2 - p0;
        float du1=u1-u0, dv1=v1-v0, du2=u2-u0, dv2=v2-v0;
        float d = du1*dv2 - du2*dv1;
        if (d > -1e-9f && d < 1e-9f) continue;     // triangulo degenerado en UV
        float r = 1.0f / d;
        Vector3 T  = (e1*dv2 - e2*dv1) * r;
        Vector3 Bt = (e2*du1 - e1*du2) * r;
        tanAcc[a] += T; tanAcc[b] += T; tanAcc[c] += T;
        bitAcc[a] += Bt; bitAcc[b] += Bt; bitAcc[c] += Bt;
    }
    for (int v = 0; v < vertexSize; v++) {
        Vector3 N(normals[v*3]/127.0f, normals[v*3+1]/127.0f, normals[v*3+2]/127.0f); N = N.Normalized();
        Vector3 T = tanAcc[v];
        T = T - N * N.Dot(T);                       // Gram-Schmidt (T perpendicular a N)
        if (T.LengthSq() < 1e-12f) {                // sin UV utiles: cualquier perpendicular a N
            T = Vector3(1,0,0); if (N.Dot(T) > 0.9f || N.Dot(T) < -0.9f) T = Vector3(0,1,0);
            T = T - N * N.Dot(T);
        }
        T = T.Normalized();
        float hand = (Vector3::Cross(N, T).Dot(bitAcc[v]) < 0.0f) ? -1.0f : 1.0f;
        tangents[v*4] = T.x; tangents[v*4+1] = T.y; tangents[v*4+2] = T.z; tangents[v*4+3] = hand;
    }
    tangentsValid = true;
}

// L (vector a la luz) en TANGENT-SPACE por vertice -> nmColors (el "primary color" del DOT3). lightLocal = la luz
// en el espacio LOCAL de la malla. Se llama cada frame (la luz/camara se mueven). [-1,1] -> [0,1]*255.
void Mesh::UpdateNormalMapColors(const Vector3& lightLocal) {
    if (!tangents || !normals || !vertex || vertexSize <= 0) return;
    if (!nmColors) nmColors = new GLubyte[vertexSize * 4];
    for (int v = 0; v < vertexSize; v++) {
        Vector3 N(normals[v*3]/127.0f, normals[v*3+1]/127.0f, normals[v*3+2]/127.0f); N = N.Normalized();
        Vector3 T(tangents[v*4], tangents[v*4+1], tangents[v*4+2]);
        Vector3 B = Vector3::Cross(N, T) * tangents[v*4+3];
        Vector3 P(vertex[v*3], vertex[v*3+1], vertex[v*3+2]);
        Vector3 L = (lightLocal - P).Normalized();
        float lt = L.Dot(T), lb = L.Dot(B), ln = L.Dot(N);
        nmColors[v*4]   = (GLubyte)((lt*0.5f+0.5f)*255.0f);
        nmColors[v*4+1] = (GLubyte)((lb*0.5f+0.5f)*255.0f);
        nmColors[v*4+2] = (GLubyte)((ln*0.5f+0.5f)*255.0f);
        nmColors[v*4+3] = 255;
    }
}

// La direccion HACIA la luz que sombrea el toon, en MUNDO: la primera luz direccional de la escena (su "arriba" local,
// como la manda Light a GL) o, si no hay, la primera luz (desde el punto 'p'). Sin luces: desde arriba y adelante.
static Vector3 ToonTowardsLight(const Vector3& p) {
    Light* picked = NULL;
    for (size_t i = 0; i < Lights.size() && !picked; i++) if (Lights[i] && Lights[i]->directional) picked = Lights[i];
    if (!picked && !Lights.empty()) picked = Lights[0];
    if (!picked) return Vector3(0.0f, 0.8f, 0.6f);
    Matrix4 LW; picked->GetWorldMatrix(LW);
    if (picked->directional) return Vector3(LW.m[4], LW.m[5], LW.m[6]);
    return Vector3(LW.m[12] - p.x, LW.m[13] - p.y, LW.m[14] - p.z);
}

// SOMBREADO TOON: s = 0.5 + 0.5 * (n . l) por vertice, con l la direccion a la luz llevada al espacio de la malla.
// La normal transformada al mundo es W^-T n, y (W^-T n) . L = n . (W^-1 L): alcanza con pasar la LUZ al espacio local
// con la inversa (vale tambien con escala no uniforme). v queda en el medio de la rampa (una fila).
void Mesh::UpdateToonUV(const GLbyte* nor) {
    if (!nor || vertexSize <= 0) { toonUV.clear(); return; }
    Matrix4 W; GetWorldMatrix(W);
    Vector3 l = ToonTowardsLight(Vector3(W.m[12], W.m[13], W.m[14]));
    Matrix4 Wi;
    if (Matrix4::InvertAffine(W, Wi)) {
        Vector3 d(Wi.m[0] * l.x + Wi.m[4] * l.y + Wi.m[8]  * l.z,
                  Wi.m[1] * l.x + Wi.m[5] * l.y + Wi.m[9]  * l.z,
                  Wi.m[2] * l.x + Wi.m[6] * l.y + Wi.m[10] * l.z);
        l = d;
    }
    const float length = sqrtf(l.x * l.x + l.y * l.y + l.z * l.z);
    if (length < 1e-6f) l = Vector3(0.0f, 1.0f, 0.0f);
    else l = l * (0.5f / (length * 127.0f));   // (ya con el 0.5 y la escala de las normales de 8 bits)
    // la FILA de la rampa: la v de la segunda capa UV si la malla la tiene (un atlas de rampas: una malla con varias
    // telas en UN material, cada vertice con la suya); si no, el medio de la rampa
    const GLfloat* rows = UVOfLayer(1);
    toonUV.resize((size_t)vertexSize * 2);
    GLfloat* t = &toonUV[0];
    // EN ENTEROS (fijo 8.24): la normal es de 8 bits, asi que n . l son tres multiplicaciones enteras y el recorte va en
    // enteros; queda UNA conversion a float por vertice. En el ARM11 cada conversion y cada comparacion de floats cuesta
    // (medido en el Galaxy Ace: 0.6 ms por cuadro con Goku y Freezer). |l| <= 0.5/127: n . l entra de sobra en 32 bits
    const float kFixed = 16777216.0f;
    const int lx = (int)(l.x * kFixed), ly = (int)(l.y * kFixed), lz = (int)(l.z * kFixed);
    const int middle = 1 << 23, one = 1 << 24;
    for (int v = 0; v < vertexSize; v++) {
        int s = middle + nor[v*3] * lx + nor[v*3+1] * ly + nor[v*3+2] * lz;
        if (s < 0) s = 0; else if (s > one) s = one;
        t[v*2]   = (float)s * (1.0f / kFixed);
        t[v*2+1] = rows ? rows[v*2+1] : 0.5f;
    }
}

// LAS SONDAS DE REFLEJO (main/objects/ReflectionProbe.h): un material con el reflejo en "La sonda mas cercana" usa la imagen
// 360 de la sonda que le toca a la malla (el gancho la busca). Con la PROYECCION EN CAJA, el reflejo de cada vertice se
// corrige por la caja de la sonda: el rayo reflejado choca contra sus paredes y se mira desde el centro de la captura
Texture* (*g_w3dReflectionProbeHook)(Object* o, float* box, float* center, bool* withBox) = NULL;
// EL ENTORNO DE UNA MALLA PBR (main/render/SceneRender.cpp): la sonda de reflejo que la contiene o el fondo de la escena
void (*g_w3dEnvironmentObjectHook)(Object* o) = NULL;
static bool  gReflBox = false;
static float gReflB[6], gReflC[3];
// === REFLEJO por SOFTWARE, matematica COMPARTIDA (la usan la malla base Y la generada) ===
// UV de UN corner: pos LOCAL + normal LOCAL -> reflejo. equirect=true: lat-long 360 en MUNDO. false: matcap
// (sphere-map en espacio de OJO, replica de GL_SPHERE_MAP). 'polo' marca el corner sobre el polo (equirect).
static void ChromeUVCorner(const Vector3& lp, const Vector3& ln, const Matrix4& W, const Vector3& cam,
                           const Vector3& cr, const Vector3& cu, const Vector3& cf, bool equirect,
                           float& u, float& v, bool& pole) {
    const float PI = 3.14159265358979f;
    Vector3 wp = W * lp;
    Vector3 wn(W.m[0]*ln.x + W.m[4]*ln.y + W.m[8]*ln.z,
               W.m[1]*ln.x + W.m[5]*ln.y + W.m[9]*ln.z,
               W.m[2]*ln.x + W.m[6]*ln.y + W.m[10]*ln.z);
    wn = wn.Normalized();
    pole = false;
    if (equirect) {
        Vector3 I = (wp - cam).Normalized();
        float dd = I.x*wn.x + I.y*wn.y + I.z*wn.z;
        Vector3 R(I.x - 2*dd*wn.x, I.y - 2*dd*wn.y, I.z - 2*dd*wn.z);
        if (gReflBox) {   // (la proyeccion en caja: donde el reflejo choca con la caja, visto desde el centro de la captura)
            float t = 1e30f;
            const float rp[3] = { wp.x, wp.y, wp.z }, rd[3] = { R.x, R.y, R.z };
            for (int k = 0; k < 3; k++) {
                if (fabsf(rd[k]) < 1e-6f) continue;
                const float tk = ((rd[k] > 0.0f ? gReflB[3 + k] : gReflB[k]) - rp[k]) / rd[k];
                if (tk > 0.0f && tk < t) t = tk;
            }
            if (t < 1e29f) {
                Vector3 H(wp.x + R.x * t - gReflC[0], wp.y + R.y * t - gReflC[1], wp.z + R.z * t - gReflC[2]);
                const float l = sqrtf(H.x * H.x + H.y * H.y + H.z * H.z);
                if (l > 1e-6f) R = Vector3(H.x / l, H.y / l, H.z / l);
            }
        }
        float ry = R.y < -1.0f ? -1.0f : (R.y > 1.0f ? 1.0f : R.y);
        u = atan2f(R.z, R.x) / (2.0f*PI) + 0.5f;
        v = acosf(ry) / PI;
        pole = (ry > 0.995f || ry < -0.995f);
    } else {
        Vector3 rel = wp - cam;
        Vector3 ep(rel.x*cr.x + rel.y*cr.y + rel.z*cr.z,
                   rel.x*cu.x + rel.y*cu.y + rel.z*cu.z,
                 -(rel.x*cf.x + rel.y*cf.y + rel.z*cf.z));  // -forward = +Z del eye space
        Vector3 in(wn.x*cr.x + wn.y*cr.y + wn.z*cr.z,
                   wn.x*cu.x + wn.y*cu.y + wn.z*cu.z,
                 -(wn.x*cf.x + wn.y*cf.y + wn.z*cf.z));
        Vector3 I = ep.Normalized();
        float dd = I.x*in.x + I.y*in.y + I.z*in.z;
        Vector3 R(I.x - 2*dd*in.x, I.y - 2*dd*in.y, I.z - 2*dd*in.z);
        float mm = 2.0f * sqrtf(R.x*R.x + R.y*R.y + (R.z+1.0f)*(R.z+1.0f));
        if (mm < 1e-5f) mm = 1e-5f;
        u = R.x / mm + 0.5f;
        v = 1.0f - (R.y / mm + 0.5f); // flip-V para matchear el GL_SPHERE_MAP del PC
    }
}
// COSTURA + POLO del equirect POR TRIANGULO (3 corners): sin esto el atan2 wrap hace "tuc"/batidora.
static void ChromeSeamPole(float u[3], const bool pole[3]) {
    float umin = 2.0f, umax = -1.0f;
    for (int k = 0; k < 3; k++) if (!pole[k]) { if (u[k] < umin) umin = u[k]; if (u[k] > umax) umax = u[k]; }
    if (umax > umin && umax - umin > 0.5f)
        for (int k = 0; k < 3; k++) if (!pole[k] && u[k] < 0.5f) u[k] += 1.0f; // GL_REPEAT -> continuo
    float sum = 0.0f; int n = 0;
    for (int k = 0; k < 3; k++) if (!pole[k]) { sum += u[k]; n++; }
    if (n > 0) { float avg = sum / n; for (int k = 0; k < 3; k++) if (pole[k]) u[k] = avg; } // polo = U promedio
}

// CHROME EQUIRECTANGULAR 360 (calidad, para renders): calcula por SOFTWARE la UV del reflejo de cada
// vertice (ChromeUVCorner + ChromeSeamPole). CACHE: solo recalcula si cambio la camara, la matriz de
// mundo o la geometria -> en una toma estatica cuesta CERO; solo "paga" al orbitar (clave para el N95).
void Mesh::UpdateChromeUV(bool equirect) {
    if (vertexSize <= 0 || !vertex || !normals || facesSize <= 0 || !faces) return;
    Matrix4 W; GetWorldMatrix(W);
    Vector3 cam = g_renderCamPos;
    // base de camara (para el MATCAP, espacio de OJO): right/up/forward en mundo
    Vector3 cr = g_renderCamRight, cu = g_renderCamUp, cf = g_renderCamForward;
    // cache hit: mismo modo + misma camara + misma matriz de mundo + misma cantidad de corners -> nada que hacer
    // (con la proyeccion en caja de una sonda de reflejo no se cachea: la caja y la sonda pueden cambiar)
    if (!gReflBox && chromeUVValid && chromeExpUV && chromeExpCount == facesSize && chromeCacheEq == equirect) {
        bool equal = (chromeCacheCam.x == cam.x && chromeCacheCam.y == cam.y && chromeCacheCam.z == cam.z);
        for (int i = 0; equal && i < 16; i++) if (chromeCacheW[i] != W.m[i]) equal = false;
        if (equal) return;
    }
    if (!chromeExpUV || chromeExpCount != facesSize) {
        delete[] chromeExpPos; chromeExpPos = new GLfloat[facesSize * 3];
        delete[] chromeExpUV;  chromeExpUV  = new GLfloat[facesSize * 2];
        chromeExpCount = facesSize;
    }
    // POR TRIANGULO (3 corners): el EQUIRECT corrige costura/polo por-cara; el MATCAP no las tiene (disco).
    for (int t = 0; t + 2 < facesSize; t += 3) {
        float u[3], v[3]; bool pole[3]; Vector3 lp[3];
        for (int k = 0; k < 3; k++) {
            int vi = faces[t + k];
            lp[k] = Vector3(vertex[vi*3], vertex[vi*3+1], vertex[vi*3+2]);
            Vector3 ln(normals[vi*3]/127.0f, normals[vi*3+1]/127.0f, normals[vi*3+2]/127.0f);
            ChromeUVCorner(lp[k], ln, W, cam, cr, cu, cf, equirect, u[k], v[k], pole[k]);
        }
        if (equirect) ChromeSeamPole(u, pole);
        for (int k = 0; k < 3; k++) {
            int c = t + k;
            chromeExpPos[c*3] = (GLfloat)lp[k].x; chromeExpPos[c*3+1] = (GLfloat)lp[k].y; chromeExpPos[c*3+2] = (GLfloat)lp[k].z;
            chromeExpUV[c*2] = u[k]; chromeExpUV[c*2+1] = v[k];
        }
    }
    chromeCacheCam = cam;
    for (int i = 0; i < 16; i++) chromeCacheW[i] = W.m[i];
    chromeCacheEq = equirect;
    chromeUVValid = true;
}

// CHROME sobre la MALLA GENERADA (screw/subdiv): mismas UV de reflejo que UpdateChromeUV pero leyendo la geo
// GENERADA (genVertex/genNormals/genFaces), y SOLO para los mesh parts con reflejo. Si ningun part tiene chrome NO
// hace NADA (costo 0, no recalcula frame a frame). Cache por camara/mundo -> solo "paga" al orbitar. NO toca la geo 3D.
void Mesh::UpdateChromeUVGen() {
    if (!genValid || !genVertex || !genNormals || !genFaces || genFacesSize <= 0) return;
    // hay algun mesh part con reflejo? (material LIVE de materialsGroup, mismo indice de grupo)
    bool hasChrome = false;
    for (size_t g = 0; g < genMaterialsGroup.size(); g++) {
        Material* mat = (g < materialsGroup.size()) ? materialsGroup[g].material : genMaterialsGroup[g].material;
        if (mat && mat->chrome) { hasChrome = true; break; }
    }
    if (!hasChrome) return; // sin reflejo: no se recalcula nada
    Matrix4 W; GetWorldMatrix(W);
    Vector3 cam = g_renderCamPos;
    Vector3 cr = g_renderCamRight, cu = g_renderCamUp, cf = g_renderCamForward;
    // cache hit: misma camara + mismo mundo + misma cantidad de corners -> nada que hacer
    if (genChromeValid && genChromeExpUV && genChromeCount == genFacesSize) {
        bool equal = (genChromeCam.x == cam.x && genChromeCam.y == cam.y && genChromeCam.z == cam.z);
        for (int i = 0; equal && i < 16; i++) if (genChromeW[i] != W.m[i]) equal = false;
        if (equal) return;
    }
    if (!genChromeExpUV || genChromeCount != genFacesSize) {
        delete[] genChromeExpPos; genChromeExpPos = new GLfloat[genFacesSize * 3];
        delete[] genChromeExpUV;  genChromeExpUV  = new GLfloat[genFacesSize * 2];
        genChromeCount = genFacesSize;
    }
    // por cada GRUPO con chrome, computa las UV de reflejo de SUS corners (los demas quedan sin tocar).
    for (size_t g = 0; g < genMaterialsGroup.size(); g++) {
        Material* mat = (g < materialsGroup.size()) ? materialsGroup[g].material : genMaterialsGroup[g].material;
        if (!mat || !mat->chrome) continue;
        bool equirect = (mat->reflectMode == 2);
        int s = genMaterialsGroup[g].startDrawn, cnt = genMaterialsGroup[g].indicesDrawnCount;
        for (int t = s; t + 2 < s + cnt; t += 3) {
            float u[3], v[3]; bool pole[3]; Vector3 lp[3];
            for (int k = 0; k < 3; k++) {
                int vi = genFaces[t + k];
                lp[k] = Vector3(genVertex[vi*3], genVertex[vi*3+1], genVertex[vi*3+2]);
                Vector3 ln(genNormals[vi*3]/127.0f, genNormals[vi*3+1]/127.0f, genNormals[vi*3+2]/127.0f);
                ChromeUVCorner(lp[k], ln, W, cam, cr, cu, cf, equirect, u[k], v[k], pole[k]);
            }
            if (equirect) ChromeSeamPole(u, pole);
            for (int k = 0; k < 3; k++) {
                int c = t + k;
                genChromeExpPos[c*3] = (GLfloat)lp[k].x; genChromeExpPos[c*3+1] = (GLfloat)lp[k].y; genChromeExpPos[c*3+2] = (GLfloat)lp[k].z;
                genChromeExpUV[c*2] = u[k]; genChromeExpUV[c*2+1] = v[k];
            }
        }
    }
    genChromeCam = cam;
    for (int i = 0; i < 16; i++) genChromeW[i] = W.m[i];
    genChromeValid = true;
}

// un material FONDO dejo el DepthRange en 1..1: el epilogo de RenderObject lo devuelve
// a 0..1 (flag para no pagar un glDepthRange por malla cuando no hay cielo de por medio)
static bool gBackgroundActive = false;

// ===================================================
// aplica TODO el estado GL de un material, leyendolo DEL material (nada
// hardcodeado). RenderObject la llama solo cuando el material cambia.
// la malla que se esta dibujando hace la pasada DOT3 del normal map (la tiene solo el camino sin modificadores). Si no,
// un material con normal map se dibuja con la luz de siempre: antes la base iba SIN luz esperando una pasada que con un
// Subdivision no existia y la malla quedaba BLANCA
static bool gNormalMapWithPass = true;   // (la apaga el loop de grupos de RenderObject mientras dibuja una malla generada)
static bool gNeedsUV = false;            // (el material tiene mapas con textura en el shader: el array de UV va prendido)
// LOS MAPAS DEL MATERIAL en el shader (normal, rugosidad, metalico, alfa, emision): los ids de sus texturas (0 = el numero).
// El normal map va por el shader solo con la luz por pixel (ahi esta la normal de cada pixel); si no, por la pasada DOT3
static unsigned TexMap(const Material* m, int k) {
    if (!m || k < W3D_MAP_EMISSION || k >= W3D_MAPS || !m->mapWithTexture[k] || !m->mapTex[k]) return 0;
    return m->mapTex[k]->iID;
}
// LA LUZ POR PIXEL del material: la que eligio, o forzada si tiene un mapa que cambia algo en cada pixel (normal,
// rugosidad, metalico, emision con textura): con la luz por vertice esos mapas no tienen donde aplicarse
static bool gShadowObject = false;   // LAS SOMBRAS EN TIEMPO REAL: la malla que se dibuja las recibe (ShadowGuard, mas abajo)
bool W3dSmallGPU() {
#if defined(W3D_ANDROID_OLD) || defined(W3D_SYMBIAN)
    return true;
#else
    return w3dEngine::GpuLimited();
#endif
}
// lo que con la luz por vertice se veria distinto: el brillo de un metal o de algo pulido (la mancha del reflejo cambia
// de forma), el vidrio. Lo mate (rugoso, sin metal) se ve igual: su luz difusa casi no cambia dentro de un triangulo
static bool PixelVisible(const Material* m) {
    return m->rtMetallic > 0.05f || m->rtRoughness < 0.5f || m->transmission > 0.0f || m->chrome;
}
static bool PerPixelEf(const Material* m) {
    if (!m) return false;
    // (en una GPU chica la luz por pixel de lo mate es la mitad del cuadro en la VideoCore IV y no se nota)
    if (m->perPixel) return !W3dSmallGPU() || PixelVisible(m);
    if (m->normalMap && m->normalTexture && m->normalTexture->iID) return true;
    return TexMap(m, W3D_MAP_ROUGHNESS) || TexMap(m, W3D_MAP_METALLIC) || TexMap(m, W3D_MAP_EMISSION);
}
// el material va con el normal map EN EL SHADER (y no con la pasada DOT3)
static bool NormalInShader(const Material* m, bool withLight, bool solid) {
    namespace gfx = w3dEngine;
    return m && m->normalMap && m->normalTexture && m->normalTexture->iID && !solid && withLight && m->LightRealTime() && PerPixelEf(m) &&
           gfx::ShadingPerPixelSupported() && gfx::MaterialMaps(0, 0, 0, 0, 0);
}

void Mesh::ApplyMaterial(Material* mat, bool withLight, bool solid, bool offsetEdit) {
    namespace gfx = w3dEngine;
    // (con normal map de verdad: el pase DOT3 de RenderObject va a correr con una textura subida y UV)
    const bool nmPass = mat && mat->normalMap && gNormalMapWithPass && !NormalInShader(mat, withLight, solid);
    // X-RAY: la malla se dibuja al 50% para poder ver lo que queda detras. Se aplica sobre el
    // ALPHA (copias locales: el material REAL no se toca), asi vale igual en Solid, Material y
    // Rendered sin duplicar cada camino de dibujo.
    const float xrayA = g_xray ? 0.5f : 1.0f;
    // TINTE del objeto (setTinte de lua): escala el ambiente y el difuso (y abajo el color plano); el material
    // compartido no se toca, asi otra malla con el mismo material se sigue viendo normal
    const float* tn = tint;
    // LA TRANSMISION (lo que es vidrio) y su F0 de Fresnel (del IOR: cuanto refleja de frente; 0.04 sin refraccion, como
    // el trazado de rayos). Con la luz PBR por pixel de los shaders, el shader hace transparente lo que no refleja segun
    // el angulo; sin ella (pipeline fijo, luz por vertice) la opacidad baja pareja. En los dos casos se dibuja con mezcla
    const float iorM = (mat->ior > 0.0f) ? mat->ior : 1.0f;
    const float transM = (mat->transmission < 0.0f) ? 0.0f : (mat->transmission > 1.0f ? 1.0f : mat->transmission);
    // (IOR 1 en algo SIN transmision es el de fabrica: refleja como un dielectrico comun, 0.04. CON transmision, IOR 1 es
    //  lo que dice: no dobla la luz ni refleja de frente -transparente perfecto-; antes tambien caia en 0.04 y el vidrio
    //  con IOR 1 se veia como con 1.5, que es el IOR que da ese reflejo)
    const float f0M = (iorM == 1.0f && transM <= 0.0f) ? 0.04f : ((iorM - 1.0f) / (iorM + 1.0f)) * ((iorM - 1.0f) / (iorM + 1.0f));
    const bool pbrShader = !solid && withLight && mat->LightRealTime() && PerPixelEf(mat) && gfx::ShadingPerPixelSupported();
    const float alphaTrans = (transM > 0.0f && !pbrShader && mat->LightRealTime()) ? (1.0f - transM * (1.0f - f0M)) : 1.0f;
    // LA LUZ HORNEADA A MEDIAS (su capa con factor f entre 0 y 1): esta pasada lleva la luz de tiempo real por (1 - f) y la
    // capa le SUMA despues la horneada por f (el bucle de capas de RenderObject): (1 - f) tiempo real + f horneada
    const float fBake = (!solid && mat->LightRealTime()) ? mat->FactorLightBaked() : 0.0f;
    const float kRt = 1.0f - fBake;
    const float amb[4] = { mat->ambient[0] * tn[0] * kRt,  mat->ambient[1] * tn[1] * kRt,  mat->ambient[2] * tn[2] * kRt,  mat->ambient[3]  * xrayA };
    const float dif[4] = { mat->diffuse[0] * tn[0] * kRt,  mat->diffuse[1] * tn[1] * kRt,  mat->diffuse[2] * tn[2] * kRt,  mat->diffuse[3]  * xrayA * alphaTrans };
    const float emi[4] = { mat->emission[0] * kRt, mat->emission[1] * kRt, mat->emission[2] * kRt, mat->emission[3] * xrayA };
    gfx::SmoothShading(true); // el look suave/plano lo dan las NORMALES de la malla, no el material
    // SOMBREADO TOON (Material::toon): la rampa (textureB) en la unidad 1 con la coordenada por vertice que dejo
    // RenderObject (toonUV), y sin la luz de GL. Sin multitextura (GLES2) o sin esa coordenada se dibuja como un material
    // comun (con su luz, si la tiene)
    const bool toonOn = mat->toon && mat->textureB && mat->textureB->iID && !toonUV.empty() &&
                        toonUV.size() >= (size_t)vertexSize * 2 && !solid && !mat->chrome && !mat->normalMap &&
                        gfx::MixTexturesSupported();
    // sin color por vertice, COLOR_MATERIAL se apaga ANTES del material: prendido, GL pisa el ambiente y el difuso con el
    // color actual (y al apagarlo quedan asi: el ambiente dependia de lo que se dibujo antes)
    const bool vertColor = !inkNoEditable && mat->vertexColor && vertexColor;
    if (!vertColor) gfx::Disable(gfx::ColorMaterial);
    gfx::Material(gfx::MatAmbient,  amb);
    gfx::Material(gfx::MatDiffuse,  dif);
    // EL BRILLO de GL (el especular y su exponente) sale de la RUGOSIDAD y el METALICO del material (los valores PBR que
    // edita el panel: el mismo material sirve al pipeline fijo, a los shaders y al trazado de rayos). Liso = brillo chico
    // e intenso; rugoso = ancho y debil; metalico = el brillo toma el color del material
    {
        float rug = mat->rtRoughness, met = mat->rtMetallic;
        if (rug < 0.0f) rug = 0.0f; if (rug > 1.0f) rug = 1.0f;
        if (met < 0.0f) met = 0.0f; if (met > 1.0f) met = 1.0f;
        const float smooth = 1.0f - rug;
        float spec[4];
        for (int c = 0; c < 3; c++) {
            const float base = 0.5f * smooth * smooth * (1.0f - met) + mat->diffuse[c] * met;
            spec[c] = base * (0.25f + 0.75f * smooth);
        }
        spec[3] = 1.0f;
        for (int c = 0; c < 3; c++) spec[c] *= kRt;   // (la luz horneada a medias: el brillo tambien)
        gfx::Material(gfx::MatSpecular, spec);
        gfx::Material(gfx::MatEmission, emi);
        gfx::MaterialShininess(2.0f + smooth * smooth * 126.0f);
        // la LUZ PBR por pixel de los shaders (GLES2 / WebGL / Vulkan): los mismos valores que el trazado de rayos
        if (pbrShader) { gfx::MaterialPBR(met, rug, f0M, transM, mat->ssr);
                         if (g_w3dEnvironmentObjectHook) g_w3dEnvironmentObjectHook(this); }   // (la sonda de reflejo de su lugar)
        else           gfx::MaterialPBR(0.0f, -1.0f, 0.04f, 0.0f, false);
        // LOS MAPAS con textura (el backend de shaders los aplica; el alfa tambien sin la luz PBR)
        if (solid) gfx::MaterialMaps(0, 0, 0, 0, 0);
        else {
            const unsigned nId = (pbrShader && mat->normalMap && mat->normalTexture) ? mat->normalTexture->iID : 0;
            const bool inShader = gfx::MaterialMaps(nId, pbrShader ? TexMap(mat, W3D_MAP_ROUGHNESS) : 0,
                                                     pbrShader ? TexMap(mat, W3D_MAP_METALLIC) : 0,
                                                     TexMap(mat, W3D_MAP_ALPHA), pbrShader ? TexMap(mat, W3D_MAP_EMISSION) : 0);
            // (leen la UV de la malla: con un mapa y sin textura base, el array igual va prendido)
            if (inShader && uv && (nId || TexMap(mat, W3D_MAP_ALPHA) || TexMap(mat, W3D_MAP_ROUGHNESS) ||
                                   TexMap(mat, W3D_MAP_METALLIC) || TexMap(mat, W3D_MAP_EMISSION)))
                gNeedsUV = true;
            else gNeedsUV = false;
        }
    }

    // color por vertice (via ColorMaterial) o el difuso plano del material
    if (inkNoEditable) {
        // SELECCION de malla no editable: 75% material + 25% verde, plano (el vcolor cede:
        // el tinte tiene que VERSE). Reemplaza al contorno de seleccion, que no existe sin edges.
        float td[4] = { mat->diffuse[0]*0.75f + 0.075f, mat->diffuse[1]*0.75f + 0.25f,
                        mat->diffuse[2]*0.75f + 0.075f, dif[3] };
        gfx::Material(gfx::MatDiffuse, td);
        gfx::Color4f(td[0], td[1], td[2], td[3]);
        gfx::DisableArray(gfx::ColorArray);
    } else if (vertColor) {
        gfx::Color4f(0.0f, 0.0f, 0.0f, 1.0f); // el color real lo pone el array
        gfx::EnableArray(gfx::ColorArray);
        gfx::Enable(gfx::ColorMaterial);
    } else {
        // con NORMAL MAP la base va sin luz -> la tiño con el COLOR de la luz aca (sino el N.L sale BLANCO).
        if (nmPass)
            gfx::Color4f(mat->diffuse[0]*g_renderLightColor.x, mat->diffuse[1]*g_renderLightColor.y,
                         mat->diffuse[2]*g_renderLightColor.z, dif[3]);
        else
            gfx::Color4f(dif[0], dif[1], dif[2], dif[3]);
        gfx::DisableArray(gfx::ColorArray);
    }

    // textura (nunca en Solid; respeta el checkbox textureOn del material)
    if (!solid && mat->texture && mat->textureOn) {
        gfx::Enable(gfx::Texture2D);
        unsigned int texId = mat->texture->iID;
        gReflBox = false;
        if (mat->chrome && mat->reflectionProbe && g_w3dReflectionProbeHook) {   // (la imagen de la sonda de reflejo de su lugar)
            Texture* ts = g_w3dReflectionProbeHook(this, gReflB, gReflC, &gReflBox);
            if (ts && ts->iID) texId = ts->iID; else gReflBox = false;
        }
        gfx::BindTexture(g_w3dForcedTexture ? g_w3dForcedTexture : texId);
        gfx::TexFilter(!gfx::PixelatedGlobal() && mat->filtered);
        gfx::TexWrap(mat->repeat);
        // MEZCLA DE TEXTURAS (Material::textureB / mixB): la unidad 1 interpola B con lo de arriba, en la MISMA pasada
        // (no con el reflejo ni el normal map: esos ya usan el combiner / la matriz de textura)
        layerUnit1 = -1;
        if (toonOn) {
            // la rampa sin filtrar ni repetir: los escalones del dibujo animado quedan netos
            gfx::MixTextures(mat->textureB->iID, 1.0f, false, false, mat->toon == 2 ? 4 : 3);
            gfx::MixTexturesUV(&toonUV[0]);
        } else if (mat->textureB && mat->textureB->iID && mat->mixB > 0.0f && !mat->chrome && !mat->normalMap)
            gfx::MixTextures(mat->textureB->iID, mat->mixB, !gfx::PixelatedGlobal() && mat->filtered, mat->repeat);
        else if (mat->layers.size() == 1 && mat->layers[0].on && mat->layers[0].tex && mat->layers[0].tex->iID && uv &&
                 !mat->chrome && !mat->normalMap && !solid && mat->layers[0].force >= 1.0f &&
                 (mat->layers[0].blend == W3D_LAYER_MULTIPLY || mat->layers[0].blend == W3D_LAYER_MULTIPLY2X) &&
                 gfx::MixTexturesSupported()) {
            // UN LIGHTMAP EN LA MISMA PASADA: la unica capa Multiply / Multiply 2x del material va en la unidad 1 con su
            // propia UV (el unwrap del lightmap) en vez de una segunda pasada: la mitad de los draws (y del relleno) del
            // mundo de un mapa con lightmaps. El pase de capas de abajo la saltea (layerUnit1)
            const TexLayer& cap = mat->layers[0];
            const GLfloat* cuv = UVOfLayer(cap.uvMap);
            gfx::MixTextures(cap.tex->iID, 1.0f, !gfx::PixelatedGlobal() && mat->filtered, mat->repeat,
                                cap.blend == W3D_LAYER_MULTIPLY2X ? 2 : 1);
            gfx::MixTexturesUV(cuv ? cuv : NULL);
            layerUnit1 = 0;
        } else gfx::MixTexturesOff();
        // REFLECTION 3 modos (mat->reflectMode):
        //  0 MATCAP   = normal-del-ojo por MATRIZ DE TEXTURA -> HARDWARE en PC Y N95 (rapido). Normales como texcoords.
        //  1 SPHEREMAP= GL_SPHERE_MAP exacto -> HARDWARE en PC (texgen); en N95 (sin texgen) cae a SOFTWARE.
        //  2 EQUIRECT = 360 -> SIEMPRE por SOFTWARE (UV por-corner en CPU, calidad).
        // MATCAP (mode 0): por HARDWARE solo si hay matriz de textura (PC/N95). En GLES2 (Android/WebGL) esa
        // matriz es un no-op -> el matcap va por SOFTWARE, igual que el equirect. Sino no se veia.
        bool matcap     = mat->chrome && mat->reflectMode == 0 && gfx::HasTexGen();  // matcap HW
        bool matcapSW   = mat->chrome && mat->reflectMode == 0 && !gfx::HasTexGen(); // matcap SOFTWARE (GLES2)
        bool sphereExact= mat->chrome && mat->reflectMode == 1;
        bool eq         = mat->chrome && mat->reflectMode == 2;
        bool sphereHW   = sphereExact && gfx::HasTexGen();      // PC: sphere exacto por texgen
        bool sw         = eq || (sphereExact && !gfx::HasTexGen()) || matcapSW; // SOFTWARE: equirect + sphere/matcap sin HW
        // OJO: TexGenSphere y TexMatrixMatcap tocan los DOS la matriz de textura -> nunca los dos a la vez.
        if (matcap) { gfx::TexGenSphere(false); gfx::TexMatrixMatcap(true); }   // matcap: matriz de textura (HW)
        else        { gfx::TexMatrixMatcap(false); gfx::TexGenSphere(sphereHW); } // sphere HW (flip-V) o reset
        gfx::TexEnvReplace(mat->chrome);        // reflejo (cualquier modo) = espejo: textura directa, sin luz
        if (matcap) { gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer3b(normals, vertexSize); } // normales -> texcoords
        else if (sw) { UpdateChromeUV(eq); // build los arrays por-corner (equirect o sphere); bind/draw en el loop
                  gfx::EnableArray(gfx::TexCoordArray); if (eq) gfx::TexWrap(true); } // REPEAT solo equirect (costura)
        else if (UVOfMap(mat, W3D_MAP_BASE)) gfx::TexCoordPointer2f(0, UVOfMap(mat, W3D_MAP_BASE)); // la UV que eligio el material (o la PERSONALIZADA del objeto)
        else if (uv) { if (vboRenderActive && vboUVInUse) gfx::TexCoordVBO(vboUVInUse); else gfx::TexCoordPointer2f(0, uv); } // UV del modelo (VBO o RAM)
    } else {
        gfx::Disable(gfx::Texture2D);
        layerUnit1 = -1;   // (sin textura base las capas van todas en su pasada: la de un material anterior no vale)
        // (un toon sin textura base: la unidad 1 recibe el color plano como "lo anterior" y le resta la rampa)
        if (toonOn) { gfx::MixTextures(mat->textureB->iID, 1.0f, false, false, mat->toon == 2 ? 4 : 3);
                      gfx::MixTexturesUV(&toonUV[0]); }
        else gfx::MixTexturesOff();
        gfx::TexMatrixMatcap(false);
        gfx::TexGenSphere(false);
        gfx::TexEnvReplace(false);
        if (uv) { if (vboRenderActive && vboUVInUse) gfx::TexCoordVBO(vboUVInUse); else gfx::TexCoordPointer2f(0, uv); }
        if (uv && gNeedsUV) gfx::EnableArray(gfx::TexCoordArray);   // (los mapas del material leen la UV)
    }

    // normales: las sube la LUZ y el SPHERE-MAP por HARDWARE (texgen genera las UV de la normal). El MATCAP por
    // matriz de textura usa las normales como TEXCOORDS (no como NormalArray). El path por SOFTWARE (equirect, y el
    // sphere exacto del N95) NO usa el array (UV precomputadas + draw NO indexado). Por eso el reflejo no depende de la luz.
    bool sphereHWn = mat->chrome && mat->reflectMode == 1 && gfx::HasTexGen();
    if (normals && (sphereHWn || (withLight && mat->LightRealTime() && !toonOn))) gfx::EnableArray(gfx::NormalArray);
    else gfx::DisableArray(gfx::NormalArray);
    // la iluminacion en si solo si el material la pide (el chrome con GL_REPLACE la ignora -> espejo perfecto).
    // Con NORMAL MAP la base va SIN luz (albedo plano): el pass DOT3 de abajo aporta toda la iluminacion (N.L).
    if (withLight && mat->LightRealTime() && !nmPass && !toonOn) gfx::Enable(gfx::Lighting);
    else gfx::Disable(gfx::Lighting);
    gfx::ShadingPerPixel(PerPixelEf(mat));   // (solo vale con shaders y con la luz prendida)
    // LAS SOMBRAS EN TIEMPO REAL, si la malla las recibe: con la luz de tiempo real, la del sol no llega donde algo la
    // tapa (1); con la luz HORNEADA entera, la sombra de lo que SE MUEVE oscurece el color (2: la de lo estatico ya esta
    // en la textura horneada, como el "Subtractive" de Unity). Sin luz (un material "sin luz"), nada
    // SOBRE LA HORNEADA la sombra va sobre la LUZ de la textura horneada (Shadowmask: ver TexLayer::maskSun): en esta
    // pasada si esa textura va en la unidad 1 (3); si llega en su propia pasada de capa, ahi (RenderObject) y aca nada
    if (gShadowObject) {
        int mode = 0;
        if (!solid && withLight) {
            if (mat->LightRealTime() && !nmPass && !toonOn) mode = 1;
            else if (mat->lighting && mat->FactorLightBaked() >= 0.999f) {
                const TexLayer* hl = NULL;
                for (size_t k = 0; k < mat->layers.size() && !hl; k++)
                    if (mat->layers[k].baked && mat->layers[k].on && mat->layers[k].tex) hl = &mat->layers[k];
                const bool mult = hl && (hl->blend == W3D_LAYER_MULTIPLY || hl->blend == W3D_LAYER_MULTIPLY2X);
                if (mult && layerUnit1 == 0 && hl == &mat->layers[0]) mode = 3 | (hl->maskSun ? 8 : 0);
                else if (mult) mode = 0;   // (en la pasada de la capa)
                else mode = 2;             // (no se sabe donde esta la luz: se oscurece el color, como antes)
            }
        }
        gfx::ShadowReceive(mode);
    }

    if (mat->culling)     gfx::Enable(gfx::CullFace);  else gfx::Disable(gfx::CullFace);
    // LAS DOS CARAS CON LUZ: un material sin culling (pelo, hojas, telas) se ve tambien por atras, y esa cara se
    // iluminaba con la normal del FRENTE, que mira para el otro lado: quedaba negra (solo el ambiente). Asi la cara
    // de atras usa la normal dada vuelta (GL fijo y GL ES 1.1; el backend de shaders ilumina una cara)
    gfx::LightModelTwoSide(!mat->culling);
    // z-test: lo decide el MATERIAL, salvo en el pase del espejo. Ahi manda el
    // espejo (ver w3dPassMirrorNoZ en w3dGraphics.h): el reflejo del agua queda
    // MAS HONDO que el fondo opaco del estanque, asi que si el material vuelve a
    // prender el z-test el reflejo no pasa el test y no se ve NUNCA. Era la raiz
    // de que 'noDepth' del Mirror no hiciera absolutamente nada.
    if (mat->depth_test && !gfx::w3dPassMirrorNoZ) gfx::Enable(gfx::DepthTest);
    else                                            gfx::Disable(gfx::DepthTest);
    // ESCRITURA de z. La decide el material: un DECAL (sombra, blob, mancha) NO escribe,
    // asi no le pisa el z a lo que venga despues ni queda coplanar consigo mismo. En el
    // pase del espejo no escribe NADIE (w3dPassMirrorNoZ manda igual que en el z-test).
    //
    // Y TAMPOCO ESCRIBE LO QUE ES *LUZ*: un material con mezcla aditiva /
    // sustractiva / multiplicativa / screen no reemplaza el fondo, lo MODULA (ver
    // W3dMaterialIsLight en Materials.h). No puede tapar nada, asi que su z solo
    // sirve para hacer dano: una chispa aditiva dibujada temprano en el arbol le
    // robaba el z-test a todo lo que viniera DESPUES y mas lejos, y dejaba un
    // AGUJERO rectangular con la forma del quad (reporte del dueno, con captura;
    // prueba por pixeles en tools/pruebas/prueba_particulas_z.w3s). Es la MISMA
    // regla que ya cumple el pase diferido de particulas: translucido = z-test SI,
    // z-write NO. `depth_write` del material queda intacto (round-trip del .w3d).
    gfx::DepthMask(mat->depth_write && !W3dMaterialIsLight(mat) && !gfx::w3dPassMirrorNoZ);
    // OFFSET DE PROFUNDIDAD del material (slope-aware, mismo mecanismo que la mascara del
    // espejo en Mirror.cpp:198-199). Corre la calcomania HACIA EL OJO lo justo para ganarle
    // a la superficie coplanar de abajo y seguir perdiendo contra lo que esta de verdad
    // adelante. El offset del EDIT MODE (rellenos un toque atras) sigue valiendo si el
    // material no pide el suyo.
    // FUNCION DE PROFUNDIDAD DE LA CALCOMANIA: LEQUAL, no LESS.
    // Reporte del dueno: "la sombra del personaje se pierde por z-fighting pese al
    // material decal". Una sombra apoyada en el piso es COPLANAR con el: con
    // GL_LESS, todo pixel cuya profundidad salga EXACTAMENTE igual a la del piso
    // (lo normal cuando el blob y el terreno comparten plano, y cada vez mas
    // probable cuanto mas inclinada esta la superficie) NO pasa el test, y la
    // sombra sale comida a lunares. Con LEQUAL el empate pasa; el sesgo de abajo
    // sigue siendo el que la hace ganar contra la superficie y perder contra lo
    // que este de verdad delante. Se restaura a LESS en el epilogo de RenderObject.
    if (mat->order_pass == 1) gfx::DepthFunc(gfx::DepthLEqual);
    // FONDO (el cielo): dibujado ULTIMO de los opacos, clavado al plano lejano.
    // DepthRange 1..1 + LEQUAL: el fragmento sale a z=1.0 y solo pasa donde el
    // buffer sigue en el clear (1.0) = los pixeles que ningun opaco toco. Cero
    // overdraw del cielo, sin escribir z. gBackgroundActive lo restaura el epilogo.
    if (mat->background) {
        gfx::Enable(gfx::DepthTest);
        gfx::DepthFunc(gfx::DepthLEqual);
        gfx::DepthRange(1.0f, 1.0f);
        gfx::DepthMask(false);
        gBackgroundActive = true;
    }
    // SESGO EN METROS: la fila de profundidad de la proyeccion corrida (z_clip = P10*z + P14: como si la malla estuviera
    // mas lejos/cerca en z de camara, sin moverla en pantalla; vale igual en perspectiva y en ortografica)
    gfx::BiasMeters(mat->bias_meters);   // (estado de gfx: solo llama al driver si cambia)
    if (mat->depth_bias != 0.0f) {
        float units = mat->depth_bias;
#ifdef W3D_SYMBIAN
        units *= 4.0f; // z de 16 bits (N95): el mismo sesgo necesita ~4x mas unidades (-4 -> -16)
#endif
        // con SIGNO: negativo = hacia la camara (una calcomania que gana contra la superficie de abajo); positivo = se
        // ALEJA (un piso que pierde los empates contra lo que se apoya en el: autos, sombras). El factor de pendiente
        // va con el mismo signo (antes era -1 fijo: un sesgo positivo empujaba para un lado y la pendiente para el otro)
        gfx::Enable(gfx::PolygonOffsetFill);
        gfx::PolygonOffset(units < 0.0f ? -1.0f : 1.0f, units);
    } else if (offsetEdit) {
        gfx::Enable(gfx::PolygonOffsetFill);
#if defined(W3D_SYMBIAN) && defined(W3D_GLES2)
        // z de 16 bits en el N8 (VideoCore III): con 4 unidades las caras seleccionadas peleaban con la malla y salian
        // RAYADAS a 45 grados. ~4x, como el sesgo de los materiales de arriba
        gfx::PolygonOffset(2.0f, 16.0f);
#else
        gfx::PolygonOffset(2.0f, 4.0f);
#endif
    } else {
        gfx::PolygonOffset(0.0f, 0.0f);
        gfx::Disable(gfx::PolygonOffsetFill);
    }
    // MEZCLA. 'mezcla' = 0 es el alpha de siempre (todo lo guardado hasta hoy); cualquier
    // otro valor es un codigo de gfx::Mezcla — el que importa aca es 2 = MixAdd (ADITIVO,
    // GL_ONE/GL_ONE): aclara y el negro queda invisible, que es como el PSX dibuja el
    // torbellino del giro, los rayos y las chispas. Anda en GL ES 1.1 (N95), sin extensiones.
    if (mat->transparent || (transM > 0.0f && mat->LightRealTime()) || TexMap(mat, W3D_MAP_ALPHA)) {   // (un mapa de alfa: con mezcla)
        gfx::Enable(gfx::Blend);
        if (mat->mix) gfx::SetMix(mat->mix); else gfx::BlendAlpha();
    }
    // X-RAY: aunque el material sea OPACO, se dibuja al 50% para ver lo que hay detras. Va
    // ACA y no arriba porque este if/else es el que decide el blend: prenderlo antes no servia
    // de nada (el 'else' de opaco lo apagaba). Sin escribir z, sino la propia malla se tapa a
    // si misma y no se ve nada a traves.
    else if (g_xray) { gfx::Enable(gfx::Blend); gfx::BlendAlpha(); gfx::DepthMask(false); }
    else                    gfx::Disable(gfx::Blend);
    gfx::AlphaTest(mat->alphaTest);   // recorte por alfa (rejas, alambrados): 0 = apagado
    // SHADER ESCRITO (objects/WrittenShader.h): los triangulos de este material van con el programa del archivo. No en
    // Solid ni en los pases de depuracion (normales, alfa): esos muestran otra cosa. El epilogo de RenderObject lo apaga
    {
        const int sh = (!solid && !w3dRenderAlpha && !w3dRenderNormalColor) ? W3dShaderMaterial(mat) : 0;
        if (sh) {
            float p[gfx::ShaderWrittenParams];
            W3dShaderParameters(p, mat->ior, mat->transmission, mat->rtRoughness, mat->rtMetallic);
            gfx::WrittenShaderUse(sh, p);
        } else gfx::WrittenShaderUse(0, NULL);
    }
}

// hook de overlays del editor (lo registra el editor al arrancar; NULL en una app sin editor)
void (*g_meshOverlayHook)(Mesh*) = NULL;

// WEIGHT PAINT: arma weightPaintColor (RGBA por vertice de render) desde un peso DENSO por
// render-vert. La rampa es AZUL(0) -> amarillo(0.5) -> red(1). El cero es azul (no negro): negro
// se confundia con el fondo/piso oscuro. Azul = "sin peso" es la convencion de Blender.
// La comparten las dos entradas (3D por control-point / UV por corner): la unica diferencia entre
// ellas es DE DONDE sale 'rvW', asi que el color se ve igual en los dos viewports.
void Mesh::WeightColorFromRenderVert(const std::vector<float>& rvW) {
    weightPaintColor.clear();
    if (vertexSize <= 0) return;
    weightPaintColor.resize((size_t)vertexSize * 4, 0);
    for (int i = 0; i < vertexSize; i++) {
        float w = (i < (int)rvW.size()) ? rvW[(size_t)i] : 0.0f;
        if (w < 0.0f) w = 0.0f; if (w > 1.0f) w = 1.0f;
        GLubyte r, g, b;
        if (w < 0.5f) { float t = w / 0.5f;          // blue(40,70,210) -> amarillo(255,255,0)
            r = (GLubyte)(40.0f  + (255.0f - 40.0f)  * t);
            g = (GLubyte)(70.0f  + (255.0f - 70.0f)  * t);
            b = (GLubyte)(210.0f + (0.0f   - 210.0f) * t);
        } else { float t = (w - 0.5f) / 0.5f;         // amarillo(255,255,0) -> red(255,0,0)
            r = 255; g = (GLubyte)(255.0f * (1.0f - t)); b = 0;
        }
        weightPaintColor[(size_t)i*4+0] = r; weightPaintColor[(size_t)i*4+1] = g;
        weightPaintColor[(size_t)i*4+2] = b; weightPaintColor[(size_t)i*4+3] = 255;
    }
}

// VIEWPORT 3D: degradado del VERTEX GROUP 'grupo' (pesos por CONTROL-POINT), mapeados a los
// vertices de render via vertCtrlPoint. Sin grupo/mapa valido queda todo en azul (peso 0).
void Mesh::BuildWeightColor(int group) {
    std::vector<float> rvW;
    if (vertexSize > 0) rvW.assign((size_t)vertexSize, 0.0f);
    VertexGroup* vg = (group >= 0 && group < (int)vertexGroups.size()) ? vertexGroups[group] : NULL;
    if (vg && !vertCtrlPoint.empty() && vertexSize > 0) {
        int maxCP = 0;
        for (size_t i = 0; i < vertCtrlPoint.size(); i++) if (vertCtrlPoint[i] > maxCP) maxCP = vertCtrlPoint[i];
        std::vector<float> cpW((size_t)maxCP + 1, 0.0f); // peso DENSO por control-point
        for (size_t i = 0; i < vg->verts.size() && i < vg->weights.size(); i++) {
            int cp = vg->verts[i];
            if (cp >= 0 && cp <= maxCP) cpW[(size_t)cp] = vg->weights[i];
        }
        for (int i = 0; i < vertexSize && i < (int)vertCtrlPoint.size(); i++) {
            int cp = vertCtrlPoint[i];
            if (cp >= 0 && cp < (int)cpW.size()) rvW[(size_t)i] = cpW[(size_t)cp];
        }
    }
    WeightColorFromRenderVert(rvW);
}

// EDITOR UV: degradado del UV GROUP 'uvGroup' (pesos por CORNER / render-vert, directos: no hay
// nada que mapear). Sin UV group valido queda todo en azul (peso 0).
void Mesh::BuildWeightColorUV(int uvGroup) {
    std::vector<float> rvW;
    if (vertexSize > 0) rvW.assign((size_t)vertexSize, 0.0f);
    UVGroup* ug = (uvGroup >= 0 && uvGroup < (int)uvGroups.size()) ? uvGroups[uvGroup] : NULL;
    if (ug) for (size_t i = 0; i < ug->verts.size() && i < ug->weights.size(); i++) {
        int rv = ug->verts[i];
        if (rv >= 0 && rv < vertexSize) rvW[(size_t)rv] = ug->weights[i];
    }
    WeightColorFromRenderVert(rvW);
}

// ===================================================================================================
//  ARMATURE 2D DEL MESH (skinning de UV) — ver el bloque W3dBone2D en Mesh.h.
//  Los huesos viven en el ESPACIO UV; cada uno deforma los UV de los corners pesados en el
//  UV GROUP de su MISMO NOMBRE. La deformacion parte SIEMPRE del rest (uv2dRest), nunca
//  acumula. La animacion en el tiempo va por los CLIPS PROPIOS de cada armature
//  (Armature2D::anims, ver animation/Armature2DAnimation.h): una curva por hueso y canal que
//  llenan el Auto Key / Insert Keyframe y evalua Armature2DEvaluate. Ya NO se hornea en la capa
//  uv de la vertex animation; ese camino viejo sigue existiendo SOLO por retrocompat, para los
//  proyectos guardados antes de que existieran los clips 2D.
// ===================================================================================================

// ====================================================================
//  ARMATURES 2D: contenedor + accesores al ACTIVO (ver struct Armature2D en Mesh.h)
// ====================================================================
Armature2D::~Armature2D() {
    for (size_t i = 0; i < anims.size(); i++) delete anims[i];
    anims.clear();
}

// DUMMIES estaticos que devuelven los accesores cuando la malla NO tiene armatures 2D: asi el
// codigo de afuera pregunta .empty() como siempre y no hay que chequear NULL en 500 lugares.
// No se escriben nunca por caminos validos (agregar huesos pasa por Arm2DEnsure).
static std::vector<W3dBone2D>            gArm2DEmptyBones;
static std::vector<Armature2DAnimation*> gArm2DEmptyAnims;
// UN dummy POR ACCESOR (no uno compartido): Arm2DActiveBone() y Arm2DActiveAnim() devuelven int&
// y sin armature los dos escribian en la MISMA variable -> setear el hueso activo pisaba el clip
// activo (y al reves). Son estados distintos aunque los dos sean "un indice de -1".
static int                               gArm2DEmptyBone = -1;
static int                               gArm2DEmptyAnim = -1;

Armature2D* Mesh::Arm2DActiveP() {
    if (armature2dActive < 0 || armature2dActive >= (int)armatures2d.size()) return NULL;
    return armatures2d[armature2dActive];
}
const Armature2D* Mesh::Arm2DActiveP() const {
    if (armature2dActive < 0 || armature2dActive >= (int)armatures2d.size()) return NULL;
    return armatures2d[armature2dActive];
}
std::vector<W3dBone2D>& Mesh::Arm2DBones() {
    Armature2D* a = Arm2DActiveP(); return a ? a->bones : gArm2DEmptyBones;
}
const std::vector<W3dBone2D>& Mesh::Arm2DBones() const {
    const Armature2D* a = Arm2DActiveP(); return a ? a->bones : gArm2DEmptyBones;
}
int& Mesh::Arm2DActiveBone() { Armature2D* a = Arm2DActiveP(); return a ? a->activeBone : gArm2DEmptyBone; }
int  Mesh::Arm2DActiveBone() const { const Armature2D* a = Arm2DActiveP(); return a ? a->activeBone : -1; }
std::vector<Armature2DAnimation*>& Mesh::Arm2DAnims() {
    Armature2D* a = Arm2DActiveP(); return a ? a->anims : gArm2DEmptyAnims;
}
const std::vector<Armature2DAnimation*>& Mesh::Arm2DAnims() const {
    const Armature2D* a = Arm2DActiveP(); return a ? a->anims : gArm2DEmptyAnims;
}
int& Mesh::Arm2DActiveAnim() { Armature2D* a = Arm2DActiveP(); return a ? a->activeAnim : gArm2DEmptyAnim; }
int  Mesh::Arm2DActiveAnim() const { const Armature2D* a = Arm2DActiveP(); return a ? a->activeAnim : -1; }

// ============================================================================
//  NOMBRES UNICOS por MALLA - TODOS delegan en W3dUniqueName (base/W3dNames.h).
//  Antes cada lista tenia su propio bucle, con topes en 999 que devolvian un
//  nombre YA OCUPADO y sin pelar el ".NNN" previo (daban "X.001.001").
// ============================================================================
namespace {
    // ctx generico: la lista de nombres tomados + el indice que se excluye
    struct MeshNameCtx { const Mesh* m; int except; int which; };
    // cual: 0 vgroup, 1 uvgroup, 2 uvmap, 3 color, 4 meshpart, 5 vertexanim, 6 armature2d
    bool MeshNameExists(const std::string& n, void* ctx) {
        MeshNameCtx* c = (MeshNameCtx*)ctx;
        const Mesh* m  = c->m;
        switch (c->which) {
            case 0: for (size_t i = 0; i < m->vertexGroups.size(); i++)
                        if ((int)i != c->except && m->vertexGroups[i] && m->vertexGroups[i]->name == n) return true;
                    break;
            case 1: for (size_t i = 0; i < m->uvGroups.size(); i++)
                        if ((int)i != c->except && m->uvGroups[i] && m->uvGroups[i]->name == n) return true;
                    break;
            case 2: for (size_t i = 0; i < m->uvMaps.size(); i++)
                        if ((int)i != c->except && m->uvMaps[i] && m->uvMaps[i]->name == n) return true;
                    break;
            case 3: for (size_t i = 0; i < m->colorLayers.size(); i++)
                        if ((int)i != c->except && m->colorLayers[i] && m->colorLayers[i]->name == n) return true;
                    break;
            case 4: for (size_t i = 0; i < m->materialsGroup.size(); i++)
                        if ((int)i != c->except && m->materialsGroup[i].name == n) return true;
                    break;
            case 5: for (size_t i = 0; i < m->animations.size(); i++)
                        if ((int)i != c->except && m->animations[i] && m->animations[i]->name == n) return true;
                    break;
            case 6: for (size_t i = 0; i < m->armatures2d.size(); i++)
                        if ((int)i != c->except && m->armatures2d[i] && m->armatures2d[i]->name == n) return true;
                    break;
        }
        return false;
    }
    std::string MeshFreeName(const Mesh* m, int which, const std::string& base,
                             const char* defaultValue, int except) {
        MeshNameCtx c; c.m = m; c.except = except; c.which = which;
        return W3dUniqueName(base, defaultValue, MeshNameExists, &c);
    }
}

std::string Mesh::FreeNameVGroup (const std::string& b, int ex) const { return MeshFreeName(this, 0, b, "Group", ex); }
std::string Mesh::FreeNameUVGroup(const std::string& b, int ex) const { return MeshFreeName(this, 1, b, "UV Group", ex); }
std::string Mesh::FreeNameUVMap  (const std::string& b, int ex) const { return MeshFreeName(this, 2, b, "UVMap", ex); }
std::string Mesh::FreeNameColor  (const std::string& b, int ex) const { return MeshFreeName(this, 3, b, "Col", ex); }
std::string Mesh::FreeNameMeshPart(const std::string& b, int ex) const { return MeshFreeName(this, 4, b, "Mesh", ex); }
std::string Mesh::FreeNameVertexAnim(const std::string& b, int ex) const { return MeshFreeName(this, 5, b, "Anim", ex); }

std::string Mesh::Arm2DUniqueName(const std::string& base) const {
    return MeshFreeName(this, 6, base, "Armature 2D", -1);
}
// idem pero excluyendo el armature 'excepto' (para el RENAME: renombrarlo al
// mismo valor no debe devolver ".001")
std::string Mesh::Arm2DFreeName(const std::string& base, int except) const {
    return MeshFreeName(this, 6, base, "Armature 2D", except);
}

// HUESOS 2D: el espacio es la MALLA ENTERA (todos sus armatures 2D a la vez),
// porque el binding es por nombre contra el UV group, que es del mesh.
namespace {
    struct Bone2DNameCtx { const Mesh* m; int exArm; int exBone; };
    bool Bone2DNameExists(const std::string& n, void* ctx) {
        Bone2DNameCtx* c = (Bone2DNameCtx*)ctx;
        for (size_t a = 0; a < c->m->armatures2d.size(); a++) {
            const Armature2D* arm = c->m->armatures2d[a];
            if (!arm) continue;
            for (size_t i = 0; i < arm->bones.size(); i++) {
                if ((int)a == c->exArm && (int)i == c->exBone) continue;
                if (arm->bones[i].name == n) return true;
            }
        }
        return false;
    }
}
std::string Mesh::FreeNameBone2D(const std::string& base, int exArm, int exBone) const {
    Bone2DNameCtx c; c.m = this; c.exArm = exArm; c.exBone = exBone;
    return W3dUniqueName(base, "Bone", Bone2DNameExists, &c);
}
Armature2D* Mesh::Arm2DEnsure() {
    if (armatures2d.empty()) Arm2DAdd("Armature 2D");
    if (armature2dActive < 0 || armature2dActive >= (int)armatures2d.size()) armature2dActive = 0;
    return armatures2d[armature2dActive];
}
int Mesh::Arm2DAdd(const std::string& armName) {
    armatures2d.push_back(new Armature2D(Arm2DUniqueName(armName)));
    armature2dActive = (int)armatures2d.size() - 1;
    return armature2dActive;
}
// borra el armature idx (y sus clips). Los UV GROUPS de sus huesos NO se tocan: son los PESOS por
// corner y el binding es por NOMBRE, asi que conservarlos deja el rig re-bindeado si el editor lo
// devuelve (undo). Un UV group sin hueso no deforma nada. Misma decision que Bone2DDelete con los
// vertex groups del rig 3D.
void Mesh::Arm2DDelete(int idx) {
    if (idx < 0 || idx >= (int)armatures2d.size()) return;
    delete armatures2d[idx];
    armatures2d.erase(armatures2d.begin() + idx);
    if (armature2dActive > idx) armature2dActive--;
    if (armature2dActive >= (int)armatures2d.size()) armature2dActive = (int)armatures2d.size() - 1;
}
// captura el rest LAZY: si uv2dRest no coincide con la topologia actual (primera pose o la
// malla cambio de layout), el uv ACTUAL pasa a ser el rest del skinning 2D.
void Mesh::Armature2DRestCapture() {
    if (!uv || vertexSize <= 0) return;
    if ((int)uv2dRest.size() == vertexSize * 2) return; // ya hay un rest valido
    uv2dRest.assign(uv, uv + (size_t)vertexSize * 2);
}

// TODOS los armatures de la malla (no solo el activo): si CUALQUIER hueso esta posado, el
// skinning tiene que correr.
bool Mesh::Armature2DPoseIdentity() const {
    for (size_t a = 0; a < armatures2d.size(); a++) {
        if (!armatures2d[a]) continue;
        const std::vector<W3dBone2D>& hs = armatures2d[a]->bones;
        for (size_t i = 0; i < hs.size(); i++)
            if (!hs[i].PoseIdentity()) return false;
    }
    return true;
}

// matrices afines 2D (FK): local = trasladar(head + poseT) * rotate(poseRot) * scale(poseS)
// * trasladar(-head)  ->  p' = head + poseT + R*S*(p - head). El mundo del hijo = mundo del
// padre * local (el invariante padre < hijo permite una sola pasada; un padre "adelantado"
// se trata como raiz y se avisa en el que autorea, no aca).
void Mesh::Armature2DMatrices(std::vector<float>& out) const { Armature2DMatricesOf(Arm2DActiveP(), out); }

void Mesh::Armature2DMatricesOf(const Armature2D* arm, std::vector<float>& out) const {
    const float DEG = 3.14159265358979f / 180.0f;
    const size_t n = arm ? arm->bones.size() : 0;
    out.assign(n * 6, 0.0f);
    for (size_t i = 0; i < n; i++) {
        const W3dBone2D& b = arm->bones[i];
        float c = cosf(b.poseRot * DEG), s = sinf(b.poseRot * DEG);
        float M[6];
        M[0] = c * b.poseSX; M[1] = -s * b.poseSY;
        M[3] = s * b.poseSX; M[4] =  c * b.poseSY;
        M[2] = b.headU + b.poseTU - (M[0] * b.headU + M[1] * b.headV);
        M[5] = b.headV + b.poseTV - (M[3] * b.headU + M[4] * b.headV);
        float* W = &out[i * 6];
        if (b.parent >= 0 && b.parent < (int)i) {
            const float* P = &out[(size_t)b.parent * 6];
            W[0] = P[0]*M[0] + P[1]*M[3];
            W[1] = P[0]*M[1] + P[1]*M[4];
            W[2] = P[0]*M[2] + P[1]*M[5] + P[2];
            W[3] = P[3]*M[0] + P[4]*M[3];
            W[4] = P[3]*M[1] + P[4]*M[4];
            W[5] = P[3]*M[2] + P[4]*M[5] + P[5];
        } else {
            for (int k = 0; k < 6; k++) W[k] = M[k];
        }
    }
}

// SKINNING 2D: uv[i] = sum_h( w_h * M_h(rest_i) ) con el resto (1 - sum w) en el rest.
// Suma > 1 se normaliza (como el skinning 3D). Sin rest valido no hace nada (el caller
// captura con Armature2DRestCapture). Con la pose en identidad restaura el rest EXACTO.
void Mesh::Armature2DBonesPlanes(std::vector<const W3dBone2D*>& bones, std::vector<float>& out) const {
    bones.clear(); out.clear();
    std::vector<float> M;
    for (size_t a = 0; a < armatures2d.size(); a++) {
        const Armature2D* arm = armatures2d[a];
        if (!arm || arm->bones.empty()) continue;
        Armature2DMatricesOf(arm, M);                 // FK PROPIA de cada armature (no se mezclan)
        for (size_t i = 0; i < arm->bones.size(); i++) {
            bones.push_back(&arm->bones[i]);
            for (int k = 0; k < 6; k++) out.push_back(M[i * 6 + k]);
        }
    }
}

// PENDIENTE CONOCIDO (performance, no se toca en esta tanda): el bucle de abajo es
// O(huesos * V * log N) al posar -- por CADA hueso arma un std::map con sus pesos y despues
// RECORRE LOS vertexSize VERTICES buscando cada uno en ese map, aunque el UV group tenga 4
// entradas. Lo correcto (y lo que ya hace Armature2DRestFromUV mas abajo) es iterar
// ug->verts, que es SPARSE: queda O(sum de entradas de los UV groups), sin map ni busqueda.
// Importa en el N95, donde posar un rig sobre una malla grande se siente. Requiere acumular
// au/av/ws por vert (ya estan) y una segunda pasada igual a la actual: cambio contenido, pero
// va con su propio test de "misma deformacion, mismo resultado".
void Mesh::Armature2DApply() {
    if (armatures2d.empty() || !uv || vertexSize <= 0) return;
    if ((int)uv2dRest.size() != vertexSize * 2) return; // rest invalido (topologia cambio)
    if (Armature2DPoseIdentity()) {
        // COW: el uv[] de un recurso compartido se copia SOLO si de verdad cambia (con la pose
        // en identidad lo normal es que ya sea el reposo y no haya nada que escribir)
        if (memcmp(uv, &uv2dRest[0], (size_t)vertexSize * 2 * sizeof(GLfloat)) == 0) return;
        UninstantiateData(W3DMD_UV);
        for (int i = 0; i < vertexSize * 2; i++) uv[i] = uv2dRest[i];
        return;
    }
    // la POSE 2D es de ESTA instancia: su uv[] deformado no puede ser el del recurso compartido
    UninstantiateData(W3DMD_UV);
    std::vector<const W3dBone2D*> bones; // TODOS los armatures aplanados (cada uno con su FK)
    std::vector<float> M;
    Armature2DBonesPlanes(bones, M);
    std::vector<float> au((size_t)vertexSize, 0.0f), av((size_t)vertexSize, 0.0f), ws((size_t)vertexSize, 0.0f);
    for (size_t bi = 0; bi < bones.size(); bi++) {
        // el UV GROUP del MISMO NOMBRE que el hueso (binding por nombre; ver la regla en Mesh.h:
        // 3D = hueso <-> vertex group, 2D = hueso <-> UV group). Sus pesos SIEMPRE son por
        // RENDER-VERT (corner) -> pintar UNA cara mueve SOLO sus corners, sin arrastrar las otras
        // caras que comparten el punto 3D. Un hueso sin UV group homonimo no deforma nada.
        UVGroup* ug = NULL;
        for (size_t g = 0; g < uvGroups.size(); g++)
            if (uvGroups[g] && uvGroups[g]->name == bones[bi]->name) { ug = uvGroups[g]; break; }
        if (!ug || ug->verts.empty()) continue;
        std::map<int, float> weightOf; // clave = RENDER-VERT
        for (size_t k = 0; k < ug->verts.size() && k < ug->weights.size(); k++)
            if (ug->weights[k] > 0.0f) weightOf[ug->verts[k]] = ug->weights[k];
        if (weightOf.empty()) continue;
        const float* W = &M[bi * 6];
        for (int i = 0; i < vertexSize; i++) {
            std::map<int, float>::iterator it = weightOf.find(i);
            if (it == weightOf.end()) continue;
            float w = it->second;
            float u0 = uv2dRest[(size_t)i * 2], v0 = uv2dRest[(size_t)i * 2 + 1];
            au[i] += w * (W[0] * u0 + W[1] * v0 + W[2]);
            av[i] += w * (W[3] * u0 + W[4] * v0 + W[5]);
            ws[i] += w;
        }
    }
    for (int i = 0; i < vertexSize; i++) {
        float u0 = uv2dRest[(size_t)i * 2], v0 = uv2dRest[(size_t)i * 2 + 1];
        if (ws[i] <= 0.0f)      { uv[i * 2] = u0; uv[i * 2 + 1] = v0; }
        else if (ws[i] >= 1.0f) { uv[i * 2] = au[i] / ws[i]; uv[i * 2 + 1] = av[i] / ws[i]; }
        else { uv[i * 2] = au[i] + (1.0f - ws[i]) * u0; uv[i * 2 + 1] = av[i] + (1.0f - ws[i]) * v0; }
    }
}

// RE-DERIVAR EL REST desde el uv[] ACTUAL (ver el INVARIANTE uv = f(uv2dRest, pose) en Mesh.h).
// La llama TODA op que escribe uv[] a mano; sin esto la edicion se pierde en el proximo
// Armature2DApply (bug de "muevo una cara en el UV, pinto y se resetea todo al mismo lugar").
//
// Con pose puesta, el skinning es AFIN POR VERT: uv_i = A_i * rest_i + b_i, donde
//   ws >= 1 : A_i = (1/ws) sum_h w_h R_h ; b_i = (1/ws) sum_h w_h t_h
//   0<ws<1  : A_i = sum_h w_h R_h + (1-ws) I ; b_i = sum_h w_h t_h
//   ws <= 0 : A_i = I, b_i = 0 (el vert no lo toca ningun hueso)
// asi que se invierte la 2x2 y sale el rest EXACTO que reproduce el uv editado.
void Mesh::Armature2DRestFromUV() {
    if (!uv || vertexSize <= 0) return;
    if (armatures2d.empty()) { uv2dRest.clear(); return; } // sin rig no hay rest que mantener
    if (Armature2DPoseIdentity()) { uv2dRest.assign(uv, uv + (size_t)vertexSize * 2); return; }
    const bool hasRest = ((int)uv2dRest.size() == vertexSize * 2);
    std::vector<const W3dBone2D*> bones;
    std::vector<float> M;
    Armature2DBonesPlanes(bones, M);
    // acumulador afin por vert: A = [a00 a01 ; a10 a11], b = (b0,b1), ws = suma de pesos
    std::vector<float> a00((size_t)vertexSize, 0.0f), a01((size_t)vertexSize, 0.0f);
    std::vector<float> a10((size_t)vertexSize, 0.0f), a11((size_t)vertexSize, 0.0f);
    std::vector<float> b0((size_t)vertexSize, 0.0f),  b1((size_t)vertexSize, 0.0f);
    std::vector<float> ws((size_t)vertexSize, 0.0f);
    for (size_t bi = 0; bi < bones.size(); bi++) {
        UVGroup* ug = NULL;
        for (size_t g = 0; g < uvGroups.size(); g++)
            if (uvGroups[g] && uvGroups[g]->name == bones[bi]->name) { ug = uvGroups[g]; break; }
        if (!ug || ug->verts.empty()) continue;
        const float* W = &M[bi * 6];
        for (size_t k = 0; k < ug->verts.size() && k < ug->weights.size(); k++) {
            int i = ug->verts[k]; float w = ug->weights[k];
            if (i < 0 || i >= vertexSize || w <= 0.0f) continue;
            a00[(size_t)i] += w * W[0]; a01[(size_t)i] += w * W[1]; b0[(size_t)i] += w * W[2];
            a10[(size_t)i] += w * W[3]; a11[(size_t)i] += w * W[4]; b1[(size_t)i] += w * W[5];
            ws[(size_t)i] += w;
        }
    }
    std::vector<GLfloat> newValue((size_t)vertexSize * 2, 0.0f);
    for (int i = 0; i < vertexSize; i++) {
        float u = uv[i * 2], v = uv[i * 2 + 1];
        float s = ws[(size_t)i];
        float A00, A01, A10, A11, B0, B1;
        if (s <= 0.0f) { newValue[(size_t)i*2] = u; newValue[(size_t)i*2+1] = v; continue; } // sin hueso: rest = uv
        if (s >= 1.0f) { const float inv = 1.0f / s;
            A00 = a00[(size_t)i]*inv; A01 = a01[(size_t)i]*inv; B0 = b0[(size_t)i]*inv;
            A10 = a10[(size_t)i]*inv; A11 = a11[(size_t)i]*inv; B1 = b1[(size_t)i]*inv;
        } else {
            A00 = a00[(size_t)i] + (1.0f - s); A01 = a01[(size_t)i]; B0 = b0[(size_t)i];
            A10 = a10[(size_t)i]; A11 = a11[(size_t)i] + (1.0f - s); B1 = b1[(size_t)i];
        }
        float det = A00 * A11 - A01 * A10;
        if (det > -1e-12f && det < 1e-12f) { // singular (escala 0): se conserva el rest previo
            newValue[(size_t)i*2]   = hasRest ? uv2dRest[(size_t)i*2]   : u;
            newValue[(size_t)i*2+1] = hasRest ? uv2dRest[(size_t)i*2+1] : v;
            continue;
        }
        float du = u - B0, dv = v - B1, inv = 1.0f / det;
        newValue[(size_t)i*2]   = ( A11 * du - A01 * dv) * inv;
        newValue[(size_t)i*2+1] = (-A10 * du + A00 * dv) * inv;
    }
    uv2dRest.swap(newValue);
}

// Sube (o actualiza) los VBOs de render con los arrays ACTUALES. onlyPose=true -> re-sube SOLO pos/nor (cambio la
// pose de skinning; col/uv/idx son estaticos y no se re-suben). Crea cada buffer on-demand. posBuf/norBuf = lo que
// realmente se dibuja (vertex o skinVertex; normals o skinNormals).
void Mesh::UploadVBO(const GLfloat* posBuf, const GLbyte* norBuf, bool onlyPose) {
    namespace gfx = w3dEngine;
    if (!posBuf || vertexSize <= 0) return;
    // las capas que este dibujo toma del RECURSO (sharedVbo) no se suben: ya estan en
    // los VBO compartidos (una subida para todos los objetos que usan la malla)
    const unsigned comp = sharedVbo;
    if (!(comp & W3DMD_POS)) {
        if (!vboPos) vboPos = gfx::GenBuffer();
        gfx::ArrayBufferData(vboPos, posBuf, vertexSize * 3 * (int)sizeof(GLfloat));
    }
    if (norBuf && !(comp & W3DMD_NOR)) { if (!vboNor) vboNor = gfx::GenBuffer(); gfx::ArrayBufferData(vboNor, norBuf, vertexSize * 3 * (int)sizeof(GLbyte)); }
    vboVertN = vertexSize;
    if (onlyPose) return; // solo la pose de skinning cambio -> col/uv/idx (estaticos) no se re-suben
    if (vertexColor && !(comp & W3DMD_COL)) { if (!vboCol) vboCol = gfx::GenBuffer(); gfx::ArrayBufferData(vboCol, vertexColor, vertexSize * 4 * (int)sizeof(GLubyte)); }
    if (uv && !(comp & W3DMD_UV))           { if (!vboUV)  vboUV  = gfx::GenBuffer(); gfx::ArrayBufferData(vboUV,  uv,          vertexSize * 2 * (int)sizeof(GLfloat)); }
    // el IBO es ESTATICO y guarda SIEMPRE la malla completa: se sube una vez con la
    // geometria y no se toca nunca mas. (P3) Con el override de visibilidad activo,
    // los indices del subconjunto van como INDICES DE CLIENTE en el draw
    // (DrawTrianglesClientIdx): cambiar de celda/sector no re-especifica ningun
    // buffer. Antes aca se subia el subconjunto del sector y el cambio de sector
    // re-subia el IBO entero (el anti-patron del stall del tiler MBX).
    if (faces && facesSize > 0 && !(comp & W3DMD_FACES)) { if (!vboIdx) vboIdx = gfx::GenBuffer(); gfx::IndexBufferData(vboIdx, faces, facesSize * (int)sizeof(MeshIndex)); vboIdxN = facesSize; vboIdxGeomVer = skinGeomVersion; }
}

// Sube los VBOs de la malla GENERADA (la de los modificadores) tal como esta ahora; RenderObject lo llama cuando cambio
// (genVersion). Los arreglos que no hay quedan sin VBO
void Mesh::UploadGenVBO() {
    namespace gfx = w3dEngine;
    vboGenVer = genVersion;
    if (!genVertex || genVertexSize <= 0 || !genFaces || genFacesSize <= 0) return;
    if (!vboGenPos) vboGenPos = gfx::GenBuffer();
    gfx::ArrayBufferData(vboGenPos, genVertex, genVertexSize * 3 * (int)sizeof(GLfloat));
    if (genNormals) { if (!vboGenNor) vboGenNor = gfx::GenBuffer(); gfx::ArrayBufferData(vboGenNor, genNormals, genVertexSize * 3 * (int)sizeof(GLbyte)); }
    if (genColor)   { if (!vboGenCol) vboGenCol = gfx::GenBuffer(); gfx::ArrayBufferData(vboGenCol, genColor, genVertexSize * 4 * (int)sizeof(GLubyte)); }
    if (genUV)      { if (!vboGenUV)  vboGenUV  = gfx::GenBuffer(); gfx::ArrayBufferData(vboGenUV, genUV, genVertexSize * 2 * (int)sizeof(GLfloat)); }
    if (!vboGenIdx) vboGenIdx = gfx::GenBuffer();
    gfx::IndexBufferData(vboGenIdx, genFaces, genFacesSize * (int)sizeof(MeshIndex));
}

// ===========================================================================
//  PASE DIFERIDO DE CALCOMANIAS (sombras sueltas)
//
//  Reporte del dueno: "la sombra del personaje se pierde por z-fighting pese al
//  material decal", sobre terreno inclinado y sobre los troncos.
//
//  El sesgo y la funcion de profundidad arreglan la sombra COPLANAR con la
//  superficie que la recibe (ver ApplyMaterial), pero no alcanzan cuando la
//  sombra es un OBJETO APARTE, declarado fuera del cuerpo que la proyecta (el
//  caso que lo destapo: la sombra del personaje y las de los enemigos, sueltas
//  en la escena). Una calcomania NO escribe z a
//  proposito; entonces cualquier cosa OPACA que se dibuje DESPUES de ella y
//  ocupe el mismo lugar (los troncos y la malla EscenarioAlpha van al final del
//  .w3d) la pisa por completo: la sombra desaparece.
//
//  `order_pass` solo ordena ADENTRO de una malla (Materials.h lo dice), asi
//  que no puede arreglar esto. La solucion es la misma que ya usan las
//  particulas: las mallas que son SOLO calcomania no se dibujan durante el
//  recorrido del arbol, se ANOTAN, y se dibujan todas juntas cuando la escena
//  opaca ya esta completa (ViewPort3D::Render, justo antes de las particulas).
//  Asi la sombra siempre cae ENCIMA del suelo, sea cual sea el orden del arbol.
//
//  w3dDecalsInline = "no diferir, dibujar aca mismo": lo prende el pase del
//  espejo (el reflejo tiene su propia matriz y su propia mascara: si su sombra
//  se difiriera se dibujaria despues, sin espejar y fuera del estencil) y el
//  propio flush. Vale para LOS DOS diferidos de este archivo (calcomanias y
//  luces): el reflejo de una chispa aditiva tiene el mismo problema que el de
//  una sombra.
//
//  ---------------------------------------------------------------------------
//  PASE DIFERIDO DE LUCES (la mitad hermana: mallas ADITIVAS/SUSTRACTIVAS)
//
//  Reporte del dueno, con captura: "primero dibuja las particulas con alpha
//  (bien) pero luego dibuja a [la mascara] atras y entonces queda un agujero".
//  Las "particulas" de ese efecto NO son el objeto Particulas: son una malla de
//  quads con material ADITIVO que el script mueve, y que vive ARRIBA en el
//  arbol; la mascara translucida a la que envuelven se declara MILES de lineas
//  mas abajo y un poco mas lejos.
//
//  Ahi hay DOS errores encadenados, y arreglar uno solo cambia un sintoma por
//  otro:
//    1) la chispa ESCRIBIA z (lo arregla W3dMaterialIsLight en ApplyMaterial):
//       la mascara perdia el z-test y quedaba un AGUJERO rectangular;
//    2) la chispa se dibujaba ANTES que la mascara: sin el z-write, la mascara
//       -- que si escribe -- la pisaba y la chispa DESAPARECIA.
//  El unico orden que no le exige nada a nadie es el de siempre para lo
//  translucido: al FINAL, con z-test y sin z-write. O sea exactamente lo que ya
//  hacen las calcomanias y las particulas. Una malla que es SOLO luz se ANOTA y
//  se dibuja en el flush, justo antes de las particulas.
//
//  No hace falta ordenarlas entre si: sumar y restar son conmutativos, asi que
//  el resultado no depende del orden (que es, justamente, la gracia del aditivo).
// ===========================================================================
bool w3dDecalsInline = false;
bool w3dReflecting = false;
unsigned g_w3dPass = 0;
// LOS PASES DE DIBUJO (Material::order_pass: opacos, calcomanias, transparentes). El arbol se recorre UNA vez y se
// dibujan las partes del pase de OPACOS; una malla con partes de los otros dos pases se ANOTA en cada uno, y al
// terminar el recorrido se dibujan esas partes: primero TODAS las calcomanias, despues TODAS las transparentes. Adentro
// de cada pase manda el orden del arbol (el outliner) y el de las partes de malla: eso lo decide el usuario.
// g_w3dPassDrawing: -1 = sin pases (cada malla dibuja todas sus partes, en orden: una miniatura, la edicion), 0 =
// recorriendo el arbol (solo opacos; se anota el resto), 1 / 2 = dibujando ese pase
int g_w3dPassDrawing = -1;
static std::vector<Mesh*> gPassPend[3];
static int PassOf(const Material* m) { const int p = m ? m->order_pass : 0; return p < 0 ? 0 : (p > 2 ? 2 : p); }
static std::vector<Mesh*> gLightPend;
// las mallas cuyo shader escrito LEE LA PANTALLA (w3d_Screen: vidrio, agua, distorsion): van despues de todo lo demas
// (las luces incluidas), si no la copia de la pantalla no tendria lo que esta detras
static std::vector<Mesh*> gScreenPend;

// true si TODOS los grupos de material de la malla son LUZ (mezcla aditiva /
// sustractiva / multiplicativa / screen; ver W3dMaterialIsLight). Mismo criterio
// que EsSoloDecal: una malla MIXTA (un cuerpo opaco con un halo aditivo) NO se
// difiere, porque su parte opaca tiene que quedarse donde el arbol la puso.
bool Mesh::IsOnlyLight() const {
    if (materialsGroup.empty()) return false;
    for (size_t g = 0; g < materialsGroup.size(); g++) {
        if (!W3dMaterialIsLight(materialsGroup[g].material)) return false;
    }
    return true;
}

// (el viewport y el pase de la escena, al EMPEZAR a recorrer el arbol: desde aca se arman los pases)
void W3dDecalsClearPending() { gPassPend[1].clear(); gPassPend[2].clear(); g_w3dPassDrawing = 0; }
void W3dLightsClearPending()   { gLightPend.clear(); gScreenPend.clear(); }

// true si TODOS los grupos de material tienen un shader escrito (compilado) que lee la pantalla
// un VIDRIO de la luz por pixel (transmision con shaders): va al final, sobre la copia de lo que ya se dibujo
static bool IsGlassScreen(const std::vector<MaterialGroup>& groups) {
    namespace gfx = w3dEngine;
    // (en el render y en la VISTA PREVIA DE MATERIAL: la que es para ver los materiales. En los modos de analisis -solido,
    //  alambre, normales, profundidad, alfa- no hay luz PBR)
    if (groups.empty() || w3dRenderSolid || w3dRenderWireframe || w3dRenderNormalColor || w3dRenderNoLight || w3dRenderAlpha ||
        !gfx::ShadingPerPixelSupported()) return false;
    for (size_t g = 0; g < groups.size(); g++) {
        const Material* mt = groups[g].material;
        if (mt && (mt->transmission > 0.0f || mt->ssr) && mt->lighting && mt->perPixel && !mt->shaderOn) return true;
    }
    return false;
}
bool Mesh::IsOnlyScreen() const {
    if (IsGlassScreen(materialsGroup)) return true;
    if (materialsGroup.empty() || w3dRenderSolid) return false;
    for (size_t g = 0; g < materialsGroup.size(); g++) {
        Material* mt = materialsGroup[g].material;
        if (!mt || !mt->shaderOn || !W3dShaderMaterial(mt) || !mt->shaderScreen) return false;
    }
    return true;
}

// El flush de las LUCES: mismo esqueleto que el de calcomanias (afirma su
// estado, arma la matriz de VISTA y dibuja cada malla con su matriz de mundo).
// Corre DESPUES de las calcomanias y ANTES de las particulas: sombra -> luz de
// malla -> billboards.
void W3dLightsDrawPending() {
    if (gLightPend.empty() && gScreenPend.empty()) return;
    namespace gfx = w3dEngine;
    CameraBase cam;
    cam.pos = g_renderCamPos; cam.rot = g_renderCamRot;
    Matrix4 V = cam.ViewMatrix();
    // NINGUNA LUZ HEREDA EL RECORTE DE NADIE (misma razon que en las
    // calcomanias: los espejos dejan estencil, planos de recorte, DepthRange y
    // ColorMask tocados, y este pase corre despues de todos ellos).
    gfx::StencilTurnOff();
    gfx::PlanesClip(0, NULL);
    gfx::DepthRange(0.0f, 1.0f);
    gfx::ColorMask(true, true, true, true);
    gfx::DepthFunc(gfx::DepthLess);

    const bool inlinePrev = w3dDecalsInline;
    w3dDecalsInline = true;   // adentro del flush se dibuja de verdad
    gfx::MatrixMode(gfx::ModelView);
    for (size_t i = 0; i < gLightPend.size(); i++) {
        Mesh* m = gLightPend[i];
        if (!m) continue;
        Matrix4 W; m->GetWorldMatrix(W);
        gfx::PushMatrix();
        gfx::LoadMatrix(V.m);
        gfx::MultMatrix(W.m);
        m->RenderObject();
        gfx::PopMatrix();
    }
    // (y al final las que leen la pantalla: ya esta todo lo de atras). Los VIDRIOS leen UNA copia (SceneCopy)
    bool hasGlass = false;
    for (size_t i = 0; i < gScreenPend.size() && !hasGlass; i++) if (gScreenPend[i]) hasGlass = IsGlassScreen(gScreenPend[i]->materialsGroup);
    if (hasGlass) gfx::SceneCopy(true);
    for (size_t i = 0; i < gScreenPend.size(); i++) {
        Mesh* m = gScreenPend[i];
        if (!m) continue;
        Matrix4 W; m->GetWorldMatrix(W);
        gfx::PushMatrix();
        gfx::LoadMatrix(V.m);
        gfx::MultMatrix(W.m);
        m->RenderObject();
        gfx::PopMatrix();
    }
    if (hasGlass) gfx::SceneCopy(false);
    w3dDecalsInline = inlinePrev;
    gLightPend.clear(); gScreenPend.clear();
    // el estado que toca ApplyMaterial queda como lo dejo la ultima luz:
    // devolverlo al baseline que espera el resto del frame.
    gfx::DepthFunc(gfx::DepthLess);
    gfx::DepthMask(true);
    gfx::PolygonOffset(0.0f, 0.0f);
    gfx::Disable(gfx::PolygonOffsetFill);
}

void W3dDecalsDrawPending() {
    // (termino el recorrido del arbol: lo que se dibuje desde aca no se anota en ningun pase)
    g_w3dPassDrawing = -1;
    if (gPassPend[1].empty() && gPassPend[2].empty()) return;
    namespace gfx = w3dEngine;
    // la VISTA que esta dibujando (la misma que bindeo el viewport). Se rearma
    // igual que en Culling::RenderChildren, para no depender de que la pila de
    // matrices haya quedado en algun estado particular.
    CameraBase cam;
    cam.pos = g_renderCamPos; cam.rot = g_renderCamRot;
    Matrix4 V = cam.ViewMatrix();

    // NINGUNA CALCOMANIA HEREDA EL RECORTE DE NADIE.
    // Este pase corre al FINAL del frame, despues de que dibujo todo el mundo --
    // incluidos los espejos (Mirror), que instalan PLANOS DE RECORTE y una MASCARA
    // DE ESTENCIL con la silueta del agua, y que ademas juegan con DepthRange y
    // ColorMask para estampar profundidad. Si alguno de esos estados llegara
    // prendido hasta aca, TODAS las sombras del nivel saldrian recortadas contra el
    // borde del pozo de agua -- que es exactamente el sintoma que reporto el dueno
    // ("la sombra del personaje sigue recortada"). Hoy el Mirror los deja balanceados,
    // pero "hoy queda balanceado" no es una garantia: el flush AFIRMA su estado en
    // vez de confiar en el de otro. Son cuatro llamadas por frame.
    gfx::StencilTurnOff();          // sin mascara: la sombra no vive adentro de ningun espejo
    gfx::PlanesClip(0, NULL);       // sin planos de recorte del espejo
    gfx::DepthRange(0.0f, 1.0f);    // el rango normal (el pase B del espejo lo lleva a (1,1))
    gfx::ColorMask(true, true, true, true);

    w3dDecalsInline = true;   // adentro del flush se dibuja de verdad
    gfx::MatrixMode(gfx::ModelView);
    for (int pass = 1; pass <= 2; pass++) {   // el pase de CALCOMANIAS y despues el de TRANSPARENTES
        g_w3dPassDrawing = pass;
        for (size_t i = 0; i < gPassPend[pass].size(); i++) {
            Mesh* m = gPassPend[pass][i];
            if (!m) continue;
            Matrix4 W; m->GetWorldMatrix(W);
            gfx::PushMatrix();
            gfx::LoadMatrix(V.m);
            gfx::MultMatrix(W.m);
            m->RenderObject();
            gfx::PopMatrix();
        }
        gPassPend[pass].clear();
    }
    g_w3dPassDrawing = -1;
    w3dDecalsInline = false;
    // el estado que toca ApplyMaterial queda como lo dejo la ultima
    // calcomania: devolverlo al baseline que espera el resto del frame.
    gfx::DepthFunc(gfx::DepthLess);
    gfx::DepthMask(true);
    gfx::PolygonOffset(0.0f, 0.0f);
    gfx::Disable(gfx::PolygonOffsetFill);
}

// (P4) sello del pase de escena para el lote estatico (ver Mesh.h)
unsigned w3dBatchStamp = 0;

// ===========================================================================
//  LINEAS DEL MATERIAL (Material::lineas + thicknessLine): dibuja las ARISTAS
//  del rango de triangulos recien dibujado, con el MISMO estado de material
//  (textura/color/blend ya aplicados por ApplyMaterial). Las aristas se
//  DEDUPLICAN (una arista compartida por dos triangulos se traza UNA vez: con
//  un material translucido/aditivo trazarla dos veces doblaria su mezcla).
//  El z-test pasa a LEqual durante el trazo: las lineas caen justo sobre el
//  relleno que acaba de escribir esa misma z (con Less no pintaria un pixel);
//  el que llama re-aplica el material del proximo grupo (ultimo = NULL).
//  En una TIRA degenerada (triangulos de area cero) el relleno no emite nada
//  y estas aristas dibujan el CAMINO de la polilinea.
// ===========================================================================
static void W3dDrawLinesMat(const MeshIndex* idx, int count, float thickness) {
    namespace gfx = w3dEngine;
    if (!idx || count < 3) return;
    static std::vector<unsigned long long> keys; // arista codificada (min,max)
    static std::vector<MeshIndex> lines;         // pares de indices a dibujar
    keys.clear();
    for (int t = 0; t + 2 < count; t += 3) {
        const MeshIndex v[3] = { idx[t], idx[t + 1], idx[t + 2] };
        for (int e = 0; e < 3; e++) {
            MeshIndex a = v[e], b = v[(e + 1) % 3];
            if (a == b) continue;                  // arista degenerada (tira): nada
            unsigned long long lo = (a < b) ? a : b, hi = (a < b) ? b : a;
            keys.push_back((lo << 32) | hi);
        }
    }
    if (keys.empty()) return;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    lines.clear(); lines.reserve(keys.size() * 2);
    for (size_t i = 0; i < keys.size(); i++) {
        lines.push_back((MeshIndex)(keys[i] >> 32));
        lines.push_back((MeshIndex)(keys[i] & 0xffffffffull));
    }
    gfx::LineWidth(thickness < 1.0f ? 1.0f : thickness);
    gfx::DepthFunc(gfx::DepthLEqual);   // coplanar con su propio relleno
    gfx::DrawLinesClientIdx((int)lines.size(), &lines[0]);
    gfx::DepthFunc(gfx::DepthLess);
    gfx::LineWidth(1.0f);
}

// LAS LINEAS DE POLIGONO del material: las aristas de las caras (faces3d) de ese mesh part -sin las diagonales con que
// se triangulan los quads, que el usuario no dibujo- y, en el primer mesh part con Lineas, las aristas SUELTAS (las que
// no tienen cara: una polilinea, un perfil). Los indices son los del render, sobre los atributos ya bindeados.
static void W3dDrawLinesIdx(const std::vector<MeshIndex>& lines, float thickness) {
    namespace gfx = w3dEngine;
    if (lines.size() < 2) return;
    gfx::LineWidth(thickness < 1.0f ? 1.0f : thickness);
    gfx::DepthFunc(gfx::DepthLEqual);   // coplanar con su propio relleno
    gfx::DrawLinesClientIdx((int)lines.size(), &lines[0]);
    gfx::DepthFunc(gfx::DepthLess);
    gfx::LineWidth(1.0f);
}
static void W3dLinesPolygon(const Mesh* m, int group, bool loose, std::vector<MeshIndex>& out) {
    static std::vector<unsigned long long> keys;
    keys.clear(); out.clear();
    for (size_t f = 0; f < m->faces3d.size(); f++) {
        if (m->faces3d[f].mat != group) continue;
        const std::vector<int>& p = m->faces3d[f].idx; const size_t n = p.size();
        for (size_t k = 0; k < n; k++) {
            const int a = p[k], b = p[(k + 1) % n];
            if (a == b || a < 0 || b < 0 || a >= m->vertexSize || b >= m->vertexSize) continue;
            const unsigned long long lo = (unsigned long long)(a < b ? a : b), hi = (unsigned long long)(a < b ? b : a);
            keys.push_back((lo << 32) | hi);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    out.reserve(keys.size() * 2 + (loose ? m->looseEdges.size() : 0));
    for (size_t i = 0; i < keys.size(); i++) { out.push_back((MeshIndex)(keys[i] >> 32)); out.push_back((MeshIndex)(keys[i] & 0xffffffffull)); }
    if (loose)
        for (size_t i = 0; i + 1 < m->looseEdges.size(); i += 2) {
            const int a = m->looseEdges[i], b = m->looseEdges[i + 1];
            if (a < 0 || b < 0 || a >= m->vertexSize || b >= m->vertexSize || a == b) continue;
            out.push_back((MeshIndex)a); out.push_back((MeshIndex)b);
        }
}
// las caras (faces3d) sirven para las lineas de poligono: hay algo que corregir (un ngon -cuyas diagonales no se
// dibujan- o aristas sueltas) y describen EXACTAMENTE lo que se dibuja (sus triangulos = el index buffer). Una malla
// recien abierta puede traer caras que no coinciden con el render (una tira degenerada que el re-merge colapso): ahi
// van las aristas de los triangulos, como siempre
static bool W3dLinesByPolygon(const Mesh* m) {
    if (m->faces3d.empty()) return !m->looseEdges.empty() && m->facesSize == 0;   // (solo aristas sueltas)
    bool hasNgon = false; int tris = 0;
    for (size_t f = 0; f < m->faces3d.size(); f++) {
        const int n = (int)m->faces3d[f].idx.size();
        if (n > 3) hasNgon = true;
        if (n >= 3) tris += n - 2;
    }
    if (!hasNgon && m->looseEdges.empty()) return false;
    return tris * 3 == m->facesSize;
}
// el primer mesh part cuyo material tiene Lineas (el que lleva las aristas sueltas y las de la malla generada)
static size_t W3dFirstGroupLines(const std::vector<MaterialGroup>& gs) {
    for (size_t g = 0; g < gs.size(); g++) if (gs[g].material && gs[g].material->lines) return g;
    return (size_t)-1;
}

// ===========================================================================
//  DIBUJO CRUDO (objeto Rutina): la geometria de la malla con el estado que ya haya puesto la rutina. Una malla
//  compartida por 20 autos se prepara UNA vez (punteros + VBO) y despues cada auto cuesta su matriz y su draw.
//  Una malla con esqueleto se dibuja en la ultima pose calculada (por arrays de RAM: la pose cambia por frame).
// ===========================================================================
unsigned int g_w3dForcedTexture = 0;

void Mesh::RawPrepare() {
    namespace gfx = w3dEngine;
    rawVBO = false;
    GLfloat* posBuf = (skinArmature && skinVertex) ? skinVertex : vertex;
    GLbyte*  norBuf = (skinArmature && skinNormals) ? skinNormals : normals;
    if (gfx::VBOSupported() && !skinArmature && posBuf && faces && facesSize >= 3) {
        unsigned comp = 0;
        if (mesh && dataComp) { comp = dataComp; if (!W3dMeshResourceEnsureVBO(mesh)) comp = 0; }
        sharedVbo = (unsigned char)comp;
        const bool posOwn = !(comp & W3DMD_POS);
        if (vboGeomVer != skinGeomVersion || vboVertN != vertexSize || (posOwn && !vboPos)) { UploadVBO(posBuf, norBuf, false); vboGeomVer = skinGeomVersion; }
        const unsigned hPos = (comp & W3DMD_POS)   ? mesh->vboPos : vboPos;
        const unsigned hNor = (comp & W3DMD_NOR)   ? mesh->vboNor : vboNor;
        const unsigned hCol = (comp & W3DMD_COL)   ? mesh->vboCol : vboCol;
        const unsigned hUV  = (comp & W3DMD_UV)    ? mesh->vboUV  : vboUV;
        const unsigned hIdx = (comp & W3DMD_FACES) ? mesh->vboIdx : vboIdx;
        if (hPos && hIdx) {
            gfx::VertexVBO(hPos);
            if (norBuf && hNor)      gfx::NormalVBO(hNor);
            if (vertexColor && hCol) gfx::ColorVBO(hCol);
            if (uv && hUV)           gfx::TexCoordVBO(hUV);
            gfx::BindIndexVBO(hIdx);
            rawVBO = true; vboUVInUse = hUV;
        }
    }
    if (!rawVBO) {
        gfx::VertexPointer3f(0, posBuf);
        if (norBuf)      gfx::NormalPointer3b(norBuf);
        if (vertexColor) gfx::ColorPointer4ub(vertexColor);
        if (uv)          gfx::TexCoordPointer2f(0, uv);
    }
    vboRenderActive = rawVBO;
}
void Mesh::RawPart(int part, bool withMaterial) {
    namespace gfx = w3dEngine;
    const size_t n = materialsGroup.size();
    const size_t g0 = (part < 0) ? 0 : (size_t)part, g1 = (part < 0) ? n : (size_t)part + 1;
    for (size_t g = g0; g < g1; g++) {
        const MaterialGroup& grp = materialsGroup[g];
        if (grp.indicesDrawnCount <= 0) continue;
        if (withMaterial && grp.material) ApplyMaterial(grp.material, !w3dRenderNoLight, w3dRenderSolid);
        if (rawVBO) gfx::DrawTrianglesVBO(grp.indicesDrawnCount, grp.startDrawn);
        else          gfx::DrawTriangles(grp.indicesDrawnCount, &faces[grp.startDrawn]);
    }
}
void Mesh::RawRange(int first, int last, bool withMaterial) {
    namespace gfx = w3dEngine;
    const int i0 = first * 3, i1 = (last + 1) * 3;     // [i0, i1) en indices
    if (i1 <= i0) return;
    if (!withMaterial) {
        if (rawVBO) gfx::DrawTrianglesVBO(i1 - i0, i0);
        else          gfx::DrawTriangles(i1 - i0, &faces[i0]);
        return;
    }
    for (size_t g = 0; g < materialsGroup.size(); g++) {
        const MaterialGroup& grp = materialsGroup[g];
        const int a = grp.startDrawn > i0 ? grp.startDrawn : i0;
        const int b = (grp.startDrawn + grp.indicesDrawnCount) < i1 ? (grp.startDrawn + grp.indicesDrawnCount) : i1;
        if (b <= a) continue;
        if (grp.material) ApplyMaterial(grp.material, !w3dRenderNoLight, w3dRenderSolid);
        if (rawVBO) gfx::DrawTrianglesVBO(b - a, a);
        else          gfx::DrawTriangles(b - a, &faces[a]);
    }
}
void Mesh::RawFinish() {
    w3dEngine::UnbindVBOs();
}

// los punteros de RAM de una malla (el camino de arrays de cliente); uvp NULL = sin el de UV
static void PointersRAM(const GLfloat* pos, const GLbyte* nor, const GLubyte* col, const GLfloat* uvp) {
    namespace gfx = w3dEngine;
    gfx::VertexPointer3f(0, pos);
    if (nor) gfx::NormalPointer3b(nor);
    if (col) gfx::ColorPointer4ub(col);
    if (uvp) gfx::TexCoordPointer2f(0, uvp);
}

// (onlyRoutine, pase de un espejo) la pose de RAM -la que dejo la vertex anim o lua- con los materiales de sus partes.
// De cliente: el reflejo no re-especifica el VBO de la pose (el stall del MBX) ni el del recurso
void Mesh::RenderOnlyRoutineReflection() {
    namespace gfx = w3dEngine;
    W3dUpToDatePose(this);
    if (!vertex || !faces || facesSize < 3) return;
    vboRenderActive = false;
    gfx::EnableArray(gfx::VertexArray);
    gfx::DisableArray(gfx::NormalArray);
    if (uv) gfx::EnableArray(gfx::TexCoordArray); else gfx::DisableArray(gfx::TexCoordArray);
    if (vertexColor) gfx::EnableArray(gfx::ColorArray); else gfx::DisableArray(gfx::ColorArray);
    PointersRAM(vertex, NULL, vertexColor, uv);
    Material* last = NULL;
    for (size_t g = 0; g < materialsGroup.size(); g++) {
        const MaterialGroup& grp = materialsGroup[g];
        if (grp.indicesDrawnCount <= 0) continue;
        Material* mat = grp.material ? grp.material : MaterialDefault;
        if (!mat) continue;
        if (mat != last) { ApplyMaterial(mat, false, false); last = mat; }
        gfx::DrawTriangles(grp.indicesDrawnCount, &faces[grp.startDrawn]);
    }
}

// ---- LA SONDA DE LUZ (main/objects/LightProbe.h) ----
// el gancho: al empezar a dibujar una malla (empieza=true) el editor/juego busca la luz indirecta de su lugar, la deja
// en g_w3dProbeSH (g_w3dProbeHas) y se la pasa al backend (AmbientSH / el ambiente de GL); al terminar la saca
void (*g_w3dObjectProbeHook)(Object* o, bool begins) = NULL;
bool  g_w3dProbeHas = false;
float g_w3dProbeSH[27];
namespace {
struct GuardProbe {   // (la saca por cualquiera de los return de RenderObject)
    Object* o; bool placed;
    explicit GuardProbe(Object* x) : o(x), placed(false) {
        if (g_w3dObjectProbeHook && w3dRenderLights && !w3dRenderSolid) { g_w3dObjectProbeHook(o, true); placed = true; }
    }
    ~GuardProbe() { if (placed && g_w3dObjectProbeHook) g_w3dObjectProbeHook(o, false); }
};
}
// ---- LAS SOMBRAS EN TIEMPO REAL (main/render/RealTimeShadows.h) ----
// g_w3dShadowsPass: el pase de Render tiene sombras. Con shaders el backend se las aplica a lo que las RECIBE: cada
// material dice como (ShadowReceive en ApplyMaterial: con la luz de tiempo real o sobre la horneada); sin shaders (el GL
// fijo, GL ES 1.1 del N95) el gancho, al terminar el dibujo de la malla, la vuelve a dibujar encima con la sombra
// proyectada (una pasada que multiplica)
bool g_w3dShadowsPass = false;
void (*g_w3dShadowMeshHook)(Mesh* m) = NULL;
// (gShadowObject: la malla que se dibuja las recibe; declarado arriba, lo lee ApplyMaterial)
namespace {
struct ShadowGuard {   // (como la de la sonda: por cualquiera de los return de RenderObject)
    Mesh* m; bool placed;
    explicit ShadowGuard(Mesh* x) : m(x), placed(false) {
        if (g_w3dShadowsPass && w3dRenderLights && !w3dRenderSolid && m->receivesShadow) { gShadowObject = true; placed = true; }
    }
    ~ShadowGuard() {
        if (!placed) return;
        gShadowObject = false;
        w3dEngine::ShadowReceive(0);
        if (g_w3dShadowMeshHook) g_w3dShadowMeshHook(m);
    }
};
}
// LA LUZ INDIRECTA POR VERTICE (Material::lightIndirect, el pipeline fijo: el N95): la CPU calcula la luz de cada vertice
// -las lamparas que prendieron su canal en este pase (Light.h) + la luz de la sonda con la normal del vertice en el
// mundo- por el color del material. Se dibuja SIN la luz de GL, con el resultado como color de vertice (la textura lo
// multiplica). En ES 1.1 no hay como sumarle a la luz de GL un segundo color por vertice sin otra pasada, y otra pasada
// es el doble de relleno para el MBX: la cuenta en la CPU sale mas barata.
// La LUZ de cada vertice (sin el material) queda guardada en la malla con una firma de todo lo que la cambia: en una
// escena quieta no se vuelve a calcular en cada cuadro (en la 3DS y el N95 era la cuenta mas cara de la CPU), y con
// varios grupos de material cada uno solo le pone su color.
static std::vector<unsigned char> gLightIndCol;
// la firma: FNV-1a de 32 bits, de a 4 bytes
static unsigned SigWords(unsigned h, const void* data, size_t nBytes) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i = 0;
    for (; i + 4 <= nBytes; i += 4) { unsigned w; memcpy(&w, p + i, 4); h = (h ^ w) * 16777619u; }
    for (; i < nBytes; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
// una lampara del pase, en el mundo (afuera de la funcion: C++03 no deja un tipo local en un std::vector)
struct LightIndLamp { float p[3], d[3], c[3], att[3], sd[3], cosCone, expo; bool dir, cone; };
// la luz que le llega a UN vertice (en el mundo): la sonda (armonicos esfericos, las mismas cuentas que el shader) y
// las lamparas del pase (la difusa de GL)
static void LightOfVertex(const Matrix4& W, const float* S, const std::vector<LightIndLamp>& ls, const GLfloat* pos,
                          const GLbyte* nor, float* light) {
    const Vector3 p = W * Vector3(pos[0], pos[1], pos[2]);
    float nx = W.m[0]*nor[0] + W.m[4]*nor[1] + W.m[8]*nor[2];
    float ny = W.m[1]*nor[0] + W.m[5]*nor[1] + W.m[9]*nor[2];
    float nz = W.m[2]*nor[0] + W.m[6]*nor[1] + W.m[10]*nor[2];
    const float ln = sqrtf(nx*nx + ny*ny + nz*nz);
    if (ln > 1e-6f) { nx /= ln; ny /= ln; nz /= ln; } else { nx = 0; ny = 1; nz = 0; }
    const float Y[9] = { 0.282095f, 0.488603f*ny, 0.488603f*nz, 0.488603f*nx, 1.092548f*nx*ny, 1.092548f*ny*nz,
                         0.315392f*(3.0f*nz*nz - 1.0f), 1.092548f*nx*nz, 0.546274f*(nx*nx - ny*ny) };
    light[0] = light[1] = light[2] = 0.0f;
    for (int i = 0; i < 9; i++) { light[0] += S[i*3] * Y[i]; light[1] += S[i*3+1] * Y[i]; light[2] += S[i*3+2] * Y[i]; }
    for (int k = 0; k < 3; k++) if (light[k] < 0.0f) light[k] = 0.0f;
    for (size_t i = 0; i < ls.size(); i++) {
        const LightIndLamp& L = ls[i];
        float lx, ly, lz, at = 1.0f;
        if (L.dir) { lx = L.d[0]; ly = L.d[1]; lz = L.d[2]; }
        else {
            lx = L.p[0] - p.x; ly = L.p[1] - p.y; lz = L.p[2] - p.z;
            const float dist = sqrtf(lx*lx + ly*ly + lz*lz);
            if (dist > 1e-6f) { lx /= dist; ly /= dist; lz /= dist; }
            const float den = L.att[0] + L.att[1] * dist + L.att[2] * dist * dist;
            at = den > 1e-6f ? 1.0f / den : 1.0f;
            if (L.cone) {
                const float sd = -(lx*L.sd[0] + ly*L.sd[1] + lz*L.sd[2]);
                at = (sd >= L.cosCone) ? at * (L.expo > 0.0f ? powf(sd, L.expo) : 1.0f) : 0.0f;
            }
        }
        const float ndl = nx*lx + ny*ly + nz*lz;
        if (ndl > 0.0f && at > 0.0f) for (int k = 0; k < 3; k++) light[k] += L.c[k] * ndl * at;
    }
}
static void IndirectLightVertices(Mesh* m, const Material* mat, const GLfloat* pos, const GLbyte* nor,
                                 const GLubyte* vcol, int n) {
    if (!pos || !nor || n <= 0) { gLightIndCol.clear(); return; }
    Matrix4 W; m->GetWorldMatrix(W);
    // las lamparas de este pase, en el mundo
    std::vector<LightIndLamp> ls;
    for (size_t i = 0; i < Lights.size(); i++) {
        Light* l = Lights[i];
        if (!l) continue;
        const int slot = (int)l->LightID - (int)GL_LIGHT0;
        if (slot < 0 || slot >= 32 || !(g_w3dLightsOnScene & (1u << slot))) continue;
        Matrix4 LW; l->GetWorldMatrix(LW);
        LightIndLamp x; memset(&x, 0, sizeof(x));   // (todos sus bytes entran en la firma)
        const float I = l->intensity < 0.0f ? 0.0f : l->intensity;
        for (int k = 0; k < 3; k++) x.c[k] = l->diffuse[k] * I;
        x.dir = l->directional;
        if (x.dir) {   // (la direccion HACIA la luz: el "arriba" local de la lampara)
            float d[3] = { LW.m[4], LW.m[5], LW.m[6] };
            const float ln = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            for (int k = 0; k < 3; k++) x.d[k] = ln > 1e-6f ? d[k] / ln : (k == 1 ? 1.0f : 0.0f);
        } else {
            const Vector3 lp = LW * Vector3(l->position[0], l->position[1], l->position[2]);
            x.p[0] = lp.x; x.p[1] = lp.y; x.p[2] = lp.z;
            x.att[0] = l->attConstant; x.att[1] = l->attLinear; x.att[2] = l->attQuadratic;
        }
        x.cone = !x.dir && l->spotCutoff < 179.9f;
        if (x.cone) {
            float d[3] = { -LW.m[8], -LW.m[9], -LW.m[10] };   // (el "adelante" local: -Z)
            const float ln = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            for (int k = 0; k < 3; k++) x.sd[k] = ln > 1e-6f ? d[k] / ln : 0.0f;
            x.cosCone = cosf(l->spotCutoff * 3.14159265f / 180.0f); x.expo = l->spotExponent;
        }
        ls.push_back(x);
    }
    const float* S = g_w3dProbeSH;
    // la FIRMA de lo que cambia la luz: la matriz, la sonda, las lamparas y la geometria
    unsigned sig = 2166136261u;
    sig = SigWords(sig, W.m, sizeof(W.m));
    sig = SigWords(sig, S, 27 * sizeof(float));
    if (!ls.empty()) sig = SigWords(sig, &ls[0], ls.size() * sizeof(LightIndLamp));
    sig = SigWords(sig, &n, sizeof(n));
    sig = SigWords(sig, pos, (size_t)n * 3 * sizeof(GLfloat));
    sig = SigWords(sig, nor, (size_t)n * 3);
    if (sig == 0) sig = 1;   // (0 = nada calculado)
    std::vector<float>& lit = m->indirectLight;
    if (sig != m->indirectLightSig || lit.size() != (size_t)n * 3) {
        lit.resize((size_t)n * 3);
        for (int v = 0; v < n; v++) LightOfVertex(W, S, ls, &pos[v*3], &nor[v*3], &lit[(size_t)v * 3]);
        m->indirectLightSig = sig;
    }
    // el color del material sobre la luz ya calculada
    gLightIndCol.resize((size_t)n * 4);
    const bool vc = mat->vertexColor && vcol;
    for (int v = 0; v < n; v++) {
        const float* light = &lit[(size_t)v * 3];
        unsigned char* out = &gLightIndCol[(size_t)v * 4];
        for (int k = 0; k < 3; k++) {
            float c = mat->emission[k] + mat->diffuse[k] * light[k];
            if (vc) c *= vcol[v*4+k] / 255.0f;
            out[k] = (unsigned char)(c <= 0.0f ? 0 : c >= 1.0f ? 255 : (int)(c * 255.0f + 0.5f));
        }
        float a = mat->diffuse[3]; if (vc) a *= vcol[v*4+3] / 255.0f;
        out[3] = (unsigned char)(a <= 0.0f ? 0 : a >= 1.0f ? 255 : (int)(a * 255.0f + 0.5f));
    }
}

void Mesh::RenderObject() {
    // la dibuja una rutina (ver onlyRoutine en Mesh.h); en el pase de un espejo, el reflejo de su pose
    if (onlyRoutine) { if (w3dReflecting) RenderOnlyRoutineReflection(); return; }
    // en el render FINAL un objeto no-renderizable no sale (en el viewport si)
    if (w3dEngine::w3dRenderFinal && !renderable) return;
    if (w3dEngine::w3dPassProbe && !inProbes) return;   // (la sonda de reflejo de tiempo real no lo ve)
    // (P4) horneada en el lote estatico de una Collection ESTE frame: el lote ya
    // la dibujo en su tanda por material; aca no se dibuja nada.
    if (w3dBatchStamp && inStaticBatch == w3dBatchStamp) return;
    // LOS PASES (ver g_w3dPassDrawing): recorriendo el arbol, la malla se anota en el pase de calcomanias y/o en el de
    // transparentes si tiene partes de esos; si no tiene ninguna de opacos, aca no dibuja nada. (La que se edita, no:
    // se dibuja entera, con sus capas de edicion)
    if (g_w3dPassDrawing == 0 && !w3dDecalsInline && (Object*)this != g_editMesh) {
        bool has[3] = { false, false, false };
        for (size_t g = 0; g < materialsGroup.size(); g++) has[PassOf(w3dRenderSolid ? NULL : materialsGroup[g].material)] = true;
        if (has[1]) gPassPend[1].push_back(this);
        if (has[2]) gPassPend[2].push_back(this);
        if (!has[0] && !materialsGroup.empty()) return;
    }
    // LUZ SUELTA (chispa/halo/rayo aditivo): mismo trato. Va DESPUES del chequeo
    // de calcomania porque un decal aditivo es, antes que nada, una calcomania.
    if (!w3dDecalsInline && (Object*)this != g_editMesh && IsOnlyLight()) {
        gLightPend.push_back(this);
        return;
    }
    // VIDRIO / AGUA con shader escrito que lee la pantalla: al final de todo (ver gScreenPend)
    // (tambien desde el pase de transparentes: un vidrio tiene que ir despues de la copia de la pantalla)
    if ((!w3dDecalsInline || g_w3dPassDrawing >= 1) && (Object*)this != g_editMesh && IsOnlyScreen()) {
        gScreenPend.push_back(this);
        return;
    }
    const bool editActive = ((Object*)this == g_editMesh); // esta malla en Edit Mode
    // sin vertices no hay nada. Sin CARAS propias igual hay que dibujar: en Edit (edit mesh: verts+bordes), o si
    // hay malla GENERADA por modificadores (ej. Screw sobre un perfil sin caras -> la botella vive en genValid),
    // o si hay loose edges (wireframe suelto que se ve como overlay). Sino en modo objeto una malla sin caras
    // propias quedaba INVISIBLE aunque tuviera geometria generada/suelta -> "no se ve la botella".
    if (!vertex || vertexSize <= 0) return;
    const bool hasGen = (genValid && genVertex && genFaces);
    if ((!faces || facesSize < 3) && !editActive && !hasGen && looseEdges.empty()) return;
    GuardProbe guardProbe(this);   // (la luz indirecta de una sonda de luz, si hay una que alcance a la malla)
    ShadowGuard shadowGuard(this); // (las sombras en tiempo real, si las recibe)
    // ---- AUTOCULL por FRUSTUM (clave N95): una malla ENTERA fuera de la camara que esta dibujando no se
    //      skinnea ni se dibuja (en un primer plano, un cuerpo skinneado de 10k verts se deformaba y subia
    //      al GL para nada). El test lo pone el EDITOR via g_meshVisibleHook (Culling.cpp/W3dAabbVisible,
    //      que ya devuelve "visible" sin vista bindeada o en ortografica; un juego sin editor no instala el
    //      hook = sin cull). Skinneadas: AABB de la ULTIMA pose con margen (la pose avanza mientras esta
    //      afuera) y cada 30 frames culleada se RE-SKINNEA sin dibujar para refrescar el AABB. Sin cull:
    //      Edit Mode / weight-vertex paint (inspectores) y la malla generada por modificadores (el AABB
    //      cacheado es de la BASE, un Array desplazado lo desbordaria).
    {
        extern bool g_meshAutocull;
        extern bool (*g_meshVisibleHook)(const Vector3&, const Vector3&);
        if (g_meshAutocull && g_meshVisibleHook && !editActive && !weightPaintOn && !vertexPaintOn && !hasGen) {
            bool test = false; Vector3 mnL(0,0,0), mxL(0,0,0);
            if (skinArmature) {
                // UN SALTO DE CUADRO (arrastrar la linea de tiempo, ir al principio, la vuelta del loop) deja la caja
                // de la pose de ANTES, que puede estar fuera de cuadro aunque el personaje ya volvio: se culleaba y no
                // se volvia a skinnear hasta 30 cuadros despues (se veia el esqueleto y la malla aparecia tarde). En
                // pausa cualquier cambio de cuadro o de pose re-skinnea antes de probar (se dibuja a demanda: barato);
                // reproduciendo, solo un salto de mas de 2 cuadros (avanzar de a uno mantiene el ahorro del N95)
                if (skinAabbOk) {
                    extern bool PlayAnimation; extern int CurrentFrame;
                    const int jump = CurrentFrame - lastSkinFrame;
                    const bool newPose = skinPoseSerial != skinArmature->poseSerial;
                    if ((!PlayAnimation && (jump != 0 || newPose)) || jump > 2 || jump < -2) {
                        extern void SkinMesh(Mesh*); SkinMesh(this);   // refresca pose + AABB
                    }
                }
                if (skinAabbOk) {
                    const float dx = (skinAabbMax.x-skinAabbMin.x)*0.15f + 0.05f;
                    const float dy = (skinAabbMax.y-skinAabbMin.y)*0.15f + 0.05f;
                    const float dz = (skinAabbMax.z-skinAabbMin.z)*0.15f + 0.05f;
                    mnL = Vector3(skinAabbMin.x-dx, skinAabbMin.y-dy, skinAabbMin.z-dz);
                    mxL = Vector3(skinAabbMax.x+dx, skinAabbMax.y+dy, skinAabbMax.z+dz);
                    test = true;
                }
            } else if (aabbOk) { mnL = aabbMin; mxL = aabbMax; test = true; }
            if (test) {
                // el hook testea el AABB en coords LOCALES contra las matrices VIGENTES del pase
                // (el ModelView ya trae el transform de este objeto): sin esquinas a mundo aca.
                if (!g_meshVisibleHook(mnL, mxL)) {
                    if (skinArmature && ++consecutiveSkinCull >= 30) {
                        consecutiveSkinCull = 0;
                        extern void SkinMesh(Mesh*); SkinMesh(this); // refresca pose + AABB, sin dibujar
                    }
                    return;
                }
                consecutiveSkinCull = 0;
            }
        }
    }
    w3dEngine::g_statMeshes++; // estadistica de frame (bench): mallas que llegan a dibujarse
    // GRABADOR de visibilidad (harness 'cullcamaras'): registra el NOMBRE de cada malla que pasa el
    // autocull y llega a dibujarse, acumulado sobre la cinematica -> lo que las camaras VEN de verdad.
    // Lo no visto nunca se puede borrar del .w3d (el exterior, el fondo tapado). g_cullRecord lo prende
    // el comando; NULL/false = costo cero en el juego real.
    extern bool g_cullRecord; extern void W3dCullViewRegister(const char*);
    if (g_cullRecord) W3dCullViewRegister(this->name.c_str());
    // el material por defecto SIEMPRE tiene que existir (en Symbian arranca NULL)
    if (!MaterialDefault) MaterialDefault = FactoryMaterial(new Material("Default", true));
    namespace gfx = w3dEngine;
    // (P1) ACA vivia un gfx::Invalidate() POR MALLA: anulaba el cache ~200 veces
    // por frame y cada Enable/Disable/DepthMask/TexEnv del material volvia a ir
    // al driver. El resync ahora es UNA vez por frame, antes del traversal de la
    // escena (ViewPort3D), y NADA del pase de escena puede tocar GL crudo (lo
    // vigila gfx::AuditState en el harness 'glaudit').
    // El editor de Symbian todavia tiene llamadas GL crudas alrededor de las mallas (el render propio de la
    // plataforma): ahi el resync por malla SIGUE (g_invalidatePerMesh). En el MODO JUEGO PURO no hay nada de eso
    // (el recorrido es el mismo de PC) y la plataforma lo apaga: cada malla re-mandaba todo su estado al driver
    if (gfx::g_invalidatePerMesh) gfx::Invalidate();

    // SKINNING: si hay esqueleto asignado, deformar a la pose (posBuf = skinVertex). Posiciones + normales rotadas.
    GLfloat* posBuf = vertex;
    GLbyte*  norBuf = normals;
    if (skinArmature) {
        // normales rotadas SOLO si algun mesh part tiene luz (sino no vale la pena; N95). w3dRenderNoLight -> todo unlit.
        // El pase Normal View tambien las necesita (colorea por normal en view-space) aunque el material sea unlit.
        // (el TOON tambien: su rampa se lee con la normal posada)
        bool anyLight = false;
        if (!w3dRenderNoLight) for (size_t g = 0; g < materialsGroup.size(); g++)
            if (materialsGroup[g].material && (materialsGroup[g].material->lighting || materialsGroup[g].material->toon)){ anyLight = true; break; }
        skinWithLight = anyLight || w3dRenderNormalColor;
        // medicion para el log PERF del device (SkeletalAnimation.cpp): sin reloj instalado, costo cero
        extern unsigned int (*g_skinClockMs)();
        extern int g_skinPerfMs, g_skinPerfSkins, g_completeSkinPerf, g_skinLastPath;
        const unsigned int _skinT0 = g_skinClockMs ? g_skinClockMs() : 0;
        extern void SkinMesh(Mesh*); SkinMesh(this);
        if (g_skinClockMs) {
            g_skinPerfMs += (int)(g_skinClockMs() - _skinT0);
            if (g_skinLastPath >= 2) { g_skinPerfSkins++; if (g_skinLastPath == 4) g_completeSkinPerf++; }
        }
        if (skinVertex) posBuf = skinVertex;
        if (skinWithLight && skinNormals) norBuf = skinNormals;
    }

    // SOMBREADO TOON: la coordenada de la rampa de cada vertice sale de la normal ya posada (una vez por dibujo: la pose,
    // la luz y el objeto se mueven). La malla generada (hasGen) no lo tiene: sus materiales toon se ven sin la rampa
    {
        bool anyToon = false;
        if (!w3dRenderSolid && !w3dRenderNoLight && !hasGen) for (size_t g = 0; g < materialsGroup.size(); g++)
            if (materialsGroup[g].material && materialsGroup[g].material->toon) { anyToon = true; break; }
        if (anyToon) UpdateToonUV(norBuf);
        else if (!toonUV.empty()) toonUV.clear();
    }

    // EL @vertex DE UN SHADER ESCRITO EN LA CPU (objects/ShaderCPU.h): en los backends sin shaders (GLES 1.1 del N95, el
    // GL fijo) el MISMO codigo que en GLES 2.0 corre la GPU mueve aca las posiciones (y normales) de la malla, despues del
    // skinning, a un arreglo propio de este dibujo. Vale si TODAS las partes usan el mismo shader (lo comun: el agua, una
    // bandera). Con la malla generada por modificadores se mueve esa. Va de RAM (sin VBO: cambia en cada cuadro)
    bool vertCPU = false;
    struct RestoreGen {   // (la malla generada se apunta a la movida solo durante este dibujo)
        Mesh* m; GLfloat* v; GLbyte* n; bool active;
        RestoreGen(Mesh* mm) : m(mm), v(mm->genVertex), n(mm->genNormals), active(false) {}
        ~RestoreGen() { if (active) { m->genVertex = v; m->genNormals = n; } }
    } restoreGen(this);
    if (!w3dRenderSolid && !w3dRenderNormalColor && !w3dRenderAlpha && !materialsGroup.empty() && !editActive) {
        Material* vm = materialsGroup[0].material;
        const W3dVertCPU* prog = (vm && vm->shaderOn) ? W3dShaderVertexCPU(vm) : NULL;
        for (size_t g = 1; g < materialsGroup.size() && prog; g++) {
            Material* mg = materialsGroup[g].material;
            if (mg != vm && (!mg || !mg->shaderOn || mg->shaderFile != vm->shaderFile)) prog = NULL;
        }
        if (prog) {
            W3dVertCPUEntries in;
            in.time = W3dShaderClock(); in.ior = vm->ior; in.transmission = vm->transmission;
            in.roughness = vm->rtRoughness; in.metallic = vm->rtMetallic;
            const bool gen = hasGen && genVertex && genVertexSize > 0;
            const int nv = gen ? genVertexSize : vertexSize;
            const GLfloat* src = gen ? genVertex : posBuf;
            const GLbyte* srcN = gen ? genNormals : norBuf;
            vertCPUPos.resize((size_t)nv * 3);
            if (srcN) vertCPUNor.resize((size_t)nv * 3);
            if (W3dVertCPUApply(prog, in, src, (const signed char*)srcN, nv, &vertCPUPos[0], srcN ? (signed char*)&vertCPUNor[0] : NULL)) {
                vertCPU = true;
                if (gen) {
                    restoreGen.active = true;
                    genVertex = &vertCPUPos[0];
                    if (srcN) genNormals = (GLbyte*)&vertCPUNor[0];
                } else {
                    posBuf = &vertCPUPos[0];
                    if (srcN) norBuf = (GLbyte*)&vertCPUNor[0];
                }
            }
        }
    }

    // punteros de los datos de la malla (una sola vez). Si el relleno va a ir por los VBO (el caso comun) NO: el camino del
    // VBO los pisaba enseguida (los punteros de RAM y su bind 0, de mas en cada dibujo). Ahi los pone ese camino (de
    // RAM los arrays sin VBO) o, si al final no va por VBO (un esqueleto animando), PointersRAM despues del bloque del VBO
    bool anyFancy = false;   // (chrome / normal map / capas: re-apuntan arrays por pasada, van de RAM)
    if (!w3dRenderSolid) for (size_t gg = 0; gg < materialsGroup.size(); gg++){ Material* mt = materialsGroup[gg].material;
        if (mt && (mt->chrome || mt->normalMap || !mt->layers.empty() || mt->toon || mt->outline > 0.0f)) { anyFancy = true; break; }
        // (una capa de material con algo de eso tambien re-apunta arrays)
        for (size_t c = 0; c < materialsGroup[gg].layersMat.size(); c++) { Material* mc = materialsGroup[gg].layersMat[c];
            if (mc && (mc->chrome || mc->normalMap || !mc->layers.empty() || mc->toon)) { anyFancy = true; break; } }
        if (anyFancy) break; }
    if (vertCPU) anyFancy = true;   // (las posiciones del @vertex cambian en cada cuadro: de RAM, sin VBO)
    // LA LUZ INDIRECTA POR VERTICE (sin luz por pixel, con una sonda): los colores salen de la CPU -> de RAM
    bool lightIndCPU = false;
    if (g_w3dProbeHas && !w3dRenderSolid && !gfx::ShadingPerPixelSupported())
        for (size_t gg = 0; gg < materialsGroup.size(); gg++)
            if (materialsGroup[gg].material && materialsGroup[gg].material->lightIndirect) { lightIndCPU = true; break; }
    if (lightIndCPU) anyFancy = true;
    const bool maybeVBO = gfx::g_pointersCache && gfx::VBOSupported() && !hasGen && !weightPaintOn && !vertexPaintOn &&
                           !editActive && !w3dRenderWireframe && !w3dRenderNormalColor && !w3dRenderNoLight && !w3dRenderAlpha &&
                           !anyFancy && faces && facesSize >= 3;
    if (!maybeVBO) PointersRAM(posBuf, norBuf, vertexColor, NULL);
    if (uv) { gfx::EnableArray(gfx::TexCoordArray); if (!maybeVBO) gfx::TexCoordPointer2f(0, uv); }

    // WEIGHT PAINT: la malla se dibuja SOLIDA con el degradado de peso por vertice (black->amarillo->red), sin luz
    // ni textura. Es un inspector visual del peso del grupo activo (editar viene despues). Reemplaza el render normal.
    if (weightPaintOn && !weightPaintColor.empty() && faces && facesSize >= 3) {
        gfx::Disable(gfx::Lighting);
        gfx::DisableArray(gfx::NormalArray);
        gfx::Disable(gfx::Texture2D);
        gfx::DisableArray(gfx::TexCoordArray);
        gfx::TexEnvAlphaOnly(false);
        gfx::Disable(gfx::Blend);
        gfx::Enable(gfx::DepthTest);
        gfx::Enable(gfx::CullFace);
        gfx::EnableArray(gfx::ColorArray);
        // OJO: este pase dibuja la malla BASE aunque haya modificadores. No es un descuido: el degradado
        // es por CONTROL-POINT (vertCtrlPoint) y la malla generada no tiene esos pesos -- el stack
        // interpola color y uv, no peso. Mostrar la generada pide primero pasar el peso por el stack.
        gfx::ColorPointer4ub(&weightPaintColor[0]);
        gfx::VertexPointer3f(0, posBuf);
        gfx::DrawTriangles(facesSize, faces);
        gfx::DisableArray(gfx::ColorArray);
        gfx::Invalidate();
        return;
    }

    // VERTEX PAINT: la malla se dibuja SOLIDA con el VERTEX COLOR de la capa activa, sin luz ni
    // textura. Es el inspector de lo que se esta pintando: con el material y las luces encima no
    // se distingue el color propio del sombreado. Misma forma que el pase de weight paint.
    if (vertexPaintOn && vertexColor && faces && facesSize >= 3) {
        gfx::Disable(gfx::Lighting);
        gfx::DisableArray(gfx::NormalArray);
        gfx::Disable(gfx::Texture2D);
        gfx::DisableArray(gfx::TexCoordArray);
        gfx::TexEnvAlphaOnly(false);
        gfx::Disable(gfx::Blend);
        gfx::Enable(gfx::DepthTest);
        gfx::Enable(gfx::CullFace);
        gfx::EnableArray(gfx::ColorArray);
        // Igual que el pase de peso: con el stack vivo se dibuja la GENERADA. El vertex color por corner
        // lo interpolan los modificadores (genColor), asi que aca no hay nada que mapear.
        const bool genCol = genValid && genVertex && genFaces && genFacesSize >= 3 && genColor;
        gfx::ColorPointer4ub(genCol ? genColor : vertexColor);
        gfx::VertexPointer3f(0, genCol ? genVertex : posBuf);
        gfx::DrawTriangles(genCol ? genFacesSize : facesSize, genCol ? genFaces : faces);
        gfx::DisableArray(gfx::ColorArray);
        if (genCol) gfx::VertexPointer3f(0, posBuf);
        gfx::Invalidate();
        return;
    }

    // PASES PLANOS (Normal View / ZBuffer / Alpha): la malla se dibuja UNLIT con un COLOR PLANO y, para
    // los materiales TRANSPARENTES, se usa SOLO el alpha de su textura (el COLOR de la textura NO se
    // muestra) via TexEnvAlphaOnly. El editor elige el modo con estos flags; el Core solo sabe "dibujar
    // plano con alpha-only". NO aplica luz, color de textura, chrome, normal map ni capas -> liviano (N95).
    //   - Normal View (w3dRenderNormalColor): color = normal en VIEW-SPACE por vertice (se recalcula por
    //     frame -> "gira" con la camara, util para composicion). Malla BASE.
    //   - ZBuffer / Alpha (w3dRenderNoLight / w3dRenderAlpha): color = BLANCO. El editor le pone fog al
    //     ZBuffer (degrade de profundidad); el Alpha va SIN fog = matte de cobertura. Soporta malla generada.
    {
    const bool passNormal = w3dRenderNormalColor;
    const bool passWhite  = w3dRenderNoLight || w3dRenderAlpha;
    if ((passNormal || passWhite) && faces && facesSize >= 3) {
        gfx::Disable(gfx::Lighting);
        gfx::DisableArray(gfx::NormalArray);
        gfx::Enable(gfx::DepthTest);

        // normal usa la malla BASE (color por vertice tiene que matchear la geometria); el blanco puede
        // usar la malla GENERADA por modificadores (color uniforme -> sin problema de indices). posBuf =
        // skinVertex si hay esqueleto -> los 3 pases muestran la malla DEFORMADA por la pose, no el bind.
        const bool useGen = passWhite && (genValid && genVertex && genFaces);
        gfx::VertexPointer3f(0, useGen ? genVertex : posBuf);

        const bool normalShader = passNormal && norBuf && gfx::PassesWithShader();
        if (normalShader) {
            // CON SHADERS (ES 2.0 / Vulkan): las normales van como siempre y el pase las pasa a color en cada pixel
            gfx::DisableArray(gfx::ColorArray);
            gfx::EnableArray(gfx::NormalArray); gfx::NormalPointer3b(norBuf);
            gfx::SpecialPass(gfx::PassNormal, NULL);
        } else if (passNormal && norBuf) {
            static std::vector<unsigned char> nvcol; // reusado entre frames (sin alloc)
            nvcol.resize((size_t)vertexSize * 4);
            Matrix4 W; GetWorldMatrix(W);
            Vector3 cr = g_renderCamRight, cu = g_renderCamUp, cf = g_renderCamForward;
            for (int i = 0; i < vertexSize; i++) {
                float lx = norBuf[i*3+0]/127.0f, ly = norBuf[i*3+1]/127.0f, lz = norBuf[i*3+2]/127.0f;
                Vector3 wn(W.m[0]*lx + W.m[4]*ly + W.m[8]*lz,   // object-space -> mundo (sin traslacion)
                           W.m[1]*lx + W.m[5]*ly + W.m[9]*lz,
                           W.m[2]*lx + W.m[6]*ly + W.m[10]*lz);
                wn = wn.Normalized();
                float ex = wn.Dot(cr), ey = wn.Dot(cu), ez = -wn.Dot(cf); // eye-space (-forward = +Z)
                nvcol[i*4+0] = (unsigned char)((ex*0.5f + 0.5f) * 255.0f); // -1..1 -> 0..255
                nvcol[i*4+1] = (unsigned char)((ey*0.5f + 0.5f) * 255.0f);
                nvcol[i*4+2] = (unsigned char)((ez*0.5f + 0.5f) * 255.0f);
                nvcol[i*4+3] = 255;
            }
            gfx::EnableArray(gfx::ColorArray); gfx::ColorPointer4ub(&nvcol[0]);
        } else {
            gfx::DisableArray(gfx::ColorArray);
            gfx::Color4f(1.0f, 1.0f, 1.0f, 1.0f); // ZBuffer / Alpha = BLANCO
        }

        // POR GRUPO de material: transparente -> textura con SOLO alpha (blend); opaco -> color plano solido
        size_t ng = useGen ? genMaterialsGroup.size() : materialsGroup.size();
        for (size_t g = 0; g < ng; g++) {
            const MaterialGroup& grp = useGen ? genMaterialsGroup[g] : materialsGroup[g];
            Material* mat = grp.material ? grp.material : MaterialDefault;
        if (!mat) continue;
            bool trans = mat->transparent && mat->texture && mat->textureOn && (useGen ? (genUV != 0) : (uv != 0));
            if (trans) {
                gfx::Enable(gfx::Texture2D);
                gfx::BindTexture(mat->texture->iID);
                gfx::TexFilter(!gfx::PixelatedGlobal() && mat->filtered); gfx::TexWrap(mat->repeat);
                gfx::EnableArray(gfx::TexCoordArray);
                gfx::TexCoordPointer2f(0, useGen ? genUV : uv);
                gfx::TexEnvAlphaOnly(true);          // RGB = color plano, ALPHA = alpha de la textura
                gfx::Enable(gfx::Blend); gfx::BlendAlpha();
            } else {
                gfx::Disable(gfx::Texture2D);
                gfx::TexEnvAlphaOnly(false);
                gfx::DisableArray(gfx::TexCoordArray);
                gfx::Disable(gfx::Blend);            // opaco: color plano solido (rapido)
            }
            if (mat->culling) gfx::Enable(gfx::CullFace); else gfx::Disable(gfx::CullFace);
            if (useGen) gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
            else        gfx::DrawTriangles(grp.indicesDrawnCount, &faces[grp.startDrawn]);
        }

        // limpieza: que el estado del pase plano no leakee al proximo objeto / overlay
        if (normalShader) { gfx::SpecialPass(gfx::PassNone, NULL); gfx::DisableArray(gfx::NormalArray); }
        gfx::TexEnvAlphaOnly(false);
        gfx::Disable(gfx::Texture2D);
        gfx::Disable(gfx::Blend);
        gfx::DisableArray(gfx::ColorArray);
        gfx::DisableArray(gfx::TexCoordArray);
        if (useGen) gfx::VertexPointer3f(0, posBuf); // restaura las posiciones base
        return;
    }
    }

    // WIREFRAME: usa los BORDES precalculados (mas barato que el wireframe de
    // triangulos). Verde si esta seleccionada, gris si no. Sin bordes -> fallback.
    if (w3dRenderWireframe) {
        if (editActive && w3dRenderOverlays) {
            // en wireframe NO hay relleno que tape el fondo: el overlay de edicion
            // (lineas con vertex color + vertices) se ve entero (todos los puntos).
            // sin overlays (limpieza de pantalla) cae al wireframe plano de abajo.
            RenderEditOverlay();
        } else {
        int cid = !select ? RC_wireframe
                          : (((Object*)this == ActiveObj) ? RC_selActive : RC_selInactive);
        const float* col = gRenderColors[cid];
        if (w3dRenderWireframeOnlyVisible && pvsFaces) {
            // "SOLO LO QUE SE DIBUJA" (default): con visibilidad por triangulo activa
            // (modificador Culling: pvsFaces es la lista de la celda/sector) las lineas
            // se arman DE ESA LISTA, no de todas las aristas -- lo que el relleno
            // dibuja es lo que el alambre muestra. Celda vacia = ninguna linea (igual
            // que el relleno; si el archivo declara sectorFallback, pvsFaces ya trae el
            // paracaidas y aca ni se nota). Aristas deduplicadas (W3dDrawLinesMat).
            gfx::Disable(gfx::Lighting); gfx::Disable(gfx::Texture2D);
            gfx::Disable(gfx::Blend); gfx::Disable(gfx::CullFace);
            gfx::DisableArray(gfx::ColorArray);
            gfx::DisableArray(gfx::NormalArray);
            gfx::DisableArray(gfx::TexCoordArray);
            gfx::Color4f(col[0], col[1], col[2], 1.0f);
            W3dDrawLinesMat(pvsFaces, pvsFacesSize, select ? 2.0f : 1.0f);
            gfx::Invalidate();
        } else if (!edges.empty() || W3dMeshEdgesView(this)) {
            // (la malla de un RECURSO con la edicion pendiente dibuja las aristas COMPARTIDAS del
            // recurso: el mismo alambre que editada, sin cargar su edicion)
            RenderBorders(col, select ? 2.0f : 1.0f, false);
        } else {
            gfx::Disable(gfx::Lighting); gfx::Disable(gfx::Texture2D);
            gfx::Disable(gfx::Blend); gfx::Disable(gfx::CullFace);
            gfx::DisableArray(gfx::ColorArray);
            gfx::Color4f(col[0], col[1], col[2], 1.0f);
            gfx::Wireframe(true);
            gfx::DrawTriangles(facesSize, faces);
            gfx::Wireframe(false);
        }
        }
    } else {
        // un dibujo por grupo. El material se aplica SOLO cuando CAMBIA (en Solid
        // es siempre el mismo -> una vez). Solid = material por defecto sin
        // texturas; ZBuffer = sin luz (solo profundidad).
        const bool solid     = w3dRenderSolid;
        const bool withLight = !w3dRenderNoLight;
        Material* last = NULL;
        bool nmReady = false; // los nmColors (L en tangent-space) se calculan UNA vez por frame
        // SELECCION de una malla NO EDITABLE: sin edges no hay contorno -> tinte verde ~25%
        // en el propio material (mas barato que redibujar la malla y sin z-fighting). Solo
        // editor con overlays (el hook existe): jugando o exportado nunca tinta.
        inkNoEditable = noEditable && select && w3dRenderOverlays && w3dShowSelection && g_meshOverlayHook && !solid;

        // MALLA GENERADA por modificadores: se dibuja el PREVIEW en Object Y en Edit Mode (real-time; en Edit el
        // overlay de vertices/aristas -editable- se dibuja ENCIMA -> editas el original y ves el resultado). En Edit,
        // GenerateMeshModified saltea los modificadores con showEdit=false (edicion mas rapida, N95). Draw SIMPLE.
        const bool useGen = (genValid && genVertex && genFaces);
        if (useGen) {
            gfx::VertexPointer3f(0, genVertex);
            if (genNormals) gfx::NormalPointer3b(genNormals);
            if (genColor)   gfx::ColorPointer4ub(genColor);
            if (genUV) { gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, genUV); }
        }

        // glPolygonOffset (slope-aware) sobre los RELLENOS:
        //  - edit mode: rellenos un toque atras (decal) -> las lineas/puntos a
        //    profundidad normal quedan ENCIMA del frente y el fondo lo tapa la malla.
        // OBJECT MODE seleccionado: NO se tocan los rellenos. Antes se ADELANTABAN (offset -4/-8) para que el
        // contorno quedara atras, pero eso empujaba las caras en profundidad y, con dos mallas de caras COINCIDENTES
        // (cubos pegados), la seleccionada ganaba el z-test y se veia "corrida"/adelante. Ahora el contorno se empuja
        // ATRAS con DepthRange (RenderBorders pushBack=true): misma silueta, sin mover las caras.
        const bool xrayEdit = (editActive && g_xray); // modo X-Ray (independiente de Show Overlays)
        if (xrayEdit) {
            // X-RAY: caras PLANAS semitransparentes (30%) SIN z-test -> la malla en edicion se ve "a traves" (retopo).
            // El overlay (bordes/vertices, incluso los de atras) lo dibuja el hook encima, tambien sin z-test.
            gfx::Disable(gfx::Lighting); gfx::Disable(gfx::Texture2D);
            gfx::DisableArray(gfx::ColorArray); gfx::DisableArray(gfx::NormalArray); gfx::DisableArray(gfx::TexCoordArray);
            gfx::Disable(gfx::CullFace);
            gfx::Enable(gfx::Blend); gfx::BlendAlpha();
            gfx::Disable(gfx::DepthTest); gfx::DepthMask(false);
            const float* xc = gRenderColors[RC_wireframe];
            gfx::Color4f(xc[0], xc[1], xc[2], 0.30f);
            gfx::VertexPointer3f(0, useGen ? genVertex : vertex);
            if (useGen) gfx::DrawTriangles(genFacesSize, genFaces);
            else if (faces && facesSize >= 3) gfx::DrawTriangles(facesSize, faces);
            gfx::Enable(gfx::DepthTest); gfx::DepthMask(true);
            gfx::Disable(gfx::Blend);
        } else {
        if (editActive) { gfx::Enable(gfx::PolygonOffsetFill); gfx::PolygonOffset(2.0f, 4.0f); }

        // FAST PATH VBO: la malla se dibuja desde memoria de GPU en vez de re-transferir los client-arrays por frame
        // -> gran salto de FPS orbitando (el MBX del N95 no re-lee el bus). Se sube 1 vez (o al cambiar geometria/pose)
        // y se bindean los VBOs (override de los punteros client). Cubre Solid Y Material Preview con materiales SIMPLES
        // (sin chrome/normalmap/capas, que re-bindean arrays por-corner -> esos quedan client-side). Bordes/overlays/gen
        // tambien siguen client-side. Fallback total si el driver no tiene VBOs (VBOSupported()==false).
        // (anyFancy: arriba, con los punteros)
        // (P3) W3D_OVERRIDE de visibilidad por triangulo ACTIVO: se decide ACA (antes del
        // setup del VBO) porque cambia el camino de los indices: los del subconjunto
        // van como INDICES DE CLIENTE sobre los atributos del VBO estatico. El IBO
        // estatico (la malla completa) NO SE TOCA NUNCA MAS al cambiar de celda: el
        // glBufferData por cambio de sector que vivia aca era exactamente el
        // anti-patron del stall del tiler MBX (re-especificar un buffer que el tiler
        // todavia lee del frame anterior).
        const bool usePvs = (!useGen && !editActive && pvsFaces &&
                             pvsGroups.size() == materialsGroup.size());
        bool drawVBO = false;
        unsigned int vboIdxInUse = 0;   // el IBO del dibujo en curso (el propio o el del recurso)
        vboUVInUse = 0;
        if (gfx::VBOSupported() && !useGen && !weightPaintOn && !vertexPaintOn && !editActive && !anyFancy && faces && facesSize >= 3) {
            // LA MALLA COMO RECURSO (MeshResource.h): las capas que siguen compartidas se dibujan
            // desde los VBO del RECURSO (subidos UNA vez para todos sus objetos). Una pose de
            // skinning (posBuf = skinVertex) es de ESTA instancia: esas capas van por los propios.
            unsigned comp = 0;
            if (mesh && dataComp) {
                comp = dataComp;
                if (posBuf != vertex)  comp &= ~(unsigned)W3DMD_POS;
                if (norBuf != normals) comp &= ~(unsigned)W3DMD_NOR;
                if (comp && !W3dMeshResourceEnsureVBO(mesh)) comp = 0;
            }
            sharedVbo = (unsigned char)comp;
            // un VBO propio de una capa que ahora viene del recurso es memoria de GPU muerta
            if ((comp & W3DMD_POS) && vboPos)   { gfx::DeleteBuffer(vboPos); vboPos = 0; }
            if ((comp & W3DMD_NOR) && vboNor)   { gfx::DeleteBuffer(vboNor); vboNor = 0; }
            if ((comp & W3DMD_COL) && vboCol)   { gfx::DeleteBuffer(vboCol); vboCol = 0; }
            if ((comp & W3DMD_UV) && vboUV)     { gfx::DeleteBuffer(vboUV);  vboUV = 0; }
            if ((comp & W3DMD_FACES) && vboIdx) { gfx::DeleteBuffer(vboIdx); vboIdx = 0; }
            unsigned geomShow = skinGeomVersion;
            unsigned poseBe   = skinArmature ? skinArmature->poseSerial : 0u; // pose ACTUAL del esqueleto (sube al posar/animar)
            const bool posOwn = !(comp & W3DMD_POS);
            // atributos ESTATICOS (col/uv/idx + pos/nor base): re-subir solo al cambiar la geometria
            if (vboGeomVer != geomShow || vboVertN != vertexSize || (posOwn && !vboPos)) { UploadVBO(posBuf, norBuf, false); vboGeomVer = geomShow; vboSkinFrame = lastSkinFrame; vboPoseSerial = poseBe; vboPoseSkinned = (skinArmature != NULL); }
            // POSE skinneada: el VBO de pos/nor ya tiene la pose ACTUAL? Se compara por SERIAL, no por # de frame: posar
            // un hueso o elegir un clip en el MISMO frame cambia la pose sin cambiar el frame -> el chequeo por frame la
            // daba por "ya subida" y quedaba la malla STALL (el bug: el esqueleto se movia pero la malla no).
            bool poseInVBO = (!skinArmature) || (vboPoseSerial == poseBe);
            if (skinArmature && !poseInVBO) {
                // la pose cambio desde la ultima subida. Si esta ANIMANDO (cambio TAMBIEN desde el render anterior) NO
                // re-subimos: re-especificar el VBO que el tiler del MBX todavia lee del frame previo = STALL de sync
                // (la causa del jitter de fps). Se dibuja pos/nor de client-array este frame (mismo transfer, sin stall).
                // Cuando la anim se ASIENTA (pose estable entre 2 renders) subimos 1 vez y volvemos al VBO (recupera el
                // beneficio de orbitar un personaje pausado).
                bool animatingActive = (poseBe != vboPoseSerialPrev);
                if (!animatingActive) { UploadVBO(posBuf, norBuf, true); vboSkinFrame = lastSkinFrame; vboPoseSerial = poseBe; vboPoseSkinned = true; poseInVBO = true; }
            } else if (!skinArmature && vboPoseSkinned) {
                UploadVBO(posBuf, norBuf, true); vboPoseSkinned = false; // armature removido -> re-subir el bind (sino queda la ultima pose)
            }
            vboSkinFramePrev = lastSkinFrame;
            vboPoseSerialPrev = poseBe;
            // los handles de ESTE dibujo: los del recurso para lo compartido, los propios para el resto
            const unsigned hPos = (comp & W3DMD_POS)   ? mesh->vboPos : vboPos;
            const unsigned hNor = (comp & W3DMD_NOR)   ? mesh->vboNor : vboNor;
            const unsigned hCol = (comp & W3DMD_COL)   ? mesh->vboCol : vboCol;
            const unsigned hUV  = (comp & W3DMD_UV)    ? mesh->vboUV  : vboUV;
            const unsigned hIdx = (comp & W3DMD_FACES) ? mesh->vboIdx : vboIdx;
            if (hPos && hIdx && poseInVBO) {
                // (un array sin su VBO va de RAM: lo que antes quedaba de los punteros de arriba)
                gfx::VertexVBO(hPos);
                if (norBuf && hNor)      gfx::NormalVBO(hNor);
                else if (norBuf && maybeVBO) gfx::NormalPointer3b(norBuf);
                if (vertexColor && hCol) gfx::ColorVBO(hCol);
                else if (vertexColor && maybeVBO) gfx::ColorPointer4ub(vertexColor);
                if (uv && hUV)           gfx::TexCoordVBO(hUV);
                else if (uv && maybeVBO) gfx::TexCoordPointer2f(0, uv);
                // (P3) con visibilidad activa el IBO estatico NO se bindea: los
                // indices del subconjunto van de CLIENTE (DrawTrianglesClientIdx)
                if (!usePvs) gfx::BindIndexVBO(hIdx);
                drawVBO = true;
                vboUVInUse = hUV;
                vboIdxInUse = hIdx;
            }
            // si !poseInVBO (malla skinneada animando): drawVBO=false -> cae al path CLIENT-ARRAY (pos/nor/col/uv/idx de
            // RAM, ya seteados arriba con VertexPointer3f(posBuf) etc). Sin re-spec del VBO -> sin stall del tiler.
        }
        if (maybeVBO && !drawVBO) PointersRAM(posBuf, norBuf, vertexColor, uv);   // (no fue por VBO: los de RAM ahora)
        // LA MALLA GENERADA (subdiv, screw...) DESDE LA GPU: sus VBOs se suben cuando el stack la vuelve a armar
        // (genVersion) y se dibuja desde ahi. Fuera de Edit Mode (ahi se regenera con cada cambio), sin capas/chrome
        // (re-apuntan arreglos por pasada) y sin el @vertex de la CPU (las posiciones cambian en cada cuadro)
        bool genVBO = false;
        if (useGen && gfx::VBOSupported() && !anyFancy && !editActive && !weightPaintOn && !vertexPaintOn &&
            !restoreGen.active && genVertexSize > 0 && genFacesSize >= 3) {
            if (vboGenVer != genVersion || !vboGenPos || !vboGenIdx) UploadGenVBO();
            if (vboGenPos && vboGenIdx) {
                gfx::VertexVBO(vboGenPos);
                if (genNormals && vboGenNor) gfx::NormalVBO(vboGenNor);
                if (genColor && vboGenCol)   gfx::ColorVBO(vboGenCol);
                if (genUV && vboGenUV)       gfx::TexCoordVBO(vboGenUV);
                gfx::BindIndexVBO(vboGenIdx);
                genVBO = true;
                vboUVInUse = vboGenUV;
            }
        }
        vboRenderActive = drawVBO || genVBO; // ApplyMaterial bindea el uv VBO (no client-uv) mientras esto este activo
        // LOS INDICES EN LA GPU AUNQUE LOS VERTICES VAYAN DE RAM (una malla skinneada animando, o con toon, contorno o
        // capas): el IBO es estatico (la malla completa, se sube una vez con la geometria, o el del recurso) y el dibujo
        // ya no convierte ni recorre sus indices. En el ARM11 del Galaxy Ace eso era ~4% de la CPU (dos pasadas por
        // dibujo sobre los 10 mil indices de Goku, el cuerpo y el contorno)
        unsigned int iboRam = 0;
        if (!drawVBO && !usePvs && gfx::VBOSupported() && !useGen && !weightPaintOn && !vertexPaintOn && !editActive &&
            faces && facesSize >= 3) {
            if (mesh && (dataComp & W3DMD_FACES) && W3dMeshResourceEnsureVBO(mesh) && mesh->vboIdx) iboRam = mesh->vboIdx;
            else {
                if (!vboIdx || vboIdxGeomVer != skinGeomVersion || vboIdxN != facesSize) {
                    if (!vboIdx) vboIdx = gfx::GenBuffer();
                    gfx::IndexBufferData(vboIdx, faces, facesSize * (int)sizeof(MeshIndex));
                    vboIdxN = facesSize; vboIdxGeomVer = skinGeomVersion;
                }
                iboRam = vboIdx;
            }
        }

        // override PVS/Vis (modificador Culling por triangulo): dibuja SOLO los indices
        // del sector/celda activo (usePvs se decidio arriba, antes del setup del VBO).
        // No aplica con malla generada (genValid manda) ni en Edit Mode (editActive:
        // se edita/dibuja la malla completa). pvsGroups espeja materialsGroup (mismo
        // orden), con los rangos recortados al subconjunto.
        size_t ng = useGen ? genMaterialsGroup.size() : materialsGroup.size();
        // ORDEN DE PASADA dentro de la malla: opaco (0) -> decal (1) -> transparente (2).
        // EL PASE que se dibuja (ver g_w3dPassDrawing): las partes de los otros se saltean (las dibuja su pase). Las partes
        // van en el orden de la lista: el orden de dibujado lo decide el usuario
        const int passFilter = (editActive || solid) ? -1
                             : (g_w3dPassDrawing == 0 ? (w3dDecalsInline ? -1 : 0) : g_w3dPassDrawing);
        static std::vector<size_t> orderG;   // (vacio: el orden de la lista)
        orderG.clear();
        // SOLIDO CON SHADERS (ES 2.0 / Vulkan): todo es el mismo material -> el programa chico del pase Solido y, si los
        // rangos de los grupos van uno detras del otro, un solo draw para la malla entera (en vez de uno por grupo)
        const bool solidShader = solid && gfx::PassesWithShader();
        int solidStart = -1, solidCount = 0;
        if (solidShader && !useGen && !usePvs && orderG.empty() && ng > 1) {
            solidStart = materialsGroup[0].startDrawn;
            int end = solidStart;
            for (size_t gg = 0; gg < ng && solidStart >= 0; gg++) {
                if (materialsGroup[gg].startDrawn != end) solidStart = -1;
                else end += materialsGroup[gg].indicesDrawnCount;
            }
            if (solidStart >= 0) solidCount = end - solidStart;
        }
        if (solidShader) gfx::SpecialPass(gfx::PassSolid, NULL);
        bool lightIndPlaced = false;   // (un grupo dibujo con los colores de la luz indirecta por CPU)
        for (size_t gi = 0; gi < ng; gi++) {
            const size_t g = orderG.empty() ? gi : orderG[gi];
            const MaterialGroup& grp = useGen ? genMaterialsGroup[g] : materialsGroup[g];
            const MaterialGroup& dgrp = usePvs ? pvsGroups[g] : materialsGroup[g]; // rangos que se DIBUJAN (path no-gen)
            const MeshIndex* idxDraw = usePvs ? pvsFaces : faces;
            // El material es un PUNTERO. genMaterialsGroup es un SNAPSHOT (copia) tomado al GENERAR la malla: si
            // despues se reasigna el material del mesh part, ese snapshot apunta al material VIEJO y el cambio no se
            // ve hasta regenerar (entrar a Edit Mode, mover un vert...). Para que sea responsive, el material se toma
            // LIVE de materialsGroup[g] (mismo indice de grupo; gen copia el orden). Solo cambia el material, no la geo.
            Material* mat = (useGen && g < materialsGroup.size()) ? materialsGroup[g].material : grp.material;
            if (passFilter >= 0 && PassOf(mat) != passFilter) continue;   // (de otro pase)
            if (solid || !mat) mat = MaterialDefault;
            if (lightIndPlaced) {   // (el grupo anterior dibujo con los colores de la CPU: vuelven los de la malla)
                const GLubyte* vc = useGen ? genColor : vertexColor;
                if (vc) gfx::ColorPointer4ub(vc);
                lightIndPlaced = false; last = NULL;
            }
            if (mat != last) {
                gNormalMapWithPass = !useGen;   // (la pasada DOT3 de abajo es solo del camino sin modificadores)
                ApplyMaterial(mat, withLight, solid, editActive); last = mat;
                gNormalMapWithPass = true;
            }
            // LA LUZ INDIRECTA POR VERTICE (ver IndirectLightVertices): sin la luz de GL, con los colores de la CPU
            if (lightIndCPU && !solid && mat->lightIndirect && mat->LightRealTime() && withLight) {
                if (useGen) IndirectLightVertices(this, mat, genVertex, genNormals, genColor, genVertexSize);
                else        IndirectLightVertices(this, mat, posBuf, norBuf, vertexColor, vertexSize);
                if (!gLightIndCol.empty()) {
                    gfx::Disable(gfx::Lighting); gfx::Disable(gfx::ColorMaterial);
                    gfx::EnableArray(gfx::ColorArray); gfx::ColorPointer4ub(&gLightIndCol[0]);
                    lightIndPlaced = true; last = NULL;
                }
            }
            if (solidCount > 0) {   // (solido con shaders, grupos contiguos: la malla entera de una vez)
                if (gi == 0) {
                    if (drawVBO) gfx::DrawTrianglesVBO(solidCount, solidStart);
                    else if (iboRam) { gfx::BindIndexVBO(iboRam); gfx::DrawTrianglesVBO(solidCount, solidStart); }
                    else gfx::DrawTriangles(solidCount, &faces[solidStart]);
                }
                continue;
            }
            if (useGen) { // malla generada
                // REFLEJO por SOFTWARE (equirect mode 2, o sphere-sw sin texgen): dibuja los corners NO-INDEXADOS
                // con las UV de reflejo calculadas desde la GEO GENERADA. Asi el reflejo se ve sin "aplicar" el
                // modificador. Solo se calcula para los mesh parts con chrome (UpdateChromeUVGen lo filtra + cachea).
                // matcap (mode 0) por SOFTWARE cuando no hay HW (GLES2) -> junto al equirect/sphere-sw. El matcap
                // HW (matriz de textura) solo existe en PC/N95 (HasTexGen).
                bool chromeSW = !solid && mat->chrome && (mat->reflectMode == 2 || (mat->reflectMode == 1 && !gfx::HasTexGen()) || (mat->reflectMode == 0 && !gfx::HasTexGen()));
                bool matcapHW = !solid && mat->chrome && mat->reflectMode == 0 && gfx::HasTexGen();
                bool sphereHW = !solid && mat->chrome && mat->reflectMode == 1 && gfx::HasTexGen();
                if (chromeSW && genNormals) {
                    UpdateChromeUVGen();
                    if (genChromeExpPos && genChromeExpUV && genChromeCount == genFacesSize) {
                        gfx::VertexPointer3f(0, genChromeExpPos);
                        gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, genChromeExpUV);
                        gfx::DrawTrianglesArrayFrom(grp.startDrawn, grp.indicesDrawnCount);
                        gfx::VertexPointer3f(0, genVertex); // re-bindea las posiciones gen para el proximo grupo
                        last = NULL;                        // cambiamos punteros -> re-ApplyMaterial el proximo
                        continue;
                    }
                }
                if (matcapHW && genNormals) { // matcap HW (solo PC/N95): normales como texcoords (matriz de textura ya puesta)
                    gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer3b(genNormals, genVertexSize);
                    gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
                    last = NULL; // cambiamos el puntero de texcoords -> re-ApplyMaterial el proximo
                    continue;
                }
                // sphere HW (el texgen genera las texcoords solo) o NO-chrome: draw indexado. ApplyMaterial
                // re-bindea el TexCoordPointer al uv BASE -> volvemos a genUV (sino la textura del Screw/Subdiv sale
                // con las UV del perfil, mal indexadas). Con texgen (sphereHW) NO forzamos genUV.
                if (!sphereHW && genUV) {
                    gfx::EnableArray(gfx::TexCoordArray);
                    if (genVBO && vboGenUV) gfx::TexCoordVBO(vboGenUV); else gfx::TexCoordPointer2f(0, genUV);
                }
                if (genVBO) gfx::DrawTrianglesVBO(grp.indicesDrawnCount, grp.startDrawn);
                else gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
                // LAS CAPAS DE MATERIAL de la parte, encima (ver MaterialGroup::layersMat)
                if (!solid && g < materialsGroup.size() && !materialsGroup[g].layersMat.empty()) {
                    const std::vector<Material*>& cm = materialsGroup[g].layersMat;
                    for (size_t c = 0; c < cm.size(); c++) {
                        if (!cm[c]) continue;
                        ApplyMaterial(cm[c], withLight, solid, editActive);
                        if (genUV) {
                            gfx::EnableArray(gfx::TexCoordArray);
                            if (genVBO && vboGenUV) gfx::TexCoordVBO(vboGenUV); else gfx::TexCoordPointer2f(0, genUV);
                        }
                        gfx::DepthFunc(gfx::DepthLEqual); gfx::DepthMask(false);   // (la misma superficie: no reescribe z)
                        if (genVBO) gfx::DrawTrianglesVBO(grp.indicesDrawnCount, grp.startDrawn);
                        else gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
                    }
                    gfx::DepthFunc(gfx::DepthLess); gfx::DepthMask(!gfx::w3dPassMirrorNoZ);
                    last = NULL;   // (la proxima parte vuelve a aplicar su material)
                }
                // LINEAS del material (aristas encima del relleno, mismo estado): las de POLIGONO de la malla
                // generada (sin las diagonales de la triangulacion), una vez, con el primer material que tenga Lineas
                if (!solid && mat->lines) {
                    if (!genLinesIdx.empty()) {
                        if (g == W3dFirstGroupLines(materialsGroup)) W3dDrawLinesIdx(genLinesIdx, mat->thicknessLine);
                    } else if (grp.indicesDrawnCount >= 3) W3dDrawLinesMat(&genFaces[grp.startDrawn], grp.indicesDrawnCount, mat->thicknessLine);
                    if (genVBO) gfx::BindIndexVBO(vboGenIdx);   // (las lineas desbindean el IBO: el resto dibuja de ahi)
                    last = NULL; // el trazo toco el z-func: re-aplicar el proximo material
                }
                continue;
            }
            // REFLECTION por SOFTWARE (equirect SIEMPRE, o sphere exacto en GLES1/N95): render NO INDEXADO por-corner.
            // El MATCAP (matriz de textura) y el sphere HW van por el draw INDEXADO normal (else) -> no entran aca.
            if (!solid && mat->chrome && (mat->reflectMode == 2 || (mat->reflectMode == 1 && !gfx::HasTexGen()) || (mat->reflectMode == 0 && !gfx::HasTexGen())) && chromeExpPos && chromeExpUV) {
                gfx::VertexPointer3f(0, chromeExpPos);
                gfx::TexCoordPointer2f(0, chromeExpUV);
                gfx::DrawTrianglesArrayFrom(materialsGroup[g].startDrawn, materialsGroup[g].indicesDrawnCount);
                gfx::VertexPointer3f(0, posBuf); // re-bindea las posiciones INDEXADAS para el resto/proximo grupo
                last = NULL;                     // el puntero de UV cambio -> re-ApplyMaterial el proximo grupo
            } else if (drawVBO) {
                if (usePvs) // (P3) subconjunto visible: indices de CLIENTE + atributos en VBO (cero re-subida de buffers al cambiar de celda)
                    gfx::DrawTrianglesClientIdx(dgrp.indicesDrawnCount, &pvsFaces[dgrp.startDrawn]);
                else
                    gfx::DrawTrianglesVBO(dgrp.indicesDrawnCount, dgrp.startDrawn); // malla completa: desde el IBO estatico en GPU
            } else if (iboRam) {
                gfx::BindIndexVBO(iboRam);   // (las lineas o una pasada de indices de RAM lo desbindean)
                gfx::DrawTrianglesVBO(dgrp.indicesDrawnCount, dgrp.startDrawn);
            } else {
                gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
            }
            // LINEAS del material (Material::lineas): las aristas del rango recien
            // dibujado, con el mismo estado de material. Los indices van de CLIENTE
            // (la copia en RAM existe siempre: idxDraw) sobre los atributos que
            // esten bindeados (VBO o client arrays).
            // Con las caras (faces3d): las aristas de POLIGONO de este mesh part (sin las diagonales de los quads) y las
            // SUELTAS en el primero con Lineas. Sin caras (o con el subconjunto del PVS): las de los triangulos
            static std::vector<MeshIndex> linesPoly;
            const bool byPolygon = !usePvs && W3dLinesByPolygon(this);
            if (!solid && mat->lines && (byPolygon || dgrp.indicesDrawnCount >= 3)) {
                if (byPolygon) {
                    W3dLinesPolygon(this, (int)g, g == W3dFirstGroupLines(materialsGroup), linesPoly);
                    W3dDrawLinesIdx(linesPoly, mat->thicknessLine);
                } else W3dDrawLinesMat(&idxDraw[dgrp.startDrawn], dgrp.indicesDrawnCount, mat->thicknessLine);
                // DrawLinesClientIdx desbindea el ELEMENT_ARRAY: si el resto de la
                // malla dibuja desde el IBO estatico, volver a bindearlo
                if (drawVBO && !usePvs && vboIdxInUse) gfx::BindIndexVBO(vboIdxInUse);
                last = NULL; // el trazo toco el z-func: re-aplicar el proximo material
            }

            // NORMAL MAPPING (DOT3) — pass 2 sobre la base: textura normal + N.L (color=L por vertice) en blend
            // MULTIPLY -> base * (N.L). Textura UNICA (sin multitextura) -> portable PC + N95. Excluyente con chrome.
            if (!solid && mat->normalMap && mat->normalTexture && mat->normalTexture->iID && uv && !NormalInShader(mat, withLight, solid)) {
                ComputeTangents();
                if (tangents) {
                    if (!nmReady) { UpdateNormalMapColors(g_renderLightPos); nmReady = true; } // N.L con la LUZ de la escena
                    gfx::Enable(gfx::Blend); gfx::BlendMode(1);             // Multiply (oscurece por N.L)
                    gfx::DepthFunc(gfx::DepthEqual); gfx::DepthMask(false); // misma superficie, NO re-escribe z
                    gfx::Disable(gfx::Lighting); gfx::DisableArray(gfx::NormalArray);
                    gfx::Disable(gfx::ColorMaterial);
                    gfx::Enable(gfx::Texture2D);
                    gfx::BindTexture(mat->normalTexture->iID);
                    gfx::TexFilter(!gfx::PixelatedGlobal() && mat->filtered); gfx::TexWrap(mat->repeat);
                    gfx::TexEnvDot3(true);                                  // combiner N.L
                    gfx::EnableArray(gfx::ColorArray); gfx::ColorPointer4ub(nmColors); // L como primary color
                    { const GLfloat* nuv = UVOfMap(mat, W3D_MAP_NORMAL);
                      gfx::TexCoordPointer2f(0, nuv ? nuv : uv); }            // la capa UV del normal map (default: la base)
                    gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
                    gfx::TexEnvDot3(false);
                    // el pase del espejo dibuja SIN escribir z: restaurar DepthMask(true)
                    // aca le corromperia el z-buffer a lo que venga despues
                    gfx::DepthFunc(gfx::DepthLess); gfx::DepthMask(!gfx::w3dPassMirrorNoZ);
                    gfx::Disable(gfx::Blend); gfx::BlendAlpha();
                    last = NULL; // cambio textura/color/blend -> re-ApplyMaterial el proximo grupo
                }
            }

            // CAPAS EXTRA (multi-pass: 1 draw por capa sobre la MISMA superficie con su blend). Cada capa elige su
            // UV (TexLayer::uvMap -> UVOfLayer: la base o una capa extra del render, ej. el unwrap de un lightmap) y
            // se dibuja PURA: sin luz ni color de vertice (eso ya lo aplico la base). GL 1.1 -> PC y N95 igual.
            // Con capas la malla dibuja de client arrays (anyFancy): el TexCoordPointer se puede re-apuntar.
            if (!solid && !mat->layers.empty() && uv && !(layerUnit1 == 0 && mat->layers.size() == 1)) {   // (la unica capa ya fue en la unidad 1)
                gfx::ShadowReceive(0);   // (la sombra ya fue en la base: las capas no la repiten)
                gfx::Enable(gfx::Blend);
                gfx::DepthFunc(gfx::DepthEqual); gfx::DepthMask(false); // misma superficie, NO re-escribe z
                gfx::Disable(gfx::Lighting); gfx::DisableArray(gfx::NormalArray);
                gfx::Disable(gfx::ColorMaterial); gfx::DisableArray(gfx::ColorArray);
                gfx::Color4f(1.0f, 1.0f, 1.0f, 1.0f);
                gfx::TexEnvReplace(false);
                gfx::MixTexturesOff();   // (las capas van con una unidad: la mezcla de texturas es de la base)
                bool uvForeign = false;
                for (size_t c = 0; c < mat->layers.size(); c++) {
                    const TexLayer& cap = mat->layers[c];
                    if (!cap.on || !cap.tex || (int)c == layerUnit1) continue;
                    gfx::Enable(gfx::Texture2D);
                    gfx::BindTexture(cap.tex->iID);
                    gfx::TexFilter(!gfx::PixelatedGlobal() && mat->filtered); gfx::TexWrap(mat->repeat);
                    const GLfloat* cuv = UVOfLayer(cap.uvMap);
                    gfx::TexCoordPointer2f(0, cuv ? cuv : uv); uvForeign = true;
                    // EL FACTOR de la capa (0..1) con las funciones de mezcla de siempre (GL 1.1: PC y N95 igual):
                    //   Mix: el alfa x f. Add / Subtract / Screen: el color x f.
                    //   Multiply: dst * (f*tex + 1-f)  -> BlendMode 6 (DST_COLOR, SRC_ALPHA) con color f y alfa 1-f.
                    //   Multiply 2x: dst * (2f*tex + 1-f). Hasta 50% igual que Multiply con color 2f; arriba, en DOS
                    //   dibujos: dst * (tex + (1-f)/2f) y despues todo x 2f (sin textura, con Multiply 2x).
                    float f = cap.force; if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
                    if (f <= 0.0f) continue;
                    // LA LUZ HORNEADA A MEDIAS: la base ya se dibujo con la luz de tiempo real por (1 - f) (ApplyMaterial);
                    // aca se SUMA la horneada por f: el color del material, sin luz, por la textura horneada x 2 (Multiply
                    // 2x). Con textura base van las dos juntas (la horneada en la unidad 1); sin ella, la horneada sola
                    // dos veces (el x 2)
                    if (cap.baked && f < 0.999f) {
                        gfx::BlendMode(W3D_LAYER_ADD);
                        gfx::Color4f(mat->diffuse[0] * f, mat->diffuse[1] * f, mat->diffuse[2] * f, 1.0f);
                        const bool withBase = mat->texture && mat->textureOn && mat->texture->iID && gfx::MixTexturesSupported();
                        if (withBase) {
                            gfx::BindTexture(mat->texture->iID);
                            gfx::TexFilter(!gfx::PixelatedGlobal() && mat->filtered); gfx::TexWrap(mat->repeat);
                            const GLfloat* buv = UVOfMap(mat, W3D_MAP_BASE);
                            gfx::TexCoordPointer2f(0, buv ? buv : uv);
                            gfx::MixTextures(cap.tex->iID, 1.0f, !gfx::PixelatedGlobal() && mat->filtered, mat->repeat, 2);
                            gfx::MixTexturesUV(cuv ? cuv : uv);
                            gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
                            gfx::MixTexturesOff();
                        } else {
                            gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
                            gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
                        }
                        continue;
                    }
                    const int bl = cap.blend;
                    bool second  = false;   // (Multiply 2x arriba de 50%: el dibujo de la escala)
                    // LA LUZ HORNEADA en su pasada: aca recibe la sombra de lo que se mueve (sobre su luz: ver
                    // ApplyMaterial). Su alfa es la mascara del sol, no transparencia: el alfa de la pantalla no se toca
                    const bool bake = cap.baked && f >= 1.0f && (bl == W3D_LAYER_MULTIPLY || bl == W3D_LAYER_MULTIPLY2X);
                    if (bake && gShadowObject && withLight && mat->lighting)
                        gfx::ShadowReceive((bl == W3D_LAYER_MULTIPLY2X ? 5 : 4) | (cap.maskSun ? 8 : 0));
                    if (cap.maskSun) gfx::ColorMask(true, true, true, false);
                    if (f >= 1.0f || bl == W3D_LAYER_MIX) {
                        gfx::Color4f(1.0f, 1.0f, 1.0f, f);
                        gfx::BlendMode(bl);
                    } else if (bl == W3D_LAYER_MULTIPLY) {
                        gfx::Color4f(f, f, f, 1.0f - f);
                        gfx::BlendMode(6);
                    } else if (bl == W3D_LAYER_MULTIPLY2X) {
                        if (f <= 0.5f) gfx::Color4f(2.0f * f, 2.0f * f, 2.0f * f, 1.0f - f);
                        else { gfx::Color4f(1.0f, 1.0f, 1.0f, (1.0f - f) / (2.0f * f)); second = true; }
                        gfx::BlendMode(6);
                    } else {
                        gfx::Color4f(f, f, f, 1.0f);
                        gfx::BlendMode(bl);
                    }
                    gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
                    if (bake) gfx::ShadowReceive(0);
                    if (second) {
                        gfx::Disable(gfx::Texture2D);
                        gfx::Color4f(f, f, f, 1.0f);   // (Multiply 2x: dst * 2f)
                        gfx::BlendMode(W3D_LAYER_MULTIPLY2X);
                        gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
                    }
                    if (cap.maskSun) gfx::ColorMask(true, true, true, true);
                }
                gfx::Color4f(1.0f, 1.0f, 1.0f, 1.0f);
                if (uvForeign) gfx::TexCoordPointer2f(0, uv);
                gfx::DepthFunc(gfx::DepthLess); gfx::DepthMask(!gfx::w3dPassMirrorNoZ); // idem: en el pase del espejo NO se escribe z
                gfx::Disable(gfx::Blend); gfx::BlendAlpha(); // restaura el blend func default (y la ecuacion)
                last = NULL; // textura/blend/luz cambiaron -> re-ApplyMaterial el proximo grupo
            }

            // LAS CAPAS DE MATERIAL de la parte (MaterialGroup::layersMat): cada una, otra pasada sobre la MISMA superficie
            // con su material completo (luz, textura, transparencia). Sin escribir z y con z igual-o-menor: queda encima
            // de lo que dibujo la capa de abajo y no de lo que esta delante
            if (!solid && !materialsGroup[g].layersMat.empty()) {
                const std::vector<Material*>& cm = materialsGroup[g].layersMat;
                for (size_t c = 0; c < cm.size(); c++) {
                    if (!cm[c]) continue;
                    ApplyMaterial(cm[c], withLight, solid, editActive);
                    gfx::DepthFunc(gfx::DepthLEqual); gfx::DepthMask(false);
                    if (drawVBO) {
                        if (usePvs) gfx::DrawTrianglesClientIdx(dgrp.indicesDrawnCount, &pvsFaces[dgrp.startDrawn]);
                        else gfx::DrawTrianglesVBO(dgrp.indicesDrawnCount, dgrp.startDrawn);
                    } else if (iboRam) { gfx::BindIndexVBO(iboRam); gfx::DrawTrianglesVBO(dgrp.indicesDrawnCount, dgrp.startDrawn); }
                    else gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
                }
                gfx::DepthFunc(gfx::DepthLess); gfx::DepthMask(!gfx::w3dPassMirrorNoZ);
                last = NULL;
            }
        }
        if (lightIndPlaced) {   // (los colores de la malla de vuelta)
            const GLubyte* vc = useGen ? genColor : vertexColor;
            if (vc) gfx::ColorPointer4ub(vc);
            if (!vc) gfx::DisableArray(gfx::ColorArray);
            last = NULL;
        }
        if (solidShader) gfx::SpecialPass(gfx::PassNone, NULL);
        // (E3) TRANSPARENTES EN ORDEN DE LISTA: con pvsSorted, los tris transparentes
        // NO se dibujaron en el loop de grupos (sus pvsGroups quedaron con count 0):
        // salen ACA, run por run, en el ORDEN del dato precalculado (far->near = el
        // orden del pintor). Sin sort en runtime: la lista ES el orden. Cada run es un
        // rango contiguo de pvsFaces de un solo material; el material se re-aplica solo
        // al cambiar (runs vecinos del mismo atlas no pagan nada).
        if (usePvs && pvsSorted && !pvsRuns.empty()) {
            for (size_t r = 0; r < pvsRuns.size(); r++) {
                const PvsRun& run = pvsRuns[r];
                if (run.group < 0 || run.group >= (int)materialsGroup.size() || run.count <= 0) continue;
                Material* mat = materialsGroup[run.group].material;
                if (passFilter >= 0 && PassOf(mat) != passFilter) continue;   // (de otro pase)
                if (solid || !mat) mat = MaterialDefault;
                if (mat != last) { ApplyMaterial(mat, withLight, solid, editActive); last = mat; }
                if (drawVBO) gfx::DrawTrianglesClientIdx(run.count, &pvsFaces[run.start]);
                else         gfx::DrawTriangles(run.count, &pvsFaces[run.start]);
                if (!solid && mat->lines && run.count >= 3) { // LINEAS del material
                    W3dDrawLinesMat(&pvsFaces[run.start], run.count, mat->thicknessLine);
                    last = NULL;
                }
            }
        }
        // CONTORNO del dibujo animado (Material::contorno), por CASCO INVERTIDO: la malla inflada sobre sus normales (ya
        // posadas por el skinning) se dibuja otra vez en negro, sin textura ni luz y mostrando solo las caras de ATRAS:
        // lo que asoma alrededor de la figura es el borde. El grosor crece con la distancia a la camara (ancho parejo en
        // la pantalla). Sin shaders: PC y N95 igual (a cambio de dibujar dos veces los triangulos de esos materiales)
        if (!solid && !useGen && !drawVBO && !w3dRenderNoLight && !w3dRenderNormalColor && !w3dRenderAlpha &&
            norBuf && posBuf && vertexSize > 0 && passFilter <= 0) {
            float thickness = 0.0f;
            for (size_t g = 0; g < materialsGroup.size(); g++) {
                Material* mt = materialsGroup[g].material;
                if (mt && mt->outline > thickness) thickness = mt->outline;
            }
            if (thickness > 0.0f) {
                Matrix4 W; GetWorldMatrix(W);
                const float dx = W.m[12] - g_renderCamPos.x, dy = W.m[13] - g_renderCamPos.y, dz = W.m[14] - g_renderCamPos.z;
                const float k = thickness * sqrtf(dx * dx + dy * dy + dz * dz) / 127.0f;   // (normales de 8 bits)
                outlinePos.resize((size_t)vertexSize * 3);
                GLfloat* cp = &outlinePos[0];
                for (int v = 0; v < vertexSize * 3; v++) cp[v] = posBuf[v] + norBuf[v] * k;
                gfx::Disable(gfx::Texture2D);
                gfx::MixTexturesOff();
                gfx::Disable(gfx::Lighting);
                gfx::DisableArray(gfx::NormalArray);
                gfx::DisableArray(gfx::ColorArray);
                gfx::Disable(gfx::ColorMaterial);
                gfx::AlphaTest(0.0f);
                gfx::Disable(gfx::Blend);
                gfx::Color4f(0.0f, 0.0f, 0.0f, 1.0f);
                gfx::Enable(gfx::CullFace);
                gfx::CullFrontalFaces(true);
                gfx::VertexPointer3f(0, cp);
                const MeshIndex* idxC = usePvs ? pvsFaces : faces;
                for (size_t g = 0; g < materialsGroup.size(); g++) {
                    Material* mt = materialsGroup[g].material;
                    if (!mt || mt->outline <= 0.0f) continue;
                    const MaterialGroup& dgrp = usePvs ? pvsGroups[g] : materialsGroup[g];
                    if (dgrp.indicesDrawnCount < 3) continue;
                    if (iboRam) { gfx::BindIndexVBO(iboRam); gfx::DrawTrianglesVBO(dgrp.indicesDrawnCount, dgrp.startDrawn); }
                    else gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxC[dgrp.startDrawn]);
                }
                gfx::CullFrontalFaces(false);
                gfx::VertexPointer3f(0, posBuf);
                last = NULL;
            }
        }
        if (drawVBO || genVBO) { gfx::UnbindVBOs(); vboRenderActive = false; } // volver a client-side (bordes/overlays/proximas mallas usan RAM)
        } // fin del relleno normal (else de xrayEdit)

        gfx::PolygonOffset(0.0f, 0.0f);
        gfx::Disable(gfx::PolygonOffsetFill);
        gfx::AlphaTest(0.0f);   // el recorte por alfa de un material (rejas) no leakea a la UI ni a la proxima malla
        gfx::WrittenShaderUse(0, NULL);   // (idem el shader escrito de un material)
        gfx::MaterialPBR(0.0f, -1.0f, 0.04f, 0.0f);   // (idem la luz PBR: lo que se dibuje despues con luz no la hereda)
        gfx::MaterialMaps(0, 0, 0, 0, 0);             // (idem los mapas del material)
        gfx::MixTexturesOff();   // (idem la unidad 1 de un material con mezcla de texturas)
        // el estado que puede haber dejado un material DECAL / ADITIVO: si no se restaura,
        // la proxima malla (o la UI) hereda "no escribas z" o "sumale al fondo".
        gfx::DepthMask(!gfx::w3dPassMirrorNoZ);
        gfx::BlendAlpha();
        gfx::TexMatrixMatcap(false); // CLAVE N95: resetea la matriz de textura del MATCAP. En PC el TexGenSphere(false)
                                     // de abajo la limpiaba de paso (hace glLoadIdentity), pero en el N95 TexGenSphere
                                     // es un stub (no hay texgen) -> la matriz quedaba sucia -> la UI texturada (fuente/
                                     // iconos) salia con texcoords transformadas = INVISIBLE. Sirve en los 4 OS.
        gfx::TexGenSphere(false); // resetea el chrome (que no leakee al contorno/overlays/proxima malla)
        gfx::TexEnvReplace(false); // vuelve a GL_MODULATE: sino la UI/fuente quedan sin tinte de color
        gfx::DepthFunc(gfx::DepthLess);
        if (gBackgroundActive) { gfx::DepthRange(0.0f, 1.0f); gBackgroundActive = false; } // el cielo (FONDO) deja el range tocado

        // overlays (contorno de seleccion / normales / overlay de edit): los dibuja el EDITOR
        // via hook, JUSTO tras el relleno (mismo timing que antes). NULL en una app sin editor.
        if (g_meshOverlayHook) g_meshOverlayHook(this);
    }
}


// dibuja el buffer de bordes PRECALCULADO (bordersBuf) como GL_LINES. No arma nada
// por frame: solo color + DrawLines. Lo usan el contorno de seleccion y el wireframe.
void Mesh::RenderBorders(const float* color, float width, bool pushBack) {
    namespace gfx = w3dEngine;
    // con malla GENERADA por modificadores (subdiv/screw) el contorno usa SUS aristas de poligono; sino la
    // base (bordersBuf). Sin esto, un objeto con modificador no mostraba borde de seleccion (la base no coincide).
    const bool usesGen = (genValid && !genBordersBuf.empty() && genVertex);
    const std::vector<GLfloat>* bufPtr = usesGen ? &genBordersBuf : &bordersBuf;
    GLfloat* restore = usesGen ? genVertex : vertex;
    // LA MALLA DE UN RECURSO con la edicion PENDIENTE (MeshResource.h) no tiene aristas propias:
    // mira con las COMPARTIDAS del recurso (y, si dibuja sus posiciones en reposo, con las lineas
    // del recurso ya armadas: una sola copia para todos sus objetos)
    const std::vector<int>* E = usesGen ? NULL : W3dMeshEdgesView(this);
    if (!usesGen && bordersBuf.empty()) { const std::vector<GLfloat>* rb = W3dMeshBordersView(this); if (rb) bufPtr = rb; }
    // SKINNING del CONTORNO/overlay: si la malla se deforma por esqueleto (skinArmature), el contorno tambien debe
    // seguir la pose (sino la silueta verde queda en bind mientras el render se deforma). Las condiciones que pidio
    // (Object Mode solo si seleccionado / Edit Mode solo si "Display in Edit Mode") YA estan codificadas: el
    // contorno solo se dibuja al seleccionar, y skinArmature es NULL cuando "Display in Edit Mode" esta OFF en edit.
    if (!usesGen && skinArmature && skinVertex && E && !E->empty()){
        extern int CurrentFrame;
        const std::vector<int>& ed = *E;
        // gate por (frame, poseSerial): al elegir un clip o posar en el MISMO frame la pose cambia sin cambiar el frame
        // -> sin el serial el contorno se quedaba en la pose vieja (flotando) hasta el play.
        if (lastSkinBordersFrame != CurrentFrame || skinBordersPoseSerial != skinArmature->poseSerial || (int)skinBordersBuf.size() != (int)ed.size()*3){
            skinBordersBuf.resize(ed.size()*3);
            int nv = vertexSize; // cantidad de vertices
            for (size_t e = 0; e + 1 < ed.size(); e += 2){
                int a = ed[e], b = ed[e+1];
                if (a<0||a>=nv||b<0||b>=nv){ // fuera de rango: colapsar al extremo valido (o degenerar) -> nunca dibujar al origen
                    int v = (a>=0&&a<nv)?a : ((b>=0&&b<nv)?b : -1);
                    float px = v>=0?skinVertex[v*3]:0, py = v>=0?skinVertex[v*3+1]:0, pz = v>=0?skinVertex[v*3+2]:0;
                    skinBordersBuf[e*3]=skinBordersBuf[e*3+3]=px; skinBordersBuf[e*3+1]=skinBordersBuf[e*3+4]=py; skinBordersBuf[e*3+2]=skinBordersBuf[e*3+5]=pz;
                    continue;
                }
                // ARISTA con estiramiento ANORMAL (largo skinneado >> largo bind): es un artefacto de posRep, que soldo
                // 2 verts COINCIDENTES de PIEZAS distintas (huesos que se separan) -> un extremo "salta" a la otra pieza.
                // En LISA daba las lineas estiradas al centro en los pies. La colapsamos a un punto (invisible). Un doblez
                // NORMAL de junta en una malla CONECTADA no estira 4x -> se mantiene la silueta continua (no mira huesos).
                float bxx=vertex[a*3]-vertex[b*3], byy=vertex[a*3+1]-vertex[b*3+1], bzz=vertex[a*3+2]-vertex[b*3+2];
                float sxx=skinVertex[a*3]-skinVertex[b*3], syy=skinVertex[a*3+1]-skinVertex[b*3+1], szz=skinVertex[a*3+2]-skinVertex[b*3+2];
                float bl2=bxx*bxx+byy*byy+bzz*bzz, sl2=sxx*sxx+syy*syy+szz*szz;
                if (bl2 > 1e-8f && sl2 > bl2*16.0f){ // > 4x el largo de bind -> colapsar (invisible)
                    skinBordersBuf[e*3]=skinBordersBuf[e*3+3]=skinVertex[a*3];
                    skinBordersBuf[e*3+1]=skinBordersBuf[e*3+4]=skinVertex[a*3+1];
                    skinBordersBuf[e*3+2]=skinBordersBuf[e*3+5]=skinVertex[a*3+2];
                    continue;
                }
                skinBordersBuf[e*3]=skinVertex[a*3]; skinBordersBuf[e*3+1]=skinVertex[a*3+1]; skinBordersBuf[e*3+2]=skinVertex[a*3+2];
                skinBordersBuf[e*3+3]=skinVertex[b*3]; skinBordersBuf[e*3+4]=skinVertex[b*3+1]; skinBordersBuf[e*3+5]=skinVertex[b*3+2];
            }
            lastSkinBordersFrame = CurrentFrame;
            skinBordersPoseSerial = skinArmature->poseSerial;
        }
        bufPtr = &skinBordersBuf; restore = skinVertex;
    }
    // AUTO-SYNC del contorno con una VERTEX ANIM: si estamos usando el bordersBuf base y
    // vertex[] cambio de version (EvalVertexAnim lo bumpea al deformar), re-armar el
    // contorno desde vertex[] -> la silueta sigue la pose en Modo Objeto (en Edit el
    // contorno sale del EditMesh, ya sincronizado). Editar no bumpea la version -> no dispara.
    // (sin lineas propias pero con aristas -la malla de un recurso posada por su vertex anim- se arman)
    if (bufPtr == &bordersBuf && (bordersGeomVersion != skinGeomVersion || (bordersBuf.empty() && E && !E->empty())))
        RefreshBordersFromVertex();
    const std::vector<GLfloat>& buf = *bufPtr;
    if (buf.empty() || !restore) return;
    gfx::Disable(gfx::Lighting);
    gfx::Disable(gfx::Texture2D);
    gfx::DisableArray(gfx::NormalArray);
    gfx::DisableArray(gfx::ColorArray);
    gfx::DisableArray(gfx::TexCoordArray);
    gfx::Color4f(color[0], color[1], color[2], 1.0f);
    gfx::LineWidth(width);
    // MODO OBJETO: el contorno se empuja ATRAS para que las caras del frente lo tapen y SOLO se vea la silueta
    // (no un wireframe encima). Con perspectiva la profundidad se agolpa cerca de 1.0, asi que 0.0008 no alcanzaba
    // para objetos lejanos (los bordes interiores traspasaban = "delante"). 0.02 los tapa bien en todo el rango.
    if (pushBack) gfx::DepthRange(0.02f, 1.0f);

    gfx::VertexPointer3f(0, &buf[0]);
    gfx::DrawLines((int)(buf.size()/3));

    if (pushBack) gfx::DepthRange(0.0f, 1.0f);
    gfx::LineWidth(1.0f);
    gfx::VertexPointer3f(0, restore);
    gfx::Invalidate();
}

// ============================================================================
//  CLAVE DE POSICION DE UN BORDE (sharpEdges / seamEdges)
//
//  VIVE EN EL CORE, NO EN EL EDITOR: la usa Mesh::PruneMarksNoEdge (aca
//  abajo) y el Core linkea TAMBIEN sin el editor (los ejemplos de ui/ y core/,
//  Android, WebGL, Symbian). Estuvo definida en main/edit/MeshEdit.cpp y el dia
//  que el Core la llamo se cayo el link de los 3 juegos con "referencia sin
//  definir": el editor no lo notaba porque linkea MeshEdit.cpp.
// ============================================================================
// clave de 12 bytes de una posicion (los 3 floats crudos). Dos verts en el MISMO
// lugar tienen exactamente los mismos bytes -> misma clave.
static std::string W3dPosKey12(const float* p){ char b[12]; memcpy(b, p, 12); return std::string(b, 12); }

std::string Mesh::SharpEdgeKey(const float* a, const float* b){
    std::string ka = W3dPosKey12(a), kb = W3dPosKey12(b);
    return (ka < kb) ? (ka + kb) : (kb + ka); // ordenado: el borde no tiene direccion
}

// ============================================================================
//  RE-ANCLAJE DE SHARP / SEAM AL MOVER POSICIONES
//  (ver Mesh::ReanchorMarksPos y la clase W3dMoveVerts en Mesh.h)
// ============================================================================
// re-escribe las claves de un set de marcas segun 'remap' (posicion VIEJA -> NUEVA,
// 12 bytes crudos cada una). Una clave que no esta en el mapa queda tal cual.
static void W3dReanchorMarks(std::set<std::string>& edges,
                              const std::map<std::string, std::string>& remap) {
    if (edges.empty() || remap.empty()) return;
    std::set<std::string> newValue;
    for (std::set<std::string>::const_iterator it = edges.begin(); it != edges.end(); ++it) {
        if (it->size() != 24) { newValue.insert(*it); continue; }   // clave rara: no la toco
        std::string a = it->substr(0, 12), b = it->substr(12, 12);
        std::map<std::string, std::string>::const_iterator ia = remap.find(a);
        std::map<std::string, std::string>::const_iterator ib = remap.find(b);
        if (ia != remap.end()) a = ia->second;
        if (ib != remap.end()) b = ib->second;
        if (a == b) continue;   // los dos extremos cayeron encima: la arista se colapso
        newValue.insert((a < b) ? (a + b) : (b + a));   // el borde no tiene direccion
    }
    edges.swap(newValue);
}

void Mesh::ReanchorMarksPos(const GLfloat* oldPos, int nOld) {
    if (!oldPos || !vertex) return;
    if (nOld != vertexSize) return;            // REBUILD (otra numeracion), no un move
    if (sharpEdges.empty() && seamEdges.empty()) return;
    std::map<std::string, std::string> remap;    // posicion VIEJA -> NUEVA (12 bytes crudos)
    for (int i = 0; i < nOld; i++) {
        const char* pv = (const char*)&oldPos[i * 3];
        const char* pn = (const char*)&vertex[i * 3];
        if (memcmp(pv, pn, 12) == 0) continue;   // no se movio
        // gana el PRIMERO: dos verts INDEPENDIENTES encimados que se separan dan dos
        // destinos para la misma clave vieja y no hay forma de distinguirlos (mismo
        // criterio que tenia EditMesh::PushPositions).
        remap.insert(std::make_pair(std::string(pv, 12), std::string(pn, 12)));
    }
    if (remap.empty()) return;
    W3dReanchorMarks(sharpEdges, remap);
    W3dReanchorMarks(seamEdges, remap);
}

// ----------------------------------------------------------------------------
//  PODA de las marcas que ya no son una arista de esta malla (ver Mesh.h).
//  El set de referencia son las ARISTAS REALES: edges[] (faces3d + looseEdges,
//  dedupeadas por posicion en ComputeBorders) llevadas a la misma clave de
//  POSICION con la que se guardan sharpEdges/seamEdges.
// ----------------------------------------------------------------------------
static int W3dPruneMarks(std::set<std::string>& marks, const std::set<std::string>& real) {
    if (marks.empty()) return 0;
    int thrown = 0;
    std::set<std::string> newValue;
    for (std::set<std::string>::const_iterator it = marks.begin(); it != marks.end(); ++it) {
        if (real.count(*it)) newValue.insert(*it);
        else thrown++;
    }
    marks.swap(newValue);
    return thrown;
}

// ----------------------------------------------------------------------------
//  LA MALLA ESTA POSADA POR UNA VERTEX ANIM? (contrato completo en Mesh.h)
//  ESTADO REAL: lo dice el flag que escribe el PLAYBACK, no una comparacion de
//  posiciones (que confunde "posada" con "editada sin autokey"; ver Mesh.h).
// ----------------------------------------------------------------------------
bool Mesh::PosedByVertexAnim() const {
    if (!vertex || vertexSize <= 0) return false;
    if (!posedByAnim) return false;                   // nadie la poso: EN REPOSO
    // el flag es del playback: si ya no queda NINGUNA anim con capa de posiciones,
    // nadie puede seguir moviendo vertex[] por atras (borrar la anim con la malla
    // posada convierte esa pose en la geometria de la malla).
    for (size_t a = 0; a < animations.size(); ++a) {
        const VertexAnimation* an = animations[a];
        if (!an) continue;
        for (size_t k = 0; k < an->frames.size(); ++k)
            if (an->frames[k] && an->frames[k]->positions) return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
//  POSICIONES DEL KEYFRAME BASE (alineadas con vertex[]) -- ver Mesh.h
// ----------------------------------------------------------------------------
const GLfloat* Mesh::PosBaseVertexAnim() const {
    for (size_t a = 0; a < animations.size(); ++a) {
        const VertexAnimation* an = animations[a];
        if (!an) continue;
        if (an->vcount != 0 && an->vcount != vertexSize) continue;  // frames desalineados: no sirven
        for (size_t k = 0; k < an->frames.size(); ++k)
            if (an->frames[k] && an->frames[k]->positions) return an->frames[k]->positions;
    }
    return NULL;
}

// ----------------------------------------------------------------------------
//  LAS POSICIONES EN LAS QUE VIVEN LAS MARCAS -- ver Mesh.h
// ----------------------------------------------------------------------------
const GLfloat* Mesh::RestPositions() const {
    if (!vertex || vertexSize <= 0) return vertex;
    if (!PosedByVertexAnim()) return vertex;          // el caso normal: no hay nada que pensar
    const GLfloat* base = PosBaseVertexAnim();
    // POSADA pero sin un base usable (frames desalineados a mitad de una op de
    // topologia): lo mejor que hay es vertex[]. Es el mismo lado conservador que
    // toma la poda, que en ese estado directamente no decide nada.
    return base ? base : vertex;
}

// ----------------------------------------------------------------------------
//  ESTADISTICAS DE LA GEOMETRIA -- ver Mesh.h. Es LITERALMENTE lo que hacian
//  ComputeBorders (con rep) y ComputeAABBOnly (sin rep), movido aca para que el
//  .w3db las hornee con los MISMOS bits: mismo orden de sumas, mismos floats.
// ----------------------------------------------------------------------------
void W3dComputeGeomStats(const GLfloat* v, int nV, const int* rep, W3dGeomStats& out) {
    out.grouped = 0;
    if (!v || nV <= 0) { out.ok = false; return; }
    float cgx = 0, cgy = 0, cgz = 0;
    if (rep) {
        // centro = promedio de las posiciones UNICAS (los grupos de vertice)
        for (int i = 0; i < nV; i++) if (rep[i] == i) {
            out.grouped++;
            cgx += v[i*3]; cgy += v[i*3+1]; cgz += v[i*3+2];
        }
        if (out.grouped > 0)
            out.center = Vector3(cgx / out.grouped, cgy / out.grouped, cgz / out.grouped);
        else
            out.center = Vector3(0, 0, 0);
    } else {
        // centro = promedio de TODOS los vertices (escenario cerrado: sin posRep)
        for (int i = 0; i < nV; i++) { cgx += v[i*3]; cgy += v[i*3+1]; cgz += v[i*3+2]; }
        out.center = Vector3(cgx / nV, cgy / nV, cgz / nV);
    }
    float rg2 = 0.0f;
    out.aabbMin = Vector3(v[0], v[1], v[2]);
    out.aabbMax = out.aabbMin;
    for (int i = 0; i < nV; i++) {
        float vx = v[i*3], vy = v[i*3+1], vz = v[i*3+2];
        float dx = vx - out.center.x, dy = vy - out.center.y, dz = vz - out.center.z;
        float d2 = dx*dx + dy*dy + dz*dz; if (d2 > rg2) rg2 = d2;
        if (vx < out.aabbMin.x) out.aabbMin.x = vx; if (vx > out.aabbMax.x) out.aabbMax.x = vx;
        if (vy < out.aabbMin.y) out.aabbMin.y = vy; if (vy > out.aabbMax.y) out.aabbMax.y = vy;
        if (vz < out.aabbMin.z) out.aabbMin.z = vz; if (vz > out.aabbMax.z) out.aabbMax.z = vz;
    }
    out.radius = sqrtf(rg2);
    out.ok = true;
}

int Mesh::PruneMarksNoEdge() {
    // GUARD DE MOVIMIENTO ABIERTO: vertex[] ya se movio pero las claves todavia no se
    // re-anclaron (eso pasa al CERRAR el guard). En ese estado intermedio toda marca
    // parece huerfana; podar aca borraria la marca en vez de SEGUIR al vertice.
    if (moveOpenVerts > 0) return 0;
    if (sharpEdges.empty() && seamEdges.empty()) return 0;
    // SIN GEOMETRIA (se borro todo, o no quedan ni caras ni aristas sueltas): no existe
    // NINGUNA arista -> ninguna marca puede sobrevivir. Sin esto, borrar toda la malla
    // en Edit Mode dejaba los sets vivos y el escritor avisaba en CADA guardado.
    if (!vertex || vertexSize <= 0 || (faces3d.empty() && looseEdges.empty())) {
        int n = (int)sharpEdges.size() + (int)seamEdges.size();
        sharpEdges.clear(); seamEdges.clear();
        return n;
    }
    // MALLA POSADA POR UNA VERTEX ANIM: vertex[] esta en la pose del cuadro y las claves
    // en la de REPOSO -> toda marca parece huerfana, igual que con el guard abierto. No se
    // decide nada; el proximo recalculo con la malla de vuelta en reposo poda de verdad.
    // (Va DESPUES del caso "sin geometria": que no queden caras/aristas no depende de la pose.)
    if (PosedByVertexAnim()) return 0;
    if (edges.empty()) return 0;   // hay geometria pero los bordes no estan al dia: no decido
    std::set<std::string> real;
    for (size_t i = 0; i + 1 < edges.size(); i += 2) {
        const int a = edges[i], b = edges[i + 1];
        if (a < 0 || b < 0 || a >= vertexSize || b >= vertexSize || a == b) continue;
        real.insert(SharpEdgeKey(&vertex[a * 3], &vertex[b * 3]));
    }
    if (real.empty()) return 0;
    return W3dPruneMarks(sharpEdges, real) + W3dPruneMarks(seamEdges, real);
}

W3dMoveVerts::W3dMoveVerts(Mesh* m) : mesh(m), closed(false) {
    if (!mesh) { closed = true; return; }
    // MIENTRAS EL GUARD ESTA ABIERTO la malla queda marcada como "a medio mover": si algo
    // recalcula los bordes en el medio, la poda se saltea (ver PruneMarksNoEdge). El
    // contador sube SIEMPRE (cuesta un int), no solo cuando hay marcas que snapshotear.
    mesh->moveOpenVerts++;
    // LAS POSICIONES SE VAN A ESCRIBIR EN EL LUGAR: si son las de un RECURSO compartido (o de
    // un .obj compartido), antes se copian (COW). Esta es LA puerta para mover posiciones: con
    // la copia aca, ningun "mover" puede escribir la geometria de los otros usuarios.
    mesh->UninstantiateData(W3DMD_POS);
    // GRATIS cuando no hay nada que re-anclar (el caso normal): ni copia el snapshot
    if (!mesh->vertex || mesh->vertexSize <= 0) return;
    if (mesh->sharpEdges.empty() && mesh->seamEdges.empty()) return;
    old.assign(mesh->vertex, mesh->vertex + (size_t)mesh->vertexSize * 3);
}

void W3dMoveVerts::Close() {
    if (closed) return;
    closed = true;   // ANTES de re-anclar: idempotente aunque re-entre
    if (!mesh) return;
    // 1ro RE-ANCLAR (las claves siguen al vertice), 2do bajar el contador: recien ahi la
    // malla vuelve a estar en un estado donde podar es correcto.
    if (!old.empty()) mesh->ReanchorMarksPos(&old[0], (int)(old.size() / 3));
    if (mesh->moveOpenVerts > 0) mesh->moveOpenVerts--;
}

W3dMoveVerts::~W3dMoveVerts() { Close(); }

// ============================================================================
//  W3dPosVerts -- ver el contrato completo (y el por que) en Mesh.h
//  Las posiciones y el ESTADO DE POSE viajan juntos SIEMPRE: no hay una sola
//  entrada de esta clase que toque uno de los dos sin el otro.
// ============================================================================
void W3dPosVerts::Capture(const Mesh* m) {
    pos.clear();
    posed = false;
    if (!m) return;
    if (m->vertex && m->vertexSize > 0) {
        pos.assign(m->vertex, m->vertex + (size_t)m->vertexSize * 3);
        posed = m->posedByAnim;   // <- el estado QUE CORRESPONDE a estas posiciones
    }
    // sin buffer no hay posiciones que guardar y por lo tanto tampoco pose (queda false)
}

void W3dPosVerts::WriteIn(Mesh* m) const {
    if (!m) return;
    m->UninstantiateData(W3DMD_POS, false);   // compartidas: se abandonan (se reemplazan enteras)
    delete[] m->vertex;
    m->vertex = NULL;
    if (!pos.empty()) {
        m->vertex = new GLfloat[pos.size()];
        for (size_t i = 0; i < pos.size(); i++) m->vertex[i] = pos[i];
    }
    // EL ESTADO VIAJA CON LAS POSICIONES. Sin buffer no hay pose posible.
    m->posedByAnim = m->vertex ? posed : false;
}

void W3dPosVerts::SwapWith(Mesh* m) {
    // SIN CORRESPONDENCIA POR INDICE (la topologia cambio) no se toca NADA: mezclar
    // medio buffer con el estado del otro es peor que no hacer nada.
    if (!m || !m->vertex || (int)pos.size() != m->vertexSize * 3) return;
    m->UninstantiateData(W3DMD_POS);   // COW: se escribe en el lugar
    for (size_t i = 0; i < pos.size(); i++) { GLfloat c = m->vertex[i]; m->vertex[i] = pos[i]; pos[i] = c; }
    bool c = m->posedByAnim; m->posedByAnim = posed; posed = c;
}

void W3dPosVerts::Swap(W3dPosVerts& o) {
    pos.swap(o.pos);
    bool c = posed; posed = o.posed; o.posed = c;
}
