#include "W3dAudio.h"
#include "W3dAudioBackend.h"   // contrato compartido con los backends (firma verificada al compilar)
#include "base/w3dlog.h"       // el mixer dice si abrio o si el juego va mudo
#include "io/w3dFilesystem.h"   // leer el .wav por el Core (disco / pak / APK)
#include <vector>
#ifdef W3D_ENABLE_AUDIO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>              // pow: los pasos de la envolvente se calculan al disparar la voz
#endif

// El mixer + el loader WAV son PORTABLES. El dispatcher (esta unidad) siempre compila:
// con el flag apagado son stubs no-op, asi la app enlaza sin backend ni dependencias.

namespace w3dEngine {

#ifdef W3D_ENABLE_AUDIO

// el contrato con el backend de salida vive en W3dAudioBackend.h (lo incluyen los dos lados)

// ---------------------------------------------------------------------------
//  Sonido = PCM 16-bit en SU rate y con SUS canales (1 o 2): el mixer lo lleva al suyo al mezclar (avanza 'paso'
//  muestras del sonido por muestra de salida). Antes se guardaba ya convertido a estereo al rate del mixer: un wav
//  mono de 22 kHz ocupaba el doble en un telefono a 22 kHz y 4 veces en una PC a 44,1 kHz.
// ---------------------------------------------------------------------------
class W3dSound {
public:
    short* pcm;   // interleaved (L,R,L,R... si canales = 2)
    int    frames;
    int    canales;   // 1 o 2
    int    rate;      // Hz del sonido (la voz avanza rate / rate del mixer muestras por muestra de salida)
    // PUNTOS DE LOOP del WAV (chunk 'smpl', el estandar de los editores de audio y de los streams de consola):
    // con loop, al pasar loopFin la voz vuelve a loopIni (la INTRO de una musica suena una sola vez). En frames
    // del sonido; loopFin <= 0 = sin puntos (el loop es el archivo entero, como siempre).
    int    loopIni, loopFin;
    W3dSound() : pcm(0), frames(0), canales(2), rate(44100), loopIni(0), loopFin(0) {}
    ~W3dSound() { delete[] pcm; }
};

// ---------------------------------------------------------------------------
//  Estado del mixer.
// ---------------------------------------------------------------------------
struct Voice {
    W3dSound* s;
    float pos;      // frame actual (fraccional: el paso 'step' da el pitch)
    float step;     // avance por muestra de salida: 1.0 = pitch normal
    float vol;
    // RAMPA de salida (W3dSoundStopFade): ganancia extra 1..0 aplicada POR MUESTRA en el
    // mixer. fadeStep 0 = sin fade; > 0 = la voz baja linealmente y al llegar a 0 se
    // LIBERA sola. Cortar de golpe (active=false) clickea: la onda se corta en cualquier
    // amplitud; la rampa la lleva a cero antes (default ~5 ms, inaudible como fade).
    float fadeGain;
    float fadeStep;
    float gL, gR;   // paneo (W3dSoundPlayPitchPan): ganancia por canal, 1/1 = centro
    bool  loop;
    bool  active;
    int   id;
    // AGENDA en el reloj del mixer (frames mezclados desde que abrio): la voz arranca en la muestra 'inicio' y
    // empieza su rampa de salida en la muestra 'finAbs' (0 = sin agenda). Un secuenciador de musica dispara las notas
    // con anticipacion y suenan en la muestra exacta, sin depender de cuando corre el frame del juego
    unsigned long long inicio;
    unsigned long long finAbs;
    float fadeFin;  // paso de la rampa agendada
    // ENVOLVENTE (W3dEnvolvente): etapa 0 = sin envolvente (la ganancia es 1 y no cuesta nada); 1 ataque, 2 decaimiento,
    // 3 sostenido, 4 liberacion. El ataque SUMA por muestra; el resto MULTIPLICA (baja lineal en dB, como los chips)
    unsigned char envEtapa;
    float envNivel;
    float envAtaque, envDecae, envSost, envCaida, envLibera;
    unsigned long long envSoltar;   // muestra del reloj del mixer en que la nota se suelta sola (0 = no)
    float envio;                    // lo que la voz manda a la reverb (0..1)
};

// 64: un secuenciador que imita una placa de sonido (Daytona USA: 2 MultiPCM x 28 slots = 56) suena entero; con 32
// las notas que llegaban con la placa llena se perdían (la voz "Gentlemen, start your engines" de la largada)
static const int MAX_VOICES = 64;
static Voice s_voices[MAX_VOICES];
static int   s_rate      = 44100;
// muestras del sonido que avanza la voz por muestra de salida (su rate llevado al del mixer, por el pitch)
static float PasoDe(const W3dSound* s, float pitch) { return pitch * (float)s->rate / (float)s_rate; }
static float s_master    = 1.0f;
static int   s_nextId    = 1;
static bool  s_ready     = false;
// acumulador de mezcla en 32 bits (se reserva una vez; crece si el backend pide mas) y la
// ganancia del limitador de picos (1 = sin reduccion; baja de golpe al saturar y se recupera)
static int*  s_acc       = 0;
static int   s_accFrames = 0;
static float s_limGain   = 1.0f;
static bool  s_pausa     = false;   // W3dAudioPause: el mixer saca silencio sin avanzar las voces
static unsigned long long s_reloj = 0;   // frames MEZCLADOS (sin contar la pausa): el reloj de las agendas

// ---------------------------------------------------------------------------
//  REVERB: una sala de Schroeder y Moorer (peines en paralelo con un pasa-bajos en la realimentacion -la cola pierde
//  agudos como en una sala de verdad- y despues pasa-todos en serie que la vuelven densa). Los largos son los de
//  Freeverb (Jezar, dominio publico) a 44100 Hz, escalados al rate del mixer; el canal derecho es un poco mas largo
//  (estereo). Todo en ENTEROS (coeficientes Q15): sin denormales y barato en un ARM sin FPU rapida. Entra el ENVIO de
//  cada voz (mono) y lo que sale se suma a la mezcla. nivel 0 = no se procesa
// ---------------------------------------------------------------------------
enum { REV_PEINES = 6, REV_PASATODOS = 2 };
static const int kRevPeine[REV_PEINES] = { 1116, 1188, 1277, 1356, 1422, 1491 };
static const int kRevPasa[REV_PASATODOS] = { 556, 441 };
static const int kRevSeparacion = 23;
struct RevPeine { int* buf; int largo, pos, filtro; };
struct RevPasa  { int* buf; int largo, pos; };
static RevPeine s_peine[2][REV_PEINES];
static RevPasa  s_pasa[2][REV_PASATODOS];
static bool s_revLista = false;      // buffers armados (al rate del mixer)
static int  s_revRealim = 0;         // realimentacion de los peines (Q15)
static int  s_revAm1 = 0, s_revAm2 = 32768;   // amortiguacion: filtro = salida * am2 + filtro * am1 (Q15)
static int  s_revNivel = 0;          // lo que se suma (Q15, ya con la escala de la salida)
static int  s_revReposo = 0;         // muestras seguidas sin envio: pasada la cola, no se procesa
static int* s_envio = 0;             // el envio de las voces, mono (por frame del buffer)
static int  s_envioFrames = 0;

// ---------------------------------------------------------------------------
//  WAV: parseo desde memoria -> PCM 16-bit con sus canales (mas de 2: los dos primeros) y su rate.
// ---------------------------------------------------------------------------
static unsigned int rdU32(const unsigned char* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((unsigned)p[3]<<24); }
static unsigned int rdU16(const unsigned char* p) { return p[0] | (p[1]<<8); }

// lee la muestra (canal c, frame f) de un PCM crudo como short.
static short srcSample(const unsigned char* data, int frames, int channels, int bits, int f, int c) {
    if (f < 0) f = 0; if (f >= frames) f = frames - 1;
    if (c >= channels) c = channels - 1;
    const unsigned char* p = data + ((size_t)f * channels + c) * (bits / 8);
    if (bits == 8) return (short)(((int)p[0] - 128) << 8);   // 8-bit unsigned -> 16-bit signed
    return (short)(short)rdU16(p);                            // 16-bit signed LE
}

static W3dSound* decodeWav(const unsigned char* b, size_t len) {
    if (len < 44 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) return 0;
    int channels = 0, srcRate = 0, bits = 0;
    const unsigned char* data = 0; size_t dataLen = 0;
    long smplIni = -1, smplFin = -1;
    size_t off = 12;
    while (off + 8 <= len) {
        const unsigned char* id = b + off;
        unsigned int csz = rdU32(b + off + 4);
        const unsigned char* body = b + off + 8;
        if (memcmp(id, "fmt ", 4) == 0 && csz >= 16 && off + 8 + 16 <= len) {   // el cuerpo (16 bytes) tiene que ENTRAR en el buffer
            if (rdU16(body) != 1) return 0;           // solo PCM sin comprimir
            channels = (int)rdU16(body + 2);
            srcRate  = (int)rdU32(body + 4);
            bits     = (int)rdU16(body + 14);
        } else if (memcmp(id, "smpl", 4) == 0 && csz >= 60 && off + 8 + 60 <= len && rdU32(body + 28) >= 1) {
            // el primer loop: [36] cue id, [40] tipo, [44] inicio, [48] fin (INCLUSIVE), en muestras del archivo
            smplIni = (long)rdU32(body + 44);
            smplFin = (long)rdU32(body + 48);
        } else if (memcmp(id, "data", 4) == 0) {
            data = body; dataLen = csz;
            if (off + 8 + dataLen > len) dataLen = len - (off + 8); // tolerar tamano pasado
        }
        off += 8 + csz + (csz & 1); // los chunks se alinean a 2
    }
    if (!data || channels < 1 || srcRate < 1 || (bits != 8 && bits != 16)) return 0;

    int srcFrames = (int)(dataLen / ((size_t)channels * (bits / 8)));
    if (srcFrames < 1) return 0;
    const int can = channels >= 2 ? 2 : 1;
    short* out = new short[(size_t)srcFrames * can];
    for (int i = 0; i < srcFrames; i++)
        for (int c = 0; c < can; c++) out[(size_t)i * can + c] = srcSample(data, srcFrames, channels, bits, i, c);
    W3dSound* s = new W3dSound();
    s->pcm = out; s->frames = srcFrames; s->canales = can; s->rate = srcRate;
    if (smplIni >= 0 && smplFin > smplIni && smplFin < srcFrames) {
        s->loopIni = (int)smplIni;
        s->loopFin = (int)(smplFin + 1);   // exclusivo
        if (s->loopIni >= s->loopFin) { s->loopIni = 0; s->loopFin = 0; }
    }
    return s;
}

// ---------------------------------------------------------------------------
//  API publica
// ---------------------------------------------------------------------------
bool W3dAudioInit(int sampleRate) {
    if (s_ready) return true;
    s_rate = sampleRate > 0 ? sampleRate : 44100;
    for (int i = 0; i < MAX_VOICES; i++) s_voices[i].active = false;
    if (!s_acc) { s_accFrames = 8192; s_acc = new int[(size_t)s_accFrames * 2]; }
    s_limGain = 1.0f;
    s_ready = W3dAudioBackendInit(s_rate);
    // QUE QUEDE DICHO: sin mixer abierto, sonido() ni siquiera intenta leer el .wav
    // (W3dSoundLoad corta en s_ready). Un juego mudo por esto no dejaba NINGUNA
    // pista en el log y parecia "faltan los sonidos en el paquete".
    if (s_ready) w3dLogf("[audio] mixer abierto a %d Hz", s_rate);
    else         w3dLogfE("[audio] no pude abrir el dispositivo: el juego va MUDO "
                          "(sonido() no va a cargar ningun .wav)");
    return s_ready;
}

void W3dAudioShutdown() {
    if (!s_ready) return;
    W3dAudioBackendShutdown();
    s_ready = false;
}

W3dSound* W3dSoundLoadMemory(const void* bytes, size_t len) {
    if (!s_ready) return 0;
    return decodeWav((const unsigned char*)bytes, len);
}

// Genera un tono de onda CUADRADA (beep estilo WhiskPaddle) como W3dSound, al rate del mixer. freq en Hz,
// dur en ms, vol 0..1. COPYRIGHT-FREE por construccion (matematica pura, sin samples de nadie). Con un
// fade de ~5ms al entrar/salir para que no "clickee". El caller lo cachea (no regenerar cada frame).
W3dSound* W3dSoundBeep(float freq, int ms, float vol) {
    if (!s_ready) return 0;
    if (freq < 1.0f) freq = 1.0f;
    if (ms < 1) ms = 1;
    if (vol < 0.0f) vol = 0.0f; if (vol > 1.0f) vol = 1.0f;
    int frames = (int)((long)ms * s_rate / 1000);
    if (frames < 1) frames = 1;
    W3dSound* s = new W3dSound();
    s->pcm = new short[frames];
    s->frames = frames; s->canales = 1; s->rate = s_rate;
    float periodo = (float)s_rate / freq;      // muestras por ciclo
    int fade = s_rate / 200; if (fade < 1) fade = 1;   // ~5 ms
    for (int i = 0; i < frames; i++) {
        float fase = (float)i / periodo;
        float sq = ((fase - (float)(int)fase) < 0.5f) ? 1.0f : -1.0f;   // onda cuadrada
        float g = vol;
        if (i < fade) g *= (float)i / (float)fade;                       // fade in
        if (i > frames - fade) g *= (float)(frames - i) / (float)fade;   // fade out
        short v = (short)(sq * g * 9000.0f);   // ~0.27 de escala: fuerte pero no satura al mezclar voces
        s->pcm[i] = v;
    }
    return s;
}

W3dSound* W3dSoundLoad(const char* path) {
    if (!s_ready) return 0;   // igual que LoadMemory: sin mixer abierto el resample usaria un rate falso
    if (!path || !*path) return 0;
    // POR LA ABSTRACCION DEL CORE (ReadFileBytes) y no con fopen: un .wav puede
    // venir de un archivo suelto, del PAK embebido (assets empaquetados) o de
    // ADENTRO DEL APK, donde NO existe como archivo y el fopen falla siempre.
    // Era el motivo de que el juego de Android estuviera mudo con los .wav
    // perfectamente empaquetados adentro del APK.
    std::vector<unsigned char> datos;
    if (!w3dFileSystem::ReadFileBytes(std::string(path), datos) || datos.empty()) {
        w3dLogfW("[audio] no pude leer '%s' (no esta ni en el disco, ni en el pak, ni en los assets)", path);
        return 0;
    }
    return decodeWav(&datos[0], datos.size());
}

void W3dSoundFree(W3dSound* s) {
    if (!s) return;
    if (s_ready) {
        W3dAudioBackendLock();
        for (int i = 0; i < MAX_VOICES; i++)
            if (s_voices[i].active && s_voices[i].s == s) s_voices[i].active = false;
        W3dAudioBackendUnlock();
    }
    delete s;
}

int W3dSoundPlay(W3dSound* s, float volume, bool loop) {
    return W3dSoundPlayPitch(s, volume, loop, 1.0f);
}

int W3dSoundPlayPitch(W3dSound* s, float volume, bool loop, float pitch) {
    return W3dSoundPlayPitchPan(s, volume, loop, pitch, 0.0f);
}

int W3dSoundPlayPitchPan(W3dSound* s, float volume, bool loop, float pitch, float pan) {
    return W3dSoundPlayAt(s, volume, loop, pitch, pan, -1.0);
}

double W3dAudioReloj() {
    if (!s_ready) return 0.0;
    W3dAudioBackendLock();
    const double t = (double)s_reloj / (double)s_rate;
    W3dAudioBackendUnlock();
    return t;
}

int W3dSoundPlayAt(W3dSound* s, float volume, bool loop, float pitch, float pan, double enSeg) {
    return W3dSoundPlayEnv(s, volume, loop, pitch, pan, enSeg, 0, 0.0f);
}

// los pasos por muestra de una envolvente (los pow se hacen aca, al disparar; el mixer solo suma y multiplica)
static void ArmarEnvolvente(Voice& v, const W3dEnvolvente* e) {
    v.envEtapa = 0; v.envNivel = 1.0f; v.envSoltar = 0;
    v.envAtaque = 0.0f; v.envDecae = 0.0f; v.envSost = 1.0f; v.envCaida = 1.0f; v.envLibera = 0.0f;
    if (!e) return;
    const double r = (double)s_rate;
    float sost = e->sostenido; if (sost < 0.0f) sost = 0.0f; if (sost > 1.0f) sost = 1.0f;
    v.envSost = sost;
    if (e->ataque > 0.0f) v.envAtaque = (float)(1.0 / ((double)e->ataque * r));
    if (e->decaimiento > 0.0f && sost < 1.0f)
        v.envDecae = (float)pow(sost > 0.001f ? (double)sost : 0.001, 1.0 / ((double)e->decaimiento * r));
    if (e->caida > 0.0f) v.envCaida = (float)pow(0.001, 1.0 / ((double)e->caida * r));
    v.envLibera = (float)pow(0.001, 1.0 / ((double)(e->liberacion > 0.0f ? e->liberacion : 0.005f) * r));
    if (e->duracion > 0.0f) {
        const unsigned long long desde = v.inicio > s_reloj ? v.inicio : s_reloj;
        v.envSoltar = desde + (unsigned long long)((double)e->duracion * r + 0.5);
        if (v.envSoltar < 1) v.envSoltar = 1;
    }
    if (v.envAtaque > 0.0f)     { v.envEtapa = 1; v.envNivel = 0.0f; }
    else if (v.envDecae > 0.0f) { v.envEtapa = 2; v.envNivel = 1.0f; }
    else                        { v.envEtapa = 3; v.envNivel = sost; }
}

int W3dSoundPlayEnv(W3dSound* s, float volume, bool loop, float pitch, float pan, double enSeg,
                    const W3dEnvolvente* env, float envioReverb) {
    if (!s_ready || !s) return 0;
    if (pitch < 0.05f) pitch = 0.05f; if (pitch > 8.0f) pitch = 8.0f;   // limites sanos
    if (pan < -1.0f) pan = -1.0f; if (pan > 1.0f) pan = 1.0f;
    int id = 0;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!s_voices[i].active) {
            Voice& v = s_voices[i];
            v.s = s; v.pos = 0.0f; v.step = PasoDe(s, pitch); v.vol = volume; v.loop = loop; v.active = true;
            v.fadeGain = 1.0f; v.fadeStep = 0.0f;   // sin rampa: la voz suena a su volumen
            v.gL = pan > 0.0f ? 1.0f - pan : 1.0f;  // el lado fuerte a 1, el otro atenuado
            v.gR = pan < 0.0f ? 1.0f + pan : 1.0f;
            v.id = id = s_nextId++;
            if (s_nextId <= 0) s_nextId = 1;
            v.inicio = (enSeg > 0.0) ? (unsigned long long)(enSeg * (double)s_rate + 0.5) : 0;
            v.finAbs = 0; v.fadeFin = 0.0f;
            ArmarEnvolvente(v, env);
            v.envio = envioReverb < 0.0f ? 0.0f : (envioReverb > 1.0f ? 1.0f : envioReverb);
            break;
        }
    }
    W3dAudioBackendUnlock();
    return id;
}

