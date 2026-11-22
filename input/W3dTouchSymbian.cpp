// ============================================================================
//  W3dTouchSymbian.cpp — la pantalla tactil de Symbian para W3dTouch.h: el TPointerEvent de un control de
//  pantalla completa -> TouchGetFinger.
//
//  S60 5th (5800, N97): un dedo, siempre el 0. Symbian^3 (N8 y los que siguen; el build W3D_GLES2): cada dedo
//  trae su numero si la ventana pidio EnableAdvancedPointers(). En el N95 (sin tactil) no llega ningun evento.
//  OJO N95: el build ES 1.1 no puede importar nada de Symbian^3; TAdvancedPointerEvent::PointerNumber() es una
//  exportacion nueva, por eso va solo con W3D_GLES2.
// ============================================================================
#ifdef W3D_SYMBIAN
#include <w32std.h>
#include "W3dTouch.h"

namespace w3dEngine {

void TouchSymbian(const TPointerEvent& e, int width, int height) {
    int finger = 0;
#ifdef W3D_GLES2
    if (e.IsAdvancedPointerEvent()) finger = e.AdvancedPointerEvent()->PointerNumber();
#endif
    int phase;
    switch (e.iType) {
        case TPointerEvent::EButton1Down: phase = FingerDown;  break;
        case TPointerEvent::EDrag:        phase = FingerMoved;  break;
        case TPointerEvent::EButton1Up:   phase = FingerUp; break;
        default: return;   // (EMove / EOutOfRange: el dedo cerca de la pantalla sin tocarla, en los que lo detectan)
    }
    TouchGetFinger(finger, phase, e.iPosition.iX, e.iPosition.iY, width, height);
}

} // namespace w3dEngine
#endif
