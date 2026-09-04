#include "Mesh.h"
#include "animation/VertexAnimation.h" // ~Mesh: liberar el controlador de vertex anim (o queda colgando)
#include "animation/Armature2DAnimation.h" // ~Mesh: liberar los clips del armature 2D
#include "objects/Armature.h" // skinArmature->poseSerial (gate del VBO skinneado por pose, no por frame)
#include "w3dGraphics.h"    // abstraccion de graficos del engine (sin GL)
#include "W3dNombres.h"     // LA regla de nombres unicos (compartida por todo el editor)
#include "CameraBase.h"     // g_renderCamPos (camara del render, para el chrome equirect)
#include "RenderColors.h"   // paleta de render del CORE (sin depender de la UI)
#include <math.h> // C puro: compila en RVCT y PC por igual
#include <algorithm> // sort/unique: dedup de aristas del material con `lineas`
#include <map>    // Armature2DAplicar: peso por control-point de cada hueso 2D
#include <string.h> // memcmp: comparar los BYTES de una posicion (ReanclarMarcasPos)
#include <stdio.h>  // aviso de COW (DesinstanciarDatos) en consola

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
    //ejemplo: "edit" o "modificadorActivo" etc... eso es del Editor de Whisk3D

    skinArmature = NULL; skinVertex = NULL; skinNormals = NULL; skinVertexCap = 0; skinConLuz = true; lastSkinFrame = -999999; skinPoseSerial = 0; lastSkinBordesFrame = -999999; skinBordesPoseSerial = 0; skinFlatSig = 0; skinNCtrl = 0; skinGeomVersion = 1; bordesGeomVersion = 0; vtxAnimRecipeBase = 0; // skinning apagado por defecto
    skinCacheOn = false; skinCacheSkip = 0; skinCacheStart = 0; skinCacheEnd = -1; skinCacheConLuz = false; skinCacheSig = 0; // cache de vertex-animation (lazy, off)
    meshTipo = -1;       // no es una primitiva regenerable por defecto
    meshSize = 2.0f;
    meshSize2 = 0.0f;
    meshDepth = 2.0f;
    meshVerts = 8;
    meshVerts2 = 8;
    meshSmooth = false; // flat por defecto (cada cara su normal)
    moverVertsAbiertos = 0; // ningun guard W3dMoverVerts abierto (ver PodarMarcasSinArista)
    posadaPorAnim = false;  // nace EN REPOSO: solo el playback la posa (ver PosadaPorVertexAnim)
    overlayLcache = -1.0f; // los buffers de normales se calculan al primer uso
    vertsAgrupados = 0;
    centroGeom = Vector3(0, 0, 0);
    radioGeom = 0.0f;
    aabbMin = Vector3(0, 0, 0); aabbMax = Vector3(0, 0, 0); aabbOk = false; // sin AABB hasta CalcularBordes
    edit = NULL; // la malla de edicion se crea on-demand al entrar a Edit Mode
    uvMapActivo = -1; colorActivo = -1; grupoActivo = -1; uvGrupoActivo = -1; // sin capas hasta PoblarCapas
    armature2dActivo = -1; // armatures 2D del mesh: la lista arranca vacia
    flipbook = NULL; flipbookPropio = true; flipAplicado = -1; flipGeomVer = 0; // sin flipbook (SetUVAnimTira lo crea)
    last2dFrame = -999999; last2dAnim = -999; pose2dDirty = false; // cache de evaluacion de los clips 2D
    geoVersion = 1;        // la geometria editable arranca en la version 1 (0 = "nunca vista")
    weightPaintOn = false; // se prende en modo Weight Paint
    vertexPaintOn = false; // idem en modo Vertex Paint (dibuja el vertex color sin luz)
    modificadorActivo = -1; // stack de modificadores vacio (lo gestiona el editor)
    genVertex = NULL; genNormals = NULL; genUV = NULL; genColor = NULL; genFaces = NULL;
    genVertexSize = 0; genFacesSize = 0; genValido = false; // sin malla generada hasta que haya modificadores
    chromeExpPos = NULL; chromeExpUV = NULL; chromeExpCount = 0; chromeUVValid = false; chromeCacheEq = true; // reflejo (lazy)
    genChromeExpPos = NULL; genChromeExpUV = NULL; genChromeCount = 0; genChromeValid = false; // reflejo de la malla generada (lazy)
    tangents = NULL; nmColors = NULL; tangentsValid = false; // normal mapping (lazy)
    vboPos = vboNor = vboCol = vboUV = vboIdx = 0; vboGeomVer = 0; vboSkinFrame = -999999; vboSkinFramePrev = -999999; vboPoseSerial = 0; vboPoseSerialPrev = 0; vboVertN = 0; vboIdxN = 0; vboRenderActivo = false; vboPoseSkinneada = false; // VBOs (lazy)
    pvsFaces = NULL; pvsFacesSize = 0; pvsVersion = 0; vboPvsVer = 0; pvsOrdenado = false; // sin override PVS (malla completa; vboPvsVer retirado por P3, ver Mesh.h)
    enLoteEstatico = 0; // (P4) sin sello: la malla se dibuja sola
    noEditable = false; tintaNoEditable = false; // malla editable (el import con noEditable / el boton de la card Mesh lo prenden)
    datosRec = NULL; datosComp = 0; // geometria propia hasta que el importador la comparta (MallaDatos.h)
}

// libera el override de indices del PVS (vuelve a dibujar la malla completa).
// pvsVersion sube para que el fast-path VBO re-suba el IBO completo.
void Mesh::PvsLimpiar() {
    if (!pvsFaces && pvsGroups.empty()) return;
    delete[] pvsFaces; pvsFaces = NULL; pvsFacesSize = 0;
    pvsGroups.clear();
    pvsRuns.clear();
    pvsOrdenado = false;   // sin override no hay lista ordenada que respetar
    pvsVersion++;
}

