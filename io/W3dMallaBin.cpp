#include "W3dMallaBin.h"
#include "W3dMalla.h"        // W3dMallaInfo + las piezas del escritor que comparten los dos formatos
#include "W3dTexto.h"        // W3dEscribirInt (los avisos)
#include "w3dFilesystem.h"   // MaterializarEdicion relee la entrada del contenedor
#include "objects/Mesh.h"
#include "objects/MallaRecurso.h"   // MaterializarEdicion: la edicion de una malla que usa un recurso
#include "base/w3dlog.h"            // w3dLogf (diagnostico del reagrupamiento de puntos)

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <cstring>
#include <cstdio>

// ============================================================================
//  LOS BLOQUES QUE ESTA VERSION ENTIENDE (ver W3dMallaBin.h). Un fourcc son 4
//  letras: los de 3 se completan con un espacio ("REQ ", "RUV ").
// ============================================================================
static const char* kBloquesBin[] = {
    "REQ ", "INFO", "RPOS", "RNRM", "RUV ", "RCOL", "RIDX", "PART", "RCTL",
    "RVGR", "RSHP", "RUVG", "RUVR", "RUVX",
    "FACE", "LOOS", "CNRM", "UVLY", "COLY", "SHRP", "SEAM", "EDGE", "AJEN", 0
};
// lo MINIMO que un lector tiene que entender para no romper la malla: van en REQ
static const char* kRequeridos[] = { "INFO", "RPOS", "RIDX", "PART", 0 };

bool W3dMallaBinBloqueConocido(const std::string& f) {
    for (int i = 0; kBloquesBin[i]; i++) if (f == kBloquesBin[i]) return true;
    return false;
}

void W3dMallaBinBloquesConocidos(std::vector<std::string>& conocidos, std::vector<std::string>* requeridos) {
    conocidos.clear();
    for (int i = 0; kBloquesBin[i]; i++) conocidos.push_back(kBloquesBin[i]);
    if (requeridos) {
        requeridos->clear();
        for (int i = 0; kRequeridos[i]; i++) requeridos->push_back(kRequeridos[i]);
    }
}

bool W3dMallaBinEs(const unsigned char* d, size_t n) {
    return d && n >= 4 && d[0] == 'W' && d[1] == '3' && d[2] == 'D' && d[3] == 'B';
}

bool W3dMallaBinIndicesAlcanzan(int nRV) {
    return sizeof(MeshIndex) >= 4 || nRV <= 65535;
}

// banderas de INFO
enum { INFO_SUAVE = 1, INFO_AABBOK = 2, INFO_STATS_NOEDIT = 4, INFO_PRIM = 8 };
// banderas de cada capa de COLY
enum { COLY_POR_VERTICE = 1 };

