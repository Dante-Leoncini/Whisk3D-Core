// ============================================================================
//  Backend WEB de video (emscripten). El NAVEGADOR decodifica por HARDWARE.
//
//  Metemos el archivo en un elemento <video> oculto: el navegador lo decodifica
//  (VP9/AV1/H.264, con alpha si el WebM lo trae) y lo reproduce en loop por su
//  cuenta. Cada frame lo subimos a una textura GL con texImage2D(video): no
//  copiamos pixeles a la CPU, el trabajo pesado lo hace el navegador.
//
//  Dos formas de abrir:
//   - desde una URL/archivo (W3dVideoOpenBackend).
//   - desde BYTES en memoria (W3dVideoOpenMemoryBackend), por ejemplo salidos de
//     un .w3dpack. Se arma un Blob INTERNO, el <video> NO se agrega al DOM y la URL
//     del blob se REVOCA apenas carga: no queda una URL viva colgando.
//
//  Interop GL: la textura se crea en C (glGenTextures) y el JS la bindea via
//  GL.textures[id]. Asi el id que ve el motor es un GLuint normal.
// ============================================================================
#if defined(W3D_ENABLE_VIDEO) && defined(__EMSCRIPTEN__)

#include "W3dVideo.h"
#include "W3dVideoBackend.h"   // contrato con el dispatcher: firma verificada al compilar
#include <emscripten.h>
#include <GLES2/gl2.h>

// --- helpers JS ---
// iOS/Safari es MUY quisquilloso con el autoplay de video. Tres cosas que si o si:
//   1) el atributo muted TIENE que estar puesto ANTES del src (no alcanza v.muted=true),
//   2) playsinline (y el webkit- viejo) o intenta abrir el reproductor a pantalla completa,
//   3) el <video> tiene que estar EN EL DOM: fuera del documento iOS no decodifica.
//      Lo metemos de 1x1 px, invisible y sin eventos: no se ve ni se puede tocar.
EM_JS(void, w3dVideoJS_Prep, (int idx, int conSonido), {
    var v = Module.__w3dVideos[idx].video;
    if (!conSonido) { v.setAttribute('muted', ''); v.muted = true; }
    else             { v.removeAttribute('muted'); v.muted = false; v.volume = 1.0; }
    v.setAttribute('playsinline', '');
    v.setAttribute('webkit-playsinline', '');
    v.setAttribute('autoplay', '');
    v.playsInline = true; v.autoplay = true; v.controls = false;
    v.style.cssText = 'position:fixed;left:0;top:0;width:1px;height:1px;opacity:0.001;' +
                      'pointer-events:none;z-index:-1;';
    if (!v.parentNode) document.body.appendChild(v);
});

// DESBLOQUEO: "estrena" un <video> dentro del gesto del usuario y lo deja reservado, para
// que el video que todavia se esta descargando pueda reproducirse cuando llegue (mismo
// truco que la musica, ver audio/W3dMusic.h).
EM_JS(void, w3dVideoJS_Unlock, (), {
    if (Module.__w3dVideoPrimed) return;
    var v = document.createElement('video');
    v.setAttribute('muted', ''); v.setAttribute('playsinline', '');
    v.setAttribute('webkit-playsinline', ''); v.setAttribute('autoplay', '');
    v.muted = true; v.playsInline = true; v.autoplay = true; v.controls = false;
    v.style.cssText = 'position:fixed;left:0;top:0;width:1px;height:1px;opacity:0.001;' +
                      'pointer-events:none;z-index:-1;';
    document.body.appendChild(v);
    var p = v.play(); if (p && p.catch) p.catch(function(){});
    Module.__w3dVideoPrimed = v;
});

// --- glue JS: pool de <video> indexado por handle entero ---

// desde una URL (archivo servido).
EM_JS(int, w3dVideoJS_OpenUrl, (const char* pathPtr, int loop, unsigned int texId), {
    if (!Module.__w3dVideos) Module.__w3dVideos = [];
    var v = Module.__w3dVideoPrimed || document.createElement('video');
    Module.__w3dVideoPrimed = null;
    v.src = UTF8ToString(pathPtr);
    v.loop = !!loop; v.muted = true; v.autoplay = true; v.playsInline = true; v.crossOrigin = 'anonymous';
    var tryPlay = function(){ var p = v.play(); if (p && p.catch) p.catch(function(){}); };
    v.addEventListener('canplay', tryPlay); tryPlay();
    Module.__w3dVideos.push({ video: v, tex: texId, w: 0, h: 0 });
    return Module.__w3dVideos.length - 1;
});

// desde BYTES en memoria: Blob interno + <video> fuera del DOM + revoke al cargar.
EM_JS(int, w3dVideoJS_OpenMem, (const unsigned char* ptr, int len, const char* mimePtr, int loop, unsigned int texId, int conSonido), {
    if (!Module.__w3dVideos) Module.__w3dVideos = [];
    var bytes = HEAPU8.slice(ptr, ptr + len);              // copia a un buffer JS (los del wasm pueden moverse)
    var blob = new Blob([bytes], { type: UTF8ToString(mimePtr) });
    var url = URL.createObjectURL(blob);
    var v = Module.__w3dVideoPrimed || document.createElement('video');   // si hay uno ESTRENADO, lo reuso
    Module.__w3dVideoPrimed = null;
    v.src = url;
    v.loop = !!loop; v.muted = !conSonido; v.autoplay = true; v.playsInline = true;
    v.volume = 1.0;
    v.addEventListener('loadeddata', function(){ URL.revokeObjectURL(url); }); // el blob deja de resolverse por URL
    var tryPlay = function(){ var p = v.play(); if (p && p.catch) p.catch(function(){}); };
    v.addEventListener('canplay', tryPlay); tryPlay();
    Module.__w3dVideos.push({ video: v, tex: texId, w: 0, h: 0 });
    return Module.__w3dVideos.length - 1;
});

