// ============================================================================
//  W3dMusicWeb.cpp — backend WEB (emscripten) de MUSICA. Solo con -DW3D_ENABLE_MUSIC.
//
//  El NAVEGADOR decodifica (mp3/ogg/aac, normalmente por hardware): metemos los bytes
//  en un elemento <audio> y el reproduce en loop por su cuenta. No se decodifica a PCM
//  ni se mezcla en la CPU -> un tema de varios minutos no cuesta RAM ni tamanio de asset.
//
//  DESDE MEMORIA: cuando los bytes ya estan en RAM (por ejemplo salidos de un
//  .w3dpack) se arma un Blob INTERNO, el <audio> NO se agrega al DOM y la URL del
//  blob se REVOCA apenas carga: no queda una URL viva colgando.
//
//  Autoplay: los navegadores exigen un gesto del usuario. Llamalo desde el toque/click
//  (o despues de uno); si lo bloquean, el play() falla en silencio y la app sigue igual.
// ============================================================================
#ifdef W3D_ENABLE_MUSIC

#include "W3dMusic.h"
#include "W3dMusicBackend.h"   // contrato con el dispatcher: firma verificada al compilar
#include <emscripten.h>

// --- glue JS: pool de <audio> indexado por handle entero ---
// DESBLOQUEO: estrena un <audio> con un wav de silencio DENTRO del gesto del usuario y lo
// deja reservado. Un elemento ya estrenado puede cambiar de src y volver a sonar SIN gesto:
// asi la musica que todavia se estaba descargando suena igual cuando llega (iOS/Safari).
// Ademas de estrenar el <audio>, RE-ACTIVA el AudioContext de WebAudio (el que usa el mixer
// de efectos por SDL). En iOS nace SUSPENDIDO y solo se puede reanudar DENTRO de un gesto del
// usuario: si no, los efectos (ej: el clic al pasar de portada) no suenan nunca.
EM_JS(void, w3dMusicJS_Unlock, (), {
    // SOLO registra la funcion que estrena el <audio>; NO lo estrena aca, porque esto se llama al
    // arrancar (fuera de cualquier gesto) y ese play() lo bloquea el navegador. Quien la llama de
    // verdad es el listener de gesto de W3dClip (touchstart/click sobre el documento).
    // (Antes se creaba el elemento aca igual: quedaba "reservado" pero NUNCA estrenado, y despues
    //  el estreno del gesto lo salteaba por ya existir -> en iOS la musica no sonaba.)
    Module.__w3dPrimeMusic = function () {
        var a = Module.__w3dPrimed;
        if (!a) {
            a = new Audio();
            a.setAttribute('playsinline', '');
            a.preload = 'auto';
            Module.__w3dPrimed = a;
        }
        if (a.__w3dOk) return;                       // ya esta estrenado
        a.src = 'data:audio/wav;base64,UklGRiwAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQgAAACAgICAgICAgA==';
        a.__w3dOk = true;                // optimista: marcarlo en el .then llegaba tarde para la
        var p = a.play();                // primera pista, que arranca casi enseguida
        if (p && p.catch) p.catch(function () { a.__w3dOk = false; });
    };
    // si el gesto del usuario ya paso, se estrena en el acto
    if (Module.__w3dGestureOk) Module.__w3dPrimeMusic();
});

// MUSICA POR WEBAUDIO, no por un elemento <audio>.
// Por que: en iPhone el <audio> con una URL de blob NUNCA llego a reproducir. El registro del
// telefono lo mostro sin lugar a dudas: la pista cargaba entera (readyState 4), no estaba
// pausada, y aun asi currentTime se quedaba en 0 y terminaba en error 3 (fallo de
// decodificacion), una y otra vez. Safari no decodifica ese mp3 desde blob:. En cambio los
// EFECTOS de sonido, que ya iban por WebAudio, funcionaron siempre en el mismo aparato: ese es
// el camino que sirve. Se decodifica una vez a un AudioBuffer y se reproduce en bucle con un
// GainNode para el volumen (el .volume de un <audio> tampoco se respeta en iOS).
EM_JS(int, w3dMusicJS_OpenMem, (const unsigned char* ptr, int len, const char* mimePtr, int loop, float vol), {
    if (!Module.__w3dMusic) Module.__w3dMusic = [];
    var C = window.AudioContext || window.webkitAudioContext;
    if (!C) return -1;
    if (!Module.__w3dCtx) {
        try { Module.__w3dCtx = new C(); } catch (e) { return -1; }
        try { if (navigator.audioSession) navigator.audioSession.type = 'playback'; } catch (e) {}
    }
    var ctx = Module.__w3dCtx;
    var i = Module.__w3dMusic.length;
    // 'quiere' = deberia estar sonando. Se decodifica en segundo plano; cuando el buffer llega,
    // arranca solo (o no, si mientras tanto lo pausaron o lo liberaron).
    var m = { buf: null, src: null, gain: null, vol: vol, loop: !!loop,
              wants: true, t0: 0, offset: 0, ctx: ctx };
    Module.__w3dMusic.push(m);

    m.start = function () {
        if (!m.buf || !m.wants || m.src) return;
        try {
            if (ctx.state !== 'running' && ctx.resume) { try { ctx.resume(); } catch (e) {} }
            var s = ctx.createBufferSource(); s.buffer = m.buf; s.loop = m.loop;
            var g = ctx.createGain(); g.gain.value = m.vol;
            s.connect(g); g.connect(ctx.destination);
            s.start(0, m.offset % (m.buf.duration || 1));
            m.src = s; m.gain = g; m.t0 = ctx.currentTime - m.offset;
        } catch (e) {}
    };

    var data = HEAPU8.slice(ptr, ptr + len).buffer;   // copia propia: decodeAudioData la consume
    try {
        ctx.decodeAudioData(data,
            function (b) { m.buf = b; m.start(); },
            function ()  { console.log('[musica] no pude decodificar la pista'); });
    } catch (e) {}
    return i;
});