namespace {

// ===========================================================================
//  LITTLE-ENDIAN EXPLICITO. En un host little-endian (x86, ARM: todos los que
//  existen hoy) los arrays se copian con memcpy; en uno big-endian se arman byte
//  a byte. Los bytes del archivo son los mismos en los dos.
// ===========================================================================
bool HostLE() { const unsigned short x = 1; return *(const unsigned char*)&x == 1; }

unsigned LeeU32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

void PonU32(std::string& s, unsigned v) {
    char b[4];
    b[0] = (char)(v & 255u); b[1] = (char)((v >> 8) & 255u);
    b[2] = (char)((v >> 16) & 255u); b[3] = (char)((v >> 24) & 255u);
    s.append(b, 4);
}
void PonI32(std::string& s, int v) { PonU32(s, (unsigned)v); }
void PonF32(std::string& s, float f) { unsigned u; memcpy(&u, &f, 4); PonU32(s, u); }
void PonStr(std::string& s, const std::string& t) { PonU32(s, (unsigned)t.size()); s.append(t); }
void PonF32s(std::string& s, const float* p, size_t n) {
    if (!n) return;
    if (HostLE()) s.append((const char*)p, n * 4);
    else for (size_t i = 0; i < n; i++) PonF32(s, p[i]);
}
void PonI32s(std::string& s, const int* p, size_t n) {
    if (!n) return;
    if (HostLE()) s.append((const char*)p, n * 4);
    else for (size_t i = 0; i < n; i++) PonI32(s, p[i]);
}
void PonBytes(std::string& s, const void* p, size_t n) { if (n) s.append((const char*)p, n); }

void CopiarF32(const unsigned char* src, float* dst, size_t n) {
    if (!n) return;
    if (HostLE()) { memcpy(dst, src, n * 4); return; }
    for (size_t i = 0; i < n; i++) { unsigned u = LeeU32(src + i * 4); memcpy(&dst[i], &u, 4); }
}
void CopiarI32(const unsigned char* src, int* dst, size_t n) {
    if (!n) return;
    if (HostLE()) { memcpy(dst, src, n * 4); return; }
    for (size_t i = 0; i < n; i++) dst[i] = (int)LeeU32(src + i * 4);
}
int LeeI32(const unsigned char* p) { return (int)LeeU32(p); }
// CLAVE DE MARCA (SHRP/SEAM): las dos posiciones de la arista, 3 floats cada una (24 bytes).
// En MEMORIA es Mesh::SharpEdgeKey: los bytes crudos del HOST, con las dos mitades ordenadas
// comparando ESOS bytes. En el ARCHIVO cada float va little-endian y las mitades se ordenan
// comparando los bytes little-endian. En un host little-endian las dos son la misma cadena;
// en uno big-endian NO alcanza con dar vuelta cada float: comparar los bytes del otro orden
// da otro orden de mitades (1.0 contra 2.0: 00 00 80 3f > 00 00 00 40, pero 3f 80 00 00 <
// 40 00 00 00). Por eso ninguna de las dos se copia de la otra: se arman desde los FLOATS.
// La del archivo (el escritor): cada mitad little-endian, la menor (memcmp) primero.
std::string ClaveMarcaArchivo(const float* a, const float* b) {
    std::string ka, kb;
    for (int k = 0; k < 3; k++) { PonF32(ka, a[k]); PonF32(kb, b[k]); }
    return (memcmp(kb.data(), ka.data(), 12) < 0) ? (kb + ka) : (ka + kb);
}
// La de memoria (el lector): los 6 floats del archivo -> la clave que arma ESTE host, sea
// cual sea el orden de las mitades en el archivo (ver arriba).
std::string ClaveMarcaMemoria(const unsigned char* p) {
    float a[3], b[3];
    for (int k = 0; k < 3; k++) {
        const unsigned ua = LeeU32(p + k * 4), ub = LeeU32(p + 12 + k * 4);
        memcpy(&a[k], &ua, 4); memcpy(&b[k], &ub, 4);
    }
    return Mesh::SharpEdgeKey(a, b);
}
unsigned LeeU16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

// ---- BLOQUES: abrir escribe la cabecera con bytes=0; cerrar parchea el largo y
//      rellena con ceros hasta multiplo de 4 (sin basura: bytes deterministas) ----
size_t AbrirBloque(std::string& s, const char* id, unsigned dominio, unsigned cantidad) {
    s.append(id, 4);
    PonU32(s, dominio);
    PonU32(s, cantidad);
    const size_t pos = s.size();
    PonU32(s, 0);
    return pos;
}
void CerrarBloque(std::string& s, size_t posBytes, unsigned& nBloques) {
    const unsigned bytes = (unsigned)(s.size() - (posBytes + 4));
    s[posBytes]     = (char)(bytes & 255u);
    s[posBytes + 1] = (char)((bytes >> 8) & 255u);
    s[posBytes + 2] = (char)((bytes >> 16) & 255u);
    s[posBytes + 3] = (char)((bytes >> 24) & 255u);
    while (s.size() % 4) s += '\0';
    nBloques++;
}

// ---- CURSOR de lectura: todo acceso controla el largo (un archivo corrupto
//      falla, jamas lee fuera del buffer ni reserva lo que no existe) ----
struct Cursor {
    const unsigned char* p;
    const unsigned char* fin;
    bool ok;
    Cursor(const unsigned char* a, size_t n) : p(a), fin(a + n), ok(true) {}
    size_t Quedan() const { return ok ? (size_t)(fin - p) : 0; }
    unsigned U32() {
        if (Quedan() < 4) { ok = false; return 0; }
        const unsigned v = LeeU32(p); p += 4; return v;
    }
    int I32() { return (int)U32(); }
    float F32() { const unsigned u = U32(); float f; memcpy(&f, &u, 4); return f; }
    bool Str(std::string& out) {
        const unsigned n = U32();
        if (!ok || Quedan() < n) { ok = false; return false; }
        out.assign((const char*)p, n); p += n; return true;
    }
    // vista de nElem elementos de tamElem bytes (con control de desborde de la multiplicacion)
    const unsigned char* Tomar(size_t nElem, size_t tamElem) {
        if (!ok) return 0;
        if (tamElem && nElem > Quedan() / tamElem) { ok = false; return 0; }
        const unsigned char* r = p;
        p += nElem * tamElem;
        return r;
    }
};

// ===========================================================================
//  LA TABLA DE BLOQUES
// ===========================================================================
struct Bloque {
    std::string id;       // fourcc
    unsigned dominio;     // bits 0..7 = dominio, 8..15 = banderas
    unsigned cantidad;
    unsigned bytes;
    const unsigned char* datos;
};

struct Tabla {
    unsigned version;
    std::vector<Bloque> bloques;
};

// false = no es un .w3db / version mas nueva / tabla rota. 'motivo' dice cual.
bool LeerTabla(const unsigned char* d, size_t n, Tabla& t, std::string& motivo, bool& masNuevo) {
    masNuevo = false;
    if (!W3dMallaBinEs(d, n) || n < 20) { motivo = "no es un .w3db (falta la firma W3DB)"; return false; }
    t.version = LeeU32(d + 4);
    if (t.version > W3DB_VERSION) {
        masNuevo = true;
        motivo = "este .w3db es de una version del formato mas nueva; no lo puedo leer";
        return false;
    }
    const unsigned nb = LeeU32(d + 12);
    if (nb > (n - 20) / 16) { motivo = "la cabecera declara mas bloques de los que entran en el archivo"; return false; }
    t.bloques.clear();
    t.bloques.reserve(nb);
    size_t off = 20;
    for (unsigned i = 0; i < nb; i++) {
        if (n - off < 16) { motivo = "archivo cortado en la cabecera de un bloque"; return false; }
        Bloque b;
        b.id.assign((const char*)d + off, 4);
        b.dominio  = LeeU32(d + off + 4);
        b.cantidad = LeeU32(d + off + 8);
        b.bytes    = LeeU32(d + off + 12);
        if (b.bytes > n - off - 16) { motivo = "el bloque " + b.id + " dice tener mas bytes de los que hay (archivo cortado)"; return false; }
        b.datos = d + off + 16;
        // un bloque CONOCIDO repetido es un archivo roto (no hay forma de saber cual vale)
        if (W3dMallaBinBloqueConocido(b.id))
            for (size_t k = 0; k < t.bloques.size(); k++)
                if (t.bloques[k].id == b.id) { motivo = "el bloque " + b.id + " esta repetido"; return false; }
        t.bloques.push_back(b);
        size_t prox = off + 16 + (size_t)b.bytes;
        prox = (prox + 3) & ~(size_t)3;
        off = (prox > n) ? n : prox;   // el relleno del ULTIMO bloque puede faltar sin romper nada
    }
    return true;
}

const Bloque* Buscar(const Tabla& t, const char* id) {
    for (size_t i = 0; i < t.bloques.size(); i++) if (t.bloques[i].id == id) return &t.bloques[i];
    return 0;
}

// ===========================================================================
//  INFO
// ===========================================================================
struct InfoBin {
    unsigned nRV, nIdx, nPartes, nCaras, nEsquinas;
    int nPuntos, nAristas;
    unsigned banderas;
    int primTipo; float primTam, primTam2, primAlto; int primVerts, primVerts2;
    float aabbMin[3], aabbMax[3], centro[3], radio;
    int vertsAgrupados;
    int uvActiva, colActiva, grupoActivo, uvGrupoActivo;
    std::string nombre;
    InfoBin() : nRV(0), nIdx(0), nPartes(0), nCaras(0), nEsquinas(0), nPuntos(-1), nAristas(-1), banderas(0),
                primTipo(-1), primTam(2.0f), primTam2(0.0f), primAlto(0.0f), primVerts(8), primVerts2(8),
                radio(0.0f), vertsAgrupados(0), uvActiva(-1), colActiva(-1), grupoActivo(-1), uvGrupoActivo(-1) {
        for (int k = 0; k < 3; k++) { aabbMin[k] = aabbMax[k] = centro[k] = 0.0f; }
    }
};

void EscribirInfo(std::string& s, const InfoBin& I) {
    PonU32(s, I.nRV); PonU32(s, I.nIdx); PonU32(s, I.nPartes); PonU32(s, I.nCaras); PonU32(s, I.nEsquinas);
    PonI32(s, I.nPuntos); PonI32(s, I.nAristas);
    PonU32(s, I.banderas);
    PonI32(s, I.primTipo); PonF32(s, I.primTam); PonF32(s, I.primTam2); PonF32(s, I.primAlto);
    PonI32(s, I.primVerts); PonI32(s, I.primVerts2);
    for (int k = 0; k < 3; k++) PonF32(s, I.aabbMin[k]);
    for (int k = 0; k < 3; k++) PonF32(s, I.aabbMax[k]);
    for (int k = 0; k < 3; k++) PonF32(s, I.centro[k]);
    PonF32(s, I.radio);
    PonI32(s, I.vertsAgrupados);
    PonI32(s, I.uvActiva); PonI32(s, I.colActiva); PonI32(s, I.grupoActivo); PonI32(s, I.uvGrupoActivo);
    PonStr(s, I.nombre);
    // CAMPOS NUEVOS: SOLO al final (un lector viejo lee lo que conoce e ignora el resto)
}

bool LeerInfo(const Bloque& b, InfoBin& I) {
    Cursor c(b.datos, b.bytes);
    I.nRV = c.U32(); I.nIdx = c.U32(); I.nPartes = c.U32(); I.nCaras = c.U32(); I.nEsquinas = c.U32();
    I.nPuntos = c.I32(); I.nAristas = c.I32();
    I.banderas = c.U32();
    I.primTipo = c.I32(); I.primTam = c.F32(); I.primTam2 = c.F32(); I.primAlto = c.F32();
    I.primVerts = c.I32(); I.primVerts2 = c.I32();
    for (int k = 0; k < 3; k++) I.aabbMin[k] = c.F32();
    for (int k = 0; k < 3; k++) I.aabbMax[k] = c.F32();
    for (int k = 0; k < 3; k++) I.centro[k] = c.F32();
    I.radio = c.F32();
    I.vertsAgrupados = c.I32();
    I.uvActiva = c.I32(); I.colActiva = c.I32(); I.grupoActivo = c.I32(); I.uvGrupoActivo = c.I32();
    c.Str(I.nombre);
    return c.ok;   // lo que sigue (campos de una version mas nueva) se ignora a proposito
}

// ===========================================================================
//  LA VISTA VALIDADA: punteros ADENTRO del buffer, ya chequeados. Primero se
//  valida TODO y recien despues se toca la malla: un archivo roto no deja la
//  malla a medio llenar.
// ===========================================================================
struct GrupoVista {           // grupo / forma: n indices + n (o 3n) floats
    std::string nombre;
    const unsigned char* idx;
    const unsigned char* val;
    unsigned n;
};
struct CapaVista {
    std::string nombre;
    unsigned banderas;
    const unsigned char* datos;
};
struct Vista {
    InfoBin I;
    std::vector<std::string> req;
    const unsigned char* pos; const unsigned char* nrm; const unsigned char* uv; const unsigned char* col;
    const unsigned char* idx; bool idx32;
    const unsigned char* ctrl;
    const unsigned char* uvRest;
    std::vector<const unsigned char*> uvx;   // RUVX: por capa UV, sus 8*nRV bytes o 0 (ausente = la activa)
    std::vector<W3dMallaBinParte> partes;
    std::vector<GrupoVista> grupos, uvGrupos, formas;
    // edicion
    bool conEdicion;
    const unsigned char* faceLados; const unsigned char* faceMat; const unsigned char* faceSmooth; const unsigned char* faceIdx;
    bool hayFace;
    std::vector<int> looseE, looseV;
    const unsigned char* cnrm;
    std::vector<CapaVista> uvly, coly;
    const unsigned char* shrp; unsigned nShrp;
    const unsigned char* seam; unsigned nSeam;
    const unsigned char* edge; unsigned nEdge; bool hayEdge;
    bool hayAjen;
    std::vector<std::string> ajenReq;
    std::vector<W3dBloqueAjeno> ajenBloques;
    // bloques binarios que esta version NO entiende (se preservan)
    std::vector<W3dBloqueAjeno> desconocidos;
    Vista() : pos(0), nrm(0), uv(0), col(0), idx(0), idx32(false), ctrl(0), uvRest(0), conEdicion(false),
              faceLados(0), faceMat(0), faceSmooth(0), faceIdx(0), hayFace(false), cnrm(0),
              shrp(0), nShrp(0), seam(0), nSeam(0), edge(0), nEdge(0), hayEdge(false), hayAjen(false) {}
};

// un indice de render-vert fuera de rango en un bloque = archivo roto
bool IndicesEnRango(const unsigned char* p, unsigned n, unsigned nRV) {
    for (unsigned i = 0; i < n; i++) {
        const int v = LeeI32(p + (size_t)i * 4);
        if (v < 0 || (unsigned)v >= nRV) return false;
    }
    return true;
}

// un bloque de GRUPOS (RVGR / RUVG: n indices + n pesos; RSHP: n indices + 3n floats)
bool LeerGrupos(const Bloque& b, unsigned nRV, unsigned floatsPorIdx, std::vector<GrupoVista>& out) {
    Cursor c(b.datos, b.bytes);
    out.clear();
    for (unsigned g = 0; g < b.cantidad; g++) {
        GrupoVista v;
        if (!c.Str(v.nombre)) return false;
        v.n = c.U32();
        v.idx = c.Tomar(v.n, 4);
        v.val = c.Tomar(v.n, 4 * floatsPorIdx);
        if (!c.ok) return false;
        if (!IndicesEnRango(v.idx, v.n, nRV)) return false;
        out.push_back(v);
    }
    return true;
}

// dominio sin las banderas
unsigned Dom(const Bloque& b) { return b.dominio & 255u; }

bool Validar(const unsigned char* d, size_t n, Vista& v, bool edicion, W3dMallaInfo* info, std::string& motivo) {
    Tabla t;
    bool masNuevo = false;
    if (!LeerTabla(d, n, t, motivo, masNuevo)) return false;
    const Bloque* bi = Buscar(t, "INFO");
    if (!bi || !LeerInfo(*bi, v.I)) { motivo = "falta el bloque INFO o esta cortado"; return false; }
    const InfoBin& I = v.I;
    const unsigned nRV = I.nRV;
    if (I.nIdx % 3 != 0) { motivo = "el index buffer no es de triangulos"; return false; }
    if (I.nPartes < 1) { motivo = "la malla no trae ningun mesh part"; return false; }
    // TOPES: ninguna cantidad de INFO puede hacer desbordar un calculo de bytes en 32 bits
    // (Symbian). Un archivo que declara mas que esto no es una malla: es un archivo roto.
    if (nRV > 0x0FFFFFFFu || I.nIdx > 0x3FFFFFFFu || I.nEsquinas > 0x0FFFFFFFu ||
        I.nCaras > 0x0FFFFFFFu || I.nPartes > 0x0FFFFFFFu) { motivo = "INFO declara cantidades imposibles"; return false; }

    // ---- REQ: lo que un lector DEBE entender ----
    if (const Bloque* b = Buscar(t, "REQ ")) {
        Cursor c(b->datos, b->bytes);
        const unsigned char* p = c.Tomar(b->cantidad, 4);
        if (!c.ok) { motivo = "bloque REQ cortado"; return false; }
        for (unsigned i = 0; i < b->cantidad; i++) {
            std::string f((const char*)p + (size_t)i * 4, 4);
            v.req.push_back(f);
            if (!W3dMallaBinBloqueConocido(f) && info) {
                info->soloLectura = true;
                info->avisos.push_back("el archivo REQUIERE el bloque '" + f +
                                       "', que esta version no entiende: lo abro en SOLO LECTURA");
            }
        }
    }

    // ---- RENDER ----
    if (nRV > 0) {
        const Bloque* b = Buscar(t, "RPOS");
        if (!b || b->cantidad != nRV || b->bytes != nRV * 12u) { motivo = "falta el bloque RPOS o no mide lo que dice INFO"; return false; }
        v.pos = b->datos;
    }
    if (const Bloque* b = Buscar(t, "RNRM")) {
        if (b->cantidad != nRV || b->bytes != nRV * 3u) { motivo = "RNRM no mide lo que dice INFO"; return false; }
        v.nrm = b->datos;
    }
    if (const Bloque* b = Buscar(t, "RUV ")) {
        if (b->cantidad != nRV || b->bytes != nRV * 8u) { motivo = "RUV no mide lo que dice INFO"; return false; }
        v.uv = b->datos;
    }
    if (const Bloque* b = Buscar(t, "RCOL")) {
        if (b->cantidad != nRV || b->bytes != nRV * 4u) { motivo = "RCOL no mide lo que dice INFO"; return false; }
        v.col = b->datos;
    }
    // RUVX: las capas UV extra por render-vert (cantidad = capas UV; cada una: u32 presente + f32 x2 x nRV)
    if (const Bloque* b = Buscar(t, "RUVX")) {
        Cursor c(b->datos, b->bytes);
        for (unsigned k = 0; k < b->cantidad && k < 64u; k++) {
            const unsigned presente = c.U32();
            const unsigned char* d = presente ? c.Tomar(nRV, 8) : 0;
            if (!c.ok) { motivo = "RUVX esta cortado"; return false; }
            v.uvx.push_back(d);
        }
    }
    if (I.nIdx > 0) {
        const Bloque* b = Buscar(t, "RIDX");
        if (!b) { motivo = "falta el bloque RIDX"; return false; }
        v.idx32 = (b->dominio & W3DB_IDX32) != 0;
        const unsigned tam = v.idx32 ? 4u : 2u;
        if (b->cantidad * 3u != I.nIdx || I.nIdx > 0x3FFFFFFFu || b->bytes != I.nIdx * tam) { motivo = "RIDX no mide lo que dice INFO"; return false; }
        if (v.idx32 && sizeof(MeshIndex) < 4) { motivo = "la malla tiene mas de 65535 vertices y esta plataforma usa indices de 16 bits"; return false; }
        v.idx = b->datos;
        for (unsigned i = 0; i < I.nIdx; i++) {
            const unsigned x = v.idx32 ? LeeU32(v.idx + (size_t)i * 4) : LeeU16(v.idx + (size_t)i * 2);
            if (x >= nRV) { motivo = "RIDX tiene un indice fuera de rango"; return false; }
        }
    }
    {
        const Bloque* b = Buscar(t, "PART");
        if (!b || b->cantidad != I.nPartes) { motivo = "falta el bloque PART o no mide lo que dice INFO"; return false; }
        Cursor c(b->datos, b->bytes);
        for (unsigned k = 0; k < b->cantidad; k++) {
            W3dMallaBinParte pa;
            c.Str(pa.nombre); c.Str(pa.material);
            pa.inicio = c.I32(); pa.cantidad = c.I32();
            if (!c.ok || pa.inicio < 0 || pa.cantidad < 0 || (unsigned)pa.inicio > I.nIdx ||
                (unsigned)pa.cantidad > I.nIdx - (unsigned)pa.inicio) { motivo = "PART esta cortado o sale del index buffer"; return false; }
            v.partes.push_back(pa);
        }
    }
    if (const Bloque* b = Buscar(t, "RCTL")) {
        if (b->cantidad != nRV || b->bytes != nRV * 4u || !IndicesEnRango(b->datos, nRV, nRV)) { motivo = "RCTL no mide lo que dice INFO o sale de rango"; return false; }
        v.ctrl = b->datos;
    }
    if (const Bloque* b = Buscar(t, "RVGR")) if (!LeerGrupos(*b, nRV, 1, v.grupos))   { motivo = "RVGR esta cortado o sale de rango"; return false; }
    if (const Bloque* b = Buscar(t, "RUVG")) if (!LeerGrupos(*b, nRV, 1, v.uvGrupos)) { motivo = "RUVG esta cortado o sale de rango"; return false; }
    if (const Bloque* b = Buscar(t, "RSHP")) if (!LeerGrupos(*b, nRV, 3, v.formas))   { motivo = "RSHP esta cortado o sale de rango"; return false; }
    if (const Bloque* b = Buscar(t, "RUVR")) {
        if (b->cantidad != nRV || b->bytes != nRV * 8u) { motivo = "RUVR no mide lo que dice INFO"; return false; }
        v.uvRest = b->datos;
    }

    // ---- EDICION (el juego los saltea sin mirarlos) ----
    v.conEdicion = edicion;
    if (edicion) {
        const unsigned nC = I.nEsquinas;
        if (const Bloque* b = Buscar(t, "FACE")) {
            Cursor c(b->datos, b->bytes);
            const unsigned nF = b->cantidad;
            const unsigned nEsq = c.U32();
            v.faceLados  = c.Tomar(nF, 4);
            v.faceMat    = c.Tomar(nF, 4);
            v.faceSmooth = c.Tomar(nF, 1);
            c.Tomar((4 - (nF % 4)) % 4, 1);   // relleno del smooth a 4
            v.faceIdx    = c.Tomar(nEsq, 4);
            if (!c.ok || nF != I.nCaras || nEsq != nC) { motivo = "FACE esta cortado o no mide lo que dice INFO"; return false; }
            unsigned suma = 0;
            for (unsigned f = 0; f < nF; f++) {
                const int lados = LeeI32(v.faceLados + (size_t)f * 4);
                const int mat = LeeI32(v.faceMat + (size_t)f * 4);
                const signed char sm = (signed char)v.faceSmooth[f];
                if (lados < 3 || (unsigned)lados > nEsq - suma || mat < 0 || (unsigned)mat >= I.nPartes || sm < -1 || sm > 1) {
                    motivo = "FACE tiene una cara invalida"; return false;
                }
                suma += (unsigned)lados;
            }
            if (suma != nEsq || !IndicesEnRango(v.faceIdx, nEsq, nRV)) { motivo = "FACE tiene indices fuera de rango"; return false; }
            v.hayFace = true;
        } else if (I.nCaras != 0) { motivo = "falta el bloque FACE"; return false; }
        if (const Bloque* b = Buscar(t, "LOOS")) {
            Cursor c(b->datos, b->bytes);
            const unsigned nE = c.U32();
            const unsigned char* pe = c.Tomar(nE, 4);
            const unsigned nVs = c.U32();
            const unsigned char* pv = c.Tomar(nVs, 4);
            if (!c.ok || (nE % 2) != 0 || !IndicesEnRango(pe, nE, nRV) || !IndicesEnRango(pv, nVs, nRV)) {
                motivo = "LOOS esta cortado o sale de rango"; return false;
            }
            v.looseE.resize(nE); v.looseV.resize(nVs);
            if (nE) CopiarI32(pe, &v.looseE[0], nE);
            if (nVs) CopiarI32(pv, &v.looseV[0], nVs);
        }
        if (const Bloque* b = Buscar(t, "CNRM")) {
            if (b->cantidad != nC || b->bytes != nC * 3u) { motivo = "CNRM no mide lo que dice INFO"; return false; }
            v.cnrm = b->datos;
        }
        if (const Bloque* b = Buscar(t, "UVLY")) {
            Cursor c(b->datos, b->bytes);
            for (unsigned k = 0; k < b->cantidad; k++) {
                CapaVista cv; cv.banderas = 0;
                c.Str(cv.nombre);
                cv.datos = c.Tomar(nC, 8);
                if (!c.ok) { motivo = "UVLY esta cortado"; return false; }
                v.uvly.push_back(cv);
            }
        }
        if (const Bloque* b = Buscar(t, "COLY")) {
            Cursor c(b->datos, b->bytes);
            for (unsigned k = 0; k < b->cantidad; k++) {
                CapaVista cv;
                c.Str(cv.nombre);
                cv.banderas = c.U32();
                cv.datos = c.Tomar(nC, 4);
                if (!c.ok) { motivo = "COLY esta cortado"; return false; }
                v.coly.push_back(cv);
            }
        }
        if (const Bloque* b = Buscar(t, "SHRP")) {
            if (b->bytes % 24u != 0 || b->cantidad != b->bytes / 24u) { motivo = "SHRP no mide lo que dice"; return false; }
            v.shrp = b->datos; v.nShrp = b->cantidad;
        }
        if (const Bloque* b = Buscar(t, "SEAM")) {
            if (b->bytes % 24u != 0 || b->cantidad != b->bytes / 24u) { motivo = "SEAM no mide lo que dice"; return false; }
            v.seam = b->datos; v.nSeam = b->cantidad;
        }
        if (const Bloque* b = Buscar(t, "EDGE")) {
            if (b->cantidad > 0x1FFFFFFFu || b->bytes != b->cantidad * 8u || !IndicesEnRango(b->datos, b->cantidad * 2u, nRV)) {
                motivo = "EDGE no mide lo que dice o sale de rango"; return false;
            }
            v.edge = b->datos; v.nEdge = b->cantidad; v.hayEdge = true;
        }
        if (const Bloque* b = Buscar(t, "AJEN")) {
            Cursor c(b->datos, b->bytes);
            const unsigned nReq = c.U32();
            for (unsigned i = 0; i < nReq && c.ok; i++) { std::string r; c.Str(r); v.ajenReq.push_back(r); }
            const unsigned nBl = c.U32();
            for (unsigned i = 0; i < nBl && c.ok; i++) {
                W3dBloqueAjeno aj;
                c.Str(aj.nombre);
                aj.dominio = c.I32();
                aj.cantidad = c.I32();
                c.Str(aj.texto);
                v.ajenBloques.push_back(aj);
            }
            if (!c.ok) { motivo = "AJEN esta cortado"; return false; }
            v.hayAjen = true;
        }
    }

    // ---- lo que esta version NO entiende: se preserva tal cual ----
    for (size_t i = 0; i < t.bloques.size(); i++) {
        const Bloque& b = t.bloques[i];
        if (W3dMallaBinBloqueConocido(b.id)) continue;
        W3dBloqueAjeno aj;
        aj.nombre = b.id;
        aj.dominio = (int)b.dominio;
        aj.cantidad = (int)b.cantidad;
        aj.texto.assign((const char*)b.datos, b.bytes);
        v.desconocidos.push_back(aj);
    }
    return true;
}

// ---- piezas de la carga (las usan Leer y MaterializarEdicion) ----

void LiberarCapasUVCol(Mesh* m) {
    for (size_t i = 0; i < m->uvMaps.size(); i++)      delete m->uvMaps[i];
    for (size_t i = 0; i < m->colorLayers.size(); i++) delete m->colorLayers[i];
    m->uvMaps.clear();      m->uvMapActivo = -1;
    m->colorLayers.clear(); m->colorActivo = -1;
}
void LiberarGrupos(Mesh* m) {
    for (size_t i = 0; i < m->vertexGroups.size(); i++) delete m->vertexGroups[i];
    for (size_t i = 0; i < m->uvGroups.size(); i++)     delete m->uvGroups[i];
    m->vertexGroups.clear(); m->grupoActivo = -1;
    m->uvGroups.clear();     m->uvGrupoActivo = -1;
}

// el activo de una lista de n elementos (misma regla que el lector de texto:
// -1 vale; fuera de rango cae al 0 si hay alguno)
int ActivoValido(int a, int n) {
    if (a >= -1 && a < n) return a;
    return n > 0 ? 0 : -1;
}

// ARISTAS UNICAS por REPRESENTANTE: exactamente lo que arma CalcularBordes con posRep
// (pares ordenados sin repetir, en el orden del std::set). Solo se usa cuando el
// archivo no las trae (el proyecto pide editable y la malla se guardo como escenario).
void AristasPorRep(const std::vector<MeshFace>& caras, const std::vector<int>& loose, const int* rep, int nV,
                   std::vector<int>& edges) {
    edges.clear();
    std::set<std::pair<int, int> > unicos;
    for (size_t f = 0; f < caras.size(); f++) {
        const std::vector<int>& idx = caras[f].idx;
        const int m = (int)idx.size();
        for (int k = 0; k < m; k++) {
            int a = idx[(size_t)k], b = idx[(size_t)((k + 1) % m)];
            if (a < 0 || a >= nV || b < 0 || b >= nV) continue;
            int ra = rep[a], rb = rep[b];
            if (ra == rb) continue;
            if (ra > rb) { int t2 = ra; ra = rb; rb = t2; }
            unicos.insert(std::make_pair(ra, rb));
        }
    }
    for (size_t e = 0; e + 1 < loose.size(); e += 2) {
        int a = loose[e], b = loose[e + 1];
        if (a < 0 || a >= nV || b < 0 || b >= nV) continue;
        int ra = rep[a], rb = rep[b];
        if (ra == rb) continue;
        if (ra > rb) { int t2 = ra; ra = rb; rb = t2; }
        unicos.insert(std::make_pair(ra, rb));
    }
    edges.reserve(unicos.size() * 2);
    for (std::set<std::pair<int, int> >::const_iterator it = unicos.begin(); it != unicos.end(); ++it) {
        edges.push_back(it->first);
        edges.push_back(it->second);
    }
}

// el buffer de lineas del contorno, LINEAL desde las aristas (lo que hacia CalcularBordes)
void ArmarBordesBuf(Mesh* m) {
    m->bordesBuf.clear();
    if (!m->vertex) return;
    m->bordesBuf.reserve(m->edges.size() * 3);
    for (size_t e = 0; e + 1 < m->edges.size(); e += 2) {
        const int a = m->edges[e], b = m->edges[e + 1];
        m->bordesBuf.push_back(m->vertex[a * 3]); m->bordesBuf.push_back(m->vertex[a * 3 + 1]); m->bordesBuf.push_back(m->vertex[a * 3 + 2]);
        m->bordesBuf.push_back(m->vertex[b * 3]); m->bordesBuf.push_back(m->vertex[b * 3 + 1]); m->bordesBuf.push_back(m->vertex[b * 3 + 2]);
    }
}

// los bloques de EDICION de la vista a la malla (caras, sueltos, capas, marcas, aristas,
// posRep y la preservacion). Requiere que los arrays de render ya esten en la malla.
void AplicarEdicion(const Vista& v, Mesh* m, bool noEditable) {
    const InfoBin& I = v.I;
    const unsigned nC = I.nEsquinas;
    const int nV = (int)I.nRV;
    // caras
    m->faces3d.clear();
    if (v.hayFace) {
        m->faces3d.resize(I.nCaras);
        size_t off = 0;
        for (unsigned f = 0; f < I.nCaras; f++) {
            MeshFace& F = m->faces3d[f];
            const int lados = LeeI32(v.faceLados + (size_t)f * 4);
            F.idx.resize((size_t)lados);
            CopiarI32(v.faceIdx + off * 4, &F.idx[0], (size_t)lados);
            off += (size_t)lados;
            F.mat = LeeI32(v.faceMat + (size_t)f * 4);
            F.smooth = (int)(signed char)v.faceSmooth[f];
        }
    }
    m->looseEdges = v.looseE;
    m->looseVerts = v.looseV;
    // normales por esquina
    m->cornerNormal.clear();
    if (v.cnrm && nC > 0) {
        m->cornerNormal.resize(nC * 3);
        memcpy(&m->cornerNormal[0], v.cnrm, nC * 3);
    }
    // capas
    LiberarCapasUVCol(m);
    for (size_t l = 0; l < v.uvly.size(); l++) {
        UVMap* u = new UVMap(v.uvly[l].nombre);
        u->uv.resize((size_t)nC * 2);
        if (nC) CopiarF32(v.uvly[l].datos, &u->uv[0], (size_t)nC * 2);
        m->uvMaps.push_back(u);
    }
    m->uvMapActivo = ActivoValido(I.uvActiva, (int)m->uvMaps.size());
    for (size_t l = 0; l < v.coly.size(); l++) {
        ColorLayer* c = new ColorLayer(v.coly[l].nombre);
        c->porVertice = (v.coly[l].banderas & COLY_POR_VERTICE) != 0;
        c->color.resize((size_t)nC * 4);
        if (nC) memcpy(&c->color[0], v.coly[l].datos, (size_t)nC * 4);
        m->colorLayers.push_back(c);
    }
    m->colorActivo = ActivoValido(I.colActiva, (int)m->colorLayers.size());
    // marcas: cada clave de 24 bytes se rearma como la arma ESTE host (en uno little-endian
    // salen los mismos bytes del archivo)
    m->sharpEdges.clear();
    m->seamEdges.clear();
    for (unsigned i = 0; i < v.nShrp; i++) m->sharpEdges.insert(ClaveMarcaMemoria(v.shrp + (size_t)i * 24));
    for (unsigned i = 0; i < v.nSeam; i++) m->seamEdges.insert(ClaveMarcaMemoria(v.seam + (size_t)i * 24));
    // identidad de puntos + aristas
    if (noEditable) {
        m->posRep.clear();
        m->edges.clear();
        m->bordesBuf.clear();
    } else {
        m->posRep = m->vertCtrlPoint;
        if (v.hayEdge) {
            m->edges.resize((size_t)v.nEdge * 2);
            if (v.nEdge) CopiarI32(v.edge, &m->edges[0], (size_t)v.nEdge * 2);
        } else if (nV > 0) {
            // se guardo como escenario cerrado y el proyecto la pide editable: se arman
            AristasPorRep(m->faces3d, m->looseEdges, &m->posRep[0], nV, m->edges);
        } else m->edges.clear();
        ArmarBordesBuf(m);
    }
    // preservacion: el "requiere" del texto (sin AJEN = el default del escritor)
    m->w3dmAjenos.bloques = v.ajenBloques;
    if (v.hayAjen) m->w3dmAjenos.requiere = v.ajenReq;
    else { m->w3dmAjenos.requiere.clear(); m->w3dmAjenos.requiere.push_back("V"); m->w3dmAjenos.requiere.push_back("F"); }
}

// sello + bloques binarios que no se entienden (van con la malla aunque sea el juego)
void AplicarSello(const Vista& v, Mesh* m) {
    m->w3dmAjenos.nPuntos     = v.I.nPuntos;
    m->w3dmAjenos.nEsquinas   = (int)v.I.nEsquinas;
    m->w3dmAjenos.nCaras      = (int)v.I.nCaras;
    m->w3dmAjenos.nAristas    = v.I.nAristas;
    m->w3dmAjenos.nRender     = (int)v.I.nRV;
    m->w3dmAjenos.nTriangulos = (int)(v.I.nIdx / 3);
    m->w3dmAjenos.bloquesBin  = v.desconocidos;
    m->w3dmAjenos.requiereBin = v.req;
    m->w3dmAjenos.truncado    = false;
    m->w3dmAjenos.noCargo     = false;
}

void AvisoMotivo(W3dMallaInfo* info, const std::string& motivo) {
    if (!info) return;
    info->avisos.push_back(motivo);
}

} // namespace

