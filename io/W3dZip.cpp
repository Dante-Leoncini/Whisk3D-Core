// ============================================================================
//  W3dZip.cpp — ver W3dZip.h. Dialecto C++03 (Symbian compila esto).
//
//  CRITERIO DE ROBUSTEZ (el .w3d pasa a ser un contenedor zip: esto lee ARCHIVOS
//  DE TERCEROS y archivos corruptos, asi que se trata TODO como hostil):
//   - las validaciones de rangos se hacen con RESTAS sobre el tamano del archivo,
//     NUNCA con sumas: 'off + 30 > d.size()' ENVUELVE con offsets cerca de
//     0xFFFFFFFF y el chequeo pasaba dejando leer fuera del buffer.
//   - toda reserva de memoria tiene COTA (ver kZipMaxEntry / kZipMaxRatio):
//     un origLen de 0xFFFFFFFF pedia 4 GB de una sola vez.
//   - el CRC32 de cada entrada se VERIFICA al leer (si no, un zip corrupto
//     entraba como datos "buenos" y el error aparecia mucho mas tarde).
//   - los nombres se validan (nada de "..", rutas absolutas, backslash ni
//     duplicados que solo difieran en mayusculas).
// ============================================================================
#include "W3dZip.h"
#include "w3dFilesystem.h"
#include "w3dCompress.h"   // Inflate (leer entradas DEFLATE de zips ajenos)
#include "w3dlog.h"
#include <stdio.h>
#include <string.h>
#include <set>
#include <new>             // std::bad_alloc: un zip bomba tiene que dar "archivo invalido", no matar el proceso

// ---------------------------------------------------------------------------
//  Las COTAS de arriba (kZipMaxEntry/kZipMaxTotal) descartan un origLen mentiroso,
//  pero NO garantizan que la memoria exista: en 32 bits (Symbian, web) un zip de 250 KB
//  puede pedir legitimamente 256 MB y el resize/reserve tira std::bad_alloc. Si nadie la
//  atrapa, el proceso MUERE en vez de devolver "archivo invalido".
//  Se compila solo donde hay excepciones (RVCT/Symbian puede venir con -fno-exceptions:
//  ahi el try/catch ni siquiera compila). Sin excepciones el comportamiento es el de antes.
// ---------------------------------------------------------------------------
#if defined(__EXCEPTIONS) || defined(__cpp_exceptions) || (defined(_MSC_VER) && defined(_CPPUNWIND))
    #define W3D_ZIP_TRY      try {
    #define W3D_ZIP_CATCH(r, n) } catch (const std::bad_alloc&) { \
        w3dLogfE("W3dZip: %s pide mas memoria de la que hay (entrada %s): lo trato como invalido", (r), (n)); \
        return false; }
#else
    #define W3D_ZIP_TRY
    #define W3D_ZIP_CATCH(r, n)
#endif

#ifdef _WIN32
    #include <direct.h>
    #define W3D_MKDIR(p) _mkdir(p)
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #define W3D_MKDIR(p) mkdir(p, 0755)
#endif

// ---------------------------------------------------------------------------
//  COTAS (ver W3dZip.h "LIMITACIONES")
// ---------------------------------------------------------------------------
// el EOCD guarda la cantidad de entradas en un u16: mas de eso necesita ZIP64
static const unsigned kZipMaxEntries = 0xFFFEu;
// expansion maxima TEORICA de DEFLATE (~1032:1). Sirve para descartar un origLen
// mentiroso ANTES de reservar: con 1 KB comprimido no pueden salir 4 GB.
static const unsigned kZipMaxRatio = 1032u;
// tope duro por entrada y del total descomprimido (zip bomba)
static const unsigned kZipMaxEntry = 256u * 1024u * 1024u;
static const unsigned kZipMaxTotal = 512u * 1024u * 1024u;
// flag 0x800 del "general purpose": dice que el NOMBRE viene en UTF-8. Sin el,
// los descompresores de Windows interpretan el nombre en la codepage local y un
// acento sale mojibake.
static const unsigned kZipFlagUtf8 = 0x800u;

// ---------------------------------------------------------------------------
//  CRC32 (polinomio estandar del zip), tablas armadas una vez.
//  "SLICING BY 8": 8 tablas de 256 (8 KB) y 8 bytes por vuelta en vez de 1.
//  Es el mismo CRC (mismos bytes en el archivo), unas 5 veces mas rapido: con
//  las mallas binarias (.w3db) abrir es leer la entrada + memcpy, y el CRC byte
//  a byte era lo que mas pesaba de "leer la entrada" (verificar CADA entrada al
//  leerla no se negocia: es lo que frena a un zip corrupto).
//  Los 4 primeros bytes de cada vuelta se arman little-endian BYTE A BYTE: no
//  depende del endian del host ni de la alineacion del puntero (C++03 puro).
//  gCrcTable[0] es la tabla clasica; gCrcTable[t][i] = el CRC de i seguido de t
//  bytes en cero (lo que avanza un byte que esta t posiciones antes del final).
// ---------------------------------------------------------------------------
static unsigned gCrcTable[8][256];
static bool gCrcReady = false;
static void CrcInit() {
    if (gCrcReady) return;
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        gCrcTable[0][i] = c;
    }
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = gCrcTable[0][i];
        for (int t = 1; t < 8; t++) {
            c = gCrcTable[0][c & 0xFFu] ^ (c >> 8);
            gCrcTable[t][i] = c;
        }
    }
    gCrcReady = true;
}
static unsigned Crc32(const unsigned char* d, size_t n) {
    CrcInit();
    unsigned c = 0xFFFFFFFFu;
    while (n >= 8) {
        const unsigned a = c ^ ((unsigned)d[0] | ((unsigned)d[1] << 8) |
                                ((unsigned)d[2] << 16) | ((unsigned)d[3] << 24));
        c = gCrcTable[7][a & 0xFFu] ^ gCrcTable[6][(a >> 8) & 0xFFu] ^
            gCrcTable[5][(a >> 16) & 0xFFu] ^ gCrcTable[4][(a >> 24) & 0xFFu] ^
            gCrcTable[3][d[4]] ^ gCrcTable[2][d[5]] ^ gCrcTable[1][d[6]] ^ gCrcTable[0][d[7]];
        d += 8; n -= 8;
    }
    // la cola (menos de 8 bytes): byte a byte, como siempre
    while (n > 0) {
        c = gCrcTable[0][(c ^ *d) & 0xFFu] ^ (c >> 8);
        d++; n--;
    }
    return c ^ 0xFFFFFFFFu;
}
unsigned W3dZipCrc32(const unsigned char* d, size_t n) { return (d && n) ? Crc32(d, n) : 0u; }
static unsigned Crc32Of(const std::vector<unsigned char>& v) {
    return v.empty() ? 0u : Crc32(&v[0], v.size());
}

