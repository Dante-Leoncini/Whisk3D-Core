#ifndef W3D_SAFE_AREA_H
#define W3D_SAFE_AREA_H
// ============================================================================
//  AREA SEGURA (safe area) — donde SE PUEDE dibujar la UI.
//
//  Regla de oro del motor:
//    · El FONDO / la escena se dibujan SIEMPRE en el 100% de la ventana (full-bleed):
//      por debajo del notch, dentro de las esquinas redondeadas y bajo las barras del
//      sistema. Asi nunca quedan franjas negras.
//    · La UI (botones, textos, HUD) se limita al AREA SEGURA, para que nada quede
//      tapado por el notch, la barra de gestos o las barras del navegador.
//
//  Los "insets" son los pixeles NO dibujables de cada borde. En la mayoria de las
//  plataformas son 0 (toda la ventana es segura: PC, Symbian, consolas). Donde NO:
//    · Navegador (iOS/Android): notch + barra de herramientas que se superpone.
//    · iOS nativo: safe area insets (notch / home indicator).
//    · Android nativo: display cutout + window insets (barra de estado / navegacion).
//
//  Uso tipico por frame:
//      w3dEngine::SafeAreaUpdate(dpr);              // la plataforma los mide (no-op donde no aplica)
//      const w3dEngine::SafeArea& sa = w3dEngine::SafeAreaGet();
//      ... dibujo el fondo en (0,0,w,h) y la UI dentro de [sa.left, w-sa.right] x [sa.top, h-sa.bottom]
// ============================================================================

namespace w3dEngine {

// pixeles NO dibujables en cada borde de la ventana (en pixeles de DISPOSITIVO)
struct SafeArea {
    float top, bottom, left, right;
};

// Los mide la plataforma. 'dpr' = device pixel ratio (1.0 si no aplica): los insets del
// navegador vienen en px CSS y se convierten a px de dispositivo. En plataformas sin
// notch ni barras superpuestas NO hace nada (los insets quedan en 0).
void SafeAreaUpdate(float dpr);

// Setear a mano (host nativo iOS/Android que ya conoce sus insets, o para forzar en tests).
void SafeAreaSet(float top, float bottom, float left, float right);

const SafeArea& SafeAreaGet();

// Rectangulo UTIL dentro de una ventana de w x h (lo que queda tras descontar los insets).
void SafeRect(int w, int h, float* x, float* y, float* sw, float* sh);

// COLOR DE LAS FRANJAS que quedan FUERA del area dibujable.
//
// En un navegador de celular (Safari/Chrome iOS) la pagina NO puede dibujar en la barra de
// estado ni detras de la barra de herramientas: esas franjas las pinta el navegador con el
// color de fondo de la pagina, y si es negro se ven dos bandas negras. Con esto le decimos
// el color de arriba y el de abajo (ej: el color promedio del borde del fondo del juego) y

// IMAGEN de fondo para esas franjas. Es lo mejor que se puede hacer en un navegador de celular:
// ningun ELEMENTO de la pagina puede pintar donde va la barra del navegador, pero el fondo del
// elemento RAIZ si se propaga a toda la ventana. Poniendo ahi la MISMA imagen del juego, con el
// MISMO encuadre con el que la dibuja el motor, la escena continua debajo de la barra y del notch.
//   bytes/len/mime = la imagen (los mismos bytes que ya tenes del pack)
//   w,h,x,y        = tamanio y posicion EN PIXELES CSS con que hay que pintarla (encuadre)
void SafeAreaSetImagen(const void* bytes, int len, const char* mime,
                       float w, float h, float x, float y);

// Igual pero con VARIOS colores (de arriba hacia abajo): la franja se pinta como un degradado
// que continua mejor la escena que un color plano. 'n' colores, en un array r,g,b consecutivos.
void SafeAreaSetDegradado(const unsigned char* rgb, int n);

} // namespace w3dEngine

#endif // W3D_SAFE_AREA_H
