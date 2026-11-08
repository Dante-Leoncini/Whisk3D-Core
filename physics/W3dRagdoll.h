#ifndef W3DRAGDOLL_H
#define W3DRAGDOLL_H

// ============================================================================
//  W3dRagdoll — MUNECOS DE TRAPO (ragdolls) caseros del Core, y dos ayudas de
//  IK para animar (dos huesos con polo, FABRIK).
//
//  QUE ES: un esqueleto que se cae solo. Cada hueso que tiene FORMA (una caja o
//  una capsula en su marco) es un cuerpo; los huesos sin forma (los dedos, la
//  columna entre la pelvis y el pecho, la raiz) siguen rigidos al cuerpo mas
//  cercano. Cada cuerpo se une al cuerpo de su ancestro mas cercano por una
//  ARTICULACION: un punto comun (el origen del hueso hijo) y los limites del
//  giro (rotula: cono + giro propio por eje; bisagra: un eje con su rango).
//  Choca contra un PLANO infinito (el piso) y contra lo que diga una funcion
//  del juego (los brushes del mapa, una malla): la forma de cada hueso, no el
//  esqueleto. Sin choques entre los huesos del mismo muneco.
//
//  DISENO (las reglas de la casa, como W3dRigido):
//    * C++03 puro, sin motor externo y SIN Object ni Armature: el juego nativo
//      lo usa directo con sus matrices; el editor lo envuelve. Las matrices son
//      afines 3x4 por COLUMNAS (12 floats: eje x, eje y, eje z, traslacion: la
//      4x4 de GL sin la ultima fila). Las unidades y el "arriba" los pone quien
//      lo usa (la gravedad es un vector: por defecto metros con Y arriba).
//    * COSTO CERO si nadie lo usa: no hay estado global; un muneco DORMIDO (quieto
//      medio segundo) sale de Paso en la primera linea hasta que algo lo toca.
//    * sin allocations por cuadro (todo se reserva en Armar).
//    * la tecnica (Jakobsen, la de Hitman): cada cuerpo son 4 PARTICULAS en
//      tetraedro (cuatro esquinas alternadas de una caja: el centro de masa es el
//      de la forma y la inercia, la de una caja maciza) integradas con Verlet (la
//      velocidad es la diferencia con la posicion anterior). Todo lo demas MUEVE
//      PARTICULAS (dinamica basada en posiciones): un punto del cuerpo (la esquina
//      de la caja, el pivote de la articulacion) es una combinacion fija de las 4,
//      y corregirlo reparte la correccion por esos pesos y por la masa. En cada
//      vuelta el cuerpo vuelve a ser RIGIDO por ajuste de forma (la rotacion que
//      mejor calza las 4 particulas, por el metodo iterativo de Muller con el giro
//      anterior de arranque, sin trigonometria; el centro de masa queda donde
//      estaba). Los LIMITES del giro: el giro de la articulacion contra el reposo,
//      partido en giro propio (sobre x) y columpio (y, z), en tangentes del medio
//      angulo, cada componente a su rango; lo que sobra lo giran el hijo y el padre
//      (repartido por la inercia) cada uno alrededor de SU centro de masa (asi no
//      se crea momento lineal) y la articulacion vuelve a juntar el pivote.
//    * paso FIJO (def.paso, 1/60 s: a 30 fps son dos pasos por cuadro; un cuadro
//      lento hace hasta 4 y el resto se pierde: no hay espiral de la muerte), con
//      'iteraciones' vueltas del solver por paso (4).
//    * la colision: las 8 esquinas de cada caja / las 2 puntas de cada capsula con
//      su radio. El plano se prueba en cada vuelta (un cuerpo lejos del plano ni
//      mira sus esquinas). La funcion del juego se llama UNA vez por punto y por
//      paso, del CENTRO de la forma al punto (el centro siempre esta afuera del
//      mundo: una esquina metida en una pared se encuentra igual): si toca, ese
//      plano queda como contacto mientras empuje. Friccion por posiciones (se frena
//      lo tangencial del punto que toca) y sin rebote.
//
//  COSTO (15 cuerpos, el policia de Half-Life 2, 4 vueltas): ~0,03 ms por paso
//  en un PC; en el N95 (ARM11 con VFP) se estima ~0,5 ms por paso (~1 ms por
//  cuadro de 30 fps), mas lo que cueste la funcion del juego por punto (el plano
//  solo no cuesta nada). Con paso = 1/30 cuesta la mitad. El rayo: ~10 us en PC.
//
//  QUIEN LO LLAMA:
//    * el juego / el editor arma la DEFINICION (W3dRagdollDef: los huesos con su
//      reposo, su forma, su masa y su articulacion) y un W3dRagdoll con Armar().
//    * al caer: Empezar(pose de ahora, pose del cuadro anterior, dt entre las dos)
//      -> sale con la velocidad que traia la animacion (ni se congela ni salta).
//    * cada cuadro: Paso(dt) y Mundos() para la piel (el skinning de siempre).
//    * un disparo: W3dRagdollRayo() contra las formas (de la pose animada o del
//      muneco: son matrices de mundo, las de quien sea) e Impulso() en ese punto.
//    * el overlay de debug: W3dRagdollLineas() (las formas en lineas).
// ============================================================================

