#include "Objects.h"
#include "script/W3dScript.h" // W3dScriptDatos completo (el destructor lo libera)
#include "w3dGraphics.h" // flags de estado de render (w3dRenderLuces) — PC y Symbian

// RAIZ de la escena. La DEFINE el Core (antes solo la declaraba y la definia el editor, asi que
// nadie podia enlazar contra el motor sin traerse el editor entero). El editor la llena con su
// propia raiz (una Scene) en el arranque; para el motor alcanza con que exista y valga 0.
Object* SceneCollection = 0;

#ifdef W3D_SYMBIAN
// Symbian: GLES + utilidades de C que en PC llegan por otros headers.
#include <GLES/gl.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdlib.h>   // atoi
#include <ctype.h>    // isdigit
#endif

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
      desplegado(true),
      showRelantionshipsLines(true),
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

    rotMode = RotEulerXYZ;       // default: XYZ Euler
    rotAngle = 0.0f;
    rotAxis = Vector3(0, 0, 1);
    scriptDatos = NULL;          // script lua opcional (ver script/W3dScript.h)
    ActualizarDisplayRot();

    if (Parent) {
        Parent->Childrens.push_back(this);
    } else if (SceneCollection && this != SceneCollection) {
        SceneCollection->Childrens.push_back(this);
    }

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
}

