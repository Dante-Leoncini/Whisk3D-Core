#include "Objects.h"
#include "animation/W3dAnimSet.h"   // ~Object suelta la biblioteca de clips de jerarquia
#include "script/W3dScript.h" // W3dScriptData completo (el destructor lo libera)
#include "physics/W3dRigid.h" // W3dRigidDef completo (idem) + W3dRigidForget
#include "physics/W3dColMesh.h" // W3dColForget: la colision leida de una malla que se va
#include "physics/W3dHitbox.h"  // W3dHitboxForget: los pares de hitbox que lo nombran
#include "w3dGraphics.h" // flags de estado de render (w3dRenderLights) — PC y Symbian
#include "W3dNames.h"    // LA regla de nombres unicos (una sola, compartida por todo el editor)
#include "CameraBase.h"  // la vista BINDEADA (g_renderCam*): es lo que leen los constraints
#include "w3dlog.h"      // avisos (ciclo, vista sin bindear, base espejada): el Core no notifica
#include "objects/Armature.h"            // Child Of: el hueso de la fuente (W3dBone::poseWorld)
#include "animation/SkeletalAnimation.h" // Child Of: EvaluatePoseSkeleton (pose al frame actual)
#include "animation/Animation.h"         // CurrentFrame
#include <stdio.h>                        // printf: la traza del harness (g_traceGLSec)

// RAIZ de la escena. La DEFINE el Core (antes solo la declaraba y la definia el editor, asi que
// nadie podia enlazar contra el motor sin traerse el editor entero). El editor la llena con su
// propia raiz (una Scene) en el arranque; para el motor alcanza con que exista y valga 0.
Object* SceneCollection = 0;
// LOCAL VIEW (ver Objects.h): el editor los publica por-frame; Object::Render los lee para AISLAR la seleccion.
bool g_localViewActive = false;
const std::set<Object*>* g_visibleLocalView = 0;
const std::set<Object*>* g_localViewStep = 0;
bool W3dViewLocalOnlyStep(Object* o){
    return g_localViewActive && g_visibleLocalView && !g_visibleLocalView->count(o) && g_localViewStep && g_localViewStep->count(o);
}
bool W3dViewLocalHidden(Object* o){
    return g_localViewActive && g_visibleLocalView && !g_visibleLocalView->count(o) && !(g_localViewStep && g_localViewStep->count(o));
}

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
static unsigned int W3dNextSerial = 1;
unsigned int W3dObjectsSpawned() { return W3dNextSerial; }   // (lectura: ver Objects.h)
unsigned int W3dSelectionSerial = 0;                          // (ver Objects.h)

// LOS OBJETOS VIVOS (ver Objects.h). Un static LOCAL con puntero y no un std::set global: la raiz del
// editor nace en un constructor ESTATICO de otra unidad (Scene.cpp) y un set global podria no estar
// construido todavia (un vector en cero "anda"; un set en cero no). Nunca se libera (vive todo el proceso).
static std::set<const Object*>& LiveObjects() {
    static std::set<const Object*>* s = 0;
    if (!s) s = new std::set<const Object*>();
    return *s;
}
// la CACHE de la consulta: W3dObjectLive corre en CADA funcion de lua que recibe un objeto (cientos por cuadro en un
// juego) y el set son ~12 saltos de puntero (en el N95, fallos de cache). Una entrada dice "o estaba vivo en la
// generacion g": cualquier muerte sube la generacion y las invalida todas (los nacimientos no: un vivo sigue vivo).
// Ceros estaticos: valen desde antes del primer constructor global (la generacion 0 no existe)
struct W3dLiveCache { const Object* o; unsigned int gen; };
static W3dLiveCache gLiveCache[256];
static unsigned int gLiveGen = 1;
static unsigned int gLiveChanges = 1;   // cualquier alta o baja del conjunto (W3dLiveOfType)
static void ObjectDies(const Object* o) { LiveObjects().erase(o); gLiveGen++; gLiveChanges++; }
bool W3dObjectLive(const Object* o) {
    if (!o) return false;
    W3dLiveCache& c = gLiveCache[((unsigned int)(size_t)o * 2654435761u) >> 24];
    if (c.o == o && c.gen == gLiveGen) return true;
    const std::set<const Object*>& v = LiveObjects();
    if (v.find(o) == v.end()) return false;
    c.o = o; c.gen = gLiveGen;
    return true;
}
unsigned W3dObjectsGen() { return gLiveGen; }
unsigned W3dLiveChanges() { return gLiveChanges; }

// LOS VIVOS POR TIPO (ver Objects.h): se rearman del conjunto de vivos solo cuando cambio (nacio, murio o volvio
// alguno); el tipo de un objeto no cambia nunca
static const int kTypesN = (int)ObjectType::routine + 1;
const std::vector<Object*>& W3dLiveOfType(int type) {
    static std::vector<Object*> lists[kTypesN];
    static unsigned int built = 0;
    static const std::vector<Object*> empty;
    if (type < 0 || type >= kTypesN) return empty;
    if (built != gLiveChanges) {
        for (int k = 0; k < kTypesN; k++) lists[k].clear();
        const std::set<const Object*>& v = LiveObjects();
        for (std::set<const Object*>::const_iterator it = v.begin(); it != v.end(); ++it) {
            Object* o = const_cast<Object*>(*it);
            const int t = (int)o->getType();
            if (t >= 0 && t < kTypesN) lists[t].push_back(o);
        }
        built = gLiveChanges;
    }
    return lists[type];
}
bool W3dAttachedTo(const Object* o, const Object* root) {
    for (; o; o = o->Parent) if (o == root) return true;
    return false;
}
bool W3dLiveObjectSerial(const Object* o, unsigned int serial) {
    return W3dObjectLive(o) && o->serial == serial;
}
void W3dLiveObjectMark(const Object* o, bool live) {
    if (!o) return;
    if (live) { LiveObjects().insert(o); gLiveChanges++; } else ObjectDies(o);
}

// ganchos para desvincular tambien lo que NO cuelga del arbol vivo (ver Objects.h).
// Todo NULL = nadie engancho (el motor sin editor). Inicializacion ESTATICA (constante):
// vale desde antes del primer constructor global, asi que registrarse desde uno es seguro.
W3dUnlinkFn W3dUnlinkHooks[W3dUnlinkMaxHooks] = {0,0,0,0,0,0,0,0,0,0,0,0};
bool W3dUnlinkRegister(W3dUnlinkFn f){
    if (!f) return false;
    for (int i = 0; i < (int)W3dUnlinkMaxHooks; i++){
        if (W3dUnlinkHooks[i] == f) return true;   // ya estaba
        if (!W3dUnlinkHooks[i]) { W3dUnlinkHooks[i] = f; return true; }
    }
    return false;   // sin lugar: agrandar W3dUnlinkMaxHooks
}

// ---- RAICES EDITABLES (ver Objects.h): el registro de las escenas/prefabs cargados ----
// Un vector chico (un proyecto tiene pocas raices): recorrerlo es gratis al lado del arbol.
bool W3dRootCreating = false;
static std::vector<Object*> gRootsReg;
void W3dRootRegister(Object* root) {
    if (!root) return;
    for (size_t i = 0; i < gRootsReg.size(); i++) if (gRootsReg[i] == root) return;
    gRootsReg.push_back(root);
}
void W3dRootForget(Object* root) {
    for (size_t i = 0; i < gRootsReg.size(); i++)
        if (gRootsReg[i] == root) { gRootsReg.erase(gRootsReg.begin() + (long)i); return; }
}
int W3dRootsRegistered() { return (int)gRootsReg.size(); }
Object* W3dRegisteredRootIn(int i) {
    return (i >= 0 && i < (int)gRootsReg.size()) ? gRootsReg[(size_t)i] : NULL;
}
bool W3dIsRootRegistered(const Object* o) {
    if (!o) return false;
    for (size_t i = 0; i < gRootsReg.size(); i++) if (gRootsReg[i] == o) return true;
    return false;
}
void W3dLiveRoots(std::vector<Object*>& out) {
    out.clear();
    if (SceneCollection) out.push_back(SceneCollection);
    for (size_t i = 0; i < gRootsReg.size(); i++)
        if (gRootsReg[i] && gRootsReg[i] != SceneCollection) out.push_back(gRootsReg[i]);
}
Object* W3dRootOf(Object* o) {
    if (!o) return NULL;
    Object* limit = o;
    while (limit->Parent) limit = limit->Parent;
    if (limit == SceneCollection || W3dIsRootRegistered(limit)) return limit;
    return NULL;
}
Object* W3dAttachedRoot(Object* o) {
    if (!o) return NULL;
    Object* n = o;
    while (n->Parent) {
        const std::vector<Object*>& h = n->Parent->Childrens;
        if (std::find(h.begin(), h.end(), n) == h.end()) return NULL;   // (detachado: su padre ya no lo tiene)
        n = n->Parent;
    }
    if (n == SceneCollection || W3dIsRootRegistered(n)) return n;
    return NULL;
}

// Variables globales
std::vector<Object*> ObjSelects;
Object* CollectionActive = NULL;
Object* ActiveObj = NULL;
Object* g_editMesh = NULL;        // edit mode: malla activa (NULL = nadie)
int EditSelectMode = SelVertex;   // edit mode: sub-elemento (vertex por defecto)
std::vector<SaveState> objectState;

#ifdef W3D_SYMBIAN
// en PC lo define ViewPort3D.cpp (todavia sin portar); SceneCollection ya
// vive en Scene.cpp para ambas plataformas
// (Viewport3DActive: ahora lo define ViewPort3D.cpp real)
#endif

Object::Object(Object* parent, const std::string& newName, Vector3 Pos, Vector3 Rot, Vector3 Scale)
    : Parent(parent),
      visible(true),
      renderable(true),
      listingRail(255),
      isStatic(true),   // por defecto NO se mueve en runtime (convencion RenderWare); el juego marca dinamicos a Crash/enemigos
      projectsShadow(true), receivesShadow(true),   // (las sombras en tiempo real: como Unity, las dos prendidas)
      bouncesLight(true), catchesShadows(false),    // (la luz precalculada: rebota y no es captador de sombras)
      inProbes(true),                               // (lo ven las sondas de reflejo y de luz)
      noTransform(false),
      expanded(true),
      showRelationshipsLines(true),
      posedByCurves(false), posedByClip(false),   // nace en reposo: nadie lo poso todavia
      select(false), // Select() al final lo deja seleccionado
      name(newName),
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

    serial = W3dNextSerial++;   // identidad estable: se asigna al NACER y no se reusa jamas
    LiveObjects().insert(this); // (ver W3dObjectLive: lo que lua trae de vuelta se valida contra esto)
    gLiveChanges++;
    activeConstraint = -1;       // stack de constraints vacio: no hay ninguno seleccionado
    consEval = false;            // guardia de reentrada de la evaluacion (ver Objects.h)
    rotMode = RotEulerXYZ;       // default: XYZ Euler
    rotAngle = 0.0f;
    rotAxis = Vector3(0, 0, 1);
    scriptData = NULL;          // script lua opcional (ver script/W3dScript.h)
    clipsHier = NULL;           // biblioteca de clips de jerarquia opcional (ver animation/W3dAnimSet.h)
    physics = NULL;             // cuerpo rigido opcional (ver physics/W3dRigid.h)
    UpdateDisplayRot();

    // PADRE NORMALIZADO: un objeto de primer nivel tiene Parent = la raiz (antes quedaba en NULL
    // aunque viviera en SceneCollection->Childrens, mientras los loaders y el reparent si ponian
    // la raiz). Con varias raices (escenas, prefabs) un Parent NULL no diria de cual es, y todos
    // los 'Parent ? Parent : SceneCollection' lo mandarian a la ACTIVA. Una raiz que nace
    // (W3dRootCreating) no se cuelga de nadie.
    if (Parent) {
        Parent->Childrens.push_back(this);
    } else if (SceneCollection && this != SceneCollection && !W3dRootCreating) {
        Parent = SceneCollection;
        SceneCollection->Childrens.push_back(this);
    }

    if (W3dRootCreating) return;   // una raiz nueva: ni nombre libre ni seleccion (vive afuera)

    // NOMBRE UNICO desde el nacimiento: ya esta en el arbol, asi que su scope
    // (la escena a la que pertenece) es el definitivo. Con esto los ~15 ctores
    // con nombre fijo ("Mesh", "Light", "UI", "Boton"...) dejan de generar
    // homonimos sin que cada uno tenga que acordarse. En Object el getType()
    // virtual todavia devuelve baseObject: no importa, el scope se decide
    // subiendo por Parent (una raiz UI queda con scope global, que es lo que
    // corresponde). Durante la carga no se toca (ver W3dNamesLoading).
    if (!W3dNamesLoading && SceneCollection && this != SceneCollection)
        name = FreeName(name);

    DeselectAll();
    Select();
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
static float NearMod360(float v, float ref){
    float k = floorf((ref - v) / 360.0f + 0.5f);
    return v + k * 360.0f;
}

Vector3 W3dNearEuler(const Vector3& e, const Vector3& ref){
    // un euler no es unico: (a) sumarle 360 a cualquier componente da la MISMA rotacion, y (b) tambien la da la
    // forma "dada vuelta" (x+180, 180-y, z+180). Se prueban las dos familias y gana la que menos se aleja de 'ref'.
    Vector3 best = e; float bestD = 1e30f;
    for (int flip = 0; flip < 2; flip++){
        Vector3 c = flip ? Vector3(e.x + 180.0f, 180.0f - e.y, e.z + 180.0f) : e;
        c.x = NearMod360(c.x, ref.x);
        c.y = NearMod360(c.y, ref.y);
        c.z = NearMod360(c.z, ref.z);
        float d = fabsf(c.x - ref.x) + fabsf(c.y - ref.y) + fabsf(c.z - ref.z);
        if (d < bestD){ bestD = d; best = c; }
    }
    return best;
}

void Object::UpdateDisplayRot(){
    // el euler se re-deriva del quaternion CONSERVANDO las vueltas que ya tenia (ver arriba)
    rotEuler = W3dNearEuler(rot.ToEulerXYZ(), rotEuler);
    rot.ToAxisAngle(rotAngle, rotAxis);
    rotQuat = rot;
}