#include <vector>

// la forma de un hueso
enum { W3D_RAG_SIN_FORMA = 0, W3D_RAG_CAJA = 1, W3D_RAG_CAPSULA = 2 };
// la articulacion con el cuerpo padre
enum { W3D_RAG_LIBRE = 0,      // solo el punto comun (gira como quiera)
       W3D_RAG_ROTULA = 1,     // limites por eje del marco: x = giro propio, y z = el columpio (el cono)
       W3D_RAG_BISAGRA = 2 };  // solo gira sobre la x del marco, en [min[0], max[0]]

// UN HUESO de la definicion. Los indices son los del esqueleto de quien lo usa.
struct W3dRagdollHueso {
    int   padre;           // el hueso padre (-1 = raiz)
    float reposo[12];      // el hueso en REPOSO en el espacio del modelo (sin escala)
    int   forma;           // W3D_RAG_*: SIN_FORMA = no es un cuerpo (sigue al mas cercano)
    float semi[3];         // caja: los semiejes; capsula: semi[0] = radio, semi[1] = medio largo del tramo recto
    int   eje;             // capsula: su eje en el marco de la forma (0 x, 1 y, 2 z)
    float centro[3];       // el centro de la forma en el marco del hueso
    float giro[4];         // la orientacion de la forma en el marco del hueso (cuaternion x y z w)
    float masa;            // kg (o lo que use quien lo arma; solo importa la proporcion entre cuerpos)
    int   articulacion;    // W3D_RAG_LIBRE / ROTULA / BISAGRA, con el cuerpo del ancestro mas cercano
    float marco[4];        // el marco de la articulacion en el del hueso (cuaternion; x = giro propio / bisagra)
    float min[3], max[3];  // radianes, en el marco, contra el REPOSO (la pose de reposo es el giro cero)
    W3dRagdollHueso();
};

struct W3dRagdollDef {
    std::vector<W3dRagdollHueso> huesos;
    float gravedad[3];     // (0, -9.81, 0)
    float amortiguar;      // fraccion de la velocidad que se pierde por segundo (0.5)
    float rozamiento;      // friccion contra lo que toca, 0..1 (0.6)
    int   iteraciones;     // vueltas del solver por paso (4)
    float paso;            // el paso fijo en segundos (1/60; 1/30 cuesta la mitad y es un poco mas blando)
    W3dRagdollDef();

    // las ayudas para armarla (h = el hueso)
    void Caja(int h, const float* centro, const float* semi);
    // capsula del punto a al b (en el marco del hueso) con ese radio
    void Capsula(int h, const float* a, const float* b, float radio);
    // rotula simetrica: el columpio hasta 'cono' radianes por eje y el giro propio en [giroMin, giroMax]
    void Rotula(int h, float cono, float giroMin, float giroMax);
    // bisagra sobre 'eje' (en el marco del hueso) en [min, max] radianes
    void Bisagra(int h, const float* eje, float min, float max);
};

