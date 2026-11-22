// ============================================================================
//  W3dNetValue.h — los VALORES de los mensajes de la red en C++, sin Lua: el mismo formato en bytes que arman
//  redEnviar / redRecibir (W3dNet.cpp), para que un juego hecho en C++ (la Dreamcast) hable con uno en Lua.
//
//  Un valor es nil, un booleano, un numero (entero de 1, 2 o 4 bytes, o float), un texto o una tabla (pares clave,
//  valor, en cualquier orden: las listas de Lua son las claves 1..n).
//
//  Escribir, en orden (una tabla dice cuantos pares siguen y despues van clave, valor, clave, valor...):
//      w3dNet::WriterValue e;
//      e.Table(2); e.Text("t"); e.Text("listo"); e.Text("si"); e.Bool(true);     // {t = "listo", si = true}
//      w3dNet::Send(-1, e.Data(), e.Bytes(), true);
//  Leer: cada valor es un numero (0 = el de afuera); los de una tabla se piden por su clave:
//      w3dNet::ReaderValue v;
//      if (v.Read(datos.data(), (int)datos.size()) && v.IsText(v.Field(0, "t"), "e")) {
//          const int f = v.Field(0, "f");
//          float t = v.Number(v.Element(f, 2));   // f[2]
//      }
//  Un valor que no esta es -1: Number(-1) da el defecto, Text(-1) "", Field(-1, ...) -1 (se puede encadenar).
//
//  C++03 (Symbian y la Dreamcast lo compilan).
// ============================================================================
#ifndef W3D_NET_VALUE_H
#define W3D_NET_VALUE_H

#include <string>
#include <vector>

namespace w3dNet {

// los tipos en los bytes (el primero de cada valor)
enum ValueType { V_NIL = 0, V_FALSE, V_TRUE, V_I8, V_I16, V_I32, V_F32, V_TEXT, V_TABLE, V_F64 };

class WriterValue {
public:
    void Nil();
    void Bool(bool yes);
    void Integer(int v);             // (el mas chico de 1, 2 o 4 bytes, como un entero de Lua)
    void Number(float v);            // (un float de Lua)
    void Text(const char* s);
    void Text(const std::string& s);
    void Table(unsigned pairs);      // siguen 'pares' claves con su valor
    const void* Data() const { return b.data(); }
    int Bytes() const { return (int)b.size(); }
    void Empty() { b.clear(); }
private:
    std::string b;
    void u8(unsigned v) { b += (char)(v & 0xFF); }
    void u32(unsigned v) { u8(v); u8(v >> 8); u8(v >> 16); u8(v >> 24); }
};

class ReaderValue {
public:
    enum Type { NIL = 0, BOOL, NUMBER, TEXT, TABLE };
    // false si los bytes estan mal (lo que se leyo hasta ahi no sirve)
    bool Read(const void* data, int bytes);
    Type TypeOf(int v) const;
    bool Bool(int v) const;                          // false: falso, nil o no esta
    float Number(int v, float defaultValue = 0) const;
    int Integer(int v, int defaultValue = 0) const;
    const char* Text(int v) const;                  // "" si no es un texto
    bool IsText(int v, const char* s) const;
    // en una tabla: el valor de la clave 'clave' (texto) o 'i' (numero), -1 si no esta
    int Field(int table, const char* key) const;
    int Element(int table, int i) const;
    // recorrer una tabla: sus pares (la clave y el valor del k-esimo)
    int Pairs(int table) const;
    int KeyOf(int table, int k) const;
    int ValueOf(int table, int k) const;
private:
    struct Node {
        unsigned char type;          // Tipo
        bool yes;
        float num;
        std::string txt;
        int from, pairs;            // TABLA: sus pares en 'pares_'
    };
    std::vector<Node> nodes_;
    std::vector<int> pairs_;         // clave, valor, clave, valor...
    int ReadNode(const unsigned char*& p, const unsigned char* end, int prof);
};

} // namespace w3dNet

#endif // W3D_NET_VALUE_H
