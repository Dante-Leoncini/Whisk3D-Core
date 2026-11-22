#ifndef W3D_RIGID_H
#define W3D_RIGID_H

// ============================================================================
//  W3dRigido — CUERPOS RIGIDOS de caja del Core (la fisica "de verdad", al lado
//  de la minima de W3dFisica que solo integra velocidad y rebota AABB).
//
//  QUE ES: cajas orientadas (OBB) con masa, gravedad, impulsos con friccion y
//  rebote, raycast y eventos de contacto para los scripts lua. Alcanza para un
//  juego de conduccion/accion simple: autos que chocan, vuelcan y empujan, un
//  personaje que no atraviesa nada, un piso y paredes estaticas.
//
//  DISENO (las mismas reglas de la casa que W3dFisica):
//    * C++03 puro, sin motor externo, sin allocations por frame: corre en el
//      N95 igual que en PC.
//    * COSTO CERO si nadie lo usa: sin cuerpos, W3dRigidStep sale en la
//      primera linea.
//    * El estado runtime vive en una lista lateral (un slot por cuerpo), NO
//      dentro de Object. Lo unico que persiste en el objeto es su DEFINICION
//      (W3dRigidDef* Object::fisica, el bloque "fisica" del .w3d): tipo,
//      masa, caja y centro en espacio LOCAL del objeto, friccion y rebote.
//    * la simulacion ESCRIBE pos/rot del objeto (cuerpos dinamicos). Por eso
//      los cuerpos se asumen objetos de PRIMER NIVEL (colgados de la escena,
//      sin padre con transform): un cuerpo emparentado se salta con un aviso.
//      Excepcion: lo GENERADO por una instancia de prefab (a cualquier
//      profundidad): simula en mundo con la escala de mundo y escribe en el
//      espacio de su padre (ver SpaceOf en el .cpp).
//
//  TIPOS de cuerpo:
//    0 ESTATICO   no se mueve nunca (piso, paredes). Sin masa.
//    1 DINAMICO   cae, choca, rota (autos, cajas).
//    2 PERSONAJE  dinamico con la rotacion BLOQUEADA (siempre parado) y sin
//                 rebote: el script le fija la velocidad horizontal y la
//                 fisica pone gravedad + choques (no atraviesa un auto y un
//                 auto lo empuja). Contra una MALLA sube escalones (un
//                 cordon, una escalera) de hasta ~30 cm.
//    3 MALLA      estatico con la forma de SUS TRIANGULOS (el objeto es una malla:
//                 el piso irregular, las rampas y las paredes de un mapa). Las
//                 cajas chocan contra cada triangulo; la box/centro/masa de la
//                 definicion se ignoran. Lee la malla con la grilla de
//                 W3dColMalla (una vez, en mundo: se asume QUIETA).
//
//  QUIEN LO LLAMA:
//    * SimPlay (editor) / arranque del juego: W3dRigidEnsure(escena)
//    * cada tick, ANTES de los scripts:        W3dRigidStep(dt)
//    * Stop / descarga:                        W3dRigidClear()
//    * importW3D en runtime:                 W3dRigidEnsure(lo nuevo)
//
//  BINDS LUA (W3dRigidRegisterBinds, los registra RegisterAPI del Core):
//    physicsVel(obj [,vx,vy,vz])        lee o fija la velocidad lineal
//    physicsAngVel(obj [,wx,wy,wz])     idem angular (rad/seg, ejes de mundo)
//    physicsImpulse(obj, ix,iy,iz [,px,py,pz])   impulso instantaneo (masa*vel)
//    physicsForce(obj, fx,fy,fz [,px,py,pz])    fuerza DE ESTE FRAME (se
//                                      acumula y se consume en el proximo paso)
//    physicsRay(ox,oy,oz, dx,dy,dz, max [,ignorar]) -> dist,nx,ny,nz,obj | nil
//                                      (cajas Y cuerpos de tipo malla)
//    contacts() -> { {a,b, x,y,z, nx,ny,nz, impulso}, ... } del ultimo paso
//    physicsGravity([g]) lee o fija el modulo de la gravedad (default 9.81)
// ============================================================================

