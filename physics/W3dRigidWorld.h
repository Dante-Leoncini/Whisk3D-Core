#ifndef W3D_RIGID_WORLD_H
#define W3D_RIGID_WORLD_H

// ============================================================================
//  W3dRigidWorld — los CUERPOS RIGIDOS de W3dRigido SIN Object: un mundo de
//  cajas orientadas (OBB) que el juego nativo usa directo (como W3dRagdoll),
//  con el mundo estatico que pone el juego por una funcion (los brushes de un
//  mapa, una malla de colision, un piso).
//
//  QUE ES: la misma fisica que W3dRigido (SAT de 15 ejes + manifold por
//  recorte de caras, impulsos secuenciales con friccion de Coulomb, rebote y
//  sesgo posicional, sub-pasos de 1/60 s como maximo, cuerpos que se DUERMEN)
//  pero sin escena: cada cuerpo es un indice. El juego lee la pose de los que
//  se movieron y la pone en sus mallas.
//
//  DISENO (las reglas de la casa):
//    * C++03 puro, sin Object, sin lua; la memoria se reserva al agregar (un
//      cuadro no aloca nada mientras no cambie la cantidad de contactos).
//    * las UNIDADES y el "arriba" los pone quien lo usa: la gravedad es un
//      vector (por defecto metros con Y arriba, -9.81). Los umbrales (hundido
//      tolerado, velocidades de sueno) estan en metros: con otras unidades se
//      cambian en W3dRigidWorldDef.
//    * COSTO CERO quieto: un cuerpo dormido no se integra ni pregunta al mundo;
//      un par se mira solo si uno de los dos esta despierto (los despiertos
//      contra todos: O(despiertos * cuerpos), con una poda por esfera).
//    * EL MUNDO ESTATICO: una funcion del juego recibe la caja de un cuerpo
//      DESPIERTO (centro, ejes, semiejes) y devuelve sus contactos (punto de la
//      caja hundido, normal hacia afuera del solido, cuanto esta hundido). El
//      juego elige como (las esquinas trazadas desde el centro contra los
//      brushes, un plano, una grilla de triangulos). Se llama una vez por
//      cuerpo despierto y por sub-paso.
//
//  CUERPOS:
//    masa > 0  dinamico (cae, choca, gira; la inercia es la de una caja maciza)
//    masa = 0  FIJO (no se mueve: un prop con el movimiento apagado), choca con
//              los dinamicos igual que el mundo
//  Cada uno tiene ademas 'activo' (apagado = no existe para nadie) y un grupo
//  de 32 bits: dos cuerpos que comparten un bit de 'noCollision' no chocan entre
//  si (lo que el jugador lleva en la mano contra lo que esta apilado arriba).
//
//  QUIEN LO LLAMA: el juego, cada cuadro: Step(dt) y despues Moved(i) para
//  saber que mallas poner. Ver hl2-sis/src/objetos.cpp.
// ============================================================================

#include <vector>
#include "math/Vector3.h"
#include "math/Quaternion.h"

// un contacto contra el mundo estatico (lo llena la funcion del juego)
struct W3dRmContact {
    float point[3];    // el punto de la caja que se metio (en mundo)
    float normal[3];   // la normal de la superficie, hacia afuera del solido (hacia la caja)
    float sunk;        // cuanto se metio, a lo largo de la normal (> 0)
};

// la funcion del mundo estatico: la caja del cuerpo 'cuerpo' (centro, ejes = 3 columnas, semiejes) -> cuantos
// contactos escribio en 'out' (hasta 'max')
typedef int (*W3dRmWorldFn)(void* data, int body, const Vector3& center, const Vector3 axes[3],
                            const Vector3& semi, W3dRmContact* out, int max);

struct W3dRigidWorldDef {
    Vector3 gravity;          // (0, -9.81, 0)
    float substepMax;         // 1/60 s: un cuadro mas largo hace mas sub-pasos
    int   substepsMin;        // 2 (a 30 fps: dos de 1/60)
    int   iterations;         // 8 vueltas del solver por sub-paso
    float baumgarte;          // 0.2
    float sunkOk;             // 0.01 m: lo que se tolera hundido (no tiembla)
    float biasMax;            // 4 m/s: lo mas rapido que saca algo hundido
    float thresholdBounce;    // 1 m/s: debajo de esto no rebota
    float sleepLin2;          // 0.005 (m/s)^2
    float sleepAng2;          // 0.01 (rad/s)^2
    int   sleepSteps;         // 45 sub-pasos quieto -> duerme
    float wakeVel2;           // 0.04: un vecino mas rapido que esto despierta al dormido que toca
    float dampLin;            // 0: por segundo, lo que frena el aire (v /= 1 + h*k): calma lo que se arrastra solo
    float dampAng;            // 0: idem el giro
    W3dRigidWorldDef() : gravity(0.0f, -9.81f, 0.0f) {
        substepMax = 1.0f / 60.0f; substepsMin = 2; iterations = 8; baumgarte = 0.2f; sunkOk = 0.01f;
        biasMax = 4.0f; thresholdBounce = 1.0f; sleepLin2 = 0.005f; sleepAng2 = 0.01f; sleepSteps = 45;
        wakeVel2 = 0.04f; dampLin = 0.0f; dampAng = 0.0f;
    }
};

