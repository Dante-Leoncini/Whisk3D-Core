#ifndef MESH_RESOURCE_H
#define MESH_RESOURCE_H
// ============================================================================
//  MeshResource — LA MALLA 3D COMO RECURSO COMPARTIDO (el "datablock" de mesh
//  de Blender): una malla con NOMBRE PROPIO, independiente de los objetos que
//  la usan, guardada UNA vez en su entrada .w3db del contenedor y cargada UNA
//  vez en memoria (CPU y GPU) sin importar cuantos objetos la dibujen.
//
//  PARA QUE: mil arboles iguales eran mil copias de la misma geometria (en el
//  archivo, en el heap y en la GPU). Con el recurso son UNA entrada, UN juego
//  de arrays y UN juego de VBOs; cada objeto guarda solo lo suyo (transform,
//  pose de skinning, vertex anims, flipbook).
//
//  EL MODELO (evolucion de MeshData, que se queda con los arrays crudos):
//   - REGISTRO del proyecto (project.json):
//       "mallas": [{"nombre": "Arbol", "entrada": "meshes/arbol.w3db", "carpeta": "Vegetacion"}]
//     y el objeto malla dice "malla": "Arbol". Un recurso del registro sin
//     usuarios (HUERFANO) se conserva igual: es del proyecto, no de un objeto.
//   - EN MEMORIA: un MeshResource por entrada del registro, vivo toda la sesion
//     (el registro es el dueno). Los que vienen del ARCHIVO no se borran hasta
//     cerrar el proyecto (sin usuarios son huerfanos y se conservan); los creados
//     en la sesion se PURGAN cuando se quedan sin usuarios y sin pasos de undo que
//     los nombren (W3dMeshesPurgeUnused). Sus ARRAYS se cargan recien cuando
//     alguien los pide (el primer objeto que lo usa, una ListaCarga "malla:") por
//     el almacen de recursos (io/W3dResources.h, tipo W3DREC_MESH, id = nombre de
//     entrada): cada objeto vinculado es UNA referencia; con la ultima que se
//     suelta, los arrays y los VBO se liberan (el recurso sigue en el registro).
//   - Mesh (el objeto) APUNTA ADENTRO: sus punteros de render (vertex, normals,
//     uv, vertexColor, faces) son los del recurso, marcados en Mesh::dataComp.
//     Todo el copy-on-write de siempre (Mesh::UninstantiateData) sigue valiendo:
//     una vertex anim, el flipbook o un script que escribe vertices se hacen su
//     copia propia de ESA capa y el resto sigue compartido.
//   - LOS DATOS DE EDICION (caras, capas, marcas, aristas) NO se cargan por
//     objeto: la malla queda con la edicion PENDIENTE y se materializa del recurso
//     (sus bloques de edicion, releidos de la entrada o de los bytes en memoria)
//     recien cuando alguien los pide: Edit Mode, el objeto ACTIVO (sus listas en
//     Properties, el editor UV), weight/vertex paint, un modificador geometrico,
//     exportar, guardar en texto. La del activo se SUELTA cuando deja de serlo.
//     Para MIRARLA sin editarla (wireframe, snap, seleccion por caja) alcanza la
//     topologia COMPARTIDA del recurso (aristas y caras, una copia para todos).
//   - VBO: una malla no skinneada y sin vertex anim dibuja con los VBO del
//     RECURSO (una subida para N objetos); una skinneada comparte color/uv/indices
//     y sube solo su pose. GLES2/WebGL sin VBO: arrays de cliente (los mismos
//     punteros compartidos).
//   - EDITAR la malla de un objeto edita el RECURSO (lo ven todos sus usuarios):
//     el editor serializa la malla editada y la aplica al recurso con
//     W3dMeshResourceApplyBytes, que re-apunta a todos los usuarios. "Hacer
//     unica" copia el recurso con otro nombre (Arbol.001) para ese objeto solo.
//
//  EL FORMATO CANONICO del recurso son sus BYTES .w3db: la entrada del contenedor
//  (sin cargar nada en memoria mientras nadie la toque) o, si se edito o se creo
//  en esta sesion, 'bytes'. Guardar un recurso sin cambios = copiar su entrada.
//  Una entrada de TEXTO (.w3dm, "formatoMallas": "texto") se convierte a .w3db una
//  vez al cargarla (W3dMeshTextToBinHook) y esos bytes quedan como cache.
//  El formato en disco (registro, dedup, huerfanas): formato/mallas-registro.md.
//
//  C++03 puro, compila en TODOS los builds (editor, juego compilado, Symbian):
//  no llama a nada de main/. El editor pone lo suyo (materiales por nombre del
//  archivo, publicar ediciones, el guardado) desde afuera.
//  Motor generico: aca no hay nombres de ningun juego.
// ============================================================================
#include "MeshData.h"
#include "io/W3dMeshBin.h"   // W3dMeshBinPart/Grupo/Forma: la malla de render sin Mesh
#include <string>
#include <vector>

