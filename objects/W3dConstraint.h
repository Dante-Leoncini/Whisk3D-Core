#ifndef W3D_CONSTRAINT_H
#define W3D_CONSTRAINT_H

#include <string>
#include <vector>
#include "math/Vector3.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"

// ============================================================================
//  UN CONSTRAINT ES UNA PROPIEDAD DEL OBJETO, NO UN OBJETO
//
//  Antes "el constraint" era un nodo mas del arbol (main/objects/Constraint.h) que
//  apuntaba al objeto que queria modificar y le ESCRIBIA la rotacion en el render.
//  Escribir el campo real es lo que hacia que el efecto se HORNEARA: el .w3d guarda
//  'rotEuler', asi que guardar despues de dibujar dejaba la orientacion de la ultima
//  camara metida adentro del archivo. Y con dos viewports abiertos ganaba el ultimo
//  que dibujo.
//
//  Ahora el constraint VIVE EN el objeto que modifica y el puntero es la FUENTE. 
//  No escribe nada: se
//  evalua al vuelo dentro de Object::GetMatrix, o sea POR VISTA, y los campos
//  pos/rot/rotEuler/scale del objeto quedan siempre como el usuario los dejo. Lo que
//  hornea geometria o escribe a disco usa la variante *Base, que ni mira esta lista.
//
//  NO es thread-safe (la guardia de ciclos 'consEval' es un bool por objeto). Hoy el
//  render es monohilo; si alguna vez deja de serlo, esto se replantea.
// ============================================================================

class Object;   // solo forward: la fuente es un Object*

// LOS NUMEROS SON ORDEN DE CODIGO Y NADA MAS: al .w3d el tipo viaja como STRING (ConsTypeStr /
// ConsTypeInt), asi que un tipo nuevo se agrega AL FINAL y nunca en el medio (un archivo viejo no
// cambia de significado). 'Count' es el tamano de la tabla, no un tipo.
struct W3dConstraintType { enum Enum { CopyLocation = 0, CopyRotation = 1, Billboard = 2, ChildOf = 3,
    CopyScale = 4, CopyTransforms = 5, TrackTo = 6, DampedTrack = 7, LockedTrack = 8, StretchTo = 9,
    LimitLocation = 10, LimitRotation = 11, LimitScale = 12, LimitDistance = 13, MaintainVolume = 14,
    Floor = 15, Transformation = 16, Pivot = 17, FollowPath = 18, ClampTo = 19, IK = 20,
    Count = 21 }; };
struct W3dConstraintSource { enum Enum { Object = 0, View = 1 }; };
// EL ESPACIO en que un constraint lee al target y escribe al dueno (los de Blender que hacen falta):
//   World = el mundo de la escena.
//   Local = relativo al padre: un objeto, sus pos/rot/escala; un HUESO, su pose relativa a su REPOSO
//           (lo que en Blender son los canales del pose bone), medida en el marco del hueso.
//   Pose  = el espacio del objeto ARMATURE (solo huesos; en un objeto es lo mismo que World).
struct W3dConstraintSpace { enum Enum { World = 0, Local = 1, Pose = 2 }; };
// un eje LOCAL del dueno (Track To / Damped / Locked / Floor / Follow Path...). Son ejes INTERNOS del
// motor (Y arriba). En un HUESO son los del MARCO del hueso: +Y = de la cabeza a la punta (como Blender).
struct W3dConstraintAxis { enum Enum { X = 0, Y = 1, Z = 2, NegX = 3, NegY = 4, NegZ = 5 }; };