// ============================================================================
//  REAGRUPAR LA IDENTIDAD DE LOS PUNTOS (vertCtrlPoint) de una malla SKINNEADA cuyo .w3db
//  trae ctrl = IDENTIDAD (cada render-vert su propio control-point). Muchos assets vienen
//  como "sopa de triangulos" (tris*3 = render-verts, cero comparticion): el G-Man de la
//  intro HL2 tiene 10170 render-verts pero SOLO 1695 posiciones unicas (6x). Sin agrupar,
//  el skinning mezcla la matriz y la aplica 6 VECES por posicion -> en el N95 eran ~200 ms/
//  frame (4 fps). El CSR del skinning (SkeletalAnimation.cpp) YA esta hecho para trabajar
//  1 vez por control-point y escalear a los render-verts ("banana 6x"); solo le faltaba un
//  vertCtrlPoint agrupado. Esto lo deja igual que una malla cargada por el loader de TEXTO
//  (que ya agrupa con W3dMallaCalcularPuntos) -> LOSSLESS: la pose resultante es identica
//  (los render-verts de un punto comparten posicion Y pesos Y deltas de forma por firma, asi
//  que su skinning es el mismo). El render (triangulos/UV) no se toca: usa los arrays por
//  render-vert. Si algun merge diera distinto peso/delta (no deberia, la firma lo separa),
//  se ABORTA y la malla queda como estaba.
// ============================================================================
void W3dReagruparPuntosSkin(Mesh* m) {
    if (!m || m->vertexSize <= 0) return;
    if (m->vertexGroups.empty() && m->shapeKeys.empty()) return;   // sin skinning no vale la pena
    const int nV = m->vertexSize;
    if ((int)m->vertCtrlPoint.size() != nV) return;
    W3dMallaPuntos P;
    W3dMallaCalcularPuntos(m, P);
    if (P.nP <= 0 || (int)P.rvAPunto.size() != nV) return;
    const std::vector<int>& aPt = P.rvAPunto;   // render-vert -> punto nuevo
    // nCtrl VIEJO (max vertCtrlPoint+1) y mapeo CP-viejo -> punto-nuevo. Los grupos/formas indexan por
    // CP viejo (no por render-vert): como TODOS los render-verts de un CP comparten posicion+pesos, todos
    // caen en el MISMO punto nuevo -> basta tomar el de cualquiera. Asi funciona desde un ctrl identidad
    // O casi-identidad (el .w3db trae 10151 CP de 10170: ya agrupa unos pocos, pero no los 6x reales).
    int nCpViejo = 0;
    for (int ri = 0; ri < nV; ri++){ int c = m->vertCtrlPoint[(size_t)ri]; if (c + 1 > nCpViejo) nCpViejo = c + 1; }
    if (nCpViejo <= 0 || P.nP >= nCpViejo) return;   // ya tan agrupado como se puede: no hay nada que ganar
    std::vector<int> cpAPt((size_t)nCpViejo, -1);
    for (int ri = 0; ri < nV; ri++){ int c = m->vertCtrlPoint[(size_t)ri]; if (c >= 0 && c < nCpViejo && cpAPt[(size_t)c] < 0) cpAPt[(size_t)c] = aPt[(size_t)ri]; }
    // --- remapear grupos a temporales (dedup por punto nuevo); abortar si hay conflicto de peso ---
    std::vector<std::vector<int> >   gV(m->vertexGroups.size());
    std::vector<std::vector<float> > gW(m->vertexGroups.size());
    for (size_t g = 0; g < m->vertexGroups.size(); g++) {
        VertexGroup* vg = m->vertexGroups[g]; if (!vg) continue;
        std::map<int,float> acc;
        for (size_t k = 0; k < vg->verts.size() && k < vg->pesos.size(); k++) {
            int viejo = vg->verts[k]; if (viejo < 0 || viejo >= nCpViejo) continue;   // fuera de rango: entrada huerfana (el skinning ya la ignora)
            int np = cpAPt[(size_t)viejo]; if (np < 0 || np >= P.nP) continue;        // CP sin render-vert: peso huerfano -> saltear
            float w = vg->pesos[k];
            std::map<int,float>::iterator it = acc.find(np);
            if (it == acc.end()) acc[np] = w;
            else if (it->second != w) { float d = it->second - w; if (d < 0) d = -d; if (d > 1e-6f) return; }  // conflicto (no deberia: la firma separa) -> abortar sin tocar
        }
        gV[g].reserve(acc.size()); gW[g].reserve(acc.size());
        for (std::map<int,float>::iterator it = acc.begin(); it != acc.end(); ++it) { gV[g].push_back(it->first); gW[g].push_back(it->second); }
    }
    // --- remapear shape keys a temporales (dedup por punto nuevo); abortar si hay conflicto de delta ---
    std::vector<std::vector<int> >   kI(m->shapeKeys.size());
    std::vector<std::vector<float> > kD(m->shapeKeys.size());
    for (size_t s = 0; s < m->shapeKeys.size(); s++) {
        const W3dShapeKey& sk = m->shapeKeys[s];
        std::map<int, size_t> pos;   // punto nuevo -> indice en kI/kD (para comparar delta)
        for (size_t j = 0; j < sk.idx.size() && j*3+2 < sk.d.size(); j++) {
            int viejo = sk.idx[j]; if (viejo < 0 || viejo >= nCpViejo) continue;   // huerfana: saltear
            int np = cpAPt[(size_t)viejo]; if (np < 0 || np >= P.nP) continue;      // CP sin render-vert: saltear
            float dx = sk.d[j*3], dy = sk.d[j*3+1], dz = sk.d[j*3+2];
            std::map<int,size_t>::iterator it = pos.find(np);
            if (it == pos.end()) { pos[np] = kI[s].size(); kI[s].push_back(np); kD[s].push_back(dx); kD[s].push_back(dy); kD[s].push_back(dz); }
            else { size_t o = it->second; float a0=kD[s][o*3]-dx, a1=kD[s][o*3+1]-dy, a2=kD[s][o*3+2]-dz;
                   if (a0<0)a0=-a0; if(a1<0)a1=-a1; if(a2<0)a2=-a2; if (a0>1e-6f||a1>1e-6f||a2>1e-6f) return; }  // conflicto (no deberia) -> abortar sin tocar
        }
    }
    // --- todo validado: COMMIT (nadie mas uso estos datos todavia; recien se cargo la malla) ---
    for (size_t i = 0; i < aPt.size(); i++) m->vertCtrlPoint[i] = aPt[i];
    m->posRep = m->vertCtrlPoint;
    for (size_t g = 0; g < m->vertexGroups.size(); g++) { VertexGroup* vg = m->vertexGroups[g]; if (!vg) continue; vg->verts.swap(gV[g]); vg->pesos.swap(gW[g]); }
    for (size_t s = 0; s < m->shapeKeys.size(); s++) { m->shapeKeys[s].idx.swap(kI[s]); m->shapeKeys[s].d.swap(kD[s]); }
}

