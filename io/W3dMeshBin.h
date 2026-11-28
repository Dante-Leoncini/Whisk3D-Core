#ifndef W3D_MESH_BIN_H
#define W3D_MESH_BIN_H

// ============================================================================
//  .w3db - LA MALLA BINARIA, LISTA PARA MEMORIA (version 1)
//
//  El .w3dm (W3dMesh.h) es texto: legible, diffeable, editable a mano. Pero
//  abrirlo cuesta: parsear floats, rehacer el merge de render-verts con un
//  std::map, triangular, reordenar por Forsyth, armar las aristas con un
//  std::set, pasar las capas al render. Todo eso se hacia CADA VEZ que se abria
//  el proyecto (y en el juego compilado, en cada carga de escena).
//
//  El .w3db guarda la malla YA HECHA: los arrays exactamente como quedan en
//  memoria despues de una carga clasica. Abrir = leer la entrada del zip +
//  memcpy a los arrays. CERO merge, triangulacion, Forsyth, ComputeBorders,
//  ApplyLayersToRender o std::map al cargar. Todo eso se hace AL GUARDAR.
//
//  La spec completa (campo por campo) esta en Whisk3D/formato/w3db.md, y cada
//  fourcc tiene su fila "w3db:" en formato/bloques.tsv: un bloque nuevo va a los
//  dos lugares (el comando de harness w3dbregistro lo exige).
//
//  FORMATO (todo little-endian EXPLICITO, sin padding basura: los mismos bytes
//  en cada guardado):
//
//      cabecera (20 B): "W3DB" | u32 version(=1) | u32 flags(0) | u32 nBlocks | u32 reservado(0)
//      bloque:          fourcc | u32 dominio | u32 cantidad | u32 bytes | payload (relleno a 4 con ceros)
//
//  - dominio: el vocabulario del .w3dm (punto, esquina, cara, arista, global) mas
//    los dos que solo tiene el binario: render (un elemento por render-vert) y
//    triangulo. Los bits 8..15 del campo son BANDERAS del bloque (RIDX: indices
//    de 32 bits).
//  - REQ (va primero): los fourcc que un lector DEBE entender para no romper la
//    malla (mismo contrato que "requiere" del texto: si nombra uno desconocido, la
//    malla abre pero no se guarda encima).
//  - Un bloque que el lector no conoce y no esta en REQ se PRESERVA tal cual y se
//    reescribe al guardar (W3dForeign::blocksBin), con la regla del SELLO: global
//    siempre; indexado solo si la cantidad de su dominio no cambio.
//  - Los strings van como u32 largo + bytes (sin terminador, sin relleno).
//  - INFO crece SOLO al final: un lector viejo lee los campos que conoce e ignora
//    el resto. Un bloque NUNCA se ensancha: si hace falta otra cosa, nace otro.
//
//  BLOQUES DE RENDER (los necesita el juego; son los arrays en memoria):
//      INFO  nombre, suave, primitiva, conteos, AABB, centro, radio, activos, sello
//      RPOS  f32 x3 por render-vert        RNRM  i8 x3        RUV   f32 x2      RCOL u8 x4
//      RIDX  el index buffer YA agrupado por mesh part y optimizado (Forsyth):
//            u16 si hay <= 65535 render-verts, si no u32 (bandera en el dominio)
//      PART  mesh parts: nombre, material (por NOMBRE), inicio, cantidad
//      RCTL  i32 por render-vert: vertCtrlPoint (= posRep): la IDENTIDAD de los puntos
//      RVGR  vertex groups (nombre + pares render-vert/peso)   RSHP shape keys
//      RUVG  UV groups                                        RUVR uv de reposo del 2D
//  BLOQUES DE EDICION (solo el editor; el juego los saltea sin leerlos):
//      FACE  poligonos nativos (lados, mesh part, shading, render-verts)
//      LOOS  aristas y vertices sueltos     CNRM normal por esquina
//      UVLY  capas UV por esquina           COLY capas de color por esquina
//      SHRP / SEAM  claves de posicion de 24 bytes, como en memoria
//      EDGE  aristas unicas (pares de representantes): sin el std::set de ComputeBorders
//      AJEN  bloques de texto de un .w3dm mas nuevo + su "requiere" (preservacion)
//
//  CONTRATOS que el binario NO puede romper (dependen de ellos las vertex anims,
//  RVMAP, el PVS/VisSet y los UV groups): el ORDEN de los render-verts se guarda
//  tal cual esta en memoria, y el index buffer es el mismo que armaria
//  RegroupMeshParts al cargar el .w3dm (W3dMeshCanonicalIndices, editor).
//  La identidad de los puntos, que capas salen, las marcas validas y como viajan
//  los pesos salen de las MISMAS piezas que usa el escritor de texto (ver el
//  final de W3dMesh.h): la misma malla guardada en .w3dm o en .w3db vuelve igual.
//
//  C++03 puro. Entra en la build del JUEGO COMPILADO (Linux/Android/web/Symbian):
//  no puede llamar a nada de main/ (el editor). El index buffer canonico, que
//  necesita la triangulacion y el Forsyth del editor, lo arma el que guarda.
// ============================================================================

