#ifndef W3DCABLES_H
#define W3DCABLES_H

// ============================================================================
//  W3dCables — CABLES FINOS en el mundo: los de luz que cuelgan de los postes,
//  los de las vias del tren, las sogas. Una polilinea con un GROSOR REAL (en
//  metros) y un color.
//
//  LA IDEA: un cable de 3 cm a 20 metros mide menos de un pixel. Dibujarlo como
//  malla (un tubo, una cinta) lo deja cortado en pedazos que titilan al moverse
//  (el pixel lo toca o no lo toca). Aca se dibuja como LINEA DE 1 PIXEL y lo que
//  falta de grosor se pone en la OPACIDAD: un cable que en pantalla mide medio
//  pixel tapa la mitad del pixel, asi que va con alfa 0,5. Por vertice:
//
//      grosor en pixeles  g = grosor * focal / z
//      focal = alto_pantalla / (2 * tan(fov / 2))   (en pixeles, fov vertical)
//      z     = la profundidad del vertice en la vista (el producto con el eje
//              de la camara: sin raiz cuadrada)
//
//      g >= 1   la linea va opaca
//      g <  1   alfa = g (la cobertura del pixel), con un PISO para que el cable
//               lejano no se borre de golpe. La cobertura pasa por una curva
//               (alfa = g ^ realce, una tabla de 256): la mezcla se hace con
//               los colores ya corregidos para la pantalla y una linea de alfa
//               0,3 se ve mas clara que un cable que tapa el 30 % del pixel
//               (realce 0,5: la raiz; 1 = la cobertura tal cual)
//      z > tope el cable no se dibuja; en el ultimo cuarto antes del tope el
//               alfa baja a cero (no desaparece de golpe)
//
//  Y de cerca, donde el cable mide 2 pixeles o mas, ademas de la linea va una
//  CINTA (dos triangulos por tramo) de cara a la camara con el ancho de verdad:
//  se ve solido y con su grosor. Son pocos tramos (los de cerca).
//
//  LA CUENTA EN ENTEROS: las posiciones se guardan tambien en centimetros
//  (int32) y los ejes de la camara en punto fijo (1024 = 1). Por vertice:
//  tres productos por eje (la profundidad y los dos de la pantalla para saber
//  si esta afuera), una division entera (el alfa) y comparaciones. El flotante
//  solo por cable (la caja contra la piramide) y en las cintas de cerca.
//
//      cobertura (0..255) = K / z      K = grosor_cm * focal * 256 (por cable y cuadro)
//      alfa = tabla[cobertura]
//
//  LEJOS (la caja del cable a mas de distMitad) va un vertice de cada dos: la
//  mitad de las lineas en lo que ya es una raya fina.
//
//  LO QUE SE SALTEA: el cable entero con su caja fuera de la piramide o mas
//  lejos que el tope; el tramo con sus dos vertices del mismo lado afuera
//  (los dos a la izquierda de la vista, los dos detras, los dos lejos...).
//  Ademas quien lo usa puede OCULTAR cables por cuadro (el PVS de un mapa BSP:
//  W3dCablesOcultar).
//
//  COMO SE DIBUJA: GL_LINES de 1 pixel con color por vertice (4 bytes: el alfa
//  de cada vertice), mezcla alfa, sin textura, con el test de profundidad pero
//  SIN escribirla (lo transparente no tapa lo que se dibuja despues). Un
//  glDrawElements para todas las lineas y otro para todas las cintas.
//  Hay que dibujarlo DESPUES de lo opaco.
//
//  LA COMBA (W3dCablesAgregarComba): un cable colgado entre dos puntos con un
//  largo mayor a la distancia toma la forma de una CATENARIA (la del coseno
//  hiperbolico): se busca su parametro con Newton para que el arco mida el
//  largo pedido y se muestrea parejo en la horizontal.
//
//  DISENO: C++03, sin STL; la memoria se pide al agregar (no por cuadro).
//  Hasta 65535 vertices por lote (indices de 16 bits).
// ============================================================================