// little-endian a un FILE*
static void W16(FILE* f, unsigned v) { unsigned char b[2] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF) }; fwrite(b, 1, 2, f); }
static void W32(FILE* f, unsigned v) { unsigned char b[4] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF), (unsigned char)((v >> 16) & 0xFF), (unsigned char)((v >> 24) & 0xFF) }; fwrite(b, 1, 4, f); }
static unsigned R16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static unsigned R32(const unsigned char* p) { return p[0] | (p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }

// ---------------------------------------------------------------------------
//  NOMBRES: que entra y que no
//
//  Un zip puede traer nombres que al extraer escriben FUERA de la carpeta
//  destino ("../../.bashrc", "/etc/passwd", "..\\x" en Windows). Se rechaza el
//  archivo entero: es basura o es un ataque, en ningun caso algo que abrir.
// ---------------------------------------------------------------------------
bool W3dZipNameSafe(const std::string& n) {
    if (n.empty() || n.size() > 0xFFFFu) return false;
    if (n[0] == '/') return false;                          // absoluta unix
    if (n.size() > 1 && n[1] == ':') return false;          // "C:\..." de windows
    for (size_t i = 0; i < n.size(); i++) {
        unsigned char c = (unsigned char)n[i];
        if (c == '\\') return false;   // el zip usa SIEMPRE '/'; el backslash es
                                       // separador en Windows (escaparia la carpeta)
        if (c < 0x20 || c == 0x7F) return false;            // control / NUL embebido
    }
    // ningun TRAMO puede ser ".." (un "foo..png" es un nombre valido y pasa)
    size_t start = 0;
    for (;;) {
        size_t end = n.find('/', start);
        if (end == std::string::npos) end = n.size();
        if (end - start == 2 && n[start] == '.' && n[start + 1] == '.') return false;
        if (end == n.size()) break;
        start = end + 1;
    }
    return true;
}
// posicion del archivo -> el u32 del zip. false si no entra (haria falta ZIP64).
// El ">> 16" dos veces (en vez de comparar con 0xFFFFFFFF) evita el warning de
// "comparacion siempre falsa" donde long ya es de 32 bits.
static bool OffsetU32(long pos, unsigned* out) {
    if (pos < 0) return false;
    unsigned long u = (unsigned long)pos;
    if (((u >> 16) >> 16) != 0) return false;
    *out = (unsigned)u;
    return true;
}
// true si la ruta es un ARCHIVO COMUN. Es la guarda del remove() del escritor:
// borrar "el zip a medias" no puede terminar borrando /dev/full, una tuberia o
// un dispositivo si alguien le pasa uno de destino (el test 'zipfull' escribe a
// /dev/full a proposito).
#ifdef _WIN32
static bool IsFileRegular(const char*) { return true; }   // Windows no tiene /dev/*
#else
static bool IsFileRegular(const char* p) {
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return S_ISREG(st.st_mode) != 0;
}
#endif

// minusculas ASCII: con esto se detectan los duplicados que Windows y macOS
// (case-insensitive) pisarian EN SILENCIO al extraer -> perdida de datos
static std::string LowercasesAscii(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); i++)
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    return r;
}

// ---------------------------------------------------------------------------
//  ESCRITURA (metodo 0 = STORE)
// ---------------------------------------------------------------------------
void W3dZipWriter::Add(const std::string& name, const void* data, size_t n) {
    W3dZipEntry e;
    e.name = name;
    e.data.assign((const unsigned char*)data, (const unsigned char*)data + n);
    entries.push_back(e);
}
void W3dZipWriter::Add(const std::string& name, const std::vector<unsigned char>& data) {
    W3dZipEntry e; e.name = name; e.data = data;
    entries.push_back(e);
}
void W3dZipWriter::AddText(const std::string& name, const std::string& text) {
    Add(name, text.c_str(), text.size());
}
bool W3dZipWriter::AddFile(const std::string& name, const std::string& diskPath) {
    std::vector<unsigned char> d;
    if (!w3dFileSystem::ReadFileBytes(diskPath, d)) return false;
    Add(name, d);
    return true;
}

