#include "Objects.h"
#include "script/W3dScript.h" // W3dScriptDatos completo (el destructor lo libera)
#include "physics/W3dRigido.h" // W3dRigidoDef completo (idem) + W3dRigidosOlvidar
#include "w3dGraphics.h" // flags de estado de render (w3dRenderLuces) — PC y Symbian
#include "W3dNombres.h"  // LA regla de nombres unicos (una sola, compartida por todo el editor)
#include "CameraBase.h"  // la vista BINDEADA (g_renderCam*): es lo que leen los constraints
#include "w3dlog.h"      // avisos (ciclo, vista sin bindear, base espejada): el Core no notifica

// RAIZ de la escena. La DEFINE el Core (antes solo la declaraba y la definia el editor, asi que
// nadie podia enlazar contra el motor sin traerse el editor entero). El editor la llena con su
// propia raiz (una Scene) en el arranque; para el motor alcanza con que exista y valga 0.
Object* SceneCollection = 0;
// LOCAL VIEW (ver Objects.h): el editor los publica por-frame; Object::Render los lee para AISLAR la seleccion.
bool g_localViewActivo = false;
const std::set<Object*>* g_localViewVisibles = 0;

#ifdef W3D_SYMBIAN
// Symbian: GLES + utilidades de C que en PC llegan por otros headers.
#include <GLES/gl.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdlib.h>   // atoi
#include <ctype.h>    // isdigit
#endif

// CONTADOR de identidad estable (ver Object::serial en Objects.h). Monotono y sin
// reuso: cada Object que nace se lleva el siguiente y nadie lo devuelve. Arranca en 1
// para que 0 pueda significar "ninguno". Si diera la vuelta harian falta 4 mil millones
// de objetos en UNA sesion; llegado el caso lo que hay que hacer es ensancharlo, NO
// reciclar (reciclar es exactamente el bug que esto evita).
// (static: no lo declara ningun header, o sea que nadie de afuera lo puede leer ni
//  -peor- escribir; sin esto tenia enlace externo "de casualidad")
static unsigned int W3dSerialProx = 1;

// ganchos para desvincular tambien lo que NO cuelga del arbol vivo (ver Objects.h).
// Todo NULL = nadie engancho (el motor sin editor). Inicializacion ESTATICA (constante):
// vale desde antes del primer constructor global, asi que registrarse desde uno es seguro.
W3dDesvincularFn W3dDesvincularGanchos[W3dDesvincularMaxGanchos] = {0,0,0,0,0,0,0,0};
bool W3dDesvincularRegistrar(W3dDesvincularFn f){
    if (!f) return false;
    for (int i = 0; i < (int)W3dDesvincularMaxGanchos; i++){
        if (W3dDesvincularGanchos[i] == f) return true;   // ya estaba
        if (!W3dDesvincularGanchos[i]) { W3dDesvincularGanchos[i] = f; return true; }
    }
    return false;   // sin lugar: agrandar W3dDesvincularMaxGanchos
}

// Variables globales
std::vector<Object*> ObjSelects;
Object* CollectionActive = NULL;
Object* ObjActivo = NULL;
Object* g_editMesh = NULL;        // edit mode: malla activa (NULL = nadie)
int EditSelectMode = SelVertex;   // edit mode: sub-elemento (vertex por defecto)
std::vector<SaveState> estadoObjetos;

#ifdef W3D_SYMBIAN
// en PC lo define ViewPort3D.cpp (todavia sin portar); SceneCollection ya
// vive en Scene.cpp para ambas plataformas
// (Viewport3DActive: ahora lo define ViewPort3D.cpp real)
#endif

Object::Object(Object* parent, const std::string& nombre, Vector3 Pos, Vector3 Rot, Vector3 Scale)
    : Parent(parent),
      visible(true),
      renderizable(true),
      estatico(true),   // por defecto NO se mueve en runtime (convencion RenderWare); el juego marca dinamicos a Crash/enemigos
      desplegado(true),
      showRelantionshipsLines(true),
      posadoPorCurvas(false),   // nace en reposo: nadie lo poso todavia
      select(false), // Seleccionar() al final lo deja seleccionado
      name(nombre),
      pos(Pos),
      scale(Scale) {

    // Convertir los ángulos de Euler (Rot.x, Rot.y, Rot.z) a tres cuaterniones simples.
    // Usaremos un orden común para una configuración inicial: YXZ (Yaw, Pitch, Roll)
    // Orden de aplicación: Z (Roll) se aplica primero, luego X (Pitch), luego Y (Yaw).
    
    Quaternion qX = Quaternion::FromAxisAngle(Vector3(1, 0, 0), Rot.x); // Pitch (Eje X)
    Quaternion qY = Quaternion::FromAxisAngle(Vector3(0, 1, 0), Rot.y); // Yaw (Eje Y)
    Quaternion qZ = Quaternion::FromAxisAngle(Vector3(0, 0, 1), Rot.z); // Roll (Eje Z)

    // Composición de Cuaterniones (ejemplo con orden YXZ):
    // Multiplicación en C++: rot = qA * qB * qC  (qC se aplica primero)
    // Para orden YXZ: qZ (Roll) primero, luego qX (Pitch), luego qY (Yaw).
    
    rot = qY * qX * qZ; // Aplicación: (Z) * (X) * (Y)

    serial = W3dSerialProx++;    // identidad estable: se asigna al NACER y no se reusa jamas
    constraintActivo = -1;       // stack de constraints vacio: no hay ninguno seleccionado
    consEval = false;            // guardia de reentrada de la evaluacion (ver Objects.h)
    rotMode = RotEulerXYZ;       // default: XYZ Euler
    rotAngle = 0.0f;
    rotAxis = Vector3(0, 0, 1);
    scriptDatos = NULL;          // script lua opcional (ver script/W3dScript.h)
    fisica = NULL;               // cuerpo rigido opcional (ver physics/W3dRigido.h)
    ActualizarDisplayRot();

    if (Parent) {
        Parent->Childrens.push_back(this);
    } else if (SceneCollection && this != SceneCollection) {
        SceneCollection->Childrens.push_back(this);
    }

    // NOMBRE UNICO desde el nacimiento: ya esta en el arbol, asi que su scope
    // (la escena a la que pertenece) es el definitivo. Con esto los ~15 ctores
    // con nombre fijo ("Mesh", "Light", "UI", "Boton"...) dejan de generar
    // homonimos sin que cada uno tenga que acordarse. En Object el getType()
    // virtual todavia devuelve baseObject: no importa, el scope se decide
    // subiendo por Parent (una raiz UI queda con scope global, que es lo que
    // corresponde). Durante la carga no se toca (ver W3dNombresCargando).
    if (!W3dNombresCargando && SceneCollection && this != SceneCollection)
        name = NombreLibre(name);

    DeseleccionarTodo();
    Seleccionar();
}

// refresca los valores de DISPLAY (rotEuler / rotAxis+rotAngle) desde 'rot'.
// El round-trip Euler XYZ es exacto (estable al editar); el axis-angle muestra
// el eje normalizado y el angulo en [0,360].
// ============================================================================
//  EULER Y VUELTAS
//  Un quaternion NO distingue 0 de 360: son la misma orientacion. El euler SI, y esa diferencia es lo que hace
//  que una animacion gire una vuelta entera en vez de quedarse quieta.
//  Antes el euler se RE-DERIVABA del quaternion en cada actualizacion (ToEulerXYZ devuelve el canonico, -180..180)
//  y ahi se borraban las vueltas: rotar 360 volvia a 0, rotar 405 quedaba en 45. No se podia AUTORAR un giro, y
//  peor: keyframear encima de una animacion importada que SI tenia 405 la aplastaba a 45 en silencio.
//  Ahora el euler es el dato que MANDA: al re-derivarlo del quaternion se elige, entre todas sus formas
//  equivalentes, la mas parecida a la que ya tenia -> las vueltas se acumulan solas mientras cada paso sea menor a
//  media vuelta (que es siempre: el transform aplica el movimiento del mouse frame a frame).
// ============================================================================

// el equivalente de 'v' (mod 360) mas cercano a 'ref'
static float CercaMod360(float v, float ref){
    float k = floorf((ref - v) / 360.0f + 0.5f);
    return v + k * 360.0f;
}

Vector3 W3dEulerCercano(const Vector3& e, const Vector3& ref){
    // un euler no es unico: (a) sumarle 360 a cualquier componente da la MISMA rotacion, y (b) tambien la da la
    // forma "dada vuelta" (x+180, 180-y, z+180). Se prueban las dos familias y gana la que menos se aleja de 'ref'.
    Vector3 mejor = e; float mejorD = 1e30f;
    for (int flip = 0; flip < 2; flip++){
        Vector3 c = flip ? Vector3(e.x + 180.0f, 180.0f - e.y, e.z + 180.0f) : e;
        c.x = CercaMod360(c.x, ref.x);
        c.y = CercaMod360(c.y, ref.y);
        c.z = CercaMod360(c.z, ref.z);
        float d = fabsf(c.x - ref.x) + fabsf(c.y - ref.y) + fabsf(c.z - ref.z);
        if (d < mejorD){ mejorD = d; mejor = c; }
    }
    return mejor;
}

void Object::ActualizarDisplayRot(){
    // el euler se re-deriva del quaternion CONSERVANDO las vueltas que ya tenia (ver arriba)
    rotEuler = W3dEulerCercano(rot.ToEulerXYZ(), rotEuler);
    rot.ToAxisAngle(rotAngle, rotAxis);
    rotQuat = rot;
}

