#ifndef W3DLOCOMOCION_H
#define W3DLOCOMOCION_H

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
//    * LA MARCHA (W3dLocoMarcha): la rapidez que acelera y frena hacia la que se
//      quiere, cual animacion va (parado, caminar, correr, con histeresis) y el
//      RITMO de esa animacion para que el recorrido de sus pies sea el que de
//      verdad avanza el personaje (W3dLocoRitmo): los pies no patinan aunque
//      camine mas despacio que el clip.
//    * EL GIRO (W3dLocoGiro): parado, si lo que quiere mirar esta cerca (menos
//      de 'umbral' grados) gira el torso (la TORSION, para W3dLocoMirar) y no los
//      pies; si esta mas lejos, DA UN PASO DE GIRO: el cuerpo gira a
//      'velPaso' grados por segundo mientras el que llama hace la animacion de
//      pasos (los pies clavados de W3dLocoPies los acompanan). Caminando gira de
//      a poco ('velCaminando').
//    * LA MIRADA (W3dLocoMirar): un giro (yaw a la izquierda, pitch arriba,
//      respecto del cuerpo) repartido en una CADENA de huesos (columna, cuello,
//      cabeza) por pesos, cada uno con su tope. Los huesos FIJOS (las claviculas)
//      conservan su giro en el modelo: los brazos no heredan la mirada (un
//      pasajero agarrado del pasamanos gira la cabeza y la mano no se tuerce).
//    * LOS PIES (W3dLocoPies): por pie, la cadera, la rodilla y el tobillo; una
//      funcion del juego dice el PISO bajo un punto (en el mundo). Cada pie se
//      corre lo que su piso difiere del plano de los pies del modelo (escalon,
//      rampa, el borde del vagon); la PELVIS baja lo que necesite el pie mas bajo
//      (hasta 'maxBaja'); la pierna se dobla con W3dIkDosHuesos (W3dRagdoll.h,
//      la rodilla hacia donde la doblaba la animacion) y el pie conserva su giro
//      de la animacion. CLAVADO: cuando el pie de la animacion apoya (su tobillo
//      a menos de 'alturaPisa' del piso), el punto donde apoyo queda fijo en el
//      mundo mientras la animacion no lo levante (o se aleje mas de
//      'maxDesliz'): es el PASO planificado sobre el piso de verdad; al girar en
//      el lugar o caminar mas lento que el clip el pie no patina. Un pie
//      clavado nunca cruza la linea media de la cadera: si queda cruzado, lejos
//      o el cuerpo giro mucho desde que apoyo, da un PASITO (se levanta y apoya
//      donde lo pone la animacion, de a un pie); al girar en el lugar los pies
//      se van replantando solos. Con 'velPisa' el apoyo lo decide tambien la velocidad del pie de la animacion en
//      el mundo (el de la caminata apoya cuando el cuerpo avanza lo que el pie retrocede).
//
//  COSTO: los pies ~6 us por personaje en un PC (dos IK y el recorrido de los
//  huesos de las piernas); la mirada ~2 us. Sin memoria por cuadro.
// ============================================================================

#include <stdint.h>

// el arbol de huesos (no se copia: los arreglos son de quien llama)
struct W3dLocoEsqueleto {
    int n;
    const int* padre;      // el padre de cada hueso (-1: raiz)
    const int* orden;      // los huesos con los padres antes que los hijos (NULL: 0..n-1 ya lo esta)
};

// ---- LA MARCHA ----------------------------------------------------------------------------------------------------
struct W3dLocoMarcha {
    float rapidez;          // la de ahora (unidades por segundo)
    float acelera, frena;   // unidades por segundo al cuadrado
    int paso;               // 0 parado, 1 camina, 2 corre
    W3dLocoMarcha();
};
// acerca la rapidez a la que se quiere y elige el paso (velCaminar, velCorrer: las de los clips)
void W3dLocoAvanzar(W3dLocoMarcha& m, float quiere, float velCaminar, float velCorrer, float dt);
// el ritmo del clip para que sus pies avancen 'rapidez' (el clip avanza 'velClip' a ritmo 1), entre 'min' y 'max'
float W3dLocoRitmo(float rapidez, float velClip, float min, float max);

// ---- EL GIRO ------------------------------------------------------------------------------------------------------
struct W3dLocoGiroDef {
    float umbral;           // grados: mas cerca solo gira el torso (parado)
    float velPaso;          // grados por segundo del paso de giro
    float velCaminando;     // grados por segundo caminando
    float torsionMax;       // grados que llega a girar el torso
    W3dLocoGiroDef();
};
struct W3dLocoGiro {
    float yaw;              // hacia donde mira el cuerpo (grados)
    float torsion;          // cuanto gira el torso hacia lo que quiere (grados, para W3dLocoMirar)
    bool pisando;           // esta dando un paso de giro (el que llama pone la animacion de pasos)
    W3dLocoGiro();
};
void W3dLocoGirar(W3dLocoGiro& g, float quiere, bool caminando, const W3dLocoGiroDef& d, float dt);

