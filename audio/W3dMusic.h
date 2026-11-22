#ifndef W3D_MUSIC_H
#define W3D_MUSIC_H

#include <stddef.h> // size_t

// ============================================================================
//  MUSICA (audio COMPRIMIDO en streaming) del motor. MODULO OPCIONAL.
//
//  Por que separamos W3dAudio de W3dSound:
//    - W3dSound = EFECTOS cortos: se decodifican a PCM y los mezcla por software el
//      mixer del Core (portable, control fino de voces). Formato: WAV.
//    - W3dMusic = MUSICA larga: un mp3/ogg/aac de varios minutos como PCM ocuparia
//      decenas de MB en RAM y en el asset. Aca NO se decodifica a PCM: se delega al
//      DECODER NATIVO de la plataforma (normalmente por HARDWARE).
//
//  Backend por plataforma (se elige en COMPILACION, con -DW3D_ENABLE_MUSIC):
//    - Web (emscripten) -> elemento <audio> del navegador.      W3dMusicWeb.cpp
//    - Symbian (N95)    -> CMdaAudioPlayerUtility (mp3/AAC HW). W3dMusicSymbian.cpp  [TODO]
//    - Desktop          -> SDL_mixer / FFmpeg.                  W3dMusicSDL.cpp      [TODO]
//
//  Sin el flag (o sin backend) las funciones devuelven NULL y la app sigue MUDA sin
//  romperse: cero codigo, cero dependencias. Los bytes pueden salir de un .w3dpack
//  cifrado (io/W3dPack.h): la musica puede venir de ahi como cualquier otro asset.
// ============================================================================

namespace w3dEngine {

// Una pista de musica en reproduccion. delete la detiene y libera.
//
// El volumen NO se manda crudo al backend: se multiplica por el VOLUMEN GLOBAL del
// motor (audio/W3dVolume.h), asi el mute y el volumen del usuario valen para todo
// lo que suena. Cada pista recuerda SU volumen propio; si cambia el global, se
// re-aplica sola (W3dMusicRefreshVolumes).
class W3dMusic {
public:
    W3dMusic();                              // se anota en el registro de pistas
    virtual ~W3dMusic();                     // se borra del registro
    virtual void Stop() = 0;                 // detiene (idempotente)
    // Pausa / reanuda. Hace falta porque iOS deja sonar UN SOLO medio a la vez: al reproducir un
    // video con audio, el sistema PAUSA la musica, y despues hay que volver a arrancarla a mano.
    virtual void Pause() {}
    virtual void Resume() {}
    virtual bool Playing() const = 0;

    void  SetVolume(float v);                // 0..1 — volumen PROPIO de esta pista
    float Volume() const { return own; }
    void  RefreshVolume();                // re-aplica propio x ganancia global

protected:
    virtual void ApplyVolume(float v) = 0;  // lo implementa el backend (valor final)
    float own;
};

// Re-aplica el volumen a todas las pistas vivas. La llama W3dVolumen al cambiar el
// volumen/mute global; la app normalmente no necesita llamarla.
void W3dMusicRefreshVolumes();

// Reproduce musica COMPRIMIDA desde bytes EN MEMORIA (ej: descifrados de un .w3dpack).
// 'mime' identifica el contenedor para el backend ("audio/mpeg", "audio/ogg", "audio/aac").
// loop=true para musica de fondo. Devuelve NULL si no hay backend (la app sigue muda).
// Manten los bytes vivos mientras suene (algunos backends leen del buffer original).
W3dMusic* W3dMusicPlayMemory(const void* bytes, size_t len, const char* mime, bool loop, float volume);

// Igual pero desde un archivo/URL (VFS en web, path en desktop/Symbian).
W3dMusic* W3dMusicPlay(const char* path, bool loop, float volume);

// DESBLOQUEO de audio. Llamalo DENTRO del gesto del usuario (toque/click) cuando la musica
// todavia no esta lista (ej: sus bytes se estan descargando). Los navegadores solo dejan
// sonar audio si un elemento fue "estrenado" en un gesto: esto estrena uno con silencio y
// lo reserva, para que un W3dMusicPlayMemory POSTERIOR (ya fuera del gesto) suene igual.
// En las plataformas que no lo necesitan (Symbian, desktop, consolas) no hace nada.
void W3dMusicUnlock();

} // namespace w3dEngine

#endif // W3D_MUSIC_H
