#ifndef OBJECT_H
#define OBJECT_H

#ifdef _WIN32
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include <vector>
#include <string>
#include <set>
#include <algorithm>

#include "base/crossplatform.h"       // W3D_OVERRIDE (C++03/RVCT) + MeshIndex, para las subclases de Object
#include "base/W3dInteractionState.h" // InteractionMode/estado (el Core NO depende del header del editor)
#include "objects/W3dConstraint.h"   // los constraints son una PROPIEDAD de cada objeto

// Dialecto C++03 compartido (PLAN-UNIFICACION.md): este header compila tanto
// en PC/Android (C++11) como en Symbian/RVCT 2.2 (C++03). Por eso:
//  - enums "tipo enum class" con el idiom struct+Enum (misma sintaxis de uso)
//  - sin inicializadores de miembro en la clase (van al constructor)
//  - NULL en vez de nullptr, std::set en vez de unordered_set

// (W3D_OVERRIDE vive en base/crossplatform.h: antes se hacia '#define override' aca, o sea que
//  este header le robaba un keyword a TODO el que lo incluyera, su propio codigo incluido)
#ifndef W3D_SYMBIAN
    // dependencias que el core todavia arrastra en PC; en la limpieza de la
    // Fase 5 vuelan de aca (el core no debe tocar GL ni la UI)
    #include <iostream>
    #include <iomanip>
    #include <functional>
    #include <sstream>
    #include <GL/gl.h>
#endif

class Viewport3D;
extern Viewport3D* Viewport3DActive;

// enum class portable: ObjectType::mesh, las comparaciones y el switch
// funcionan igual que antes en PC
struct ObjectType {
    enum Enum {
        scene, mesh, camera, light, empty, armature, curve,
        // (aca vivia 'constraint', el objeto que apuntaba a otro y le escribia la rotacion.
        //  Se dio de baja: un constraint es una PROPIEDAD del objeto, ver W3dConstraint.h.
        //  Sacarlo del enum es seguro porque el tipo NUNCA viaja como numero: el .w3d
        //  guarda el string "constraint", y ese string lo sigue leyendo el importador para
        //  MIGRAR los archivos viejos.)
        collection, baseObject, mirror,
        script,         // colgadero de SCRIPTS con un target opcional (se llamaba 'gamepad')
        instance,
        ui,             // raiz de una INTERFAZ 2D (se edita en el viewport Editor 2D)
        texto2d,        // elemento de TEXTO de una interfaz 2D (vive siempre dentro de un UI)
        imagen2d,       // elemento de IMAGEN de una interfaz 2D (una textura con modo de ajuste)
        rect2d,         // elemento RECTANGULO: color solido o transparente (acomoda elementos)
        cont2d,         // elemento CONTENEDOR: rectangulo invisible que solo ordena a sus hijos
        slice9,         // elemento SLICE 9: imagen con bordes fijos (marcos, botones, paneles)
        boton2d,        // elemento BOTON: card con texto y/o icono (estilo Whisk3D)
        expandir2d,     // elemento EXPANDIR: absorbe el espacio libre en filas/columnas
        video2d,        // elemento VIDEO: fondos animados y festejos (sin sonido)
        // AL FINAL a proposito (el tipo nunca viaja como numero: el .w3d guarda strings)
        lod,            // dibuja UN solo hijo segun la distancia a la camara (niveles de detalle)
        culling,        // frustum culling de sus hijos por AABB (y orden adelante->atras)
        particulas,     // emisor de PARTICULAS (billboards del Core, gfx/w3dParticles.h)
        viszona,        // selector de CELDA de visibilidad (VisSet/.w3dvis): grilla/volumenes/curva/manual
        gridcull        // culling ESPACIAL POR GRILLA (estilo GTA/RenderWare): celdas + frustum + distancia + LOD
    };
    Enum v;
    ObjectType(Enum e) : v(e) {}
    operator Enum() const { return v; }
};

// Forward declarations
class Object;
extern Object* SceneCollection;
extern std::vector<Object*> ObjSelects;
// LOCAL VIEW (aislar la seleccion en un viewport, tecla "/" del editor): el editor publica esto por-frame ANTES
// del pase de escena; el Core lo LEE en Object::Render. En el runtime del juego queda en false/NULL (gratis).
extern bool g_localViewActivo;
extern const std::set<Object*>* g_localViewVisibles; // objetos a traversar/dibujar (NULL = no filtrar)
extern Object* CollectionActive;
extern Object* ObjActivo;

// ===== EDIT MODE =====
// la malla ACTIVA en Edit Mode (NULL = nadie esta en edit). La setea el viewport
// segun InteractionMode + ObjActivo; el render de la malla la consulta para
// dibujar el overlay de edicion (vertices/bordes) en vez del contorno de objeto.
extern Object* g_editMesh;
// sub-elemento activo en Edit Mode. El selector de la barra lo cambia; la
// seleccion (todo/nada/invertir) y el render se refieren a esto.
enum { SelVertex, SelEdge, SelFace };
extern int EditSelectMode; // default SelVertex

