#ifndef W3D_SCATTER_MOD_H
#define W3D_SCATTER_MOD_H
// ============================================================================
//  EL MODIFICADOR SCATTER (ModifierType::Scatter, sus campos en edit/Modifier.h)
//
//  Reparte COPIAS de otros objetos sobre la malla: en sus vertices, en sus aristas, en el centro de sus caras o en
//  cualquier punto de sus caras (al azar, parejo por area: una cara del doble de grande recibe el doble). Es el
//  sistema de particulas "Render As: Collection" de Blender, sin la fisica: las piedras de un suelo, el pasto, los
//  tornillos de un panel.
//
//  LAS FUENTES ("pool"): el objeto (con sus hijos), los objetos de una coleccion, los de otra escena del proyecto, o
//  los de una escena de un .w3d EXTERNO (una libreria vinculada, io/Libraries.h: el .w3d con todas las piedras). Con
//  mas de una fuente cada copia elige UNA al azar. Las colecciones de adentro se abren: sus objetos son fuentes.
//  Cada fuente va con su giro y su escala, pero NO con su lugar: la copia nace en el punto de la malla (por eso la
//  coleccion de piedras puede estar escondida en un rincon, o apagada).
//
//  LA SEMILLA MANDA: las copias salen de un generador propio (xorshift), no de rand(): la misma semilla da las mismas
//  copias en el editor, en el juego compilado y en cualquier maquina. Los lugares salen de un flujo de numeros y lo de
//  cada copia (cual fuente, escala, giro, inclinacion) de otro, siempre 5 numeros por copia: subir el azar de la escala
//  no mueve las piedras de lugar.
//
//  LOS DOS MODOS (Modifier::scatMode):
//    0 UNIDA       la geometria de las copias se SUMA a la malla generada (con sus materiales: PolyMesh::extraMats).
//                  Un dibujo por material, cada copia gasta su memoria. Sombras, lightmaps, trazador y Apply (UNA
//                  malla con quads) como cualquier malla generada.
//    1 INSTANCIAS  cada copia dibuja la malla de su fuente con otra matriz: no gasta memoria por copia, si una
//                  llamada GL. Las sombras en tiempo real las ven (W3dScatterPieces). Apply (en el editor) = un objeto
//                  por copia, que comparten la malla de su fuente.
//
//  Compartido editor/juego (C++03). El panel y el Apply en objetos viven en el editor (main/).
// ============================================================================
#include <vector>
#include <string>
#include "math/Matrix4.h"

class Mesh;
class Modifier;
class Object;
class Material;
struct PolyMesh;

// las FUENTES del modificador, resueltas ahora (el objeto, los de la coleccion, los de la escena, los de la libreria
// cargada). Nunca el emisor ni algo que lo contenga
void W3dScatterPool(Modifier* md, Mesh* emitter, std::vector<Object*>& out);

// corre el modificador sobre W (el stack, edit/MeshEdit.cpp): calcula las copias (Modifier::scatRt) y, en modo unida,
// suma su geometria a W (y sus materiales a W.extraMats). Con el emisor apagado (scatShowEmitter) saca sus caras
void W3dScatterRun(Modifier* md, Mesh* emitter, PolyMesh& W, bool render);

// EL DIBUJO (Mesh::RenderObject, con la ModelView del emisor): las copias del modo instancias (draw = false: no las
// dibuja, solo contesta). Devuelve true si el emisor NO se dibuja (lo apago su Scatter)
bool W3dScatterRender(Mesh* m, bool draw = true);
// la malla tiene un Scatter en modo instancias que dibuja algo
bool W3dScatterHasInstances(const Mesh* m);

// LAS COPIAS COMO (malla, matriz de MUNDO): las sombras en tiempo real y el trazador de rayos (modo instancias)
struct W3dScatterPiece { Mesh* m; Matrix4 W; };
void W3dScatterPieces(Mesh* emitter, const Matrix4& emitterWorld, std::vector<W3dScatterPiece>& out);

// suelta las escenas de librerias que cargo el Scatter (al cerrar el proyecto: ANTES de cerrar sus librerias)
void W3dScatterForgetAll();

// la firma de las fuentes (sus mallas y sus geoVersion): si cambio, hay que regenerar (editar una piedra)
unsigned W3dScatterSignature(Modifier* md, Mesh* emitter);
// algun Scatter de la malla quedo viejo (la firma de sus fuentes cambio)
bool W3dScatterStale(Mesh* m);

// la matriz con la que se dibuja la fuente 'src' en una copia: la de la copia por el giro y la escala de la fuente
// (sin su lugar)
void W3dScatterSourceMatrix(const float* copy16, Object* src, Matrix4& out);

#endif