// EL QUATERNION MANDA (el transform de la vista, un constraint, una matriz importada): normaliza y deja todos
// los displays al dia. Es la puerta para "ya tengo la orientacion armada, ponela".
void Object::SetRot(const Quaternion& q){
    rot = q;
    rot.normalize();
    ActualizarDisplayRot();
}

// Pone la rotacion desde un EULER que ya trae sus vueltas (la curva de animacion, el panel, el .w3d): el euler
// MANDA y el quaternion se deriva de el. Usarla en vez de SetRot: ese camino tomaria el euler como "a que se
// parece" en vez de como el dato, y una vuelta entera se perderia.
void Object::SetRotEuler(const Vector3& e){
    rotEuler = e;
    rot = Quaternion::FromEulerXYZ(e.x, e.y, e.z);
    rot.ToAxisAngle(rotAngle, rotAxis);
    rotQuat = rot;
}

// EL AXIS/ANGLE MANDA (el panel en modo Axis Angle). El eje tiene que ser unitario para que el angulo signifique
// lo que dice: se normaliza aca y no en el caller.
void Object::SetRotAxisAngle(const Vector3& eje, float angDeg){
    rot = Quaternion::FromAxisAngle(eje.Normalized(), angDeg);
    rot.normalize();
    ActualizarDisplayRot();
}

// Restaura un SNAPSHOT (undo, cancelar el transform, la sim del juego): el euler se escribe TAL CUAL se habia
// capturado. No se re-deriva del quaternion a proposito: eso se comeria las vueltas que el snapshot tenia.
void Object::SetRotSnapshot(const Quaternion& q, const Vector3& euler){
    rot = q;
    rotEuler = euler;
    rot.ToAxisAngle(rotAngle, rotAxis);
    rotQuat = rot;
}

// ============================================================================
//  CONSTRAINTS (objects/W3dConstraint.h). Vive todo aca -y no en un .cpp propio-
//  porque un .cpp nuevo hay que agregarlo a las 9 listas de fuentes del proyecto
//  (el .mmp de Symbian, los 3 CMakeLists y los 3 Android.mk de los ejemplos, y las
//  dos listas de CompilarJuego.cpp); Objects.cpp ya esta en todas.
// ============================================================================

// CONTADOR de identidad estable de los constraints (ver W3dConSerial en
// objects/W3dConstraint.h). Arranca en 1: el 0 significa "no es un constraint".
static unsigned int W3dConSerialProx = 1;
W3dConSerial::W3dConSerial() : v(W3dConSerialProx++) {}
W3dConSerial::W3dConSerial(const W3dConSerial&) : v(W3dConSerialProx++) {}

// nombre por defecto de cada tipo (para la lista + el menu "Add")
const char* W3dNombreTipoConstraint(int tipo){
    switch (tipo) {
        case W3dConstraintTipo::CopyLocation: return "Copy Location";
        case W3dConstraintTipo::CopyRotation: return "Copy Rotation";
        case W3dConstraintTipo::Billboard:    return "Billboard";
    }
    return "Constraint";
}

int Object::ConstraintsCount() const { return (int)constraints.size(); }

std::string Object::NombreConstraint(int i) const {
    if (i < 0 || i >= (int)constraints.size()) return std::string();
    return constraints[i]->nombre;
}

void Object::SetConstraintActivo(int i){
    constraintActivo = (i >= 0 && i < (int)constraints.size()) ? i : -1;
}

// busca por SERIAL, nunca por indice ni por puntero (ver Objects.h). NULL = ya no esta.
W3dConstraint* Object::ConstraintPorSerial(unsigned int s) const {
    if (!s) return NULL;
    for (size_t i = 0; i < constraints.size(); i++)
        if (constraints[i]->serial.v == s) return constraints[i];
    return NULL;
}

// ============================================================================
//  EL VINCULO POR NOMBRE DE LA FUENTE (contrato en Objects.h)
//
//  Se corre UNA vez, con la escena entera ya construida. Va por FindObjectByName,
//  o sea EXACTAMENTE la misma busqueda que resuelve targetName / RielName /
//  modArmature: si dos objetos comparten nombre, el constraint queda ligado al
//  MISMO de los dos que resolveria cualquier otro vinculo del archivo.
//
//  fuenteTipo == Vista no lleva nombre y no se toca: 'Vista' es un tipo explicito
//  justo para que "sin puntero" no signifique nunca "segui a la camara".
// ============================================================================
static void W3dConsResolverRec(Object* nodo, Object* raiz, int& rotos){
    if (!nodo) return;
    for (size_t i = 0; i < nodo->constraints.size(); i++) {
        W3dConstraint* c = nodo->constraints[i];
        if (!c || c->fuenteTipo != W3dConstraintFuente::Objeto) continue;
        if (c->fuenteNombre.empty()) { c->fuenteObj = NULL; continue; }
        c->fuenteObj = FindObjectByName(raiz, c->fuenteNombre);
        if (!c->fuenteObj) {
            // el NOMBRE se conserva a proposito: es el dato del modelo, y si el objeto
            // vuelve (undo del borrado, o el usuario lo crea) el vinculo se recupera solo.
            w3dLogfW("[W3D] '%s': el constraint '%s' apunta a '%s', que no esta en la escena "
                     "(queda sin fuente y no hace nada)",
                     nodo->name.c_str(), c->nombre.c_str(), c->fuenteNombre.c_str());
            rotos++;
        }
    }
    for (size_t i = 0; i < nodo->Childrens.size(); i++)
        W3dConsResolverRec(nodo->Childrens[i], raiz, rotos);
}

// 'raiz' es a la vez lo que se RECORRE y donde se BUSCA (en la carga es SceneCollection):
// asi la busqueda de la fuente cubre exactamente los objetos que se estan resolviendo.
void W3dConstraintsResolverNombres(Object* raiz){
    if (!raiz) return;
    int rotos = 0;
    W3dConsResolverRec(raiz, raiz, rotos);
    if (rotos) w3dLogfW("[W3D] %d constraint(s) quedaron sin fuente", rotos);
}

// duplicar el stack (contrato en Objects.h). El 'new W3dConstraint(*c)' es la clave:
// el copy-ctor implicito copia todos los campos MENOS el serial, que W3dConSerial
// re-genera para que la copia tenga identidad propia.
void W3dCopiarConstraints(Object* dst, const Object* src){
    if (!dst || !src) return;
    for (size_t i = 0; i < src->constraints.size(); i++) {
        const W3dConstraint* c = src->constraints[i];
        if (!c) continue;
        dst->constraints.push_back(new W3dConstraint(*c));
    }
    // el seleccionado de la lista es estado de UI y viaja con el stack: si no, el panel
    // de la copia se abre en "ninguno" con la lista llena.
    if (!dst->constraints.empty() && dst->constraintActivo < 0)
        dst->constraintActivo = (int)dst->constraints.size() - 1;
}

// ---- LA PUERTA DE REFS: solo RUTEA (ver el bloque grande de Objects.h) ----
// [0 .. RefsPropias()-1] = la subclase; de ahi en adelante, un slot por constraint.
int Object::RefsObjeto() const { return RefsPropias() + (int)constraints.size(); }

Object* Object::RefObjeto(int i) const {
    const int propias = RefsPropias();
    if (i < propias) return RefPropia(i);
    const int c = i - propias;
    if (c < 0 || c >= (int)constraints.size()) return NULL;
    return constraints[c]->fuenteObj;
}

void Object::SetRefObjeto(int i, Object* o){
    const int propias = RefsPropias();
    if (i < propias) { SetRefPropia(i, o); return; }
    const int c = i - propias;
    if (c < 0 || c >= (int)constraints.size()) return;
    constraints[c]->fuenteObj = o;
}

std::string* Object::RefObjetoNombre(int i){
    const int propias = RefsPropias();
    if (i < propias) return RefPropiaNombre(i);
    const int c = i - propias;
    if (c < 0 || c >= (int)constraints.size()) return NULL;
    return &constraints[c]->fuenteNombre;
}

// suelta TODA referencia por puntero a 'borrado' (la enumeracion generica de Objects.h).
// El vinculo por NOMBRE se deja como esta: es dato del modelo y lo maneja quien borra.
void Object::DesvincularDe(Object* borrado){
    const int n = RefsObjeto();
    for (int i = 0; i < n; i++)
        if (RefObjeto(i) == borrado) SetRefObjeto(i, NULL);
}

// recorre el arbol y le avisa a cada objeto que 'borrado' deja de existir, asi
// el que lo referencia (ej: Instance->target) suelta el puntero y no crashea
static void DesvincularDelArbol(Object* nodo, Object* borrado){
    if (!nodo) return;
    nodo->DesvincularDe(borrado);
    for (size_t i = 0; i < nodo->Childrens.size(); i++)
        DesvincularDelArbol(nodo->Childrens[i], borrado);
}

