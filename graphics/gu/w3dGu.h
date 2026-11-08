#pragma once
// ============================================================================
//  El backend sceGu (la PSP, su GPU: el GE): lo que la plataforma le pide ademas de la API de w3dGraphics.h.
//  La pantalla es una sola, de 480x272. Cada cuadro se dibuja entre GuCuadroEmpezar y GuCuadroTerminar; lo que se
//  dibuje fuera de un cuadro (la pasada de seleccion, la barra de progreso de un export) abre su propia lista y va
//  al buffer de atras, que no se ve hasta el proximo GuCuadroTerminar.
// ============================================================================

namespace w3dEngine {
    enum { GuAncho = 480, GuAlto = 272 };
    bool GuIniciar();                     // sceGuInit, los buffers en la VRAM y la pantalla prendida
    void GuFin();
    void GuCuadroEmpezar();
    void GuCuadroTerminar();              // espera al GE, el vsync y da vuelta los buffers: lo dibujado sale a la pantalla
    bool GuEnCuadro();
    // la pantalla que se esta viendo (RGBA de 8 bits, filas de GuPasoPantalla() pixeles, de arriba hacia abajo), para
    // la captura de prueba. Se lee de la VRAM sin cache
    const unsigned char* GuPantallaMostrada();
    int GuPasoPantalla();
}
