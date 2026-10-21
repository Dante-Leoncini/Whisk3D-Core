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
//  stereo 16-bit; reproducirlo = agregar una "voz". El mixer acumula las voces en 32 bits
//  con interpolacion LINEAL entre muestras (pitch fraccional sin el "grano" del nearest) y
//  pasa la suma por un LIMITADOR de picos (ataque instantaneo, recuperacion ~0.3 s) antes
//  de bajar a 16 bits: muchas voces a la vez (un secuenciador de musica) ya no saturan ni
//  distorsionan como con el recorte duro de antes. Sirve para EFECTOS y para MUSICA
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
// Igual, desde bytes EN MEMORIA (por ejemplo salidos de un .w3dpack).
W3dSound* W3dSoundLoadMemory(const void* bytes, size_t len);
// Genera un TONO de onda cuadrada (beep) al rate del mixer. freq en Hz, dur en ms, vol 0..1.
// Copyright-free (matematica pura). Lo usa el juego para efectos estilo WhiskPaddle sin archivos de audio.
W3dSound* W3dSoundBeep(float freq, int ms, float vol);
void      W3dSoundFree(W3dSound* s);

// Reproduce 's'. Devuelve un id de VOZ (>0) para pararla/ajustarla, o 0 si no hay lugar.
// volume 0..1; loop=true para musica/ambiente (se repite hasta W3dSoundStop).
int  W3dSoundPlay(W3dSound* s, float volume, bool loop);
// Igual pero con PITCH: la voz avanza con paso fraccional 'pitch' (1.0 = normal, 0.5 = una
// octava abajo, 2.0 = una arriba). Lo usa el bind lua sonido() para los pitchs de un juego
// (rate_Hz = pitch/4096*11025 en el PSX -> factor pitch/1024 aca). Sin filtro: nearest,
// suficiente para efectos (mismo caracter lo-fi que el original).
int  W3dSoundPlayPitch(W3dSound* s, float volume, bool loop, float pitch);
// Igual, con PANEO: pan -1 (todo a la izquierda) .. 0 (centro) .. 1 (derecha). Ganancias
// gL = 1 - max(pan, 0), gR = 1 + min(pan, 0) (el lado fuerte queda a 1; el otro se atenua
// linealmente), que es como panea una placa de arcade tipo MultiPCM (pan por slot en dB).
int  W3dSoundPlayPitchPan(W3dSound* s, float volume, bool loop, float pitch, float pan);
void W3dSoundStop(int voice);        // para una voz por su id (no-op si ya termino)
// Corta la voz con una RAMPA lineal de 'fadeSeg' segundos (<= 0 -> ~5 ms, el minimo
// anti-click). La rampa corre en el MIXER, por muestra, y al llegar a cero la voz se
// libera sola. Cortar con W3dSoundStop clickea (la onda se corta en cualquier
// amplitud); esta variante la baja a cero antes. Pensada para el SECUENCIADOR de
// musica en lua: notas sostenidas = samples en loop que se apagan por nota.
//   lua: h = sonido("nota.wav", vol, pitch)   -- ahora DEVUELVE el handle de voz
//        pararSonido(h)                       -- corta con el fade default (~5 ms)
//        pararSonido(h, 0.25)                 -- o con un release de 250 ms
// El handle es el id (> 0) que devuelven W3dSoundPlay/W3dSoundPlayPitch; en lua,
// sonido() devuelve nil si la voz NO sono (mute global, mixer cerrado, WAV faltante
// o las 64 voces ocupadas) y pararSonido(nil) es un no-op seguro.
void W3dSoundStopFade(int voice, float fadeSeg);
// AGENDA en el RELOJ DEL MIXER (segundos de audio mezclado desde que abrio, sin contar la pausa): la voz arranca /
// empieza su rampa de salida en la MUESTRA exacta de 'enSeg' (<= 0 = ya). Un secuenciador de musica las dispara con
// anticipacion (W3dAudioReloj() + unos ms) y el ritmo no depende de cuando corre el frame del juego.
//   lua: t = relojAudio(); h = sonido(wav, vol, pitch, loop, pan, t + 0.05); pararSonido(h, rampa, t + 0.05 + dur)
double W3dAudioReloj();
int  W3dSoundPlayAt(W3dSound* s, float volume, bool loop, float pitch, float pan, double enSeg);
void W3dSoundStopFadeAt(int voice, float fadeSeg, double enSeg);
void W3dSoundStopAll();

