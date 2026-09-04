//NOTA SUPER IMPORTANTE: mucho de este codigo NO va aca y se va a borrar/simplificar.....
//Casi todo tiene que ir en Whisk3D EDITOR. y no en el core.... de a poco voy a ir limpiando este codigo y moviendo las cosas
//si ves cosas como "edit mode" o "render preview" etc etc... es 100% seguro que se vaya a borrar.
//este codigo quedo ridiculamente complejo de forma inecesaria y me va a tomar mas tiempo del que quisiera limpiarlo.

#ifndef MESH_H
#define MESH_H

#ifdef _WIN32
    #define NOMINMAX
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <vector>
#include <string>
#include <set>
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>
#else
    #include <GL/gl.h>
#endif

#include "crossplatform.h" // MeshIndex (16 bits N95 / 32 bits PC/Android/WebGL/N8)
#include "Materials.h"
#include "Objects.h"
#include "animation/Flipbook.h"  // Flipbook + FlipbookPlayer (la animacion UV UNIFICADA del Core)


class FaceCorner {
	public:
		int vertex;
		int uv;
		int normal;
		int color; // indice de color POR ESQUINA (vc del .obj); -1 = usar el color por-vertice
		FaceCorner() : vertex(-1), uv(-1), normal(-1), color(-1) {}
};

class Face { 
	public:
		std::vector<FaceCorner> corners;
};

class VertexAnimation;
class Armature2DAnimation; // clips del ARMATURE 2D (animation/Armature2DAnimation.h). Forward + vector de
                           // PUNTEROS a proposito (patron Modifier/VertexAnimation): Mesh.cpp entra en la
                           // build de Symbian y libs/Whisk3DCore/animation NO esta en el .mmp -> este header
                           // pesado no se puede incluir desde aca.
class EditMesh; // malla de EDICION (datos de edit mode), en EditMesh.h

// ===================================================
// Enumeraciones y estructuras auxiliares
// ===================================================
// enum class portable (idiom struct+Enum, como ObjectType)
struct MeshType {
    enum Enum { cube, UVsphere, IcoSphere, plane, vertice, circle, cone, cylinder };
    Enum v;
    MeshType(Enum e) : v(e) {}
    operator Enum() const { return v; }
};

// ====================================================================
//  CAPAS DE DATOS de la malla (UV / vertex color / vertex group) — PERSISTENTES y
//  EXCLUSIVAS de cada malla. Se guardan como LISTAS DE PUNTEROS con un indice ACTIVO.
//  Las edita el editor; el render usa la capa ACTIVA. Indexadas por CORNER (esquina de
//  cara, en el orden de faces3d) salvo el vertex color cuando es 'porVertice'.
// ====================================================================
class UVMap {
    public:
        std::string nombre;
        std::vector<GLfloat> uv; // 2 por corner (u, v)
        UVMap(const std::string& n) : nombre(n) {}
};

class ColorLayer {
    public:
        std::string nombre;
        bool porVertice;             // true = 1 color por posicion unica; false = por corner
        std::vector<GLubyte> color;  // 4 (rgba) por corner (o por posicion unica si porVertice)

        // ---- COLOR POR INDICE DE PALETA (concepto del EDITOR) ----
        // Con porIndice, cada elemento no guarda un color libre sino el INDICE de un color de la
        // paleta EFECTIVA del objeto (-1 = sin asignar, se respeta el color libre que tenga).
        // El CORE no interpreta esto: sigue rindiendo 'color', que es el RGBA ya resuelto -- al
        // final del dia en GL hay que dejar en memoria como se dibuja. El editor es el que
        // rehornea 'color' desde los indices cuando se pinta o cuando cambia la paleta, y de ahi
        // sale el palette-swap: cambiar la paleta del padre re-pinta a todos sus herederos.
        // Combinado con porVertice dan las cuatro formas de guardar el vertex color:
        //   corner libre | corner por indice | vertice libre | vertice por indice
        bool porIndice;
        std::vector<int> indice;     // 1 por elemento (mismo largo que color/4). Vacio = sin usar.

        ColorLayer(const std::string& n) : nombre(n), porVertice(false), porIndice(false) {}
};

// ====================================================================
//  LOS DOS GRUPOS DE PESOS DE LA MALLA (entidades DISTINTAS, sin cruces entre si):
//
//    VertexGroup  -> pesos por CONTROL-POINT (punto 3D). Es el grupo del VIEWPORT 3D:
//                    lo pinta el weight paint del 3D, lo lee el skinning 3D (SkinearMesh),
//                    lo exporta el GLB y lo bindea POR NOMBRE el hueso del ARMATURE 3D.
//    UVGroup      -> pesos por RENDER-VERT (CORNER, esquina de cara). Es el grupo del
//                    EDITOR UV: lo pinta el pincel del UV, lo lee el skinning 2D
//                    (Armature2DAplicar) + el relleno de color del UV, y lo bindea POR
//                    NOMBRE el hueso del ARMATURE 2D. SIEMPRE por corner (no hay
//                    variante por control-point: si fuera por punto 3D, pesar UNA cara
//                    del cubo arrastraria las 3 islas UV que comparten ese punto).
//
//  REGLA DE BINDING (la unica que hay que recordar):
//    3D: hueso <-> vertex group (vertices / control-points)
//    2D: hueso <-> UV group     (corners / render-verts)
//  Los dos viven en listas propias del Mesh (vertexGroups/grupoActivo y uvGroups/
//  uvGrupoActivo) y NO se bakean uno del otro: son datos distintos de la misma malla.
// ====================================================================
class VertexGroup {
    public:
        std::string nombre;
        std::vector<int>     verts;  // indices de posicion afectados (SPARSE, paralelo a pesos)
        std::vector<GLfloat> pesos;  // peso 0..1 de cada vert
        VertexGroup(const std::string& n) : nombre(n) {}
};

class UVGroup {
    public:
        std::string nombre;
        std::vector<int>     verts;  // indices de RENDER-VERT / CORNER (SPARSE, paralelo a pesos)
        std::vector<GLfloat> pesos;  // peso 0..1 de cada corner
        UVGroup(const std::string& n) : nombre(n) {}
};

// ====================================================================
//  ARMATURE 2D DEL MESH (skinning de UV). Huesos-LINEA 2D que viven DENTRO de la
//  malla, en el espacio UV [0..1]. Se crean y editan SOLO en el editor UV; su
//  proposito es EMPARENTAR y ANIMAR los UV: cada hueso deforma (pose 2D con
//  traslacion/rotacion/escala en X e Y) los UV de los corners pesados en el
//  UV GROUP de SU MISMO NOMBRE (binding POR NOMBRE, pesos por RENDER-VERT).
//  El armature 3D usa la OTRA entidad (vertex group, por control-point): ver el
//  bloque VertexGroup/UVGroup de arriba.
//  VARIOS ARMATURES POR MALLA: la malla guarda una LISTA (Mesh::armatures2d) + el indice del
//  ACTIVO (Mesh::armature2dActivo), igual patron que uvMaps/vertexGroups/uvGroups. Cada armature
//  es INDEPENDIENTE: sus huesos, SUS clips de animacion y su hueso/clip activo (struct Armature2D
//  aca abajo). El editor UV los selecciona en su MODO OBJETO y todo lo que edita huesos opera
//  sobre el ACTIVO (Mesh::Arm2DHuesos() y compania). El SKINNING en cambio aplica TODOS los
//  armatures a la vez (cada hueso pesa por su UV group homonimo; ver Armature2DAplicar).
//  ANIMACION: va por CLIPS PROPIOS (Armature2D::anims, ver animation/Armature2DAnimation.h):
//  cada clip tiene un track por hueso y una curva por canal (posU/posV/rot/sclX/sclY).
//  Los reproduce Armature2DEvaluar: con el clip 2D en el timeline (ActiveAnimKind==4) o,
//  en MODO JUEGO (kind 2), TODAS las mallas de la escena via Armature2DEvaluarEscena.
//  RETROCOMPAT: una malla SIN clips 2D se comporta como antes -> la animacion sale de la
//  VERTEX ANIMATION UV horneada (VertexFrame::uvs, el camino viejo) y sigue corriendo
//  igual en el editor y en el runtime. La pose ACTUAL (no keyframeada) se re-aplica al
//  abrir el .w3d (Armature2DAplicar corre en el editor y en el juego compilado).
// ====================================================================
struct W3dBone2D {
    std::string nombre;           // = nombre del UV GROUP que lo pesa (binding por nombre)
    int   padre;                  // indice en Armature2D::huesos (-1 = raiz). Invariante: padre < hijo
                                  // (extrude/borrar lo conservan; re-parentar REORDENA topologico en el editor)
    float headU, headV;           // extremos del hueso (rest) en espacio UV
    float tailU, tailV;
    float poseTU, poseTV;         // pose 2D: traslacion (UV)
    float poseRot;                // rotacion en GRADOS alrededor del head
    float poseSX, poseSY;         // escala X / Y alrededor del head
    // CONECTADO al padre: la punta compartida (head == tail del padre) se mueve SOLDADA (una
    // sola). "Disconnect Bone" (Alt+P) lo apaga SIN tocar el padre: el hueso sigue emparentado
    // (linea punteada al separarlo) pero las puntas quedan libres. Se guarda en el .w3d
    // ("suelto": true solo cuando esta apagado con padre -> los archivos viejos no cambian).
    bool  conectado;
    bool  select;                 // seleccion en el editor UV (transitorio, no se guarda)
    bool  selHead, selTail;       // seleccion POR PUNTA estilo Blender (transitorio; hueso entero = select + ambas)
    W3dBone2D() : padre(-1), headU(0.5f), headV(0.5f), tailU(0.5f), tailV(0.3f),
                  poseTU(0.0f), poseTV(0.0f), poseRot(0.0f), poseSX(1.0f), poseSY(1.0f),
                  conectado(true), select(false), selHead(false), selTail(false) {}
    bool PoseIdentidad() const {
        return poseTU > -1e-6f && poseTU < 1e-6f && poseTV > -1e-6f && poseTV < 1e-6f &&
               poseRot > -1e-6f && poseRot < 1e-6f &&
               poseSX > 1.0f - 1e-6f && poseSX < 1.0f + 1e-6f &&
               poseSY > 1.0f - 1e-6f && poseSY < 1.0f + 1e-6f;
    }
};

// UN armature 2D de la malla: sus huesos, SUS clips de animacion y su estado activo. La malla
// tiene una LISTA de estos (Mesh::armatures2d) + el indice del activo, asi que dos rigs 2D de la
// MISMA malla (ej: un personaje y su sombrero, o cara y cuerpo) tienen curvas y keyframes
// SEPARADOS. La animacion (ActiveAnimKind==4) reproduce el clip activo del armature ACTIVO.
struct Armature2D {
    std::string nombre;                        // unico dentro de la malla (lo muestra el outliner / el UV)
    std::vector<W3dBone2D> huesos;             // vacio = armature sin huesos (existe igual)
    std::vector<Armature2DAnimation*> anims;   // clips PROPIOS de ESTE armature (punteros: se liberan aca)
    int animActiva;                            // clip activo (-1 = ninguno)
    int boneActivo;                            // hueso activo del editor UV (-1 = ninguno; transitorio)
    Armature2D(const std::string& n = "Armature 2D") : nombre(n), animActiva(-1), boneActivo(-1) {}
    ~Armature2D();                             // libera anims (definido en Mesh.cpp: el tipo es incompleto aca)
private: // no copiable (es dueno de punteros): la lista guarda Armature2D*
    Armature2D(const Armature2D&);
    Armature2D& operator=(const Armature2D&);
};

