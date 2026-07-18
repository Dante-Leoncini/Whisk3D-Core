#include "W3dAudio.h"
#ifdef W3D_ENABLE_AUDIO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

// El mixer + el loader WAV son PORTABLES. El dispatcher (esta unidad) siempre compila:
// con el flag apagado son stubs no-op, asi la app enlaza sin backend ni dependencias.

namespace w3dEngine {

#ifdef W3D_ENABLE_AUDIO

// --- lo provee el backend de salida (W3dAudioSDL.cpp / W3dAudioSymbian.cpp) ---
bool W3dAudioBackendInit(int sampleRate);   // abre el dispositivo y arranca a pedir W3dAudioMix
void W3dAudioBackendShutdown();
void W3dAudioBackendLock();                  // exclusion con el hilo de audio (SDL_LockAudioDevice / mutex)
void W3dAudioBackendUnlock();

// ---------------------------------------------------------------------------
//  Sonido = PCM stereo 16-bit ya al rate del mixer.
// ---------------------------------------------------------------------------
class W3dSound {
public:
    short* pcm;   // interleaved L,R,L,R...
    int    frames;
    W3dSound() : pcm(0), frames(0) {}
    ~W3dSound() { delete[] pcm; }
};

// ---------------------------------------------------------------------------
//  Estado del mixer.
// ---------------------------------------------------------------------------
struct Voice {
    W3dSound* s;
    int   pos;      // frame actual
    float vol;
    bool  loop;
    bool  active;
    int   id;
};

static const int MAX_VOICES = 32;
static Voice s_voices[MAX_VOICES];
static int   s_rate      = 44100;
static float s_master    = 1.0f;
static int   s_nextId    = 1;
static bool  s_ready     = false;

// ---------------------------------------------------------------------------
//  WAV: parseo desde memoria -> resample/convert a stereo 16-bit al rate del mixer.
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
    size_t off = 12;
    while (off + 8 <= len) {
        const unsigned char* id = b + off;
        unsigned int csz = rdU32(b + off + 4);
        const unsigned char* body = b + off + 8;
        if (memcmp(id, "fmt ", 4) == 0 && csz >= 16) {
            if (rdU16(body) != 1) return 0;           // solo PCM sin comprimir
            channels = (int)rdU16(body + 2);
            srcRate  = (int)rdU32(body + 4);
            bits     = (int)rdU16(body + 14);
        } else if (memcmp(id, "data", 4) == 0) {
            data = body; dataLen = csz;
            if (off + 8 + dataLen > len) dataLen = len - (off + 8); // tolerar tamano pasado
        }
        off += 8 + csz + (csz & 1); // los chunks se alinean a 2
    }
    if (!data || channels < 1 || srcRate < 1 || (bits != 8 && bits != 16)) return 0;

    int srcFrames = (int)(dataLen / ((size_t)channels * (bits / 8)));
    if (srcFrames < 1) return 0;
    // resample lineal a s_rate + a stereo 16-bit
    long outFrames = (long)((double)srcFrames * s_rate / srcRate);
    if (outFrames < 1) outFrames = 1;
    short* out = new short[(size_t)outFrames * 2];
    for (long i = 0; i < outFrames; i++) {
        double t  = (double)i * srcRate / s_rate;
        int    i0 = (int)t;
        double fr = t - i0;
        for (int c = 0; c < 2; c++) {
            short a = srcSample(data, srcFrames, channels, bits, i0,     c);
            short bb = srcSample(data, srcFrames, channels, bits, i0 + 1, c);
            out[i * 2 + c] = (short)(a + (short)((bb - a) * fr));
        }
    }
    W3dSound* s = new W3dSound();
    s->pcm = out; s->frames = (int)outFrames;
    return s;
}

// ---------------------------------------------------------------------------
//  API publica
// ---------------------------------------------------------------------------
bool W3dAudioInit(int sampleRate) {
    if (s_ready) return true;
    s_rate = sampleRate > 0 ? sampleRate : 44100;
    for (int i = 0; i < MAX_VOICES; i++) s_voices[i].active = false;
    s_ready = W3dAudioBackendInit(s_rate);
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

W3dSound* W3dSoundLoad(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    unsigned char* buf = new unsigned char[n];
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    W3dSound* s = (rd == (size_t)n) ? decodeWav(buf, (size_t)n) : 0;
    delete[] buf;
    return s;
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
    if (!s_ready || !s) return 0;
    int id = 0;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!s_voices[i].active) {
            Voice& v = s_voices[i];
            v.s = s; v.pos = 0; v.vol = volume; v.loop = loop; v.active = true;
            v.id = id = s_nextId++;
            if (s_nextId <= 0) s_nextId = 1;
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

void W3dSoundStopAll() {
    if (!s_ready) return;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++) s_voices[i].active = false;
    W3dAudioBackendUnlock();
}

void W3dSoundSetVolume(int voice, float volume) {
    if (!s_ready || voice <= 0) return;
    W3dAudioBackendLock();
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active && s_voices[i].id == voice) s_voices[i].vol = volume;
    W3dAudioBackendUnlock();
}

void W3dAudioMasterVolume(float v) { s_master = v < 0 ? 0 : (v > 1 ? 1 : v); }

// Corre en el hilo de audio (el backend garantiza exclusion con los cambios de voces).
void W3dAudioMix(short* out, int frames) {
    for (int i = 0; i < frames * 2; i++) out[i] = 0;
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice& v = s_voices[vi];
        if (!v.active) continue;
        float g = v.vol * s_master;
        const short* p = v.s->pcm;
        int fr = v.s->frames;
        int pos = v.pos;
        for (int i = 0; i < frames; i++) {
            if (pos >= fr) {
                if (v.loop) { pos = 0; if (fr == 0) { v.active = false; break; } }
                else        { v.active = false; break; }
            }
            int l = out[i * 2]     + (int)(p[pos * 2]     * g);
            int r = out[i * 2 + 1] + (int)(p[pos * 2 + 1] * g);
            if (l < -32768) l = -32768; else if (l > 32767) l = 32767;
            if (r < -32768) r = -32768; else if (r > 32767) r = 32767;
            out[i * 2]     = (short)l;
            out[i * 2 + 1] = (short)r;
            pos++;
        }
        v.pos = pos;
    }
}

#else // ---------------- modulo apagado: stubs ----------------

bool W3dAudioInit(int) { return false; }
void W3dAudioShutdown() {}
W3dSound* W3dSoundLoad(const char*) { return 0; }
W3dSound* W3dSoundLoadMemory(const void*, size_t) { return 0; }
void W3dSoundFree(W3dSound*) {}
int  W3dSoundPlay(W3dSound*, float, bool) { return 0; }
void W3dSoundStop(int) {}
void W3dSoundStopAll() {}
void W3dSoundSetVolume(int, float) {}
void W3dAudioMasterVolume(float) {}
void W3dAudioMix(short*, int) {}

#endif

} // namespace w3dEngine