// Sube el frame actual a la textura si el <video> tiene datos. Devuelve 1 si subio frame.
EM_JS(int, w3dVideoJS_Update, (int h), {
    var r = Module.__w3dVideos[h];
    var v = r.video;
    if (v.readyState < 2) { var p = v.play(); if (p && p.catch) p.catch(function(){}); return 0; } // HAVE_CURRENT_DATA
    r.w = v.videoWidth; r.h = v.videoHeight;
    GLctx.bindTexture(GLctx.TEXTURE_2D, GL.textures[r.tex]);
    GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, true);
    // PREMULTIPLICADO: asi lo entrega el decodificador de video con alpha. Pidiendolo sin
    // premultiplicar, Safari pierde el canal alpha (el video sale opaco). Al dibujar hay
    // que usar la mezcla premultiplicada (ONE, ONE_MINUS_SRC_ALPHA).
    GLctx.pixelStorei(GLctx.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA, GLctx.UNSIGNED_BYTE, v);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_MIN_FILTER, GLctx.LINEAR);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_MAG_FILTER, GLctx.LINEAR);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_WRAP_S, GLctx.CLAMP_TO_EDGE);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_WRAP_T, GLctx.CLAMP_TO_EDGE);
    // MUY IMPORTANTE: UNPACK_FLIP_Y_WEBGL es estado GLOBAL del contexto. Si lo dejamos
    // prendido, TODA textura que se suba despues (logo, marco, simbolos, botones del
    // siguiente juego...) queda DADA VUELTA. Se apaga siempre al terminar.
    GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, false);
    GLctx.pixelStorei(GLctx.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);   // estado global: se restaura
    return 1;
});

EM_JS(void, w3dVideoJS_Close, (int h), {
    var r = Module.__w3dVideos && Module.__w3dVideos[h];
    if (!r) return;
    var v = r.video;
    try { v.pause(); v.removeAttribute('src'); v.load(); } catch (e) {}
    if (v.parentNode) v.parentNode.removeChild(v);
    Module.__w3dVideos[h] = null;
});

EM_JS(int, w3dVideoJS_Fin, (int h), {
    var r = Module.__w3dVideos && Module.__w3dVideos[h];
    return (r && r.video && r.video.ended) ? 1 : 0;
});

EM_JS(int, w3dVideoJS_W, (int h), { return Module.__w3dVideos[h].w; });
EM_JS(int, w3dVideoJS_H, (int h), { return Module.__w3dVideos[h].h; });

namespace w3dEngine {

class W3dVideoWeb : public W3dVideo {
public:
    W3dVideoWeb(int hnd, unsigned int t) : handle(hnd), tex(t), w(0), h(0) {}
    ~W3dVideoWeb() { if (handle >= 0) w3dVideoJS_Close(handle); if (tex) glDeleteTextures(1, &tex); }

    // El navegador maneja el reloj de reproduccion; solo subimos el frame vigente.
    bool Update(double /*nowMs*/) {
        if (handle < 0) return false;
        int changed = w3dVideoJS_Update(handle);
        if (changed) { w = w3dVideoJS_W(handle); h = w3dVideoJS_H(handle); }
        return changed != 0;
    }
    unsigned int Texture() const { return tex; }
    bool Terminado() const { return handle >= 0 && w3dVideoJS_Fin(handle) != 0; }
    bool Premultiplicado() const { return true; }
    int  Width()    const { return w; }
    int  Height()   const { return h; }
    bool HasAlpha() const { return true; } // el WebM puede traer alpha; el blend del motor lo respeta

private:
    int handle;
    unsigned int tex;
    int w, h;
};

void W3dVideoUnlockBackend() { w3dVideoJS_Unlock(); }

W3dVideo* W3dVideoOpenBackend(const char* path, bool loop) {
    unsigned int tex = 0; glGenTextures(1, &tex);
    int hnd = w3dVideoJS_OpenUrl(path, loop ? 1 : 0, tex);
    if (hnd >= 0) w3dVideoJS_Prep(hnd, 0);
    return new W3dVideoWeb(hnd, tex);
}

W3dVideo* W3dVideoOpenMemoryBackend(const void* bytes, size_t len, const char* mime, bool loop, bool conSonido) {
    unsigned int tex = 0; glGenTextures(1, &tex);
    int hnd = w3dVideoJS_OpenMem((const unsigned char*)bytes, (int)len, mime, loop ? 1 : 0, tex, conSonido ? 1 : 0);
    if (hnd >= 0) w3dVideoJS_Prep(hnd, conSonido ? 1 : 0);
    return new W3dVideoWeb(hnd, tex);
}

} // namespace w3dEngine

#endif // W3D_ENABLE_VIDEO && __EMSCRIPTEN__