void DeseleccionarTodo(bool IncluirColecciones = false);

// modos de rotacion del editor. Por ahora 3 (los otros 5
// ordenes de Euler vienen despues). El default es XYZ Euler.
enum RotMode { RotEulerXYZ = 0, RotQuaternion = 1, RotAxisAngle = 2 };

class Object {
    public:
        // los defaults estan en la lista de inicializacion del constructor
        // (Objects.cpp); los inicializadores en la clase son C++11
        Object* Parent;
        std::vector<Object*> Childrens;
        bool visible;
        // se VE en el viewport pero puede NO salir en el render final (la foto/
        // los pases): la marca clasica de "renderizable" de los editores 3D
        bool renderizable;
        // ESTATICO vs DINAMICO (para el culling por GRILLA, W3dGridCull): true = NO se mueve en runtime, su
        // celda se cachea (escenario, cajas); false = DINAMICO (Crash, enemigos), se re-evalua su celda por
        // frame. Solo importa cuando el objeto cuelga de un GridCull (afuera se ignora). Default true.
        bool estatico;
        bool desplegado;
        bool showRelantionshipsLines;
        // EL PLAYHEAD POSO A ESTE OBJETO alguna vez en esta sesion (lo prende W3dAplicarCurvasEnFrame
        // cuando de verdad escribe algun canal). Es ESTADO REAL, igual que Mesh::posadaPorAnim para
        // la geometria, y por el mismo motivo: al guardar hay que devolver el objeto a su cuadro base
        // para no HORNEAR la pose del scrub, pero SOLO si alguien lo poso. Deducirlo de "tiene curvas"
        // estaba mal y pisaba el trabajo del usuario: una malla con curvas propias que el playhead
        // nunca aplico (el timeline no estaba en su animacion) se movia A MANO, y el guardado
        // escribia el cuadro base en vez del movimiento. Ver ReposoAnimObjetos (main/io/GuardarW3D.cpp).
        bool posadoPorCurvas;
        bool select;
        std::string name;

        // ====================================================================
        //  IDENTIDAD ESTABLE (serial): el numero que NO se recicla
        //
        //  El nombre es unico EN UN MOMENTO DADO, pero NO es una identidad: al
        //  renombrar se LIBERA y el sistema de nombres unicos se lo da al
        //  siguiente que lo pida, y al borrar un objeto tambien. O sea que
        //  "RigA" puede ser dos objetos distintos en dos momentos distintos,
        //  exactamente como una direccion de memoria reciclada por el
        //  allocator (delete + new = mismo puntero; rename + rename = mismo
        //  nombre). Cualquier ESTADO EFIMERO que se guarde apuntando "al que
        //  se llama X" cae sobre el equivocado, en silencio, cuando X cambia
        //  de dueno.
        //  'serial' es un contador MONOTONO asignado al NACER que no se reusa
        //  NUNCA (ni al renombrar, ni al borrar y crear otro): es la identidad
        //  con la que ese estado efimero tiene que apuntar. Hoy lo usan las
        //  claves del dope sheet (ver main/ui/ViewPorts/Timeline.cpp).
        //  NO se serializa ni se compara entre sesiones: es runtime puro. Dos
        //  cargas del mismo .w3d dan seriales distintos y eso esta BIEN.
        //  CUIDADO: esto NO reemplaza a los vinculos del MODELO que van por
        //  nombre A PROPOSITO (hueso <-> vertex group, modArmature, targetName,
        //  RielName, refs de lua): esos tienen que SEGUIR por nombre porque
        //  son lo que el usuario re-liga renombrando, y ademas se guardan.
        // ====================================================================
        unsigned int serial;

        // PALETA DE COLORES elegida por este objeto, POR NOMBRE ("" = la del
        // padre: se HEREDA subiendo por Parent). Las paletas viven a nivel
        // PROYECTO (ver main/W3dPaletas.h); los elementos 2D referencian sus
        // colores por INDICE contra la paleta EFECTIVA del objeto. El campo
        // es generico (tambien en objetos 3D) como hook para materiales.
        std::string paleta;

        // SCRATCH: NO LEER. No es "la transform del objeto" y nadie la escribe ya.
        // Era el destino de GetMatrix() en Render(), o sea que quedaba con la matriz de la ULTIMA
        // vista que dibujo. Mientras la matriz salia solo de pos/rot/scale eso daba lo mismo; en
        // cuanto la matriz efectiva puede depender de la vista bindeada (dos viewports, dos
        // camaras) un miembro publico cacheado es una trampa: el que lo lea se lleva la respuesta
        // del OTRO viewport, muda. Todos los call sites usan una Matrix4 LOCAL (Render, Pick3D,
        // el gizmo de origen, el icono 3D, el Mirror). Se conserva el campo para no romper codigo
        // externo que herede de Object; nace sin inicializar.
        Matrix4 M;

        Vector3 pos;
        Vector3 scale;