// ============================================================================
//  IDENTIDAD ESTABLE DE UN CONSTRAINT (calcado de W3dModSerial, libs/Whisk3DCore/edit/Modifier.h:36)
//
//  Un W3dConstraint vive en el heap y el stack lo borra con delete: la direccion se
//  RECICLA, asi que nada que sobreviva a la lista (el undo del borrado de objetos)
//  puede identificar una entrada por su puntero. Por el indice tampoco: Add / Remove /
//  Move Up / Move Down reordenan la lista y el indice guardado pasa a nombrar a OTRO
//  constraint, en silencio. Por eso cada uno nace con un numero MONOTONO que no se
//  reusa nunca.
//
//  El serial NO se copia: duplicar un objeto da OTROS constraints, y darles el mismo
//  numero seria justo el reciclaje que esto evita. Va en un tipo propio para que el
//  copy-constructor IMPLICITO de W3dConstraint haga lo correcto solo.
// ============================================================================
struct W3dConstraintSerial {
    unsigned int v;
    W3dConstraintSerial();                            // v = el proximo del contador. Nunca es 0.
    W3dConstraintSerial(const W3dConstraintSerial&);  // una COPIA es otro constraint -> numero NUEVO
    W3dConstraintSerial& operator=(const W3dConstraintSerial&) { return *this; } // idem: no se pisa
};

// "Copy Location" / "Copy Rotation" / "Billboard" / "Child Of" / ... / "IK"
// (gemelo de ModifierTypeName, libs/Whisk3DCore/edit/Modifier.h:105). Se define en
// libs/Whisk3DCore/objects/Objects.cpp: este header no tiene .cpp propio a proposito
// (un .cpp nuevo hay que agregarlo a las 9 listas de fuentes del proyecto).
const char* W3dConstraintTypeName(int type);

class W3dConstraint {
    public:
        W3dConstraintSerial serial;    // identidad estable (ver arriba). 'serial.v' nunca es 0.

        int          type;      // W3dConstraintType::Enum
        std::string  name;      // lo que muestra la lista; regla de nombres unicos POR OBJETO
        bool         active;    // el "ojito" de la lista

        // ================================================================================
        //  VER EN MODO EDICION (lo pidio el dueno, textual: "si el constraint tiene tildado
        //  'ver en modo edicion' se ve en modo edicion. sino, dejame editarlo en su posicion,
        //  rotacion, escala real porque sino se hace muy dificil").
        //  Cuando esta APAGADO, el objeto que se esta editando dibuja y se edita con su
        //  transform REAL (la que muestra la pestania Objeto y la que va al .w3d): un arbol
        //  con billboard deja de perseguir a la camara mientras uno le mueve los vertices.
        //  Solo afecta al objeto EN EDICION, no a sus padres ni a sus hermanos: apagar el
        //  constraint de un padre moveria tambien a los otros hijos, que nadie pidio.
        //  ARRANCA EN OFF, al reves que el 'showEdit' de los modificadores (Modifier.h),
        //  y a proposito: un modificador que no se ve en Edit Mode esconde geometria, pero un
        //  constraint que no se aplica en Edit Mode es justamente la comodidad que se pidio.
        // ================================================================================
        bool         showEdit;

        // La influencia se guarda y se edita en 0..100 (el panel la muestra con "%"), no
        // en 0..1: PropFloat bindea un float* DIRECTO al campo, asi que un rango 0..1
        // obligaria al proxy global de Properties.cpp (que se rompe con dos paneles
        // abiertos). Rompe la convencion de Opacity (0..1) a proposito.
        float        influence;
        // 0..1 para las formulas. Los extremos son EXACTOS por CLAMP explicito: 'influencia'
        // viene de un campo editable y de un archivo, asi que 99.9998 no puede dar 1.0
        // (el objetivo se aplicaria "casi" y no se veria igual que el objeto viejo) y
        // 100.0001 no puede pasarse.
        float Weight() const {
            if (influence <= 0.0f) return 0.0f;
            if (influence >= 100.0f) return 1.0f;
            return influence * 0.01f;
        }

