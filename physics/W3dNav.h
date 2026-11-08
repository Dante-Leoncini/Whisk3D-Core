#ifndef W3DNAV_H
#define W3DNAV_H

// ============================================================================
//  W3dNav — LA NAVEGACION de los personajes: por donde se puede caminar y el
//  camino de un punto a otro.
//
//  QUE ES: una REJILLA sobre el piso (el plano X Y, con la altura en Z: las
//  unidades y el "arriba" los pone quien la arma; Half-Life 2 la usa en pulgadas
//  con Z arriba). Cada celda tiene hasta 'capas' pisos (el piso y, arriba, el
//  descanso de una escalera o el piso de un balcon): la altura donde se para una
//  persona. La arma un CONVERSOR afuera del juego (ver nav_hl2.py del demo de
//  Half-Life 2: la cara de arriba de los brushes solidos, con la caja de una
//  persona libre encima) y el juego la carga de un archivo:
//
//      'NAV1'  f32 x0 y0 celda paso   i32 nx ny capas   i16 z[ny][nx][capas]
//      (x0, y0: la esquina de la rejilla; z = -32768: sin piso)
//
//  Dos celdas vecinas (las 8 de alrededor; en diagonal solo si las dos de los
//  costados tambien se pisan: no se cortan esquinas) estan UNIDAS si sus pisos
//  difieren en menos de 'paso' (el escalon que se sube).
//
//  QUE HACE:
//    * W3dNavCamino: el camino de 'desde' a 'hasta': A* sobre las celdas (costo
//      la distancia, la heuristica octil) y despues TENSADO (string pulling):
//      desde cada punto del camino, el mas lejano que se ve en linea recta
//      caminable (W3dNavDirecto). Quedan pocas esquinas, en los quiebres de
//      verdad, cada una con la altura del piso.
//    * los BLOQUEOS dinamicos: una caja (W3dNavBloqueoCaja) marca sus celdas con
//      un numero; W3dNavBloquear lo prende y lo apaga (una puerta cerrada, una
//      reja). Las celdas de un bloqueo prendido no se pisan.
//    * consultas: el piso bajo un punto (W3dNavSuelo, para apoyar los pies), la
//      celda mas cerca que se pisa (W3dNavMasCerca) y la linea recta caminable.
//
//  DISENO: C++03, sin motor; lo que usa el A* se reserva al armar (un float, un
//  int y un sello por celda y capa) y no se pide memoria por consulta. Un camino
//  revisa a lo sumo 'maxNodos' celdas (por defecto 40000): si no llega, devuelve
//  el camino hasta la celda revisada mas cerca del destino.
//
//  COSTO: la rejilla de Half-Life 2 (206 x 193 celdas de 16 pulgadas, 2 capas):
//  155 KB de datos, ~800 KB del A*; un camino a traves de la estacion ~1 ms en un
//  PC (se pide pocas veces: al empezar a caminar o cuando algo se abre).
// ============================================================================

#include <stdint.h>

struct W3dNav {
    float x0, y0, celda, paso;
    float radio;           // W3dNavDirecto: el ancho a cada lado de la linea que tambien se pisa (0: solo la linea)
    int nx, ny, capas;
    int16_t* z;            // ny * nx * capas
    uint8_t* bloqueo;      // el numero de bloqueo de cada celda y capa (0: ninguno)
    bool prendido[256];    // los bloqueos prendidos
    int nBloqueos;
    int maxNodos;
    // el A*
    float* g;
    int* padre;
    uint32_t* sello;
    uint32_t selloActual;
    int* monton;           // (el monticulo de los abiertos)
    float* montonF;
    int capMonton;
    W3dNav();
};

enum { W3DNAV_NADA = -32768 };

// carga la rejilla de los datos del archivo (los copia); false si no es un NAV1
bool W3dNavArmar(W3dNav& n, const void* datos, uint32_t largo);
void W3dNavSoltar(W3dNav& n);
// el piso bajo p (la capa mas cerca de la altura de p, hasta 'paso' por arriba y 4 pasos por abajo); false si no hay
bool W3dNavSuelo(const W3dNav& n, const float* p, float* z);
// la celda que se pisa mas cerca de p (hasta 'radio' unidades): su centro con la altura de su piso
bool W3dNavMasCerca(const W3dNav& n, const float* p, float radio, float* out);
// se puede caminar derecho de a a b (las celdas de la linea se pisan, sin escalones de mas)
bool W3dNavDirecto(const W3dNav& n, const float* a, const float* b);
// el camino: hasta 'max' puntos (x y z) en 'out', el ultimo es 'hasta' (o lo mas cerca que se llega; si no entran
// todas las esquinas, la ultima que entra y *llega = false); 0 si 'desde'
// no esta sobre la rejilla. *llega = si llego de verdad
int W3dNavCamino(W3dNav& n, const float* desde, const float* hasta, float* out, int max, bool* llega);
// un bloqueo nuevo con las celdas de la caja (la altura de sus pisos dentro de [mn z, mx z]); devuelve su numero
// (1..255; 0 si no hay mas). Empieza apagado
int W3dNavBloqueoCaja(W3dNav& n, const float* mn, const float* mx);
void W3dNavBloquear(W3dNav& n, int bloqueo, bool si);

#endif // W3DNAV_H