void W3dSoundStop(int voice) {
    if (!s_ready || voice <= 0) return;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active && s_voices[i].id == voice) s_voices[i].active = false;
    W3dAudioBackendUnlock();
}

// corta la voz con una RAMPA lineal de 'fadeSeg' segundos (<= 0 -> ~5 ms, el minimo
// anti-click). La rampa corre en el MIXER, por muestra, desde la ganancia ACTUAL de la
// voz (llamarlo dos veces no la "sube" de vuelta); al llegar a 0 la voz se libera sola.
// Pensado para notas SOSTENIDAS (samples en loop) del secuenciador de musica en lua.
void W3dSoundStopFade(int voice, float fadeSeg) {
    W3dSoundStopFadeAt(voice, fadeSeg, -1.0);
}

// la rampa de salida AGENDADA en el reloj del mixer (enSeg <= 0: ya). Una voz agendada que todavia no arranco y se
// corta ya, no suena nunca.
void W3dSoundStopFadeAt(int voice, float fadeSeg, double enSeg) {
    if (!s_ready || voice <= 0) return;
    if (fadeSeg <= 0.0f) fadeSeg = 0.005f;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active && s_voices[i].id == voice) {
            Voice& v = s_voices[i];
            if (enSeg > 0.0) {
                float frames = fadeSeg * (float)s_rate;
                if (frames < 1.0f) frames = 1.0f;
                v.finAbs = (unsigned long long)(enSeg * (double)s_rate + 0.5);
                if (v.finAbs < 1) v.finAbs = 1;
                v.fadeFin = 1.0f / frames;
                continue;
            }
            if (v.inicio > s_reloj) { v.active = false; continue; }
            float frames = fadeSeg * (float)s_rate;
            if (frames < 1.0f) frames = 1.0f;
            float paso = v.fadeGain / frames;   // llega a 0 en fadeSeg desde el gain actual
            if (paso > v.fadeStep) v.fadeStep = paso;   // un fade MAS corto que el que ya corre, manda
            if (v.fadeStep <= 0.0f) v.fadeStep = paso;
        }
    W3dAudioBackendUnlock();
}

