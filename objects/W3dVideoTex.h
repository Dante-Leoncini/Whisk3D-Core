#ifndef W3DVIDEOTEX_H
#define W3DVIDEOTEX_H

// ============================================================================
//  W3dVideoTex — UN VIDEO COMO TEXTURA (las pantallas de un mapa: la
//  transmision de un monitor, un cartel que se mueve). El video se graba antes
//  (en la PC, con el mismo motor) y el telefono solo lo pasa: no hay render a
//  textura ni copias de pantalla por cuadro.
//
//  EL ARCHIVO (.w3dv): cuadros RGB565 chicos (64x64, 64x128) a 12-15 por
//  segundo, cada uno comprimido con zlib (el inflate del Core, io/w3dCompress).
//  Cada 'clave' cuadros va uno ENTERO; los demas son la DIFERENCIA con el
//  anterior (XOR de los pixeles: lo que no cambia queda en cero y casi no
//  ocupa). Se lee de a un cuadro desde el disco (fseek + fread): el video
//  entero nunca esta en memoria, solo la tabla de donde empieza cada cuadro.
//
//      cabecera (32 B): "W3DV" | u16 version (1) | u16 formato (1 = RGB565) |
//                       u16 ancho | u16 alto | u16 clave | u16 0 |
//                       f32 fps | u32 cuadros | u32 0 | u32 0
//      tabla:           u32 desplazamiento[cuadros + 1] (desde el principio
//                       del archivo; el ultimo = el final del ultimo cuadro)
//      cada cuadro:     u8 tipo (0 = entero, 1 = XOR con el anterior,
//                       2 = igual al anterior: sin datos) + 3 bytes en cero +
//                       los pixeles en zlib (filas apretadas, la primera es la
//                       de v = 0: la de arriba de la imagen, como las texturas)
//      (todo little-endian)
//
//  LAS DOS TEXTURAS: la que se ve y la que se escribe. Un cuadro nuevo se sube
//  a la que NO se dibujo en el cuadro anterior y recien entonces pasa a ser la
//  visible: el PowerVR del N95 dibuja un cuadro mientras se arma el siguiente,
//  y reescribir la textura que todavia usa lo obliga a esperar o a copiarla.
//
//  EL RELOJ lo pone quien lo usa (Go(segundos)): el de la escena que suena con
//  el video, asi la imagen va con el audio aunque el juego vaya a 20 o a 60
//  cuadros por segundo. A 15 cuadros de video y 30 de juego se sube uno cada
//  dos cuadros del juego. Si nadie lo mira no hay que llamarlo: al volver a
//  verse salta al cuadro que toca (desde el entero mas cercano).
//
//  C++03, sin dependencias fuera del Core (stdio para leer).
// ============================================================================

#include <stdio.h>

namespace w3dEngine {

class W3dVideoTex {
public:
    W3dVideoTex();
    ~W3dVideoTex();

    // abre el .w3dv: lee la cabecera y la tabla y crea las dos texturas (con el primer cuadro). false si no esta o no
    // es un video que se entienda
    bool Open(const char* path);
    void Close();
    bool IsOpen() const { return file != 0; }

    // la textura con el cuadro del segundo 't' (el reloj de quien lo usa; mas alla del final, el ultimo; < 0 el
    // primero): decodifica lo que falte y, si cambio de cuadro, lo sube a la otra textura, que pasa a ser la visible.
    // Devuelve el id de la textura visible (0 si no hay video)
    unsigned int Go(float t);
    // lo mismo por el numero de cuadro
    unsigned int GoFrame(int frame);
    unsigned int Id() const { return ids[visible]; }

    int Width() const { return width; }
    int Height() const { return height; }
    int Frames() const { return frames; }
    float Fps() const { return fps; }
    float Duration() const { return fps > 0.0f ? (float)frames / fps : 0.0f; }
    int CurrentFrame() const { return current; }

    // (las medidas) cuantos cuadros se decodificaron y se subieron desde que se abrio, y los bytes leidos del disco
    int Decoded() const { return nDecode; }
    int Uploaded() const { return nUploaded; }
    long BytesRead() const { return readBytes; }

private:
    W3dVideoTex(const W3dVideoTex&);
    W3dVideoTex& operator=(const W3dVideoTex&);
    bool Decode(int k);     // el cuadro k sobre 'px' (que tiene el k - 1 si es una diferencia)

    FILE* file;
    int width, height, key, frames;
    float fps;
    unsigned int* table;        // cuadros + 1 desplazamientos
    unsigned short* px;         // el ultimo cuadro decodificado
    unsigned short* tmp;        // la diferencia descomprimida
    unsigned char* read;        // el cuadro comprimido tal como esta en el disco
    int readMax;
    int decode;                   // el cuadro que esta en 'px' (-1: ninguno)
    int current;                  // el que se ve
    unsigned int ids[2];
    int visible;
    int nDecode, nUploaded;
    long readBytes;
};

}

#endif
