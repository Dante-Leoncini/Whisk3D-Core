#ifndef W3D_PACK_H
#define W3D_PACK_H

#include <stddef.h>

// ============================================================================
//  .w3dpack — contenedor de assets con el blob CIFRADO (imagenes/sonido/video).
//
//  El Core lo descifra EN MEMORIA y le pasa los bytes al decoder, sin escribir
//  ningun archivo intermedio. La CLAVE la aporta la aplicacion: el Core no la
//  guarda, no la genera y no opina de donde sale.
//
//  Cifrado: ChaCha20 (D. J. Bernstein), verificado contra su vector conocido.
//  Como todo cifrado del lado del cliente, su fuerza depende de la custodia de
//  la clave, que es responsabilidad de quien integra el motor.
//
//  Formato:  "W3DPACK1" | count | [name(32) offset(4) length(4)]xN | blob-cifrado
//  El blob entero se cifra con ChaCha20(key). Open() lo descifra a un buffer
//  propio; Get(name) devuelve un puntero DENTRO de ese buffer (no copia).
// ============================================================================

namespace w3dEngine {

class W3dPack {
public:
    W3dPack();
    ~W3dPack();

    // Abre un pack que YA esta en memoria. false si la cabecera o el formato no
    // validan. La clave la aporta la aplicacion.
    bool OpenMemory(const void* packBytes, size_t packLen, const unsigned char key[32]);

    // Abre un pack desde un archivo del disco (desktop). En web se usa OpenMemory.
    bool OpenFile(const char* path, const unsigned char key[32]);

    // Bytes YA DESCIFRADOS del asset 'name' (puntero interno, valido hasta el ~W3dPack).
    // Devuelve NULL si no existe. *outLen recibe el largo.
    const unsigned char* Get(const char* name, size_t* outLen) const;

    // Suelta el buffer descifrado y el indice (queda como recien construido). Sirve para
    // liberar los MB de un pack que ya no se usa (ej: al cambiar de juego) sin destruirlo.
    void Close();


private:
    W3dPack(const W3dPack&);            // no copiable (es duenio del buffer descifrado)
    W3dPack& operator=(const W3dPack&);
    struct Entry { char name[32]; unsigned int offset, length; };
    Entry*         entries;
    int            count;
    unsigned char* blob;   // buffer descifrado (duenio)
    size_t         blobLen;
};

// Cifra/descifra en el lugar con ChaCha20 (stream: la misma operacion en ambos
// sentidos). counter = bloque inicial (0 para el pack entero). Lo usa el Core y la
// herramienta de empaquetado (w3dpack), asi el cifrado es UNO SOLO y no se desincroniza.
void W3dChaCha20(const unsigned char key[32], unsigned int counter,
                 const unsigned char* in, unsigned char* out, size_t n);

// Construye un .w3dpack (lado AUTOR). names/datas/lens son 'count' assets; se cifran
// con 'key' y se escribe 'outPath'. false si no pudo escribir. La usa la herramienta.
bool W3dPackBuild(const char* outPath, const char* const* names,
                  const unsigned char* const* data, const size_t* lens,
                  int count, const unsigned char key[32]);

} // namespace w3dEngine

#endif