// ---------------------------------------------------------------------------
//  ESCRITURA INCREMENTAL (streaming): cada entrada se vuelca EN EL ACTO
// ---------------------------------------------------------------------------
W3dZipWriter::W3dZipWriter() : wf(0), wfallo(false) {}
W3dZipWriter::~W3dZipWriter() {
    // si alguien se olvido de Close(), NO se deja un zip a medias en el disco:
    // sin directorio central no lo abre nadie y ademas pisaria un archivo bueno
    if (wf) Abort();
}

bool W3dZipWriter::OpenWriting(const std::string& path) {
    if (wf) { w3dLogfE("W3dZip: AbrirEscritura con otro zip ya abierto (%s)", wruta.c_str()); return false; }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { w3dLogfE("W3dZip: no pude escribir %s", path.c_str()); return false; }
    wf = (void*)f; wruta = path; wdir.clear(); wvistos.clear(); wfallo = false;
    return true;
}

// local header + nombre. Deja el archivo posicionado justo donde van los datos.
bool W3dZipWriter::DumpHeader(const std::string& name, unsigned crc, unsigned compLen,
                                  unsigned origLen, unsigned method) {
    if (!wf || wfallo) return false;
    FILE* f = (FILE*)wf;
    if (wdir.size() >= (size_t)kZipMaxEntries) {
        w3dLogfE("W3dZip: mas de %u entradas: no entra sin ZIP64", kZipMaxEntries);
        wfallo = true; return false;
    }
    if (!W3dZipNameSafe(name)) {
        w3dLogfE("W3dZip: nombre de entrada invalido: %s", name.c_str());
        wfallo = true; return false;
    }
    if (!wvistos.insert(LowercasesAscii(name)).second) {
        // en Windows/macOS el unzip pisaria una con la otra SIN AVISAR
        w3dLogfE("W3dZip: dos entradas que solo difieren en mayusculas: %s", name.c_str());
        wfallo = true; return false;
    }
    Turn v;
    // el offset del local header va en un u32: pasado los 4 GB haria falta ZIP64
    // y el zip saldria roto (mejor fallar y decirlo)
    if (!OffsetU32(ftell(f), &v.off)) {
        w3dLogfE("W3dZip: %s pasa los 4 GB (haria falta ZIP64)", wruta.c_str());
        wfallo = true; return false;
    }
    v.name = name; v.crc = crc; v.compLen = compLen; v.origLen = origLen; v.method = method;
    W32(f, 0x04034B50);            // PK\3\4
    W16(f, 20); W16(f, kZipFlagUtf8);   // version / flags (bit 11: nombre UTF-8)
    W16(f, method);                // 0 = STORE (lo que escribe el editor)
    W16(f, 0); W16(f, 0x21);       // hora/fecha FIJA: dos guardados sin cambios dan
                                   // los MISMOS bytes (diff en git, dedup, round-trip)
    W32(f, crc);
    W32(f, compLen);
    W32(f, origLen);
    W16(f, (unsigned)name.size()); W16(f, 0);
    fwrite(name.c_str(), 1, name.size(), f);
    wdir.push_back(v);
    return true;
}

bool W3dZipWriter::Write(const std::string& name, const void* data, size_t n) {
    if (!wf || wfallo) return false;
    // sin ZIP64 una entrada no puede pasar los 4 GB
    if (((((unsigned long)n) >> 16) >> 16) != 0) {
        w3dLogfE("W3dZip: la entrada %s pasa los 4 GB (haria falta ZIP64)", name.c_str());
        wfallo = true; return false;
    }
    unsigned crc = n ? Crc32((const unsigned char*)data, n) : 0u;
    if (!DumpHeader(name, crc, (unsigned)n, (unsigned)n, 0)) return false;
    if (n) fwrite(data, 1, n, (FILE*)wf);
    return true;
}
bool W3dZipWriter::Write(const std::string& name, const std::vector<unsigned char>& data) {
    return Write(name, data.empty() ? (const void*)"" : (const void*)&data[0], data.size());
}
bool W3dZipWriter::WriteText(const std::string& name, const std::string& text) {
    return Write(name, text.c_str(), text.size());
}

bool W3dZipWriter::CopyFrom(W3dZipReader& origin, const std::string& name) {
    if (!wf || wfallo) return false;
    std::vector<unsigned char> raw;
    unsigned method = 0, crc = 0, origLen = 0;
    if (!origin.ReadRaw(name, raw, &method, &crc, &origLen)) {
        w3dLogfE("W3dZip: CopiarDe: no pude leer '%s' de %s", name.c_str(), origin.Path().c_str());
        wfallo = true; return false;
    }
    // se reenvasan los MISMOS bytes: ni se descomprime ni se recalcula el CRC
    if (!DumpHeader(name, crc, (unsigned)raw.size(), origLen, method)) return false;
    if (!raw.empty()) fwrite(&raw[0], 1, raw.size(), (FILE*)wf);
    return true;
}

