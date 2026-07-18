// ============================================================================
//  Backend de salida SDL2 (web / linux / windows / android). SDL abre el dispositivo
//  y llama al callback cuando necesita mas PCM: ahi le pedimos al mixer del Core.
//  SDL_LockAudioDevice pausa el callback -> lo usamos para tocar las voces sin races.
// ============================================================================
#if defined(W3D_ENABLE_AUDIO) && !defined(W3D_SYMBIAN)

#include "W3dAudio.h"
#include <SDL2/SDL.h>

namespace w3dEngine {

static SDL_AudioDeviceID s_dev = 0;

static void sdlCallback(void* /*user*/, Uint8* stream, int len) {
    W3dAudioMix((short*)stream, len / 4); // 4 bytes por frame (stereo 16-bit)
}

bool W3dAudioBackendInit(int rate) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq     = rate;
    want.format   = AUDIO_S16SYS; // stereo 16-bit nativo, como el mixer
    want.channels = 2;
    want.samples  = 1024;         // buffer chico -> baja latencia
    want.callback = sdlCallback;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); // 0 = SDL entrega exactamente este formato
    if (s_dev == 0) return false;
    SDL_PauseAudioDevice(s_dev, 0); // arranca la reproduccion
    return true;
}

void W3dAudioBackendShutdown() {
    if (s_dev) { SDL_CloseAudioDevice(s_dev); s_dev = 0; }
}

void W3dAudioBackendLock()   { if (s_dev) SDL_LockAudioDevice(s_dev); }
void W3dAudioBackendUnlock() { if (s_dev) SDL_UnlockAudioDevice(s_dev); }

} // namespace w3dEngine

#endif // W3D_ENABLE_AUDIO && !W3D_SYMBIAN
