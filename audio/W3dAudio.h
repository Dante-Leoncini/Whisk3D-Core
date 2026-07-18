#ifndef W3D_AUDIO_H
#define W3D_AUDIO_H

#include <stddef.h> // size_t

// ============================================================================
//  Motor de AUDIO de Whisk3D. MODULO OPCIONAL (solo con -DW3D_ENABLE_AUDIO).
//
//  Arquitectura (la misma que usa Quake, y que corre hasta en el N95):
//   - MIXER PORTABLE (en el Core): mezcla por software todas las voces activas a un
//     stream PCM stereo 16-bit. Puro C++03: identico en todas las plataformas.
//   - SALIDA por plataforma, FINA: solo le pide al mixer el proximo buffer.
//       * web / linux / windows / android -> SDL2 audio (W3dAudioSDL.cpp)
//       * Symbian (N95)                    -> CMdaAudioOutputStream (W3dAudioSymbian.cpp)
//
//  Un sonido se carga a memoria, se RESAMPLEA al rate del mixer y se guarda como
//  stereo 16-bit; reproducirlo = agregar una "voz". Sirve para EFECTOS y para MUSICA
//  (loop=true). Los bytes pueden venir de un archivo o de un .w3dpack cifrado
//  (io/W3dPack.h) -> tambien podes proteger el audio.
//
//  Sin el flag, todas las funciones son stubs no-op: cero codigo, cero dependencias.
// ============================================================================

namespace w3dEngine {

// Arranca el motor. sampleRate = frecuencia del mixer (44100 en desktop/web; 22050 en el
// N95, mas liviano). false si el backend no pudo abrir el dispositivo (la app sigue muda).
bool W3dAudioInit(int sampleRate);
void W3dAudioShutdown();

// Un sonido ya decodificado y resampleado al rate del mixer (stereo 16-bit en memoria).
class W3dSound;

// Carga un WAV (PCM 8/16-bit, mono/estereo, cualquier rate: se resamplea). NULL si falla.
W3dSound* W3dSoundLoad(const char* path);
// Igual, desde bytes EN MEMORIA (ej: descifrados de un .w3dpack -> audio protegido).
W3dSound* W3dSoundLoadMemory(const void* bytes, size_t len);
void      W3dSoundFree(W3dSound* s);

// Reproduce 's'. Devuelve un id de VOZ (>0) para pararla/ajustarla, o 0 si no hay lugar.
// volume 0..1; loop=true para musica/ambiente (se repite hasta W3dSoundStop).
int  W3dSoundPlay(W3dSound* s, float volume, bool loop);
void W3dSoundStop(int voice);        // para una voz por su id (no-op si ya termino)
void W3dSoundStopAll();
void W3dSoundSetVolume(int voice, float volume);
void W3dAudioMasterVolume(float v);  // volumen global 0..1

// Lo llama el BACKEND de salida para llenar su buffer (frames = pares L/R, stereo 16-bit).
// Lo define el mixer del Core; corre en el hilo de audio (bajo exclusion del backend).
void W3dAudioMix(short* stereoOut, int frames);

} // namespace w3dEngine

#endif
