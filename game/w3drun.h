#ifndef W3DRUN_H
#define W3DRUN_H
#include <string>

// ============================================================================
//  w3drun — el RUNTIME de un juego Whisk3D (escena 3D + UI + scripts lua), SIN
//  el editor.
//
//  Un juego hecho en el editor es: una ESCENA 3D (el project.json del .w3d) +
//  un arbol de UI para el HUD (.w3dui) + uno o varios scripts lua colgados de
//  los objetos, con sus propiedades expuestas resueltas a objetos de la escena.
//  Este runtime reusa EXACTAMENTE el mismo lector de proyecto (import_w3d), el
//  mismo pase 3D (main/render/EscenaRender: encuadre, camara, recorrido de la
//  escena con culling/LOD/visibilidad/espejos, calcomanias, luces y particulas),
//  el mismo render 2D y el mismo interprete lua del editor (UIOverlay +
//  W3dScript del Core), asi que el juego se ve y se comporta igual que en el
//  Play del editor, pero corre solo.
//
//  Un juego 2D puro (whiskpaddle, calculadora) simplemente no carga escena 3D:
//  el pase 3D no se dibuja y todo queda como siempre.
//
//  Uso (ver ui/whiskpaddle/main.cpp):
//     W3dGameFontPngSet("res/font.png");     // la tipografia pixel
//     W3dGameLoadUI("whiskpaddle.w3dui");
//     W3dGameScript("whiskpaddle.lua");       // agrega un script
//     W3dGameRef("jugador1", "Pala 1");       // sus referencias (por nombre)
//     W3dGameStart();                        // resuelve refs + llama start()
//     ... por frame:  W3dGameUpdate(dt);  W3dGameRender(w, h);
//     ... input:      W3dGameKey(sdlk, down);  W3dGameStick(0, x, y);
//
//  El runtime corre en PC (OpenGL) y en la web (WebGL): el mismo binario/wasm.
// ============================================================================

// ruta de font.png (la tipografia pixel del skin). Llamar ANTES de cargar la UI.
void W3dGameFontPngSet(const std::string& path);

// carga el arbol de la interfaz (.w3dui). false si no pudo.
// CAMINO LEGACY (una sola escena): despues se le agregan scripts con W3dGameScript/W3dGameRef.
bool W3dGameLoadUI(const std::string& path);

// MULTI-ESCENA: carga UNA escena mas (un .w3dui) y la ACUMULA (sin pisar las anteriores). Sus
// scripts salen del propio .w3dui (no se agregan con W3dGameScript). Llamar una por escena.
bool W3dGameLoadScene(const std::string& path);
// nombre de la escena que ARRANCA (el "nombre" de su .w3dui). Sin esto arranca la primera.
void W3dGameInitialScene(const std::string& name);

// ---------------------------------------------------------------------------
//  ESCENA 3D (juegos 3D). 'ruta' es el project.json que "Compilar juego" dejo al
//  lado del binario: adentro esta el arbol de la escena (mallas, materiales,
//  animaciones, camaras, rieles, culling/LOD/visibilidad, espejos, particulas).
//  Se carga con EL MISMO lector que el editor (W3dProjectLoadScene3D), asi
//  que el juego arma exactamente la misma escena que el Play.
//  Llamar ANTES de W3dGameStart (los scripts resuelven sus refs contra ella).
//  Un juego 2D puro simplemente no la llama y todo sigue como antes.
bool W3dGameLoadScene3D(const std::string& path);
// true si hay escena 3D cargada (el render hace el pase 3D antes del HUD).
bool W3dGameHas3D();

// ---------------------------------------------------------------------------
//  CAPTURA (verificacion automatica): guarda el frame numero 'frame' en 'png' y
//  despues pide cerrar el juego. Es lo que usa la prueba de regresion para
//  comprobar POR PIXELES que un juego compilado dibuja de verdad su escena 3D.
//  El main.cpp generado la cablea a las variables de entorno W3D_CAPTURA (ruta
//  del png) y W3D_CAPTURA_FRAME (numero de frame, default 30).
void W3dGameCapture(const std::string& png, int frame);

// agrega un script lua (se resuelve al llamar W3dGameStart)
void W3dGameScript(const std::string& pathLua);
// asigna una propiedad expuesta del ULTIMO script: el nombre del objeto
// referenciado, o la opcion elegida (los desplegables)
void W3dGameRef(const std::string& prop, const std::string& value);

// resuelve las referencias por nombre y llama start() de cada script
void W3dGameStart();
// un frame de logica (dt en segundos)
void W3dGameUpdate(float dt);
// dibuja la UI centrada y escalada para entrar en (winW x winH)
void W3dGameRender(int winW, int winH);
// cuanto tardaron (en la CPU, ms) el pase 3D y el HUD en el ultimo W3dGameRender (el medidor del modo debug)
void W3dGameTimes(float* ms3d, float* ms2d);

// ZONA SEGURA: insets (notch/barras) en px de ventana. El fondo llena toda la ventana; la UI se acota
// a este rect. La plataforma lo actualiza (ej. Android, del display cutout). PC/web: 0 (sin recorte).
void W3dGameSafeArea(int l, int t, int r, int b);
// true si el juego pidio cerrarse (lua exit()); la plataforma corta su loop.
bool W3dGameWantsStop(void);

// ---- input ----
void W3dGameKey(int sdlk, bool pressed);   // teclado (SDL keycode)
// toque/arrastre en PIXELES DE VENTANA (se convierte al lienzo con el escalado
// del ultimo W3dGameRender); activo=false al soltar. Llega a lua como touch()
void W3dGameTouch(int winX, int winY, bool active);
// multi-touch: dedo idx (0-based) en px de ventana -> finger(idx+1) en lua (2 jugadores tactiles)
void W3dGameFinger(int idx, int winX, int winY, bool active);
void W3dGameMouse(int winX, int winY, bool inside); // mouse-over (posicion sin clic) -> mouse() en lua
void W3dGameStick(int which, float x, float y);// stick analogico (0 izq, 1 der)
void W3dGameTrigger(int which, float v);       // gatillo analogico 0..1 (0 izq / LT, 1 der / RT): pedales
void W3dGameButton(const char* name, bool v);// boton del gamepad ("a".."y")

#endif // W3DRUN_H