        // ---- FUENTE (Copy Location / Copy Rotation) ----
        // 'Vista' es la camara que esta dibujando, y es un tipo EXPLICITO: si "NULL
        // significa la vista", borrar el objeto fuente (que por diseno deja el puntero en
        // NULL) convertiria en silencio un Copy Location en un "seguir a la camara".
        int          sourceType;    // W3dConstraintSource::Enum
        // puntero VIVO, no se serializa. NULL => el constraint no hace nada.
        // Lo enumera Object::RefsObject (Objects.h), asi que el destructor y el undo lo
        // sueltan solos cuando el apuntado se borra.
        Object*      sourceObj;
        std::string  sourceName;  // el vinculo POR NOMBRE: ESTO es lo que se guarda

        // ---- EJES (Copy Location / Copy Rotation) ----
        bool         axisX, axisY, axisZ;

        // ---- BILLBOARD ----
        bool         bbYaw;         // gira en horizontal
        bool         bbPitch;       // ademas se inclina
        // "tipo arbol"      = bbYaw true,  bbPitch false
        // "mira a la vista" = bbYaw true,  bbPitch true
        // (!bbYaw && bbPitch) es legible del formato viejo y se evalua bien, pero NO es
        // alcanzable desde la UI. (!bbYaw && !bbPitch) NO EXISTE: un billboard que no hace
        // nada no puede ser representable, lo arregla Normalize().
        //
        // OJO, conducta conocida y ACEPTADA: con influencia intermedia (ej. 50%) el
        // billboard pega un giro de 180 grados cuando la diferencia de angulo con la
        // camara cruza los 180. Es inherente a mezclar contra una orientacion ABSOLUTA
        // que gira; suavizarlo pide acumular vueltas, o sea estado POR VISTA, que es
        // justo lo que este diseno elimina.

        // ---- CHILD OF ----
        // El objeto se comporta como HIJO de la fuente (o de UN HUESO de ella, si la fuente es un
        // armature) en vez de su padre real: mundo = fuente * inversa * local. Con influencia 0 manda
        // el padre real y con 100 la fuente, asi que un arma hija del armature (padre real, mueve al
        // personaje entero) que ademas tiene Child Of a la mano la sigue sin transformarse dos veces.
        // Es la forma BARATA de pegar un objeto RIGIDO a un hueso: una matriz por frame y no un
        // skinning vertice por vertice (lo que importa en el N95).
        std::string  bone;         // nombre del hueso de la fuente armature ("" = el objeto entero)
        int          boneCache;    // indice resuelto de 'hueso' (runtime; se revalida por nombre)
        bool         coLoc, coRot, coEsc;   // que componentes de la fuente se heredan
        // "Set Inverse": inversa del mundo de la fuente (y del padre) al momento de fijarla, para que
        // el objeto quede donde estaba. Identidad = "Clear Inverse" (el local se lee en el espacio
        // del hueso). Se guarda en el .w3d.
        Matrix4      inverse;

