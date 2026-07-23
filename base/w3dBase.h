#pragma once

typedef float ColorType;

void w3dSetColor(const ColorType c[4]);

#ifdef W3D_OPENGL
    // windows.h ya lo resuelve crossplatform.h (con LEAN_AND_MEAN y NOMINMAX: sin las
    // macros min/max que rompen std::min/std::max en todo lo que incluya este header)
    #include "crossplatform.h"

    #include <GL/gl.h>

    #ifndef GL_POINT_SPRITE
    #define GL_POINT_SPRITE 0x8861
    #endif

    #ifndef GL_COORD_REPLACE
    #define GL_COORD_REPLACE 0x8862
    #endif
#endif