// ============================================================================
//                               L E C T U R A
// ============================================================================

bool W3dMallaBinLeer(const unsigned char* d, size_t n, Mesh* m, W3dMallaInfo* info, const W3dMallaBinOpciones& op) {
    if (!m || !d) return false;
    Vista v;
    std::string motivo;
    if (!Validar(d, n, v, op.edicion, info, motivo)) {
        AvisoMotivo(info, motivo + ": NO toco la malla que ya estaba");
        if (info) info->truncado = true;
        return false;
    }
    const InfoBin& I = v.I;
    const unsigned nRV = I.nRV;

    // ---- ARRAYS DE RENDER: memcpy desde el archivo ----
    // lo compartido (MallaDatos) se suelta, no se borra: no es de esta malla
    m->SoltarDatosCompartidos();
    delete[] m->vertex;      m->vertex = 0;
    delete[] m->normals;     m->normals = 0;
    delete[] m->uv;          m->uv = 0;
    delete[] m->vertexColor; m->vertexColor = 0;
    m->vertexSize = (int)nRV;
    // ESTADO DE POSE: el .w3db, igual que el .w3dm, guarda la malla EN REPOSO
    m->posadaPorAnim = false;
    if (nRV > 0) {
        m->vertex      = new GLfloat[(size_t)nRV * 3];
        m->normals     = new GLbyte[(size_t)nRV * 3];
        m->uv          = new GLfloat[(size_t)nRV * 2];
        m->vertexColor = new GLubyte[(size_t)nRV * 4];
        CopiarF32(v.pos, m->vertex, (size_t)nRV * 3);
        if (v.nrm) memcpy(m->normals, v.nrm, (size_t)nRV * 3);
        else for (unsigned i = 0; i < nRV; i++) { m->normals[i*3] = 0; m->normals[i*3+1] = 127; m->normals[i*3+2] = 0; }
        if (v.uv) CopiarF32(v.uv, m->uv, (size_t)nRV * 2);
        else memset(m->uv, 0, (size_t)nRV * 2 * sizeof(GLfloat));
        if (v.col) memcpy(m->vertexColor, v.col, (size_t)nRV * 4);
        else memset(m->vertexColor, 255, (size_t)nRV * 4);
    }
    // ---- RUVX: capas UV extra por render-vert (las capas de textura con UV propio) ----
    m->uvExtra.clear();
    m->uvExtra.resize(v.uvx.size());
    for (size_t k = 0; k < v.uvx.size(); k++)
        if (v.uvx[k] && nRV > 0) { m->uvExtra[k].resize((size_t)nRV * 2); CopiarF32(v.uvx[k], &m->uvExtra[k][0], (size_t)nRV * 2); }
    // ---- INDEX BUFFER (ya agrupado por mesh part y optimizado) ----
    delete[] m->faces;
    m->facesSize = (int)I.nIdx;
    m->faces = new MeshIndex[I.nIdx > 0 ? I.nIdx : 1];
    if (I.nIdx > 0) {
        if (v.idx32 && sizeof(MeshIndex) == 4 && HostLE()) memcpy(m->faces, v.idx, (size_t)I.nIdx * 4);
        else if (!v.idx32 && sizeof(MeshIndex) == 2 && HostLE()) memcpy(m->faces, v.idx, (size_t)I.nIdx * 2);
        else if (v.idx32) for (unsigned i = 0; i < I.nIdx; i++) m->faces[i] = (MeshIndex)LeeU32(v.idx + (size_t)i * 4);
        else for (unsigned i = 0; i < I.nIdx; i++) m->faces[i] = (MeshIndex)LeeU16(v.idx + (size_t)i * 2);
    }
    // ---- cabecera ----
    if (!I.nombre.empty()) m->name = I.nombre;
    m->meshSmooth = (I.banderas & INFO_SUAVE) != 0;
    if (I.banderas & INFO_PRIM) {
        m->meshTipo = I.primTipo; m->meshSize = I.primTam; m->meshSize2 = I.primTam2;
        m->meshDepth = I.primAlto; m->meshVerts = I.primVerts; m->meshVerts2 = I.primVerts2;
    } else m->meshTipo = -1;
    // ---- mesh parts (el MATERIAL lo resuelve el que llama, por nombre) ----
    m->materialsGroup.clear();
    if (info) info->materiales.clear();
    for (size_t k = 0; k < v.partes.size(); k++) {
        MaterialGroup mg;
        mg.name = v.partes[k].nombre;
        mg.startDrawn = v.partes[k].inicio;
        mg.indicesDrawnCount = v.partes[k].cantidad;
        m->materialsGroup.push_back(mg);
        if (info) info->materiales.push_back(v.partes[k].material);
    }
    // ---- IDENTIDAD DE LOS PUNTOS (vertCtrlPoint; posRep sale de aca) ----
    m->vertCtrlPoint.resize(nRV);
    if (nRV > 0) {
        if (v.ctrl) CopiarI32(v.ctrl, &m->vertCtrlPoint[0], nRV);
        else for (unsigned i = 0; i < nRV; i++) m->vertCtrlPoint[i] = (int)i;
    }
    // ---- grupos, formas, UV groups, reposo 2D ----
    LiberarGrupos(m);
    for (size_t g = 0; g < v.grupos.size(); g++) {
        VertexGroup* vg = new VertexGroup(v.grupos[g].nombre);
        vg->verts.resize(v.grupos[g].n); vg->pesos.resize(v.grupos[g].n);
        if (v.grupos[g].n) { CopiarI32(v.grupos[g].idx, &vg->verts[0], v.grupos[g].n); CopiarF32(v.grupos[g].val, &vg->pesos[0], v.grupos[g].n); }
        m->vertexGroups.push_back(vg);
    }
    m->grupoActivo = ActivoValido(I.grupoActivo, (int)m->vertexGroups.size());
    m->shapeKeys.clear();
    for (size_t k = 0; k < v.formas.size(); k++) {
        W3dShapeKey sk;
        sk.nombre = v.formas[k].nombre;
        sk.idx.resize(v.formas[k].n); sk.d.resize((size_t)v.formas[k].n * 3);
        if (v.formas[k].n) { CopiarI32(v.formas[k].idx, &sk.idx[0], v.formas[k].n); CopiarF32(v.formas[k].val, &sk.d[0], (size_t)v.formas[k].n * 3); }
        m->shapeKeys.push_back(sk);
    }
    m->shapePesos.assign(m->shapeKeys.size(), 0.0f);
    for (size_t g = 0; g < v.uvGrupos.size(); g++) {
        UVGroup* ug = new UVGroup(v.uvGrupos[g].nombre);
        ug->verts.resize(v.uvGrupos[g].n); ug->pesos.resize(v.uvGrupos[g].n);
        if (v.uvGrupos[g].n) { CopiarI32(v.uvGrupos[g].idx, &ug->verts[0], v.uvGrupos[g].n); CopiarF32(v.uvGrupos[g].val, &ug->pesos[0], v.uvGrupos[g].n); }
        m->uvGroups.push_back(ug);
    }
    m->uvGrupoActivo = ActivoValido(I.uvGrupoActivo, (int)m->uvGroups.size());
    m->uv2dRest.clear();
    if (v.uvRest && nRV > 0) { m->uv2dRest.resize((size_t)nRV * 2); CopiarF32(v.uvRest, &m->uv2dRest[0], (size_t)nRV * 2); }

    // (El agrupamiento de los puntos de skinning NO se hace aca: viene horneado en el .w3db de una malla
    //  guardada como no-editable. Ver W3dMallaBinEscribir / W3dReagruparPuntosSkin.)

    // ---- AABB / centro / radio / posiciones unicas: HORNEADOS. Si el proyecto pide
    //      el OTRO modo (se guardo editable y ahora es escenario, o al reves) se
    //      recalculan con el MISMO codigo que CalcularBordes / CalcularAABBSolo ----
    const bool statsNoEdit = (I.banderas & INFO_STATS_NOEDIT) != 0;
    if (statsNoEdit == op.noEditable) {
        m->aabbOk = (I.banderas & INFO_AABBOK) != 0;
        m->aabbMin = Vector3(I.aabbMin[0], I.aabbMin[1], I.aabbMin[2]);
        m->aabbMax = Vector3(I.aabbMax[0], I.aabbMax[1], I.aabbMax[2]);
        m->centroGeom = Vector3(I.centro[0], I.centro[1], I.centro[2]);
        m->radioGeom = I.radio;
        m->vertsAgrupados = op.noEditable ? 0 : I.vertsAgrupados;
    } else {
        W3dGeomStats st;
        W3dCalcularGeomStats(m->vertex, (int)nRV, (op.noEditable || nRV == 0) ? NULL : &m->vertCtrlPoint[0], st);
        m->aabbOk = st.ok;
        if (st.ok) {
            m->aabbMin = st.aabbMin; m->aabbMax = st.aabbMax;
            m->centroGeom = st.centro; m->radioGeom = st.radio;
        }
        m->vertsAgrupados = op.noEditable ? 0 : st.agrupados;
    }

    // ---- EDICION ----
    if (op.edicion) AplicarEdicion(v, m, op.noEditable);
    else {
        // el juego compilado: nada de edicion en memoria (ConstruirPolyMesh lo pide si hace falta)
        m->faces3d.clear(); m->looseEdges.clear(); m->looseVerts.clear(); m->cornerNormal.clear();
        LiberarCapasUVCol(m);
        m->sharpEdges.clear(); m->seamEdges.clear();
        m->posRep.clear(); m->edges.clear(); m->bordesBuf.clear();
        m->w3dmAjenos.bloques.clear(); m->w3dmAjenos.requiere.clear();
    }
    AplicarSello(v, m);
    m->uvSelVert.clear();

    // ---- las versiones que dejaba la carga clasica (Reagrupar sube una, CalcularBordes
    //      otra y fija bordesGeomVersion; AplicarCapasAlRender invalida el VBO) ----
    m->skinGeomVersion += op.noEditable ? 1 : 2;
    if (!op.noEditable && op.edicion) m->bordesGeomVersion = m->skinGeomVersion;
    m->overlayLcache = -1.0f;
    m->vboGeomVer = 0xFFFFFFFFu;
    if (info) { info->rverts = (int)nRV; info->rvertsReales = (int)nRV; info->truncado = false; }
    return true;
}

bool W3dMallaBinMaterializarEdicion(Mesh* m) {
    if (!m || m->edicionPendiente.empty()) return false;
    // LA MALLA USA UN RECURSO: la edicion sale de SUS bytes (en memoria si se edito en la
    // sesion, o su entrada del contenedor), no de una entrada propia de la malla
    if (m->malla) {
        std::string b;
        if (!W3dMallaRecursoLeerBytes(m->malla, b) || b.empty()) {
            m->edicionPendiente.clear();   // una sola vez: si falla, no se reintenta en cada frame
            return false;
        }
        return W3dMallaBinMaterializarEdicion(m, (const unsigned char*)b.data(), b.size());
    }
    const std::string entrada = m->edicionPendiente;
    std::vector<unsigned char> datos;
    if (!w3dFileSystem::ReadFileBytes(entrada, datos) || datos.empty()) {
        m->edicionPendiente.clear();   // una sola vez: si falla, no se reintenta en cada frame
        return false;
    }
    return W3dMallaBinMaterializarEdicion(m, &datos[0], datos.size());
}