        // ================================================================================
        //  LOS TIPOS NUEVOS (fase 2: todos los de Blender que tienen sentido aca + los de HUESO).
        //  Los campos son PLANOS y compartidos entre tipos (un Copy Scale y un Limit Scale usan
        //  los mismos ejes): el panel los bindea por puntero (float*/bool*/int*) y una union o una
        //  subclase por tipo obligaria a re-armar la tarjeta cada vez que cambia el tipo. Pesan
        //  ~300 bytes por constraint, que no es nada al lado de un hueso.
        //  Los DEFAULTS son los de Blender (el conversor de rigs mapea uno a uno); los que dependen
        //  del tipo los pone el ctor y los que dependen de si el dueno es un HUESO los pone
        //  W3dConstraintBoneDefaults.
        // ================================================================================
        // ---- espacios (Copy*/Limit*/Transformation) ----
        int          ownerSpace, targetSpace;   // W3dConstraintSpace
        // ---- Copy Location / Rotation / Scale ----
        bool         invX, invY, invZ;   // invertir el valor copiado de ese eje
        bool         useOffset;          // sumarlo a lo que el dueno ya tenia (en vez de reemplazar)
        // Copy Rotation: 0 Replace, 1 Add (suma de euler), 2 Before (target * dueno), 3 After (dueno * target)
        // Copy Transforms: 0 Replace, 2 Before, 3 After. Transformation: 0 Replace, 1 Add.
        int          mixMode;
        float        headTail;           // target HUESO: 0 = su cabeza, 1 = su punta (lo que se lee como "posicion")
        float        power;              // Copy Scale: escala copiada ^ power
        bool         scaleAdd;           // Copy Scale con offset: suma (s + t - 1) en vez de multiplicar
        // ---- Track To / Damped Track / Locked Track / Floor / Follow Path ----
        int          trackAxis;          // W3dConstraintAxis del dueno que APUNTA (Floor: el eje del piso)
        int          upAxis;             // X/Y/Z: el eje del dueno que queda ARRIBA (Track To, Follow Path)
        int          lockAxis;           // X/Y/Z: Locked Track gira SOLO alrededor de este eje
        bool         targetUp;           // Track To: "arriba" es el +Y del target y no el del mundo (Blender: Target Z)
        // ---- Stretch To ----
        float        restLength;         // largo de reposo; 0 = el del hueso (o la distancia del 1er frame en un objeto)
        float        bulge;              // cuanto engorda al encogerse (1 = conserva el volumen)
        int          volumeMode;         // 0 XZ, 1 X, 2 Z, 3 ninguno
        int          keepAxis;           // 0 Plane X, 1 Plane Z, 2 Swing (giro minimo)
        // ---- Limit Location / Rotation (grados) / Scale ----
        bool         useMin[3], useMax[3];
        Vector3      vmin, vmax;
        // ---- Limit Distance ----
        float        distance;           // 0 = la distancia del primer frame (como Blender)
        int          distMode;           // 0 Inside, 1 Outside, 2 On Surface
        // ---- Maintain Volume ----
        float        volume;
        int          freeAxis;           // X/Y/Z: el eje que manda (los otros dos compensan)
        int          volMode;            // 0 Strict, 1 Uniform, 2 Single Axis
        // ---- Floor ----
        float        floorOffset;
        bool         useRotation;        // el piso gira con el target
        // ---- Transformation: canal del target -> canal del dueno ----
        int          mapFrom, mapTo;     // 0 posicion, 1 rotacion (grados), 2 escala
        int          mapAxis[3];         // para cada eje del DUENO, que eje del target lo maneja (0/1/2)
        Vector3      fromMin, fromMax, toMin, toMax;
        bool         extrapolate;
        // ---- Pivot ----
        Vector3      pivotOffset;
        bool         pivotAbs;           // sin target: el offset es un punto ABSOLUTO (si no, relativo al dueno)
        int          pivotRotAxis;       // -1 siempre; 0..5 = pivotea solo si la rotacion de ese eje es <0 (0-2) o >0 (3-5)
        // ---- Follow Path / Clamp To (el target es una Curve) ----
        bool         fixedPosition;      // true: la posicion es offsetFactor (0..1); false: el frame / pathDuration
        float        offsetFactor;
        float        pathOffset;         // en frames (se RESTA al frame, como Blender)
        float        pathDuration;       // frames que tarda en recorrer la curva entera
        bool         followCurve;        // ademas se orienta con la tangente (trackAxis = adelante, upAxis = arriba)
        int          clampAxis;          // Clamp To: 0 auto (el mas largo de la caja), 1 X, 2 Y, 3 Z
        bool         cyclic;             // Clamp To: da la vuelta en vez de frenar en las puntas
        // ---- IK (solo huesos) ----
        int          chainLength;        // huesos de la cadena contando el dueno; 0 = hasta la raiz
        int          iterations;
        bool         useTail;            // el efector es la PUNTA del hueso (si no, su cabeza y el no gira)
        bool         useStretch;         // si el target no llega, los huesos de la cadena se estiran
        float        poleAngle;          // grados: giro extra de la cadena alrededor del eje raiz->target
        // POLE TARGET: hacia donde se dobla la cadena (la rodilla mira al pole). Mismo trato que la fuente:
        // puntero VIVO (lo suelta la puerta de refs del Armature) + el NOMBRE que se guarda.
        Object*      poleObj;
        std::string  poleName;
        std::string  poleBone;           // hueso del pole (si el pole es un armature)
        int          poleBoneCache;

