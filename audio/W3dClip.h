#ifndef W3D_CLIP_H
#define W3D_CLIP_H

#include <stddef.h> // size_t

// ============================================================================
//  CLIP — sonido CORTO de interfaz (clic, swipe, "tuk" de un boton).
//
//  Existe aparte de W3dSound porque en el navegador conviene un camino distinto:
//  el clip se decodifica UNA vez a un AudioBuffer y cada disparo cuesta casi nada
//  (un BufferSource). Lo que NO hay que hacer en iOS es dispararlo con elementos
//  <audio>: cada play() arranca el pipeline de media y con varios seguidos (un
//  fling del menu) el telefono se traba.
//
//  Los dos requisitos del navegador los resuelve W3dClipInstallUnlock():
//    1) el AudioContext nace suspendido y solo se reanuda DENTRO del gesto del
//       usuario; los eventos del motor se procesan despues, ya fuera del gesto.
//    2) iOS silencia WebAudio con la palanca de silencio del telefono: se pide la
//       categoria "playback" (navigator.audioSession) para que igual suene.
//
//  En el resto de las plataformas (desktop, Symbian, consolas) un clip es un
//  sonido del mixer PCM del Core (W3dAudio.h): mismo comportamiento, cero costo.
//
//  El volumen final pasa SIEMPRE por el volumen/mute global (W3dVolume.h).
// ============================================================================

namespace w3dEngine {

class W3dClip;

// Carga un clip desde bytes EN MEMORIA (ej: descifrados de un .w3dpack).
// 'mime' solo lo usa el backend web ("audio/wav", "audio/mpeg"). NULL si falla.
W3dClip* W3dClipLoad(const void* bytes, size_t len, const char* mime);

// Lo dispara. 'vol' 0..1 es el volumen PROPIO del clip (se multiplica por el global).
// Se puede llamar muchas veces seguidas: las repeticiones se reparten en el pool.
void W3dClipPlay(W3dClip* c, float vol);

void W3dClipFree(W3dClip* c);

// DESBLOQUEO: instala (una vez) los enganches que el navegador exige para poder sonar:
// escucha el PRIMER toque/click del usuario y ahi mismo reanuda el audio y "estrena" los
// clips. Hay que instalarlo apenas arranca la app; en el resto de las plataformas no hace
// nada. Es lo unico que corre DENTRO del gesto, que es lo que iOS pide.
void W3dClipInstallUnlock();

} // namespace w3dEngine

#endif // W3D_CLIP_H
