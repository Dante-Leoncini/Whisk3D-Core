#ifndef W3D_PHYSICS_H
#define W3D_PHYSICS_H

// ============================================================================
//  W3dFisica — la fisica MINIMA del Core: velocidad + rebote AABB. Nada mas.
//  Pensada para juegos simples y livianos (2D y 3D) que tienen que correr hasta
//  en un Symbian: C++03 puro, sin motor externo, sin allocations por frame y
//  con COSTO CERO si nadie usa velocidad (el paso sale en la primera linea).
//
//  QUE HACE (y que NO):
//    * integra pos += velocidad*dt de los objetos que tienen velocidad
//    * rebota AABB contra otro objeto, contra un rect o contra el area de otro
//      objeto, separando para que no quede pegado
//    * NO hay gravedad, ni masa, ni rotacion, ni friccion, ni resolucion de
//      contactos multiples. Eso lo hace el juego en lua si lo necesita.
//
//  DONDE CORRE: W3dPhysicsStep(dt) lo llaman los DOS lados con el MISMO dt, y
//  ANTES de correr los scripts del frame:
//      runtime  -> W3dGameUpdate()  (Whisk3D-Examples/game/w3drun.cpp)
//      editor   -> TickReal()           (main/script/SimGame.cpp, el Play)
//  Asi el juego se comporta IGUAL compilado y en el Play. El orden "primero
//  mover, despues correr el script" es a proposito: el script ve la posicion
//  nueva y resuelve el choque (rebotar) en el MISMO frame, antes de dibujar.
//
//  ESPACIOS (importante). Cada objeto se integra en SU espacio, el mismo que ya
//  usan los binds de lua para leerlo/escribirlo:
//    * objetos 2D (ui/texto/imagen/rect/contenedor/slice9/button/expandir/video):
//      LIENZO en px, centro (0,0) — igual que getPosPx/getSizePx/uiBox/collide. Ejes:
//      +x a la DERECHA, +y hacia ABAJO (es una pantalla, no un plano matematico).
//      En el objeto la posicion se guarda como FRACCION del lienzo; la conversion
//      px<->fraccion la hace la fisica sola (el adaptador de abajo le da el
//      tamano del lienzo y el tamano del objeto).
//    * objetos 3D: unidades del MOTOR, ejes del motor (Y arriba), y la posicion
//      es LOCAL (relativa al padre), igual que getPosition()/move().
//  El eje Z: en 3D es el eje de profundidad normal; en 2D es la profundidad en
//  px del elemento (se integra tal cual, sin conversion). Un juego 2D deja vz=0.
//
//  OJO 2D: el CUERPO MOVIL (el que tiene velocidad) se lee/escribe CRUDO
//  (pos * lienzo), exactamente como getPosPx/setPosPx. Es a proposito: la fisica
//  integra y separa escribiendo esa pos cruda, y es para los objetos que el
//  juego mueve libremente (la pelota, las palas: hijos directos de la raiz,
//  ancla centro, layout libre), no para widgets anclados/flex.
//  Los OTROS operandos siguen la regla CRUDO-vs-RESUELTO (la misma de chocan
//  en BindsJuego):
//    * OBSTACULO de bounce(obj, otro): si su pos cruda es autoritativa (lo
//      dice el adaptador 'cruda' de abajo: layout libre + ancla centro) se usa
//      la caja cruda de ESTE frame (una pala movida por el script este mismo
//      frame pega donde esta AHORA, sin lag); si lo acomoda el layout se usa
//      el rect RESUELTO del ultimo render (donde se dibuja de verdad).
//    * AREA (la cancha de bounceOn(obj, area)): SIEMPRE el rect del layout
//      (el mismo que da uiBox). Un area es justamente un widget que el layout
//      acomoda, y asi el juego no tiene que recalcular ni un borde: el layout
//      vive en el .w3dui. (Un area no se mueve por script: no hay lag posible.)
//
//  CAJA de colision (la que usan los rebotes):
//    * 2D: por defecto el rect del elemento (ancho x alto en px de lienzo), el
//      MISMO que compara collide(). Cero configuracion.
//    * 3D: por defecto un cubo de lado 2*FocusRadius() (el bounding de la malla)
//      centrado en el objeto. Es una aproximacion: si no alcanza, el juego fija
//      la caja exacta con box(obj, ancho, alto, profundo).
//
//  VIDA de los datos: la getVelocity/box de un objeto vive en una lista chica
//  (un slot por objeto que se mueve). Se borra todo en W3dPhysicsClear(), que
//  llama W3dScriptUnloadAll() -> el Stop del editor y el fin del juego
//  dejan la lista vacia. No se guarda nada dentro de Object.
//  Un CAMBIO DE ESCENA no la borra (puede haber cuerpos que no son de ninguna
//  escena): si una escena deja algo moviendose, el script la frena al salir con
//  getVelocity(obj, 0, 0) o la vuelve a fijar en su start().
// ============================================================================