        // ---- RUNTIME de los constraints de HUESO (no se guarda) ----
        // la matriz de MUNDO del target y del pole si son de OTRO objeto, juntadas ANTES del FK
        // (EvaluatePoseSkeleton no puede llamar afuera a mitad del FK: comparte scratch estatico).
        Matrix4      cacheT, cacheP;
        Matrix4      cacheTRest;         // la base local del target (para targetSpace Local)
        float        cacheTLen;          // largo del hueso target (0 = no es un hueso)
        bool         cacheTOk, cachePOk;

        // aviso de ciclo (A copia de B y B de A): se prende al avisar para no inundar la
        // pantalla con una notificacion POR FRAME. Runtime puro, no se guarda. Significa
        // "ya avise para ESTA fuente": el panel lo vuelve a poner en false cada vez que se
        // elige otra fuente (ActionConstraintPickedSource), o sea que romper el ciclo y volver a
        // armarlo avisa de nuevo.
        bool         noticeCycle;

        // C++03: nada de inicializadores en la clase, todo en la lista del ctor.
        explicit W3dConstraint(int t)
            : type(t),
              name(W3dConstraintTypeName(t)),
              active(true),
              showEdit(false),   // OFF: en Edit Mode el objeto se edita en su transform real
              influence(100.0f),
              sourceType(W3dConstraintSource::Object),
              sourceObj(NULL),
              sourceName(),
              axisX(true), axisY(true), axisZ(true),
              bbYaw(true), bbPitch(false),
              boneCache(-1), coLoc(true), coRot(true), coEsc(true),
              ownerSpace(W3dConstraintSpace::World), targetSpace(W3dConstraintSpace::World),
              invX(false), invY(false), invZ(false), useOffset(false), mixMode(0), headTail(0.0f),
              power(1.0f), scaleAdd(false),
              trackAxis(W3dConstraintAxis::Y), upAxis(W3dConstraintAxis::Z), lockAxis(W3dConstraintAxis::Z),
              targetUp(false),
              restLength(0.0f), bulge(1.0f), volumeMode(0), keepAxis(0),
              vmin(0, 0, 0), vmax(0, 0, 0),
              distance(0.0f), distMode(0),
              volume(1.0f), freeAxis(W3dConstraintAxis::Y), volMode(0),
              floorOffset(0.0f), useRotation(false),
              mapFrom(0), mapTo(0), fromMin(0, 0, 0), fromMax(0, 0, 0), toMin(0, 0, 0), toMax(0, 0, 0),
              extrapolate(false),
              pivotOffset(0, 0, 0), pivotAbs(false), pivotRotAxis(-1),
              fixedPosition(false), offsetFactor(0.0f), pathOffset(0.0f), pathDuration(100.0f),
              followCurve(false), clampAxis(0), cyclic(false),
              chainLength(0), iterations(20), useTail(true), useStretch(false), poleAngle(0.0f),
              poleObj(NULL), poleName(), poleBone(), poleBoneCache(-1),
              cacheTLen(0.0f), cacheTOk(false), cachePOk(false),
              noticeCycle(false) {
            inverse.Identity(); cacheT.Identity(); cacheP.Identity(); cacheTRest.Identity();
            for (int k = 0; k < 3; k++) { useMin[k] = false; useMax[k] = false; mapAxis[k] = k; }
            // defaults que dependen del TIPO (los de Blender; ver arriba)
            switch (t) {
                case W3dConstraintType::TrackTo:
                    // un OBJETO mira por su -Z con el +Y arriba (la convencion de camaras del motor,
                    // Quaternion::FromDirection). Un hueso: ver W3dConstraintBoneDefaults.
                    trackAxis = W3dConstraintAxis::NegZ; upAxis = W3dConstraintAxis::Y; break;
                case W3dConstraintType::LimitScale:
                    vmin = Vector3(1, 1, 1); vmax = Vector3(1, 1, 1); break;
                case W3dConstraintType::Floor:
                    trackAxis = W3dConstraintAxis::Y; break;   // el piso: +Y (arriba del motor)
                case W3dConstraintType::FollowPath:
                    trackAxis = W3dConstraintAxis::NegZ; upAxis = W3dConstraintAxis::Y; break;
                case W3dConstraintType::Transformation:
                    fromMax = Vector3(1, 1, 1); toMax = Vector3(1, 1, 1); break;
                default: break;
            }
        }