#include <vector>
#include "math/Vector3.h"

class Object;

// definicion que PERSISTE (bloque "fisica" del objeto en el .w3d)
struct W3dRigidDef {
    int   type;        // 0 estatico | 1 dinamico | 2 personaje | 3 malla
    float mass;        // kg (los estaticos la ignoran)
    float box[3];      // tamano TOTAL de la caja en espacio LOCAL del objeto
    float center[3];   // centro de la caja respecto del origen del objeto
    float friction;    // 0..1 (por par se combina con sqrt(a*b))
    float bounce;      // 0..1
    W3dRigidDef() {
        type = 1; mass = 1.0f;
        box[0] = box[1] = box[2] = 1.0f;
        center[0] = center[1] = center[2] = 0.0f;
        friction = 0.5f; bounce = 0.0f;
    }
};

// un contacto reportado a los scripts (el punto mas hundido del par, con el
// impulso normal acumulado del paso: "que tan fuerte fue el golpe")
struct W3dRigidContact {
    Object* a;
    Object* b;
    float point[3];    // en mundo
    float normal[3];   // de a hacia b
    float impulse;     // kg*m/s aplicado en el paso
};

// ---- ciclo de vida ---------------------------------------------------------
// recorre el subarbol y crea el cuerpo runtime de cada objeto con ->physics que
// todavia no lo tenga (idempotente; los emparentados se saltan con aviso).
void W3dRigidEnsure(Object* root);
void W3dRigidClear(void);
void W3dRigidForget(Object* o);      // si el juego borra un objeto a mano
bool W3dRigidHas(void);

// ---- simulacion ------------------------------------------------------------
void W3dRigidStep(float dt);
const std::vector<W3dRigidContact>& W3dRigidContacts(void);

// ---- consultas/acciones desde C++ (los binds usan estas mismas) ------------
bool W3dRigidVel(Object* o, Vector3* out);
void W3dRigidSetVel(Object* o, const Vector3& v);
bool W3dRigidVelAng(Object* o, Vector3* out);
void W3dRigidSetVelAng(Object* o, const Vector3& v);
void W3dRigidImpulse(Object* o, const Vector3& imp, const Vector3& pointWorld);
void W3dRigidForce(Object* o, const Vector3& f, const Vector3& pointWorld);
void W3dRigidActive(Object* o, bool active);   // apagado = no colisiona
// rayo contra TODAS las cajas (menos 'ignorar'); devuelve la distancia al
// impacto mas cercano o <0 si no toco nada.
float W3dRigidRay(const Vector3& origin, const Vector3& dir, float maxDist,
                     Object* ignore, Vector3* normalOut, Object** touchedOut);
void  W3dRigidGravity(float g);
float W3dRigidCurrentGravity(void);

// ---- las CAJAS en mundo, por indice (las lee el motor de HITBOX, physics/W3dHitbox.h) --
// cuantos cuerpos runtime hay, y la caja del i-esimo DESPUES del ultimo paso: su objeto, el
// centro en mundo, los ejes (columnas de la rotacion) y los semiejes (escala ya horneada).
// 'activo' = physicsActive; 'quieto' = estatico o dormido (la fisica NO evalua los pares
// entre dos quietos: sus contactos no se reportan mientras sigan asi). false = fuera de rango
// o un cuerpo de tipo MALLA (no es una caja).
int  W3dRigidCount(void);
bool W3dRigidBox(int i, Object** o, Vector3* center, Vector3 axes[3], Vector3* semi,
                   bool* active, bool* still);

// ---- binds lua (recibe el lua_State como void*, igual que W3dFisica) -------
void W3dRigidRegisterBinds(void* L);

#endif // W3D_RIGID_H
