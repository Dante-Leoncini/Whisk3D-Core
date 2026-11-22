// ============================================================================
//  W3dTouch.cpp — ver W3dTouch.h. Cada dedo -> los eventos que SDL sintetiza en Android, en el mismo orden:
//  el primer dedo mueve el mouse (si cambio de lugar) y lo aprieta, y despues avisa como dedo; al subir, suelta
//  el mouse y despues avisa. El mouse queda en el dedo que lo agarro hasta que ese dedo sube.
// ============================================================================
#include "W3dTouch.h"

namespace w3dEngine {

namespace {
    enum { kMaxFingers = 10 };
    struct TouchFinger { bool down; float x, y; };   // x/y normalizados, como los guarda SDL
    TouchFinger gFingers[kMaxFingers];
    int  gFingerMouse = -1;          // el dedo que maneja el mouse (el primero que bajo); -1 = ninguno
    int  gMouseX = 0, gMouseY = 0;   // donde quedo el mouse (SDL arranca en 0,0)
    int  gWidth = 1, gHeight = 1;
    bool gInEvent = false;
    TouchReceptor gReceptor = 0;

    void Deliver(TouchEvent& e) {
        if (!gReceptor) return;
        const bool prev = gInEvent;
        gInEvent = true;
        gReceptor(e);
        gInEvent = prev;
    }

    TouchEvent New(TouchType type) {
        TouchEvent e;
        e.type = type; e.finger = 0; e.x = e.y = e.xrel = e.yrel = 0; e.pressed = false;
        e.nx = e.ny = e.ndx = e.ndy = 0.0f;
        return e;
    }

    // SDL descarta el movimiento del mouse que no cambia la posicion
    void MouseA(int x, int y, bool pressed) {
        if (x < 0) x = 0;
        if (x > gWidth - 1) x = gWidth - 1;
        if (y < 0) y = 0;
        if (y > gHeight - 1) y = gHeight - 1;
        const int xrel = x - gMouseX, yrel = y - gMouseY;
        if (xrel == 0 && yrel == 0) return;
        gMouseX = x; gMouseY = y;
        TouchEvent e = New(TouchMouseMoved);
        e.x = x; e.y = y; e.xrel = xrel; e.yrel = yrel; e.pressed = pressed;
        Deliver(e);
    }

    void ButtonMouse(bool pressed) {
        TouchEvent e = New(TouchMouseButton);
        e.x = gMouseX; e.y = gMouseY; e.pressed = pressed;
        Deliver(e);
    }

    void NotifyFinger(TouchType type, int id, float nx, float ny, float ndx, float ndy) {
        TouchEvent e = New(type);
        e.finger = id; e.nx = nx; e.ny = ny; e.ndx = ndx; e.ndy = ndy;
        Deliver(e);
    }

    void Up(int id) {
        TouchFinger& d = gFingers[id];
        if (!d.down) return;   // (SDL: "this finger is already up")
        if (gFingerMouse == id) { gFingerMouse = -1; ButtonMouse(false); }
        d.down = false;
        NotifyFinger(TouchFingerUp, id, d.x, d.y, 0.0f, 0.0f);   // (SDL manda las coordenadas que ya tenia)
    }
}

void TouchSetReceptor(TouchReceptor r) { gReceptor = r; }

void TouchGetFinger(int id, int phase, int x, int y, int width, int height) {
    if (id < 0 || id >= kMaxFingers) return;
    if (width > 0) gWidth = width;
    if (height > 0)  gHeight = height;
    const float nx = (float)x / (float)gWidth, ny = (float)y / (float)gHeight;
    TouchFinger& d = gFingers[id];
    if (phase == FingerUp) { Up(id); return; }
    if (phase == FingerMoved && !d.down) phase = FingerDown;   // (SDL: un movimiento de un dedo que no bajo es un down)
    if (phase == FingerDown) {
        if (d.down) Up(id);   // se perdio el up anterior: SDL lo manda antes del nuevo down
        if (gFingerMouse < 0) {
            gFingerMouse = id;
            MouseA(x, y, false);
            ButtonMouse(true);
        }
        d.down = true; d.x = nx; d.y = ny;
        NotifyFinger(TouchFingerUnder, id, nx, ny, 0.0f, 0.0f);
        return;
    }
    // se movio
    if (gFingerMouse == id) MouseA(x, y, true);
    const float ndx = nx - d.x, ndy = ny - d.y;
    if (ndx == 0.0f && ndy == 0.0f) return;   // SDL descarta el que no cambia nada
    d.x = nx; d.y = ny;
    NotifyFinger(TouchFingerMoved, id, nx, ny, ndx, ndy);
}

void TouchReleaseAll() {
    // primero los que no tienen el mouse: el ultimo en subir lo suelta, como al levantar la mano
    for (int i = 0; i < kMaxFingers; i++) if (i != gFingerMouse) Up(i);
    if (gFingerMouse >= 0) Up(gFingerMouse);
}

int TouchGetFingers() {
    int n = 0;
    for (int i = 0; i < kMaxFingers; i++) if (gFingers[i].down) n++;
    return n;
}

bool TouchInEvent() { return gInEvent; }

} // namespace w3dEngine
