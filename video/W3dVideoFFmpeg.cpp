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

namespace w3dEngine {

class W3dVideoFFmpeg : public W3dVideo {
public:
    W3dVideoFFmpeg() : fmt(0), codecCtx(0), swsCtx(0), hwDev(0), frame(0), swFrame(0),
                       pkt(0), rgba(0), tex(0), vStream(-1), w(0), h(0), alpha(false),
                       t0(-1.0), ok(false) {}

    bool Open(const char* path, bool doLoop) {
        loop = doLoop;
        if (avformat_open_input(&fmt, path, 0, 0) != 0) return false;
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
        uint8_t* dstData[4] = { rgba, 0, 0, 0 };
        int dstLines[4] = { w * 4, 0, 0, 0 };
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
    int  vStream, w, h;
    bool alpha, loop, ok;
    double t0;
};

W3dVideo* W3dVideoOpenBackend(const char* path, bool loop) {
    W3dVideoFFmpeg* v = new W3dVideoFFmpeg();
    if (!v->Open(path, loop)) { delete v; return 0; }
    return v;
}

} // namespace w3dEngine

#endif // W3D_ENABLE_VIDEO && !__EMSCRIPTEN__ && !W3D_SYMBIAN