    // ========================================================================
    //  LA ROTACION: EL QUATERNION **Y** SUS DISPLAYS, SIEMPRE JUNTOS
    //
    //  Una rotacion vive en CUATRO campos que dicen lo mismo de formas
    //  distintas: 'rot' (el quaternion, lo que se RENDERIZA), 'rotEuler' (los
    //  grados CON sus vueltas, que es lo que se GUARDA en el .w3d y lo que
    //  keyframea el timeline), y 'rotAngle'/'rotAxis' + 'rotQuat' (los otros
    //  dos modos del panel). Ninguno se explica solo:
    //    - el quaternion no distingue 0 de 360 -> sin el euler no hay vueltas;
    //    - el euler no se renderiza -> sin el quaternion no rota nada.
    //
    //  Mientras 'rot' fue un miembro publico, cada camino que rotaba tenia que
    //  ACORDARSE de arrastrar el resto, y los que se olvidaron rompieron cosas
    //  distintas segun de que lado se olvidaron:
    //    - LEER un .w3d escribia SOLO rotEuler -> la escena abria SIN ROTAR
    //      (el render usa el quaternion): "la camara abre en 0 grados";
    //    - el trackball de la vista derivaba el euler con ToEulerYXZ mientras
    //      el resto del editor usa XYZ -> lo que se guardaba NO era lo que se
    //      veia (se guardaba una rotacion distinta);
    //    - el billboard constraint, el 'objrot' del harness y la correccion de
    //      ejes del FBX escribian SOLO el quaternion -> se veia bien y al
    //      guardar se perdia (el .w3d escribe el euler).
    //
    //  Es exactamente el mismo tipo de olvido que cerro W3dPosVerts con las
    //  posiciones (ver Mesh.h) y se cierra igual: HACIENDO QUE NO HAYA NADA
    //  QUE RECORDAR. 'rot' es PRIVADO; se lee con Rot() y solo se escribe por
    //  una de las puertas de abajo, que actualizan TODO. No existe la forma de
    //  escribir uno sin el otro.
    //
    //  Los campos de DISPLAY siguen publicos porque el panel de propiedades
    //  necesita punteros a sus floats para editarlos; escribirlos a mano NO
    //  rota nada (son display) -> hay que cerrar la edicion con la puerta que
    //  corresponda (lo hace SincronizarRotacionActiva, ObjectMode.cpp).
    // ========================================================================
    private:
        Quaternion rot;        // la rotacion REAL (siempre). PRIVADA: ver arriba.
    public:
        const Quaternion& Rot() const { return rot; }   // lectura (render, matrices, snapshots)

        // --- LAS PUERTAS DE ESCRITURA (cada una dice QUIEN MANDA) ---
        // el QUATERNION manda: normaliza y re-deriva todos los displays. El euler conserva sus vueltas
        // (se elige la forma equivalente mas parecida a la que ya tenia, ver W3dEulerCercano).
        void SetRot(const Quaternion& q);
        // el EULER manda (ya trae sus vueltas: la curva de animacion, el panel, el .w3d). El quaternion se
        // deriva de el. Usar esta y no SetRot: ahi el euler seria solo "a que parecerse" y una vuelta entera
        // se perderia.
        void SetRotEuler(const Vector3& e);
        // el AXIS/ANGLE manda (el panel en modo Axis Angle). Normaliza el eje.
        void SetRotAxisAngle(const Vector3& eje, float angDeg);
        // restaura un SNAPSHOT tal cual se capturo (undo, cancelar un transform, la sim del juego): el euler NO
        // se re-deriva, se escribe el que se habia guardado, con sus vueltas exactas.
        void SetRotSnapshot(const Quaternion& q, const Vector3& euler);
        // re-deriva los displays desde 'rot' sin cambiar la rotacion. Con las puertas de arriba ya no hace
        // falta llamarla despues de rotar; queda para refrescar el panel al seleccionar / cambiar de modo.
        void ActualizarDisplayRot();

        Vector3 rotEuler;      // display en grados (modos Euler), CON sus vueltas: es lo que se guarda
        int rotMode;           // 0=XYZ Euler, 1=Quaternion, 2=Axis Angle
        float rotAngle;        // display Axis Angle: angulo en grados
        Vector3 rotAxis;       // display Axis Angle: eje
        Quaternion rotQuat;    // display Quaternion (WXYZ): la copia que edita el panel

        virtual ObjectType getType() { return ObjectType::baseObject; }

        // SCRIPT estilo Unity (opcional, casi nadie lo usa: puntero, NULL default).
        // La ruta del .lua + las referencias expuestas viven aca; el runtime (las
        // instancias lua vivas durante el play) lo maneja script/W3dScript.
        struct W3dScriptDatos* scriptDatos;

        // FISICA de cuerpo rigido (opcional, puntero, NULL default): la
        // DEFINICION que persiste en el .w3d (tipo, masa, caja, centro,
        // friccion, rebote). El cuerpo runtime lo maneja physics/W3dRigido
        // durante el play; sin definicion el objeto no paga nada.
        struct W3dRigidoDef* fisica;

