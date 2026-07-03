#include "RenderColors.h"

// DEFAULTS razonables (el editor los pisa con su tema en SincronizarRenderColors,
// main.cpp, tras cargar el skin). Orden = enum RenderColorId.
float gRenderColors[RC_COUNT][4] = {
    {0.50f, 0.50f, 0.50f, 1.0f}, // RC_wireframe   (gris)
    {1.00f, 0.55f, 0.00f, 1.0f}, // RC_selActive   (naranja)
    {0.70f, 0.45f, 0.15f, 1.0f}, // RC_selInactive (naranja apagado)
    {0.20f, 0.70f, 1.00f, 1.0f}, // RC_normalFace  (cian)
    {1.00f, 1.00f, 0.20f, 1.0f}, // RC_normalVert  (amarillo)
    {1.00f, 0.20f, 1.00f, 1.0f}, // RC_normalCustom(magenta)
    {0.40f, 0.25f, 0.00f, 1.0f}  // RC_gizmoDark   (naranja oscuro)
};
