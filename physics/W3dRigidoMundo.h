#ifndef W3DRIGIDOMUNDO_H
#define W3DRIGIDOMUNDO_H

// ============================================================================
//  W3dRigidoMundo — los CUERPOS RIGIDOS de W3dRigido SIN Object: un mundo de
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
//      cambian en W3dRigidoMundoDef.
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
//  de 32 bits: dos cuerpos que comparten un bit de 'sinChoque' no chocan entre
//  si (lo que el jugador lleva en la mano contra lo que esta apilado arriba).
//
//  QUIEN LO LLAMA: el juego, cada cuadro: Paso(dt) y despues Movido(i) para
//  saber que mallas poner. Ver hl2-sis/src/objetos.cpp.
// ============================================================================

#include <vector>
#include "math/Vector3.h"
#include "math/Quaternion.h"

// un contacto contra el mundo estatico (lo llena la funcion del juego)
struct W3dRmContacto {
    float punto[3];    // el punto de la caja que se metio (en mundo)
    float normal[3];   // la normal de la superficie, hacia afuera del solido (hacia la caja)
    float hundido;     // cuanto se metio, a lo largo de la normal (> 0)
};

// la funcion del mundo estatico: la caja del cuerpo 'cuerpo' (centro, ejes = 3 columnas, semiejes) -> cuantos
// contactos escribio en 'out' (hasta 'max')
typedef int (*W3dRmMundoFn)(void* dato, int cuerpo, const Vector3& centro, const Vector3 ejes[3],
                            const Vector3& semi, W3dRmContacto* out, int max);

struct W3dRigidoMundoDef {
    Vector3 gravedad;       // (0, -9.81, 0)
    float subpasoMax;       // 1/60 s: un cuadro mas largo hace mas sub-pasos
    int   subpasosMin;      // 2 (a 30 fps: dos de 1/60)
    int   iteraciones;      // 8 vueltas del solver por sub-paso
    float baumgarte;        // 0.2
    float hundidoOk;        // 0.01 m: lo que se tolera hundido (no tiembla)
    float sesgoMax;         // 4 m/s: lo mas rapido que saca algo hundido
    float umbralRebote;     // 1 m/s: debajo de esto no rebota
    float dormirLin2;       // 0.005 (m/s)^2
    float dormirAng2;       // 0.01 (rad/s)^2
    int   dormirPasos;      // 45 sub-pasos quieto -> duerme
    float despertarVel2;    // 0.04: un vecino mas rapido que esto despierta al dormido que toca
    float amortiguarLin;    // 0: por segundo, lo que frena el aire (v /= 1 + h*k): calma lo que se arrastra solo
    float amortiguarAng;    // 0: idem el giro
    W3dRigidoMundoDef() : gravedad(0.0f, -9.81f, 0.0f) {
        subpasoMax = 1.0f / 60.0f; subpasosMin = 2; iteraciones = 8; baumgarte = 0.2f; hundidoOk = 0.01f;
        sesgoMax = 4.0f; umbralRebote = 1.0f; dormirLin2 = 0.005f; dormirAng2 = 0.01f; dormirPasos = 45;
        despertarVel2 = 0.04f; amortiguarLin = 0.0f; amortiguarAng = 0.0f;
    }
};

class W3dRigidoMundo {
public:
    W3dRigidoMundo();
    void Definir(const W3dRigidoMundoDef& d) { def = d; }
    const W3dRigidoMundoDef& Def() const { return def; }
    void MundoEstatico(W3dRmMundoFn fn, void* dato) { mundoFn = fn; mundoDato = dato; }
    void Limpiar();

    // ---- cuerpos -----------------------------------------------------------
    // el centro de la caja en mundo, su giro, los semiejes; masa 0 = fijo. Arranca DORMIDO (nada cae solo).
    int  Agregar(const Vector3& centro, const Quaternion& rot, const Vector3& semi, float masa,
                 float friccion = 0.5f, float rebote = 0.0f);
    void Quitar(int i);                    // el indice queda libre (no se reusa hasta Limpiar)
    int  Cantidad() const { return (int)c.size(); }
    bool Existe(int i) const { return i >= 0 && i < (int)c.size() && c[i].vivo; }