        // deja los campos en un estado que el evaluador y la UI puedan representar. La
        // llama el lector del .w3d, que es el unico que trae valores de afuera (un archivo
        // lo pudo editar cualquiera). El panel NO la llama y no la necesita: el desplegable
        // de modo prende bbYaw a mano y el campo de influencia ya nace con SetRange(0,100).
        void Normalize() {
            if (influence < 0.0f)   influence = 0.0f;
            if (influence > 100.0f) influence = 100.0f;
            if (type == W3dConstraintType::Billboard && !bbYaw && !bbPitch) bbYaw = true;
            // los enums que vienen de afuera se acotan: un indice fuera de rango no puede llegar al evaluador
            if (ownerSpace  < 0 || ownerSpace  > 2) ownerSpace  = W3dConstraintSpace::World;
            if (targetSpace < 0 || targetSpace > 2) targetSpace = W3dConstraintSpace::World;
            if (trackAxis < 0 || trackAxis > 5) trackAxis = W3dConstraintAxis::Y;
            if (upAxis   < 0 || upAxis   > 2) upAxis   = W3dConstraintAxis::Z;
            if (lockAxis < 0 || lockAxis > 2) lockAxis = W3dConstraintAxis::Z;
            if (freeAxis < 0 || freeAxis > 2) freeAxis = W3dConstraintAxis::Y;
            if (mixMode < 0 || mixMode > 3) mixMode = 0;
            if (volumeMode < 0 || volumeMode > 3) volumeMode = 0;
            if (keepAxis < 0 || keepAxis > 2) keepAxis = 0;
            if (distMode < 0 || distMode > 2) distMode = 0;
            if (volMode < 0 || volMode > 2) volMode = 0;
            if (mapFrom < 0 || mapFrom > 2) mapFrom = 0;
            if (mapTo < 0 || mapTo > 2) mapTo = 0;
            for (int k = 0; k < 3; k++) if (mapAxis[k] < 0 || mapAxis[k] > 2) mapAxis[k] = k;
            if (pivotRotAxis < -1 || pivotRotAxis > 5) pivotRotAxis = -1;
            if (clampAxis < 0 || clampAxis > 3) clampAxis = 0;
            if (chainLength < 0) chainLength = 0;
            if (iterations < 1) iterations = 1;
            if (iterations > 1000) iterations = 1000;
            if (pathDuration < 1.0f) pathDuration = 1.0f;
            if (headTail < 0.0f) headTail = 0.0f;
            if (headTail > 1.0f) headTail = 1.0f;
        }
};

// defaults de Blender cuando el dueno es un HUESO (el marco del hueso tiene el +Y A LO LARGO del hueso):
// Track To / Damped / Locked apuntan con el +Y; Track To deja el +Z arriba. Lo llaman el Add del panel
// y el lector del .w3d ANTES de leer las claves (una clave ausente = este default).
inline void W3dConstraintBoneDefaults(W3dConstraint* c) {
    if (!c) return;
    if (c->type == W3dConstraintType::TrackTo || c->type == W3dConstraintType::DampedTrack ||
        c->type == W3dConstraintType::LockedTrack) {
        c->trackAxis = W3dConstraintAxis::Y; c->upAxis = W3dConstraintAxis::Z; c->lockAxis = W3dConstraintAxis::Z;
    }
}

