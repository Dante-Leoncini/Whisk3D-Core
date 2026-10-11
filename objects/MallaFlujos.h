#ifndef W3D_MALLA_FLUJOS_H
#define W3D_MALLA_FLUJOS_H
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

struct MallaRecurso;

// las primitivas del paso "Dibujar elementos" (el orden es el del desplegable y el del .w3d)
enum W3dPrimRutina {
    RPrimPuntos = 0, RPrimLineas, RPrimTiraLineas, RPrimLazoLineas, RPrimTriangulos, RPrimTiraTriangulos,
    RPrimAbanico, RPrimQuads, RPrimTiraQuads, RPrimPoligono, RPrimN
};
bool W3dPrimSimulada(int prim);   // quads / tira de quads / poligono: GL ES 1.1 no los tiene

// lo que hay que mandar a dibujar: del IBO (ibo != 0: offset 'primero') o de RAM ('idx')
struct W3dFlujo {
    const MeshIndex* idx;
    int cantidad;
    unsigned int ibo;
    int primero;
    int primGL;              // w3dEngine::Primitiva con la que se dibuja (las simuladas: triangulos)
};

// el flujo de la parte 'parte' (-1 = todas) para la primitiva 'prim'. false = la malla no tiene geometria
bool W3dMallaFlujo(MallaRecurso* r, int parte, int prim, W3dFlujo& f);
// cuantos indices manda esa primitiva para esa parte (el panel: "lista 96 / tira 40")
int  W3dMallaFlujoIndices(MallaRecurso* r, int parte, int prim);
// la caja de la parte (-1 = toda la malla), en el espacio de la malla
bool W3dMallaParteCaja(MallaRecurso* r, int parte, float mn[3], float mx[3]);
// el indice de vertice mas alto que usa la parte (-1 = todas): para validar los punteros
int  W3dMallaParteMaxIndice(MallaRecurso* r, int parte);
// (MallaRecurso) suelta todo (IBOs incluidos)
void W3dMallaFlujosBorrar(MallaRecurso* r);
// ---- la malla SOLO EN GPU (MallaRecurso.h): sin r->faces / r->vertex, los triangulos salen del IBO de la malla y
//      la caja y el indice maximo del cache; un flujo derivado (tira, aristas...) solo si ya estaba armado ----
// el flujo de esa parte y primitiva ya se puede mandar sin la RAM de la malla (los triangulos siempre)
bool W3dMallaFlujoListo(MallaRecurso* r, int parte, int prim);
// deja en el cache la caja y el indice maximo de CADA parte y de toda la malla (antes de soltar la RAM)
void W3dMallaFlujosPrecalcular(MallaRecurso* r);
// los arrays cambiaron de lugar con el MISMO contenido (se soltaron o se releyeron de los bytes): el cache sigue
void W3dMallaFlujosReapuntar(MallaRecurso* r);
// el contexto GL se perdio: los IBO de los flujos se olvidan (sin glDelete) y se vuelven a subir de su copia al usarlos
void W3dMallaFlujosGLPerdido(MallaRecurso* r);

// las tiras de una lista de triangulos, cosidas en un solo flujo (lo usa tambien el harness)
void W3dTirasDeTriangulos(const MeshIndex* tri, int nIndices, std::vector<MeshIndex>& out);

#endif
