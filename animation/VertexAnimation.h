#ifndef VERTEXANIMATION_H
#define VERTEXANIMATION_H

#ifdef _WIN32
    #define NOMINMAX
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <vector>
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>
#else
    #include <GL/gl.h>
#endif
#include "objects/Mesh.h"
#include "animation/Animation.h"   // AnimProperty: curvas propias de la animacion del objeto
#include <string>

// Un KEYFRAME de vertices: la malla completa capturada en UNA posicion del
// timeline. Antes los frames eran 1,2,3 contiguos (uno detras de otro); ahora
// cada frame vive en un cuadro ARBITRARIO (3, 5, 10...) y define como se
// interpola HACIA el siguiente (igual que las curvas de canal): constante
// (escalon), lineal (recta) o bezier (ease). El frame IZQUIERDO manda el tramo.
// CAPAS: un frame puede traer la capa de POSICIONES, la de NORMALES, la de UV, o
// cualquier combinacion. El flag por-frame es el propio puntero (NULL = no trae esa
// capa): la anim tiene TRES curvas separadas -"Vertices" (positions), "Normales"
// (normals) y "UV" (uvs)- y cada una se evalua SOLO con los frames que traen su capa.
// Insertar keyframe desde el 3D crea frames posiciones+normales (normalmente van
// juntas); desde el UV editor, solo-UV. La capa de normales es BORRABLE entera
// (VertexAnimBorrarCapaNormales): un modelo sin iluminacion no la necesita y la
// evaluacion se la saltea por completo. Si un frame trae varias capas, comparte
// frame/interp/handles entre sus curvas (es UN cuadro).
struct VertexFrame {
    // capa VERTICES: floats [x,y,z,...] (NULL = este frame no keyframea posiciones)
    const GLfloat* positions;
    // capa NORMALES: 3 bytes por vertice (NULL = este frame no keyframea normales).
    // Independiente de positions: puede haber frames solo-normales o solo-posiciones.
    const GLbyte* normals;
    // capa UV: 2 floats por VERTICE (u,v), paralelo a mesh->uv (por render-vert, ya
    // con las costuras splitteadas). NULL si este frame no keyframea UV. Se interpola
    // igual que las posiciones (morph): editar el mapeo en un keyframe y otro distinto
    // en otro -> el UV "resbala" entre ambos.
    const GLfloat* uvs;
    int frame;    // posicion en el timeline (antes era implicita por el indice)
    int interp;   // interpolacion del tramo que SALE de aca: KfConstant/KfLinear/KfBezier
    // handles del BEZIER (solo si interp==KfBezier). Como en el vertex anim no hay un
    // "valor", la curva editable es el PROGRESO entre pose y pose: el editor de curvas la
    // muestra con valor = indice del keyframe, y estos handles moldean el EASE. La mezcla
    // se clampea a [0,1] (no tiene sentido "pasarse" de la pose A o la B). Offsets desde
    // el keyframe en (frames, valor-indice), igual que keyFrame.
    int handleType;   // HFree/HAligned/HVector/HAuto/HAutoClamped
    float inDF, inDV, outDF, outDV;
    VertexFrame() : positions(NULL), normals(NULL), uvs(NULL), frame(0), interp(KfLinear),
                    handleType(HAuto), inDF(0.0f), inDV(0.0f), outDF(0.0f), outDV(0.0f) {}
};

class VertexAnimation {
    public:
        // Nombre de la animación
        std::string name;

        // Declaración (desde .w3d)
        std::string basePath;
        // defaults en el constructor de abajo (C++03)
        int frameCount;
        int proximaAnimacion;
        float speed;
        int padding;
        bool repeat;
        // rango/fps PROPIOS de esta animacion (cada animacion del objeto los suyos).
        // Default 1..250 @ 30 (250 cuadros: tener 1 no tiene sentido). Los usa el
        // timeline al seleccionar la animacion, y se guardan con ella.
        int startFrame;
        int endFrame;
        int fps;

        // Runtime
        Mesh* target;