// MODIFICADOR: concepto del EDITOR (definido en main/edit/Modifier.h). El core solo guarda una lista de
// PUNTEROS (no conoce la clase ni la procesa; el editor la crea/gestiona y a futuro genera la malla de render).
// pero la realidad es que no me termina de gustar esta logica y lo quiero cambiar... me gustaria que el Core quede lo mas simple y limpio posible
// pero toda la logica de modificadores, capas, etc etc solo lo complico de mas al pedo.
class Modifier;

// cara LOGICA (poligono): triangulo / quad / ngon. La malla de RENDER la
// triangula en faces[]; el overlay (y a futuro la edicion de mallas) la usa
// como UNA sola cara -> una sola normal. Primer paso del refactor de malla
// editable: por ahora vive junto a la malla de render.
class MeshFace {
    public:
        std::vector<int> idx; // indices (en vertex[]) en orden alrededor del poligono
        int mat;              // indice del mesh part (materialsGroup) al que pertenece la cara
        int smooth;           // shading POR CARA: -1 = hereda meshSmooth global; 0 = flat; 1 = smooth (Face>Shade)
        MeshFace() : mat(0), smooth(-1) {} // por defecto el primer mesh part + hereda el shading global. SOBREVIVE GenerarRender
};

// fuente de la capa de UN corner nuevo (para reconstruir las capas tras una op que
// restructura la topologia con verts INTERPOLADOS, ej loop cut): copiar del corner 'a'
// (si b<0), o interpolar (lerp) entre los corners 'a' y 'b' en 's'.
struct CornerSrc {
    int a, b; float s;
    CornerSrc(int A=-1, int B=-1, float S=0.0f) : a(A), b(B), s(S) {}
};

class MaterialGroup { 
    public:
        std::string name;
        //podria ser triangulos GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN
        //lineas: GL_LINES, GL_LINE_STRIP, GL_LINE_LOOP
        //puntos: GL_POINTS
        GLenum drawMode;
        int start;              // índice del primer triángulo real
        int count;              // cantidad de triángulos reales
        int startDrawn;         // índice del primer triángulo a dibujar
        int indicesDrawnCount;  // cantidad de vertices a dibujar
        Material* material;

        MaterialGroup()
            : name("Mesh"), drawMode(GL_TRIANGLES), start(0), count(0),
              startDrawn(0), indicesDrawnCount(0), material(NULL) {}
};

// ====================================================================
//  PRESERVACION DE LO DESCONOCIDO del .w3dm (formato de malla propio, io/W3dMalla.h)
//
//  El .w3dm es EXTENSIBLE: agregar una capa nueva es AGREGAR un bloque. Para que
//  guardar con un Whisk3D VIEJO no DESTRUYA los datos de uno NUEVO, el lector se
//  guarda VERBATIM todo bloque cuyo nombre no conoce y el escritor los vuelve a
//  emitir al final del archivo.
//
//  LA TRAMPA: un bloque desconocido casi seguro INDEXA puntos, esquinas, caras o
//  aristas. Si el usuario edito la topologia esos indices ya no significan nada y
//  re-escribirlos seria PEOR que perderlos (datos CORRIDOS en silencio). Por eso
//  cada bloque declara su DOMINIO en la cabecera y al cargar se anota cuantos
//  elementos tenia cada dominio (el "sello"): al guardar, un bloque global se
//  reescribe siempre y uno indexado solo si su dominio conserva la cantidad; si
//  no, se DESCARTA con aviso. Un lector viejo puede aplicar esa regla SIN
//  ENTENDER NADA del bloque, que es todo el punto.
// ====================================================================
struct W3dBloqueAjeno {
    std::string texto;  // el bloque tal cual vino (cabecera .. END, con sus saltos de linea)
    std::string nombre; // nombre del bloque (para el aviso)
    int dominio;        // 0=punto 1=esquina 2=cara 3=arista 4=global
    int cantidad;       // cantidad declarada en la cabecera (para el aviso)
    W3dBloqueAjeno() : dominio(4), cantidad(0) {}
};
struct W3dAjenos {
    std::vector<W3dBloqueAjeno> bloques;
    // SELLO: cantidades por dominio en el momento de la carga (-1 = no se cargo de un .w3dm)
    int nPuntos, nEsquinas, nCaras, nAristas;
    // EL FRENO DE MANO del formato: los nombres de la directiva "requiere" TAL CUAL
    // vinieron en el archivo. Es la unica pieza que no se puede agregar mas tarde
    // (un lector viejo ignoraria una valvula nueva como cualquier directiva
    // desconocida), asi que TIENE que sobrevivir un ciclo abrir->guardar de un
    // Whisk3D que no entiende el bloque protegido. Antes se perdia: el escritor
    // emitia siempre "requiere V F" a mano y el primer guardado dejaba el archivo
    // SIN la valvula, o sea el degradado silencioso que 'requiere' existe para
    // evitar. Vacio = malla que no salio de un .w3dm -> el escritor pone el default.
    std::vector<std::string> requiere;
    // la malla se abrio con errores (bloque sin END / faltaba V o F): guardar
    // encima HORNEA lo que se perdio. Ver W3dMallaInfo::truncado.
    bool truncado;
    // EL LECTOR RECHAZO EL ARCHIVO ENTERO: no es un W3DMESH, o es de una version
    // del lexico MAS NUEVA, o le falta V/F. La malla que quedo en la escena esta
    // VACIA (no es la del archivo) y guardar encima HORNEA esa nada: el objeto
    // sigue con su nombre y su transform y la geometria desaparece para siempre.
    // Es el degradado silencioso que el formato existe para impedir, y en el caso
    // estrella (un .w3d escrito por un Whisk3D mas nuevo). 'truncado' NO lo cubre:
    // el lector se va ANTES de setearlo y ademas la malla es otra, recien creada.
    // Con esto puesto EscribirMallaW3dm se NIEGA, igual que con el freno "requiere".
    bool noCargo;
    W3dAjenos() : nPuntos(-1), nEsquinas(-1), nCaras(-1), nAristas(-1), truncado(false), noCargo(false) {}
    void Limpiar() {
        bloques.clear(); requiere.clear();
        nPuntos = -1; nEsquinas = -1; nCaras = -1; nAristas = -1; truncado = false; noCargo = false;
    }
};

// ===========================================================================
//  ANIMACION UV de la malla = un FLIPBOOK del Core (ver animation/Flipbook.h).
//
//  La malla referencia un Flipbook (grilla de celdas + curvas de UV) y lo reproduce
//  con un FlipbookPlayer PROPIO. Animar = sumarle a la capa UV del render el OFFSET
//  de la esquina 0 de la ventana actual (uvActual[0],[1]); para una tira clasica de
//  N celdas eso es cuadro/N sobre u (o v), igual que antes. El wrap lo da GL_REPEAT.
//
//  AUTOPLAY: el player se tickea en UpdateUVAnims(dt) (el mismo pase de siempre, que
//  ademas ESCRIBE uv[]); NO va en el registro global de UpdateFlipbooks porque la
//  malla escribe geometria, no solo lee uvActual. Sin lua ni timeline.
//
//  flipUvBase: copia del uv[] SIN desplazar, capturada perezosamente. Si OTRO sistema
//  regenera la geometria (skinGeomVersion salta sin pasar por aca) la base se recaptura
//  sola. GUARDAR: ReposarUVAnimTira() deja uv[] = base antes de escribir el .w3dm.
// ===========================================================================

// avanza TODAS las animaciones UV de la escena (autoplay). true si alguna malla
// cambio (el caller redibuja). Llamar una vez por frame con el dt real.
bool UpdateUVAnims(float dtSeg = 1.0f / 60.0f);

// ===========================================================================
//  DATOS COMPARTIDOS (MallaDatos.h + io/W3dRecursos.h): mascara de capas de
//  render cuya MEMORIA es del dato compartido (no de esta instancia). Todo
//  camino que ESCRIBE una de estas capas llama antes a DesinstanciarDatos()
//  con su bit (copy-on-write); todo camino que la BORRA para regenerarla usa
//  copiar=false (abandona sin copiar). Con la mascara en 0 nada cambia.
// ===========================================================================
enum {
    W3DMD_POS   = 1,    // vertex[]
    W3DMD_NOR   = 2,    // normals[]
    W3DMD_UV    = 4,    // uv[]
    W3DMD_COL   = 8,    // vertexColor[]
    W3DMD_FACES = 16,   // faces[]
    W3DMD_TODO  = 31
};

// ===================================================
// Clase Mesh
// ===================================================
class Armature; // objects/Armature.h — skinning (deform por esqueleto)

class Mesh : public Object {
    public:
        // defaults en el constructor (Mesh.cpp), C++03
        int vertexSize;
        // POSICIONES de los render-verts (3 floats por vert).
        // OJO: si vas a MOVER estas posiciones (no rebuildear la malla entera),
        // abri un W3dMoverVerts sobre la malla ANTES de escribir. Sin eso las
        // marcas sharp/seam (indexadas por los BYTES de la posicion) quedan
        // apuntando a donde ya no hay nada y se pierden SIN AVISO al guardar.
        // Ver Mesh::ReanclarMarcasPos y la clase W3dMoverVerts (abajo del todo).
        //
        // Y SI VAS A COPIARLAS (a otra malla, o a un snapshot de undo) NO son
        // dato suficiente: las mismas posiciones significan EL MODELO o LA POSE
        // de un cuadro segun 'posadaPorAnim' (abajo). Copiar el buffer sin el
        // estado deja una malla que dice "estoy en reposo" con la pose adentro.
        // Usa W3dPosVerts, que guarda y escribe las dos cosas juntas.
        GLfloat* vertex;

        // ARCHIVO del que se importo esta malla ("" = modelada en el editor).
        // INFORMATIVO: la geometria SIEMPRE se guarda horneada en el .w3dm del
        // proyecto, asi que esto es "de donde salio" (y de donde reimportar). Si
        // el archivo ya no esta, se avisa y la malla carga igual. Antes era al
        // reves: sin origen se exportaba un GLB y CON origen no se guardaba
        // geometria ninguna -> mover el .obj dejaba la malla vacia.
        std::string origen;