class Material;
struct W3dResource;

struct MeshResource : public MeshData {
    std::string name;      // el nombre del recurso (unico en el proyecto)
    std::string entry;     // su entrada del contenedor ("meshes/arbol.w3db"); "" = nunca se guardo
    std::string folder;    // carpeta COSMETICA (outliner); no afecta al codigo
    std::string bytes;     // el .w3db EN MEMORIA (editado o creado en la sesion); "" = leer 'entrada'
    bool        modified;  // 'bytes' difiere de la entrada: guardar = escribirlos
    // 'bytes' es la CONVERSION de una entrada de TEXTO (.w3dm, proyecto con "formatoMallas":
    // "texto"): el recurso vive en binario, asi que el texto se convierte UNA vez y queda como
    // cache (no es una edicion: 'modificado' sigue en false). Se tira al descargar los arrays.
    bool        textBytes;
    // EN QUE FORMATO SE GUARDA (la opcion "Malla en texto" de su tarjeta Recurso): -1 = el de su entrada (.w3dm
    // texto, .w3db binario; una malla nueva, el default del proyecto); 0 = binario, 1 = texto, elegido y todavia sin
    // guardar (el guardado lo escribe asi y lo vuelve a -1). Ver W3dMeshResourceInText (io/ProjectMeshes.h)
    int         format;
    int         serial;      // identidad en la sesion (id del almacen mientras no tenga entrada;
                             // la del undo de un rename: el registro se purga, el serial no cambia)
    int         refsUndo;    // pasos de undo que lo nombran: mientras haya, no se purga
    // HUERFANO PEDIDO: creado en la sesion a proposito sin usuarios (el "Duplicate" de la biblioteca):
    // se conserva como un huerfano del archivo (la purga de la sesion no lo borra)
    bool        keep;
    // BORRADO por el usuario (la biblioteca): ya no es del proyecto (no se lista ni se guarda), pero
    // sigue vivo mientras algun paso de undo lo retenga (un objeto borrado que lo usaba). Si un undo le
    // devuelve un usuario a la escena vuelve a ser del proyecto; si no, la purga lo borra cuando nadie
    // mas lo nombra (aunque venga del archivo).
    bool        deleted;
    unsigned    version;     // sube con cada cambio de datos (republicar)
    W3dResource* rec;        // su descriptor en el almacen mientras los arrays estan cargados
    // TODAS las mallas vinculadas: las de la escena y las que un paso de undo tiene
    // detachadas (siguen vivas y apuntando adentro: al republicar hay que moverlas igual)
    std::vector<Mesh*> users;

    // lo que la malla de render trae ademas de los 5 arrays (W3dMeshBinReadRender)
    bool  smooth;
    int   primType; float primSize, primSize2, primHeight; int primVerts, primVerts2;
    std::vector<W3dMeshBinPart> parts;
    std::vector<Material*> materials;   // el material de cada parte (NULL = el por defecto)
    // capas UV EXTRA de render (bloque RUVX; ver Mesh::uvExtra). No son un array compartido: cada usuario
    // recibe SU copia al vincularse (son chicas y nunca pasan por el COW de MeshData).
    std::vector< std::vector<GLfloat> > uvExtra;
    // LOS EXTRAS (identidad de los puntos, vertex/UV groups, shape keys, reposo 2D): cada usuario
    // tiene SU copia (el skinning, el weight paint y el editor los leen de la malla), asi que el
    // recurso NO se queda con otra: la suelta apenas un usuario LIMPIO la tiene
    // (extrasInResource = false) y, para vincular al siguiente, copia la de un usuario limpio o
    // la relee de los bytes (W3dMeshBinReadExtras). Mil personajes con el mismo rig = mil
    // copias, como antes de los recursos; nunca mil y una.
    std::vector<int> ctrl;               // render-vert -> representante de su punto
    std::vector<W3dMeshBinGroup> groups, uvGroups;
    std::vector<W3dMeshBinShape> shapes;
    std::vector<float> uvRest;
    bool  extrasInResource;
    // la FIRMA de los extras (nombre y tamano de cada grupo, cuantas formas, el largo del
    // reposo 2D): lo que W3dMeshClean compara aunque el recurso ya no tenga los datos
    struct SignatureGroup { std::string name; size_t n; };
    std::vector<SignatureGroup> signatureGroups, signatureUVGroups;
    size_t signatureShapes, signatureUVRest;
    int   activeGroup, uvActiveGroup;
    float aabbMin[3], aabbMax[3], center[3], radius;
    bool  aabbOk;
    int   groupedVerts;
    bool  statsNoEditable;               // el AABB/centro se hornearon como escenario cerrado
    W3dForeign stamp;                    // sello por dominio + bloques .w3db que no se entienden + REQ

