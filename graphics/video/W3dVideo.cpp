#include "W3dVideo.h"
#ifdef W3D_ENABLE_VIDEO
#include "W3dVideoBackend.h"   // contrato compartido con los backends
#endif

// Dispatcher del modulo de video. SIEMPRE se compila (aunque el modulo este apagado):
// cuando W3D_ENABLE_VIDEO no esta definido, W3dVideoOpen es un stub que devuelve NULL,
// asi el resto del motor y las apps enlazan igual sin ningun backend ni dependencia.

namespace w3dEngine {

#ifdef W3D_ENABLE_VIDEO
// Un solo bloque para TODO el modulo prendido (antes habia dos #ifdef consecutivos del mismo flag).
void W3dVideoUnlock() { W3dVideoUnlockBackend(); }

W3dVideo* W3dVideoOpen(const char* path, bool loop) {
    return W3dVideoOpenBackend(path, loop);
}
W3dVideo* W3dVideoOpenMemory(const void* bytes, size_t len, const char* mime, bool loop, bool conSonido) {
    return W3dVideoOpenMemoryBackend(bytes, len, mime, loop, conSonido);
}
#else
// Modulo apagado: sin backend, sin dependencias. La app maneja el NULL (frame estatico o nada).
void W3dVideoUnlock() {}
W3dVideo* W3dVideoOpen(const char* /*path*/, bool /*loop*/) { return 0; }
W3dVideo* W3dVideoOpenMemory(const void*, size_t, const char*, bool, bool) { return 0; }
#endif

} // namespace w3dEngine