        Object(
            Object* parent,
            const std::string& nombre = "Objeto",
            Vector3 Pos = Vector3(0,0,0),
            Vector3 Rot = Vector3(0.0f, 0.0f, 0.0f),
            Vector3 scale = Vector3(1.0f, 1.0f, 1.0f)
        );
        virtual ~Object();

        // LA PUERTA del nombre: uniquifica en el scope del objeto (su escena UI,
        // o toda la escena si no cuelga de ninguna). Ver Objects.cpp.
        void SetNameObj(const std::string& nombre);
        // asignacion CRUDA (sin uniquificar): SOLO loaders y harness de tests.
        void SetNameCrudo(const std::string& nombre);
        // nombre libre para este objeto SIN escribirlo
        std::string NombreLibre(const std::string& base);

        void EliminarObjetosSeleccionados(bool IncluirCollecciones = false);

        std::string SetName(const std::string& baseName);

        void Seleccionar();
        void Deseleccionar();
        void DeseleccionarCompleto(bool IncluirColecciones = false);
        bool EstaSeleccionado(bool IncluirColecciones = false);
        bool SeleccionarCompleto(bool IncluirColecciones = false);
        void InvertirSeleccionCompleto(bool IncluirColecciones = false);

        // Edit Mode: seleccion de sub-elementos (la implementa Mesh). Las funciones
        // globales de seleccion (Seleccionar/DeseleccionarTodo/Invertir) las llaman
        // cuando g_editMesh != NULL, asi NO hay logica de modo en cada plataforma.
        virtual void EditSeleccionarTodo(bool sel) { (void)sel; }
        virtual void EditInvertir() {}

        // ====================================================================
        //  LAS DOS TRANSFORMS: **EFECTIVA** y **BASE**
        //
        //  Desde que existen los constraints la transform de un objeto se puede
        //  preguntar de dos maneras, y NO son la misma:
        //    - EFECTIVA (GetMatrix / GetWorldMatrix / LocalAMundo): lo que se VE.
        //      Es T*R*S de los campos CON el stack de constraints aplicado encima.
        //    - BASE (GetMatrixBase / GetWorldMatrixBase / LocalAMundoBase): la
        //      transform CRUDA, exactamente T*R*S de pos/Rot()/scale, como si el
        //      objeto no tuviera constraints.
        //
        //  LA REGLA, y no tiene excepciones: **lo que DIBUJA o MIDE EN PANTALLA usa
        //  la EFECTIVA; lo que HORNEA GEOMETRIA o ESCRIBE A DISCO usa la BASE.**
        //  Si el que hornea usara la efectiva, la orientacion de la camara que dibujo
        //  ultimo se quedaria METIDA ADENTRO del archivo exportado / de los vertices
        //  del Join: el mismo bug que tenia el objeto Constraint viejo (dado de baja), que escribia
        //  target->rot en el render. El precedente ya estaba en el repo:
        //  AplicarTransform (main/objects/ObjectMode.cpp) nunca uso GetMatrix, arma
        //  la matriz con BuildMatrix(pos, Rot(), scale) a mano.
        //
        //  *** LA EFECTIVA NO ES PURA. Con constraints, el MISMO objeto con los
        //      MISMOS pos/rot/scale devuelve matrices DISTINTAS segun cual fue el
        //      ultimo W3dVistaBind (objects/CameraBase.h). El que pregunte fuera del
        //      render de una vista tiene que bindear la suya primero (en el editor,
        //      Viewport3D::BindVista). No hay cache: preguntar dos veces con la misma
        //      vista bindeada da lo mismo, y nada queda guardado en el objeto. ***
        //
        //  Sin constraints las dos son LA MISMA CUENTA: GetMatrix sale por el
        //  early-out de 'constraints.empty()' y un proyecto que no usa constraints se
        //  comporta bit a bit como antes de que esto existiera.
        // ====================================================================
        void GetMatrix(Matrix4& out) const;        // LOCAL efectiva (T*R*S + constraints)
        void GetMatrixBase(Matrix4& out) const;    // LOCAL cruda (T*R*S de los campos)
        // matriz de MUNDO: GetMatrix + toda la cadena de padres (T*R*S de cada uno).
        // Es la UNICA fuente de la transform de mundo: la usan GetGlobalPosition,
        // LocalAMundo, el foco y el color-pick de edit mode. NO reimplementar la
        // cadena a mano (un pick que la rearmaba con "while(Parent)" salteaba la
        // matriz del propio objeto top-level -Parent NULL- y rompia a escala!=1).
        void GetWorldMatrix(Matrix4& out) const;
        // idem pero con la BASE de CADA eslabon: ni la del objeto ni la de ningun
        // ancestro pasa por sus constraints. Es la que va a disco / a los vertices.
        void GetWorldMatrixBase(Matrix4& out) const;
        void RotateLocal(float pitch, float yaw, float roll);
        Vector3 GetGlobalPosition() const;
        // idem con la cadena BASE. La necesita el que convierte MUNDO->LOCAL para ESCRIBIR
        // (reparent, snap al cursor 3D, la preservacion de hijos al borrar el padre): esos
        // caminos ya usan RotGlobalDe/ScaleGlobalDe, que leen los CAMPOS y por lo tanto son
        // BASE. Mezclar una posicion efectiva con una rotacion base deja la ida y vuelta
        // mundo<->local descuadrada, y ademas mete la camara en un pos que va a disco.
        Vector3 GetGlobalPositionBase() const;
        // transforma un punto LOCAL a MUNDO (cadena completa de matrices de padres)
        Vector3 LocalAMundo(const Vector3& local) const;
        Vector3 LocalAMundoBase(const Vector3& local) const;   // idem con la BASE
        // punto de FOCO/pivot del objeto en MUNDO. Por defecto el origen; la Mesh
        // lo override al CENTRO GEOMETRICO (promedio de sus grupos de vertice), asi
        // "." enfoca el centro de la geometria y no el origen (clave en .obj importados).
        virtual Vector3 PuntoFoco() const { return GetGlobalPosition(); }
        // radio del bounding (en MUNDO) alrededor de PuntoFoco. Lo usa el encuadre del foco ('.') para
        // ajustar el zoom. Default 0 = un punto (empties/luces/camaras); Mesh lo override con su tamaño.
        virtual float RadioFoco() const { return 0.0f; }

