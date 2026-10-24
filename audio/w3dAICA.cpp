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
struct Flujo {
    snd_stream_hnd_t hnd;
    const uint8_t* datos;
    uint32_t bytes, pos;
    bool lazo, sonando;
    uint64_t fin_us;   // (sin lazo: cuando termina de sonar lo ultimo que se le paso)
};
Flujo gFlujos[FLUJOS];
bool gIniciado = false;
// el hilo que atiende los flujos y lo que comparte con el juego
mutex_t gMutex = MUTEX_INITIALIZER;
kthread_t* gHilo = 0;
volatile bool gSeguir = false;

// los efectos cargados (el handle de KallistiOS y su frecuencia, para el tono); Efecto = indice + 1
enum { EFECTOS = 256 };
struct Cargado { sfxhnd_t hnd; uint32_t hz; };
Cargado gEfectos[EFECTOS];

// el AICA pide 'pedidos' bytes (de los dos canales juntos si es estereo): se le da lo que sigue, sin pasar el final
// (lo que falte lo pide en la vuelta siguiente: con lazo, desde el principio)
void* PedirDatos(snd_stream_hnd_t hnd, int pedidos, int* dados) {
    for (Flujo& f : gFlujos) {
        if (f.hnd != hnd || !f.datos) continue;
        if (f.pos >= f.bytes) {
            if (!f.lazo) { *dados = 0; return 0; }
            f.pos = 0;
        }
        uint32_t n = f.bytes - f.pos;
        if (n > (uint32_t)pedidos) n = (uint32_t)pedidos;
        const uint8_t* p = f.datos + f.pos;
        f.pos += n;
        *dados = (int)n;
        return (void*)p;
    }
    *dados = 0;
    return 0;
}

void AtenderFlujos();

// el hilo: cada 50 ms, los flujos (si espera un DMA, espera el: el juego sigue). Mas seguido no hace falta (el buffer
// de un flujo dura mas de un segundo) y cuesta: cada vuelta lee la posicion de los canales por el bus del AICA
void* Hilo(void*) {
    while (gSeguir) {
        mutex_lock(&gMutex);
        AtenderFlujos();
        mutex_unlock(&gMutex);
        thd_sleep(50);
    }
    return 0;
}
}

bool Iniciar() {
    if (gIniciado) return true;
    if (snd_stream_init() < 0) return false;   // (arranca el programa del AICA: snd_init)
    for (Cargado& c : gEfectos) c.hnd = SFXHND_INVALID;
    for (Flujo& f : gFlujos) {
        memset(&f, 0, sizeof(f));
        f.hnd = snd_stream_alloc(PedirDatos, SND_STREAM_BUFFER_MAX_ADPCM);
    }
    gIniciado = true;
    gSeguir = true;
    gHilo = thd_create(false, Hilo, 0);
    return true;
}

void Terminar() {
    if (!gIniciado) return;
    if (gHilo) {
        gSeguir = false;
        thd_join(gHilo, 0);
        gHilo = 0;
    }
    for (Flujo& f : gFlujos) {
        if (f.hnd == SND_STREAM_INVALID) continue;
        if (f.sonando) snd_stream_stop(f.hnd);
        snd_stream_destroy(f.hnd);
    }
    for (Cargado& c : gEfectos) c.hnd = SFXHND_INVALID;
    snd_sfx_unload_all();
    snd_stream_shutdown();
    gIniciado = false;
}

// ------------------------------------------------------------------------------------------------- efectos
Efecto CargarEfecto(const void* datos, uint32_t bytes, uint32_t hz, Formato formato) {
    if (!gIniciado || !datos || !bytes) return 0;
    for (int i = 0; i < EFECTOS; i++) {
        if (gEfectos[i].hnd != SFXHND_INVALID) continue;
        gEfectos[i].hnd = snd_sfx_load_raw_buf((char*)datos, bytes, hz, (uint16_t)formato, 1);
        if (gEfectos[i].hnd == SFXHND_INVALID) return 0;
        gEfectos[i].hz = hz;
        return (Efecto)(i + 1);
    }
    return 0;
}

