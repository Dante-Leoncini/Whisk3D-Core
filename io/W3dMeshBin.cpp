#include "W3dMeshBin.h"
#include "W3dMesh.h"         // W3dMeshInfo + las piezas del escritor que comparten los dos formatos
#include "W3dText.h"         // W3dWriteInt (los avisos)
#include "w3dFilesystem.h"   // MaterializarEdicion relee la entrada del contenedor
#include "objects/Mesh.h"
#include "objects/MeshResource.h"   // MaterializarEdicion: la edicion de una malla que usa un recurso
#include "base/w3dlog.h"            // w3dLogf (diagnostico del reagrupamiento de puntos)

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <cstring>
#include <cstdio>

// ============================================================================
//  LOS BLOQUES QUE ESTA VERSION ENTIENDE (ver W3dMeshBin.h). Un fourcc son 4
//  letras: los de 3 se completan con un espacio ("REQ ", "RUV ").
// ============================================================================
static const char* kBlocksBin[] = {
    "REQ ", "INFO", "RPOS", "RNRM", "RUV ", "RCOL", "RIDX", "PART", "RCTL",
    "RVGR", "RSHP", "RUVG", "RUVR", "RUVX",
    "FACE", "LOOS", "CNRM", "UVLY", "COLY", "SHRP", "SEAM", "EDGE", "AJEN", 0
};
// lo MINIMO que un lector tiene que entender para no romper la malla: van en REQ
static const char* kRequired[] = { "INFO", "RPOS", "RIDX", "PART", 0 };

bool W3dMeshBinBlockKnown(const std::string& f) {
    for (int i = 0; kBlocksBin[i]; i++) if (f == kBlocksBin[i]) return true;
    return false;
}

void W3dMeshBinBlocksKnown(std::vector<std::string>& known, std::vector<std::string>* required) {
    known.clear();
    for (int i = 0; kBlocksBin[i]; i++) known.push_back(kBlocksBin[i]);
    if (required) {
        required->clear();
        for (int i = 0; kRequired[i]; i++) required->push_back(kRequired[i]);
    }
}

bool W3dMeshBinIs(const unsigned char* d, size_t n) {
    return d && n >= 4 && d[0] == 'W' && d[1] == '3' && d[2] == 'D' && d[3] == 'B';
}

bool W3dMeshBinIndicesReach(int nRV) {
    return sizeof(MeshIndex) >= 4 || nRV <= 65535;
}

// banderas de INFO
enum { INFO_SMOOTH = 1, INFO_AABBOK = 2, INFO_STATS_NOEDIT = 4, INFO_PRIM = 8 };
// banderas de cada capa de COLY
enum { COLY_BY_VERTEX = 1 };

// (W3dMesh.cpp) une por posicion los puntos de una malla sin pesos: la costura y los polos de los archivos de antes
class Mesh;
void W3dMeshJoinByPosition(Mesh* m);

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

void SetU32(std::string& s, unsigned v) {
    char b[4];
    b[0] = (char)(v & 255u); b[1] = (char)((v >> 8) & 255u);
    b[2] = (char)((v >> 16) & 255u); b[3] = (char)((v >> 24) & 255u);
    s.append(b, 4);
}
void SetI32(std::string& s, int v) { SetU32(s, (unsigned)v); }
void SetF32(std::string& s, float f) { unsigned u; memcpy(&u, &f, 4); SetU32(s, u); }
void SetStr(std::string& s, const std::string& t) { SetU32(s, (unsigned)t.size()); s.append(t); }
void SetF32s(std::string& s, const float* p, size_t n) {
    if (!n) return;
    if (HostLE()) s.append((const char*)p, n * 4);
    else for (size_t i = 0; i < n; i++) SetF32(s, p[i]);
}
void SetI32s(std::string& s, const int* p, size_t n) {
    if (!n) return;
    if (HostLE()) s.append((const char*)p, n * 4);
    else for (size_t i = 0; i < n; i++) SetI32(s, p[i]);
}
void SetBytes(std::string& s, const void* p, size_t n) { if (n) s.append((const char*)p, n); }