        Matrix4 BuildMatrix(const Vector3& pos, const Quaternion& rot, const Vector3& scale);

        virtual void Reload();
        void ReloadAll();

        virtual void RenderObject();
        void Render();
        // LOS HIJOS del traversal (Render los llama con la matriz de este objeto ya
        // puesta). Default: todos, en orden. Lo overridean LOD (elige UN hijo por
        // distancia a la camara) y Culling (saltea los que quedan fuera del frustum
        // y ordena los visibles de adelante hacia atras). Es virtual ACA y no en
        // RenderObject porque la decision es sobre los hijos, no sobre el dibujo
        // propio (mismo motivo por el que 'visible' vive en Render()).
        virtual void RenderHijos();

        // ====================================================================
        //  CONSTRAINTS: propiedades de ESTE objeto, no objetos aparte
        //
        //  Un stack (como los modificadores de la Mesh) que se evalua al vuelo en
        //  GetMatrix, POR VISTA, sin escribir nunca pos/rot/rotEuler/scale. Vacio en
        //  el 99% de los objetos y ese caso no paga nada (GetMatrix sale por el
        //  early-out de siempre). Ver objects/W3dConstraint.h para el por que.
        //
        //  Son punteros y NO valores: el panel de propiedades bindea &c->influencia
        //  a un PropFloat, y con un vector por VALOR un push_back realoca y ese
        //  float* queda apuntando a memoria liberada. Los borra ~Object.
        // ====================================================================
        std::vector<W3dConstraint*> constraints;
        int  constraintActivo;   // el seleccionado en la lista del panel. -1 = ninguno.
                                 // Es estado de UI (como modificadorActivo): no se guarda.
        // guardia de reentrada de la evaluacion (A copia de B y B copia de A): la
        // SEGUNDA entrada devuelve la transform base en vez de recursar para siempre.
        // 'mutable' porque GetMatrix es const y no cambia nada observable del objeto.
        mutable bool consEval;

        // accesores para que libs/WhiskUI pueda listar los constraints SIN ver la
        // clase (el mismo truco que Mesh::NombreModificador, ver PropList.cpp:58)
        int         ConstraintsCount() const;
        std::string NombreConstraint(int i) const;
        void        SetConstraintActivo(int i);
        // el UNDO no puede guardarse ni el puntero (la direccion se recicla) ni el
        // indice (Add/Remove/Move reordenan): resuelve por serial, como ModVivoEn.
        // NULL = ese constraint ya no esta -> el que pregunta no escribe nada.
        W3dConstraint* ConstraintPorSerial(unsigned int s) const;

