#ifndef W3DRIGIDO_H
#define W3DRIGIDO_H

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
//    * COSTO CERO si nadie lo usa: sin cuerpos, W3dRigidosPaso sale en la
//      primera linea.
//    * El estado runtime vive en una lista lateral (un slot por cuerpo), NO
//      dentro de Object. Lo unico que persiste en el objeto es su DEFINICION
//      (W3dRigidoDef* Object::fisica, el bloque "fisica" del .w3d): tipo,
//      masa, caja y centro en espacio LOCAL del objeto, friccion y rebote.
//    * la simulacion ESCRIBE pos/rot del objeto (cuerpos dinamicos). Por eso
//      los cuerpos se asumen objetos de PRIMER NIVEL (colgados de la escena,
//      sin padre con transform): un cuerpo emparentado se salta con un aviso.
//
//  TIPOS de cuerpo:
//    0 ESTATICO   no se mueve nunca (piso, paredes). Sin masa.
//    1 DINAMICO   cae, choca, rota (autos, cajas).
//    2 PERSONAJE  dinamico con la rotacion BLOQUEADA (siempre parado) y sin
//                 rebote: el script le fija la velocidad horizontal y la
//                 fisica pone gravedad + choques (no atraviesa un auto y un
//                 auto lo empuja).
//
//  QUIEN LO LLAMA:
//    * SimPlay (editor) / arranque del juego: W3dRigidosAsegurar(escena)
//    * cada tick, ANTES de los scripts:        W3dRigidosPaso(dt)
//    * Stop / descarga:                        W3dRigidosLimpiar()
//    * importarW3D en runtime:                 W3dRigidosAsegurar(lo nuevo)
//
//  BINDS LUA (W3dRigidosRegistrarBinds, los registra RegistrarAPI del Core):
//    fisicaVel(obj [,vx,vy,vz])        lee o fija la velocidad lineal
//    fisicaVelAng(obj [,wx,wy,wz])     idem angular (rad/seg, ejes de mundo)
//    fisicaImpulso(obj, ix,iy,iz [,px,py,pz])   impulso instantaneo (masa*vel)
//    fisicaFuerza(obj, fx,fy,fz [,px,py,pz])    fuerza DE ESTE FRAME (se
//                                      acumula y se consume en el proximo paso)
//    fisicaRayo(ox,oy,oz, dx,dy,dz, max [,ignorar]) -> dist,nx,ny,nz,obj | nil
//    contactos() -> { {a,b, x,y,z, nx,ny,nz, impulso}, ... } del ultimo paso
//    fisicaGravedad([g]) lee o fija el modulo de la gravedad (default 9.81)
// ============================================================================

#include <vector>
#include "math/Vector3.h"

class Object;

// definicion que PERSISTE (bloque "fisica" del objeto en el .w3d)
struct W3dRigidoDef {
    int   tipo;        // 0 estatico | 1 dinamico | 2 personaje
    float masa;        // kg (los estaticos la ignoran)
    float caja[3];     // tamano TOTAL de la caja en espacio LOCAL del objeto
    float centro[3];   // centro de la caja respecto del origen del objeto
    float friccion;    // 0..1 (por par se combina con sqrt(a*b))
    float rebote;      // 0..1
    W3dRigidoDef() {
        tipo = 1; masa = 1.0f;
        caja[0] = caja[1] = caja[2] = 1.0f;
        centro[0] = centro[1] = centro[2] = 0.0f;
        friccion = 0.5f; rebote = 0.0f;
    }
};

// un contacto reportado a los scripts (el punto mas hundido del par, con el
// impulso normal acumulado del paso: "que tan fuerte fue el golpe")
struct W3dRigidoContacto {
    Object* a;
    Object* b;
    float punto[3];    // en mundo
    float normal[3];   // de a hacia b
    float impulso;     // kg*m/s aplicado en el paso
};

// ---- ciclo de vida ---------------------------------------------------------
// recorre el subarbol y crea el cuerpo runtime de cada objeto con ->fisica que
// todavia no lo tenga (idempotente; los emparentados se saltan con aviso).
void W3dRigidosAsegurar(Object* raiz);
void W3dRigidosLimpiar(void);
void W3dRigidosOlvidar(Object* o);      // si el juego borra un objeto a mano
bool W3dRigidosHay(void);

// ---- simulacion ------------------------------------------------------------
void W3dRigidosPaso(float dt);
const std::vector<W3dRigidoContacto>& W3dRigidosContactos(void);

// ---- consultas/acciones desde C++ (los binds usan estas mismas) ------------
bool W3dRigidoVel(Object* o, Vector3* out);
void W3dRigidoSetVel(Object* o, const Vector3& v);
bool W3dRigidoVelAng(Object* o, Vector3* out);
void W3dRigidoSetVelAng(Object* o, const Vector3& v);
void W3dRigidoImpulso(Object* o, const Vector3& imp, const Vector3& puntoMundo);
void W3dRigidoFuerza(Object* o, const Vector3& f, const Vector3& puntoMundo);
void W3dRigidoActivo(Object* o, bool activo);   // apagado = no colisiona
// rayo contra TODAS las cajas (menos 'ignorar'); devuelve la distancia al
// impacto mas cercano o <0 si no toco nada.
float W3dRigidosRayo(const Vector3& origen, const Vector3& dir, float maxDist,
                     Object* ignorar, Vector3* normalOut, Object** tocadoOut);
void  W3dRigidosGravedad(float g);
float W3dRigidosGravedadActual(void);

// ---- binds lua (recibe el lua_State como void*, igual que W3dFisica) -------
void W3dRigidosRegistrarBinds(void* L);

#endif // W3DRIGIDO_H
