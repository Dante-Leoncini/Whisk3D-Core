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

#include "base/W3dInteractionState.h" // InteractionMode/estado (el Core NO depende del header del editor)

// Dialecto C++03 compartido (PLAN-UNIFICACION.md): este header compila tanto
// en PC/Android (C++11) como en Symbian/RVCT 2.2 (C++03). Por eso:
//  - enums "tipo enum class" con el idiom struct+Enum (misma sintaxis de uso)
//  - sin inicializadores de miembro en la clase (van al constructor)
//  - NULL en vez de nullptr, std::set en vez de unordered_set

// 'override' es C++11: en compiladores viejos (RVCT 2.2) se anula; en PC
// sigue siendo el keyword real con su chequeo
#if defined(__cplusplus) && __cplusplus < 201103L
    #define override
#endif
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
        collection, baseObject, mirror, gamepad, instance, constraint
    };
    Enum v;
    ObjectType(Enum e) : v(e) {}
    operator Enum() const { return v; }
};

// Forward declarations
class Object;
extern Object* SceneCollection;
extern std::vector<Object*> ObjSelects;
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
        bool desplegado;
        bool showRelantionshipsLines;
        bool select;
        std::string name;

        Matrix4 M;

        Vector3 pos;
        Quaternion rot;        // la rotacion REAL (siempre)
        Vector3 scale;
        Vector3 rotEuler;      // display en grados (modos Euler)
        int rotMode;           // 0=XYZ Euler, 1=Quaternion, 2=Axis Angle
        float rotAngle;        // display Axis Angle: angulo en grados
        Vector3 rotAxis;       // display Axis Angle: eje
        // refresca los valores de DISPLAY (rotEuler / rotAxis+rotAngle) desde el
        // quaternion 'rot'. Se llama al rotar en la vista y al seleccionar.
        // Re-deriva rotEuler y rotAngle/rotAxis del quaternion. El euler CONSERVA sus vueltas (ver Objects.cpp):
        // se elige la forma equivalente mas parecida a la que ya tenia.
        void ActualizarDisplayRot();
        // Pone la rotacion desde un euler que YA trae sus vueltas (curva de animacion / panel): el euler manda y el
        // quaternion se deriva. NO usar rot=... + ActualizarDisplayRot para esto: ahi el euler seria solo "a que
        // parecerse" y una vuelta entera se perderia.
        void SetRotEuler(const Vector3& e);

        virtual ObjectType getType() { return ObjectType::baseObject; }

        Object(
            Object* parent,
            const std::string& nombre = "Objeto",
            Vector3 Pos = Vector3(0,0,0),
            Vector3 Rot = Vector3(0.0f, 0.0f, 0.0f),
            Vector3 scale = Vector3(1.0f, 1.0f, 1.0f)
        );
        virtual ~Object();

        void SetNameObj(const std::string& nombre);

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

        //todo lo nuevo para tranformaciones locales/globales y quaternion
        void GetMatrix(Matrix4& out) const;        // matriz LOCAL del objeto (T*R*S)
        // matriz de MUNDO: GetMatrix + toda la cadena de padres (T*R*S de cada uno).
        // Es la UNICA fuente de la transform de mundo: la usan GetGlobalPosition,
        // LocalAMundo, el foco y el color-pick de edit mode. NO reimplementar la
        // cadena a mano (un pick que la rearmaba con "while(Parent)" salteaba la
        // matriz del propio objeto top-level -Parent NULL- y rompia a escala!=1).
        void GetWorldMatrix(Matrix4& out) const;
        void RotateLocal(float pitch, float yaw, float roll);
        Vector3 GetGlobalPosition() const;
        // transforma un punto LOCAL a MUNDO (cadena completa de matrices de padres)
        Vector3 LocalAMundo(const Vector3& local) const;
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

        // cuando 'borrado' se va a destruir, los objetos que lo referencian
        // (ej: una Instance con target=borrado) tienen que soltar el puntero
        // para no quedar colgados. Por defecto nada; Instance lo override.
        virtual void DesvincularDe(Object* borrado) { (void)borrado; }
};

// Funciones auxiliares
Object* FindObjectByName(Object* node, const std::string& name);
bool DetectLoop(Object* node,
                std::set<Object*>& visited,
                std::set<Object*>& stack,
                int depth = 0);
void SearchLoop();
size_t GetIndexInParent(Object* obj);
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

void ApagarLucesHijas(Object* obj);
void SetDesplegado(bool desplegado);
void ChangeVisibilityObj();
void SeleccionarTodo(bool IncluirColecciones = false);
// selecciona TODO siempre (no togglea como SeleccionarTodo): el menu Select>All
// y la tecla A
void SeleccionarTodoForzado(bool IncluirColecciones = false);
// invierte la seleccion: lo seleccionado se deselecciona y viceversa (Ctrl+I)
void InvertirSeleccion(bool IncluirColecciones = false);
bool HayObjetosSeleccionados(bool IncluirColecciones = false);

#endif
