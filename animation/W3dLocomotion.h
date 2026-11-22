#ifndef W3D_LOCOMOTION_H
#define W3D_LOCOMOTION_H

// ============================================================================
//  W3dLocomocion — COMO CAMINA UN PERSONAJE: la rapidez y el ritmo de la
//  animacion, el giro (el torso primero, los pies despues), la mirada repartida
//  en la columna y los PIES APOYADOS EN EL PISO (IK de dos huesos con la cadera
//  bajando al escalon y el pie que pisa clavado donde piso: no patina).
//
//  QUE ES: funciones sobre una POSE LOCAL (por hueso: el giro, un cuaternion
//  x y z w, y la traslacion respecto del padre) y el arbol de huesos (el padre
//  de cada uno y un orden con los padres antes que los hijos). No sabe de
//  modelos, archivos ni del motor: el que anima (el juego nativo, el editor)
//  saca la pose de sus clips, la pasa por aca y la usa para la piel. Las
//  unidades, el "arriba" y el "adelante" del MODELO los da quien llama (en el
//  demo de Half-Life 2: metros, Y arriba, el personaje mira a +X).
//
//  LAS PIEZAS (cada una se usa sola):
//    * LA MARCHA (W3dLocoGait): la rapidez que acelera y frena hacia la que se
//      quiere, cual animacion va (parado, caminar, correr, con histeresis) y el
//      RITMO de esa animacion para que el recorrido de sus pies sea el que de
//      verdad avanza el personaje (W3dLocoRhythm): los pies no patinan aunque
//      camine mas despacio que el clip.
//    * EL GIRO (W3dLocoTurn): parado, si lo que quiere mirar esta cerca (menos
//      de 'umbral' grados) gira el torso (la TORSION, para W3dLocoLook) y no los
//      pies; si esta mas lejos, DA UN PASO DE GIRO: el cuerpo gira a
//      'velStep' grados por segundo mientras el que llama hace la animacion de
//      pasos (los pies clavados de W3dLocoFeet los acompanan). Caminando gira de
//      a poco ('velWalking').
//    * LA MIRADA (W3dLocoLook): un giro (yaw a la izquierda, pitch arriba,
//      respecto del cuerpo) repartido en una CADENA de huesos (columna, cuello,
//      cabeza) por pesos, cada uno con su tope. Los huesos FIJOS (las claviculas)
//      conservan su giro en el modelo: los brazos no heredan la mirada (un
//      pasajero agarrado del pasamanos gira la cabeza y la mano no se tuerce).
//    * LOS PIES (W3dLocoFeet): por pie, la cadera, la rodilla y el tobillo; una
//      funcion del juego dice el PISO bajo un punto (en el mundo). Cada pie se
//      corre lo que su piso difiere del plano de los pies del modelo (escalon,
//      rampa, el borde del vagon); la PELVIS baja lo que necesite el pie mas bajo
//      (hasta 'lowMax'); la pierna se dobla con W3dIkTwoBones (W3dRagdoll.h,
//      la rodilla hacia donde la doblaba la animacion) y el pie conserva su giro
//      de la animacion. CLAVADO: cuando el pie de la animacion apoya (su tobillo
//      a menos de 'stepHeight' del piso), el punto donde apoyo queda fijo en el
//      mundo mientras la animacion no lo levante (o se aleje mas de
//      'maxSlip'): es el PASO planificado sobre el piso de verdad; al girar en
//      el lugar o caminar mas lento que el clip el pie no patina. Un pie
//      clavado nunca cruza la linea media de la cadera: si queda cruzado, lejos
//      o el cuerpo giro mucho desde que apoyo, da un PASITO (se levanta y apoya
//      donde lo pone la animacion, de a un pie); al girar en el lugar los pies
//      se van replantando solos. Con 'stepVel' el apoyo lo decide tambien la velocidad del pie de la animacion en
//      el mundo (el de la caminata apoya cuando el cuerpo avanza lo que el pie retrocede).
//
//  COSTO: los pies ~6 us por personaje en un PC (dos IK y el recorrido de los
//  huesos de las piernas); la mirada ~2 us. Sin memoria por cuadro.
// ============================================================================

#include <stdint.h>

// el arbol de huesos (no se copia: los arreglos son de quien llama)
struct W3dLocoSkeleton {
    int n;
    const int* parent;      // el padre de cada hueso (-1: raiz)
    const int* order;       // los huesos con los padres antes que los hijos (NULL: 0..n-1 ya lo esta)
};

// ---- LA MARCHA ----------------------------------------------------------------------------------------------------
struct W3dLocoGait {
    float speed;                // la de ahora (unidades por segundo)
    float accelerates, frena;   // unidades por segundo al cuadrado
    int step;                   // 0 parado, 1 camina, 2 corre
    W3dLocoGait();
};
// acerca la rapidez a la que se quiere y elige el paso (velWalk, velRun: las de los clips)
void W3dLocoAdvance(W3dLocoGait& m, float wants, float velWalk, float velRun, float dt);
// el ritmo del clip para que sus pies avancen 'rapidez' (el clip avanza 'velClip' a ritmo 1), entre 'min' y 'max'
float W3dLocoRhythm(float speed, float velClip, float min, float max);