        // SKINNING (deform por esqueleto, del CORE): si skinArmature != NULL, la malla se deforma a la pose del
        // esqueleto (por los vertex groups = pesos por hueso) y se dibuja skinVertex en vez de vertex. El modificador
        // "Armature" del editor setea este puntero al target. Liviano (N95): solo posiciones, y solo si cambio el frame.
        Armature* skinArmature; // NULL = sin skinning
        GLfloat*  skinVertex;   // posiciones deformadas (vertexSize floats)
        GLbyte*   skinNormals;  // normales deformadas (rotadas por los huesos) -> iluminacion correcta al doblar
        int       skinVertexCap;// floats reservados en skinVertex (realloc si vertexSize cambia -> evita overflow)
        bool      skinConLuz;   // el render lo prende si algun meshpart tiene luz (sino no vale la pena rotar normales)
        int       lastSkinFrame;// cache: no re-skinnea si el frame no cambio
        unsigned  skinPoseSerial;// poseSerial del armature con el que se calculo skinVertex; si cambia (posar/elegir clip
                                 // en el MISMO frame) se re-skinnea aunque lastSkinFrame no cambie
        std::vector<GLfloat> skinBordesBuf; // contorno/overlay deformado (edges con skinVertex); cache por frame
        int       lastSkinBordesFrame;      // cache del contorno skinneado
        unsigned  skinBordesPoseSerial;     // poseSerial con el que se calculo el contorno; si cambia (posar/elegir clip
                                            // en el mismo frame) se recalcula el borde (sino la silueta flota en bind)
        // CACHE CSR de pesos por CONTROL-POINT (no por render-vert): se arma UNA vez, no por frame. Un mesh de juego
        // duplica cada control-point en varios render-verts por seams de UV/normal/material (banana: 42840 render pero
        // solo 7186 CP = 6x). Los render-verts de un mismo CP comparten bind + pesos -> el MISMO skinning. Por eso el
        // blend caro de matrices se hace 1 vez POR CP (skinCpMat) y se ESCALEA a los render-verts (6x menos matematica).
        // Invalida via firma de topologia (skinFlatSig). vertCtrlPoint[render] -> CP da el mapeo al escalear.
        std::vector<int>    skinCpBone;   // indices de hueso (valores CSR, por CP)
        std::vector<float>  skinCpW;      // pesos 0..1 normalizados a suma 1 (CSR, paralelo a skinCpBone)
        std::vector<int>    skinCpOff;    // offset por control-point (size skinNCtrl+1)
        int                 skinNCtrl;    // cantidad de control-points (max vertCtrlPoint + 1)
        std::vector<float>  skinCpMat;    // TEMP por-frame: matriz de skin MEZCLADA 4x3 (12 floats) por CP (reusado, sin realloc)
        unsigned            skinFlatSig;  // firma de la topologia con la que se armo el CSR (0 = sin armar)
        unsigned            skinGeomVersion;// se incrementa en CalcularBordes (regenerar geometria); entra en skinFlatSig
                                            // para invalidar el CSR si GenerarRender reindexa vertex[] con igual tamaño

        // ---- CACHE de vertex-animation (bake del skinning): guarda el resultado del skinning (posiciones + normales si
        //      hay luz) por-frame para NO recomputarlo en cada reproduccion. LAZY: la 1ra vuelta skinnea y guarda (lento,
        //      ~4fps), despues solo copia/interpola el snapshot (rapido -> se acerca al techo de render). 'skip' decima
        //      frames para ahorrar memoria (interpola los intermedios). Se activa desde el modificador Armature. ----
        struct SkinSnapshot { GLfloat* pos; GLbyte* nor; }; // pos = vertexSize*3 floats; nor = vertexSize*3 bytes o NULL (unlit)
        std::vector<SkinSnapshot> skinCache; // por slot (frame-skinCacheStart)/(skip+1); pos==NULL = todavia no bakeado
        bool     skinCacheOn;       // flag del modificador Armature (default OFF)
        int      skinCacheSkip;     // 0 = todos los frames; 1 = 1 si 1 no (interpola el intermedio); N = guarda cada N+1
        int      skinCacheStart, skinCacheEnd; // rango cubierto por el cache (= StartFrame..EndFrame al (re)validar)
        bool     skinCacheConLuz;   // se guardaron normales en los snapshots?
        unsigned skinCacheSig;      // firma de invalidacion (geom + clip + rig + rango + skip + luz)
        void LiberarSkinCache();    // libera todos los snapshots (pos/nor) y vacia el vector

        // ---- VBOs de render (buffer objects): la malla se sube 1 vez a memoria de GPU y se dibuja desde ahi, en vez
        //      de re-transferir los client-arrays de RAM en cada glDrawElements. 0 = sin crear. Solo para el caso
        //      "simple" (solido / materiales sin chrome-normalmap, sin gen/weightpaint/xray); el resto sigue client-side. ----
        unsigned int vboPos, vboNor, vboCol, vboUV, vboIdx; // handles (posiciones/normales/color/uv/indices)
        unsigned     vboGeomVer;   // skinGeomVersion con el que se subieron los atributos ESTATICOS (col/uv/idx + base)
        int          vboSkinFrame; // lastSkinFrame con el que se subieron pos/nor (skinneado: re-sube al cambiar la pose)
        int          vboSkinFramePrev; // lastSkinFrame del render ANTERIOR: detecta si la pose esta cambiando ACTIVAMENTE
                                       // (animando) -> se dibuja de client-array para NO re-especificar el VBO cada frame
                                       // (el re-spec del buffer que el tiler MBX todavia lee = STALL de sync = jitter de fps)
        unsigned     vboPoseSerial;    // poseSerial subido al VBO de pos/nor (se re-sube si cambia; capta posar/elegir clip
        unsigned     vboPoseSerialPrev;// en el mismo frame, donde vboSkinFrame no alcanzaba). Prev = el del render anterior.
        int          vboVertN, vboIdxN; // cantidades subidas (para no re-crear si no cambio el tamaño)
        unsigned     vboPvsVer;    // RETIRADO (P3): el IBO ya no se re-sube por cambio de sector; los indices
                                   // del subconjunto van de CLIENTE (DrawTrianglesClientIdx). Queda declarado
                                   // para no tocar el layout; nadie lo consulta.
        bool         vboRenderActivo; // true mientras se dibuja desde VBOs -> AplicarMaterial bindea el uv VBO (no client-uv)
        bool         vboPoseSkinneada; // el VBO de posiciones tiene una pose DEFORMADA subida? -> al quitar el armature
                                       // (skinArmature=NULL) hay que re-subir el bind 1 vez (sino queda la ultima pose pegada)
        void SubirVBO(const GLfloat* posBuf, const GLbyte* norBuf, bool soloPose); // sube/actualiza los VBOs

        // capas persistentes EXCLUSIVAS de la malla (lista de punteros + indice ACTIVO,
        // -1 = ninguna). El render usa la activa; el editor las crea/edita/borra.
        std::vector<UVMap*>       uvMaps;       int uvMapActivo;
        std::vector<ColorLayer*>  colorLayers;  int colorActivo;
        // los DOS grupos de pesos (ver el bloque VertexGroup/UVGroup arriba): por
        // CONTROL-POINT (3D, armature 3D) y por CORNER (editor UV, armature 2D).
        std::vector<VertexGroup*> vertexGroups; int grupoActivo;
        std::vector<UVGroup*>     uvGroups;     int uvGrupoActivo;

        // ===== ANIMACION UV de la malla = FLIPBOOK del Core (ver comentario arriba) =====
        Flipbook*      flipbook;   // NULL = la malla no anima sus UV (heap, como uvAnim antes)
        bool           flipbookPropio; // true = la malla creo/posee el flipbook (lo borra al quitar);
                                       // false = es un asset CON NOMBRE compartido (SceneFlipbooks, NO se borra)
        FlipbookPlayer flipPlay;   // estado de reproduccion (fuera del registro global: la malla lo tickea)
        std::vector<GLfloat> flipUvBase;  // uv[] sin desplazar (2 floats por render-vert)
        int      flipAplicado;     // cuadro APLICADO en uv[] (-1 = uv[] esta en base)
        unsigned flipGeomVer;      // skinGeomVersion tras aplicar (detecta rebuilds ajenos)
        bool EsFlipbookMesh() const { return flipbook != 0; }
        void SetUVAnimTira(int frames, float fps, int eje, int desfase,
                           float ancho = 1.0f, float alto = 1.0f); // crea/reconfigura y registra; ancho/alto = fraccion de sub-tira en el atlas
        void UsarFlipbook(Flipbook* fb, int desfase);   // usa un flipbook COMPARTIDO (con nombre) sin poseerlo
        void QuitarUVAnimTira();          // libera + desregistra (tambien la llama ~Mesh)
        bool TickUVAnimTira(float dtSeg); // avanza y escribe uv[]; true si cambio el cuadro
        void ReposarUVAnimTira();         // uv[] = base (llamar ANTES de escribir el .w3dm)

        // ===== ARMATURES 2D DEL MESH (ver W3dBone2D / struct Armature2D arriba) =====
        // LISTA de armatures 2D + el ACTIVO (patron uvMaps/vertexGroups/uvGroups). Vacia = la
        // malla no tiene rig 2D. TODO lo que edita huesos/clips (editor UV, panel, timeline)
        // pasa por el ACTIVO via los accesores de abajo; el SKINNING aplica TODOS.
        std::vector<Armature2D*> armatures2d;
        int armature2dActivo;               // indice en armatures2d (-1 = ninguno)
        bool TieneArm2D() const { return !armatures2d.empty(); }
        // hay ALGUN armature 2D con huesos? (no "el activo": el SKINNING aplica TODOS, ver
        // Armature2DAplicar). Es el gate correcto para "hay rig que pueda deformar": con el
        // activo vacio (o siendo otro rig) el de al lado igual deforma sus UVs.
        bool Arm2DAlgunHueso() const {
            for (size_t a = 0; a < armatures2d.size(); a++)
                if (armatures2d[a] && !armatures2d[a]->huesos.empty()) return true;
            return false;
        }
        // ---- acceso al armature ACTIVO. Sin armature devuelven un DUMMY estatico vacio (los
        // consumidores ya preguntan .empty() antes de tocar nada) -> nunca hay que chequear NULL.
        // OJO: NO agregar huesos por estos accesores sin asegurar antes el armature (Arm2DAsegurar).
        Armature2D* Arm2DActivoP();
        const Armature2D* Arm2DActivoP() const;
        std::vector<W3dBone2D>& Arm2DHuesos();
        const std::vector<W3dBone2D>& Arm2DHuesos() const;
        int& Arm2DBoneActivo();
        int  Arm2DBoneActivo() const;
        std::vector<Armature2DAnimation*>& Arm2DAnims();
        const std::vector<Armature2DAnimation*>& Arm2DAnims() const;
        int& Arm2DAnimActiva();
        int  Arm2DAnimActiva() const;
        // ---- gestion de la lista ----
        Armature2D* Arm2DAsegurar();                        // crea el armature 0 si no hay ninguno; devuelve el activo
        int  Arm2DAgregar(const std::string& nombre = "");   // crea uno nuevo (nombre unico) y lo deja activo; devuelve su indice
        void Arm2DBorrar(int idx);                          // borra uno (libera sus clips) y reacomoda el activo
        std::string Arm2DNombreUnico(const std::string& base) const; // "Armature 2D", "Armature 2D.001", ...
        std::string Arm2DNombreLibre(const std::string& base, int excepto) const; // idem, para el RENAME
        // ===== NOMBRES UNICOS por MALLA (misma regla que todo el editor, base/W3dNombres.h) =====
        // Cada lista es su PROPIO espacio de nombres: un UV group puede llamarse igual que un
        // vertex group (de hecho es lo normal: el rig 2D y el 3D comparten nombres de hueso).
        // 'excepto' = el elemento que puede conservar su nombre (rename); -1 = ninguno.
        // NO globalizar estos scopes: el Join (Ctrl+J) mergea los grupos POR NOMBRE entre
        // mallas del mismo rig, y con "Brazo"/"Brazo.001" perderia el binding.
        std::string NombreLibreVGroup (const std::string& base, int excepto) const;
        std::string NombreLibreUVGroup(const std::string& base, int excepto) const;
        std::string NombreLibreUVMap  (const std::string& base, int excepto) const;
        std::string NombreLibreColor  (const std::string& base, int excepto) const;
        std::string NombreLibreMeshPart(const std::string& base, int excepto) const;
        std::string NombreLibreVertexAnim(const std::string& base, int excepto) const;
        // hueso 2D: unico por MALLA a traves de TODOS sus armatures 2D (el binding es
        // contra el UV group, que es del mesh y no del rig). 'exArm'/'exBone' = el que
        // puede conservar su nombre (-1/-1 = ninguno).
        std::string NombreLibreBone2D(const std::string& base, int exArm, int exBone) const;
        // UV de REST (base del skinning 2D): Armature2DAplicar deforma DESDE aca hacia uv[].
        // Se captura LAZY (Armature2DRestCapturar) al posar; un tamano distinto de
        // vertexSize*2 = rest invalido (la topologia cambio) -> se recaptura del uv actual.
        //
        // ===== INVARIANTE DEL MAPEO (no se puede violar):  uv = f(uv2dRest, pose) =====
        // uv[] es SIEMPRE el resultado de aplicar la pose 2D al rest. O sea que uv[] es
        // DERIVADO: cualquiera puede re-ejecutar Armature2DAplicar y tiene que salir lo mismo.
        // Por eso TODA escritura a mano de uv[] (G/R/S del editor UV, tarjeta Transform UV,
        // snap, split de caras, etc.) tiene que cerrar con Armature2DRestDesdeUV(): el rest se
        // re-deriva del uv nuevo y el invariante se restablece. Si no se hace, el proximo
        // Armature2DAplicar (posar, pintar con pose activa, abrir el proyecto, undo...) pisa
        // uv[] con el rest VIEJO y la edicion del usuario se pierde en silencio (era el bug de
        // "muevo una cara en el UV, pinto y se resetea todo al mismo lugar").
        std::vector<GLfloat> uv2dRest;
        void Armature2DRestCapturar();      // uv[] -> uv2dRest si no hay un rest valido
        // RE-DERIVA el rest desde el uv[] ACTUAL para que valga uv = f(uv2dRest, pose):
        //  - sin armature 2D: no hay rest que mantener -> se descarta (se recaptura lazy).
        //  - pose en identidad: el rest ES el uv (copia directa).
        //  - con pose puesta: el skinning es AFIN por vert (uv_i = A_i*rest_i + b_i, ver
        //    Armature2DAplicar), asi que se invierte -> rest_i = A_i^-1 (uv_i - b_i). Con eso la
        //    edicion queda EXACTAMENTE donde el usuario la solto y sobrevive al re-evaluar.
        //    A_i singular (escala 0) = no se puede invertir: ese vert conserva su rest previo.
        void Armature2DRestDesdeUV();
        bool Armature2DPoseIdentidad() const; // true = TODOS los huesos de TODOS los armatures en identidad
        // matrices AFINES 2D de la pose de cada hueso del armature ACTIVO, con la jerarquia (FK).
        // 6 floats por hueso: [a b tx c d ty] tal que u' = a*u + b*v + tx ; v' = c*u + d*v + ty.
        void Armature2DMatrices(std::vector<float>& out) const;
        // idem para UN armature cualquiera de la lista (el editor UV dibuja/pickea TODOS).
        void Armature2DMatricesDe(const Armature2D* a, std::vector<float>& out) const;
        // APLANA todos los armatures 2D: un puntero por hueso + sus matrices FK concatenadas
        // (6 floats por hueso, mismo orden que 'bones'). Es lo que consume el skinning 2D.
        void Armature2DHuesosPlanos(std::vector<const W3dBone2D*>& bones, std::vector<float>& out) const;
        // SKINNING 2D: uv[i] = mezcla de (matriz de cada hueso aplicada a uv2dRest[i]) por los
        // pesos del UV GROUP del MISMO NOMBRE del hueso (POR CORNER / render-vert; ver la regla
        // de binding en el bloque VertexGroup/UVGroup: 3D = vertex group, 2D = UV group).
        // Suma de pesos < 1 -> el resto se queda en el rest (como el skinning 3D).
        // Aplica los huesos de TODOS los armatures 2D de la malla (no solo el activo): el rig que
        // no estas editando tambien deforma sus UVs. Corre en el editor (al posar) y en el runtime.
        void Armature2DAplicar();
        // cache de evaluacion (TRANSITORIO, no se guarda): mismo rol que lastPoseFrame/lastPoseAnim/
        // poseDirty del armature 3D. pose2dDirty = la pose se edito a mano -> re-aplicar el skinning
        // sin refrescar los canales desde la curva.
        int  last2dFrame, last2dAnim;
        bool pose2dDirty;