// ===================================================
// Destructor
// ===================================================
Mesh::~Mesh() {
    //LiberarMemoria();
    // GEOMETRIA COMPARTIDA (MallaDatos.h): soltar la referencia ANTES que nada.
    // Deja en NULL las capas que no eran nuestras; el dato compartido muere
    // solo cuando su refcount llega a 0 (almacen de recursos).
    SoltarDatosCompartidos();
    // vertex anims: sacar el controlador de reproduccion (o queda COLGANDO en
    // VertexAnimationActives -> UpdateAnimations crashea) y liberar las anims que
    // posee esta malla (sus frames se van con ~VertexAnimation). El borrado por la
    // lista (Properties) ya las saca del vector, asi que aca solo quedan las vivas.
    VertexAnimLiberarControlador(this);
    for (size_t i = 0; i < animations.size(); i++) delete animations[i];
    animations.clear();
    // ARMATURES 2D: los posee la malla (y cada uno posee SUS clips, ver ~Armature2D)
    for (size_t i = 0; i < armatures2d.size(); i++) delete armatures2d[i];
    armatures2d.clear(); armature2dActivo = -1;
    QuitarUVAnimTira();   // saca la malla del registro de UpdateUVAnims (o quedaria colgando)
    LiberarCapas();
    // UV GROUPS (pesos por corner del editor UV): los posee la malla. LiberarCapas ya los
    // libera en el EDITOR, pero en el RUNTIME del juego esa funcion es un stub vacio -> el
    // borrado va aca (el vector ya vino vaciado si LiberarCapas hizo su trabajo).
    for (size_t i = 0; i < uvGroups.size(); i++) delete uvGroups[i];
    uvGroups.clear(); uvGrupoActivo = -1;
    InvalidarEdit();
    LiberarModificadores(); // definida en el editor (como InvalidarEdit): libera el stack de modificadores
    LiberarMallaModificada(); // libera las gen buffers
    delete[] skinVertex; delete[] skinNormals; // buffers de skinning (deform por esqueleto)
    LiberarSkinCache(); // snapshots del cache de vertex-animation
    delete[] pvsFaces; pvsFaces = NULL; // override de indices del PVS (modificador Culling)
    // VBOs de render (buffer objects en GPU)
    namespace gfx = w3dEngine;
    gfx::DeleteBuffer(vboPos); gfx::DeleteBuffer(vboNor); gfx::DeleteBuffer(vboCol); gfx::DeleteBuffer(vboUV); gfx::DeleteBuffer(vboIdx);
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
//  van en el registro global de UpdateFlipbooks, que solo lee uvActual sin tocar geometria).
// ===========================================================================
static std::vector<Mesh*> gUVAnimMeshes;

void Mesh::SetUVAnimTira(int frames, float fps, int eje, int desfase, float ancho, float alto) {
    if (frames < 1) frames = 1;
    if (fps < 0.0f) fps = 0.0f;
    if (eje != 1) eje = 0;
    if (desfase < 0) desfase = 0;
    if (flipbook && !flipbookPropio) QuitarUVAnimTira();   // suelta el compartido antes de crear el propio
    if (!flipbook) {
        flipbook = new Flipbook(); flipbookPropio = true;
        gUVAnimMeshes.push_back(this);
    }
    // eje u = tira horizontal (cols=frames, filas=1); eje v = vertical (cols=1, filas=frames).
    // ancho/alto (default 1) = fraccion de la textura que ocupa la tira: con la tira
    // metida en el atlas unico el paso por celda es ancho/frames, no 1/frames.
    flipbook->ConfigurarTira("", eje ? 1 : frames, eje ? frames : 1, frames, fps, ancho, alto);
    flipPlay.Set(flipbook, desfase % frames, false);   // false: la malla lo tickea (escribe uv[])
    // reconfigurar resetea la aplicacion (la base se recaptura al primer tick)
    flipAplicado = -1; flipGeomVer = 0; flipUvBase.clear();
}

// usa un flipbook COMPARTIDO con nombre (SceneFlipbooks): NO lo posee, no lo libera al quitar.
void Mesh::UsarFlipbook(Flipbook* fb, int desfase) {
    if (!fb) { QuitarUVAnimTira(); return; }
    if (flipbook && flipbookPropio) delete flipbook;   // soltar el propio anterior si lo habia
    bool reg = false;
    for (size_t i = 0; i < gUVAnimMeshes.size(); i++) if (gUVAnimMeshes[i] == this) { reg = true; break; }
    if (!reg) gUVAnimMeshes.push_back(this);
    flipbook = fb; flipbookPropio = false;
    flipPlay.Set(fb, desfase, false);
    flipAplicado = -1; flipGeomVer = 0; flipUvBase.clear();
}

void Mesh::QuitarUVAnimTira() {
    if (!flipbook) return;
    flipPlay.Set(0, 0, false);
    if (flipbookPropio) delete flipbook;   // el compartido vive en SceneFlipbooks: NO se libera aca
    flipbook = NULL; flipbookPropio = true;
    flipUvBase.clear(); flipAplicado = -1;
    for (size_t i = 0; i < gUVAnimMeshes.size(); i++)
        if (gUVAnimMeshes[i] == this) { gUVAnimMeshes.erase(gUVAnimMeshes.begin() + i); break; }
}

bool Mesh::TickUVAnimTira(float dtSeg) {
    if (!flipbook || !uv || vertexSize < 1 || flipbook->cuadros < 1) return false;
    flipPlay.Tick(dtSeg);                 // avanza el player (no esta en el registro global de UpdateFlipbooks)
    int cuadro = flipPlay.cuadroActual;
    const size_t n = (size_t)vertexSize * 2;
    // offset APLICADO (para el chequeo de 'intacto'): corner0 de la celda ya aplicada
    float apU = 0.0f, apV = 0.0f;
    if (flipAplicado >= 0) {
        float u0, v0, u1, v1; flipbook->RectDeCelda(flipAplicado, u0, v0, u1, v1); apU = u0; apV = v0;
    }
    // (re)captura de la base: primera vez, cambio de topologia, o un rebuild AJENO que regenero
    // uv[]. Una VERTEX ANIM en la misma malla sube skinGeomVersion sin tocar uv[]; recapturar a
    // ciegas arrastraria el offset aplicado (deriva) -> antes se verifica que uv[] siga siendo base+offset.
    if (flipUvBase.size() != n) {
        flipUvBase.assign(uv, uv + n);
        flipAplicado = -1;   // uv[] esta en base: nada aplicado
    } else if (flipGeomVer != skinGeomVersion) {
        bool intacto = (flipAplicado >= 0);
        for (size_t i = 0; i < n && intacto; i++) {
            const float esp = flipUvBase[i] + (((i & 1) == 0) ? apU : apV);
            if (uv[i] != esp) intacto = false;
        }
        if (!intacto) { flipUvBase.assign(uv, uv + n); flipAplicado = -1; }
        flipGeomVer = skinGeomVersion;
    }
    if (cuadro == flipAplicado) return false;
    DesinstanciarDatos(W3DMD_UV);         // COW: escribimos uv[] (geometria compartida)
    const float offU = flipPlay.uvActual[0], offV = flipPlay.uvActual[1];   // corner0 de la celda actual
    for (size_t i = 0; i < n; i += 2) {
        uv[i]     = flipUvBase[i]     + offU;
        uv[i + 1] = flipUvBase[i + 1] + offV;   // offV = 0 en una tira horizontal (no toca v)
    }
    flipAplicado = cuadro;
    skinGeomVersion++;                    // re-subir el VBO (mismo camino que EvalVertexAnim)
    flipGeomVer = skinGeomVersion;        // nuestra propia subida no invalida la base
    return true;
}

void Mesh::ReposarUVAnimTira() {
    if (!flipbook || !uv || flipAplicado < 0) return;
    const size_t n = (size_t)vertexSize * 2;
    if (flipUvBase.size() != n) return;   // base invalida: uv[] ya es lo mejor que hay
    for (size_t i = 0; i < n; i++) uv[i] = flipUvBase[i];
    flipAplicado = -1;   // el proximo tick reaplica el cuadro actual (y re-sube el VBO)
}

bool UpdateUVAnims(float dtSeg) {
    bool cambio = false;
    for (size_t i = 0; i < gUVAnimMeshes.size(); i++)
        if (gUVAnimMeshes[i] && gUVAnimMeshes[i]->TickUVAnimTira(dtSeg)) cambio = true;
    return cambio;
}

// hay alguna malla animando sus UV? (lo consulta HayAnimacionActiva, Materials.cpp)
bool HayUVAnims() {
    return !gUVAnimMeshes.empty();
}

// re-arma bordesBuf (contorno/wireframe) desde vertex[] con las aristas ACTUALES, sin
// recalcular topologia. Lo usa RenderBordes cuando una vertex anim movio vertex[] (el
// contorno estatico quedaria en la pose vieja). Marca bordesGeomVersion = skinGeomVersion.
void Mesh::RefrescarBordesDesdeVertex() {
    bordesGeomVersion = skinGeomVersion;
    if (!vertex || edges.empty()) return;
    const int nv = vertexSize;
    bordesBuf.clear(); bordesBuf.reserve(edges.size()*3);
    for (size_t e = 0; e + 1 < edges.size(); e += 2) {
        int a = edges[e], b = edges[e+1];
        if (a<0||a>=nv||b<0||b>=nv) continue;
        bordesBuf.push_back(vertex[a*3]); bordesBuf.push_back(vertex[a*3+1]); bordesBuf.push_back(vertex[a*3+2]);
        bordesBuf.push_back(vertex[b*3]); bordesBuf.push_back(vertex[b*3+1]); bordesBuf.push_back(vertex[b*3+2]);
    }
}

// libera todos los snapshots del cache de vertex-animation (posiciones + normales). Se llama al cambiar de clip,
// invalidar por geometria/rango, apagar el cache, o destruir la malla. Deja el cache vacio (skinCache.empty()).
void Mesh::LiberarSkinCache() {
    for (size_t i = 0; i < skinCache.size(); i++) { delete[] skinCache[i].pos; delete[] skinCache[i].nor; }
    skinCache.clear();
    skinCacheSig = 0; skinCacheEnd = -1;
}

// ===================================================
// Liberar memoria
// ===================================================
void Mesh::LiberarMemoria() {
    SoltarDatosCompartidos(); // capas compartidas: quedan NULL y se va la ref (MallaDatos.h)
    delete[] vertex;
    delete[] vertexColor;
    delete[] normals;
    delete[] uv;

    LiberarCapas(); // uv maps / color layers / vertex groups

    delete[] faces;
    materialsGroup.clear();
}

// ===========================================================================
//  GEOMETRIA COMPARTIDA (MallaDatos.h) — el lado del Mesh, compilado en TODAS
//  las plataformas (este .cpp esta en el .mmp). La liberacion de la referencia
//  pasa por un HOOK que MallaDatos.cpp setea al registrarse: si nadie comparte
//  (Symbian, o `mallacache off` desde el arranque) el hook queda NULL y este
//  archivo no depende del almacen de recursos.
// ===========================================================================
void (*W3dMallaDatosSoltarHook)(void*) = 0;   // lo apunta MallaDatos.cpp
int W3dMallaDesinstanciados = 0;              // contador COW (lo muestra meminfo)

void Mesh::SoltarDatosCompartidos() {
    if (!datosRec) return;
    // los punteros compartidos no son nuestros: NULLearlos para que ningun
    // delete[] posterior (LiberarMemoria, editor) toque memoria ajena
    if (datosComp & W3DMD_POS)   vertex = NULL;
    if (datosComp & W3DMD_NOR)   normals = NULL;
    if (datosComp & W3DMD_UV)    uv = NULL;
    if (datosComp & W3DMD_COL)   vertexColor = NULL;
    if (datosComp & W3DMD_FACES) faces = NULL;
    datosComp = 0;
    if (W3dMallaDatosSoltarHook) W3dMallaDatosSoltarHook(datosRec);
    datosRec = NULL;
}

void Mesh::DesinstanciarDatos(unsigned mask, bool copiar) {
    if (!datosRec) return;
    unsigned afectadas = (unsigned)datosComp & mask;
    if (!afectadas) return;
    if (copiar) {
        // COW de verdad: copiar la capa a memoria propia y recien ahi escribir
        if ((afectadas & W3DMD_POS) && vertex) {
            GLfloat* nv = new GLfloat[(size_t)vertexSize * 3];
            memcpy(nv, vertex, (size_t)vertexSize * 3 * sizeof(GLfloat));
            vertex = nv;
        }
        if ((afectadas & W3DMD_NOR) && normals) {
            GLbyte* nn = new GLbyte[(size_t)vertexSize * 3];
            memcpy(nn, normals, (size_t)vertexSize * 3 * sizeof(GLbyte));
            normals = nn;
        }
        if ((afectadas & W3DMD_UV) && uv) {
            GLfloat* nu = new GLfloat[(size_t)vertexSize * 2];
            memcpy(nu, uv, (size_t)vertexSize * 2 * sizeof(GLfloat));
            uv = nu;
        }
        if ((afectadas & W3DMD_COL) && vertexColor) {
            GLubyte* nc = new GLubyte[(size_t)vertexSize * 4];
            memcpy(nc, vertexColor, (size_t)vertexSize * 4 * sizeof(GLubyte));
            vertexColor = nc;
        }
        if ((afectadas & W3DMD_FACES) && faces) {
            MeshIndex* nf = new MeshIndex[facesSize > 0 ? facesSize : 1];
            memcpy(nf, faces, (size_t)(facesSize > 0 ? facesSize : 0) * sizeof(MeshIndex));
            faces = nf;
        }
    } else {
        // abandono: el caller va a REGENERAR la capa (la habria borrado con
        // delete[]) -> queda NULL, sin copiar nada
        if (afectadas & W3DMD_POS)   vertex = NULL;
        if (afectadas & W3DMD_NOR)   normals = NULL;
        if (afectadas & W3DMD_UV)    uv = NULL;
        if (afectadas & W3DMD_COL)   vertexColor = NULL;
        if (afectadas & W3DMD_FACES) faces = NULL;
    }
    datosComp = (unsigned char)(datosComp & ~mask);
    W3dMallaDesinstanciados++;
    if (!datosComp) {
        if (W3dMallaDatosSoltarHook) W3dMallaDatosSoltarHook(datosRec);
        datosRec = NULL;
    }
}

// ===================================================
// Renderizado
// ===================================================
// --- NORMAL MAPPING (DOT3) ---------------------------------------------------------------------------------
// Base tangente POR VERTICE de render (T en .xyz, handedness en .w). Acumula por triangulo desde pos+UV,
// ortonormaliza contra la normal y guarda. CACHE: solo recalcula si cambio la geometria/UV (tangentsValid).
void Mesh::CalcularTangentes() {
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

// L (vector a la luz) en TANGENT-SPACE por vertice -> nmColors (el "primary color" del DOT3). luzLocal = la luz
// en el espacio LOCAL de la malla. Se llama cada frame (la luz/camara se mueven). [-1,1] -> [0,1]*255.
void Mesh::ActualizarNormalMapColors(const Vector3& luzLocal) {
    if (!tangents || !normals || !vertex || vertexSize <= 0) return;
    if (!nmColors) nmColors = new GLubyte[vertexSize * 4];
    for (int v = 0; v < vertexSize; v++) {
        Vector3 N(normals[v*3]/127.0f, normals[v*3+1]/127.0f, normals[v*3+2]/127.0f); N = N.Normalized();
        Vector3 T(tangents[v*4], tangents[v*4+1], tangents[v*4+2]);
        Vector3 B = Vector3::Cross(N, T) * tangents[v*4+3];
        Vector3 P(vertex[v*3], vertex[v*3+1], vertex[v*3+2]);
        Vector3 L = (luzLocal - P).Normalized();
        float lt = L.Dot(T), lb = L.Dot(B), ln = L.Dot(N);
        nmColors[v*4]   = (GLubyte)((lt*0.5f+0.5f)*255.0f);
        nmColors[v*4+1] = (GLubyte)((lb*0.5f+0.5f)*255.0f);
        nmColors[v*4+2] = (GLubyte)((ln*0.5f+0.5f)*255.0f);
        nmColors[v*4+3] = 255;
    }
}

// === REFLEJO por SOFTWARE, matematica COMPARTIDA (la usan la malla base Y la generada) ===
// UV de UN corner: pos LOCAL + normal LOCAL -> reflejo. equirect=true: lat-long 360 en MUNDO. false: matcap
// (sphere-map en espacio de OJO, replica de GL_SPHERE_MAP). 'polo' marca el corner sobre el polo (equirect).
static void ChromeUVCorner(const Vector3& lp, const Vector3& ln, const Matrix4& W, const Vector3& cam,
                           const Vector3& cr, const Vector3& cu, const Vector3& cf, bool equirect,
                           float& u, float& v, bool& polo) {
    const float PI = 3.14159265358979f;
    Vector3 wp = W * lp;
    Vector3 wn(W.m[0]*ln.x + W.m[4]*ln.y + W.m[8]*ln.z,
               W.m[1]*ln.x + W.m[5]*ln.y + W.m[9]*ln.z,
               W.m[2]*ln.x + W.m[6]*ln.y + W.m[10]*ln.z);
    wn = wn.Normalized();
    polo = false;
    if (equirect) {
        Vector3 I = (wp - cam).Normalized();
        float dd = I.x*wn.x + I.y*wn.y + I.z*wn.z;
        Vector3 R(I.x - 2*dd*wn.x, I.y - 2*dd*wn.y, I.z - 2*dd*wn.z);
        float ry = R.y < -1.0f ? -1.0f : (R.y > 1.0f ? 1.0f : R.y);
        u = atan2f(R.z, R.x) / (2.0f*PI) + 0.5f;
        v = acosf(ry) / PI;
        polo = (ry > 0.995f || ry < -0.995f);
    } else {
        Vector3 rel = wp - cam;
        Vector3 ep(rel.x*cr.x + rel.y*cr.y + rel.z*cr.z,
                   rel.x*cu.x + rel.y*cu.y + rel.z*cu.z,
                 -(rel.x*cf.x + rel.y*cf.y + rel.z*cf.z));  // -forward = +Z del eye space
        Vector3 en(wn.x*cr.x + wn.y*cr.y + wn.z*cr.z,
                   wn.x*cu.x + wn.y*cu.y + wn.z*cu.z,
                 -(wn.x*cf.x + wn.y*cf.y + wn.z*cf.z));
        Vector3 I = ep.Normalized();
        float dd = I.x*en.x + I.y*en.y + I.z*en.z;
        Vector3 R(I.x - 2*dd*en.x, I.y - 2*dd*en.y, I.z - 2*dd*en.z);
        float mm = 2.0f * sqrtf(R.x*R.x + R.y*R.y + (R.z+1.0f)*(R.z+1.0f));
        if (mm < 1e-5f) mm = 1e-5f;
        u = R.x / mm + 0.5f;
        v = 1.0f - (R.y / mm + 0.5f); // flip-V para matchear el GL_SPHERE_MAP del PC
    }
}
// COSTURA + POLO del equirect POR TRIANGULO (3 corners): sin esto el atan2 wrap hace "tuc"/batidora.
static void ChromeSeamPolo(float u[3], const bool polo[3]) {
    float umin = 2.0f, umax = -1.0f;
    for (int k = 0; k < 3; k++) if (!polo[k]) { if (u[k] < umin) umin = u[k]; if (u[k] > umax) umax = u[k]; }
    if (umax > umin && umax - umin > 0.5f)
        for (int k = 0; k < 3; k++) if (!polo[k] && u[k] < 0.5f) u[k] += 1.0f; // GL_REPEAT -> continuo
    float sum = 0.0f; int n = 0;
    for (int k = 0; k < 3; k++) if (!polo[k]) { sum += u[k]; n++; }
    if (n > 0) { float avg = sum / n; for (int k = 0; k < 3; k++) if (polo[k]) u[k] = avg; } // polo = U promedio
}

// CHROME EQUIRECTANGULAR 360 (calidad, para renders): calcula por SOFTWARE la UV del reflejo de cada
// vertice (ChromeUVCorner + ChromeSeamPolo). CACHE: solo recalcula si cambio la camara, la matriz de
// mundo o la geometria -> en una toma estatica cuesta CERO; solo "paga" al orbitar (clave para el N95).
void Mesh::ActualizarChromeUV(bool equirect) {
    if (vertexSize <= 0 || !vertex || !normals || facesSize <= 0 || !faces) return;
    Matrix4 W; GetWorldMatrix(W);
    Vector3 cam = g_renderCamPos;
    // base de camara (para el MATCAP, espacio de OJO): right/up/forward en mundo
    Vector3 cr = g_renderCamRight, cu = g_renderCamUp, cf = g_renderCamForward;
    // cache hit: mismo modo + misma camara + misma matriz de mundo + misma cantidad de corners -> nada que hacer
    if (chromeUVValid && chromeExpUV && chromeExpCount == facesSize && chromeCacheEq == equirect) {
        bool igual = (chromeCacheCam.x == cam.x && chromeCacheCam.y == cam.y && chromeCacheCam.z == cam.z);
        for (int i = 0; igual && i < 16; i++) if (chromeCacheW[i] != W.m[i]) igual = false;
        if (igual) return;
    }
    if (!chromeExpUV || chromeExpCount != facesSize) {
        delete[] chromeExpPos; chromeExpPos = new GLfloat[facesSize * 3];
        delete[] chromeExpUV;  chromeExpUV  = new GLfloat[facesSize * 2];
        chromeExpCount = facesSize;
    }
    // POR TRIANGULO (3 corners): el EQUIRECT corrige costura/polo por-cara; el MATCAP no las tiene (disco).
    for (int t = 0; t + 2 < facesSize; t += 3) {
        float u[3], v[3]; bool polo[3]; Vector3 lp[3];
        for (int k = 0; k < 3; k++) {
            int vi = faces[t + k];
            lp[k] = Vector3(vertex[vi*3], vertex[vi*3+1], vertex[vi*3+2]);
            Vector3 ln(normals[vi*3]/127.0f, normals[vi*3+1]/127.0f, normals[vi*3+2]/127.0f);
            ChromeUVCorner(lp[k], ln, W, cam, cr, cu, cf, equirect, u[k], v[k], polo[k]);
        }
        if (equirect) ChromeSeamPolo(u, polo);
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

// CHROME sobre la MALLA GENERADA (screw/subdiv): mismas UV de reflejo que ActualizarChromeUV pero leyendo la geo
// GENERADA (genVertex/genNormals/genFaces), y SOLO para los mesh parts con reflejo. Si ningun part tiene chrome NO
// hace NADA (costo 0, no recalcula frame a frame). Cache por camara/mundo -> solo "paga" al orbitar. NO toca la geo 3D.
void Mesh::ActualizarChromeUVGen() {
    if (!genValido || !genVertex || !genNormals || !genFaces || genFacesSize <= 0) return;
    // hay algun mesh part con reflejo? (material LIVE de materialsGroup, mismo indice de grupo)
    bool hayChrome = false;
    for (size_t g = 0; g < genMaterialsGroup.size(); g++) {
        Material* mat = (g < materialsGroup.size()) ? materialsGroup[g].material : genMaterialsGroup[g].material;
        if (mat && mat->chrome) { hayChrome = true; break; }
    }
    if (!hayChrome) return; // sin reflejo: no se recalcula nada
    Matrix4 W; GetWorldMatrix(W);
    Vector3 cam = g_renderCamPos;
    Vector3 cr = g_renderCamRight, cu = g_renderCamUp, cf = g_renderCamForward;
    // cache hit: misma camara + mismo mundo + misma cantidad de corners -> nada que hacer
    if (genChromeValid && genChromeExpUV && genChromeCount == genFacesSize) {
        bool igual = (genChromeCam.x == cam.x && genChromeCam.y == cam.y && genChromeCam.z == cam.z);
        for (int i = 0; igual && i < 16; i++) if (genChromeW[i] != W.m[i]) igual = false;
        if (igual) return;
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
            float u[3], v[3]; bool polo[3]; Vector3 lp[3];
            for (int k = 0; k < 3; k++) {
                int vi = genFaces[t + k];
                lp[k] = Vector3(genVertex[vi*3], genVertex[vi*3+1], genVertex[vi*3+2]);
                Vector3 ln(genNormals[vi*3]/127.0f, genNormals[vi*3+1]/127.0f, genNormals[vi*3+2]/127.0f);
                ChromeUVCorner(lp[k], ln, W, cam, cr, cu, cf, equirect, u[k], v[k], polo[k]);
            }
            if (equirect) ChromeSeamPolo(u, polo);
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
static bool gFondoActivo = false;

// ===================================================
// aplica TODO el estado GL de un material, leyendolo DEL material (nada
// hardcodeado). RenderObject la llama solo cuando el material cambia.
void Mesh::AplicarMaterial(Material* mat, bool conLuz, bool solido, bool offsetEdit) {
    namespace gfx = w3dEngine;
    // X-RAY: la malla se dibuja al 50% para poder ver lo que queda detras. Se aplica sobre el
    // ALPHA (copias locales: el material REAL no se toca), asi vale igual en Solid, Material y
    // Rendered sin duplicar cada camino de dibujo.
    const float xrayA = g_xray ? 0.5f : 1.0f;
    const float amb[4] = { mat->ambient[0],  mat->ambient[1],  mat->ambient[2],  mat->ambient[3]  * xrayA };
    const float dif[4] = { mat->diffuse[0],  mat->diffuse[1],  mat->diffuse[2],  mat->diffuse[3]  * xrayA };
    const float emi[4] = { mat->emission[0], mat->emission[1], mat->emission[2], mat->emission[3] * xrayA };
    gfx::SmoothShading(true); // el look suave/plano lo dan las NORMALES de la malla, no el material
    gfx::Material(gfx::MatAmbient,  amb);
    gfx::Material(gfx::MatDiffuse,  dif);
    gfx::Material(gfx::MatSpecular, mat->specular);
    gfx::Material(gfx::MatEmission, emi);
    gfx::MaterialShininess(mat->shininess);

    // color por vertice (via ColorMaterial) o el difuso plano del material
    if (tintaNoEditable) {
        // SELECCION de malla no editable: 75% material + 25% verde, plano (el vcolor cede:
        // el tinte tiene que VERSE). Reemplaza al contorno de seleccion, que no existe sin edges.
        float td[4] = { mat->diffuse[0]*0.75f + 0.075f, mat->diffuse[1]*0.75f + 0.25f,
                        mat->diffuse[2]*0.75f + 0.075f, dif[3] };
        gfx::Material(gfx::MatDiffuse, td);
        gfx::Color4f(td[0], td[1], td[2], td[3]);
        gfx::DisableArray(gfx::ColorArray);
        gfx::Disable(gfx::ColorMaterial);
    } else if (mat->vertexColor && vertexColor) {
        gfx::Color4f(0.0f, 0.0f, 0.0f, 1.0f); // el color real lo pone el array
        gfx::EnableArray(gfx::ColorArray);
        gfx::Enable(gfx::ColorMaterial);
    } else {
        // con NORMAL MAP la base va sin luz -> la tiño con el COLOR de la luz aca (sino el N.L sale BLANCO).
        if (mat->normalMap)
            gfx::Color4f(mat->diffuse[0]*g_renderLightColor.x, mat->diffuse[1]*g_renderLightColor.y,
                         mat->diffuse[2]*g_renderLightColor.z, dif[3]);
        else
            gfx::Color4f(mat->diffuse[0], mat->diffuse[1], mat->diffuse[2], dif[3]);
        gfx::DisableArray(gfx::ColorArray);
        gfx::Disable(gfx::ColorMaterial);
    }

    // textura (nunca en Solid; respeta el checkbox textureOn del material)
    if (!solido && mat->texture && mat->textureOn) {
        gfx::Enable(gfx::Texture2D);
        gfx::BindTexture(mat->texture->iID);
        gfx::TexFilter(!gfx::PixeladoGlobal() && mat->filtrado);
        gfx::TexWrap(mat->repeat);
        // REFLECTION 3 modos (mat->reflectMode):
        //  0 MATCAP   = normal-del-ojo por MATRIZ DE TEXTURA -> HARDWARE en PC Y N95 (rapido). Normales como texcoords.
        //  1 SPHEREMAP= GL_SPHERE_MAP exacto -> HARDWARE en PC (texgen); en N95 (sin texgen) cae a SOFTWARE.
        //  2 EQUIRECT = 360 -> SIEMPRE por SOFTWARE (UV por-corner en CPU, calidad).
        // MATCAP (mode 0): por HARDWARE solo si hay matriz de textura (PC/N95). En GLES2 (Android/WebGL) esa
        // matriz es un no-op -> el matcap va por SOFTWARE, igual que el equirect. Sino no se veia.
        bool matcap     = mat->chrome && mat->reflectMode == 0 && gfx::TieneTexGen();  // matcap HW
        bool matcapSW   = mat->chrome && mat->reflectMode == 0 && !gfx::TieneTexGen(); // matcap SOFTWARE (GLES2)
        bool sphereExact= mat->chrome && mat->reflectMode == 1;
        bool eq         = mat->chrome && mat->reflectMode == 2;
        bool sphereHW   = sphereExact && gfx::TieneTexGen();      // PC: sphere exacto por texgen
        bool sw         = eq || (sphereExact && !gfx::TieneTexGen()) || matcapSW; // SOFTWARE: equirect + sphere/matcap sin HW
        // OJO: TexGenSphere y TexMatrixMatcap tocan los DOS la matriz de textura -> nunca los dos a la vez.
        if (matcap) { gfx::TexGenSphere(false); gfx::TexMatrixMatcap(true); }   // matcap: matriz de textura (HW)
        else        { gfx::TexMatrixMatcap(false); gfx::TexGenSphere(sphereHW); } // sphere HW (flip-V) o reset
        gfx::TexEnvReplace(mat->chrome);        // reflejo (cualquier modo) = espejo: textura directa, sin luz
        if (matcap) { gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer3b(normals, vertexSize); } // normales -> texcoords
        else if (sw) { ActualizarChromeUV(eq); // build los arrays por-corner (equirect o sphere); bind/draw en el loop
                  gfx::EnableArray(gfx::TexCoordArray); if (eq) gfx::TexWrap(true); } // REPEAT solo equirect (costura)
        else if (uv) { if (vboRenderActivo && vboUV) gfx::TexCoordVBO(vboUV); else gfx::TexCoordPointer2f(0, uv); } // UV del modelo (VBO o RAM)
    } else {
        gfx::Disable(gfx::Texture2D);
        gfx::TexMatrixMatcap(false);
        gfx::TexGenSphere(false);
        gfx::TexEnvReplace(false);
        if (uv) { if (vboRenderActivo && vboUV) gfx::TexCoordVBO(vboUV); else gfx::TexCoordPointer2f(0, uv); }
    }

    // normales: las sube la LUZ y el SPHERE-MAP por HARDWARE (texgen genera las UV de la normal). El MATCAP por
    // matriz de textura usa las normales como TEXCOORDS (no como NormalArray). El path por SOFTWARE (equirect, y el
    // sphere exacto del N95) NO usa el array (UV precomputadas + draw NO indexado). Por eso el reflejo no depende de la luz.
    bool sphereHWn = mat->chrome && mat->reflectMode == 1 && gfx::TieneTexGen();
    if (normals && (sphereHWn || (conLuz && mat->lighting))) gfx::EnableArray(gfx::NormalArray);
    else gfx::DisableArray(gfx::NormalArray);
    // la iluminacion en si solo si el material la pide (el chrome con GL_REPLACE la ignora -> espejo perfecto).
    // Con NORMAL MAP la base va SIN luz (albedo plano): el pass DOT3 de abajo aporta toda la iluminacion (N.L).
    if (conLuz && mat->lighting && !mat->normalMap) gfx::Enable(gfx::Lighting);
    else gfx::Disable(gfx::Lighting);

    if (mat->culling)     gfx::Enable(gfx::CullFace);  else gfx::Disable(gfx::CullFace);
    // z-test: lo decide el MATERIAL, salvo en el pase del espejo. Ahi manda el
    // espejo (ver w3dPaseEspejoSinZ en w3dGraphics.h): el reflejo del agua queda
    // MAS HONDO que el fondo opaco del estanque, asi que si el material vuelve a
    // prender el z-test el reflejo no pasa el test y no se ve NUNCA. Era la raiz
    // de que 'sinProfundidad' del Mirror no hiciera absolutamente nada.
    if (mat->depth_test && !gfx::w3dPaseEspejoSinZ) gfx::Enable(gfx::DepthTest);
    else                                            gfx::Disable(gfx::DepthTest);
    // ESCRITURA de z. La decide el material: un DECAL (sombra, blob, mancha) NO escribe,
    // asi no le pisa el z a lo que venga despues ni queda coplanar consigo mismo. En el
    // pase del espejo no escribe NADIE (w3dPaseEspejoSinZ manda igual que en el z-test).
    //
    // Y TAMPOCO ESCRIBE LO QUE ES *LUZ*: un material con mezcla aditiva /
    // sustractiva / multiplicativa / screen no reemplaza el fondo, lo MODULA (ver
    // W3dMaterialEsLuz en Materials.h). No puede tapar nada, asi que su z solo
    // sirve para hacer dano: una chispa aditiva dibujada temprano en el arbol le
    // robaba el z-test a todo lo que viniera DESPUES y mas lejos, y dejaba un
    // AGUJERO rectangular con la forma del quad (reporte del dueno, con captura;
    // prueba por pixeles en tools/pruebas/prueba_particulas_z.w3s). Es la MISMA
    // regla que ya cumple el pase diferido de particulas: translucido = z-test SI,
    // z-write NO. `depth_write` del material queda intacto (round-trip del .w3d).
    gfx::DepthMask(mat->depth_write && !W3dMaterialEsLuz(mat) && !gfx::w3dPaseEspejoSinZ);
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
    if (mat->orden_pasada == 1) gfx::DepthFunc(gfx::DepthLEqual);
    // FONDO (el cielo): dibujado ULTIMO de los opacos, clavado al plano lejano.
    // DepthRange 1..1 + LEQUAL: el fragmento sale a z=1.0 y solo pasa donde el
    // buffer sigue en el clear (1.0) = los pixeles que ningun opaco toco. Cero
    // overdraw del cielo, sin escribir z. gFondoActivo lo restaura el epilogo.
    if (mat->fondo) {
        gfx::Enable(gfx::DepthTest);
        gfx::DepthFunc(gfx::DepthLEqual);
        gfx::DepthRange(1.0f, 1.0f);
        gfx::DepthMask(false);
        gFondoActivo = true;
    }
    if (mat->depth_bias != 0.0f) {
        float unidades = mat->depth_bias;
#ifdef W3D_SYMBIAN
        unidades *= 4.0f; // z de 16 bits (N95): el mismo sesgo necesita ~4x mas unidades (-4 -> -16)
#endif
        gfx::Enable(gfx::PolygonOffsetFill);
        gfx::PolygonOffset(-1.0f, unidades);
    } else if (offsetEdit) {
        gfx::Enable(gfx::PolygonOffsetFill);
        gfx::PolygonOffset(2.0f, 4.0f);
    } else {
        gfx::PolygonOffset(0.0f, 0.0f);
        gfx::Disable(gfx::PolygonOffsetFill);
    }
    // MEZCLA. 'mezcla' = 0 es el alpha de siempre (todo lo guardado hasta hoy); cualquier
    // otro valor es un codigo de gfx::Mezcla — el que importa aca es 2 = MezclaAdd (ADITIVO,
    // GL_ONE/GL_ONE): aclara y el negro queda invisible, que es como el PSX dibuja el
    // torbellino del giro, los rayos y las chispas. Anda en GL ES 1.1 (N95), sin extensiones.
    if (mat->transparent) {
        gfx::Enable(gfx::Blend);
        if (mat->mezcla) gfx::SetMezcla(mat->mezcla); else gfx::BlendAlpha();
    }
    // X-RAY: aunque el material sea OPACO, se dibuja al 50% para ver lo que hay detras. Va
    // ACA y no arriba porque este if/else es el que decide el blend: prenderlo antes no servia
    // de nada (el 'else' de opaco lo apagaba). Sin escribir z, sino la propia malla se tapa a
    // si misma y no se ve nada a traves.
    else if (g_xray) { gfx::Enable(gfx::Blend); gfx::BlendAlpha(); gfx::DepthMask(false); }
    else                    gfx::Disable(gfx::Blend);
}

// hook de overlays del editor (lo registra el editor al arrancar; NULL en una app sin editor)
void (*g_meshOverlayHook)(Mesh*) = NULL;

// WEIGHT PAINT: arma weightPaintColor (RGBA por vertice de render) desde un peso DENSO por
// render-vert. La rampa es AZUL(0) -> amarillo(0.5) -> rojo(1). El cero es azul (no negro): negro
// se confundia con el fondo/piso oscuro. Azul = "sin peso" es la convencion de Blender.
// La comparten las dos entradas (3D por control-point / UV por corner): la unica diferencia entre
// ellas es DE DONDE sale 'rvW', asi que el color se ve igual en los dos viewports.
void Mesh::ColorPesoDesdeRenderVert(const std::vector<float>& rvW) {
    weightPaintColor.clear();
    if (vertexSize <= 0) return;
    weightPaintColor.resize((size_t)vertexSize * 4, 0);
    for (int i = 0; i < vertexSize; i++) {
        float w = (i < (int)rvW.size()) ? rvW[(size_t)i] : 0.0f;
        if (w < 0.0f) w = 0.0f; if (w > 1.0f) w = 1.0f;
        GLubyte r, g, b;
        if (w < 0.5f) { float t = w / 0.5f;          // azul(40,70,210) -> amarillo(255,255,0)
            r = (GLubyte)(40.0f  + (255.0f - 40.0f)  * t);
            g = (GLubyte)(70.0f  + (255.0f - 70.0f)  * t);
            b = (GLubyte)(210.0f + (0.0f   - 210.0f) * t);
        } else { float t = (w - 0.5f) / 0.5f;         // amarillo(255,255,0) -> rojo(255,0,0)
            r = 255; g = (GLubyte)(255.0f * (1.0f - t)); b = 0;
        }
        weightPaintColor[(size_t)i*4+0] = r; weightPaintColor[(size_t)i*4+1] = g;
        weightPaintColor[(size_t)i*4+2] = b; weightPaintColor[(size_t)i*4+3] = 255;
    }
}

// VIEWPORT 3D: degradado del VERTEX GROUP 'grupo' (pesos por CONTROL-POINT), mapeados a los
// vertices de render via vertCtrlPoint. Sin grupo/mapa valido queda todo en azul (peso 0).
void Mesh::ConstruirColorPeso(int grupo) {
    std::vector<float> rvW;
    if (vertexSize > 0) rvW.assign((size_t)vertexSize, 0.0f);
    VertexGroup* vg = (grupo >= 0 && grupo < (int)vertexGroups.size()) ? vertexGroups[grupo] : NULL;
    if (vg && !vertCtrlPoint.empty() && vertexSize > 0) {
        int maxCP = 0;
        for (size_t i = 0; i < vertCtrlPoint.size(); i++) if (vertCtrlPoint[i] > maxCP) maxCP = vertCtrlPoint[i];
        std::vector<float> cpW((size_t)maxCP + 1, 0.0f); // peso DENSO por control-point
        for (size_t i = 0; i < vg->verts.size() && i < vg->pesos.size(); i++) {
            int cp = vg->verts[i];
            if (cp >= 0 && cp <= maxCP) cpW[(size_t)cp] = vg->pesos[i];
        }
        for (int i = 0; i < vertexSize && i < (int)vertCtrlPoint.size(); i++) {
            int cp = vertCtrlPoint[i];
            if (cp >= 0 && cp < (int)cpW.size()) rvW[(size_t)i] = cpW[(size_t)cp];
        }
    }
    ColorPesoDesdeRenderVert(rvW);
}

// EDITOR UV: degradado del UV GROUP 'uvGrupo' (pesos por CORNER / render-vert, directos: no hay
// nada que mapear). Sin UV group valido queda todo en azul (peso 0).
void Mesh::ConstruirColorPesoUV(int uvGrupo) {
    std::vector<float> rvW;
    if (vertexSize > 0) rvW.assign((size_t)vertexSize, 0.0f);
    UVGroup* ug = (uvGrupo >= 0 && uvGrupo < (int)uvGroups.size()) ? uvGroups[uvGrupo] : NULL;
    if (ug) for (size_t i = 0; i < ug->verts.size() && i < ug->pesos.size(); i++) {
        int rv = ug->verts[i];
        if (rv >= 0 && rv < vertexSize) rvW[(size_t)rv] = ug->pesos[i];
    }
    ColorPesoDesdeRenderVert(rvW);
}

// ===================================================================================================
//  ARMATURE 2D DEL MESH (skinning de UV) — ver el bloque W3dBone2D en Mesh.h.
//  Los huesos viven en el ESPACIO UV; cada uno deforma los UV de los corners pesados en el
//  UV GROUP de su MISMO NOMBRE. La deformacion parte SIEMPRE del rest (uv2dRest), nunca
//  acumula. La animacion en el tiempo va por los CLIPS PROPIOS de cada armature
//  (Armature2D::anims, ver animation/Armature2DAnimation.h): una curva por hueso y canal que
//  llenan el Auto Key / Insert Keyframe y evalua Armature2DEvaluar. Ya NO se hornea en la capa
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
// No se escriben nunca por caminos validos (agregar huesos pasa por Arm2DAsegurar).
static std::vector<W3dBone2D>            gArm2DHuesosVacio;
static std::vector<Armature2DAnimation*> gArm2DAnimsVacio;
// UN dummy POR ACCESOR (no uno compartido): Arm2DBoneActivo() y Arm2DAnimActiva() devuelven int&
// y sin armature los dos escribian en la MISMA variable -> setear el hueso activo pisaba el clip
// activo (y al reves). Son estados distintos aunque los dos sean "un indice de -1".
static int                               gArm2DBoneVacio = -1;
static int                               gArm2DAnimVacio = -1;

Armature2D* Mesh::Arm2DActivoP() {
    if (armature2dActivo < 0 || armature2dActivo >= (int)armatures2d.size()) return NULL;
    return armatures2d[armature2dActivo];
}
const Armature2D* Mesh::Arm2DActivoP() const {
    if (armature2dActivo < 0 || armature2dActivo >= (int)armatures2d.size()) return NULL;
    return armatures2d[armature2dActivo];
}
std::vector<W3dBone2D>& Mesh::Arm2DHuesos() {
    Armature2D* a = Arm2DActivoP(); return a ? a->huesos : gArm2DHuesosVacio;
}
const std::vector<W3dBone2D>& Mesh::Arm2DHuesos() const {
    const Armature2D* a = Arm2DActivoP(); return a ? a->huesos : gArm2DHuesosVacio;
}
int& Mesh::Arm2DBoneActivo() { Armature2D* a = Arm2DActivoP(); return a ? a->boneActivo : gArm2DBoneVacio; }
int  Mesh::Arm2DBoneActivo() const { const Armature2D* a = Arm2DActivoP(); return a ? a->boneActivo : -1; }
std::vector<Armature2DAnimation*>& Mesh::Arm2DAnims() {
    Armature2D* a = Arm2DActivoP(); return a ? a->anims : gArm2DAnimsVacio;
}
const std::vector<Armature2DAnimation*>& Mesh::Arm2DAnims() const {
    const Armature2D* a = Arm2DActivoP(); return a ? a->anims : gArm2DAnimsVacio;
}
int& Mesh::Arm2DAnimActiva() { Armature2D* a = Arm2DActivoP(); return a ? a->animActiva : gArm2DAnimVacio; }
int  Mesh::Arm2DAnimActiva() const { const Armature2D* a = Arm2DActivoP(); return a ? a->animActiva : -1; }

// ============================================================================
//  NOMBRES UNICOS por MALLA - TODOS delegan en W3dNombreUnico (base/W3dNombres.h).
//  Antes cada lista tenia su propio bucle, con topes en 999 que devolvian un
//  nombre YA OCUPADO y sin pelar el ".NNN" previo (daban "X.001.001").
// ============================================================================
namespace {
    // ctx generico: la lista de nombres tomados + el indice que se excluye
    struct MeshNomCtx { const Mesh* m; int excepto; int cual; };
    // cual: 0 vgroup, 1 uvgroup, 2 uvmap, 3 color, 4 meshpart, 5 vertexanim, 6 armature2d
    bool MeshNomExiste(const std::string& n, void* ctx) {
        MeshNomCtx* c = (MeshNomCtx*)ctx;
        const Mesh* m = c->m;
        switch (c->cual) {
            case 0: for (size_t i = 0; i < m->vertexGroups.size(); i++)
                        if ((int)i != c->excepto && m->vertexGroups[i] && m->vertexGroups[i]->nombre == n) return true;
                    break;
            case 1: for (size_t i = 0; i < m->uvGroups.size(); i++)
                        if ((int)i != c->excepto && m->uvGroups[i] && m->uvGroups[i]->nombre == n) return true;
                    break;
            case 2: for (size_t i = 0; i < m->uvMaps.size(); i++)
                        if ((int)i != c->excepto && m->uvMaps[i] && m->uvMaps[i]->nombre == n) return true;
                    break;
            case 3: for (size_t i = 0; i < m->colorLayers.size(); i++)
                        if ((int)i != c->excepto && m->colorLayers[i] && m->colorLayers[i]->nombre == n) return true;
                    break;
            case 4: for (size_t i = 0; i < m->materialsGroup.size(); i++)
                        if ((int)i != c->excepto && m->materialsGroup[i].name == n) return true;
                    break;
            case 5: for (size_t i = 0; i < m->animations.size(); i++)
                        if ((int)i != c->excepto && m->animations[i] && m->animations[i]->name == n) return true;
                    break;
            case 6: for (size_t i = 0; i < m->armatures2d.size(); i++)
                        if ((int)i != c->excepto && m->armatures2d[i] && m->armatures2d[i]->nombre == n) return true;
                    break;
        }
        return false;
    }
    std::string MeshNomLibre(const Mesh* m, int cual, const std::string& base,
                             const char* porDefecto, int excepto) {
        MeshNomCtx c; c.m = m; c.excepto = excepto; c.cual = cual;
        return W3dNombreUnico(base, porDefecto, MeshNomExiste, &c);
    }
}

std::string Mesh::NombreLibreVGroup (const std::string& b, int ex) const { return MeshNomLibre(this, 0, b, "Group", ex); }
std::string Mesh::NombreLibreUVGroup(const std::string& b, int ex) const { return MeshNomLibre(this, 1, b, "UV Group", ex); }
std::string Mesh::NombreLibreUVMap  (const std::string& b, int ex) const { return MeshNomLibre(this, 2, b, "UVMap", ex); }
std::string Mesh::NombreLibreColor  (const std::string& b, int ex) const { return MeshNomLibre(this, 3, b, "Col", ex); }
std::string Mesh::NombreLibreMeshPart(const std::string& b, int ex) const { return MeshNomLibre(this, 4, b, "Mesh", ex); }
std::string Mesh::NombreLibreVertexAnim(const std::string& b, int ex) const { return MeshNomLibre(this, 5, b, "Anim", ex); }

std::string Mesh::Arm2DNombreUnico(const std::string& base) const {
    return MeshNomLibre(this, 6, base, "Armature 2D", -1);
}
// idem pero excluyendo el armature 'excepto' (para el RENAME: renombrarlo al
// mismo valor no debe devolver ".001")
std::string Mesh::Arm2DNombreLibre(const std::string& base, int excepto) const {
    return MeshNomLibre(this, 6, base, "Armature 2D", excepto);
}

// HUESOS 2D: el espacio es la MALLA ENTERA (todos sus armatures 2D a la vez),
// porque el binding es por nombre contra el UV group, que es del mesh.
namespace {
    struct Bone2DNomCtx { const Mesh* m; int exArm; int exBone; };
    bool Bone2DNomExiste(const std::string& n, void* ctx) {
        Bone2DNomCtx* c = (Bone2DNomCtx*)ctx;
        for (size_t a = 0; a < c->m->armatures2d.size(); a++) {
            const Armature2D* arm = c->m->armatures2d[a];
            if (!arm) continue;
            for (size_t i = 0; i < arm->huesos.size(); i++) {
                if ((int)a == c->exArm && (int)i == c->exBone) continue;
                if (arm->huesos[i].nombre == n) return true;
            }
        }
        return false;
    }
}
std::string Mesh::NombreLibreBone2D(const std::string& base, int exArm, int exBone) const {
    Bone2DNomCtx c; c.m = this; c.exArm = exArm; c.exBone = exBone;
    return W3dNombreUnico(base, "Bone", Bone2DNomExiste, &c);
}
Armature2D* Mesh::Arm2DAsegurar() {
    if (armatures2d.empty()) Arm2DAgregar("Armature 2D");
    if (armature2dActivo < 0 || armature2dActivo >= (int)armatures2d.size()) armature2dActivo = 0;
    return armatures2d[armature2dActivo];
}
int Mesh::Arm2DAgregar(const std::string& nombre) {
    armatures2d.push_back(new Armature2D(Arm2DNombreUnico(nombre)));
    armature2dActivo = (int)armatures2d.size() - 1;
    return armature2dActivo;
}
// borra el armature idx (y sus clips). Los UV GROUPS de sus huesos NO se tocan: son los PESOS por
// corner y el binding es por NOMBRE, asi que conservarlos deja el rig re-bindeado si el editor lo
// devuelve (undo). Un UV group sin hueso no deforma nada. Misma decision que Bone2DBorrar con los
// vertex groups del rig 3D.
void Mesh::Arm2DBorrar(int idx) {
    if (idx < 0 || idx >= (int)armatures2d.size()) return;
    delete armatures2d[idx];
    armatures2d.erase(armatures2d.begin() + idx);
    if (armature2dActivo > idx) armature2dActivo--;
    if (armature2dActivo >= (int)armatures2d.size()) armature2dActivo = (int)armatures2d.size() - 1;
}
// captura el rest LAZY: si uv2dRest no coincide con la topologia actual (primera pose o la
// malla cambio de layout), el uv ACTUAL pasa a ser el rest del skinning 2D.
void Mesh::Armature2DRestCapturar() {
    if (!uv || vertexSize <= 0) return;
    if ((int)uv2dRest.size() == vertexSize * 2) return; // ya hay un rest valido
    uv2dRest.assign(uv, uv + (size_t)vertexSize * 2);
}

// TODOS los armatures de la malla (no solo el activo): si CUALQUIER hueso esta posado, el
// skinning tiene que correr.
bool Mesh::Armature2DPoseIdentidad() const {
    for (size_t a = 0; a < armatures2d.size(); a++) {
        if (!armatures2d[a]) continue;
        const std::vector<W3dBone2D>& hs = armatures2d[a]->huesos;
        for (size_t i = 0; i < hs.size(); i++)
            if (!hs[i].PoseIdentidad()) return false;
    }
    return true;
}

// matrices afines 2D (FK): local = trasladar(head + poseT) * rotar(poseRot) * escalar(poseS)
// * trasladar(-head)  ->  p' = head + poseT + R*S*(p - head). El mundo del hijo = mundo del
// padre * local (el invariante padre < hijo permite una sola pasada; un padre "adelantado"
// se trata como raiz y se avisa en el que autorea, no aca).
void Mesh::Armature2DMatrices(std::vector<float>& out) const { Armature2DMatricesDe(Arm2DActivoP(), out); }

void Mesh::Armature2DMatricesDe(const Armature2D* arm, std::vector<float>& out) const {
    const float DEG = 3.14159265358979f / 180.0f;
    const size_t n = arm ? arm->huesos.size() : 0;
    out.assign(n * 6, 0.0f);
    for (size_t i = 0; i < n; i++) {
        const W3dBone2D& b = arm->huesos[i];
        float c = cosf(b.poseRot * DEG), s = sinf(b.poseRot * DEG);
        float M[6];
        M[0] = c * b.poseSX; M[1] = -s * b.poseSY;
        M[3] = s * b.poseSX; M[4] =  c * b.poseSY;
        M[2] = b.headU + b.poseTU - (M[0] * b.headU + M[1] * b.headV);
        M[5] = b.headV + b.poseTV - (M[3] * b.headU + M[4] * b.headV);
        float* W = &out[i * 6];
        if (b.padre >= 0 && b.padre < (int)i) {
            const float* P = &out[(size_t)b.padre * 6];
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
// captura con Armature2DRestCapturar). Con la pose en identidad restaura el rest EXACTO.
void Mesh::Armature2DHuesosPlanos(std::vector<const W3dBone2D*>& bones, std::vector<float>& out) const {
    bones.clear(); out.clear();
    std::vector<float> M;
    for (size_t a = 0; a < armatures2d.size(); a++) {
        const Armature2D* arm = armatures2d[a];
        if (!arm || arm->huesos.empty()) continue;
        Armature2DMatricesDe(arm, M);                 // FK PROPIA de cada armature (no se mezclan)
        for (size_t i = 0; i < arm->huesos.size(); i++) {
            bones.push_back(&arm->huesos[i]);
            for (int k = 0; k < 6; k++) out.push_back(M[i * 6 + k]);
        }
    }
}

// PENDIENTE CONOCIDO (performance, no se toca en esta tanda): el bucle de abajo es
// O(huesos * V * log N) al posar -- por CADA hueso arma un std::map con sus pesos y despues
// RECORRE LOS vertexSize VERTICES buscando cada uno en ese map, aunque el UV group tenga 4
// entradas. Lo correcto (y lo que ya hace Armature2DRestDesdeUV mas abajo) es iterar
// ug->verts, que es SPARSE: queda O(sum de entradas de los UV groups), sin map ni busqueda.
// Importa en el N95, donde posar un rig sobre una malla grande se siente. Requiere acumular
// au/av/ws por vert (ya estan) y una segunda pasada igual a la actual: cambio contenido, pero
// va con su propio test de "misma deformacion, mismo resultado".
void Mesh::Armature2DAplicar() {
    if (armatures2d.empty() || !uv || vertexSize <= 0) return;
    if ((int)uv2dRest.size() != vertexSize * 2) return; // rest invalido (topologia cambio)
    if (Armature2DPoseIdentidad()) {
        for (int i = 0; i < vertexSize * 2; i++) uv[i] = uv2dRest[i];
        return;
    }
    std::vector<const W3dBone2D*> huesos; // TODOS los armatures aplanados (cada uno con su FK)
    std::vector<float> M;
    Armature2DHuesosPlanos(huesos, M);
    std::vector<float> au((size_t)vertexSize, 0.0f), av((size_t)vertexSize, 0.0f), ws((size_t)vertexSize, 0.0f);
    for (size_t bi = 0; bi < huesos.size(); bi++) {
        // el UV GROUP del MISMO NOMBRE que el hueso (binding por nombre; ver la regla en Mesh.h:
        // 3D = hueso <-> vertex group, 2D = hueso <-> UV group). Sus pesos SIEMPRE son por
        // RENDER-VERT (corner) -> pintar UNA cara mueve SOLO sus corners, sin arrastrar las otras
        // caras que comparten el punto 3D. Un hueso sin UV group homonimo no deforma nada.
        UVGroup* ug = NULL;
        for (size_t g = 0; g < uvGroups.size(); g++)
            if (uvGroups[g] && uvGroups[g]->nombre == huesos[bi]->nombre) { ug = uvGroups[g]; break; }
        if (!ug || ug->verts.empty()) continue;
        std::map<int, float> pesoDe; // clave = RENDER-VERT
        for (size_t k = 0; k < ug->verts.size() && k < ug->pesos.size(); k++)
            if (ug->pesos[k] > 0.0f) pesoDe[ug->verts[k]] = ug->pesos[k];
        if (pesoDe.empty()) continue;
        const float* W = &M[bi * 6];
        for (int i = 0; i < vertexSize; i++) {
            std::map<int, float>::iterator it = pesoDe.find(i);
            if (it == pesoDe.end()) continue;
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
// Armature2DAplicar (bug de "muevo una cara en el UV, pinto y se resetea todo al mismo lugar").
//
// Con pose puesta, el skinning es AFIN POR VERT: uv_i = A_i * rest_i + b_i, donde
//   ws >= 1 : A_i = (1/ws) sum_h w_h R_h ; b_i = (1/ws) sum_h w_h t_h
//   0<ws<1  : A_i = sum_h w_h R_h + (1-ws) I ; b_i = sum_h w_h t_h
//   ws <= 0 : A_i = I, b_i = 0 (el vert no lo toca ningun hueso)
// asi que se invierte la 2x2 y sale el rest EXACTO que reproduce el uv editado.
void Mesh::Armature2DRestDesdeUV() {
    if (!uv || vertexSize <= 0) return;
    if (armatures2d.empty()) { uv2dRest.clear(); return; } // sin rig no hay rest que mantener
    if (Armature2DPoseIdentidad()) { uv2dRest.assign(uv, uv + (size_t)vertexSize * 2); return; }
    const bool hayRest = ((int)uv2dRest.size() == vertexSize * 2);
    std::vector<const W3dBone2D*> huesos;
    std::vector<float> M;
    Armature2DHuesosPlanos(huesos, M);
    // acumulador afin por vert: A = [a00 a01 ; a10 a11], b = (b0,b1), ws = suma de pesos
    std::vector<float> a00((size_t)vertexSize, 0.0f), a01((size_t)vertexSize, 0.0f);
    std::vector<float> a10((size_t)vertexSize, 0.0f), a11((size_t)vertexSize, 0.0f);
    std::vector<float> b0((size_t)vertexSize, 0.0f),  b1((size_t)vertexSize, 0.0f);
    std::vector<float> ws((size_t)vertexSize, 0.0f);
    for (size_t bi = 0; bi < huesos.size(); bi++) {
        UVGroup* ug = NULL;
        for (size_t g = 0; g < uvGroups.size(); g++)
            if (uvGroups[g] && uvGroups[g]->nombre == huesos[bi]->nombre) { ug = uvGroups[g]; break; }
        if (!ug || ug->verts.empty()) continue;
        const float* W = &M[bi * 6];
        for (size_t k = 0; k < ug->verts.size() && k < ug->pesos.size(); k++) {
            int i = ug->verts[k]; float w = ug->pesos[k];
            if (i < 0 || i >= vertexSize || w <= 0.0f) continue;
            a00[(size_t)i] += w * W[0]; a01[(size_t)i] += w * W[1]; b0[(size_t)i] += w * W[2];
            a10[(size_t)i] += w * W[3]; a11[(size_t)i] += w * W[4]; b1[(size_t)i] += w * W[5];
            ws[(size_t)i] += w;
        }
    }
    std::vector<GLfloat> nuevo((size_t)vertexSize * 2, 0.0f);
    for (int i = 0; i < vertexSize; i++) {
        float u = uv[i * 2], v = uv[i * 2 + 1];
        float s = ws[(size_t)i];
        float A00, A01, A10, A11, B0, B1;
        if (s <= 0.0f) { nuevo[(size_t)i*2] = u; nuevo[(size_t)i*2+1] = v; continue; } // sin hueso: rest = uv
        if (s >= 1.0f) { const float inv = 1.0f / s;
            A00 = a00[(size_t)i]*inv; A01 = a01[(size_t)i]*inv; B0 = b0[(size_t)i]*inv;
            A10 = a10[(size_t)i]*inv; A11 = a11[(size_t)i]*inv; B1 = b1[(size_t)i]*inv;
        } else {
            A00 = a00[(size_t)i] + (1.0f - s); A01 = a01[(size_t)i]; B0 = b0[(size_t)i];
            A10 = a10[(size_t)i]; A11 = a11[(size_t)i] + (1.0f - s); B1 = b1[(size_t)i];
        }
        float det = A00 * A11 - A01 * A10;
        if (det > -1e-12f && det < 1e-12f) { // singular (escala 0): se conserva el rest previo
            nuevo[(size_t)i*2]   = hayRest ? uv2dRest[(size_t)i*2]   : u;
            nuevo[(size_t)i*2+1] = hayRest ? uv2dRest[(size_t)i*2+1] : v;
            continue;
        }
        float du = u - B0, dv = v - B1, inv = 1.0f / det;
        nuevo[(size_t)i*2]   = ( A11 * du - A01 * dv) * inv;
        nuevo[(size_t)i*2+1] = (-A10 * du + A00 * dv) * inv;
    }
    uv2dRest.swap(nuevo);
}

// Sube (o actualiza) los VBOs de render con los arrays ACTUALES. soloPose=true -> re-sube SOLO pos/nor (cambio la
// pose de skinning; col/uv/idx son estaticos y no se re-suben). Crea cada buffer on-demand. posBuf/norBuf = lo que
// realmente se dibuja (vertex o skinVertex; normals o skinNormals).
void Mesh::SubirVBO(const GLfloat* posBuf, const GLbyte* norBuf, bool soloPose) {
    namespace gfx = w3dEngine;
    if (!posBuf || vertexSize <= 0) return;
    if (!vboPos) vboPos = gfx::GenBuffer();
    gfx::ArrayBufferData(vboPos, posBuf, vertexSize * 3 * (int)sizeof(GLfloat));
    if (norBuf) { if (!vboNor) vboNor = gfx::GenBuffer(); gfx::ArrayBufferData(vboNor, norBuf, vertexSize * 3 * (int)sizeof(GLbyte)); }
    vboVertN = vertexSize;
    if (soloPose) return; // solo la pose de skinning cambio -> col/uv/idx (estaticos) no se re-suben
    if (vertexColor) { if (!vboCol) vboCol = gfx::GenBuffer(); gfx::ArrayBufferData(vboCol, vertexColor, vertexSize * 4 * (int)sizeof(GLubyte)); }
    if (uv)          { if (!vboUV)  vboUV  = gfx::GenBuffer(); gfx::ArrayBufferData(vboUV,  uv,          vertexSize * 2 * (int)sizeof(GLfloat)); }
    // el IBO es ESTATICO y guarda SIEMPRE la malla completa: se sube una vez con la
    // geometria y no se toca nunca mas. (P3) Con el override de visibilidad activo,
    // los indices del subconjunto van como INDICES DE CLIENTE en el draw
    // (DrawTrianglesClientIdx): cambiar de celda/sector no re-especifica ningun
    // buffer. Antes aca se subia el subconjunto del sector y el cambio de sector
    // re-subia el IBO entero (el anti-patron del stall del tiler MBX).
    if (faces && facesSize > 0) { if (!vboIdx) vboIdx = gfx::GenBuffer(); gfx::IndexBufferData(vboIdx, faces, facesSize * (int)sizeof(MeshIndex)); vboIdxN = facesSize; }
}

// ===========================================================================
//  PASE DIFERIDO DE CALCOMANIAS (sombras sueltas)
//
//  Reporte del dueno: "la sombra del personaje se pierde por z-fighting pese al
//  material decal", sobre terreno inclinado y sobre los troncos.
//
//  El sesgo y la funcion de profundidad arreglan la sombra COPLANAR con la
//  superficie que la recibe (ver AplicarMaterial), pero no alcanzan cuando la
//  sombra es un OBJETO APARTE, declarado fuera del cuerpo que la proyecta (el
//  caso que lo destapo: la sombra del personaje y las de los enemigos, sueltas
//  en la escena). Una calcomania NO escribe z a
//  proposito; entonces cualquier cosa OPACA que se dibuje DESPUES de ella y
//  ocupe el mismo lugar (los troncos y la malla EscenarioAlpha van al final del
//  .w3d) la pisa por completo: la sombra desaparece.
//
//  `orden_pasada` solo ordena ADENTRO de una malla (Materials.h lo dice), asi
//  que no puede arreglar esto. La solucion es la misma que ya usan las
//  particulas: las mallas que son SOLO calcomania no se dibujan durante el
//  recorrido del arbol, se ANOTAN, y se dibujan todas juntas cuando la escena
//  opaca ya esta completa (ViewPort3D::Render, justo antes de las particulas).
//  Asi la sombra siempre cae ENCIMA del suelo, sea cual sea el orden del arbol.
//
//  w3dDecalesInline = "no diferir, dibujar aca mismo": lo prende el pase del
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
//    1) la chispa ESCRIBIA z (lo arregla W3dMaterialEsLuz en AplicarMaterial):
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
bool w3dDecalesInline = false;
static std::vector<Mesh*> gDecalPend;
static std::vector<Mesh*> gLuzPend;

// true si TODOS los grupos de material de la malla son calcomania (orden_pasada
// 1). Una malla mixta (bicho + su blob horneado) NO se difiere: para esa ya
// funciona el orden interno opaco -> decal -> transparente.
bool Mesh::EsSoloDecal() const {
    if (materialsGroup.empty()) return false;
    for (size_t g = 0; g < materialsGroup.size(); g++) {
        const Material* mt = materialsGroup[g].material;
        if (!mt || mt->orden_pasada != 1) return false;
    }
    return true;
}

// true si TODOS los grupos de material de la malla son LUZ (mezcla aditiva /
// sustractiva / multiplicativa / screen; ver W3dMaterialEsLuz). Mismo criterio
// que EsSoloDecal: una malla MIXTA (un cuerpo opaco con un halo aditivo) NO se
// difiere, porque su parte opaca tiene que quedarse donde el arbol la puso.
bool Mesh::EsSoloLuz() const {
    if (materialsGroup.empty()) return false;
    for (size_t g = 0; g < materialsGroup.size(); g++) {
        if (!W3dMaterialEsLuz(materialsGroup[g].material)) return false;
    }
    return true;
}

void W3dDecalesLimpiarPendientes() { gDecalPend.clear(); }
void W3dLucesLimpiarPendientes()   { gLuzPend.clear(); }

// El flush de las LUCES: mismo esqueleto que el de calcomanias (afirma su
// estado, arma la matriz de VISTA y dibuja cada malla con su matriz de mundo).
// Corre DESPUES de las calcomanias y ANTES de las particulas: sombra -> luz de
// malla -> billboards.
void W3dLucesDibujarPendientes() {
    if (gLuzPend.empty()) return;
    namespace gfx = w3dEngine;
    CameraBase cam;
    cam.pos = g_renderCamPos; cam.rot = g_renderCamRot;
    Matrix4 V = cam.ViewMatrix();
    // NINGUNA LUZ HEREDA EL RECORTE DE NADIE (misma razon que en las
    // calcomanias: los espejos dejan estencil, planos de recorte, DepthRange y
    // ColorMask tocados, y este pase corre despues de todos ellos).
    gfx::EstencilApagar();
    gfx::PlanosRecorte(0, NULL);
    gfx::DepthRange(0.0f, 1.0f);
    gfx::ColorMask(true, true, true, true);
    gfx::DepthFunc(gfx::DepthLess);

    const bool inlineAntes = w3dDecalesInline;
    w3dDecalesInline = true;   // adentro del flush se dibuja de verdad
    gfx::MatrixMode(gfx::ModelView);
    for (size_t i = 0; i < gLuzPend.size(); i++) {
        Mesh* m = gLuzPend[i];
        if (!m) continue;
        Matrix4 W; m->GetWorldMatrix(W);
        gfx::PushMatrix();
        gfx::LoadMatrix(V.m);
        gfx::MultMatrix(W.m);
        m->RenderObject();
        gfx::PopMatrix();
    }
    w3dDecalesInline = inlineAntes;
    gLuzPend.clear();
    // el estado que toca AplicarMaterial queda como lo dejo la ultima luz:
    // devolverlo al baseline que espera el resto del frame.
    gfx::DepthFunc(gfx::DepthLess);
    gfx::DepthMask(true);
    gfx::PolygonOffset(0.0f, 0.0f);
    gfx::Disable(gfx::PolygonOffsetFill);
}

void W3dDecalesDibujarPendientes() {
    if (gDecalPend.empty()) return;
    namespace gfx = w3dEngine;
    // la VISTA que esta dibujando (la misma que bindeo el viewport). Se rearma
    // igual que en Culling::RenderHijos, para no depender de que la pila de
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
    gfx::EstencilApagar();          // sin mascara: la sombra no vive adentro de ningun espejo
    gfx::PlanosRecorte(0, NULL);    // sin planos de recorte del espejo
    gfx::DepthRange(0.0f, 1.0f);    // el rango normal (el pase B del espejo lo lleva a (1,1))
    gfx::ColorMask(true, true, true, true);

    w3dDecalesInline = true;   // adentro del flush se dibuja de verdad
    gfx::MatrixMode(gfx::ModelView);
    for (size_t i = 0; i < gDecalPend.size(); i++) {
        Mesh* m = gDecalPend[i];
        if (!m) continue;
        Matrix4 W; m->GetWorldMatrix(W);
        gfx::PushMatrix();
        gfx::LoadMatrix(V.m);
        gfx::MultMatrix(W.m);
        m->RenderObject();
        gfx::PopMatrix();
    }
    w3dDecalesInline = false;
    gDecalPend.clear();
    // el estado que toca AplicarMaterial queda como lo dejo la ultima
    // calcomania: devolverlo al baseline que espera el resto del frame.
    gfx::DepthFunc(gfx::DepthLess);
    gfx::DepthMask(true);
    gfx::PolygonOffset(0.0f, 0.0f);
    gfx::Disable(gfx::PolygonOffsetFill);
}

// (P4) sello del pase de escena para el lote estatico (ver Mesh.h)
unsigned w3dLoteStamp = 0;

// ===========================================================================
//  LINEAS DEL MATERIAL (Material::lineas + grosorLinea): dibuja las ARISTAS
//  del rango de triangulos recien dibujado, con el MISMO estado de material
//  (textura/color/blend ya aplicados por AplicarMaterial). Las aristas se
//  DEDUPLICAN (una arista compartida por dos triangulos se traza UNA vez: con
//  un material translucido/aditivo trazarla dos veces doblaria su mezcla).
//  El z-test pasa a LEqual durante el trazo: las lineas caen justo sobre el
//  relleno que acaba de escribir esa misma z (con Less no pintaria un pixel);
//  el que llama re-aplica el material del proximo grupo (ultimo = NULL).
//  En una TIRA degenerada (triangulos de area cero) el relleno no emite nada
//  y estas aristas dibujan el CAMINO de la polilinea.
// ===========================================================================
static void W3dDibujarLineasMat(const MeshIndex* idx, int count, float grosor) {
    namespace gfx = w3dEngine;
    if (!idx || count < 3) return;
    static std::vector<unsigned long long> claves; // arista codificada (min,max)
    static std::vector<MeshIndex> lineas;          // pares de indices a dibujar
    claves.clear();
    for (int t = 0; t + 2 < count; t += 3) {
        const MeshIndex v[3] = { idx[t], idx[t + 1], idx[t + 2] };
        for (int e = 0; e < 3; e++) {
            MeshIndex a = v[e], b = v[(e + 1) % 3];
            if (a == b) continue;                  // arista degenerada (tira): nada
            unsigned long long lo = (a < b) ? a : b, hi = (a < b) ? b : a;
            claves.push_back((lo << 32) | hi);
        }
    }
    if (claves.empty()) return;
    std::sort(claves.begin(), claves.end());
    claves.erase(std::unique(claves.begin(), claves.end()), claves.end());
    lineas.clear(); lineas.reserve(claves.size() * 2);
    for (size_t i = 0; i < claves.size(); i++) {
        lineas.push_back((MeshIndex)(claves[i] >> 32));
        lineas.push_back((MeshIndex)(claves[i] & 0xffffffffull));
    }
    gfx::LineWidth(grosor < 1.0f ? 1.0f : grosor);
    gfx::DepthFunc(gfx::DepthLEqual);   // coplanar con su propio relleno
    gfx::DrawLinesClientIdx((int)lineas.size(), &lineas[0]);
    gfx::DepthFunc(gfx::DepthLess);
    gfx::LineWidth(1.0f);
}

void Mesh::RenderObject() {
    // en el render FINAL un objeto no-renderizable no sale (en el viewport si)
    if (w3dEngine::w3dRenderFinal && !renderizable) return;
    // (P4) horneada en el lote estatico de una Collection ESTE frame: el lote ya
    // la dibujo en su tanda por material; aca no se dibuja nada.
    if (w3dLoteStamp && enLoteEstatico == w3dLoteStamp) return;
    // CALCOMANIA SUELTA: se anota y se dibuja al final (ver el comentario largo
    // arriba). El chequeo va ANTES de cualquier trabajo de dibujo.
    if (!w3dDecalesInline && (Object*)this != g_editMesh && EsSoloDecal()) {
        gDecalPend.push_back(this);
        return;
    }
    // LUZ SUELTA (chispa/halo/rayo aditivo): mismo trato. Va DESPUES del chequeo
    // de calcomania porque un decal aditivo es, antes que nada, una calcomania.
    if (!w3dDecalesInline && (Object*)this != g_editMesh && EsSoloLuz()) {
        gLuzPend.push_back(this);
        return;
    }
    const bool editActiva = ((Object*)this == g_editMesh); // esta malla en Edit Mode
    // sin vertices no hay nada. Sin CARAS propias igual hay que dibujar: en Edit (edit mesh: verts+bordes), o si
    // hay malla GENERADA por modificadores (ej. Screw sobre un perfil sin caras -> la botella vive en genValido),
    // o si hay loose edges (wireframe suelto que se ve como overlay). Sino en modo objeto una malla sin caras
    // propias quedaba INVISIBLE aunque tuviera geometria generada/suelta -> "no se ve la botella".
    if (!vertex || vertexSize <= 0) return;
    const bool hayGen = (genValido && genVertex && genFaces);
    if ((!faces || facesSize < 3) && !editActiva && !hayGen && looseEdges.empty()) return;
    w3dEngine::g_statMeshes++; // estadistica de frame (bench): mallas que llegan a dibujarse
    // el material por defecto SIEMPRE tiene que existir (en Symbian arranca NULL)
    if (!MaterialDefecto) MaterialDefecto = new Material("Default", true);
    namespace gfx = w3dEngine;
    // (P1) ACA vivia un gfx::Invalidate() POR MALLA: anulaba el cache ~200 veces
    // por frame y cada Enable/Disable/DepthMask/TexEnv del material volvia a ir
    // al driver. El resync ahora es UNA vez por frame, antes del traversal de la
    // escena (ViewPort3D), y NADA del pase de escena puede tocar GL crudo (lo
    // vigila gfx::AuditarEstado en el harness 'glaudit').
#ifdef W3D_SYMBIAN
    // El loop de Symbian todavia tiene llamadas GL crudas alrededor de las mallas
    // (Light::RenderObject y el render propio de la plataforma): ahi el resync por
    // malla SIGUE hasta que ese loop tenga su Invalidate por frame. En PC no corre.
    gfx::Invalidate();
#endif

    // SKINNING: si hay esqueleto asignado, deformar a la pose (posBuf = skinVertex). Posiciones + normales rotadas.
    GLfloat* posBuf = vertex;
    GLbyte*  norBuf = normals;
    if (skinArmature) {
        // normales rotadas SOLO si algun mesh part tiene luz (sino no vale la pena; N95). w3dRenderSinLuz -> todo unlit.
        // El pase Normal View tambien las necesita (colorea por normal en view-space) aunque el material sea unlit.
        bool algunaLuz = false;
        if (!w3dRenderSinLuz) for (size_t g = 0; g < materialsGroup.size(); g++)
            if (materialsGroup[g].material && materialsGroup[g].material->lighting){ algunaLuz = true; break; }
        skinConLuz = algunaLuz || w3dRenderNormalColor;
        extern void SkinearMesh(Mesh*); SkinearMesh(this);
        if (skinVertex) posBuf = skinVertex;
        if (skinConLuz && skinNormals) norBuf = skinNormals;
    }

    // punteros de los datos de la malla (una sola vez)
    gfx::VertexPointer3f(0, posBuf);
    if (norBuf)      gfx::NormalPointer3b(norBuf);
    if (vertexColor) gfx::ColorPointer4ub(vertexColor);
    if (uv) { gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, uv); }

    // WEIGHT PAINT: la malla se dibuja SOLIDA con el degradado de peso por vertice (negro->amarillo->rojo), sin luz
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
        const bool genCol = genValido && genVertex && genFaces && genFacesSize >= 3 && genColor;
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
    //   - ZBuffer / Alpha (w3dRenderSinLuz / w3dRenderAlpha): color = BLANCO. El editor le pone fog al
    //     ZBuffer (degrade de profundidad); el Alpha va SIN fog = matte de cobertura. Soporta malla generada.
    {
    const bool paseNormal = w3dRenderNormalColor;
    const bool paseBlanco = w3dRenderSinLuz || w3dRenderAlpha;
    if ((paseNormal || paseBlanco) && faces && facesSize >= 3) {
        gfx::Disable(gfx::Lighting);
        gfx::DisableArray(gfx::NormalArray);
        gfx::Enable(gfx::DepthTest);

        // normal usa la malla BASE (color por vertice tiene que matchear la geometria); el blanco puede
        // usar la malla GENERADA por modificadores (color uniforme -> sin problema de indices). posBuf =
        // skinVertex si hay esqueleto -> los 3 pases muestran la malla DEFORMADA por la pose, no el bind.
        const bool useGen = paseBlanco && (genValido && genVertex && genFaces);
        gfx::VertexPointer3f(0, useGen ? genVertex : posBuf);

        if (paseNormal && norBuf) {
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
            Material* mat = grp.material ? grp.material : MaterialDefecto;
            bool trans = mat->transparent && mat->texture && mat->textureOn && (useGen ? (genUV != 0) : (uv != 0));
            if (trans) {
                gfx::Enable(gfx::Texture2D);
                gfx::BindTexture(mat->texture->iID);
                gfx::TexFilter(!gfx::PixeladoGlobal() && mat->filtrado); gfx::TexWrap(mat->repeat);
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
        if (editActiva && w3dRenderOverlays) {
            // en wireframe NO hay relleno que tape el fondo: el overlay de edicion
            // (lineas con vertex color + vertices) se ve entero (todos los puntos).
            // sin overlays (limpieza de pantalla) cae al wireframe plano de abajo.
            RenderEditOverlay();
        } else {
        int cid = !select ? RC_wireframe
                          : (((Object*)this == ObjActivo) ? RC_selActive : RC_selInactive);
        const float* col = gRenderColors[cid];
        if (w3dRenderWireframeSoloVisible && pvsFaces) {
            // "SOLO LO QUE SE DIBUJA" (default): con visibilidad por triangulo activa
            // (modificador Culling: pvsFaces es la lista de la celda/sector) las lineas
            // se arman DE ESA LISTA, no de todas las aristas -- lo que el relleno
            // dibuja es lo que el alambre muestra. Celda vacia = ninguna linea (igual
            // que el relleno; si el archivo declara sectorFallback, pvsFaces ya trae el
            // paracaidas y aca ni se nota). Aristas deduplicadas (W3dDibujarLineasMat).
            gfx::Disable(gfx::Lighting); gfx::Disable(gfx::Texture2D);
            gfx::Disable(gfx::Blend); gfx::Disable(gfx::CullFace);
            gfx::DisableArray(gfx::ColorArray);
            gfx::DisableArray(gfx::NormalArray);
            gfx::DisableArray(gfx::TexCoordArray);
            gfx::Color4f(col[0], col[1], col[2], 1.0f);
            W3dDibujarLineasMat(pvsFaces, pvsFacesSize, select ? 2.0f : 1.0f);
            gfx::Invalidate();
        } else if (!edges.empty()) {
            RenderBordes(col, select ? 2.0f : 1.0f, false);
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
        const bool solido = w3dRenderSolido;
        const bool conLuz = !w3dRenderSinLuz;
        Material* ultimo = NULL;
        bool nmListo = false; // los nmColors (L en tangent-space) se calculan UNA vez por frame
        // SELECCION de una malla NO EDITABLE: sin edges no hay contorno -> tinte verde ~25%
        // en el propio material (mas barato que redibujar la malla y sin z-fighting). Solo
        // editor con overlays (el hook existe): jugando o exportado nunca tinta.
        tintaNoEditable = noEditable && select && w3dRenderOverlays && w3dVerSeleccion && g_meshOverlayHook && !solido;

        // MALLA GENERADA por modificadores: se dibuja el PREVIEW en Object Y en Edit Mode (real-time; en Edit el
        // overlay de vertices/aristas -editable- se dibuja ENCIMA -> editas el original y ves el resultado). En Edit,
        // GenerarMallaModificada saltea los modificadores con mostrarEdit=false (edicion mas rapida, N95). Draw SIMPLE.
        const bool useGen = (genValido && genVertex && genFaces);
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
        // ATRAS con DepthRange (RenderBordes pushBack=true): misma silueta, sin mover las caras.
        const bool xrayEdit = (editActiva && g_xray); // modo X-Ray (independiente de Show Overlays)
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
        if (editActiva) { gfx::Enable(gfx::PolygonOffsetFill); gfx::PolygonOffset(2.0f, 4.0f); }

        // FAST PATH VBO: la malla se dibuja desde memoria de GPU en vez de re-transferir los client-arrays por frame
        // -> gran salto de FPS orbitando (el MBX del N95 no re-lee el bus). Se sube 1 vez (o al cambiar geometria/pose)
        // y se bindean los VBOs (override de los punteros client). Cubre Solid Y Material Preview con materiales SIMPLES
        // (sin chrome/normalmap/capas, que re-bindean arrays por-corner -> esos quedan client-side). Bordes/overlays/gen
        // tambien siguen client-side. Fallback total si el driver no tiene VBOs (VBOSoportado()==false).
        bool anyFancy = false;
        if (!solido) for (size_t gg = 0; gg < materialsGroup.size(); gg++){ Material* mt = materialsGroup[gg].material;
            if (mt && (mt->chrome || mt->normalMap || !mt->capas.empty())) { anyFancy = true; break; } }
        // (P3) W3D_OVERRIDE de visibilidad por triangulo ACTIVO: se decide ACA (antes del
        // setup del VBO) porque cambia el camino de los indices: los del subconjunto
        // van como INDICES DE CLIENTE sobre los atributos del VBO estatico. El IBO
        // estatico (la malla completa) NO SE TOCA NUNCA MAS al cambiar de celda: el
        // glBufferData por cambio de sector que vivia aca era exactamente el
        // anti-patron del stall del tiler MBX (re-especificar un buffer que el tiler
        // todavia lee del frame anterior).
        const bool usePvs = (!useGen && !editActiva && pvsFaces &&
                             pvsGroups.size() == materialsGroup.size());
        bool drawVBO = false;
        if (gfx::VBOSoportado() && !useGen && !weightPaintOn && !vertexPaintOn && !editActiva && !anyFancy && faces && facesSize >= 3) {
            unsigned geomVer = skinGeomVersion;
            unsigned poseSer = skinArmature ? skinArmature->poseSerial : 0u; // pose ACTUAL del esqueleto (sube al posar/animar)
            // atributos ESTATICOS (col/uv/idx + pos/nor base): re-subir solo al cambiar la geometria
            if (vboGeomVer != geomVer || vboVertN != vertexSize || !vboPos) { SubirVBO(posBuf, norBuf, false); vboGeomVer = geomVer; vboSkinFrame = lastSkinFrame; vboPoseSerial = poseSer; vboPoseSkinneada = (skinArmature != NULL); }
            // POSE skinneada: el VBO de pos/nor ya tiene la pose ACTUAL? Se compara por SERIAL, no por # de frame: posar
            // un hueso o elegir un clip en el MISMO frame cambia la pose sin cambiar el frame -> el chequeo por frame la
            // daba por "ya subida" y quedaba la malla STALL (el bug: el esqueleto se movia pero la malla no).
            bool poseEnVBO = (!skinArmature) || (vboPoseSerial == poseSer);
            if (skinArmature && !poseEnVBO) {
                // la pose cambio desde la ultima subida. Si esta ANIMANDO (cambio TAMBIEN desde el render anterior) NO
                // re-subimos: re-especificar el VBO que el tiler del MBX todavia lee del frame previo = STALL de sync
                // (la causa del jitter de fps). Se dibuja pos/nor de client-array este frame (mismo transfer, sin stall).
                // Cuando la anim se ASIENTA (pose estable entre 2 renders) subimos 1 vez y volvemos al VBO (recupera el
                // beneficio de orbitar un personaje pausado).
                bool animandoActivo = (poseSer != vboPoseSerialPrev);
                if (!animandoActivo) { SubirVBO(posBuf, norBuf, true); vboSkinFrame = lastSkinFrame; vboPoseSerial = poseSer; vboPoseSkinneada = true; poseEnVBO = true; }
            } else if (!skinArmature && vboPoseSkinneada) {
                SubirVBO(posBuf, norBuf, true); vboPoseSkinneada = false; // armature removido -> re-subir el bind (sino queda la ultima pose)
            }
            vboSkinFramePrev = lastSkinFrame;
            vboPoseSerialPrev = poseSer;
            if (vboPos && vboIdx && poseEnVBO) {
                gfx::VertexVBO(vboPos);
                if (norBuf && vboNor)      gfx::NormalVBO(vboNor);
                if (vertexColor && vboCol) gfx::ColorVBO(vboCol);
                if (uv && vboUV)           gfx::TexCoordVBO(vboUV);
                // (P3) con visibilidad activa el IBO estatico NO se bindea: los
                // indices del subconjunto van de CLIENTE (DrawTrianglesClientIdx)
                if (!usePvs) gfx::BindIndexVBO(vboIdx);
                drawVBO = true;
            }
            // si !poseEnVBO (malla skinneada animando): drawVBO=false -> cae al path CLIENT-ARRAY (pos/nor/col/uv/idx de
            // RAM, ya seteados arriba con VertexPointer3f(posBuf) etc). Sin re-spec del VBO -> sin stall del tiler.
        }
        vboRenderActivo = drawVBO; // AplicarMaterial bindea el uv VBO (no client-uv) mientras esto este activo

        // override PVS/Vis (modificador Culling por triangulo): dibuja SOLO los indices
        // del sector/celda activo (usePvs se decidio arriba, antes del setup del VBO).
        // No aplica con malla generada (genValido manda) ni en Edit Mode (editActiva:
        // se edita/dibuja la malla completa). pvsGroups espeja materialsGroup (mismo
        // orden), con los rangos recortados al subconjunto.
        size_t ng = useGen ? genMaterialsGroup.size() : materialsGroup.size();
        // ORDEN DE PASADA dentro de la malla: opaco (0) -> decal (1) -> transparente (2).
        // El decal necesita el z del piso YA escrito, y tiene que ir antes de los transparentes.
        // Orden ESTABLE y se arma solo si de verdad hay pasadas distintas: una malla normal
        // (todos los materiales en 0) no paga ni un recorrido extra ni una alocacion.
        static std::vector<size_t> ordenG; // buffer reusado entre mallas/frames (cero allocs por frame)
        ordenG.clear();
        {
            int primero = -1; bool mezclado = false;
            for (size_t gg = 0; gg < ng && !mezclado; gg++) {
                Material* mt = (useGen && gg < materialsGroup.size()) ? materialsGroup[gg].material
                                                                     : (useGen ? genMaterialsGroup[gg].material : materialsGroup[gg].material);
                int op = (solido || !mt) ? 0 : mt->orden_pasada;
                if (primero < 0) primero = op; else if (op != primero) mezclado = true;
            }
            if (mezclado) for (int pasada = 0; pasada <= 2; pasada++)
                for (size_t gg = 0; gg < ng; gg++) {
                    Material* mt = (useGen && gg < materialsGroup.size()) ? materialsGroup[gg].material
                                                                         : (useGen ? genMaterialsGroup[gg].material : materialsGroup[gg].material);
                    int op = (solido || !mt) ? 0 : mt->orden_pasada;
                    if (op < 0) op = 0; if (op > 2) op = 2;
                    if (op == pasada) ordenG.push_back(gg);
                }
        }
        for (size_t gi = 0; gi < ng; gi++) {
            const size_t g = ordenG.empty() ? gi : ordenG[gi];
            const MaterialGroup& grp = useGen ? genMaterialsGroup[g] : materialsGroup[g];
            const MaterialGroup& dgrp = usePvs ? pvsGroups[g] : materialsGroup[g]; // rangos que se DIBUJAN (path no-gen)
            const MeshIndex* idxDraw = usePvs ? pvsFaces : faces;
            // El material es un PUNTERO. genMaterialsGroup es un SNAPSHOT (copia) tomado al GENERAR la malla: si
            // despues se reasigna el material del mesh part, ese snapshot apunta al material VIEJO y el cambio no se
            // ve hasta regenerar (entrar a Edit Mode, mover un vert...). Para que sea responsive, el material se toma
            // LIVE de materialsGroup[g] (mismo indice de grupo; gen copia el orden). Solo cambia el material, no la geo.
            Material* mat = (useGen && g < materialsGroup.size()) ? materialsGroup[g].material : grp.material;
            if (solido || !mat) mat = MaterialDefecto;
            if (mat != ultimo) { AplicarMaterial(mat, conLuz, solido, editActiva); ultimo = mat; }
            if (useGen) { // malla generada
                // REFLEJO por SOFTWARE (equirect mode 2, o sphere-sw sin texgen): dibuja los corners NO-INDEXADOS
                // con las UV de reflejo calculadas desde la GEO GENERADA. Asi el reflejo se ve sin "aplicar" el
                // modificador. Solo se calcula para los mesh parts con chrome (ActualizarChromeUVGen lo filtra + cachea).
                // matcap (mode 0) por SOFTWARE cuando no hay HW (GLES2) -> junto al equirect/sphere-sw. El matcap
                // HW (matriz de textura) solo existe en PC/N95 (TieneTexGen).
                bool chromeSW = !solido && mat->chrome && (mat->reflectMode == 2 || (mat->reflectMode == 1 && !gfx::TieneTexGen()) || (mat->reflectMode == 0 && !gfx::TieneTexGen()));
                bool matcapHW = !solido && mat->chrome && mat->reflectMode == 0 && gfx::TieneTexGen();
                bool sphereHW = !solido && mat->chrome && mat->reflectMode == 1 && gfx::TieneTexGen();
                if (chromeSW && genNormals) {
                    ActualizarChromeUVGen();
                    if (genChromeExpPos && genChromeExpUV && genChromeCount == genFacesSize) {
                        gfx::VertexPointer3f(0, genChromeExpPos);
                        gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, genChromeExpUV);
                        gfx::DrawTrianglesArrayFrom(grp.startDrawn, grp.indicesDrawnCount);
                        gfx::VertexPointer3f(0, genVertex); // re-bindea las posiciones gen para el proximo grupo
                        ultimo = NULL;                      // cambiamos punteros -> re-AplicarMaterial el proximo
                        continue;
                    }
                }
                if (matcapHW && genNormals) { // matcap HW (solo PC/N95): normales como texcoords (matriz de textura ya puesta)
                    gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer3b(genNormals, genVertexSize);
                    gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
                    ultimo = NULL; // cambiamos el puntero de texcoords -> re-AplicarMaterial el proximo
                    continue;
                }
                // sphere HW (el texgen genera las texcoords solo) o NO-chrome: draw indexado. AplicarMaterial
                // re-bindea el TexCoordPointer al uv BASE -> volvemos a genUV (sino la textura del Screw/Subdiv sale
                // con las UV del perfil, mal indexadas). Con texgen (sphereHW) NO forzamos genUV.
                if (!sphereHW && genUV) { gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, genUV); }
                gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
                // LINEAS del material (aristas encima del relleno, mismo estado)
                if (!solido && mat->lineas && grp.indicesDrawnCount >= 3) {
                    W3dDibujarLineasMat(&genFaces[grp.startDrawn], grp.indicesDrawnCount, mat->grosorLinea);
                    ultimo = NULL; // el trazo toco el z-func: re-aplicar el proximo material
                }
                continue;
            }
            // REFLECTION por SOFTWARE (equirect SIEMPRE, o sphere exacto en GLES1/N95): render NO INDEXADO por-corner.
            // El MATCAP (matriz de textura) y el sphere HW van por el draw INDEXADO normal (else) -> no entran aca.
            if (!solido && mat->chrome && (mat->reflectMode == 2 || (mat->reflectMode == 1 && !gfx::TieneTexGen()) || (mat->reflectMode == 0 && !gfx::TieneTexGen())) && chromeExpPos && chromeExpUV) {
                gfx::VertexPointer3f(0, chromeExpPos);
                gfx::TexCoordPointer2f(0, chromeExpUV);
                gfx::DrawTrianglesArrayFrom(materialsGroup[g].startDrawn, materialsGroup[g].indicesDrawnCount);
                gfx::VertexPointer3f(0, posBuf); // re-bindea las posiciones INDEXADAS para el resto/proximo grupo
                ultimo = NULL;                   // el puntero de UV cambio -> re-AplicarMaterial el proximo grupo
            } else if (drawVBO) {
                if (usePvs) // (P3) subconjunto visible: indices de CLIENTE + atributos en VBO (cero re-subida de buffers al cambiar de celda)
                    gfx::DrawTrianglesClientIdx(dgrp.indicesDrawnCount, &pvsFaces[dgrp.startDrawn]);
                else
                    gfx::DrawTrianglesVBO(dgrp.indicesDrawnCount, dgrp.startDrawn); // malla completa: desde el IBO estatico en GPU
            } else {
                gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
            }
            // LINEAS del material (Material::lineas): las aristas del rango recien
            // dibujado, con el mismo estado de material. Los indices van de CLIENTE
            // (la copia en RAM existe siempre: idxDraw) sobre los atributos que
            // esten bindeados (VBO o client arrays).
            if (!solido && mat->lineas && dgrp.indicesDrawnCount >= 3) {
                W3dDibujarLineasMat(&idxDraw[dgrp.startDrawn], dgrp.indicesDrawnCount, mat->grosorLinea);
                // DrawLinesClientIdx desbindea el ELEMENT_ARRAY: si el resto de la
                // malla dibuja desde el IBO estatico, volver a bindearlo
                if (drawVBO && !usePvs && vboIdx) gfx::BindIndexVBO(vboIdx);
                ultimo = NULL; // el trazo toco el z-func: re-aplicar el proximo material
            }

            // NORMAL MAPPING (DOT3) — pass 2 sobre la base: textura normal + N.L (color=L por vertice) en blend
            // MULTIPLY -> base * (N.L). Textura UNICA (sin multitextura) -> portable PC + N95. Excluyente con chrome.
            if (!solido && mat->normalMap && mat->normalTexture && mat->normalTexture->iID && uv) {
                CalcularTangentes();
                if (tangents) {
                    if (!nmListo) { ActualizarNormalMapColors(g_renderLightPos); nmListo = true; } // N.L con la LUZ de la escena
                    gfx::Enable(gfx::Blend); gfx::BlendMode(1);             // Multiply (oscurece por N.L)
                    gfx::DepthFunc(gfx::DepthEqual); gfx::DepthMask(false); // misma superficie, NO re-escribe z
                    gfx::Disable(gfx::Lighting); gfx::DisableArray(gfx::NormalArray);
                    gfx::Disable(gfx::ColorMaterial);
                    gfx::Enable(gfx::Texture2D);
                    gfx::BindTexture(mat->normalTexture->iID);
                    gfx::TexFilter(!gfx::PixeladoGlobal() && mat->filtrado); gfx::TexWrap(mat->repeat);
                    gfx::TexEnvDot3(true);                                  // combiner N.L
                    gfx::EnableArray(gfx::ColorArray); gfx::ColorPointer4ub(nmColors); // L como primary color
                    gfx::TexCoordPointer2f(0, uv);                         // mismas UV que la base
                    gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
                    gfx::TexEnvDot3(false);
                    // el pase del espejo dibuja SIN escribir z: restaurar DepthMask(true)
                    // aca le corromperia el z-buffer a lo que venga despues
                    gfx::DepthFunc(gfx::DepthLess); gfx::DepthMask(!gfx::w3dPaseEspejoSinZ);
                    gfx::Disable(gfx::Blend); gfx::BlendAlpha();
                    ultimo = NULL; // cambio textura/color/blend -> re-AplicarMaterial el proximo grupo
                }
            }

            // CAPAS EXTRA (multi-pass, eficiente: 1 draw por capa sobre la MISMA superficie con su blend).
            // Comparten el UV del modelo. GL 1.1 -> anda igual en PC y N95 (sin multitextura/extensiones).
            if (!solido && !mat->capas.empty() && uv) {
                gfx::Enable(gfx::Blend);
                gfx::DepthFunc(gfx::DepthEqual); gfx::DepthMask(false); // misma superficie, NO re-escribe z
                for (size_t c = 0; c < mat->capas.size(); c++) {
                    const TexLayer& cap = mat->capas[c];
                    if (!cap.on || !cap.tex) continue;
                    gfx::Enable(gfx::Texture2D);
                    gfx::BindTexture(cap.tex->iID);
                    gfx::BlendMode(cap.blend); // Mix / Multiply / Add
                    gfx::DrawTriangles(dgrp.indicesDrawnCount, &idxDraw[dgrp.startDrawn]);
                }
                gfx::DepthFunc(gfx::DepthLess); gfx::DepthMask(!gfx::w3dPaseEspejoSinZ); // idem: en el pase del espejo NO se escribe z
                gfx::Disable(gfx::Blend); gfx::BlendAlpha(); // restaura el blend func default
                ultimo = NULL; // la textura/blend cambio -> re-AplicarMaterial el proximo grupo
            }
        }
        // (E3) TRANSPARENTES EN ORDEN DE LISTA: con pvsOrdenado, los tris transparentes
        // NO se dibujaron en el loop de grupos (sus pvsGroups quedaron con count 0):
        // salen ACA, run por run, en el ORDEN del dato precalculado (lejos->cerca = el
        // orden del pintor). Sin sort en runtime: la lista ES el orden. Cada run es un
        // rango contiguo de pvsFaces de un solo material; el material se re-aplica solo
        // al cambiar (runs vecinos del mismo atlas no pagan nada).
        if (usePvs && pvsOrdenado && !pvsRuns.empty()) {
            for (size_t r = 0; r < pvsRuns.size(); r++) {
                const PvsRun& run = pvsRuns[r];
                if (run.grupo < 0 || run.grupo >= (int)materialsGroup.size() || run.count <= 0) continue;
                Material* mat = materialsGroup[run.grupo].material;
                if (solido || !mat) mat = MaterialDefecto;
                if (mat != ultimo) { AplicarMaterial(mat, conLuz, solido, editActiva); ultimo = mat; }
                if (drawVBO) gfx::DrawTrianglesClientIdx(run.count, &pvsFaces[run.start]);
                else         gfx::DrawTriangles(run.count, &pvsFaces[run.start]);
                if (!solido && mat->lineas && run.count >= 3) { // LINEAS del material
                    W3dDibujarLineasMat(&pvsFaces[run.start], run.count, mat->grosorLinea);
                    ultimo = NULL;
                }
            }
        }
        if (drawVBO) { gfx::UnbindVBOs(); vboRenderActivo = false; } // volver a client-side (bordes/overlays/proximas mallas usan RAM)
        } // fin del relleno normal (else de xrayEdit)

        gfx::PolygonOffset(0.0f, 0.0f);
        gfx::Disable(gfx::PolygonOffsetFill);
        // el estado que puede haber dejado un material DECAL / ADITIVO: si no se restaura,
        // la proxima malla (o la UI) hereda "no escribas z" o "sumale al fondo".
        gfx::DepthMask(!gfx::w3dPaseEspejoSinZ);
        gfx::BlendAlpha();
        gfx::TexMatrixMatcap(false); // CLAVE N95: resetea la matriz de textura del MATCAP. En PC el TexGenSphere(false)
                                     // de abajo la limpiaba de paso (hace glLoadIdentity), pero en el N95 TexGenSphere
                                     // es un stub (no hay texgen) -> la matriz quedaba sucia -> la UI texturada (fuente/
                                     // iconos) salia con texcoords transformadas = INVISIBLE. Sirve en los 4 OS.
        gfx::TexGenSphere(false); // resetea el chrome (que no leakee al contorno/overlays/proxima malla)
        gfx::TexEnvReplace(false); // vuelve a GL_MODULATE: sino la UI/fuente quedan sin tinte de color
        gfx::DepthFunc(gfx::DepthLess);
        if (gFondoActivo) { gfx::DepthRange(0.0f, 1.0f); gFondoActivo = false; } // el cielo (FONDO) deja el range tocado

        // overlays (contorno de seleccion / normales / overlay de edit): los dibuja el EDITOR
        // via hook, JUSTO tras el relleno (mismo timing que antes). NULL en una app sin editor.
        if (g_meshOverlayHook) g_meshOverlayHook(this);
    }
}


// dibuja el buffer de bordes PRECALCULADO (bordesBuf) como GL_LINES. No arma nada
// por frame: solo color + DrawLines. Lo usan el contorno de seleccion y el wireframe.
void Mesh::RenderBordes(const float* color, float width, bool pushBack) {
    namespace gfx = w3dEngine;
    // con malla GENERADA por modificadores (subdiv/screw) el contorno usa SUS aristas de poligono; sino la
    // base (bordesBuf). Sin esto, un objeto con modificador no mostraba borde de seleccion (la base no coincide).
    const bool usaGen = (genValido && !genBordesBuf.empty() && genVertex);
    const std::vector<GLfloat>* bufPtr = usaGen ? &genBordesBuf : &bordesBuf;
    GLfloat* restore = usaGen ? genVertex : vertex;
    // SKINNING del CONTORNO/overlay: si la malla se deforma por esqueleto (skinArmature), el contorno tambien debe
    // seguir la pose (sino la silueta verde queda en bind mientras el render se deforma). Las condiciones que pidio
    // (Object Mode solo si seleccionado / Edit Mode solo si "Display in Edit Mode") YA estan codificadas: el
    // contorno solo se dibuja al seleccionar, y skinArmature es NULL cuando "Display in Edit Mode" esta OFF en edit.
    if (!usaGen && skinArmature && skinVertex && !edges.empty()){
        extern int CurrentFrame;
        // gate por (frame, poseSerial): al elegir un clip o posar en el MISMO frame la pose cambia sin cambiar el frame
        // -> sin el serial el contorno se quedaba en la pose vieja (flotando) hasta el play.
        if (lastSkinBordesFrame != CurrentFrame || skinBordesPoseSerial != skinArmature->poseSerial || (int)skinBordesBuf.size() != (int)edges.size()*3){
            skinBordesBuf.resize(edges.size()*3);
            int nv = vertexSize; // cantidad de vertices
            for (size_t e = 0; e + 1 < edges.size(); e += 2){
                int a = edges[e], b = edges[e+1];
                if (a<0||a>=nv||b<0||b>=nv){ // fuera de rango: colapsar al extremo valido (o degenerar) -> nunca dibujar al origen
                    int v = (a>=0&&a<nv)?a : ((b>=0&&b<nv)?b : -1);
                    float px = v>=0?skinVertex[v*3]:0, py = v>=0?skinVertex[v*3+1]:0, pz = v>=0?skinVertex[v*3+2]:0;
                    skinBordesBuf[e*3]=skinBordesBuf[e*3+3]=px; skinBordesBuf[e*3+1]=skinBordesBuf[e*3+4]=py; skinBordesBuf[e*3+2]=skinBordesBuf[e*3+5]=pz;
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
                    skinBordesBuf[e*3]=skinBordesBuf[e*3+3]=skinVertex[a*3];
                    skinBordesBuf[e*3+1]=skinBordesBuf[e*3+4]=skinVertex[a*3+1];
                    skinBordesBuf[e*3+2]=skinBordesBuf[e*3+5]=skinVertex[a*3+2];
                    continue;
                }
                skinBordesBuf[e*3]=skinVertex[a*3]; skinBordesBuf[e*3+1]=skinVertex[a*3+1]; skinBordesBuf[e*3+2]=skinVertex[a*3+2];
                skinBordesBuf[e*3+3]=skinVertex[b*3]; skinBordesBuf[e*3+4]=skinVertex[b*3+1]; skinBordesBuf[e*3+5]=skinVertex[b*3+2];
            }
            lastSkinBordesFrame = CurrentFrame;
            skinBordesPoseSerial = skinArmature->poseSerial;
        }
        bufPtr = &skinBordesBuf; restore = skinVertex;
    }
    // AUTO-SYNC del contorno con una VERTEX ANIM: si estamos usando el bordesBuf base y
    // vertex[] cambio de version (EvalVertexAnim lo bumpea al deformar), re-armar el
    // contorno desde vertex[] -> la silueta sigue la pose en Modo Objeto (en Edit el
    // contorno sale del EditMesh, ya sincronizado). Editar no bumpea la version -> no dispara.
    if (bufPtr == &bordesBuf && bordesGeomVersion != skinGeomVersion) RefrescarBordesDesdeVertex();
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
//  VIVE EN EL CORE, NO EN EL EDITOR: la usa Mesh::PodarMarcasSinArista (aca
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
//  (ver Mesh::ReanclarMarcasPos y la clase W3dMoverVerts en Mesh.h)
// ============================================================================
// re-escribe las claves de un set de marcas segun 'remap' (posicion VIEJA -> NUEVA,
// 12 bytes crudos cada una). Una clave que no esta en el mapa queda tal cual.
static void W3dReanclarMarcas(std::set<std::string>& edges,
                              const std::map<std::string, std::string>& remap) {
    if (edges.empty() || remap.empty()) return;
    std::set<std::string> nuevo;
    for (std::set<std::string>::const_iterator it = edges.begin(); it != edges.end(); ++it) {
        if (it->size() != 24) { nuevo.insert(*it); continue; }   // clave rara: no la toco
        std::string a = it->substr(0, 12), b = it->substr(12, 12);
        std::map<std::string, std::string>::const_iterator ia = remap.find(a);
        std::map<std::string, std::string>::const_iterator ib = remap.find(b);
        if (ia != remap.end()) a = ia->second;
        if (ib != remap.end()) b = ib->second;
        if (a == b) continue;   // los dos extremos cayeron encima: la arista se colapso
        nuevo.insert((a < b) ? (a + b) : (b + a));   // el borde no tiene direccion
    }
    edges.swap(nuevo);
}

void Mesh::ReanclarMarcasPos(const GLfloat* posViejas, int nViejo) {
    if (!posViejas || !vertex) return;
    if (nViejo != vertexSize) return;            // REBUILD (otra numeracion), no un move
    if (sharpEdges.empty() && seamEdges.empty()) return;
    std::map<std::string, std::string> remap;    // posicion VIEJA -> NUEVA (12 bytes crudos)
    for (int i = 0; i < nViejo; i++) {
        const char* pv = (const char*)&posViejas[i * 3];
        const char* pn = (const char*)&vertex[i * 3];
        if (memcmp(pv, pn, 12) == 0) continue;   // no se movio
        // gana el PRIMERO: dos verts INDEPENDIENTES encimados que se separan dan dos
        // destinos para la misma clave vieja y no hay forma de distinguirlos (mismo
        // criterio que tenia EditMesh::EmpujarPosiciones).
        remap.insert(std::make_pair(std::string(pv, 12), std::string(pn, 12)));
    }
    if (remap.empty()) return;
    W3dReanclarMarcas(sharpEdges, remap);
    W3dReanclarMarcas(seamEdges, remap);
}

// ----------------------------------------------------------------------------
//  PODA de las marcas que ya no son una arista de esta malla (ver Mesh.h).
//  El set de referencia son las ARISTAS REALES: edges[] (faces3d + looseEdges,
//  dedupeadas por posicion en CalcularBordes) llevadas a la misma clave de
//  POSICION con la que se guardan sharpEdges/seamEdges.
// ----------------------------------------------------------------------------
static int W3dPodarMarcas(std::set<std::string>& marcas, const std::set<std::string>& reales) {
    if (marcas.empty()) return 0;
    int tiradas = 0;
    std::set<std::string> nuevo;
    for (std::set<std::string>::const_iterator it = marcas.begin(); it != marcas.end(); ++it) {
        if (reales.count(*it)) nuevo.insert(*it);
        else tiradas++;
    }
    marcas.swap(nuevo);
    return tiradas;
}

// ----------------------------------------------------------------------------
//  LA MALLA ESTA POSADA POR UNA VERTEX ANIM? (contrato completo en Mesh.h)
//  ESTADO REAL: lo dice el flag que escribe el PLAYBACK, no una comparacion de
//  posiciones (que confunde "posada" con "editada sin autokey"; ver Mesh.h).
// ----------------------------------------------------------------------------
bool Mesh::PosadaPorVertexAnim() const {
    if (!vertex || vertexSize <= 0) return false;
    if (!posadaPorAnim) return false;                   // nadie la poso: EN REPOSO
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
const GLfloat* Mesh::PosicionesReposo() const {
    if (!vertex || vertexSize <= 0) return vertex;
    if (!PosadaPorVertexAnim()) return vertex;          // el caso normal: no hay nada que pensar
    const GLfloat* base = PosBaseVertexAnim();
    // POSADA pero sin un base usable (frames desalineados a mitad de una op de
    // topologia): lo mejor que hay es vertex[]. Es el mismo lado conservador que
    // toma la poda, que en ese estado directamente no decide nada.
    return base ? base : vertex;
}

int Mesh::PodarMarcasSinArista() {
    // GUARD DE MOVIMIENTO ABIERTO: vertex[] ya se movio pero las claves todavia no se
    // re-anclaron (eso pasa al CERRAR el guard). En ese estado intermedio toda marca
    // parece huerfana; podar aca borraria la marca en vez de SEGUIR al vertice.
    if (moverVertsAbiertos > 0) return 0;
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
    if (PosadaPorVertexAnim()) return 0;
    if (edges.empty()) return 0;   // hay geometria pero los bordes no estan al dia: no decido
    std::set<std::string> reales;
    for (size_t i = 0; i + 1 < edges.size(); i += 2) {
        const int a = edges[i], b = edges[i + 1];
        if (a < 0 || b < 0 || a >= vertexSize || b >= vertexSize || a == b) continue;
        reales.insert(SharpEdgeKey(&vertex[a * 3], &vertex[b * 3]));
    }
    if (reales.empty()) return 0;
    return W3dPodarMarcas(sharpEdges, reales) + W3dPodarMarcas(seamEdges, reales);
}

W3dMoverVerts::W3dMoverVerts(Mesh* m) : malla(m), cerrado(false) {
    if (!malla) { cerrado = true; return; }
    // MIENTRAS EL GUARD ESTA ABIERTO la malla queda marcada como "a medio mover": si algo
    // recalcula los bordes en el medio, la poda se saltea (ver PodarMarcasSinArista). El
    // contador sube SIEMPRE (cuesta un int), no solo cuando hay marcas que snapshotear.
    malla->moverVertsAbiertos++;
    // GRATIS cuando no hay nada que re-anclar (el caso normal): ni copia el snapshot
    if (!malla->vertex || malla->vertexSize <= 0) return;
    if (malla->sharpEdges.empty() && malla->seamEdges.empty()) return;
    viejo.assign(malla->vertex, malla->vertex + (size_t)malla->vertexSize * 3);
}

void W3dMoverVerts::Cerrar() {
    if (cerrado) return;
    cerrado = true;   // ANTES de re-anclar: idempotente aunque re-entre
    if (!malla) return;
    // 1ro RE-ANCLAR (las claves siguen al vertice), 2do bajar el contador: recien ahi la
    // malla vuelve a estar en un estado donde podar es correcto.
    if (!viejo.empty()) malla->ReanclarMarcasPos(&viejo[0], (int)(viejo.size() / 3));
    if (malla->moverVertsAbiertos > 0) malla->moverVertsAbiertos--;
}

W3dMoverVerts::~W3dMoverVerts() { Cerrar(); }

// ============================================================================
//  W3dPosVerts -- ver el contrato completo (y el por que) en Mesh.h
//  Las posiciones y el ESTADO DE POSE viajan juntos SIEMPRE: no hay una sola
//  entrada de esta clase que toque uno de los dos sin el otro.
// ============================================================================
void W3dPosVerts::Capturar(const Mesh* m) {
    pos.clear();
    posada = false;
    if (!m) return;
    if (m->vertex && m->vertexSize > 0) {
        pos.assign(m->vertex, m->vertex + (size_t)m->vertexSize * 3);
        posada = m->posadaPorAnim;   // <- el estado QUE CORRESPONDE a estas posiciones
    }
    // sin buffer no hay posiciones que guardar y por lo tanto tampoco pose (queda false)
}

void W3dPosVerts::EscribirEn(Mesh* m) const {
    if (!m) return;
    delete[] m->vertex;
    m->vertex = NULL;
    if (!pos.empty()) {
        m->vertex = new GLfloat[pos.size()];
        for (size_t i = 0; i < pos.size(); i++) m->vertex[i] = pos[i];
    }
    // EL ESTADO VIAJA CON LAS POSICIONES. Sin buffer no hay pose posible.
    m->posadaPorAnim = m->vertex ? posada : false;
}

void W3dPosVerts::IntercambiarCon(Mesh* m) {
    // SIN CORRESPONDENCIA POR INDICE (la topologia cambio) no se toca NADA: mezclar
    // medio buffer con el estado del otro es peor que no hacer nada.
    if (!m || !m->vertex || (int)pos.size() != m->vertexSize * 3) return;
    for (size_t i = 0; i < pos.size(); i++) { GLfloat c = m->vertex[i]; m->vertex[i] = pos[i]; pos[i] = c; }
    bool c = m->posadaPorAnim; m->posadaPorAnim = posada; posada = c;
}

void W3dPosVerts::Intercambiar(W3dPosVerts& o) {
    pos.swap(o.pos);
    bool c = posada; posada = o.posada; o.posada = c;
}