Object::~Object() {
    delete scriptDatos; scriptDatos = NULL;   // datos del script lua (si tenia)
    W3dRigidosOlvidar(this);                  // su cuerpo rigido runtime (si tenia)
    delete fisica; fisica = NULL;             // y la definicion de fisica
    // antes de irse: soltar cualquier puntero que apunte a este objeto (las
    // instancias linkeadas a el) para evitar punteros colgados al renderizar
    if (SceneCollection && SceneCollection != this)
        DesvincularDelArbol(SceneCollection, this);
    // ...y lo que NO esta en el arbol: los subarboles DETACHADOS que tienen los comandos del
    // undo son inalcanzables desde SceneCollection, asi que el recorrido de arriba nunca les
    // llega y se quedaban apuntando a esta direccion despues del free. Lo mismo los punteros
    // que se guarda la UI del editor (popup de color, panel "Add"): ver Objects.h.
    for (int g = 0; g < (int)W3dDesvincularMaxGanchos; g++)
        if (W3dDesvincularGanchos[g]) W3dDesvincularGanchos[g](this);

    // el stack de constraints es NUESTRO (nadie mas lo comparte). Va ANTES de la
    // promocion de hijos: mientras la lista viva, las refs de constraints siguen
    // enumerandose por la puerta de arriba y un gancho que corra en el medio las ve.
    for (size_t c = 0; c < constraints.size(); c++) delete constraints[c];
    constraints.clear();

    // los hijos SUBEN al nivel del padre (antes solo se les cambiaba el
    // Parent pero nunca entraban a Parent->Childrens: quedaban huerfanos
    // fuera del arbol); se conserva la posicion v1 (traslacion)
    for (size_t i = 0; i < Childrens.size(); i++) {
        Childrens[i]->Parent = Parent;
        Childrens[i]->pos += pos;
        if (Parent) Parent->Childrens.push_back(Childrens[i]);
    }
    Childrens.clear();
}

// libera el objeto y TODO su subarbol (ver el bloque de Objects.h). Va POST-ORDEN y le saca
// la lista de hijos a cada dueno ANTES de destruirlo: cuando corre su ~Object el Childrens ya
// esta VACIO, asi que la promocion de arriba no tiene a quien mover a ningun lado. El Parent
// se pone en NULL por lo mismo (ese padre sigue vivo y no es nuestro).
void W3dLiberarSubarbol(Object* raiz){
    if (!raiz) return;
    std::vector<Object*> hijos; hijos.swap(raiz->Childrens);
    for (size_t i = 0; i < hijos.size(); i++) W3dLiberarSubarbol(hijos[i]);
    raiz->Parent = NULL;
    delete raiz;
}

// ============================================================================
//  NOMBRES UNICOS de objeto - LA PUERTA.
//
//  SCOPE (decision documentada): un objeto es unico dentro de SU ESCENA, no
//  globalmente. La escena es la raiz UI de la que cuelga (el MISMO scope con el
//  que se resuelven las refs de los scripts, SimJuego.cpp) o SceneCollection si
//  no cuelga de ninguna. Las RAICES UI en cambio son unicas GLOBALMENTE: su
//  nombre es la clave de gEscenas y el nombre del .w3dui en disco.
//  Sin esto, abrir whiskpaddle (14 widgets homonimos entre sus 3 escenas)
//  renombraria medio proyecto y dejaria los objeto("op1") de lua en nil.
// ============================================================================

// true mientras se esta CARGANDO un archivo: los loaders escriben el nombre
// crudo (asi el .w3d entra tal cual) y la reparacion se hace de una sola vez al
// final, con el arbol completo y arrastrando los vinculos por nombre. Ademas
// evita el O(n^2) de uniquificar objeto por objeto durante la carga.
bool W3dNombresCargando = false;

Object* W3dNombreScopeDe(Object* o) {
    if (!o) return SceneCollection;
    // una raiz UI (escena) NO se mide contra si misma: va contra TODO el arbol
    if (o->getType() != ObjectType::ui)
        for (Object* p = o->Parent; p; p = p->Parent)
            if (p->getType() == ObjectType::ui) return p;
    return SceneCollection;
}

// le pasa al INDICE los nombres del scope de una sola pasada (menos el propio, que
// puede conservar el suyo).
//
// LA PARTICION ES LA MISMA QUE LA DE LA REPARACION (RepararRec en ObjectMode.cpp), y
// esa coherencia importa: antes esto indexaba el SUBARBOL ENTERO desde la raiz, o sea
// que los widgets de una UI ANIDADA contaban como tomados en el scope de AFUERA. No
// fabricaba duplicados (indexar de mas solo renombra de mas), pero mudar un objeto al
// scope global lo renumeraba contra nombres que ahi no son suyos.
// La regla, textual, es la de W3dNombreScopeDe: un objeto pertenece a la UI mas cercana
// que lo contiene, y una RAIZ UI (a cualquier profundidad) pertenece al scope GLOBAL.
//   - scope GLOBAL (raiz == SceneCollection): cuenta todo lo que NO esta dentro de
//     ninguna UI, MAS todas las raices UI (hay que seguir bajando para encontrarlas).
//   - scope de una UI: cuenta sus descendientes que no cruzan otra UI; al toparse con
//     una UI anidada se corta (esa UI y todo lo suyo viven en otros scopes).
static void IndexarScopeGlobal(Object* obj, const Object* me, W3dNombreIndice& ix, bool dentroDeUI) {
    if (!obj) return;
    for (size_t i = 0; i < obj->Childrens.size(); i++) {
        Object* h = obj->Childrens[i];
        const bool esUI = (h->getType() == ObjectType::ui);
        if ((esUI || !dentroDeUI) && h != me) ix.Ver(h->name);
        IndexarScopeGlobal(h, me, ix, dentroDeUI || esUI);
    }
}
static void IndexarScopeUI(Object* obj, const Object* me, W3dNombreIndice& ix) {
    if (!obj) return;
    for (size_t i = 0; i < obj->Childrens.size(); i++) {
        Object* h = obj->Childrens[i];
        if (h->getType() == ObjectType::ui) continue;   // otro scope (el suyo y el global)
        if (h != me) ix.Ver(h->name);
        IndexarScopeUI(h, me, ix);
    }
}

// nombre libre para 'this' en su scope (NO escribe nada). Con el scope vacio
// (SceneCollection == NULL: el runtime del juego antes de armar la escena)
// devuelve el nombre normalizado tal cual.
//
// PERFORMANCE: UNA sola pasada por el scope (antes: una pasada por CANDIDATO,
// o sea O(N^2) por nombre y O(N^3) para armar la escena - 3000 "add vertex"
// tardaban 19 segundos). Ver W3dNombreIndice en W3dNombres.h.
std::string Object::NombreLibre(const std::string& base) {
    Object* raiz = W3dNombreScopeDe(this);
    if (!raiz) return W3dNombreNormalizar(base, "Objeto");
    W3dNombreIndice ix;
    ix.Empezar(base, "Objeto");
    // la raiz del scope tambien ocupa su nombre (la busqueda por nombre arranca EN ella;
    // es la misma semilla que usa RepararRec)
    if (raiz != this) ix.Ver(raiz->name);
    if (raiz->getType() == ObjectType::ui) IndexarScopeUI(raiz, this, ix);
    else                                   IndexarScopeGlobal(raiz, this, ix, false);
    return ix.Resolver();
}

// LA PUERTA: todo el editor pasa por aca para escribir el nombre de un objeto
// (crear, menu Add, duplicar, importar, pegar, y el campo de texto del panel).
void Object::SetNameObj(const std::string& nombre) {
    if (W3dNombresCargando) { name = nombre; return; }  // carga: crudo + reparacion al final
    name = NombreLibre(nombre);
}

// asignacion CRUDA, sin uniquificar. SOLO para los loaders (el .w3d puede traer
// duplicados de archivos viejos: se reparan de una en W3dNombresRepararEscena,
// que ademas arrastra las refs de scripts) y para el harness de tests, que
// necesita poder FABRICAR duplicados para verificar la reparacion.
void Object::SetNameCrudo(const std::string& nombre) {
    name = nombre;
}

void Object::EliminarObjetosSeleccionados(bool IncluirCollecciones) {
    for (int i = (int)Childrens.size() - 1; i >= 0; i--) {
        Object* child = Childrens[i];
        child->EliminarObjetosSeleccionados(IncluirCollecciones);
        if (child->select && (IncluirCollecciones || child->getType() != ObjectType::collection)) {
            std::cout << "Se borro '" << child->name << "'" << std::endl;
            Childrens.erase(Childrens.begin() + i);
            delete child;
        }
    }
}

// SetName: nombre libre para 'this' SIN escribirlo (lo escribe el caller).
// Se conserva por compatibilidad; por dentro es la MISMA regla que SetNameObj.
std::string Object::SetName(const std::string& baseName) {
    return NombreLibre(baseName);
}

void Object::Seleccionar(){
    ObjActivo = this;
    if (!select){
        select = true;
        ObjSelects.push_back(this);
    }
}

void Object::Deseleccionar(){
    select = false;
    for(size_t o=0; o < ObjSelects.size(); o++){
        if (this == ObjSelects[o]){
            ObjSelects.erase(ObjSelects.begin() + o);
            break;
        }                
    } 
}

void Object::DeseleccionarCompleto(bool IncluirColecciones){
    select = false;
    for(size_t o=0; o < Childrens.size(); o++){
        Childrens[o]->DeseleccionarCompleto(IncluirColecciones);		
    } 
}

bool Object::EstaSeleccionado(bool IncluirCollecciones){
    // Si este objeto está seleccionado y cumple la condición de colecciones → true
    if ( select && ( IncluirCollecciones || getType() != ObjectType::collection ) ) {
        return true;
    }

    //si no estaba seleccionado. mira recursivamente a los hijos
    for(size_t o=0; o < Childrens.size(); o++){   
        if (Childrens[o]->EstaSeleccionado(IncluirCollecciones)) return true;
    }
    return false;       
}

bool Object::SeleccionarCompleto(bool IncluirColecciones){
    //si hay algo seleccionado retorna true para deseleccionar todo
    if (select) return true;

    if (IncluirColecciones || (getType() != ObjectType::collection && getType() != ObjectType::baseObject)){
        select = true;
        ObjSelects.push_back(this);
        if (!ObjActivo) ObjActivo = this;
    }

    for(size_t o=0; o < Childrens.size(); o++){
        if (Childrens[o]->SeleccionarCompleto(IncluirColecciones)) return true;
    }
    //nada seleccionado
    return false;
}

