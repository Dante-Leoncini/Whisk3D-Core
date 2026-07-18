#include "W3dVideo.h"

// Dispatcher del modulo de video. SIEMPRE se compila (aunque el modulo este apagado):
// cuando W3D_ENABLE_VIDEO no esta definido, W3dVideoOpen es un stub que devuelve NULL,
// asi el resto del motor y las apps enlazan igual sin ningun backend ni dependencia.

namespace w3dEngine {

#ifdef W3D_ENABLE_VIDEO
// Los define el backend de la plataforma (W3dVideoWeb.cpp / W3dVideoFFmpeg.cpp / ...).
W3dVideo* W3dVideoOpenBackend(const char* path, bool loop);
W3dVideo* W3dVideoOpenMemoryBackend(const void* bytes, size_t len, const char* mime, bool loop);

W3dVideo* W3dVideoOpen(const char* path, bool loop) {
    return W3dVideoOpenBackend(path, loop);
}
W3dVideo* W3dVideoOpenMemory(const void* bytes, size_t len, const char* mime, bool loop) {
    return W3dVideoOpenMemoryBackend(bytes, len, mime, loop);
}
#else
// Modulo apagado: sin backend, sin dependencias. La app maneja el NULL (frame estatico o nada).
W3dVideo* W3dVideoOpen(const char* /*path*/, bool /*loop*/) { return 0; }
W3dVideo* W3dVideoOpenMemory(const void*, size_t, const char*, bool) { return 0; }
#endif

} // namespace w3dEngine
