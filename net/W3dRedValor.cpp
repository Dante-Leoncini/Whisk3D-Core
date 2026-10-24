// ============================================================================
//  W3dRedValor.cpp — ver W3dRedValor.h. El formato es el de EscribirValor / LeerValor de W3dRed.cpp (los binds de
//  Lua): un byte de tipo y despues el valor, en little endian.
// ============================================================================
#include "W3dRedValor.h"
#include <string.h>

namespace w3dRed {

// ------------------------------------------------------------------------------------------------- escribir
void EscritorValor::Nil() { u8(V_NIL); }
void EscritorValor::Bool(bool si) { u8(si ? V_VERDADERO : V_FALSO); }
void EscritorValor::Entero(int v) {
    if (v >= -128 && v <= 127) { u8(V_I8); u8((unsigned)v); return; }
    if (v >= -32768 && v <= 32767) { u8(V_I16); u8((unsigned)v); u8((unsigned)v >> 8); return; }
    u8(V_I32);
    u32((unsigned)v);
}
void EscritorValor::Numero(float v) {
    unsigned u;
    memcpy(&u, &v, 4);
    u8(V_F32);
    u32(u);
}
void EscritorValor::Texto(const char* s) {
    const unsigned n = s ? (unsigned)strlen(s) : 0;
    u8(V_TEXTO);
    u32(n);
    if (n) b.append(s, n);
}
void EscritorValor::Texto(const std::string& s) {
    u8(V_TEXTO);
    u32((unsigned)s.size());
    b += s;
}
void EscritorValor::Tabla(unsigned pares) {
    u8(V_TABLA);
    u32(pares);
}

// ---------------------------------------------------------------------------------------------------- leer
namespace {
inline unsigned U32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
}

bool LectorValor::Leer(const void* datos, int bytes) {
    nodos_.clear();
    pares_.clear();
    const unsigned char* p = (const unsigned char*)datos;
    return bytes > 0 && LeerNodo(p, p + bytes, 0) == 0;
}

// devuelve el numero del nodo (-1 si los bytes estan mal); los de una tabla quedan seguidos en pares_
int LectorValor::LeerNodo(const unsigned char*& p, const unsigned char* fin, int prof) {
    if (prof > 24 || p >= fin) return -1;
    const int yo = (int)nodos_.size();
    nodos_.push_back(Nodo());
    Nodo n;
    n.tipo = NIL; n.si = false; n.num = 0; n.desde = 0; n.pares = 0;
    const unsigned t = *p++;
    switch (t) {
    case V_NIL: break;
    case V_FALSO: n.tipo = BOOL; break;
    case V_VERDADERO: n.tipo = BOOL; n.si = true; break;
    case V_I8:
        if (p + 1 > fin) return -1;
        n.tipo = NUMERO; n.num = (float)(signed char)p[0]; p += 1;
        break;
    case V_I16:
        if (p + 2 > fin) return -1;
        n.tipo = NUMERO; n.num = (float)(short)(p[0] | (p[1] << 8)); p += 2;
        break;
    case V_I32:
        if (p + 4 > fin) return -1;
        n.tipo = NUMERO; n.num = (float)(int)U32(p); p += 4;
        break;
    case V_F32: {
        if (p + 4 > fin) return -1;
        const unsigned u = U32(p);
        memcpy(&n.num, &u, 4);
        n.tipo = NUMERO; p += 4;
        break;
    }
    case V_F64: {
        if (p + 8 > fin) return -1;
        double d;
        memcpy(&d, p, 8);
        n.tipo = NUMERO; n.num = (float)d; p += 8;
        break;
    }
    case V_TEXTO: {
        if (p + 4 > fin) return -1;
        const unsigned largo = U32(p);
        p += 4;
        if (largo > (unsigned)(fin - p)) return -1;
        n.tipo = TEXTO;
        n.txt.assign((const char*)p, largo);
        p += largo;
        break;
    }
    case V_TABLA: {
        if (p + 4 > fin) return -1;
        const unsigned cuantos = U32(p);
        p += 4;
        if (cuantos > (unsigned)(fin - p)) return -1;
        // (los pares se juntan aca y van seguidos a pares_ al final: las tablas de adentro agregan los suyos antes)
        std::vector<int> mios;
        mios.reserve(cuantos * 2);
        for (unsigned k = 0; k < cuantos; k++) {
            const int c = LeerNodo(p, fin, prof + 1);
            if (c < 0) return -1;
            const int v = LeerNodo(p, fin, prof + 1);
            if (v < 0) return -1;
            mios.push_back(c);
            mios.push_back(v);
        }
        n.tipo = TABLA;
        n.desde = (int)pares_.size();
        n.pares = (int)cuantos;
        pares_.insert(pares_.end(), mios.begin(), mios.end());
        break;
    }
    default: return -1;
    }
    nodos_[yo] = n;
    return yo;
}

LectorValor::Tipo LectorValor::TipoDe(int v) const {
    return v >= 0 && v < (int)nodos_.size() ? (Tipo)nodos_[v].tipo : NIL;
}
bool LectorValor::Bool(int v) const { return TipoDe(v) == BOOL && nodos_[v].si; }
float LectorValor::Numero(int v, float defecto) const { return TipoDe(v) == NUMERO ? nodos_[v].num : defecto; }
int LectorValor::Entero(int v, int defecto) const {
    if (TipoDe(v) != NUMERO) return defecto;
    const float f = nodos_[v].num;
    return (int)(f < 0 ? f - 0.5f : f + 0.5f);
}
const char* LectorValor::Texto(int v) const { return TipoDe(v) == TEXTO ? nodos_[v].txt.c_str() : ""; }
bool LectorValor::EsTexto(int v, const char* s) const { return TipoDe(v) == TEXTO && nodos_[v].txt == s; }

int LectorValor::Campo(int tabla, const char* clave) const {
    if (TipoDe(tabla) != TABLA) return -1;
    const Nodo& t = nodos_[tabla];
    for (int k = 0; k < t.pares; k++) {
        const int c = pares_[t.desde + 2 * k];
        if (EsTexto(c, clave)) return pares_[t.desde + 2 * k + 1];
    }
    return -1;
}
int LectorValor::Elemento(int tabla, int i) const {
    if (TipoDe(tabla) != TABLA) return -1;
    const Nodo& t = nodos_[tabla];
    for (int k = 0; k < t.pares; k++) {
        const int c = pares_[t.desde + 2 * k];
        if (TipoDe(c) == NUMERO && nodos_[c].num == (float)i) return pares_[t.desde + 2 * k + 1];
    }
    return -1;
}
int LectorValor::Pares(int tabla) const { return TipoDe(tabla) == TABLA ? nodos_[tabla].pares : 0; }
int LectorValor::ClaveDe(int tabla, int k) const {
    return k >= 0 && k < Pares(tabla) ? pares_[nodos_[tabla].desde + 2 * k] : -1;
}
int LectorValor::ValorDe(int tabla, int k) const {
    return k >= 0 && k < Pares(tabla) ? pares_[nodos_[tabla].desde + 2 * k + 1] : -1;
}

} // namespace w3dRed