class W3dRigidWorld {
public:
    W3dRigidWorld();
    void Define(const W3dRigidWorldDef& d) { def = d; }
    const W3dRigidWorldDef& Def() const { return def; }
    void StaticWorld(W3dRmWorldFn fn, void* data) { worldFn = fn; worldData = data; }
    void Clear();

    // ---- cuerpos -----------------------------------------------------------
    // el centro de la caja en mundo, su giro, los semiejes; masa 0 = fijo. Arranca DORMIDO (nada cae solo).
    int  Add(const Vector3& center, const Quaternion& rot, const Vector3& semi, float mass,
                 float friction = 0.5f, float bounce = 0.0f);
    void Remove(int i);                    // el indice queda libre (no se reusa hasta Limpiar)
    int  Count() const { return (int)c.size(); }
    bool Exists(int i) const { return i >= 0 && i < (int)c.size() && c[i].live; }

    void Pose(int i, Vector3* center, Quaternion* rot) const;
    void Axes(int i, Vector3 axes[3]) const;
    Vector3 Semi(int i) const;
    float Mass(int i) const;
    void SetPose(int i, const Vector3& center, const Quaternion& rot);   // teletransporte (despierta)
    Vector3 Vel(int i) const;
    Vector3 VelAng(int i) const;
    void SetVel(int i, const Vector3& v);
    void SetVelAng(int i, const Vector3& w);
    void Impulse(int i, const Vector3& imp, const Vector3& point);
    void Wake(int i);
    // despierta a los que tocan (con un margen) la caja del cuerpo i: lo que estaba apoyado en algo que se saca
    void WakeNeighbors(int i, float margin);
    bool Asleep(int i) const;
    void SetActive(int i, bool yes);
    bool Active(int i) const;
    void SetFixed(int i, bool fixed);        // fijo = masa infinita (el movimiento apagado); al soltarlo, su masa
    bool Fixed(int i) const;
    void SetNoGravity(int i, bool yes);   // (lo que se lleva en la mano: lo sostiene el juego)
    void SetGroup(int i, unsigned noCollision);
    // una BISAGRA: el cuerpo solo gira alrededor de la recta de 'eje' (mundo) que pasa por 'pivote' (mundo; NULL = su
    // centro), sin trasladarse (los cubos del ta-te-ti que giran en su barra). eje NULL la saca
    void SetAxis(int i, const Vector3* axis, const Vector3* pivot = 0);
    // se movio en el ultimo Paso (el juego pone su malla)
    bool Moved(int i) const;
    // la caja alineada a los ejes que la envuelve
    void Box(int i, Vector3* mn, Vector3* mx) const;

    // ---- simulacion --------------------------------------------------------
    void Step(float dt);
    int  Awake() const { return nAwake; }   // los que simulo el ultimo Paso

    // rayo contra las cajas activas (menos 'ignorar'): la distancia o < 0; el cuerpo y la normal
    float Ray(const Vector3& origin, const Vector3& dir, float maxDist, int ignore, int* body,
               Vector3* normal) const;

    // el impulso normal total que recibio el cuerpo en el ultimo Paso (que tan fuerte fue el golpe: lo que suena,
    // lo que rompe)
    float HitImpulse(int i) const;

private:
    struct Body {
        Vector3 pos, vel, velAng;
        Quaternion rot;
        Vector3 half, invInertia, invSavedInertia;
        Vector3 u[3];
        float invMass, invSavedMass, friction, bounce;
        float hit;
        unsigned group;
        int stillness;
        Vector3 axis, pivot, arm;   // (bisagra) el eje y un punto suyo en mundo; el centro desde el pivote, en local
        bool live, asleep, active, fixed, noGravity, moved, withAxis;
    };
    struct Point {
        Vector3 pos;
        float sunk, accumN, accumT1, accumT2, vnInitial;
    };
    struct Manifold {
        int ia, ib;            // ia = -1: el mundo estatico
        Vector3 normal;        // de A hacia B
        int n;
        Point p[4];
    };
    bool Collision(int ia, int ib, Manifold& m);
    void BoxFace(const Body& k, int axis, float sg, Vector3 out[4]) const;
    void Resolve(float h);
    void ContactsWorld(int i);
    void Update(Body& k);
    Vector3 InertiaInv(int i, const Vector3& v) const;
    Vector3 VelIn(int i, const Vector3& p) const;
    float InvMassOf(int i) const { return (i < 0 || c[i].asleep) ? 0.0f : c[i].invMass; }

    W3dRigidWorldDef def;
    std::vector<Body> c;
    std::vector<Manifold> ms;
    std::vector<int> awake, newValue;
    W3dRmWorldFn worldFn;
    void* worldData;
    int nAwake;
};

#endif // W3D_RIGID_WORLD_H