        // STACK de MODIFICADORES (del EDITOR): lista de punteros + activo (-1=ninguno). El core solo los guarda;
        // el editor los crea/gestiona (main/edit) y a futuro genera la malla de render. El ORDEN importa.
        std::vector<Modifier*>    modificadores; int modificadorActivo;

        // gestion del stack (DEFINIDAS EN EL EDITOR, main/edit/MeshEdit.cpp; el core solo las declara + las llama en ~Mesh):
        void AgregarModificador(int tipo);           // agrega uno del tipo dado al final + lo deja activo
        void QuitarModificadorActivo();              // borra el modificador activo (lo LIBERA)
        // saca el activo del stack SIN liberarlo y lo devuelve (el que llama pasa a ser dueno).
        // La usa el paso de undo del Remove, que lo adopta: ver UndoModQuitar en undo/Undo.h.
        Modifier* SacarModificadorActivo();
        void MoverModificador(int dir);              // reordena el activo (-1 sube / +1 baja) intercambiando
        void LiberarModificadores();                 // libera todos (lo llama el destructor)
        std::string NombreModificador(int i) const;  // nombre del modificador i (para el selector, sin exponer la clase)

        // MALLA GENERADA por el stack de modificadores (buffers de RENDER). Cuando genValido, RenderObject dibuja
        // ESTA en vez de la editable (vertex[]/faces[]). La produce el EDITOR (GenerarMallaModificada, main/edit);
        // el core solo la guarda + la dibuja. La malla EDITABLE (vertex/faces3d) queda intacta (se edita esa).
        GLfloat*  genVertex; GLbyte* genNormals; GLfloat* genUV; GLubyte* genColor;
        MeshIndex* genFaces; int genVertexSize; int genFacesSize;
        std::vector<MaterialGroup> genMaterialsGroup;
        std::vector<GLfloat> genBordesBuf; // aristas de POLIGONO de la malla generada (sin diagonales de tri) para el
                                           // CONTORNO de seleccion (subdiv/screw): sino el objeto seleccionado no muestra borde
        bool genValido; // true = hay malla generada por modificadores -> usarla en el render
        // VERSION DE LA GEOMETRIA EDITABLE: sube cada vez que algo cambia vertex[]/faces3d por las dos
        // puertas al render (RefrescarRender / GenerarRender, en el editor). El Core no la usa: es para
        // que OTRA malla que depende de esta (un Boolean cuyo target es esta) sepa, comparando UN entero,
        // si tiene que regenerarse -- sin hashear vertices por frame.
        unsigned geoVersion;
        void GenerarMallaModificada(); // EDITOR: rehace las gen buffers aplicando el stack (genValido=false si vacio)
        void LiberarMallaModificada(); // libera las gen buffers
        void AplicarModificadorActivo(); // "Apply Modifier": hornea la malla generada en la editable + saca el modificador

        // ================================================================
        //  PVS (modificador "Culling" por TRIANGULO, estilo PS1): override del INDEX
        //  BUFFER. Cuando pvsFaces != NULL, el render dibuja ESTOS indices (el subconjunto
        //  de triangulos del sector activo) en vez de faces[], con pvsGroups como mesh
        //  parts (mismo orden/material que materialsGroup, rangos recortados al sector).
        //  Los llena el EDITOR (W3dPVSAplicarSector, main/edit/MeshEdit.cpp) al cambiar de
        //  sector; el core solo dibuja: cambiar de sector = swap de indices (el VBO re-sube
        //  SOLO el IBO al ver pvsVersion distinto), NUNCA trabajo por frame. Los atributos
        //  (vertex/normals/uv/color) son los de SIEMPRE: la malla editable queda intacta.
        //  NULL = sin override (malla completa). No pisa a la malla generada por
        //  modificadores geometricos (genValido manda primero) ni al Edit Mode (ahi se
        //  dibuja completa: se edita todo).
        // ================================================================
        MeshIndex* pvsFaces;    int pvsFacesSize;
        std::vector<MaterialGroup> pvsGroups;
        unsigned   pvsVersion;  // bump en cada swap de sector -> el fast-path re-sube el IBO
        // pvsOrdenado: los indices del override vienen EN ORDEN DE DIBUJO (lejos->cerca,
        // el orden del pintor que trae un VisSet ordenado). Los OPACOS siguen agrupados
        // por material (con z-buffer el orden interno es gratis); los TRANSPARENTES se
        // dibujan en EL ORDEN DE LA LISTA via pvsRuns: transparencia sin sort en runtime.
        bool       pvsOrdenado;
        // (P4) SELLO de lote estatico: si es IGUAL al sello del frame actual
        // (w3dLoteStamp), la malla esta horneada en el lote de una Collection y
        // su RenderObject no dibuja (el lote ya la contiene). El lote re-marca a
        // sus miembros EN CADA FRAME: una malla que sale del lote (reparent,
        // visibilidad, fin del juego) deja de marcarse y vuelve a dibujarse sola
        // al frame siguiente, sin bookkeeping de liberacion. 0 = nunca marcada.
        unsigned   enLoteEstatico;
        // runs de TRANSPARENTES en orden de lista (solo con pvsOrdenado): cada run es un
        // rango CONTIGUO de pvsFaces que comparte mesh part (material). Los emite el
        // materializador (editor); el render los dibuja DESPUES de opacos/decales, en
        // este orden exacto (el orden del pintor del dato precalculado). Los grupos
        // transparentes de pvsGroups quedan con count 0 (sus tris viven aca).
        struct PvsRun { int grupo; int start; int count; };
        std::vector<PvsRun> pvsRuns;
        void PvsLimpiar();      // libera el override (vuelve a la malla completa)

        // noEditable: malla de ESCENARIO cerrada a edicion. Al importar NO se calculan
        // posRep/edges/bordesBuf (CalcularAABBSolo deja solo AABB+radio para el culling)
        // y el Tab a Edit Mode la ignora. Ahorra tiempo de carga y memoria. La oclusion
        // por triangulo de estas mallas la lleva el modificador CullingTri ("Oclusion"),
        // que puede seguir un PATH (Curve o malla de aristas) eligiendo el nodo solo.
        bool noEditable;
        void CalcularAABBSolo();      // AABB + radioGeom + centroGeom sin soldaduras ni edges
        // (runtime, por frame) SELECCION de una malla no editable: sin edges no hay contorno
        // que dibujar -> las caras se tintan ~25% verde EN EL MISMO draw (AplicarMaterial),
        // sin segunda pasada ni z-fighting. Lo computa RenderObject; solo editor con overlays.
        bool tintaNoEditable;

        // NORMAL autoritativa POR CORNER (3 GLbyte/corner). El render la DERIVA (no al
        // reves): las edit-ops la ACARREAN (copiar/interpolar via los helpers de capas) ->
        // mover/loop-cut/etc. NO arruinan las normales; solo RecalcularNormales/shade las
        // reescriben. Si queda stale (size != corners*3), el render cae a normals[] (fallback).
        std::vector<GLbyte> cornerNormal;

        // crea las capas iniciales (1 UVMap "UVMap", 1 ColorLayer "Col") desde los arrays
        // de render actuales si todavia no hay ninguna. Por corner (orden de faces3d).
        void PoblarCapas();
        void LiberarCapas(bool incluirGrupos = true); // libera uv/color (+ vertex groups si incluirGrupos). Join/Apply usan false para PRESERVAR el skinning
        int  ContarCorners() const; // cantidad de esquinas de cara (indice de las capas por-corner)

        // el render uv[]/vertexColor[] se DERIVA de la capa ACTIVA (la llama el editor al
        // cambiar de capa activa o editar una capa)
        void AplicarCapasAlRender();

        // === Las DOS unicas puertas al render (las ops NO tocan vertex[]/faces3d a mano) ===
        // IN-PLACE rapido (mover verts / pintar): empuja posiciones del edit + capa activa,
        // SIN realloc ni cambio de topologia. Para tiempo real (N95).
        void RefrescarRender();

