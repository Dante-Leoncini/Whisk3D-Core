// ============================================================================
//  VisSet.cpp — ver VisSet.h y el spec CONGELADO en formato/w3dvis.md.
//
//  Decision de representacion: el blob interno es CANONICO (los registros de
//  celda concatenados en orden de celda, tabla[] = offset de cada uno DENTRO
//  del blob). LoadW3dvis PARSEA cada registro al cargar (eso valida el
//  archivo entero una sola vez y mide el largo real de cada registro, que el
//  formato no declara: los registros son auto-terminados); Guardar escribe
//  header + blob + tabla, asi que un archivo escrito por el motor re-carga
//  byte a byte identico.
// ============================================================================
#include "VisSet.h"
#include <string.h> // memcpy

// ---------------------------------------------------------------------------
//  primitivas de encoding (spec: varint LEB128 + zigzag + palabra u32 LE)
// ---------------------------------------------------------------------------
namespace {

struct Cur {                     // cursor de LECTURA con chequeo de rango
    const unsigned char* p;
    size_t n;                    // tope
    size_t i;                    // posicion
    bool ok;
    Cur(const unsigned char* p_, size_t n_, size_t i_) : p(p_), n(n_), i(i_), ok(true) {}
};

unsigned ReadVarint(Cur& c) {
    unsigned v = 0; int shift = 0;
    while (c.ok) {
        if (c.i >= c.n || shift > 28) { c.ok = false; break; }
        unsigned char b = c.p[c.i++];
        v |= (unsigned)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
        shift += 7;
    }
    return 0;
}

unsigned ReadU32(Cur& c) {       // palabra cruda u32 LE
    if (!c.ok || c.i + 4 > c.n) { c.ok = false; return 0; }
    unsigned v = (unsigned)c.p[c.i] | ((unsigned)c.p[c.i+1] << 8)
               | ((unsigned)c.p[c.i+2] << 16) | ((unsigned)c.p[c.i+3] << 24);
    c.i += 4;
    return v;
}

int FromZigzag(unsigned v) { return (int)(v >> 1) ^ -(int)(v & 1); }
unsigned Zigzag(int n)   { return ((unsigned)n << 1) ^ (unsigned)(n >> 31); }

void SetVarint(std::vector<unsigned char>& out, unsigned v) {
    while (v >= 0x80) { out.push_back((unsigned char)(v | 0x80)); v >>= 7; }
    out.push_back((unsigned char)v);
}
void SetU32(std::vector<unsigned char>& out, unsigned v) {
    out.push_back((unsigned char)(v & 0xFF));
    out.push_back((unsigned char)((v >> 8) & 0xFF));
    out.push_back((unsigned char)((v >> 16) & 0xFF));
    out.push_back((unsigned char)((v >> 24) & 0xFF));
}

// opcodes del registro DELTA (spec)
enum { OpEnd = 0, OpAdd = 1, OpRemove = 2, OpMove = 3 };

// una op parseada (para poder aplicarlas al reves al invertir)
struct OpDelta { unsigned char op; unsigned a, b; };

// parsea el registro de una celda y AVANZA el cursor hasta su fin.
// isKey/nWords/hasOrder describen que tipo de registro se espera.
// Si 'lista' != NULL, ademas lo decodifica (bitset -> ascendente; clave ->
// la lista; delta -> deja las ops en 'ops' para que el caller las aplique).
bool ParseRegistry(Cur& c, bool isBitset, bool isKey, int nTris, bool hasOrder,
                     std::vector<unsigned>* list, std::vector<OpDelta>* ops,
                     std::vector<unsigned>* order) {
    if (isBitset) {
        const unsigned nWords = (unsigned)((nTris + 31) / 32);
        unsigned done = 0;
        while (done < nWords && c.ok) {
            unsigned ctrl = ReadVarint(c);
            unsigned len = ctrl >> 1;
            if (!c.ok || len == 0 || done + len > nWords) { c.ok = false; break; }
            if (ctrl & 1) {                       // REPETIDA: 1 palabra, len veces
                unsigned w = ReadU32(c);
                if (list && w) for (unsigned k = 0; k < len; k++) {
                    unsigned base = (done + k) * 32;
                    for (int b = 0; b < 32; b++)
                        if ((w >> b) & 1u) { unsigned t = base + (unsigned)b; if ((int)t < nTris) list->push_back(t); }
                }
                done += len;
            } else {                              // LITERAL: len palabras crudas
                for (unsigned k = 0; k < len && c.ok; k++) {
                    unsigned w = ReadU32(c);
                    if (list && w) {
                        unsigned base = (done + k) * 32;
                        for (int b = 0; b < 32; b++)
                            if ((w >> b) & 1u) { unsigned t = base + (unsigned)b; if ((int)t < nTris) list->push_back(t); }
                    }
                }
                done += len;
            }
        }
        if (c.ok && hasOrder) {                   // stream opcional de orden de transparentes
            unsigned nOrd = ReadVarint(c);
            if (nOrd > (unsigned)nTris) { c.ok = false; }
            for (unsigned k = 0; k < nOrd && c.ok; k++) {
                unsigned id = ReadVarint(c);
                if (order) order->push_back(id);
            }
        }
        return c.ok;
    }
    if (isKey) {                                // lista completa, zigzag delta-encoded
        unsigned n = ReadVarint(c);
        if (!c.ok || n > (unsigned)nTris) { c.ok = false; return false; }
        int prev = 0;
        for (unsigned k = 0; k < n && c.ok; k++) {
            int d = FromZigzag(ReadVarint(c));
            int id = (k == 0) ? d : prev + d;
            prev = id;
            if (id < 0 || id >= nTris) { c.ok = false; break; }
            if (list) list->push_back((unsigned)id);
        }
        return c.ok;
    }
    // registro DELTA: ops hasta FIN
    unsigned limit = 0;
    while (c.ok) {
        if (c.i >= c.n || ++limit > (unsigned)nTris * 4u + 16u) { c.ok = false; break; }
        unsigned char op = c.p[c.i++];
        if (op == OpEnd) break;
        if (op > OpMove) { c.ok = false; break; }
        OpDelta o; o.op = op;
        o.a = ReadVarint(c);
        o.b = ReadVarint(c);
        if (ops && c.ok) ops->push_back(o);
    }
    return c.ok;
}

// aplica UNA op (o su inversa) sobre la lista. false = stream corrupto
// (posicion fuera de rango o REMOVE cuyo id no coincide).
bool ApplyOp(std::vector<unsigned>& list, const OpDelta& o, bool invert) {
    unsigned char op = o.op;
    unsigned a = o.a, b = o.b;
    if (invert) {
        if (op == OpAdd) op = OpRemove;           // ADD(pos,id) <-> REMOVE(pos,id)
        else if (op == OpRemove) op = OpAdd;
        else { unsigned t = a; a = b; b = t; }    // MOVE(desde,hacia) <-> MOVE(hacia,desde)
    }
    if (op == OpAdd) {
        if (a > list.size()) return false;
        list.insert(list.begin() + a, b);
        return true;
    }
    if (op == OpRemove) {
        if (a >= list.size() || list[a] != b) return false;
        list.erase(list.begin() + a);
        return true;
    }
    // MOVE: sacar de 'a', insertar en 'b' (b medido en la lista YA sin el)
    if (a >= list.size()) return false;
    unsigned id = list[a];
    list.erase(list.begin() + a);
    if (b > list.size()) return false;
    list.insert(list.begin() + b, id);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
void VisSet::Clear() {
    nTris = 0; nCells = 0; mode = ModeBitset;
    orderFarToNear = false; hasOrder = false;
    K = 0; hashTopo = 0;
    blob.clear(); table.clear();
}

// ---------------------------------------------------------------------------
//  CARGA del binario (valida TODO el archivo una vez y canonicaliza el blob)
// ---------------------------------------------------------------------------
bool VisSet::LoadW3dvis(const unsigned char* bytes, size_t n, std::string* err) {
    Clear();
    if (!bytes || n < 32) { if (err) *err = "archivo demasiado corto"; return false; }
    if (bytes[0] != 'W' || bytes[1] != '3' || bytes[2] != 'V' || bytes[3] != 'S') {
        if (err) *err = "magic invalido (no es un .w3dvis)"; return false;
    }
    Cur h(bytes, n, 4);
    unsigned version = (unsigned)bytes[4] | ((unsigned)bytes[5] << 8);
    unsigned modeF   = (unsigned)bytes[6] | ((unsigned)bytes[7] << 8);
    h.i = 8;
    unsigned flags    = ReadU32(h);
    unsigned nTrisF   = ReadU32(h);
    unsigned nCellsF  = ReadU32(h);
    unsigned KF       = ReadU32(h);
    unsigned offTable = ReadU32(h);
    unsigned hashF    = ReadU32(h);
    if (version != 1) { if (err) *err = "version desconocida"; return false; }
    if (modeF > 1)    { if (err) *err = "modo desconocido"; return false; }
    if (nTrisF == 0 || nCellsF == 0 || nTrisF > 0x7FFFFFFFu) { if (err) *err = "header vacio"; return false; }
    if (nCellsF > 1000000u) { if (err) *err = "nCeldas absurdo"; return false; }
    if (modeF == 1 && KF < 1) { if (err) *err = "playlist con K < 1"; return false; }
    if ((size_t)offTable + (size_t)nCellsF * 4 > n) { if (err) *err = "tabla fuera del archivo"; return false; }

    nTris = (int)nTrisF; nCells = (int)nCellsF; mode = (int)modeF;
    orderFarToNear = (flags & 1u) != 0;
    hasOrder       = (flags & 2u) != 0 && mode == ModeBitset;
    K = (mode == ModePlaylist) ? (int)KF : 0;
    hashTopo = hashF;

    table.resize((size_t)nCells);
    blob.clear();
    blob.reserve(n > 32 ? n - 32 : 0);
    for (int c = 0; c < nCells; c++) {
        Cur t(bytes, n, (size_t)offTable + (size_t)c * 4);
        unsigned off = ReadU32(t);
        if (!t.ok || off < 32 || off >= n) { Clear(); if (err) *err = "offset de celda invalido"; return false; }
        Cur r(bytes, n, off);
        bool isKey = (mode == ModePlaylist) && (c % K == 0);
        if (!ParseRegistry(r, mode == ModeBitset, isKey, nTris, hasOrder, 0, 0, 0) || !r.ok) {
            Clear(); if (err) *err = "registro de celda corrupto"; return false;
        }
        table[c] = (unsigned)blob.size();
        blob.insert(blob.end(), bytes + off, bytes + r.i);   // canonicaliza
    }
    return true;
}

// ---------------------------------------------------------------------------
//  construccion desde listas (legacy .pvs.json -> bitset; horneador/pruebas
//  -> playlist)
// ---------------------------------------------------------------------------
bool VisSet::FromLists(const std::vector< std::vector<int> >& lists, int nTrisMesh) {
    Clear();
    if (lists.empty() || nTrisMesh <= 0) return false;
    nTris = nTrisMesh; nCells = (int)lists.size(); mode = ModeBitset;
    const unsigned nWords = (unsigned)((nTris + 31) / 32);
    std::vector<unsigned> words(nWords);
    table.resize((size_t)nCells);
    for (int c = 0; c < nCells; c++) {
        for (unsigned w = 0; w < nWords; w++) words[w] = 0;
        const std::vector<int>& l = lists[c];
        for (size_t k = 0; k < l.size(); k++) {
            int t = l[k];
            if (t >= 0 && t < nTris) words[t / 32] |= 1u << (t % 32);
        }
        table[c] = (unsigned)blob.size();
        // RLE: corridas de palabras repetidas -> (len<<1)|1 + palabra; el resto
        // literales agrupadas -> (len<<1) + palabras crudas
        unsigned i = 0;
        while (i < nWords) {
            unsigned j = i + 1;
            while (j < nWords && words[j] == words[i]) j++;
            unsigned run = j - i;
            if (run >= 2) {
                SetVarint(blob, (run << 1) | 1u);
                SetU32(blob, words[i]);
                i = j;
            } else {
                unsigned start = i;                  // juntar literales hasta la proxima corrida
                while (j < nWords) {
                    unsigned j2 = j + 1;
                    while (j2 < nWords && words[j2] == words[j]) j2++;
                    if (j2 - j >= 2) break;
                    j = j2;
                }
                SetVarint(blob, (j - start) << 1);
                for (unsigned k = start; k < j; k++) SetU32(blob, words[k]);
                i = j;
            }
        }
    }
    return true;
}

bool VisSet::FromListsPlaylist(const std::vector< std::vector<int> >& lists,
                                 int nTrisMesh, int stepK, bool sorted) {
    Clear();
    if (lists.empty() || nTrisMesh <= 0 || stepK < 1) return false;
    nTris = nTrisMesh; nCells = (int)lists.size(); mode = ModePlaylist;
    K = stepK; orderFarToNear = sorted;
    table.resize((size_t)nCells);
    std::vector<unsigned> prev, cur, work;
    for (int c = 0; c < nCells; c++) {
        cur.clear();
        for (size_t k = 0; k < lists[c].size(); k++) {
            int t = lists[c][k];
            if (t >= 0 && t < nTris) cur.push_back((unsigned)t);
        }
        table[c] = (unsigned)blob.size();
        if (c % K == 0) {
            // celda CLAVE: lista completa zigzag delta-encoded
            SetVarint(blob, (unsigned)cur.size());
            int prevId = 0;
            for (size_t k = 0; k < cur.size(); k++) {
                int id = (int)cur[k];
                SetVarint(blob, Zigzag(k == 0 ? id : id - prevId));
                prevId = id;
            }
        } else {
            // registro DELTA: diff posicional prev -> cur (correcto; minimal no
            // hace falta: esto lo usan el horneador y las pruebas)
            work = prev;
            // 1) REMOVE de atras hacia adelante de todo lo que no sigue
            std::vector<bool> inCur((size_t)nTris, false);
            for (size_t k = 0; k < cur.size(); k++) inCur[cur[k]] = true;
            for (int p = (int)work.size() - 1; p >= 0; p--) {
                if (!inCur[work[p]]) {
                    blob.push_back((unsigned char)OpRemove);
                    SetVarint(blob, (unsigned)p);
                    SetVarint(blob, work[p]);
                    work.erase(work.begin() + p);
                }
            }
            // 2) MOVE/ADD hasta que work == cur
            for (size_t i = 0; i < cur.size(); i++) {
                if (i < work.size() && work[i] == cur[i]) continue;
                size_t j = i + 1; bool is = false;
                for (; j < work.size(); j++) if (work[j] == cur[i]) { is = true; break; }
                if (is) {
                    unsigned id = work[j];
                    work.erase(work.begin() + j);
                    blob.push_back((unsigned char)OpMove);
                    SetVarint(blob, (unsigned)j);
                    SetVarint(blob, (unsigned)i);
                    work.insert(work.begin() + i, id);
                } else {
                    blob.push_back((unsigned char)OpAdd);
                    SetVarint(blob, (unsigned)i);
                    SetVarint(blob, cur[i]);
                    work.insert(work.begin() + i, cur[i]);
                }
            }
            blob.push_back((unsigned char)OpEnd);
        }
        prev.swap(cur);
    }
    return true;
}

// ---------------------------------------------------------------------------
bool VisSet::Save(std::vector<unsigned char>& out) const {
    out.clear();
    if (!Valid()) return false;
    out.push_back('W'); out.push_back('3'); out.push_back('V'); out.push_back('S');
    out.push_back(1); out.push_back(0);                       // version u16
    out.push_back((unsigned char)mode); out.push_back(0);     // modo u16
    unsigned flags = (orderFarToNear ? 1u : 0u) | (hasOrder ? 2u : 0u);
    SetU32(out, flags);
    SetU32(out, (unsigned)nTris);
    SetU32(out, (unsigned)nCells);
    SetU32(out, (unsigned)((mode == ModePlaylist) ? K : 0));
    SetU32(out, 32u + (unsigned)blob.size());               // offTable: tabla tras el blob
    SetU32(out, hashTopo);
    out.insert(out.end(), blob.begin(), blob.end());
    for (int c = 0; c < nCells; c++) SetU32(out, 32u + table[c]);
    return true;
}

// ---------------------------------------------------------------------------
//  consumo
// ---------------------------------------------------------------------------
bool VisSet::Decode(int cell, std::vector<unsigned>& list) const {
    list.clear();
    if (!Valid() || cell < 0 || cell >= nCells) return false;
    if (mode == ModeBitset) {
        Cur r(&blob[0], blob.size(), table[cell]);
        return ParseRegistry(r, true, false, nTris, hasOrder, &list, 0, 0);
    }
    // playlist: desde la celda-clave anterior, aplicando los deltas intermedios
    int kc = cell - (cell % K);
    {
        Cur r(&blob[0], blob.size(), table[kc]);
        if (!ParseRegistry(r, false, true, nTris, false, &list, 0, 0)) return false;
    }
    for (int c = kc + 1; c <= cell; c++)
        if (!ApplyDelta(c, list, false)) return false;
    return true;
}

bool VisSet::ApplyDelta(int cell, std::vector<unsigned>& list, bool invert) const {
    if (cell <= 0 || cell >= nCells || (cell % K) == 0) return false;
    std::vector<OpDelta> ops;
    Cur r(&blob[0], blob.size(), table[cell]);
    if (!ParseRegistry(r, false, false, nTris, false, 0, &ops, 0)) return false;
    if (!invert) {
        for (size_t k = 0; k < ops.size(); k++)
            if (!ApplyOp(list, ops[k], false)) return false;
    } else {
        for (size_t k = ops.size(); k > 0; k--)
            if (!ApplyOp(list, ops[k - 1], true)) return false;
    }
    return true;
}

bool VisSet::Delta(int from, int towards, std::vector<unsigned>& list) const {
    if (!Valid() || from < 0 || from >= nCells || towards < 0 || towards >= nCells)
        return false;
    if (from == towards) return true;
    if (mode == ModePlaylist && towards == from + 1) {
        if (towards % K == 0) return Decode(towards, list);   // clave: lista completa
        return ApplyDelta(towards, list, false);
    }
    if (mode == ModePlaylist && towards == from - 1) {
        if (from % K == 0) return Decode(towards, list);   // una clave no se invierte
        return ApplyDelta(from, list, true);               // registro de 'desde' al reves
    }
    return Decode(towards, list);                           // salto largo / bitset
}

bool VisSet::OrderTransparent(int cell, std::vector<unsigned>& order) const {
    order.clear();
    if (!Valid() || mode != ModeBitset || !hasOrder) return false;
    if (cell < 0 || cell >= nCells) return false;
    Cur r(&blob[0], blob.size(), table[cell]);
    return ParseRegistry(r, true, false, nTris, true, 0, 0, &order);
}

// ---------------------------------------------------------------------------
//  hash de topologia (contrato del spec: FNV-1a de nTris + faces como u32 LE)
// ---------------------------------------------------------------------------
static unsigned FnvU32(unsigned h, unsigned x) {
    for (int b = 0; b < 4; b++) { h = (h ^ ((x >> (b * 8)) & 0xFF)) * 16777619u; }
    return h;
}
unsigned VisSet::HashTopology(int nTris, const unsigned int* faces, int nIndices) {
    unsigned h = 2166136261u;
    h = FnvU32(h, (unsigned)nTris);
    for (int i = 0; i < nIndices; i++) h = FnvU32(h, faces[i]);
    return h;
}
unsigned VisSet::HashTopology16(int nTris, const unsigned short* faces, int nIndices) {
    unsigned h = 2166136261u;
    h = FnvU32(h, (unsigned)nTris);
    for (int i = 0; i < nIndices; i++) h = FnvU32(h, (unsigned)faces[i]);
    return h;
}
