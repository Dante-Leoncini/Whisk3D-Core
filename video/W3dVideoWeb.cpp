// ============================================================================
//  Backend WEB de video (emscripten). El NAVEGADOR decodifica por HARDWARE.
//
//  Metemos el archivo en un elemento <video> oculto: el navegador lo decodifica
//  (VP9/AV1/H.264, con alpha si el WebM lo trae) y lo reproduce en loop por su
//  cuenta. Cada frame lo subimos a una textura GL con texImage2D(video): no
//  copiamos pixeles a la CPU, el trabajo pesado lo hace el navegador.
//
//  Interop GL: la textura se crea en C (glGenTextures) y el JS la bindea via
//  GL.textures[id]. Asi el id que ve el motor es un GLuint normal.
// ============================================================================
#if defined(W3D_ENABLE_VIDEO) && defined(__EMSCRIPTEN__)

#include "W3dVideo.h"
#include <emscripten.h>
#include <GLES2/gl2.h>

// --- glue JS: pool de <video> indexado por handle entero ---

// Crea un <video> + arranca la reproduccion; 'texId' es la textura GL ya creada en C.
EM_JS(int, w3dVideoJS_Open, (const char* pathPtr, int loop, unsigned int texId), {
    if (!Module.__w3dVideos) Module.__w3dVideos = [];
    var path = UTF8ToString(pathPtr);
    var v = document.createElement('video');
    v.src = path;
    v.loop = !!loop;
    v.muted = true;          // autoplay sin gesto del usuario requiere muted
    v.autoplay = true;
    v.playsInline = true;
    v.crossOrigin = 'anonymous';
    // algunos navegadores exigen un gesto para arrancar; si falla, el loop de Update reintenta
    var tryPlay = function(){ var p = v.play(); if (p && p.catch) p.catch(function(){}); };
    v.addEventListener('canplay', tryPlay);
    tryPlay();
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
    GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, true);            // GL: origen abajo-izq
    GLctx.pixelStorei(GLctx.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false); // alpha recto (el blend lo maneja el motor)
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
    W3dVideoWeb(const char* path, bool loop) : handle(-1), tex(0), w(0), h(0) {
        glGenTextures(1, &tex);
        handle = w3dVideoJS_Open(path, loop ? 1 : 0, tex);
    }
    ~W3dVideoWeb() {
        if (tex) glDeleteTextures(1, &tex);
        // el <video> queda en el pool JS; para una demo en loop no hace falta liberarlo.
    }

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
    return new W3dVideoWeb(path, loop);
}

} // namespace w3dEngine

#endif // W3D_ENABLE_VIDEO && __EMSCRIPTEN__
