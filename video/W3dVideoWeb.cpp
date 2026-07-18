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
//   - desde BYTES en memoria (W3dVideoOpenMemoryBackend): para assets PROTEGIDOS,
//     los bytes salen descifrados de un .w3dpack. Se arma un Blob INTERNO, el
//     <video> NO se agrega al DOM (no aparece en el inspector) y la URL del blob se
//     REVOCA al cargar. No queda ningun archivo con nombre ni URL plana que copiar.
//
//  Interop GL: la textura se crea en C (glGenTextures) y el JS la bindea via
//  GL.textures[id]. Asi el id que ve el motor es un GLuint normal.
// ============================================================================
#if defined(W3D_ENABLE_VIDEO) && defined(__EMSCRIPTEN__)

#include "W3dVideo.h"
#include <emscripten.h>
#include <GLES2/gl2.h>

// --- glue JS: pool de <video> indexado por handle entero ---

// desde una URL (archivo servido).
EM_JS(int, w3dVideoJS_OpenUrl, (const char* pathPtr, int loop, unsigned int texId), {
    if (!Module.__w3dVideos) Module.__w3dVideos = [];
    var v = document.createElement('video');
    v.src = UTF8ToString(pathPtr);
    v.loop = !!loop; v.muted = true; v.autoplay = true; v.playsInline = true; v.crossOrigin = 'anonymous';
    var tryPlay = function(){ var p = v.play(); if (p && p.catch) p.catch(function(){}); };
    v.addEventListener('canplay', tryPlay); tryPlay();
    Module.__w3dVideos.push({ video: v, tex: texId, w: 0, h: 0 });
    return Module.__w3dVideos.length - 1;
});

// desde BYTES en memoria (assets protegidos): Blob interno + <video> fuera del DOM + revoke.
EM_JS(int, w3dVideoJS_OpenMem, (const unsigned char* ptr, int len, const char* mimePtr, int loop, unsigned int texId), {
    if (!Module.__w3dVideos) Module.__w3dVideos = [];
    var bytes = HEAPU8.slice(ptr, ptr + len);              // copia a un buffer JS (los del wasm pueden moverse)
    var blob = new Blob([bytes], { type: UTF8ToString(mimePtr) });
    var url = URL.createObjectURL(blob);
    var v = document.createElement('video');               // NO se hace appendChild: no esta en el DOM inspeccionable
    v.src = url;
    v.loop = !!loop; v.muted = true; v.autoplay = true; v.playsInline = true;
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
    GLctx.pixelStorei(GLctx.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA, GLctx.UNSIGNED_BYTE, v);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_MIN_FILTER, GLctx.LINEAR);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_MAG_FILTER, GLctx.LINEAR);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_WRAP_S, GLctx.CLAMP_TO_EDGE);
    GLctx.texParameteri(GLctx.TEXTURE_2D, GLctx.TEXTURE_WRAP_T, GLctx.CLAMP_TO_EDGE);
    return 1;
});

EM_JS(int, w3dVideoJS_W, (int h), { return Module.__w3dVideos[h].w; });
EM_JS(int, w3dVideoJS_H, (int h), { return Module.__w3dVideos[h].h; });

namespace w3dEngine {

class W3dVideoWeb : public W3dVideo {
public:
    W3dVideoWeb(int hnd, unsigned int t) : handle(hnd), tex(t), w(0), h(0) {}
    ~W3dVideoWeb() { if (tex) glDeleteTextures(1, &tex); }

    // El navegador maneja el reloj de reproduccion; solo subimos el frame vigente.
    bool Update(double /*nowMs*/) {
        if (handle < 0) return false;
        int changed = w3dVideoJS_Update(handle);
        if (changed) { w = w3dVideoJS_W(handle); h = w3dVideoJS_H(handle); }
        return changed != 0;
    }
    unsigned int Texture() const { return tex; }
    int  Width()    const { return w; }
    int  Height()   const { return h; }
    bool HasAlpha() const { return true; } // el WebM puede traer alpha; el blend del motor lo respeta

private:
    int handle;
    unsigned int tex;
    int w, h;
};

W3dVideo* W3dVideoOpenBackend(const char* path, bool loop) {
    unsigned int tex = 0; glGenTextures(1, &tex);
    int hnd = w3dVideoJS_OpenUrl(path, loop ? 1 : 0, tex);
    return new W3dVideoWeb(hnd, tex);
}

W3dVideo* W3dVideoOpenMemoryBackend(const void* bytes, size_t len, const char* mime, bool loop) {
    unsigned int tex = 0; glGenTextures(1, &tex);
    int hnd = w3dVideoJS_OpenMem((const unsigned char*)bytes, (int)len, mime, loop ? 1 : 0, tex);
    return new W3dVideoWeb(hnd, tex);
}

} // namespace w3dEngine

#endif // W3D_ENABLE_VIDEO && __EMSCRIPTEN__