    void Pose(int i, Vector3* centro, Quaternion* rot) const;
    void Ejes(int i, Vector3 ejes[3]) const;
    Vector3 Semi(int i) const;
    float Masa(int i) const;
    void SetPose(int i, const Vector3& centro, const Quaternion& rot);   // teletransporte (despierta)
    Vector3 Vel(int i) const;
    Vector3 VelAng(int i) const;
    void SetVel(int i, const Vector3& v);
    void SetVelAng(int i, const Vector3& w);
    void Impulso(int i, const Vector3& imp, const Vector3& punto);
    void Despertar(int i);
    // despierta a los que tocan (con un margen) la caja del cuerpo i: lo que estaba apoyado en algo que se saca
    void DespertarVecinos(int i, float margen);
    bool Dormido(int i) const;
    void SetActivo(int i, bool si);
    bool Activo(int i) const;
    void SetFijo(int i, bool fijo);        // fijo = masa infinita (el movimiento apagado); al soltarlo, su masa
    bool Fijo(int i) const;
    void SetSinGravedad(int i, bool si);   // (lo que se lleva en la mano: lo sostiene el juego)
    void SetGrupo(int i, unsigned sinChoque);
    // una BISAGRA: el cuerpo solo gira alrededor de la recta de 'eje' (mundo) que pasa por 'pivote' (mundo; NULL = su
    // centro), sin trasladarse (los cubos del ta-te-ti que giran en su barra). eje NULL la saca
    void SetEje(int i, const Vector3* eje, const Vector3* pivote = 0);
    // se movio en el ultimo Paso (el juego pone su malla)
    bool Movido(int i) const;
    // la caja alineada a los ejes que la envuelve
    void Caja(int i, Vector3* mn, Vector3* mx) const;

    // ---- simulacion --------------------------------------------------------
    void Paso(float dt);
    int  Despiertos() const { return nDespiertos; }   // los que simulo el ultimo Paso

    // rayo contra las cajas activas (menos 'ignorar'): la distancia o < 0; el cuerpo y la normal
    float Rayo(const Vector3& origen, const Vector3& dir, float maxDist, int ignorar, int* cuerpo,
               Vector3* normal) const;

    // el impulso normal total que recibio el cuerpo en el ultimo Paso (que tan fuerte fue el golpe: lo que suena,
    // lo que rompe)
    float GolpeImpulso(int i) const;

private:
    struct Cuerpo {
        Vector3 pos, vel, velAng;
        Quaternion rot;
        Vector3 half, invInercia, invInerciaGuardada;
        Vector3 u[3];
        float invMasa, invMasaGuardada, friccion, rebote;
        float golpe;
        unsigned grupo;
        int quietud;
        Vector3 eje, pivote, brazo;   // (bisagra) el eje y un punto suyo en mundo; el centro desde el pivote, en local
        bool vivo, dormido, activo, fijo, sinGravedad, movido, conEje;
    };
    struct Punto {
        Vector3 pos;
        float hundido, acumN, acumT1, acumT2, vnInicial;
    };
    struct Manifold {
        int ia, ib;            // ia = -1: el mundo estatico
        Vector3 normal;        // de A hacia B
        int n;
        Punto p[4];
    };
    bool Colision(int ia, int ib, Manifold& m);
    void CaraDeCaja(const Cuerpo& k, int eje, float sg, Vector3 out[4]) const;
    void Resolver(float h);
    void ContactosMundo(int i);
    void Actualizar(Cuerpo& k);
    Vector3 InerciaInv(int i, const Vector3& v) const;
    Vector3 VelEn(int i, const Vector3& p) const;
    float InvMasaDe(int i) const { return (i < 0 || c[i].dormido) ? 0.0f : c[i].invMasa; }

    W3dRigidoMundoDef def;
    std::vector<Cuerpo> c;
    std::vector<Manifold> ms;
    std::vector<int> despiertos, nuevos;
    W3dRmMundoFn mundoFn;
    void* mundoDato;
    int nDespiertos;
};

#endif // W3DRIGIDOMUNDO_H