void Object::InvertirSeleccionCompleto(bool IncluirColecciones){
    // flip al mismo conjunto de objetos elegibles que SeleccionarCompleto
    if (IncluirColecciones || (getType() != ObjectType::collection && getType() != ObjectType::baseObject)){
        select = !select;
        if (select){
            ObjSelects.push_back(this);
            ObjActivo = this;
        }
    }
    for(size_t o=0; o < Childrens.size(); o++){
        Childrens[o]->InvertirSeleccionCompleto(IncluirColecciones);
    }
}

        
void Object::Reload(){}
        
void Object::ReloadAll(){
    Reload();

    // Procesar cada hijo
    for (size_t c = 0; c < Childrens.size(); c++) {
        Childrens[c]->ReloadAll();
    }      
}

// El LOCAL de un transform: T * R * S. Esta suelta (no es metodo) porque tambien la necesita el motion trail,
// que arma la matriz de un objeto en OTRO frame sin tocar el objeto. Un solo lugar compone el local -> el trail
// no puede quedar con una convencion distinta a la del render.
Matrix4 W3dLocalTRS(const Vector3& pos, const Quaternion& rot, const Vector3& scale){
    Matrix4 R;
    rot.ToMatrix(R.m);

    Matrix4 S;
    S.Identity();
    S.m[0]  = scale.x;
    S.m[5]  = scale.y;
    S.m[10] = scale.z;

    Matrix4 T;
    T.Identity();
    T.m[12] = pos.x;
    T.m[13] = pos.y;
    T.m[14] = pos.z;

    return T * R * S;
}

// ============================================================================
//  LOS AUXILIARES DE LA EVALUACION DE CONSTRAINTS
// ============================================================================

// ORIENTACION de una matriz de mundo (contrato completo en Objects.h).
bool W3dQuatDeMatriz(const Matrix4& m, Quaternion& out){
    // column-major: las columnas de la 3x3 son los ejes de la base
    Vector3 cx(m.m[0], m.m[1], m.m[2]);
    Vector3 cy(m.m[4], m.m[5], m.m[6]);
    Vector3 cz(m.m[8], m.m[9], m.m[10]);
    const float lx = cx.Length(), ly = cy.Length(), lz = cz.Length();
    // columna ~0 = escala cero en un eje: la base es PLANA y no tiene orientacion que sacar
    if (lx < 1e-6f || ly < 1e-6f || lz < 1e-6f) return false;
    cx = cx * (1.0f / lx);      // normalizar se come la ESCALA, que es justo lo que se quiere
    cy = cy * (1.0f / ly);
    cz = cz * (1.0f / lz);
    // determinante negativo = base ESPEJADA (escala negativa en un numero impar de ejes).
    // Un quaternion solo describe rotaciones PROPIAS: forzarlo devuelve una orientacion que
    // NO es la del padre, y el objeto sale girado sin que nada en pantalla lo explique.
    if (Vector3::Cross(cx, cy).Dot(cz) <= 0.0f) return false;
    Matrix4 base;
    base.Identity();
    base.m[0] = cx.x; base.m[1] = cx.y; base.m[2]  = cx.z;
    base.m[4] = cy.x; base.m[5] = cy.y; base.m[6]  = cy.z;
    base.m[8] = cz.x; base.m[9] = cz.y; base.m[10] = cz.z;
    out = Quaternion::FromMatrix(base);   // ya es ortonormal: FromMatrix lo ASUME
    return true;
}

// el camino CORTO entre dos angulos en grados (ver Objects.h)
float W3dDifAngCorta(float destino, float origen){
    float d = destino - origen;
    // los dos vienen de ToEulerYXZ / atan2f, o sea acotados a +-180: esto da a lo sumo una
    // vuelta. while y no fmodf porque tiene que compilar igual en RVCT (Symbian).
    while (d >   180.0f) d -= 360.0f;
    while (d <= -180.0f) d += 360.0f;
    return d;
}

// AVISOS de una sola vez POR PROCESO. Son condiciones de la escena entera (nadie bindeo una
// vista, un padre espejado) y no de un constraint puntual, asi que no tienen donde guardar su
// bandera. Y tienen que ser una sola vez si o si: el evaluador corre por objeto, POR VISTA y
// POR FRAME, o sea que un log sin bandera son 60 lineas por segundo por objeto.
static bool gAvisoSinVista = false;
static bool gAvisoBaseEspejada = false;

// ORIENTACION en mundo de la fuente de un Copy Rotation. false = la fuente cuelga de una base
// espejada / de escala cero y no hay quaternion que la describa -> el constraint se saltea.
static bool W3dQuatMundoDe(const Object* o, Quaternion& out){
    Matrix4 W;
    o->GetWorldMatrix(W);      // recursivo a proposito: la fuente llega con SUS constraints puestos
    return W3dQuatDeMatriz(W, out);
}

// ===================================================================================================
//  EL OBJETO QUE SE ESTA EDITANDO (Edit Mode). NULL = ninguno.
//  Vive en el Core -y no en el editor- porque el que tiene que saltearse los constraints es el
//  EVALUADOR, y el evaluador tambien corre sin editor (juego, Android, Symbian), donde este puntero
//  se queda en NULL para siempre y no cuesta nada. Lo setea el editor al entrar y salir de Edit Mode
//  (W3dConSetObjEditando, main/objects/ObjectMode.cpp).
// ===================================================================================================
Object* g_consObjEditando = NULL;
void W3dConSetObjEditando(Object* o){ g_consObjEditando = o; }

// un constraint es EFECTIVO si PUEDE cambiar algo. Lo usan el pre-pase (para poder devolver la
// base sin hacer una sola cuenta) y tambien el bucle, para que el criterio viva en UN solo lugar
// y no se vayan separando con el tiempo.
// 'dueno' es el objeto al que pertenece: hace falta para el "ver en modo edicion", que depende de
// SI ES EL QUE SE ESTA EDITANDO y no del constraint solo.
static bool W3dConEfectivo(const W3dConstraint* c, const Object* dueno){
    // VER EN MODO EDICION apagado: mientras se editan los vertices de ESTE objeto, el constraint no
    // se aplica y la malla se dibuja y se edita donde dicen sus campos reales (ver W3dConstraint.h).
    // Solo el objeto en edicion: apagarselo tambien a los padres moveria a sus otros hijos.
    if (!c->mostrarEdit && dueno && dueno == g_consObjEditando) return false;
    if (!c->activo) return false;
    if (c->Peso() <= 0.0f) return false;
    if (c->tipo == W3dConstraintTipo::Billboard) return (c->bbYaw || c->bbPitch);
    // Copy Location / Copy Rotation: sin ejes no copia nada, y sin fuente tampoco
    if (!c->ejeX && !c->ejeY && !c->ejeZ) return false;
    return (c->fuenteTipo == W3dConstraintFuente::Vista || c->fuenteObj != NULL);
}

// la version publica del predicado (ver Objects.h): el editor pregunta por aca en vez de mirar
// constraints.empty(), que dice "tiene" y no "hace algo".
bool W3dObjTieneConstraintEfectivo(const Object* o){
    if (!o) return false;
    for (size_t i = 0; i < o->constraints.size(); i++)
        if (o->constraints[i] && W3dConEfectivo(o->constraints[i], o)) return true;
    return false;
}

// contador de DIAGNOSTICO: cuantas veces corrio el CUERPO del evaluador (o sea, las que pasaron
// el pre-pase y realmente hicieron cuentas). No lo lee nadie del motor; existe para que un test
// pueda afirmar que una consulta sobre una cadena de N objetos con constraint cuesta O(N) y no
// O(2^N), que es lo que costaba cuando cada eslabon volvia a pedir el mundo de su padre.
unsigned long g_consEvalCorridas = 0;

