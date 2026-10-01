// ============================================================================
//  Whisk3DCore (engine) — MallaDatos: geometria compartida entre mallas.
//  Ver el contrato en MallaDatos.h. La contabilidad (id -> descriptor,
//  refcount, liberacion al llegar a 0) es del almacen de recursos
//  (io/W3dRecursos.h), tipo W3DREC_MALLA; aca vive lo especifico de mallas.
//
//  NOTA SYMBIAN: este .cpp NO esta en el .mmp (alli no existe el camino
//  Wavefront-del-.w3d que comparte). Por eso Mesh.cpp (que SI compila alla)
//  no llama a nada de aca directo: la liberacion de la referencia pasa por el
//  hook W3dMallaDatosSoltarHook, que este archivo setea al registrarse. Si
//  nadie registra, el hook queda NULL y Mesh no depende del almacen.
// ============================================================================
#include "MallaDatos.h"
#include "MallaRecurso.h"   // W3dMallasAsegurarOps: el alta del tipo en el almacen
#include "io/W3dRecursos.h"
#include <stdio.h>

MallaDatos::~MallaDatos() {
    delete[] vertex; delete[] vertexColor; delete[] normals;
    delete[] uv; delete[] faces;
}

long MallaDatos::Bytes() const {
    long b = 0;
    if (vertex)      b += (long)vertexSize * 3L * (long)sizeof(GLfloat);
    if (vertexColor) b += (long)vertexSize * 4L * (long)sizeof(GLubyte);
    if (normals)     b += (long)vertexSize * 3L * (long)sizeof(GLbyte);
    if (uv)          b += (long)vertexSize * 2L * (long)sizeof(GLfloat);
    if (faces)       b += (long)facesSize * (long)sizeof(MeshIndex);
    return b;
}

static bool gMallaCacheOn = true;
void MallasCacheActivo(bool on) { gMallaCacheOn = on; }
bool MallasCacheEstaActivo()    { return gMallaCacheOn; }

// los ops del tipo W3DREC_MALLA (Cargar/Liberar) y el gancho con el que Mesh suelta la
// referencia se registran en UN solo lugar, junto con las mallas del proyecto: el tipo es
// el mismo para la geometria de un .obj y para una malla con nombre (MallaRecurso.cpp).
static void AsegurarOpsMalla() { W3dMallasAsegurarOps(); }

void W3dMallaCompartirTrasImport(Mesh* m, const std::string& id) {
    if (!gMallaCacheOn || !m || !m->vertex || m->vertexSize <= 0 || id.empty()) return;
    if (m->datosRec) return;   // ya comparte (no deberia pasar en un import fresco)
    AsegurarOpsMalla();

    W3dRecurso* r = W3dRecursoBuscar(W3DREC_MALLA, id);
    if (r && r->estado == W3DREC_LISTO && r->dato) {
        MallaDatos* d = (MallaDatos*)r->dato;
        // compartir SOLO si la geometria recien importada es equivalente: mismos
        // tamanos y mismas capas presentes. Si el archivo cambio en el medio de
        // la sesion los tamanos difieren y cada uno se queda con lo suyo.
        if (d->vertexSize != m->vertexSize || d->facesSize != m->facesSize ||
            (!d->vertexColor) != (!m->vertexColor) ||
            (!d->normals)     != (!m->normals) ||
            (!d->uv)          != (!m->uv)) return;

        unsigned mask = W3DMD_POS | W3DMD_FACES;
        delete[] m->vertex; m->vertex = d->vertex;
        delete[] m->faces;  m->faces  = d->faces;
        if (m->vertexColor) { delete[] m->vertexColor; m->vertexColor = d->vertexColor; mask |= W3DMD_COL; }
        if (m->normals)     { delete[] m->normals;     m->normals     = d->normals;     mask |= W3DMD_NOR; }
        if (m->uv)          { delete[] m->uv;          m->uv          = d->uv;          mask |= W3DMD_UV; }
        m->datosComp = (unsigned char)mask;
        m->datosRec  = r;
        W3dRecursoRetener(r, W3DREC_PERMANENTE);
        return;
    }
    if (r) return;   // FALLO/EN_VUELO recordado: no ensuciar

    // primera importacion de esta ruta: MallaDatos TOMA la posesion de los
    // arrays de la malla (sin copiar) y queda como el dato compartido del id.
    MallaDatos* d = new MallaDatos();
    d->vertexSize  = m->vertexSize;
    d->facesSize   = m->facesSize;
    d->vertex      = m->vertex;
    d->vertexColor = m->vertexColor;
    d->normals     = m->normals;
    d->uv          = m->uv;
    d->faces       = m->faces;
    W3dRecurso* nr = W3dRecursoRegistrarExterno(W3DREC_MALLA, id, d, d->Bytes());
    if (!nr) { d->vertex = 0; d->vertexColor = 0; d->normals = 0; d->uv = 0; d->faces = 0; delete d; return; }
    unsigned mask = W3DMD_POS | W3DMD_FACES;
    if (m->vertexColor) mask |= W3DMD_COL;
    if (m->normals)     mask |= W3DMD_NOR;
    if (m->uv)          mask |= W3DMD_UV;
    m->datosComp = (unsigned char)mask;
    m->datosRec  = nr;   // la ref que dejo RegistrarExterno (refTotal=1) es de esta malla
}
