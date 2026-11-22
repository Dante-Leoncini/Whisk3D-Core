// ============================================================================
//  W3dNetValue.cpp — ver W3dNetValue.h. El formato es el de WriteValue / ReadValue de W3dNet.cpp (los binds de
//  Lua): un byte de tipo y despues el valor, en little endian.
// ============================================================================
#include "W3dNetValue.h"
#include <string.h>

namespace w3dNet {

// ------------------------------------------------------------------------------------------------- escribir
void WriterValue::Nil() { u8(V_NIL); }
void WriterValue::Bool(bool yes) { u8(yes ? V_TRUE : V_FALSE); }
void WriterValue::Integer(int v) {
    if (v >= -128 && v <= 127) { u8(V_I8); u8((unsigned)v); return; }
    if (v >= -32768 && v <= 32767) { u8(V_I16); u8((unsigned)v); u8((unsigned)v >> 8); return; }
    u8(V_I32);
    u32((unsigned)v);
}
void WriterValue::Number(float v) {
    unsigned u;
    memcpy(&u, &v, 4);
    u8(V_F32);
    u32(u);
}
void WriterValue::Text(const char* s) {
    const unsigned n = s ? (unsigned)strlen(s) : 0;
    u8(V_TEXT);
    u32(n);
    if (n) b.append(s, n);
}
void WriterValue::Text(const std::string& s) {
    u8(V_TEXT);
    u32((unsigned)s.size());
    b += s;
}
void WriterValue::Table(unsigned pairs) {
    u8(V_TABLE);
    u32(pairs);
}

// ---------------------------------------------------------------------------------------------------- leer
namespace {
inline unsigned U32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
}

bool ReaderValue::Read(const void* data, int bytes) {
    nodes_.clear();
    pairs_.clear();
    const unsigned char* p = (const unsigned char*)data;
    return bytes > 0 && ReadNode(p, p + bytes, 0) == 0;
}

// devuelve el numero del nodo (-1 si los bytes estan mal); los de una tabla quedan seguidos en pares_
int ReaderValue::ReadNode(const unsigned char*& p, const unsigned char* end, int prof) {
    if (prof > 24 || p >= end) return -1;
    const int yo = (int)nodes_.size();
    nodes_.push_back(Node());
    Node n;
    n.type = NIL; n.yes = false; n.num = 0; n.from = 0; n.pairs = 0;
    const unsigned t = *p++;
    switch (t) {
    case V_NIL: break;
    case V_FALSE: n.type = BOOL; break;
    case V_TRUE: n.type = BOOL; n.yes = true; break;
    case V_I8:
        if (p + 1 > end) return -1;
        n.type = NUMBER; n.num = (float)(signed char)p[0]; p += 1;
        break;
    case V_I16:
        if (p + 2 > end) return -1;
        n.type = NUMBER; n.num = (float)(short)(p[0] | (p[1] << 8)); p += 2;
        break;
    case V_I32:
        if (p + 4 > end) return -1;
        n.type = NUMBER; n.num = (float)(int)U32(p); p += 4;
        break;
    case V_F32: {
        if (p + 4 > end) return -1;
        const unsigned u = U32(p);
        memcpy(&n.num, &u, 4);
        n.type = NUMBER; p += 4;
        break;
    }
    case V_F64: {
        if (p + 8 > end) return -1;
        double d;
        memcpy(&d, p, 8);
        n.type = NUMBER; n.num = (float)d; p += 8;
        break;
    }
    case V_TEXT: {
        if (p + 4 > end) return -1;
        const unsigned length = U32(p);
        p += 4;
        if (length > (unsigned)(end - p)) return -1;
        n.type = TEXT;
        n.txt.assign((const char*)p, length);
        p += length;
        break;
    }
    case V_TABLE: {
        if (p + 4 > end) return -1;
        const unsigned count = U32(p);
        p += 4;
        if (count > (unsigned)(end - p)) return -1;
        // (los pares se juntan aca y van seguidos a pares_ al final: las tablas de adentro agregan los suyos antes)
        std::vector<int> mine;
        mine.reserve(count * 2);
        for (unsigned k = 0; k < count; k++) {
            const int c = ReadNode(p, end, prof + 1);
            if (c < 0) return -1;
            const int v = ReadNode(p, end, prof + 1);
            if (v < 0) return -1;
            mine.push_back(c);
            mine.push_back(v);
        }
        n.type = TABLE;
        n.from  = (int)pairs_.size();
        n.pairs = (int)count;
        pairs_.insert(pairs_.end(), mine.begin(), mine.end());
        break;
    }
    default: return -1;
    }
    nodes_[yo] = n;
    return yo;
}

ReaderValue::Type ReaderValue::TypeOf(int v) const {
    return v >= 0 && v < (int)nodes_.size() ? (Type)nodes_[v].type : NIL;
}
bool ReaderValue::Bool(int v) const { return TypeOf(v) == BOOL && nodes_[v].yes; }
float ReaderValue::Number(int v, float defaultValue) const { return TypeOf(v) == NUMBER ? nodes_[v].num : defaultValue; }
int ReaderValue::Integer(int v, int defaultValue) const {
    if (TypeOf(v) != NUMBER) return defaultValue;
    const float f = nodes_[v].num;
    return (int)(f < 0 ? f - 0.5f : f + 0.5f);
}
const char* ReaderValue::Text(int v) const { return TypeOf(v) == TEXT ? nodes_[v].txt.c_str() : ""; }
bool ReaderValue::IsText(int v, const char* s) const { return TypeOf(v) == TEXT && nodes_[v].txt == s; }

int ReaderValue::Field(int table, const char* key) const {
    if (TypeOf(table) != TABLE) return -1;
    const Node& t = nodes_[table];
    for (int k = 0; k < t.pairs; k++) {
        const int c = pairs_[t.from + 2 * k];
        if (IsText(c, key)) return pairs_[t.from + 2 * k + 1];
    }
    return -1;
}
int ReaderValue::Element(int table, int i) const {
    if (TypeOf(table) != TABLE) return -1;
    const Node& t = nodes_[table];
    for (int k = 0; k < t.pairs; k++) {
        const int c = pairs_[t.from + 2 * k];
        if (TypeOf(c) == NUMBER && nodes_[c].num == (float)i) return pairs_[t.from + 2 * k + 1];
    }
    return -1;
}
int ReaderValue::Pairs(int table) const { return TypeOf(table) == TABLE ? nodes_[table].pairs : 0; }
int ReaderValue::KeyOf(int table, int k) const {
    return k >= 0 && k < Pairs(table) ? pairs_[nodes_[table].from + 2 * k] : -1;
}
int ReaderValue::ValueOf(int table, int k) const {
    return k >= 0 && k < Pairs(table) ? pairs_[nodes_[table].from + 2 * k + 1] : -1;
}

} // namespace w3dNet
