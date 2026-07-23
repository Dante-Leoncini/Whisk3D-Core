#ifndef W3D_VOLUMEN_H
#define W3D_VOLUMEN_H
// ============================================================================
//  VOLUMEN GLOBAL del motor — vale para TODO lo que suena: la MUSICA (W3dMusic.h)
//  y los EFECTOS (W3dAudio.h). Esta unidad SIEMPRE compila (no depende de que los
//  modulos de audio esten activados).
//
//  Por que vive en el Core y no en cada juego: el volumen y el mute son del USUARIO,
//  no de un juego. Valen para toda la app y se GUARDAN SOLOS en la configuracion
//  persistente (base/W3dConfig.h: localStorage en web, archivo en el resto) -> al
//  volver a entrar siguen como los dejo.
//
//  Volumen efectivo de una pista = volumen propio de la pista x VolumenGanancia().
//  Cambiar el maestro RE-APLICA el volumen a todo lo que ya esta sonando.
//
//  Claves en la config: "audio.vol" (0..1) y "audio.mute" (0/1).
// ============================================================================

namespace w3dEngine {

// Lee volumen/mute de la config (hace ConfigLoad si hace falta) y los aplica.
void VolumenInit();

void  VolumenSet(float v);   // 0..1 — volumen MAESTRO. Guarda y re-aplica.
float VolumenGet();

void  VolumenSetMute(bool m);       // silencio total. Guarda y re-aplica.
bool  VolumenMute();

// Multiplicador a aplicar a cada pista: el volumen maestro, o 0 si esta muteado.
float VolumenGanancia();

} // namespace w3dEngine

#endif // W3D_VOLUMEN_H