#include <string>
#include <vector>
#include <cstddef>
#include "crossplatform.h"   // MeshIndex (16 bits en el N95, 32 en el resto)

class Mesh;
class MeshFace;
struct W3dMeshInfo;
struct W3dForeign;

// dominios de un bloque (los cinco del .w3dm + los dos del binario)
enum {
    W3DB_POINT = 0, W3DB_CORNER = 1, W3DB_FACE = 2, W3DB_EDGE = 3, W3DB_GLOBAL = 4,
    W3DB_RENDER = 5, W3DB_TRIANGLE = 6
};
enum { W3DB_VERSION = 1 };
// banderas del campo dominio (bits 8..15)
enum { W3DB_IDX32 = 0x100 };   // RIDX: indices de 32 bits (si no, 16)

// true si los bytes arrancan con la firma "W3DB" (asi se distingue del .w3dm de texto)
bool W3dMeshBinIs(const unsigned char* data, size_t n);

// true si el fourcc es un bloque que ESTA version entiende (lo usa el freno REQ)
bool W3dMeshBinBlockKnown(const std::string& fourcc);

// TODOS los fourcc que esta version entiende, en el orden en que se escriben
// (los de 3 letras con su espacio: "REQ ", "RUV "). 'requeridos' (opcional) recibe
// los que el escritor pone en REQ. Sirve para cruzar el codigo con el registro de
// nombres (formato/bloques.tsv, filas "w3db:") y con la documentacion (formato/w3db.md).
void W3dMeshBinBlocksKnown(std::vector<std::string>& known,
                                 std::vector<std::string>* required = 0);

// ---------------------------------------------------------------------------
//  ESCRITURA
// ---------------------------------------------------------------------------
// el index buffer CANONICO (lo que RegroupMeshParts armaria al abrir la malla):
// triangulos por mesh part + Forsyth. Lo arma el EDITOR (W3dMeshCanonicalIndices,
// libs/Whisk3DCore/edit/MeshEdit.cpp) porque la triangulacion y el Forsyth viven ahi.
struct W3dMeshBinIndices {
    std::vector<MeshIndex> idx;
    std::vector<int> start, count;   // por mesh part (startDrawn / indicesDrawnCount)
};

// escribe la malla en 'out' (se limpia primero). canon = el index buffer canonico
// (NULL = el faces[] que la malla tiene en memoria, tal cual). La malla tiene que
// venir EN REPOSO (vertex anims, flipbook UV: lo hace el que guarda).
// DETERMINISTA: la misma malla da SIEMPRE los mismos bytes.
// 'avisos' recibe un renglon por cada cosa que no sale entera (los mismos del .w3dm).
// false (y 'out' vacio) = malla NULL, bajo el freno "requiere" (W3dMeshMissingBlock),
// o una malla que el index buffer de esta plataforma no alcanza (W3dMeshBinIndicesReach).
bool W3dMeshBinWrite(const Mesh* m, const W3dMeshBinIndices* canon, std::string& out,
                         std::vector<std::string>* notices = 0);

