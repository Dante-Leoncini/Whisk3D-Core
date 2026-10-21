#ifndef W3DCOLMALLA_H
#define W3DCOLMALLA_H

// ============================================================================
//  W3dColMalla — COLISION CONTRA TRIANGULOS de una malla (piso y paredes de un
//  escenario), para personajes de juego. Complementa a W3dRigido (cajas).
//
//  QUE RESUELVE: caminar sobre un piso irregular (rampas, escalones) y que las
//  paredes frenen al personaje, con la geometria de colision del propio mapa
//  (ej: la colision de un escenario exportada como dos mallas: piso y pared). El script sigue
//  moviendo al personaje a mano (root motion, velocidad...) y despues le pide
//  al motor: "a que altura esta el piso aca" y "empujame fuera de las paredes".
//
//  DISENO (las reglas de la casa, como W3dFisica / W3dRigido):
//    * C++03 puro, sin allocations por consulta: corre en el N95.
//    * la malla se lee UNA vez (la primera consulta) y se guarda en MUNDO con
//      una grilla XZ de celdas (indices de triangulos por celda). Una consulta
//      mira 1..4 celdas, no los miles de triangulos del mapa.
//    * ESTATICA: si la malla se mueve despues de la primera consulta, la
//      colision no se entera (se asume escenario fijo). colOlvidar(malla) la
//      vuelve a leer en la proxima consulta.
//    * la malla de colision puede estar OCULTA (visible false): se lee igual.
//    * costo cero si nadie la usa.
//
//  BINDS LUA (W3dColMallaRegistrarBinds, los registra RegistrarAPI del Core):
//    colSuelo(malla, x, y, z [, subida=0.5]) -> y | nil
//        altura del triangulo MAS ALTO bajo (x, z) que no quede mas de 'subida'
//        por encima de y (lo que se puede pisar subiendo un escalon). nil = no
//        hay piso (borde del mapa, agujero).
//    colPared(malla, x, y, z, radio [, alto=1.7]) -> x, z
//        empuja en HORIZONTAL fuera de los triangulos que tocan al cuerpo: un
//        cilindro de 'radio' desde y+radio (rodillas: los escalones no frenan)
//        hasta y+alto. Devuelve la posicion corregida (sin cambios si no toca).
//    colRayo(malla, ox, oy, oz, dx, dy, dz, max) -> dist, nx, ny, nz | nil
//        primer triangulo que corta el rayo (ej: que la camara no atraviese
//        una pared: se la acerca al personaje hasta 'dist').
//    colOlvidar(malla)   descarta lo leido (la proxima consulta relee la malla)
//
//  W3dRigido usa la misma grilla para los cuerpos de tipo MALLA (una malla estatica
//  contra la que chocan las cajas): W3dColCaja + W3dColTriangulos.
// ============================================================================

#include "math/Vector3.h"

class Object;

// altura del piso bajo (x, z) (ver colSuelo). false = no hay piso.
bool  W3dColSuelo(Object* malla, float x, float y, float z, float subida, float* sueloY);
// empuja (x, z) fuera de las paredes (ver colPared). true = hubo contacto.
bool  W3dColPared(Object* malla, float* x, float y, float* z, float radio, float alto);
// distancia al primer triangulo que corta el rayo (d normalizado); < 0 = nada. n = normal del triangulo.
float W3dColRayo(Object* malla, const Vector3& o, const Vector3& d, float max, Vector3* n);
// la caja (en mundo) de toda la malla. false = no es una malla con triangulos.
bool  W3dColCaja(Object* malla, Vector3* mn, Vector3* mx);
// un triangulo en mundo con su normal (la del orden de sus vertices)
struct W3dColTri { Vector3 a, b, c, n; };
// los triangulos cuya caja toca [mn, mx], a lo sumo 'max' (sin allocations: el buffer es del que llama)
int   W3dColTriangulos(Object* malla, const Vector3& mn, const Vector3& mx, W3dColTri* out, int max);
void  W3dColOlvidar(Object* malla);
// descarta TODO lo leido (al parar el juego / descargar la escena: las mallas pueden dejar de existir)
void  W3dColLimpiar(void);

void  W3dColMallaRegistrarBinds(void* L);

#endif
