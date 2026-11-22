// ============================================================================
//  W3dTilt.h — la INCLINACION del equipo (el acelerometro), para jugar con el telefono como volante.
//
//  Los valores estan en los ejes de la PANTALLA como se la ve (los mismos de inclinacion() de lua, ver
//  script/W3dScript.h): x = cuanto esta girada a la derecha, como un volante (el seno del angulo); y = 1 parada y
//  derecha; z = 1 acostada boca arriba. En g (la gravedad mide 1).
//
//  QUE SE PUEDE, HOY:
//   · Symbian S60 3rd FP1 (el N95): la Sensor API de Nokia (RRSensorApi.dll), cargada al abrir: si el telefono no la
//     tiene, el juego anda igual sin sensor (Available() = false). W3dTiltSymbian.cpp.
//   · El resto: todavia nada (Android y la PC la pasan por SDL a W3dScriptTilt, sin esta unidad).
//
//  QUE LADO ES ARRIBA: el que juega sostiene el telefono con la imagen derecha, asi que el eje del telefono que mas
//  peso marca (con la pantalla vertical, el largo; apaisada, el corto) es el de arriba de la pantalla, con su signo.
//  Asi no importa para que lado gira el sistema la pantalla apaisada ni el signo con que mide el sensor.
//
//  Uso:
//      TiltTurnOn(true);                    // el jugador eligio el volante (se guarda en la config)
//      TiltSetLandscape(ancho > alto);        // la plataforma, cuando cambia la ventana
//      float x, y, z;
//      if (TiltRead(x, y, z)) volante = x;   // cada cuadro
//
//  C++03 (Symbian).
// ============================================================================
#ifndef W3D_TILT_H
#define W3D_TILT_H

namespace w3dEngine {

// Prende o apaga el sensor (apagado no gasta bateria) y lo guarda en la config del Core ("incl.active"). Prender en
// un equipo sin sensor no hace nada y Prendida() sigue en false
void TiltTurnOn(bool yes);
bool OnTilt();
// Lo que dijo la config la ultima vez (por defecto, si): para prenderla al arrancar
bool PickedTilt();
// Hay sensor en este equipo (se averigua al prender la primera vez; antes, true si la plataforma tiene con que)
bool AvailableTilt();
// La pantalla esta apaisada (ancho > alto): cambia que eje del telefono es el de arriba
void TiltSetLandscape(bool yes);
// El ultimo dato, en los ejes de la pantalla y en g. false si esta apagada o todavia no llego nada
bool TiltRead(float& x, float& y, float& z);
// Lo mismo en los ejes del TELEFONO (x a la derecha, y arriba, z saliendo de la pantalla, con la pantalla vertical),
// en g, SIN elegir que lado es arriba: para medir cuanto se GIRO desde un momento (el editor: rotar inclinando), sin el
// salto de 180 grados que da TiltRead al cambiar de lado
bool TiltReadPhone(float& x, float& y, float& z);
// De donde sale el dato (para el log): "RRSensorApi", "sin sensor"...
const char* TiltInfo();

// --- la plataforma (W3dTiltSymbian.cpp; en el resto, nada) ---
namespace tilt_platform {
bool Open();                        // prende el sensor; false si no hay
void Close();
// la ultima muestra en los ejes del TELEFONO (x a la derecha, y arriba, z saliendo de la pantalla, con la pantalla
// vertical), en las unidades del sensor; 'n' cuenta las muestras que llegaron (para saber si hay una nueva)
bool Raw(float v[3], unsigned& n);
const char* Info();
}

} // namespace w3dEngine

#endif // W3D_TILT_H