        // ====================================================================
        //  REFERENCIAS POR PUNTERO A OTRO OBJETO - LA PUERTA UNICA
        //
        //  Varios objetos se guardan un Object* de OTRO objeto: toda la familia
        //  Target (Instance/Mirror/Camera/ObjetoScript) con su 'target',
        //  y la Camera ademas con su riel (Curve*). Son punteros que SOBREVIVEN
        //  al borrado del apuntado, o sea la clase de bug de siempre: el que
        //  borran se libera, el allocator recicla la direccion y la referencia
        //  cae sobre OTRO objeto, en silencio.
        //
        //  Antes cada clase resolvia esto por su cuenta con un DesvincularDe a
        //  mano, y el que maneja el BORRADO (DeleteUndo, main/undo/Undo.cpp) NO
        //  tenia forma de enumerarlas: solo conocia Modifier::target y
        //  Mesh::skinArmature, asi que la familia Target entera quedaba afuera
        //  (no se limpiaba al borrar ni se restauraba al deshacer, y el .w3d
        //  guardado apuntaba a un objeto que ya no estaba en el archivo).
        //
        //  Ahora se enumeran GENERICAMENTE, por indice: el destructor y el undo
        //  entran por el MISMO lugar y ninguno necesita conocer la clase
        //  concreta. RefObjetoNombre es el vinculo POR NOMBRE que acompania al
        //  puntero (targetName / RielName): ese es el que se GUARDA, asi que
        //  quien suelta el puntero tiene que decidir tambien que hace con el.
        //
        //  LA PUERTA ESTA PARTIDA EN DOS. Desde que CUALQUIER objeto puede tener
        //  constraints (y cada uno se guarda un Object* como fuente), las refs de
        //  un objeto son dos bloques:
        //      [0 .. RefsPropias()-1]      las de su clase (target, riel, ...)
        //      [RefsPropias() .. +n-1]     una por CONSTRAINT (fuenteObj/fuenteNombre)
        //  El bloque de constraints lo enumera la BASE, y ninguna subclase se tiene
        //  que acordar de encadenarlo:
        //   - RefsObjeto/RefObjeto/SetRefObjeto/RefObjetoNombre NO son virtuales y
        //     solo RUTEAN. Es a proposito: si siguieran siendo virtuales, una
        //     subclase que las overridee (todas las que guardan un target lo hacen) taparia
        //     las refs de constraints de su base EN SILENCIO y volveria el puntero
        //     colgado. Con la puerta partida, un 'override' sobre ellas ya no
        //     compila: el error salta en el build, no en el usuario.
        //   - lo que se override es el bloque PROPIO, abajo.
        //
        //  *** UNA CLASE NUEVA QUE GUARDE UN Object* IMPLEMENTA RefsPropias /
        //      RefPropia / SetRefPropia / RefPropiaNombre Y NO TOCA NADA MAS: ni
        //      el destructor, ni el undo, ni los constraints. ***
        // ====================================================================
        int     RefsObjeto() const;
        Object* RefObjeto(int i) const;
        void    SetRefObjeto(int i, Object* o);
        std::string* RefObjetoNombre(int i);

        // ---- lo que overridean las subclases (sus refs PROPIAS, indice desde 0) ----
        virtual int     RefsPropias() const { return 0; }
        virtual Object* RefPropia(int i) const { (void)i; return NULL; }
        virtual void    SetRefPropia(int i, Object* o) { (void)i; (void)o; }
        virtual std::string* RefPropiaNombre(int i) { (void)i; return NULL; }

        // cuando 'borrado' se va a destruir, los objetos que lo referencian
        // (ej: una Instance con target=borrado) tienen que soltar el puntero
        // para no quedar colgados. Sale SOLO de las refs de arriba; el nombre
        // NO se toca (el destructor no edita el modelo, solo evita el puntero
        // muerto: el que decide sobre el nombre es el comando de borrado).
        virtual void DesvincularDe(Object* borrado);
};

// ============================================================================
//  GANCHOS: no todo lo que puede referenciar a un objeto cuelga del ARBOL VIVO.
//
//  Los comandos del undo se quedan con subarboles DETACHADOS (ver DeleteUndo,
//  main/undo/Undo.cpp): estan fuera de SceneCollection, asi que DesvincularDelArbol no
//  los alcanza y ~Object los dejaria con un puntero a memoria liberada. Y no es solo el
//  undo: la UI del editor tambien se guarda punteros a objetos VIVOS que no salen del
//  arbol (el popup de color se queda con el elemento dueno de la paleta, el panel "Add"
//  con la malla que edita, y sus campos apuntan a MIEMBROS de esa malla y los ESCRIBEN).
//  Antes esto era UN solo puntero de funcion y el primero que enganchaba se quedaba con
//  el lugar; ahora es una LISTA: cada subsistema que guarde punteros a Object fuera del
//  arbol se registra y ~Object los avisa a todos. El motor sin editor no registra nada y
//  no paga nada. Se llama con el objeto que SE ESTA destruyendo.
//
//  OJO (lo que el gancho NO puede hacer): cuando ~Object corre, los destructores de las
//  clases DERIVADAS ya terminaron (~Mesh ya llamo a LiberarModificadores y libero sus
//  capas). O sea que desde aca NO se puede mirar el contenido de la parte derivada para
//  decidir que liberar: hay que llegar con esa cuenta ya hecha.
// ============================================================================
typedef void (*W3dDesvincularFn)(Object* borrado);
enum { W3dDesvincularMaxGanchos = 8 };
extern W3dDesvincularFn W3dDesvincularGanchos[W3dDesvincularMaxGanchos]; // NULL = slot libre
// registra un gancho (idempotente: registrar dos veces el mismo no lo duplica). Devuelve
// false si no quedaban slots. Pensado para llamarse desde un constructor global: el array
// arranca en NULL por inicializacion ESTATICA (constante), o sea antes de cualquier ctor.
bool W3dDesvincularRegistrar(W3dDesvincularFn f);

