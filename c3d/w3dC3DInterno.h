#pragma once
// ============================================================================
//  Lo que comparten w3dGraphicsC3D.cpp (el dibujo) y w3dGLc3d.cpp (las funciones de GL que usan w3dTexture.cpp y
//  Light.cpp): la tabla de texturas. No es parte de la API del motor.
// ============================================================================
#include <3ds.h>
#include <citro3d.h>

namespace w3dC3D {
    C3D_Tex* Textura(unsigned int id);   // NULL si no existe o todavia no tiene imagen
}
namespace w3dEngine {
    void C3DTexturaTocada(C3D_Tex* t);   // cambiaron sus datos o su filtro, o se borro: el dibujo la vuelve a atar
}
