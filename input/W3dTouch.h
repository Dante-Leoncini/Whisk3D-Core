// ============================================================================
//  W3dTouch.h — la PANTALLA TACTIL (uno o varios dedos) para las plataformas que no tienen SDL.
//
//  En Android y la web SDL convierte cada dedo en eventos: el PRIMER dedo ademas mueve y aprieta un mouse
//  (el "mouse del tactil"), los demas solo avisan como dedos. El editor y los juegos estan escritos sobre eso.
//  Esta unidad hace la misma conversion, en el mismo orden (thirdparty/SDL2/src/events/SDL_touch.c), para el
//  resto: Symbian S60 5th (un dedo) y Symbian^3 / Anna / Belle (multitactil, el N8).
//
//  QUE SE PUEDE, HOY:
//   · Symbian: TouchSymbian() toma el TPointerEvent del control (W3dTouchSymbian.cpp). Con multitactil la
//     ventana tiene que haber pedido RWindowBase::EnableAdvancedPointers() antes de activarse, y el build
//     definir W3D_GLES2 (Symbian^3); sin eso todo es el dedo 0.
//   · Cualquier otra: TouchGetFinger() con los pixeles de cada dedo.
//
//  Uso:
//      TouchSetReceptor(MiReceptor);    // quien recibe los eventos (el editor: los vuelve SDL_Event)
//      TouchGetFinger(id, FingerDown, x, y, screenWidth, screenHeight);   // la plataforma, por cada dedo
//      TouchReleaseAll();               // al perder el foco
//
//  C++03 (Symbian).
// ============================================================================
#ifndef W3D_TOUCH_H
#define W3D_TOUCH_H

#ifdef W3D_SYMBIAN
class TPointerEvent;
#endif

namespace w3dEngine {

enum { FingerDown = 0, FingerMoved = 1, FingerUp = 2 };

enum TouchType {
    TouchMouseMoved,     // el mouse del tactil se movio (x, y, xrel, yrel; apretado = con el dedo apoyado)
    TouchMouseButton,    // se apreto (apretado) o se solto el boton izquierdo, en x, y
    TouchFingerUnder,    // un dedo: nx, ny normalizados 0..1 (como SDL)
    TouchFingerUp,
    TouchFingerMoved    // ...con ndx, ndy (el cambio normalizado)
};

struct TouchEvent {
    TouchType type;
    int finger;                // (eventos de dedo)
    int x, y, xrel, yrel;      // (eventos del mouse) pixeles
    bool pressed;
    float nx, ny, ndx, ndy;    // (eventos de dedo) normalizados
};

typedef void (*TouchReceptor)(const TouchEvent& e);
void TouchSetReceptor(TouchReceptor r);

// un dedo (id 0..9) bajo, se movio o subio en (x, y) pixeles, con la pantalla de ancho x alto
void TouchGetFinger(int id, int phase, int x, int y, int width, int height);
// se perdio el foco (llamada, otra app): se levantan todos los dedos que quedaron apoyados
void TouchReleaseAll();
// cuantos dedos hay apoyados
int TouchGetFingers();
// se esta entregando un evento de esta unidad al receptor (el que recibe sabe que viene de un dedo)
bool TouchInEvent();

#ifdef W3D_SYMBIAN
// el evento de puntero de un control de pantalla completa de ancho x alto -> TouchGetFinger
void TouchSymbian(const TPointerEvent& e, int width, int height);
#endif

} // namespace w3dEngine

#endif
