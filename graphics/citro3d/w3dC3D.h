#pragma once
// ============================================================================
//  El backend citro3d (Nintendo 3DS, GPU PICA200): lo que la plataforma le pide ademas de la API de w3dGraphics.h.
//  La 3DS tiene dos pantallas: abajo la tactil (320x240), arriba la grande (400x240). Cada cuadro se dibuja entre
//  C3DCuadroEmpezar y C3DCuadroTerminar, eligiendo antes la pantalla con C3DPantalla.
// ============================================================================

namespace w3dEngine {
    enum { PantallaAbajo = 0, PantallaArriba = 1 };
    bool C3DInit();                       // despues de gfxInit (SDL lo hace al iniciar su video)
    void C3DFin();
    void C3DCuadroEmpezar();              // C3D_FrameBegin
    void C3DPantalla(int pantalla);       // lo que sigue va a esa pantalla (la limpia la primera vez en el cuadro)
    void C3DCuadroTerminar();             // C3D_FrameEnd: lo dibujado sale a las pantallas
    int  C3DAnchoPantalla(int pantalla);  // 320 abajo / 400 arriba (el alto es 240 en las dos)
    bool C3DEnCuadro();                   // true entre Empezar y Terminar (afuera los draws no hacen nada)
    // la prueba de rendimiento: cuanto tardan los draws del backend (armar el estado, copiar los vertices, las lineas
    // y puntos en la CPU). Medir cuesta (dos llamadas al sistema por draw): solo mide si se prende
    void   C3DMedir(bool on);
    double C3DTiempoDrawsMs();            // lo acumulado desde la ultima llamada (y vuelve a cero)
}
