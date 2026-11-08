// ============================================================================
//  Backend del portapapeles para PC / web (SDL2). SDL mapea esto al portapapeles del
//  SO (en PC) o del navegador (emscripten, vDSP -sUSE_SDL=2). Mismo guard que el
//  backend de audio SDL: entra en todos los builds MENOS Symbian.
// ============================================================================
#if !defined(W3D_SYMBIAN)

#include "W3dClipboard.h"
#include <SDL2/SDL.h>

namespace w3dEngine {

void W3dClipboardSet(const std::string& s) {
    SDL_SetClipboardText(s.c_str());
}

std::string W3dClipboardGet() {
    std::string r;
    char* c = SDL_GetClipboardText();   // SDL siempre devuelve una cadena (posiblemente vacia) que hay que liberar
    if (c) { r = c; SDL_free(c); }
    return r;
}

} // namespace w3dEngine

#endif // !W3D_SYMBIAN