// ============================================================================
//  W3dEvaluarConstraints: EL CORAZON (contrato completo en Objects.h)
//
//  Todo el trabajo se hace en MUNDO, porque ahi es donde estan la fuente y la camara:
//  se sube la transform del objeto al espacio de mundo, se corre el stack, y se baja de
//  vuelta al espacio del padre. Sin padre no hay ni subida ni bajada.
//
//  'WpIn' es la matriz de MUNDO DEL PADRE ya calculada, o NULL si el que llama no la tiene.
//  Es lo unico que separa esta evaluacion de una EXPONENCIAL: ver el comentario grande de
//  Object::GetWorldMatrix.
// ============================================================================
static bool W3dEvalCons(const Object* o, const Matrix4* WpIn, Vector3& posOut, Quaternion& rotOut){
    if (!o) return false;

    // ---- 1. GUARDIA DE CICLOS ----
    // segunda entrada al MISMO objeto (A copia de B y B copia de A): devolver la base corta
    // la recursion en O(1), sin allocar y sin importar cuantos objetos haya en el ciclo. Es
    // determinista: el que se pregunta primero es el que termina con su propia base.
    if (o->consEval) return false;

    // ---- 2. EL PRE-PASE BARATO ----
    // si NO hay ningun constraint efectivo se sale ACA, antes de tocar la matriz del padre.
    // Esto es lo que hace que "apagado" o "influencia 0" devuelva la transform base BIT A BIT
    // incluso con padre: sin el pre-pase habria una ida mundo -> InvertirAfin -> local que
    // ensucia el ultimo decimal, y apagar un constraint moveria el objeto un epsilon.
    bool hayAlguno = false;
    for (size_t i = 0; i < o->constraints.size(); i++)
        if (W3dConEfectivo(o->constraints[i], o)) { hayAlguno = true; break; }
    if (!hayAlguno) return false;

    // ---- 3. de aca en adelante hay UN SOLO punto de salida (el consEval = false del final) ----
    o->consEval = true;
    g_consEvalCorridas++;        // solo diagnostico (ver arriba)

    // ---- 4. el espacio del PADRE ----
    const bool tienePadre = (o->Parent != NULL);
    Matrix4 Wp;
    Quaternion qWp;              // identidad por su propio ctor
    bool baseOk = true;          // false = el padre esta espejado: no hay rotacion que componer
    if (tienePadre) {
        // si el que llama viene BAJANDO la cadena ya lo tiene calculado y lo pasa: eso es lo que
        // hace que la cuenta sea lineal. El que entra suelto (GetMatrix de un objeto cualquiera)
        // lo pide una sola vez, que ya es lo mismo que hacia antes.
        if (WpIn) Wp = *WpIn;
        else      o->Parent->GetWorldMatrix(Wp);   // recursivo: el padre llega con SUS constraints
        baseOk = W3dQuatDeMatriz(Wp, qWp);
        if (!baseOk && !gAvisoBaseEspejada) {
            gAvisoBaseEspejada = true;
            w3dLogfW("constraints: '%s' cuelga de una base espejada o de escala cero; sus constraints de ROTACION se saltean (la posicion se aplica igual)",
                     o->name.c_str());
        }
    }

    // ---- 5. la transform del objeto, llevada a MUNDO ----
    Vector3    pw = tienePadre ? (Wp * o->pos)      : o->pos;
    Quaternion qw = tienePadre ? (qWp * o->Rot())   : o->Rot();

    // ---- 6. EL STACK, en orden (como los modificadores de la malla) ----
    for (size_t i = 0; i < o->constraints.size(); i++) {
        W3dConstraint* c = o->constraints[i];
        if (!W3dConEfectivo(c, o)) continue;
        const float t = c->Peso();

        // el que necesita la vista y no la tiene usa el default (mirando al -Z) y avisa: "no
        // hay camara" no es lo mismo que "la camara mira para alla", y sin el aviso el juego
        // que se olvido de bindear ve el billboard clavado y no tiene por donde empezar.
        if (!g_vistaBindeada && !gAvisoSinVista &&
            (c->tipo == W3dConstraintTipo::Billboard || c->fuenteTipo == W3dConstraintFuente::Vista)) {
            gAvisoSinVista = true;
            w3dLogfW("constraints: nadie llamo a W3dVistaBind todavia; '%s' usa la vista por defecto (mirando al -Z)",
                     o->name.c_str());
        }

        // la fuente esta a mitad de SU propia evaluacion, o sea que se llego hasta aca desde
        // ella: hay un ciclo. La guardia de arriba ya hace que esto TERMINE, pero hay que
        // decirlo, y una sola vez por constraint: esto corre por vista y por frame. El flag lo
        // resetea el panel al elegir otra fuente (AccionConFuenteElegida, ui/ViewPorts/
        // Properties.cpp), asi que romper el ciclo y volver a armarlo vuelve a avisar.
        if (c->fuenteObj && c->fuenteObj->consEval && !c->avisoCiclo) {
            c->avisoCiclo = true;
            w3dLogfW("constraints: ciclo entre '%s' y '%s' (constraint '%s'); se usa la transform base del otro",
                     o->name.c_str(), c->fuenteObj->name.c_str(), c->nombre.c_str());
        }

        if (c->tipo == W3dConstraintTipo::CopyLocation) {
            // ---- COPY LOCATION: LERP en MUNDO ----
            // lerp y no otra cosa: la posicion vive en un espacio AFIN y ahi el camino recto
            // ES el correcto (al reves que la rotacion, que va por el arco).
            const Vector3 fw = (c->fuenteTipo == W3dConstraintFuente::Vista)
                             ? g_renderCamPos : c->fuenteObj->GetGlobalPosition();
            Vector3 obj = pw;
            if (c->ejeX) obj.x = fw.x;
            if (c->ejeY) obj.y = fw.y;
            if (c->ejeZ) obj.z = fw.z;
            pw = (t >= 1.0f) ? obj : (pw + (obj - pw) * t);   // con t=1 ASIGNA, no interpola
        }
        else if (c->tipo == W3dConstraintTipo::CopyRotation) {
            if (!baseOk) continue;                      // padre espejado: ya se aviso arriba
            Quaternion qf;
            if (c->fuenteTipo == W3dConstraintFuente::Vista) qf = g_renderCamRot;
            else if (!W3dQuatMundoDe(c->fuenteObj, qf))  continue;   // la fuente esta espejada
            Quaternion qObj;
            if (c->ejeX && c->ejeY && c->ejeZ) {
                qObj = qf;   // ATAJO de los 3 ejes: es "la misma orientacion", sin ida y vuelta por euler
            } else {
                // la MASCARA por eje solo existe en euler: "copiar la X" ES un angulo.
                Vector3 e = qw.ToEulerXYZ(), ef = qf.ToEulerXYZ();
                if (c->ejeX) e.x = ef.x;
                if (c->ejeY) e.y = ef.y;
                if (c->ejeZ) e.z = ef.z;
                qObj = Quaternion::FromEulerXYZ(e.x, e.y, e.z);
            }
            // la INFLUENCIA se mezcla en QUATERNION: arco corto y extremos exactos.
            // Semantica que hay que conocer y que es la correcta: "350 grados al 50%" da 355 y
            // no 175, porque un quaternion codifica una ORIENTACION, no vueltas.
            qw = Quaternion::Slerp(qw, qObj, t);
        }
        else if (c->tipo == W3dConstraintTipo::Billboard) {
            if (!baseOk) continue;                      // padre espejado: ya se aviso arriba
            // el billboard se alinea con el PLANO de la camara (NO apunta a su posicion): es
            // lo que hacia el objeto Constraint viejo, o sea el look que ya tiene la demo.
            const Vector3 f = g_renderCamForward;       // ya viene en MUNDO
            float yawObj = 0.0f, pitchObj = 0.0f;
            if (c->bbYaw) {
                if (fabsf(f.x) < 1e-5f && fabsf(f.z) < 1e-5f) {
                    // camara a plomo (mirando derecho para abajo o para arriba): el yaw es
                    // INDETERMINADO. El codigo viejo normalizaba el (0,0,0) y se comia un
                    // atan2f(0,0) = 0, o sea que el objeto pegaba un salto a mirar al +Z.
                    yawObj = qw.ToEulerYXZ().y;         // se queda como estaba
                } else {
                    yawObj = atan2f(f.x, f.z) * (180.0f / M_PI);
                }
            }
            if (c->bbPitch) {
                // CLAMP, que el codigo viejo NO tenia. 'forward' sale de rotar un unitario por
                // un quaternion, asi que un f.y = 1.0000001 es de lo mas normal, y
                // asinf(1.0000001) es NaN: la matriz entera queda en NaN y el objeto
                // DESAPARECE de la pantalla sin un solo error.
                float fy = f.y;
                if (fy >  1.0f) fy =  1.0f;
                if (fy < -1.0f) fy = -1.0f;
                pitchObj = asinf(fy) * (180.0f / M_PI);
            }
            if (t >= 1.0f) {
                // IDENTICO al objeto Constraint viejo (dado de baja): mismo orden YXZ,
                // mismo signo del pitch y roll en 0. Por eso la demo se sigue viendo igual.
                qw = Quaternion::FromEulerYXZ(-pitchObj, yawObj, 0.0f);
            } else {
                // con influencia intermedia se mezclan los ANGULOS y no el quaternion: el
                // objetivo del billboard SE CONSTRUYE de dos angulos, asi que mezclar ahi es lo
                // que da "50% = la mitad de la inclinacion" y deja el roll en 0 exacto.
                // Por la diferencia CORTA, si no el objeto se va por el camino largo.
                // CONDUCTA CONOCIDA Y ACEPTADA (ver W3dConstraint.h): cuando esa diferencia
                // cruza los 180 grados el destino se da vuelta y el objeto pega un giro.
                const Vector3 e = qw.ToEulerYXZ();      // (pitch, yaw, roll)
                const float p = e.x + W3dDifAngCorta(-pitchObj, e.x) * t;
                const float y = e.y + W3dDifAngCorta( yawObj,   e.y) * t;
                const float r = e.z + W3dDifAngCorta( 0.0f,     e.z) * t;
                qw = Quaternion::FromEulerYXZ(p, y, r);
            }
        }
    }

    // ---- 7. de vuelta al espacio del PADRE ----
    if (tienePadre) {
        Matrix4 iWp;
        // si Wp fuera singular InvertirAfin deja iWp = Wp: una matriz usable en vez de basura
        // (y con escala cero no hay local que devuelva esa posicion de mundo, de todas formas).
        Matrix4::InvertirAfin(Wp, iWp);
        posOut = iWp * pw;
        rotOut = qWp.Inverted() * qw;   // qWp sale de W3dQuatDeMatriz: unitario -> inversa = conjugado
    } else {
        posOut = pw;
        rotOut = qw;
    }

    o->consEval = false;
    return true;
}

// la puerta publica (contrato en Objects.h): el que pregunta suelto no tiene el mundo del padre.
bool W3dEvaluarConstraints(const Object* o, Vector3& posOut, Quaternion& rotOut){
    return W3dEvalCons(o, NULL, posOut, rotOut);
}