        // espejo de la capa de NORMALES: hay frames con normales? Se prende al capturar
        // keyframes con normales y lo apaga VertexAnimBorrarCapaNormales. Como UseUV,
        // manda lo que realmente hay guardado en los frames.
        bool UseNormals;

        // igual que UseNormals pero para las UV: si esta activo, cada keyframe guarda tambien
        // las coordenadas de textura y EvalVertexAnim las interpola (UV morph). Se prende cuando
        // la malla tiene UV al crear la anim / capturar el primer keyframe.
        bool UseUV;

        // cantidad de VERTICES para la que estan armados los frames (las posiciones/normales
        // de cada keyframe son paralelas a mesh->vertex). Si la topologia cambia (editar la
        // malla: borrar/subdividir/extrude), hay que remapear los frames a este nuevo conteo
        // o la evaluacion leeria fuera de rango (crash). 0 = sin definir. Ver RemapVertexAnims.
        int vcount;

        // EL BLOB DE FRAMES ESTABA Y NO SE PUDO LEER (hermano de W3dAjenos::noCargo de la
        // malla). La anim se declaraba en el .w3d con su entrada "buffers" y esa entrada no
        // se pudo abrir o no deserializo: en memoria queda con 0 frames. Guardar encima la
        // BORRABA para siempre (el escritor no emite "buffers" si no hay frames y el
        // contenedor descarta lo huerfano bajo "animaciones/"), y todo eso en silencio. Con
        // el flag prendido el guardado se FRENA, igual que con una malla que no cargo.
        bool noCargo;

        // Frames de la animación (solo posiciones) — la parte VERTEX ANIMATION
        std::vector<VertexFrame*> frames;

        // ---- FRAMES COMPARTIDOS (io/W3dRecursos.h, tipo ANIM): si datosRec !=
        // NULL, los VertexFrame de 'frames' pertenecen a un AnimDatos del
        // almacen de recursos y NO se poseen: N instancias del mismo basePath
        // usan UN solo juego de frames (el ahorro grande: cada frame es la
        // malla entera). COW: toda mutacion de keyframes (SetKey/Borrar*/
        // Mover*/Remap) llama antes a DesinstanciarFrames(). La POSE no vive
        // aca (se escribe en mesh->vertex, por instancia): el playback NO
        // desinstancia frames.
        void* datosRec;             // W3dRecurso* (tipo ANIM); NULL = propios
        void DesinstanciarFrames(); // copia profunda + suelta la ref (no-op si propios)

        // curvas PROPIAS de esta animacion del objeto: transform (pos/rot/escala),
        // visible, render... Una "animacion de objeto" es un CONTENEDOR: puede
        // tener frames de vertices Y/O curvas de canal (cada animacion las suyas,
        // distintas de la animacion de ESCENA). Se evaluan cuando esta activa (kind 3).
        std::vector<AnimProperty> curvas;

        VertexAnimation()
            : frameCount(0), proximaAnimacion(-1), speed(1.0f), padding(0),
              repeat(true), target(NULL), UseNormals(false), UseUV(false), vcount(0),
              noCargo(false), startFrame(1), endFrame(250), fps(30) {
            // frames PROPIOS (como el otro constructor): sin esto datosRec quedaba con
            // basura y ~VertexAnimation -> LiberarFrames "soltaba" un recurso inexistente
            datosRec = 0;
        }
        VertexAnimation(Mesh* tgt, const std::string& animName, bool useNormals = false, float Speed = 1, bool Repeat = true, int ProximaAnimacion = -1);
    
        ~VertexAnimation();          // libera los frames (posiciones/normales new[])
        void LiberarFrames();

        // Cargar animaciones desde archivos .obj
        bool LoadFrames();

        size_t FrameCount() const { return frames.size(); }
};

class VertexAnimationActive {
    public:
        // defaults en el constructor (VertexAnimation.cpp), C++03
        Mesh* meshToAnim;

        int currentAnim;   // animación activa (idle/run o la que sea)
        int nextAnim;      // animación a la que queremos ir

        int currentFrame;   // legacy (modelo viejo 1,2,3 contiguo); lo mantiene playFrame
        int nextFrame;      // legacy