// soltar la nota: la envolvente pasa a su liberacion. Sin envolvente, la rampa corta de siempre
void W3dSoundSoltar(int voice) {
    if (!s_ready || voice <= 0) return;
    bool sinEnvolvente = false;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active && s_voices[i].id == voice) {
            if (s_voices[i].envEtapa) { s_voices[i].envEtapa = 4; s_voices[i].envSoltar = 0; }
            else sinEnvolvente = true;
        }
    W3dAudioBackendUnlock();
    if (sinEnvolvente) W3dSoundStopFade(voice, 0.005f);
}

void W3dSoundSetEnvio(int voice, float envio) {
    if (!s_ready || voice <= 0) return;
    if (envio < 0.0f) envio = 0.0f; if (envio > 1.0f) envio = 1.0f;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active && s_voices[i].id == voice) s_voices[i].envio = envio;
    W3dAudioBackendUnlock();
}

// arma los buffers de la reverb (una vez, al rate del mixer) y fija la sala. Los buffers se piden aca (hilo del juego)
// y se cuelgan bajo el lock: el hilo de audio nunca reserva memoria
void W3dAudioReverb(float sala, float amortiguacion, float nivel) {
    if (!s_ready) return;
    if (sala < 0.0f) sala = 0.0f; if (sala > 1.0f) sala = 1.0f;
    if (amortiguacion < 0.0f) amortiguacion = 0.0f; if (amortiguacion > 1.0f) amortiguacion = 1.0f;
    if (nivel < 0.0f) nivel = 0.0f; if (nivel > 1.0f) nivel = 1.0f;
    if (!s_revLista && nivel > 0.0f) {
        RevPeine pe[2][REV_PEINES];
        RevPasa pa[2][REV_PASATODOS];
        for (int c = 0; c < 2; c++) {
            for (int k = 0; k < REV_PEINES; k++) {
                int n = (int)((long)(kRevPeine[k] + c * kRevSeparacion) * s_rate / 44100); if (n < 8) n = 8;
                pe[c][k].buf = new int[n]; memset(pe[c][k].buf, 0, sizeof(int) * (size_t)n);
                pe[c][k].largo = n; pe[c][k].pos = 0; pe[c][k].filtro = 0;
            }
            for (int k = 0; k < REV_PASATODOS; k++) {
                int n = (int)((long)(kRevPasa[k] + c * kRevSeparacion) * s_rate / 44100); if (n < 4) n = 4;
                pa[c][k].buf = new int[n]; memset(pa[c][k].buf, 0, sizeof(int) * (size_t)n);
                pa[c][k].largo = n; pa[c][k].pos = 0;
            }
        }
        W3dAudioBackendLock();
        memcpy(s_peine, pe, sizeof(pe));
        memcpy(s_pasa, pa, sizeof(pa));
        s_revLista = true;
        W3dAudioBackendUnlock();
    }
    // los valores de Freeverb: realimentacion 0.7..0.98, amortiguacion hasta 0.4; la entrada entra a 0.02 (6 peines).
    // La salida va por 9: con nivel 1 la cola de un sonido de banda ancha suena tan fuerte como lo que se le mando
    // (medido con ruido: con 3, la de Freeverb, quedaba 10 dB abajo). Asi un envio de -15 dB (el de un anillo en la
    // Dreamcast) se oye a -15 dB, como con el DSP del chip
    const float realim = 0.7f + 0.28f * sala;
    const float am = 0.4f * amortiguacion;
    W3dAudioBackendLock();
    s_revRealim = (int)(realim * 32768.0f);
    s_revAm1 = (int)(am * 32768.0f);
    s_revAm2 = 32768 - s_revAm1;
    s_revNivel = (int)(nivel * 9.0f * 32768.0f);
    W3dAudioBackendUnlock();
}