// ENVOLVENTE de una voz: la de los chips de sonido (el AICA de la Dreamcast, el SCSP de la Saturn, el OPL...): ADSR mas
// la CAIDA del sostenido. Tiempos en segundos. El ataque es lineal (de 0 al maximo); despues todo baja en curva
// exponencial (lineal en dB, como bajan los chips): el decaimiento va del maximo al nivel 'sostenido', la caida lo
// sigue bajando mientras la nota sigue (60 dB en 'caida' segundos; 0 = se mantiene) y la liberacion baja 60 dB al SOLTAR
// la nota. La nota se suelta con W3dSoundSoltar, sola a los 'duracion' segundos, o no se suelta (el sonido termina).
// Por defecto (sin envolvente) la voz suena entera a su volumen. Cuando la envolvente baja de -60 dB la voz se libera.
struct W3dEnvolvente {
    float ataque;        // de 0 al maximo (0 = de golpe)
    float decaimiento;   // del maximo al sostenido (0 = salta al sostenido)
    float sostenido;     // nivel 0..1 mientras la nota sigue
    float caida;         // segundos en que el sostenido baja 60 dB aunque la nota siga (0 = no baja)
    float liberacion;    // segundos en que baja 60 dB al soltar la nota (0 = ~5 ms, el corte sin click)
    float duracion;      // la nota se suelta sola a los N segundos de empezar (0 = no se suelta)
    W3dEnvolvente() : ataque(0.0f), decaimiento(0.0f), sostenido(1.0f), caida(0.0f), liberacion(0.0f), duracion(0.0f) {}
};
// Igual que W3dSoundPlayAt con una ENVOLVENTE (NULL = sin) y el ENVIO de la voz a la reverb (0..1: cuanto de su sonido
// entra a la reverb global; lo directo no cambia). Devuelve el id de la voz o 0.
int  W3dSoundPlayEnv(W3dSound* s, float volume, bool loop, float pitch, float pan, double enSeg,
                     const W3dEnvolvente* env, float envioReverb);
// SOLTAR la nota de una voz: su envolvente pasa a la liberacion (una voz sin envolvente se corta con la rampa de ~5 ms)
void W3dSoundSoltar(int voice);
void W3dSoundSetEnvio(int voice, float envioReverb);
// REVERB global (una sala: peines con amortiguacion y pasa-todos, la estructura de Schroeder y Moorer; estereo). Las
// voces le mandan su ENVIO y lo que sale se suma a la mezcla. sala 0..1 = el largo de la cola, amortiguacion 0..1 =
// cuanto se apagan los agudos en la cola, nivel 0..1 = cuanto se suma (0 = apagada: no cuesta nada). En enteros: en un
// telefono sin FPU rapida (o con denormales lentos) cuesta lo mismo que una voz o dos.
void W3dAudioReverb(float sala, float amortiguacion, float nivel);
// el rate del mixer (Hz; 0 si no abrio)
int  W3dAudioRate();
// MEZCLA SIN DISPOSITIVO (pruebas, grabar a WAV): 'frames' frames estereo de lo que suena ahora, como si los pidiera la
// salida (las voces avanzan). Pensado con el mixer en PAUSA (la salida no avanza nada): asi lo grabado es exactamente
// lo que el juego disparo. false si el mixer no abrio.
bool W3dAudioGrabar(short* stereoOut, int frames);
// PAUSA global del mixer: con true las voces se congelan (el mixer saca silencio y NO avanza ninguna
// posicion); con false siguen donde estaban. Para el "Pausa" del editor / del juego: la musica se
// detiene de verdad y retoma del mismo punto. W3dSoundStopAll es el "Parar": corta todo.
void W3dAudioPause(bool pausa);
bool W3dAudioPaused();
void W3dSoundSetVolume(int voice, float volume);
// cambia el PITCH de una voz que ya suena (sin re-dispararla: sigue desde donde iba). Para sonidos
// continuos cuyo tono sigue a algo del juego: el motor de un auto (como hace la placa del arcade,
// que reescribe el registro de tono del slot sin key-on). pitch 0.05..8 como en W3dSoundPlayPitch.
void W3dSoundSetPitch(int voice, float pitch);
// POSICION de reproduccion de una voz: segundos del SONIDO ya mezclados (el reloj real del
// stream, el que escucha el usuario). Avanza en el hilo de audio aunque el render se trabe,
// asi que sirve de reloj maestro para sincronizar una animacion con su audio (frameskip:
// la animacion salta a donde va el sonido). < 0 = la voz no existe (termino / handle viejo).
// Con pitch != 1 son segundos DEL SONIDO (posicion en el wav), no de pared.
float W3dSoundPos(int voice);
void W3dAudioMasterVolume(float v);  // volumen global 0..1 (master de usuario)
void W3dAudioJuegoVolume(float v);   // volumen del JUEGO 0..1 (lo setea la tarjeta Juego; indep. del master)

// Lo llama el BACKEND de salida para llenar su buffer (frames = pares L/R, stereo 16-bit).
// Lo define el mixer del Core; corre en el hilo de audio (bajo exclusion del backend).
void W3dAudioMix(short* stereoOut, int frames);

} // namespace w3dEngine

#endif
