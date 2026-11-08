// ============================================================================
//  W3dTactilSymbian.cpp — la pantalla tactil de Symbian para W3dTactil.h: el TPointerEvent de un control de
//  pantalla completa -> TactilDedo.
//
//  S60 5th (5800, N97): un dedo, siempre el 0. Symbian^3 (N8 y los que siguen; el build W3D_GLES2): cada dedo
//  trae su numero si la ventana pidio EnableAdvancedPointers(). En el N95 (sin tactil) no llega ningun evento.
//  OJO N95: el build ES 1.1 no puede importar nada de Symbian^3; TAdvancedPointerEvent::PointerNumber() es una
//  exportacion nueva, por eso va solo con W3D_GLES2.
// ============================================================================
#ifdef W3D_SYMBIAN
#include <w32std.h>
#include "W3dTactil.h"

namespace w3dEngine {

void TactilSymbian(const TPointerEvent& e, int ancho, int alto) {
    int dedo = 0;
#ifdef W3D_GLES2
    if (e.IsAdvancedPointerEvent()) dedo = e.AdvancedPointerEvent()->PointerNumber();
#endif
    int fase;
    switch (e.iType) {
        case TPointerEvent::EButton1Down: fase = DedoAbajo;  break;
        case TPointerEvent::EDrag:        fase = DedoMovio;  break;
        case TPointerEvent::EButton1Up:   fase = DedoArriba; break;
        default: return;   // (EMove / EOutOfRange: el dedo cerca de la pantalla sin tocarla, en los que lo detectan)
    }
    TactilDedo(dedo, fase, e.iPosition.iX, e.iPosition.iY, ancho, alto);
}

} // namespace w3dEngine
#endif