        // REBUILD completo: destruye y rehace verts/uv/color/normal/faces3d desde los corners
        // + capas activas (mergeando verts). Para cambios de TOPOLOGIA. Lento.
        // recomputarNormales=false: CONSERVA las normales actuales (normals[]) en vez de recalcularlas.
        // Lo usa el JOIN: las mallas importadas (glTF/FBX) traen normales del archivo con SPLITS en los bordes
        // filosos, y recalcularlas (promediando) los REDONDEA. Al anexar no cambia la forma de cada malla, asi
        // que sus normales siguen siendo validas -> se preservan.
        void GenerarRender(bool recomputarNormales = true);

        // JOIN (Ctrl+J): anexa la geometria de 'otra' a ESTA malla, transformando cada vertice por M (lleva el
        // espacio LOCAL de 'otra' al LOCAL de esta). Preserva UV/color (via capas) + materiales (mesh parts nuevos).
        // NO rebuildea: el caller hace LiberarCapas + PoblarCapas + GenerarRender al terminar de anexar todas.
        void AnexarMallaTransformada(Mesh* otra, const Matrix4& M);

        // APPLY (Alt+A): hornea la matriz B en la geometria (cada vertice v -> B*v). Lo usa Apply Location/
        // Rotation/Scale para bakear el transform del objeto en la malla y despues resetear ese componente.
        void AplicarMatriz(const Matrix4& B);

        // MESH PARTS (materialsGroup): cada cara (faces3d[f].mat) pertenece a un mesh part. Estas ops
        // tocan SOLO faces3d.mat + materialsGroup + el index buffer (no la geometria ni el edit mesh).
        void ReagruparMeshParts(); // rearma faces[] + rangos por mesh part desde faces3d.mat
        void OptimizarCacheRender(); // reordena los triangulos de faces[] para el cache de vertices del GPU (Forsyth, MIT)
        // (def en main/edit/MeshEdit.cpp: usan el EditMesh + la seleccion de caras)
        void AsignarFacesAMeshPart(int idx);    // caras SELECCIONADAS (edit) -> mesh part idx
        // llena sel3d[f] (por faces3d) = 1 para las caras "seleccionadas" segun el EditSelectMode ACTUAL (una
        // cara cuenta si TODOS sus corners estan seleccionados). Sirve en modo vertice/borde/cara (como Delete>Faces).
        void CarasSelPorModo(std::vector<char>& sel3d);
        // llena selVert[gi] (por RENDER-vert) = 1 si el render-vert gi esta seleccionado en el edit 3D segun el
        // EditSelectMode ACTUAL, expandido por posRep (las copias por costura tambien). Lo usa el editor UV para
        // reflejar la seleccion del 3D en las UV (Sync Selection).
        void VertsSelPorModo(std::vector<char>& selVert);
        void SeleccionarMeshPart(int idx, bool sel); // (de)selecciona en edit las caras del mesh part idx
        GLubyte* vertexColor;
        GLbyte* normals;
        GLfloat* uv;

        // CHROME equirect: UV del reflejo por SOFTWARE (PC y N95). Render NO INDEXADO (por CORNER) para poder
        // corregir la COSTURA (atan2 wrap) y el POLO (al vert del polo le damos la U promedio de sus vecinos)
        // por triangulo -> sin "tuc" ni "batidora". CACHE: recalcula solo si cambia camara/mundo/geometria.
        GLfloat* chromeExpPos;     // posiciones LOCALES expandidas por corner (facesSize*3); NULL hasta usarse
        GLfloat* chromeExpUV;      // UV equirect por corner, ya corregida (facesSize*2)
        int      chromeExpCount;   // corners del buffer (= facesSize) para realloc
        bool     chromeUVValid;    // cache valido
        bool     chromeCacheEq;    // modo con que se calculo (true=equirect / false=matcap) -> recalcula si cambia
        Vector3  chromeCacheCam;   // camara con la que se calculo
        float    chromeCacheW[16]; // matriz de mundo con la que se calculo
        void ActualizarChromeUV(bool equirect); // recalcula los buffers (equirect 360 o matcap sphere-map por software)

        // CHROME sobre la MALLA GENERADA (screw/subdiv): mismas UV de reflejo pero calculadas desde la geo GENERADA
        // (genVertex/genNormals/genFaces), y SOLO para los mesh parts con reflejo (los demas no cuestan nada). Asi el
        // reflejo se ve sin tener que "aplicar" el modificador, sin recalcular la malla 3D (solo las UV, y con cache).
        GLfloat* genChromeExpPos;  // posiciones LOCALES expandidas por corner de la gen (genFacesSize*3)
        GLfloat* genChromeExpUV;   // UV de reflejo por corner (genFacesSize*2)
        int      genChromeCount;   // corners del buffer (= genFacesSize)
        bool     genChromeValid;   // cache valido
        Vector3  genChromeCam;     // camara con la que se calculo
        float    genChromeW[16];   // matriz de mundo con la que se calculo
        void ActualizarChromeUVGen(); // recalcula genChromeExp* para los mesh parts con reflejo (cache por camara/mundo)

        // NORMAL MAPPING (DOT3): base tangente POR VERTICE de render (T.xyz + handedness en .w), derivada de
        // pos+UV (cache: solo se recalcula si cambia la geometria). 'nmColors' = el vector luz L en tangent-space
        // por vertice (se rellena cada frame; es el "primary color" del DOT3). Indexados como vertex/normals/uv.
        GLfloat*  tangents;     // vertexSize*4
        GLubyte*  nmColors;     // vertexSize*4
        bool      tangentsValid;
        void CalcularTangentes();                       // T por vertice desde pos+UV (cache)
        void ActualizarNormalMapColors(const Vector3& luzLocal); // L en tangent-space -> nmColors (por frame)

        int facesSize;
        MeshIndex* faces; // index buffer GPU: 16 bits N95 / 32 bits escritorio (ver crossplatform.h)
        std::vector<MaterialGroup> materialsGroup;

        // ---- GEOMETRIA COMPARTIDA (MallaDatos.h): si datosRec != NULL, las
        //      capas marcadas en datosComp (W3DMD_*) apuntan a arrays de un
        //      MallaDatos del almacen de recursos y NO se poseen. Copy-on-write:
        //      escribir una capa exige DesinstanciarDatos() antes. ----
        void*         datosRec;   // W3dRecurso* (tipo MALLA); NULL = todo propio
        unsigned char datosComp;  // mascara W3DMD_* de capas compartidas
        // COW: garantiza que las capas de 'mask' sean PROPIAS. copiar=true las
        // duplica (para escrituras in-place); copiar=false las ABANDONA en NULL
        // (para quien va a regenerarlas de cero y las habria borrado con
        // delete[]). Suelta la referencia al llegar la mascara a 0. No-op si
        // nada esta compartido: es gratis llamarla "por las dudas".
        void DesinstanciarDatos(unsigned mask, bool copiar = true);
        // destruccion: NULLea las capas compartidas y suelta la referencia (el
        // dato muere solo cuando su refcount llega a 0)
        void SoltarDatosCompartidos();

        // caras logicas (tri/quad/ngon) para el overlay de normales y la futura
        // edicion. Si esta vacio, el overlay usa los triangulos de faces[].
        std::vector<MeshFace> faces3d;
        bool meshSmooth; // shading: true = normales compartidas (suave); false = por cara (flat)

        // BORDES FILOSOS (sharp): en una malla SMOOTH, estos bordes NO promedian las
        // normales (quedan flat/afilados) -> cilindro = lados suaves + tapas planas. La
        // clave es la POSICION de los 2 extremos (estable: sobrevive el re-merge de
        // GenerarRender y NO acopla el core al editor). Ver CornerNormalConSharp.
        std::set<std::string> sharpEdges;

        // COSTURAS UV (seams): bordes donde el UV se ABRE al desplegar (unwrap). Se ven
        // MAGENTA en Edit Mode. Misma clave de POSICION que sharpEdges (estable al re-merge).
        std::set<std::string> seamEdges;

        // cuantos guards W3dMoverVerts hay ABIERTOS sobre esta malla. Mientras sea > 0 las
        // posiciones estan a MEDIO MOVER (vertex[] ya se escribio, las claves de sharp/seam
        // todavia NO se re-anclaron) -> PodarMarcasSinArista NO PODA, porque en ese estado
        // intermedio TODA marca parece huerfana y podarla borraria justo lo que hay que
        // seguir. Ver el invariante de orden en el comentario de W3dMoverVerts, abajo.
        int moverVertsAbiertos;

        // LA POSO EL PLAYBACK? ESTADO REAL, no una comparacion de posiciones.
        // Lo escribe UNICAMENTE la reproduccion de vertex anims (EvalVertexAnim /
        // ApplyVertexFrame / BlendVertexAnimations, en animation/VertexAnimation.cpp):
        // true = vertex[] tiene la pose de un cuadro que NO es el keyframe base,
        // false = la malla esta en REPOSO (nadie la poso, o el playhead volvio al
        // cuadro base). Ver el contrato de PosadaPorVertexAnim, que es quien lo lee.
        //
        // POR QUE UN FLAG Y NO "vertex[] == frames[0]": comparar no puede distinguir
        // "posada por el playback" de "editada sin autokey". Un move/scale en Edit
        // Mode, Set Origin, Apply Loc/Rot/Scale, Merge By Distance, Apply Mirror o
        // Apply Modifier mueven vertex[] SIN escribir el keyframe base -> la
        // comparacion daba POSADA en una malla que nadie poso y la poda quedaba
        // apagada PARA SIEMPRE (marcas fantasma que sobrevivian a cortes y un aviso
        // al guardar que el usuario no podia accionar). Esas operaciones re-anclan
        // solas (W3dMoverVerts) y NO tocan este flag, que es justo lo correcto.
        //
        // INVARIANTE (la parte que se olvido y hubo que arreglar): ESTE FLAG ES
        // PARTE DE vertex[], no un adorno del objeto. Toda puerta que COPIE,
        // RESTAURE o INTERCAMBIE posiciones -entre dos mallas o contra un
        // snapshot- tiene que mover el flag CON ellas, o la copia queda diciendo
        // "estoy en reposo" con la POSE adentro: el gate se apaga, la poda se
        // lleva TODAS las marcas y el guardado hornea la pose. No se cumple a
        // fuerza de memoria: se cumple pasando por W3dPosVerts (abajo del todo),
        // que es un solo valor con las posiciones Y el estado.
        bool posadaPorAnim;

        // SELECCION del editor UV (1 por vertice de RENDER, indexado igual que vertex[]/uv[]).
        // Es por-render-vert (no por posicion) a proposito: un mismo vert 3D aparece en VARIAS
        // coordenadas UV y se selecciona INDEPENDIENTE en cada una. Transitorio (no se exporta);
        // se redimensiona/limpia al re-split de GenerarRender. Lo usa el UVEditor (pick + move/etc).
        std::vector<unsigned char> uvSelVert;

        // BORDES unicos (sin repetir, dedup por POSICION) calculados desde faces3d.
        // Pares de indices de vertice: (edges[2i], edges[2i+1]). Se usan para el
        // contorno verde al seleccionar y para el modo wireframe (y a futuro, editar).
        std::vector<int> edges;

