// ============================================================================
//  W3dMandos.h — los MANDOS (joysticks) de las plataformas que no los reciben por SDL.
//
//  Hasta 4, cada uno con sus sticks, gatillos y botones, y si esta conectado. Los botones y los ejes van en el orden
//  de SDL_GameController (A B X Y Back Guia Start L3 R3 L1 R1 arriba abajo izquierda derecha; LX LY RX RY LT RT): el
//  que los recibe los pone en el mismo lugar donde SDL pone los suyos y de ahi para adentro es todo igual. Con la
//  disposicion de Xbox: A = el de abajo (la X de PlayStation), B = el de la derecha (el circulo), X = el de la
//  izquierda (el cuadrado), Y = el de arriba (el triangulo).
//
//  QUE SE PUEDE, HOY:
//   · Symbian (S60 3rd, 5th y Symbian^3): los joysticks de PS3 y PS4 (y los genericos que hablan como ellos) por el
//     driver Bluetooth bthid-s60 (bthidhost.dll), cargado al arrancar: si el telefono no lo tiene instalado no hay
//     mandos y todo anda igual. W3dMandosSymbian.cpp.
//   · El resto: SDL (no usa esta unidad).
//
//  Uso, cada cuadro:
//      MandoEstado e;
//      for (int i = 0; i < MandosMax; i++) if (MandoLeer(i, e)) ...   // false = no hay mando con ese numero
//      MandoVibrar(0, true);                                           // hasta que se apague
//
//  C++03 (Symbian).
// ============================================================================
#ifndef W3D_MANDOS_H
#define W3D_MANDOS_H

namespace w3dEngine {

enum { MandosMax = 4 };
enum MandoBoton {
    MandoA, MandoB, MandoX, MandoY, MandoSelect, MandoGuia, MandoStart, MandoL3, MandoR3, MandoL1, MandoR1,
    MandoArriba, MandoAbajo, MandoIzquierda, MandoDerecha,
    MandoBotones
};
enum MandoEje { MandoLX, MandoLY, MandoRX, MandoRY, MandoLT, MandoRT, MandoEjes };
enum MandoTipo { MandoGenerico = 0, MandoPS3 = 3, MandoPS4 = 4 };

struct MandoEstado {
    int tipo;                      // MandoTipo
    float ejes[MandoEjes];         // sticks -1..1 (Y positivo = hacia abajo, como SDL); gatillos 0..1
    bool botones[MandoBotones];
};

// el mando i (0..3) esta conectado? -> su estado. Lo pide al driver: llamarlo una vez por cuadro y por mando
bool MandoLeer(int i, MandoEstado& e);
// prender o apagar la vibracion del mando i
void MandoVibrar(int i, bool si);
// soltar el driver (al cerrar la app)
void MandosCerrar();

} // namespace w3dEngine

#endif
