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

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#if !defined(_WIN32) && !defined(W3D_SYMBIAN)
#include <GL/glext.h>
#endif

#include "crossplatform.h" // MeshIndex (16 bits N95 / 32 bits PC/Android/WebGL/N8)
#include "Materials.h"
#include "Objects.h"

typedef GLshort Edge[2];

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
        ColorLayer(const std::string& n) : nombre(n), porVertice(false) {}
};

class VertexGroup {
    public:
        std::string nombre;
        std::vector<int>     verts;  // indices de posicion afectados (SPARSE, paralelo a pesos)
        std::vector<GLfloat> pesos;  // peso 0..1 de cada vert
        VertexGroup(const std::string& n) : nombre(n) {}
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

// ===================================================
// Clase Mesh
// ===================================================
class Mesh : public Object { 
    public:
        // defaults en el constructor (Mesh.cpp), C++03
        int vertexSize;
        GLfloat* vertex;

        // capas persistentes EXCLUSIVAS de la malla (lista de punteros + indice ACTIVO,
        // -1 = ninguna). El render usa la activa; el editor las crea/edita/borra.
        std::vector<UVMap*>       uvMaps;       int uvMapActivo;
        std::vector<ColorLayer*>  colorLayers;  int colorActivo;
        std::vector<VertexGroup*> vertexGroups; int grupoActivo;

        // STACK de MODIFICADORES (del EDITOR): lista de punteros + activo (-1=ninguno). El core solo los guarda;
        // el editor los crea/gestiona (main/edit) y a futuro genera la malla de render. El ORDEN importa.
        std::vector<Modifier*>    modificadores; int modificadorActivo;

        // gestion del stack (DEFINIDAS EN EL EDITOR, main/edit/MeshEdit.cpp; el core solo las declara + las llama en ~Mesh):
        void AgregarModificador(int tipo);           // agrega uno del tipo dado al final + lo deja activo
        void QuitarModificadorActivo();              // borra el modificador activo
        void MoverModificador(int dir);              // reordena el activo (-1 sube / +1 baja) intercambiando
        void LiberarModificadores();                 // libera todos (lo llama el destructor)
        std::string NombreModificador(int i) const;  // nombre del modificador i (para el selector, sin exponer la clase)

        // MALLA GENERADA por el stack de modificadores (buffers de RENDER). Cuando genValido, RenderObject dibuja
        // ESTA en vez de la editable (vertex[]/faces[]). La produce el EDITOR (GenerarMallaModificada, main/edit);
        // el core solo la guarda + la dibuja. La malla EDITABLE (vertex/faces3d) queda intacta (se edita esa).
        GLfloat*  genVertex; GLbyte* genNormals; GLfloat* genUV; GLubyte* genColor;
        MeshIndex* genFaces; int genVertexSize; int genFacesSize;
        std::vector<MaterialGroup> genMaterialsGroup;
        bool genValido; // true = hay malla generada por modificadores -> usarla en el render
        void GenerarMallaModificada(); // EDITOR: rehace las gen buffers aplicando el stack (genValido=false si vacio)
        void LiberarMallaModificada(); // libera las gen buffers
        void AplicarModificadorActivo(); // "Apply Modifier": hornea la malla generada en la editable + saca el modificador

        // NORMAL autoritativa POR CORNER (3 GLbyte/corner). El render la DERIVA (no al
        // reves): las edit-ops la ACARREAN (copiar/interpolar via los helpers de capas) ->
        // mover/loop-cut/etc. NO arruinan las normales; solo RecalcularNormales/shade las
        // reescriben. Si queda stale (size != corners*3), el render cae a normals[] (fallback).
        std::vector<GLbyte> cornerNormal;

        // crea las capas iniciales (1 UVMap "UVMap", 1 ColorLayer "Col") desde los arrays
        // de render actuales si todavia no hay ninguna. Por corner (orden de faces3d).
        void PoblarCapas();
        void LiberarCapas(); // borra y libera todas las capas (destructor / Regenerar)
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
        void GenerarRender();

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

        // centro geometrico LOCAL (promedio de las posiciones unicas). El foco ("."")
        // y el pivot lo usan (en mundo) en vez del origen. Se calcula en CalcularBordes.
        Vector3 centroGeom;
        float   radioGeom;  // radio del bounding LOCAL alrededor de centroGeom (lo usa el foco/encuadre)
        Vector3 PuntoFoco() const override; // centro geometrico en MUNDO
        float   RadioFoco() const override; // radio del bounding en MUNDO (centroGeom*escala); foco '.'

        // escala un radio LOCAL (alrededor de cLocal) a MUNDO tomando el mayor de los 3 ejes (robusto
        // a escala no uniforme). Lo usan RadioFoco (malla entera) y el encuadre del foco en Edit Mode.
        float   EscalarRadioLocal(const Vector3& cLocal, float rLocal) const;

        // recalcula edges + posRep + bordesBuf + centroGeom. invalidarEdit=false lo
        // usa el transform de malla al CONFIRMAR (mueve vertices sin cambiar la
        // topologia -> conserva la malla de edicion y su seleccion).
        void CalcularBordes(bool invalidarEdit = true);

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
        static std::string SharpEdgeKey(const float* a, const float* b);

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
        void EditSeleccionarTodo(bool sel) override;
        void EditInvertir() override;

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
        bool LoopCutEdit(int startEditEdge, int numCuts, float factor);

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

        Mesh(Object* parent = NULL, Vector3 pos = Vector3(0,0,0));

        ~Mesh();

        ObjectType getType() override;

        void LiberarMemoria();
        void RenderObject() override;
        // dibuja los bordes (edges) como GL_LINES. pushBack empuja el zbuffer
        // atras (para el contorno verde: tapa las lineas internas, deja el borde).
        void RenderBordes(const float* color, float width, bool pushBack);
    private:
        // aplica TODO el estado GL de un material. RenderObject la llama SOLO
        // cuando el material cambia (asi no se repiten llamadas por grupo).
        void AplicarMaterial(Material* mat, bool conLuz, bool solido);
};

// ===================================================
// Funciones globales
// ===================================================
Object* NewMesh(MeshType type = MeshType::cube, Object* parent = NULL, bool query = false);
int DuplicateMesh(int meshIndex);

// HOOK del EDITOR: RenderObject lo llama tras dibujar la malla para pintar los overlays
// (contorno de seleccion, normales, edit mode). NULL por defecto -> una app/juego sin
// editor no dibuja overlays (el Core no conoce esa logica).
extern void (*g_meshOverlayHook)(Mesh*);

#include "animation/VertexAnimation.h"

#endif