    // VBO COMPARTIDOS (una subida para todos los usuarios): 0 = sin crear
    void*        flows;                   // los flujos de indices derivados por parte (MeshFlows.h; NULL = sin armar)
    int          refsRoutine;             // pasos de Rutina que la dibujan (la mantienen cargada aunque no la use un objeto)
    unsigned int vboPos, vboNor, vboCol, vboUV, vboIdx;
    std::vector<unsigned int> vboUVx;    // las capas UV extra (uvExtra[k]; la 0 es la activa: su VBO es vboUV)
    unsigned     vboShow;                // 'version' con la que se subieron (0 = nunca)
    unsigned     rejectedVbo;            // 'version' + 1 cuyos VBO el driver no acepto (0 = ninguna): va desde RAM

    // LA TOPOLOGIA COMPARTIDA para MIRAR a los usuarios con la edicion PENDIENTE (wireframe,
    // snap a aristas y caras, seleccion por caja, estadisticas): las aristas y las caras del
    // recurso, leidas UNA vez de sus bloques de edicion cuando alguien las pide (nunca por
    // objeto: mil arboles en wireframe = una lista de aristas). Se tiran al descargar los
    // arrays y al republicar. Ver W3dMeshEdgesView / W3dMeshFacesView / W3dMeshBordersView.
    std::vector<int>      edges;        // pares de render-verts (lo que seria Mesh::edges)
    std::vector<GLfloat>  borders;      // esas aristas EN REPOSO como lineas (Mesh::bordersBuf)
    std::vector<MeshFace> polyFaces;    // lo que seria Mesh::faces3d
    int                   topoState;    // 0 = sin leer, 1 = aristas, 2 = aristas y caras, -1 = no se pudo
    // LA LIBRERIA de la que es ("" = del proyecto). Una malla de una libreria externa (otro .w3d de solo
    // lectura, ver W3dMeshResourceNewLib) NO esta en el registro del proyecto: no se lista, no se guarda ni se
    // edita. Su nombre lleva el prefijo ("Personajes/Cuerpo") y su entrada la ruta de la libreria
    // ("lib:Personajes/meshes/cuerpo.w3db"), asi que dos librerias no chocan y N proxies la comparten.
    std::string           lib;
    // SOLO EN GPU (la opcion de proyecto "mallasSoloGPU", ver W3dMeshesOnlyGPUActivate): una malla que solo dibujan
    // RUTINAS (sin usuarios Mesh) suelta sus arrays de RAM cuando sus VBO quedaron subidos; vertexSize, facesSize,
    // las partes, el AABB y el cache de cajas/indice maximo (MallaFlujos) se quedan. Se relee de sus bytes si algo
    // pide la RAM (W3dMeshResourceEnsureRAM): un objeto que pasa a usarla, el editor, un contexto GL perdido.
    bool                  onlyGPU;        // los arrays de RAM se soltaron (vertex/normals/uv/vertexColor/faces = NULL)
    bool                  ramFixed;       // un paso la lee de RAM en cada cuadro (el matcap): no se suelta
    bool                  releasePend;    // anotada para soltarse al cerrar la corrida de rutinas
    unsigned char         arraysHas;      // (onlyGPU) los arrays que tenia: mascara W3DMD_* (Mesh.h)
    std::vector<unsigned char> layersUVHas; // (onlyGPU) la capa UV extra k tenia una UV por vertice (subida a vboUVx[k])
    long                  releasedBytes; // (onlyGPU) la RAM que libero (arrays, capas UV, extras y la conversion de texto)

