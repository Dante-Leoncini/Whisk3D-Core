#pragma once
// ============================================================================
//  El backend citro3d (Nintendo 3DS, GPU PICA200): lo que la plataforma le pide ademas de la API de w3dGraphics.h.
//  La 3DS tiene dos pantallas: abajo la tactil (320x240), arriba la grande (400x240). Cada cuadro se dibuja entre
//  C3DFrameBegin y C3DFrameFinish, eligiendo antes la pantalla con C3DScreen.
// ============================================================================

namespace w3dEngine {
    enum { ScreenDown = 0, ScreenUp = 1 };
    bool C3DInit();                       // despues de gfxInit (SDL lo hace al iniciar su video)
    void C3DEnd();
    void C3DFrameBegin();             // C3D_FrameBegin
    void C3DScreen(int screen);       // lo que sigue va a esa pantalla (la limpia la primera vez en el cuadro)
    void C3DFrameFinish();            // C3D_FrameEnd: lo dibujado sale a las pantallas
    int  C3DScreenWidth(int screen);  // 320 abajo / 400 arriba (el alto es 240 en las dos)
    bool C3DInFrame();                // true entre Empezar y Terminar (afuera los draws no hacen nada)
    // la prueba de rendimiento: cuanto tardan los draws del backend (armar el estado, copiar los vertices, las lineas
    // y puntos en la CPU). Medir cuesta (dos llamadas al sistema por draw): solo mide si se prende
    void   C3DMeasure(bool on);
    double C3DTimeDrawsMs();            // lo acumulado desde la ultima llamada (y vuelve a cero)
}