int W3dAudioRate() { return s_ready ? s_rate : 0; }

void W3dSoundStopAll() {
    if (!s_ready) return;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++) s_voices[i].active = false;
    W3dAudioBackendUnlock();
}

void W3dAudioPause(bool pausa) {
    if (!s_ready) return;
    W3dAudioBackendLock();
    s_pausa = pausa;
    W3dAudioBackendUnlock();
}
bool W3dAudioPaused() { return s_pausa; }

void W3dSoundSetVolume(int voice, float volume) {
    if (!s_ready || voice <= 0) return;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active && s_voices[i].id == voice) s_voices[i].vol = volume;
    W3dAudioBackendUnlock();
}

void W3dSoundSetPitch(int voice, float pitch) {
    if (!s_ready || voice <= 0) return;
    if (pitch < 0.05f) pitch = 0.05f; if (pitch > 8.0f) pitch = 8.0f;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active && s_voices[i].id == voice) s_voices[i].step = PasoDe(s_voices[i].s, pitch);
    W3dAudioBackendUnlock();
}

// la posicion de la voz en segundos del sonido (ver W3dAudio.h): el reloj maestro para
// sincronizar animacion con audio. pos esta en frames DEL SONIDO, asi que pos / su rate = segundos del wav.
float W3dSoundPos(int voice) {
    if (!s_ready || voice <= 0) return -1.0f;
    float seg = -1.0f;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active && s_voices[i].id == voice) { seg = s_voices[i].pos / (float)s_voices[i].s->rate; break; }
    W3dAudioBackendUnlock();
    return seg;
}

