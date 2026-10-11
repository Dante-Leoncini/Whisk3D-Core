#ifndef W3D_MUNDOBSP_H
#define W3D_MUNDOBSP_H
// ============================================================================
//  MundoBsp — el MUNDO de un mapa BSP (Quake 1/2/3, Half-Life 1/2...) dibujado
//  por el motor, como lo hace el juego original: la geometria viene en el mismo
//  `.w3dbsp` (version 2, formato/w3dbsp.md) agrupada por HOJA del arbol y por
//  MATERIAL (textura + pagina de lightmap + banderas), ya en coordenadas del
//  motor. Por cuadro:
//    1. la camara baja por el arbol hasta su hoja -> cluster -> fila del PVS
//       (VisBsp); con ella se marcan las hojas visibles y, hacia arriba, los
//       nodos que tienen alguna (una vez por cambio de cluster, no por cuadro);
//    2. se recorre el arbol desde la raiz: un nodo sin hojas visibles o con su
//       caja fuera del frustum se descarta con su subarbol entero; de cada hoja
//       visible se toman sus caras (una cara que esta en varias hojas se dibuja
//       una vez: sello por cuadro);
//    3. las caras se juntan por (grupo de vertices, material) y se manda UN
//       glDrawElements por material: los atributos quedan en VBOs (un grupo =
//       hasta 65535 vertices, indices de 16 bits) y por el bus viajan solo los
//       indices de lo visible (DrawTrianglesClientIdx). El lightmap va en la
//       unidad 1 de textura en la misma pasada (gfx::MezclaTexturas, Multiply 2x).
//  Cero objetos, cero rutinas, cero interprete: un mapa de 200 mil triangulos
//  cuesta por cuadro lo que se ve (en el N95: ~100 materiales = ~100 draws).
//  Lo usa el Culling con metodo Bsp (main/objects/Culling.cpp), que ademas
//  cullea sus HIJOS (props, entidades, NPCs) con el mismo PVS y frustum.
//  C++03 / Symbian-safe. Sin nada de ningun juego.
// ============================================================================
#include <vector>
#include <string>
#include <stddef.h>
#include "base/crossplatform.h"   // MeshIndex (16 bits en el telefono, 32 en PC)

class VisBsp;

class MundoBsp {
public:
    // banderas de un material (u32 en el archivo)
    enum { MatAlfaTest = 1, MatMezcla = 2, MatAditivo = 4, MatDosCaras = 8, MatSinZ = 16, MatSinRepetir = 32,
           MatSinFiltro = 64, MatSinTextura = 128 };
    struct Material {
        std::string textura;    // ruta de la textura base (una entrada del proyecto: "texturas/x.png")
        std::string lightmap;   // ruta de la pagina del lightmap (vacia = sin lightmap)
        unsigned flags;
        float alfaRef;          // umbral del alpha test (con MatAlfaTest)
        unsigned idTex, idLm;   // los ids GL resueltos por quien tiene el cache de texturas (0 = todavia no)
        Material() : flags(0), alfaRef(0.5f), idTex(0), idLm(0) {}
    };
    // un grupo de vertices = un juego de VBOs (hasta 65535 vertices: indices de 16 bits, GL ES 1.1)
    struct Grupo {
        unsigned nVerts;
        std::vector<float> pos, uv, lm;          // 3 / 2 / 2 floats por vertice (lm vacio = sin lightmap)
        std::vector<unsigned char> col;          // 4 bytes por vertice
        std::vector<unsigned short> idx;         // el stream de indices del grupo (por material, por cara)
        unsigned vboPos, vboUV, vboLm, vboCol;   // 0 = no subido (o sin VBO: se dibuja desde RAM)
        bool subido;
        Grupo() : nVerts(0), vboPos(0), vboUV(0), vboLm(0), vboCol(0), subido(false) {}
    };
    struct Cara { unsigned short grupo, material; unsigned primero, cantidad; };   // 'primero'/'cantidad' en indices
    struct Estad { int hojas, caras, draws, tris; Estad() : hojas(0), caras(0), draws(0), tris(0) {} };