// matriz LOCAL efectiva de 'o' con el mundo del padre YA calculado ('Wp' = NULL si es top-level
// o si el que llama no lo tiene). Es GetMatrix() con ese dato de mas: los dos early-outs son
// IDENTICOS a los de GetMatrix a proposito, para que "objeto sin constraints" pese lo mismo se
// entre por donde se entre.
static void W3dLocalEfectiva(const Object* o, const Matrix4* Wp, Matrix4& out){
    if (o->constraints.empty()) { o->GetMatrixBase(out); return; }
    Vector3 p;
    Quaternion q;
    if (!W3dEvalCons(o, Wp, p, q)) { o->GetMatrixBase(out); return; }
    out = W3dLocalTRS(p, q, o->scale);   // la ESCALA nunca la toca un constraint
}

// ============================================================================
//  LA TRANSFORM **BASE**: T*R*S de los campos, sin mirar los constraints.
//  Es la que va a los vertices que se hornean y a lo que se escribe a disco (ver el
//  bloque grande de Objects.h). No tiene early-out ni caso especial: es UNA linea a
//  proposito, para que sea imposible que "la base" signifique cosas distintas en dos
//  lugares del editor.
// ============================================================================
void Object::GetMatrixBase(Matrix4& out) const {
    out = W3dLocalTRS(pos, rot, scale);
}

// ============================================================================
//  LA TRANSFORM **EFECTIVA**: la base con el stack de constraints encima.
//  Es la fuente UNICA de lo que se ve: el render, el pick, el gizmo, el foco y los
//  iconos entran todos por aca, asi que un billboard se DIBUJA, se CLICKEA y se
//  ENFOCA en el mismo lugar sin que ninguno de esos 29 call sites sepa que existen
//  los constraints.
//  Los DOS early-outs (lista vacia / el evaluador dice que no aplico nada) devuelven
//  exactamente la misma expresion que GetMatrixBase: el 99% de los objetos no paga ni
//  una cuenta, y "constraint apagado" da la base BIT A BIT (ver W3dEvaluarConstraints).
// ============================================================================
void Object::GetMatrix(Matrix4& out) const {
    // NULL = "no te traigo el mundo del padre": el que entra por aca no lo tiene, asi que si hace
    // falta el evaluador lo pide una vez. El que SI lo tiene (GetWorldMatrix, bajando la cadena)
    // llama derecho a W3dLocalEfectiva y por eso no paga la cuenta exponencial.
    W3dLocalEfectiva(this, NULL, out);
}

void Object::RotateLocal(float pitch, float yaw, float roll){
    Quaternion qPitch = Quaternion::FromAxisAngle(1,0,0, pitch);
    Quaternion qYaw   = Quaternion::FromAxisAngle(0,1,0, yaw);
    Quaternion qRoll  = Quaternion::FromAxisAngle(0,0,1, roll);

    // por la puerta: rotar tiene que dejar el euler/axis-angle al dia o lo que se guarda no es lo que se ve
    SetRot(rot * qYaw * qPitch * qRoll);
}

// matriz de MUNDO del objeto: su matriz LOCAL (T*R*S) por toda la cadena de
// padres. Fuente UNICA de la transform de mundo (foco, LocalAMundo, color-pick).
//
//  *** POR QUE ESTO BAJA Y NO SUBE ***
//  La version obvia (subir con un while(Parent) pidiendo GetMatrix de cada eslabon) es
//  EXPONENCIAL desde que existen los constraints: GetMatrix de un objeto con constraint
//  necesita el MUNDO de su padre, o sea que vuelve a recorrer toda la cadena de arriba, y
//  ese mundo vuelve a pedir GetMatrix de cada uno... M(n) = 1 + W(n-1) y W(n) = suma de
//  M(0..n) da M(n) = 2^n. Diez padres con constraint = 1024 corridas del evaluador para UNA
//  consulta, por vista y por frame: en el N95 eso no existe.
//  Bajando, cada eslabon recibe el mundo de su padre YA calculado y lo unico que hace es
//  su propia local: una corrida del evaluador por ancestro, o sea LINEAL.
//  NO hay cache y no la va a haber: el cacheo es justo el estado que este disenio elimina
//  (la matriz efectiva depende de la vista bindeada, guardarla es publicar la respuesta de
//  un viewport para que la lea otro). Lo que se elimina aca es trabajo REPETIDO, no estado.
void Object::GetWorldMatrix(Matrix4& out) const {
    if (!Parent) { GetMatrix(out); return; }   // top-level: su local YA es su mundo
    Matrix4 Wp;
    Parent->GetWorldMatrix(Wp);     // recursion de profundidad = la cadena, una vez cada uno
    Matrix4 L;
    W3dLocalEfectiva(this, &Wp, L); // ...y aca se le PASA, en vez de que lo vuelva a pedir
    out = Wp * L;
}

// la MISMA cadena pero con la BASE de cada eslabon. No alcanza con usar la base del
// propio objeto: si el PADRE tiene un billboard, la matriz de mundo que se hornea
// tambien se llevaria puesta la camara. La cadena entera tiene que ser cruda.
void Object::GetWorldMatrixBase(Matrix4& out) const {
    GetMatrixBase(out);
    const Object* q = Parent;
    while (q) {
        Matrix4 P;
        q->GetMatrixBase(P);
        out = P * out;
        q = q->Parent;
    }
}

// obtener posición global: traslacion de la matriz de mundo
// (antes solo sumaba traslaciones y con un padre rotado/escalado quedaba mal)
Vector3 Object::GetGlobalPosition() const {
    Matrix4 M;
    GetWorldMatrix(M);
    return Vector3(M.m[12], M.m[13], M.m[14]);
}

// la misma traslacion pero de la cadena BASE (ver Objects.h). Sin constraints devuelve
// EXACTAMENTE lo mismo que GetGlobalPosition: es la misma cuenta.
Vector3 Object::GetGlobalPositionBase() const {
    Matrix4 M;
    GetWorldMatrixBase(M);
    return Vector3(M.m[12], M.m[13], M.m[14]);
}

// transforma un punto LOCAL a MUNDO usando la matriz de mundo completa
// (Matrix4::operator*(Vector3) es exactamente mundo = M * (local, 1), con M column-major)
Vector3 Object::LocalAMundo(const Vector3& local) const {
    Matrix4 M;
    GetWorldMatrix(M);
    return M * local;
}

// idem con la matriz de mundo BASE: es la que usa el que EXPORTA vertices (ver Objects.h)
Vector3 Object::LocalAMundoBase(const Vector3& local) const {
    Matrix4 M;
    GetWorldMatrixBase(M);
    return M * local;
}

Matrix4 Object::BuildMatrix(const Vector3& pos, const Quaternion& rot, const Vector3& scale) {
    // misma composicion T*R*S que W3dLocalTRS: UNA sola fuente de verdad (esta version escalaba
    // columnas a mano; si divergian, el motion trail y el objeto dibujado no coincidian)
    return W3dLocalTRS(pos, rot, scale);
}

void Object::RenderObject(){}

// Funcion recursiva para renderizar un objeto y sus hijos
void Object::Render(){
    if (!visible) return;
    // LOCAL VIEW: si un viewport esta aislando, solo se traversa/dibuja lo que esta en el set (seleccion +
    // descendencia + cadena de padres para poder ALCANZAR a los seleccionados). Fuera del editor: false -> gratis.
    if (g_localViewActivo && g_localViewVisibles && !g_localViewVisibles->count(this)) return;
    // Guardar la matriz actual (por la abstraccion: glPushMatrix en desktop, stack propio en ES2/WebGL)
    w3dEngine::PushMatrix();

    // matriz LOCAL (no el miembro Object::M): la matriz efectiva puede depender de la vista que se
    // esta dibujando, asi que dejarla guardada en el objeto seria publicar la respuesta de UN
    // viewport para que la lea cualquier otro. Ver el comentario de 'M' en Objects.h.
    Matrix4 local;
    GetMatrix(local);
    w3dEngine::MultMatrix(local.m);   // aplica T * R * S -> incluye traslacion

    // ---- ESCALA NEGATIVA = ESPEJO: hay que dar vuelta el WINDING ----
    // Una escala con un numero IMPAR de componentes negativas invierte el sentido
    // de las caras: con backface culling prendido el objeto se ve DEL REVES (se
    // dibuja su interior). Lo usan los clips espejados -- una frenada hacia el otro
    // lado es el MISMO modelo con scale.x = -1 -- y varios efectos.
    // El Mirror y la Instance ya lo hacian para su caso; un objeto comun con
    // setEscala(o, -1, 1, 1) no, y por eso se veia dado vuelta.
    // El winding es estado GLOBAL de GL (no hay pila), asi que se lleva la cuenta de
    // cuantos espejos hay ABIERTOS: anidar dos vuelve al winding normal, que es lo
    // que corresponde (dos reflexiones son una rotacion).
    static int gEspejosAbiertos = 0;
    const bool espeja = (scale.x * scale.y * scale.z) < 0.0f;
    if (espeja) { gEspejosAbiertos++; w3dEngine::FrontFace((gEspejosAbiertos & 1) == 0); }

    // Si es visible y no es un mesh, lo dibuja
    RenderObject();

    // Procesar cada hijo (virtual: LOD/Culling deciden cuales y en que orden)
    RenderHijos();

    if (espeja) { gEspejosAbiertos--; w3dEngine::FrontFace((gEspejosAbiertos & 1) == 0); }

    // Restaurar la matriz previa
    w3dEngine::PopMatrix();
}

// default del traversal: TODOS los hijos, en su orden del arbol
void Object::RenderHijos(){
    for (size_t c = 0; c < Childrens.size(); c++) {
        Childrens[c]->Render();
    }
}

