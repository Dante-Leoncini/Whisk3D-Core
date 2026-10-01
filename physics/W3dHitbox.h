#ifndef W3DHITBOX_H
#define W3DHITBOX_H

// ============================================================================
//  W3dHitbox — CAJAS DE DETECCION (triggers estilo Unity) del Core, y el
//  despacho de los EVENTOS de contacto de los cuerpos rigidos a los scripts.
//
//  QUE ES: un objeto Hitbox es una caja orientada (tamano + centro en espacio
//  LOCAL del objeto; la escala de MUNDO se aplica) que NO empuja nada: solo
//  avisa. Cada tick el motor calcula que pares de hitbox se SOLAPAN (y, si el
//  hitbox lo pide, que cuerpos rigidos le entraron) y le avisa a los scripts:
//
//    alEntrar(otro, miHitbox, suHitbox)    el tick en que el par empieza a solaparse
//    alQuedarse(otro, miHitbox, suHitbox)  cada tick SIGUIENTE en que sigue solapado
//    alSalir(otro, miHitbox, suHitbox)     el primer tick en que ya no se solapa
//                                          (tambien si uno se apaga o se oculta)
//    alTocar(otro, impulso, x,y,z, nx,ny,nz)   un par de CUERPOS RIGIDOS empieza a
//                                          tocarse (los contactos de W3dRigido); la
//                                          normal va de este objeto hacia 'otro'. Un par
//                                          que se DUERME apoyado o que la fisica deja de
//                                          reportar por 0,15 s o menos sigue "tocandose"
//                                          (no vuelve a avisar)
//
//  A QUIEN SE LE AVISA: a los scripts del HITBOX y a los de su PADRE (si el
//  padre no es la raiz de la escena ni una coleccion). 'otro' es el DUENO del
//  otro hitbox (su padre, con la misma regla; si no, el hitbox mismo) o el
//  cuerpo rigido; 'suHitbox' es nil cuando el otro es un cuerpo. Un cuerpo
//  rigido que entra a un hitbox (detectarCuerpos) recibe en SUS scripts
//  alEntrar/alQuedarse/alSalir(duenoDelHitbox, nil, hitbox).
//
//  REGLAS:
//    * etiqueta / filtro: un hitbox con filtro solo REACCIONA (recibe eventos,
//      lo lista hitboxDentro) a otros cuyo 'etiqueta' sea igual al filtro. Vacio
//      = todos. Cada lado filtra por su cuenta. Los cuerpos rigidos no tienen
//      etiqueta: un hitbox con filtro no los ve (ni ellos a el).
//    * los hitbox del MISMO dueno no se detectan entre si, ni un hitbox con el
//      cuerpo rigido que es su dueno (un auto con su sensor).
//    * inactivo (activo=false / setHitboxActivo) u OCULTO (el objeto o algun
//      ancestro con visible=false) = no participa. Un cuerpo rigido oculto
//      tampoco: ni entra a los hitbox ni dispara alTocar (en el ni en el otro).
//    * pares identificados por Object::serial (nunca punteros viejos): si uno de
//      los dos se destruye, el par se olvida SIN evento. El serial es solo la
//      IDENTIDAD: cambia entre cargas y depende de la historia de creacion.
//    * orden de un tick: alTocar, alSalir, alEntrar, alQuedarse. Cada grupo en el
//      orden del ARBOL (preorden del outliner, como SimPlay corre los scripts): la
//      misma escena da el mismo orden en el Play del editor, reabierta y en el
//      juego compilado. hitboxDentro() lista en ese mismo orden.
//
//  DISENO (las reglas de la casa, como W3dRigido):
//    * C++03 puro, sin motor externo.
//    * COSTO CERO si nadie lo usa: sin hitboxes vivos ni contactos, el paso sale
//      en la primera linea.
//    * broadphase: GRILLA uniforme (celda = 2 * radio medio) con cubetas por hash
//      (orden por conteo, O(n)); las cajas enormes (mas de 4x4x4 celdas) van a
//      una lista aparte que se prueba contra todo. Cada candidato se poda por
//      esfera y por caja alineada a los ejes antes del solape exacto: SAT de 15
//      ejes (el mismo test de cajas que W3dRigido, sin manifold: solo si/no). Un
//      hitbox rotado bajo un padre con escala NO uniforme es un PARALELEPIPEDO (su
//      matriz de mundo tiene cizalla): ahi el SAT usa las normales de cara de verdad
//      y la esfera la diagonal mas larga.
//      3000 hitbox al azar: unos pocos ms por paso en un PC.
//
//  QUIEN LO LLAMA (editor TickReal y runtime W3dGameActualizar, mismo orden):
//      W3dFisicaPaso -> W3dRigidosPaso -> W3dHitboxPaso(dt) -> actualizar() de scripts
//    * Stop / rewind / descarga:  W3dHitboxLimpiar() (sin eventos)
//
//  BINDS LUA (W3dHitboxRegistrarBinds, los registra RegistrarAPI del Core). 'o'
//  es un hitbox, o un objeto con hitboxes HIJOS directos (se usan todos):
//    hitboxActivo(o) -> bool
//    setHitboxActivo(o, bool)
//    setHitboxTam(o, x, y, z)          ancho X, alto Y, largo Z (locales)
//    hitboxDentro(o) -> { otro, ... }  lo que esta adentro AHORA (duenos / cuerpos)
//    hitboxTocando(o, otro) -> bool    'otro' = el dueno, el hitbox o el cuerpo
//    verHitboxes(bool)                 overlay de DEBUG: dibuja las cajas en el
//                                      juego compilado (el editor siempre las dibuja)
//                                      hasta el fin de la partida (Stop)
//
//  El formato en el .w3d ("tipo":"hitbox") y estas reglas, para quien escribe
//  escenas o scripts: formato/hitbox.md.
// ============================================================================