    std::vector<Material> materiales;
    std::vector<Grupo>    grupos;
    std::vector<Cara>     caras;
    std::vector<unsigned> hojaCaraOff;   // por hoja: offset en hojaCaras (nHojas + 1 entradas)
    std::vector<unsigned> hojaCaras;     // indices de cara, hoja por hoja
    std::vector<float>    cajaNodo;      // 6 floats por nodo (min xyz, max xyz) en coordenadas del motor
    std::vector<float>    cajaHoja;      // 6 floats por hoja
    std::vector<float>    cajaCara;      // 6 floats por cara (opcional: vacio = sin test por cara). Un prop grande que toca
                                         // varias hojas no se manda si su caja queda fuera del frustum
    Estad ultimo;                        // lo que dibujo el ultimo cuadro (bspinfo / bench)

    MundoBsp();
    ~MundoBsp();
    bool Valido() const { return !grupos.empty() && !caras.empty(); }
    // parsea la seccion de geometria del .w3dbsp v2 desde 'off' (lo que sigue a la parte de visibilidad que leyo
    // VisBsp). 'bsp' da la cantidad de nodos y hojas que la geometria tiene que respetar. false + err = invalido
    // 'subirAlCargar': cada grupo va a la GPU apenas se parsea (hay contexto: se carga desde el render) y su RAM se
    // suelta; false = se sube al primer dibujo (pruebas sin contexto)
    bool Cargar(const unsigned char* bytes, size_t n, size_t off, const VisBsp& bsp, std::string* err = 0, bool subirAlCargar = true);
    void Limpiar();      // suelta todo (VBOs incluidos)
    size_t Bytes() const;   // RAM que ocupa el dato (sin los VBOs)

    // dibuja el mundo visible desde 'cluster' con la fila del PVS 'fila' (cluster < 0 = sin PVS: todo pasa) y los 6
    // planos del frustum en 'planos' (24 floats, en las coordenadas del mundo; NULL = sin frustum: vista orto). La
    // matriz de GL ya tiene puesta la del objeto dueno. Deja la unidad 1 apagada; el resto del estado queda cacheado
    // en gfx como lo dejo el ultimo material (quien dibuje despues pone el suyo, como siempre).
    // 'fase': 0 = todo; 1 = solo los materiales OPACOS (y alpha test), juntando lo visible del cuadro; 2 = solo los
    // MEZCLADOS (alfa, aditivos), con lo que junto la fase 1 de este mismo cuadro. Asi el dueno dibuja sus hijos
    // (entidades, props con objeto) entre las dos: un haz de luz aditivo va DESPUES de la reja que tiene adelante
    // (no escribe z: lo opaco que viniera despues lo pisaria)
    void Dibujar(const VisBsp& bsp, int cluster, const std::vector<unsigned char>& fila, const float* planos, int fase = 0);

private:
    int clusterMarcado;                      // el cluster con el que se marcaron hojaVis/nodoVis (-2 = nunca)
    std::vector<unsigned char> hojaVis, nodoVis;
    std::vector<unsigned> selloCara;         // por cara: el sello del ultimo cuadro en que se junto
    unsigned sello;
    std::vector<std::vector<unsigned> > cubos;   // por (grupo * nMateriales + material): pares (primero, cantidad)
    std::vector<unsigned char> cuboTocado;
    std::vector<unsigned> tocados;
    std::vector<int> pila;
    std::vector<MeshIndex> indices;          // los indices de un material visible, juntados (viajan por el bus)

    void Marcar(const VisBsp& bsp, int cluster, const std::vector<unsigned char>& fila);
    void DibujarTocados(int fase);
    bool SubirGrupo(Grupo& g, int indice);
    void PunterosGrupo(Grupo& g);
    void EstadoMaterial(const Material& m, const Grupo& g);
    MundoBsp(const MundoBsp&);
    MundoBsp& operator=(const MundoBsp&);
};

#endif
