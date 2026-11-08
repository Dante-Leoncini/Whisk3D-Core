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
           MatSinFiltro = 64, MatSinTextura = 128, MatCalcomania = 256, MatCapaSuma = 512, MatAnimada = 1024 };
    // MatAnimada: un material que se mueve solo (los campos de fuerza, las pantallas, los paneles que titilan): sus
    // parametros (Anim) vienen en la seccion opcional 'MANI' del archivo. Las UV de la textura corren por la matriz de
    // textura de la unidad 0 y las de la 2da capa (el campo 'lightmap', con las mismas UV) por la de la unidad 1, cada
    // una a su velocidad; el color sale de un pulso seno con un parpadeo; los cuadros, de una textura por cuadro. Todo
    // por estado de GL: el mismo draw por material, nada por cara
    // MatCapaSuma: la textura de la 2da capa (el campo 'lightmap') se SUMA en vez de multiplicar x2: lo que brilla solo
    // (la mascara de autoiluminacion ya multiplicada), con las mismas UV de la textura base
    // MatCalcomania: una calcomania pegada a otra cara (sangre, carteles, mugre): va con glPolygonOffset hacia la
    // camara (con z de 16 bits y de lejos la geometria despegada no alcanza) y tiene su propio corte por tamano
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
    // la animacion de un material (MatAnimada). La intensidad: brillo + pulso * seno(2 pi t / periodo) - ruido * azar
    // (un azar nuevo 15 veces por segundo: el parpadeo), por la distancia (de 1 en distApagar[0] metros a distApagar[2]
    // en distApagar[1]) y entre 0 y 1. Con mezcla va al alfa (los aditivos se suman por su alfa); sin, al color
    struct Anim {
        float scroll[4];          // UV por segundo: la textura (u, v) y la 2da capa (u, v)
        int modoCapa;             // el modo de MezclaTexturas de la 2da capa (5 suma, 4 multiplica, 2 multiplica x2)
        float color[3];           // el tinte (glColor)
        float brillo, pulso, periodo, ruido;
        float fps;                // los cuadros: > 0 por tiempo; 0 = por distancia (de distCuadros[0] a distCuadros[1] m)
        float distCuadros[2];
        float distApagar[3];      // (distApagar[1] <= distApagar[0] = sin apagarse con la distancia)
        std::vector<std::string> cuadros;   // las texturas de los cuadros (vacio = la del material)
        std::vector<unsigned> idCuadros;    // los ids GL, resueltos por quien tiene el cache (como idTex)
        Anim();
    };
    // lee un Anim del archivo en 'p' (el formato de la seccion 'MANI', ver Cargar): la posicion que sigue, 0 = cortado.
    // (lo usa tambien quien guarda mallas propias con el mismo dato: las entidades de un mapa)
    static size_t LeerAnim(const unsigned char* d, size_t n, size_t p, Anim& a);
    // la textura de 'a' a los 't' segundos y a 'distancia' metros (< 0 = sin distancia: el primer cuadro si van por
    // distancia): la de su cuadro, o 'idTex' si no tiene cuadros (o el cuadro no se resolvio)
    static unsigned CuadroAnim(const Anim& a, float t, float distancia, unsigned idTex);
    // pone el estado de 'a' (despues de bindear la textura): las dos matrices de textura, la 2da capa 'idCapa' (0 = sin;
    // usa las mismas UV que la unidad 0) y el color (apaga el array de color). 'semilla' separa el parpadeo de dos
    // materiales; 'mezclado': la intensidad va al alfa
    static void AplicarAnim(const Anim& a, float t, float distancia, unsigned idCapa, unsigned semilla, bool mezclado);
    // vuelve lo que toco AplicarAnim (las matrices a la identidad, el array de color prendido, blanco)
    static void QuitarAnim();
    std::vector<Anim> anims;             // las animaciones (seccion opcional 'MANI')
    std::vector<int>  animDe;            // por material: el indice en 'anims' (-1 = quieto)
    // el reloj de las animaciones (segundos): lo pone el dueno cada cuadro antes de Dibujar
    void TiempoAnim(float segundos) { tiempoAnim = segundos; }

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
    // el corte por TAMANO EN PANTALLA (con las cajas de las caras): una cara cuya caja (la mitad de su diagonal) es
    // menor que 'proporcion' x su distancia a 'ojo' no se manda (los props chicos y lejanos: un tornillo a 20 m).
    // proporcion 0 = apagado (el defecto). Vale para los cuadros que siguen
    void CorteTamano(const float* ojo, float proporcion);
    // el corte por tamano de las CALCOMANIAS (MatCalcomania): mas fuerte que el de CorteTamano (una mancha de 30 cm a
    // 15 m no aporta nada y es la que pelea con la pared). 0 = el mismo de CorteTamano (el defecto)
    void CorteCalcomanias(float proporcion);
    // los NIVELES DE DETALLE: si el dato trae el rango de distancia de cada cara (rangoCara), se dibuja solo la que
    // tiene a la camara ('ojo' de CorteTamano) adentro de su rango. 'escala' multiplica la distancia (2 = todo cambia
    // al nivel de lejos a la mitad de distancia: un telefono lento); 1 = la del dato
    void EscalaRangos(float escala);
    std::vector<float> rangoCara;        // 2 floats por cara (desde, hasta) en metros (opcional: vacio = siempre)
    std::vector<float> centroRango;      // 3 floats por cara: el punto desde el que se mide su rango (opcional: vacio =
                                         // el centro de su caja; el del prop hace cambiar juntas todas sus partes)

private:
    float corteOjo[3], corteProp, corteCalco, escalaRango, tiempoAnim;
    bool animPuesta;                         // (el ultimo material dibujado fue animado: hay que volver su estado)
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
    void EstadoMaterial(const Material& m, const Grupo& g, unsigned indice);
    MundoBsp(const MundoBsp&);
    MundoBsp& operator=(const MundoBsp&);
};

#endif