bool W3dMallaBinMaterializarEdicion(Mesh* m, const unsigned char* datos, size_t n) {
    if (!m || m->edicionPendiente.empty() || !datos) return false;
    m->edicionPendiente.clear();   // una sola vez: si falla, no se reintenta en cada frame
    Vista v;
    std::string motivo;
    if (!Validar(datos, n, v, true, 0, motivo)) return false;
    // la entrada tiene que ser ESTA malla (mismos render-verts); si no, no se toca nada
    if ((int)v.I.nRV != m->vertexSize) return false;
    // la IDENTIDAD de los puntos (posRep sale de aca): una malla de un recurso sin grupos ni
    // formas no la carga hasta que hace falta (MallaRecurso.cpp)
    if ((int)m->vertCtrlPoint.size() != m->vertexSize) {
        m->vertCtrlPoint.resize((size_t)(m->vertexSize > 0 ? m->vertexSize : 0));
        if (m->vertexSize > 0) {
            if (v.ctrl) CopiarI32(v.ctrl, &m->vertCtrlPoint[0], (size_t)m->vertexSize);
            else for (int i = 0; i < m->vertexSize; i++) m->vertCtrlPoint[(size_t)i] = i;
        }
    }
    AplicarEdicion(v, m, m->noEditable);
    return true;
}

// LOS EXTRAS de la malla de render (identidad de los puntos, grupos, formas, reposo 2D y los
// activos): los comparten LeerRender y LeerExtras
static void LlenarExtras(const Vista& v, W3dMallaRender& out) {
    const InfoBin& I = v.I;
    const unsigned nRV = I.nRV;
    out.ctrl.resize(nRV);
    if (nRV > 0) {
        if (v.ctrl) CopiarI32(v.ctrl, &out.ctrl[0], nRV);
        else for (unsigned i = 0; i < nRV; i++) out.ctrl[i] = (int)i;
    }
    for (int pasada = 0; pasada < 2; pasada++) {
        const std::vector<GrupoVista>& src = pasada ? v.uvGrupos : v.grupos;
        std::vector<W3dMallaBinGrupo>& dst = pasada ? out.uvGrupos : out.grupos;
        for (size_t g = 0; g < src.size(); g++) {
            W3dMallaBinGrupo gr;
            gr.nombre = src[g].nombre;
            gr.idx.resize(src[g].n); gr.pesos.resize(src[g].n);
            if (src[g].n) { CopiarI32(src[g].idx, &gr.idx[0], src[g].n); CopiarF32(src[g].val, &gr.pesos[0], src[g].n); }
            dst.push_back(gr);
        }
    }
    for (size_t k = 0; k < v.formas.size(); k++) {
        W3dMallaBinForma fo;
        fo.nombre = v.formas[k].nombre;
        fo.idx.resize(v.formas[k].n); fo.d.resize((size_t)v.formas[k].n * 3);
        if (v.formas[k].n) { CopiarI32(v.formas[k].idx, &fo.idx[0], v.formas[k].n); CopiarF32(v.formas[k].val, &fo.d[0], (size_t)v.formas[k].n * 3); }
        out.formas.push_back(fo);
    }
    if (v.uvRest && nRV > 0) { out.uvRest.resize((size_t)nRV * 2); CopiarF32(v.uvRest, &out.uvRest[0], (size_t)nRV * 2); }
    out.grupoActivo = ActivoValido(I.grupoActivo, (int)out.grupos.size());
    out.uvGrupoActivo = ActivoValido(I.uvGrupoActivo, (int)out.uvGrupos.size());
}

bool W3dMallaBinLeerExtras(const unsigned char* d, size_t n, W3dMallaRender& out) {
    Vista v;
    std::string motivo;
    if (!Validar(d, n, v, false, 0, motivo)) return false;
    out = W3dMallaRender();
    out.nombre = v.I.nombre;
    out.nRV = (int)v.I.nRV;
    LlenarExtras(v, out);
    return true;
}

bool W3dMallaBinLeerTopologia(const unsigned char* d, size_t n, std::vector<int>& aristas,
                              std::vector<MeshFace>* caras) {
    aristas.clear();
    if (caras) caras->clear();
    Vista v;
    std::string motivo;
    if (!Validar(d, n, v, true, 0, motivo)) return false;
    const InfoBin& I = v.I;
    const int nV = (int)I.nRV;
    // las caras (lo mismo que AplicarEdicion deja en faces3d)
    std::vector<MeshFace> propias;
    std::vector<MeshFace>& F = caras ? *caras : propias;
    const bool hacenFalta = caras || !v.hayEdge;   // sin EDGE, las aristas salen de las caras
    if (v.hayFace && hacenFalta) {
        F.resize(I.nCaras);
        size_t off = 0;
        for (unsigned f = 0; f < I.nCaras; f++) {
            MeshFace& C = F[f];
            const int lados = LeeI32(v.faceLados + (size_t)f * 4);
            C.idx.resize((size_t)lados);
            CopiarI32(v.faceIdx + off * 4, &C.idx[0], (size_t)lados);
            off += (size_t)lados;
            C.mat = LeeI32(v.faceMat + (size_t)f * 4);
            C.smooth = (int)(signed char)v.faceSmooth[f];
        }
    }
    // las aristas: las del archivo (EDGE) o, si no las trae, por representante como el lector
    if (v.hayEdge) {
        aristas.resize((size_t)v.nEdge * 2);
        if (v.nEdge) CopiarI32(v.edge, &aristas[0], (size_t)v.nEdge * 2);
    } else if (nV > 0) {
        std::vector<int> rep((size_t)nV);
        if (v.ctrl) CopiarI32(v.ctrl, &rep[0], (size_t)nV);
        else for (int i = 0; i < nV; i++) rep[(size_t)i] = i;
        AristasPorRep(F, v.looseE, &rep[0], nV, aristas);
    }
    return true;
}

bool W3dMallaBinLeerRender(const unsigned char* d, size_t n, W3dMallaRender& out, W3dMallaInfo* info,
                           W3dAjenos* sello) {
    Vista v;
    std::string motivo;
    if (!Validar(d, n, v, false, info, motivo)) { AvisoMotivo(info, motivo); if (info) info->truncado = true; return false; }
    const InfoBin& I = v.I;
    const unsigned nRV = I.nRV;
    out = W3dMallaRender();
    out.nombre = I.nombre;
    out.suave = (I.banderas & INFO_SUAVE) != 0;
    if (I.banderas & INFO_PRIM) {
        out.primTipo = I.primTipo; out.primTam = I.primTam; out.primTam2 = I.primTam2;
        out.primAlto = I.primAlto; out.primVerts = I.primVerts; out.primVerts2 = I.primVerts2;
    }
    out.nRV = (int)nRV;
    out.pos.resize((size_t)nRV * 3); out.nrm.resize((size_t)nRV * 3);
    out.uv.resize((size_t)nRV * 2);  out.col.resize((size_t)nRV * 4);
    if (nRV > 0) {
        CopiarF32(v.pos, &out.pos[0], (size_t)nRV * 3);
        if (v.nrm) memcpy(&out.nrm[0], v.nrm, (size_t)nRV * 3);
        else for (unsigned i = 0; i < nRV; i++) { out.nrm[i*3] = 0; out.nrm[i*3+1] = 127; out.nrm[i*3+2] = 0; }
        if (v.uv) CopiarF32(v.uv, &out.uv[0], (size_t)nRV * 2);
        if (v.col) memcpy(&out.col[0], v.col, (size_t)nRV * 4);
        else std::fill(out.col.begin(), out.col.end(), (unsigned char)255);
    }
    out.uvExtra.clear();
    out.uvExtra.resize(v.uvx.size());
    for (size_t k = 0; k < v.uvx.size(); k++)
        if (v.uvx[k] && nRV > 0) { out.uvExtra[k].resize((size_t)nRV * 2); CopiarF32(v.uvx[k], &out.uvExtra[k][0], (size_t)nRV * 2); }
    out.idx.resize(I.nIdx);
    if (I.nIdx > 0) {
        // el index buffer ya viene en el ancho de ESTA plataforma: una copia (como W3dMallaBinLeer)
        if (v.idx32 && sizeof(MeshIndex) == 4 && HostLE()) memcpy(&out.idx[0], v.idx, (size_t)I.nIdx * 4);
        else if (!v.idx32 && sizeof(MeshIndex) == 2 && HostLE()) memcpy(&out.idx[0], v.idx, (size_t)I.nIdx * 2);
        else for (unsigned i = 0; i < I.nIdx; i++)
            out.idx[i] = (MeshIndex)(v.idx32 ? LeeU32(v.idx + (size_t)i * 4) : LeeU16(v.idx + (size_t)i * 2));
    }
    out.partes = v.partes;
    LlenarExtras(v, out);
    for (int k = 0; k < 3; k++) { out.aabbMin[k] = I.aabbMin[k]; out.aabbMax[k] = I.aabbMax[k]; out.centro[k] = I.centro[k]; }
    out.radio = I.radio;
    out.aabbOk = (I.banderas & INFO_AABBOK) != 0;
    out.vertsAgrupados = I.vertsAgrupados;
    out.statsNoEditable = (I.banderas & INFO_STATS_NOEDIT) != 0;
    if (info) { info->materiales.clear(); for (size_t k = 0; k < out.partes.size(); k++) info->materiales.push_back(out.partes[k].material); }
    if (sello) {
        // lo mismo que AplicarSello deja en la malla (sin la edicion: AJEN es un bloque de edicion)
        sello->nPuntos     = I.nPuntos;
        sello->nEsquinas   = (int)I.nEsquinas;
        sello->nCaras      = (int)I.nCaras;
        sello->nAristas    = I.nAristas;
        sello->nRender     = (int)I.nRV;
        sello->nTriangulos = (int)(I.nIdx / 3);
        sello->bloquesBin  = v.desconocidos;
        sello->requiereBin = v.req;
        sello->truncado    = false;
        sello->noCargo     = false;
    }
    return true;
}

W3dMallaRender::W3dMallaRender()
    : suave(false), primTipo(-1), primTam(2.0f), primTam2(0.0f), primAlto(0.0f), primVerts(8), primVerts2(8),
      nRV(0), grupoActivo(-1), uvGrupoActivo(-1), radio(0.0f), aabbOk(false), vertsAgrupados(0),
      statsNoEditable(false) {
    for (int k = 0; k < 3; k++) { aabbMin[k] = aabbMax[k] = centro[k] = 0.0f; }
}

long W3dMallaRender::Bytes() const {
    long b = (long)(pos.size() * sizeof(float) + nrm.size() + uv.size() * sizeof(float) + col.size() +
                    idx.size() * sizeof(MeshIndex) + ctrl.size() * sizeof(int) + uvRest.size() * sizeof(float));
    for (size_t g = 0; g < grupos.size(); g++)   b += (long)(grupos[g].idx.size() * sizeof(int) + grupos[g].pesos.size() * sizeof(float));
    for (size_t g = 0; g < uvGrupos.size(); g++) b += (long)(uvGrupos[g].idx.size() * sizeof(int) + uvGrupos[g].pesos.size() * sizeof(float));
    for (size_t k = 0; k < formas.size(); k++)   b += (long)(formas[k].idx.size() * sizeof(int) + formas[k].d.size() * sizeof(float));
    for (size_t k = 0; k < uvExtra.size(); k++)  b += (long)(uvExtra[k].size() * sizeof(float));
    return b;
}

// ============================================================================
//  RETOQUES: el mismo archivo con UN bloque reescrito (nombre en INFO, materiales en PART)
// ============================================================================
namespace {
// donde empieza el nombre en el payload de INFO (lo que LeerInfo lee antes del string)
bool OffsetNombreInfo(const Bloque& b, size_t& off, size_t& finNombre) {
    InfoBin I;
    Cursor c(b.datos, b.bytes);
    I.nRV = c.U32(); I.nIdx = c.U32(); I.nPartes = c.U32(); I.nCaras = c.U32(); I.nEsquinas = c.U32();
    I.nPuntos = c.I32(); I.nAristas = c.I32();
    I.banderas = c.U32();
    I.primTipo = c.I32(); I.primTam = c.F32(); I.primTam2 = c.F32(); I.primAlto = c.F32();
    I.primVerts = c.I32(); I.primVerts2 = c.I32();
    for (int k = 0; k < 9; k++) c.F32();   // aabbMin, aabbMax, centro
    c.F32();                               // radio
    c.I32();                               // vertsAgrupados
    for (int k = 0; k < 4; k++) c.I32();   // activos
    if (!c.ok) return false;
    off = (size_t)(c.p - b.datos);
    std::string nom;
    if (!c.Str(nom)) return false;
    finNombre = (size_t)(c.p - b.datos);
    return true;
}
// rearma el archivo con los MISMOS bloques, en el mismo orden, salvo 'id' con 'payload' nuevo
// (misma cabecera de bloque: dominio y cantidad; bytes y relleno recalculados)
bool ReemplazarBloque(const std::string& in, const Tabla& t, const char* id, const std::string& payload,
                      std::string& out) {
    out.clear();
    out.reserve(in.size() + payload.size());
    out.append(in, 0, 20);   // cabecera tal cual (firma, version, banderas, nBloques, reservado)
    bool hecho = false;
    for (size_t i = 0; i < t.bloques.size(); i++) {
        const Bloque& b = t.bloques[i];
        out.append(b.id.data(), 4);
        PonU32(out, b.dominio);
        PonU32(out, b.cantidad);
        if (b.id == id && !hecho) {
            PonU32(out, (unsigned)payload.size());
            out += payload;
            hecho = true;
        } else {
            PonU32(out, b.bytes);
            if (b.bytes) out.append((const char*)b.datos, b.bytes);
        }
        while (out.size() % 4) out += '\0';
    }
    if (!hecho) { out.clear(); return false; }
    return true;
}
} // namespace

bool W3dMallaBinRenombrar(const std::string& in, const std::string& nombre, std::string& out) {
    out.clear();
    Tabla t; std::string motivo; bool masNuevo = false;
    const unsigned char* d = (const unsigned char*)in.data();
    if (in.empty() || !LeerTabla(d, in.size(), t, motivo, masNuevo)) return false;
    const Bloque* bi = Buscar(t, "INFO");
    size_t off = 0, fin = 0;
    if (!bi || !OffsetNombreInfo(*bi, off, fin)) return false;
    std::string pay((const char*)bi->datos, off);
    PonStr(pay, nombre);
    pay.append((const char*)bi->datos + fin, bi->bytes - fin);   // campos de una version mas nueva: tal cual
    return ReemplazarBloque(in, t, "INFO", pay, out);
}