EM_JS(void, w3dMusicJS_Stop, (int h), {
    var m = Module.__w3dMusic && Module.__w3dMusic[h];
    if (!m) return;
    m.wants = false;
    try { if (m.src) { m.src.stop(0); m.src.disconnect(); } } catch (e) {}
    m.src = null; m.buf = null;                 // suelta el PCM decodificado (son varios MB)
    Module.__w3dMusic[h] = null;
});

// OJO iOS: en iPhone/iPad la propiedad .volume de un <audio> es de SOLO LECTURA (el volumen
// lo maneja el usuario con los botones del aparato): asignarla NO hace nada y la musica se
// escucha a todo volumen. Lo que SI se respeta es .muted -> el silencio se hace con muted, y
// el volumen fino queda para las plataformas que lo permiten (desktop/Android).
EM_JS(void, w3dMusicJS_Vol, (int h, float v), {
    var m = Module.__w3dMusic && Module.__w3dMusic[h];
    if (!m) return;
    m.vol = v;
    // El GainNode SI funciona en iOS (el .volume de un <audio> se ignora).
    if (m.gain) { try { m.gain.gain.value = v; } catch (e) {} }
});

EM_JS(void, w3dMusicJS_Pause, (int h, int pause), {
    var m = Module.__w3dMusic && Module.__w3dMusic[h];
    if (!m) return;
    if (pause) {
        m.wants = false;
        if (m.src) {
            // Un BufferSource no se puede repausar: se guarda POR DONDE IBA y al reanudar se
            // crea uno nuevo que arranca desde ahi.
            try { m.offset = m.ctx.currentTime - m.t0; m.src.stop(0); m.src.disconnect(); } catch (e) {}
            m.src = null;
        }
    } else {
        m.wants = true;
        if (m.start) m.start();
    }
});

EM_JS(int, w3dMusicJS_Playing, (int h), {
    var m = Module.__w3dMusic && Module.__w3dMusic[h];
    return (m && m.src) ? 1 : 0;
});


namespace w3dEngine {

class W3dMusicWeb : public W3dMusic {
public:
    int h;
    explicit W3dMusicWeb(int handle) : h(handle) {}
    virtual ~W3dMusicWeb() { Stop(); }
    virtual void Stop() { if (h >= 0) { w3dMusicJS_Stop(h); h = -1; } }
    virtual void ApplyVolume(float v) { if (h >= 0) w3dMusicJS_Vol(h, v); }
    virtual bool Playing() const { return h >= 0 && w3dMusicJS_Playing(h) != 0; }
    virtual void Pause()   { if (h >= 0) w3dMusicJS_Pause(h, 1); }
    virtual void Resume() { if (h >= 0) w3dMusicJS_Pause(h, 0); }
};

W3dMusic* W3dMusicPlayMemoryBackend(const void* bytes, size_t len, const char* mime, bool loop, float volume) {
    int h = w3dMusicJS_OpenMem((const unsigned char*)bytes, (int)len,
                               mime ? mime : "audio/mpeg", loop ? 1 : 0, volume);
    return (h < 0) ? 0 : new W3dMusicWeb(h);
}
void W3dMusicUnlockBackend() { w3dMusicJS_Unlock(); }

W3dMusic* W3dMusicPlayBackend(const char* path, bool loop, float volume) {
    // en web NO hay musica por URL: el <audio> crudo no convive con el pool WebAudio (Stop no
    // pausaba, y en iOS .volume es solo-lectura). Todo entra por PlayMemory (bytes del pack).
    (void)path; (void)loop; (void)volume;
    return 0;
}

} // namespace w3dEngine

#endif // W3D_ENABLE_MUSIC
