// ============================================================================
//  w3dSafeArea.cpp — medicion del AREA SEGURA por plataforma. Ver w3dSafeArea.h.
//
//  WEB (emscripten): dos fuentes, se toma la MAYOR de cada borde.
//    1) env(safe-area-inset-*) del CSS -> notch, esquinas redondeadas, home indicator.
//       (Requiere viewport-fit=cover en el <meta viewport>, si no da 0.)
//    2) visualViewport -> cuanto TAPA la barra de herramientas del navegador, que se
//       SUPERPONE al viewport de 100vh y aparece/desaparece al scrollear. Esto NO lo
//       reporta env(safe-area-inset-bottom): por eso hacen falta las dos.
//
//  Resto de plataformas: insets en 0 (toda la ventana es dibujable). Un host nativo
//  iOS/Android puede llamar SafeAreaSet() con los insets que ya conoce.
// ============================================================================
#include "w3dSafeArea.h"

#ifdef __EMSCRIPTEN__
    #include <emscripten.h>

// insets del CSS (notch / esquinas / home indicator), en px CSS. Es la parte "cara"
// (crea un nodo), asi que el caller no deberia llamarla a 60fps sin necesidad.
EM_JS(double, w3dSA_env, (int borde), {
    var props = ['top','bottom','left','right'];
    var d = document.createElement('div');
    d.style.cssText = 'position:fixed;visibility:hidden;pointer-events:none;width:0;height:0;' +
                      'padding-' + props[borde] + ':env(safe-area-inset-' + props[borde] + ');';
    document.body.appendChild(d);
    var v = parseFloat(getComputedStyle(d)['padding' + props[borde].charAt(0).toUpperCase() + props[borde].slice(1)]) || 0;
    document.body.removeChild(d);
    return v;
});

// cuanto del canvas tapa la barra del navegador ARRIBA y ABAJO (px CSS). Barato.
EM_JS(double, w3dSA_occ, (int abajo), {
    var vv = window.visualViewport; if (!vv) return 0;
    var c = document.getElementById('canvas');
    var r = c ? c.getBoundingClientRect() : null;
    var alto = r ? r.height : window.innerHeight;
    var top0 = r ? r.top : 0;
    var occ = abajo ? (alto - (vv.offsetTop - top0 + vv.height)) : (vv.offsetTop - top0);
    return (occ > 0) ? occ : 0;
});
// color de las franjas que el navegador pinta fuera del area dibujable (barra de estado
// arriba, barra de herramientas abajo): se las damos como degradado del fondo de la pagina.
// imagen de fondo para las franjas: se arma un Blob UNA vez (se recuerda por el puntero) y se
// pinta como fondo del elemento raiz con el encuadre exacto que usa el motor.
EM_JS(void, w3dSA_imagen, (const unsigned char* ptr, int len, const char* mimePtr,
                           float w, float h, float x, float y), {
    var raiz = document.documentElement;
    if (!Module.__w3dFondoLen || Module.__w3dFondoLen !== len || Module.__w3dFondoPtr !== ptr) {
        var blob = new Blob([HEAPU8.slice(ptr, ptr + len)], { type: UTF8ToString(mimePtr) });
        if (Module.__w3dFondoUrl) { try { URL.revokeObjectURL(Module.__w3dFondoUrl); } catch (e) {} }
        Module.__w3dFondoUrl = URL.createObjectURL(blob);
        Module.__w3dFondoLen = len; Module.__w3dFondoPtr = ptr;
        // la IMAGEN va arriba y el DEGRADADO debajo: si el navegador no propaga la imagen a las
        // franjas (pero si el degradado), igual se ve algo parecido a la escena.
        var grad = Module.__w3dFondoGrad || '';
        raiz.style.backgroundImage = 'url(' + Module.__w3dFondoUrl + ')' + (grad ? ', ' + grad : '');
        raiz.style.backgroundRepeat = 'no-repeat, no-repeat';
    }
    raiz.style.backgroundSize = w.toFixed(1) + 'px ' + h.toFixed(1) + 'px, cover';
    raiz.style.backgroundPosition = x.toFixed(1) + 'px ' + y.toFixed(1) + 'px, center';
    document.body.style.background = 'transparent';
});

