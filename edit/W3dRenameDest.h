#ifndef W3D_RENAME_DEST_H
#define W3D_RENAME_DEST_H
// (vino de main/undo/Undo.h: el DESTINO de un rename es un dato de la ESCENA -una malla, un hueso, una capa- que
//  usan las operaciones de malla del Core (MeshEdit) y el deshacer del editor. Las funciones W3dDest* y
//  W3dDestResolve las define el ANFITRION: ver base/W3dHost.h)
#include <string>
class Mesh;
// ============================================================================
//  DESTINO de un rename (W3dRenameDest) - IDENTIDAD ESTABLE, no un puntero crudo.
//
//  NINGUN destino se guarda como puntero "y listo": TODOS se revalidan al aplicar.
//  Hay dos formas de morir, y las dos terminaban escribiendo en memoria liberada:
//
//  (1) el nombre vive DENTRO de un contenedor POR VALOR:
//      Armature::bones (vector<W3dBone>), Armature2D::huesos (vector<W3dBone2D>),
//      Mesh::materialsGroup (mesh parts), W3dScriptEntry::refs (vector<pair>).
//      Cualquier alta/baja posterior REALOCA el vector y el puntero queda colgando.
//      -> se guardan como DUENO + INDICE (Bone3D/Bone2D/MeshPart/RefLua) y se
//         resuelven al aplicar, chequeando que el dueno siga en la escena y que el
//         indice siga existiendo.
//
//  (2) el dueno del heap SE BORRA Y SE RECREA. La premisa vieja ("VertexGroup* /
//      UVGroup* / UVMap* / ColorLayer* son duenos del heap que no se mueven") era
//      FALSA: Mesh::FreeLayers los DESTRUYE y MeshGeoUndo::ApplyTo los rehace con
//      new (punteros distintos), asi que CUALQUIER operacion de geometria apilada
//      encima de un rename de grupo/capa dejaba el destino colgado; lo mismo el
//      boton "-" de las tarjetas, o borrar un material / un clip / un armature 2D /
//      un objeto. -> el caso Directo se valida POR IDENTIDAD contra los nombres
//         vivos (W3dPointerNameLive, ObjectMode.cpp).
//
//  Sintomas reales de no hacerlo: crash (heap-use-after-free / std::length_error) o
//  -peor- corrupcion SILENCIOSA: el undo escribia el nombre viejo ENCIMA de otra
//  cosa (un UV group ajeno), rompiendo el binding por nombre sin ningun aviso.
//  Si el destino ya no existe, el paso es un NO-OP seguro (nunca escribe "en
//  cualquier lado").
// ============================================================================
class Armature;    // huesos 3D (por valor en Armature::bones)
struct Armature2D; // huesos 2D (por valor en Armature2D::huesos)
class Object;      // refs de los scripts lua (por valor en scriptData->scripts[].refs)

struct W3dRenameDest {
    enum Type { Direct = 0, Bone3D, Bone2D, MeshPart, RefLua, LayerMesh, ClipArm, Clip2D, Global };
    // LayerMesh: CUAL de las listas con nombre de la malla (todas son vector<T*>, ver Mesh.h)
    enum Layer  { VGroup = 0, UVGroup, UVMap, ColorLayer, Arm2D, VertAnim };
    // Global: cual de las listas con nombre del PROYECTO
    // MeshG / MeshFolderG: el NOMBRE / la CARPETA de una malla del registro (MeshResource.h);
    // su 'i' es el SERIAL del recurso, no una posicion (el registro se purga y se corre)
    enum List { MaterialG = 0, SceneAnimG, MeshG, MeshFolderG };
    int   type;
    void* owner;      // Armature* (Bone3D, ClipArm) / Mesh* (Bone2D, MeshPart, LayerMesh, Clip2D) / Object* (RefLua)
    void* owner2;     // Bone2D / Clip2D: el Armature2D* CONCRETO (la lista de la malla puede cambiar)
    int   which;      // LayerMesh: Capa / Global: Lista
    int   i, j;       // indice del hueso / de la parte / del elemento de la lista / (script, ref)
    std::string* ptr; // solo Directo
    W3dRenameDest() : type(Direct), owner(0), owner2(0), which(0), i(-1), j(-1), ptr(0) {}
};
// Directo: SOLO para nombres que son MIEMBRO de un Object (Object::name, y las puntas de
// vinculo que enumera Object::RefObjectName: targetName / RailName / la fuente de cada
// constraint). Esos no se recrean: o el objeto esta en la escena
// o no esta, y eso se chequea (W3dPointerNameLive). NO usarlo para elementos de una lista:
// esos se borran y se re-crean EN LA MISMA DIRECCION (el allocator recicla el bloque), asi que
// validar "el puntero sigue siendo un nombre vivo" NO alcanza -> van por (dueno + indice).
// Pasarle CUALQUIER otro puntero da un destino que W3dPointerNameLive NO reconoce -> el
// resolver devuelve NULL SIEMPRE y el paso de undo queda MUERTO (no restaura nada, sin aviso).
// Justo eso pasaba con los UV groups del rename de hueso 2D. Por eso ya NO existe ninguna API
// de captura que tome std::string* pelado: todas piden un W3dRenameDest y el compilador rompe
// si alguien intenta capturar un elemento de lista por puntero. Aca se loguea si igual llega
// un puntero que no es un nombre de Object.
W3dRenameDest W3dDestName(std::string* p);
W3dRenameDest W3dDestBone3D(Armature* a, int idx);
W3dRenameDest W3dDestBone2D(Mesh* m, Armature2D* arm, int idx);
W3dRenameDest W3dDestMeshPart(Mesh* m, int idx);
W3dRenameDest W3dDestRefLua(Object* o, int script, int ref);
// listas con nombre que viven en un vector<T*>: el elemento SE LIBERA (boton "-", MeshGeoUndo,
// borrar un clip/material) y el proximo new puede caer en la misma direccion -> (dueno, indice)
W3dRenameDest W3dDestLayerMesh(Mesh* m, int layer, int idx);       // capa = W3dRenameDest::Capa
W3dRenameDest W3dDestClipArm(Armature* a, int idx);                // clip 3D del esqueleto
W3dRenameDest W3dDestClip2D(Mesh* m, Armature2D* arm, int idx);    // clip del armature 2D
W3dRenameDest W3dDestGlobal(int list, int idx);                    // lista = W3dRenameDest::Lista
std::string*  W3dDestResolve(const W3dRenameDest& d);              // NULL = ya no existe

#endif // W3D_RENAME_DEST_H