    MeshResource();
    ~MeshResource();
    bool Loaded() const { return rec != 0; }
    // la clave en el almacen de recursos: la entrada, o "*<serial>" mientras no tenga
    std::string IdStore() const;
private:
    MeshResource(const MeshResource&);             // sin copia (C++03: privadas y sin definir)
    MeshResource& operator=(const MeshResource&);
};

// ---------------------------------------------------------------------------
//  EL REGISTRO del proyecto (orden = el del archivo; los nuevos al final)
// ---------------------------------------------------------------------------
const std::vector<MeshResource*>& W3dMeshesRegistry();
MeshResource* W3dMeshResourceByName(const std::string& name);
MeshResource* W3dMeshResourceByEntry(const std::string& entry);
// indice en el registro (-1 = no esta)
int           W3dMeshResourceIndex(const MeshResource* r);
// por su serial de la sesion (NULL = ya no esta): la identidad ESTABLE de los pasos de undo
// (un rename de la malla o de su carpeta), porque el registro se purga y los indices se corren
MeshResource* W3dMeshResourceBySerial(int serial);
// nombre libre en el registro para 'base' ("Arbol" ocupado -> "Arbol.001"); 'excepto' puede conservarlo
std::string   W3dMeshFreeName(const std::string& base, const MeshResource* except = 0);
// la CARPETA cosmetica escrita por el usuario, normalizada: separador '/' (tambien acepta '\\'),
// sin espacios en los bordes de cada tramo y sin tramos vacios ("  Vegetacion // Arboles/ " ->
// "Vegetacion/Arboles"). "" = sin carpeta (la raiz de la vista de mallas).
std::string   W3dMeshFolderNormalize(const std::string& folder);
// crea un recurso VACIO en el registro con un nombre libre derivado de 'nombre'. Sus datos
// llegan por 'entrada' (proyecto que se abre) o por W3dMeshResourceSetBytes.
MeshResource* W3dMeshNewResource(const std::string& name, const std::string& entry,
                                   const std::string& folder);
// cierra el proyecto: borra el registro entero (lo llama RestartScene con el arbol YA
// destruido). Un recurso que todavia tuviera usuarios se deja vivo con un aviso (sus
// punteros no pueden quedar colgando).
void          W3dMeshesRegistryClear();
// ---- LAS MALLAS DE LAS LIBRERIAS EXTERNAS (aparte del registro del proyecto) ----
// un recurso de la libreria 'libreria' ('nombre' ya con su prefijo, 'entrada' ya con "lib:<libreria>/"). Lo
// encuentran W3dMeshResourceByName / PorEntrada (despues de los del proyecto), pero W3dMeshesRegistry no lo
// lista: no es del proyecto. Si ya habia uno con ese nombre, devuelve ese.
MeshResource* W3dMeshResourceNewLib(const std::string& name, const std::string& entry,
                                      const std::string& lib);
// las mallas de las librerias ("" = de todas): se borran las que nadie usa (las usadas quedan vivas con un aviso,
// como en W3dMeshesRegistryClear). W3dMeshesRegistryClear tambien las borra (cierre del proyecto).
void          W3dMeshesLibClear(const std::string& lib);
// cuantas mallas de librerias hay registradas (la prueba)
int           W3dMeshesLibCount();
// las mallas de las librerias (aparte del registro del proyecto; ver W3dMeshResourceNewLib)
const std::vector<MeshResource*>& W3dMeshesLib();
// PURGA los recursos que ya no sirven: creados en la sesion (sin entrada en el archivo), sin
// usuarios y sin ningun paso de undo que los nombre (un "New Copy" deshecho cuyo redo se
// descarto, un Shift+D borrado). Los del ARCHIVO no se purgan: son huerfanos del proyecto y se
// conservan. La llama el tick del editor y el guardado; nunca en medio de un vinculo.
// Devuelve cuantos borro.
int           W3dMeshesPurgeUnused();
// BORRA UN HUERFANO del registro aunque venga del ARCHIVO (el "Purge orphans" / "Delete" del
// outliner por recursos): el proximo guardado ya no lo escribe (su entrada se descarta como toda
// malla no referenciada). Solo si nadie lo usa: sin usuarios (tampoco detachados por un borrado
// deshacible) y sin pasos de undo que lo nombren. false = no se puede (queda como estaba).
bool          W3dMeshResourceDelete(MeshResource* r);
// los pasos de undo que guardan un MeshResource* lo RETIENEN (no se purga) hasta soltarlo
void          W3dMeshResourceUndoRetain(MeshResource* r);
void          W3dMeshResourceUndoRelease(MeshResource* r);

