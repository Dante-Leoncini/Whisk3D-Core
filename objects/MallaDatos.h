#ifndef W3D_MALLADATOS_H
#define W3D_MALLADATOS_H
// ============================================================================
//  MallaDatos — GEOMETRIA COMPARTIDA entre mallas (el "MeshData" del almacen
//  de recursos, io/W3dRecursos.h).
//
//  EL BUG QUE ARREGLA (el mismo que el cache de texturas, pero en mallas): el
//  formato .w3d declara `Wavefront { filePath }` por objeto, y cada objeto
//  RE-IMPORTA su OBJ entero: N cajas del mismo modelo = N copias identicas de
//  vertex/uv/normals/color/faces en el heap. Con el cache, la PRIMERA
//  importacion de una ruta registra sus arrays aca (MallaDatos toma la
//  posesion) y las siguientes REAPUNTAN a los mismos arrays y suman una
//  referencia (W3dRecursoRetener). El resto del Mesh (transform, materiales,
//  capas del editor, faces3d, scripts, pose) sigue siendo POR INSTANCIA.
//
//  COPY-ON-WRITE: los datos compartidos son INMUTABLES en runtime. Todo camino
//  que ESCRIBE una capa de render llama antes a Mesh::DesinstanciarDatos(mask)
//  (Mesh.cpp): si la capa esta compartida se copia a memoria propia y recien
//  ahi se escribe -- editar una caja NO mueve las otras 16. El contador de
//  desinstanciadas se ve en `meminfo` (una escena que desinstancia mucho esta
//  pagando el cache sin usarlo).
//
//  IDENTIDAD: la ruta del archivo de origen (+ "|nomerge" si se importo sin
//  merge), la misma identidad por-nombre de todo el almacen. NO hay GUIDs.
//
//  A/B: `mallacache off|on` (harness) reproduce el motor de antes (una copia
//  por objeto) con el MISMO binario, para medir el ahorro con `meminfo`.
//
//  C++03. Motor generico: aca no hay nombres de ningun juego.
// ============================================================================
#include "Mesh.h"
#include <string>

// la geometria compartida: exactamente las capas de render pesadas del Mesh.
// Es dueña de sus arrays; los Mesh que la comparten apuntan ADENTRO de esto
// (mismos punteros) con la mascara W3DMD_* diciendo que capas.
struct MallaDatos {
    int        vertexSize;
    int        facesSize;
    GLfloat*   vertex;       // 3 por vert
    GLubyte*   vertexColor;  // 4 por vert (NULL si el modelo no trae color)
    GLbyte*    normals;      // 3 por vert (NULL si no trae normales)
    GLfloat*   uv;           // 2 por vert (NULL si no trae uv)
    MeshIndex* faces;        // index buffer
    // true = es un MallaRecurso (la malla 3D con nombre del proyecto, MallaRecurso.h): el almacen
    // la DESCARGA al soltar la ultima referencia (el recurso sigue en el registro); false = la
    // geometria compartida de un .obj, que se borra entera.
    bool       esRecurso;

    MallaDatos() : vertexSize(0), facesSize(0), vertex(0), vertexColor(0),
                   normals(0), uv(0), faces(0), esRecurso(false) {}
    ~MallaDatos();           // libera todos los arrays
    long Bytes() const;      // medida para meminfo
};

// A/B del cache (comando 'mallacache off|on' del harness). Default: prendido.
void MallasCacheActivo(bool on);
bool MallasCacheEstaActivo();

// La llamada del IMPORTADOR (ImportWOBJ), despues de armar la malla completa:
// si 'id' ya esta registrada y la geometria coincide (tamanos y capas), libera
// los arrays recien importados y REAPUNTA la malla a los compartidos (+1 ref);
// si no, registra los arrays de esta malla como el dato compartido de 'id'.
// Con el cache apagado no hace nada (el comportamiento de antes).
void W3dMallaCompartirTrasImport(Mesh* m, const std::string& id);

// contador de mallas desinstanciadas por COW (lo define Mesh.cpp; meminfo lo muestra)
extern int W3dMallaDesinstanciados;

#endif
