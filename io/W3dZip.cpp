// ============================================================================
//  W3dZip.cpp — ver W3dZip.h. Dialecto C++03 (Symbian compila esto).
//
//  CRITERIO DE ROBUSTEZ (el .w3d pasa a ser un contenedor zip: esto lee ARCHIVOS
//  DE TERCEROS y archivos corruptos, asi que se trata TODO como hostil):
//   - las validaciones de rangos se hacen con RESTAS sobre el tamano del archivo,
//     NUNCA con sumas: 'off + 30 > d.size()' ENVUELVE con offsets cerca de
//     0xFFFFFFFF y el chequeo pasaba dejando leer fuera del buffer.
//   - toda reserva de memoria tiene COTA (ver kZipMaxEntrada / kZipMaxRatio):
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
//  Las COTAS de arriba (kZipMaxEntrada/kZipMaxTotal) descartan un origLen mentiroso,
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
static const unsigned kZipMaxEntradas = 0xFFFEu;
// expansion maxima TEORICA de DEFLATE (~1032:1). Sirve para descartar un origLen
// mentiroso ANTES de reservar: con 1 KB comprimido no pueden salir 4 GB.
static const unsigned kZipMaxRatio = 1032u;
// tope duro por entrada y del total descomprimido (zip bomba)
static const unsigned kZipMaxEntrada = 256u * 1024u * 1024u;
static const unsigned kZipMaxTotal   = 512u * 1024u * 1024u;
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
//  gCrcTabla[0] es la tabla clasica; gCrcTabla[t][i] = el CRC de i seguido de t
//  bytes en cero (lo que avanza un byte que esta t posiciones antes del final).
// ---------------------------------------------------------------------------
static unsigned gCrcTabla[8][256];
static bool gCrcListo = false;
static void CrcInit() {
    if (gCrcListo) return;
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        gCrcTabla[0][i] = c;
    }
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = gCrcTabla[0][i];
        for (int t = 1; t < 8; t++) {
            c = gCrcTabla[0][c & 0xFFu] ^ (c >> 8);
            gCrcTabla[t][i] = c;
        }
    }
    gCrcListo = true;
}
static unsigned Crc32(const unsigned char* d, size_t n) {
    CrcInit();
    unsigned c = 0xFFFFFFFFu;
    while (n >= 8) {
        const unsigned a = c ^ ((unsigned)d[0] | ((unsigned)d[1] << 8) |
                                ((unsigned)d[2] << 16) | ((unsigned)d[3] << 24));
        c = gCrcTabla[7][a & 0xFFu] ^ gCrcTabla[6][(a >> 8) & 0xFFu] ^
            gCrcTabla[5][(a >> 16) & 0xFFu] ^ gCrcTabla[4][(a >> 24) & 0xFFu] ^
            gCrcTabla[3][d[4]] ^ gCrcTabla[2][d[5]] ^ gCrcTabla[1][d[6]] ^ gCrcTabla[0][d[7]];
        d += 8; n -= 8;
    }
    // la cola (menos de 8 bytes): byte a byte, como siempre
    while (n > 0) {
        c = gCrcTabla[0][(c ^ *d) & 0xFFu] ^ (c >> 8);
        d++; n--;
    }
    return c ^ 0xFFFFFFFFu;
}
unsigned W3dZipCrc32(const unsigned char* d, size_t n) { return (d && n) ? Crc32(d, n) : 0u; }
static unsigned Crc32De(const std::vector<unsigned char>& v) {
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
bool W3dZipNombreSeguro(const std::string& n) {
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
    size_t ini = 0;
    for (;;) {
        size_t fin = n.find('/', ini);
        if (fin == std::string::npos) fin = n.size();
        if (fin - ini == 2 && n[ini] == '.' && n[ini + 1] == '.') return false;
        if (fin == n.size()) break;
        ini = fin + 1;
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
static bool EsArchivoRegular(const char*) { return true; }   // Windows no tiene /dev/*
#else
static bool EsArchivoRegular(const char* p) {
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return S_ISREG(st.st_mode) != 0;
}
#endif

// minusculas ASCII: con esto se detectan los duplicados que Windows y macOS
// (case-insensitive) pisarian EN SILENCIO al extraer -> perdida de datos
static std::string MinusculasAscii(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); i++)
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    return r;
}

// ---------------------------------------------------------------------------
//  ESCRITURA (metodo 0 = STORE)
// ---------------------------------------------------------------------------
void W3dZipWriter::Agregar(const std::string& nombre, const void* datos, size_t n) {
    W3dZipEntrada e;
    e.nombre = nombre;
    e.datos.assign((const unsigned char*)datos, (const unsigned char*)datos + n);
    entradas.push_back(e);
}
void W3dZipWriter::Agregar(const std::string& nombre, const std::vector<unsigned char>& datos) {
    W3dZipEntrada e; e.nombre = nombre; e.datos = datos;
    entradas.push_back(e);
}
void W3dZipWriter::AgregarTexto(const std::string& nombre, const std::string& texto) {
    Agregar(nombre, texto.c_str(), texto.size());
}
bool W3dZipWriter::AgregarArchivo(const std::string& nombre, const std::string& rutaDisco) {
    std::vector<unsigned char> d;
    if (!w3dFileSystem::ReadFileBytes(rutaDisco, d)) return false;
    Agregar(nombre, d);
    return true;
}

// ---------------------------------------------------------------------------
//  ESCRITURA INCREMENTAL (streaming): cada entrada se vuelca EN EL ACTO
// ---------------------------------------------------------------------------
W3dZipWriter::W3dZipWriter() : wf(0), wfallo(false) {}
W3dZipWriter::~W3dZipWriter() {
    // si alguien se olvido de Cerrar(), NO se deja un zip a medias en el disco:
    // sin directorio central no lo abre nadie y ademas pisaria un archivo bueno
    if (wf) Abortar();
}

bool W3dZipWriter::AbrirEscritura(const std::string& ruta) {
    if (wf) { w3dLogfE("W3dZip: AbrirEscritura con otro zip ya abierto (%s)", wruta.c_str()); return false; }
    FILE* f = fopen(ruta.c_str(), "wb");
    if (!f) { w3dLogfE("W3dZip: no pude escribir %s", ruta.c_str()); return false; }
    wf = (void*)f; wruta = ruta; wdir.clear(); wvistos.clear(); wfallo = false;
    return true;
}

// local header + nombre. Deja el archivo posicionado justo donde van los datos.
bool W3dZipWriter::VolcarCabecera(const std::string& nombre, unsigned crc, unsigned compLen,
                                  unsigned origLen, unsigned metodo) {
    if (!wf || wfallo) return false;
    FILE* f = (FILE*)wf;
    if (wdir.size() >= (size_t)kZipMaxEntradas) {
        w3dLogfE("W3dZip: mas de %u entradas: no entra sin ZIP64", kZipMaxEntradas);
        wfallo = true; return false;
    }
    if (!W3dZipNombreSeguro(nombre)) {
        w3dLogfE("W3dZip: nombre de entrada invalido: %s", nombre.c_str());
        wfallo = true; return false;
    }
    if (!wvistos.insert(MinusculasAscii(nombre)).second) {
        // en Windows/macOS el unzip pisaria una con la otra SIN AVISAR
        w3dLogfE("W3dZip: dos entradas que solo difieren en mayusculas: %s", nombre.c_str());
        wfallo = true; return false;
    }
    Vuelta v;
    // el offset del local header va en un u32: pasado los 4 GB haria falta ZIP64
    // y el zip saldria roto (mejor fallar y decirlo)
    if (!OffsetU32(ftell(f), &v.off)) {
        w3dLogfE("W3dZip: %s pasa los 4 GB (haria falta ZIP64)", wruta.c_str());
        wfallo = true; return false;
    }
    v.nombre = nombre; v.crc = crc; v.compLen = compLen; v.origLen = origLen; v.metodo = metodo;
    W32(f, 0x04034B50);            // PK\3\4
    W16(f, 20); W16(f, kZipFlagUtf8);   // version / flags (bit 11: nombre UTF-8)
    W16(f, metodo);                // 0 = STORE (lo que escribe el editor)
    W16(f, 0); W16(f, 0x21);       // hora/fecha FIJA: dos guardados sin cambios dan
                                   // los MISMOS bytes (diff en git, dedup, round-trip)
    W32(f, crc);
    W32(f, compLen);
    W32(f, origLen);
    W16(f, (unsigned)nombre.size()); W16(f, 0);
    fwrite(nombre.c_str(), 1, nombre.size(), f);
    wdir.push_back(v);
    return true;
}

bool W3dZipWriter::Escribir(const std::string& nombre, const void* datos, size_t n) {
    if (!wf || wfallo) return false;
    // sin ZIP64 una entrada no puede pasar los 4 GB
    if (((((unsigned long)n) >> 16) >> 16) != 0) {
        w3dLogfE("W3dZip: la entrada %s pasa los 4 GB (haria falta ZIP64)", nombre.c_str());
        wfallo = true; return false;
    }
    unsigned crc = n ? Crc32((const unsigned char*)datos, n) : 0u;
    if (!VolcarCabecera(nombre, crc, (unsigned)n, (unsigned)n, 0)) return false;
    if (n) fwrite(datos, 1, n, (FILE*)wf);
    return true;
}
bool W3dZipWriter::Escribir(const std::string& nombre, const std::vector<unsigned char>& datos) {
    return Escribir(nombre, datos.empty() ? (const void*)"" : (const void*)&datos[0], datos.size());
}
bool W3dZipWriter::EscribirTexto(const std::string& nombre, const std::string& texto) {
    return Escribir(nombre, texto.c_str(), texto.size());
}

bool W3dZipWriter::CopiarDe(W3dZipLector& origen, const std::string& nombre) {
    if (!wf || wfallo) return false;
    std::vector<unsigned char> crudo;
    unsigned metodo = 0, crc = 0, origLen = 0;
    if (!origen.LeerCrudo(nombre, crudo, &metodo, &crc, &origLen)) {
        w3dLogfE("W3dZip: CopiarDe: no pude leer '%s' de %s", nombre.c_str(), origen.Ruta().c_str());
        wfallo = true; return false;
    }
    // se reenvasan los MISMOS bytes: ni se descomprime ni se recalcula el CRC
    if (!VolcarCabecera(nombre, crc, (unsigned)crudo.size(), origLen, metodo)) return false;
    if (!crudo.empty()) fwrite(&crudo[0], 1, crudo.size(), (FILE*)wf);
    return true;
}

bool W3dZipWriter::Cerrar() {
    if (!wf) return false;
    FILE* f = (FILE*)wf;
    if (wfallo) { Abortar(); return false; }
    // 2) central directory
    unsigned cdIni = 0;
    if (!OffsetU32(ftell(f), &cdIni)) {
        w3dLogfE("W3dZip: %s pasa los 4 GB (haria falta ZIP64)", wruta.c_str());
        Abortar(); return false;
    }
    for (size_t i = 0; i < wdir.size(); i++) {
        const Vuelta& v = wdir[i];
        W32(f, 0x02014B50);            // PK\1\2
        W16(f, 20); W16(f, 20); W16(f, kZipFlagUtf8); W16(f, v.metodo);   // flags: idem local
        W16(f, 0); W16(f, 0x21);
        W32(f, v.crc);
        W32(f, v.compLen);
        W32(f, v.origLen);
        W16(f, (unsigned)v.nombre.size()); W16(f, 0); W16(f, 0);
        W16(f, 0); W16(f, 0); W32(f, 0);
        W32(f, v.off);
        fwrite(v.nombre.c_str(), 1, v.nombre.size(), f);
    }
    unsigned cdFin = 0;
    if (!OffsetU32(ftell(f), &cdFin)) {
        w3dLogfE("W3dZip: %s pasa los 4 GB (haria falta ZIP64)", wruta.c_str());
        Abortar(); return false;
    }
    // 3) end of central directory
    W32(f, 0x06054B50);                // PK\5\6
    W16(f, 0); W16(f, 0);
    W16(f, (unsigned)wdir.size()); W16(f, (unsigned)wdir.size());
    W32(f, cdFin - cdIni);
    W32(f, cdIni);
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

void W3dZipWriter::Abortar() {
    if (!wf) return;
    fclose((FILE*)wf);
    wf = 0;
    // el zip quedo sin directorio central: no lo abre nadie. Se borra, pero SOLO
    // si es un archivo comun (ver EsArchivoRegular)
    if (!wruta.empty() && EsArchivoRegular(wruta.c_str())) remove(wruta.c_str());
    wruta.clear(); wdir.clear(); wvistos.clear(); wfallo = false;
}

// ---------------------------------------------------------------------------
//  ESCRITURA EN LOTE: es el modo incremental con los bytes juntados antes.
//  Se conserva TAL CUAL la propiedad de antes: si algun nombre no sirve, NO se
//  toca el disco (se valida el lote entero primero).
// ---------------------------------------------------------------------------
bool W3dZipWriter::Guardar(const std::string& ruta) {
    // 0) VALIDAR antes de tocar el disco: si el zip que se va a escribir tiene
    //    nombres peligrosos, duplicados case-folded o no entra en el formato
    //    (sin ZIP64), no se escribe NADA y se dice por que.
    if (entradas.size() > (size_t)kZipMaxEntradas) {
        w3dLogfE("W3dZip: %d entradas: no entran en el zip sin ZIP64 (max %u)",
                 (int)entradas.size(), kZipMaxEntradas);
        return false;
    }
    {
        std::set<std::string> vistos;
        for (size_t i = 0; i < entradas.size(); i++) {
            const std::string& n = entradas[i].nombre;
            if (!W3dZipNombreSeguro(n)) {
                w3dLogfE("W3dZip: nombre de entrada invalido: %s", n.c_str());
                return false;
            }
            if (!vistos.insert(MinusculasAscii(n)).second) {
                // en Windows/macOS el unzip pisaria una con la otra SIN AVISAR
                w3dLogfE("W3dZip: dos entradas que solo difieren en mayusculas: %s", n.c_str());
                return false;
            }
        }
    }
    if (!AbrirEscritura(ruta)) return false;
    for (size_t i = 0; i < entradas.size(); i++) {
        if (!Escribir(entradas[i].nombre, entradas[i].datos)) { Abortar(); return false; }
    }
    return Cerrar();
}

// ---------------------------------------------------------------------------
//  LECTURA
// ---------------------------------------------------------------------------
bool W3dZipEs(const std::string& ruta) {
    FILE* f = fopen(ruta.c_str(), "rb");
    if (!f) return false;
    unsigned char m[4] = { 0, 0, 0, 0 };
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    return n == 4 && m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4;
}

bool W3dZipLeer(const std::string& ruta, std::vector<W3dZipEntrada>* out) {
    if (!out) return false;
    std::vector<unsigned char> d;
    if (!w3dFileSystem::ReadFileBytes(ruta, d) || d.size() < 22) return false;
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
    unsigned nEntradas = R16(&d[eocd + 10]);
    unsigned cdTam     = R32(&d[eocd + 12]);
    unsigned cdIni     = R32(&d[eocd + 16]);
    // TODAS las cuentas con restas: 'cdIni + cdTam > N' envuelve y deja pasar
    // offsets absurdos. El directorio central tiene que estar COMPLETO antes del EOCD.
    if (cdIni > eocd || (size_t)(eocd - cdIni) < cdTam) return false;
    // cota barata contra un nEntradas mentiroso (cada header central son >= 46 bytes)
    if ((size_t)(eocd - cdIni) / 46 < nEntradas) return false;

    // se arma aparte y recien al final se pasa a 'out': si el zip resulta
    // corrupto a mitad de camino, el vector del llamador queda como estaba
    std::vector<W3dZipEntrada> res;
    std::set<std::string> vistos;
    // las cotas se toman contra el TAMANO REAL del archivo: lo guardado sin
    // comprimir (STORE) nunca puede pasar de N, asi que un zip legitimo grande
    // (assets de varios GB) sigue abriendo, y un origLen inventado no.
    const size_t cotaEntrada = (N > (size_t)kZipMaxEntrada) ? N : (size_t)kZipMaxEntrada;
    const size_t cotaTotal   = (N > (size_t)kZipMaxTotal)   ? N : (size_t)kZipMaxTotal;
    size_t total = 0;
    size_t p = cdIni;
    // FIN del directorio central (cdIni+cdTam, ya validado <= eocd <= N mas arriba). TODAS las
    // cotas del recorrido van contra ESTE limite, no contra N: acotar solo contra el tamano del
    // archivo dejaba pasar una entrada cuyo extraLen/comLen de 0xFFFF se comia los datos que
    // vienen DESPUES del directorio (el zip se aceptaba igual).
    const size_t cdFin = (size_t)cdIni + cdTam;
    for (unsigned i = 0; i < nEntradas; i++) {
        if (p > cdFin || cdFin - p < 46 || R32(&d[p]) != 0x02014B50) return false;
        unsigned metodo   = R16(&d[p + 10]);
        unsigned crcEsp   = R32(&d[p + 16]);
        unsigned compLen  = R32(&d[p + 20]);
        unsigned origLen  = R32(&d[p + 24]);
        unsigned nomLen   = R16(&d[p + 28]);
        unsigned extraLen = R16(&d[p + 30]);
        unsigned comLen   = R16(&d[p + 32]);
        unsigned off      = R32(&d[p + 42]);
        if (cdFin - p - 46 < nomLen) return false;   // nombre truncado (zip corrupto)
        std::string nombre = nomLen ? std::string((const char*)&d[p + 46], nomLen)
                                    : std::string();
        // NOMBRES peligrosos / duplicados: ver NombreZipSeguro
        if (!W3dZipNombreSeguro(nombre)) {
            w3dLogfE("W3dZip: %s trae un nombre de entrada invalido", ruta.c_str());
            return false;
        }
        if (!vistos.insert(MinusculasAscii(nombre)).second) {
            w3dLogfE("W3dZip: %s trae dos entradas que solo difieren en mayusculas (%s)",
                     ruta.c_str(), nombre.c_str());
            return false;
        }
        // el local header tiene SUS PROPIOS largos de nombre/extra
        if (off > N || N - off < 30 || R32(&d[off]) != 0x04034B50) return false;
        unsigned lNom = R16(&d[off + 26]), lExtra = R16(&d[off + 28]);
        if ((size_t)(N - off - 30) < (size_t)lNom + lExtra) return false;
        size_t datosIni = (size_t)off + 30 + lNom + lExtra;
        if (N - datosIni < compLen) return false;
        // el nombre del local header tiene que ser el MISMO que el del central
        if (lNom != nomLen || (nomLen && memcmp(&d[off + 30], &d[p + 46], nomLen) != 0))
            return false;
        // COTAS de memoria ANTES de reservar (un origLen de 0xFFFFFFFF pedia 4 GB)
        if (origLen > cotaEntrada || compLen > 0x7FFFFFF0u) return false;
        if (cotaTotal - total < origLen) return false;
        if (metodo == 0 && compLen != origLen) return false;   // STORE: los dos largos son el mismo
        if (metodo == 8 && compLen < origLen / kZipMaxRatio) return false;  // expansion imposible
        total += origLen;

        W3dZipEntrada e; e.nombre = nombre;
        // el assign/resize/reserve de aca abajo son las UNICAS reservas grandes del lector:
        // si no hay memoria -> "archivo invalido", nunca un abort (ver W3D_ZIP_TRY arriba)
        W3D_ZIP_TRY
        if (metodo == 0) {
            e.datos.assign(d.begin() + datosIni, d.begin() + datosIni + compLen);
        } else if (metodo == 8) {
            // DEFLATE crudo del zip (zips de terceros; el editor SIEMPRE escribe STORE).
            // Inflate() espera formato ZLIB, asi que se le antepone el header minimo
            // "78 01". No hace falta el trailer adler32: nuestro inflate (w3dCompress)
            // termina en el ultimo bloque y nunca lo lee.
            e.datos.resize(origLen);
            if (origLen > 0) {
                std::vector<unsigned char> conHeader;
                conHeader.reserve(compLen + 2);
                conHeader.push_back(0x78); conHeader.push_back(0x01);
                conHeader.insert(conHeader.end(), d.begin() + datosIni, d.begin() + datosIni + compLen);
                if (!w3dEngine::Inflate(&conHeader[0], (int)conHeader.size(), &e.datos[0], (int)origLen)) {
                    w3dLogfE("W3dZip: entrada DEFLATE que no pude inflar: %s", nombre.c_str());
                    return false;
                }
            }
        } else {
            w3dLogfE("W3dZip: metodo %u no soportado (%s)", metodo, nombre.c_str());
            return false;
        }
        // CRC32: la unica forma de saber que lo leido es lo que se guardo. Sin esto
        // un zip corrupto entraba como datos buenos y el error salia mucho despues.
        if (Crc32De(e.datos) != crcEsp) {
            w3dLogfE("W3dZip: CRC32 que no coincide en %s (%s): archivo corrupto",
                     ruta.c_str(), nombre.c_str());
            return false;
        }
        res.push_back(W3dZipEntrada());
        res.back().nombre.swap(e.nombre);
        res.back().datos.swap(e.datos);   // swap: no duplica la memoria de la entrada
        W3D_ZIP_CATCH(ruta.c_str(), nombre.c_str())

        // el header central COMPLETO (con extra + comentario) tiene que entrar en el directorio.
        // Antes se comparaba contra N y la ULTIMA entrada se dejaba pasar igual ('break'), asi que
        // un extraLen/comLen de 0xFFFF al final daba un zip "valido" con el directorio desbordado.
        size_t paso = 46 + (size_t)nomLen + extraLen + comLen;
        if (cdFin - p < paso) return false;
        p += paso;
    }
    for (size_t i = 0; i < res.size(); i++) {
        out->push_back(W3dZipEntrada());
        out->back().nombre.swap(res[i].nombre);
        out->back().datos.swap(res[i].datos);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  LECTOR POR INDICE (ver W3dZip.h)
//
//  Mismas defensas que W3dZipLeer, pero SIN levantar el archivo entero: al abrir
//  se leen el EOCD y el directorio central y nada mas. Cada Leer() hace un fseek
//  al local header de ESA entrada. Pico de RAM = la entrada mas grande.
// ---------------------------------------------------------------------------
// tope de los offsets: se los pasamos a fseek() como 'long', que en 32 bits es
// de 32 bits CON SIGNO. Un .w3d de mas de 2 GB se rechaza en vez de leer basura.
static const unsigned long kZipMaxArchivo = 0x7FFFFFF0uL;
// tope del directorio central (lo unico que se levanta entero al abrir). Con
// 65534 entradas de nombre largo no llega ni cerca: un valor mayor es mentira.
static const unsigned kZipMaxDirectorio = 64u * 1024u * 1024u;

// reserva con red: si no hay memoria devuelve false en vez de matar el proceso.
// Va en una funcion aparte porque el 'return false' del catch, puesto en el medio
// de Abrir(), se llevaria puesto el fclose del FILE* (fuga de descriptor).
static bool ReservarBytes(std::vector<unsigned char>& v, size_t n,
                          const char* ruta, const char* que) {
    W3D_ZIP_TRY
    v.resize(n);
    W3D_ZIP_CATCH(ruta, que)
    return true;
}

W3dZipLector::W3dZipLector() : fh(0), tamArchivo(0) {}
W3dZipLector::~W3dZipLector() { Cerrar(); }

void W3dZipLector::Cerrar() {
    if (fh) fclose((FILE*)fh);
    fh = 0; tamArchivo = 0;
    ruta.clear(); ents.clear(); porNombre.clear();
}

const W3dZipIndice* W3dZipLector::Buscar(const std::string& nombre) const {
    std::map<std::string, size_t>::const_iterator it = porNombre.find(nombre);
    if (it == porNombre.end()) return 0;
    return &ents[it->second];
}

bool W3dZipLector::Existe(const std::string& nombre) const { return Buscar(nombre) != 0; }

unsigned W3dZipLector::Tam(const std::string& nombre) const {
    const W3dZipIndice* e = Buscar(nombre);
    return e ? e->origLen : 0u;
}

void W3dZipLector::Listar(std::vector<std::string>& out) const {
    for (size_t i = 0; i < ents.size(); i++) out.push_back(ents[i].nombre);
}

bool W3dZipLector::Abrir(const std::string& r) {
    Cerrar();
    FILE* f = fopen(r.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long lt = ftell(f);
    if (lt < 22) { fclose(f); return false; }                       // ni el EOCD entra
    unsigned long N = (unsigned long)lt;
    if (N > kZipMaxArchivo) {
        w3dLogfE("W3dZip: %s pasa los 2 GB: no lo puedo indexar (haria falta ZIP64/fseeko)", r.c_str());
        fclose(f); return false;
    }
    // --- EOCD: esta en los ultimos 22 bytes + el comentario del zip (max 64 KB) ---
    unsigned long colaTam = 22uL + 0xFFFFuL;
    if (colaTam > N) colaTam = N;
    std::vector<unsigned char> cola((size_t)colaTam);   // <= 64 KB: no puede faltar memoria
    if (fseek(f, (long)(N - colaTam), SEEK_SET) != 0 ||
        fread(&cola[0], 1, (size_t)colaTam, f) != (size_t)colaTam) { fclose(f); return false; }
    size_t eocd = (size_t)-1;
    // el largo del comentario tiene que ENTRAR en lo que queda: asi un 0x06054B50
    // que cae por casualidad adentro de los datos no matchea
    for (size_t i = (size_t)colaTam - 22; ; i--) {
        if (R32(&cola[i]) == 0x06054B50 && ((size_t)colaTam - i - 22) >= R16(&cola[i + 20])) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == (size_t)-1) { fclose(f); return false; }
    unsigned nEntradas = R16(&cola[eocd + 10]);
    unsigned cdTam     = R32(&cola[eocd + 12]);
    unsigned cdIni     = R32(&cola[eocd + 16]);
    unsigned long eocdAbs = (N - colaTam) + eocd;
    // TODAS las cuentas con RESTAS (una suma de u32 envuelve y deja pasar offsets absurdos)
    if ((unsigned long)cdIni > eocdAbs || (eocdAbs - cdIni) < (unsigned long)cdTam) { fclose(f); return false; }
    // cota barata contra un nEntradas mentiroso (cada header central son >= 46 bytes)
    if (cdTam / 46u < nEntradas) { fclose(f); return false; }
    // el directorio central es lo UNICO que se levanta a RAM al abrir: se le pone
    // tope duro para no depender de que el resize no tire bad_alloc en 32 bits
    if (cdTam > kZipMaxDirectorio) {
        w3dLogfE("W3dZip: %s dice tener un directorio central de %u bytes: no le creo", r.c_str(), cdTam);
        fclose(f); return false;
    }

    std::vector<unsigned char> cd;
    if (cdTam) {
        if (!ReservarBytes(cd, cdTam, r.c_str(), "directorio central")) { fclose(f); return false; }
        if (fseek(f, (long)cdIni, SEEK_SET) != 0 ||
            fread(&cd[0], 1, cdTam, f) != cdTam) { fclose(f); return false; }
    }
    std::vector<W3dZipIndice> idx;
    std::map<std::string, size_t> mapa;
    std::set<std::string> vistos;
    size_t p = 0;
    const size_t cdFin = (size_t)cdTam;
    for (unsigned i = 0; i < nEntradas; i++) {
        if (p > cdFin || cdFin - p < 46 || R32(&cd[p]) != 0x02014B50) { fclose(f); return false; }
        W3dZipIndice e;
        e.metodo         = R16(&cd[p + 10]);
        e.crc            = R32(&cd[p + 16]);
        e.compLen        = R32(&cd[p + 20]);
        e.origLen        = R32(&cd[p + 24]);
        unsigned nomLen   = R16(&cd[p + 28]);
        unsigned extraLen = R16(&cd[p + 30]);
        unsigned comLen   = R16(&cd[p + 32]);
        e.offLocal       = R32(&cd[p + 42]);
        if (cdFin - p - 46 < nomLen) { fclose(f); return false; }   // nombre truncado
        e.nombre = nomLen ? std::string((const char*)&cd[p + 46], nomLen) : std::string();
        if (!W3dZipNombreSeguro(e.nombre)) {
            w3dLogfE("W3dZip: %s trae un nombre de entrada invalido", r.c_str());
            fclose(f); return false;
        }
        if (!vistos.insert(MinusculasAscii(e.nombre)).second) {
            // R3.3 de la spec: en Windows/macOS se extraeria mal (una pisa a la otra)
            w3dLogfE("W3dZip: %s trae dos entradas que solo difieren en mayusculas (%s)",
                     r.c_str(), e.nombre.c_str());
            fclose(f); return false;
        }
        // el local header (30 bytes) tiene que estar ADENTRO del archivo. Lo demas
        // (nombre/extra propios y los datos) se valida al leer la entrada.
        if ((unsigned long)e.offLocal > N || N - e.offLocal < 30) { fclose(f); return false; }
        if (e.metodo == 0 && e.compLen != e.origLen) { fclose(f); return false; }  // STORE
        if ((unsigned long)e.compLen > N) { fclose(f); return false; }
        mapa[e.nombre] = idx.size();
        idx.push_back(e);
        size_t paso = 46 + (size_t)nomLen + extraLen + comLen;
        if (cdFin - p < paso) { fclose(f); return false; }
        p += paso;
    }
    fh = (void*)f; ruta = r; tamArchivo = N;
    ents.swap(idx); porNombre.swap(mapa);
    return true;
}

bool W3dZipLector::LeerCrudo(const std::string& nombre, std::vector<unsigned char>& out,
                             unsigned* metodo, unsigned* crc, unsigned* origLen) {
    out.clear();
    if (!fh) return false;
    const W3dZipIndice* e = Buscar(nombre);
    if (!e) return false;
    FILE* f = (FILE*)fh;
    if (fseek(f, (long)e->offLocal, SEEK_SET) != 0) return false;
    unsigned char lh[30];
    if (fread(lh, 1, 30, f) != 30 || R32(lh) != 0x04034B50) return false;
    unsigned lNom = R16(lh + 26), lExtra = R16(lh + 28);
    // el nombre del local header tiene que ser el MISMO que el del central
    if (lNom != (unsigned)e->nombre.size()) return false;
    if (lNom) {
        std::vector<char> nl(lNom);
        if (fread(&nl[0], 1, lNom, f) != lNom) return false;
        if (memcmp(&nl[0], e->nombre.c_str(), lNom) != 0) return false;
    }
    if (lExtra && fseek(f, (long)lExtra, SEEK_CUR) != 0) return false;
    unsigned long datosIni = (unsigned long)e->offLocal + 30uL + lNom + lExtra;
    // con RESTAS: los datos tienen que entrar enteros en el archivo
    if (datosIni > tamArchivo || tamArchivo - datosIni < (unsigned long)e->compLen) return false;
    W3D_ZIP_TRY
    out.resize(e->compLen);
    W3D_ZIP_CATCH(ruta.c_str(), nombre.c_str())
    if (e->compLen && fread(&out[0], 1, e->compLen, f) != e->compLen) { out.clear(); return false; }
    if (metodo)  *metodo  = e->metodo;
    if (crc)     *crc     = e->crc;
    if (origLen) *origLen = e->origLen;
    return true;
}

bool W3dZipLector::Leer(const std::string& nombre, std::vector<unsigned char>& out) {
    out.clear();
    if (!fh) return false;
    const W3dZipIndice* e = Buscar(nombre);
    if (!e) return false;
    // COTAS antes de reservar: lo guardado sin comprimir nunca puede pasar del
    // tamano del archivo, y un origLen inventado no pasa de la cota dura
    const unsigned long cotaEntrada = (tamArchivo > (unsigned long)kZipMaxEntrada)
                                    ? tamArchivo : (unsigned long)kZipMaxEntrada;
    if ((unsigned long)e->origLen > cotaEntrada) {
        w3dLogfE("W3dZip: %s: la entrada %s dice %u bytes: no le creo",
                 ruta.c_str(), nombre.c_str(), e->origLen);
        return false;
    }
    if (e->metodo == 0) {
        // STORE: los bytes del archivo SON la entrada (una sola copia en RAM)
        if (!LeerCrudo(nombre, out, 0, 0, 0)) return false;
    } else if (e->metodo == 8) {
        if (e->compLen < e->origLen / kZipMaxRatio) return false;   // expansion imposible
        std::vector<unsigned char> comp;
        if (!LeerCrudo(nombre, comp, 0, 0, 0)) return false;
        W3D_ZIP_TRY
        out.resize(e->origLen);
        W3D_ZIP_CATCH(ruta.c_str(), nombre.c_str())
        if (e->origLen > 0) {
            // Inflate() espera formato ZLIB: se le antepone el header minimo "78 01"
            // (el trailer adler32 no hace falta, el decoder termina en el ultimo bloque)
            std::vector<unsigned char> conHeader;
            W3D_ZIP_TRY
            conHeader.reserve(comp.size() + 2);
            W3D_ZIP_CATCH(ruta.c_str(), nombre.c_str())
            conHeader.push_back(0x78); conHeader.push_back(0x01);
            conHeader.insert(conHeader.end(), comp.begin(), comp.end());
            if (!w3dEngine::Inflate(&conHeader[0], (int)conHeader.size(), &out[0], (int)e->origLen)) {
                w3dLogfE("W3dZip: entrada DEFLATE que no pude inflar: %s", nombre.c_str());
                out.clear(); return false;
            }
        }
    } else {
        w3dLogfE("W3dZip: metodo %u no soportado (%s)", e->metodo, nombre.c_str());
        return false;
    }
    // CRC32: la unica forma de saber que lo leido es lo que se guardo
    if (Crc32De(out) != e->crc) {
        w3dLogfE("W3dZip: CRC32 que no coincide en %s (%s): archivo corrupto",
                 ruta.c_str(), nombre.c_str());
        out.clear(); return false;
    }
    return true;
}

// mkdir -p portable (crea cada tramo de la ruta)
static void CrearCarpetas(const std::string& ruta) {
    for (size_t i = 1; i < ruta.size(); i++)
        if (ruta[i] == '/' || ruta[i] == '\\')
            W3D_MKDIR(ruta.substr(0, i).c_str());
    W3D_MKDIR(ruta.c_str());
}

bool W3dZipExtraer(const std::string& ruta, const std::string& carpetaDestino) {
    std::vector<W3dZipEntrada> entradas;
    if (!W3dZipLeer(ruta, &entradas)) return false;
    CrearCarpetas(carpetaDestino);
    for (size_t i = 0; i < entradas.size(); i++) {
        const W3dZipEntrada& e = entradas[i];
        if (e.nombre.empty() || e.nombre[e.nombre.size() - 1] == '/') continue; // carpeta
        // seguridad: W3dZipLeer ya rechaza rutas absolutas y ".." (el zip entero);
        // esto queda como segunda barrera por si alguien arma las entradas a mano
        if (!W3dZipNombreSeguro(e.nombre)) continue;
        std::string destino = carpetaDestino + "/" + e.nombre;
        size_t barra = destino.find_last_of("/\\");
        if (barra != std::string::npos) CrearCarpetas(destino.substr(0, barra));
        FILE* f = fopen(destino.c_str(), "wb");
        if (!f) { w3dLogfE("W3dZip: no pude extraer %s", destino.c_str()); return false; }
        if (!e.datos.empty()) fwrite(&e.datos[0], 1, e.datos.size(), f);
        if (fclose(f) != 0) { w3dLogfE("W3dZip: escritura incompleta de %s", destino.c_str()); return false; }
    }
    return true;
}