// ---------------------------------------------------------------------------
//  DATOS: los bytes canonicos y la carga de los arrays
// ---------------------------------------------------------------------------
// el .w3db del recurso: los bytes en memoria, o la entrada leida del contenedor. false = no hay.
bool W3dMeshResourceReadBytes(const MeshResource* r, std::string& out);
// reemplaza los datos del recurso por estos bytes (el editor, al crear/copiar/editar): quedan
// en memoria y 'modificado'. NO toca a los usuarios: para eso W3dMeshResourceApplyBytes.
void W3dMeshResourceSetBytes(MeshResource* r, const std::string& bytes);
// el material de cada parte al CARGAR un recurso por primera vez: lo pone el lector del
// proyecto (los nombres de PARTMAT son los del bloque "materiales" del archivo). NULL =
// FindMaterialByName (el nombre vivo).
extern Material* (*W3dMeshMaterialByNameHook)(const std::string& name);
// una entrada del registro guardada en TEXTO (.w3dm) se convierte a .w3db para cargarla: el
// lector de texto y la derivacion (index buffer, bordes, capas) son del lector del proyecto, que
// pone este gancho (import_w3d.cpp). 'nombre' va en INFO. NULL / false = no se puede leer.
extern bool (*W3dMeshTextToBinHook)(const unsigned char* data, size_t n, const std::string& name,
                                     std::string& out);

// ---------------------------------------------------------------------------
//  LOS USUARIOS
// ---------------------------------------------------------------------------
// VINCULA la malla al recurso: carga sus arrays si hace falta, suelta lo que la malla tenia
// (propio o compartido) y apunta sus capas de render ADENTRO del recurso. Copia lo chico por
// instancia (mesh parts con su material, grupos, formas, reposo 2D, AABB, sello). La edicion
// queda PENDIENTE (se materializa del recurso al pedirla) salvo con 'keepEdit': la
// malla YA tiene su edicion y coincide con el recurso (acaba de publicarlo o de guardarse).
// false = el recurso no se pudo cargar: la malla NO se toca.
bool W3dMeshLink(Mesh* m, MeshResource* r, bool keepEdit = false);
// lo llaman W3dMeshLink y W3dMeshMove ANTES de tocar nada: el editor publica ahi las mallas que
// salieron de Edit con la edicion sin publicar (la malla 'm' o un usuario de 'r'), asi quien se vincula
// ve el contenido al dia. NULL = sin editor (el runtime del juego).
extern void (*W3dMeshBeforeLinkHook)(Mesh* m, MeshResource* r);
// MUDA el vinculo de la malla a otro recurso SIN tocar sus datos: lo que compartia pasa a
// ser propio (copia) y la malla sigue con su edicion y su jaula (la malla en Edit Mode que un
// guardado separo a otro recurso). false = el recurso no se pudo cargar (no cambia nada).
bool W3dMeshMove(Mesh* m, MeshResource* r);
// suelta el recurso (lo llama Mesh::ReleaseDataShared, o sea ~Mesh): las capas
// compartidas quedan NULL, sale de la lista de usuarios y se suelta la referencia.
void W3dMeshUnlink(Mesh* m);
// cuantas veces una malla quedo SIN recurso en la sesion (contador monotono): el editor, que quiere
// que TODA malla tenga el suyo, solo recorre la escena cuando esto o W3dObjectsSpawned cambian
unsigned W3dMeshesUnlinked();
// la edicion materializada de una malla LIMPIA (sus capas siguen siendo las del recurso)
// vuelve a quedar pendiente: libera caras, capas y aristas que se pueden releer. No-op si la
// malla no es de un recurso o si algo suyo pudo cambiar.
void W3dMeshReleaseEdit(Mesh* m);
// usuarios que estan en la ESCENA (colgados de 'raiz'; si es NULL, de TODAS las raices cargadas:
// la activa y las escenas/prefabs inactivos, ver W3dLiveRoots)
int  W3dMeshResourceUsersInScene(const MeshResource* r, const Object* root = 0);
// la malla sigue siendo la del recurso? (todas las capas que el recurso tiene siguen
// compartidas y no hay edicion propia que pueda haberse tocado): guardar = sus bytes
bool W3dMeshClean(const Mesh* m);
// lo que la malla copia POR INSTANCIA del recurso sigue igual? (mesh parts con su material,
// vertex/UV groups, cuantas formas, el suavizado): la parte de W3dMeshClean que no mira la
// edicion ni los arrays. false = la malla tiene algo propio sin publicar (un grupo nuevo, un
// rename de mesh part) o no es de un recurso.
bool W3dMeshExtrasAsResource(const Mesh* m);
// los datos del recurso CAMBIARON (el editor publico una edicion): se reemplazan por 'bytes'
// (en memoria, modificado) y se re-apunta a TODOS los usuarios salvo 'excepto' (el que
// publico: se queda con lo suyo, que es lo mismo). 'mats' = el material de cada parte (el
// de la malla que publico; NULL = por nombre). false = los bytes no son un .w3db legible.
bool W3dMeshResourceApplyBytes(MeshResource* r, const std::string& bytes, Mesh* except,
                                 const std::vector<Material*>* mats);
