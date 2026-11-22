// ============================================================================
//  w3dScreen.cpp — orientacion global. Ver w3dScreen.h.
//  Sin dependencias: son dos enteros. C++03 puro (sirve igual en Symbian).
// ============================================================================
#include "w3dScreen.h"

namespace w3dEngine {

static int gW = 0, gH = 0;

void ScreenSetSize(int width, int height) {
    if (width < 1) width = 1;
    if (height  < 1) height  = 1;
    gW = width; gH = height;
}

bool ScreenIsVertical() { return gH >= gW; }

} // namespace w3dEngine