        float blendStep;    // legacy (ticks de 120 Hz del loop viejo)

        // POSICION de reproduccion en el timeline (float): la anim corre por el
        // rango startFrame..endFrame @ fps y se evalua con EvalVertexAnim. Reemplaza
        // al viejo avance por indice de frame (currentFrame/nextFrame/blendStep).
        float playFrame;

        VertexAnimationActive(Mesh* mesh);

        void UpdateAnimation(float dtSeg);   // dt REAL: la anim no depende de los fps

        // CAMBIO DE CLIP EN EL ACTO, arrancando en un cuadro elegido.
        // `animar(obj, n)` deja el cambio PENDIENTE: el clip nuevo entra recien
        // cuando el actual llega a su final, que es lo correcto para encadenar
        // (saludar -> volver al idle). El EMPALME POR PIE de una locomocion
        // (el caminar <-> correr de una locomocion horneada, que empalma en cuadros
        // concretos) necesita lo contrario: cortar AHORA y caer en el cuadro donde
        // el pie del clip destino esta en la misma fase.
        // 'frame' se clampea al rango del clip destino. Reposa la malla en el acto
        // (no espera al proximo UpdateAnimation) para que el cuadro que pidio el
        // script sea el que se ve en ESTE tick.
        void CambiarYa(int clip, float frame);
};

// ---- A/B del cache de frames compartidos (comando 'animcache off|on' del
//      harness) + contador de desinstancias por COW (lo muestra meminfo) ----
void AnimsCacheActivo(bool on);
bool AnimsCacheEstaActivo();
int  AnimsDesinstanciadas();

// Mezcla dos animaciones y escribe en la malla target
// - fromAnim: animación actual (ej: idle)
// - toAnim: animación destino (ej: run)
// - fromFrame: frame actual en fromAnim
// - blendT: 0..1 (0 = solo fromAnim, 1 = solo toAnim)
// - toFrame: frame en toAnim (usualmente 0 al comenzar la mezcla)
void BlendVertexAnimations(
    const VertexAnimation& fromAnim,
    const VertexAnimation& toAnim,
    size_t fromFrame,
    size_t toFrame,
    float blendT,
    Mesh* mesh
);

// Copia directa de un frame a la malla target (sin mezcla), por INDICE
void ApplyVertexFrame(
    const VertexAnimation& anim,
    size_t frameIndex
);

// Evalua la vertex anim a una POSICION del timeline (float) y escribe la malla.
// CADA CAPA por su cuenta: las posiciones interpolan entre los frames que traen
// positions, y las UV entre los que traen uvs (curvas separadas). El tramo lo
// manda el keyframe izquierdo (constante/lineal/bezier via EvalCurvaVertexAnim).
// Antes/despues del rango de cada capa: HOLD su extremo.
void EvalVertexAnim(const VertexAnimation& anim, Mesh* mesh, float frame);

// ===== EVALUADOR COMUN del EASE de la vertex/UV anim (SIN overshoot) =====
// La "curva" de un tramo A->B es el PROGRESO 0..1 entre pose y pose. Regla del
// dueno: en vertex/UV anim la curva NUNCA sobrepasa a sus keyframes ni retrocede
// (nada de overshoot/bounce del bezier). Se logra CLAMPEANDO los handles
// efectivos del tramo al rectangulo entre los dos keys (tiempo dentro del tramo,
// valor dentro de 0..1): una bezier con los 4 puntos de control en esa caja es
// monotona y queda acotada. Estas funciones las COMPARTEN la reproduccion
// (EvalVertexAnim) y el editor de curvas (VertexCanalTemp dibuja el trazo y los
// handles con los valores de VertexAnimHandlesTramo): lo que se VE es lo que
// corre, no pueden divergir. Las curvas de canal (objetos/huesos) NO pasan por
// aca: conservan su overshoot libre.
// clamp de UN handle al rectangulo del tramo. spanFrames = frames entre los dos
// keys; salida=true es el handle derecho del key izquierdo (dF>=0, dV en 0..1),
// salida=false el izquierdo del key derecho (dF<=0, dV en -1..0).
void VertexAnimClampHandle(bool salida, float spanFrames, float& dF, float& dV);
// handles EFECTIVOS del tramo A->B (resuelve Auto/Vector/etc + clamp), en la
// escala progreso 0..1
void VertexAnimHandlesTramo(const VertexFrame& A, const VertexFrame& B,
                            float& outDF, float& outDV, float& inDF, float& inDV);