// ---- LA MIRADA ----------------------------------------------------------------------------------------------------
enum { W3DLOCO_CADENA = 6, W3DLOCO_FIJOS = 4 };
struct W3dLocoCadena {
    int n;
    int hueso[W3DLOCO_CADENA];       // de la columna a la cabeza
    float peso[W3DLOCO_CADENA];      // la parte del giro que hace cada uno (suman 1)
    float topeYaw[W3DLOCO_CADENA], topePitch[W3DLOCO_CADENA];   // grados
    int nFijos;
    int fijo[W3DLOCO_FIJOS];          // conservan su giro en el modelo (los brazos no heredan la mirada)
    float arriba[3], derecha[3];     // los ejes del modelo (yaw alrededor de arriba, pitch alrededor de derecha)
};
// gira la cadena (yaw a la izquierda, pitch arriba, en grados respecto del cuerpo) en la pose local
void W3dLocoMirar(const W3dLocoEsqueleto& e, float (*q)[4], float (*t)[3], const W3dLocoCadena& c, float yaw,
                  float pitch);

// ---- LOS PIES -----------------------------------------------------------------------------------------------------
// el piso bajo 'p' (en el mundo): el punto del piso en 'out'; false si no hay
typedef bool (*W3dLocoSueloFn)(void* usuario, const float* p, float* out);
struct W3dLocoPie { int cadera, rodilla, tobillo; };
struct W3dLocoPiesDef {
    W3dLocoPie pie[2];
    int pelvis;             // el hueso que baja (la raiz de las piernas)
    float arriba[3];        // el arriba del MUNDO (unitario)
    float maxBaja;          // lo que llega a bajar la pelvis
    float maxPiso;          // un piso mas lejos que esto del plano de los pies no se usa (otro piso, un hueco)
    float alturaPisa;       // el tobillo a menos de esto (sobre su altura mas baja) apoya
    float maxDesliz;        // un pie clavado que la animacion corre mas que esto se suelta
    // EL PASITO: un pie clavado que quedo mal (cruzo la linea media de la cadera, quedo lejos o el cuerpo giro mas de
    // 'maxGiro' grados desde que apoyo) se levanta y se vuelve a apoyar donde lo pone la animacion, en 'durPaso'
    // segundos y 'altoPaso' de alto (de a un pie). Sin clavar (parado), el pie que no esta donde lo pone la animacion
    // (mas de 'asentar') tambien da el pasito: queda la pose del clip.
    float maxGiro;          // grados
    float margenMedio;      // lo que tiene que quedar del lado de su cadera
    float durPaso, altoPaso;
    float asentar;
    float entrePasos;       // segundos minimos entre el comienzo de un pasito y el del siguiente (0: sin espera)
    // EL APOYO POR LA VELOCIDAD: con 'velPisa' > 0 un pie apoya solo si la animacion (con el cuerpo que la lleva) lo
    // mueve en el mundo mas despacio que eso, y se suelta cuando lo lleva mas rapido que 'velSuelta': el pie que baja al
    // final del vuelo no se clava antes de tiempo (quedaba atras y lo arrastraba el pasito). Clavado asi, el pie queda
    // fijo sobre el piso pero sube y baja como en la animacion (el talon que se levanta antes de despegar)
    float velPisa, velSuelta;   // unidades por segundo (0: solo la altura)
    float frente[3];        // el adelante del MODELO (hacia donde se doblan las rodillas); 0 0 0 = el de la animacion
    W3dLocoPiesDef();
};
struct W3dLocoPiesEstado {
    bool clavado[2];
    float ancla[2][3];      // donde apoyo (mundo)
    float peso[2];          // cuanto lo sigue (0..1, se funde)
    float minTobillo[2];    // la altura del tobillo apoyado (se aprende: la mas baja vista)
    float pelvis;           // la bajada de la pelvis de ahora (suavizada)
    float pasoT[2];         // el pasito: lo que va (0..1; <0: no da ninguno)
    float pasoDe[2][3];     // de donde salio el pie (mundo)
    float frente[2][3];     // un eje horizontal del cuerpo cuando apoyo (el giro desde entonces)
    bool hayFrentePie[2];
    float polo[2][3];       // hacia donde dobla cada rodilla (en el modelo)
    bool hayPolo[2];
    float desdePaso;        // segundos desde que empezo el ultimo pasito
    float antes[2][3];      // donde estaba el tobillo de la animacion el cuadro anterior (mundo)
    float vel[2];           // su velocidad sobre el piso (unidades por segundo)
    bool listo;
    W3dLocoPiesEstado();
};
// apoya los pies: 'modelo' es la afin modelo -> mundo (3x4 por columnas: x, y, z, traslacion). 'clavar' = false
// (parado sin girar no hace falta; en el aire) apoya sin clavar: los pies que estaban clavados en otro lado vuelven con
// un pasito a donde los pone la animacion
void W3dLocoPies(const W3dLocoEsqueleto& e, float (*q)[4], float (*t)[3], const float* modelo,
                 const W3dLocoPiesDef& d, W3dLocoPiesEstado& s, W3dLocoSueloFn suelo, void* usuario, bool clavar,
                 float dt);

#endif // W3DLOCOMOCION_H
