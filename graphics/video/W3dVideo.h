#ifndef W3D_VIDEO_H
#define W3D_VIDEO_H

#include <stddef.h> // size_t

// ============================================================================
//  Reproduccion de VIDEO en el motor. MODULO OPCIONAL.
//
//  Solo existe si se compila con -DW3D_ENABLE_VIDEO. Sin ese flag, W3dVideoOpen()
//  devuelve NULL y la app degrada sola (frame estatico o nada): cero codigo, cero
//  dependencias. Por eso Symbian y los builds minimos no pagan nada por esto.
//
//  La interfaz es AGNOSTICA al codec: una fuente de frames RGBA en el tiempo. El
//  backend que decodifica se elige EN COMPILACION segun la plataforma:
//    - Web (emscripten): elemento <video> del navegador -> decode por HARDWARE
//      (VP9/AV1/H.264, con alpha si el WebM lo trae). Backend: W3dVideoWeb.cpp
//    - Desktop (Win/Linux): FFmpeg con aceleracion por hardware. W3dVideoFFmpeg.cpp
//    - Symbian: CPU a baja resolucion (futuro).
//
//  El motor deja el frame actual en una TEXTURA GL; el editor/ejemplo solo dibuja
//  esa textura. La composicion (un video con alpha SOBRE otro) es render normal:
//  dos quads texturizados, el de atras opaco y el de adelante con alpha blending.
//  No hay codigo de "video compositing" especial.
// ============================================================================

namespace w3dEngine {

// Fuente de frames de video. La implementacion concreta la da el backend de la plataforma.
class W3dVideo {
public:
    virtual ~W3dVideo() {}

    // Pone en la textura el frame que corresponde al reloj de pared 'nowMs' (loop
    // interno si se abrio con loop=true). Devuelve true si el frame cambio -> hay
    // que redibujar. No bloquea: si el frame todavia no esta listo, deja el anterior.
    virtual bool Update(double nowMs) = 0;

    // Textura GL con el frame actual (RGBA; el alpha viene incluido si el video lo
    // tiene). Devuelve 0 hasta que haya al menos un frame decodificado.
    virtual unsigned int Texture() const = 0;
    // true cuando la reproduccion LLEGO AL FINAL (util para los videos que van una sola vez)
    virtual bool Terminado() const { return false; }
    // true si la textura viene con ALPHA PREMULTIPLICADO (asi entrega el decodificador de video):
    // hay que dibujarla con MezclaPremult, no con la mezcla alpha comun.
    virtual bool Premultiplicado() const { return false; }

    virtual int  Width()    const = 0;
    virtual int  Height()   const = 0;
    virtual bool HasAlpha() const = 0;
};

// Abre un video (con loop opcional). Devuelve NULL si el modulo no esta compilado
// (-DW3D_ENABLE_VIDEO ausente), el archivo no existe, o el backend no soporta el
// formato. El caller es duenio del puntero: delete para cerrar y liberar la textura.
W3dVideo* W3dVideoOpen(const char* path, bool loop);

// Igual pero desde bytes EN MEMORIA: por ejemplo salidos de un .w3dpack (ver
// io/W3dPack.h), sin pasar por un archivo. 'mime' identifica
// el contenedor para el backend web ("video/mp4", "video/webm"). Manten los bytes vivos
// hasta el primer frame (el backend web arma un Blob interno a partir de ellos).
// 'conSonido' = el video se reproduce con su audio (por defecto va mudo, que es lo unico que
// los navegadores dejan autoreproducir). Solo tiene sentido si arranca por una accion del usuario.
W3dVideo* W3dVideoOpenMemory(const void* bytes, size_t len, const char* mime, bool loop,
                             bool conSonido = false);

// DESBLOQUEO de video. Llamalo DENTRO del gesto del usuario (toque/click) cuando el video
// todavia no esta listo (ej: sus bytes se estan descargando): "estrena" un reproductor y lo
// reserva, para que el W3dVideoOpen* POSTERIOR pueda reproducir. En las plataformas que no
// lo necesitan no hace nada.
void W3dVideoUnlock();

} // namespace w3dEngine

#endif
