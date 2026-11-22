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
//    * paso FIJO (def.step, 1/60 s: a 30 fps son dos pasos por cuadro; un cuadro
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
//      reposo, su forma, su masa y su articulacion) y un W3dRagdoll con Build().
//    * al caer: Begin(pose de ahora, pose del cuadro anterior, dt entre las dos)
//      -> sale con la velocidad que traia la animacion (ni se congela ni salta).
//    * cada cuadro: Step(dt) y Worlds() para la piel (el skinning de siempre).
//    * un disparo: W3dRagdollRay() contra las formas (de la pose animada o del
//      muneco: son matrices de mundo, las de quien sea) e Impulse() en ese punto.
//    * el overlay de debug: W3dRagdollLines() (las formas en lineas).
// ============================================================================

#include <vector>

// la forma de un hueso
enum { W3D_RAG_NO_SHAPE = 0, W3D_RAG_BOX = 1, W3D_RAG_CAPSULE = 2 };
// la articulacion con el cuerpo padre
enum { W3D_RAG_FREE = 0,     // solo el punto comun (gira como quiera)
       W3D_RAG_KNEE = 1,     // limites por eje del marco: x = giro propio, y z = el columpio (el cono)
       W3D_RAG_HINGE = 2 };  // solo gira sobre la x del marco, en [min[0], max[0]]

// UN HUESO de la definicion. Los indices son los del esqueleto de quien lo usa.
struct W3dRagdollBone {
    int   parent;          // el hueso padre (-1 = raiz)
    float rest[12];        // el hueso en REPOSO en el espacio del modelo (sin escala)
    int   shape;           // W3D_RAG_*: SIN_FORMA = no es un cuerpo (sigue al mas cercano)
    float semi[3];         // caja: los semiejes; capsula: semi[0] = radio, semi[1] = medio largo del tramo recto
    int   axis;            // capsula: su eje en el marco de la forma (0 x, 1 y, 2 z)
    float center[3];       // el centro de la forma en el marco del hueso
    float turn[4];         // la orientacion de la forma en el marco del hueso (cuaternion x y z w)
    float mass;            // kg (o lo que use quien lo arma; solo importa la proporcion entre cuerpos)
    int   joint;           // W3D_RAG_FREE / ROTULA / BISAGRA, con el cuerpo del ancestro mas cercano
    float frame[4];        // el marco de la articulacion en el del hueso (cuaternion; x = giro propio / bisagra)
    float min[3], max[3];  // radianes, en el marco, contra el REPOSO (la pose de reposo es el giro cero)
    W3dRagdollBone();
};

struct W3dRagdollDef {
    std::vector<W3dRagdollBone> bones;
    float gravity[3];     // (0, -9.81, 0)
    float damp;           // fraccion de la velocidad que se pierde por segundo (0.5)
    float friction;       // friccion contra lo que toca, 0..1 (0.6)
    int   iterations;     // vueltas del solver por paso (4)
    float step;           // el paso fijo en segundos (1/60; 1/30 cuesta la mitad y es un poco mas blando)
    W3dRagdollDef();

    // las ayudas para armarla (h = el hueso)
    void Box(int h, const float* center, const float* semi);
    // capsula del punto a al b (en el marco del hueso) con ese radio
    void Capsule(int h, const float* a, const float* b, float radius);
    // rotula simetrica: el columpio hasta 'cono' radianes por eje y el giro propio en [turnMin, turnMax]
    void BallJoint(int h, float cone, float turnMin, float turnMax);
    // bisagra sobre 'eje' (en el marco del hueso) en [min, max] radianes
    void Hinge(int h, const float* axis, float min, float max);
};

// lo que toco un rayo
struct W3dRagdollImpact {
    int   bone;
    float distance;
    float point[3];
    float normal[3];
};

// LA COLISION DEL JUEGO: el segmento de 'desde' a 'hasta' (un punto de una forma moviendose en un paso) contra el
// mundo de quien lo usa. true si choca, con el punto de choque y la normal de la superficie (hacia afuera).
typedef bool (*W3dRagdollStroke)(void* user, const float* from, const float* until, float* point, float* normal);

class W3dRagdoll {
public:
    W3dRagdoll();
    ~W3dRagdoll();

    // copia la definicion y reserva todo; false si no hay ningun hueso con forma
    bool Build(const W3dRagdollDef& def);
    void Release();
    bool Built() const { return !bodies_.empty(); }
    int  Bones() const { return (int)def_.bones.size(); }
    const W3dRagdollDef& Def() const { return def_; }

    // EMPIEZA a caer desde una pose: 'mundos' = las matrices de mundo de todos los huesos (Bones() * 12 floats,
    // pueden tener escala: se conserva). 'antes' = las del cuadro anterior (NULL = quieto) y 'dtPrev' los segundos
    // entre las dos: de ahi sale la velocidad de cada cuerpo.
    void Begin(const float* worlds, const float* prev, float dtPrev);
    void Step(float dt);
    // las matrices de mundo de todos los huesos despues del ultimo Paso (para la piel)
    const float* Worlds() const { return worlds_.empty() ? 0 : &worlds_[0]; }

