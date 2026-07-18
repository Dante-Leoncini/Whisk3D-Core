#ifndef W3D_PACK_H
#define W3D_PACK_H

#include <stddef.h>

// ============================================================================
//  .w3dpack — contenedor de assets CIFRADO (imagenes / sonido / video).
//
//  OBJETIVO (honesto): que robar los assets NO sea "copiar y pegar". En el
//  navegador no queda ningun archivo con nombre ni URL plana: solo el pack
//  cifrado (bytes ilegibles). El Core lo descifra EN MEMORIA y le pasa los bytes
//  al decoder. Frena al inspector, al "guardar como" y a los scripts (~99%).
//
//  NO es DRM inquebrantable: el cifrado es ChaCha20 y la CLAVE la aporta la app.
//  Si la clave va EMBEBIDA en el binario, alguien que lea el binario la saca y
//  descifra (principio de Kerckhoffs) -> eso es DETERRENCE, no una garantia. Para
//  resistir a un atacante que conoce el binario, la app debe traer la clave de un
//  SERVIDOR por sesion (misma interfaz: solo cambia de donde sale la clave).
//  Ver THREAT_MODEL.md.
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

    // Abre un pack ya en memoria (los bytes crudos del .w3dpack) y lo descifra con
    // 'key' (32 bytes). No se queda con el puntero de entrada. false si el formato
    // no valida. La clave la decide la APP (embebida = deterrence; de server = fuerte).
    bool OpenMemory(const void* packBytes, size_t packLen, const unsigned char key[32]);

    // Abre un pack desde un archivo del disco (desktop). En web se usa OpenMemory.
    bool OpenFile(const char* path, const unsigned char key[32]);

    // Bytes YA DESCIFRADOS del asset 'name' (puntero interno, valido hasta el ~W3dPack).
    // Devuelve NULL si no existe. *outLen recibe el largo.
    const unsigned char* Get(const char* name, size_t* outLen) const;

    int Count() const;

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
                  const unsigned char* const* datas, const size_t* lens,
                  int count, const unsigned char key[32]);

} // namespace w3dEngine

#endif