// factor de mezcla 0..1 del tramo A->B en 'frame': monotono y sin overshoot
float EvalCurvaVertexAnim(const VertexFrame& A, const VertexFrame& B, float frame);

// inserta/actualiza un keyframe en la posicion 'frame' (ordenado). SOLO pisa las
// capas que vienen NO-NULL: positions(+normals) la capa de vertices, uvs la de
// UV; la otra capa de un frame existente se CONSERVA (insertar un keyframe de
// vertices no borra el de UV del mismo cuadro, y al reves). Con positions=NULL
// y uvs validos crea/actualiza un frame SOLO-UV. Copia los buffers (los toma
// prestados: hace su propia copia con new[]). Devuelve el indice del keyframe
// afectado (-1 si no habia nada que insertar).
int VertexAnimSetKey(VertexAnimation& anim, int frame,
                     const GLfloat* positions, const GLbyte* normals,
                     const GLfloat* uvs, int interp);

// borra el keyframe de vertices en 'frame' (libera sus buffers). true si borro alguno.
bool VertexAnimBorrarFrame(VertexAnimation& anim, int frame);
// borra UNA CAPA (0 = posiciones, 1 = uv, 2 = normales) del keyframe en 'frame'. Si el
// frame queda sin ninguna capa se borra entero. true si borro algo.
bool VertexAnimBorrarCapa(VertexAnimation& anim, int frame, int capa);
// borra la capa de NORMALES de TODOS los frames de la anim (los frames que quedan sin
// ninguna capa se van enteros) y apaga UseNormals -> EvalVertexAnim se saltea la
// interpolacion de normales por completo (rendimiento: modelos sin iluminacion estilo
// de los juegos de plataformas de PS1). El render queda con las normales que tenga la malla (estaticas);
// el llamador del editor las recalcula si quiere (Mesh::RecalcularNormales).
// true si borro algo.
bool VertexAnimBorrarCapaNormales(VertexAnimation& anim);
// ===== MOVE SELECTIVO por MASCARA de vertices (toggle "Solo vertices seleccionados") =====
// Mueve n keyframes (origen[i] -> destino[i]) aplicando la mascara 'mask' (vcount
// entradas, 1 = vertice seleccionado). Para CADA capa que traiga el frame movido:
//  - destino OCUPADO por un frame con esa capa -> MERGE SELECTIVO: en el destino, los
//    valores de los vertices de la mascara se pisan con los del movido; el resto del
//    destino queda como estaba (interp/handles del destino tampoco cambian).
//  - destino VACIO (o sin esa capa) -> se crea el keyframe COMPLETO (obligatorio: un
//    keyframe guarda TODOS los verts): los de la mascara con el valor del movido y los NO
//    seleccionados con su valor EVALUADO en ese frame (la interpolacion vigente ANTES de
//    mover: no saltan). Un frame nuevo hereda interp/handles del movido.
// EL FRAME ORIGEN (regla exacta): NO se borra a ciegas. Se le SACA LA CONTRIBUCION de los
// vertices seleccionados -cada uno pasa a valer lo que valdria ahi SIN este keyframe, o
// sea la interpolacion de los vecinos de su capa-, y recien si con eso la capa entera
// queda REDUNDANTE (todos sus verts caen sobre esa interpolacion: ya no aporta nada
// distinto) se borra la capa; el frame se borra cuando se queda sin ninguna. Con la
// mascara COMPLETA eso siempre termina borrando el frame -> el move de siempre. Con un
// subconjunto, los vertices NO seleccionados conservan SU keyframe en el origen (que es
// justamente lo que promete el toggle: no tocar lo que no elegiste), y los seleccionados
// dejan de estar keyframeados ahi -> asi se ANIMA o se DES-ANIMA un subconjunto.
// Todas las mediciones (evaluaciones) se hacen contra el estado PREVIO al move, en una
// pasada aparte, asi mover varios keys a la vez no se contamina.
// Devuelve true si movio algo.
bool VertexAnimMoverKeysSelectivo(VertexAnimation& anim,
                                  const std::vector<int>& origen,
                                  const std::vector<int>& destino,
                                  const unsigned char* mask);