// el TIPO puede ir en el dueno? Un hueso no tiene vista (la pose se cachea por FRAME, no por vista): el
// Billboard no va. El IK es SOLO de huesos (una cadena de objetos no tiene FK que resolver).
inline bool W3dConstraintTypeForBone(int t)   { return t >= 0 && t < W3dConstraintType::Count && t != W3dConstraintType::Billboard; }
inline bool W3dConstraintTypeForObject(int t) { return t >= 0 && t < W3dConstraintType::Count && t != W3dConstraintType::IK; }

// ============================================================================
//  EL STACK DE CONSTRAINTS DE UN HUESO (W3dBone::constraints, objects/Armature.h)
//
//  W3dBone es un struct que el editor copia POR VALOR todo el tiempo: el snapshot del undo de
//  huesos, Shift+D, el borrado (que reconstruye el vector), y el propio std::vector al crecer.
//  Un vector<W3dConstraint*> pelado ahi adentro daria DOS duenos del mismo constraint a la
//  primera copia (doble delete). Este envoltorio copia en PROFUNDIDAD y CONSERVA el serial: la
//  copia de un hueso es EL MISMO hueso (el snapshot del undo vuelve a la escena con la identidad
//  que tenia, y un RefEntry pendiente lo encuentra). Duplicar un hueso de verdad (Shift+D) es el
//  unico caso de "otro" hueso: ahi se llama RenewSerials.
// ============================================================================
class W3dBoneConstraints {
    public:
        std::vector<W3dConstraint*> list;   // en ORDEN DE STACK
        int active;                         // el elegido en la lista del panel (-1 = ninguno)
        W3dBoneConstraints() : active(-1) {}
        W3dBoneConstraints(const W3dBoneConstraints& o) : active(o.active) { CopyFrom(o); }
        W3dBoneConstraints& operator=(const W3dBoneConstraints& o) {
            if (this != &o) { Clear(); CopyFrom(o); active = o.active; }
            return *this;
        }
        ~W3dBoneConstraints() { Clear(); }
        void Clear() { for (size_t i = 0; i < list.size(); i++) delete list[i]; list.clear(); active = -1; }
        void Swap(W3dBoneConstraints& o) { list.swap(o.list); int a = active; active = o.active; o.active = a; }
        bool empty() const { return list.empty(); }
        int  size() const { return (int)list.size(); }
        // otra identidad para cada entrada (el hueso se DUPLICO: ya no es el mismo)
        void RenewSerials() {
            for (size_t i = 0; i < list.size(); i++) { W3dConstraintSerial fresh; list[i]->serial.v = fresh.v; }
        }
    private:
        void CopyFrom(const W3dBoneConstraints& o) {
            list.reserve(o.list.size());
            for (size_t i = 0; i < o.list.size(); i++) {
                if (!o.list[i]) continue;
                W3dConstraint* c = new W3dConstraint(*o.list[i]);
                c->serial.v = o.list[i]->serial.v;   // MISMA identidad (ver arriba)
                list.push_back(c);
            }
        }
};