// EL QUATERNION MANDA (el transform de la vista, un constraint, una matriz importada): normaliza y deja todos
// los displays al dia. Es la puerta para "ya tengo la orientacion armada, ponela".
void Object::SetRot(const Quaternion& q){
    rot = q;
    rot.normalize();
    UpdateDisplayRot();
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
void Object::SetRotAxisAngle(const Vector3& axis, float angDeg){
    rot = Quaternion::FromAxisAngle(axis.Normalized(), angDeg);
    rot.normalize();
    UpdateDisplayRot();
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
//  dos listas de CompileGame.cpp); Objects.cpp ya esta en todas.
// ============================================================================

// CONTADOR de identidad estable de los constraints (ver W3dConstraintSerial en
// objects/W3dConstraint.h). Arranca en 1: el 0 significa "no es un constraint".
static unsigned int W3dConstraintNextSerial = 1;
W3dConstraintSerial::W3dConstraintSerial() : v(W3dConstraintNextSerial++) {}
W3dConstraintSerial::W3dConstraintSerial(const W3dConstraintSerial&) : v(W3dConstraintNextSerial++) {}

// nombre por defecto de cada tipo (para la lista + el menu "Add")
const char* W3dConstraintTypeName(int type){
    switch (type) {
        case W3dConstraintType::CopyLocation: return "Copy Location";
        case W3dConstraintType::CopyRotation: return "Copy Rotation";
        case W3dConstraintType::Billboard:    return "Billboard";
        case W3dConstraintType::ChildOf:      return "Child Of";
        case W3dConstraintType::CopyScale:      return "Copy Scale";
        case W3dConstraintType::CopyTransforms: return "Copy Transforms";
        case W3dConstraintType::TrackTo:        return "Track To";
        case W3dConstraintType::DampedTrack:    return "Damped Track";
        case W3dConstraintType::LockedTrack:    return "Locked Track";
        case W3dConstraintType::StretchTo:      return "Stretch To";
        case W3dConstraintType::LimitLocation:  return "Limit Location";
        case W3dConstraintType::LimitRotation:  return "Limit Rotation";
        case W3dConstraintType::LimitScale:     return "Limit Scale";
        case W3dConstraintType::LimitDistance:  return "Limit Distance";
        case W3dConstraintType::MaintainVolume: return "Maintain Volume";
        case W3dConstraintType::Floor:          return "Floor";
        case W3dConstraintType::Transformation: return "Transformation";
        case W3dConstraintType::Pivot:          return "Pivot";
        case W3dConstraintType::FollowPath:     return "Follow Path";
        case W3dConstraintType::ClampTo:        return "Clamp To";
        case W3dConstraintType::IK:             return "IK";
    }
    return "Constraint";
}

// el gancho de la polilinea de una Curve (ver W3dConstraint.h). NULL = nadie lo registro (un juego 2D,
// que no compila Curve.cpp): Follow Path / Clamp To quedan sin efecto, sin crashear.
W3dCurvePolyFn g_w3dCurvePoly = 0;

int Object::ConstraintsCount() const { return (int)constraints.size(); }

std::string Object::ConstraintName(int i) const {
    if (i < 0 || i >= (int)constraints.size()) return std::string();
    return constraints[i]->name;
}

void Object::SetActiveConstraint(int i){
    activeConstraint = (i >= 0 && i < (int)constraints.size()) ? i : -1;
}

// busca por SERIAL, nunca por indice ni por puntero (ver Objects.h). NULL = ya no esta.
W3dConstraint* Object::ConstraintBySerial(unsigned int s) const {
    if (!s) return NULL;
    for (size_t i = 0; i < constraints.size(); i++)
        if (constraints[i]->serial.v == s) return constraints[i];
    return NULL;
}

// ============================================================================
//  EL VINCULO POR NOMBRE DE LA FUENTE (contrato en Objects.h)
//
//  Se corre UNA vez, con la escena entera ya construida. Va por FindObjectByName,
//  o sea EXACTAMENTE la misma busqueda que resuelve targetName / RailName /
//  modArmature: si dos objetos comparten nombre, el constraint queda ligado al
//  MISMO de los dos que resolveria cualquier otro vinculo del archivo.
//
//  sourceType == Vista no lleva nombre y no se toca: 'Vista' es un tipo explicito
//  justo para que "sin puntero" no signifique nunca "segui a la camara".
// ============================================================================
static void W3dConsResolveRec(Object* node, Object* root, int& broken){
    if (!node) return;
    for (size_t i = 0; i < node->constraints.size(); i++) {
        W3dConstraint* c = node->constraints[i];
        if (!c || c->sourceType != W3dConstraintSource::Object) continue;
        if (c->sourceName.empty()) { c->sourceObj = NULL; continue; }
        // POR SCOPE: el constraint de un objeto generado por una instancia de prefab mira primero
        // adentro de SU instancia (su propia "Guia", no la de la primera instancia); afuera, igual que siempre
        c->sourceObj = W3dFindNameFrom(node, c->sourceName, root);
        if (!c->sourceObj) {
            // el NOMBRE se conserva a proposito: es el dato del modelo, y si el objeto
            // vuelve (undo del borrado, o el usuario lo crea) el vinculo se recupera solo.
            w3dLogfW("[W3D] '%s': el constraint '%s' apunta a '%s', que no esta en la escena "
                     "(queda sin fuente y no hace nada)",
                     node->name.c_str(), c->name.c_str(), c->sourceName.c_str());
            broken++;
        }
    }
    // los constraints de los HUESOS de un armature: su target y su pole, con la MISMA busqueda por scope
    if (node->getType() == ObjectType::armature) {
        Armature* a = (Armature*)node;
        for (size_t b = 0; b < a->bones.size(); b++) {
            std::vector<W3dConstraint*>& L = a->bones[b].constraints.list;
            for (size_t i = 0; i < L.size(); i++) {
                W3dConstraint* c = L[i]; if (!c) continue;
                c->boneCache = -1; c->poleBoneCache = -1;
                if (c->sourceType == W3dConstraintSource::Object) {
                    c->sourceObj = c->sourceName.empty() ? NULL : W3dFindNameFrom(node, c->sourceName, root);
                    if (!c->sourceName.empty() && !c->sourceObj) {
                        w3dLogfW("[W3D] '%s' hueso '%s': el constraint '%s' apunta a '%s', que no esta en la escena",
                                 node->name.c_str(), a->bones[b].name.c_str(), c->name.c_str(), c->sourceName.c_str());
                        broken++;
                    }
                }
                c->poleObj = c->poleName.empty() ? NULL : W3dFindNameFrom(node, c->poleName, root);
                if (!c->poleName.empty() && !c->poleObj) {
                    w3dLogfW("[W3D] '%s' hueso '%s': el pole de '%s' apunta a '%s', que no esta en la escena",
                             node->name.c_str(), a->bones[b].name.c_str(), c->name.c_str(), c->poleName.c_str());
                    broken++;
                }
            }
        }
        a->lastPoseFrame = -999999;   // los targets cambiaron: la pose se recalcula con ellos
    }
    for (size_t i = 0; i < node->Childrens.size(); i++)
        W3dConsResolveRec(node->Childrens[i], root, broken);
}

// ---- LA IDENTIDAD DE UNA REF DE CONSTRAINT (contrato en Objects.h) ----
W3dConstraint* W3dConstraintAtRef(Object* o, int slot, int* which){
    if (!o || slot < 0) return NULL;
    const int own = o->OwnRefs();
    if (slot >= own) {
        const int k = slot - own;
        if (k >= (int)o->constraints.size()) return NULL;
        if (which) *which = 0;
        return o->constraints[k];
    }
    if (o->getType() == ObjectType::armature) return ((Armature*)o)->BoneConstraintOfRef(slot, which);
    return NULL;
}
W3dConstraint* W3dConstraintBySerialAny(Object* o, unsigned int serial){
    if (!o || !serial) return NULL;
    W3dConstraint* c = o->ConstraintBySerial(serial);
    if (!c && o->getType() == ObjectType::armature) c = ((Armature*)o)->BoneConstraintBySerial(serial);
    return c;
}
void W3dConstraintSetRef(W3dConstraint* c, int which, Object* target, const std::string& name){
    if (!c) return;
    if (which == 1) { c->poleObj = target; c->poleName = name; c->poleBoneCache = -1; }
    else            { c->sourceObj = target; c->sourceName = name; c->boneCache = -1; }
}

// RENOMBRAR UN HUESO (contrato en Armature.h): el subtarget / pole de todo constraint que apunta a 'a'
static void W3dConsRenameBoneIn(W3dConstraint* c, Armature* a, const std::string& o, const std::string& n){
    if (!c) return;
    if (c->sourceObj == (Object*)a && c->bone == o) { c->bone = n; c->boneCache = -1; }
    if (c->poleObj == (Object*)a && c->poleBone == o) { c->poleBone = n; c->poleBoneCache = -1; }
}
void W3dConstraintsRenameBone(Object* root, Armature* a, const std::string& oldName, const std::string& newName){
    if (!root || !a || oldName == newName) return;
    for (size_t i = 0; i < root->constraints.size(); i++) W3dConsRenameBoneIn(root->constraints[i], a, oldName, newName);
    if (root->getType() == ObjectType::armature) {
        Armature* r = (Armature*)root;
        for (size_t b = 0; b < r->bones.size(); b++)
            for (size_t i = 0; i < r->bones[b].constraints.list.size(); i++)
                W3dConsRenameBoneIn(r->bones[b].constraints.list[i], a, oldName, newName);
        r->poseDirty = true;
    }
    for (size_t i = 0; i < root->Childrens.size(); i++) W3dConstraintsRenameBone(root->Childrens[i], a, oldName, newName);
}

// 'raiz' es lo que se RECORRE (en la carga es SceneCollection; al generar una instancia de prefab, la
// instancia). Se BUSCA por scope (W3dFindNameFrom) y, afuera de toda instancia, en el arbol entero
// del que cuelga 'raiz' (la escena), como siempre.
void W3dConstraintsResolveNames(Object* root){
    if (!root) return;
    int broken = 0;
    Object* tree = W3dRootOf(root);
    W3dConsResolveRec(root, tree ? tree : root, broken);
    if (broken) w3dLogfW("[W3D] %d constraint(s) quedaron sin fuente", broken);
}

// duplicar el stack (contrato en Objects.h). El 'new W3dConstraint(*c)' es la clave:
// el copy-ctor implicito copia todos los campos MENOS el serial, que W3dConstraintSerial
// re-genera para que la copia tenga identidad propia.
void W3dCopyConstraints(Object* dst, const Object* src){
    if (!dst || !src) return;
    for (size_t i = 0; i < src->constraints.size(); i++) {
        const W3dConstraint* c = src->constraints[i];
        if (!c) continue;
        dst->constraints.push_back(new W3dConstraint(*c));
    }
    // el seleccionado de la lista es estado de UI y viaja con el stack: si no, el panel
    // de la copia se abre en "ninguno" con la lista llena.
    if (!dst->constraints.empty() && dst->activeConstraint < 0)
        dst->activeConstraint = (int)dst->constraints.size() - 1;
}

// ---- LA PUERTA DE REFS: solo RUTEA (ver el bloque grande de Objects.h) ----
// [0 .. OwnRefs()-1] = la subclase; de ahi en adelante, un slot por constraint.
int Object::RefsObject() const { return OwnRefs() + (int)constraints.size(); }

Object* Object::RefObject(int i) const {
    const int own = OwnRefs();
    if (i < own) return OwnRef(i);
    const int c = i - own;
    if (c < 0 || c >= (int)constraints.size()) return NULL;
    return constraints[c]->sourceObj;
}

void Object::SetRefObject(int i, Object* o){
    const int own = OwnRefs();
    if (i < own) { SetOwnRef(i, o); return; }
    const int c = i - own;
    if (c < 0 || c >= (int)constraints.size()) return;
    constraints[c]->sourceObj = o;
}

std::string* Object::RefObjectName(int i){
    const int own = OwnRefs();
    if (i < own) return RefOwnName(i);
    const int c = i - own;
    if (c < 0 || c >= (int)constraints.size()) return NULL;
    return &constraints[c]->sourceName;
}

// suelta TODA referencia por puntero a 'borrado' (la enumeracion generica de Objects.h).
// El vinculo por NOMBRE se deja como esta: es dato del modelo y lo maneja quien borra.
void Object::UnlinkFrom(Object* deleted){
    const int n = RefsObject();
    for (int i = 0; i < n; i++)
        if (RefObject(i) == deleted) SetRefObject(i, NULL);
}

// recorre el arbol y le avisa a cada objeto que 'borrado' deja de existir, asi
// el que lo referencia (ej: Instance->target) suelta el puntero y no crashea
static void UnlinkFromTree(Object* node, Object* deleted){
    if (!node) return;
    node->UnlinkFrom(deleted);
    for (size_t i = 0; i < node->Childrens.size(); i++)
        UnlinkFromTree(node->Childrens[i], deleted);
}

Object::~Object() {
    ObjectDies(this);                       // deja de ser un objeto VIVO (ver W3dObjectLive)
    delete scriptData; scriptData = NULL;   // datos del script lua (si tenia)
    W3dRigidForget(this);                   // su cuerpo rigido runtime (si tenia)
    // ...y su colision de mallas (colSuelo/colPared) si alguna consulta la leyo: la cache es por
    // Object* + el buffer de vertices, y los dos se RECICLAN (~Mesh ya libera su geometria):
    // un objeto nuevo en la misma direccion usaria la grilla vieja, en otro lugar del mundo.
    W3dColForget(this);
    W3dHitboxForget(this);                      // ...y los pares de hitbox que lo nombran (sin evento)
    delete physics; physics = NULL;             // y la definicion de fisica
    // su biblioteca de clips de jerarquia (suelta la referencia al animset compartido)
    if (clipsHier) { W3dHierRelease(this); delete clipsHier; clipsHier = NULL; }
    // antes de irse: soltar cualquier puntero que apunte a este objeto (las
    // instancias linkeadas a el) para evitar punteros colgados al renderizar
    if (SceneCollection && SceneCollection != this)
        UnlinkFromTree(SceneCollection, this);
    // ...y las OTRAS raices cargadas (escenas/prefabs que no son la activa): una referencia
    // de alla (una instancia, un riel, la fuente de un constraint) no puede quedar colgando.
    // Sin raices registradas (el caso normal) no cuesta nada.
    for (size_t r = 0; r < gRootsReg.size(); r++) {
        Object* root = gRootsReg[r];
        if (root && root != SceneCollection && root != this) UnlinkFromTree(root, this);
    }
    // ...y lo que NO esta en el arbol: los subarboles DETACHADOS que tienen los comandos del
    // undo son inalcanzables desde SceneCollection, asi que el recorrido de arriba nunca les
    // llega y se quedaban apuntando a esta direccion despues del free. Lo mismo los punteros
    // que se guarda la UI del editor (popup de color, panel "Add"): ver Objects.h.
    for (int g = 0; g < (int)W3dUnlinkMaxHooks; g++)
        if (W3dUnlinkHooks[g]) W3dUnlinkHooks[g](this);

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
void W3dFreeSubtree(Object* root){
    if (!root) return;
    std::vector<Object*> children; children.swap(root->Childrens);
    for (size_t i = 0; i < children.size(); i++) W3dFreeSubtree(children[i]);
    root->Parent = NULL;
    delete root;
}

// ============================================================================
//  NOMBRES UNICOS de objeto - LA PUERTA.
//
//  SCOPE (decision documentada): un objeto es unico dentro de SU ESCENA, no
//  globalmente. La escena es la raiz UI de la que cuelga (el MISMO scope con el
//  que se resuelven las refs de los scripts, SimGame.cpp) o SceneCollection si
//  no cuelga de ninguna. Las RAICES UI en cambio son unicas GLOBALMENTE: su
//  nombre es la clave de gScenes y el nombre del .w3dui en disco.
//  Sin esto, abrir whiskpaddle (14 widgets homonimos entre sus 3 escenas)
//  renombraria medio proyecto y dejaria los object("op1") de lua en nil.
// ============================================================================

// true mientras se esta CARGANDO un archivo: los loaders escriben el nombre
// crudo (asi el .w3d entra tal cual) y la reparacion se hace de una sola vez al
// final, con el arbol completo y arrastrando los vinculos por nombre. Ademas
// evita el O(n^2) de uniquificar objeto por objeto durante la carga.
bool W3dNamesLoading = false;

// ---- LAS FRONTERAS DE SCOPE (ver Objects.h): UN solo predicado para todo el motor ----
bool W3dIsBoundaryScope(const Object* o) {
    if (!o) return false;
    const ObjectType t = const_cast<Object*>(o)->getType();
    return t == ObjectType::ui || W3dIsInstanceType(t);   // (una instancia de prefab o un proxy de libreria)
}
Object* W3dInstanceOf(const Object* o) {
    if (!o) return NULL;
    for (Object* p = o->Parent; p; p = p->Parent)
        if (W3dIsInstanceType(p->getType())) return p;
    return NULL;
}
// preorden por el SCOPE de 'inst' (sus hijos generados): no entra a las instancias anidadas (su contenido es
// de ellas) pero si ve su nombre (una instancia anidada es un objeto de este scope)
static Object* FindInScopeInstance(Object* node, const std::string& name) {
    for (size_t i = 0; i < node->Childrens.size(); i++) {
        Object* h = node->Childrens[i];
        if (!h) continue;
        if (h->name == name) return h;
        if (W3dIsBoundaryScope(h)) continue;
        Object* r = FindInScopeInstance(h, name);
        if (r) return r;
    }
    return NULL;
}
Object* W3dFindNameFrom(Object* from, const std::string& name, Object* root) {
    if (name.empty()) return NULL;
    for (Object* inst = W3dInstanceOf(from); inst; inst = W3dInstanceOf(inst)) {
        Object* r = FindInScopeInstance(inst, name);
        if (r) return r;
    }
    if (!root) root = from ? W3dRootOf(from) : NULL;
    if (!root) root = SceneCollection;
    return FindObjectByName(root, name);
}

Object* W3dScopeNameOf(Object* o) {
    if (!o) return SceneCollection;
    // una raiz UI (escena) NO se mide contra si misma: va contra TODO el arbol. Cualquier otro objeto
    // es del scope de la FRONTERA mas cercana que lo contiene (una UI o una instancia de prefab)
    const bool isUI = (o->getType() == ObjectType::ui);
    Object* limit = o;
    for (Object* p = o->Parent; p; p = p->Parent) {
        if (!isUI && W3dIsBoundaryScope(p)) return p;
        limit = p;
    }
    // el scope global de un objeto de OTRA raiz cargada (una escena o un prefab que no es el
    // activo) es esa raiz, no la activa. Para los de la activa (el caso de siempre) no cambia nada.
    if (limit != o && limit != SceneCollection && W3dIsRootRegistered(limit)) return limit;
    return SceneCollection;
}

// le pasa al INDICE los nombres del scope de una sola pasada (menos el propio, que
// puede conservar el suyo).
//
// LA PARTICION ES LA MISMA QUE LA DE LA REPARACION (RepairRec en ObjectMode.cpp), y
// esa coherencia importa: antes esto indexaba el SUBARBOL ENTERO desde la raiz, o sea
// que los widgets de una UI ANIDADA contaban como tomados en el scope de AFUERA. No
// fabricaba duplicados (indexar de mas solo renombra de mas), pero mudar un objeto al
// scope global lo renumeraba contra nombres que ahi no son suyos.
// La regla, textual, es la de W3dScopeNameOf: un objeto pertenece a la UI mas cercana
// que lo contiene, y una RAIZ UI (a cualquier profundidad) pertenece al scope GLOBAL.
//   - scope GLOBAL (raiz == SceneCollection): cuenta todo lo que NO esta dentro de
//     ninguna UI, MAS todas las raices UI (hay que seguir bajando para encontrarlas).
//   - scope de una UI: cuenta sus descendientes que no cruzan otra UI; al toparse con
//     una UI anidada se corta (esa UI y todo lo suyo viven en otros scopes).
//   - con INSTANCIAS DE PREFAB (la otra frontera, W3dIsBoundaryScope): la instancia es un objeto
//     del scope de afuera, y lo que genera es de ella (se corta ahi, igual que con una UI).
static void IndexScopeGlobal(Object* obj, const Object* me, W3dIndexName& ix, bool insideUI) {
    if (!obj) return;
    for (size_t i = 0; i < obj->Childrens.size(); i++) {
        Object* h = obj->Childrens[i];
        const bool isUI = (h->getType() == ObjectType::ui);
        if ((isUI || !insideUI) && h != me) ix.Show(h->name);
        IndexScopeGlobal(h, me, ix, insideUI || W3dIsBoundaryScope(h));
    }
}
static void IndexScopeUI(Object* obj, const Object* me, W3dIndexName& ix) {
    if (!obj) return;
    for (size_t i = 0; i < obj->Childrens.size(); i++) {
        Object* h = obj->Childrens[i];
        if (h->getType() == ObjectType::ui) continue;   // otro scope (el suyo y el global)
        if (h != me) ix.Show(h->name);
        if (W3dIsBoundaryScope(h)) continue;             // una instancia anidada: su nombre es de aca, su contenido no
        IndexScopeUI(h, me, ix);
    }
}

// nombre libre para 'this' en su scope (NO escribe nada). Con el scope vacio
// (SceneCollection == NULL: el runtime del juego antes de armar la escena)
// devuelve el nombre normalizado tal cual.
//
// PERFORMANCE: UNA sola pasada por el scope (antes: una pasada por CANDIDATO,
// o sea O(N^2) por nombre y O(N^3) para armar la escena - 3000 "add vertex"
// tardaban 19 segundos). Ver W3dIndexName en W3dNames.h.
std::string Object::FreeName(const std::string& base) {
    Object* root = W3dScopeNameOf(this);
    if (!root) return W3dNameNormalize(base, "Object");
    W3dIndexName ix;
    ix.Begin(base, "Object");
    // la raiz del scope tambien ocupa su nombre (la busqueda por nombre arranca EN ella;
    // es la misma semilla que usa RepairRec). Una INSTANCIA DE PREFAB no: su nombre es del scope de
    // afuera y la busqueda adentro de ella arranca en sus hijos (W3dFindNameFrom)
    const bool isInst = W3dIsInstanceType(root->getType());
    if (root != this && !isInst) ix.Show(root->name);
    if (root->getType() == ObjectType::ui || isInst) IndexScopeUI(root, this, ix);
    else                                             IndexScopeGlobal(root, this, ix, false);
    return ix.Resolve();
}

// LA PUERTA: todo el editor pasa por aca para escribir el nombre de un objeto
// (crear, menu Add, duplicar, importar, pegar, y el campo de texto del panel).
void Object::SetNameObj(const std::string& newName) {
    if (W3dNamesLoading) { name = newName; return; }  // carga: crudo + reparacion al final
    name = FreeName(newName);
}

// asignacion CRUDA, sin uniquificar. SOLO para los loaders (el .w3d puede traer
// duplicados de archivos viejos: se reparan de una en W3dNamesRepairScene,
// que ademas arrastra las refs de scripts) y para el harness de tests, que
// necesita poder FABRICAR duplicados para verificar la reparacion.
void Object::SetRawName(const std::string& newName) {
    name = newName;
}

void Object::RemoveObjectsSelected(bool IncludeCollections) {
    for (int i = (int)Childrens.size() - 1; i >= 0; i--) {
        Object* child = Childrens[i];
        child->RemoveObjectsSelected(IncludeCollections);
        if (child->select && (IncludeCollections || child->getType() != ObjectType::collection)) {
            std::cout << "Se borro '" << child->name << "'" << std::endl;
            Childrens.erase(Childrens.begin() + i);
            delete child;
        }
    }
}

// SetName: nombre libre para 'this' SIN escribirlo (lo escribe el caller).
// Se conserva por compatibilidad; por dentro es la MISMA regla que SetNameObj.
std::string Object::SetName(const std::string& baseName) {
    return FreeName(baseName);
}

void Object::Select(){
    W3dSelectionSerial++;   // la escena eligio algo (ver Objects.h)
    ActiveObj = this;
    if (!select){
        select = true;
        ObjSelects.push_back(this);
    }
}

void Object::Deselect(){
    select = false;
    for(size_t o=0; o < ObjSelects.size(); o++){
        if (this == ObjSelects[o]){
            ObjSelects.erase(ObjSelects.begin() + o);
            break;
        }                
    } 
}

void Object::DeselectComplete(bool IncludeCollections){
    select = false;
    for(size_t o=0; o < Childrens.size(); o++){
        Childrens[o]->DeselectComplete(IncludeCollections);		
    } 
}

bool Object::IsSelected(bool IncludeCollections){
    // Si este objeto está seleccionado y cumple la condición de colecciones → true
    if ( select && ( IncludeCollections || getType() != ObjectType::collection ) ) {
        return true;
    }

    //si no estaba seleccionado. mira recursivamente a los hijos
    for(size_t o=0; o < Childrens.size(); o++){   
        if (Childrens[o]->IsSelected(IncludeCollections)) return true;
    }
    return false;       
}

bool Object::SelectComplete(bool IncludeCollections){
    //si hay algo seleccionado retorna true para deseleccionar todo
    if (select) return true;

    if (IncludeCollections || (getType() != ObjectType::collection && getType() != ObjectType::baseObject)){
        select = true;
        ObjSelects.push_back(this);
        if (!ActiveObj) ActiveObj = this;
    }

    for(size_t o=0; o < Childrens.size(); o++){
        if (Childrens[o]->SelectComplete(IncludeCollections)) return true;
    }
    //nada seleccionado
    return false;
}

void Object::InvertCompleteSelection(bool IncludeCollections){
    // flip al mismo conjunto de objetos elegibles que SelectComplete
    if (IncludeCollections || (getType() != ObjectType::collection && getType() != ObjectType::baseObject)){
        select = !select;
        if (select){
            ObjSelects.push_back(this);
            ActiveObj = this;
        }
    }
    for(size_t o=0; o < Childrens.size(); o++){
        Childrens[o]->InvertCompleteSelection(IncludeCollections);
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
bool W3dMatrixQuat(const Matrix4& m, Quaternion& out){
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
float W3dShortDifAng(float destination, float origin){
    float d = destination - origin;
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
static bool gNoticeNoView = false;
static bool gNoticeBaseMirrored = false;

// ORIENTACION en mundo de la fuente de un Copy Rotation. false = la fuente cuelga de una base
// espejada / de escala cero y no hay quaternion que la describa -> el constraint se saltea.
static bool W3dQuatWorldOf(const Object* o, Quaternion& out){
    Matrix4 W;
    o->GetWorldMatrix(W);      // recursivo a proposito: la fuente llega con SUS constraints puestos
    return W3dMatrixQuat(W, out);
}

// ===================================================================================================
//  EL OBJETO QUE SE ESTA EDITANDO (Edit Mode). NULL = ninguno.
//  Vive en el Core -y no en el editor- porque el que tiene que saltearse los constraints es el
//  EVALUADOR, y el evaluador tambien corre sin editor (juego, Android, Symbian), donde este puntero
//  se queda en NULL para siempre y no cuesta nada. Lo setea el editor al entrar y salir de Edit Mode
//  (W3dConstraintSetObjEditing, main/objects/ObjectMode.cpp).
// ===================================================================================================
Object* g_consObjEditing = NULL;
void W3dConstraintSetObjEditing(Object* o){ g_consObjEditing = o; }

// un constraint es EFECTIVO si PUEDE cambiar algo. Lo usan el pre-pase (para poder devolver la
// base sin hacer una sola cuenta) y tambien el bucle, para que el criterio viva en UN solo lugar
// y no se vayan separando con el tiempo.
// 'dueno' es el objeto al que pertenece: hace falta para el "ver en modo edicion", que depende de
// SI ES EL QUE SE ESTA EDITANDO y no del constraint solo.
static bool W3dConstraintEffective(const W3dConstraint* c, const Object* owner){
    // VER EN MODO EDICION apagado: mientras se editan los vertices de ESTE objeto, el constraint no
    // se aplica y la malla se dibuja y se edita donde dicen sus campos reales (ver W3dConstraint.h).
    // Solo el objeto en edicion: apagarselo tambien a los padres moveria a sus otros hijos.
    if (!c->showEdit && owner && owner == g_consObjEditing) return false;
    if (!c->active) return false;
    if (c->Weight() <= 0.0f) return false;
    if (c->type == W3dConstraintType::Billboard) return (c->bbYaw || c->bbPitch);
    // Child Of: sin fuente (objeto) o sin ningun componente heredado no hace nada
    if (c->type == W3dConstraintType::ChildOf)
        return c->sourceType == W3dConstraintSource::Object && c->sourceObj != NULL && (c->coLoc || c->coRot || c->coEsc);
    // los tipos NUEVOS (fase 2): el criterio comun con los huesos
    if (c->type != W3dConstraintType::CopyLocation && c->type != W3dConstraintType::CopyRotation)
        return W3dConsTypeEffective(c, false);
    // Copy Location / Copy Rotation: sin ejes no copia nada, y sin fuente tampoco
    if (!c->axisX && !c->axisY && !c->axisZ) return false;
    return (c->sourceType == W3dConstraintSource::View || c->sourceObj != NULL);
}

// la version publica del predicado (ver Objects.h): el editor pregunta por aca en vez de mirar
// constraints.empty(), que dice "tiene" y no "hace algo".
bool W3dObjHasConstraintEffective(const Object* o){
    if (!o) return false;
    for (size_t i = 0; i < o->constraints.size(); i++)
        if (o->constraints[i] && W3dConstraintEffective(o->constraints[i], o)) return true;
    return false;
}

// ============================================================================
//  CHILD OF: la matriz de la FUENTE (objeto, o hueso de un armature) en MUNDO
// ============================================================================
static float W3dLengthCol(const Matrix4& m, int c){
    return sqrtf(m.m[c*4]*m.m[c*4] + m.m[c*4+1]*m.m[c*4+1] + m.m[c*4+2]*m.m[c*4+2]);
}
// el hueso por NOMBRE, con el indice cacheado (se revalida por nombre: renombrar/borrar huesos no
// deja un indice colgado). -1 = no esta.
static int W3dChildOfBone(W3dConstraint* c, const Armature* a){
    const int n = (int)a->bones.size();
    if (c->boneCache >= 0 && c->boneCache < n && a->bones[c->boneCache].name == c->bone) return c->boneCache;
    c->boneCache = -1;
    for (int i = 0; i < n; i++) if (a->bones[i].name == c->bone) { c->boneCache = i; break; }
    return c->boneCache;
}
// Mt = mundo de la fuente (con el hueso si hay), con los componentes NO heredados sacados (Loc/Rot/Scale
// como en Blender). 'Wf' = el mundo de la fuente si el que llama ya lo tiene (la fuente es el padre).
static void W3dChildOfMatrix(W3dConstraint* c, const Matrix4* Wf, Matrix4& Mt){
    Object* f = c->sourceObj;
    if (Wf) Mt = *Wf; else f->GetWorldMatrix(Mt);
    if (!c->bone.empty() && f->getType() == ObjectType::armature) {
        Armature* a = (Armature*)f;
        EvaluatePoseSkeleton(a, CurrentFrame);          // cacheado por frame: gratis si ya se dibujo
        const int h = W3dChildOfBone(c, a);
        if (h >= 0) Mt = Mt * a->bones[h].poseWorld;
    }
    if (c->coLoc && c->coRot && c->coEsc) return;       // lo normal: la matriz tal cual, sin cuentas
    Vector3 t(Mt.m[12], Mt.m[13], Mt.m[14]);
    Vector3 e(W3dLengthCol(Mt, 0), W3dLengthCol(Mt, 1), W3dLengthCol(Mt, 2));
    Quaternion q;
    if (!W3dMatrixQuat(Mt, q)) q = Quaternion();
    Mt = W3dLocalTRS(c->coLoc ? t : Vector3(0, 0, 0), c->coRot ? q : Quaternion(), c->coEsc ? e : Vector3(1, 1, 1));
}

bool W3dChildOfSetInverse(Object* o, W3dConstraint* c){
    if (!o || !c || c->type != W3dConstraintType::ChildOf || !c->sourceObj) return false;
    // que el objeto quede donde esta: fuente * inversa * local = padre * local -> inversa = fuente^-1 * padre
    Matrix4 Mt, Wp, iMt;
    W3dChildOfMatrix(c, NULL, Mt);
    if (o->Parent) o->Parent->GetWorldMatrix(Wp); else Wp.Identity();
    if (!Matrix4::InvertAffine(Mt, iMt)) return false;
    c->inverse = iMt * Wp;
    return true;
}

// ============================================================================
//  EVALUACION GENERICA (fase 2): un constraint sobre una MATRIZ (ver W3dConsCtx en W3dConstraint.h)
//
//  Los huesos y los tipos nuevos de objeto pasan por aca. Todo se hace en el "espacio comun" del
//  dueno (el mundo para un objeto, el del objeto armature para un hueso) y se pasa a World / Local /
//  Pose solo donde el constraint lo pide. Las formulas son las de Blender (constraint.cc), con los
//  ejes del motor (Y arriba): un rig convertido se comporta igual eligiendo los ejes que corresponden.
//  NADA de esto aloca por frame salvo el scratch ESTATICO de la polilinea de Follow Path / Clamp To.
// ============================================================================
static inline Vector3 W3dCol(const Matrix4& m, int c) { return Vector3(m.m[c*4], m.m[c*4+1], m.m[c*4+2]); }
static inline void W3dSetCol(Matrix4& m, int c, const Vector3& v) { m.m[c*4] = v.x; m.m[c*4+1] = v.y; m.m[c*4+2] = v.z; }
static inline Vector3 W3dTr(const Matrix4& m) { return Vector3(m.m[12], m.m[13], m.m[14]); }
static inline void W3dSetTr(Matrix4& m, const Vector3& v) { m.m[12] = v.x; m.m[13] = v.y; m.m[14] = v.z; }
// solo la parte lineal (una DIRECCION: sin la traslacion)
static inline Vector3 W3dDir(const Matrix4& m, const Vector3& v) {
    return Vector3(m.m[0]*v.x + m.m[4]*v.y + m.m[8]*v.z, m.m[1]*v.x + m.m[5]*v.y + m.m[9]*v.z, m.m[2]*v.x + m.m[6]*v.y + m.m[10]*v.z);
}
static inline Vector3 W3dAxisVec(int axis) {
    switch (axis) {
        case W3dConstraintAxis::X:    return Vector3( 1, 0, 0);
        case W3dConstraintAxis::Y:    return Vector3( 0, 1, 0);
        case W3dConstraintAxis::Z:    return Vector3( 0, 0, 1);
        case W3dConstraintAxis::NegX: return Vector3(-1, 0, 0);
        case W3dConstraintAxis::NegY: return Vector3( 0,-1, 0);
        default:                      return Vector3( 0, 0,-1);
    }
}
static inline Matrix4 W3dRotMat(const Quaternion& q) { Matrix4 R; q.ToMatrix(R.m); return R; }

Quaternion W3dQuatFromTo(const Vector3& from, const Vector3& to){
    const float d = from.Dot(to);
    // (sin atajo para "casi paralelos": (1+d, cruz) normalizado es estable ahi, y un umbral como 0.999999
    //  se comia giros de hasta 0.08 grados -> un IK que quedaba a 1 mm del target en una cadena de 3 m)
    if (d < -0.999999f) {
        // opuestos: medio giro alrededor de CUALQUIER perpendicular (se elige la mas estable)
        Vector3 ax = Vector3::Cross(Vector3(1, 0, 0), from);
        if (ax.LengthSq() < 1e-6f) ax = Vector3::Cross(Vector3(0, 1, 0), from);
        ax = ax.Normalized();
        return Quaternion(0.0f, ax.x, ax.y, ax.z);
    }
    const Vector3 c = Vector3::Cross(from, to);
    Quaternion q(1.0f + d, c.x, c.y, c.z);   // (la mitad del angulo, sin trigonometria)
    q.normalize();
    return q;
}

// EULER <-> MATRIZ en los 6 ordenes de MatRotEuler: orden (a,b,c) compone R_c * R_b * R_a (a se aplica
// primero). La extraccion es la de siempre generalizada con la paridad de la permutacion (verificada contra
// la composicion en los 6 ordenes); en el gimbal lock fija el ultimo angulo en 0, como SkelMatrixToEulerFBX.
static const int kEulerAxes[6][3] = { {0,1,2}, {0,2,1}, {1,2,0}, {1,0,2}, {2,0,1}, {2,1,0} };
static inline float W3dM(const Matrix4& m, int row, int col) { return m.m[col*4 + row]; }
Vector3 W3dEulerFromMatrix(const Matrix4& R, int order){
    if (order < 0 || order > 5) order = 0;
    const int a = kEulerAxes[order][0], b = kEulerAxes[order][1], c = kEulerAxes[order][2];
    const float sg = (b == (a + 1) % 3) ? 1.0f : -1.0f;   // paridad: par = +1
    float sb = -sg * W3dM(R, c, a);
    if (sb > 1.0f) sb = 1.0f; if (sb < -1.0f) sb = -1.0f;
    const float beta = asinf(sb);
    float alpha, gamma;
    if (fabsf(cosf(beta)) > 1e-4f) {
        alpha = atan2f(sg * W3dM(R, c, b), W3dM(R, c, c));
        gamma = atan2f(sg * W3dM(R, b, a), W3dM(R, a, a));
    } else {
        alpha = atan2f(-sg * W3dM(R, b, c), W3dM(R, b, b));
        gamma = 0.0f;
    }
    const float R2D = 57.2957795f;
    float e[3]; e[a] = alpha * R2D; e[b] = beta * R2D; e[c] = gamma * R2D;
    return Vector3(e[0], e[1], e[2]);
}
Matrix4 W3dMatrixFromEuler(const Vector3& deg, int order){
    if (order < 0 || order > 5) order = 0;
    const float D2R = 0.0174532925f;
    const float ang[3] = { deg.x, deg.y, deg.z };
    Matrix4 out; out.Identity();
    // R = R_c * R_b * R_a: se arma multiplicando de la izquierda en el orden a, b, c
    for (int k = 0; k < 3; k++) {
        const int ax = kEulerAxes[order][k];
        const float cs = cosf(ang[ax] * D2R), sn = sinf(ang[ax] * D2R);
        Matrix4 r; r.Identity();
        if (ax == 0) { r.m[5] = cs; r.m[6] = sn; r.m[9] = -sn; r.m[10] = cs; }
        else if (ax == 1) { r.m[0] = cs; r.m[2] = -sn; r.m[8] = sn; r.m[10] = cs; }
        else { r.m[0] = cs; r.m[1] = sn; r.m[4] = -sn; r.m[5] = cs; }
        out = r * out;
    }
    return out;
}

// descompone T * R * S. false = base espejada o plana (no hay rotacion PROPIA que describa la orientacion)
static bool W3dDecomp(const Matrix4& m, Vector3& t, Quaternion& q, Vector3& s){
    t = W3dTr(m);
    s = Vector3(W3dCol(m, 0).Length(), W3dCol(m, 1).Length(), W3dCol(m, 2).Length());
    return W3dMatrixQuat(m, q);
}

Matrix4 W3dConsBlend(const Matrix4& a, const Matrix4& b, float t){
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    Vector3 ta, sa, tb, sb; Quaternion qa, qb;
    if (!W3dDecomp(a, ta, qa, sa) || !W3dDecomp(b, tb, qb, sb)) {
        // sin rotacion propia que interpolar: mezcla lineal elemento a elemento (no crashea ni da NaN)
        Matrix4 r; for (int i = 0; i < 16; i++) r.m[i] = a.m[i] + (b.m[i] - a.m[i]) * t;
        return r;
    }
    return W3dLocalTRS(ta + (tb - ta) * t, Quaternion::Slerp(qa, qb, t), sa + (sb - sa) * t);
}

void W3dChildOfMaskMatrix(const W3dConstraint* c, Matrix4& T){
    if (!c || (c->coLoc && c->coRot && c->coEsc)) return;
    Vector3 t, s; Quaternion q;
    if (!W3dDecomp(T, t, q, s)) q = Quaternion();
    T = W3dLocalTRS(c->coLoc ? t : Vector3(0, 0, 0), c->coRot ? q : Quaternion(), c->coEsc ? s : Vector3(1, 1, 1));
}

bool W3dConsNeedsTarget(int type){
    switch (type) {
        case W3dConstraintType::Billboard:     case W3dConstraintType::LimitLocation:
        case W3dConstraintType::LimitRotation: case W3dConstraintType::LimitScale:
        case W3dConstraintType::MaintainVolume: case W3dConstraintType::Pivot:
            return false;
        default: return true;
    }
}

bool W3dConsTypeEffective(const W3dConstraint* c, bool bone){
    if (!c || !c->active || c->Weight() <= 0.0f) return false;
    if (bone ? !W3dConstraintTypeForBone(c->type) : !W3dConstraintTypeForObject(c->type)) return false;
    // un hueso no tiene VISTA: su pose se cachea por frame y no por viewport
    if (bone && c->sourceType == W3dConstraintSource::View) return false;
    switch (c->type) {
        case W3dConstraintType::LimitLocation: case W3dConstraintType::LimitRotation: case W3dConstraintType::LimitScale:
            for (int k = 0; k < 3; k++) if (c->useMin[k] || c->useMax[k]) return true;
            return false;
        case W3dConstraintType::MaintainVolume: case W3dConstraintType::Pivot:
            return true;
        case W3dConstraintType::CopyLocation: case W3dConstraintType::CopyRotation: case W3dConstraintType::CopyScale:
            if (!c->axisX && !c->axisY && !c->axisZ) return false;
            return c->sourceType == W3dConstraintSource::View || c->sourceObj != NULL;
        case W3dConstraintType::ChildOf:
            return c->sourceObj != NULL && (c->coLoc || c->coRot || c->coEsc);
        case W3dConstraintType::FollowPath: case W3dConstraintType::ClampTo:
            return c->sourceObj != NULL && c->sourceObj->getType() == ObjectType::curve && g_w3dCurvePoly != 0;
        default:
            return c->sourceObj != NULL;
    }
}

// ---- espacios ----
static Matrix4 W3dToSpace(const W3dConsCtx& x, const W3dConsMat& m, int space){
    if (space == W3dConstraintSpace::World) return x.toWorld * m.M;
    if (space == W3dConstraintSpace::Local) { Matrix4 iB; Matrix4::InvertAffine(m.rest, iB); return iB * m.M; }
    return m.M;
}
static Matrix4 W3dFromSpace(const W3dConsCtx& x, const W3dConsMat& m, const Matrix4& Ms, int space){
    if (space == W3dConstraintSpace::World) return x.fromWorld * Ms;
    if (space == W3dConstraintSpace::Local) return m.rest * Ms;
    return Ms;
}

// una base ortonormal con el eje 'a' (0..2, con signo 'sa') = f y el eje 'b' (0..2) = u (ya perpendicular
// a f y unitario). El tercero sale del producto cruz CON LA MANO CORRECTA (X x Y = Z).
static bool W3dBasis(int a, float sa, const Vector3& f, int b, const Vector3& u, Quaternion& q){
    if (a == b) return false;
    Vector3 col[3];
    col[a] = f * sa; col[b] = u;
    const int c = 3 - a - b;
    col[c] = (b == (a + 1) % 3) ? Vector3::Cross(col[a], col[b]) : Vector3::Cross(col[b], col[a]);
    Matrix4 R; R.Identity();
    for (int k = 0; k < 3; k++) W3dSetCol(R, k, col[k]);
    return W3dMatrixQuat(R, q);
}
// orientacion que apunta el eje 'track' (W3dConstraintAxis, con signo) del dueno a 'f' (unitario) con su
// eje 'up' (0..2) lo mas cerca posible de 'upRef'. 'qCur' = la orientacion actual (si 'upRef' es paralelo a
// 'f' se conserva el "arriba" que ya tenia, en vez de pegar un giro arbitrario).
static bool W3dAimBasis(int track, int up, const Vector3& f, const Vector3& upRef, const Quaternion& qCur, Quaternion& q){
    const int a = track % 3; const float sa = (track >= 3) ? -1.0f : 1.0f;
    if (a == up) return false;
    Vector3 u = upRef - f * upRef.Dot(f);
    if (u.LengthSq() < 1e-8f) {
        const Vector3 cu = qCur * W3dAxisVec(up);
        u = cu - f * cu.Dot(f);
        if (u.LengthSq() < 1e-8f) return false;
    }
    return W3dBasis(a, sa, f, up, u.Normalized(), q);
}

// ---- la POLILINEA de una curva, ORDENADA (Follow Path / Clamp To) ----
// Sin aristas es la polilinea implicita 0-1-2-...; con aristas se camina desde una PUNTA (un nodo de
// grado 1; si no hay ninguna es un lazo y se arranca del 0) por el primer vecino sin visitar. Las RAMAS
// que salen del camino principal se ignoran (documentado). Scratch estatico: sin allocs por frame.
static std::vector<Vector3> gPathPts;
static std::vector<float>   gPathLen;
static std::vector<int>     gPathDeg, gPathAdj, gPathFirst;
static std::vector<char>    gPathSeen;
static bool W3dPathBuild(const Object* curve, bool& closed){
    closed = false;
    gPathPts.clear(); gPathLen.clear();
    const float* v = NULL; const unsigned short* e = NULL; int n = 0, ne = 0;
    if (!g_w3dCurvePoly || !curve || !g_w3dCurvePoly(curve, &v, &n, &e, &ne) || !v || n < 1) return false;
    if (ne <= 0 || !e) {
        for (int i = 0; i < n; i++) gPathPts.push_back(Vector3(v[i*3], v[i*3+1], v[i*3+2]));
    } else {
        // adyacencia CSR (cada arista aparece en los dos nodos)
        gPathDeg.assign(n, 0); gPathFirst.assign(n + 1, 0);
        for (int k = 0; k < ne; k++) { const int p = e[k*2], q2 = e[k*2+1];
            if (p < n && q2 < n && p != q2) { gPathDeg[p]++; gPathDeg[q2]++; } }
        for (int i = 0; i < n; i++) gPathFirst[i + 1] = gPathFirst[i] + gPathDeg[i];
        gPathAdj.assign(gPathFirst[n], 0);
        std::vector<int>& fill = gPathDeg;   // se reusa como cursor de llenado
        for (int i = 0; i < n; i++) fill[i] = gPathFirst[i];
        for (int k = 0; k < ne; k++) { const int p = e[k*2], q2 = e[k*2+1];
            if (p < n && q2 < n && p != q2) { gPathAdj[fill[p]++] = q2; gPathAdj[fill[q2]++] = p; } }
        int start = -1;
        for (int i = 0; i < n && start < 0; i++) if (gPathFirst[i + 1] - gPathFirst[i] == 1) start = i;
        if (start < 0) start = 0;
        gPathSeen.assign(n, 0);
        int cur = start;
        while (cur >= 0) {
            gPathSeen[cur] = 1; gPathPts.push_back(Vector3(v[cur*3], v[cur*3+1], v[cur*3+2]));
            int next = -1;
            for (int k = gPathFirst[cur]; k < gPathFirst[cur + 1]; k++) if (!gPathSeen[gPathAdj[k]]) { next = gPathAdj[k]; break; }
            if (next < 0 && gPathPts.size() > 2)
                for (int k = gPathFirst[cur]; k < gPathFirst[cur + 1]; k++) if (gPathAdj[k] == start) closed = true;
            cur = next;
        }
    }
    if (closed) gPathPts.push_back(gPathPts[0]);   // el lazo se recorre hasta volver al principio
    gPathLen.resize(gPathPts.size());
    float acc = 0.0f;
    for (size_t i = 0; i < gPathPts.size(); i++) {
        if (i) acc += (gPathPts[i] - gPathPts[i - 1]).Length();
        gPathLen[i] = acc;
    }
    return !gPathPts.empty();
}
// punto y tangente (espacio LOCAL de la curva) en la fraccion 'u' (0..1) del LARGO del camino
static void W3dPathEval(float u, Vector3& pt, Vector3& tan){
    const size_t n = gPathPts.size();
    tan = Vector3(0, 0, -1);
    if (n == 0) { pt = Vector3(0, 0, 0); return; }
    if (n == 1) { pt = gPathPts[0]; return; }
    const float total = gPathLen[n - 1];
    if (total <= 1e-8f) { pt = gPathPts[0]; return; }
    if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;
    const float s = u * total;
    size_t i = 1;
    while (i < n - 1 && gPathLen[i] < s) i++;
    const float seg = gPathLen[i] - gPathLen[i - 1];
    const float f = (seg > 1e-8f) ? (s - gPathLen[i - 1]) / seg : 0.0f;
    pt = gPathPts[i - 1] + (gPathPts[i] - gPathPts[i - 1]) * f;
    const Vector3 d = gPathPts[i] - gPathPts[i - 1];
    if (d.LengthSq() > 1e-12f) tan = d.Normalized();
}

bool W3dConsApply(W3dConstraint* c, W3dConsCtx& x){
    if (!c) return false;
    const float t = c->Weight();
    const Matrix4 Mo = x.owner.M;
    Matrix4 R = Mo;
    const bool ax[3]  = { c->axisX, c->axisY, c->axisZ };
    const bool inv[3] = { c->invX, c->invY, c->invZ };
    // el "arriba" del mundo (+Y) en el espacio comun (en un hueso, el del armature puede estar girado)
    const Vector3 worldUp = W3dDir(x.fromWorld, Vector3(0, 1, 0)).Normalized();
    switch (c->type) {
        case W3dConstraintType::CopyLocation: {
            if (!x.target.valid) return false;
            Matrix4 O = W3dToSpace(x, x.owner, c->ownerSpace);
            const Matrix4 T = W3dToSpace(x, x.target, c->targetSpace);
            Vector3 o = W3dTr(O); const Vector3 tt = W3dTr(T);
            for (int k = 0; k < 3; k++) if (ax[k]) {
                const float v = inv[k] ? -tt[k] : tt[k];
                o[k] = c->useOffset ? o[k] + v : v;
            }
            W3dSetTr(O, o);
            R = W3dFromSpace(x, x.owner, O, c->ownerSpace);
        } break;
        case W3dConstraintType::CopyRotation: {
            if (!x.target.valid) return false;
            const Matrix4 O = W3dToSpace(x, x.owner, c->ownerSpace);
            const Matrix4 T = W3dToSpace(x, x.target, c->targetSpace);
            Vector3 ot, os, tt, ts; Quaternion oq, tq;
            if (!W3dDecomp(O, ot, oq, os) || !W3dDecomp(T, tt, tq, ts)) return false;
            const int ord = x.eulerOrder;
            const bool all = ax[0] && ax[1] && ax[2] && !inv[0] && !inv[1] && !inv[2];
            Quaternion qm = tq, nq;
            Vector3 oe(0, 0, 0), te(0, 0, 0);
            if (!all || c->mixMode == 1) {
                oe = W3dEulerFromMatrix(W3dRotMat(oq), ord);
                te = W3dEulerFromMatrix(W3dRotMat(tq), ord);
                for (int k = 0; k < 3; k++) {
                    if (!ax[k]) te[k] = (c->mixMode == 0) ? oe[k] : 0.0f;   // Replace: el eje no copiado queda como estaba
                    else if (inv[k]) te[k] = -te[k];
                }
                if (!W3dMatrixQuat(W3dMatrixFromEuler(te, ord), qm)) return false;
            }
            if (c->mixMode == 1) {          // Add: suma de euler (el "Add" de Blender)
                if (!W3dMatrixQuat(W3dMatrixFromEuler(oe + te, ord), nq)) return false;
            }
            else if (c->mixMode == 2) nq = qm * oq;   // Before: el target ANTES que el original
            else if (c->mixMode == 3) nq = oq * qm;   // After
            else nq = qm;                             // Replace
            R = W3dFromSpace(x, x.owner, W3dLocalTRS(ot, nq, os), c->ownerSpace);
        } break;
        case W3dConstraintType::CopyScale: {
            if (!x.target.valid) return false;
            const Matrix4 O = W3dToSpace(x, x.owner, c->ownerSpace);
            const Matrix4 T = W3dToSpace(x, x.target, c->targetSpace);
            Vector3 ot, os, tt, ts; Quaternion oq, tq;
            if (!W3dDecomp(O, ot, oq, os)) return false;
            tt = W3dTr(T); ts = Vector3(W3dCol(T, 0).Length(), W3dCol(T, 1).Length(), W3dCol(T, 2).Length());
            Vector3 ns = os;
            for (int k = 0; k < 3; k++) if (ax[k]) {
                const float v = (c->power == 1.0f) ? ts[k] : powf(ts[k], c->power);
                ns[k] = !c->useOffset ? v : (c->scaleAdd ? os[k] + v - 1.0f : os[k] * v);
            }
            R = W3dFromSpace(x, x.owner, W3dLocalTRS(ot, oq, ns), c->ownerSpace);
        } break;
        case W3dConstraintType::CopyTransforms: {
            if (!x.target.valid) return false;
            const Matrix4 O = W3dToSpace(x, x.owner, c->ownerSpace);
            const Matrix4 T = W3dToSpace(x, x.target, c->targetSpace);
            const Matrix4 N = (c->mixMode == 2) ? T * O : (c->mixMode == 3) ? O * T : T;
            R = W3dFromSpace(x, x.owner, N, c->ownerSpace);
        } break;
        case W3dConstraintType::ChildOf: {
            // (solo huesos: el Child Of de un OBJETO va por su camino de la fase 1, que reemplaza al padre)
            if (!x.target.valid) return false;
            Matrix4 T = x.target.M;
            W3dChildOfMaskMatrix(c, T);
            R = T * c->inverse * Mo;
        } break;
        case W3dConstraintType::TrackTo:
        case W3dConstraintType::DampedTrack:
        case W3dConstraintType::LockedTrack: {
            if (!x.target.valid) return false;
            Vector3 ot, os; Quaternion oq;
            if (!W3dDecomp(Mo, ot, oq, os)) return false;
            Vector3 d = W3dTr(x.target.M) - ot;
            if (d.LengthSq() < 1e-12f) return false;
            d = d.Normalized();
            Quaternion nq;
            if (c->type == W3dConstraintType::DampedTrack) {
                // el giro MINIMO que lleva el eje al target: nunca pega vueltas (por eso "damped")
                const Vector3 a = (oq * W3dAxisVec(c->trackAxis)).Normalized();
                nq = W3dQuatFromTo(a, d) * oq;
            } else if (c->type == W3dConstraintType::LockedTrack) {
                // gira SOLO alrededor del eje bloqueado: el eje que apunta va a la proyeccion del target
                const Vector3 L = (oq * W3dAxisVec(c->lockAxis)).Normalized();
                const Vector3 a = oq * W3dAxisVec(c->trackAxis);
                const Vector3 dp = d - L * d.Dot(L), ap = a - L * a.Dot(L);
                if (dp.LengthSq() < 1e-10f || ap.LengthSq() < 1e-10f) return false;   // eje = eje bloqueado
                nq = W3dQuatFromTo(ap.Normalized(), dp.Normalized()) * oq;
            } else {
                // Track To: el eje apunta al target y el 'up' queda lo mas arriba posible (el +Y del mundo,
                // o el del target si targetUp: lo de "Target Z" de Blender, con el arriba del motor)
                const Vector3 upRef = c->targetUp ? W3dDir(x.target.M, Vector3(0, 1, 0)).Normalized() : worldUp;
                if (!W3dAimBasis(c->trackAxis, c->upAxis, d, upRef, oq, nq)) return false;
            }
            R = W3dLocalTRS(ot, nq, os);
        } break;
        case W3dConstraintType::StretchTo: {
            if (!x.target.valid) return false;
            Vector3 ot, os; Quaternion oq;
            if (!W3dDecomp(Mo, ot, oq, os)) return false;
            Vector3 d = W3dTr(x.target.M) - ot;
            const float dist = d.Length();
            if (dist < 1e-8f) return false;
            d = d * (1.0f / dist);
            // el largo de REPOSO: el dado; si no, el del hueso; si no (un objeto), la distancia del primer
            // frame, que se ANOTA (como hace Blender con orglength) para que de ahi en mas estire contra eso
            float L0 = c->restLength;
            if (L0 <= 0.0f) {
                if (x.isBone && x.owner.length > 0.0f) L0 = x.owner.length;
                else { c->restLength = dist; L0 = dist; }
            }
            const float sy = dist / L0;
            const float bul = powf(L0 / dist, c->bulge);
            float sx = 1.0f, sz = 1.0f;
            if (c->volumeMode == 0) { sx = sz = sqrtf(bul); }
            else if (c->volumeMode == 1) sx = bul;
            else if (c->volumeMode == 2) sz = bul;
            Quaternion nq;
            const Vector3 yCur = (oq * Vector3(0, 1, 0)).Normalized();
            bool okq = false;
            if (c->keepAxis == 0) {          // Plane X: conserva el X lo mas posible
                const Vector3 xCur = oq * Vector3(1, 0, 0);
                Vector3 z = Vector3::Cross(xCur, d);
                if (z.LengthSq() > 1e-10f) { z = z.Normalized(); okq = W3dBasis(1, 1.0f, d, 2, z, nq); }
            } else if (c->keepAxis == 1) {   // Plane Z
                const Vector3 zCur = oq * Vector3(0, 0, 1);
                Vector3 xx = Vector3::Cross(d, zCur);
                if (xx.LengthSq() > 1e-10f) { xx = xx.Normalized(); okq = W3dBasis(1, 1.0f, d, 0, xx, nq); }
            }
            if (!okq) nq = W3dQuatFromTo(yCur, d) * oq;   // Swing (o el plano quedo degenerado)
            R = W3dLocalTRS(ot, nq, Vector3(os.x * sx, sy, os.z * sz));
        } break;
        case W3dConstraintType::LimitLocation: {
            Matrix4 O = W3dToSpace(x, x.owner, c->ownerSpace);
            Vector3 p = W3dTr(O);
            for (int k = 0; k < 3; k++) {
                if (c->useMin[k] && p[k] < c->vmin[k]) p[k] = c->vmin[k];
                if (c->useMax[k] && p[k] > c->vmax[k]) p[k] = c->vmax[k];
            }
            W3dSetTr(O, p);
            R = W3dFromSpace(x, x.owner, O, c->ownerSpace);
        } break;
        case W3dConstraintType::LimitRotation: {
            const Matrix4 O = W3dToSpace(x, x.owner, c->ownerSpace);
            Vector3 ot, os; Quaternion oq;
            if (!W3dDecomp(O, ot, oq, os)) return false;
            Vector3 e = W3dEulerFromMatrix(W3dRotMat(oq), x.eulerOrder);
            for (int k = 0; k < 3; k++) {
                if (c->useMin[k] && e[k] < c->vmin[k]) e[k] = c->vmin[k];
                if (c->useMax[k] && e[k] > c->vmax[k]) e[k] = c->vmax[k];
            }
            Quaternion nq;
            if (!W3dMatrixQuat(W3dMatrixFromEuler(e, x.eulerOrder), nq)) return false;
            R = W3dFromSpace(x, x.owner, W3dLocalTRS(ot, nq, os), c->ownerSpace);
        } break;
        case W3dConstraintType::LimitScale: {
            const Matrix4 O = W3dToSpace(x, x.owner, c->ownerSpace);
            Vector3 ot, os; Quaternion oq;
            if (!W3dDecomp(O, ot, oq, os)) return false;
            for (int k = 0; k < 3; k++) {
                if (c->useMin[k] && os[k] < c->vmin[k]) os[k] = c->vmin[k];
                if (c->useMax[k] && os[k] > c->vmax[k]) os[k] = c->vmax[k];
            }
            R = W3dFromSpace(x, x.owner, W3dLocalTRS(ot, oq, os), c->ownerSpace);
        } break;
        case W3dConstraintType::LimitDistance: {
            if (!x.target.valid) return false;
            const Vector3 ctr = W3dTr(x.target.M);
            Vector3 p = W3dTr(Mo);
            Vector3 v = p - ctr;
            const float dcur = v.Length();
            if (c->distance <= 0.0f) c->distance = dcur;   // como Blender: la del primer frame
            const float D = c->distance;
            const Vector3 dir = (dcur > 1e-8f) ? v * (1.0f / dcur) : worldUp;
            if (c->distMode == 0)      { if (dcur > D) p = ctr + dir * D; }
            else if (c->distMode == 1) { if (dcur < D) p = ctr + dir * D; }
            else p = ctr + dir * D;
            W3dSetTr(R, p);
        } break;
        case W3dConstraintType::MaintainVolume: {
            Matrix4 O = W3dToSpace(x, x.owner, c->ownerSpace);
            const Vector3 s(W3dCol(O, 0).Length(), W3dCol(O, 1).Length(), W3dCol(O, 2).Length());
            const int fa = c->freeAxis;
            float total = 1.0f;
            if (c->volMode == 0) total = s.x * s.y * s.z;
            else if (c->volMode == 1) total = s[fa] * s[fa] * s[fa];
            else total = s[fa];
            if (fabsf(total) < 1e-12f) return false;
            const float fac = sqrtf(fabsf(c->volume / total));
            for (int k = 0; k < 3; k++) if (k != fa) W3dSetCol(O, k, W3dCol(O, k) * fac);
            R = W3dFromSpace(x, x.owner, O, c->ownerSpace);
        } break;
        case W3dConstraintType::Floor: {
            if (!x.target.valid) return false;
            const int idx = c->trackAxis % 3; const bool neg = (c->trackAxis >= 3);
            Vector3 p = W3dTr(Mo);
            if (c->useRotation) {
                // el piso gira con el target: se compara en el espacio del target
                Matrix4 iT; Matrix4::InvertAffine(x.target.M, iT);
                Vector3 lp = iT * p;
                if (!neg) { if (lp[idx] < c->floorOffset) lp[idx] = c->floorOffset; }
                else      { if (lp[idx] > -c->floorOffset) lp[idx] = -c->floorOffset; }
                p = x.target.M * lp;
            } else {
                // en MUNDO (el piso es horizontal en el mundo aunque el armature este girado)
                Vector3 pw = x.toWorld * p;
                const Vector3 tw = x.toWorld * W3dTr(x.target.M);
                if (!neg) { if (pw[idx] < tw[idx] + c->floorOffset) pw[idx] = tw[idx] + c->floorOffset; }
                else      { if (pw[idx] > tw[idx] - c->floorOffset) pw[idx] = tw[idx] - c->floorOffset; }
                p = x.fromWorld * pw;
            }
            W3dSetTr(R, p);
        } break;
        case W3dConstraintType::Transformation: {
            if (!x.target.valid) return false;
            const Matrix4 T = W3dToSpace(x, x.target, c->targetSpace);
            const Matrix4 O = W3dToSpace(x, x.owner, c->ownerSpace);
            Vector3 tt, ts, ot, os; Quaternion tq, oq;
            if (!W3dDecomp(T, tt, tq, ts) || !W3dDecomp(O, ot, oq, os)) return false;
            const Vector3 src = (c->mapFrom == 0) ? tt : (c->mapFrom == 1) ? W3dEulerFromMatrix(W3dRotMat(tq), x.eulerOrder) : ts;
            Vector3 oe = W3dEulerFromMatrix(W3dRotMat(oq), x.eulerOrder);
            for (int k = 0; k < 3; k++) {
                const int sAx = c->mapAxis[k];
                const float fmin = c->fromMin[sAx], fmax = c->fromMax[sAx];
                float u = (fmax != fmin) ? (src[sAx] - fmin) / (fmax - fmin) : 0.0f;
                if (!c->extrapolate) { if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f; }
                const float out = c->toMin[k] + u * (c->toMax[k] - c->toMin[k]);
                const bool add = (c->mixMode == 1);
                if (c->mapTo == 0)      ot[k] = add ? ot[k] + out : out;
                else if (c->mapTo == 1) oe[k] = add ? oe[k] + out : out;
                else                    os[k] = add ? os[k] * out : out;   // escala: "Add" multiplica
            }
            if (c->mapTo == 1 && !W3dMatrixQuat(W3dMatrixFromEuler(oe, x.eulerOrder), oq)) return false;
            R = W3dFromSpace(x, x.owner, W3dLocalTRS(ot, oq, os), c->ownerSpace);
        } break;
        case W3dConstraintType::Pivot: {
            Vector3 ot, os; Quaternion oq;
            if (!W3dDecomp(Mo, ot, oq, os)) return false;
            if (c->pivotRotAxis >= 0) {
                // pivotea SOLO en un rango de la rotacion (Blender: "Pivot When"): 0-2 = el eje negativo, 3-5 = positivo
                const Vector3 e = W3dEulerFromMatrix(W3dRotMat(oq), x.eulerOrder);
                const int k = c->pivotRotAxis % 3;
                if (c->pivotRotAxis < 3 ? (e[k] > 0.0f) : (e[k] < 0.0f)) return false;
            }
            const Vector3 piv = x.target.valid ? W3dTr(x.target.M) + c->pivotOffset
                              : (c->pivotAbs ? c->pivotOffset : ot + c->pivotOffset);
            W3dSetTr(R, piv + oq * (ot - piv));
        } break;
        case W3dConstraintType::FollowPath: {
            if (!x.targetObj) return false;
            bool closed = false;
            if (!W3dPathBuild(x.targetObj, closed)) return false;
            float u = c->fixedPosition ? c->offsetFactor
                    : ((float)(CurrentFrame - 1) - c->pathOffset) / c->pathDuration;
            if (closed) { u = u - floorf(u); }
            Vector3 pt, tan; W3dPathEval(u, pt, tan);
            Matrix4 P; P.Identity();
            if (c->followCurve) {
                // la tangente es el eje de AVANCE del dueno; el arriba, el +Y de la curva
                Quaternion q;
                if (W3dAimBasis(c->trackAxis, c->upAxis, tan, Vector3(0, 1, 0), Quaternion(), q)) P = W3dRotMat(q);
            }
            W3dSetTr(P, pt);
            // como Blender: mundo' = curva * camino * mundo del dueno (el pos propio queda como OFFSET sobre
            // el camino), conservando la escala del dueno
            Matrix4 N = x.targetObjM * P * Mo;
            const Vector3 os(W3dCol(Mo, 0).Length(), W3dCol(Mo, 1).Length(), W3dCol(Mo, 2).Length());
            for (int k = 0; k < 3; k++) { const float l = W3dCol(N, k).Length(); if (l > 1e-12f) W3dSetCol(N, k, W3dCol(N, k) * (os[k] / l)); }
            R = N;
        } break;
        case W3dConstraintType::ClampTo: {
            if (!x.targetObj) return false;
            bool closed = false;
            if (!W3dPathBuild(x.targetObj, closed)) return false;
            Matrix4 iC; Matrix4::InvertAffine(x.targetObjM, iC);
            const Vector3 lp = iC * W3dTr(Mo);   // el dueno en el espacio de la curva
            Vector3 mn = gPathPts[0], mx = gPathPts[0];
            for (size_t i = 1; i < gPathPts.size(); i++) for (int k = 0; k < 3; k++) {
                if (gPathPts[i][k] < mn[k]) mn[k] = gPathPts[i][k];
                if (gPathPts[i][k] > mx[k]) mx[k] = gPathPts[i][k];
            }
            int axc = c->clampAxis - 1;
            if (axc < 0) { axc = 0; for (int k = 1; k < 3; k++) if (mx[k] - mn[k] > mx[axc] - mn[axc]) axc = k; }
            const float len = mx[axc] - mn[axc];
            if (len < 1e-8f) return false;
            float u = (lp[axc] - mn[axc]) / len;
            if (c->cyclic) u = u - floorf(u);
            Vector3 pt, tan; W3dPathEval(u, pt, tan);
            W3dSetTr(R, x.targetObjM * pt);
        } break;
        default:
            return false;   // Billboard / IK no van por aca
    }
    x.owner.M = (t >= 1.0f) ? R : W3dConsBlend(Mo, R, t);
    return true;
}

// ---- el TARGET de un constraint generico cuyo dueno es un OBJETO (en MUNDO) ----
static bool W3dConsTargetForObject(W3dConstraint* c, W3dConsCtx& x){
    x.target.valid = false; x.targetObj = NULL; x.targetObjM.Identity();
    if (c->sourceType == W3dConstraintSource::View) {
        // la vista: una matriz con la posicion y la orientacion de la camara bindeada (sin escala)
        x.target.M = W3dLocalTRS(g_renderCamPos, g_renderCamRot, Vector3(1, 1, 1));
        x.target.rest.Identity(); x.target.length = 0.0f; x.target.valid = true;
        return true;
    }
    Object* f = c->sourceObj;
    if (!f) return false;
    Matrix4 W; f->GetWorldMatrix(W);         // recursivo: la fuente llega con SUS constraints
    x.targetObj = f; x.targetObjM = W;
    x.target.M = W; x.target.length = 0.0f;
    if (f->Parent) f->Parent->GetWorldMatrix(x.target.rest); else x.target.rest.Identity();
    if (!c->bone.empty() && f->getType() == ObjectType::armature) {
        Armature* a = (Armature*)f;
        EvaluatePoseSkeleton(a, CurrentFrame);          // cacheado por frame: gratis si ya se dibujo
        const int h = W3dChildOfBone(c, a);
        Matrix4 P, B; float len = 0.0f;
        if (h >= 0 && W3dBoneFrameMatrix(a, h, P, &B, &len)) {
            x.target.M = W * P; x.target.rest = W * B; x.target.length = len;
            if (c->headTail > 0.0f) W3dSetTr(x.target.M, x.target.M * Vector3(0, len * c->headTail, 0));
        }
    }
    x.target.valid = true;
    return true;
}

// un constraint de OBJETO va por la matriz generica? Los 4 tipos de la fase 1 siguen por su camino de
// siempre (resultados BIT A BIT iguales a los de antes) salvo un Copy Location/Rotation que use una opcion
// de la fase 2 (espacios, invertir, offset, mezcla, un hueso de la fuente): esos van por la generica.
static bool W3dConsGenericForObject(const W3dConstraint* c){
    switch (c->type) {
        case W3dConstraintType::Billboard: case W3dConstraintType::ChildOf: return false;
        case W3dConstraintType::CopyLocation: case W3dConstraintType::CopyRotation:
            return c->ownerSpace != W3dConstraintSpace::World || c->targetSpace != W3dConstraintSpace::World ||
                   c->invX || c->invY || c->invZ || c->useOffset || c->mixMode != 0 || !c->bone.empty();
        default: return true;
    }
}

// contador de DIAGNOSTICO: cuantas veces corrio el CUERPO del evaluador (o sea, las que pasaron
// el pre-pase y realmente hicieron cuentas). No lo lee nadie del motor; existe para que un test
// pueda afirmar que una consulta sobre una cadena de N objetos con constraint cuesta O(N) y no
// O(2^N), que es lo que costaba cuando cada eslabon volvia a pedir el mundo de su padre.
unsigned long g_consEvalRuns = 0;

// ============================================================================
//  W3dEvaluateConstraints: EL CORAZON (contrato completo en Objects.h)
//
//  Todo el trabajo se hace en MUNDO, porque ahi es donde estan la fuente y la camara:
//  se sube la transform del objeto al espacio de mundo, se corre el stack, y se baja de
//  vuelta al espacio del padre. Sin padre no hay ni subida ni bajada.
//
//  'WpIn' es la matriz de MUNDO DEL PADRE ya calculada, o NULL si el que llama no la tiene.
//  Es lo unico que separa esta evaluacion de una EXPONENCIAL: ver el comentario grande de
//  Object::GetWorldMatrix.
// ============================================================================
static bool W3dEvalCons(const Object* o, const Matrix4* WpIn, Vector3& posOut, Quaternion& rotOut, Vector3* escOut = NULL){
    if (!o) return false;

    // ---- 1. GUARDIA DE CICLOS ----
    // segunda entrada al MISMO objeto (A copia de B y B copia de A): devolver la base corta
    // la recursion en O(1), sin allocar y sin importar cuantos objetos haya en el ciclo. Es
    // determinista: el que se pregunta primero es el que termina con su propia base.
    if (o->consEval) return false;

    // ---- 2. EL PRE-PASE BARATO ----
    // si NO hay ningun constraint efectivo se sale ACA, antes de tocar la matriz del padre.
    // Esto es lo que hace que "apagado" o "influencia 0" devuelva la transform base BIT A BIT
    // incluso con padre: sin el pre-pase habria una ida mundo -> InvertAffine -> local que
    // ensucia el ultimo decimal, y apagar un constraint moveria el objeto un epsilon.
    bool hasAny = false;
    for (size_t i = 0; i < o->constraints.size(); i++)
        if (W3dConstraintEffective(o->constraints[i], o)) { hasAny = true; break; }
    if (!hasAny) return false;

    // ---- 3. de aca en adelante hay UN SOLO punto de salida (el consEval = false del final) ----
    o->consEval = true;
    g_consEvalRuns++;        // solo diagnostico (ver arriba)

    // ---- 4. el espacio del PADRE ----
    const bool hasParent = (o->Parent != NULL);
    Matrix4 Wp;
    Quaternion qWp;              // identidad por su propio ctor
    bool baseOk = true;          // false = el padre esta espejado: no hay rotacion que componer
    if (hasParent) {
        // si el que llama viene BAJANDO la cadena ya lo tiene calculado y lo pasa: eso es lo que
        // hace que la cuenta sea lineal. El que entra suelto (GetMatrix de un objeto cualquiera)
        // lo pide una sola vez, que ya es lo mismo que hacia antes.
        if (WpIn) Wp = *WpIn;
        else      o->Parent->GetWorldMatrix(Wp);   // recursivo: el padre llega con SUS constraints
        baseOk = W3dMatrixQuat(Wp, qWp);
        if (!baseOk && !gNoticeBaseMirrored) {
            gNoticeBaseMirrored = true;
            w3dLogfW("constraints: '%s' cuelga de una base espejada o de escala cero; sus constraints de ROTACION se saltean (la posicion se aplica igual)",
                     o->name.c_str());
        }
    }

    // ---- 5. la transform del objeto, llevada a MUNDO ----
    Vector3    pw = hasParent ? (Wp * o->pos)      : o->pos;
    Quaternion qw = hasParent ? (qWp * o->Rot())   : o->Rot();
    // ESCALA en mundo: solo la toca el Child Of. Mientras no la toque se devuelve o->scale BIT A BIT.
    const Vector3 ep = hasParent ? Vector3(W3dLengthCol(Wp, 0), W3dLengthCol(Wp, 1), W3dLengthCol(Wp, 2)) : Vector3(1, 1, 1);
    Vector3    sw(o->scale.x * ep.x, o->scale.y * ep.y, o->scale.z * ep.z);
    bool       touchedScale = false;

    // ---- 6. EL STACK, en orden (como los modificadores de la malla) ----
    for (size_t i = 0; i < o->constraints.size(); i++) {
        W3dConstraint* c = o->constraints[i];
        if (!W3dConstraintEffective(c, o)) continue;
        const float t = c->Weight();

        // el que necesita la vista y no la tiene usa el default (mirando al -Z) y avisa: "no
        // hay camara" no es lo mismo que "la camara mira para alla", y sin el aviso el juego
        // que se olvido de bindear ve el billboard clavado y no tiene por donde empezar.
        if (!g_viewBound && !gNoticeNoView &&
            (c->type == W3dConstraintType::Billboard || c->sourceType == W3dConstraintSource::View)) {
            gNoticeNoView = true;
            w3dLogfW("constraints: nadie llamo a W3dVistaBind todavia; '%s' usa la vista por defecto (mirando al -Z)",
                     o->name.c_str());
        }

        // la fuente esta a mitad de SU propia evaluacion, o sea que se llego hasta aca desde
        // ella: hay un ciclo. La guardia de arriba ya hace que esto TERMINE, pero hay que
        // decirlo, y una sola vez por constraint: esto corre por vista y por frame. El flag lo
        // resetea el panel al elegir otra fuente (ActionConstraintPickedSource, ui/ViewPorts/
        // Properties.cpp), asi que romper el ciclo y volver a armarlo vuelve a avisar.
        if (c->sourceObj && c->sourceObj->consEval && !c->noticeCycle) {
            c->noticeCycle = true;
            w3dLogfW("constraints: ciclo entre '%s' y '%s' (constraint '%s'); se usa la transform base del otro",
                     o->name.c_str(), c->sourceObj->name.c_str(), c->name.c_str());
        }

        // ---- LOS TIPOS DE LA FASE 2 (Copy Scale .. Clamp To): por la matriz generica, en MUNDO ----
        if (W3dConsGenericForObject(c)) {
            if (!baseOk) continue;                      // padre espejado: no hay orientacion que componer
            W3dConsCtx x;
            x.isBone = false; x.eulerOrder = 5;         // el euler de un objeto es Rx*Ry*Rz (Quaternion::FromEulerXYZ)
            x.owner.M = W3dLocalTRS(pw, qw, sw);
            if (hasParent) x.owner.rest = Wp; else x.owner.rest.Identity();
            x.owner.valid = true;
            W3dConsTargetForObject(c, x);              // (los que no usan target siguen igual sin el)
            if (!W3dConsApply(c, x)) continue;
            Vector3 pn, sn; Quaternion qn;
            if (!W3dDecomp(x.owner.M, pn, qn, sn)) continue;
            pw = pn; qw = qn;
            // la ESCALA se toma solo si el constraint la cambio de verdad: la ida y vuelta por la matriz
            // ensucia el ultimo decimal y un Track To no tiene por que mover la escala del objeto
            if (fabsf(sn.x - sw.x) > 1e-5f * (fabsf(sw.x) + 1e-6f) || fabsf(sn.y - sw.y) > 1e-5f * (fabsf(sw.y) + 1e-6f) ||
                fabsf(sn.z - sw.z) > 1e-5f * (fabsf(sw.z) + 1e-6f)) { sw = sn; touchedScale = true; }
            continue;
        }

        if (c->type == W3dConstraintType::CopyLocation) {
            // ---- COPY LOCATION: LERP en MUNDO ----
            // lerp y no otra cosa: la posicion vive en un espacio AFIN y ahi el camino recto
            // ES el correcto (al reves que la rotacion, que va por el arco).
            const Vector3 fw = (c->sourceType == W3dConstraintSource::View)
                             ? g_renderCamPos : c->sourceObj->GetGlobalPosition();
            Vector3 obj = pw;
            if (c->axisX) obj.x = fw.x;
            if (c->axisY) obj.y = fw.y;
            if (c->axisZ) obj.z = fw.z;
            pw = (t >= 1.0f) ? obj : (pw + (obj - pw) * t);   // con t=1 ASIGNA, no interpola
        }
        else if (c->type == W3dConstraintType::CopyRotation) {
            if (!baseOk) continue;                      // padre espejado: ya se aviso arriba
            Quaternion qf;
            if (c->sourceType == W3dConstraintSource::View) qf = g_renderCamRot;
            else if (!W3dQuatWorldOf(c->sourceObj, qf))  continue;   // la fuente esta espejada
            Quaternion qObj;
            if (c->axisX && c->axisY && c->axisZ) {
                qObj = qf;   // ATAJO de los 3 ejes: es "la misma orientacion", sin ida y vuelta por euler
            } else {
                // la MASCARA por eje solo existe en euler: "copiar la X" ES un angulo.
                Vector3 e = qw.ToEulerXYZ(), ef = qf.ToEulerXYZ();
                if (c->axisX) e.x = ef.x;
                if (c->axisY) e.y = ef.y;
                if (c->axisZ) e.z = ef.z;
                qObj = Quaternion::FromEulerXYZ(e.x, e.y, e.z);
            }
            // la INFLUENCIA se mezcla en QUATERNION: arco corto y extremos exactos.
            // Semantica que hay que conocer y que es la correcta: "350 grados al 50%" da 355 y
            // no 175, porque un quaternion codifica una ORIENTACION, no vueltas.
            qw = Quaternion::Slerp(qw, qObj, t);
        }
        else if (c->type == W3dConstraintType::ChildOf) {
            // ---- CHILD OF: mundo = fuente * inversa * LOCAL (la fuente REEMPLAZA al padre) ----
            // el local es el del objeto con lo que los constraints de arriba ya le hicieron
            Matrix4 Mt;
            W3dChildOfMatrix(c, (hasParent && c->sourceObj == o->Parent) ? &Wp : NULL, Mt);
            Matrix4 L = W3dLocalTRS(pw, qw, sw);                 // en MUNDO...
            if (hasParent) { Matrix4 iWp; Matrix4::InvertAffine(Wp, iWp); L = iWp * L; } // ...al espacio del padre
            const Matrix4 W = Mt * c->inverse * L;
            Quaternion qn;
            if (!W3dMatrixQuat(W, qn)) continue;               // fuente espejada / escala cero
            const Vector3 pn(W.m[12], W.m[13], W.m[14]);
            const Vector3 sn(W3dLengthCol(W, 0), W3dLengthCol(W, 1), W3dLengthCol(W, 2));
            if (t >= 1.0f) { pw = pn; qw = qn; sw = sn; }        // con t=1 ASIGNA, no interpola
            else {
                pw = pw + (pn - pw) * t;
                qw = Quaternion::Slerp(qw, qn, t);
                sw = sw + (sn - sw) * t;
            }
            touchedScale = true;
        }
        else if (c->type == W3dConstraintType::Billboard) {
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
                const float p = e.x + W3dShortDifAng(-pitchObj, e.x) * t;
                const float y = e.y + W3dShortDifAng( yawObj,   e.y) * t;
                const float r = e.z + W3dShortDifAng( 0.0f,     e.z) * t;
                qw = Quaternion::FromEulerYXZ(p, y, r);
            }
        }
    }

    // ---- 7. de vuelta al espacio del PADRE ----
    if (hasParent) {
        Matrix4 iWp;
        // si Wp fuera singular InvertAffine deja iWp = Wp: una matriz usable en vez de basura
        // (y con escala cero no hay local que devuelva esa posicion de mundo, de todas formas).
        Matrix4::InvertAffine(Wp, iWp);
        posOut = iWp * pw;
        rotOut = qWp.Inverted() * qw;   // qWp sale de W3dMatrixQuat: unitario -> inversa = conjugado
    } else {
        posOut = pw;
        rotOut = qw;
    }
    if (escOut) *escOut = !touchedScale ? o->scale
        : Vector3(ep.x > 1e-8f ? sw.x / ep.x : sw.x, ep.y > 1e-8f ? sw.y / ep.y : sw.y, ep.z > 1e-8f ? sw.z / ep.z : sw.z);

    o->consEval = false;
    return true;
}

// la puerta publica (contrato en Objects.h): el que pregunta suelto no tiene el mundo del padre.
bool W3dEvaluateConstraints(const Object* o, Vector3& posOut, Quaternion& rotOut){
    return W3dEvalCons(o, NULL, posOut, rotOut);
}

// matriz LOCAL efectiva de 'o' con el mundo del padre YA calculado ('Wp' = NULL si es top-level
// o si el que llama no lo tiene). Es GetMatrix() con ese dato de mas: los dos early-outs son
// IDENTICOS a los de GetMatrix a proposito, para que "objeto sin constraints" pese lo mismo se
// entre por donde se entre.
static void W3dEffectiveLocal(const Object* o, const Matrix4* Wp, Matrix4& out){
    if (o->constraints.empty()) { o->GetMatrixBase(out); return; }
    Vector3 p, e;
    Quaternion q;
    if (!W3dEvalCons(o, Wp, p, q, &e)) { o->GetMatrixBase(out); return; }
    out = W3dLocalTRS(p, q, e);          // la ESCALA solo la toca el Child Of (sino es o->scale tal cual)
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
//  una cuenta, y "constraint apagado" da la base BIT A BIT (ver W3dEvaluateConstraints).
// ============================================================================
void Object::GetMatrix(Matrix4& out) const {
    // NULL = "no te traigo el mundo del padre": el que entra por aca no lo tiene, asi que si hace
    // falta el evaluador lo pide una vez. El que SI lo tiene (GetWorldMatrix, bajando la cadena)
    // llama derecho a W3dEffectiveLocal y por eso no paga la cuenta exponencial.
    W3dEffectiveLocal(this, NULL, out);
}

void Object::RotateLocal(float pitch, float yaw, float roll){
    Quaternion qPitch = Quaternion::FromAxisAngle(1,0,0, pitch);
    Quaternion qYaw   = Quaternion::FromAxisAngle(0,1,0, yaw);
    Quaternion qRoll  = Quaternion::FromAxisAngle(0,0,1, roll);

    // por la puerta: rotar tiene que dejar el euler/axis-angle al dia o lo que se guarda no es lo que se ve
    SetRot(rot * qYaw * qPitch * qRoll);
}

// matriz de MUNDO del objeto: su matriz LOCAL (T*R*S) por toda la cadena de
// padres. Fuente UNICA de la transform de mundo (foco, LocalToWorld, color-pick).
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
    W3dEffectiveLocal(this, &Wp, L); // ...y aca se le PASA, en vez de que lo vuelva a pedir
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
Vector3 Object::LocalToWorld(const Vector3& local) const {
    Matrix4 M;
    GetWorldMatrix(M);
    return M * local;
}

// idem con la matriz de mundo BASE: es la que usa el que EXPORTA vertices (ver Objects.h)
Vector3 Object::LocalToWorldBase(const Vector3& local) const {
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

// algun hijo visible dibuja algo (el mismo o alguno de los suyos)
static bool ChildrenDraw(Object* o) {
    for (size_t c = 0; c < o->Childrens.size(); c++) {
        Object* h = o->Childrens[c];
        if (h->visible && (h->DrawsSomething() || ChildrenDraw(h))) return true;
    }
    return false;
}

// Funcion recursiva para renderizar un objeto y sus hijos
void Object::Render(){
    if (!visible) return;
#ifndef W3D_SYMBIAN
    if (w3dEngine::g_traceGLSec) printf("      [glsec] -- %s\n", name.c_str());   // (harness) la traza en orden: de quien es cada llamada
#endif
    // LOCAL VIEW: si un viewport esta aislando, solo se traversa/dibuja lo que esta en el set (seleccion +
    // descendencia). Los PADRES de lo aislado (g_localViewStep) se atraviesan para ALCANZAR a los seleccionados con su
    // matriz, pero no se dibujan: el armature de una malla elegida no tiene que aparecer. Fuera del editor: false -> gratis.
    // (las RUTINAS son pasos del render -limpiar la pantalla, la camara, la niebla-, no cosas de la escena: siguen)
    if (g_localViewActive && !noTransform && W3dViewLocalHidden(this)) return;
    const bool onlyStep = g_localViewActive && !noTransform && W3dViewLocalOnlyStep(this);
    // nada que dibujar en todo el subarbol visible (los vacios que mueve un script y que dibuja una rutina, o con todo lo
    // de adentro oculto): ni push, ni mult, ni pop
    if ((onlyStep || !DrawsSomething()) && !ChildrenDraw(this)) return;
    // sin transformacion propia (la Rutina): ni push, ni mult, ni pop (3 llamadas GL menos por objeto)
    if (noTransform) {
        // (el CONSTRUCTOR ya corrido en modo solo-juego: su estado se repuso. Lo que cuelga de el tambien es de UNA vez
        //  -su estado ya esta en la foto-: no se recorre. Lo de cada cuadro, como limpiar la pantalla, va afuera)
        if (SkipThisFrame()) return;
        RenderObject();
        if (!DrawsItsChildren()) RenderChildren();
        AfterRender();
        return;
    }
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
    static int gOpenMirrors = 0;
    const bool espeja = (scale.x * scale.y * scale.z) < 0.0f;
    if (espeja) { gOpenMirrors++; w3dEngine::FrontFace((gOpenMirrors & 1) == 0); }

    // Si es visible y no es un mesh, lo dibuja (el padre de lo aislado en la vista local, no)
    if (!onlyStep) RenderObject();

    // Procesar cada hijo (virtual: LOD/Culling deciden cuales y en que orden)
    RenderChildren();

    if (espeja) { gOpenMirrors--; w3dEngine::FrontFace((gOpenMirrors & 1) == 0); }

    // Restaurar la matriz previa
    w3dEngine::PopMatrix();
}

// default del traversal: TODOS los hijos, en su orden del arbol
void Object::RenderChildren(){
    for (size_t c = 0; c < Childrens.size(); c++) {
        Childrens[c]->Render();
    }
}

// nombre LEGIBLE del tipo (ver Objects.h). Un switch a proposito (sin tabla indexada por el enum):
// si manana se agrega un ObjectType, el compilador avisa del caso faltante en vez de devolver basura.
const char* W3dTypeName(ObjectType t){
    switch (t.v){
        case ObjectType::scene:      return "scene";
        case ObjectType::mesh:       return "mesh";
        case ObjectType::camera:     return "camera";
        case ObjectType::light:      return "light";
        case ObjectType::empty:      return "empty";
        case ObjectType::armature:   return "armature";
        case ObjectType::curve:      return "curve";
        case ObjectType::collection: return "collection";
        case ObjectType::baseObject: return "object";
        case ObjectType::mirror:     return "mirror";
        case ObjectType::script:     return "script";
        case ObjectType::instance:   return "instance";
        case ObjectType::ui:         return "ui";
        case ObjectType::text2d:    return "text";
        case ObjectType::image2d:   return "image";
        case ObjectType::rect2d:     return "rect";
        case ObjectType::cont2d:     return "container";
        case ObjectType::slice9:     return "slice9";
        case ObjectType::button2d:    return "button";
        case ObjectType::expand2d: return "expand";
        case ObjectType::video2d:    return "video";
        case ObjectType::lod:        return "lod";
        case ObjectType::culling:    return "culling";
        case ObjectType::particles: return "particles";
        case ObjectType::fog:     return "fog";
        case ObjectType::clearz:   return "clearz";
        case ObjectType::clip:    return "clip";
        case ObjectType::hitbox:     return "hitbox";
        case ObjectType::prefab:     return "prefab";
        case ObjectType::proxy:      return "proxy";
        case ObjectType::routine:     return "routine";
        case ObjectType::sound:     return "sound";
        case ObjectType::postProcess: return "postProcess";
        case ObjectType::lightProbe:   return "sondaluz";
        case ObjectType::reflectionProbe: return "reflectionprobe";
    }
    return "object";
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
// Collection, Instance, Mirror, ObjectScript, Curve, Camera) estaban MUERTOS. Solo usa
// Light (TurnOffChildLights) + el core-bound Mesh.
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
Quaternion RotGlobalOf(Object* o) {
    if (!o) return Quaternion(1, 0, 0, 0);
    if (!o->Parent) return o->Rot();
    return RotGlobalOf(o->Parent) * o->Rot();
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
static Object* ParentInTree(Object* o) {
    if (!o || o == SceneCollection) return NULL;
    return o->Parent ? o->Parent : SceneCollection;
}

Object* GetPrevDFS(Object* current){
    if (!current) return NULL;

    Object* parent = ParentInTree(current);

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
        Object* parent = ParentInTree(node);

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
                std::cout << "Object: " << node->name << "\n";
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

bool IsSelectable(Object* obj, bool IncludeCollections) {
    if (!obj) return false;

    // La RAIZ nunca: no es una fila del outliner y no se selecciona a mano. Con
    // IncludeCollections=true pasaba el filtro (reporta tipo collection) y GetPrevDFS
    // la devuelve al subir desde el primer hijo -> la flecha "arriba" se clavaba ahi.
    if (obj == SceneCollection) return false;

    const ObjectType t = obj->getType();

    // Si NO queremos incluir colecciones → bloquearlas
    if (!IncludeCollections) {
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
        if (!p->expanded) return false;
    // (la raiz no entra: su 'desplegado' no se dibuja ni se puede togglear)

    return true;
}

Object* GetFirstDFS(){
    if (!SceneCollection) return NULL;
    if (SceneCollection->Childrens.empty()) return NULL;
    return SceneCollection->Childrens[0];
}

void changeSelect(SelectMode mode, bool IncludeCollections){
    if (InteractionMode != ObjectMode) return;
    if (state != editNavigation) return;
    if (SceneCollection->Childrens.empty()) return;

    //std::cout << "changeSelect " << ActiveObj << std::endl;
    //std::cout << "Seleccionados: " << ObjSelects.size() << std::endl;
    //std::cout << "IncludeCollections: " << IncludeCollections << std::endl;

    // Si no hay activo → elegir el primero DFS
    if (!ActiveObj){
        if (SceneCollection->Childrens.empty()){
            std::cout << "no hay objetos para seleccionar" << std::endl;
            return;
        }
        Object* it = SceneCollection->Childrens[0];

        while(it && !IsSelectable(it, IncludeCollections))
            it = GetNextDFS(it);

        if (it){
            it->Select();
        }
        else {
            std::cout << "los objetos no eran seleccionables" << std::endl;
        }
        return;
    }

    Object* next = NULL;

    // elegir next o previous según modo
    if (mode == SelectMode::NextSingle || mode == SelectMode::NextAdd){
        next = GetNextDFS(ActiveObj);
    }
    else if (mode == SelectMode::PrevSingle || mode == SelectMode::PrevAdd){
        next = GetPrevDFS(ActiveObj);
    }

    const bool towardsBack = (mode == SelectMode::PrevSingle || mode == SelectMode::PrevAdd);

    if (!next) {
        // Llegamos a la punta del DFS → wrap. Hacia ADELANTE se vuelve al primero;
        // hacia ATRAS se va al ULTIMO (el nodo mas profundo de la ultima rama). Antes
        // el wrap era SIEMPRE GetFirstDFS(): con la flecha arriba en el tope no se movia.
        next = towardsBack ? GetDeepestDFS(SceneCollection->Childrens.back()) : GetFirstDFS();
        if (!next) return;
    }

    // Buscar un selectable EN LA MISMA DIRECCION en la que veniamos. El ternario viejo
    // solo contemplaba PrevAdd: con PrevSingle -que es el que dispara la flecha ARRIBA-
    // el salteo caminaba para ADELANTE y la flecha terminaba yendo al reves.
    // El tope de vueltas es la red de seguridad: GetPrevDFS CICLA (del primer hijo de la
    // raiz vuelve al ultimo), asi que si no hubiera ni un objeto navegable -todo plegado-
    // este while no terminaria nunca.
    Object* it = next;
    int turns = 0;
    const int limit = 100000;
    while(it && !IsSelectable(it, IncludeCollections)){
        if (++turns > limit) return;
        it = towardsBack ? GetPrevDFS(it) : GetNextDFS(it);
        if (it == next) return;   // dio la vuelta entera sin encontrar nada
    }

    if (!it) return;

    // ------------------------
    // Aplicar modo de selección
    // ------------------------
    if (mode == SelectMode::NextSingle || mode == SelectMode::PrevSingle){
        ActiveObj->Deselect();
    }

    it->Select();
}

void TurnOffChildLights(Object* obj){
#ifndef W3D_SYMBIAN
    // (Light.h todavia no esta portado al dialecto C++03; cuando lo este,
    // esta guarda vuela y Symbian apaga luces igual que PC)
    //si es una luz. la apaga
    if (obj->getType() == ObjectType::light) {
        Light* light = dynamic_cast<Light*>(obj);
        if (light) {
            w3dEngine::SetLightEnabled(light->LightID, false);
        }
    }
#endif
    //lo mismo con los hijos
    for(size_t o=0; o < obj->Childrens.size(); o++){
        TurnOffChildLights(obj->Childrens[o]);
    }
}

void SetExpanded(bool expanded){
    if (SceneCollection && ActiveObj){
        ActiveObj->expanded = expanded;
    }
}

void ChangeVisibilityObj(){
    if (InteractionMode == ObjectMode && state == editNavigation && SceneCollection && ActiveObj){
        ActiveObj->visible = !ActiveObj->visible;
        //apagar luces en caso de que era una luz o sus hijas eran luces
        if (!ActiveObj->visible && w3dRenderLights) TurnOffChildLights(ActiveObj);
    }
}

// visibilidad de RENDER del objeto activo (distinta de 'visible', que oculta en el EDITOR): tecla '3' del
// outliner en el N95. Mismo gate que ChangeVisibilityObj (object mode, navegando).
void ChangeRenderableObj(){
    if (InteractionMode == ObjectMode && state == editNavigation && SceneCollection && ActiveObj){
        ActiveObj->renderable = !ActiveObj->renderable;
    }
}

// FLECHA IZQUIERDA en el outliner: si el activo esta desplegado y tiene hijos, lo pliega; si YA esta plegado
// (o es hoja) y tiene padre, pliega al PADRE y lo selecciona (sube un nivel). Antes solo hacia desplegado=false,
// asi que re-apretar izquierda sobre un hijo ya plegado no hacia nada.
void OutlinerCollapseLeft(){
    if (!(SceneCollection && ActiveObj)) return;
    if (ActiveObj->expanded && !ActiveObj->Childrens.empty()){
        ActiveObj->expanded = false;
        return;
    }
    Object* p = ActiveObj->Parent;
    if (p && p != SceneCollection){
        p->expanded = false;
        DeselectAll();
        p->Select();
    }
}

void DeselectAll(bool IncludeCollections){
	// NO cambiar la seleccion durante un transform: objectState apunta a los
	// seleccionados y si cambia mientras se transforma, crashea.
	if (state != editNavigation) return;
	if (InteractionMode == EditMode && g_editMesh){
        g_editMesh->EditSelectAll(false); // edit: deseleccionar todos los sub-elementos
	} else if (InteractionMode == PoseMode && ActiveObj && ActiveObj->getType() == ObjectType::armature){
        Armature* a = (Armature*)ActiveObj; // Pose Mode: None deselecciona TODOS los huesos (+ sin hueso activo)
        for (size_t b = 0; b < a->bones.size(); b++) a->bones[b].select = false;
        a->activeBone = -1; // sino el hueso activo seguia resaltado (parecia seleccionado)
	} else if (InteractionMode == ObjectMode && SceneCollection){
        ObjSelects.clear();
        SceneCollection->DeselectComplete(IncludeCollections);
	}
}

void SelectAll(bool IncludeCollections){
    //recorre las colecciones y selecciona todo. si llega a encontrar algo hace lo contrario. deselecciona todo
	if (state != editNavigation) return; // no durante un transform
	if (InteractionMode == EditMode && g_editMesh){
        g_editMesh->EditSelectAll(true); // edit: seleccionar todos los sub-elementos
        return;
    }
	if (InteractionMode == ObjectMode && SceneCollection){
        ObjSelects.clear();
        //habia algo seleccionado... asi que hacemos lo contrario. deseleccionar todo
        if (SceneCollection->SelectComplete(IncludeCollections)){
            //std::cout << "habia algo seleccionado! se deselecciona todo\n";
            DeselectAll(IncludeCollections);
            return;
        }
        //std::cout << "Todos los objetos seleccionados\n";
    }
}

// selecciona TODO siempre (a diferencia de SelectAll que togglea): primero
// deselecciona, asi SelectComplete no corta por "ya habia algo seleccionado"
void SelectAllForced(bool IncludeCollections){
    if (state != editNavigation) return; // no durante un transform
    if (InteractionMode == EditMode && g_editMesh){
        g_editMesh->EditSelectAll(true); // edit: seleccionar todos los sub-elementos
    } else if (InteractionMode == PoseMode && ActiveObj && ActiveObj->getType() == ObjectType::armature){
        Armature* a = (Armature*)ActiveObj; // Pose Mode: A selecciona TODOS los huesos (antes no hacia nada)
        for (size_t b = 0; b < a->bones.size(); b++) a->bones[b].select = true;
    } else if (InteractionMode == ObjectMode && SceneCollection){
        DeselectAll(IncludeCollections);
        SceneCollection->SelectComplete(IncludeCollections);
    }
}

// invierte la seleccion: lo seleccionado pasa a no, lo no seleccionado pasa a si
void InvertSelection(bool IncludeCollections){
    if (state != editNavigation) return; // no durante un transform
    if (InteractionMode == EditMode && g_editMesh){
        g_editMesh->EditInvert(); // edit: invertir la seleccion de sub-elementos
    } else if (InteractionMode == PoseMode && ActiveObj && ActiveObj->getType() == ObjectType::armature){
        Armature* a = (Armature*)ActiveObj; // Pose Mode: Invert togglea el select de cada hueso
        for (size_t b = 0; b < a->bones.size(); b++) a->bones[b].select = !a->bones[b].select;
        if (a->activeBone >= 0 && a->activeBone < (int)a->bones.size() && !a->bones[a->activeBone].select) a->activeBone = -1; // el activo quedo deseleccionado
    } else if (InteractionMode == ObjectMode && SceneCollection){
        ObjSelects.clear();
        SceneCollection->InvertCompleteSelection(IncludeCollections);
    }
}

//si hay objetos seleccionados, devuelve true
bool HasObjectsSelected(bool IncludeCollections){
    return SceneCollection && SceneCollection->IsSelected(IncludeCollections);
}