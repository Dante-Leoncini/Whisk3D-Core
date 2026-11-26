#ifndef W3DZIP_H
#define W3DZIP_H

#include <stddef.h>   // size_t (W3dZipCrc32)
#include <string>
#include <vector>
#include <map>
#include <set>

// ============================================================================
//  W3dZip — ZIP minimo del Core (C++03, sin dependencias; compila en los 4 OS).
//
//  El formato de PROYECTO .w3d es un ZIP ESTANDAR: se abre con cualquier
//  descompresor (la gente puede mirar adentro), y el editor lo lee/escribe con
//  esto. Se ESCRIBE sin comprimir (metodo STORE: los assets tipicos -png, glb,
//  ogg- ya vienen comprimidos y volver a comprimir no gana nada); se LEEN
//  tambien entradas DEFLATE (zips re-armados por otras herramientas) usando el
//  Inflate que el Core ya trae (w3dCompress).
//
//  LIMITACIONES CONOCIDAS (a proposito, NO son bugs):
//   - SIN ZIP64: la cantidad de entradas del EOCD es un u16 (max 65534 entradas)
//     y los offsets/tamanos son u32 (max 4 GB por archivo y por entrada). Pasado
//     eso Save() falla en vez de escribir un zip roto, y Read() no entiende
//     los registros ZIP64 (devuelve false).
//   - un solo "disco": los zip partidos en volumenes no se leen.
//   - sin encriptar, sin data descriptors al escribir, metodos 0 (STORE) y 8
//     (DEFLATE) al leer.
//
//  LO QUE SE RECHAZA AL LEER (archivo entero, devolviendo false): CRC32 que no
//  coincide, nombres con ".." / absolutos / con backslash / con caracteres de
//  control, dos nombres que solo difieran en mayusculas (en Windows y macOS uno
//  pisaria al otro EN SILENCIO al extraer) y todo tamano que no cierre con el
//  tamano real del archivo.
// ============================================================================

struct W3dZipEntry {
    std::string name;                 // ruta interna ("scene.json", "assets/x.png")
    std::vector<unsigned char> data;
};

// true si el nombre de entrada es usable: relativo, con '/', sin "..", sin
// backslash, sin caracteres de control. Lo comparten el zip y el backend de
// carpeta del almacen (los dos resuelven el MISMO layout de nombres).
bool W3dZipNameSafe(const std::string& n);

// el CRC32 del zip (polinomio 0xEDB88320, el mismo de zlib/PNG) de n bytes. Es el que
// escribe y verifica este modulo; publico para probarlo contra los vectores conocidos.
unsigned W3dZipCrc32(const unsigned char* d, size_t n);

// ============================================================================
//  LECTOR POR INDICE (el .w3d MONTADO: no levanta el archivo entero a RAM)
//
//  W3dZipRead() de mas abajo carga TODO: el archivo entero + una copia de cada
//  entrada descomprimida. Con 50 MB de texturas eso pica ~100 MB solo por abrir
//  el proyecto, y en Android/Symbian directamente no entra. NO ES UNA
//  OPTIMIZACION: es lo que hace posible montar el contenedor.
//
//  El lector por indice lee al abrir SOLO el EOCD y el directorio central
//  (decenas de bytes por entrada), deja el FILE* abierto y levanta cada entrada
//  a PEDIDO con un fseek al local header. Pico de RAM = la entrada mas grande.
//
//  Valida lo mismo que W3dZipRead (nombres, duplicados case-folded, cotas con
//  RESTA, CRC32 por entrada) y ademas rechaza archivos de mas de 2 GB: los
//  offsets se le pasan a fseek() como 'long', que en 32 bits llega hasta ahi.
// ============================================================================
struct W3dZipIndex {
    std::string name;
    unsigned    offLocal;   // offset del local header dentro del archivo
    unsigned    compLen;    // bytes tal cual estan guardados
    unsigned    origLen;    // bytes ya descomprimidos
    unsigned    method;     // 0 = STORE, 8 = DEFLATE
    unsigned    crc;        // el que declara el directorio central
    // lo que se averigua leyendo (el archivo abierto no cambia): donde empiezan los datos (0 = todavia no se leyo
    // el local header) y si el CRC32 ya se verifico una vez (el streaming relee la misma entrada muchas veces)
    mutable unsigned long dataStart;
    mutable bool          verified;
    W3dZipIndex() : offLocal(0), compLen(0), origLen(0), method(0), crc(0), dataStart(0), verified(false) {}
};

class W3dZipReader {
public:
    W3dZipReader();
    ~W3dZipReader();

    // indexa el zip y deja el archivo abierto. false = no es un zip legible
    bool Open(const std::string& path);
    bool IsOpen() const { return fh != 0; }
    void Close();