#include <stdint.h>

struct W3dCable {
    int primero, cuantos;      // sus vertices en el lote
    float grosor;              // metros
    unsigned char rgba[4];
    float mn[3], mx[3];        // la caja (metros)
};

// la camara del cuadro: posicion, ejes en el mundo (mira hacia -atras), fov VERTICAL en grados, el alto y el ancho
// del rectangulo de la vista en pixeles. 'planos': la piramide como 6 planos (normal hacia adentro, d: adentro si
// n.p + d >= 0) para descartar cables enteros; NULL = solo la distancia
struct W3dCablesVista {
    float pos[3];
    float der[3], arr[3], atras[3];
    float fov;
    int anchoPx, altoPx;
    float cerca;
    const float* planos;
};

struct W3dCablesCuenta {
    int cables;     // los que pasaron la caja
    int vertices;   // los vertices calculados
    int lineas;     // los tramos dibujados como linea
    int cintas;     // los tramos con cinta (de cerca)
};

struct W3dCables {
    float* pos;              // x y z en metros
    int32_t* cm;             // x y z en centimetros
    int nVerts, capVerts;
    W3dCable* cables;
    unsigned char* oculto;   // por cable: 1 = no se dibuja este cuadro
    int nCables, capCables;
    // lo de cada cuadro (se reserva al agregar)
    unsigned char* color;    // 4 por vertice
    int32_t* px;             // el grosor en pantalla de cada vertice (256 = 1 pixel)
    uint8_t* fuera;          // los bits de afuera de cada vertice
    unsigned short* idxLineas;
    float* cinta;            // 4 vertices de 3 floats por tramo de cinta
    unsigned char* colCinta;
    unsigned short* idxCinta;
    int capCintas;
    // los parametros (se pueden cambiar en cualquier momento)
    float tope;              // la distancia a partir de la cual no se dibuja (metros; por defecto 120)
    float piso;              // el alfa minimo de lejos (0..1; por defecto 0,2)
    float umbralCinta;       // los pixeles desde los que va la cinta (por defecto 2; 0 = nunca)
    float realce;            // el exponente de la cobertura (por defecto 0,5; 1 = lineal)
    unsigned char tablaAlfa[256];
    float tablaRealce;       // (el realce con el que se armo la tabla)
    float distMitad;         // desde esta distancia (metros, la caja del cable) va un vertice de cada dos (0 = nunca;
                             // por defecto 30)
    W3dCablesCuenta ultimo;
    W3dCables();
};

// un cable por sus puntos (n >= 2, x y z en metros), su grosor en metros y su color (rgba 0..255; el alfa es el
// maximo). Devuelve su numero o -1 (sin memoria o mas de 65535 vertices)
int W3dCablesAgregar(W3dCables& l, const float* puntos, int n, float grosor, const unsigned char* rgba);
// un cable COLGADO de 'a' a 'b': 'largo' (metros) mayor a la distancia hace la comba (una catenaria con ese arco;
// menor o igual: derecho); 'tramos' = los pedazos (>= 1); 'abajo' = la direccion de la gravedad (NULL: -Y)
int W3dCablesAgregarComba(W3dCables& l, const float* a, const float* b, float largo, int tramos, float grosor,
                          const unsigned char* rgba, const float* abajo);
// los puntos de la catenaria sin agregarlos (tramos + 1 puntos x y z en 'out')
void W3dCablesComba(const float* a, const float* b, float largo, int tramos, const float* abajo, float* out);
// oculta o muestra un cable (por cuadro: el PVS)
inline void W3dCablesOcultar(W3dCables& l, int cable, bool si) { if (cable >= 0 && cable < l.nCables) l.oculto[cable] = si ? 1 : 0; }
// dibuja el lote. Devuelve los vertices mandados (lineas + cintas)
int W3dCablesDibujar(W3dCables& l, const W3dCablesVista& v);
void W3dCablesSoltar(W3dCables& l);

#endif // W3DCABLES_H
