// ============================================================================
//  W3dRedValor.h — los VALORES de los mensajes de la red en C++, sin Lua: el mismo formato en bytes que arman
//  redEnviar / redRecibir (W3dRed.cpp), para que un juego hecho en C++ (la Dreamcast) hable con uno en Lua.
//
//  Un valor es nil, un booleano, un numero (entero de 1, 2 o 4 bytes, o float), un texto o una tabla (pares clave,
//  valor, en cualquier orden: las listas de Lua son las claves 1..n).
//
//  Escribir, en orden (una tabla dice cuantos pares siguen y despues van clave, valor, clave, valor...):
//      w3dRed::EscritorValor e;
//      e.Tabla(2); e.Texto("t"); e.Texto("listo"); e.Texto("si"); e.Bool(true);     // {t = "listo", si = true}
//      w3dRed::Enviar(-1, e.Datos(), e.Bytes(), true);
//  Leer: cada valor es un numero (0 = el de afuera); los de una tabla se piden por su clave:
//      w3dRed::LectorValor v;
//      if (v.Leer(datos.data(), (int)datos.size()) && v.EsTexto(v.Campo(0, "t"), "e")) {
//          const int f = v.Campo(0, "f");
//          float t = v.Numero(v.Elemento(f, 2));   // f[2]
//      }
//  Un valor que no esta es -1: Numero(-1) da el defecto, Texto(-1) "", Campo(-1, ...) -1 (se puede encadenar).
//
//  C++03 (Symbian y la Dreamcast lo compilan).
// ============================================================================
#ifndef W3D_RED_VALOR_H
#define W3D_RED_VALOR_H

#include <string>
#include <vector>

namespace w3dRed {

// los tipos en los bytes (el primero de cada valor)
enum TipoValor { V_NIL = 0, V_FALSO, V_VERDADERO, V_I8, V_I16, V_I32, V_F32, V_TEXTO, V_TABLA, V_F64 };

class EscritorValor {
public:
    void Nil();
    void Bool(bool si);
    void Entero(int v);              // (el mas chico de 1, 2 o 4 bytes, como un entero de Lua)
    void Numero(float v);            // (un float de Lua)
    void Texto(const char* s);
    void Texto(const std::string& s);
    void Tabla(unsigned pares);      // siguen 'pares' claves con su valor
    const void* Datos() const { return b.data(); }
    int Bytes() const { return (int)b.size(); }
    void Vaciar() { b.clear(); }
private:
    std::string b;
    void u8(unsigned v) { b += (char)(v & 0xFF); }
    void u32(unsigned v) { u8(v); u8(v >> 8); u8(v >> 16); u8(v >> 24); }
};

class LectorValor {
public:
    enum Tipo { NIL = 0, BOOL, NUMERO, TEXTO, TABLA };
    // false si los bytes estan mal (lo que se leyo hasta ahi no sirve)
    bool Leer(const void* datos, int bytes);
    Tipo TipoDe(int v) const;
    bool Bool(int v) const;                          // false: falso, nil o no esta
    float Numero(int v, float defecto = 0) const;
    int Entero(int v, int defecto = 0) const;
    const char* Texto(int v) const;                  // "" si no es un texto
    bool EsTexto(int v, const char* s) const;
    // en una tabla: el valor de la clave 'clave' (texto) o 'i' (numero), -1 si no esta
    int Campo(int tabla, const char* clave) const;
    int Elemento(int tabla, int i) const;
    // recorrer una tabla: sus pares (la clave y el valor del k-esimo)
    int Pares(int tabla) const;
    int ClaveDe(int tabla, int k) const;
    int ValorDe(int tabla, int k) const;
private:
    struct Nodo {
        unsigned char tipo;          // Tipo
        bool si;
        float num;
        std::string txt;
        int desde, pares;            // TABLA: sus pares en 'pares_'
    };
    std::vector<Nodo> nodos_;
    std::vector<int> pares_;         // clave, valor, clave, valor...
    int LeerNodo(const unsigned char*& p, const unsigned char* fin, int prof);
};

} // namespace w3dRed

#endif // W3D_RED_VALOR_H