void SoltarEfecto(Efecto e) {
    if (e < 1 || e > EFECTOS || gEfectos[e - 1].hnd == SFXHND_INVALID) return;
    snd_sfx_unload(gEfectos[e - 1].hnd);
    gEfectos[e - 1].hnd = SFXHND_INVALID;
}

int MemoriaLibre() { return (int)snd_mem_available(); }

int Tocar(Efecto e, float vol, float pan, float tono, bool lazo) {
    if (!gIniciado || e < 1 || e > EFECTOS || gEfectos[e - 1].hnd == SFXHND_INVALID) return -1;
    const Cargado& c = gEfectos[e - 1];
    sfx_play_data_t d;
    memset(&d, 0, sizeof(d));
    d.chn = -1;   // (el que sigue de los libres: si estan todos, el mas viejo)
    d.idx = c.hnd;
    d.vol = A255(vol);
    d.pan = A255((pan + 1.0f) * 0.5f);
    d.loop = lazo ? 1 : 0;
    // el tono: el AICA toca a la frecuencia que se le diga
    d.freq = tono == 1.0f ? 0 : (int)(c.hz * tono + 0.5f);
    const int canal = snd_sfx_play_ex(&d);
    return canal >= 0 ? canal : -1;
}

void Parar(int canal) {
    if (gIniciado && canal >= 0) snd_sfx_stop(canal);
}

// ------------------------------------------------------------------------------------------------- flujos
int TocarFlujo(const void* adpcm, uint32_t bytes, uint32_t hz, int canales, float vol, bool lazo) {
    if (!gIniciado || !adpcm || !bytes) return -1;
    mutex_lock(&gMutex);
    int elegido = -1;
    for (int i = 0; i < FLUJOS && elegido < 0; i++) {
        Flujo& f = gFlujos[i];
        if (f.sonando || f.hnd == SND_STREAM_INVALID) continue;
        f.datos = (const uint8_t*)adpcm;
        f.bytes = bytes;
        f.pos = 0;
        f.lazo = lazo;
        f.sonando = true;
        // (dos muestras por byte y canal)
        const uint64_t muestras = (uint64_t)bytes * 2 / (canales > 1 ? 2 : 1);
        f.fin_us = timer_us_gettime64() + muestras * 1000000ULL / hz;
        snd_stream_volume(f.hnd, A255(vol));
        snd_stream_start_adpcm(f.hnd, hz, canales > 1 ? 1 : 0);
        elegido = i;
    }
    mutex_unlock(&gMutex);
    return elegido;
}

namespace {
void Parar(Flujo& f) {
    if (!f.sonando) return;
    snd_stream_stop(f.hnd);
    f.sonando = false;
    f.datos = 0;
}

// los que suenan piden lo que sigue; uno sin lazo que ya sono entero se suelta (lo ultimo que se le paso estaba en el
// buffer del AICA). (Con el mutex tomado)
void AtenderFlujos() {
    const uint64_t ahora = timer_us_gettime64();
    for (Flujo& f : gFlujos) {
        if (!f.sonando) continue;
        if (!f.lazo && ahora > f.fin_us + 100000) {
            Parar(f);
            continue;
        }
        snd_stream_poll(f.hnd);
    }
}
}

void PararFlujo(int i) {
    if (i < 0 || i >= FLUJOS) return;
    mutex_lock(&gMutex);
    Parar(gFlujos[i]);
    mutex_unlock(&gMutex);
}

bool FlujoSonando(int i) {
    return i >= 0 && i < FLUJOS && gFlujos[i].sonando;
}

void VolumenFlujo(int i, float vol) {
    mutex_lock(&gMutex);
    if (i >= 0 && i < FLUJOS && gFlujos[i].sonando) snd_stream_volume(gFlujos[i].hnd, A255(vol));
    mutex_unlock(&gMutex);
}

void Actualizar() {
    if (!gIniciado || gHilo) return;
    AtenderFlujos();
}

}

#endif // W3D_DREAMCAST
