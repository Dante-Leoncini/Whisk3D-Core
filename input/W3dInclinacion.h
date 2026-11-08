// ============================================================================
//  W3dInclinacion.h — la INCLINACION del equipo (el acelerometro), para jugar con el telefono como volante.
//
//  Los valores estan en los ejes de la PANTALLA como se la ve (los mismos de inclinacion() de lua, ver
//  script/W3dScript.h): x = cuanto esta girada a la derecha, como un volante (el seno del angulo); y = 1 parada y
//  derecha; z = 1 acostada boca arriba. En g (la gravedad mide 1).
//
//  QUE SE PUEDE, HOY:
//   · Symbian S60 3rd FP1 (el N95): la Sensor API de Nokia (RRSensorApi.dll), cargada al abrir: si el telefono no la
//     tiene, el juego anda igual sin sensor (Disponible() = false). W3dInclinacionSymbian.cpp.
//   · El resto: todavia nada (Android y la PC la pasan por SDL a W3dScriptInclinacion, sin esta unidad).
//
//  QUE LADO ES ARRIBA: el que juega sostiene el telefono con la imagen derecha, asi que el eje del telefono que mas
//  peso marca (con la pantalla vertical, el largo; apaisada, el corto) es el de arriba de la pantalla, con su signo.
//  Asi no importa para que lado gira el sistema la pantalla apaisada ni el signo con que mide el sensor.
//
//  Uso:
//      InclinacionPrender(true);                    // el jugador eligio el volante (se guarda en la config)
//      InclinacionSetApaisada(ancho > alto);        // la plataforma, cuando cambia la ventana
//      float x, y, z;
//      if (InclinacionLeer(x, y, z)) volante = x;   // cada cuadro
//
//  C++03 (Symbian).
// ============================================================================
#ifndef W3D_INCLINACION_H
#define W3D_INCLINACION_H

namespace w3dEngine {

// Prende o apaga el sensor (apagado no gasta bateria) y lo guarda en la config del Core ("incl.activa"). Prender en
// un equipo sin sensor no hace nada y Prendida() sigue en false
void InclinacionPrender(bool si);
bool InclinacionPrendida();
// Lo que dijo la config la ultima vez (por defecto, si): para prenderla al arrancar
bool InclinacionElegida();
// Hay sensor en este equipo (se averigua al prender la primera vez; antes, true si la plataforma tiene con que)
bool InclinacionDisponible();
// La pantalla esta apaisada (ancho > alto): cambia que eje del telefono es el de arriba
void InclinacionSetApaisada(bool si);
// El ultimo dato, en los ejes de la pantalla y en g. false si esta apagada o todavia no llego nada
bool InclinacionLeer(float& x, float& y, float& z);
// De donde sale el dato (para el log): "RRSensorApi", "sin sensor"...
const char* InclinacionInfo();

// --- la plataforma (W3dInclinacionSymbian.cpp; en el resto, nada) ---
namespace inclinacion_plataforma {
bool Abrir();                        // prende el sensor; false si no hay
void Cerrar();
// la ultima muestra en los ejes del TELEFONO (x a la derecha, y arriba, z saliendo de la pantalla, con la pantalla
// vertical), en las unidades del sensor; 'n' cuenta las muestras que llegaron (para saber si hay una nueva)
bool Cruda(float v[3], unsigned& n);
const char* Info();
}

} // namespace w3dEngine

#endif // W3D_INCLINACION_H
