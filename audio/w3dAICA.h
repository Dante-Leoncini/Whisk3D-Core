#pragma once
// ============================================================================
//  Whisk3DCore — EL SONIDO DE LA DREAMCAST, DIRECTO AL AICA (solo W3D_DREAMCAST)
//
//  El audio del Core (W3dAudio) mezcla por software y guarda cada sonido como PCM estereo de 16 bits: en la Dreamcast
//  la musica de una pelea sola pasaria los 16 MB de la consola. El AICA mezcla 64 canales por hardware y decodifica
//  ADPCM de 4 bits (el de Yamaha) solo: este modulo los usa como vienen (lo que GLdc es a w3dPVR, W3dAudio es a este).
//
//  Lo que hay que saber del AICA (y que este modulo deja a la vista):
//    - EFECTOS: van a la memoria de sonido (2 MB, con el programa del AICA y los buffers de los flujos) y suenan en un
//      canal cada uno. Hasta 65534 muestras (el contador de posicion del canal es de 16 bits: 2,7 s a 24 kHz).
//    - FLUJOS: lo largo (musica, ambiente, frases) queda en la RAM principal y se le pasa al AICA de a pedazos: hasta
//      cuatro a la vez. Los atiende un hilo aparte, cada 50 ms (el buffer de cada uno dura mas de un segundo; cada
//      10 ms le costaba al juego 1,2 ms por cuadro): pasarle un pedazo al AICA es un DMA que hay que esperar, y
//      esperarlo en el bucle del juego le costaba al cuadro varios milisegundos.
//    - ADPCM: el de Yamaha (wav2adpcm de KallistiOS, sin encabezado); estereo intercalado: un byte = una muestra de
//      cada canal (la izquierda en el nibble de arriba). Un efecto en lazo conviene en PCM de 16 bits: el decodificador
//      ADPCM no vuelve a su estado inicial al repetir.
//
//  No convive con W3dAudio: un programa usa uno o el otro.
// ============================================================================
#ifdef W3D_DREAMCAST
#include <stdint.h>

namespace w3dAICA {

    // el programa del AICA y los flujos (vuelve a arrancar el AICA: va antes de cargar nada)
    bool Iniciar();
    void Terminar();

    // --- efectos: en la memoria de sonido ---
    enum Formato { ADPCM4 = 4, PCM16 = 16 };
    typedef uint32_t Efecto;   // 0 = no se pudo cargar
    // mono; 'bytes' multiplo de 32 (el AICA los copia de a bloques). Se copia: 'datos' se puede soltar despues
    Efecto CargarEfecto(const void* datos, uint32_t bytes, uint32_t hz, Formato formato);
    void SoltarEfecto(Efecto e);
    int  MemoriaLibre();       // bytes que quedan en la memoria de sonido
    // suena en un canal libre: volumen 0..1, paneo -1 (izquierda) .. 1, tono 1 = normal. Devuelve el canal o -1
    int  Tocar(Efecto e, float vol, float pan = 0.0f, float tono = 1.0f, bool lazo = false);
    void Parar(int canal);

    // --- flujos: desde la RAM principal (ADPCM; 'datos' tiene que seguir ahi mientras suena) ---
    enum { FLUJOS = 4 };
    // devuelve el flujo (0..FLUJOS-1) o -1 si estan todos ocupados
    int  TocarFlujo(const void* adpcm, uint32_t bytes, uint32_t hz, int canales, float vol, bool lazo);
    void PararFlujo(int f);
    bool FlujoSonando(int f);  // (uno sin lazo termina solo; al terminar se libera)
    void VolumenFlujo(int f, float vol);

    // (los flujos los atiende el hilo: queda por si un programa lo arranco sin hilo)
    void Actualizar();
}

#endif // W3D_DREAMCAST