// lo que toco un rayo
struct W3dRagdollImpacto {
    int   hueso;
    float distancia;
    float punto[3];
    float normal[3];
};

// LA COLISION DEL JUEGO: el segmento de 'desde' a 'hasta' (un punto de una forma moviendose en un paso) contra el
// mundo de quien lo usa. true si choca, con el punto de choque y la normal de la superficie (hacia afuera).
typedef bool (*W3dRagdollTrazo)(void* usuario, const float* desde, const float* hasta, float* punto, float* normal);

class W3dRagdoll {
public:
    W3dRagdoll();
    ~W3dRagdoll();

    // copia la definicion y reserva todo; false si no hay ningun hueso con forma
    bool Armar(const W3dRagdollDef& def);
    void Soltar();
    bool Armado() const { return !cuerpos_.empty(); }
    int  Huesos() const { return (int)def_.huesos.size(); }
    const W3dRagdollDef& Def() const { return def_; }

    // EMPIEZA a caer desde una pose: 'mundos' = las matrices de mundo de todos los huesos (Huesos() * 12 floats,
    // pueden tener escala: se conserva). 'antes' = las del cuadro anterior (NULL = quieto) y 'dtAntes' los segundos
    // entre las dos: de ahi sale la velocidad de cada cuerpo.
    void Empezar(const float* mundos, const float* antes, float dtAntes);
    void Paso(float dt);
    // las matrices de mundo de todos los huesos despues del ultimo Paso (para la piel)
    const float* Mundos() const { return mundos_.empty() ? 0 : &mundos_[0]; }

    // un impulso (masa * velocidad) en un punto de mundo, sobre el cuerpo del hueso (o el que lo lleva); despierta
    void Impulso(int hueso, const float* punto, const float* impulso);
    // el cuerpo que lleva al hueso (el suyo o su ancla): el centro de su forma, su velocidad (unidades por segundo) y
    // su masa; cualquiera puede ser NULL. false si no esta armado. (Para llevarlo flotando: un impulso por cuadro de
    // masa * (velocidad que se quiere - la que tiene), como la pistola de gravedad)
    bool CuerpoDe(int hueso, float* centro, float* vel, float* masa) const;
    // el piso: n . p >= d (n unitario). Sin piso por defecto.
    void Piso(const float* normal, float d);
    void SinPiso() { hayPiso_ = false; }
    // la colision del juego (NULL = ninguna)
    void Mundo(W3dRagdollTrazo trazo, void* usuario) { trazo_ = trazo; usuario_ = usuario; }
    bool Dormido() const { return dormido_; }
    void Despertar() { dormido_ = false; quietud_ = 0; }
    // EL TAMANO EN VIVO: la forma nueva de un hueso que ya tenia cuerpo (forma, semi, eje, centro, giro y masa de
    // 'h'); el hueso queda donde estaba.
    void CambiarForma(int hueso, const W3dRagdollHueso& h);
    // los limites nuevos de la articulacion de un hueso con cuerpo (articulacion, marco, min y max de 'h')
    void CambiarLimites(int hueso, const W3dRagdollHueso& h);

private:
    // un hueso con forma: sus 4 particulas (un tetraedro centrado en la forma, de semiejes 'brazo')
    struct Cuerpo {
        int   hueso, padre;           // padre = el cuerpo del ancestro mas cercano (indice en cuerpos_, -1 = raiz)
        float brazo[3];
        float x[4][3], xp[4][3];      // las particulas ahora y en el paso anterior (Verlet)
        float q[4];                   // el giro de la forma (el del ultimo ajuste: arranca el siguiente)
        float invm;                   // la inversa de la masa de cada particula
        float invI;                   // la inversa de la inercia del cuerpo (un promedio de los tres ejes)
        float wPiv[4], sPiv;          // el pivote (el origen del hueso) como mezcla de las particulas, y suma w^2
        float wPivPadre[4], sPivPadre; // el mismo punto en el cuerpo padre
        // la articulacion: qJ = izq * giroPadre^-1 * giro * der es el giro en su marco contra el reposo (izq y der
        // juntan el reposo, el giro de las formas en sus huesos y el marco); los limites en tangentes del medio angulo
        float izq[4], der[4];
        float tanMin[3], tanMax[3];
        int   npts;                   // los puntos que chocan: 8 esquinas o 2 puntas
        float radio, alcance;         // el radio de los puntos; lo mas lejos del centro que llega la forma
        float wp[8][4], sp[8];
        float plano[8][4];            // el ultimo plano que toco cada punto (n, d) y cuantos pasos le quedan
        int   vida[8];
        bool  toca[8];
    };
    struct Seguidor { int hueso, ancla; float rel[12]; };
    W3dRagdollDef def_;
    std::vector<Cuerpo> cuerpos_;
    std::vector<int> cuerpoDe_;        // por hueso: su cuerpo o -1
    std::vector<Seguidor> seguidores_;
    std::vector<float> escala_;        // por hueso: la escala de cada eje en la pose de Empezar
    std::vector<float> mundos_;
    float acum_;
    bool  hayPiso_;
    float piso_[4];
    W3dRagdollTrazo trazo_;
    void* usuario_;
    bool  dormido_;
    int   quietud_;

