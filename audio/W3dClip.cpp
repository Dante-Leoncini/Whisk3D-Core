// ============================================================================
//  W3dClip.cpp — sonidos cortos de interfaz. Ver W3dClip.h.
//
//  WEB: el clip se DECODIFICA UNA VEZ a un AudioBuffer y cada disparo es un
//       BufferSource + start(): microsegundos, sin tocar el hilo principal. Nada de
//       elementos <audio> (en iOS cada play() arranca el pipeline de media y con
//       varios seguidos el telefono se traba) ni de ScriptProcessorNode. Instala un
//       enganche en el PRIMER gesto del usuario para reanudar el AudioContext.
//  RESTO: es un sonido del mixer PCM del Core (W3dAudio.h).
// ============================================================================
#include "W3dClip.h"
#include "W3dVolumen.h"

#ifdef __EMSCRIPTEN__
    #include <emscripten.h>

// Un clip = un AudioBuffer ya decodificado. Dispararlo es crear un BufferSource y start():
// microsegundos, sin tocar el hilo principal. NO se usan elementos <audio>: en iOS cada play()
// de un <audio> arranca el pipeline de media y con varios seguidos (un fling del cover flow)
// el telefono se traba. Tampoco se usa el mixer por ScriptProcessorNode (deprecado y caro).
EM_JS(void, w3dClipJS_Ctx, (), {
    if (Module.__w3dCtx) return;
    var C = window.AudioContext || window.webkitAudioContext;
    if (!C) return;
    try { Module.__w3dCtx = new C(); } catch (e) { return; }
    // iOS >= 16.4: pide la categoria "playback" para que el audio NO lo silencie la palanca
    // de silencio del telefono (por defecto WebAudio queda mudo con el timbre en silencio).
    try { if (navigator.audioSession) navigator.audioSession.type = 'playback'; } catch (e) {}
});

EM_JS(int, w3dClipJS_Load, (const unsigned char* ptr, int len, const char* mimePtr), {
    if (!Module.__w3dClips) Module.__w3dClips = [];
    var ctx = Module.__w3dCtx;
    if (!ctx) return -1;
    var i = Module.__w3dClips.length;
    Module.__w3dClips.push({ buf: null });
    var datos = HEAPU8.slice(ptr, ptr + len).buffer;      // copia propia: decodeAudioData la consume
    try {
        ctx.decodeAudioData(datos,
            function (b) { Module.__w3dClips[i].buf = b; },
            function ()  { });                            // formato no soportado: queda mudo
    } catch (e) {}
    return i;
});

EM_JS(void, w3dClipJS_Play, (int h, float vol), {
    var c = Module.__w3dClips && Module.__w3dClips[h];
    var ctx = Module.__w3dCtx;
    if (!c || !c.buf || !ctx || vol <= 0.001) return;
    // OJO: resume() es ASINCRONO. El primer clip que se dispara EN el mismo gesto que desbloquea
    // el audio llegaba con el contexto todavia en 'suspended' y antes se descartaba (no se
    // escuchaba el primer sonido de la app). Ahora se pide el resume y se dispara igual: el
    // BufferSource arranca solo cuando el contexto se pone en marcha.
    if (ctx.state !== 'running' && ctx.resume) { try { ctx.resume(); } catch (e) {} }
    try {
        var s = ctx.createBufferSource(); s.buffer = c.buf;
        var g = ctx.createGain(); g.gain.value = vol;     // el gain SI funciona en iOS (.volume no)
        s.connect(g); g.connect(ctx.destination);
        s.start(0);
    } catch (e) {}
});

EM_JS(void, w3dClipJS_Free, (int h), {
    if (Module.__w3dClips && Module.__w3dClips[h]) Module.__w3dClips[h] = null;
});