// ---- EL GIRO ------------------------------------------------------------------------------------------------------
struct W3dLocoTurnDef {
    float threshold;      // grados: mas cerca solo gira el torso (parado)
    float velStep;        // grados por segundo del paso de giro
    float velWalking;     // grados por segundo caminando
    float torsionMax;     // grados que llega a girar el torso
    W3dLocoTurnDef();
};
struct W3dLocoTurn {
    float yaw;                 // hacia donde mira el cuerpo (grados)
    float torsion;             // cuanto gira el torso hacia lo que quiere (grados, para W3dLocoLook)
    bool overwriting;          // esta dando un paso de giro (el que llama pone la animacion de pasos)
    W3dLocoTurn();
};
void W3dLocoRotate(W3dLocoTurn& g, float wants, bool walking, const W3dLocoTurnDef& d, float dt);

// ---- LA MIRADA ----------------------------------------------------------------------------------------------------
enum { W3DLOCO_CHAIN = 6, W3DLOCO_FIXED = 4 };
struct W3dLocoChain {
    int n;
    int bone[W3DLOCO_CHAIN];          // de la columna a la cabeza
    float weight[W3DLOCO_CHAIN];      // la parte del giro que hace cada uno (suman 1)
    float limitYaw[W3DLOCO_CHAIN], limitPitch[W3DLOCO_CHAIN];   // grados
    int nFixed;
    int fixed[W3DLOCO_FIXED];          // conservan su giro en el modelo (los brazos no heredan la mirada)
    float up[3], right[3];     // los ejes del modelo (yaw alrededor de arriba, pitch alrededor de derecha)
};
// gira la cadena (yaw a la izquierda, pitch arriba, en grados respecto del cuerpo) en la pose local
void W3dLocoLook(const W3dLocoSkeleton& e, float (*q)[4], float (*t)[3], const W3dLocoChain& c, float yaw,
                  float pitch);

// ---- LOS PIES -----------------------------------------------------------------------------------------------------
// el piso bajo 'p' (en el mundo): el punto del piso en 'out'; false si no hay
typedef bool (*W3dLocoGroundFn)(void* user, const float* p, float* out);
struct W3dLocoFoot { int hip, knee, ankle; };
struct W3dLocoFeetDef {
    W3dLocoFoot foot[2];
    int pelvis;             // el hueso que baja (la raiz de las piernas)
    float up[3];            // el arriba del MUNDO (unitario)
    float lowMax;           // lo que llega a bajar la pelvis
    float maxFloor;         // un piso mas lejos que esto del plano de los pies no se usa (otro piso, un hueco)
    float stepHeight;       // el tobillo a menos de esto (sobre su altura mas baja) apoya
    float maxSlip;          // un pie clavado que la animacion corre mas que esto se suelta
    // EL PASITO: un pie clavado que quedo mal (cruzo la linea media de la cadera, quedo lejos o el cuerpo giro mas de
    // 'maxTurn' grados desde que apoyo) se levanta y se vuelve a apoyar donde lo pone la animacion, en 'durStep'
    // segundos y 'heightStep' de alto (de a un pie). Sin clavar (parado), el pie que no esta donde lo pone la animacion
    // (mas de 'asentar') tambien da el pasito: queda la pose del clip.
    float maxTurn;           // grados
    float marginMiddle;      // lo que tiene que quedar del lado de su cadera
    float durStep, heightStep;
    float settle;
    float betweenSteps;       // segundos minimos entre el comienzo de un pasito y el del siguiente (0: sin espera)
    // EL APOYO POR LA VELOCIDAD: con 'stepVel' > 0 un pie apoya solo si la animacion (con el cuerpo que la lleva) lo
    // mueve en el mundo mas despacio que eso, y se suelta cuando lo lleva mas rapido que 'looseVel': el pie que baja al
    // final del vuelo no se clava antes de tiempo (quedaba atras y lo arrastraba el pasito). Clavado asi, el pie queda
    // fijo sobre el piso pero sube y baja como en la animacion (el talon que se levanta antes de despegar)
    float stepVel, looseVel;   // unidades por segundo (0: solo la altura)
    float front[3];        // el adelante del MODELO (hacia donde se doblan las rodillas); 0 0 0 = el de la animacion
    W3dLocoFeetDef();
};
struct W3dLocoFeetState {
    bool pinned[2];
    float anchor[2][3];    // donde apoyo (mundo)
    float weight[2];       // cuanto lo sigue (0..1, se funde)
    float minAnkle[2];     // la altura del tobillo apoyado (se aprende: la mas baja vista)
    float pelvis;          // la bajada de la pelvis de ahora (suavizada)
    float stepT[2];        // el pasito: lo que va (0..1; <0: no da ninguno)
    float stepOf[2][3];    // de donde salio el pie (mundo)
    float front[2][3];     // un eje horizontal del cuerpo cuando apoyo (el giro desde entonces)
    bool hasFrontFoot[2];
    float pole[2][3];       // hacia donde dobla cada rodilla (en el modelo)
    bool hasPole[2];
    float fromStep;        // segundos desde que empezo el ultimo pasito
    float prev[2][3];      // donde estaba el tobillo de la animacion el cuadro anterior (mundo)
    float vel[2];          // su velocidad sobre el piso (unidades por segundo)
    bool ready;
    W3dLocoFeetState();
};
// apoya los pies: 'modelo' es la afin modelo -> mundo (3x4 por columnas: x, y, z, traslacion). 'clavar' = false
// (parado sin girar no hace falta; en el aire) apoya sin clavar: los pies que estaban clavados en otro lado vuelven con
// un pasito a donde los pone la animacion
void W3dLocoFeet(const W3dLocoSkeleton& e, float (*q)[4], float (*t)[3], const float* model,
                 const W3dLocoFeetDef& d, W3dLocoFeetState& s, W3dLocoGroundFn ground, void* user, bool pin,
                 float dt);

#endif // W3D_LOCOMOTION_H