// las partes de PART tal cual vienen (nombre, material, inicio, cantidad) + lo que sigue
static bool LeerPartes(const Bloque& b, std::vector<W3dMallaBinParte>& ps, size_t& finPartes) {
    Cursor c(b.datos, b.bytes);
    ps.clear();
    for (unsigned k = 0; k < b.cantidad; k++) {
        W3dMallaBinParte pa;
        c.Str(pa.nombre); c.Str(pa.material);
        pa.inicio = c.I32(); pa.cantidad = c.I32();
        if (!c.ok) return false;
        ps.push_back(pa);
    }
    finPartes = (size_t)(c.p - b.datos);
    return true;
}

bool W3dMallaBinMateriales(const std::string& in, std::vector<std::string>& mats) {
    mats.clear();
    Tabla t; std::string motivo; bool masNuevo = false;
    if (in.empty() || !LeerTabla((const unsigned char*)in.data(), in.size(), t, motivo, masNuevo)) return false;
    const Bloque* bp = Buscar(t, "PART");
    std::vector<W3dMallaBinParte> ps; size_t fin = 0;
    if (!bp || !LeerPartes(*bp, ps, fin)) return false;
    for (size_t k = 0; k < ps.size(); k++) mats.push_back(ps[k].material);
    return true;
}

bool W3dMallaBinCambiarMateriales(const std::string& in, const std::vector<std::string>& mats, std::string& out) {
    out.clear();
    Tabla t; std::string motivo; bool masNuevo = false;
    if (in.empty() || !LeerTabla((const unsigned char*)in.data(), in.size(), t, motivo, masNuevo)) return false;
    const Bloque* bp = Buscar(t, "PART");
    std::vector<W3dMallaBinParte> ps; size_t fin = 0;
    if (!bp || !LeerPartes(*bp, ps, fin) || ps.size() != mats.size()) return false;
    std::string pay;
    for (size_t k = 0; k < ps.size(); k++) {
        PonStr(pay, ps[k].nombre); PonStr(pay, mats[k]);
        PonI32(pay, ps[k].inicio); PonI32(pay, ps[k].cantidad);
    }
    pay.append((const char*)bp->datos + fin, bp->bytes - fin);
    return ReemplazarBloque(in, t, "PART", pay, out);
}

bool W3dMallaBinListar(const unsigned char* d, size_t n, std::vector<std::string>& lineas) {
    lineas.clear();
    Tabla t;
    std::string motivo;
    bool masNuevo = false;
    if (!LeerTabla(d, n, t, motivo, masNuevo)) { lineas.push_back(motivo); return false; }
    static const char* kDom[] = { "punto", "esquina", "cara", "arista", "global", "render", "triangulo" };
    for (size_t i = 0; i < t.bloques.size(); i++) {
        const Bloque& b = t.bloques[i];
        std::string s = b.id + " ";
        const unsigned dom = Dom(b);
        s += (dom < 7) ? kDom[dom] : "?";
        if (b.dominio & W3DB_IDX32) s += "+idx32";
        s += " "; W3dEscribirInt(s, (int)b.cantidad);
        s += " "; W3dEscribirInt(s, (int)b.bytes); s += " B";
        lineas.push_back(s);
    }
    return true;
}