// conveniencia: UN solo keyframe (origen -> destino) con la misma semantica.
bool VertexAnimMoverKeySelectivo(VertexAnimation& anim, int origen, int destino,
                                 const unsigned char* mask);
// El keyframe de 'frame' le ANIMA algo a los vertices de 'mask' en esa capa (0/1/2)? True si alguno de esos
// vertices vale ahi algo distinto de lo que la capa evaluaria SIN este keyframe (un cuadro que cae justo sobre la
// interpolacion de sus vecinos no anima nada: es un cuadro de paso). mask NULL = todos los vertices; un keyframe
// sin vecinos en su capa siempre aporta. Lo usa el FILTRO de vista del toggle "Solo vertices seleccionados" del
// dope/curvas, y es la misma regla con la que el move selectivo decide si el frame origen sobrevive.
bool VertexAnimKeyAportaSel(const VertexAnimation& anim, int capa, int frame, const unsigned char* mask);
// interpolacion (KfConstant/KfLinear/KfBezier) del keyframe en 'frame'. No-op si no existe.
void VertexAnimSetInterp(VertexAnimation& anim, int frame, int interp);
// re-ordena los frames por su campo 'frame' (tras mover keyframes en el dope). SOLO
// ordena: NO borra ni fusiona (los punteros siguen todos vivos -> seguro durante el
// arrastre, donde un snapshot los referencia). Las colisiones (dos en el mismo cuadro)
// las resuelve el llamador al confirmar.
void VertexAnimOrdenar(VertexAnimation& anim);

// SERIALIZACION de los frames de vertices a un blob BINARIO compacto (para guardar en
// el .w3d y no perder las animaciones autoradas en el editor). Formato little-endian
// (PC y N95/ARM lo son): magic "W3DV" + version + vertexCount + useNormals [+ useUV v3+]
// + nFrames + por frame (frame, interp, [handles v2+], [mascara de CAPAS v4+] y los
// buffers de las capas presentes). Mascara v5: bit0 = posiciones, bit1 = uv, bit2 =
// NORMALES (capa propia, borrable); los bloques van en orden posiciones, normales, uv.
// v4 no tenia bit2: sus normales viajaban PEGADAS a las posiciones (si useNormals).
// Leer v1..v3 asume el layout viejo (posiciones siempre; uv en todos los frames si
// useUV); leer v4 asume normales-con-posiciones -> retrocompat total.
// Serializar vuelca anim.frames a 'out'. Deserializar reconstruye anim.frames (libera
// los previos) leyendo 'in'; devuelve false si el blob no valida. 'vertexCount' es la
// cantidad de vertices de la malla (se valida contra el header).
void VertexAnimSerializar(const VertexAnimation& anim, std::vector<unsigned char>& out);
bool VertexAnimDeserializar(VertexAnimation& anim, const unsigned char* in, size_t n);

void LoadVertexFrames(Mesh* mesh);

extern std::vector<VertexAnimationActive*> VertexAnimationActives;

// hooks de la LISTA de animaciones del OBJETO (tarjeta del editor, PropList modo 7):
// elegir una anim en la lista + leer cual es la activa. NULL sin editor.
extern void (*OnSeleccionarAnimVertex)(Mesh* m, int idx);
extern int  (*ActivaAnimVertexDe)(Mesh* m);

VertexAnimationActive* FindTargetAnim(Mesh* target);