    bool Exists(const std::string& name) const;
    // tamano DESCOMPRIMIDO. 0 si la entrada no esta (o si esta vacia)
    unsigned Size(const std::string& name) const;
    // levanta UNA entrada (descomprimiendola si hace falta) y verifica su CRC32 (la PRIMERA vez: el archivo
    // abierto no cambia, una entrada ya verificada no se vuelve a sumar en cada relectura)
    bool Read(const std::string& name, std::vector<unsigned char>& out);
    // 'n' bytes desde 'desde' de UNA entrada. Guardada sin comprimir (STORE, lo que escribe el editor): un fseek y
    // un fread de SOLO esos bytes, sin leer el resto ni sumar el CRC (el streaming lee de una malla solo sus bloques
    // de render). Comprimida: la entrada entera y el recorte. false = no esta o el rango se pasa
    bool ReadRange(const std::string& name, unsigned from, unsigned n, std::vector<unsigned char>& out);
    // los bytes TAL CUAL estan en el archivo, sin descomprimir: lo usa
    // W3dZipWriter::CopyFrom para reenvasar una entrada sin recomprimirla
    bool ReadRaw(const std::string& name, std::vector<unsigned char>& out,
                   unsigned* method, unsigned* crc, unsigned* origLen);
    // la HUELLA de una entrada (el CRC32 y el tamano descomprimido del directorio central) SIN leer sus datos: lo que
    // necesita el guardado de cada entrada que reenvasa (antes leia la entrada entera con ReadRaw para eso, y despues
    // CopyFrom la volvia a leer). Valida su local header como ReadRaw: false = no esta o esta rota
    bool Info(const std::string& name, unsigned* crc, unsigned* origLen);

    void List(std::vector<std::string>& out) const;   // en el orden del directorio
    size_t Count() const { return ents.size(); }
    const std::string& Path() const { return path; }

private:
    // no copiable: adentro hay un FILE* con dueno
    W3dZipReader(const W3dZipReader&);
    W3dZipReader& operator=(const W3dZipReader&);
    const W3dZipIndex* Find(const std::string& name) const;
    // donde empiezan los datos de la entrada (valida su local header la primera vez y lo recuerda)
    bool DataOf(const W3dZipIndex* e, unsigned long& start);

    void*         fh;          // FILE* (void* para no meter <stdio.h> en el header)
    std::string   path;
    unsigned long fileSize;
    std::vector<W3dZipIndex>     ents;
    std::map<std::string, size_t> byName;
};

// ---- ESCRITURA ----
//  DOS MODOS, el mismo formato de salida (STORE, fecha fija, bit UTF-8):
//   - EN LOTE (el de siempre): Agregar... + Save(). Se queda con una COPIA de
//     los bytes de cada entrada hasta el final -> pico ~2x.
//   - INCREMENTAL: OpenWriting + Escribir/CopyFrom + Cerrar. Cada entrada se
//     vuelca EN EL ACTO y solo queda en memoria {nombre, crc, tamano, offset}.
//     Pico de RAM = la entrada mas grande. Save() esta implementado ARRIBA de
//     este modo, asi que los bytes que salen son los mismos en los dos casos.
class W3dZipWriter {
public:
    W3dZipWriter();
    ~W3dZipWriter();

    // --- modo LOTE (API vieja, no se toca: la usan GuardarVersion y los tests) ---
    // agrega una entrada (copia los datos)
    void Add(const std::string& name, const void* data, size_t n);
    void Add(const std::string& name, const std::vector<unsigned char>& data);
    void AddText(const std::string& name, const std::string& text);
    // agrega un ARCHIVO del disco (false si no se pudo leer)
    bool AddFile(const std::string& name, const std::string& diskPath);
    // escribe el zip completo. false si no pudo escribir
    bool Save(const std::string& path);

    // --- modo INCREMENTAL (streaming) ---
    // abre el archivo destino (se escribe a medida que llegan las entradas)
    bool OpenWriting(const std::string& path);
    bool Writing() const { return wf != 0; }
    // vuelca una entrada YA. false si el nombre no sirve o fallo el disco
    bool Write(const std::string& name, const void* data, size_t n);
    bool Write(const std::string& name, const std::vector<unsigned char>& data);
    bool WriteText(const std::string& name, const std::string& text);
    // copia una entrada de OTRO zip sin descomprimirla ni recalcular su CRC
    // (guardado incremental: lo que no cambio se reenvasa byte a byte)
    bool CopyFrom(W3dZipReader& origin, const std::string& name);
    // escribe el directorio central + EOCD y cierra. false si algo fallo antes
    bool Close();
    // cierra y BORRA el archivo a medio escribir (un zip truncado no sirve a nadie)
    void Abort();

private:
    W3dZipWriter(const W3dZipWriter&);
    W3dZipWriter& operator=(const W3dZipWriter&);
    // una entrada ya volcada: lo unico que queda en RAM hasta el cierre
    struct Turn {
        std::string name;
        unsigned crc, compLen, origLen, method, off;
        Turn() : crc(0), compLen(0), origLen(0), method(0), off(0) {}
    };
    bool DumpHeader(const std::string& name, unsigned crc, unsigned compLen,
                        unsigned origLen, unsigned method);

    std::vector<W3dZipEntry> entries;   // modo lote
    void*                 wf;           // FILE* del modo incremental
    std::string           wruta;
    std::vector<Turn>   wdir;
    std::set<std::string> wvistos;         // nombres YA minusculizados
    bool                  wfallo;
};

// ---- LECTURA ----
// true si el archivo EMPIEZA como zip (magic PK\3\4): distingue el .w3d nuevo
// (zip) del viejo (texto plano)
bool W3dZipIs(const std::string& path);

// lee TODAS las entradas (STORE directo; DEFLATE via el Inflate del Core).
// false si el archivo no es un zip legible.
bool W3dZipRead(const std::string& path, std::vector<W3dZipEntry>* out);

// extrae todas las entradas a una carpeta (crea subcarpetas). false si fallo algo.
bool W3dZipExtract(const std::string& path, const std::string& folderDestination);

#endif // W3DZIP_H
