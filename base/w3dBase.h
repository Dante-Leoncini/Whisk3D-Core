#pragma once

typedef float ColorType;

void w3dSetColor(const ColorType c[4]);

#ifdef W3D_OPENGL
    #ifdef _WIN32
        #ifndef W3D_SYMBIAN
            #include <windows.h>
        #endif
    #endif

    #include <GL/gl.h>

    #ifndef GL_POINT_SPRITE
    #define GL_POINT_SPRITE 0x8861
    #endif

    #ifndef GL_COORD_REPLACE
    #define GL_COORD_REPLACE 0x8862
    #endif
#endif