// true si el index buffer de ESTA plataforma (MeshIndex) direcciona nRV render-verts. Con
// indices de 16 bits (el N95) una malla de mas de 65535 NO tiene index buffer correcto en
// memoria (ni canonico): se truncan modulo 65536 y siguen "en rango". El .w3db no se puede
// escribir (W3dMeshBinWrite se niega); el texto si, porque guarda las caras con int.
bool W3dMeshBinIndicesReach(int nRV);

// ---------------------------------------------------------------------------
//  LECTURA
// ---------------------------------------------------------------------------
struct W3dMeshBinOptions {
    bool edit;         // leer tambien los bloques de EDICION (el editor: siempre)
    bool noEditable;   // la malla es escenario cerrado (el "noEditable" del proyecto):
                       // sin posRep ni aristas, AABB/centro de ComputeAABBOnly
    W3dMeshBinOptions() : edit(true), noEditable(false) {}
};

// llena 'm' desde un .w3db: arrays de render, index buffer, mesh parts (el MATERIAL
// lo resuelve el que llama con info->materials), identidad de puntos, grupos,
// formas, AABB/centro/radio y (con op.edit) caras, capas, marcas y aristas. Deja
// la malla EXACTAMENTE como la dejaba la carga clasica de su .w3dm, sin derivar nada.
// false = no es un .w3db, es de una version mas nueva o esta corrupto; en los tres
// casos la malla NO SE TOCA y 'info' dice por que.
bool W3dMeshBinRead(const unsigned char* data, size_t n, Mesh* m, W3dMeshInfo* info,
                     const W3dMeshBinOptions& op);

// MATERIALIZA los bloques de EDICION de una malla que se cargo sin ellos (el juego
// compilado, o cualquier malla que usa un recurso compartido, objects/MeshResource.h):
// relee su entrada (Mesh::pendingEdit; la del RECURSO si la malla tiene uno) y llena
// faces3d, capas, marcas, aristas y posRep. Se llama sola desde BuildPolyMesh
// (un modificador geometrico, el Boolean que usa esta malla de target). Idempotente;
// false si no habia nada pendiente o la entrada ya no calza con la malla.
bool W3dMeshBinMaterializeEdit(Mesh* m);
// lo mismo, desde los bytes de la entrada que el que llama YA tiene en memoria (la carga:
// una malla cuyo stack va a pedir sus poligonos apenas se resuelva, sin releer la entrada).
bool W3dMeshBinMaterializeEdit(Mesh* m, const unsigned char* data, size_t n);