// COPIA PROFUNDA de una vertex anim (frames + curvas), para DUPLICAR / SEPARAR una malla
// animada. Los VertexFrame son duenios de sus buffers -> una copia plana daria doble free.
// Analoga a Arm2DClonar (armature 2D). 'nuevoTarget' es la malla de la copia.
VertexAnimation* VertexAnimClonar(const VertexAnimation* src, Mesh* nuevoTarget);

// ---- FRAMES COMPARTIDOS ENTRE COPIAS (las instancias de un prefab) ----
// Como los de LoadFrames: el juego de frames vive en el almacen (tipo ANIM), cada anim que lo usa
// tiene una ref, y cualquier edicion de keyframes pasa antes por DesinstanciarFrames (COW). Asi 50
// instancias de un prefab con una vertex anim tienen UN solo juego de frames en memoria.
// POR CLAVE: N mallas que leen el MISMO blob de frames. La primera lo deserializa y lo REGISTRA bajo
// 'clave' (la arma el lector: la entrada del blob + su medida + su CRC); las demas lo ADOPTAN sin
// deserializar (true), siempre que los frames esten armados para 'vcountMalla' vertices.
bool VertexAnimAdoptarClave(VertexAnimation& anim, const std::string& clave, int vcountMalla);
void VertexAnimRegistrarClave(VertexAnimation& anim, const std::string& clave);
// CON OTRA ANIM: una copia de 'src' para 'nuevoTarget' (como VertexAnimClonar) que COMPARTE sus frames.
// Si los de 'src' eran propios (una anim sin guardar) pasan antes al almacen con un id de memoria y
// 'src' queda compartiendolos tambien. Sin frames (o con el cache apagado) es una copia profunda.
VertexAnimation* VertexAnimCompartir(VertexAnimation* src, Mesh* nuevoTarget);

// PUNTO UNICO de coherencia vertex-anim vs TOPOLOGIA: remapea los frames de vertices de
// TODAS las anims de 'mesh' tras un cambio de topologia (borrar/subdividir/loop cut/
// extrude/duplicar/rip/etc). Toda op de edicion desemboca en Mesh::GenerarRender, y
// GenerarRender llama aca con el remapeo que ya calcula -> ninguna op tiene que sincronizar
// frames a mano. Contrato:
//  - newToOld[gi] = indice del vertice GPU VIEJO del que sale el nuevo gi (los frames estan
//    armados para ese layout viejo, de an->vcount verts). >= vcount o < 0 = vertice NUEVO.
//  - los vertices EXISTENTES siguen la animacion (se copia su valor de cada keyframe);
//  - un vertice NUEVO con RECETA (mesh->vtxAnimRecipes: loop cut/subdivision, (a,b,t)) se
//    recalcula EN CADA keyframe como lerp(vert a, vert b, t) -> el corte queda ANIMADO;
//  - un vertice NUEVO sin receta (extrude/duplicar) queda ESTATICO en la pose actual en
//    todos los keyframes (fallback documentado: consistente, sin animar hasta keyframearlo).
void RemapVertexAnims(Mesh* mesh, const int* newToOld, int newVcount, int oldVcount);

// SNAPSHOT por valor de los BUFFERS de los keyframes de TODAS las vertex anims de una
// malla (posiciones/normales/uv + vcount). Para los flujos RESTAURAR-Y-REAPLICAR del
// editor (slide del loop cut, panel redo, undo de topologia): restaurar la geometria
// PRE-op sin restaurar los frames dejaba los frames ya remapeados al layout NUEVO sobre
// una malla con el layout VIEJO; el proximo RemapVertexAnims los leia DESALINEADOS y la
// malla animada se rompia (bug: loop cut + slide sobre un cubo con vertex animation).
// Snapshotear JUNTO con la geometria pre-op; Restaurar JUNTO con la geometria.
struct VtxAnimSnap {
    struct Frame { std::vector<GLfloat> pos; std::vector<GLbyte> nrm; std::vector<GLfloat> uv; };
    struct Anim  { int vcount; std::vector<Frame> frames; Anim() : vcount(0) {} };
    std::vector<Anim> anims;
    bool tiene;                      // hubo snapshot (aunque la malla no tenga anims)
    VtxAnimSnap() : tiene(false) {}
};
void VertexAnimSnapshot(Mesh* mesh, VtxAnimSnap& out);
// escribe los buffers del snapshot de vuelta en los keyframes vivos (misma estructura de
// anims/frames; si una anim cambio de cantidad de keyframes en el medio, se saltea).
void VertexAnimRestaurarSnap(Mesh* mesh, const VtxAnimSnap& snap);