        // BORDES SUELTOS (no pertenecen a ninguna cara): pares de indices de
        // vertice. Los crea el borrado en Edit Mode ("Delete > Faces/Edges" deja
        // los bordes sin sus caras). Se MERGEAN en 'edges' (CalcularBordes) para
        // que el wireframe / contorno / edit los dibujen igual.
        std::vector<int> looseEdges;
        // VERTICES SUELTOS (no pertenecen a ninguna cara NI arista): indices de vertice. Un "Add > Vertex" o lo
        // que queda al borrar toda la geometria alrededor de un vert. GenerarRender los preserva (sino se perderian
        // por no estar en ninguna cara/arista). Analogo a looseEdges.
        std::vector<int> looseVerts;

        // representante de cada vertice por POSICION (cacheado por CalcularBordes):
        // posRep[i] = indice del primer vertice en el mismo lugar. Evita el O(nV^2)
        // por frame en el overlay de vertex-normal.        
        std::vector<int> posRep;
        int vertsAgrupados; // cantidad de posiciones UNICAS (para el overlay de stats)

        // WEIGHT PAINT / rig: por cada vertice de render, el indice de CONTROL-POINT del FBX del que salio (lo llena
        // el importador FBX). Con esto los pesos de un vertex group (indexados por control-point) se mapean a los
        // vertices de render para pintar el degradado de peso. Vacio en mallas que no vienen de un FBX con skin.
        std::vector<int> vertCtrlPoint;
        // WEIGHT PAINT: color por vertice (RGBA, vertexSize*4) del degradado de peso del grupo activo (negro=0 ->
        // amarillo=0.5 -> rojo=1). weightPaintOn lo prende el editor en modo Weight Paint; RenderObject lo dibuja.
        std::vector<GLubyte> weightPaintColor;
        bool weightPaintOn;
        // VERTEX PAINT: lo prende el editor en modo Vertex Paint. La malla se dibuja SOLIDA con
        // el vertexColor de la capa ACTIVA, sin luz ni textura: con el material encima no se
        // entiende que estas pintando (es el mismo criterio que weightPaintOn).
        bool vertexPaintOn;
        // rellena weightPaintColor (rampa azul 0 -> amarillo 0.5 -> rojo 1) desde UNO de los dos
        // grupos. Sin grupo valido queda todo en azul (peso 0). Una entrada POR ENTIDAD, sin flags:
        //   ConstruirColorPeso   -> VIEWPORT 3D: vertex group 'grupo', pesos por CONTROL-POINT
        //                           (mapeados al render con vertCtrlPoint).
        //   ConstruirColorPesoUV -> EDITOR UV:   UV group 'uvGrupo', pesos por CORNER (render-vert).
        void ConstruirColorPeso(int grupo);
        void ConstruirColorPesoUV(int uvGrupo);
    private:
        void ColorPesoDesdeRenderVert(const std::vector<float>& rvW); // rampa comun de las dos
    public:

        // centro geometrico LOCAL (promedio de las posiciones unicas). El foco ("."")
        // y el pivot lo usan (en mundo) en vez del origen. Se calcula en CalcularBordes.
        Vector3 centroGeom;
        float   radioGeom;  // radio del bounding LOCAL alrededor de centroGeom (lo usa el foco/encuadre)

        // AABB LOCAL de la geometria (min/max de las posiciones). Lo usa el objeto
        // Culling para el frustum culling. Se recalcula en CalcularBordes -el MISMO
        // invalidador que centroGeom/radioGeom: corre en cada cambio de geometria-,
        // asi que no hay que invalidarlo a mano. aabbOk=false = sin geometria (o
        // nunca corrio CalcularBordes): el que pregunta cae a la esfera radioGeom.
        Vector3 aabbMin, aabbMax;
        bool    aabbOk;
        Vector3 PuntoFoco() const W3D_OVERRIDE; // centro geometrico en MUNDO
        float   RadioFoco() const W3D_OVERRIDE; // radio del bounding en MUNDO (centroGeom*escala); foco '.'

        // escala un radio LOCAL (alrededor de cLocal) a MUNDO tomando el mayor de los 3 ejes (robusto
        // a escala no uniforme). Lo usan RadioFoco (malla entera) y el encuadre del foco en Edit Mode.
        float   EscalarRadioLocal(const Vector3& cLocal, float rLocal) const;

        // recalcula edges + posRep + bordesBuf + centroGeom. invalidarEdit=false lo
        // usa el transform de malla al CONFIRMAR (mueve vertices sin cambiar la
        // topologia -> conserva la malla de edicion y su seleccion).
        // reagruparPosRep=false CONSERVA el agrupamiento posRep existente en vez de rederivarlo
        // por posicion: clave al confirmar un move (el snap puede dejar 2 verts INDEPENDIENTES
        // encima -> rederivar por posicion los SOLDARIA sin querer, desincronizando caras y edit).
        void CalcularBordes(bool invalidarEdit = true, bool reagruparPosRep = true);

        // recalcula el array de normales desde faces3d (Newell por cara, agrupado
        // por POSICION si meshSmooth). Lo usa el transform de malla al confirmar.
        void RecalcularNormales();

        void SincronizarCornerNormal(); // normals[] -> cornerNormal (tras escribir normales)
        // FLAT: rellena cornerNormal con la normal POR CARA -> GenerarRender NO mergea verts
        // entre caras distintas (cada cara su normal) => shading plano tras extrude/loop cut/etc.
        void CornerNormalPorCara();

        // SMOOTH con bordes SHARP: normal por GRUPO DE SUAVIZADO (promedia las caras de
        // alrededor sin cruzar un borde sharp). meshSmooth=false => todo flat. Es lo que
        // permite el cilindro (lados suaves, tapas planas con el aro marcado sharp).
        void CornerNormalConSharp();

        // marca/desmarca como SHARP los bordes seleccionados en Edit Mode (edita sharpEdges
        // por POSICION + regenera). Definida en el editor (lee la EditMesh).
        void MarcarSharpEdit(bool sharp);

        // marca/desmarca como SEAM (costura UV) los bordes seleccionados (edita seamEdges por
        // POSICION). Se ven magenta; el unwrap los usa para cortar. Definida en el editor.
        void MarcarSeamEdit(bool seam);

        // clave de posicion (12 bytes de los 3 floats) para sharpEdges; el borde es el par
        // de extremos ordenado. Comparten la MISMA posicion -> mismos bytes -> match exacto.
        // DEFINIDA EN EL CORE (Mesh.cpp), no en el editor: la usa PodarMarcasSinArista y el
        // Core tiene que linkear sin main/ (juegos de ui/, Android, WebGL, Symbian).
        static std::string SharpEdgeKey(const float* a, const float* b);

        // ====================================================================
        //  RE-ANCLAR sharp/seam DESPUES DE MOVER POSICIONES
        //
        //  La clave de sharpEdges/seamEdges son los BYTES CRUDOS de la posicion
        //  de los dos extremos: es ESTABLE ante el re-merge de render-verts
        //  (para eso se eligio) pero NO ante MOVER un vertice. Cada lugar que
        //  escribia vertex[] a mano tenia que ACORDARSE de re-anclar, y el que
        //  se olvidaba tiraba TODAS las marcas en silencio: seguian vivas en
        //  memoria (se dibujaban) pero al guardar no habia punto donde
        //  escribirlas. Paso con Apply Location/Rotation/Scale y con el merge
        //  de vertices, en un flujo normalisimo (marcar costuras -> Apply Scale
        //  -> desplegar UV).
        //
        //  'posViejas' es una copia de vertex[] de ANTES del movimiento
        //  (nViejo*3 floats) y se llama DESPUES de escribir. Si nViejo no es el
        //  vertexSize actual NO hay correspondencia por indice (eso es un
        //  REBUILD, no un move) y no re-ancla nada.
        //
        //  NO LLAMARLA A MANO: usa el guard W3dMoverVerts, que la llama solo.
        void ReanclarMarcasPos(const GLfloat* posViejas, int nViejo);

        // ====================================================================
        //  PODAR las marcas que YA NO SON UNA ARISTA DE ESTA MALLA
        //
        //  Hermana de ReanclarMarcasPos, para el otro tipo de cambio: no MOVER
        //  posiciones sino QUEDARSE CON UN PEDAZO (Separate/P, y el borrado que
        //  lo acompania). Ahi las claves no hay que moverlas: hay que TIRAR las
        //  que ya no corresponden, y solo esas.
        //
        //  El criterio es "la arista existe": una marca sobrevive si sus DOS
        //  extremos siguen siendo posiciones de la malla Y hay un borde de esta
        //  malla que los une (edges[], que CalcularBordes arma desde faces3d +
        //  looseEdges). Mirar solo si las POSICIONES existen no alcanza: al
        //  separar una parte, las esquinas del otro pedazo pueden caer en las
        //  mismas posiciones y la marca reaparecia como una costura FANTASMA
        //  sobre una arista que el usuario nunca marco.
        //
        //  NO HACE FALTA LLAMARLA A MANO EN CADA OPERACION: la llama sola
        //  CalcularBordes, que es EL UNICO lugar donde se recalcula edges[].
        //  Toda op de edicion (borrar, aplicar un modificador, extruir, cortar,
        //  separar, importar...) desemboca ahi via GenerarRender o directo, asi
        //  que la clase entera queda cerrada en un solo punto y no hay forma de
        //  olvidarse en la proxima. Se deja publica para los casos que rehacen
        //  la geometria sin pasar por CalcularBordes.
        //
        //  INVARIANTE DE ORDEN (importa): un MOVIMIENTO tiene que RE-ANCLAR
        //  antes de que se pode, sino mover un vertice BORRARIA la marca en vez
        //  de seguirla. Lo garantiza W3dMoverVerts: mientras haya un guard
        //  abierto (moverVertsAbiertos > 0) esta funcion no poda nada, y el
        //  cierre del guard re-ancla ANTES de bajar el contador.
        //
        //  INVARIANTE DE POSE (el tercero, hermano del anterior): la poda
        //  compara claves contra las POSICIONES ACTUALES de vertex[], asi que
        //  solo puede decidir con la malla EN LA POSE en la que viven las
        //  marcas. Si una VERTEX ANIM tiene la malla POSADA (vertex[] escrito
        //  por el playback, a proposito SIN re-anclar: las marcas son de la
        //  malla en REPOSO y re-anclarlas por cuadro las destruiria), esta
        //  funcion NO PODA NADA, igual que con el guard abierto. Ver
        //  PosadaPorVertexAnim.
        //
        //  Y ESE INVARIANTE ES DE LA CLASE ENTERA, NO SOLO DE LA PODA: las
        //  marcas viven en el espacio de REPOSO, asi que TODA puerta que las
        //  resuelva por posicion tiene que pedir las posiciones por
        //  Mesh::PosicionesReposo() y no leer vertex[] a mano. Son cuatro y
        //  cada una se rompio por su lado antes de cerrarse aca:
        //    - PODAR   -> esta funcion (se saltea entera si esta posada);
        //    - CREAR   -> MarcarSharpEdit/MarcarSeamEdit (marcar posado nacia
        //                 la clave en el espacio de la pose y al volver a
        //                 reposo la poda se llevaba TODO);
        //    - DIBUJAR -> los seams magenta del Edit Mode y las normales de
        //                 CornerNormalConSharp (posada no matcheaba ninguna:
        //                 las marcas se apagaban justo mientras se anima);
        //    - GUARDAR -> el escritor del .w3dm, y ademas el guardado deja la
        //                 malla EN REPOSO antes de serializar
        //                 (W3dReposoVertexAnim), o el archivo sale con la pose
        //                 HORNEADA como geometria.
        //
        //  Necesita edges[] al dia. Si hay geometria pero edges esta vacio no
        //  toca nada (no hay con que decidir); si NO hay geometria (ni caras ni
        //  aristas sueltas, o vertex[] vacio) tira TODAS las marcas, porque no
        //  existe ninguna arista donde puedan vivir.
        //  Devuelve cuantas marcas se tiraron.
        int PodarMarcasSinArista();

