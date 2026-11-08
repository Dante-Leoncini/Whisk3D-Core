#pragma once
// ============================================================================
//  Lo que comparten w3dGraphicsGu.cpp (el dibujo) y w3dGLgu.cpp (las funciones de GL que usan w3dTexture.cpp y
//  Light.cpp): la tabla de texturas. No es parte de la API del motor.
//
//  LAS TEXTURAS DEL GE: potencia de dos de 8 a 512 por lado, en la RAM comun (el GE las lee de ahi; la VRAM queda para
//  la pantalla y el z), alineadas a 16 bytes y "swizzled": en bloques de 16 bytes x 8 filas, que el GE lee mucho mas
//  rapido que las filas enteras. Las opacas van en 16 bits (565), las que tienen alfa en 32 (8888).
// ============================================================================

namespace w3dGu {
    struct Textura {
        unsigned char* datos;   // (memalign 16; NULL: todavia sin imagen)
        int w, h;               // potencia de dos
        int psm;                // GU_PSM_8888 / GU_PSM_5650
        bool filtro, repetir;
        unsigned version;       // sube con cada cambio (datos, filtro, repeticion): el dibujo la vuelve a atar
    };
    const Textura* TexturaDe(unsigned int id);   // NULL si no existe o no tiene imagen
    // memoria que el GE puede estar leyendo todavia (una textura que se borra o cambia de tamano a mitad del cuadro):
    // se libera cuando termine el cuadro
    void LiberarDespues(void* p);
}