    // un impulso (masa * velocidad) en un punto de mundo, sobre el cuerpo del hueso (o el que lo lleva); despierta
    void Impulse(int bone, const float* point, const float* impulse);
    // el cuerpo que lleva al hueso (el suyo o su ancla): el centro de su forma, su velocidad (unidades por segundo) y
    // su masa; cualquiera puede ser NULL. false si no esta armado. (Para llevarlo flotando: un impulso por cuadro de
    // masa * (velocidad que se quiere - la que tiene), como la pistola de gravedad)
    bool BodyOf(int bone, float* center, float* vel, float* mass) const;
    // el piso: n . p >= d (n unitario). Sin piso por defecto.
    void Floor(const float* normal, float d);
    void NoFloor() { hasFloor_ = false; }
    // la colision del juego (NULL = ninguna)
    void World(W3dRagdollStroke stroke, void* user) { stroke_ = stroke; user_ = user; }
    bool Asleep() const { return asleep_; }
    void Wake() { asleep_ = false; stillness_ = 0; }
    // EL TAMANO EN VIVO: la forma nueva de un hueso que ya tenia cuerpo (forma, semi, eje, centro, giro y masa de
    // 'h'); el hueso queda donde estaba.
    void ChangeShape(int bone, const W3dRagdollBone& h);
    // los limites nuevos de la articulacion de un hueso con cuerpo (articulacion, marco, min y max de 'h')
    void ChangeLimits(int bone, const W3dRagdollBone& h);

private:
    // un hueso con forma: sus 4 particulas (un tetraedro centrado en la forma, de semiejes 'brazo')
    struct Body {
        int   bone, parent;           // padre = el cuerpo del ancestro mas cercano (indice en cuerpos_, -1 = raiz)
        float arm[3];
        float x[4][3], xp[4][3];      // las particulas ahora y en el paso anterior (Verlet)
        float q[4];                   // el giro de la forma (el del ultimo ajuste: arranca el siguiente)
        float invm;                   // la inversa de la masa de cada particula
        float invI;                   // la inversa de la inercia del cuerpo (un promedio de los tres ejes)
        float wPiv[4], sPiv;          // el pivote (el origen del hueso) como mezcla de las particulas, y suma w^2
        float wPivParent[4], sPivParent; // el mismo punto en el cuerpo padre
        // la articulacion: qJ = izq * giroPadre^-1 * giro * der es el giro en su marco contra el reposo (izq y der
        // juntan el reposo, el giro de las formas en sus huesos y el marco); los limites en tangentes del medio angulo
        float left[4], right[4];
        float tanMin[3], tanMax[3];
        int   npts;                  // los puntos que chocan: 8 esquinas o 2 puntas
        float radius, range;         // el radio de los puntos; lo mas lejos del centro que llega la forma
        float wp[8][4], sp[8];
        float plane[8][4];            // el ultimo plano que toco cada punto (n, d) y cuantos pasos le quedan
        int   life[8];
        bool  touches[8];
    };
    struct Follower { int bone, anchor; float rel[12]; };
    W3dRagdollDef def_;
    std::vector<Body> bodies_;
    std::vector<int> bodyOf_;        // por hueso: su cuerpo o -1
    std::vector<Follower> followers_;
    std::vector<float> scale_;        // por hueso: la escala de cada eje en la pose de Empezar
    std::vector<float> worlds_;
    float accum_;
    bool  hasFloor_;
    float floor_[4];
    W3dRagdollStroke stroke_;
    void* user_;
    bool  asleep_;
    int   stillness_;

    void Substep(float h);
    float FixedStep() const { return def_.step > 0.004f ? def_.step : 1.0f / 60.0f; }
    void Contacts();
    void Iteration(bool last);
    void Friction();
    void MakeRigid(Body& c);
    void Limit(Body& c);
    void BuildBody(Body& c, int bone);
    void BuildJoint(Body& c);
    void BodyToMatrix(const Body& c, bool prev, float* bone) const;
    void MatrixToBody(Body& c, const float* bone, bool prev);
    void PivotsOf(Body& c);
    void BuildWorlds();
};

// EL RAYO contra las formas de un esqueleto en la pose que sea ('mundos' = Bones() * 12 floats: los de la animacion
// o los de W3dRagdoll::Mundos). 'dir' unitario. true si toco algo antes de maxDist: el hueso, la distancia, el punto
// y la normal de la forma.
bool W3dRagdollRay(const W3dRagdollDef& def, const float* worlds, const float* origin, const float* dir,
                    float maxDist, W3dRagdollImpact* out);

// LAS FORMAS EN LINEAS (el overlay de debug, la malla de alambre por hueso del editor): pares de puntos (x y z) en
// 'out' (hasta 'max' puntos); devuelve cuantos puntos escribio (cada par es una linea). Caja: 24 puntos; capsula: 72.
int W3dRagdollLines(const W3dRagdollDef& def, const float* worlds, float* out, int max);

// ---- IK (para animar; el editor las usa sobre las cadenas de huesos) ---------------------------------------------
// DOS HUESOS (el brazo, la pierna): raiz, codo y punta en mundo; el codo nuevo y la punta nueva para que la punta
// llegue al objetivo, con el codo doblando hacia el lado del polo. Los largos se conservan; si no llega, estira
// derecho hacia el objetivo y devuelve false.
bool W3dIkTwoBones(const float* root, const float* elbow, const float* tip, const float* target,
                    const float* pole, float* elbowOut, float* tipOut);
// FABRIK: una cadena de n articulaciones (pos = n * 3 floats, se escriben en el lugar) con la raiz fija; los largos
// salen de las posiciones de entrada. Devuelve las vueltas que hizo (llega a menos de 'tolerancia' o 'vueltas').
int  W3dIkFabrik(float* pos, int n, const float* target, int turns, float tolerance);
// el giro mas corto que lleva la direccion a a la b (cuaternion x y z w): para pasar las posiciones nuevas de una
// cadena a giros de hueso (giroNuevo = W3dIkTurnBetween(eje viejo, eje nuevo) * giroViejo)
void W3dIkTurnBetween(const float* a, const float* b, float* q);

#endif // W3DRAGDOLL_H
