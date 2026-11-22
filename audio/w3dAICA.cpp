// Whisk3DCore — el sonido de la Dreamcast directo al AICA (ver w3dAICA.h)
#ifdef W3D_DREAMCAST
#include "w3dAICA.h"
#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include <kos/thread.h>
#include <kos/mutex.h>
#include <string.h>

namespace w3dAICA {

namespace {
inline int A255(float v) {
    const int i = (int)(v * 255.0f + 0.5f);
    return i < 0 ? 0 : (i > 255 ? 255 : i);
}

// un flujo: sus datos en la RAM principal, por donde va y cuando termina
struct Flow {
    snd_stream_hnd_t hnd;
    const uint8_t* data;
    uint32_t bytes, pos;
    bool loop, playing;
    uint64_t end_us;   // (sin lazo: cuando termina de sonar lo ultimo que se le paso)
};
Flow gFlows[FLOWS];
bool gStarted = false;
// el hilo que atiende los flujos y lo que comparte con el juego
mutex_t gMutex = MUTEX_INITIALIZER;
kthread_t* gThread = 0;
volatile bool gContinue = false;

// los efectos cargados (el handle de KallistiOS y su frecuencia, para el tono); Efecto = indice + 1
enum { EFFECTS = 256 };
struct Loaded { sfxhnd_t hnd; uint32_t hz; };
Loaded gEffects[EFFECTS];

// el AICA pide 'pedidos' bytes (de los dos canales juntos si es estereo): se le da lo que sigue, sin pasar el final
// (lo que falte lo pide en la vuelta siguiente: con lazo, desde el principio)
void* RequestData(snd_stream_hnd_t hnd, int requests, int* dados) {
    for (Flow& f : gFlows) {
        if (f.hnd != hnd || !f.data) continue;
        if (f.pos >= f.bytes) {
            if (!f.loop) { *dados = 0; return 0; }
            f.pos = 0;
        }
        uint32_t n = f.bytes - f.pos;
        if (n > (uint32_t)requests) n = (uint32_t)requests;
        const uint8_t* p = f.data + f.pos;
        f.pos += n;
        *dados = (int)n;
        return (void*)p;
    }
    *dados = 0;
    return 0;
}

void HandleFlows();

// el hilo: cada 50 ms, los flujos (si espera un DMA, espera el: el juego sigue). Mas seguido no hace falta (el buffer
// de un flujo dura mas de un segundo) y cuesta: cada vuelta lee la posicion de los canales por el bus del AICA
void* Thread(void*) {
    while (gContinue) {
        mutex_lock(&gMutex);
        HandleFlows();
        mutex_unlock(&gMutex);
        thd_sleep(50);
    }
    return 0;
}
}

bool Start() {
    if (gStarted) return true;
    if (snd_stream_init() < 0) return false;   // (arranca el programa del AICA: snd_init)
    for (Loaded& c : gEffects) c.hnd = SFXHND_INVALID;
    for (Flow& f : gFlows) {
        memset(&f, 0, sizeof(f));
        f.hnd = snd_stream_alloc(RequestData, SND_STREAM_BUFFER_MAX_ADPCM);
    }
    gStarted = true;
    gContinue = true;
    gThread = thd_create(false, Thread, 0);
    return true;
}

void Finish() {
    if (!gStarted) return;
    if (gThread) {
        gContinue = false;
        thd_join(gThread, 0);
        gThread = 0;
    }
    for (Flow& f : gFlows) {
        if (f.hnd == SND_STREAM_INVALID) continue;
        if (f.playing) snd_stream_stop(f.hnd);
        snd_stream_destroy(f.hnd);
    }
    for (Loaded& c : gEffects) c.hnd = SFXHND_INVALID;
    snd_sfx_unload_all();
    snd_stream_shutdown();
    gStarted = false;
}

// ------------------------------------------------------------------------------------------------- efectos
Effect LoadEffect(const void* data, uint32_t bytes, uint32_t hz, Format format) {
    if (!gStarted || !data || !bytes) return 0;
    for (int i = 0; i < EFFECTS; i++) {
        if (gEffects[i].hnd != SFXHND_INVALID) continue;
        gEffects[i].hnd = snd_sfx_load_raw_buf((char*)data, bytes, hz, (uint16_t)format, 1);
        if (gEffects[i].hnd == SFXHND_INVALID) return 0;
        gEffects[i].hz = hz;
        return (Effect)(i + 1);
    }
    return 0;
}

void ReleaseEffect(Effect e) {
    if (e < 1 || e > EFFECTS || gEffects[e - 1].hnd == SFXHND_INVALID) return;
    snd_sfx_unload(gEffects[e - 1].hnd);
    gEffects[e - 1].hnd = SFXHND_INVALID;
}

int FreeMemory() { return (int)snd_mem_available(); }

int Touch(Effect e, float vol, float pan, float tone, bool loop) {
    if (!gStarted || e < 1 || e > EFFECTS || gEffects[e - 1].hnd == SFXHND_INVALID) return -1;
    const Loaded& c = gEffects[e - 1];
    sfx_play_data_t d;
    memset(&d, 0, sizeof(d));
    d.chn = -1;   // (el que sigue de los libres: si estan todos, el mas viejo)
    d.idx = c.hnd;
    d.vol = A255(vol);
    d.pan = A255((pan + 1.0f) * 0.5f);
    d.loop = loop ? 1 : 0;
    // el tono: el AICA toca a la frecuencia que se le diga
    d.freq = tone == 1.0f ? 0 : (int)(c.hz * tone + 0.5f);
    const int channel = snd_sfx_play_ex(&d);
    return channel >= 0 ? channel : -1;
}

void Stop(int channel) {
    if (gStarted && channel >= 0) snd_sfx_stop(channel);
}

// ------------------------------------------------------------------------------------------------- flujos
int TouchFlow(const void* adpcm, uint32_t bytes, uint32_t hz, int channels, float vol, bool loop) {
    if (!gStarted || !adpcm || !bytes) return -1;
    mutex_lock(&gMutex);
    int picked = -1;
    for (int i = 0; i < FLOWS && picked < 0; i++) {
        Flow& f = gFlows[i];
        if (f.playing || f.hnd == SND_STREAM_INVALID) continue;
        f.data  = (const uint8_t*)adpcm;
        f.bytes = bytes;
        f.pos = 0;
        f.loop = loop;
        f.playing = true;
        // (dos muestras por byte y canal)
        const uint64_t samples = (uint64_t)bytes * 2 / (channels > 1 ? 2 : 1);
        f.end_us = timer_us_gettime64() + samples * 1000000ULL / hz;
        snd_stream_volume(f.hnd, A255(vol));
        snd_stream_start_adpcm(f.hnd, hz, channels > 1 ? 1 : 0);
        picked = i;
    }
    mutex_unlock(&gMutex);
    return picked;
}

namespace {
void Stop(Flow& f) {
    if (!f.playing) return;
    snd_stream_stop(f.hnd);
    f.playing = false;
    f.data = 0;
}

// los que suenan piden lo que sigue; uno sin lazo que ya sono entero se suelta (lo ultimo que se le paso estaba en el
// buffer del AICA). (Con el mutex tomado)
void HandleFlows() {
    const uint64_t now = timer_us_gettime64();
    for (Flow& f : gFlows) {
        if (!f.playing) continue;
        if (!f.loop && now > f.end_us + 100000) {
            Stop(f);
            continue;
        }
        snd_stream_poll(f.hnd);
    }
}
}

void StopFlow(int i) {
    if (i < 0 || i >= FLOWS) return;
    mutex_lock(&gMutex);
    Stop(gFlows[i]);
    mutex_unlock(&gMutex);
}

bool FlowPlaying(int i) {
    return i >= 0 && i < FLOWS && gFlows[i].playing;
}

void VolumeFlow(int i, float vol) {
    mutex_lock(&gMutex);
    if (i >= 0 && i < FLOWS && gFlows[i].playing) snd_stream_volume(gFlows[i].hnd, A255(vol));
    mutex_unlock(&gMutex);
}

void Update() {
    if (!gStarted || gThread) return;
    HandleFlows();
}

}

#endif // W3D_DREAMCAST
