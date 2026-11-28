#ifndef W3DCONSOLA_H
#define W3DCONSOLA_H
// ============================================================================
//  w3dconsola — lo propio de cada CONSOLA para un juego Whisk3D: la PSP, la
//  PS Vita, la Nintendo 3DS y la Dreamcast. Lo usan el main.cpp que genera el
//  editor (Compilar juego) y los ejemplos en C++ puro (core/).
//
//  En la PC, la web y Android el juego abre su ventana y su contexto de OpenGL
//  con SDL, y SDL_GL_SwapWindow muestra cada cuadro. En las consolas cambia:
//    - PSP: no hay OpenGL. Dibuja el GE con el backend sceGu del Core
//      (graphics/gu): la ventana de SDL va sin contexto y el cuadro lo cierra
//      el GE.
//    - 3DS: tampoco hay OpenGL. Dibuja citro3d (graphics/citro3d) en la
//      pantalla de arriba (400x240); la de abajo queda en negro.
//    - PS Vita: OpenGL de vitaGL, con el contexto de SDL. Necesita el
//      compilador de shaders del sistema (libshacccg.suprx): sin el, avisa y
//      cierra.
//    - Dreamcast: OpenGL de GLdc, con el contexto de SDL.
//  Y los assets: la PSP los lee al lado del EBOOT.PBP, la Vita adentro del .vpk
//  (app0:), la 3DS del romfs del .3dsx y la Dreamcast del romdisk (/rd). Antes
//  de abrir nada, W3dConsoleBeforeSDL deja esa carpeta como la actual: el
//  juego nombra sus archivos por ruta relativa, igual que en la PC.
//
//  Se compila solo en las consolas (W3D_CONSOLE, ademas de la de cada una:
//  W3D_PSP, W3D_VITA, W3D_3DS o W3D_DREAMCAST).
// ============================================================================
#ifdef W3D_CONSOLE
#include <SDL2/SDL.h>
#include <string>

// antes de SDL_Init (y antes del primer mensaje del log): los hints de la consola, la carpeta de los assets como la
// actual y el log del juego en su carpeta de datos (no pisa el del editor de la consola)
void W3dConsoleBeforeSDL(const char* game);
// crea la ventana de la pantalla, el contexto de OpenGL donde lo hay e inicia los graficos.
// NULL = no se pudo (ya quedo dicho en el log, y en la Vita en un cartel)
SDL_Window* W3dConsoleWindow(const char* title);
// el tamano al que dibuja el juego (la pantalla: 480x272, 960x544, 400x240 o 640x480)
void W3dConsoleSize(int* w, int* h);
// la carpeta ESCRIBIBLE para los datos del juego (config.set de lua): ya creada
std::string W3dConsoleData(const char* game);
// cada cuadro: antes de dibujar y despues (lo muestra en la pantalla y espera el vsync)
void W3dConsoleFrameBegin();
void W3dConsoleFrameFinish(SDL_Window* window);
// al salir, antes de SDL_Quit: lo de la consola que SDL no conoce (la 3DS cierra citro3d; si no, al apagar SDL las
// pantallas con citro3d enganchado al sistema, la consola se traba)
void W3dConsoleEnd(SDL_Window* window);
#endif

#endif // W3DCONSOLA_H