// nombre LEGIBLE del tipo (ver Objects.h). Un switch a proposito (sin tabla indexada por el enum):
// si manana se agrega un ObjectType, el compilador avisa del caso faltante en vez de devolver basura.
const char* W3dNombreTipo(ObjectType t){
    switch (t.v){
        case ObjectType::scene:      return "escena";
        case ObjectType::mesh:       return "mesh";
        case ObjectType::camera:     return "camara";
        case ObjectType::light:      return "luz";
        case ObjectType::empty:      return "vacio";
        case ObjectType::armature:   return "esqueleto";
        case ObjectType::curve:      return "curva";
        case ObjectType::collection: return "coleccion";
        case ObjectType::baseObject: return "objeto";
        case ObjectType::mirror:     return "espejo";
        case ObjectType::script:     return "script";
        case ObjectType::instance:   return "instancia";
        case ObjectType::ui:         return "ui";
        case ObjectType::texto2d:    return "texto";
        case ObjectType::imagen2d:   return "imagen";
        case ObjectType::rect2d:     return "rect";
        case ObjectType::cont2d:     return "contenedor";
        case ObjectType::slice9:     return "slice9";
        case ObjectType::boton2d:    return "boton";
        case ObjectType::expandir2d: return "expandir";
        case ObjectType::video2d:    return "video";
        case ObjectType::lod:        return "lod";
        case ObjectType::culling:    return "culling";
        case ObjectType::particulas: return "particulas";
    }
    return "objeto";
}

Object* FindObjectByName(Object* node, const std::string& name){
    if (!node) return NULL;

    // Coincide con este nodo → devolverlo
    if (node->name == name)
        return node;

    // Buscar en hijos
    for (size_t i = 0; i < node->Childrens.size(); i++){
        Object* r = FindObjectByName(node->Childrens[i], name);
        if (r) return r; // si lo encontró en un hijo, devolverlo
    }

    return NULL; // no encontrado
}

// Armature es core-bound y Symbian-safe (solo incluye Objects.h/Matrix4.h): fuera del ifndef para que
// las ramas de Pose Mode (A/None/Invert -> a->bones) compilen tambien en Symbian.
#include "Armature.h" // Pose Mode: seleccionar/deseleccionar huesos
#ifndef W3D_SYMBIAN
// La clase base NO depende de los subtipos del editor: esos includes (Target, Scene,
// Collection, Instance, Mirror, ObjetoScript, Curve, Camera) estaban MUERTOS. Solo usa
// Light (ApagarLucesHijas) + el core-bound Mesh.
#include "Light.h"
#include "Mesh.h"
#endif

bool DetectLoop(Object* node,
                std::set<Object*>& visited,
                std::set<Object*>& stack,
                int depth){
    if (!node) return false;

    // Si el nodo ya está en recursion → BUCLE encontrado
    if (stack.count(node)) {
        std::cout << "⚠ LOOP DETECTADO en nodo: " << node->name << std::endl;
        return true;
    }

    // Si ya lo visitamos pero no está en la pila actual → no hay loop aquí
    if (visited.count(node)) return false;

    visited.insert(node);
    stack.insert(node);

    // revisar hijos
    for (size_t i = 0; i < node->Childrens.size(); i++)
        if (DetectLoop(node->Childrens[i], visited, stack, depth + 1))
            return true;

    stack.erase(node);
    return false;
}

// ROTACION GLOBAL: la acumulada por la cadena de padres (ver Objects.h). Vino de
// main/objects/ObjectMode.cpp cuando el runtime de un juego -que no linkea el modo
// objeto del editor- empezo a necesitarla por el importador.
Quaternion RotGlobalDe(Object* o) {
    if (!o) return Quaternion(1, 0, 0, 0);
    if (!o->Parent) return o->Rot();
    return RotGlobalDe(o->Parent) * o->Rot();
}

void SearchLoop(){
    std::set<Object*> visited;
    std::set<Object*> stack;

    w3dLogf("Analizando arbol para loops...");   // log PROPIO, nunca stdout (abria la consola blanca)

    if (DetectLoop(SceneCollection, visited, stack)) {
        w3dLogfW("RESULTADO: hay bucles en la jerarquia!");
    } else {
        w3dLogf("No se detectaron loops. Arbol sano.");
    }
}

// Devuelve el último nodo DFS (más profundo) a partir de 'node'
Object* GetDeepestDFS(Object* node){
    if (!node) return NULL;

    while(!node->Childrens.empty()){
        node = node->Childrens.back();
    }
    return node;
}

// EL PADRE TAL COMO LO VE EL OUTLINER. Un objeto creado desde el menu Add nace con
// Parent = NULL y se cuelga igual de SceneCollection->Childrens (ver el ctor de Object):
// el arbol queda con la arista en UN solo sentido. GetPrevDFS/GetNextDFS leian ->Parent
// crudo, no encontraban al objeto, escupian "[ERROR] Root inesperado" y devolvian NULL
// ==> desde CUALQUIER objeto recien creado la flecha ARRIBA no se movia. Esta es la
// lectura correcta del vinculo: si no declara padre pero no es la raiz, su padre es la
// raiz. (Arreglar el ctor tocaria la vida de todos los objetos; esto arregla la
// NAVEGACION, que es lo que estaba roto.)
static Object* PadreEnArbol(Object* o) {
    if (!o || o == SceneCollection) return NULL;
    return o->Parent ? o->Parent : SceneCollection;
}

Object* GetPrevDFS(Object* current){
    if (!current) return NULL;

    Object* parent = PadreEnArbol(current);

    // --- Caso especial: current es el primer hijo del root ---
    if (parent == SceneCollection){
        std::vector<Object*>& siblings = parent->Childrens;
        if (!siblings.empty() && current == siblings.front()){
            // Volver al último nodo DFS del árbol completo
            return GetDeepestDFS(siblings.back());
        }
    }

    // --- Si existe hermano anterior ---
    if (parent){
        std::vector<Object*>& siblings = parent->Childrens;
        std::vector<Object*>::iterator it = std::find(siblings.begin(), siblings.end(), current);

        if (it == siblings.end()){
            std::cout << "GetPrevDFS: inconsistencia, parent no contiene a current\n";
            return NULL;
        }

        if (it != siblings.begin()){
            --it; // hermano anterior
            return GetDeepestDFS(*it);
        }

        // Si no hay hermano anterior, el anterior es el padre
        return parent;
    }

    // Si es root real => no hay anterior
    if (current == SceneCollection)
        return NULL;

    std::cout << "[ERROR] Root inesperado\n";
    return NULL;
}

Object* GetNextDFS(Object* current) {
    if (!current) {
        //std::cout << "GetNextDFS: current fue nulo!\n";
        return NULL;
    }

    // 1) Si tiene hijos -> primero hijo
    if (!current->Childrens.empty()) {
        //std::cout << "Tenia hijos, se selecciono su hijo\n";
        return current->Childrens[0];
    }

    // 2) Si no tiene hijos -> subimos buscando el siguiente hermano de algún ancestro
    Object* node = current;

    while (node) {
        Object* parent = PadreEnArbol(node);

        // --------------------------------------------------------
        // Caso especial: node es la raíz (SceneCollection)
        // Antes buscábamos el "siguiente root" en Objects[], ahora no existe.
        // Por lo tanto, no hay siguiente.
        // --------------------------------------------------------
        if (!parent) {
            if (node == SceneCollection) {
                return NULL;  // fin total del DFS
            } else {
                std::cout << "[ERROR] Root inesperado diferente a SceneCollection " << parent << "\n";
                std::cout << "Objeto: " << node->name << "\n";
                return NULL;
            }
        }

        // Buscar node entre los hijos del padre
        std::vector<Object*>& siblings = parent->Childrens;
        std::vector<Object*>::iterator it = std::find(siblings.begin(), siblings.end(), node);

        if (it == siblings.end()) {
            std::cout << "GetNextDFS: inconsistencia, parent no contiene al hijo\n";
            return NULL;
        }

        // Intentar el siguiente hermano
        ++it;
        if (it != siblings.end()) {
            return *it;  // siguiente hermano → siguiente en DFS
        }

        // No hay más hermanos → seguir subiendo
        node = parent;
    }

    return NULL;
}

bool IsSelectable(Object* obj, bool IncluirColecciones) {
    if (!obj) return false;

    // La RAIZ nunca: no es una fila del outliner y no se selecciona a mano. Con
    // IncluirColecciones=true pasaba el filtro (reporta tipo collection) y GetPrevDFS
    // la devuelve al subir desde el primer hijo -> la flecha "arriba" se clavaba ahi.
    if (obj == SceneCollection) return false;

    const ObjectType t = obj->getType();

    // Si NO queremos incluir colecciones → bloquearlas
    if (!IncluirColecciones) {
        if (t == ObjectType::collection)
            return false;
    }

    // Siempre bloquear baseObject
    if (t == ObjectType::baseObject)
        return false;

    // NO NAVEGAR A FILAS QUE NO EXISTEN EN PANTALLA: el outliner solo dibuja las
    // filas cuyos ancestros estan TODOS desplegados, pero el recorrido usaba el DFS
    // del arbol completo. Con 315 objetos, un Culling de 52 hijos y una Collection
    // de 91, el highlight se iba adentro de ramas plegadas y parecia que la flecha
    // "no hacia nada". Un objeto adentro de una rama plegada no es navegable.
    for (Object* p = obj->Parent; p; p = p->Parent)
        if (!p->desplegado) return false;
    // (la raiz no entra: su 'desplegado' no se dibuja ni se puede togglear)

    return true;
}

Object* GetFirstDFS(){
    if (!SceneCollection) return NULL;
    if (SceneCollection->Childrens.empty()) return NULL;
    return SceneCollection->Childrens[0];
}