bool W3dZipWriter::Close() {
    if (!wf) return false;
    FILE* f = (FILE*)wf;
    if (wfallo) { Abort(); return false; }
    // 2) central directory
    unsigned cdStart = 0;
    if (!OffsetU32(ftell(f), &cdStart)) {
        w3dLogfE("W3dZip: %s pasa los 4 GB (haria falta ZIP64)", wruta.c_str());
        Abort(); return false;
    }
    for (size_t i = 0; i < wdir.size(); i++) {
        const Turn& v = wdir[i];
        W32(f, 0x02014B50);            // PK\1\2
        W16(f, 20); W16(f, 20); W16(f, kZipFlagUtf8); W16(f, v.method);   // flags: idem local
        W16(f, 0); W16(f, 0x21);
        W32(f, v.crc);
        W32(f, v.compLen);
        W32(f, v.origLen);
        W16(f, (unsigned)v.name.size()); W16(f, 0); W16(f, 0);
        W16(f, 0); W16(f, 0); W32(f, 0);
        W32(f, v.off);
        fwrite(v.name.c_str(), 1, v.name.size(), f);
    }
    unsigned cdEnd = 0;
    if (!OffsetU32(ftell(f), &cdEnd)) {
        w3dLogfE("W3dZip: %s pasa los 4 GB (haria falta ZIP64)", wruta.c_str());
        Abort(); return false;
    }
    // 3) end of central directory
    W32(f, 0x06054B50);                // PK\5\6
    W16(f, 0); W16(f, 0);
    W16(f, (unsigned)wdir.size()); W16(f, (unsigned)wdir.size());
    W32(f, cdEnd - cdStart);
    W32(f, cdStart);
    W16(f, 0);
    // reportar el fallo de ESCRITURA (disco lleno, USB desconectado): antes se devolvia
    // true con un .w3d truncado y el usuario creia que guardo bien.
    // OJO: ferror() ANTES de cerrar NO alcanza. stdio bufferea (4 KB): un zip mas chico que
    // el buffer todavia no toco el disco cuando se lo consulta, asi que ferror da 0 y el
    // ENOSPC recien aparece en el FLUSH que hace fclose. El retorno de fclose es parte del
    // resultado, no un descarte (reproducido escribiendo 630 bytes a /dev/full: ferror=0,
    // fclose=-1). Se evalua fclose SIEMPRE (nunca en corto) para no filtrar el FILE*.
    bool ok = (ferror(f) == 0);
    if (fclose(f) != 0) ok = false;
    wf = 0; wdir.clear(); wvistos.clear();
    // se DEJA lo que haya quedado en el disco (mismo comportamiento de siempre): el
    // guardado del proyecto escribe a un .w3dtmp y recien renombra, asi que un tmp
    // truncado no pisa nada, y borrar a ciegas el destino que nos dieron es peor.
    if (!ok) w3dLogfE("W3dZip: no pude escribir %s (disco lleno / medio desconectado?)", wruta.c_str());
    wruta.clear();
    return ok;
}

void W3dZipWriter::Abort() {
    if (!wf) return;
    fclose((FILE*)wf);
    wf = 0;
    // el zip quedo sin directorio central: no lo abre nadie. Se borra, pero SOLO
    // si es un archivo comun (ver IsFileRegular)
    if (!wruta.empty() && IsFileRegular(wruta.c_str())) remove(wruta.c_str());
    wruta.clear(); wdir.clear(); wvistos.clear(); wfallo = false;
}

// ---------------------------------------------------------------------------
//  ESCRITURA EN LOTE: es el modo incremental con los bytes juntados antes.
//  Se conserva TAL CUAL la propiedad de antes: si algun nombre no sirve, NO se
//  toca el disco (se valida el lote entero primero).
// ---------------------------------------------------------------------------
bool W3dZipWriter::Save(const std::string& path) {
    // 0) VALIDAR antes de tocar el disco: si el zip que se va a escribir tiene
    //    nombres peligrosos, duplicados case-folded o no entra en el formato
    //    (sin ZIP64), no se escribe NADA y se dice por que.
    if (entries.size() > (size_t)kZipMaxEntries) {
        w3dLogfE("W3dZip: %d entradas: no entran en el zip sin ZIP64 (max %u)",
                 (int)entries.size(), kZipMaxEntries);
        return false;
    }
    {
        std::set<std::string> seen;
        for (size_t i = 0; i < entries.size(); i++) {
            const std::string& n = entries[i].name;
            if (!W3dZipNameSafe(n)) {
                w3dLogfE("W3dZip: nombre de entrada invalido: %s", n.c_str());
                return false;
            }
            if (!seen.insert(LowercasesAscii(n)).second) {
                // en Windows/macOS el unzip pisaria una con la otra SIN AVISAR
                w3dLogfE("W3dZip: dos entradas que solo difieren en mayusculas: %s", n.c_str());
                return false;
            }
        }
    }
    if (!OpenWriting(path)) return false;
    for (size_t i = 0; i < entries.size(); i++) {
        if (!Write(entries[i].name, entries[i].data)) { Abort(); return false; }
    }
    return Close();
}

// ---------------------------------------------------------------------------
//  LECTURA
// ---------------------------------------------------------------------------
bool W3dZipIs(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char m[4] = { 0, 0, 0, 0 };
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    return n == 4 && m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4;
}

