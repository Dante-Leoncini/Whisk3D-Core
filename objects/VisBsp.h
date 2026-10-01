#ifndef W3D_VISBSP_H
#define W3D_VISBSP_H
// ============================================================================
//  VisBsp — visibilidad precalculada por ARBOL BSP + PVS (el esquema de Quake /
//  Half-Life): el dato dice en que HOJA cae un punto, a que CLUSTER pertenece
//  cada hoja, que clusters se ven desde cada cluster (PVS) y que clusters ocupa
//  cada HIJO del Culling (por nombre). Con eso el Culling (main/objects/Culling.cpp,
//  metodo Bsp) resuelve "que hijos dibujo" con una bajada por el arbol y un AND de
//  bitsets: cero geometria, cero rayos. Quien escribe el dato es un conversor
//  externo (el de HL2 lo saca del .bsp del juego) o, a futuro, un horneador del
//  editor; el motor NO sabe de donde salio.
//
//  Formato en disco: `.w3dbsp` (binario LE, spec en formato/w3dbsp.md, version 1).
//  C++03 / Symbian-safe. Sin nada de ningun juego.
// ============================================================================
#include <vector>
#include <string>

class VisBsp {
public:
    struct Nodo { float a, b, c, d; int hijo0, hijo1; };   // a x + b y + c z - d >= 0 -> hijo0; hijo < 0 = hoja -(hijo+1)
    struct Hijo { std::string nombre; std::vector<unsigned char> bits; };   // los clusters que ocupa (bit c)

    std::vector<Nodo> nodos;
    std::vector<int>  hojaCluster;   // por hoja: su cluster (-1 = solido / sin PVS)
    int               nClusters;
    int               bytesFila;     // (nClusters + 7) / 8
    bool              rle;           // el PVS viene comprimido (byte 0 + cantidad = ceros)
    std::vector<unsigned>      pvsOff;   // por cluster: offset de su fila en 'pvs'
    std::vector<unsigned char> pvs;      // el stream de filas
    std::vector<Hijo> hijos;

    VisBsp() { Limpiar(); }
    void Limpiar();
    bool Valido() const { return !nodos.empty() && nClusters > 0; }

    // parsea un `.w3dbsp` completo desde memoria. false + err = archivo invalido.
    bool Cargar(const unsigned char* bytes, size_t n, std::string* err = 0);
    // la hoja que contiene al punto (bajando por el arbol); -1 si el dato no es valido
    int HojaEn(float x, float y, float z) const;
    // el cluster del punto (-1 = solido / fuera del arbol / sin dato)
    int ClusterEn(float x, float y, float z) const;
    // decodifica la fila del PVS del cluster en 'out' (bytesFila bytes). false = cluster fuera de rango / stream roto
    bool Fila(int cluster, std::vector<unsigned char>& out) const;
    // algun bit en comun entre la fila del PVS y los clusters del hijo
    static bool Interseca(const std::vector<unsigned char>& fila, const std::vector<unsigned char>& bits);
    // indice en 'hijos' del nombre (-1 = no esta: ese hijo se dibuja siempre)
    int IndiceHijo(const std::string& nombre) const;
};

#endif