// degradado de N paradas (cada una r,g,b): se reparten de arriba hacia abajo
EM_JS(void, w3dSA_grad, (const unsigned char* rgb, int n), {
    if (n < 2) return;
    var partes = [];
    for (var i = 0; i < n; i++) {
        var r = HEAPU8[rgb + i*3], g = HEAPU8[rgb + i*3 + 1], b = HEAPU8[rgb + i*3 + 2];
        var pos = (i * 100 / (n - 1)).toFixed(1);
        partes.push('rgb(' + r + ',' + g + ',' + b + ') ' + pos + '%');
    }
    var raiz = document.documentElement;
    raiz.style.backgroundColor = 'rgb(' + HEAPU8[rgb] + ',' + HEAPU8[rgb+1] + ',' + HEAPU8[rgb+2] + ')';
    var css = 'linear-gradient(' + partes.join(',') + ')';
    Module.__w3dFondoGrad = css;   // respaldo debajo de la imagen (w3dSA_imagen lo compone)
    raiz.style.backgroundImage = css;
    document.body.style.background = 'transparent';
    // theme-color: es lo UNICO que tiñe la barra del navegador en iOS. Se usa el color del borde
    // de ABAJO de la escena, que es justo donde va esa barra.
    var u = (n - 1) * 3;
    var abajo = 'rgb(' + HEAPU8[rgb+u] + ',' + HEAPU8[rgb+u+1] + ',' + HEAPU8[rgb+u+2] + ')';
    var m = document.querySelector('meta[name=theme-color]');
    if (!m) { m = document.createElement('meta'); m.name = 'theme-color'; document.head.appendChild(m); }
    m.content = abajo;
    // y el COLOR del raiz (lo unico que el navegador propaga a las franjas: la imagen NO la
    // propaga) se deja en ese mismo tono, para que la union se note lo menos posible.
    raiz.style.backgroundColor = abajo;
});

#endif

namespace w3dEngine {

void SafeAreaSetImagen(const void* bytes, int len, const char* mime,
                       float w, float h, float x, float y) {
#ifdef __EMSCRIPTEN__
    if (bytes && len > 0) w3dSA_imagen((const unsigned char*)bytes, len, mime ? mime : "image/jpeg", w, h, x, y);
#else
    (void)bytes; (void)len; (void)mime; (void)w; (void)h; (void)x; (void)y;
#endif
}

void SafeAreaSetDegradado(const unsigned char* rgb, int n) {
#ifdef __EMSCRIPTEN__
    if (rgb && n >= 2) w3dSA_grad(rgb, n);
#else
    (void)rgb; (void)n;
#endif
}


static SafeArea gSA = { 0.0f, 0.0f, 0.0f, 0.0f };

void SafeAreaSet(float top, float bottom, float left, float right) {
    gSA.top = top; gSA.bottom = bottom; gSA.left = left; gSA.right = right;
}

const SafeArea& SafeAreaGet() { return gSA; }

void SafeAreaUpdate(float dpr) {
#ifdef __EMSCRIPTEN__
    if (dpr <= 0.0f) dpr = 1.0f;
    // los insets del CSS casi no cambian: los remido cada tanto, no en cada frame.
    static int  n = 0;
    static float envT = 0, envB = 0, envL = 0, envR = 0;
    if ((n++ % 30) == 0) {
        envT = (float)w3dSA_env(0); envB = (float)w3dSA_env(1);
        envL = (float)w3dSA_env(2); envR = (float)w3dSA_env(3);
    }
    // la barra del navegador SI cambia seguido (aparece/desaparece): se mide siempre.
    float occT = (float)w3dSA_occ(0), occB = (float)w3dSA_occ(1);
    float t = (occT > envT) ? occT : envT;
    float b = (occB > envB) ? occB : envB;
    gSA.top = t * dpr; gSA.bottom = b * dpr;
    gSA.left = envL * dpr; gSA.right = envR * dpr;
#else
    (void)dpr;   // PC / Symbian / consolas: toda la ventana es dibujable
#endif
}

void SafeRect(int w, int h, float* x, float* y, float* sw, float* sh) {
    float x0 = gSA.left, y0 = gSA.top;
    float x1 = (float)w - gSA.right, y1 = (float)h - gSA.bottom;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    if (x)  *x  = x0;
    if (y)  *y  = y0;
    if (sw) *sw = x1 - x0;
    if (sh) *sh = y1 - y0;
}

} // namespace w3dEngine