        // ====================================================================
        //  LA MALLA ESTA POSADA POR UNA VERTEX ANIM? (no esta en REPOSO)
        //
        //  El playback (EvalVertexAnim/ApplyVertexFrame/Blend) pisa vertex[]
        //  con la pose del cuadro y NO re-ancla sharp/seam (bien: las marcas
        //  son de la malla en reposo; re-anclarlas cada cuadro las destruye).
        //  Entonces, POSADA, las posiciones de vertex[] no son las de las
        //  claves y cualquier decision por posicion es basura.
        //
        //  El REPOSO de una malla con vertex anim es su KEYFRAME BASE: el
        //  primer keyframe con posiciones (el mismo que valida 'PoseBaseOk' de
        //  vatest y con el que se edita la topologia).
        //
        //  LA RESPUESTA SALE DE UN ESTADO REAL, no de comparar posiciones: el
        //  flag 'posadaPorAnim', que escribe SOLO el playback. La version vieja
        //  contestaba "vertex[] != frames[0] byte a byte" y eso NO distingue
        //  "la poso el playback" de "la edito el usuario sin autokey": un move
        //  en Edit Mode, Set Origin, Apply Loc/Rot/Scale, Merge By Distance,
        //  Apply Mirror o Apply Modifier dan POSADA en una malla que nadie poso
        //  y dejaban la poda apagada hasta que el usuario scrubeara al cuadro
        //  base (marcas fantasma + un aviso al guardar que no se podia
        //  accionar). Ver el comentario de Mesh::posadaPorAnim.
        //
        //  Sin ninguna anim con capa de posiciones -> false: nadie puede estar
        //  moviendo vertex[] por atras (si se borro la anim con la malla
        //  posada, esa pose PASA A SER la geometria).
        bool PosadaPorVertexAnim() const;

        // ====================================================================
        //  POSICIONES EN LAS QUE VIVEN LAS MARCAS (sharp/seam) -- LA OTRA PUNTA
        //  DEL INVARIANTE DE POSE
        //
        //  Las claves de sharpEdges/seamEdges son los bytes de la posicion de
        //  la malla EN REPOSO. Con la malla en reposo eso es vertex[] y no hay
        //  nada que pensar; POSADA por una vertex anim, vertex[] esta en la
        //  pose del cuadro y NINGUNA clave matchea. Entonces TODO el que
        //  resuelve una marca por posicion -crearla (MarcarSharpEdit/
        //  MarcarSeamEdit), dibujarla (seams magenta, normales con sharp) o
        //  escribirla al .w3dm- tiene que pedir las posiciones POR ACA y no
        //  usar vertex[] directo. Devuelve vertex[] o el keyframe base, siempre
        //  con la MISMA numeracion (indice de render-vert) y nunca NULL si hay
        //  geometria.
        const GLfloat* PosicionesReposo() const;

        // posiciones del KEYFRAME BASE alineadas con vertex[] (primer keyframe
        // con posiciones de la primera anim que anima posiciones), o NULL si no
        // hay anims de posiciones o los frames no alinean con vertexSize.
        const GLfloat* PosBaseVertexAnim() const;

        // === PROYECCIONES de UV sobre las CARAS SELECCIONADAS (Edit Mode) ===
        // tipo: 0=Cube, 1=Cylinder, 2=Sphere. Calcula UV por corner desde la POSICION del vert
        // y reconstruye. (Las "from view" van por el editor: necesitan la camara -> arman el
        // uvPorCorner y llaman EscribirUVProyeccion.)
        void ProyectarUVCaras(int tipo);

        // escribe el UV (2 floats por corner, en ORDEN de faces3d) en la capa activa SOLO en los
        // corners de las caras SELECCIONADAS (el resto conserva su UV) y reconstruye (re-split).
        void EscribirUVProyeccion(const std::vector<float>& uvPorCorner);

        // ===== BUFFERS PRECALCULADOS (no se rehacen por frame, solo se dibujan) =====
        // lineas del contorno/wireframe (pares de puntos xyz), armadas en CalcularBordes.
        std::vector<GLfloat> bordesBuf;
        // RECETA de un vertice NUEVO creado por un corte (loop cut / subdivision): es la
        // INTERPOLACION de dos verts viejos -> pos = (1-t)*vert[a] + t*vert[b]. La usa
        // RemapVertexAnims para recalcular ese vert en CADA keyframe (que el corte SIGA la
        // animacion, no quede estatico). a==b = copia simple. La op de corte llena
        // vtxAnimRecipes (una por vert nuevo, desde vtxAnimRecipeBase) antes de GenerarRender.
        // soloUV (duplicar/extrude, pedido del dueno): la receta aplica SOLO a la capa UV
        // (el vert nuevo COPIA el UV animado de su vert de ORIGEN en cada keyframe);
        // posiciones y normales quedan ESTATICAS en la pose actual (regla previa intacta:
        // lo duplicado/extruido no hereda la animacion de posiciones).
        struct VtxRecipe { int a, b; float t; bool soloUV;
            VtxRecipe(): a(-1), b(-1), t(0.0f), soloUV(false) {}
            VtxRecipe(int A,int B,float T): a(A), b(B), t(T), soloUV(false) {} };
        std::vector<VtxRecipe> vtxAnimRecipes; // receta de cada vert nuevo [vtxAnimRecipeBase, base+size)
        int vtxAnimRecipeBase;                 // primer indice de vert nuevo cubierto por las recetas

        // skinGeomVersion con el que se lleno bordesBuf. Si difiere, algo movio vertex[]
        // POR FUERA (tipico: una vertex anim parada en un frame) y el contorno quedo en la
        // pose vieja -> RenderBordes lo re-arma desde vertex[]. Editar no bumpea la version
        // (va por EditMesh), asi que no dispara re-arme espurio.
        unsigned bordesGeomVersion;
        // re-arma bordesBuf desde vertex[] usando las aristas actuales (sin recalcular
        // topologia): para seguir la deformacion de una vertex anim en Modo Objeto.
        void RefrescarBordesDesdeVertex();

        // lineas de los overlays de normales (pares de puntos xyz), armadas en
        // CalcularOverlayNormales SOLO cuando cambia la geometria o el largo L.
        std::vector<GLfloat> normFaceBuf, normCustomBuf, normVertBuf;
        float overlayLcache; // largo (OverlayNormalSize) con el que se armaron; -1 = rehacer

        // ===== EDIT MODE (en una clase APARTE: EditMesh) =====
        // La malla de render de Whisk3DCore (esto) NO carga datos de edicion. La
        // malla de EDICION (vertices/bordes editables, seleccion, vertex color) vive
        // en EditMesh, se construye on-demand al entrar a Edit Mode y se invalida al
        // cambiar la geometria. Ver EditMesh.h.
        EditMesh* edit;       // NULL hasta que se entra a Edit Mode (fwd-decl: core no incluye EditMesh)
        void EnsureEdit();    // construye 'edit' desde esta malla si hace falta
        void InvalidarEdit(); // borra 'edit' (geometria cambio)
        void RenderEditOverlay(); // dibuja el overlay de edit (hook: el cuerpo deref edit, vive en main/)

        // delegan en EditMesh (los llaman SeleccionarTodo/DeseleccionarTodo/Invertir)
        void EditSeleccionarTodo(bool sel) W3D_OVERRIDE;
        void EditInvertir() W3D_OVERRIDE;

        // BORRA la seleccion de Edit Mode segun el modo (SelVertex/SelEdge/SelFace):
        //  - SelFace:   borra las caras seleccionadas; mantiene sus bordes+vertices
        //  - SelEdge:   borra las aristas sel + las caras que las usan; mantiene vertices
        //  - SelVertex: borra los verts sel + toda arista/cara que los USE; las DEMAS aristas de esas caras
        //               quedan como lineas sueltas y los verts que quedan aislados como puntos sueltos (queda
        //               el wireframe, no se pierde el resto de la malla)
        // Reconstruye la malla de render (preserva UV/normales) e invalida el edit.
        void BorrarSeleccionEdit(int mode);

        bool BorrarEdgeLoopEdit(); // Delete > Edge Loops: disuelve el loop seleccionado (inverso del loop cut)
        // EXTRUDE en Edit Mode segun el modo (Vertex/Edge/Face): vertice->arista,
        // arista->quad (cadena=pared), cara->tapa+paredes de contorno. Duplica la
        // "tapa" (offset epsilon) y la deja seleccionada para que el editor la mueva.
        // outDirLocal = direccion (normal); outConstrain=false si no hay caras (libre).
        bool ExtruirEdit(Vector3& outDirLocal, bool& outConstrain);

        // reconstruye la malla de edicion y deja seleccionados los verts marcados en
        // selVertNuevo (indice GPU nuevo). Lo usan extrude/duplicate/delete.
        void ReconstruirEditSel(const std::vector<char>& selVertNuevo);

        // restaurar la seleccion POR POSICION alrededor de un GenerarRender (que re-numera
        // los verts): capturar ANTES, reconstruir DESPUES. Robusto al re-merge.
        std::vector<Vector3> CapturarPosSel(const std::vector<char>& selPorGPU);
        void ReconstruirEditSelPorPos(const std::vector<Vector3>& posSel);

        // DUPLICATE (Shift+D): copia la seleccion (verts/aristas/caras), offset chico,
        // y la deja seleccionada para moverla (libre). false si no hay seleccion.
        bool DuplicarSeleccionEdit();

        // RIP (V): separa la malla a lo largo de la seleccion (flood-fill de lados + duplica los verts del corte).
        // Deja seleccionada la pieza nueva. false si la seleccion no separa nada.
        bool RipSeleccionEdit();

        // F "New Edge/Face from Vertices": con los verts sel crea arista(2)/tri(3)/
        // ngon(4+). NO crea verts (conecta los existentes). false si hay < 2.
        bool CrearCaraEdit();

        // SHADE SMOOTH/FLAT (menu Face) sobre las caras sel: smooth=normal promedio
        // por posicion (redondea); flat=split + normal de cara (aplana). false si
        // no hay caras seleccionadas.
        bool ShadeEdit(bool smooth);

        // RECALCULATE NORMALS (menu Face): re-orienta el winding de las caras sel (o
        // todas si no hay sel) para que las normales miren para AFUERA respecto al centro
        // de la malla; inside=true las deja para adentro (tilde del panel). Arregla caras
        // invisibles (normal al reves). No cambia la geometria, solo el orden de los verts.
        bool RecalcularOrientacionEdit(bool inside);

        // FLIP NORMALS (menu Mesh > Normals > Flip): invierte el winding de las caras sel (o todas) sin
        // condicion -> da vuelta las normales. Simple: si miran para un lado, quedan para el otro.
        bool FlipNormalesEdit();

        // TRIANGULATE FACES (Ctrl+T, menu Face): parte las caras SELECCIONADAS de >3 lados en triangulos
        // (abanico desde el 1er vertice). Preserva UV/color/normal por corner. false si no hay caras que triangular.
        bool TriangularSeleccionEdit();

        // CLIPPING del modificador Mirror (edit-time): impide que los verts CRUCEN el plano del mirror al moverlos
        // (half-space). editKs = verts que se mueven (indices en edit->pos); startLocal = su pos LOCAL
        // al empezar el transform (define de que lado estan). Los que arrancan a <mergeDist quedan pegados al plano.        
        void ClipMirrorVerts(const std::vector<int>& editKs, const std::vector<Vector3>& startLocal);