// ============================================================================
//  BORRAR UN OBJETO **CON SU SUBARBOL**
//
//  'delete obj' NO borra a los hijos: ~Object LOS PROMUEVE al padre (es a proposito, para
//  que borrar un nodo del medio no haga desaparecer la rama). O sea que un 'delete' pelado
//  sobre algo que tiene hijos deja siempre uno de estos dos:
//    - los hijos SUBEN a la escena (aparecen solos, en un momento que nadie explica), o
//    - si el padre del borrado era NULL -como nace TODO objeto de primer nivel, ver
//      Object::Object- no suben a ningun lado: quedan fuera de Childrens de todos y NO se
//      liberan nunca.
//  Cuando lo que se quiere es que el subarbol DESAPAREZCA (cerrar el proyecto, el destructor
//  del comando de borrado del undo) hay que usar esto. Como 'delete', NO se quita de
//  Parent->Childrens: eso lo hace el caller.
// ============================================================================
void W3dLiberarSubarbol(Object* raiz);

// Funciones auxiliares
// NOMBRE LEGIBLE del tipo de objeto (en espanol, sin acentos): "mesh", "luz", "camara", "ui",
// "texto", "imagen", "rect", "vacio", ... Es la UNICA fuente de verdad de esos nombres: la usa
// tipo() de lua. Nunca devuelve NULL (un tipo desconocido cae en "objeto").
const char* W3dNombreTipo(ObjectType t);
Object* FindObjectByName(Object* node, const std::string& name);
// raiz del ESPACIO DE NOMBRES de 'o': su escena UI, o SceneCollection. Ver Objects.cpp.
Object* W3dNombreScopeDe(Object* o);
// true mientras un loader esta armando la escena (los nombres entran crudos y
// se reparan de una sola vez al final). Ver Objects.cpp / W3dNombresRepararEscena.
extern bool W3dNombresCargando;
bool DetectLoop(Object* node,
                std::set<Object*>& visited,
                std::set<Object*>& stack,
                int depth = 0);
void SearchLoop();

// ROTACION GLOBAL de un objeto: la acumulada por la cadena de padres. Consulta pura
// del grafo (lee los CAMPOS, no las matrices), la usan tanto el transform del editor
// como los builds sin editor (el runtime de un juego linkea el importador, que la usa).
Quaternion RotGlobalDe(Object* o);
Object* GetDeepestDFS(Object* node);
Object* GetPrevDFS(Object* current);
Object* GetNextDFS(Object* current);
bool IsSelectable(Object* obj, bool IncluirColecciones = false);

// enum class portable (mismo idiom que ObjectType)
struct SelectMode {
    enum Enum { NextSingle, PrevSingle, NextAdd, PrevAdd };
    Enum v;
    SelectMode(Enum e) : v(e) {}
    operator Enum() const { return v; }
};
Object* GetFirstDFS();
void changeSelect(SelectMode mode, bool IncluirColecciones = false);

class SaveState {
    public:
        Object* obj;
        Vector3 pos;
        Quaternion rot;
        Vector3 rotEuler; // el euler CON sus vueltas: el quaternion no distingue 0 de 360, asi que sin esto
                          // cancelar un transform te devolvia la orientacion pero te comia los giros
        Vector3 scale;
        Vector3 worldPos; // posicion en MUNDO al empezar el transform (pivot rotar/escalar)
        // sin esto, olvidarse de llenar un campo dejaba BASURA de la pila: el auto key comparaba contra ella y
        // se inventaba keyframes. Con el constructor el olvido da un valor determinista y se ve enseguida.
        SaveState() : obj(0), scale(1,1,1) {}
};
extern std::vector<SaveState> estadoObjetos;

// El LOCAL de un transform (T*R*S). Lo usan Object::GetMatrix y el motion trail (que arma la matriz de un objeto
// en otro frame sin tocarlo): una sola composicion, imposible que diverjan.
Matrix4 W3dLocalTRS(const Vector3& pos, const Quaternion& rot, const Vector3& scale);

// Entre todas las formas equivalentes del euler 'e' (mismas rotacion), la mas parecida a 'ref'. Es lo que conserva
// las vueltas al re-derivar el euler de un quaternion.
Vector3 W3dEulerCercano(const Vector3& e, const Vector3& ref);

// ============================================================================
//  LA EVALUACION DE LOS CONSTRAINTS (ver objects/W3dConstraint.h)
//
//  Calcula la posicion y la rotacion LOCALES que le quedan a 'o' despues de correr
//  su stack, leyendo la vista bindeada (g_renderCam*, objects/CameraBase.h) para los
//  constraints que dependen de la camara. NO escribe NADA del objeto: la respuesta
//  sale por 'posOut'/'rotOut' y el que la pidio la usa y la tira.
//
//  Devuelve FALSE cuando no hay nada que aplicar (stack vacio, todo apagado,
//  influencia 0, sin fuente, o segunda entrada por un ciclo). En ese caso posOut/
//  rotOut NO se tocan y el que llama tiene que usar la transform BASE: asi
//  "constraint apagado" da la matriz base BIT A BIT y no una ida y vuelta por la
//  matriz del padre que la ensuciaria en el ultimo decimal.
//
//  La ESCALA no se toca nunca: el que llama compone con la scale del objeto.
// ============================================================================
bool W3dEvaluarConstraints(const Object* o, Vector3& posOut, Quaternion& rotOut);