// idem con VARIOS exceptuados (el que publica y la malla en Edit Mode: su jaula y sus capas son
// suyas y re-apuntarla las tiraria). Cada exceptuado se queda con una copia PROPIA de lo que ve.
bool W3dMeshResourceApplyBytes(MeshResource* r, const std::string& bytes, const std::vector<Mesh*>& exceptions,
                                 const std::vector<Material*>* mats);
// el material de cada parte del recurso (r->materials) resuelto aunque NO este cargado (un
// huerfano: se leen solo los nombres de su PART y se resuelven una vez por el nombre del archivo).
// 'inFile' (opcional) = los nombres de material que dicen sus bytes. false = no se pudo leer.
bool W3dMeshResourceMaterials(MeshResource* r, std::vector<std::string>* inFile);
// cambia el material de la parte 'k' en el RECURSO y en TODOS sus usuarios (el material es
// dato de la malla, como en Blender). Los bytes se regeneran al guardar (PART por nombre).
void W3dMeshResourceChangeMaterial(MeshResource* r, int k, Material* mat);

// ---------------------------------------------------------------------------
//  LA TOPOLOGIA PARA MIRAR (sin materializar la edicion)
//  La de la malla si la tiene en memoria; si su edicion esta PENDIENTE en su recurso, la
//  COMPARTIDA del recurso (leida una vez para todos sus usuarios). NULL = no hay (sin caras,
//  escenario cerrado a edicion, o no se pudo leer). Lo que devuelven es de SOLO LECTURA y vale
//  hasta el proximo cambio de la malla o del recurso (no guardar el puntero).
// ---------------------------------------------------------------------------
const std::vector<int>*      W3dMeshEdgesView(const Mesh* m);
const std::vector<MeshFace>* W3dMeshFacesView(const Mesh* m);
// las aristas como lineas EN REPOSO (lo que dibuja el wireframe): las de la malla, o las del
// recurso si la malla dibuja sus posiciones (no esta posada ni tiene copia propia)
const std::vector<GLfloat>*  W3dMeshBordersView(const Mesh* m);

