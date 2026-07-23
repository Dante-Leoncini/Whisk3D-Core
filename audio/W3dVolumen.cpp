// ============================================================================
//  W3dVolumen.cpp — volumen global + mute, persistidos. Ver W3dVolumen.h.
//
//  Siempre compila. Reenvia el cambio a los modulos de audio QUE ESTEN activados:
//    · musica  -> W3dMusicRefrescarVolumenes() (re-aplica a las pistas sonando)
//    · efectos -> W3dAudioMasterVolume()       (solo con -DW3D_ENABLE_AUDIO)
// ============================================================================
#include "W3dVolumen.h"
#include "W3dMusic.h"
#include "../base/W3dConfig.h"

#ifdef W3D_ENABLE_AUDIO
    #include "W3dAudio.h"
#endif

namespace w3dEngine {

static float gVol   = 0.6f;
static bool  gMute  = false;
static bool  gListo = false;

static void aplicar() {
    W3dMusicRefrescarVolumenes();          // pistas de musica ya sonando
#ifdef W3D_ENABLE_AUDIO
    W3dAudioMasterVolume(VolumenGanancia()); // mixer de efectos
#endif
}

// Version del bloque de audio en la config. Sirve para MIGRAR ajustes viejos.
enum { AUDIO_CFG_VER = 2 };

void VolumenInit() {
    if (gListo) return;
    gListo = true;
    ConfigLoad();                          // idempotente: si ya estaba cargada no molesta
    gVol  = ConfigGetFloat("audio.vol", 0.6f);
    gMute = ConfigGetInt("audio.mute", 0) != 0;
    // MIGRACION: hasta la version 1, el mute se GUARDABA aunque en algunas plataformas no se
    // aplicara (en iOS el .volume de un <audio> se ignora), asi que pudo quedar grabado un
    // silencio que el usuario nunca llego a escuchar. Se limpia UNA vez.
    if (ConfigGetInt("audio.ver", 1) < AUDIO_CFG_VER) {
        gMute = false;
        ConfigSetInt("audio.mute", 0);
        ConfigSetInt("audio.ver", AUDIO_CFG_VER);
        ConfigSave();
    }
    if (gVol < 0.0f) gVol = 0.0f;
    if (gVol > 1.0f) gVol = 1.0f;
    aplicar();
}

float VolumenGet() { return gVol; }
bool  VolumenMute()    { return gMute; }
float VolumenGanancia(){ return gMute ? 0.0f : gVol; }

void VolumenSet(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    gVol = v;
    ConfigSetFloat("audio.vol", v);
    ConfigSave();
    aplicar();
}

void VolumenSetMute(bool m) {
    gMute = m;
    ConfigSetInt("audio.mute", m ? 1 : 0);
    ConfigSave();
    aplicar();
}

} // namespace w3dEngine
