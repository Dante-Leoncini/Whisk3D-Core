// ============================================================================
//  W3dVibrar.h — vibracion / haptica, con lo que cada plataforma permita.
//
//  QUE SE PUEDE, HOY:
//   · Android (Chrome/WebView): navigator.vibrate(ms). Anda bien y es preciso.
//   · iOS (Safari): navigator.vibrate NO EXISTE. Apple nunca lo implemento, ni
//     siquiera con la app agregada al inicio. Lo unico que dispara el motor
//     haptico desde una pagina es un truco: activar un <input type="checkbox"
//     switch> (iOS 17.4+), que hace el "tic" del sistema. No se puede elegir la
//     intensidad ni la duracion, y Apple lo puede cerrar cuando quiera.
//   · Escritorio: no hay nada que vibrar.
//
//  Por eso la API promete POCO: "hace un toque corto si se puede". El que la
//  usa no tiene que saber en que plataforma esta ni si funciono.
//
//  Uso:
//      VibrarInit();                 // una vez, al arrancar (lee la config)
//      Vibrar(w3dEngine::VibTic);    // en cada golpe que el jugador ve
//      VibrarSetActiva(false);       // la opcion "activar vibracion", se guarda sola
// ============================================================================
#ifndef W3D_VIBRAR_H
#define W3D_VIBRAR_H

namespace w3dEngine {

// Intensidades. En iOS todas suenan igual (el sistema no deja elegir): la
// diferencia se nota en Android.
enum VibFuerza {
    VibTic = 0,    // golpecito seco: un rodillo que frena, cambiar de categoria
    VibToque,      // confirmacion: apretar un boton
    VibGolpe       // algo importante: un premio
};

// Prepara la vibracion y lee de la config si el jugador la quiere. Idempotente.
void VibrarInit();

// Un toque, si la plataforma puede y el jugador no la apago. Nunca falla.
void Vibrar(VibFuerza f);

// La opcion "activar vibracion": se guarda con el resto de la config del Core.
void VibrarSetActiva(bool activa);
bool VibrarActiva();

// true si en ESTA plataforma la vibracion hace algo. Sirve para no mostrar la
// opcion en un escritorio, donde no tendria sentido.
bool VibrarDisponible();

// Una vibracion de 'ms' milisegundos con 'fuerza' 0..1 (lo que pide un juego: el golpe de un choque, el traqueteo del
// pasto). Android: el vibrador del telefono (la actividad del juego tiene vibrar(int); la fuerza no se puede elegir y
// acorta el pulso); web: navigator.vibrate; y siempre el motor de los MANDOS (W3dScriptMandosVibrarHook de
// script/W3dScript.h, lo pone quien los abre). Respeta la opcion. Es lo que llama vibrar() de lua (este .cpp se instala
// solo en W3dScriptVibrarHook)
void VibrarMs(int ms, float fuerza);

} // namespace w3dEngine

#endif // W3D_VIBRAR_H