void W3dAudioMasterVolume(float v) { s_master = v < 0 ? 0 : (v > 1 ? 1 : v); }

// volumen del JUEGO (0..1): un SEGUNDO escalar, independiente del master de usuario, para que el aplicar() del
// volumen de usuario no lo pise. Lo setea la tarjeta Juego (0..100 -> /100) y se aplica al abrir el .w3d.
static float s_juego = 1.0f;
void W3dAudioJuegoVolume(float v) { s_juego = v < 0 ? 0 : (v > 1 ? 1 : v); }

// un producto Q15 REDONDEADO al mas cercano: con '>> 15' solo (hacia abajo) la realimentacion de los peines junta un
// sesgo negativo y la cola termina en una continua que no se apaga nunca
static inline int Q15(long long x) { return (int)((x + 16384) >> 15); }
// la senal de la reverb va con 8 bits de FRACCION (x256): con enteros justos, el redondeo deja una banda muerta donde
// la realimentacion ya no achica la senal y los peines quedan oscilando para siempre (un "ciclo limite": a -50 dB, a
// la frecuencia de lo ultimo que sono). Con la fraccion, esa banda queda debajo del bit menos significativo de la salida
enum { REV_FRAC = 8 };

// la REVERB sobre el envio de este buffer, sumada a la mezcla (ver arriba). Pasada la cola sin envio no se procesa
static void ReverbMezclar(int* acc, const int* envio, int frames) {
    bool hayEnvio = false;
    for (int i = 0; i < frames && !hayEnvio; i++) hayEnvio = envio[i] != 0;
    if (hayEnvio) s_revReposo = 0;
    else {
        if (s_revReposo > s_rate * 8) return;   // la cola ya se apago (8 s alcanzan para la sala mas larga)
        s_revReposo += frames;
    }
    for (int i = 0; i < frames; i++) {
        const int ent = Q15((long long)envio[i] * (655 << REV_FRAC));   // 0.02, con la fraccion
        for (int c = 0; c < 2; c++) {
            int sal = 0;
            for (int k = 0; k < REV_PEINES; k++) {
                RevPeine& p = s_peine[c][k];
                const int o = p.buf[p.pos];
                p.filtro = Q15((long long)o * s_revAm2 + (long long)p.filtro * s_revAm1);
                p.buf[p.pos] = ent + Q15((long long)p.filtro * s_revRealim);
                if (++p.pos >= p.largo) p.pos = 0;
                sal += o;
            }
            for (int k = 0; k < REV_PASATODOS; k++) {
                RevPasa& a = s_pasa[c][k];
                const int bo = a.buf[a.pos];
                a.buf[a.pos] = sal + bo / 2;   // (division: simetrica, sin sesgo)
                sal = bo - sal;
                if (++a.pos >= a.largo) a.pos = 0;
            }
            acc[i * 2 + c] += (int)(((long long)sal * s_revNivel + (1LL << (14 + REV_FRAC))) >> (15 + REV_FRAC));
        }
    }
}