bool W3dZipRead(const std::string& path, std::vector<W3dZipEntry>* out) {
    if (!out) return false;
    std::vector<unsigned char> d;
    if (!w3dFileSystem::ReadFileBytes(path, d) || d.size() < 22) return false;
    const size_t N = d.size();
    // --- EOCD: buscarlo desde el final (el comentario del zip puede correrlo).
    //     El largo del comentario tiene que ENTRAR en lo que queda: asi un
    //     0x06054B50 que cae por casualidad adentro de los datos no matchea.
    size_t eocd = (size_t)-1;
    for (size_t i = N - 22; ; i--) {
        if (R32(&d[i]) == 0x06054B50 && (N - i - 22) >= R16(&d[i + 20])) { eocd = i; break; }
        if (i == 0 || N - i > 22 + 0xFFFFu) break;
    }
    if (eocd == (size_t)-1) return false;
    unsigned nEntries = R16(&d[eocd + 10]);
    unsigned cdSize   = R32(&d[eocd + 12]);
    unsigned cdStart  = R32(&d[eocd + 16]);
    // TODAS las cuentas con restas: 'cdStart + cdSize > N' envuelve y deja pasar
    // offsets absurdos. El directorio central tiene que estar COMPLETO antes del EOCD.
    if (cdStart > eocd || (size_t)(eocd - cdStart) < cdSize) return false;
    // cota barata contra un nEntries mentiroso (cada header central son >= 46 bytes)
    if ((size_t)(eocd - cdStart) / 46 < nEntries) return false;

    // se arma aparte y recien al final se pasa a 'out': si el zip resulta
    // corrupto a mitad de camino, el vector del llamador queda como estaba
    std::vector<W3dZipEntry> res;
    std::set<std::string> seen;
    // las cotas se toman contra el TAMANO REAL del archivo: lo guardado sin
    // comprimir (STORE) nunca puede pasar de N, asi que un zip legitimo grande
    // (assets de varios GB) sigue abriendo, y un origLen inventado no.
    const size_t boundEntry = (N > (size_t)kZipMaxEntry) ? N : (size_t)kZipMaxEntry;
    const size_t boundTotal = (N > (size_t)kZipMaxTotal)   ? N : (size_t)kZipMaxTotal;
    size_t total = 0;
    size_t p = cdStart;
    // FIN del directorio central (cdStart+cdSize, ya validado <= eocd <= N mas arriba). TODAS las
    // cotas del recorrido van contra ESTE limite, no contra N: acotar solo contra el tamano del
    // archivo dejaba pasar una entrada cuyo extraLen/comLen de 0xFFFF se comia los datos que
    // vienen DESPUES del directorio (el zip se aceptaba igual).
    const size_t cdEnd = (size_t)cdStart + cdSize;
    for (unsigned i = 0; i < nEntries; i++) {
        if (p > cdEnd || cdEnd - p < 46 || R32(&d[p]) != 0x02014B50) return false;
        unsigned method   = R16(&d[p + 10]);
        unsigned crcEsp   = R32(&d[p + 16]);
        unsigned compLen  = R32(&d[p + 20]);
        unsigned origLen  = R32(&d[p + 24]);
        unsigned nameLen  = R16(&d[p + 28]);
        unsigned extraLen = R16(&d[p + 30]);
        unsigned comLen   = R16(&d[p + 32]);
        unsigned off      = R32(&d[p + 42]);
        if (cdEnd - p - 46 < nameLen) return false;   // nombre truncado (zip corrupto)
        std::string name = nameLen ? std::string((const char*)&d[p + 46], nameLen)
                                    : std::string();
        // NOMBRES peligrosos / duplicados: ver NombreZipSeguro
        if (!W3dZipNameSafe(name)) {
            w3dLogfE("W3dZip: %s trae un nombre de entrada invalido", path.c_str());
            return false;
        }
        if (!seen.insert(LowercasesAscii(name)).second) {
            w3dLogfE("W3dZip: %s trae dos entradas que solo difieren en mayusculas (%s)",
                     path.c_str(), name.c_str());
            return false;
        }
        // el local header tiene SUS PROPIOS largos de nombre/extra
        if (off > N || N - off < 30 || R32(&d[off]) != 0x04034B50) return false;
        unsigned lName = R16(&d[off + 26]), lExtra = R16(&d[off + 28]);
        if ((size_t)(N - off - 30) < (size_t)lName + lExtra) return false;
        size_t dataStart = (size_t)off + 30 + lName + lExtra;
        if (N - dataStart < compLen) return false;
        // el nombre del local header tiene que ser el MISMO que el del central
        if (lName != nameLen || (nameLen && memcmp(&d[off + 30], &d[p + 46], nameLen) != 0))
            return false;
        // COTAS de memoria ANTES de reservar (un origLen de 0xFFFFFFFF pedia 4 GB)
        if (origLen > boundEntry || compLen > 0x7FFFFFF0u) return false;
        if (boundTotal - total < origLen) return false;
        if (method == 0 && compLen != origLen) return false;   // STORE: los dos largos son el mismo
        if (method == 8 && compLen < origLen / kZipMaxRatio) return false;  // expansion imposible
        total += origLen;

        W3dZipEntry e; e.name = name;
        // el assign/resize/reserve de aca abajo son las UNICAS reservas grandes del lector:
        // si no hay memoria -> "archivo invalido", nunca un abort (ver W3D_ZIP_TRY arriba)
        W3D_ZIP_TRY
        if (method == 0) {
            e.data.assign(d.begin() + dataStart, d.begin() + dataStart + compLen);
        } else if (method == 8) {
            // DEFLATE crudo del zip (zips de terceros; el editor SIEMPRE escribe STORE).
            // Inflate() espera formato ZLIB, asi que se le antepone el header minimo
            // "78 01". No hace falta el trailer adler32: nuestro inflate (w3dCompress)
            // termina en el ultimo bloque y nunca lo lee.
            e.data.resize(origLen);
            if (origLen > 0) {
                std::vector<unsigned char> withHeader;
                withHeader.reserve(compLen + 2);
                withHeader.push_back(0x78); withHeader.push_back(0x01);
                withHeader.insert(withHeader.end(), d.begin() + dataStart, d.begin() + dataStart + compLen);
                if (!w3dEngine::Inflate(&withHeader[0], (int)withHeader.size(), &e.data[0], (int)origLen)) {
                    w3dLogfE("W3dZip: entrada DEFLATE que no pude inflar: %s", name.c_str());
                    return false;
                }
            }
        } else {
            w3dLogfE("W3dZip: metodo %u no soportado (%s)", method, name.c_str());
            return false;
        }
        // CRC32: la unica forma de saber que lo leido es lo que se guardo. Sin esto
        // un zip corrupto entraba como datos buenos y el error salia mucho despues.
        if (Crc32Of(e.data) != crcEsp) {
            w3dLogfE("W3dZip: CRC32 que no coincide en %s (%s): archivo corrupto",
                     path.c_str(), name.c_str());
            return false;
        }
        res.push_back(W3dZipEntry());
        res.back().name.swap(e.name);
        res.back().data.swap(e.data);   // swap: no duplica la memoria de la entrada
        W3D_ZIP_CATCH(path.c_str(), name.c_str())

        // el header central COMPLETO (con extra + comentario) tiene que entrar en el directorio.
        // Antes se comparaba contra N y la ULTIMA entrada se dejaba pasar igual ('break'), asi que
        // un extraLen/comLen de 0xFFFF al final daba un zip "valido" con el directorio desbordado.
        size_t step = 46 + (size_t)nameLen + extraLen + comLen;
        if (cdEnd - p < step) return false;
        p += step;
    }
    for (size_t i = 0; i < res.size(); i++) {
        out->push_back(W3dZipEntry());
        out->back().name.swap(res[i].name);
        out->back().data.swap(res[i].data);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  LECTOR POR INDICE (ver W3dZip.h)
//
//  Mismas defensas que W3dZipRead, pero SIN levantar el archivo entero: al abrir
//  se leen el EOCD y el directorio central y nada mas. Cada Read() hace un fseek
//  al local header de ESA entrada. Pico de RAM = la entrada mas grande.
// ---------------------------------------------------------------------------
// tope de los offsets: se los pasamos a fseek() como 'long', que en 32 bits es
// de 32 bits CON SIGNO. Un .w3d de mas de 2 GB se rechaza en vez de leer basura.
static const unsigned long kZipMaxFile = 0x7FFFFFF0uL;
// tope del directorio central (lo unico que se levanta entero al abrir). Con
// 65534 entradas de nombre largo no llega ni cerca: un valor mayor es mentira.
static const unsigned kZipMaxDirectory = 64u * 1024u * 1024u;

// reserva con red: si no hay memoria devuelve false en vez de matar el proceso.
// Va en una funcion aparte porque el 'return false' del catch, puesto en el medio
// de Open(), se llevaria puesto el fclose del FILE* (fuga de descriptor).
static bool ReserveBytes(std::vector<unsigned char>& v, size_t n,
                          const char* path, const char* that) {
    W3D_ZIP_TRY
    v.resize(n);
    W3D_ZIP_CATCH(path, that)
    return true;
}

W3dZipReader::W3dZipReader() : fh(0), fileSize(0) {}
W3dZipReader::~W3dZipReader() { Close(); }

void W3dZipReader::Close() {
    if (fh) fclose((FILE*)fh);
    fh = 0; fileSize = 0;
    path.clear(); ents.clear(); byName.clear();
}

const W3dZipIndex* W3dZipReader::Find(const std::string& name) const {
    std::map<std::string, size_t>::const_iterator it = byName.find(name);
    if (it == byName.end()) return 0;
    return &ents[it->second];
}

bool W3dZipReader::Exists(const std::string& name) const { return Find(name) != 0; }

unsigned W3dZipReader::Size(const std::string& name) const {
    const W3dZipIndex* e = Find(name);
    return e ? e->origLen : 0u;
}

void W3dZipReader::List(std::vector<std::string>& out) const {
    for (size_t i = 0; i < ents.size(); i++) out.push_back(ents[i].name);
}

bool W3dZipReader::Open(const std::string& r) {
    Close();
    FILE* f = fopen(r.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long lt = ftell(f);
    if (lt < 22) { fclose(f); return false; }                       // ni el EOCD entra
    unsigned long N = (unsigned long)lt;
    if (N > kZipMaxFile) {
        w3dLogfE("W3dZip: %s pasa los 2 GB: no lo puedo indexar (haria falta ZIP64/fseeko)", r.c_str());
        fclose(f); return false;
    }
    // --- EOCD: esta en los ultimos 22 bytes + el comentario del zip (max 64 KB) ---
    unsigned long queueSize = 22uL + 0xFFFFuL;
    if (queueSize > N) queueSize = N;
    std::vector<unsigned char> queue((size_t)queueSize);   // <= 64 KB: no puede faltar memoria
    if (fseek(f, (long)(N - queueSize), SEEK_SET) != 0 ||
        fread(&queue[0], 1, (size_t)queueSize, f) != (size_t)queueSize) { fclose(f); return false; }
    size_t eocd = (size_t)-1;
    // el largo del comentario tiene que ENTRAR en lo que queda: asi un 0x06054B50
    // que cae por casualidad adentro de los datos no matchea
    for (size_t i = (size_t)queueSize - 22; ; i--) {
        if (R32(&queue[i]) == 0x06054B50 && ((size_t)queueSize - i - 22) >= R16(&queue[i + 20])) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == (size_t)-1) { fclose(f); return false; }
    unsigned nEntries = R16(&queue[eocd + 10]);
    unsigned cdSize   = R32(&queue[eocd + 12]);
    unsigned cdStart  = R32(&queue[eocd + 16]);
    unsigned long eocdAbs = (N - queueSize) + eocd;
    // TODAS las cuentas con RESTAS (una suma de u32 envuelve y deja pasar offsets absurdos)
    if ((unsigned long)cdStart > eocdAbs || (eocdAbs - cdStart) < (unsigned long)cdSize) { fclose(f); return false; }
    // cota barata contra un nEntries mentiroso (cada header central son >= 46 bytes)
    if (cdSize / 46u < nEntries) { fclose(f); return false; }
    // el directorio central es lo UNICO que se levanta a RAM al abrir: se le pone
    // tope duro para no depender de que el resize no tire bad_alloc en 32 bits
    if (cdSize > kZipMaxDirectory) {
        w3dLogfE("W3dZip: %s dice tener un directorio central de %u bytes: no le creo", r.c_str(), cdSize);
        fclose(f); return false;
    }

    std::vector<unsigned char> cd;
    if (cdSize) {
        if (!ReserveBytes(cd, cdSize, r.c_str(), "directorio central")) { fclose(f); return false; }
        if (fseek(f, (long)cdStart, SEEK_SET) != 0 ||
            fread(&cd[0], 1, cdSize, f) != cdSize) { fclose(f); return false; }
    }
    std::vector<W3dZipIndex> idx;
    std::map<std::string, size_t> map;
    std::set<std::string> seen;
    size_t p = 0;
    const size_t cdEnd = (size_t)cdSize;
    for (unsigned i = 0; i < nEntries; i++) {
        if (p > cdEnd || cdEnd - p < 46 || R32(&cd[p]) != 0x02014B50) { fclose(f); return false; }
        W3dZipIndex e;
        e.method         = R16(&cd[p + 10]);
        e.crc            = R32(&cd[p + 16]);
        e.compLen        = R32(&cd[p + 20]);
        e.origLen        = R32(&cd[p + 24]);
        unsigned nameLen  = R16(&cd[p + 28]);
        unsigned extraLen = R16(&cd[p + 30]);
        unsigned comLen   = R16(&cd[p + 32]);
        e.offLocal       = R32(&cd[p + 42]);
        if (cdEnd - p - 46 < nameLen) { fclose(f); return false; }   // nombre truncado
        e.name = nameLen ? std::string((const char*)&cd[p + 46], nameLen) : std::string();
        if (!W3dZipNameSafe(e.name)) {
            w3dLogfE("W3dZip: %s trae un nombre de entrada invalido", r.c_str());
            fclose(f); return false;
        }
        if (!seen.insert(LowercasesAscii(e.name)).second) {
            // R3.3 de la spec: en Windows/macOS se extraeria mal (una pisa a la otra)
            w3dLogfE("W3dZip: %s trae dos entradas que solo difieren en mayusculas (%s)",
                     r.c_str(), e.name.c_str());
            fclose(f); return false;
        }
        // el local header (30 bytes) tiene que estar ADENTRO del archivo. Lo demas
        // (nombre/extra propios y los datos) se valida al leer la entrada.
        if ((unsigned long)e.offLocal > N || N - e.offLocal < 30) { fclose(f); return false; }
        if (e.method == 0 && e.compLen != e.origLen) { fclose(f); return false; }  // STORE
        if ((unsigned long)e.compLen > N) { fclose(f); return false; }
        map[e.name] = idx.size();
        idx.push_back(e);
        size_t step = 46 + (size_t)nameLen + extraLen + comLen;
        if (cdEnd - p < step) { fclose(f); return false; }
        p += step;
    }
    fh = (void*)f; path = r; fileSize = N;
    ents.swap(idx); byName.swap(map);
    return true;
}

bool W3dZipReader::DataOf(const W3dZipIndex* e, unsigned long& start) {
    if (e->dataStart) { start = e->dataStart; return true; }
    FILE* f = (FILE*)fh;
    if (fseek(f, (long)e->offLocal, SEEK_SET) != 0) return false;
    unsigned char lh[30];
    if (fread(lh, 1, 30, f) != 30 || R32(lh) != 0x04034B50) return false;
    unsigned lName = R16(lh + 26), lExtra = R16(lh + 28);
    // el nombre del local header tiene que ser el MISMO que el del central
    if (lName != (unsigned)e->name.size()) return false;
    if (lName) {
        std::vector<char> nl(lName);
        if (fread(&nl[0], 1, lName, f) != lName) return false;
        if (memcmp(&nl[0], e->name.c_str(), lName) != 0) return false;
    }
    const unsigned long dataStart = (unsigned long)e->offLocal + 30uL + lName + lExtra;
    // con RESTAS: los datos tienen que entrar enteros en el archivo
    if (dataStart > fileSize || fileSize - dataStart < (unsigned long)e->compLen) return false;
    e->dataStart = start = dataStart;
    return true;
}

bool W3dZipReader::ReadRaw(const std::string& name, std::vector<unsigned char>& out,
                             unsigned* method, unsigned* crc, unsigned* origLen) {
    out.clear();
    if (!fh) return false;
    const W3dZipIndex* e = Find(name);
    if (!e) return false;
    FILE* f = (FILE*)fh;
    unsigned long dataStart = 0;
    if (!DataOf(e, dataStart) || fseek(f, (long)dataStart, SEEK_SET) != 0) return false;
    W3D_ZIP_TRY
    out.resize(e->compLen);
    W3D_ZIP_CATCH(path.c_str(), name.c_str())
    if (e->compLen && fread(&out[0], 1, e->compLen, f) != e->compLen) { out.clear(); return false; }
    if (method)  *method  = e->method;
    if (crc)     *crc     = e->crc;
    if (origLen) *origLen = e->origLen;
    return true;
}

bool W3dZipReader::Info(const std::string& name, unsigned* crc, unsigned* origLen) {
    if (!fh) return false;
    const W3dZipIndex* e = Find(name);
    unsigned long dataStart = 0;
    if (!e || !DataOf(e, dataStart)) return false;
    if (crc)     *crc     = e->crc;
    if (origLen) *origLen = e->origLen;
    return true;
}

bool W3dZipReader::Read(const std::string& name, std::vector<unsigned char>& out) {
    out.clear();
    if (!fh) return false;
    const W3dZipIndex* e = Find(name);
    if (!e) return false;
    // COTAS antes de reservar: lo guardado sin comprimir nunca puede pasar del
    // tamano del archivo, y un origLen inventado no pasa de la cota dura
    const unsigned long boundEntry = (fileSize > (unsigned long)kZipMaxEntry)
                                    ? fileSize : (unsigned long)kZipMaxEntry;
    if ((unsigned long)e->origLen > boundEntry) {
        w3dLogfE("W3dZip: %s: la entrada %s dice %u bytes: no le creo",
                 path.c_str(), name.c_str(), e->origLen);
        return false;
    }
    if (e->method == 0) {
        // STORE: los bytes del archivo SON la entrada (una sola copia en RAM)
        if (!ReadRaw(name, out, 0, 0, 0)) return false;
    } else if (e->method == 8) {
        if (e->compLen < e->origLen / kZipMaxRatio) return false;   // expansion imposible
        std::vector<unsigned char> comp;
        if (!ReadRaw(name, comp, 0, 0, 0)) return false;
        W3D_ZIP_TRY
        out.resize(e->origLen);
        W3D_ZIP_CATCH(path.c_str(), name.c_str())
        if (e->origLen > 0) {
            // Inflate() espera formato ZLIB: se le antepone el header minimo "78 01"
            // (el trailer adler32 no hace falta, el decoder termina en el ultimo bloque)
            std::vector<unsigned char> withHeader;
            W3D_ZIP_TRY
            withHeader.reserve(comp.size() + 2);
            W3D_ZIP_CATCH(path.c_str(), name.c_str())
            withHeader.push_back(0x78); withHeader.push_back(0x01);
            withHeader.insert(withHeader.end(), comp.begin(), comp.end());
            if (!w3dEngine::Inflate(&withHeader[0], (int)withHeader.size(), &out[0], (int)e->origLen)) {
                w3dLogfE("W3dZip: entrada DEFLATE que no pude inflar: %s", name.c_str());
                out.clear(); return false;
            }
        }
    } else {
        w3dLogfE("W3dZip: metodo %u no soportado (%s)", e->method, name.c_str());
        return false;
    }
    // CRC32: la unica forma de saber que lo leido es lo que se guardo (una vez por entrada: el archivo abierto no
    // cambia y el streaming relee lo mismo cada vez que una celda vuelve a entrar)
    if (!e->verified) {
        if (Crc32Of(out) != e->crc) {
            w3dLogfE("W3dZip: CRC32 que no coincide en %s (%s): archivo corrupto",
                     path.c_str(), name.c_str());
            out.clear(); return false;
        }
        e->verified = true;
    }
    return true;
}

bool W3dZipReader::ReadRange(const std::string& name, unsigned from, unsigned n, std::vector<unsigned char>& out) {
    out.clear();
    if (!fh) return false;
    const W3dZipIndex* e = Find(name);
    if (!e) return false;
    if (from > e->origLen || e->origLen - from < n) return false;   // (con RESTA)
    if (e->method != 0) {
        std::vector<unsigned char> all;
        if (!Read(name, all)) return false;
        out.assign(all.begin() + from, all.begin() + from + n);
        return true;
    }
    unsigned long start = 0;
    FILE* f = (FILE*)fh;
    if (!DataOf(e, start) || fseek(f, (long)(start + from), SEEK_SET) != 0) return false;
    W3D_ZIP_TRY
    out.resize(n);
    W3D_ZIP_CATCH(path.c_str(), name.c_str())
    if (n && fread(&out[0], 1, n, f) != n) { out.clear(); return false; }
    return true;
}

// mkdir -p portable (crea cada tramo de la ruta)
static void CreateFolders(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/' || path[i] == '\\')
            W3D_MKDIR(path.substr(0, i).c_str());
    W3D_MKDIR(path.c_str());
}

bool W3dZipExtract(const std::string& path, const std::string& folderDestination) {
    std::vector<W3dZipEntry> entries;
    if (!W3dZipRead(path, &entries)) return false;
    CreateFolders(folderDestination);
    for (size_t i = 0; i < entries.size(); i++) {
        const W3dZipEntry& e = entries[i];
        if (e.name.empty() || e.name[e.name.size() - 1] == '/') continue; // carpeta
        // seguridad: W3dZipRead ya rechaza rutas absolutas y ".." (el zip entero);
        // esto queda como segunda barrera por si alguien arma las entradas a mano
        if (!W3dZipNameSafe(e.name)) continue;
        std::string destination = folderDestination + "/" + e.name;
        size_t bar = destination.find_last_of("/\\");
        if (bar != std::string::npos) CreateFolders(destination.substr(0, bar));
        FILE* f = fopen(destination.c_str(), "wb");
        if (!f) { w3dLogfE("W3dZip: no pude extraer %s", destination.c_str()); return false; }
        if (!e.data.empty()) fwrite(&e.data[0], 1, e.data.size(), f);
        if (fclose(f) != 0) { w3dLogfE("W3dZip: escritura incompleta de %s", destination.c_str()); return false; }
    }
    return true;
}
