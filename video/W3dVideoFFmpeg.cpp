// ============================================================================
//  Backend DESKTOP de video (FFmpeg). Decodifica por HARDWARE cuando se puede.
//
//  DEPENDENCIA (por eso el modulo es opcional): headers + libs de FFmpeg
//  (libavformat, libavcodec, libavutil, libswscale). Se linkea SOLO cuando el
//  build define W3D_ENABLE_VIDEO (CMake: -DWHISK3D_VIDEO=ON).
//
//  Estrategia:
//   - Aceleracion por HW via av_hwdevice (d3d11va en Windows, vaapi en Linux) con
//     FALLBACK a software si no hay HW. El decode pesado (entropia, motion-comp) lo
//     hace la GPU; bajamos el frame a RAM (av_hwframe_transfer) y lo pasamos a RGBA
//     con swscale. No es zero-copy (eso pide interop D3D11/GL), pero el trabajo caro
//     es HW.
//   - ALPHA: los decoders HW de VP9 descartan el canal alpha (el alpha de VP9 es un
//     stream lateral). Si el video trae alpha (formato yuva*), forzamos SOFTWARE para
//     no perderlo. El fondo (opaco) si aprovecha HW.
//   - LOOP: al llegar a EOF hace seek al principio.
// ============================================================================
#if defined(W3D_ENABLE_VIDEO) && !defined(__EMSCRIPTEN__) && !defined(W3D_SYMBIAN)

#include "W3dVideo.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>
#include <cstring>
#include <cstdio> // SEEK_SET/CUR/END

namespace w3dEngine {

// IO sobre un buffer en MEMORIA (para assets descifrados de un .w3dpack): FFmpeg lee
// el contenedor desde RAM, sin archivo temporal ni ruta.
struct W3dMemIO { const unsigned char* data; size_t size, pos; };
static int w3dMemRead(void* opaque, uint8_t* buf, int bufSize) {
    W3dMemIO* m = (W3dMemIO*)opaque;
    size_t remain = m->size - m->pos;
    if (remain == 0) return AVERROR_EOF;
    int n = bufSize; if ((size_t)n > remain) n = (int)remain;
    memcpy(buf, m->data + m->pos, n); m->pos += n; return n;
}
static int64_t w3dMemSeek(void* opaque, int64_t offset, int whence) {
    W3dMemIO* m = (W3dMemIO*)opaque;
    if (whence == AVSEEK_SIZE) return (int64_t)m->size;
    int64_t np;
    if (whence == SEEK_SET)      np = offset;
    else if (whence == SEEK_CUR) np = (int64_t)m->pos + offset;
    else if (whence == SEEK_END) np = (int64_t)m->size + offset;
    else return -1;
    if (np < 0 || np > (int64_t)m->size) return -1;
    m->pos = (size_t)np; return np;
}

class W3dVideoFFmpeg : public W3dVideo {
public:
    W3dVideoFFmpeg() : fmt(0), codecCtx(0), swsCtx(0), hwDev(0), frame(0), swFrame(0),
                       pkt(0), rgba(0), tex(0), avio(0), ioBuf(0), ownedBytes(0),
                       vStream(-1), w(0), h(0), alpha(false), loop(false), ok(false), t0(-1.0) {
        mem.data = 0; mem.size = 0; mem.pos = 0;
    }

    bool Open(const char* path, bool doLoop) {
        loop = doLoop;
        if (avformat_open_input(&fmt, path, 0, 0) != 0) return false;
        return OpenComun();
    }

    // Abre el contenedor desde bytes en memoria (assets protegidos). Copia los bytes:
    // se hace duenio, asi el caller puede liberar el .w3dpack cuando quiera.
    bool OpenMem(const void* bytes, size_t len, bool doLoop) {
        loop = doLoop;
        ownedBytes = (unsigned char*)av_malloc(len ? len : 1);
        memcpy(ownedBytes, bytes, len);
        mem.data = ownedBytes; mem.size = len; mem.pos = 0;
        const int bufSz = 1 << 16;
        ioBuf = (unsigned char*)av_malloc(bufSz);
        avio = avio_alloc_context(ioBuf, bufSz, 0, &mem, w3dMemRead, 0, w3dMemSeek);
        if (!avio) return false;
        fmt = avformat_alloc_context();
        fmt->pb = avio;
        if (avformat_open_input(&fmt, 0, 0, 0) != 0) return false;
        return OpenComun();
    }

    // Comun a Open(path) y OpenMem: ya tenemos 'fmt' abierto.
    bool OpenComun() {
        if (avformat_find_stream_info(fmt, 0) < 0) return false;
        const AVCodec* codec = 0;
        vStream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (vStream < 0 || !codec) return false;

        codecCtx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codecCtx, fmt->streams[vStream]->codecpar);

        // el video trae alpha? (yuva420p / yuva444p ...) -> SW para no perderlo
        AVPixelFormat sfmt = (AVPixelFormat)fmt->streams[vStream]->codecpar->format;
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(sfmt);
        alpha = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);

        if (!alpha) TryInitHW(); // solo videos opacos usan HW (los con alpha van por SW)

