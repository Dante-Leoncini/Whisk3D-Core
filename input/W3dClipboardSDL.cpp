// ============================================================================
//  Backend del portapapeles para PC / web (SDL2). En la PC SDL lo mapea al portapapeles del SO. Mismo guard que el
//  backend de audio SDL: entra en todos los builds MENOS Symbian.
//
//  LA WEB: el SDL2 de emscripten NO tiene portapapeles del navegador (solo uno propio, adentro de la pagina): copiar en
//  el IDE no llegaba afuera y pegar en el IDE codigo copiado afuera no andaba (Chrome). Y no alcanzaba con leerlo al
//  apretar Ctrl+V: SDL le hace preventDefault a toda tecla con Ctrl, asi que el navegador ni dispara su "pegar". Por eso:
//    - PEGAR: el Ctrl+V (o Cmd+V) no llega a SDL; el navegador hace su "pegar" y dispara 'paste' con el texto (sin pedir
//      permiso). Ese texto pasa al portapapeles de SDL y la app recibe el MISMO Ctrl+V de siempre (w3dWebPaste), que
//      pega lo que tiene el portapapeles.
//    - COPIAR: ademas del de SDL, el del navegador (navigator.clipboard.writeText: pagina segura y con el foco).
//  Un campo de texto de la PAGINA (no el lienzo de la app) pega y copia como siempre.
// ============================================================================
#if !defined(W3D_SYMBIAN)

#include "W3dClipboard.h"
#include <SDL2/SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <vector>
#endif

#ifdef __EMSCRIPTEN__
// el texto que trajo el 'paste' del navegador (JS lo escribe en este buffer: ver w3dWebPasteBuffer)
static std::vector<char> gStuck;
extern "C" EMSCRIPTEN_KEEPALIVE char* w3dWebPasteBuffer(int bytes) {
    gStuck.assign(bytes > 0 ? (size_t)bytes : 1, 0);
    return &gStuck[0];
}
// el 'paste' del navegador llego: al portapapeles de SDL, y a la app el Ctrl+V que el navegador se quedo
extern "C" EMSCRIPTEN_KEEPALIVE void w3dWebPaste() {
    SDL_SetClipboardText(gStuck.empty() ? "" : &gStuck[0]);
    SDL_Window* w = SDL_GetKeyboardFocus();
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN;
    e.key.windowID = w ? SDL_GetWindowID(w) : 0;
    e.key.state = SDL_PRESSED;
    e.key.keysym.scancode = SDL_SCANCODE_V;
    e.key.keysym.sym = SDLK_v;
    e.key.keysym.mod = (Uint16)(SDL_GetModState() | KMOD_LCTRL);
    SDL_PushEvent(&e);
    e.type = SDL_KEYUP;
    e.key.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}
EM_JS(void, w3dWebClipboardInstall, (), {
    if (Module.__w3dClipboard) return;
    Module.__w3dClipboard = true;
    var isField = function (el) {   // un campo de texto de la pagina: ahi pegar y copiar son del navegador
        return el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable);
    };
    // (en la fase de CAPTURA de la ventana: antes que SDL, que le haria preventDefault y el 'paste' no saldria)
    window.addEventListener('keydown', function (e) {
        if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === 'v' || e.key === 'V') && !isField(e.target))
            e.stopImmediatePropagation();
    }, true);
    document.addEventListener('paste', function (e) {
        if (isField(e.target)) return;
        var t = '';
        try { t = (e.clipboardData || window.clipboardData).getData('text') || ''; } catch (err) {}
        e.preventDefault();
        var buf = Module['_w3dWebPasteBuffer'] || (typeof _w3dWebPasteBuffer === 'function' ? _w3dWebPasteBuffer : null);
        var paste = Module['_w3dWebPaste'] || (typeof _w3dWebPaste === 'function' ? _w3dWebPaste : null);
        if (!buf || !paste) return;
        var bytes = new TextEncoder().encode(t);
        var p = buf(bytes.length + 1);
        HEAPU8.set(bytes, p);
        HEAPU8[p + bytes.length] = 0;
        paste();
    });
});
EM_JS(void, w3dWebCopy, (const char* p, int n), {
    try {
        var t = new TextDecoder().decode(HEAPU8.slice(p, p + n));
        if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(t).catch(function () {});
    } catch (err) {}
});
// se instala al arrancar (antes que la ventana de SDL: su escucha del teclado queda despues de la nuestra)
struct W3dClipboardWeb { W3dClipboardWeb() { w3dWebClipboardInstall(); } };
static W3dClipboardWeb gW3dClipboardWeb;
#endif

namespace w3dEngine {

void W3dClipboardSet(const std::string& s) {
    SDL_SetClipboardText(s.c_str());
#ifdef __EMSCRIPTEN__
    w3dWebCopy(s.c_str(), (int)s.size());   // (y al del navegador: copiar en el IDE llega afuera)
#endif
}

std::string W3dClipboardGet() {
    std::string r;
    char* c = SDL_GetClipboardText();   // SDL siempre devuelve una cadena (posiblemente vacia) que hay que liberar
    if (c) { r = c; SDL_free(c); }
    return r;
}

} // namespace w3dEngine

#endif // !W3D_SYMBIAN