        // LOOP CUT (Ctrl+R): corta un loop de quads desde la arista startEditEdge (en la
        // malla de edicion) con numCuts cortes; factor in [-1,1] desliza (0=parejo). Crea
        // los verts/caras nuevos. Solo quads. false si no se puede.
        // correctUV: los corners nuevos interpolan uv/color a lo largo de la arista (true, lo normal);
        // false = copian los del extremo mas cercano (sin interpolar).
        bool LoopCutEdit(int startEditEdge, int numCuts, float factor, bool correctUV = true);

        // recorre el loop de quads desde startEditEdge (helper de LoopCut: corte + preview)
        bool LoopCutRecorrido(int startEditEdge, std::vector<int>& rungEg, std::vector<int>& rungA,
                              std::vector<int>& rungB, std::vector<int>& loopFaces, bool& cerrado);

        // vista previa: segmentos de linea (pares de puntos LOCALES) del corte, sin modificar nada
        //esto es del editor y NO va a estar mas aca....
        bool LoopCutPreview(int startEditEdge, int numCuts, float factor, std::vector<float>& outSegs);

        //animacion
        std::vector<VertexAnimation*> animations;

        // PRIMITIVA: tipo + parametros para regenerar la geometria en vivo (el
        // panel "Add" de la ventanita los edita). meshSize = span del cubo/plano
        // o radio del circulo; meshVerts = vertices del circulo (default 8).
        int meshTipo;     // MeshType, -1 si no es una primitiva regenerable
        float meshSize;   // span (cubo/plano) o radio (circulo/esfera) o radio1/base (cono)
        float meshSize2;  // radio2/punta del cono (0 = termina en vertice)
        float meshDepth;  // altura del cono (depth)
        int meshVerts;    // vertices (circulo/cono) o segments/longitud (UV sphere)
        int meshVerts2;   // rings/latitud (UV sphere)
        void Regenerar(); // reconstruye vertex/normals/uv/faces desde los parametros

        // bloques del .w3dm que este Whisk3D no entiende (los trajo una version mas nueva) +
        // el sello de cantidades por dominio con el que se cargaron. Ver W3dAjenos arriba.
        W3dAjenos w3dmAjenos;

        Mesh(Object* parent = NULL, Vector3 pos = Vector3(0,0,0));

        ~Mesh();

        ObjectType getType() W3D_OVERRIDE;

        void LiberarMemoria();
        void RenderObject() W3D_OVERRIDE;
        // true si TODOS sus grupos de material son CALCOMANIA (orden_pasada 1):
        // una sombra SUELTA, declarada aparte del cuerpo. Esas mallas no se dibujan durante
        // el recorrido del arbol: se anotan y se dibujan al final del frame (ver
        // W3dDecalesDibujarPendientes en Mesh.cpp).
        bool EsSoloDecal() const;
        // true si TODOS sus grupos de material son LUZ (mezcla aditiva/sustractiva/
        // multiplicativa/screen; ver W3dMaterialEsLuz): una chispa, un halo, un rayo.
        // Igual que las calcomanias, esas mallas no se dibujan en el recorrido del
        // arbol: se anotan y se dibujan al final (ver W3dLucesDibujarPendientes).
        bool EsSoloLuz() const;
        // dibuja los bordes (edges) como GL_LINES. pushBack empuja el zbuffer
        // atras (para el contorno verde: tapa las lineas internas, deja el borde).
        void RenderBordes(const float* color, float width, bool pushBack);
    public:
        // aplica TODO el estado GL de un material. RenderObject la llama SOLO
        // cuando el material cambia (asi no se repiten llamadas por grupo).
        // offsetEdit = usar el glPolygonOffset del EDIT MODE cuando el material no pide el suyo.
        // PUBLICA desde P4: el lote estatico (Collection) la pide prestada para
        // aplicar el material de cada rango horneado (y despues re-bindea SUS
        // punteros: los de la malla que presto el material no valen para el lote).
        void AplicarMaterial(Material* mat, bool conLuz, bool solido, bool offsetEdit = false);
};

// ============================================================================
//  W3dMoverVerts: LA UNICA PUERTA PARA MOVER POSICIONES DE UNA MALLA
//
//  Escribir vertex[] a mano y acordarse de re-anclar sharp/seam no funciono:
//  el re-anclaje se puso en UNA puerta (EditMesh::EmpujarPosiciones) y las
//  otras -Apply Location/Rotation/Scale, Merge Vertices, Apply Armature- lo
//  seguian tirando entero, en silencio y en flujos normalisimos. El olvido no
//  se arregla con disciplina: se arregla haciendo que no haya nada que
//  recordar.
//
//  USO: se abre el guard sobre la malla, se escriben las posiciones que sea
//  (por indice, sin cambiar la cantidad de verts) y al CERRAR EL SCOPE el
//  destructor re-ancla. Ejemplo:
//
//      {
//          W3dMoverVerts mv(m);          // snapshotea (gratis si no hay marcas)
//          for (...) m->vertex[i] = ...; // mover lo que sea
//      }                                 // <- aca se re-ancla sharp/seam
//      m->GenerarRender();               // recien DESPUES (re-numera los verts)
//
//  Tiene que CERRARSE ANTES de cualquier GenerarRender()/CalcularBordes():
//    (a) el guard compara por INDICE contra el snapshot y un rebuild re-numera
//        los verts (si la cantidad cambia, el guard no hace nada, que es lo
//        correcto);
//    (b) CalcularBordes PODA las marcas que ya no son una arista, y con el
//        movimiento a medio hacer TODAS parecen huerfanas. Por eso el guard
//        marca la malla mientras esta abierto (Mesh::moverVertsAbiertos) y
//        Mesh::PodarMarcasSinArista se saltea: un recalculo en el medio NO
//        dispara una poda que no corresponde. Es una RED, no un permiso:
//        el orden correcto sigue siendo cerrar y despues regenerar.
//  Cuesta CERO cuando la malla no tiene marcas: ni copia el snapshot.
// ============================================================================
class W3dMoverVerts {
    public:
        explicit W3dMoverVerts(Mesh* m);
        ~W3dMoverVerts();
        void Cerrar();   // re-ancla YA (para cerrar antes del fin del scope); idempotente
    private:
        Mesh* malla;
        std::vector<GLfloat> viejo;
        bool cerrado;
        W3dMoverVerts(const W3dMoverVerts&);            // sin copia (C++03: privadas y sin definir)
        W3dMoverVerts& operator=(const W3dMoverVerts&);
};

// ============================================================================
//  W3dPosVerts: LAS POSICIONES **Y SU ESTADO DE POSE**, EN UN SOLO VALOR
//               (LA UNICA PUERTA PARA COPIAR/RESTAURAR vertex[])
//
//  vertex[] NO se explica solo. Las mismas 24 posiciones significan cosas
//  distintas segun Mesh::posadaPorAnim: en false son EL MODELO (el espacio
//  donde viven las claves de sharp/seam y lo que se escribe al .w3dm); en true
//  son LA POSE de un cuadro y ninguna decision por posicion vale. El estado es
//  PARTE del dato.
//
//  Mientras el flag fue un miembro suelto de Mesh, cada puerta que copiaba o
//  restauraba posiciones tenia que ACORDARSE de arrastrarlo, y las que se
//  olvidaron dejaban la copia diciendo "estoy en reposo" con la pose adentro:
//    - Shift+D (W3dDuplicarUno) -> la copia guardaba la pose HORNEADA, sin
//      bloques SHARP/SEAM y con aviso, mientras el original salia perfecto;
//    - Separate (P) -> el GenerarRender del clon podaba el 100% de las marcas;
//    - Ctrl+Z de una op de topologia hecha posado -> restauraba el vertex[]
//      POSADO con el flag apagado y podaba contra la pose;
//    - el snapshot del loop cut, idem.
//  Es exactamente el mismo tipo de olvido que arregla W3dMoverVerts (la puerta
//  de MOVER), y se arregla igual: haciendo que no haya nada que recordar.
//
//  USO:
//      W3dPosVerts p; p.Capturar(orig);   // posiciones + estado, juntos
//      d->vertexSize = orig->vertexSize;  // la TOPOLOGIA la sigue manejando el caller
//      p.EscribirEn(d);                   // las dos cosas, siempre juntas
//
//  Un comando de undo que guarde su vertex[] ACA ADENTRO (en vez de un
//  std::vector<GLfloat> pelado) ya no puede olvidarse: no existe la forma de
//  guardar uno sin el otro.
//
//  NO toca vertexSize a proposito: el tamano es de la TOPOLOGIA y lo maneja el
//  caller (que tambien restaura faces3d/capas). EscribirEn realoca vertex[] con
//  lo que haya guardado; sin posiciones no hay pose posible -> el estado queda
//  en false.
// ============================================================================
class W3dPosVerts {
    public:
        std::vector<GLfloat> pos;   // copia de vertex[] (vertexSize*3); vacio = la malla no tenia
        bool posada;                // el Mesh::posadaPorAnim QUE CORRESPONDE a esas posiciones
        W3dPosVerts() : posada(false) {}
        void Capturar(const Mesh* m);        // lee vertex[] + el estado
        void EscribirEn(Mesh* m) const;      // realoca vertex[] + escribe el estado
        void IntercambiarCon(Mesh* m);       // swap EN EL LUGAR con lo vivo (undo/redo de un move)
        void Intercambiar(W3dPosVerts& o);   // swap de los dos snapshots (undo/redo de topologia)
};

// ===================================================
// Funciones globales
// ===================================================
Object* NewMesh(MeshType type = MeshType::cube, Object* parent = NULL, bool query = false);

// HOOK del EDITOR: RenderObject lo llama tras dibujar la malla para pintar los overlays
// (contorno de seleccion, normales, edit mode). NULL por defecto -> una app/juego sin
// editor no dibuja overlays (el Core no conoce esa logica).
extern void (*g_meshOverlayHook)(Mesh*);

// (P4) sello del PASE DE ESCENA actual para el lote estatico: lo incrementa el
// viewport al abrir cada pase; la Collection con lote marca a sus mallas
// horneadas con este valor y Mesh::RenderObject las saltea SOLO ese frame.
extern unsigned w3dLoteStamp;

// ---- PASE DIFERIDO DE CALCOMANIAS (sombras sueltas) ----
// Ver el comentario largo en Mesh.cpp. Una calcomania NO escribe z; si se dibuja
// durante el recorrido del arbol, cualquier cosa OPACA que venga despues y ocupe
// el mismo lugar (los troncos, la malla EscenarioAlpha del final del .w3d) la
// borra. Por eso las mallas que son SOLO calcomania se anotan y se dibujan
// cuando la escena opaca ya esta completa.
// El que dibuja la escena llama Limpiar antes y Dibujar despues (igual que con
// las particulas): ver Viewport3D::Render.
void W3dDecalesLimpiarPendientes();
void W3dDecalesDibujarPendientes();
// LUCES SUELTAS (mallas 100% aditivas/sustractivas: chispas, halos, rayos,
// estelas). Mismo problema al reves que la calcomania: no escriben z (no pueden
// tapar nada), asi que cualquier translucido que se dibuje DESPUES y ocupe el
// mismo lugar las borra. Se anotan y se dibujan al final, entre las calcomanias
// y las particulas. Ver el comentario largo en Mesh.cpp.
void W3dLucesLimpiarPendientes();
void W3dLucesDibujarPendientes();
// "dibujar la calcomania / la luz ACA MISMO, sin diferir". Lo prende el pase del
// espejo (el reflejo tiene su propia matriz y su propio recorte por estencil) y
// los propios flushes.
extern bool w3dDecalesInline;

#include "animation/VertexAnimation.h"

#endif