// ---------------------------------------------------------------------------
//  RENDER: los VBO compartidos
// ---------------------------------------------------------------------------
// sube (una vez por version) los arrays del recurso a sus VBO. false = sin VBO en esta plataforma.
// Una malla onlyGPU con el VBO al dia da true sin RAM; con el VBO perdido se relee de sus bytes para subirlo.
// Con la opcion onlyGPU, una que puede soltar su RAM queda anotada (se suelta al cerrar la corrida de rutinas).
bool W3dMeshResourceEnsureVBO(MeshResource* r);
// ---------------------------------------------------------------------------
//  SOLO EN GPU (opcion de PROYECTO, project.json "mallasSoloGPU": true; default false = todo como siempre).
//  En el N95 el VBO vive en la misma RAM que el heap: una malla unida del mundo estaba DOS veces. Con la opcion,
//  una malla cargada SIN usuarios Mesh y retenida por rutinas, con TODOS sus arrays ya en VBO, suelta su RAM al
//  cerrar la corrida de rutinas en la que se subio (no antes: esa corrida todavia arma flujos y cajas). Lo que
//  necesite la RAM despues la relee de los BYTES (como al cargarla) y la malla vuelve a ser como cualquier otra;
//  si sigue sin usuarios, se vuelve a soltar en la proxima corrida. Prueba: tools/pruebas/prueba_mallas_solo_gpu.w3s
//  (el cuadro y sus llamadas GL son los mismos con la opcion prendida y apagada). Doc: docs/rutinas.md.
// ---------------------------------------------------------------------------
void W3dMeshesOnlyGPUActivate(bool on);       // off = las sueltas vuelven a RAM
bool W3dMeshesOnlyGPUActive();
// los arrays de RAM de vuelta (relee los bytes si estaba onlyGPU). true = estan (o la malla no tiene geometria)
bool W3dMeshResourceEnsureRAM(MeshResource* r);
// suelta la RAM si se puede (opcion prendida, sin usuarios, retenida por una rutina, VBO al dia, no fija):
// antes deja cacheadas las cajas y el indice maximo de cada parte. true = la solto
bool W3dMeshResourceReleaseRAM(MeshResource* r);
// (la Rutina, al cerrar la corrida) suelta las anotadas por W3dMeshResourceEnsureVBO
void W3dMeshesOnlyGPUReleasePending();
// el recurso TIENE ese array (mascara W3DMD_*): en RAM o, si esta onlyGPU, en su VBO
bool W3dMeshResourceHasArray(const MeshResource* r, unsigned mask);
// la capa UV extra k (> 0) con una UV por vertice: en RAM o, si esta onlyGPU, la que subio a su VBO
bool W3dMeshResourceHasLayerUV(const MeshResource* r, int layer);
// EL CONTEXTO GL SE PERDIO: los VBO/IBO de los recursos ya no existen (se olvidan sin glDelete) y se vuelven a
// subir al dibujar; una malla onlyGPU se relee de sus bytes para eso
void W3dMeshesVBOLost();
// (meminfo) cuantas estan onlyGPU y la RAM que liberaron; cuantas veces se releyo una
int  W3dMeshesOnlyGPUCount(long* savedBytes);
int  W3dMeshesOnlyGPUReloads();

// (la Rutina) una referencia PERMANENTE sobre el recurso: lo carga si hace falta y lo deja cargado aunque ningun
// objeto lo use. true = cargado. Cada Retener se paga con un Soltar
bool W3dMeshResourceRetain(MeshResource* r);
void W3dMeshResourceRelease(MeshResource* r);
// el recurso sigue en el registro y no se borro (un puntero guardado por una rutina)
bool W3dMeshResourceLive(const MeshResource* r);
// sube cada vez que un recurso sale del registro (un puntero vivo sigue vivo mientras no cambie)
unsigned W3dMeshesGen();
// sube con todo lo que puede cambiar lo que valida el editor de una RUTINA: sus pasos (Resolver, el panel, setPaso, la
// animacion de pasos) y los datos de una malla (version). Jugando, una rutina se revalida solo si cambio (RutinaEditor)
extern unsigned g_w3dRoutinesGen;

// ---------------------------------------------------------------------------
//  EDICION PENDIENTE y MALLAS EDITADAS
// ---------------------------------------------------------------------------
// marca que va en Mesh::pendingEdit cuando la edicion sale del recurso (no de una entrada suelta)
extern const char* const W3D_RESOURCE_EDIT;
// las dos puertas al render de la malla (RefreshRender / GenerateRender, en el editor)
// anotan aca las mallas VINCULADAS que cambiaron: el editor las publica al recurso cuando
// salen de edicion. Sin editor nadie las procesa (una entrada por malla, como mucho).
void W3dMeshMarkEdited(Mesh* m);
// saca y devuelve las anotadas (el que las procesa)
void W3dMeshesTakeEdited(std::vector<Mesh*>& out);

// ---------------------------------------------------------------------------
//  MEDICION (meminfo / mallainfo)
// ---------------------------------------------------------------------------
int  W3dMeshesResourcesLoaded();        // recursos con los arrays en memoria
int  W3dMeshesVBOShared();              // recursos con VBO compartido subido
long W3dMeshesVBOBytes();               // bytes de GPU de esos VBO

// el alta de los ops del tipo W3DREC_MESH en el almacen (Cargar/Liberar) y del gancho con
// el que Mesh suelta una MeshData legacy. Idempotente.
void W3dMeshesEnsureOps();

#endif