// La MEZCLA (corre en el hilo de audio, bajo la exclusion del backend; W3dAudioGrabar la llama igual):
// 1) cada voz se suma a un acumulador de 32 bits (sin recortar por voz) leyendo el PCM con interpolacion LINEAL entre
//    la muestra actual y la siguiente (pitch fraccional limpio), por su volumen, su rampa de salida, su ENVOLVENTE y
//    su paneo; lo que manda a la reverb (su ENVIO) va a otro acumulador, mono;
// 2) la REVERB procesa ese envio y lo suma;
// 3) la suma pasa por un LIMITADOR de picos: si un frame supera el rango de 16 bits, la ganancia baja en el acto a lo
//    justo y despues se recupera en ~0.3 s. Muchas voces a la vez (musica secuenciada) no saturan ni distorsionan.
static void Mezclar(short* out, int frames) {
    if (frames > s_accFrames || !s_acc) {
        delete[] s_acc; s_accFrames = frames; s_acc = new int[(size_t)frames * 2];
    }
    if (frames > s_envioFrames || !s_envio) {
        delete[] s_envio; s_envioFrames = frames; s_envio = new int[(size_t)frames];
    }
    int* acc = s_acc;
    int* envio = s_envio;
    for (int i = 0; i < frames * 2; i++) acc[i] = 0;
    for (int i = 0; i < frames; i++) envio[i] = 0;
    const bool conReverb = s_revLista && s_revNivel > 0;
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice& v = s_voices[vi];
        if (!v.active) continue;
        float g = v.vol * s_master * s_juego;
        const short* p = v.s->pcm;
        const bool mono = v.s->canales == 1;
        int fr = v.s->frames;
        // con puntos de loop (smpl): la voz en loop termina su vuelta en loopFin y vuelve a loopIni
        const bool conPuntos = v.loop && v.s->loopFin > 0;
        const int finVuelta = conPuntos ? v.s->loopFin : fr;
        const int iniVuelta = conPuntos ? v.s->loopIni : 0;
        const bool manda = conReverb && v.envio > 0.0f;
        float fpos = v.pos;
        int i0 = 0;
        if (v.inicio > s_reloj) {
            const unsigned long long falta = v.inicio - s_reloj;
            if (falta >= (unsigned long long)frames) continue;      // arranca en otro buffer
            i0 = (int)falta;
        }
        for (int i = i0; i < frames; i++) {
            if (v.finAbs && s_reloj + (unsigned long long)i >= v.finAbs) {   // la rampa agendada empieza en esta muestra
                if (v.fadeStep <= 0.0f || v.fadeFin > v.fadeStep) v.fadeStep = v.fadeFin * v.fadeGain;
                v.finAbs = 0;
            }
            int pos = (int)fpos;
            if (pos >= finVuelta) {
                if (v.loop && fr > 0) {
                    fpos -= (float)(finVuelta - iniVuelta); pos = (int)fpos;
                    if (pos < 0 || pos >= fr) { fpos = (float)iniVuelta; pos = iniVuelta; }
                }
                else                  { v.active = false; break; }
            }
            // la muestra siguiente para interpolar: en loop, pasado el final de la vuelta sigue el inicio
            int pos1 = pos + 1;
            if (pos1 >= finVuelta) pos1 = v.loop ? iniVuelta : pos;
            const float fr1 = fpos - (float)pos;
            // ganancia = volumen * master * la RAMPA de salida (W3dSoundStopFade) * la ENVOLVENTE * el paneo. La rampa
            // y la envolvente avanzan POR MUESTRA: sin click aunque el buffer del backend sea grande. Sin rampa ni
            // envolvente pedidas, valen 1
            float gg = g * v.fadeGain;
            if (v.envEtapa) {
                if (v.envSoltar && s_reloj + (unsigned long long)i >= v.envSoltar) { v.envEtapa = 4; v.envSoltar = 0; }
                switch (v.envEtapa) {
                case 1:
                    v.envNivel += v.envAtaque;
                    if (v.envNivel >= 1.0f) {
                        v.envNivel = 1.0f;
                        if (v.envDecae > 0.0f) v.envEtapa = 2;
                        else { v.envEtapa = 3; v.envNivel = v.envSost; }
                    }
                    break;
                case 2:
                    v.envNivel *= v.envDecae;
                    if (v.envNivel <= v.envSost || v.envNivel < 0.001f) {
                        if (v.envNivel < v.envSost) v.envNivel = v.envSost;
                        v.envEtapa = 3;
                    }
                    break;
                case 3: v.envNivel *= v.envCaida; break;
                default: v.envNivel *= v.envLibera; break;
                }
                if (v.envEtapa >= 3 && v.envNivel < 0.001f) { v.active = false; break; }   // bajo -60 dB: voz libre
                gg *= v.envNivel;
            }
            float l0, l1, r0, r1;
            if (mono) { l0 = r0 = (float)p[pos]; l1 = r1 = (float)p[pos1]; }
            else {
                l0 = (float)p[pos * 2];     l1 = (float)p[pos1 * 2];
                r0 = (float)p[pos * 2 + 1]; r1 = (float)p[pos1 * 2 + 1];
            }
            const float l = (l0 + (l1 - l0) * fr1) * gg * v.gL;
            const float r = (r0 + (r1 - r0) * fr1) * gg * v.gR;
            acc[i * 2]     += (int)l;
            acc[i * 2 + 1] += (int)r;
            if (manda) envio[i] += (int)((l + r) * 0.5f * v.envio);
            fpos += v.step;   // paso fraccional = pitch (1.0 en W3dSoundPlay clasico)
            if (v.fadeStep > 0.0f) {
                v.fadeGain -= v.fadeStep;
                if (v.fadeGain <= 0.0f) { v.active = false; break; }   // rampa terminada: voz libre
            }
        }
        v.pos = fpos;
    }
    s_reloj += (unsigned long long)frames;
    if (conReverb) ReverbMezclar(acc, envio, frames);
    // el limitador: ataque instantaneo (nunca recorta en escalon), recuperacion exponencial ~0.3 s
    const float recup = 1.0f / (0.3f * (float)s_rate);
    for (int i = 0; i < frames; i++) {
        int l = acc[i * 2], r = acc[i * 2 + 1];
        int pico = l < 0 ? -l : l;
        const int ar = r < 0 ? -r : r;
        if (ar > pico) pico = ar;
        if (pico > 32767) {
            const float nec = 32767.0f / (float)pico;
            if (nec < s_limGain) s_limGain = nec;
        }
        if (s_limGain < 1.0f) {
            s_limGain += (1.0f - s_limGain) * recup;
            l = (int)((float)l * s_limGain);
            r = (int)((float)r * s_limGain);
        }
        if (l < -32768) l = -32768; else if (l > 32767) l = 32767;
        if (r < -32768) r = -32768; else if (r > 32767) r = 32767;
        out[i * 2]     = (short)l;
        out[i * 2 + 1] = (short)r;
    }
}