// Pone la rotacion desde un EULER que ya trae sus vueltas (la curva de animacion, el panel): el euler MANDA y el
// quaternion se deriva de el. Usarla en vez de escribir rot y llamar a ActualizarDisplayRot: ese camino tomaria el
// euler como "a que se parece" en vez de como el dato.
void Object::SetRotEuler(const Vector3& e){
    rotEuler = e;
    rot = Quaternion::FromEulerXYZ(e.x, e.y, e.z);
    rot.ToAxisAngle(rotAngle, rotAxis);
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
    // antes de irse: soltar cualquier puntero que apunte a este objeto (las
    // instancias linkeadas a el) para evitar punteros colgados al renderizar
    if (SceneCollection && SceneCollection != this)
        DesvincularDelArbol(SceneCollection, this);

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

void Object::SetNameObj(const std::string& nombre) {
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

// helpers de SetName como funciones estaticas (eran lambdas con
// std::function: C++11; asi compila igual en PC y en RVCT/C++03)
static bool NameExistsInTree(const Object* obj, const Object* me, const std::string& name) {
    if (!obj) return false;

    // Si este objeto NO es 'me' y tiene nombre igual -> existe
    if (obj != me) {
        if (obj->name == name) return true;
    }

    for (size_t i = 0; i < obj->Childrens.size(); i++) {
        if (NameExistsInTree(obj->Childrens[i], me, name)) return true;
    }

    return false;
}

static bool NameExists(const Object* me, const std::string& name) {
    if (!SceneCollection) return false;
    return NameExistsInTree(SceneCollection, me, name);
}

std::string Object::SetName(const std::string& baseName) {
    if (!SceneCollection) return baseName;

    // conservar puntero a este objeto para ignorarlo en la búsqueda
    const Object* me = this;

    // ----------------------------
    // Si el nombre base NO existe → usarlo directamente
    // ----------------------------
    if (!NameExists(me, baseName)) {
        return baseName;
    }

    // ----------------------------
    // 4) Particionar si termina en ".NNN"
    // ----------------------------
    std::string root = baseName;
    int startCounter = 1;

    size_t pos = baseName.find_last_of('.');
    if (pos != std::string::npos && pos + 1 < baseName.size()) {
        bool digits = true;
        for (size_t i = pos + 1; i < baseName.size(); ++i) {
            if (!isdigit((unsigned char)baseName[i])) { digits = false; break; }
        }

        if (digits) {
            root = baseName.substr(0, pos);
            // atoi en vez de std::stoi (C++11); con digits ya validado no
            // hay caso de error
            startCounter = atoi(baseName.c_str() + pos + 1) + 1;
            if (startCounter < 1) startCounter = 1;
        }
    }

    // ----------------------------
    // 5) Buscar el primer ".NNN" libre
    // ----------------------------
    int counter = startCounter;
    std::string newName;

    do {
        std::ostringstream ss;
        ss << root << "." << std::setw(3) << std::setfill('0') << counter;
        newName = ss.str();
        counter++;
    } while (NameExists(me, newName));

    return newName;
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

void Object::GetMatrix(Matrix4& out) const {
    out = W3dLocalTRS(pos, rot, scale);
}

void Object::RotateLocal(float pitch, float yaw, float roll){
    Quaternion qPitch = Quaternion::FromAxisAngle(1,0,0, pitch);
    Quaternion qYaw   = Quaternion::FromAxisAngle(0,1,0, yaw);
    Quaternion qRoll  = Quaternion::FromAxisAngle(0,0,1, roll);

    rot = rot * qYaw * qPitch * qRoll;
}

// matriz de MUNDO del objeto: su matriz LOCAL (T*R*S) por toda la cadena de
// padres. Fuente UNICA de la transform de mundo (foco, LocalAMundo, color-pick).
void Object::GetWorldMatrix(Matrix4& out) const {
    GetMatrix(out);                 // empieza por la matriz del PROPIO objeto
    const Object* q = Parent;
    while (q) {
        Matrix4 P;
        q->GetMatrix(P);
        out = P * out;              // ...y le va anteponiendo cada padre
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

// transforma un punto LOCAL a MUNDO usando la matriz de mundo completa
Vector3 Object::LocalAMundo(const Vector3& local) const {
    Matrix4 M;
    GetWorldMatrix(M);
    // M es column-major (m[12..14] = traslacion): mundo = M * (local, 1)
    float x = M.m[0]*local.x + M.m[4]*local.y + M.m[8]*local.z  + M.m[12];
    float y = M.m[1]*local.x + M.m[5]*local.y + M.m[9]*local.z  + M.m[13];
    float z = M.m[2]*local.x + M.m[6]*local.y + M.m[10]*local.z + M.m[14];
    return Vector3(x, y, z);
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
    // Guardar la matriz actual (por la abstraccion: glPushMatrix en desktop, stack propio en ES2/WebGL)
    w3dEngine::PushMatrix();

    GetMatrix(M);
    w3dEngine::MultMatrix(M.m);   // aplica T * R * S -> incluye traslación

    // Si es visible y no es un mesh, lo dibuja
    RenderObject();

    // Procesar cada hijo
    for (size_t c = 0; c < Childrens.size(); c++) {
        Childrens[c]->Render();
    }

    // Restaurar la matriz previa
    w3dEngine::PopMatrix();
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
// La clase base NO depende de los subtipos del editor: esos includes (Target, Constraint,
// Scene, Collection, Instance, Mirror, Gamepad, Curve, Camera) estaban MUERTOS. Solo usa
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

void SearchLoop(){
    std::set<Object*> visited;
    std::set<Object*> stack;

    std::cout << "Analizando árbol para loops..." << std::endl;

    if (DetectLoop(SceneCollection, visited, stack)) {
        std::cout << "\n🟥 RESULTADO: Hay bucles en la jerarquía!\n" << std::endl;
    } else {
        std::cout << "\n🟩 No se detectaron loops. Árbol sano.\n" << std::endl;
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

Object* GetPrevDFS(Object* current){
    if (!current) return NULL;

    Object* parent = current->Parent;

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
        Object* parent = node->Parent;

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

    const ObjectType t = obj->getType();

    // Si NO queremos incluir colecciones → bloquearlas
    if (!IncluirColecciones) {
        if (t == ObjectType::collection)
            return false;
    }

    // Siempre bloquear baseObject
    if (t == ObjectType::baseObject)
        return false;

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

    if (!next) {
        // Llegamos al final del DFS → wrap
        next = GetFirstDFS();
        if (!next) return;
    }

    // Buscar un selectable
    Object* it = next;
    while(it && !IsSelectable(it, IncluirColecciones)){
        it = (mode == SelectMode::PrevAdd)
               ? GetPrevDFS(it)
               : GetNextDFS(it);
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

//si hay objetos seleccionasos, devuelve true
bool HayObjetosSeleccionados(bool IncluirColecciones){
    return SceneCollection->EstaSeleccionado(IncluirColecciones);
}