class Object;

// ---- paso de simulacion ----------------------------------------------------
// integra pos += vel*dt de TODOS los cuerpos con velocidad. Si no hay ninguno
// no hace nada (un juego sin fisica no paga costo).
void W3dPhysicsStep(float dt);
// olvida todos los cuerpos (Stop / fin del juego). La llama W3dScriptUnloadAll().
void W3dPhysicsClear(void);
// olvida UN objeto (por si el juego lo destruye a mano).
void W3dPhysicsForget(Object* o);
// true si el objeto tiene un cuerpo (velocidad o caja propia asignada).
bool W3dPhysicsHas(Object* o);
// true si hay algun cuerpo vivo.
bool W3dPhysicsAny(void);

// ---- velocidad desde C++ (el editor la usa para su snapshot por frame) -----
// unidades: px de lienzo por segundo en 2D, unidades del motor por segundo en 3D.
void W3dPhysicsGetVel(Object* o, float* vx, float* vy, float* vz);
void W3dPhysicsSetVel(Object* o, float vx, float vy, float vz);

// ---- adaptador 2D ----------------------------------------------------------
// El Core NO conoce el lienzo del juego ni el Element2D (viven en main/), asi
// que main/ le presta cuatro funciones (las registra BindsGameRegister, que
// corre tanto en el editor como en el runtime):
//   canvas(w,h)      -> tamano del lienzo del juego en px (UI2D_CanvasSize)
//   size2d(o,anc,alt) -> tamano del objeto 2D en px de lienzo; false si no es 2D
//   cajaui(o,...)    -> rect RESUELTO por el layout (anclas/flex) del ultimo
//                       render, centro + tamano en px de lienzo; false si ese
//                       objeto no se dibujo todavia. Es lo mismo que devuelve
//                       uiBox() en lua; se usa para las AREAS y para los
//                       OBSTACULOS no autoritativos (ver la nota de arriba).
//                       Puede ser NULL.
//   raw(o)         -> true si la pos CRUDA del objeto es AUTORITATIVA (hijo
//                       directo de la raiz + layout libre + ancla centro: la
//                       regla crudo-vs-resuelto de chocan, RawPosAuthoritative
//                       en BindsJuego). Decide la caja del OBSTACULO de
//                       bounce(). NULL = todo se considera crudo (el
//                       comportamiento historico).
// SIN adaptador registrado la fisica trata TODO como 3D (las posiciones de los
// objetos 2D quedarian en fracciones): los dos builds reales lo registran.
typedef void (*W3dPhysicsCanvasFn)(float* w, float* h);
typedef bool (*W3dPhysicsSize2DFn)(Object* o, float* width, float* height);
typedef bool (*W3dPhysicsBoxUIFn)(Object* o, float* cx, float* cy, float* width, float* height);
typedef bool (*W3dRawPhysicsFn)(Object* o);
void W3dPhysicsSetAdapter2D(W3dPhysicsCanvasFn canvas, W3dPhysicsSize2DFn size2d,
                             W3dPhysicsBoxUIFn cajaui, W3dRawPhysicsFn raw);

// ---- binds lua -------------------------------------------------------------
// los registra RegisterAPI de script/W3dScript.cpp (por eso los ve igual el
// Play del editor y el juego compilado). Recibe el lua_State crudo como void*.
void W3dPhysicsRegisterBinds(void* L);

#endif // W3D_PHYSICS_H