// VER EN MODO EDICION: el objeto que el editor tiene en Edit Mode (NULL = ninguno). Mientras es el
// que se edita, sus constraints con 'mostrarEdit' apagado NO se aplican, asi la malla se dibuja y se
// edita en su transform real (ver W3dConstraint.h). El editor lo setea por una sola puerta
// (ActualizarEditMeshActivo, main/ui/ViewPorts/LayoutInput.cpp); sin editor queda en NULL.
void W3dConSetObjEditando(Object* o);

// true si el objeto tiene AL MENOS UN constraint que HOY cambia algo (mismo criterio exacto que usa
// el evaluador, incluido el filtro de Edit Mode). Existe para que los caminos del editor que
// preguntan "hace falta la transform efectiva?" no reimplementen el predicado y se vayan separando.
bool W3dObjTieneConstraintEfectivo(const Object* o);

// cuantas veces corrio el CUERPO del evaluador desde que arranco el proceso (las que pasaron el
// pre-pase; las que salen por lista vacia / todo apagado no cuentan). Es DIAGNOSTICO puro: el
// motor no lo lee nunca. Existe para poder afirmar en un test que una consulta sobre una cadena de
// N objetos con constraint cuesta O(N) -antes costaba O(2^N)-, que es una propiedad que no se ve
// mirando el resultado, solo el trabajo. Se puede poner en 0 antes de medir.
extern unsigned long g_consEvalCorridas;

// ============================================================================
//  LAS DOS OPERACIONES DE LISTA DEL STACK DE CONSTRAINTS
//
//  Viven en el CORE y no en el editor porque las dos son parte del dato:
//
//  - W3dConstraintsResolverNombres: el .w3d guarda la fuente POR NOMBRE
//    ('fuenteNombre'); el puntero 'fuenteObj' se resuelve una vez, cuando ya
//    existen TODOS los objetos de la escena (igual que targetName / RielName /
//    modArmature). El runtime de un juego carga el mismo archivo y necesita
//    exactamente esta pasada, asi que no puede vivir en main/.
//    Un nombre que no esta en la escena deja el puntero en NULL (el constraint
//    no hace nada) y AVISA: el nombre NO se borra, que es lo que permite que el
//    vinculo se recupere si el objeto vuelve (undo del borrado).
//
//  - W3dCopiarConstraints: duplicar un objeto tiene que duplicar su stack. Los
//    constraints son punteros de los que el objeto es DUENO (~Object los borra),
//    asi que una copia plana del vector daria dos duenos del mismo constraint y
//    un doble free. Cada entrada se copia con el copy-ctor IMPLICITO de
//    W3dConstraint, que es el que le da a la copia un SERIAL NUEVO (ver
//    W3dConSerial en objects/W3dConstraint.h) sin que nadie se tenga que
//    acordar. La FUENTE (fuenteObj + fuenteNombre) se comparte tal cual: como el
//    target de un modificador, la copia mira al mismo objeto.
//    'dst' se queda con lo que ya tenia y la lista de 'src' va al final.
// ============================================================================
void W3dConstraintsResolverNombres(Object* raiz);
void W3dCopiarConstraints(Object* dst, const Object* src);

// ORIENTACION de una matriz de mundo. Normaliza las 3 columnas de la base 3x3 (o sea
// que se come la escala, que es lo que se quiere) y arma el quaternion.
// Devuelve FALSE -y deja 'out' como estaba- si la base no se puede representar con un
// quaternion: alguna columna ~0 (escala cero) o determinante negativo (padre ESPEJADO).
// Hace falta porque Quaternion::FromMatrix ASUME una base ortonormal y con una que no
// lo es devuelve basura EN SILENCIO, que es la peor forma de fallar.
bool W3dQuatDeMatriz(const Matrix4& m, Quaternion& out);

// diferencia 'destino - origen' llevada al rango (-180, 180]: el camino CORTO entre dos
// angulos en grados. Es el equivalente en euler de lo que hace el slerp con dot<0.
float W3dDifAngCorta(float destino, float origen);

void ApagarLucesHijas(Object* obj);
void SetDesplegado(bool desplegado);
void ChangeVisibilityObj();
void ChangeRenderizableObj();     // toggle visibilidad de RENDER del activo (outliner N95, tecla 3)
void OutlinerColapsarIzquierda(); // outliner: plegar; si ya plegado, subir al padre (tecla izquierda)
void SeleccionarTodo(bool IncluirColecciones = false);
// selecciona TODO siempre (no togglea como SeleccionarTodo): el menu Select>All
// y la tecla A
void SeleccionarTodoForzado(bool IncluirColecciones = false);
// invierte la seleccion: lo seleccionado se deselecciona y viceversa (Ctrl+I)
void InvertirSeleccion(bool IncluirColecciones = false);
bool HayObjetosSeleccionados(bool IncluirColecciones = false);

#endif
