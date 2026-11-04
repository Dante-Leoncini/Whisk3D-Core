// ============================================================================
//  W3dTactil.h — la PANTALLA TACTIL (uno o varios dedos) para las plataformas que no tienen SDL.
//
//  En Android y la web SDL convierte cada dedo en eventos: el PRIMER dedo ademas mueve y aprieta un mouse
//  (el "mouse del tactil"), los demas solo avisan como dedos. El editor y los juegos estan escritos sobre eso.
//  Esta unidad hace la misma conversion, en el mismo orden (thirdparty/SDL2/src/events/SDL_touch.c), para el
//  resto: Symbian S60 5th (un dedo) y Symbian^3 / Anna / Belle (multitactil, el N8).
//
//  QUE SE PUEDE, HOY:
//   · Symbian: TactilSymbian() toma el TPointerEvent del control (W3dTactilSymbian.cpp). Con multitactil la
//     ventana tiene que haber pedido RWindowBase::EnableAdvancedPointers() antes de activarse, y el build
//     definir W3D_GLES2 (Symbian^3); sin eso todo es el dedo 0.
//   · Cualquier otra: TactilDedo() con los pixeles de cada dedo.
//
//  Uso:
//      TactilSetReceptor(MiReceptor);    // quien recibe los eventos (el editor: los vuelve SDL_Event)
//      TactilDedo(id, DedoAbajo, x, y, anchoPantalla, altoPantalla);   // la plataforma, por cada dedo
//      TactilSoltarTodo();               // al perder el foco
//
//  C++03 (Symbian).
// ============================================================================
#ifndef W3D_TACTIL_H
#define W3D_TACTIL_H

#ifdef W3D_SYMBIAN
class TPointerEvent;
#endif

namespace w3dEngine {

enum { DedoAbajo = 0, DedoMovio = 1, DedoArriba = 2 };

enum TactilTipo {
    TactilMouseMovio,    // el mouse del tactil se movio (x, y, xrel, yrel; apretado = con el dedo apoyado)
    TactilMouseBoton,    // se apreto (apretado) o se solto el boton izquierdo, en x, y
    TactilDedoBajo,      // un dedo: nx, ny normalizados 0..1 (como SDL)
    TactilDedoSubio,
    TactilDedoSeMovio    // ...con ndx, ndy (el cambio normalizado)
};

struct TactilEvento {
    TactilTipo tipo;
    int dedo;                  // (eventos de dedo)
    int x, y, xrel, yrel;      // (eventos del mouse) pixeles
    bool apretado;
    float nx, ny, ndx, ndy;    // (eventos de dedo) normalizados
};

typedef void (*TactilReceptor)(const TactilEvento& e);
void TactilSetReceptor(TactilReceptor r);

// un dedo (id 0..9) bajo, se movio o subio en (x, y) pixeles, con la pantalla de ancho x alto
void TactilDedo(int id, int fase, int x, int y, int ancho, int alto);
// se perdio el foco (llamada, otra app): se levantan todos los dedos que quedaron apoyados
void TactilSoltarTodo();
// cuantos dedos hay apoyados
int TactilDedos();
// se esta entregando un evento de esta unidad al receptor (el que recibe sabe que viene de un dedo)
bool TactilEnEvento();

#ifdef W3D_SYMBIAN
// el evento de puntero de un control de pantalla completa de ancho x alto -> TactilDedo
void TactilSymbian(const TPointerEvent& e, int ancho, int alto);
#endif

} // namespace w3dEngine

#endif