// remapea los frames de 'anim' al conteo de vertices de mesh MATCHEANDO POR POSICION contra
// el keyframe BASE (frames[0]). Lo usa la CARGA del .w3d: el GLB re-splitea los verts (32->60
// por costuras UV/normal) y el blob de la anim quedaria desalineado; se exporta el GLB en la
// pose base y aca cada vert de la malla se asocia al vert del blob mas cercano en esa pose.
void VertexAnimRemapPorPosicion(VertexAnimation& anim, Mesh* mesh);

// saca (y libera) el controlador de reproduccion de 'mesh' de VertexAnimationActives.
// Lo llama ~Mesh: sin esto el controlador queda con meshToAnim COLGANDO tras borrar la
// malla (borrar objeto o reabrir proyecto) y UpdateAnimations lo dereferencia -> crash.
void VertexAnimLiberarControlador(Mesh* mesh);

void UpdateAnimations(float dtSeg = 1.0f / 60.0f);
// posa la vertex-anim ACTIVA en el timeline (kind 3) al frame del playhead (CurrentFrame). Se llama por frame;
// re-evalua solo al cambiar de malla/anim/frame. En PC vive inline en main.cpp; el loop de Symbian la invoca.
void AplicarVertexAnimTimeline();
void NewActiveVertexAnimation(Mesh* mesh, VertexAnimation* anim);

// ============================================================================
//  W3dReposoVertexAnim: LA MALLA EN REPOSO MIENTRAS DURA EL SCOPE
//
//  LO QUE SE GUARDA A DISCO ES LA MALLA EN REPOSO, NUNCA LA POSE DEL CUADRO.
//  El playhead es estado de la SESION, no del modelo: si el usuario mueve el
//  playhead y guarda, el archivo tiene que salir igual que si no lo hubiera
//  movido. Sin esto pasaban las dos mitades del mismo desastre:
//    - la geometria se HORNEABA POSADA (el cubo de +-1 se guardaba como la pose
//      de +-10, y al reabrir esa deformacion ERA el modelo), y
//    - las claves de sharp/seam (que viven en el espacio de REPOSO) no
//      resolvian contra las posiciones POSADAS -> el escritor las daba por
//      perdidas y NO ESCRIBIA NI UNA MARCA. Ese es, literal, el sintoma que
//      reporto el dueno: marcar, animar, y perder las marcas.
//  El camino viejo por GLB evaluaba el frame base antes de exportar; al
//  conectar el .w3dm ese paso se perdio y nadie lo puso de vuelta.
//
//  USO: abrir el guard, escribir, y al cerrarse la pose vuelve EXACTA (se
//  restauran vertex/normals/uv byte a byte, no se re-evalua nada) junto con el
//  flag de pose. Gratis si la malla NO esta posada (que es el caso normal): no
//  copia ni un buffer. NO re-ancla las marcas a proposito: ir de la pose al
//  reposo no es "mover vertices", es volver al espacio en el que las claves ya
//  estaban escritas.
// ============================================================================
class W3dReposoVertexAnim {
    public:
        explicit W3dReposoVertexAnim(Mesh* m);
        ~W3dReposoVertexAnim();
    private:
        Mesh* malla;
        bool  restaurar;                 // habia una pose que devolver
        std::vector<GLfloat> pos;        // vertex[] posado (vertexSize*3)
        std::vector<GLbyte>  nrm;        // normals[] posado (vertexSize*3)
        std::vector<GLfloat> uvs;        // uv[] posado (vertexSize*2)
        W3dReposoVertexAnim(const W3dReposoVertexAnim&);            // sin copia (C++03)
        W3dReposoVertexAnim& operator=(const W3dReposoVertexAnim&);
};

#endif