void W3dAudioMix(short* out, int frames) {
    if (frames <= 0) return;
    if (s_pausa) { for (int i = 0; i < frames * 2; i++) out[i] = 0; return; }   // pausa: silencio, nada avanza
    Mezclar(out, frames);
}

bool W3dAudioGrabar(short* out, int frames) {
    if (!s_ready || frames <= 0) return false;
    W3dAudioBackendLock();
    Mezclar(out, frames);
    W3dAudioBackendUnlock();
    return true;
}

#else // ---------------- modulo apagado: stubs ----------------

bool W3dAudioInit(int) { return false; }
void W3dAudioShutdown() {}
W3dSound* W3dSoundLoad(const char*) { return 0; }
W3dSound* W3dSoundLoadMemory(const void*, size_t) { return 0; }
W3dSound* W3dSoundBeep(float, int, float) { return 0; }
void W3dSoundFree(W3dSound*) {}
int  W3dSoundPlay(W3dSound*, float, bool) { return 0; }
int  W3dSoundPlayPitch(W3dSound*, float, bool, float) { return 0; }
int  W3dSoundPlayPitchPan(W3dSound*, float, bool, float, float) { return 0; }
void W3dSoundStop(int) {}
void W3dSoundStopFade(int, float) {}
void W3dSoundStopFadeAt(int, float, double) {}
int  W3dSoundPlayAt(W3dSound*, float, bool, float, float, double) { return 0; }
double W3dAudioReloj() { return 0.0; }
void W3dSoundStopAll() {}
int  W3dSoundPlayEnv(W3dSound*, float, bool, float, float, double, const W3dEnvolvente*, float) { return 0; }
void W3dSoundSoltar(int) {}
void W3dSoundSetEnvio(int, float) {}
void W3dAudioReverb(float, float, float) {}
int  W3dAudioRate() { return 0; }
bool W3dAudioGrabar(short*, int) { return false; }
void W3dAudioPause(bool) {}
bool W3dAudioPaused() { return false; }
void W3dSoundSetVolume(int, float) {}
void W3dSoundSetPitch(int, float) {}
float W3dSoundPos(int) { return -1.0f; }
void W3dAudioMasterVolume(float) {}
void W3dAudioMix(short*, int) {}

#endif

} // namespace w3dEngine