// Enganche del PRIMER gesto: es lo unico que corre DENTRO del toque del usuario (los eventos
// del motor se procesan despues, en el frame, y para iOS eso ya no cuenta como gesto).
EM_JS(void, w3dClipJS_Desbloqueo, (), {
    if (Module.__w3dUnlockInstalado) return;
    Module.__w3dUnlockInstalado = true;
    var gesto = function () {
        Module.__w3dGestoOk = true;
        // estrena tambien el <audio> de la MUSICA (lo registra el backend de musica): un elemento
        // ya estrenado puede cambiar de src y sonar despues SIN gesto, que es lo que hace falta
        // cuando la musica todavia se esta descargando.
        try { if (Module.__w3dPrimeMusica) Module.__w3dPrimeMusica(); } catch (e) {}
        try { if (navigator.audioSession) navigator.audioSession.type = 'playback'; } catch (e) {}
        // DESPERTAR LA SESION DE AUDIO (iOS): mientras la pagina no haya reproducido ningun
        // medio, iOS no le activa la sesion y todo suena mudo aunque el AudioContext diga
        // 'running'. Se estrena EL ELEMENTO DE LA MUSICA (arriba, __w3dPrimeMusica): NO otro
        // aparte. iOS deja sonar UN solo <audio> a la vez, asi que un elemento silencioso propio
        // le ganaba la carrera al de la musica y la musica no sonaba hasta que algo mas (un
        // video) volvia a activar la sesion.
        var ctxs = [];
        if (Module.__w3dCtx) ctxs.push(Module.__w3dCtx);
        if (typeof SDL2 !== 'undefined' && SDL2.audioContext) ctxs.push(SDL2.audioContext);
        for (var i = 0; i < ctxs.length; i++) {
            try { if (ctxs[i].state !== 'running' && ctxs[i].resume) ctxs[i].resume(); } catch (e) {}
        }
        // y el desbloqueo clasico de WebAudio: disparar un buffer vacio DENTRO del gesto.
        try {
            var c0 = Module.__w3dCtx;
            if (c0 && !Module.__w3dCtxOk) {
                Module.__w3dCtxOk = true;
                var b = c0.createBuffer(1, 1, 22050);
                var f = c0.createBufferSource(); f.buffer = b;
                f.connect(c0.destination); f.start(0);
            }
        } catch (e) {}
    };
    var evs = ['touchstart', 'touchend', 'pointerdown', 'mousedown', 'click', 'keydown'];
    for (var k = 0; k < evs.length; k++)
        document.addEventListener(evs[k], gesto, { capture: true, passive: true });
});

namespace w3dEngine {

class W3dClip { public: int h; };

W3dClip* W3dClipLoad(const void* bytes, size_t len, const char* mime) {
    if (!bytes || !len) return 0;
    w3dClipJS_Ctx();                                      // crea el AudioContext si no estaba
    int h = w3dClipJS_Load((const unsigned char*)bytes, (int)len, mime ? mime : "audio/wav");
    if (h < 0) return 0;
    W3dClip* c = new W3dClip(); c->h = h; return c;
}
void W3dClipPlay(W3dClip* c, float vol) {
    if (c) w3dClipJS_Play(c->h, vol * VolumenGanancia());
}
void W3dClipFree(W3dClip* c) { if (c) { w3dClipJS_Free(c->h); delete c; } }
void W3dClipInstalarDesbloqueo() { w3dClipJS_Ctx(); w3dClipJS_Desbloqueo(); }

} // namespace w3dEngine

#else   // ---------- resto de plataformas: el mixer PCM del Core ----------

#include "W3dAudio.h"

namespace w3dEngine {

class W3dClip { public: W3dSound* s; };

W3dClip* W3dClipLoad(const void* bytes, size_t len, const char* /*mime*/) {
    W3dSound* s = W3dSoundLoadMemory(bytes, len);
    if (!s) return 0;
    W3dClip* c = new W3dClip(); c->s = s; return c;
}
void W3dClipPlay(W3dClip* c, float vol) {
    if (c && c->s) W3dSoundPlay(c->s, vol * VolumenGanancia(), false);
}
void W3dClipFree(W3dClip* c) { if (c) { W3dSoundFree(c->s); delete c; } }
void W3dClipInstalarDesbloqueo() {}   // sin navegador no hay nada que desbloquear

} // namespace w3dEngine

#endif