#include <vector>
#include <string>
#include "objects/Objects.h"
#include "math/Vector3.h"

// ---------------------------------------------------------------------------
//  LA DEFINICION que persiste ("tipo":"hitbox" en el .w3d). Es la base de la
//  clase Hitbox del editor (main/objects/Hitbox.h), que agrega el dibujo. Vive
//  en el Core para que el motor de solapes la lea sin conocer main/.
// ---------------------------------------------------------------------------
class W3dHitboxBase : public Object {
public:
    bool  activo;
    float tam[3];          // ancho X, alto Y, largo Z en espacio LOCAL (la escala de mundo se aplica)
    float centro[3];       // centro de la caja respecto del origen del objeto (local)
    std::string etiqueta;  // lo que ven los filtros de los OTROS hitbox
    std::string filtro;    // solo reacciona a otros con esta etiqueta ("" = todos)
    bool  detectarCuerpos; // tambien contra los cuerpos rigidos (Object::fisica)

    W3dHitboxBase(Object* parent, const std::string& nombre, const Vector3& pos);
    virtual ~W3dHitboxBase();
    ObjectType getType() W3D_OVERRIDE { return ObjectType::hitbox; }
};

// el DUENO de un hitbox: su padre si cuelga de algo que no es la raiz de la escena (ni una
// coleccion, que solo ordena el arbol); si no, el hitbox mismo. Es el 'otro' que ven los
// scripts del otro lado y el segundo destinatario de los eventos.
Object* W3dHitboxDuenio(Object* hb);

// ---- el paso -----------------------------------------------------------------
// dt = el del tick (solo lo usa la gracia de los contactos, ver alTocar arriba)
void W3dHitboxPaso(float dt);
void W3dHitboxLimpiar(void);          // Stop / rewind / descarga: sin pares ni contactos (sin eventos)
// FIN DE LA PARTIDA (Stop del editor, abrir otro proyecto: W3dScriptDescargarTodo): Limpiar y
// ademas apaga el overlay de debug que haya prendido un script (verHitboxes). El rebobinado NO
// la usa: la partida sigue y el overlay tambien.
void W3dHitboxFinPartida(void);
// ~Object avisa aca: los pares que nombran al objeto que se destruye se olvidan SIN evento
void W3dHitboxOlvidar(Object* o);
bool W3dHitboxHay(void);              // hay algun hitbox vivo en el proceso?

// ---- consultas (los binds usan estas mismas) ---------------------------------
// true si el hitbox esta solapado con algo que le importa (su filtro) en el ultimo paso.
// Es lo que pinta de AMARILLO la caja en el editor durante el Play.
bool W3dHitboxTocaAlgo(const Object* hb);
// lo que esta adentro de 'o' (hitbox, o sus hitbox hijos) AHORA: los duenos del otro lado, sin
// repetir, en el orden de los pares. Devuelve la cantidad.
int  W3dHitboxDentro(Object* o, std::vector<Object*>* out);
// 'o' (hitbox o sus hijos) esta solapado con 'otro' (el dueno, el hitbox o el cuerpo)?
bool W3dHitboxTocando(Object* o, Object* otro);
bool W3dHitboxActivo(Object* o);
void W3dHitboxSetActivo(Object* o, bool activo);
void W3dHitboxSetTam(Object* o, float x, float y, float z);

// cantidades del ultimo paso (harness / overlay de stats)
struct W3dHitboxStats {
    int hitboxes;      // hitbox que participaron (activos y visibles)
    int cuerpos;       // cuerpos rigidos que entraron al broadphase
    int probados;      // pares que llegaron al SAT
    int pares;         // pares solapados
    int contactos;     // pares de cuerpos rigidos en contacto
    W3dHitboxStats() : hitboxes(0), cuerpos(0), probados(0), pares(0), contactos(0) {}
};
W3dHitboxStats W3dHitboxEstadisticas(void);

// solape de dos cajas orientadas (SAT de 15 ejes, el de W3dRigido sin manifold). c = centro en
// mundo, u = ejes (unitarios), h = semiejes. Tocarse justo cuenta como solape. Los ejes pueden
// no ser perpendiculares (paralelepipedo: la caja de un hitbox con cizalla en su matriz de mundo).
bool W3dObbSolapan(const Vector3& cA, const Vector3* uA, const Vector3& hA,
                   const Vector3& cB, const Vector3* uB, const Vector3& hB);

// OVERLAY de DEBUG del juego compilado: con true la clase Hitbox dibuja las cajas aunque el
// render no tenga overlays (bind verHitboxes). Default false; es estado de la PARTIDA: lo
// vuelve a false W3dHitboxFinPartida (el Stop no lo deja prendido para el Render ni otro archivo).
extern bool g_w3dHitboxVer;

// ---- binds lua (recibe el lua_State como void*, igual que W3dRigido) ---------
void W3dHitboxRegistrarBinds(void* L);

#endif // W3DHITBOX_H