void changeSelect(SelectMode mode, bool IncluirColecciones){
    if (InteractionMode != ObjectMode) return;
    if (estado != editNavegacion) return;
    if (SceneCollection->Childrens.empty()) return;

    //std::cout << "changeSelect " << ObjActivo << std::endl;
    //std::cout << "Seleccionados: " << ObjSelects.size() << std::endl;
    //std::cout << "IncluirColecciones: " << IncluirColecciones << std::endl;

    // Si no hay activo → elegir el primero DFS
    if (!ObjActivo){
        if (SceneCollection->Childrens.empty()){
            std::cout << "no hay objetos para seleccionar" << std::endl;
            return;
        }
        Object* it = SceneCollection->Childrens[0];

        while(it && !IsSelectable(it, IncluirColecciones))
            it = GetNextDFS(it);

        if (it){
            it->Seleccionar();
        }
        else {
            std::cout << "los objetos no eran seleccionables" << std::endl;
        }
        return;
    }

    Object* next = NULL;

    // elegir next o previous según modo
    if (mode == SelectMode::NextSingle || mode == SelectMode::NextAdd){
        next = GetNextDFS(ObjActivo);
    }
    else if (mode == SelectMode::PrevSingle || mode == SelectMode::PrevAdd){
        next = GetPrevDFS(ObjActivo);
    }

    const bool haciaAtras = (mode == SelectMode::PrevSingle || mode == SelectMode::PrevAdd);

    if (!next) {
        // Llegamos a la punta del DFS → wrap. Hacia ADELANTE se vuelve al primero;
        // hacia ATRAS se va al ULTIMO (el nodo mas profundo de la ultima rama). Antes
        // el wrap era SIEMPRE GetFirstDFS(): con la flecha arriba en el tope no se movia.
        next = haciaAtras ? GetDeepestDFS(SceneCollection->Childrens.back()) : GetFirstDFS();
        if (!next) return;
    }

    // Buscar un selectable EN LA MISMA DIRECCION en la que veniamos. El ternario viejo
    // solo contemplaba PrevAdd: con PrevSingle -que es el que dispara la flecha ARRIBA-
    // el salteo caminaba para ADELANTE y la flecha terminaba yendo al reves.
    // El tope de vueltas es la red de seguridad: GetPrevDFS CICLA (del primer hijo de la
    // raiz vuelve al ultimo), asi que si no hubiera ni un objeto navegable -todo plegado-
    // este while no terminaria nunca.
    Object* it = next;
    int vueltas = 0;
    const int tope = 100000;
    while(it && !IsSelectable(it, IncluirColecciones)){
        if (++vueltas > tope) return;
        it = haciaAtras ? GetPrevDFS(it) : GetNextDFS(it);
        if (it == next) return;   // dio la vuelta entera sin encontrar nada
    }

    if (!it) return;

    // ------------------------
    // Aplicar modo de selección
    // ------------------------
    if (mode == SelectMode::NextSingle || mode == SelectMode::PrevSingle){
        ObjActivo->Deseleccionar();
    }

    it->Seleccionar();
}

void ApagarLucesHijas(Object* obj){
#ifndef W3D_SYMBIAN
    // (Light.h todavia no esta portado al dialecto C++03; cuando lo este,
    // esta guarda vuela y Symbian apaga luces igual que PC)
    //si es una luz. la apaga
    if (obj->getType() == ObjectType::light) {
        Light* luz = dynamic_cast<Light*>(obj);
        if (luz) {
            w3dEngine::SetLightEnabled(luz->LightID, false);
        }
    }
#endif
    //lo mismo con los hijos
    for(size_t o=0; o < obj->Childrens.size(); o++){
        ApagarLucesHijas(obj->Childrens[o]);
    }
}

void SetDesplegado(bool desplegado){
    if (SceneCollection && ObjActivo){
        ObjActivo->desplegado = desplegado;
    }
}

void ChangeVisibilityObj(){
    if (InteractionMode == ObjectMode && estado == editNavegacion && SceneCollection && ObjActivo){
        ObjActivo->visible = !ObjActivo->visible;
        //apagar luces en caso de que era una luz o sus hijas eran luces
        if (!ObjActivo->visible && w3dRenderLuces) ApagarLucesHijas(ObjActivo);
    }
}

// visibilidad de RENDER del objeto activo (distinta de 'visible', que oculta en el EDITOR): tecla '3' del
// outliner en el N95. Mismo gate que ChangeVisibilityObj (object mode, navegando).
void ChangeRenderizableObj(){
    if (InteractionMode == ObjectMode && estado == editNavegacion && SceneCollection && ObjActivo){
        ObjActivo->renderizable = !ObjActivo->renderizable;
    }
}

// FLECHA IZQUIERDA en el outliner: si el activo esta desplegado y tiene hijos, lo pliega; si YA esta plegado
// (o es hoja) y tiene padre, pliega al PADRE y lo selecciona (sube un nivel). Antes solo hacia desplegado=false,
// asi que re-apretar izquierda sobre un hijo ya plegado no hacia nada.
void OutlinerColapsarIzquierda(){
    if (!(SceneCollection && ObjActivo)) return;
    if (ObjActivo->desplegado && !ObjActivo->Childrens.empty()){
        ObjActivo->desplegado = false;
        return;
    }
    Object* p = ObjActivo->Parent;
    if (p && p != SceneCollection){
        p->desplegado = false;
        DeseleccionarTodo();
        p->Seleccionar();
    }
}

void DeseleccionarTodo(bool IncluirColecciones){
	// NO cambiar la seleccion durante un transform: estadoObjetos apunta a los
	// seleccionados y si cambia mientras se transforma, crashea.
	if (estado != editNavegacion) return;
	if (InteractionMode == EditMode && g_editMesh){
        g_editMesh->EditSeleccionarTodo(false); // edit: deseleccionar todos los sub-elementos
	} else if (InteractionMode == PoseMode && ObjActivo && ObjActivo->getType() == ObjectType::armature){
        Armature* a = (Armature*)ObjActivo; // Pose Mode: None deselecciona TODOS los huesos (+ sin hueso activo)
        for (size_t b = 0; b < a->bones.size(); b++) a->bones[b].select = false;
        a->boneActivo = -1; // sino el hueso activo seguia resaltado (parecia seleccionado)
	} else if (InteractionMode == ObjectMode && SceneCollection){
        ObjSelects.clear();
        SceneCollection->DeseleccionarCompleto(IncluirColecciones);
	}
}

void SeleccionarTodo(bool IncluirColecciones){
    //recorre las colecciones y selecciona todo. si llega a encontrar algo hace lo contrario. deselecciona todo
	if (estado != editNavegacion) return; // no durante un transform
	if (InteractionMode == EditMode && g_editMesh){
        g_editMesh->EditSeleccionarTodo(true); // edit: seleccionar todos los sub-elementos
        return;
    }
	if (InteractionMode == ObjectMode && SceneCollection){
        ObjSelects.clear();
        //habia algo seleccionado... asi que hacemos lo contrario. deseleccionar todo
        if (SceneCollection->SeleccionarCompleto(IncluirColecciones)){
            //std::cout << "habia algo seleccionado! se deselecciona todo\n";
            DeseleccionarTodo(IncluirColecciones);
            return;
        }
        //std::cout << "Todos los objetos seleccionados\n";
    }
}

// selecciona TODO siempre (a diferencia de SeleccionarTodo que togglea): primero
// deselecciona, asi SeleccionarCompleto no corta por "ya habia algo seleccionado"
void SeleccionarTodoForzado(bool IncluirColecciones){
    if (estado != editNavegacion) return; // no durante un transform
    if (InteractionMode == EditMode && g_editMesh){
        g_editMesh->EditSeleccionarTodo(true); // edit: seleccionar todos los sub-elementos
    } else if (InteractionMode == PoseMode && ObjActivo && ObjActivo->getType() == ObjectType::armature){
        Armature* a = (Armature*)ObjActivo; // Pose Mode: A selecciona TODOS los huesos (antes no hacia nada)
        for (size_t b = 0; b < a->bones.size(); b++) a->bones[b].select = true;
    } else if (InteractionMode == ObjectMode && SceneCollection){
        DeseleccionarTodo(IncluirColecciones);
        SceneCollection->SeleccionarCompleto(IncluirColecciones);
    }
}

// invierte la seleccion: lo seleccionado pasa a no, lo no seleccionado pasa a si
void InvertirSeleccion(bool IncluirColecciones){
    if (estado != editNavegacion) return; // no durante un transform
    if (InteractionMode == EditMode && g_editMesh){
        g_editMesh->EditInvertir(); // edit: invertir la seleccion de sub-elementos
    } else if (InteractionMode == PoseMode && ObjActivo && ObjActivo->getType() == ObjectType::armature){
        Armature* a = (Armature*)ObjActivo; // Pose Mode: Invert togglea el select de cada hueso
        for (size_t b = 0; b < a->bones.size(); b++) a->bones[b].select = !a->bones[b].select;
        if (a->boneActivo >= 0 && a->boneActivo < (int)a->bones.size() && !a->bones[a->boneActivo].select) a->boneActivo = -1; // el activo quedo deseleccionado
    } else if (InteractionMode == ObjectMode && SceneCollection){
        ObjSelects.clear();
        SceneCollection->InvertirSeleccionCompleto(IncluirColecciones);
    }
}

//si hay objetos seleccionados, devuelve true
bool HayObjetosSeleccionados(bool IncluirColecciones){
    return SceneCollection && SceneCollection->EstaSeleccionado(IncluirColecciones);
}