        if (avcodec_open2(codecCtx, codec, 0) < 0) return false;

        frame   = av_frame_alloc();
        swFrame = av_frame_alloc();
        pkt     = av_packet_alloc();

        glGenTextures(1, &tex);
        ok = true;
        return true;
    }

    ~W3dVideoFFmpeg() {
        if (tex) glDeleteTextures(1, &tex);
        if (rgba) av_free(rgba);
        if (swsCtx) sws_freeContext(swsCtx);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (swFrame) av_frame_free(&swFrame);
        if (hwDev) av_buffer_unref(&hwDev);
        if (codecCtx) avcodec_free_context(&codecCtx);
        if (fmt) avformat_close_input(&fmt);
        if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); } // AVIO propio (memoria): lo liberamos nosotros
        if (ownedBytes) av_free(ownedBytes);
    }

    bool Update(double nowMs) {
        if (!ok) return false;
        if (t0 < 0) t0 = nowMs;
        AVRational tb = fmt->streams[vStream]->time_base;

        // decodifica hasta alcanzar el frame que corresponde al reloj (o hasta que no haya mas listo)
        for (int guard = 0; guard < 64; guard++) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) { if (loop) { Rewind(); return false; } else return false; }
            if (pkt->stream_index != vStream) { av_packet_unref(pkt); continue; }

            avcodec_send_packet(codecCtx, pkt);
            av_packet_unref(pkt);

            while (avcodec_receive_frame(codecCtx, frame) == 0) {
                AVFrame* src = frame;
                if (frame->hw_frames_ctx) { // frame en GPU: bajar a RAM
                    av_hwframe_transfer_data(swFrame, frame, 0);
                    src = swFrame;
                }
                UploadFrame(src);
                av_frame_unref(frame);
                return true; // un frame nuevo por Update (pacing simple; el caller llama cada frame)
            }
        }
        return false;
    }

    unsigned int Texture() const { return tex; }
    int  Width()    const { return w; }
    int  Height()   const { return h; }
    bool HasAlpha() const { return alpha; }

private:
    void TryInitHW() {
#ifdef _WIN32
        AVHWDeviceType type = AV_HWDEVICE_TYPE_D3D11VA;
#else
        AVHWDeviceType type = AV_HWDEVICE_TYPE_VAAPI;
#endif
        if (av_hwdevice_ctx_create(&hwDev, type, 0, 0, 0) == 0)
            codecCtx->hw_device_ctx = av_buffer_ref(hwDev);
        // si falla, codecCtx->hw_device_ctx queda NULL -> decode por software, todo sigue funcionando
    }

    void UploadFrame(AVFrame* f) {
        w = f->width; h = f->height;
        AVPixelFormat dst = AV_PIX_FMT_RGBA;
        swsCtx = sws_getCachedContext(swsCtx, w, h, (AVPixelFormat)f->format,
                                      w, h, dst, SWS_BILINEAR, 0, 0, 0);
        if (!rgba) rgba = (uint8_t*)av_malloc(av_image_get_buffer_size(dst, w, h, 1));
        // flip vertical (stride negativo desde la ultima fila): deja la textura bottom-up
        // como GL espera, igual que el backend web (UNPACK_FLIP_Y). Asi la demo usa las
        // mismas UVs en las dos plataformas.
        uint8_t* dstData[4] = { rgba + (size_t)(h - 1) * w * 4, 0, 0, 0 };
        int dstLines[4] = { -w * 4, 0, 0, 0 };
        sws_scale(swsCtx, f->data, f->linesize, 0, h, dstData, dstLines);

        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    void Rewind() {
        av_seek_frame(fmt, vStream, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecCtx);
        t0 = -1.0;
    }

    AVFormatContext* fmt;
    AVCodecContext*  codecCtx;
    SwsContext*      swsCtx;
    AVBufferRef*     hwDev;
    AVFrame*         frame;
    AVFrame*         swFrame;
    AVPacket*        pkt;
    uint8_t*         rgba;
    unsigned int     tex;
    AVIOContext*     avio;       // IO en memoria (assets protegidos); 0 si se abrio por archivo
    unsigned char*   ioBuf;
    unsigned char*   ownedBytes; // copia propia de los bytes descifrados
    W3dMemIO         mem;
    int  vStream, w, h;
    bool alpha, loop, ok;
    double t0;
};

W3dVideo* W3dVideoOpenBackend(const char* path, bool loop) {
    W3dVideoFFmpeg* v = new W3dVideoFFmpeg();
    if (!v->Open(path, loop)) { delete v; return 0; }
    return v;
}

W3dVideo* W3dVideoOpenMemoryBackend(const void* bytes, size_t len, const char* /*mime*/, bool loop) {
    W3dVideoFFmpeg* v = new W3dVideoFFmpeg(); // FFmpeg detecta el contenedor solo; el mime no hace falta
    if (!v->OpenMem(bytes, len, loop)) { delete v; return 0; }
    return v;
}

} // namespace w3dEngine

#endif // W3D_ENABLE_VIDEO && !__EMSCRIPTEN__ && !W3D_SYMBIAN