    void Subpaso(float h);
    float PasoFijo() const { return def_.paso > 0.004f ? def_.paso : 1.0f / 60.0f; }
    void Contactos();
    void Iteracion(bool ultima);
    void Friccion();
    void Rigidizar(Cuerpo& c);
    void Limitar(Cuerpo& c);
    void ArmarCuerpo(Cuerpo& c, int hueso);
    void ArmarArticulacion(Cuerpo& c);
    void CuerpoAMatriz(const Cuerpo& c, bool anterior, float* hueso) const;
    void MatrizACuerpo(Cuerpo& c, const float* hueso, bool anterior);
    void PivotesDe(Cuerpo& c);
    void ArmarMundos();
};

// EL RAYO contra las formas de un esqueleto en la pose que sea ('mundos' = Huesos() * 12 floats: los de la animacion
// o los de W3dRagdoll::Mundos). 'dir' unitario. true si toco algo antes de maxDist: el hueso, la distancia, el punto
// y la normal de la forma.
bool W3dRagdollRayo(const W3dRagdollDef& def, const float* mundos, const float* origen, const float* dir,
                    float maxDist, W3dRagdollImpacto* out);

// LAS FORMAS EN LINEAS (el overlay de debug, la malla de alambre por hueso del editor): pares de puntos (x y z) en
// 'out' (hasta 'max' puntos); devuelve cuantos puntos escribio (cada par es una linea). Caja: 24 puntos; capsula: 72.
int W3dRagdollLineas(const W3dRagdollDef& def, const float* mundos, float* out, int max);

// ---- IK (para animar; el editor las usa sobre las cadenas de huesos) ---------------------------------------------
// DOS HUESOS (el brazo, la pierna): raiz, codo y punta en mundo; el codo nuevo y la punta nueva para que la punta
// llegue al objetivo, con el codo doblando hacia el lado del polo. Los largos se conservan; si no llega, estira
// derecho hacia el objetivo y devuelve false.
bool W3dIkDosHuesos(const float* raiz, const float* codo, const float* punta, const float* objetivo,
                    const float* polo, float* codoOut, float* puntaOut);
// FABRIK: una cadena de n articulaciones (pos = n * 3 floats, se escriben en el lugar) con la raiz fija; los largos
// salen de las posiciones de entrada. Devuelve las vueltas que hizo (llega a menos de 'tolerancia' o 'vueltas').
int  W3dIkFabrik(float* pos, int n, const float* objetivo, int vueltas, float tolerancia);
// el giro mas corto que lleva la direccion a a la b (cuaternion x y z w): para pasar las posiciones nuevas de una
// cadena a giros de hueso (giroNuevo = W3dIkGiroEntre(eje viejo, eje nuevo) * giroViejo)
void W3dIkGiroEntre(const float* a, const float* b, float* q);

#endif // W3DRAGDOLL_H
