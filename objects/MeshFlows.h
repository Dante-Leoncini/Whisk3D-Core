#ifndef MESH_FLOWS_H
#define MESH_FLOWS_H
// ============================================================================
//  LOS FLUJOS DE INDICES de una malla 3D: lo que hace falta para dibujar CADA
//  PARTE con CUALQUIER primitiva de OpenGL (la Rutina: "Dibujar elementos").
//
//  La malla guarda sus partes como LISTA DE TRIANGULOS (el index buffer: lo que
//  edita el editor, lo que usa el picking y la fisica). Para las otras
//  primitivas se arma, UNA vez y a pedido, un flujo propio por parte, y queda en
//  un IBO (o en RAM sin VBO):
//
//    puntos              los VERTICES unicos de la parte
//    lineas              las ARISTAS unicas de sus triangulos (el alambre)
//    tira de lineas,     los indices de la parte TAL CUAL (para mallas armadas
//    lazo de lineas,     asi: con una lista de triangulos dibujan cualquier
//    abanico             cosa, como en GL)
//    triangulos          el index buffer de la parte, tal cual
//    tira de triangulos  las tiras de la parte COSIDAS con triangulos degenerados
//                        en una sola llamada (greedy por adyacencia, respetando
//                        el sentido de las caras): 2 triangulos = 4 indices
//    quads, tira de      NO existen en GL ES 1.1: los indices de la parte leidos
//    quads, poligono     asi y convertidos a triangulos (SIMULADO: mas lento)
//
//  Mismo contrato que el resto del Core: sin chequeos en el dibujo (los hace el
//  editor al validar la rutina).
// ============================================================================
#include "crossplatform.h"   // MeshIndex
#include <vector>

struct MeshResource;

// las primitivas del paso "Dibujar elementos" (el orden es el del desplegable y el del .w3d)
enum W3dPrimRoutine {
    RPrimPoints = 0, RPrimLines, RPrimStripLines, RPrimLoopLines, RPrimTriangles, RPrimStripTriangles,
    RPrimFan, RPrimQuads, RPrimStripQuads, RPrimPolygon, RPrimN
};
bool W3dPrimSimulated(int prim);   // quads / tira de quads / poligono: GL ES 1.1 no los tiene

// lo que hay que mandar a dibujar: del IBO (ibo != 0: offset 'primero') o de RAM ('idx')
struct W3dFlow {
    const MeshIndex* idx;
    int count;
    unsigned int ibo;
    int first;
    int primGL;              // w3dEngine::Primitiva con la que se dibuja (las simuladas: triangulos)
};

// el flujo de la parte 'parte' (-1 = todas) para la primitiva 'prim'. false = la malla no tiene geometria
bool W3dMeshFlow(MeshResource* r, int part, int prim, W3dFlow& f);
// cuantos indices manda esa primitiva para esa parte (el panel: "lista 96 / tira 40")
int  W3dMeshFlowIndices(MeshResource* r, int part, int prim);
// la caja de la parte (-1 = toda la malla), en el espacio de la malla
bool W3dMeshPartBox(MeshResource* r, int part, float mn[3], float mx[3]);
// el indice de vertice mas alto que usa la parte (-1 = todas): para validar los punteros
int  W3dMeshPartMaxIndex(MeshResource* r, int part);
// (MeshResource) suelta todo (IBOs incluidos)
void W3dMeshFlowsDelete(MeshResource* r);
// ---- la malla SOLO EN GPU (MeshResource.h): sin r->faces / r->vertex, los triangulos salen del IBO de la malla y
//      la caja y el indice maximo del cache; un flujo derivado (tira, aristas...) solo si ya estaba armado ----
// el flujo de esa parte y primitiva ya se puede mandar sin la RAM de la malla (los triangulos siempre)
bool W3dMeshFlowReady(MeshResource* r, int part, int prim);
// deja en el cache la caja y el indice maximo de CADA parte y de toda la malla (antes de soltar la RAM)
void W3dMeshFlowsPrecompute(MeshResource* r);
// los arrays cambiaron de lugar con el MISMO contenido (se soltaron o se releyeron de los bytes): el cache sigue
void W3dMeshFlowsRepoint(MeshResource* r);
// el contexto GL se perdio: los IBO de los flujos se olvidan (sin glDelete) y se vuelven a subir de su copia al usarlos
void W3dMeshFlowsGLLost(MeshResource* r);

// las tiras de una lista de triangulos, cosidas en un solo flujo (lo usa tambien el harness)
void W3dTriangleStrips(const MeshIndex* tri, int nIndices, std::vector<MeshIndex>& out);

#endif