// ============================================================================
//                             E S C R I T U R A
// ============================================================================
//
//  LA MISMA MALLA QUE VOLVERIA DEL .w3dm. El binario guarda los arrays de memoria
//  (en el ORDEN de memoria: es contrato), pero todo lo que el lector de texto
//  RE-DERIVA se deriva aca igual que alla, con las mismas piezas:
//   - la identidad de los puntos (W3dMallaCalcularPuntos) -> vertCtrlPoint/posRep;
//   - que capas salen, la activa y las LAZY (W3dMallaElegirCapas);
//   - los arrays de render desde la capa ACTIVA (lo que hace AplicarCapasAlRender,
//     incluido el colapso del color por vertice), con los defaults del lector para
//     lo que ninguna cara usa (normal (0,127,0), uv 0, blanco);
//   - los pesos de vertex groups y shape keys POR PUNTO, repartidos a todos los
//     render-verts del punto; los UV groups y el reposo 2D por esquina;
//   - las marcas que siguen siendo una arista (W3dMallaMarcasValidas);
//   - las aristas por representante y el AABB/centro/radio (W3dCalcularGeomStats).
//  El index buffer canonico lo trae el que guarda (canon): triangular y Forsyth
//  son del editor.
// ============================================================================
bool W3dMallaBinEscribir(const Mesh* m, const W3dMallaBinIndices* canon, std::string& out,
                         std::vector<std::string>* avisos) {
    out.clear();
    if (!m) return false;
    // EL FRENO DE MANO (el mismo que el texto): un archivo que REQUIERE algo que no entendemos
    const std::string falta = W3dMallaBloqueQueFalta(m);
    if (!falta.empty()) {
        if (avisos) avisos->push_back("no guardo esta malla: el archivo REQUIERE el bloque '" + falta +
                                      "' y esta version no lo entiende");
        return false;
    }
    // INDICES DE 16 BITS (el N95): con mas de 65535 render-verts el index buffer de memoria y
    // el canonico (los dos son MeshIndex) ya vienen TRUNCADOS modulo 65536. Todos quedan "en
    // rango", asi que ningun chequeo de abajo lo ve: saldria un RIDX de 32 bits con los
    // triangulos equivocados, que el PC abriria sin chistar. No hay index buffer correcto
    // que escribir: se niega (el que guarda la manda en texto, que guarda las caras con int).
    if (!W3dMallaBinIndicesAlcanzan(m->vertexSize)) {
        if (avisos) {
            std::string s = "no guardo esta malla en binario: tiene ";
            W3dEscribirInt(s, m->vertexSize);
            s += " vertices de render y esta plataforma usa indices de 16 bits (hasta 65535)";
            avisos->push_back(s);
        }
        return false;
    }

    // ---- PUNTOS: la identidad de los vertices ----
    W3dMallaPuntos P;
    W3dMallaCalcularPuntos(m, P);
    const int nV = P.nV;
    const int nP = P.nP;

    // ---- CARAS: el lector del .w3dm descarta las de menos de 3 lados o con un indice fuera
    //      de rango; aca tampoco salen (y se avisa). nCtodas cuenta TODAS las esquinas: es
    //      contra lo que se miden las capas, igual que en el texto. ----
    const size_t nFt = m->faces3d.size();
    std::vector<char> caraOk(nFt > 0 ? nFt : 1, 0);
    int nF = 0, nC = 0, nCtodas = 0, carasMalas = 0;
    for (size_t f = 0; f < nFt; f++) {
        const std::vector<int>& idx = m->faces3d[f].idx;
        nCtodas += (int)idx.size();
        bool ok = idx.size() >= 3;
        for (size_t c = 0; c < idx.size() && ok; c++) if (idx[c] < 0 || idx[c] >= nV) ok = false;
        if (!ok) { carasMalas++; continue; }
        caraOk[f] = 1; nF++; nC += (int)idx.size();
    }
    if (carasMalas > 0 && avisos) {
        std::string s = "descarte "; W3dEscribirInt(s, carasMalas);
        s += " cara(s) con menos de 3 lados o con indices fuera de rango";
        avisos->push_back(s);
    }

    // ---- IDENTIDAD: cada render-vert -> su punto de control ----
    //  EDITABLE: el PRIMER render-vert del punto (representante), porque la edicion (posRep/bordes) razona en
    //    indices de render-vert. Deja nCtrl ~= nV (disperso) pero preserva toda la info de edicion.
    //  NO-EDITABLE (malla de JUEGO): el INDICE DE PUNTO COMPACTO (0..nP-1). Asi al abrir, vertCtrlPoint ya viene
    //    agrupado (nCtrl = nP) y el skinning NO recalcula nada en el telefono (el agrupamiento queda HORNEADO).
    //    Para que cierre, los vertex groups y shape keys de abajo tambien se escriben POR PUNTO en este modo.
    std::vector<int> ctrl((size_t)(nV > 0 ? nV : 1), 0);
    if (m->noEditable) {
        for (int i = 0; i < nV; i++) { const int p = P.rvAPunto[(size_t)i]; ctrl[(size_t)i] = (p >= 0) ? p : i; }
    } else {
        std::vector<int> repDePunto((size_t)(nP > 0 ? nP : 1), -1);
        for (int i = 0; i < nV; i++) {
            const int p = P.rvAPunto[(size_t)i];
            if (p >= 0 && repDePunto[(size_t)p] < 0) repDePunto[(size_t)p] = i;
        }
        for (int i = 0; i < nV; i++) {
            const int p = P.rvAPunto[(size_t)i];
            ctrl[(size_t)i] = (p >= 0) ? repDePunto[(size_t)p] : i;
        }
    }

    // ---- CAPAS: cuales salen (los mismos avisos que el texto) ----
    W3dMallaCapasSalen capas;
    W3dMallaElegirCapas(m, nCtodas, capas, avisos);

    // ---- ARRAYS DE RENDER: la capa ACTIVA por esquina, defaults para lo que no es de una cara ----
    std::vector<signed char>   rnrm((size_t)nV * 3);
    std::vector<float>         ruv((size_t)nV * 2, 0.0f);
    std::vector<unsigned char> rcol((size_t)nV * 4, (unsigned char)255);
    std::vector<char>          deCara((size_t)(nV > 0 ? nV : 1), 0);
    for (int i = 0; i < nV; i++) { rnrm[(size_t)i*3] = 0; rnrm[(size_t)i*3+1] = 127; rnrm[(size_t)i*3+2] = 0; }
    const bool nrmPorCapa = ((int)m->cornerNormal.size() == nCtodas * 3);
    const bool hayNRM = (nCtodas > 0 && (nrmPorCapa || m->normals));
    const UVMap* uvAct = (capas.uvActiva >= 0) ? m->uvMaps[capas.uv[(size_t)capas.uvActiva]] : 0;
    const ColorLayer* colAct = (capas.colActiva >= 0) ? m->colorLayers[capas.col[(size_t)capas.colActiva]] : 0;
    {
        int L = 0;
        for (size_t f = 0; f < nFt; f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                if (!caraOk[f]) continue;
                const int gv = m->faces3d[f].idx[c];
                deCara[(size_t)gv] = 1;
                if (hayNRM) {
                    const GLbyte* n3 = nrmPorCapa ? &m->cornerNormal[(size_t)L * 3] : &m->normals[(size_t)gv * 3];
                    rnrm[(size_t)gv*3] = (signed char)n3[0]; rnrm[(size_t)gv*3+1] = (signed char)n3[1]; rnrm[(size_t)gv*3+2] = (signed char)n3[2];
                }
                if (uvAct) { ruv[(size_t)gv*2] = uvAct->uv[(size_t)L*2]; ruv[(size_t)gv*2+1] = uvAct->uv[(size_t)L*2+1]; }
                else if (capas.uvDerivada) { ruv[(size_t)gv*2] = m->uv[(size_t)gv*2]; ruv[(size_t)gv*2+1] = m->uv[(size_t)gv*2+1]; }
                if (colAct) { for (int q = 0; q < 4; q++) rcol[(size_t)gv*4+(size_t)q] = colAct->color[(size_t)L*4+(size_t)q]; }
                else if (capas.colDerivada) { for (int q = 0; q < 4; q++) rcol[(size_t)gv*4+(size_t)q] = m->vertexColor[(size_t)gv*4+(size_t)q]; }
            }
    }
    // capa de color POR VERTICE: el color se colapsa por posicion (el primero gana), como
    // AplicarCapasAlRender despues de la carga clasica
    if (colAct && colAct->porVertice && nV > 0) {
        std::map<std::string, int> rep;
        for (int i = 0; i < nV; i++) {
            std::string k((const char*)&m->vertex[(size_t)i * 3], 12);
            std::map<std::string, int>::iterator it = rep.find(k);
            if (it == rep.end()) rep[k] = i;
            else for (int q = 0; q < 4; q++) rcol[(size_t)i*4+(size_t)q] = rcol[(size_t)it->second*4+(size_t)q];
        }
    }

    // ---- GEOMETRIA SUELTA, como la devuelve el lector: extremos por representante; un
    //      vertice suelto cuyo punto ya usa otra cosa se fusiona (y se avisa) ----
    std::vector<int> sueltasE, sueltosV;
    {
        std::vector<char> usado((size_t)(nP > 0 ? nP : 1), 0);
        for (int i = 0; i < nV; i++) if (deCara[(size_t)i] && P.rvAPunto[(size_t)i] >= 0) usado[(size_t)P.rvAPunto[(size_t)i]] = 1;
        int malas = 0, fusionados = 0;
        for (size_t i = 0; i + 1 < m->looseEdges.size(); i += 2) {
            const int a = m->looseEdges[i], b = m->looseEdges[i + 1];
            if (a < 0 || a >= nV || b < 0 || b >= nV) { malas++; continue; }
            sueltasE.push_back(ctrl[(size_t)a]); sueltasE.push_back(ctrl[(size_t)b]);
            usado[(size_t)P.rvAPunto[(size_t)a]] = 1; usado[(size_t)P.rvAPunto[(size_t)b]] = 1;
        }
        for (size_t i = 0; i < m->looseVerts.size(); i++) {
            const int a = m->looseVerts[i];
            if (a < 0 || a >= nV) { malas++; continue; }
            const int p = P.rvAPunto[(size_t)a];
            if (usado[(size_t)p]) { fusionados++; continue; }
            usado[(size_t)p] = 1;
            sueltosV.push_back(ctrl[(size_t)a]);
        }
        if (malas > 0 && avisos) {
            std::string s = "descarte "; W3dEscribirInt(s, malas);
            s += " arista(s)/vertice(s) suelto(s) con indices fuera de rango";
            avisos->push_back(s);
        }
        if (fusionados > 0 && avisos) {
            std::string s = ""; W3dEscribirInt(s, fusionados);
            s += " vertice(s) suelto(s) caian en un punto que ya estaba en uso: los fusiono con el que ya estaba";
            avisos->push_back(s);
        }
    }

    // ---- ARISTAS REALES (pares de puntos): validan las marcas y cuentan el dominio arista ----
    const bool hayMarcas = (!m->sharpEdges.empty() || !m->seamEdges.empty());
    const bool hayAjenos = (!m->w3dmAjenos.bloques.empty() || !m->w3dmAjenos.bloquesBin.empty());
    std::set<std::pair<int, int> > aristasReales;
    const bool conAristas = (hayMarcas || hayAjenos);
    if (conAristas) W3dMallaAristasReales(m, P, aristasReales);

    // ---- MESH PARTS: al menos una (la malla sin partes vuelve con una por defecto) ----
    const int nPartes = m->materialsGroup.empty() ? 1 : (int)m->materialsGroup.size();

    // ---- INDEX BUFFER: el canonico que trae el editor, o el de memoria tal cual ----
    const MeshIndex* idxSrc = 0;
    int nIdx = 0;
    std::vector<int> partIni((size_t)nPartes, 0), partCnt((size_t)nPartes, 0);
    if (canon) {
        if ((int)canon->inicio.size() != nPartes || (int)canon->cantidad.size() != nPartes) {
            if (avisos) avisos->push_back("el index buffer canonico no tiene un rango por mesh part: no guardo la malla");
            return false;
        }
        idxSrc = canon->idx.empty() ? 0 : &canon->idx[0];
        nIdx = (int)canon->idx.size();
        partIni = canon->inicio; partCnt = canon->cantidad;
    } else {
        idxSrc = m->faces;
        nIdx = (m->faces && m->facesSize > 0) ? m->facesSize : 0;
        for (int k = 0; k < nPartes && k < (int)m->materialsGroup.size(); k++) {
            partIni[(size_t)k] = m->materialsGroup[(size_t)k].startDrawn;
            partCnt[(size_t)k] = m->materialsGroup[(size_t)k].indicesDrawnCount;
        }
    }
    if (nIdx % 3 != 0) { if (avisos) avisos->push_back("el index buffer no es de triangulos: no guardo la malla"); return false; }
    for (int i = 0; i < nIdx; i++)
        if ((unsigned)idxSrc[i] >= (unsigned)nV) { if (avisos) avisos->push_back("el index buffer apunta fuera de la malla: no guardo la malla"); return false; }
    for (int k = 0; k < nPartes; k++) {
        const int a = partIni[(size_t)k], c = partCnt[(size_t)k];
        if (a < 0 || c < 0 || a > nIdx || c > nIdx - a) {
            if (avisos) avisos->push_back("un mesh part sale del index buffer: no guardo la malla");
            return false;
        }
    }

    // ---- AABB / centro / radio (el mismo codigo que CalcularBordes / CalcularAABBSolo) ----
    W3dGeomStats st;
    W3dCalcularGeomStats(m->vertex, nV, (m->noEditable || nV <= 0) ? NULL : &ctrl[0], st);

    // ---- INFO ----
    InfoBin I;
    I.nRV = (unsigned)nV;
    I.nIdx = (unsigned)nIdx;
    I.nPartes = (unsigned)nPartes;
    I.nCaras = (unsigned)nF;
    I.nEsquinas = (unsigned)nC;
    I.nPuntos = nP;
    I.nAristas = conAristas ? (int)aristasReales.size() : -1;
    I.banderas = (m->meshSmooth ? INFO_SUAVE : 0) | (st.ok ? INFO_AABBOK : 0) |
                 (m->noEditable ? INFO_STATS_NOEDIT : 0) | (m->meshTipo >= 0 ? INFO_PRIM : 0);
    if (m->meshTipo >= 0) {
        I.primTipo = m->meshTipo; I.primTam = m->meshSize; I.primTam2 = m->meshSize2;
        I.primAlto = m->meshDepth; I.primVerts = m->meshVerts; I.primVerts2 = m->meshVerts2;
    }
    if (st.ok) {
        I.aabbMin[0] = st.aabbMin.x; I.aabbMin[1] = st.aabbMin.y; I.aabbMin[2] = st.aabbMin.z;
        I.aabbMax[0] = st.aabbMax.x; I.aabbMax[1] = st.aabbMax.y; I.aabbMax[2] = st.aabbMax.z;
        I.centro[0] = st.centro.x;   I.centro[1] = st.centro.y;   I.centro[2] = st.centro.z;
        I.radio = st.radio;
    }
    I.vertsAgrupados = m->noEditable ? 0 : st.agrupados;
    // los ACTIVOS como los deja el lector de texto
    // sin capas del editor (uvDerivada) salen la derivada "UVMap" + las capas extra que la malla ya tenga en el
    // render (uvExtra: TEXCOORD_1 importada o un RUVX leido), cada una en su mismo indice
    int nUVder = 1;
    for (size_t k = 1; k < m->uvExtra.size(); k++) if ((int)m->uvExtra[k].size() == nV * 2) nUVder = (int)k + 1;
    const int nUVsalen = capas.uvDerivada ? nUVder : (int)capas.uv.size();
    const int nColSalen = capas.colDerivada ? 1 : (int)capas.col.size();
    I.uvActiva  = capas.uvDerivada  ? 0 : (m->uvMaps.empty()      ? -1 : ActivoValido(capas.uvActiva, nUVsalen));
    I.colActiva = capas.colDerivada ? 0 : (m->colorLayers.empty() ? -1 : ActivoValido(capas.colActiva, nColSalen));
    int nVGsalen = 0, nUVGsalen = 0;
    for (size_t g = 0; g < m->vertexGroups.size(); g++) if (m->vertexGroups[g]) nVGsalen++;
    for (size_t g = 0; g < m->uvGroups.size(); g++)     if (m->uvGroups[g]) nUVGsalen++;
    I.grupoActivo   = ActivoValido(m->vertexGroups.empty() ? -1 : m->grupoActivo, nVGsalen);
    I.uvGrupoActivo = ActivoValido(m->uvGroups.empty() ? -1 : m->uvGrupoActivo, nUVGsalen);
    I.nombre = W3dMallaNombreSano(m->name, avisos, "de la malla");

    // ======================================================================
    //  EMISION
    // ======================================================================
    unsigned nBloques = 0;
    out.reserve((size_t)nV * 64 + (size_t)nIdx * 4 + (size_t)nC * 16 + 1024);
    out.append("W3DB", 4);
    PonU32(out, W3DB_VERSION);
    PonU32(out, 0);             // banderas
    const size_t posNBloques = out.size();
    PonU32(out, 0);             // nBloques (se parchea al final)
    PonU32(out, 0);             // reservado

    size_t pb;
    // REQ
    {
        int nReq = 0; while (kRequeridos[nReq]) nReq++;
        pb = AbrirBloque(out, "REQ ", W3DB_GLOBAL, (unsigned)nReq);
        for (int i = 0; i < nReq; i++) out.append(kRequeridos[i], 4);
        CerrarBloque(out, pb, nBloques);
    }
    // INFO
    pb = AbrirBloque(out, "INFO", W3DB_GLOBAL, 1);
    EscribirInfo(out, I);
    CerrarBloque(out, pb, nBloques);
    // RENDER
    if (nV > 0) {
        pb = AbrirBloque(out, "RPOS", W3DB_RENDER, (unsigned)nV); PonF32s(out, m->vertex, (size_t)nV * 3); CerrarBloque(out, pb, nBloques);
        pb = AbrirBloque(out, "RNRM", W3DB_RENDER, (unsigned)nV); PonBytes(out, &rnrm[0], (size_t)nV * 3); CerrarBloque(out, pb, nBloques);
        pb = AbrirBloque(out, "RUV ", W3DB_RENDER, (unsigned)nV); PonF32s(out, &ruv[0], (size_t)nV * 2); CerrarBloque(out, pb, nBloques);
        pb = AbrirBloque(out, "RCOL", W3DB_RENDER, (unsigned)nV); PonBytes(out, &rcol[0], (size_t)nV * 4); CerrarBloque(out, pb, nBloques);
        // ---- RUVX: las capas UV que NO son la activa, por render-vert (Mesh::uvExtra: las capas de textura con
        //      UV propio, TexLayer::uvMapa). La activa va "ausente" (su render es RUV). Solo si alguna existe. ----
        {
            std::vector< std::vector<float> > rx((size_t)(nUVsalen > 0 ? nUVsalen : 0));
            bool alguna = false;
            for (int l = 0; l < nUVsalen; l++) {
                if (l == I.uvActiva) continue;
                std::vector<float>& dd = rx[(size_t)l];
                if (!capas.uvDerivada) {
                    const UVMap* u = m->uvMaps[capas.uv[(size_t)l]];
                    if ((int)u->uv.size() != nCtodas * 2) continue;
                    dd.assign((size_t)nV * 2, 0.0f);
                    int L = 0;
                    for (size_t f = 0; f < nFt; f++)
                        for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                            if (!caraOk[f]) continue;
                            const int gv = m->faces3d[f].idx[c];
                            dd[(size_t)gv*2] = u->uv[(size_t)L*2]; dd[(size_t)gv*2+1] = u->uv[(size_t)L*2+1];
                        }
                } else if (l < (int)m->uvExtra.size() && (int)m->uvExtra[(size_t)l].size() == nV * 2) {
                    dd = m->uvExtra[(size_t)l];
                } else continue;
                alguna = true;
            }
            if (alguna) {
                pb = AbrirBloque(out, "RUVX", W3DB_GLOBAL, (unsigned)nUVsalen);
                for (int l = 0; l < nUVsalen; l++) {
                    const bool presente = !rx[(size_t)l].empty();
                    PonU32(out, presente ? 1u : 0u);
                    if (presente) PonF32s(out, &rx[(size_t)l][0], (size_t)nV * 2);
                }
                CerrarBloque(out, pb, nBloques);
            }
        }
    }
    if (nIdx > 0) {
        const bool i32 = nV > 65535;
        pb = AbrirBloque(out, "RIDX", W3DB_TRIANGULO | (i32 ? W3DB_IDX32 : 0), (unsigned)(nIdx / 3));
        if (i32) for (int i = 0; i < nIdx; i++) PonU32(out, (unsigned)idxSrc[i]);
        else for (int i = 0; i < nIdx; i++) {
            const unsigned x = (unsigned)idxSrc[i];
            char b2[2]; b2[0] = (char)(x & 255u); b2[1] = (char)((x >> 8) & 255u);
            out.append(b2, 2);
        }
        CerrarBloque(out, pb, nBloques);
    }
    // PART
    pb = AbrirBloque(out, "PART", W3DB_GLOBAL, (unsigned)nPartes);
    for (int k = 0; k < nPartes; k++) {
        std::string nom = "Mesh", mat;
        if (k < (int)m->materialsGroup.size()) {
            const MaterialGroup& mg = m->materialsGroup[(size_t)k];
            nom = W3dMallaNombreSano(mg.name, avisos, "de un mesh part");
            if (mg.material && !mg.material->name.empty()) mat = W3dMallaNombreSano(mg.material->name, avisos, "de un material");
        }
        PonStr(out, nom); PonStr(out, mat);
        PonI32(out, partIni[(size_t)k]); PonI32(out, partCnt[(size_t)k]);
    }
    CerrarBloque(out, pb, nBloques);
    // RCTL
    if (nV > 0) { pb = AbrirBloque(out, "RCTL", W3DB_RENDER, (unsigned)nV); PonI32s(out, &ctrl[0], (size_t)nV); CerrarBloque(out, pb, nBloques); }

    // ---- VERTEX GROUPS y SHAPE KEYS: por PUNTO (como el texto), repartidos a TODOS los
    //      render-verts del punto (como el lector de texto) ----
    std::vector<int> csrOff, csrRv;   // punto -> sus render-verts, de menor a mayor
    if (!m->vertexGroups.empty() || !m->shapeKeys.empty()) {
        csrOff.assign((size_t)nP + 1, 0);
        for (int i = 0; i < nV; i++) { const int p = P.rvAPunto[(size_t)i]; if (p >= 0) csrOff[(size_t)p + 1]++; }
        for (int p = 0; p < nP; p++) csrOff[(size_t)p + 1] += csrOff[(size_t)p];
        csrRv.assign((size_t)(csrOff[(size_t)nP] > 0 ? csrOff[(size_t)nP] : 1), 0);
        std::vector<int> llenos((size_t)(nP > 0 ? nP : 1), 0);
        for (int i = 0; i < nV; i++) {
            const int p = P.rvAPunto[(size_t)i];
            if (p < 0) continue;
            csrRv[(size_t)(csrOff[(size_t)p] + llenos[(size_t)p]++)] = i;
        }
    }
    std::map<int, int> repDeCp;
    W3dMallaRepDeCp(m, repDeCp);
    // EDITABLE: los pesos/deltas se reparten a TODOS los render-verts del punto (vg->verts = render-verts), que es
    //   lo que el lector editable y el weight paint esperan. NO-EDITABLE: se escriben POR PUNTO (vg->verts = punto
    //   compacto), en el MISMO espacio que el ctrl compacto -> el skinning los usa directo, sin repartir ni agrupar.
    const bool compacto = m->noEditable;
    if (nVGsalen > 0) {
        pb = AbrirBloque(out, "RVGR", W3DB_GLOBAL, (unsigned)nVGsalen);
        for (size_t g = 0; g < m->vertexGroups.size(); g++) {
            std::vector<int> pi; std::vector<float> pw;
            if (!W3dMallaGrupoPorPunto(m, P, repDeCp, g, pi, pw, avisos)) continue;
            std::vector<int> ri; std::vector<float> rw;
            if (compacto) { ri = pi; rw = pw; }   // por punto compacto (0..nP-1)
            else for (size_t k = 0; k < pi.size(); k++) {
                const int p = pi[k];
                for (int q = csrOff[(size_t)p]; q < csrOff[(size_t)p + 1]; q++) { ri.push_back(csrRv[(size_t)q]); rw.push_back(pw[k]); }
            }
            PonStr(out, W3dMallaNombreSano(m->vertexGroups[g]->nombre, avisos, "de un vertex group"));
            PonU32(out, (unsigned)ri.size());
            if (!ri.empty()) { PonI32s(out, &ri[0], ri.size()); PonF32s(out, &rw[0], rw.size()); }
        }
        CerrarBloque(out, pb, nBloques);
    }
    if (!m->shapeKeys.empty()) {
        pb = AbrirBloque(out, "RSHP", W3DB_GLOBAL, (unsigned)m->shapeKeys.size());
        for (size_t k = 0; k < m->shapeKeys.size(); k++) {
            std::vector<int> pi; std::vector<float> pd;
            W3dMallaFormaPorPunto(m, P, repDeCp, k, pi, pd);
            std::vector<int> ri; std::vector<float> rd;
            if (compacto) { ri = pi; rd = pd; }   // por punto compacto (0..nP-1)
            else for (size_t q = 0; q < pi.size(); q++) {
                const int p = pi[q];
                for (int r = csrOff[(size_t)p]; r < csrOff[(size_t)p + 1]; r++) {
                    ri.push_back(csrRv[(size_t)r]);
                    rd.push_back(pd[q*3]); rd.push_back(pd[q*3+1]); rd.push_back(pd[q*3+2]);
                }
            }
            PonStr(out, W3dMallaNombreSano(m->shapeKeys[k].nombre, avisos, "de una shape key"));
            PonU32(out, (unsigned)ri.size());
            if (!ri.empty()) { PonI32s(out, &ri[0], ri.size()); PonF32s(out, &rd[0], rd.size()); }
        }
        CerrarBloque(out, pb, nBloques);
    }
    // ---- UV GROUPS: el peso de cada render-vert que usa una cara (el lector de texto
    //      colapsa las esquinas a su render-vert: el resto se pierde) ----
    if (nUVGsalen > 0) {
        pb = AbrirBloque(out, "RUVG", W3DB_GLOBAL, (unsigned)nUVGsalen);
        for (size_t g = 0; g < m->uvGroups.size(); g++) {
            const UVGroup* ug = m->uvGroups[g];
            if (!ug) continue;
            std::vector<float> w((size_t)(nV > 0 ? nV : 1), 0.0f);
            for (size_t k = 0; k < ug->verts.size() && k < ug->pesos.size(); k++) {
                const int rv = ug->verts[k];
                if (rv >= 0 && rv < nV) w[(size_t)rv] = ug->pesos[k];
            }
            std::vector<int> ri; std::vector<float> rw;
            for (int i = 0; i < nV; i++) if (deCara[(size_t)i] && w[(size_t)i] > 0.0f) { ri.push_back(i); rw.push_back(w[(size_t)i]); }
            PonStr(out, W3dMallaNombreSano(ug->nombre, avisos, "de un UV group"));
            PonU32(out, (unsigned)ri.size());
            if (!ri.empty()) { PonI32s(out, &ri[0], ri.size()); PonF32s(out, &rw[0], rw.size()); }
        }
        CerrarBloque(out, pb, nBloques);
    }
    // ---- REPOSO 2D (base del skinning de UV): por render-vert de cara, 0 el resto ----
    if ((int)m->uv2dRest.size() == nV * 2 && nV > 0 && nCtodas > 0) {
        std::vector<float> r((size_t)nV * 2, 0.0f);
        for (int i = 0; i < nV; i++) if (deCara[(size_t)i]) { r[(size_t)i*2] = m->uv2dRest[(size_t)i*2]; r[(size_t)i*2+1] = m->uv2dRest[(size_t)i*2+1]; }
        pb = AbrirBloque(out, "RUVR", W3DB_RENDER, (unsigned)nV); PonF32s(out, &r[0], r.size()); CerrarBloque(out, pb, nBloques);
    }

    // ======================================================================
    //  BLOQUES DE EDICION
    // ======================================================================
    // ---- FACE: lados | mesh part | shading (i8, relleno a 4) | render-verts ----
    if (nF > 0) {
        const int nGr = nPartes;
        pb = AbrirBloque(out, "FACE", W3DB_CARA, (unsigned)nF);
        PonU32(out, (unsigned)nC);
        for (size_t f = 0; f < nFt; f++) if (caraOk[f]) PonI32(out, (int)m->faces3d[f].idx.size());
        for (size_t f = 0; f < nFt; f++) if (caraOk[f]) {
            const int g = m->faces3d[f].mat;
            PonI32(out, (g < 0 || g >= nGr) ? 0 : g);   // el lector de texto manda al 0 una parte que no existe
        }
        for (size_t f = 0; f < nFt; f++) if (caraOk[f]) {
            const int sm = m->faces3d[f].smooth;
            out += (char)(signed char)(sm < 0 ? -1 : (sm != 0 ? 1 : 0));
        }
        while ((out.size() - (pb + 4)) % 4) out += '\0';
        for (size_t f = 0; f < nFt; f++) if (caraOk[f]) PonI32s(out, &m->faces3d[f].idx[0], m->faces3d[f].idx.size());
        CerrarBloque(out, pb, nBloques);
    }
    // ---- LOOS: aristas y vertices sueltos ----
    if (!sueltasE.empty() || !sueltosV.empty()) {
        pb = AbrirBloque(out, "LOOS", W3DB_GLOBAL, 2);
        PonU32(out, (unsigned)sueltasE.size()); if (!sueltasE.empty()) PonI32s(out, &sueltasE[0], sueltasE.size());
        PonU32(out, (unsigned)sueltosV.size()); if (!sueltosV.empty()) PonI32s(out, &sueltosV[0], sueltosV.size());
        CerrarBloque(out, pb, nBloques);
    }
    // ---- CNRM: la normal por esquina (la capa, o la derivada del render) ----
    if (hayNRM && nC > 0) {
        pb = AbrirBloque(out, "CNRM", W3DB_ESQUINA, (unsigned)nC);
        int L = 0;
        for (size_t f = 0; f < nFt; f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                if (!caraOk[f]) continue;
                const GLbyte* n3 = nrmPorCapa ? &m->cornerNormal[(size_t)L * 3] : &m->normals[(size_t)m->faces3d[f].idx[c] * 3];
                out += (char)n3[0]; out += (char)n3[1]; out += (char)n3[2];
            }
        CerrarBloque(out, pb, nBloques);
    }
    // ---- UVLY: las capas UV que salen (o la derivada "UVMap") ----
    if (nUVsalen > 0 && (capas.uvDerivada || !capas.uv.empty())) {
        pb = AbrirBloque(out, "UVLY", W3DB_GLOBAL, (unsigned)nUVsalen);
        for (int l = 0; l < nUVsalen; l++) {
            const UVMap* u = capas.uvDerivada ? 0 : m->uvMaps[capas.uv[(size_t)l]];
            // derivada: la capa 0 es uv[]; las siguientes, uvExtra[l] (si falta, ceros)
            const GLfloat* der = 0;
            if (!u) der = (l == 0) ? m->uv
                        : ((l < (int)m->uvExtra.size() && (int)m->uvExtra[(size_t)l].size() == nV * 2) ? &m->uvExtra[(size_t)l][0] : 0);
            char nomDer[16];
            if (l == 0) strcpy(nomDer, "UVMap"); else snprintf(nomDer, sizeof nomDer, "UV%d", l + 1);
            PonStr(out, u ? W3dMallaNombreSano(u->nombre, avisos, "de una capa UV") : std::string(nomDer));
            int L = 0;
            for (size_t f = 0; f < nFt; f++)
                for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                    if (!caraOk[f]) continue;
                    if (u) { PonF32(out, u->uv[(size_t)L*2]); PonF32(out, u->uv[(size_t)L*2+1]); }
                    else if (der) { const int gv = m->faces3d[f].idx[c]; PonF32(out, der[(size_t)gv*2]); PonF32(out, der[(size_t)gv*2+1]); }
                    else { PonF32(out, 0.0f); PonF32(out, 0.0f); }
                }
        }
        CerrarBloque(out, pb, nBloques);
    }
    // ---- COLY: las capas de color que salen (o la derivada "Col") ----
    if (nColSalen > 0 && (capas.colDerivada || !capas.col.empty())) {
        pb = AbrirBloque(out, "COLY", W3DB_GLOBAL, (unsigned)nColSalen);
        for (int l = 0; l < nColSalen; l++) {
            const ColorLayer* cl = capas.colDerivada ? 0 : m->colorLayers[capas.col[(size_t)l]];
            PonStr(out, cl ? W3dMallaNombreSano(cl->nombre, avisos, "de una capa de color") : std::string("Col"));
            PonU32(out, (cl && cl->porVertice) ? COLY_POR_VERTICE : 0);
            int L = 0;
            for (size_t f = 0; f < nFt; f++)
                for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                    if (!caraOk[f]) continue;
                    const unsigned char* c4 = cl ? &cl->color[(size_t)L*4] : &m->vertexColor[(size_t)m->faces3d[f].idx[c]*4];
                    PonBytes(out, c4, 4);
                }
        }
        CerrarBloque(out, pb, nBloques);
    }
    // ---- SHRP / SEAM: las que siguen siendo una arista, como claves de posicion ----
    if (hayMarcas) {
        for (int pasada = 0; pasada < 2; pasada++) {
            std::vector<std::pair<int, int> > pares;
            W3dMallaMarcasValidas(m, P, aristasReales, pasada != 0, pares, avisos);
            // par de PUNTOS -> clave de POSICION (la que rearma el lector de texto), armada en
            // little-endian desde los floats: ni el orden ADENTRO de cada clave (sus dos mitades)
            // ni el del set (el del archivo) dependen del host
            std::set<std::string> claves;
            for (size_t i = 0; i < pares.size(); i++)
                claves.insert(ClaveMarcaArchivo(&P.pts[(size_t)pares[i].first * 3], &P.pts[(size_t)pares[i].second * 3]));
            if (claves.empty()) continue;
            pb = AbrirBloque(out, pasada ? "SEAM" : "SHRP", W3DB_ARISTA, (unsigned)claves.size());
            for (std::set<std::string>::const_iterator it = claves.begin(); it != claves.end(); ++it) out += *it;
            CerrarBloque(out, pb, nBloques);
        }
    }
    // ---- EDGE: las aristas por representante (sin el std::set de CalcularBordes al abrir).
    //      Un escenario cerrado no las tiene. ----
    if (!m->noEditable && nV > 0 && (nF > 0 || !sueltasE.empty())) {
        std::vector<MeshFace> carasOk;
        carasOk.reserve((size_t)nF);
        for (size_t f = 0; f < nFt; f++) if (caraOk[f]) carasOk.push_back(m->faces3d[f]);
        std::vector<int> edges;
        AristasPorRep(carasOk, sueltasE, &ctrl[0], nV, edges);
        pb = AbrirBloque(out, "EDGE", W3DB_ARISTA, (unsigned)(edges.size() / 2));
        if (!edges.empty()) PonI32s(out, &edges[0], edges.size());
        CerrarBloque(out, pb, nBloques);
    }
    // ---- AJEN: bloques de TEXTO de un .w3dm mas nuevo + su "requiere" ----
    {
        std::vector<std::string> req = m->w3dmAjenos.requiere;
        const bool reqDefault = req.empty() || (req.size() == 2 && req[0] == "V" && req[1] == "F");
        std::vector<const W3dBloqueAjeno*> quedan;
        for (size_t i = 0; i < m->w3dmAjenos.bloques.size(); i++) {
            const W3dBloqueAjeno& b = m->w3dmAjenos.bloques[i];
            int ahora = -1, antes = -1;
            switch (b.dominio) {
                case W3DB_PUNTO:   ahora = nP; antes = m->w3dmAjenos.nPuntos;   break;
                case W3DB_ESQUINA: ahora = nC; antes = m->w3dmAjenos.nEsquinas; break;
                case W3DB_CARA:    ahora = nF; antes = m->w3dmAjenos.nCaras;    break;
                case W3DB_ARISTA:  ahora = (int)aristasReales.size(); antes = m->w3dmAjenos.nAristas; break;
                default: break;
            }
            if (b.dominio != W3DB_GLOBAL && ahora != antes) {
                if (avisos) {
                    std::string s = "no pude conservar el bloque " + b.nombre + " (";
                    W3dEscribirInt(s, antes); s += " -> "; W3dEscribirInt(s, ahora);
                    s += "): lo trajo una version mas nueva de Whisk3D y cambiaste la topologia";
                    avisos->push_back(s);
                }
                continue;
            }
            quedan.push_back(&b);
        }
        if (!reqDefault || !quedan.empty()) {
            pb = AbrirBloque(out, "AJEN", W3DB_GLOBAL, 1);
            if (req.empty()) { req.push_back("V"); req.push_back("F"); }
            PonU32(out, (unsigned)req.size());
            for (size_t i = 0; i < req.size(); i++) PonStr(out, W3dMallaNombreSano(req[i], avisos, "de un bloque de 'requiere'"));
            PonU32(out, (unsigned)quedan.size());
            for (size_t i = 0; i < quedan.size(); i++) {
                PonStr(out, quedan[i]->nombre); PonI32(out, quedan[i]->dominio);
                PonI32(out, quedan[i]->cantidad); PonStr(out, quedan[i]->texto);
            }
            CerrarBloque(out, pb, nBloques);
        }
    }
    // ---- BLOQUES BINARIOS DE UNA VERSION MAS NUEVA: tal cual, con la regla del sello ----
    for (size_t i = 0; i < m->w3dmAjenos.bloquesBin.size(); i++) {
        const W3dBloqueAjeno& b = m->w3dmAjenos.bloquesBin[i];
        const unsigned dom = (unsigned)b.dominio & 255u;
        int ahora = -1, antes = -1;
        switch (dom) {
            case W3DB_PUNTO:     ahora = nP;       antes = m->w3dmAjenos.nPuntos;     break;
            case W3DB_ESQUINA:   ahora = nC;       antes = m->w3dmAjenos.nEsquinas;   break;
            case W3DB_CARA:      ahora = nF;       antes = m->w3dmAjenos.nCaras;      break;
            case W3DB_ARISTA:    ahora = (int)aristasReales.size(); antes = m->w3dmAjenos.nAristas; break;
            case W3DB_RENDER:    ahora = nV;       antes = m->w3dmAjenos.nRender;     break;
            case W3DB_TRIANGULO: ahora = nIdx / 3; antes = m->w3dmAjenos.nTriangulos; break;
            default: break;
        }
        if (dom != W3DB_GLOBAL && ahora != antes) {
            if (avisos) {
                std::string s = "no pude conservar el bloque binario " + b.nombre + " (";
                W3dEscribirInt(s, antes); s += " -> "; W3dEscribirInt(s, ahora);
                s += "): lo trajo una version mas nueva de Whisk3D y cambiaste la topologia";
                avisos->push_back(s);
            }
            continue;
        }
        if (b.nombre.size() != 4) continue;
        pb = AbrirBloque(out, b.nombre.c_str(), (unsigned)b.dominio, (unsigned)b.cantidad);
        out += b.texto;
        CerrarBloque(out, pb, nBloques);
    }

    // la cantidad de bloques, al final (la cabecera se escribio antes de saberla)
    out[posNBloques]     = (char)(nBloques & 255u);
    out[posNBloques + 1] = (char)((nBloques >> 8) & 255u);
    out[posNBloques + 2] = (char)((nBloques >> 16) & 255u);
    out[posNBloques + 3] = (char)((nBloques >> 24) & 255u);
    return true;
}