void CopyF32(const unsigned char* src, float* dst, size_t n) {
    if (!n) return;
    if (HostLE()) { memcpy(dst, src, n * 4); return; }
    for (size_t i = 0; i < n; i++) { unsigned u = LeeU32(src + i * 4); memcpy(&dst[i], &u, 4); }
}
void CopyI32(const unsigned char* src, int* dst, size_t n) {
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
std::string KeyMarkFile(const float* a, const float* b) {
    std::string ka, kb;
    for (int k = 0; k < 3; k++) { SetF32(ka, a[k]); SetF32(kb, b[k]); }
    return (memcmp(kb.data(), ka.data(), 12) < 0) ? (kb + ka) : (ka + kb);
}
// La de memoria (el lector): los 6 floats del archivo -> la clave que arma ESTE host, sea
// cual sea el orden de las mitades en el archivo (ver arriba).
std::string KeyMarkMemory(const unsigned char* p) {
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
size_t OpenBlock(std::string& s, const char* id, unsigned domain, unsigned count) {
    s.append(id, 4);
    SetU32(s, domain);
    SetU32(s, count);
    const size_t pos = s.size();
    SetU32(s, 0);
    return pos;
}
void CloseBlock(std::string& s, size_t posBytes, unsigned& nBlocks) {
    const unsigned bytes = (unsigned)(s.size() - (posBytes + 4));
    s[posBytes]     = (char)(bytes & 255u);
    s[posBytes + 1] = (char)((bytes >> 8) & 255u);
    s[posBytes + 2] = (char)((bytes >> 16) & 255u);
    s[posBytes + 3] = (char)((bytes >> 24) & 255u);
    while (s.size() % 4) s += '\0';
    nBlocks++;
}

// ---- CURSOR de lectura: todo acceso controla el largo (un archivo corrupto
//      falla, jamas lee fuera del buffer ni reserva lo que no existe) ----
struct Cursor {
    const unsigned char* p;
    const unsigned char* end;
    bool ok;
    Cursor(const unsigned char* a, size_t n) : p(a), end(a + n), ok(true) {}
    size_t Remain() const { return ok ? (size_t)(end - p) : 0; }
    unsigned U32() {
        if (Remain() < 4) { ok = false; return 0; }
        const unsigned v = LeeU32(p); p += 4; return v;
    }
    int I32() { return (int)U32(); }
    float F32() { const unsigned u = U32(); float f; memcpy(&f, &u, 4); return f; }
    bool Str(std::string& out) {
        const unsigned n = U32();
        if (!ok || Remain() < n) { ok = false; return false; }
        out.assign((const char*)p, n); p += n; return true;
    }
    // vista de nElem elementos de elemSize bytes (con control de desborde de la multiplicacion)
    const unsigned char* Take(size_t nElem, size_t elemSize) {
        if (!ok) return 0;
        if (elemSize && nElem > Remain() / elemSize) { ok = false; return 0; }
        const unsigned char* r = p;
        p += nElem * elemSize;
        return r;
    }
};

// ===========================================================================
//  LA TABLA DE BLOQUES
// ===========================================================================
struct Block {
    std::string id;      // fourcc
    unsigned domain;     // bits 0..7 = dominio, 8..15 = banderas
    unsigned count;
    unsigned bytes;
    const unsigned char* data;
};

struct Table {
    unsigned version;
    std::vector<Block> blocks;
};

// false = no es un .w3db / version mas nueva / tabla rota. 'motivo' dice cual.
bool ReadTable(const unsigned char* d, size_t n, Table& t, std::string& reason, bool& moreNew) {
    moreNew = false;
    if (!W3dMeshBinIs(d, n) || n < 20) { reason = "no es un .w3db (falta la firma W3DB)"; return false; }
    t.version = LeeU32(d + 4);
    if (t.version > W3DB_VERSION) {
        moreNew = true;
        reason = "este .w3db es de una version del formato mas nueva; no lo puedo leer";
        return false;
    }
    const unsigned nb = LeeU32(d + 12);
    if (nb > (n - 20) / 16) { reason = "la cabecera declara mas bloques de los que entran en el archivo"; return false; }
    t.blocks.clear();
    t.blocks.reserve(nb);
    size_t off = 20;
    for (unsigned i = 0; i < nb; i++) {
        if (n - off < 16) { reason = "archivo cortado en la cabecera de un bloque"; return false; }
        Block b;
        b.id.assign((const char*)d + off, 4);
        b.domain = LeeU32(d + off + 4);
        b.count  = LeeU32(d + off + 8);
        b.bytes  = LeeU32(d + off + 12);
        if (b.bytes > n - off - 16) { reason = "el bloque " + b.id + " dice tener mas bytes de los que hay (archivo cortado)"; return false; }
        b.data = d + off + 16;
        // un bloque CONOCIDO repetido es un archivo roto (no hay forma de saber cual vale)
        if (W3dMeshBinBlockKnown(b.id))
            for (size_t k = 0; k < t.blocks.size(); k++)
                if (t.blocks[k].id == b.id) { reason = "el bloque " + b.id + " esta repetido"; return false; }
        t.blocks.push_back(b);
        size_t next = off + 16 + (size_t)b.bytes;
        next = (next + 3) & ~(size_t)3;
        off = (next > n) ? n : next;   // el relleno del ULTIMO bloque puede faltar sin romper nada
    }
    return true;
}

const Block* Find(const Table& t, const char* id) {
    for (size_t i = 0; i < t.blocks.size(); i++) if (t.blocks[i].id == id) return &t.blocks[i];
    return 0;
}

// ===========================================================================
//  INFO
// ===========================================================================
struct InfoBin {
    unsigned nRV, nIdx, nParts, nFaces, nCorners;
    int nPoints, nEdges;
    unsigned flags;
    int primType; float primSize, primSize2, primHeight; int primVerts, primVerts2;
    float aabbMin[3], aabbMax[3], center[3], radius;
    int groupedVerts;
    int uvActive, activeCol, activeGroup, uvActiveGroup;
    std::string name;
    InfoBin() : nRV(0), nIdx(0), nParts(0), nFaces(0), nCorners(0), nPoints(-1), nEdges(-1), flags(0),
                primType(-1), primSize(2.0f), primSize2(0.0f), primHeight(0.0f), primVerts(8), primVerts2(8),
                radius(0.0f), groupedVerts(0), uvActive(-1), activeCol(-1), activeGroup(-1), uvActiveGroup(-1) {
        for (int k = 0; k < 3; k++) { aabbMin[k] = aabbMax[k] = center[k] = 0.0f; }
    }
};

void WriteInfo(std::string& s, const InfoBin& I) {
    SetU32(s, I.nRV); SetU32(s, I.nIdx); SetU32(s, I.nParts); SetU32(s, I.nFaces); SetU32(s, I.nCorners);
    SetI32(s, I.nPoints); SetI32(s, I.nEdges);
    SetU32(s, I.flags);
    SetI32(s, I.primType); SetF32(s, I.primSize); SetF32(s, I.primSize2); SetF32(s, I.primHeight);
    SetI32(s, I.primVerts); SetI32(s, I.primVerts2);
    for (int k = 0; k < 3; k++) SetF32(s, I.aabbMin[k]);
    for (int k = 0; k < 3; k++) SetF32(s, I.aabbMax[k]);
    for (int k = 0; k < 3; k++) SetF32(s, I.center[k]);
    SetF32(s, I.radius);
    SetI32(s, I.groupedVerts);
    SetI32(s, I.uvActive); SetI32(s, I.activeCol); SetI32(s, I.activeGroup); SetI32(s, I.uvActiveGroup);
    SetStr(s, I.name);
    // CAMPOS NUEVOS: SOLO al final (un lector viejo lee lo que conoce e ignora el resto)
}

bool ReadInfo(const Block& b, InfoBin& I) {
    Cursor c(b.data, b.bytes);
    I.nRV = c.U32(); I.nIdx = c.U32(); I.nParts = c.U32(); I.nFaces = c.U32(); I.nCorners = c.U32();
    I.nPoints = c.I32(); I.nEdges = c.I32();
    I.flags    = c.U32();
    I.primType = c.I32(); I.primSize = c.F32(); I.primSize2 = c.F32(); I.primHeight = c.F32();
    I.primVerts = c.I32(); I.primVerts2 = c.I32();
    for (int k = 0; k < 3; k++) I.aabbMin[k] = c.F32();
    for (int k = 0; k < 3; k++) I.aabbMax[k] = c.F32();
    for (int k = 0; k < 3; k++) I.center[k] = c.F32();
    I.radius = c.F32();
    I.groupedVerts = c.I32();
    I.uvActive = c.I32(); I.activeCol = c.I32(); I.activeGroup = c.I32(); I.uvActiveGroup = c.I32();
    c.Str(I.name);
    return c.ok;   // lo que sigue (campos de una version mas nueva) se ignora a proposito
}

// ===========================================================================
//  LA VISTA VALIDADA: punteros ADENTRO del buffer, ya chequeados. Primero se
//  valida TODO y recien despues se toca la malla: un archivo roto no deja la
//  malla a medio llenar.
// ===========================================================================
struct GroupView {           // grupo / forma: n indices + n (o 3n) floats
    std::string name;
    const unsigned char* idx;
    const unsigned char* val;
    unsigned n;
};
struct LayerView {
    std::string name;
    unsigned flags;
    const unsigned char* data;
};
struct View {
    InfoBin I;
    std::vector<std::string> req;
    const unsigned char* pos; const unsigned char* nrm; const unsigned char* uv; const unsigned char* col;
    const unsigned char* idx; bool idx32;
    const unsigned char* ctrl;
    const unsigned char* uvRest;
    std::vector<const unsigned char*> uvx;   // RUVX: por capa UV, sus 8*nRV bytes o 0 (ausente = la activa)
    std::vector<W3dMeshBinPart> parts;
    std::vector<GroupView> groups, uvGroups, shapes;
    // edicion
    bool withEdit;
    const unsigned char* faceSides; const unsigned char* faceMat; const unsigned char* faceSmooth; const unsigned char* faceIdx;
    bool hasFace;
    std::vector<int> looseE, looseV;
    const unsigned char* cnrm;
    std::vector<LayerView> uvly, coly;
    const unsigned char* shrp; unsigned nShrp;
    const unsigned char* seam; unsigned nSeam;
    const unsigned char* edge; unsigned nEdge; bool hasEdge;
    bool hasForeign;
    std::vector<std::string> foreignReq;
    std::vector<W3dForeignBlock> foreignBlocks;
    // bloques binarios que esta version NO entiende (se preservan)
    std::vector<W3dForeignBlock> unknown;
    View() : pos(0), nrm(0), uv(0), col(0), idx(0), idx32(false), ctrl(0), uvRest(0), withEdit(false),
              faceSides(0), faceMat(0), faceSmooth(0), faceIdx(0), hasFace(false), cnrm(0),
              shrp(0), nShrp(0), seam(0), nSeam(0), edge(0), nEdge(0), hasEdge(false), hasForeign(false) {}
};

// un indice de render-vert fuera de rango en un bloque = archivo roto
bool IndicesInRange(const unsigned char* p, unsigned n, unsigned nRV) {
    for (unsigned i = 0; i < n; i++) {
        const int v = LeeI32(p + (size_t)i * 4);
        if (v < 0 || (unsigned)v >= nRV) return false;
    }
    return true;
}

// un bloque de GRUPOS (RVGR / RUVG: n indices + n pesos; RSHP: n indices + 3n floats)
bool ReadGroups(const Block& b, unsigned nRV, unsigned floatsByIdx, std::vector<GroupView>& out) {
    Cursor c(b.data, b.bytes);
    out.clear();
    for (unsigned g = 0; g < b.count; g++) {
        GroupView v;
        if (!c.Str(v.name)) return false;
        v.n = c.U32();
        v.idx = c.Take(v.n, 4);
        v.val = c.Take(v.n, 4 * floatsByIdx);
        if (!c.ok) return false;
        if (!IndicesInRange(v.idx, v.n, nRV)) return false;
        out.push_back(v);
    }
    return true;
}

// dominio sin las banderas
unsigned Dom(const Block& b) { return b.domain & 255u; }

bool Validate(const unsigned char* d, size_t n, View& v, bool edit, W3dMeshInfo* info, std::string& reason) {
    Table t;
    bool moreNew = false;
    if (!ReadTable(d, n, t, reason, moreNew)) return false;
    const Block* bi = Find(t, "INFO");
    if (!bi || !ReadInfo(*bi, v.I)) { reason = "falta el bloque INFO o esta cortado"; return false; }
    const InfoBin& I = v.I;
    const unsigned nRV = I.nRV;
    if (I.nIdx % 3 != 0) { reason = "el index buffer no es de triangulos"; return false; }
    if (I.nParts < 1) { reason = "la malla no trae ningun mesh part"; return false; }
    // TOPES: ninguna cantidad de INFO puede hacer desbordar un calculo de bytes en 32 bits
    // (Symbian). Un archivo que declara mas que esto no es una malla: es un archivo roto.
    if (nRV > 0x0FFFFFFFu || I.nIdx > 0x3FFFFFFFu || I.nCorners > 0x0FFFFFFFu ||
        I.nFaces > 0x0FFFFFFFu || I.nParts > 0x0FFFFFFFu) { reason = "INFO declara cantidades imposibles"; return false; }

    // ---- REQ: lo que un lector DEBE entender ----
    if (const Block* b = Find(t, "REQ ")) {
        Cursor c(b->data, b->bytes);
        const unsigned char* p = c.Take(b->count, 4);
        if (!c.ok) { reason = "bloque REQ cortado"; return false; }
        for (unsigned i = 0; i < b->count; i++) {
            std::string f((const char*)p + (size_t)i * 4, 4);
            v.req.push_back(f);
            if (!W3dMeshBinBlockKnown(f) && info) {
                info->readOnly = true;
                info->notices.push_back("el archivo REQUIERE el bloque '" + f +
                                       "', que esta version no entiende: lo abro en SOLO LECTURA");
            }
        }
    }

    // ---- RENDER ----
    if (nRV > 0) {
        const Block* b = Find(t, "RPOS");
        if (!b || b->count != nRV || b->bytes != nRV * 12u) { reason = "falta el bloque RPOS o no mide lo que dice INFO"; return false; }
        v.pos = b->data;
    }
    if (const Block* b = Find(t, "RNRM")) {
        if (b->count != nRV || b->bytes != nRV * 3u) { reason = "RNRM no mide lo que dice INFO"; return false; }
        v.nrm = b->data;
    }
    if (const Block* b = Find(t, "RUV ")) {
        if (b->count != nRV || b->bytes != nRV * 8u) { reason = "RUV no mide lo que dice INFO"; return false; }
        v.uv = b->data;
    }
    if (const Block* b = Find(t, "RCOL")) {
        if (b->count != nRV || b->bytes != nRV * 4u) { reason = "RCOL no mide lo que dice INFO"; return false; }
        v.col = b->data;
    }
    // RUVX: las capas UV extra por render-vert (cantidad = capas UV; cada una: u32 presente + f32 x2 x nRV)
    if (const Block* b = Find(t, "RUVX")) {
        Cursor c(b->data, b->bytes);
        for (unsigned k = 0; k < b->count && k < 64u; k++) {
            const unsigned present = c.U32();
            const unsigned char* d = present ? c.Take(nRV, 8) : 0;
            if (!c.ok) { reason = "RUVX esta cortado"; return false; }
            v.uvx.push_back(d);
        }
    }
    if (I.nIdx > 0) {
        const Block* b = Find(t, "RIDX");
        if (!b) { reason = "falta el bloque RIDX"; return false; }
        v.idx32 = (b->domain & W3DB_IDX32) != 0;
        const unsigned size = v.idx32 ? 4u : 2u;
        if (b->count * 3u != I.nIdx || I.nIdx > 0x3FFFFFFFu || b->bytes != I.nIdx * size) { reason = "RIDX no mide lo que dice INFO"; return false; }
        if (v.idx32 && sizeof(MeshIndex) < 4) { reason = "la malla tiene mas de 65535 vertices y esta plataforma usa indices de 16 bits"; return false; }
        v.idx = b->data;
        for (unsigned i = 0; i < I.nIdx; i++) {
            const unsigned x = v.idx32 ? LeeU32(v.idx + (size_t)i * 4) : LeeU16(v.idx + (size_t)i * 2);
            if (x >= nRV) { reason = "RIDX tiene un indice fuera de rango"; return false; }
        }
    }
    {
        const Block* b = Find(t, "PART");
        if (!b || b->count != I.nParts) { reason = "falta el bloque PART o no mide lo que dice INFO"; return false; }
        Cursor c(b->data, b->bytes);
        for (unsigned k = 0; k < b->count; k++) {
            W3dMeshBinPart pa;
            c.Str(pa.name); c.Str(pa.material);
            pa.start = c.I32(); pa.count = c.I32();
            if (!c.ok || pa.start < 0 || pa.count < 0 || (unsigned)pa.start > I.nIdx ||
                (unsigned)pa.count > I.nIdx - (unsigned)pa.start) { reason = "PART esta cortado o sale del index buffer"; return false; }
            v.parts.push_back(pa);
        }
    }
    if (const Block* b = Find(t, "RCTL")) {
        if (b->count != nRV || b->bytes != nRV * 4u || !IndicesInRange(b->data, nRV, nRV)) { reason = "RCTL no mide lo que dice INFO o sale de rango"; return false; }
        v.ctrl = b->data;
    }
    if (const Block* b = Find(t, "RVGR")) if (!ReadGroups(*b, nRV, 1, v.groups))   { reason = "RVGR esta cortado o sale de rango"; return false; }
    if (const Block* b = Find(t, "RUVG")) if (!ReadGroups(*b, nRV, 1, v.uvGroups)) { reason = "RUVG esta cortado o sale de rango"; return false; }
    if (const Block* b = Find(t, "RSHP")) if (!ReadGroups(*b, nRV, 3, v.shapes))   { reason = "RSHP esta cortado o sale de rango"; return false; }
    if (const Block* b = Find(t, "RUVR")) {
        if (b->count != nRV || b->bytes != nRV * 8u) { reason = "RUVR no mide lo que dice INFO"; return false; }
        v.uvRest = b->data;
    }

    // ---- EDICION (el juego los saltea sin mirarlos) ----
    v.withEdit = edit;
    if (edit) {
        const unsigned nC = I.nCorners;
        if (const Block* b = Find(t, "FACE")) {
            Cursor c(b->data, b->bytes);
            const unsigned nF = b->count;
            const unsigned nEsq = c.U32();
            v.faceSides  = c.Take(nF, 4);
            v.faceMat    = c.Take(nF, 4);
            v.faceSmooth = c.Take(nF, 1);
            c.Take((4 - (nF % 4)) % 4, 1);   // relleno del smooth a 4
            v.faceIdx    = c.Take(nEsq, 4);
            if (!c.ok || nF != I.nFaces || nEsq != nC) { reason = "FACE esta cortado o no mide lo que dice INFO"; return false; }
            unsigned sum = 0;
            for (unsigned f = 0; f < nF; f++) {
                const int sides = LeeI32(v.faceSides + (size_t)f * 4);
                const int mat = LeeI32(v.faceMat + (size_t)f * 4);
                const signed char sm = (signed char)v.faceSmooth[f];
                if (sides < 3 || (unsigned)sides > nEsq - sum || mat < 0 || (unsigned)mat >= I.nParts || sm < -1 || sm > 1) {
                    reason = "FACE tiene una cara invalida"; return false;
                }
                sum += (unsigned)sides;
            }
            if (sum != nEsq || !IndicesInRange(v.faceIdx, nEsq, nRV)) { reason = "FACE tiene indices fuera de rango"; return false; }
            v.hasFace = true;
        } else if (I.nFaces != 0) { reason = "falta el bloque FACE"; return false; }
        if (const Block* b = Find(t, "LOOS")) {
            Cursor c(b->data, b->bytes);
            const unsigned nE = c.U32();
            const unsigned char* pe = c.Take(nE, 4);
            const unsigned nVs = c.U32();
            const unsigned char* pv = c.Take(nVs, 4);
            if (!c.ok || (nE % 2) != 0 || !IndicesInRange(pe, nE, nRV) || !IndicesInRange(pv, nVs, nRV)) {
                reason = "LOOS esta cortado o sale de rango"; return false;
            }
            v.looseE.resize(nE); v.looseV.resize(nVs);
            if (nE) CopyI32(pe, &v.looseE[0], nE);
            if (nVs) CopyI32(pv, &v.looseV[0], nVs);
        }
        if (const Block* b = Find(t, "CNRM")) {
            if (b->count != nC || b->bytes != nC * 3u) { reason = "CNRM no mide lo que dice INFO"; return false; }
            v.cnrm = b->data;
        }
        if (const Block* b = Find(t, "UVLY")) {
            Cursor c(b->data, b->bytes);
            for (unsigned k = 0; k < b->count; k++) {
                LayerView cv; cv.flags = 0;
                c.Str(cv.name);
                cv.data = c.Take(nC, 8);
                if (!c.ok) { reason = "UVLY esta cortado"; return false; }
                v.uvly.push_back(cv);
            }
        }
        if (const Block* b = Find(t, "COLY")) {
            Cursor c(b->data, b->bytes);
            for (unsigned k = 0; k < b->count; k++) {
                LayerView cv;
                c.Str(cv.name);
                cv.flags = c.U32();
                cv.data = c.Take(nC, 4);
                if (!c.ok) { reason = "COLY esta cortado"; return false; }
                v.coly.push_back(cv);
            }
        }
        if (const Block* b = Find(t, "SHRP")) {
            if (b->bytes % 24u != 0 || b->count != b->bytes / 24u) { reason = "SHRP no mide lo que dice"; return false; }
            v.shrp = b->data; v.nShrp = b->count;
        }
        if (const Block* b = Find(t, "SEAM")) {
            if (b->bytes % 24u != 0 || b->count != b->bytes / 24u) { reason = "SEAM no mide lo que dice"; return false; }
            v.seam = b->data; v.nSeam = b->count;
        }
        if (const Block* b = Find(t, "EDGE")) {
            if (b->count > 0x1FFFFFFFu || b->bytes != b->count * 8u || !IndicesInRange(b->data, b->count * 2u, nRV)) {
                reason = "EDGE no mide lo que dice o sale de rango"; return false;
            }
            v.edge = b->data; v.nEdge = b->count; v.hasEdge = true;
        }
        if (const Block* b = Find(t, "AJEN")) {
            Cursor c(b->data, b->bytes);
            const unsigned nReq = c.U32();
            for (unsigned i = 0; i < nReq && c.ok; i++) { std::string r; c.Str(r); v.foreignReq.push_back(r); }
            const unsigned nBl = c.U32();
            for (unsigned i = 0; i < nBl && c.ok; i++) {
                W3dForeignBlock settings;
                c.Str(settings.name);
                settings.domain = c.I32();
                settings.count = c.I32();
                c.Str(settings.text);
                v.foreignBlocks.push_back(settings);
            }
            if (!c.ok) { reason = "AJEN esta cortado"; return false; }
            v.hasForeign = true;
        }
    }

    // ---- lo que esta version NO entiende: se preserva tal cual ----
    for (size_t i = 0; i < t.blocks.size(); i++) {
        const Block& b = t.blocks[i];
        if (W3dMeshBinBlockKnown(b.id)) continue;
        W3dForeignBlock settings;
        settings.name = b.id;
        settings.domain = (int)b.domain;
        settings.count = (int)b.count;
        settings.text.assign((const char*)b.data, b.bytes);
        v.unknown.push_back(settings);
    }
    return true;
}

// ---- piezas de la carga (las usan Leer y MaterializarEdicion) ----

void FreeLayersUVCol(Mesh* m) {
    for (size_t i = 0; i < m->uvMaps.size(); i++)      delete m->uvMaps[i];
    for (size_t i = 0; i < m->colorLayers.size(); i++) delete m->colorLayers[i];
    m->uvMaps.clear();      m->uvActiveMap = -1;
    m->colorLayers.clear(); m->activeColor = -1;
}
void FreeGroups(Mesh* m) {
    for (size_t i = 0; i < m->vertexGroups.size(); i++) delete m->vertexGroups[i];
    for (size_t i = 0; i < m->uvGroups.size(); i++)     delete m->uvGroups[i];
    m->vertexGroups.clear(); m->activeGroup = -1;
    m->uvGroups.clear();     m->uvActiveGroup = -1;
}

// el activo de una lista de n elementos (misma regla que el lector de texto:
// -1 vale; fuera de rango cae al 0 si hay alguno)
int ActiveValid(int a, int n) {
    if (a >= -1 && a < n) return a;
    return n > 0 ? 0 : -1;
}

// ARISTAS UNICAS por REPRESENTANTE: exactamente lo que arma ComputeBorders con posRep
// (pares ordenados sin repetir, en el orden del std::set). Solo se usa cuando el
// archivo no las trae (el proyecto pide editable y la malla se guardo como escenario).
void EdgesByRep(const std::vector<MeshFace>& faces, const std::vector<int>& loose, const int* rep, int nV,
                   std::vector<int>& edges) {
    edges.clear();
    std::set<std::pair<int, int> > unique;
    for (size_t f = 0; f < faces.size(); f++) {
        const std::vector<int>& idx = faces[f].idx;
        const int m = (int)idx.size();
        for (int k = 0; k < m; k++) {
            int a = idx[(size_t)k], b = idx[(size_t)((k + 1) % m)];
            if (a < 0 || a >= nV || b < 0 || b >= nV) continue;
            int ra = rep[a], rb = rep[b];
            if (ra == rb) continue;
            if (ra > rb) { int t2 = ra; ra = rb; rb = t2; }
            unique.insert(std::make_pair(ra, rb));
        }
    }
    for (size_t e = 0; e + 1 < loose.size(); e += 2) {
        int a = loose[e], b = loose[e + 1];
        if (a < 0 || a >= nV || b < 0 || b >= nV) continue;
        int ra = rep[a], rb = rep[b];
        if (ra == rb) continue;
        if (ra > rb) { int t2 = ra; ra = rb; rb = t2; }
        unique.insert(std::make_pair(ra, rb));
    }
    edges.reserve(unique.size() * 2);
    for (std::set<std::pair<int, int> >::const_iterator it = unique.begin(); it != unique.end(); ++it) {
        edges.push_back(it->first);
        edges.push_back(it->second);
    }
}

// el buffer de lineas del contorno, LINEAL desde las aristas (lo que hacia ComputeBorders)
void BuildBordersBuf(Mesh* m) {
    m->bordersBuf.clear();
    if (!m->vertex) return;
    m->bordersBuf.reserve(m->edges.size() * 3);
    for (size_t e = 0; e + 1 < m->edges.size(); e += 2) {
        const int a = m->edges[e], b = m->edges[e + 1];
        m->bordersBuf.push_back(m->vertex[a * 3]); m->bordersBuf.push_back(m->vertex[a * 3 + 1]); m->bordersBuf.push_back(m->vertex[a * 3 + 2]);
        m->bordersBuf.push_back(m->vertex[b * 3]); m->bordersBuf.push_back(m->vertex[b * 3 + 1]); m->bordersBuf.push_back(m->vertex[b * 3 + 2]);
    }
}

// los bloques de EDICION de la vista a la malla (caras, sueltos, capas, marcas, aristas,
// posRep y la preservacion). Requiere que los arrays de render ya esten en la malla.
void ApplyEdit(const View& v, Mesh* m, bool noEditable) {
    const InfoBin& I = v.I;
    const unsigned nC = I.nCorners;
    const int nV = (int)I.nRV;
    // caras
    m->faces3d.clear();
    if (v.hasFace) {
        m->faces3d.resize(I.nFaces);
        size_t off = 0;
        for (unsigned f = 0; f < I.nFaces; f++) {
            MeshFace& F = m->faces3d[f];
            const int sides = LeeI32(v.faceSides + (size_t)f * 4);
            F.idx.resize((size_t)sides);
            CopyI32(v.faceIdx + off * 4, &F.idx[0], (size_t)sides);
            off += (size_t)sides;
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
    FreeLayersUVCol(m);
    for (size_t l = 0; l < v.uvly.size(); l++) {
        UVMap* u = new UVMap(v.uvly[l].name);
        u->uv.resize((size_t)nC * 2);
        if (nC) CopyF32(v.uvly[l].data, &u->uv[0], (size_t)nC * 2);
        m->uvMaps.push_back(u);
    }
    m->uvActiveMap = ActiveValid(I.uvActive, (int)m->uvMaps.size());
    for (size_t l = 0; l < v.coly.size(); l++) {
        ColorLayer* c = new ColorLayer(v.coly[l].name);
        c->perVertex  = (v.coly[l].flags & COLY_BY_VERTEX) != 0;
        c->color.resize((size_t)nC * 4);
        if (nC) memcpy(&c->color[0], v.coly[l].data, (size_t)nC * 4);
        m->colorLayers.push_back(c);
    }
    m->activeColor = ActiveValid(I.activeCol, (int)m->colorLayers.size());
    // marcas: cada clave de 24 bytes se rearma como la arma ESTE host (en uno little-endian
    // salen los mismos bytes del archivo)
    m->sharpEdges.clear();
    m->seamEdges.clear();
    for (unsigned i = 0; i < v.nShrp; i++) m->sharpEdges.insert(KeyMarkMemory(v.shrp + (size_t)i * 24));
    for (unsigned i = 0; i < v.nSeam; i++) m->seamEdges.insert(KeyMarkMemory(v.seam + (size_t)i * 24));
    // identidad de puntos + aristas
    if (noEditable) {
        m->posRep.clear();
        m->edges.clear();
        m->bordersBuf.clear();
    } else {
        m->posRep = m->vertCtrlPoint;
        W3dMeshJoinByPosition(m);   // (la costura y los polos de antes: W3dMesh.cpp)
        if (v.hasEdge) {
            m->edges.resize((size_t)v.nEdge * 2);
            if (v.nEdge) CopyI32(v.edge, &m->edges[0], (size_t)v.nEdge * 2);
        } else if (nV > 0) {
            // se guardo como escenario cerrado y el proyecto la pide editable: se arman
            EdgesByRep(m->faces3d, m->looseEdges, &m->posRep[0], nV, m->edges);
        } else m->edges.clear();
        BuildBordersBuf(m);
    }
    // preservacion: el "requiere" del texto (sin AJEN = el default del escritor)
    m->w3dmForeign.blocks = v.foreignBlocks;
    if (v.hasForeign) m->w3dmForeign.requiresValue = v.foreignReq;
    else { m->w3dmForeign.requiresValue.clear(); m->w3dmForeign.requiresValue.push_back("V"); m->w3dmForeign.requiresValue.push_back("F"); }
}

// sello + bloques binarios que no se entienden (van con la malla aunque sea el juego)
void ApplyStamp(const View& v, Mesh* m) {
    m->w3dmForeign.nPoints     = v.I.nPoints;
    m->w3dmForeign.nCorners    = (int)v.I.nCorners;
    m->w3dmForeign.nFaces      = (int)v.I.nFaces;
    m->w3dmForeign.nEdges      = v.I.nEdges;
    m->w3dmForeign.nRender     = (int)v.I.nRV;
    m->w3dmForeign.nTriangles  = (int)(v.I.nIdx / 3);
    m->w3dmForeign.blocksBin   = v.unknown;
    m->w3dmForeign.requiresBin = v.req;
    m->w3dmForeign.truncated   = false;
    m->w3dmForeign.notLoaded   = false;
}

void NoticeReason(W3dMeshInfo* info, const std::string& reason) {
    if (!info) return;
    info->notices.push_back(reason);
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
//  (que ya agrupa con W3dMeshComputePoints) -> LOSSLESS: la pose resultante es identica
//  (los render-verts de un punto comparten posicion Y pesos Y deltas de forma por firma, asi
//  que su skinning es el mismo). El render (triangulos/UV) no se toca: usa los arrays por
//  render-vert. Si algun merge diera distinto peso/delta (no deberia, la firma lo separa),
//  se ABORTA y la malla queda como estaba.
// ============================================================================
void W3dRegroupPointsSkin(Mesh* m) {
    if (!m || m->vertexSize <= 0) return;
    if (m->vertexGroups.empty() && m->shapeKeys.empty()) return;   // sin skinning no vale la pena
    const int nV = m->vertexSize;
    if ((int)m->vertCtrlPoint.size() != nV) return;
    W3dMeshPoints P;
    W3dMeshComputePoints(m, P);
    if (P.nP <= 0 || (int)P.rvToPoint.size() != nV) return;
    const std::vector<int>& aPt = P.rvToPoint;   // render-vert -> punto nuevo
    // nCtrl VIEJO (max vertCtrlPoint+1) y mapeo CP-viejo -> punto-nuevo. Los grupos/formas indexan por
    // CP viejo (no por render-vert): como TODOS los render-verts de un CP comparten posicion+pesos, todos
    // caen en el MISMO punto nuevo -> basta tomar el de cualquiera. Asi funciona desde un ctrl identidad
    // O casi-identidad (el .w3db trae 10151 CP de 10170: ya agrupa unos pocos, pero no los 6x reales).
    int nOldCp = 0;
    for (int ri = 0; ri < nV; ri++){ int c = m->vertCtrlPoint[(size_t)ri]; if (c + 1 > nOldCp) nOldCp = c + 1; }
    if (nOldCp <= 0 || P.nP >= nOldCp) return;   // ya tan agrupado como se puede: no hay nada que ganar
    std::vector<int> cpAPt((size_t)nOldCp, -1);
    for (int ri = 0; ri < nV; ri++){ int c = m->vertCtrlPoint[(size_t)ri]; if (c >= 0 && c < nOldCp && cpAPt[(size_t)c] < 0) cpAPt[(size_t)c] = aPt[(size_t)ri]; }
    // --- remapear grupos a temporales (dedup por punto nuevo); abortar si hay conflicto de peso ---
    std::vector<std::vector<int> >   gV(m->vertexGroups.size());
    std::vector<std::vector<float> > gW(m->vertexGroups.size());
    for (size_t g = 0; g < m->vertexGroups.size(); g++) {
        VertexGroup* vg = m->vertexGroups[g]; if (!vg) continue;
        std::map<int,float> acc;
        for (size_t k = 0; k < vg->verts.size() && k < vg->weights.size(); k++) {
            int old = vg->verts[k]; if (old < 0 || old >= nOldCp) continue;    // fuera de rango: entrada huerfana (el skinning ya la ignora)
            int np = cpAPt[(size_t)old]; if (np < 0 || np >= P.nP) continue;   // CP sin render-vert: peso huerfano -> saltear
            float w = vg->weights[k];
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
            int old = sk.idx[j]; if (old < 0 || old >= nOldCp) continue;   // huerfana: saltear
            int np = cpAPt[(size_t)old]; if (np < 0 || np >= P.nP) continue;      // CP sin render-vert: saltear
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
    for (size_t g = 0; g < m->vertexGroups.size(); g++) { VertexGroup* vg = m->vertexGroups[g]; if (!vg) continue; vg->verts.swap(gV[g]); vg->weights.swap(gW[g]); }
    for (size_t s = 0; s < m->shapeKeys.size(); s++) { m->shapeKeys[s].idx.swap(kI[s]); m->shapeKeys[s].d.swap(kD[s]); }
}

// ============================================================================
//                               L E C T U R A
// ============================================================================

bool W3dMeshBinRead(const unsigned char* d, size_t n, Mesh* m, W3dMeshInfo* info, const W3dMeshBinOptions& op) {
    if (!m || !d) return false;
    View v;
    std::string reason;
    if (!Validate(d, n, v, op.edit, info, reason)) {
        NoticeReason(info, reason + ": NO toco la malla que ya estaba");
        if (info) info->truncated = true;
        return false;
    }
    const InfoBin& I = v.I;
    const unsigned nRV = I.nRV;

    // ---- ARRAYS DE RENDER: memcpy desde el archivo ----
    // lo compartido (MeshData) se suelta, no se borra: no es de esta malla
    m->ReleaseDataShared();
    delete[] m->vertex;      m->vertex = 0;
    delete[] m->normals;     m->normals = 0;
    delete[] m->uv;          m->uv = 0;
    delete[] m->vertexColor; m->vertexColor = 0;
    m->vertexSize = (int)nRV;
    // ESTADO DE POSE: el .w3db, igual que el .w3dm, guarda la malla EN REPOSO
    m->posedByAnim = false;
    if (nRV > 0) {
        m->vertex      = new GLfloat[(size_t)nRV * 3];
        m->normals     = new GLbyte[(size_t)nRV * 3];
        m->uv          = new GLfloat[(size_t)nRV * 2];
        m->vertexColor = new GLubyte[(size_t)nRV * 4];
        CopyF32(v.pos, m->vertex, (size_t)nRV * 3);
        if (v.nrm) memcpy(m->normals, v.nrm, (size_t)nRV * 3);
        else for (unsigned i = 0; i < nRV; i++) { m->normals[i*3] = 0; m->normals[i*3+1] = 127; m->normals[i*3+2] = 0; }
        if (v.uv) CopyF32(v.uv, m->uv, (size_t)nRV * 2);
        else memset(m->uv, 0, (size_t)nRV * 2 * sizeof(GLfloat));
        if (v.col) memcpy(m->vertexColor, v.col, (size_t)nRV * 4);
        else memset(m->vertexColor, 255, (size_t)nRV * 4);
    }
    // ---- RUVX: capas UV extra por render-vert (las capas de textura con UV propio) ----
    m->uvExtra.clear();
    m->uvExtra.resize(v.uvx.size());
    for (size_t k = 0; k < v.uvx.size(); k++)
        if (v.uvx[k] && nRV > 0) { m->uvExtra[k].resize((size_t)nRV * 2); CopyF32(v.uvx[k], &m->uvExtra[k][0], (size_t)nRV * 2); }
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
    if (!I.name.empty()) m->name = I.name;
    m->meshSmooth = (I.flags & INFO_SMOOTH) != 0;
    if (I.flags & INFO_PRIM) {
        m->meshType = I.primType; m->meshSize = I.primSize; m->meshSize2 = I.primSize2;
        m->meshDepth = I.primHeight; m->meshVerts = I.primVerts; m->meshVerts2 = I.primVerts2;
    } else m->meshType = -1;
    // ---- mesh parts (el MATERIAL lo resuelve el que llama, por nombre) ----
    m->materialsGroup.clear();
    if (info) info->materials.clear();
    for (size_t k = 0; k < v.parts.size(); k++) {
        MaterialGroup mg;
        mg.name = v.parts[k].name;
        mg.startDrawn = v.parts[k].start;
        mg.indicesDrawnCount = v.parts[k].count;
        m->materialsGroup.push_back(mg);
        if (info) info->materials.push_back(v.parts[k].material);
    }
    // ---- IDENTIDAD DE LOS PUNTOS (vertCtrlPoint; posRep sale de aca) ----
    m->vertCtrlPoint.resize(nRV);
    if (nRV > 0) {
        if (v.ctrl) CopyI32(v.ctrl, &m->vertCtrlPoint[0], nRV);
        else for (unsigned i = 0; i < nRV; i++) m->vertCtrlPoint[i] = (int)i;
    }
    // ---- grupos, formas, UV groups, reposo 2D ----
    FreeGroups(m);
    for (size_t g = 0; g < v.groups.size(); g++) {
        VertexGroup* vg = new VertexGroup(v.groups[g].name);
        vg->verts.resize(v.groups[g].n); vg->weights.resize(v.groups[g].n);
        if (v.groups[g].n) { CopyI32(v.groups[g].idx, &vg->verts[0], v.groups[g].n); CopyF32(v.groups[g].val, &vg->weights[0], v.groups[g].n); }
        m->vertexGroups.push_back(vg);
    }
    m->activeGroup = ActiveValid(I.activeGroup, (int)m->vertexGroups.size());
    m->shapeKeys.clear();
    for (size_t k = 0; k < v.shapes.size(); k++) {
        W3dShapeKey sk;
        sk.name = v.shapes[k].name;
        sk.idx.resize(v.shapes[k].n); sk.d.resize((size_t)v.shapes[k].n * 3);
        if (v.shapes[k].n) { CopyI32(v.shapes[k].idx, &sk.idx[0], v.shapes[k].n); CopyF32(v.shapes[k].val, &sk.d[0], (size_t)v.shapes[k].n * 3); }
        m->shapeKeys.push_back(sk);
    }
    m->shapeWeights.assign(m->shapeKeys.size(), 0.0f);
    for (size_t g = 0; g < v.uvGroups.size(); g++) {
        UVGroup* ug = new UVGroup(v.uvGroups[g].name);
        ug->verts.resize(v.uvGroups[g].n); ug->weights.resize(v.uvGroups[g].n);
        if (v.uvGroups[g].n) { CopyI32(v.uvGroups[g].idx, &ug->verts[0], v.uvGroups[g].n); CopyF32(v.uvGroups[g].val, &ug->weights[0], v.uvGroups[g].n); }
        m->uvGroups.push_back(ug);
    }
    m->uvActiveGroup = ActiveValid(I.uvActiveGroup, (int)m->uvGroups.size());
    m->uv2dRest.clear();
    if (v.uvRest && nRV > 0) { m->uv2dRest.resize((size_t)nRV * 2); CopyF32(v.uvRest, &m->uv2dRest[0], (size_t)nRV * 2); }

    // (El agrupamiento de los puntos de skinning NO se hace aca: viene horneado en el .w3db de una malla
    //  guardada como no-editable. Ver W3dMeshBinWrite / W3dRegroupPointsSkin.)

    // ---- AABB / centro / radio / posiciones unicas: HORNEADOS. Si el proyecto pide
    //      el OTRO modo (se guardo editable y ahora es escenario, o al reves) se
    //      recalculan con el MISMO codigo que ComputeBorders / ComputeAABBOnly ----
    const bool statsNoEdit = (I.flags & INFO_STATS_NOEDIT) != 0;
    if (statsNoEdit == op.noEditable) {
        m->aabbOk = (I.flags & INFO_AABBOK) != 0;
        m->aabbMin = Vector3(I.aabbMin[0], I.aabbMin[1], I.aabbMin[2]);
        m->aabbMax = Vector3(I.aabbMax[0], I.aabbMax[1], I.aabbMax[2]);
        m->centerGeom = Vector3(I.center[0], I.center[1], I.center[2]);
        m->radiusGeom = I.radius;
        m->groupedVerts = op.noEditable ? 0 : I.groupedVerts;
    } else {
        W3dGeomStats st;
        W3dComputeGeomStats(m->vertex, (int)nRV, (op.noEditable || nRV == 0) ? NULL : &m->vertCtrlPoint[0], st);
        m->aabbOk = st.ok;
        if (st.ok) {
            m->aabbMin = st.aabbMin; m->aabbMax = st.aabbMax;
            m->centerGeom = st.center; m->radiusGeom = st.radius;
        }
        m->groupedVerts = op.noEditable ? 0 : st.grouped;
    }

    // ---- EDICION ----
    if (op.edit) ApplyEdit(v, m, op.noEditable);
    else {
        // el juego compilado: nada de edicion en memoria (BuildPolyMesh lo pide si hace falta)
        m->faces3d.clear(); m->looseEdges.clear(); m->looseVerts.clear(); m->cornerNormal.clear();
        FreeLayersUVCol(m);
        m->sharpEdges.clear(); m->seamEdges.clear();
        m->posRep.clear(); m->edges.clear(); m->bordersBuf.clear();
        m->w3dmForeign.blocks.clear(); m->w3dmForeign.requiresValue.clear();
    }
    ApplyStamp(v, m);
    m->uvSelVert.clear();

    // ---- las versiones que dejaba la carga clasica (Reagrupar sube una, ComputeBorders
    //      otra y fija bordersGeomVersion; ApplyLayersToRender invalida el VBO) ----
    m->skinGeomVersion += op.noEditable ? 1 : 2;
    if (!op.noEditable && op.edit) m->bordersGeomVersion = m->skinGeomVersion;
    m->overlayLcache = -1.0f;
    m->vboGeomVer = 0xFFFFFFFFu;
    if (info) { info->rverts = (int)nRV; info->realRverts = (int)nRV; info->truncated = false; }
    return true;
}

bool W3dMeshBinMaterializeEdit(Mesh* m) {
    if (!m || m->pendingEdit.empty()) return false;
    // LA MALLA USA UN RECURSO: la edicion sale de SUS bytes (en memoria si se edito en la
    // sesion, o su entrada del contenedor), no de una entrada propia de la malla
    if (m->mesh) {
        std::string b;
        if (!W3dMeshResourceReadBytes(m->mesh, b) || b.empty()) {
            m->pendingEdit.clear();   // una sola vez: si falla, no se reintenta en cada frame
            return false;
        }
        return W3dMeshBinMaterializeEdit(m, (const unsigned char*)b.data(), b.size());
    }
    const std::string entry = m->pendingEdit;
    std::vector<unsigned char> data;
    if (!w3dFileSystem::ReadFileBytes(entry, data) || data.empty()) {
        m->pendingEdit.clear();   // una sola vez: si falla, no se reintenta en cada frame
        return false;
    }
    return W3dMeshBinMaterializeEdit(m, &data[0], data.size());
}

bool W3dMeshBinMaterializeEdit(Mesh* m, const unsigned char* data, size_t n) {
    if (!m || m->pendingEdit.empty() || !data) return false;
    m->pendingEdit.clear();   // una sola vez: si falla, no se reintenta en cada frame
    View v;
    std::string reason;
    if (!Validate(data, n, v, true, 0, reason)) return false;
    // la entrada tiene que ser ESTA malla (mismos render-verts); si no, no se toca nada
    if ((int)v.I.nRV != m->vertexSize) return false;
    // la IDENTIDAD de los puntos (posRep sale de aca): una malla de un recurso sin grupos ni
    // formas no la carga hasta que hace falta (MeshResource.cpp)
    if ((int)m->vertCtrlPoint.size() != m->vertexSize) {
        m->vertCtrlPoint.resize((size_t)(m->vertexSize > 0 ? m->vertexSize : 0));
        if (m->vertexSize > 0) {
            if (v.ctrl) CopyI32(v.ctrl, &m->vertCtrlPoint[0], (size_t)m->vertexSize);
            else for (int i = 0; i < m->vertexSize; i++) m->vertCtrlPoint[(size_t)i] = i;
        }
    }
    ApplyEdit(v, m, m->noEditable);
    return true;
}

// LOS EXTRAS de la malla de render (identidad de los puntos, grupos, formas, reposo 2D y los
// activos): los comparten LeerRender y LeerExtras
static void FillExtras(const View& v, W3dMeshRender& out) {
    const InfoBin& I = v.I;
    const unsigned nRV = I.nRV;
    out.ctrl.resize(nRV);
    if (nRV > 0) {
        if (v.ctrl) CopyI32(v.ctrl, &out.ctrl[0], nRV);
        else for (unsigned i = 0; i < nRV; i++) out.ctrl[i] = (int)i;
    }
    for (int pass = 0; pass < 2; pass++) {
        const std::vector<GroupView>& src = pass ? v.uvGroups : v.groups;
        std::vector<W3dMeshBinGroup>& dst = pass ? out.uvGroups : out.groups;
        for (size_t g = 0; g < src.size(); g++) {
            W3dMeshBinGroup gr;
            gr.name = src[g].name;
            gr.idx.resize(src[g].n); gr.weights.resize(src[g].n);
            if (src[g].n) { CopyI32(src[g].idx, &gr.idx[0], src[g].n); CopyF32(src[g].val, &gr.weights[0], src[g].n); }
            dst.push_back(gr);
        }
    }
    for (size_t k = 0; k < v.shapes.size(); k++) {
        W3dMeshBinShape fo;
        fo.name = v.shapes[k].name;
        fo.idx.resize(v.shapes[k].n); fo.d.resize((size_t)v.shapes[k].n * 3);
        if (v.shapes[k].n) { CopyI32(v.shapes[k].idx, &fo.idx[0], v.shapes[k].n); CopyF32(v.shapes[k].val, &fo.d[0], (size_t)v.shapes[k].n * 3); }
        out.shapes.push_back(fo);
    }
    if (v.uvRest && nRV > 0) { out.uvRest.resize((size_t)nRV * 2); CopyF32(v.uvRest, &out.uvRest[0], (size_t)nRV * 2); }
    out.activeGroup = ActiveValid(I.activeGroup, (int)out.groups.size());
    out.uvActiveGroup = ActiveValid(I.uvActiveGroup, (int)out.uvGroups.size());
}

bool W3dMeshBinReadExtras(const unsigned char* d, size_t n, W3dMeshRender& out) {
    View v;
    std::string reason;
    if (!Validate(d, n, v, false, 0, reason)) return false;
    out = W3dMeshRender();
    out.name = v.I.name;
    out.nRV = (int)v.I.nRV;
    FillExtras(v, out);
    return true;
}

bool W3dMeshBinReadTopology(const unsigned char* d, size_t n, std::vector<int>& edges,
                              std::vector<MeshFace>* faces) {
    edges.clear();
    if (faces) faces->clear();
    View v;
    std::string reason;
    if (!Validate(d, n, v, true, 0, reason)) return false;
    const InfoBin& I = v.I;
    const int nV = (int)I.nRV;
    // las caras (lo mismo que ApplyEdit deja en faces3d)
    std::vector<MeshFace> own;
    std::vector<MeshFace>& F = faces ? *faces : own;
    const bool makeMissing = faces || !v.hasEdge;   // sin EDGE, las aristas salen de las caras
    if (v.hasFace && makeMissing) {
        F.resize(I.nFaces);
        size_t off = 0;
        for (unsigned f = 0; f < I.nFaces; f++) {
            MeshFace& C = F[f];
            const int sides = LeeI32(v.faceSides + (size_t)f * 4);
            C.idx.resize((size_t)sides);
            CopyI32(v.faceIdx + off * 4, &C.idx[0], (size_t)sides);
            off += (size_t)sides;
            C.mat = LeeI32(v.faceMat + (size_t)f * 4);
            C.smooth = (int)(signed char)v.faceSmooth[f];
        }
    }
    // las aristas: las del archivo (EDGE) o, si no las trae, por representante como el lector
    if (v.hasEdge) {
        edges.resize((size_t)v.nEdge * 2);
        if (v.nEdge) CopyI32(v.edge, &edges[0], (size_t)v.nEdge * 2);
    } else if (nV > 0) {
        std::vector<int> rep((size_t)nV);
        if (v.ctrl) CopyI32(v.ctrl, &rep[0], (size_t)nV);
        else for (int i = 0; i < nV; i++) rep[(size_t)i] = i;
        EdgesByRep(F, v.looseE, &rep[0], nV, edges);
    }
    return true;
}

bool W3dMeshBinReadRender(const unsigned char* d, size_t n, W3dMeshRender& out, W3dMeshInfo* info,
                           W3dForeign* stamp) {
    View v;
    std::string reason;
    if (!Validate(d, n, v, false, info, reason)) { NoticeReason(info, reason); if (info) info->truncated = true; return false; }
    const InfoBin& I = v.I;
    const unsigned nRV = I.nRV;
    out = W3dMeshRender();
    out.name = I.name;
    out.smooth = (I.flags & INFO_SMOOTH) != 0;
    if (I.flags & INFO_PRIM) {
        out.primType   = I.primType; out.primSize = I.primSize; out.primSize2 = I.primSize2;
        out.primHeight = I.primHeight; out.primVerts = I.primVerts; out.primVerts2 = I.primVerts2;
    }
    out.nRV = (int)nRV;
    out.pos.resize((size_t)nRV * 3); out.nrm.resize((size_t)nRV * 3);
    out.uv.resize((size_t)nRV * 2);  out.col.resize((size_t)nRV * 4);
    if (nRV > 0) {
        CopyF32(v.pos, &out.pos[0], (size_t)nRV * 3);
        if (v.nrm) memcpy(&out.nrm[0], v.nrm, (size_t)nRV * 3);
        else for (unsigned i = 0; i < nRV; i++) { out.nrm[i*3] = 0; out.nrm[i*3+1] = 127; out.nrm[i*3+2] = 0; }
        if (v.uv) CopyF32(v.uv, &out.uv[0], (size_t)nRV * 2);
        if (v.col) memcpy(&out.col[0], v.col, (size_t)nRV * 4);
        else std::fill(out.col.begin(), out.col.end(), (unsigned char)255);
    }
    out.uvExtra.clear();
    out.uvExtra.resize(v.uvx.size());
    for (size_t k = 0; k < v.uvx.size(); k++)
        if (v.uvx[k] && nRV > 0) { out.uvExtra[k].resize((size_t)nRV * 2); CopyF32(v.uvx[k], &out.uvExtra[k][0], (size_t)nRV * 2); }
    out.idx.resize(I.nIdx);
    if (I.nIdx > 0) {
        // el index buffer ya viene en el ancho de ESTA plataforma: una copia (como W3dMeshBinRead)
        if (v.idx32 && sizeof(MeshIndex) == 4 && HostLE()) memcpy(&out.idx[0], v.idx, (size_t)I.nIdx * 4);
        else if (!v.idx32 && sizeof(MeshIndex) == 2 && HostLE()) memcpy(&out.idx[0], v.idx, (size_t)I.nIdx * 2);
        else for (unsigned i = 0; i < I.nIdx; i++)
            out.idx[i] = (MeshIndex)(v.idx32 ? LeeU32(v.idx + (size_t)i * 4) : LeeU16(v.idx + (size_t)i * 2));
    }
    out.parts = v.parts;
    FillExtras(v, out);
    for (int k = 0; k < 3; k++) { out.aabbMin[k] = I.aabbMin[k]; out.aabbMax[k] = I.aabbMax[k]; out.center[k] = I.center[k]; }
    out.radius = I.radius;
    out.aabbOk = (I.flags & INFO_AABBOK) != 0;
    out.groupedVerts = I.groupedVerts;
    out.statsNoEditable = (I.flags & INFO_STATS_NOEDIT) != 0;
    if (info) { info->materials.clear(); for (size_t k = 0; k < out.parts.size(); k++) info->materials.push_back(out.parts[k].material); }
    if (stamp) {
        // lo mismo que ApplyStamp deja en la malla (sin la edicion: AJEN es un bloque de edicion)
        stamp->nPoints     = I.nPoints;
        stamp->nCorners    = (int)I.nCorners;
        stamp->nFaces      = (int)I.nFaces;
        stamp->nEdges      = I.nEdges;
        stamp->nRender     = (int)I.nRV;
        stamp->nTriangles  = (int)(I.nIdx / 3);
        stamp->blocksBin   = v.unknown;
        stamp->requiresBin = v.req;
        stamp->truncated   = false;
        stamp->notLoaded   = false;
    }
    return true;
}

W3dMeshRender::W3dMeshRender()
    : smooth(false), primType(-1), primSize(2.0f), primSize2(0.0f), primHeight(0.0f), primVerts(8), primVerts2(8),
      nRV(0), activeGroup(-1), uvActiveGroup(-1), radius(0.0f), aabbOk(false), groupedVerts(0),
      statsNoEditable(false) {
    for (int k = 0; k < 3; k++) { aabbMin[k] = aabbMax[k] = center[k] = 0.0f; }
}

long W3dMeshRender::Bytes() const {
    long b = (long)(pos.size() * sizeof(float) + nrm.size() + uv.size() * sizeof(float) + col.size() +
                    idx.size() * sizeof(MeshIndex) + ctrl.size() * sizeof(int) + uvRest.size() * sizeof(float));
    for (size_t g = 0; g < groups.size(); g++)   b += (long)(groups[g].idx.size() * sizeof(int) + groups[g].weights.size() * sizeof(float));
    for (size_t g = 0; g < uvGroups.size(); g++) b += (long)(uvGroups[g].idx.size() * sizeof(int) + uvGroups[g].weights.size() * sizeof(float));
    for (size_t k = 0; k < shapes.size(); k++)   b += (long)(shapes[k].idx.size() * sizeof(int) + shapes[k].d.size() * sizeof(float));
    for (size_t k = 0; k < uvExtra.size(); k++)  b += (long)(uvExtra[k].size() * sizeof(float));
    return b;
}

// ============================================================================
//  RETOQUES: el mismo archivo con UN bloque reescrito (nombre en INFO, materiales en PART)
// ============================================================================
namespace {
// donde empieza el nombre en el payload de INFO (lo que ReadInfo lee antes del string)
bool OffsetNameInfo(const Block& b, size_t& off, size_t& endName) {
    InfoBin I;
    Cursor c(b.data, b.bytes);
    I.nRV = c.U32(); I.nIdx = c.U32(); I.nParts = c.U32(); I.nFaces = c.U32(); I.nCorners = c.U32();
    I.nPoints = c.I32(); I.nEdges = c.I32();
    I.flags    = c.U32();
    I.primType = c.I32(); I.primSize = c.F32(); I.primSize2 = c.F32(); I.primHeight = c.F32();
    I.primVerts = c.I32(); I.primVerts2 = c.I32();
    for (int k = 0; k < 9; k++) c.F32();   // aabbMin, aabbMax, centro
    c.F32();                               // radio
    c.I32();                               // groupedVerts
    for (int k = 0; k < 4; k++) c.I32();   // activos
    if (!c.ok) return false;
    off = (size_t)(c.p - b.data);
    std::string name;
    if (!c.Str(name)) return false;
    endName = (size_t)(c.p - b.data);
    return true;
}
// rearma el archivo con los MISMOS bloques, en el mismo orden, salvo 'id' con 'payload' nuevo
// (misma cabecera de bloque: dominio y cantidad; bytes y relleno recalculados)
bool ReplaceBlock(const std::string& in, const Table& t, const char* id, const std::string& payload,
                      std::string& out) {
    out.clear();
    out.reserve(in.size() + payload.size());
    out.append(in, 0, 20);   // cabecera tal cual (firma, version, banderas, nBlocks, reservado)
    bool done = false;
    for (size_t i = 0; i < t.blocks.size(); i++) {
        const Block& b = t.blocks[i];
        out.append(b.id.data(), 4);
        SetU32(out, b.domain);
        SetU32(out, b.count);
        if (b.id == id && !done) {
            SetU32(out, (unsigned)payload.size());
            out += payload;
            done = true;
        } else {
            SetU32(out, b.bytes);
            if (b.bytes) out.append((const char*)b.data, b.bytes);
        }
        while (out.size() % 4) out += '\0';
    }
    if (!done) { out.clear(); return false; }
    return true;
}
} // namespace

bool W3dMeshBinRename(const std::string& in, const std::string& name, std::string& out) {
    out.clear();
    Table t; std::string reason; bool moreNew = false;
    const unsigned char* d = (const unsigned char*)in.data();
    if (in.empty() || !ReadTable(d, in.size(), t, reason, moreNew)) return false;
    const Block* bi = Find(t, "INFO");
    size_t off = 0, end = 0;
    if (!bi || !OffsetNameInfo(*bi, off, end)) return false;
    std::string pay((const char*)bi->data, off);
    SetStr(pay, name);
    pay.append((const char*)bi->data + end, bi->bytes - end);   // campos de una version mas nueva: tal cual
    return ReplaceBlock(in, t, "INFO", pay, out);
}

// las partes de PART tal cual vienen (nombre, material, inicio, cantidad) + lo que sigue
static bool ReadParts(const Block& b, std::vector<W3dMeshBinPart>& ps, size_t& endParts) {
    Cursor c(b.data, b.bytes);
    ps.clear();
    for (unsigned k = 0; k < b.count; k++) {
        W3dMeshBinPart pa;
        c.Str(pa.name); c.Str(pa.material);
        pa.start = c.I32(); pa.count = c.I32();
        if (!c.ok) return false;
        ps.push_back(pa);
    }
    endParts = (size_t)(c.p - b.data);
    return true;
}

bool W3dMeshBinMaterials(const std::string& in, std::vector<std::string>& mats) {
    mats.clear();
    Table t; std::string reason; bool moreNew = false;
    if (in.empty() || !ReadTable((const unsigned char*)in.data(), in.size(), t, reason, moreNew)) return false;
    const Block* bp = Find(t, "PART");
    std::vector<W3dMeshBinPart> ps; size_t end = 0;
    if (!bp || !ReadParts(*bp, ps, end)) return false;
    for (size_t k = 0; k < ps.size(); k++) mats.push_back(ps[k].material);
    return true;
}

bool W3dMeshBinChangeMaterials(const std::string& in, const std::vector<std::string>& mats, std::string& out) {
    out.clear();
    Table t; std::string reason; bool moreNew = false;
    if (in.empty() || !ReadTable((const unsigned char*)in.data(), in.size(), t, reason, moreNew)) return false;
    const Block* bp = Find(t, "PART");
    std::vector<W3dMeshBinPart> ps; size_t end = 0;
    if (!bp || !ReadParts(*bp, ps, end) || ps.size() != mats.size()) return false;
    std::string pay;
    for (size_t k = 0; k < ps.size(); k++) {
        SetStr(pay, ps[k].name); SetStr(pay, mats[k]);
        SetI32(pay, ps[k].start); SetI32(pay, ps[k].count);
    }
    pay.append((const char*)bp->data + end, bp->bytes - end);
    return ReplaceBlock(in, t, "PART", pay, out);
}

bool W3dMeshBinList(const unsigned char* d, size_t n, std::vector<std::string>& lines) {
    lines.clear();
    Table t;
    std::string reason;
    bool moreNew = false;
    if (!ReadTable(d, n, t, reason, moreNew)) { lines.push_back(reason); return false; }
    static const char* kDom[] = { "point", "corner", "face", "edge", "global", "render", "triangle" };
    for (size_t i = 0; i < t.blocks.size(); i++) {
        const Block& b = t.blocks[i];
        std::string s = b.id + " ";
        const unsigned dom = Dom(b);
        s += (dom < 7) ? kDom[dom] : "?";
        if (b.domain & W3DB_IDX32) s += "+idx32";
        s += " "; W3dWriteInt(s, (int)b.count);
        s += " "; W3dWriteInt(s, (int)b.bytes); s += " B";
        lines.push_back(s);
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
//   - la identidad de los puntos (W3dMeshComputePoints) -> vertCtrlPoint/posRep;
//   - que capas salen, la activa y las LAZY (W3dMeshPickLayers);
//   - los arrays de render desde la capa ACTIVA (lo que hace ApplyLayersToRender,
//     incluido el colapso del color por vertice), con los defaults del lector para
//     lo que ninguna cara usa (normal (0,127,0), uv 0, blanco);
//   - los pesos de vertex groups y shape keys POR PUNTO, repartidos a todos los
//     render-verts del punto; los UV groups y el reposo 2D por esquina;
//   - las marcas que siguen siendo una arista (W3dMeshMarksValid);
//   - las aristas por representante y el AABB/centro/radio (W3dComputeGeomStats).
//  El index buffer canonico lo trae el que guarda (canon): triangular y Forsyth
//  son del editor.
// ============================================================================
bool W3dMeshBinWrite(const Mesh* m, const W3dMeshBinIndices* canon, std::string& out,
                         std::vector<std::string>* notices) {
    out.clear();
    if (!m) return false;
    // EL FRENO DE MANO (el mismo que el texto): un archivo que REQUIERE algo que no entendemos
    const std::string missing = W3dMeshMissingBlock(m);
    if (!missing.empty()) {
        if (notices) notices->push_back("no guardo esta malla: el archivo REQUIERE el bloque '" + missing +
                                      "' y esta version no lo entiende");
        return false;
    }
    // INDICES DE 16 BITS (el N95): con mas de 65535 render-verts el index buffer de memoria y
    // el canonico (los dos son MeshIndex) ya vienen TRUNCADOS modulo 65536. Todos quedan "en
    // rango", asi que ningun chequeo de abajo lo ve: saldria un RIDX de 32 bits con los
    // triangulos equivocados, que el PC abriria sin chistar. No hay index buffer correcto
    // que escribir: se niega (el que guarda la manda en texto, que guarda las caras con int).
    if (!W3dMeshBinIndicesReach(m->vertexSize)) {
        if (notices) {
            std::string s = "no guardo esta malla en binario: tiene ";
            W3dWriteInt(s, m->vertexSize);
            s += " vertices de render y esta plataforma usa indices de 16 bits (hasta 65535)";
            notices->push_back(s);
        }
        return false;
    }

    // ---- PUNTOS: la identidad de los vertices ----
    W3dMeshPoints P;
    W3dMeshComputePoints(m, P);
    const int nV = P.nV;
    const int nP = P.nP;

    // ---- CARAS: el lector del .w3dm descarta las de menos de 3 lados o con un indice fuera
    //      de rango; aca tampoco salen (y se avisa). nCtodas cuenta TODAS las esquinas: es
    //      contra lo que se miden las capas, igual que en el texto. ----
    const size_t nFt = m->faces3d.size();
    std::vector<char> faceOk(nFt > 0 ? nFt : 1, 0);
    int nF = 0, nC = 0, nCtodas = 0, badFaces = 0;
    for (size_t f = 0; f < nFt; f++) {
        const std::vector<int>& idx = m->faces3d[f].idx;
        nCtodas += (int)idx.size();
        bool ok = idx.size() >= 3;
        for (size_t c = 0; c < idx.size() && ok; c++) if (idx[c] < 0 || idx[c] >= nV) ok = false;
        if (!ok) { badFaces++; continue; }
        faceOk[f] = 1; nF++; nC += (int)idx.size();
    }
    if (badFaces > 0 && notices) {
        std::string s = "descarte "; W3dWriteInt(s, badFaces);
        s += " cara(s) con menos de 3 lados o con indices fuera de rango";
        notices->push_back(s);
    }

    // ---- IDENTIDAD: cada render-vert -> su punto de control ----
    //  EDITABLE: el PRIMER render-vert del punto (representante), porque la edicion (posRep/bordes) razona en
    //    indices de render-vert. Deja nCtrl ~= nV (disperso) pero preserva toda la info de edicion.
    //  NO-EDITABLE (malla de JUEGO): el INDICE DE PUNTO COMPACTO (0..nP-1). Asi al abrir, vertCtrlPoint ya viene
    //    agrupado (nCtrl = nP) y el skinning NO recalcula nada en el telefono (el agrupamiento queda HORNEADO).
    //    Para que cierre, los vertex groups y shape keys de abajo tambien se escriben POR PUNTO en este modo.
    std::vector<int> ctrl((size_t)(nV > 0 ? nV : 1), 0);
    if (m->noEditable) {
        for (int i = 0; i < nV; i++) { const int p = P.rvToPoint[(size_t)i]; ctrl[(size_t)i] = (p >= 0) ? p : i; }
    } else {
        std::vector<int> pointRep((size_t)(nP > 0 ? nP : 1), -1);
        for (int i = 0; i < nV; i++) {
            const int p = P.rvToPoint[(size_t)i];
            if (p >= 0 && pointRep[(size_t)p] < 0) pointRep[(size_t)p] = i;
        }
        for (int i = 0; i < nV; i++) {
            const int p = P.rvToPoint[(size_t)i];
            ctrl[(size_t)i] = (p >= 0) ? pointRep[(size_t)p] : i;
        }
    }

    // ---- CAPAS: cuales salen (los mismos avisos que el texto) ----
    W3dMeshLayersExit layers;
    W3dMeshPickLayers(m, nCtodas, layers, notices);

    // ---- ARRAYS DE RENDER: la capa ACTIVA por esquina, defaults para lo que no es de una cara ----
    std::vector<signed char>   rnrm((size_t)nV * 3);
    std::vector<float>         ruv((size_t)nV * 2, 0.0f);
    std::vector<unsigned char> rcol((size_t)nV * 4, (unsigned char)255);
    std::vector<char>          fromFace((size_t)(nV > 0 ? nV : 1), 0);
    for (int i = 0; i < nV; i++) { rnrm[(size_t)i*3] = 0; rnrm[(size_t)i*3+1] = 127; rnrm[(size_t)i*3+2] = 0; }
    const bool nrmByLayer = ((int)m->cornerNormal.size() == nCtodas * 3);
    const bool hasNRM = (nCtodas > 0 && (nrmByLayer || m->normals));
    const UVMap* uvAct = (layers.uvActive >= 0) ? m->uvMaps[layers.uv[(size_t)layers.uvActive]] : 0;
    const ColorLayer* colAct = (layers.activeCol >= 0) ? m->colorLayers[layers.col[(size_t)layers.activeCol]] : 0;
    {
        int L = 0;
        for (size_t f = 0; f < nFt; f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                if (!faceOk[f]) continue;
                const int gv = m->faces3d[f].idx[c];
                fromFace[(size_t)gv] = 1;
                if (hasNRM) {
                    const GLbyte* n3 = nrmByLayer ? &m->cornerNormal[(size_t)L * 3] : &m->normals[(size_t)gv * 3];
                    rnrm[(size_t)gv*3] = (signed char)n3[0]; rnrm[(size_t)gv*3+1] = (signed char)n3[1]; rnrm[(size_t)gv*3+2] = (signed char)n3[2];
                }
                if (uvAct) { ruv[(size_t)gv*2] = uvAct->uv[(size_t)L*2]; ruv[(size_t)gv*2+1] = uvAct->uv[(size_t)L*2+1]; }
                else if (layers.uvDerived) { ruv[(size_t)gv*2] = m->uv[(size_t)gv*2]; ruv[(size_t)gv*2+1] = m->uv[(size_t)gv*2+1]; }
                if (colAct) { for (int q = 0; q < 4; q++) rcol[(size_t)gv*4+(size_t)q] = colAct->color[(size_t)L*4+(size_t)q]; }
                else if (layers.colDerived) { for (int q = 0; q < 4; q++) rcol[(size_t)gv*4+(size_t)q] = m->vertexColor[(size_t)gv*4+(size_t)q]; }
            }
    }
    // capa de color POR VERTICE: el color se colapsa por posicion (el primero gana), como
    // ApplyLayersToRender despues de la carga clasica
    if (colAct && colAct->perVertex && nV > 0) {
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
    std::vector<int> looseE, looseV;
    {
        std::vector<char> used((size_t)(nP > 0 ? nP : 1), 0);
        for (int i = 0; i < nV; i++) if (fromFace[(size_t)i] && P.rvToPoint[(size_t)i] >= 0) used[(size_t)P.rvToPoint[(size_t)i]] = 1;
        int bad = 0, merged = 0;
        for (size_t i = 0; i + 1 < m->looseEdges.size(); i += 2) {
            const int a = m->looseEdges[i], b = m->looseEdges[i + 1];
            if (a < 0 || a >= nV || b < 0 || b >= nV) { bad++; continue; }
            looseE.push_back(ctrl[(size_t)a]); looseE.push_back(ctrl[(size_t)b]);
            used[(size_t)P.rvToPoint[(size_t)a]] = 1; used[(size_t)P.rvToPoint[(size_t)b]] = 1;
        }
        for (size_t i = 0; i < m->looseVerts.size(); i++) {
            const int a = m->looseVerts[i];
            if (a < 0 || a >= nV) { bad++; continue; }
            const int p = P.rvToPoint[(size_t)a];
            if (used[(size_t)p]) { merged++; continue; }
            used[(size_t)p] = 1;
            looseV.push_back(ctrl[(size_t)a]);
        }
        if (bad > 0 && notices) {
            std::string s = "descarte "; W3dWriteInt(s, bad);
            s += " arista(s)/vertice(s) suelto(s) con indices fuera de rango";
            notices->push_back(s);
        }
        if (merged > 0 && notices) {
            std::string s = ""; W3dWriteInt(s, merged);
            s += " vertice(s) suelto(s) caian en un punto que ya estaba en uso: los fusiono con el que ya estaba";
            notices->push_back(s);
        }
    }

    // ---- ARISTAS REALES (pares de puntos): validan las marcas y cuentan el dominio arista ----
    const bool hasMarks   = (!m->sharpEdges.empty() || !m->seamEdges.empty());
    const bool hasForeign = (!m->w3dmForeign.blocks.empty() || !m->w3dmForeign.blocksBin.empty());
    std::set<std::pair<int, int> > realEdges;
    const bool withEdges = (hasMarks || hasForeign);
    if (withEdges) W3dMeshRealEdges(m, P, realEdges);

    // ---- MESH PARTS: al menos una (la malla sin partes vuelve con una por defecto) ----
    const int nParts = m->materialsGroup.empty() ? 1 : (int)m->materialsGroup.size();

    // ---- INDEX BUFFER: el canonico que trae el editor, o el de memoria tal cual ----
    const MeshIndex* idxSrc = 0;
    int nIdx = 0;
    std::vector<int> partStart((size_t)nParts, 0), partCnt((size_t)nParts, 0);
    if (canon) {
        if ((int)canon->start.size() != nParts || (int)canon->count.size() != nParts) {
            if (notices) notices->push_back("el index buffer canonico no tiene un rango por mesh part: no guardo la malla");
            return false;
        }
        idxSrc = canon->idx.empty() ? 0 : &canon->idx[0];
        nIdx = (int)canon->idx.size();
        partStart = canon->start; partCnt = canon->count;
    } else {
        idxSrc = m->faces;
        nIdx = (m->faces && m->facesSize > 0) ? m->facesSize : 0;
        for (int k = 0; k < nParts && k < (int)m->materialsGroup.size(); k++) {
            partStart[(size_t)k] = m->materialsGroup[(size_t)k].startDrawn;
            partCnt[(size_t)k] = m->materialsGroup[(size_t)k].indicesDrawnCount;
        }
    }
    if (nIdx % 3 != 0) { if (notices) notices->push_back("el index buffer no es de triangulos: no guardo la malla"); return false; }
    for (int i = 0; i < nIdx; i++)
        if ((unsigned)idxSrc[i] >= (unsigned)nV) { if (notices) notices->push_back("el index buffer apunta fuera de la malla: no guardo la malla"); return false; }
    for (int k = 0; k < nParts; k++) {
        const int a = partStart[(size_t)k], c = partCnt[(size_t)k];
        if (a < 0 || c < 0 || a > nIdx || c > nIdx - a) {
            if (notices) notices->push_back("un mesh part sale del index buffer: no guardo la malla");
            return false;
        }
    }

    // ---- AABB / centro / radio (el mismo codigo que ComputeBorders / ComputeAABBOnly) ----
    W3dGeomStats st;
    W3dComputeGeomStats(m->vertex, nV, (m->noEditable || nV <= 0) ? NULL : &ctrl[0], st);

    // ---- INFO ----
    InfoBin I;
    I.nRV = (unsigned)nV;
    I.nIdx = (unsigned)nIdx;
    I.nParts = (unsigned)nParts;
    I.nFaces = (unsigned)nF;
    I.nCorners = (unsigned)nC;
    I.nPoints = nP;
    I.nEdges = withEdges ? (int)realEdges.size() : -1;
    I.flags  = (m->meshSmooth ? INFO_SMOOTH : 0) | (st.ok ? INFO_AABBOK : 0) |
                 (m->noEditable ? INFO_STATS_NOEDIT : 0) | (m->meshType >= 0 ? INFO_PRIM : 0);
    if (m->meshType >= 0) {
        I.primType   = m->meshType; I.primSize = m->meshSize; I.primSize2 = m->meshSize2;
        I.primHeight = m->meshDepth; I.primVerts = m->meshVerts; I.primVerts2 = m->meshVerts2;
    }
    if (st.ok) {
        I.aabbMin[0] = st.aabbMin.x; I.aabbMin[1] = st.aabbMin.y; I.aabbMin[2] = st.aabbMin.z;
        I.aabbMax[0] = st.aabbMax.x; I.aabbMax[1] = st.aabbMax.y; I.aabbMax[2] = st.aabbMax.z;
        I.center[0] = st.center.x;   I.center[1] = st.center.y;   I.center[2] = st.center.z;
        I.radius = st.radius;
    }
    I.groupedVerts = m->noEditable ? 0 : st.grouped;
    // los ACTIVOS como los deja el lector de texto
    // sin capas del editor (uvDerived) salen la derivada "UVMap" + las capas extra que la malla ya tenga en el
    // render (uvExtra: TEXCOORD_1 importada o un RUVX leido), cada una en su mismo indice
    int nUVRight = 1;
    for (size_t k = 1; k < m->uvExtra.size(); k++) if ((int)m->uvExtra[k].size() == nV * 2) nUVRight = (int)k + 1;
    const int nUVExit = layers.uvDerived ? nUVRight : (int)layers.uv.size();
    const int nColExit = layers.colDerived ? 1 : (int)layers.col.size();
    I.uvActive  = layers.uvDerived  ? 0 : (m->uvMaps.empty()      ? -1 : ActiveValid(layers.uvActive, nUVExit));
    I.activeCol = layers.colDerived ? 0 : (m->colorLayers.empty() ? -1 : ActiveValid(layers.activeCol, nColExit));
    int nVGExit = 0, nUVGExit = 0;
    for (size_t g = 0; g < m->vertexGroups.size(); g++) if (m->vertexGroups[g]) nVGExit++;
    for (size_t g = 0; g < m->uvGroups.size(); g++)     if (m->uvGroups[g]) nUVGExit++;
    I.activeGroup   = ActiveValid(m->vertexGroups.empty() ? -1 : m->activeGroup, nVGExit);
    I.uvActiveGroup = ActiveValid(m->uvGroups.empty() ? -1 : m->uvActiveGroup, nUVGExit);
    I.name = W3dMeshSaneName(m->name, notices, "de la malla");

    // ======================================================================
    //  EMISION
    // ======================================================================
    unsigned nBlocks = 0;
    out.reserve((size_t)nV * 64 + (size_t)nIdx * 4 + (size_t)nC * 16 + 1024);
    out.append("W3DB", 4);
    SetU32(out, W3DB_VERSION);
    SetU32(out, 0);             // banderas
    const size_t posNBlocks = out.size();
    SetU32(out, 0);             // nBlocks (se parchea al final)
    SetU32(out, 0);             // reservado

    size_t pb;
    // REQ
    {
        int nReq = 0; while (kRequired[nReq]) nReq++;
        pb = OpenBlock(out, "REQ ", W3DB_GLOBAL, (unsigned)nReq);
        for (int i = 0; i < nReq; i++) out.append(kRequired[i], 4);
        CloseBlock(out, pb, nBlocks);
    }
    // INFO
    pb = OpenBlock(out, "INFO", W3DB_GLOBAL, 1);
    WriteInfo(out, I);
    CloseBlock(out, pb, nBlocks);
    // RENDER
    if (nV > 0) {
        pb = OpenBlock(out, "RPOS", W3DB_RENDER, (unsigned)nV); SetF32s(out, m->vertex, (size_t)nV * 3); CloseBlock(out, pb, nBlocks);
        pb = OpenBlock(out, "RNRM", W3DB_RENDER, (unsigned)nV); SetBytes(out, &rnrm[0], (size_t)nV * 3); CloseBlock(out, pb, nBlocks);
        pb = OpenBlock(out, "RUV ", W3DB_RENDER, (unsigned)nV); SetF32s(out, &ruv[0], (size_t)nV * 2); CloseBlock(out, pb, nBlocks);
        pb = OpenBlock(out, "RCOL", W3DB_RENDER, (unsigned)nV); SetBytes(out, &rcol[0], (size_t)nV * 4); CloseBlock(out, pb, nBlocks);
        // ---- RUVX: las capas UV que NO son la activa, por render-vert (Mesh::uvExtra: las capas de textura con
        //      UV propio, TexLayer::uvMap). La activa va "ausente" (su render es RUV). Solo si alguna existe. ----
        {
            std::vector< std::vector<float> > rx((size_t)(nUVExit > 0 ? nUVExit : 0));
            bool any = false;
            for (int l = 0; l < nUVExit; l++) {
                if (l == I.uvActive) continue;
                std::vector<float>& dd = rx[(size_t)l];
                if (!layers.uvDerived) {
                    const UVMap* u = m->uvMaps[layers.uv[(size_t)l]];
                    if ((int)u->uv.size() != nCtodas * 2) continue;
                    dd.assign((size_t)nV * 2, 0.0f);
                    int L = 0;
                    for (size_t f = 0; f < nFt; f++)
                        for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                            if (!faceOk[f]) continue;
                            const int gv = m->faces3d[f].idx[c];
                            dd[(size_t)gv*2] = u->uv[(size_t)L*2]; dd[(size_t)gv*2+1] = u->uv[(size_t)L*2+1];
                        }
                } else if (l < (int)m->uvExtra.size() && (int)m->uvExtra[(size_t)l].size() == nV * 2) {
                    dd = m->uvExtra[(size_t)l];
                } else continue;
                any = true;
            }
            if (any) {
                pb = OpenBlock(out, "RUVX", W3DB_GLOBAL, (unsigned)nUVExit);
                for (int l = 0; l < nUVExit; l++) {
                    const bool present = !rx[(size_t)l].empty();
                    SetU32(out, present ? 1u : 0u);
                    if (present) SetF32s(out, &rx[(size_t)l][0], (size_t)nV * 2);
                }
                CloseBlock(out, pb, nBlocks);
            }
        }
    }
    if (nIdx > 0) {
        const bool i32 = nV > 65535;
        pb = OpenBlock(out, "RIDX", W3DB_TRIANGLE | (i32 ? W3DB_IDX32 : 0), (unsigned)(nIdx / 3));
        if (i32) for (int i = 0; i < nIdx; i++) SetU32(out, (unsigned)idxSrc[i]);
        else for (int i = 0; i < nIdx; i++) {
            const unsigned x = (unsigned)idxSrc[i];
            char b2[2]; b2[0] = (char)(x & 255u); b2[1] = (char)((x >> 8) & 255u);
            out.append(b2, 2);
        }
        CloseBlock(out, pb, nBlocks);
    }
    // PART
    pb = OpenBlock(out, "PART", W3DB_GLOBAL, (unsigned)nParts);
    for (int k = 0; k < nParts; k++) {
        std::string name = "Mesh", mat;
        if (k < (int)m->materialsGroup.size()) {
            const MaterialGroup& mg = m->materialsGroup[(size_t)k];
            name = W3dMeshSaneName(mg.name, notices, "de un mesh part");
            if (mg.material && !mg.material->name.empty()) mat = W3dMeshSaneName(mg.material->name, notices, "de un material");
        }
        SetStr(out, name); SetStr(out, mat);
        SetI32(out, partStart[(size_t)k]); SetI32(out, partCnt[(size_t)k]);
    }
    CloseBlock(out, pb, nBlocks);
    // RCTL
    if (nV > 0) { pb = OpenBlock(out, "RCTL", W3DB_RENDER, (unsigned)nV); SetI32s(out, &ctrl[0], (size_t)nV); CloseBlock(out, pb, nBlocks); }

    // ---- VERTEX GROUPS y SHAPE KEYS: por PUNTO (como el texto), repartidos a TODOS los
    //      render-verts del punto (como el lector de texto) ----
    std::vector<int> csrOff, csrRv;   // punto -> sus render-verts, de menor a mayor
    if (!m->vertexGroups.empty() || !m->shapeKeys.empty()) {
        csrOff.assign((size_t)nP + 1, 0);
        for (int i = 0; i < nV; i++) { const int p = P.rvToPoint[(size_t)i]; if (p >= 0) csrOff[(size_t)p + 1]++; }
        for (int p = 0; p < nP; p++) csrOff[(size_t)p + 1] += csrOff[(size_t)p];
        csrRv.assign((size_t)(csrOff[(size_t)nP] > 0 ? csrOff[(size_t)nP] : 1), 0);
        std::vector<int> full((size_t)(nP > 0 ? nP : 1), 0);
        for (int i = 0; i < nV; i++) {
            const int p = P.rvToPoint[(size_t)i];
            if (p < 0) continue;
            csrRv[(size_t)(csrOff[(size_t)p] + full[(size_t)p]++)] = i;
        }
    }
    std::map<int, int> cpRep;
    W3dMeshCpRep(m, cpRep);
    // EDITABLE: los pesos/deltas se reparten a TODOS los render-verts del punto (vg->verts = render-verts), que es
    //   lo que el lector editable y el weight paint esperan. NO-EDITABLE: se escriben POR PUNTO (vg->verts = punto
    //   compacto), en el MISMO espacio que el ctrl compacto -> el skinning los usa directo, sin repartir ni agrupar.
    const bool compact = m->noEditable;
    if (nVGExit > 0) {
        pb = OpenBlock(out, "RVGR", W3DB_GLOBAL, (unsigned)nVGExit);
        for (size_t g = 0; g < m->vertexGroups.size(); g++) {
            std::vector<int> pi; std::vector<float> pw;
            if (!W3dMeshGroupByPoint(m, P, cpRep, g, pi, pw, notices)) continue;
            std::vector<int> ri; std::vector<float> rw;
            if (compact) { ri = pi; rw = pw; }   // por punto compacto (0..nP-1)
            else for (size_t k = 0; k < pi.size(); k++) {
                const int p = pi[k];
                for (int q = csrOff[(size_t)p]; q < csrOff[(size_t)p + 1]; q++) { ri.push_back(csrRv[(size_t)q]); rw.push_back(pw[k]); }
            }
            SetStr(out, W3dMeshSaneName(m->vertexGroups[g]->name, notices, "de un vertex group"));
            SetU32(out, (unsigned)ri.size());
            if (!ri.empty()) { SetI32s(out, &ri[0], ri.size()); SetF32s(out, &rw[0], rw.size()); }
        }
        CloseBlock(out, pb, nBlocks);
    }
    if (!m->shapeKeys.empty()) {
        pb = OpenBlock(out, "RSHP", W3DB_GLOBAL, (unsigned)m->shapeKeys.size());
        for (size_t k = 0; k < m->shapeKeys.size(); k++) {
            std::vector<int> pi; std::vector<float> pd;
            W3dMeshShapeByPoint(m, P, cpRep, k, pi, pd);
            std::vector<int> ri; std::vector<float> rd;
            if (compact) { ri = pi; rd = pd; }   // por punto compacto (0..nP-1)
            else for (size_t q = 0; q < pi.size(); q++) {
                const int p = pi[q];
                for (int r = csrOff[(size_t)p]; r < csrOff[(size_t)p + 1]; r++) {
                    ri.push_back(csrRv[(size_t)r]);
                    rd.push_back(pd[q*3]); rd.push_back(pd[q*3+1]); rd.push_back(pd[q*3+2]);
                }
            }
            SetStr(out, W3dMeshSaneName(m->shapeKeys[k].name, notices, "de una shape key"));
            SetU32(out, (unsigned)ri.size());
            if (!ri.empty()) { SetI32s(out, &ri[0], ri.size()); SetF32s(out, &rd[0], rd.size()); }
        }
        CloseBlock(out, pb, nBlocks);
    }
    // ---- UV GROUPS: el peso de cada render-vert que usa una cara (el lector de texto
    //      colapsa las esquinas a su render-vert: el resto se pierde) ----
    if (nUVGExit > 0) {
        pb = OpenBlock(out, "RUVG", W3DB_GLOBAL, (unsigned)nUVGExit);
        for (size_t g = 0; g < m->uvGroups.size(); g++) {
            const UVGroup* ug = m->uvGroups[g];
            if (!ug) continue;
            std::vector<float> w((size_t)(nV > 0 ? nV : 1), 0.0f);
            for (size_t k = 0; k < ug->verts.size() && k < ug->weights.size(); k++) {
                const int rv = ug->verts[k];
                if (rv >= 0 && rv < nV) w[(size_t)rv] = ug->weights[k];
            }
            std::vector<int> ri; std::vector<float> rw;
            for (int i = 0; i < nV; i++) if (fromFace[(size_t)i] && w[(size_t)i] > 0.0f) { ri.push_back(i); rw.push_back(w[(size_t)i]); }
            SetStr(out, W3dMeshSaneName(ug->name, notices, "de un UV group"));
            SetU32(out, (unsigned)ri.size());
            if (!ri.empty()) { SetI32s(out, &ri[0], ri.size()); SetF32s(out, &rw[0], rw.size()); }
        }
        CloseBlock(out, pb, nBlocks);
    }
    // ---- REPOSO 2D (base del skinning de UV): por render-vert de cara, 0 el resto ----
    if ((int)m->uv2dRest.size() == nV * 2 && nV > 0 && nCtodas > 0) {
        std::vector<float> r((size_t)nV * 2, 0.0f);
        for (int i = 0; i < nV; i++) if (fromFace[(size_t)i]) { r[(size_t)i*2] = m->uv2dRest[(size_t)i*2]; r[(size_t)i*2+1] = m->uv2dRest[(size_t)i*2+1]; }
        pb = OpenBlock(out, "RUVR", W3DB_RENDER, (unsigned)nV); SetF32s(out, &r[0], r.size()); CloseBlock(out, pb, nBlocks);
    }

    // ======================================================================
    //  BLOQUES DE EDICION
    // ======================================================================
    // ---- FACE: lados | mesh part | shading (i8, relleno a 4) | render-verts ----
    if (nF > 0) {
        const int nGr = nParts;
        pb = OpenBlock(out, "FACE", W3DB_FACE, (unsigned)nF);
        SetU32(out, (unsigned)nC);
        for (size_t f = 0; f < nFt; f++) if (faceOk[f]) SetI32(out, (int)m->faces3d[f].idx.size());
        for (size_t f = 0; f < nFt; f++) if (faceOk[f]) {
            const int g = m->faces3d[f].mat;
            SetI32(out, (g < 0 || g >= nGr) ? 0 : g);   // el lector de texto manda al 0 una parte que no existe
        }
        for (size_t f = 0; f < nFt; f++) if (faceOk[f]) {
            const int sm = m->faces3d[f].smooth;
            out += (char)(signed char)(sm < 0 ? -1 : (sm != 0 ? 1 : 0));
        }
        while ((out.size() - (pb + 4)) % 4) out += '\0';
        for (size_t f = 0; f < nFt; f++) if (faceOk[f]) SetI32s(out, &m->faces3d[f].idx[0], m->faces3d[f].idx.size());
        CloseBlock(out, pb, nBlocks);
    }
    // ---- LOOS: aristas y vertices sueltos ----
    if (!looseE.empty() || !looseV.empty()) {
        pb = OpenBlock(out, "LOOS", W3DB_GLOBAL, 2);
        SetU32(out, (unsigned)looseE.size()); if (!looseE.empty()) SetI32s(out, &looseE[0], looseE.size());
        SetU32(out, (unsigned)looseV.size()); if (!looseV.empty()) SetI32s(out, &looseV[0], looseV.size());
        CloseBlock(out, pb, nBlocks);
    }
    // ---- CNRM: la normal por esquina (la capa, o la derivada del render) ----
    if (hasNRM && nC > 0) {
        pb = OpenBlock(out, "CNRM", W3DB_CORNER, (unsigned)nC);
        int L = 0;
        for (size_t f = 0; f < nFt; f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                if (!faceOk[f]) continue;
                const GLbyte* n3 = nrmByLayer ? &m->cornerNormal[(size_t)L * 3] : &m->normals[(size_t)m->faces3d[f].idx[c] * 3];
                out += (char)n3[0]; out += (char)n3[1]; out += (char)n3[2];
            }
        CloseBlock(out, pb, nBlocks);
    }
    // ---- UVLY: las capas UV que salen (o la derivada "UVMap") ----
    if (nUVExit > 0 && (layers.uvDerived || !layers.uv.empty())) {
        pb = OpenBlock(out, "UVLY", W3DB_GLOBAL, (unsigned)nUVExit);
        for (int l = 0; l < nUVExit; l++) {
            const UVMap* u = layers.uvDerived ? 0 : m->uvMaps[layers.uv[(size_t)l]];
            // derivada: la capa 0 es uv[]; las siguientes, uvExtra[l] (si falta, ceros)
            const GLfloat* right = 0;
            if (!u) right = (l == 0) ? m->uv
                        : ((l < (int)m->uvExtra.size() && (int)m->uvExtra[(size_t)l].size() == nV * 2) ? &m->uvExtra[(size_t)l][0] : 0);
            char nameRight[16];
            if (l == 0) strcpy(nameRight, "UVMap"); else snprintf(nameRight, sizeof nameRight, "UV%d", l + 1);
            SetStr(out, u ? W3dMeshSaneName(u->name, notices, "de una capa UV") : std::string(nameRight));
            int L = 0;
            for (size_t f = 0; f < nFt; f++)
                for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                    if (!faceOk[f]) continue;
                    if (u) { SetF32(out, u->uv[(size_t)L*2]); SetF32(out, u->uv[(size_t)L*2+1]); }
                    else if (right) { const int gv = m->faces3d[f].idx[c]; SetF32(out, right[(size_t)gv*2]); SetF32(out, right[(size_t)gv*2+1]); }
                    else { SetF32(out, 0.0f); SetF32(out, 0.0f); }
                }
        }
        CloseBlock(out, pb, nBlocks);
    }
    // ---- COLY: las capas de color que salen (o la derivada "Col") ----
    if (nColExit > 0 && (layers.colDerived || !layers.col.empty())) {
        pb = OpenBlock(out, "COLY", W3DB_GLOBAL, (unsigned)nColExit);
        for (int l = 0; l < nColExit; l++) {
            const ColorLayer* cl = layers.colDerived ? 0 : m->colorLayers[layers.col[(size_t)l]];
            SetStr(out, cl ? W3dMeshSaneName(cl->name, notices, "de una capa de color") : std::string("Col"));
            SetU32(out, (cl && cl->perVertex) ? COLY_BY_VERTEX : 0);
            int L = 0;
            for (size_t f = 0; f < nFt; f++)
                for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                    if (!faceOk[f]) continue;
                    const unsigned char* c4 = cl ? &cl->color[(size_t)L*4] : &m->vertexColor[(size_t)m->faces3d[f].idx[c]*4];
                    SetBytes(out, c4, 4);
                }
        }
        CloseBlock(out, pb, nBlocks);
    }
    // ---- SHRP / SEAM: las que siguen siendo una arista, como claves de posicion ----
    if (hasMarks) {
        for (int pass = 0; pass < 2; pass++) {
            std::vector<std::pair<int, int> > pairs;
            W3dMeshMarksValid(m, P, realEdges, pass != 0, pairs, notices);
            // par de PUNTOS -> clave de POSICION (la que rearma el lector de texto), armada en
            // little-endian desde los floats: ni el orden ADENTRO de cada clave (sus dos mitades)
            // ni el del set (el del archivo) dependen del host
            std::set<std::string> keys;
            for (size_t i = 0; i < pairs.size(); i++)
                keys.insert(KeyMarkFile(&P.pts[(size_t)pairs[i].first * 3], &P.pts[(size_t)pairs[i].second * 3]));
            if (keys.empty()) continue;
            pb = OpenBlock(out, pass ? "SEAM" : "SHRP", W3DB_EDGE, (unsigned)keys.size());
            for (std::set<std::string>::const_iterator it = keys.begin(); it != keys.end(); ++it) out += *it;
            CloseBlock(out, pb, nBlocks);
        }
    }
    // ---- EDGE: las aristas por representante (sin el std::set de ComputeBorders al abrir).
    //      Un escenario cerrado no las tiene. ----
    if (!m->noEditable && nV > 0 && (nF > 0 || !looseE.empty())) {
        std::vector<MeshFace> facesOk;
        facesOk.reserve((size_t)nF);
        for (size_t f = 0; f < nFt; f++) if (faceOk[f]) facesOk.push_back(m->faces3d[f]);
        std::vector<int> edges;
        EdgesByRep(facesOk, looseE, &ctrl[0], nV, edges);
        pb = OpenBlock(out, "EDGE", W3DB_EDGE, (unsigned)(edges.size() / 2));
        if (!edges.empty()) SetI32s(out, &edges[0], edges.size());
        CloseBlock(out, pb, nBlocks);
    }
    // ---- AJEN: bloques de TEXTO de un .w3dm mas nuevo + su "requiere" ----
    {
        std::vector<std::string> req = m->w3dmForeign.requiresValue;
        const bool reqDefault = req.empty() || (req.size() == 2 && req[0] == "V" && req[1] == "F");
        std::vector<const W3dForeignBlock*> remain;
        for (size_t i = 0; i < m->w3dmForeign.blocks.size(); i++) {
            const W3dForeignBlock& b = m->w3dmForeign.blocks[i];
            int now = -1, prev = -1;
            switch (b.domain) {
                case W3DB_POINT:   now = nP; prev = m->w3dmForeign.nPoints;   break;
                case W3DB_CORNER: now  = nC; prev = m->w3dmForeign.nCorners; break;
                case W3DB_FACE:    now = nF; prev = m->w3dmForeign.nFaces;    break;
                case W3DB_EDGE:  now   = (int)realEdges.size(); prev = m->w3dmForeign.nEdges; break;
                default: break;
            }
            if (b.domain != W3DB_GLOBAL && now != prev) {
                if (notices) {
                    std::string s = "no pude conservar el bloque " + b.name + " (";
                    W3dWriteInt(s, prev); s += " -> "; W3dWriteInt(s, now);
                    s += "): lo trajo una version mas nueva de Whisk3D y cambiaste la topologia";
                    notices->push_back(s);
                }
                continue;
            }
            remain.push_back(&b);
        }
        if (!reqDefault || !remain.empty()) {
            pb = OpenBlock(out, "AJEN", W3DB_GLOBAL, 1);
            if (req.empty()) { req.push_back("V"); req.push_back("F"); }
            SetU32(out, (unsigned)req.size());
            for (size_t i = 0; i < req.size(); i++) SetStr(out, W3dMeshSaneName(req[i], notices, "de un bloque de 'requiere'"));
            SetU32(out, (unsigned)remain.size());
            for (size_t i = 0; i < remain.size(); i++) {
                SetStr(out, remain[i]->name); SetI32(out, remain[i]->domain);
                SetI32(out, remain[i]->count); SetStr(out, remain[i]->text);
            }
            CloseBlock(out, pb, nBlocks);
        }
    }
    // ---- BLOQUES BINARIOS DE UNA VERSION MAS NUEVA: tal cual, con la regla del sello ----
    for (size_t i = 0; i < m->w3dmForeign.blocksBin.size(); i++) {
        const W3dForeignBlock& b = m->w3dmForeign.blocksBin[i];
        const unsigned dom = (unsigned)b.domain & 255u;
        int now = -1, prev = -1;
        switch (dom) {
            case W3DB_POINT:     now = nP;       prev = m->w3dmForeign.nPoints;     break;
            case W3DB_CORNER:   now  = nC;       prev = m->w3dmForeign.nCorners;   break;
            case W3DB_FACE:      now = nF;       prev = m->w3dmForeign.nFaces;      break;
            case W3DB_EDGE:    now   = (int)realEdges.size(); prev = m->w3dmForeign.nEdges; break;
            case W3DB_RENDER:    now = nV;       prev = m->w3dmForeign.nRender;     break;
            case W3DB_TRIANGLE: now  = nIdx / 3; prev = m->w3dmForeign.nTriangles; break;
            default: break;
        }
        if (dom != W3DB_GLOBAL && now != prev) {
            if (notices) {
                std::string s = "no pude conservar el bloque binario " + b.name + " (";
                W3dWriteInt(s, prev); s += " -> "; W3dWriteInt(s, now);
                s += "): lo trajo una version mas nueva de Whisk3D y cambiaste la topologia";
                notices->push_back(s);
            }
            continue;
        }
        if (b.name.size() != 4) continue;
        pb = OpenBlock(out, b.name.c_str(), (unsigned)b.domain, (unsigned)b.count);
        out += b.text;
        CloseBlock(out, pb, nBlocks);
    }

    // la cantidad de bloques, al final (la cabecera se escribio antes de saberla)
    out[posNBlocks]     = (char)(nBlocks & 255u);
    out[posNBlocks + 1] = (char)((nBlocks >> 8) & 255u);
    out[posNBlocks + 2] = (char)((nBlocks >> 16) & 255u);
    out[posNBlocks + 3] = (char)((nBlocks >> 24) & 255u);
    return true;
}