// ============================================================================
//  EVALUACION GENERICA (Objects.cpp): un constraint sobre una MATRIZ en un "espacio comun".
//  La usan los huesos (espacio comun = el del objeto armature) y los tipos nuevos de objeto
//  (espacio comun = el mundo). Los 4 tipos de la fase 1 en un OBJETO siguen por su camino de
//  siempre en W3dEvalCons, que da resultados BIT A BIT iguales a los de antes.
// ============================================================================
struct W3dConsMat {
    Matrix4 M;        // la matriz (posicion + orientacion + escala) en el espacio comun
    Matrix4 rest;     // su "base local": Local = inv(rest) * M (objeto: mundo del padre; hueso: padre*reposo*marco)
    float   length;   // hueso: largo de la cabeza a la punta (en su marco, el +Y). 0 = no es un hueso
    bool    valid;
    W3dConsMat() : length(0.0f), valid(false) { M.Identity(); rest.Identity(); }
};
struct W3dConsCtx {
    Matrix4 toWorld, fromWorld;  // espacio comun <-> mundo (objeto: identidad; hueso: el mundo del armature)
    bool    isBone;
    int     eulerOrder;          // orden de euler del DUENO para los ejes en grados (objeto: 5 = Rx*Ry*Rz; hueso: su rotOrder)
    W3dConsMat owner;            // ENTRA y SALE (el constraint la modifica)
    W3dConsMat target;           // ya resuelto por el que llama (con headTail aplicado)
    const Object* targetObj;     // el objeto target (Follow Path / Clamp To leen su curva)
    Matrix4 targetObjM;          // la matriz del OBJETO target sin hueso, en el espacio comun
    W3dConsCtx() : isBone(false), eulerOrder(5), targetObj(NULL) {
        toWorld.Identity(); fromWorld.Identity(); targetObjM.Identity(); }
};
// aplica UN constraint (cualquier tipo salvo Billboard e IK) a ctx.owner.M, mezclando por la influencia.
// false = no hizo nada (sin target, sin ejes, ...). No toca nada fuera de ctx (salvo el restLength /
// distance automaticos, que se fijan en la primera evaluacion como en Blender).
bool W3dConsApply(W3dConstraint* c, W3dConsCtx& ctx);
// el constraint necesita un TARGET para hacer algo?
bool W3dConsNeedsTarget(int type);
// PUEDE cambiar algo (activo, influencia > 0, con target si lo necesita, con ejes si los usa...)? El
// criterio de los tipos genericos, para un dueno objeto (bone=false) o hueso (bone=true). Los 4 tipos
// de la fase 1 en un objeto los decide W3dConstraintEffective (Objects.cpp), que ademas mira la edicion.
bool W3dConsTypeEffective(const W3dConstraint* c, bool bone);
// Child Of: la matriz del target con los componentes NO heredados sacados (Location/Rotation/Scale)
void W3dChildOfMaskMatrix(const W3dConstraint* c, Matrix4& T);
// mezcla de dos matrices por partes (posicion lineal, rotacion por arco corto, escala lineal). t en [0,1]
Matrix4 W3dConsBlend(const Matrix4& a, const Matrix4& b, float t);
// rotacion minima que lleva el unitario 'from' a 'to' (robusta con vectores opuestos)
Quaternion W3dQuatFromTo(const Vector3& from, const Vector3& to);
// euler (grados) <-> matriz de rotacion en los 6 ordenes de MatRotEuler (SkeletalAnimation.cpp):
// orden 0 = Rz*Ry*Rx ... 5 = Rx*Ry*Rz. Inversas EXACTAS (fuera del gimbal lock).
Vector3 W3dEulerFromMatrix(const Matrix4& rot, int order);
Matrix4 W3dMatrixFromEuler(const Vector3& deg, int order);

// la POLILINEA de una Curve (Follow Path / Clamp To). El Core no ve la clase Curve (vive en
// main/objects y no esta en el build de los juegos 2D): la lee por este gancho, que registra el
// lector del .w3d (libs/Whisk3DCore/importers/import_w3d.cpp, que esta en el editor y en los juegos 3D).
// Devuelve los vertices EVALUADOS (3 floats c/u, espacio LOCAL de la curva) y sus aristas (pares;
// 0 aristas = la polilinea implicita 0-1-2-...). false = no es una curva o esta vacia.
typedef bool (*W3dCurvePolyFn)(const Object* curve, const float** verts, int* count,
                               const unsigned short** edges, int* edgeCount);
extern W3dCurvePolyFn g_w3dCurvePoly;

#endif // W3D_CONSTRAINT_H
