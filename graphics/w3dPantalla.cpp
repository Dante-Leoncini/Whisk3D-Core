// ============================================================================
//  w3dPantalla.cpp — orientacion global. Ver w3dPantalla.h.
//  Sin dependencias: son dos enteros. C++03 puro (sirve igual en Symbian).
// ============================================================================
#include "w3dPantalla.h"

namespace w3dEngine {

static int gW = 0, gH = 0;

void PantallaSetTam(int ancho, int alto) {
    if (ancho < 1) ancho = 1;
    if (alto  < 1) alto  = 1;
    gW = ancho; gH = alto;
}

bool PantallaEsVertical() { return gH >= gW; }

} // namespace w3dEngine