// ---------------------------------------------------------------------------
//  LA MALLA DE RENDER, SIN Mesh
//  Para el recurso compartido (una malla en memoria para N objetos): los mismos
//  bloques de render en una estructura propia, independiente del objeto. Los
//  punteros de render de cada Mesh usuario pueden apuntar ADENTRO de estos vectores.
// ---------------------------------------------------------------------------
struct W3dMeshBinGroup {   // vertex group / UV group: indices de render-vert + pesos
    std::string name;
    std::vector<int>   idx;
    std::vector<float> weights;
};
struct W3dMeshBinShape {   // shape key: indices de render-vert + 3 floats por indice
    std::string name;
    std::vector<int>   idx;
    std::vector<float> d;
};
struct W3dMeshBinPart {   // mesh part: rango del index buffer + material por nombre
    std::string name, material;
    int start, count;
    W3dMeshBinPart() : start(0), count(0) {}
};
struct W3dMeshRender {
    std::string name;
    bool  smooth;
    int   primType;                    // -1 = no es una primitiva regenerable
    float primSize, primSize2, primHeight;
    int   primVerts, primVerts2;
    int   nRV;                         // render-verts
    std::vector<float>         pos;    // 3 por render-vert
    std::vector<signed char>   nrm;    // 3 por render-vert
    std::vector<float>         uv;     // 2 por render-vert
    std::vector<unsigned char> col;    // 4 por render-vert
    std::vector<MeshIndex>     idx;    // index buffer (agrupado por parte, optimizado)
    std::vector<W3dMeshBinPart> parts;
    std::vector<int>           ctrl;   // render-vert -> representante de su punto
    std::vector<W3dMeshBinGroup> groups, uvGroups;
    std::vector<W3dMeshBinShape> shapes;
    std::vector<float>         uvRest; // 2 por render-vert (vacio = sin reposo 2D)
    std::vector< std::vector<float> > uvExtra; // RUVX: capas UV extra por render-vert (una vacia = ausente/la activa)
    int   activeGroup, uvActiveGroup;
    float aabbMin[3], aabbMax[3], center[3], radius;
    bool  aabbOk;
    int   groupedVerts;
    bool  statsNoEditable;             // AABB/centro calculados como escenario cerrado
    W3dMeshRender();
    long Bytes() const;                // memoria de los arrays (para el almacen de recursos)
};
// lee SOLO los bloques de render (los de edicion se saltean sin leerlos). 'sello' (opcional)
// recibe la PRESERVACION del archivo: el sello de cantidades por dominio, los bloques que esta
// version no entiende y el REQ (lo que Mesh::w3dmForeign guarda de un .w3db).
bool W3dMeshBinReadRender(const unsigned char* data, size_t n, W3dMeshRender& out, W3dMeshInfo* info,
                           W3dForeign* stamp = 0);
// lee SOLO los EXTRAS de la malla de render (identidad de los puntos, vertex/UV groups, shape
// keys, reposo 2D y los activos), sin copiar los arrays grandes: el recurso compartido los suelta
// cuando sus usuarios ya los tienen y los vuelve a pedir aca si hace falta (MeshResource.cpp).
bool W3dMeshBinReadExtras(const unsigned char* data, size_t n, W3dMeshRender& out);
// lee SOLO la TOPOLOGIA de los bloques de edicion: las aristas (EDGE, o por representante si no
// vienen) y, si 'caras' no es NULL, las caras (FACE). Para MIRAR la malla (wireframe, snap,
// seleccion por caja) sin materializar su edicion entera. false = no es un .w3db legible.
bool W3dMeshBinReadTopology(const unsigned char* data, size_t n, std::vector<int>& edges,
                              std::vector<MeshFace>* faces);

// ---------------------------------------------------------------------------
//  RETOQUES SIN RELEER LA MALLA (la malla como recurso, objects/MeshResource.h): un recurso
//  que no cambio se guarda copiando sus bytes; si solo cambio su NOMBRE o el nombre de los
//  materiales de sus partes, se reescribe ESE bloque y el resto viaja tal cual.
//  Deterministas (mismos bytes de entrada + mismo cambio = mismos bytes de salida).
// ---------------------------------------------------------------------------
// el mismo .w3db con otro nombre en INFO. false = no es un .w3db legible.
bool W3dMeshBinRename(const std::string& in, const std::string& name, std::string& out);
// los nombres de material de las partes (PART), en orden. false = no es un .w3db legible.
bool W3dMeshBinMaterials(const std::string& in, std::vector<std::string>& mats);
// el mismo .w3db con estos nombres de material en PART (uno por parte, en orden).
// false = no es un .w3db legible o la cantidad no coincide con la de partes.
bool W3dMeshBinChangeMaterials(const std::string& in, const std::vector<std::string>& mats, std::string& out);

// DIAGNOSTICO: una linea por bloque ("INFO global 1 184 B"). false = no es un .w3db valido.
bool W3dMeshBinList(const unsigned char* data, size_t n, std::vector<std::string>& lines);

#endif
