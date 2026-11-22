// ============================================================================
//  W3dVolume.cpp — volumen global + mute, persistidos. Ver W3dVolume.h.
//
//  Siempre compila. Reenvia el cambio a los modulos de audio QUE ESTEN activados:
//    · musica  -> W3dMusicRefreshVolumes() (re-aplica a las pistas sonando)
//    · efectos -> W3dAudioMasterVolume()       (solo con -DW3D_ENABLE_AUDIO)
// ============================================================================
#include "W3dVolume.h"
#include "W3dMusic.h"
#include "../base/W3dConfig.h"

#ifdef W3D_ENABLE_AUDIO
    #include "W3dAudio.h"
#endif

namespace w3dEngine {

static float gVol   = 0.6f;
static bool  gMute  = false;
static bool  gReady = false;

static void apply() {
    W3dMusicRefreshVolumes();          // pistas de musica ya sonando
#ifdef W3D_ENABLE_AUDIO
    W3dAudioMasterVolume(VolumeGain()); // mixer de efectos
#endif
}

// Version del bloque de audio en la config. Sirve para MIGRAR ajustes viejos.
enum { AUDIO_CFG_SHOW = 2 };

void VolumeInit() {
    if (gReady) return;
    gReady = true;
    ConfigLoad();                          // idempotente: si ya estaba cargada no molesta
    gVol  = ConfigGetFloat("audio.vol", 0.6f);
    gMute = ConfigGetInt("audio.mute", 0) != 0;
    // MIGRACION: hasta la version 1, el mute se GUARDABA aunque en algunas plataformas no se
    // aplicara (en iOS el .volume de un <audio> se ignora), asi que pudo quedar grabado un
    // silencio que el usuario nunca llego a escuchar. Se limpia UNA vez.
    if (ConfigGetInt("audio.ver", 1) < AUDIO_CFG_SHOW) {
        gMute = false;
        ConfigSetInt("audio.mute", 0);
        ConfigSetInt("audio.ver", AUDIO_CFG_SHOW);
        ConfigSave();
    }
    if (gVol < 0.0f) gVol = 0.0f;
    if (gVol > 1.0f) gVol = 1.0f;
    apply();
}

float VolumeGet() { return gVol; }
bool  VolumeMute()    { return gMute; }
float VolumeGain(){ return gMute ? 0.0f : gVol; }

void VolumeSet(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    gVol = v;
    ConfigSetFloat("audio.vol", v);
    ConfigSave();
    apply();
}

// Igual que VolumeSet pero SIN persistir: es el volumen de ARRANQUE de un proyecto, no una
// preferencia del usuario. Asi abrir un .w3d con "volumen: 0.5" no le pisa para siempre el
// volumen global que tenia. gReady se marca por si el proyecto se aplica antes que VolumeInit
// (evita que un Init posterior vuelva a leer el 0.6 de la config y tape el valor del proyecto).
void VolumeApplyProject(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    gReady = true;
    gVol = v;
    apply();
}

void VolumeSetMute(bool m) {
    gMute = m;
    ConfigSetInt("audio.mute", m ? 1 : 0);
    ConfigSave();
    apply();
}

} // namespace w3dEngine
