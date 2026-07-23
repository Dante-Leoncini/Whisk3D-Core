// ============================================================================
//  W3dMusic.cpp — DISPATCHER del modulo de musica. Esta unidad SIEMPRE compila.
//  Con -DW3D_ENABLE_MUSIC delega en el backend de la plataforma; sin el flag son
//  stubs que devuelven NULL (la app sigue muda, sin dependencias). Ver W3dMusic.h.
// ============================================================================
#include "W3dMusic.h"
#include "W3dMusicBackend.h"   // contrato compartido con los backends
#include "W3dVolumen.h"

namespace w3dEngine {

// --- registro de pistas vivas (sin memoria dinamica: sirve igual en Symbian) ---
enum { W3D_MUSIC_MAX = 8 };
static W3dMusic* gPistas[W3D_MUSIC_MAX] = {0,0,0,0,0,0,0,0};

W3dMusic::W3dMusic() : propio(1.0f) {
    for (int i = 0; i < W3D_MUSIC_MAX; i++) if (!gPistas[i]) { gPistas[i] = this; break; }
}
W3dMusic::~W3dMusic() {
    for (int i = 0; i < W3D_MUSIC_MAX; i++) if (gPistas[i] == this) { gPistas[i] = 0; break; }
}
void W3dMusic::SetVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    propio = v;
    AplicarVolumen(propio * VolumenGanancia());
}
void W3dMusic::RefrescarVolumen() { AplicarVolumen(propio * VolumenGanancia()); }

void W3dMusicRefrescarVolumenes() {
    for (int i = 0; i < W3D_MUSIC_MAX; i++) if (gPistas[i]) gPistas[i]->RefrescarVolumen();
}

#ifdef W3D_ENABLE_MUSIC

// el contrato con el backend vive en W3dMusicBackend.h (lo incluyen los dos lados)

W3dMusic* W3dMusicPlayMemory(const void* bytes, size_t len, const char* mime, bool loop, float volume) {
    if (!bytes || !len) return 0;
    W3dMusic* m = W3dMusicPlayMemoryBackend(bytes, len, mime, loop, volume);
    if (m) m->SetVolume(volume);   // UNICO aplicador del volumen real (ya multiplica la ganancia global)
    return m;
}
W3dMusic* W3dMusicPlay(const char* path, bool loop, float volume) {
    if (!path) return 0;
    W3dMusic* m = W3dMusicPlayBackend(path, loop, volume);
    if (m) m->SetVolume(volume);   // UNICO aplicador del volumen real
    return m;
}
void W3dMusicUnlock() { W3dMusicUnlockBackend(); }

#else   // sin el modulo: no-op

W3dMusic* W3dMusicPlayMemory(const void*, size_t, const char*, bool, float) { return 0; }
W3dMusic* W3dMusicPlay(const char*, bool, float) { return 0; }
void      W3dMusicUnlock() {}

#endif

} // namespace w3dEngine
