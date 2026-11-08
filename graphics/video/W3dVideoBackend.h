#ifndef W3D_VIDEO_BACKEND_H
#define W3D_VIDEO_BACKEND_H
// ============================================================================
//  CONTRATO dispatcher <-> backend de VIDEO (interno del Core).
//  Lo incluyen W3dVideo.cpp y cada backend (W3dVideoWeb.cpp / W3dVideoFFmpeg.cpp):
//  este contrato compartido es el que caza al COMPILAR bugs como el que hubo de
//  verdad aca: el backend FFmpeg definia OpenMemory con 4 parametros y el
//  dispatcher lo declaraba con 5 -> por name-mangling eran simbolos distintos.
// ============================================================================
#include <stddef.h>

namespace w3dEngine {

class W3dVideo;

W3dVideo* W3dVideoOpenBackend(const char* path, bool loop);
W3dVideo* W3dVideoOpenMemoryBackend(const void* bytes, size_t len, const char* mime, bool loop, bool conSonido);
void      W3dVideoUnlockBackend();

} // namespace w3dEngine

#endif // W3D_VIDEO_BACKEND_H
