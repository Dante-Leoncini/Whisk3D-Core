// ============================================================================
//  Whisk3DCore (engine) — MallaRecurso: la malla 3D como recurso compartido.
//  Ver el contrato en MallaRecurso.h. La contabilidad (id -> descriptor,
//  refcount, liberacion al llegar a 0) es del almacen de recursos
//  (io/W3dRecursos.h, tipo W3DREC_MALLA, el mismo de MallaDatos); aca vive el
//  registro del proyecto, la carga de la entrada .w3db, el vinculo con cada
//  Mesh usuario y los VBO compartidos.
//  C++03. Motor generico: aca no hay nombres de ningun juego.
// ============================================================================
#include "MallaRecurso.h"
#include "MallaFlujos.h"   // los flujos de indices derivados (se sueltan con el recurso)
#include "io/W3dRecursos.h"
#include "io/W3dMalla.h"        // W3dMallaInfo
#include "w3dFilesystem.h"      // la entrada del contenedor montado
#include "w3dGraphics.h"        // VBO compartidos
#include "W3dNombres.h"         // LA regla de nombres unicos (Arbol -> Arbol.001)
#include "animation/Animation.h"        // CurrentFrame / ActiveAnimKind: el cuadro de la pose de la sesion
#include "animation/VertexAnimation.h"  // re-posar a los usuarios animados tras re-apuntarlos
#include "w3dlog.h"
#include <string.h>

namespace gfx = w3dEngine;

const char* const W3D_EDICION_DEL_RECURSO = "@recurso";
Material* (*W3dMallaMaterialPorNombreHook)(const std::string& nombre) = 0;
bool (*W3dMallaTextoABinHook)(const unsigned char* datos, size_t n, const std::string& nombre, std::string& out) = 0;

// el registro del proyecto: dueno de los MallaRecurso (viven hasta cerrar el proyecto)
static std::vector<MallaRecurso*> gRegistro;
// las mallas de las LIBRERIAS externas (MallaRecurso::libreria): aparte del registro del proyecto (no se listan,
// no se guardan ni se purgan), vivas hasta que su libreria se cierra
static std::vector<MallaRecurso*> gLibMallas;
// sube cada vez que un recurso sale de gRegistro o de gLibMallas (ver W3dMallaRecursoVivo)
static unsigned gMallasGen = 1;
// recursos que al cerrar el proyecto todavia tenian usuarios (no deberia pasar): se dejan
// vivos para que ningun puntero quede colgando
static std::vector<MallaRecurso*> gRetenidos;
static int gSerialProx = 1;
// las mallas vinculadas que las puertas al render marcaron como EDITADAS (ver el .h)
static std::vector<Mesh*> gEditadas;

// entero -> texto sin sprintf de largo variable (C++03 / RVCT)
static std::string EnteroTexto(int v) {
    char b[16]; int i = 15; b[i] = '\0';
    const bool neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    do { b[--i] = (char)('0' + (u % 10u)); u /= 10u; } while (u && i > 1);
    if (neg && i > 0) b[--i] = '-';
    return std::string(b + i);
}

MallaRecurso::MallaRecurso()
    : modificado(false), bytesDeTexto(false), serial(gSerialProx++), refsUndo(0), conservar(false), borrado(false),
      version(1), rec(0),
      suave(false), primTipo(-1), primTam(2.0f), primTam2(0.0f), primAlto(0.0f), primVerts(8), primVerts2(8),
      extrasEnRecurso(false), firmaFormas(0), firmaUVRest(0),
      grupoActivo(-1), uvGrupoActivo(-1), radio(0.0f), aabbOk(false), vertsAgrupados(0), statsNoEditable(false),
      flujos(0), refsRutina(0), vboPos(0), vboNor(0), vboCol(0), vboUV(0), vboIdx(0), vboVer(0), topoEstado(0) {
    esRecurso = true;
    for (int k = 0; k < 3; k++) { aabbMin[k] = aabbMax[k] = centro[k] = 0.0f; }
}

// los VBO del recurso (el contexto GL tiene que estar vivo, igual que en ~Mesh)
static void BorrarVBO(MallaRecurso* r) {
    gfx::DeleteBuffer(r->vboPos); gfx::DeleteBuffer(r->vboNor); gfx::DeleteBuffer(r->vboCol);
    gfx::DeleteBuffer(r->vboUV);  gfx::DeleteBuffer(r->vboIdx);
    r->vboPos = r->vboNor = r->vboCol = r->vboUV = r->vboIdx = 0;
    r->vboVer = 0;
}

MallaRecurso::~MallaRecurso() {
    W3dMallaFlujosBorrar(this);
    BorrarVBO(this);
    // los arrays los libera ~MallaDatos
}

std::string MallaRecurso::IdAlmacen() const {
    return entrada.empty() ? ("*" + EnteroTexto(serial)) : entrada;
}

// ============================================================================
//  REGISTRO
// ============================================================================
const std::vector<MallaRecurso*>& W3dMallasRegistro() { return gRegistro; }

MallaRecurso* W3dMallaRecursoPorNombre(const std::string& nombre) {
    for (size_t i = 0; i < gRegistro.size(); i++) if (gRegistro[i]->nombre == nombre) return gRegistro[i];
    // (las de las librerias: su nombre lleva el prefijo de la libreria, "Personajes/Cuerpo")
    for (size_t i = 0; i < gLibMallas.size(); i++) if (gLibMallas[i]->nombre == nombre) return gLibMallas[i];
    return 0;
}
MallaRecurso* W3dMallaRecursoPorEntrada(const std::string& entrada) {
    if (entrada.empty()) return 0;
    for (size_t i = 0; i < gRegistro.size(); i++) if (gRegistro[i]->entrada == entrada) return gRegistro[i];
    for (size_t i = 0; i < gLibMallas.size(); i++) if (gLibMallas[i]->entrada == entrada) return gLibMallas[i];
    return 0;
}
int W3dMallaRecursoIndice(const MallaRecurso* r) {
    for (size_t i = 0; i < gRegistro.size(); i++) if (gRegistro[i] == r) return (int)i;
    return -1;
}
MallaRecurso* W3dMallaRecursoPorSerial(int serial) {
    for (size_t i = 0; i < gRegistro.size(); i++) if (gRegistro[i]->serial == serial) return gRegistro[i];
    return 0;
}

struct NombreLibreCtx { const MallaRecurso* excepto; };
static bool NombreMallaExiste(const std::string& n, void* ctx) {
    const MallaRecurso* ex = ctx ? ((NombreLibreCtx*)ctx)->excepto : 0;
    for (size_t i = 0; i < gRegistro.size(); i++)
        if (gRegistro[i] != ex && gRegistro[i]->nombre == n) return true;
    return false;
}
std::string W3dMallaNombreLibre(const std::string& base, const MallaRecurso* excepto) {
    NombreLibreCtx c; c.excepto = excepto;
    return W3dNombreUnico(base, "Malla", NombreMallaExiste, &c);
}

std::string W3dMallaCarpetaNormalizar(const std::string& c) {
    std::string out, tramo;
    for (size_t i = 0; i <= c.size(); i++) {
        const char ch = (i < c.size()) ? c[i] : '/';
        if (ch != '/' && ch != '\\') { tramo += ch; continue; }
        // fin de un tramo: sin espacios en los bordes; vacio = se descarta ("a//b" = "a/b")
        size_t a = 0, b = tramo.size();
        while (a < b && (tramo[a] == ' ' || tramo[a] == '\t')) a++;
        while (b > a && (tramo[b - 1] == ' ' || tramo[b - 1] == '\t')) b--;
        if (b > a) {
            if (!out.empty()) out += '/';
            out.append(tramo, a, b - a);
        }
        tramo.clear();
    }
    return out;
}

MallaRecurso* W3dMallaRecursoNuevo(const std::string& nombre, const std::string& entrada,
                                   const std::string& carpeta) {
    W3dMallasAsegurarOps();
    MallaRecurso* r = new MallaRecurso();
    r->nombre = W3dMallaNombreLibre(nombre);
    r->entrada = entrada;
    r->carpeta = carpeta;
    gRegistro.push_back(r);
    return r;
}

// borra un recurso que se cierra (el del proyecto o el de una libreria). false = todavia lo usan: queda vivo
// (retenido) con un aviso, para que ningun puntero quede colgando
static bool CerrarRecurso(MallaRecurso* r) {
    if (!r->usuarios.empty()) {
        w3dLogfW("[mallas] cierre: el recurso '%s' todavia tiene %d usuario(s); queda vivo",
                 r->nombre.c_str(), (int)r->usuarios.size());
        gRetenidos.push_back(r);
        return false;
    }
    if (r->rec) {
        if (!W3dRecursoOlvidar(W3DREC_MALLA, r->rec->id)) {
            w3dLogfW("[mallas] cierre: el recurso '%s' todavia tiene referencias; queda vivo", r->nombre.c_str());
            gRetenidos.push_back(r);
            return false;
        }
        r->rec = 0;
    }
    delete r;
    return true;
}

MallaRecurso* W3dMallaRecursoNuevoLib(const std::string& nombre, const std::string& entrada,
                                      const std::string& libreria) {
    W3dMallasAsegurarOps();
    for (size_t i = 0; i < gLibMallas.size(); i++) if (gLibMallas[i]->nombre == nombre) return gLibMallas[i];
    MallaRecurso* r = new MallaRecurso();
    r->nombre = nombre;          // (con su prefijo: no compite con los nombres del proyecto)
    r->entrada = entrada;
    r->libreria = libreria;
    gLibMallas.push_back(r);
    return r;
}
void W3dMallasLibLimpiar(const std::string& libreria) {
    for (size_t i = gLibMallas.size(); i-- > 0; ) {
        MallaRecurso* r = gLibMallas[i];
        if (!libreria.empty() && r->libreria != libreria) continue;
        gLibMallas.erase(gLibMallas.begin() + (long)i);
        gMallasGen++;
        CerrarRecurso(r);
    }
}
int W3dMallasLibCantidad() { return (int)gLibMallas.size(); }
const std::vector<MallaRecurso*>& W3dMallasLib() { return gLibMallas; }

void W3dMallasRegistroLimpiar() {
    // las de las LIBRERIAS se cierran con el proyecto (el arbol que las usaba ya no existe)
    W3dMallasLibLimpiar(std::string());
    for (size_t i = 0; i < gRegistro.size(); i++) {
        MallaRecurso* r = gRegistro[i];
        if (!r->usuarios.empty()) {
            // no deberia pasar: el arbol ya se destruyo. Borrarlo dejaria punteros colgando.
            w3dLogfW("[mallas] cierre: el recurso '%s' todavia tiene %d usuario(s); queda vivo",
                     r->nombre.c_str(), (int)r->usuarios.size());
            gRetenidos.push_back(r);
            continue;
        }
        if (r->rec) {
            // cargado sin usuarios: su descriptor se olvida (los arrays se van con el recurso).
            // Si alguien todavia tiene una referencia (no deberia: las transitorias ya se
            // purgaron), el descriptor apunta aca y el recurso no se puede borrar.
            // (Por la clave del DESCRIPTOR: es la que vale en el almacen.)
            if (!W3dRecursoOlvidar(W3DREC_MALLA, r->rec->id)) {
                w3dLogfW("[mallas] cierre: el recurso '%s' todavia tiene referencias; queda vivo",
                         r->nombre.c_str());
                gRetenidos.push_back(r);
                continue;
            }
            r->rec = 0;
        }
        delete r;
    }
    gRegistro.clear();
    gEditadas.clear();
    gMallasGen++;
}

void W3dMallaRecursoUndoRetener(MallaRecurso* r) { if (r) r->refsUndo++; }
void W3dMallaRecursoUndoSoltar(MallaRecurso* r)  { if (r && r->refsUndo > 0) r->refsUndo--; }

int W3dMallasPurgarSinUso() {
    int n = 0;
    for (size_t i = 0; i < gRegistro.size(); ) {
        MallaRecurso* r = gRegistro[i];
        // usado (tambien por una malla detachada que un undo puede devolver) o nombrado por un paso de
        // undo: se queda. Del archivo (huerfano del proyecto) o un huerfano PEDIDO: se queda, salvo que
        // el usuario lo haya BORRADO (ya no es del proyecto: se va apenas nadie lo nombra)
        if (!r->usuarios.empty() || r->refsUndo > 0 || r->refsRutina > 0) { i++; continue; }
        if (!r->borrado && (!r->entrada.empty() || r->conservar)) { i++; continue; }
        if (r->rec) {
            // cargado sin usuarios: solo si nadie mas lo referencia (una ListaCarga no nombra
            // recursos sin entrada, pero el almacen es quien sabe)
            if (!W3dRecursoOlvidar(W3DREC_MALLA, r->rec->id)) { i++; continue; }
            r->rec = 0;
        }
        gRegistro.erase(gRegistro.begin() + (long)i);
        gMallasGen++;
        delete r;
        n++;
    }
    return n;
}

bool W3dMallaRecursoBorrar(MallaRecurso* r) {
    const int i = W3dMallaRecursoIndice(r);
    if (i < 0) return false;
    // usado (tambien por una malla detachada que un undo puede devolver) o nombrado por el undo
    if (!r->usuarios.empty() || r->refsUndo > 0) return false;
    if (r->rec) {
        // arrays cargados sin usuarios: solo si el almacen no tiene otra referencia (una
        // ListaCarga "malla:" que la pidio); el descriptor se olvida con la clave que tiene
        if (!W3dRecursoOlvidar(W3DREC_MALLA, r->rec->id)) return false;
        r->rec = 0;
    }
    gRegistro.erase(gRegistro.begin() + (long)i);
    gMallasGen++;
    delete r;
    return true;
}

// ============================================================================
//  DATOS
// ============================================================================
bool W3dMallaRecursoLeerBytes(const MallaRecurso* r, std::string& out) {
    out.clear();
    if (!r) return false;
    if (!r->bytes.empty()) { out = r->bytes; return true; }
    if (r->entrada.empty()) return false;
    std::vector<unsigned char> d;
    if (!w3dFileSystem::ReadFileBytes(r->entrada, d) || d.empty()) return false;
    if (!W3dMallaBinEs(&d[0], d.size())) {
        // UNA MALLA DEL REGISTRO GUARDADA EN TEXTO (.w3dm, "formatoMallas": "texto"): el recurso
        // vive en binario, asi que el texto se convierte UNA vez (lo hace el lector del proyecto)
        // y queda como cache en memoria hasta que se descarguen los arrays
        if (!W3dMallaTextoABinHook || !W3dMallaTextoABinHook(&d[0], d.size(), r->nombre, out) || out.empty()) {
            out.clear();
            return false;
        }
        MallaRecurso* rw = const_cast<MallaRecurso*>(r);
        rw->bytes = out;
        rw->bytesDeTexto = true;
        return true;
    }
    out.assign((const char*)&d[0], d.size());
    return true;
}

// resuelve el material de cada parte por su NOMBRE (el del archivo en la carga del proyecto). En una malla de
// LIBRERIA los nombres de sus partes son los de SU archivo: sus materiales viven con el prefijo de la libreria
// ("Personajes/Piel"), en su propio espacio de nombres
static Material* MaterialDeNombre(const MallaRecurso* r, const std::string& n0) {
    if (n0.empty()) return 0;
    const std::string n = (r && !r->libreria.empty()) ? r->libreria + "/" + n0 : n0;
    if (W3dMallaMaterialPorNombreHook) return W3dMallaMaterialPorNombreHook(n);
    return BuscarMaterialPorNombre(n);
}

// ============================================================================
//  LOS EXTRAS (ver el .h): la firma, soltarlos y conseguirlos de nuevo
// ============================================================================
// la firma de lo que el recurso tiene AHORA en sus listas (recien llenadas de los bytes)
static void FirmarExtras(MallaRecurso* r) {
    r->firmaGrupos.clear(); r->firmaUVGrupos.clear();
    for (size_t g = 0; g < r->grupos.size(); g++) {
        MallaRecurso::FirmaGrupo f; f.nombre = r->grupos[g].nombre; f.n = r->grupos[g].idx.size();
        r->firmaGrupos.push_back(f);
    }
    for (size_t g = 0; g < r->uvGrupos.size(); g++) {
        MallaRecurso::FirmaGrupo f; f.nombre = r->uvGrupos[g].nombre; f.n = r->uvGrupos[g].idx.size();
        r->firmaUVGrupos.push_back(f);
    }
    r->firmaFormas = r->formas.size();
    r->firmaUVRest = r->uvRest.size();
    r->extrasEnRecurso = true;
}
// el recurso ya no guarda los extras (un usuario limpio los tiene): se libera la memoria
static void SoltarExtras(MallaRecurso* r) {
    if (!r->extrasEnRecurso) return;
    std::vector<int>().swap(r->ctrl);
    std::vector<W3dMallaBinGrupo>().swap(r->grupos);
    std::vector<W3dMallaBinGrupo>().swap(r->uvGrupos);
    std::vector<W3dMallaBinForma>().swap(r->formas);
    std::vector<float>().swap(r->uvRest);
    r->extrasEnRecurso = false;
}
// la malla necesita la identidad de los puntos? (el skinning con grupos o formas la lee)
static bool ConRig(const MallaRecurso* r) { return !r->firmaGrupos.empty() || r->firmaFormas > 0; }

// ============================================================================
//  LA TOPOLOGIA COMPARTIDA (ver el .h)
// ============================================================================
static void TirarTopo(MallaRecurso* r) {
    std::vector<int>().swap(r->aristas);
    std::vector<GLfloat>().swap(r->bordes);
    std::vector<MeshFace>().swap(r->caras);
    r->topoEstado = 0;
}
static bool AsegurarTopo(MallaRecurso* r, bool conCaras) {
    if (r->topoEstado < 0) return false;
    if (r->topoEstado >= (conCaras ? 2 : 1)) return true;
    std::string b;
    std::vector<int> ar;
    std::vector<MeshFace> cs;
    if (!W3dMallaRecursoLeerBytes(r, b) || b.empty() ||
        !W3dMallaBinLeerTopologia((const unsigned char*)b.data(), b.size(), ar, conCaras ? &cs : 0)) {
        r->topoEstado = -1;   // una sola vez: no se reintenta en cada frame
        return false;
    }
    // indices fuera de los render-verts del recurso = no calza (no se dibuja nada raro)
    for (size_t i = 0; i < ar.size(); i++) if (ar[i] < 0 || ar[i] >= r->vertexSize) { r->topoEstado = -1; return false; }
    r->aristas.swap(ar);
    if (conCaras) r->caras.swap(cs);
    std::vector<GLfloat>().swap(r->bordes);
    r->topoEstado = conCaras ? 2 : 1;
    return true;
}
// la malla se puede MIRAR con la topologia del recurso? (su edicion sigue ahi y mide lo mismo)
static bool TopoDelRecurso(const Mesh* m) {
    return m && m->malla && m->malla->rec && !m->noEditable &&
           m->edicionPendiente == W3D_EDICION_DEL_RECURSO && m->vertexSize == m->malla->vertexSize;
}
const std::vector<int>* W3dMallaAristasVista(const Mesh* m) {
    if (!m) return 0;
    if (!m->edges.empty()) return &m->edges;
    if (!TopoDelRecurso(m) || !AsegurarTopo(m->malla, false) || m->malla->aristas.empty()) return 0;
    return &m->malla->aristas;
}
const std::vector<MeshFace>* W3dMallaCarasVista(const Mesh* m) {
    if (!m) return 0;
    if (!m->faces3d.empty()) return &m->faces3d;
    if (!TopoDelRecurso(m) || !AsegurarTopo(m->malla, true) || m->malla->caras.empty()) return 0;
    return &m->malla->caras;
}
const std::vector<GLfloat>* W3dMallaBordesVista(const Mesh* m) {
    if (!m) return 0;
    if (!m->bordesBuf.empty()) return &m->bordesBuf;
    if (!TopoDelRecurso(m) || !(m->datosComp & W3DMD_POS) || m->vertex != m->malla->vertex) return 0;
    MallaRecurso* r = m->malla;
    if (!AsegurarTopo(r, false) || r->aristas.empty() || !r->vertex) return 0;
    if (r->bordes.empty()) {
        // lineal desde las aristas, como ArmarBordesBuf del lector (una vez para todos los usuarios)
        r->bordes.reserve(r->aristas.size() * 3);
        for (size_t e = 0; e + 1 < r->aristas.size(); e += 2) {
            const int a = r->aristas[e], b = r->aristas[e + 1];
            r->bordes.push_back(r->vertex[a * 3]); r->bordes.push_back(r->vertex[a * 3 + 1]); r->bordes.push_back(r->vertex[a * 3 + 2]);
            r->bordes.push_back(r->vertex[b * 3]); r->bordes.push_back(r->vertex[b * 3 + 1]); r->bordes.push_back(r->vertex[b * 3 + 2]);
        }
    }
    return &r->bordes;
}

// llena los arrays y los extras del recurso desde un .w3db. Los arrays NUEVOS se alocan aca:
// el que llama ya aparto los viejos (los libera despues de re-apuntar a los usuarios).
static bool LlenarDesdeBytes(MallaRecurso* r, const unsigned char* d, size_t n) {
    W3dMallaRender R;
    W3dMallaInfo info;
    W3dAjenos sello;
    if (!W3dMallaBinLeerRender(d, n, R, &info, &sello)) {
        for (size_t k = 0; k < info.avisos.size(); k++)
            w3dLogfE("[mallas] '%s': %s", r->nombre.c_str(), info.avisos[k].c_str());
        return false;
    }
    const int nRV = R.nRV;
    r->vertexSize = nRV;
    r->facesSize = (int)R.idx.size();
    r->vertex = 0; r->normals = 0; r->uv = 0; r->vertexColor = 0; r->faces = 0;
    if (nRV > 0) {
        r->vertex      = new GLfloat[(size_t)nRV * 3];
        r->normals     = new GLbyte[(size_t)nRV * 3];
        r->uv          = new GLfloat[(size_t)nRV * 2];
        r->vertexColor = new GLubyte[(size_t)nRV * 4];
        memcpy(r->vertex, &R.pos[0], (size_t)nRV * 3 * sizeof(GLfloat));
        memcpy(r->normals, &R.nrm[0], (size_t)nRV * 3);
        memcpy(r->uv, &R.uv[0], (size_t)nRV * 2 * sizeof(GLfloat));
        memcpy(r->vertexColor, &R.col[0], (size_t)nRV * 4);
    }
    r->faces = new MeshIndex[r->facesSize > 0 ? r->facesSize : 1];
    if (r->facesSize > 0) memcpy(r->faces, &R.idx[0], (size_t)r->facesSize * sizeof(MeshIndex));
    r->uvExtra = R.uvExtra;
    r->suave = R.suave;
    r->primTipo = R.primTipo; r->primTam = R.primTam; r->primTam2 = R.primTam2; r->primAlto = R.primAlto;
    r->primVerts = R.primVerts; r->primVerts2 = R.primVerts2;
    r->partes.swap(R.partes);
    // el material de cada parte: se resuelve UNA vez (la primera carga). Recargar el mismo
    // recurso (se libero y se volvio a pedir) conserva los punteros: el nombre del archivo pudo
    // quedar viejo si el material se renombro en la sesion.
    if (r->materiales.size() != r->partes.size()) {
        r->materiales.assign(r->partes.size(), (Material*)0);
        for (size_t k = 0; k < r->partes.size(); k++) r->materiales[k] = MaterialDeNombre(r, r->partes[k].material);
    }
    r->ctrl.swap(R.ctrl);
    r->grupos.swap(R.grupos);
    r->uvGrupos.swap(R.uvGrupos);
    r->formas.swap(R.formas);
    r->uvRest.swap(R.uvRest);
    FirmarExtras(r);
    r->grupoActivo = R.grupoActivo; r->uvGrupoActivo = R.uvGrupoActivo;
    for (int k = 0; k < 3; k++) { r->aabbMin[k] = R.aabbMin[k]; r->aabbMax[k] = R.aabbMax[k]; r->centro[k] = R.centro[k]; }
    r->radio = R.radio; r->aabbOk = R.aabbOk; r->vertsAgrupados = R.vertsAgrupados;
    r->statsNoEditable = R.statsNoEditable;
    r->sello = sello;
    return true;
}

static void LiberarArrays(MallaRecurso* r) {
    delete[] r->vertex; delete[] r->normals; delete[] r->uv; delete[] r->vertexColor; delete[] r->faces;
    r->vertex = 0; r->normals = 0; r->uv = 0; r->vertexColor = 0; r->faces = 0;
    r->uvExtra.clear();
    r->vertexSize = 0; r->facesSize = 0;
}

void W3dMallaRecursoPonerBytes(MallaRecurso* r, const std::string& bytes) {
    if (!r) return;
    r->bytes = bytes;
    r->modificado = true;
    r->bytesDeTexto = false;
    r->version++;
    g_w3dRutinasGen++;
    TirarTopo(r);
    // cargado sin usuarios (una referencia de ListaCarga): los arrays se rehacen de los bytes nuevos
    if (r->rec && r->usuarios.empty() && !bytes.empty()) {
        LiberarArrays(r);
        BorrarVBO(r);
        if (LlenarDesdeBytes(r, (const unsigned char*)bytes.data(), bytes.size())) r->rec->bytes = r->Bytes();
    }
}

bool W3dMallaRecursoMateriales(MallaRecurso* r, std::vector<std::string>* enArchivo) {
    if (!r) return false;
    // CARGADO: las partes (con el nombre de material de los bytes) y sus materiales ya estan
    if (!r->partes.empty() && r->materiales.size() == r->partes.size()) {
        if (enArchivo) {
            enArchivo->clear();
            for (size_t k = 0; k < r->partes.size(); k++) enArchivo->push_back(r->partes[k].material);
        }
        return true;
    }
    // SIN CARGAR (un huerfano que nadie pidio): los nombres de su PART, sin leer la geometria.
    // Se resuelven UNA vez (como en la primera carga, por el nombre del archivo) y quedan en el
    // recurso: LlenarDesdeBytes conserva los punteros si mide lo mismo.
    std::string b;
    std::vector<std::string> nombres;
    if (!W3dMallaRecursoLeerBytes(r, b) || !W3dMallaBinMateriales(b, nombres)) return false;
    if (r->materiales.size() != nombres.size()) {
        r->materiales.assign(nombres.size(), (Material*)0);
        for (size_t k = 0; k < nombres.size(); k++) r->materiales[k] = MaterialDeNombre(r, nombres[k]);
    }
    if (enArchivo) enArchivo->swap(nombres);
    return true;
}

// ============================================================================
//  EL TIPO EN EL ALMACEN: Cargar lee la entrada (o los bytes en memoria);
//  Liberar DESCARGA los arrays (el recurso sigue en el registro).
// ============================================================================
static MallaRecurso* PorIdAlmacen(const std::string& id) {
    for (size_t i = 0; i < gRegistro.size(); i++) if (gRegistro[i]->IdAlmacen() == id) return gRegistro[i];
    for (size_t i = 0; i < gLibMallas.size(); i++) if (gLibMallas[i]->IdAlmacen() == id) return gLibMallas[i];
    return 0;
}

static void* OpCargarMalla(const std::string& id, long* bytes) {
    MallaRecurso* r = PorIdAlmacen(id);
    if (!r) return 0;   // solo se cargan las del REGISTRO del proyecto (o las de una libreria)
    // YA CARGADO bajo OTRO descriptor (su clave quedo vieja): recargarlo liberaria los arrays que
    // sus usuarios estan apuntando (uso de memoria liberada). Un recurso tiene UN descriptor.
    if (r->rec && r->rec->id != id) {
        w3dLogfE("[mallas] la malla '%s' ya esta cargada como '%s': no la vuelvo a cargar como '%s'",
                 r->nombre.c_str(), r->rec->id.c_str(), id.c_str());
        return 0;
    }
    std::string b;
    if (!W3dMallaRecursoLeerBytes(r, b)) {
        w3dLogfE("[mallas] no pude leer la malla '%s' (%s)", r->nombre.c_str(), id.c_str());
        return 0;
    }
    LiberarArrays(r);
    BorrarVBO(r);
    TirarTopo(r);
    if (!LlenarDesdeBytes(r, (const unsigned char*)b.data(), b.size())) {
        w3dLogfE("[mallas] '%s' (%s) no es un .w3db que este Whisk3D pueda leer", r->nombre.c_str(), id.c_str());
        return 0;
    }
    if (bytes) *bytes = r->Bytes();
    // el descriptor ya existe (el almacen lo crea antes de llamar al op): el recurso lo anota,
    // lo haya pedido un objeto (W3dMallaVincular) o una ListaCarga "malla:" (la bomba ASYNC)
    r->rec = W3dRecursoBuscar(W3DREC_MALLA, id);
    return (void*)(MallaDatos*)r;
}

static bool OpLiberarMalla(void* dato) {
    MallaDatos* d = (MallaDatos*)dato;
    if (!d) return true;
    if (d->esRecurso) {
        // el recurso NO se borra (es del registro): se descargan sus arrays y sus VBO
        MallaRecurso* r = static_cast<MallaRecurso*>(d);
        LiberarArrays(r);
        BorrarVBO(r);
        TirarTopo(r);
        SoltarExtras(r);
        // la conversion de una entrada de texto es cache: se vuelve a hacer si se recarga
        if (r->bytesDeTexto) { std::string().swap(r->bytes); r->bytesDeTexto = false; }
        r->rec = 0;
        return true;
    }
    delete d;   // la geometria compartida de un .obj (MallaDatos.h)
    return true;
}

// el gancho con el que Mesh suelta una MallaDatos legacy (W3dMallaDatosSoltarHook, Mesh.cpp)
extern void (*W3dMallaDatosSoltarHook)(void*);
static void SoltarRecMalla(void* rec) {
    W3dRecursoSoltar((W3dRecurso*)rec, W3DREC_PERMANENTE);
}

void W3dMallasAsegurarOps() {
    static bool listo = false;
    if (listo) return;
    W3dRecursoOps ops;
    ops.Cargar = OpCargarMalla;
    ops.Liberar = OpLiberarMalla;
    W3dRecursosRegistrarTipo(W3DREC_MALLA, ops);
    W3dMallaDatosSoltarHook = SoltarRecMalla;
    listo = true;
}

// ============================================================================
//  USUARIOS
// ============================================================================
static void QuitarUsuario(MallaRecurso* r, Mesh* m) {
    for (size_t i = 0; i < r->usuarios.size(); i++)
        if (r->usuarios[i] == m) { r->usuarios.erase(r->usuarios.begin() + (long)i); return; }
}
static void QuitarEditada(Mesh* m) {
    for (size_t i = 0; i < gEditadas.size(); i++)
        if (gEditadas[i] == m) { gEditadas.erase(gEditadas.begin() + (long)i); return; }
}

// la edicion materializada de la malla (caras, capas, marcas, aristas): se tira
static void LimpiarEdicion(Mesh* m) {
    m->InvalidarEdit();
    m->faces3d.clear();
    m->looseEdges.clear(); m->looseVerts.clear();
    m->cornerNormal.clear();
    for (size_t i = 0; i < m->uvMaps.size(); i++)      delete m->uvMaps[i];
    for (size_t i = 0; i < m->colorLayers.size(); i++) delete m->colorLayers[i];
    m->uvMaps.clear();      m->uvMapActivo = -1;
    m->colorLayers.clear(); m->colorActivo = -1;
    m->sharpEdges.clear();  m->seamEdges.clear();
    m->posRep.clear();
    m->edges.clear();
    m->bordesBuf.clear();
    m->uvSelVert.clear();
    m->normFaceBuf.clear(); m->normCustomBuf.clear(); m->normVertBuf.clear();
    m->w3dmAjenos.bloques.clear();
    m->w3dmAjenos.requiere.clear();
}

// un usuario que tiene los EXTRAS tal cual el recurso: limpio (nada suyo pudo cambiar: su
// edicion sigue pendiente y sus listas miden lo que dice la firma) y con lo que haga falta
static const Mesh* DonanteDeExtras(const MallaRecurso* r, const Mesh* excepto, bool conCtrl) {
    for (size_t i = 0; i < r->usuarios.size(); i++) {
        const Mesh* u = r->usuarios[i];
        if (!u || u == excepto || !W3dMallaLimpia(u)) continue;
        if (u->uv2dRest.size() != r->firmaUVRest) continue;          // un reposo 2D capturado en la sesion
        if (conCtrl && (int)u->vertCtrlPoint.size() != u->vertexSize) continue;
        return u;
    }
    return 0;
}
// los extras de unas listas (las del recurso o las releidas de los bytes) a la malla
static void ExtrasDeListas(Mesh* m, const std::vector<W3dMallaBinGrupo>& grupos,
                           const std::vector<W3dMallaBinGrupo>& uvGrupos, const std::vector<W3dMallaBinForma>& formas,
                           const std::vector<float>& uvRest, const std::vector<int>& ctrl, bool conCtrl) {
    for (size_t g = 0; g < grupos.size(); g++) {
        VertexGroup* vg = new VertexGroup(grupos[g].nombre);
        vg->verts = grupos[g].idx; vg->pesos = grupos[g].pesos;
        m->vertexGroups.push_back(vg);
    }
    for (size_t g = 0; g < uvGrupos.size(); g++) {
        UVGroup* ug = new UVGroup(uvGrupos[g].nombre);
        ug->verts = uvGrupos[g].idx; ug->pesos = uvGrupos[g].pesos;
        m->uvGroups.push_back(ug);
    }
    for (size_t k = 0; k < formas.size(); k++) {
        W3dShapeKey sk; sk.nombre = formas[k].nombre; sk.idx = formas[k].idx; sk.d = formas[k].d;
        m->shapeKeys.push_back(sk);
    }
    m->uv2dRest = uvRest;
    if (conCtrl) m->vertCtrlPoint = ctrl;
    else m->vertCtrlPoint.clear();
}
// releer los extras de los bytes del recurso (nadie en memoria los tiene tal cual)
static bool ReleerExtras(const MallaRecurso* r, W3dMallaRender& X) {
    std::string b;
    if (!W3dMallaRecursoLeerBytes(r, b) || b.empty()) return false;
    return W3dMallaBinLeerExtras((const unsigned char*)b.data(), b.size(), X) && X.nRV == r->vertexSize;
}
// grupos, formas, reposo 2D e identidad de los puntos: CHICOS y por instancia (el skinning, el
// weight paint y el editor los leen de la malla). Salen del recurso si todavia los guarda, de
// otro usuario limpio (la copia que ya esta en memoria) o, si no hay, de los bytes.
static void ExtrasAMalla(Mesh* m, MallaRecurso* r, bool conservarEdicion) {
    for (size_t i = 0; i < m->vertexGroups.size(); i++) delete m->vertexGroups[i];
    for (size_t i = 0; i < m->uvGroups.size(); i++)     delete m->uvGroups[i];
    m->vertexGroups.clear(); m->uvGroups.clear();
    m->shapeKeys.clear();
    // LA IDENTIDAD DE LOS PUNTOS: la necesita el skinning (vertex groups / shape keys) y la
    // edicion (posRep sale de aca). Sin grupos ni formas se difiere a la materializacion: mil
    // arboles sin rig no pagan un int por vertice cada uno.
    const bool conCtrl = ConRig(r) || conservarEdicion;
    const Mesh* d = r->extrasEnRecurso ? 0 : DonanteDeExtras(r, m, conCtrl);
    if (r->extrasEnRecurso) {
        ExtrasDeListas(m, r->grupos, r->uvGrupos, r->formas, r->uvRest, r->ctrl, conCtrl);
    } else if (d) {
        for (size_t g = 0; g < d->vertexGroups.size(); g++)
            m->vertexGroups.push_back(d->vertexGroups[g] ? new VertexGroup(*d->vertexGroups[g]) : new VertexGroup(""));
        for (size_t g = 0; g < d->uvGroups.size(); g++)
            m->uvGroups.push_back(d->uvGroups[g] ? new UVGroup(*d->uvGroups[g]) : new UVGroup(""));
        m->shapeKeys = d->shapeKeys;
        m->uv2dRest = d->uv2dRest;
        if (conCtrl) m->vertCtrlPoint = d->vertCtrlPoint;
        else m->vertCtrlPoint.clear();
    } else {
        W3dMallaRender X;
        if (ReleerExtras(r, X)) ExtrasDeListas(m, X.grupos, X.uvGrupos, X.formas, X.uvRest, X.ctrl, conCtrl);
        else {
            w3dLogfW("[mallas] no pude releer los grupos/formas de la malla '%s' para '%s'",
                     r->nombre.c_str(), m->name.c_str());
            m->uv2dRest.clear();
            m->vertCtrlPoint.clear();
        }
    }
    m->grupoActivo = r->grupoActivo; m->uvGrupoActivo = r->uvGrupoActivo;
    m->shapePesos.assign(m->shapeKeys.size(), 0.0f);
    // NO se reagrupa al cargar: seria gastar CPU del telefono en cada apertura. El agrupamiento de los puntos
    // de skinning viene HORNEADO en el .w3db (vertCtrlPoint compacto + grupos/formas por punto) cuando la malla
    // se guardo como NO-EDITABLE (ver W3dMallaBinEscribir). Una malla editable vieja carga sin agrupar (mas lenta,
    // pero editable); convertila a no-editable y volve a guardar para hornear el agrupamiento.
}
// la identidad de los puntos del RECURSO (canonica) para el AABB: la de la malla si la tiene,
// la del recurso si todavia la guarda, o releida
static const int* CtrlDelRecurso(const MallaRecurso* r, const Mesh* m, std::vector<int>& tmp) {
    if (r->vertexSize <= 0) return 0;
    if ((int)m->vertCtrlPoint.size() == r->vertexSize) return &m->vertCtrlPoint[0];
    if ((int)r->ctrl.size() == r->vertexSize) return &r->ctrl[0];
    W3dMallaRender X;
    if (!ReleerExtras(r, X) || (int)X.ctrl.size() != r->vertexSize) return 0;
    tmp.swap(X.ctrl);
    return &tmp[0];
}

// LA POSE DE LA SESION vuelve en el acto: re-apuntar una malla a los datos del recurso la deja
// EN REPOSO (los arrays compartidos son el modelo). Si estaba posada por una vertex anim o tiene
// flipbook / rig 2D, esa pose se vuelve a aplicar YA, en el mismo cuadro: sin esto, publicar la
// edicion de OTRO usuario (o un Ctrl+Z del vinculo) la dejaba en reposo hasta el proximo cuadro
// de playback, y con el playback en pausa, para siempre.
static void RePosar(Mesh* u, bool estabaPosada) {
    if (estabaPosada && !u->animations.empty()) {
        // el mismo cuadro que la poso: el del playhead si es la anim del timeline (kind 3), si no
        // el del controlador de reproduccion de la malla
        VertexAnimationActive* va = FindTargetAnim(u);
        const int ai = va ? va->currentAnim : -1;
        if (ai >= 0 && ai < (int)u->animations.size() && u->animations[(size_t)ai] &&
            !u->animations[(size_t)ai]->frames.empty()) {
            const float cuadro = (ActiveAnimKind == 3 && ActiveAnimMesh == u) ? (float)CurrentFrame : va->playFrame;
            EvalVertexAnim(*u->animations[(size_t)ai], u, cuadro);
        }
    }
    if (u->TieneArm2D()) u->Armature2DAplicar();   // uv = f(reposo 2D, pose)
    if (u->flipbook) u->TickUVAnimTira(0.0f);      // el cuadro actual del flipbook (sin avanzar)
}

// los datos del recurso -> la malla (los punteros de render APUNTAN ADENTRO del recurso)
static void AplicarAMalla(Mesh* m, MallaRecurso* r, bool conservarEdicion) {
    m->vertexSize = r->vertexSize;
    m->vertex = r->vertex; m->normals = r->normals; m->uv = r->uv; m->vertexColor = r->vertexColor;
    m->facesSize = r->facesSize; m->faces = r->faces;
    m->uvExtra = r->uvExtra;   // copia por usuario (no es un array compartido: ver Mesh.h)
    unsigned mask = 0;
    if (r->vertex)      mask |= W3DMD_POS;
    if (r->normals)     mask |= W3DMD_NOR;
    if (r->uv)          mask |= W3DMD_UV;
    if (r->vertexColor) mask |= W3DMD_COL;
    if (r->faces)       mask |= W3DMD_FACES;
    m->datosComp = (unsigned char)mask;
    m->posadaPorAnim = false;   // el recurso esta EN REPOSO (el playback la vuelve a posar)
    m->meshSmooth = r->suave;
    if (r->primTipo >= 0) {
        m->meshTipo = r->primTipo; m->meshSize = r->primTam; m->meshSize2 = r->primTam2;
        m->meshDepth = r->primAlto; m->meshVerts = r->primVerts; m->meshVerts2 = r->primVerts2;
    } else m->meshTipo = -1;
    // mesh parts: rango del index buffer compartido + el material (dato de la malla)
    m->materialsGroup.clear();
    for (size_t k = 0; k < r->partes.size(); k++) {
        MaterialGroup mg;
        mg.name = r->partes[k].nombre;
        mg.startDrawn = r->partes[k].inicio;
        mg.indicesDrawnCount = r->partes[k].cantidad;
        mg.material = k < r->materiales.size() ? r->materiales[k] : 0;
        m->materialsGroup.push_back(mg);
    }
    // grupos, formas, reposo 2D e identidad de los puntos (ver ExtrasAMalla)
    ExtrasAMalla(m, r, conservarEdicion);
    // AABB / centro / radio: horneados en el modo del guardado; el objeto puede pedir el otro
    if (m->noEditable == r->statsNoEditable) {
        m->aabbOk = r->aabbOk;
        m->aabbMin = Vector3(r->aabbMin[0], r->aabbMin[1], r->aabbMin[2]);
        m->aabbMax = Vector3(r->aabbMax[0], r->aabbMax[1], r->aabbMax[2]);
        m->centroGeom = Vector3(r->centro[0], r->centro[1], r->centro[2]);
        m->radioGeom = r->radio;
        m->vertsAgrupados = m->noEditable ? 0 : r->vertsAgrupados;
    } else {
        W3dGeomStats st;
        std::vector<int> tmp;
        const int* rep = m->noEditable ? 0 : CtrlDelRecurso(r, m, tmp);
        W3dCalcularGeomStats(r->vertex, r->vertexSize, rep, st);
        m->aabbOk = st.ok;
        if (st.ok) { m->aabbMin = st.aabbMin; m->aabbMax = st.aabbMax; m->centroGeom = st.centro; m->radioGeom = st.radio; }
        m->vertsAgrupados = m->noEditable ? 0 : st.agrupados;
    }
    // la preservacion del .w3db (sello + bloques que no se entienden + REQ)
    m->w3dmAjenos.nPuntos = r->sello.nPuntos;   m->w3dmAjenos.nEsquinas = r->sello.nEsquinas;
    m->w3dmAjenos.nCaras = r->sello.nCaras;     m->w3dmAjenos.nAristas = r->sello.nAristas;
    m->w3dmAjenos.nRender = r->sello.nRender;   m->w3dmAjenos.nTriangulos = r->sello.nTriangulos;
    m->w3dmAjenos.bloquesBin = r->sello.bloquesBin;
    m->w3dmAjenos.requiereBin = r->sello.requiereBin;
    m->w3dmAjenos.truncado = false;
    m->w3dmAjenos.noCargo = false;
    // LA EDICION: pendiente (sale del recurso cuando alguien la pida) o la que la malla ya tiene
    if (!conservarEdicion) {
        LimpiarEdicion(m);
        m->edicionPendiente = W3D_EDICION_DEL_RECURSO;
    } else {
        m->InvalidarEdit();   // la jaula de Edit Mode se rearma sobre los punteros nuevos
        m->edicionPendiente.clear();
    }
    // TODO lo derivado de la geometria vieja se rehace (VBO propios, skinning, reflejos...)
    m->skinGeomVersion += 2;
    m->bordesGeomVersion = m->skinGeomVersion;
    m->overlayLcache = -1.0f;
    m->vboGeomVer = 0xFFFFFFFFu;
    m->lastSkinFrame = -999999;
    m->skinFlatSig = 0;
    m->skinShapeSerial = 0xFFFFFFFFu;
    m->LiberarSkinCache();
    m->tangentsValid = false;
    m->chromeUVValid = false;
    m->genChromeValid = false;
    m->flipUvBase.clear(); m->flipAplicado = -1;
}

// un usuario LIMPIO ya tiene los extras: el recurso suelta los suyos (el proximo usuario los
// copia de ese o los relee de los bytes). Se llama con TODOS los usuarios al dia (nunca en
// medio de un re-apuntado: uno todavia con los datos viejos pareceria limpio).
static void SoltarExtrasSiSobran(MallaRecurso* r) {
    if (r->extrasEnRecurso && DonanteDeExtras(r, 0, ConRig(r))) SoltarExtras(r);
}

// UNA REFERENCIA PERMANENTE sobre el recurso, cargandolo si hace falta. NULL = no se pudo, y en
// ese caso NO queda ninguna referencia sumada (el que llama no tiene nada que soltar).
static W3dRecurso* AdquirirMalla(MallaRecurso* r) {
    W3dMallasAsegurarOps();
    // YA CARGADO: una referencia mas sobre SU descriptor, por puntero y no por clave (la clave
    // sigue a la entrada: buscarla podria dar OTRO descriptor, que lo volveria a cargar)
    if (r->rec && r->rec->estado == W3DREC_LISTO && r->rec->dato == (void*)(MallaDatos*)r) {
        W3dRecursoRetener(r->rec, W3DREC_PERMANENTE);
        return r->rec;
    }
    const std::string id = r->IdAlmacen();
    // un FALLO viejo de esa clave que nadie referencia no se recuerda (el proyecto pudo arreglarse)
    W3dRecurso* viejo = W3dRecursoBuscar(W3DREC_MALLA, id);
    if (viejo && viejo->estado == W3DREC_FALLO && viejo->refTotal <= 0) W3dRecursoOlvidar(W3DREC_MALLA, id);
    // BLOQUEANTE: un descriptor EN_VUELO (una ListaCarga async sin bombear) se carga ahora
    W3dRecurso* d = W3dRecursoAdquirir(W3DREC_MALLA, id, W3DREC_PERMANENTE, W3DREC_BLOQUEANTE, "malla");
    if (d && d->estado == W3DREC_LISTO && d->dato == (void*)(MallaDatos*)r) return d;
    // no se pudo: la referencia que el almacen haya sumado se devuelve (un FALLO no suma) y el
    // fallo no se recuerda si nadie mas lo referencia
    if (d && d->estado != W3DREC_FALLO) W3dRecursoSoltar(d, W3DREC_PERMANENTE);
    else if (d && d->refTotal <= 0) W3dRecursoOlvidar(W3DREC_MALLA, id);
    return 0;
}

bool W3dMallaRecursoRetener(MallaRecurso* r) {
    if (!r) return false;
    W3dRecurso* d = AdquirirMalla(r);
    if (!d) return false;
    r->rec = d;
    r->refsRutina++;
    return true;
}
void W3dMallaRecursoSoltar(MallaRecurso* r) {
    if (!r || r->refsRutina <= 0) return;
    r->refsRutina--;
    if (r->rec) W3dRecursoSoltar(r->rec, W3DREC_PERMANENTE);   // la ultima -> OpLiberarMalla descarga
}
// (con una cache como W3dObjetoVivo: la validacion de las rutinas pregunta por cada paso y recorrer los registros
// era lineal. Una entrada dice "r estaba registrado en la generacion g"; sacar cualquiera del registro la sube)
unsigned W3dMallasGen() { return gMallasGen; }
unsigned g_w3dRutinasGen = 1;
struct MallaVivaCache { const MallaRecurso* r; unsigned gen; };
static MallaVivaCache gMallaViva[64];
bool W3dMallaRecursoVivo(const MallaRecurso* r) {
    if (!r) return false;
    MallaVivaCache& c = gMallaViva[((unsigned)(size_t)r * 2654435761u) >> 26];
    if (c.r == r && c.gen == gMallasGen) return !r->borrado;
    bool esta = false;
    for (size_t i = 0; i < gRegistro.size() && !esta; i++) esta = gRegistro[i] == r;
    for (size_t i = 0; i < gLibMallas.size() && !esta; i++) esta = gLibMallas[i] == r;
    if (!esta) return false;
    c.r = r; c.gen = gMallasGen;
    return !r->borrado;
}

bool W3dMallaVincular(Mesh* m, MallaRecurso* r, bool conservarEdicion) {
    if (!m || !r) return false;
    // la referencia NUEVA antes de soltar la vieja (si es el mismo recurso, no se descarga en el medio)
    W3dRecurso* d = AdquirirMalla(r);
    if (!d) return false;
    r->rec = d;
    const bool posada = m->posadaPorAnim;   // la pose de la sesion vuelve al final (RePosar)
    // lo que la malla tenia: lo COMPARTIDO se suelta (queda NULL) y lo PROPIO se libera
    m->SoltarDatosCompartidos();
    delete[] m->vertex;      m->vertex = 0;
    delete[] m->normals;     m->normals = 0;
    delete[] m->uv;          m->uv = 0;
    delete[] m->vertexColor; m->vertexColor = 0;
    delete[] m->faces;       m->faces = 0;
    m->malla = r;
    r->usuarios.push_back(m);
    AplicarAMalla(m, r, conservarEdicion);
    if (posada || m->flipbook || m->TieneArm2D()) RePosar(m, posada);
    SoltarExtrasSiSobran(r);
    return true;
}

bool W3dMallaMudar(Mesh* m, MallaRecurso* r) {
    if (!m || !r) return false;
    if (m->malla == r) return true;
    W3dRecurso* d = AdquirirMalla(r);
    if (!d) return false;
    r->rec = d;
    m->DesinstanciarDatos(W3DMD_TODO, true);   // lo que compartia con el recurso viejo, propio
    const std::string pend = m->edicionPendiente;
    if (m->malla) W3dMallaDesvincular(m);
    m->edicionPendiente = pend;
    m->malla = r;
    r->usuarios.push_back(m);
    return true;
}

void W3dMallaSoltarEdicion(Mesh* m) {
    if (!m || !m->malla || !m->edicionPendiente.empty()) return;
    if (m->edit) return;   // la jaula de Edit Mode la esta usando
    const MallaRecurso* r = m->malla;
    unsigned mask = 0;
    if (r->vertex)      mask |= W3DMD_POS;
    if (r->normals)     mask |= W3DMD_NOR;
    if (r->uv)          mask |= W3DMD_UV;
    if (r->vertexColor) mask |= W3DMD_COL;
    if (r->faces)       mask |= W3DMD_FACES;
    if ((unsigned)m->datosComp != mask) return;   // algo propio: la edicion puede ser distinta
    LimpiarEdicion(m);
    if (!ConRig(r)) m->vertCtrlPoint.clear();
    m->edicionPendiente = W3D_EDICION_DEL_RECURSO;
}

static unsigned gDesvinculadas = 0;
unsigned W3dMallasDesvinculadas() { return gDesvinculadas; }
void W3dMallaDesvincular(Mesh* m) {
    if (!m || !m->malla) return;
    gDesvinculadas++;   // (una malla quedo sin recurso: el editor le da uno, ver W3dMallasDesvinculadas)
    MallaRecurso* r = m->malla;
    // las capas compartidas no son de la malla: NULL (ningun delete[] posterior las toca)
    if (m->datosComp & W3DMD_POS)   m->vertex = 0;
    if (m->datosComp & W3DMD_NOR)   m->normals = 0;
    if (m->datosComp & W3DMD_UV)    m->uv = 0;
    if (m->datosComp & W3DMD_COL)   m->vertexColor = 0;
    if (m->datosComp & W3DMD_FACES) m->faces = 0;
    m->datosComp = 0;
    QuitarUsuario(r, m);
    QuitarEditada(m);
    m->malla = 0;
    // sin recurso, la edicion pendiente ya no tiene de donde salir
    if (m->edicionPendiente == W3D_EDICION_DEL_RECURSO) m->edicionPendiente.clear();
    if (r->rec) W3dRecursoSoltar(r->rec, W3DREC_PERMANENTE);   // la ultima -> OpLiberarMalla descarga
}

static bool EnArbol(const Object* raiz, const MallaRecurso* r, int& n) {
    for (size_t i = 0; i < raiz->Childrens.size(); i++) {
        Object* o = raiz->Childrens[i];
        if (!o) continue;
        if (o->getType() == ObjectType::mesh && ((Mesh*)o)->malla == r) n++;
        EnArbol(o, r, n);
    }
    return true;
}
int W3dMallaRecursoUsuariosEnEscena(const MallaRecurso* r, const Object* raiz) {
    if (!r) return 0;
    int n = 0;
    if (raiz) { EnArbol(raiz, r, n); return n; }
    // sin raiz: TODAS las raices cargadas del proyecto (la activa y las escenas/prefabs que no se
    // estan mirando). Una malla que solo usa otra escena NO es huerfana ni de un solo usuario.
    std::vector<Object*> raices;
    W3dRaicesVivas(raices);
    for (size_t i = 0; i < raices.size(); i++) EnArbol(raices[i], r, n);
    return n;
}

bool W3dMallaLimpia(const Mesh* m) {
    if (!m || !m->malla) return false;
    const MallaRecurso* r = m->malla;
    // la edicion tiene que seguir en el recurso: una materializada se pudo tocar (marcas, capas)
    if (m->edicionPendiente != W3D_EDICION_DEL_RECURSO) return false;
    unsigned mask = 0;
    if (r->vertex)      mask |= W3DMD_POS;
    if (r->normals)     mask |= W3DMD_NOR;
    if (r->uv)          mask |= W3DMD_UV;
    if (r->vertexColor) mask |= W3DMD_COL;
    if (r->faces)       mask |= W3DMD_FACES;
    if (!r->rec || (unsigned)m->datosComp != mask) return false;
    // lo que se copia por instancia tambien tiene que seguir igual (un rename de mesh part o de
    // grupo en la tarjeta, un grupo nuevo, otra cantidad de formas)
    return W3dMallaExtrasComoRecurso(m);
}

bool W3dMallaExtrasComoRecurso(const Mesh* m) {
    if (!m || !m->malla) return false;
    const MallaRecurso* r = m->malla;
    if (m->materialsGroup.size() != r->partes.size()) return false;
    for (size_t k = 0; k < r->partes.size(); k++) {
        if (m->materialsGroup[k].name != r->partes[k].nombre) return false;
        if (k < r->materiales.size() && m->materialsGroup[k].material != r->materiales[k]) return false;
    }
    // (contra la FIRMA: el recurso pudo soltar los datos, ver ExtrasAMalla)
    if (m->vertexGroups.size() != r->firmaGrupos.size() || m->uvGroups.size() != r->firmaUVGrupos.size() ||
        m->shapeKeys.size() != r->firmaFormas) return false;
    for (size_t g = 0; g < r->firmaGrupos.size(); g++)
        if (!m->vertexGroups[g] || m->vertexGroups[g]->nombre != r->firmaGrupos[g].nombre ||
            m->vertexGroups[g]->verts.size() != r->firmaGrupos[g].n) return false;
    for (size_t g = 0; g < r->firmaUVGrupos.size(); g++)
        if (!m->uvGroups[g] || m->uvGroups[g]->nombre != r->firmaUVGrupos[g].nombre ||
            m->uvGroups[g]->verts.size() != r->firmaUVGrupos[g].n) return false;
    if (m->meshSmooth != r->suave) return false;
    return true;
}

bool W3dMallaRecursoAplicarBytes(MallaRecurso* r, const std::string& bytes, Mesh* excepto,
                                 const std::vector<Material*>* mats) {
    std::vector<Mesh*> ex;
    if (excepto) ex.push_back(excepto);
    return W3dMallaRecursoAplicarBytes(r, bytes, ex, mats);
}

static bool EsExcepto(const std::vector<Mesh*>& ex, const Mesh* u) {
    for (size_t i = 0; i < ex.size(); i++) if (ex[i] == u) return true;
    return false;
}

bool W3dMallaRecursoAplicarBytes(MallaRecurso* r, const std::string& bytes, const std::vector<Mesh*>& exceptos,
                                 const std::vector<Material*>* mats) {
    if (!r || bytes.empty()) return false;
    // los EXCEPTUADOS (el que publica, la malla en Edit Mode) se quedan con LO SUYO: una capa que
    // todavia compartieran apunta a los arrays que se van a liberar -> se copia antes (tiene el
    // mismo contenido que ven hoy)
    for (size_t i = 0; i < exceptos.size(); i++)
        if (exceptos[i] && exceptos[i]->malla == r) exceptos[i]->DesinstanciarDatos(W3DMD_TODO, true);
    // los arrays viejos se apartan: los usuarios los siguen apuntando hasta re-vincularse
    GLfloat* vVie = r->vertex; GLbyte* nVie = r->normals; GLfloat* uVie = r->uv;
    GLubyte* cVie = r->vertexColor; MeshIndex* fVie = r->faces;
    const int vsVie = r->vertexSize, fsVie = r->facesSize;
    if (!LlenarDesdeBytes(r, (const unsigned char*)bytes.data(), bytes.size())) {
        r->vertex = vVie; r->normals = nVie; r->uv = uVie; r->vertexColor = cVie; r->faces = fVie;
        r->vertexSize = vsVie; r->facesSize = fsVie;
        return false;
    }
    if (mats && mats->size() == r->partes.size()) r->materiales = *mats;
    r->bytes = bytes;
    r->modificado = true;
    r->bytesDeTexto = false;
    r->version++;
    g_w3dRutinasGen++;
    BorrarVBO(r);   // se vuelven a subir con los arrays nuevos (en el proximo dibujo)
    TirarTopo(r);   // las aristas/caras compartidas eran de la malla vieja
    // TODOS los usuarios (tambien los detachados por el undo) pasan a los arrays nuevos
    std::vector<Mesh*> us = r->usuarios;
    for (size_t i = 0; i < us.size(); i++) {
        Mesh* u = us[i];
        if (!u || EsExcepto(exceptos, u)) continue;
        const bool posada = u->posadaPorAnim;   // la pose de la sesion vuelve enseguida (RePosar)
        // lo PROPIO de la malla (una capa copiada por una vertex anim, el flipbook...) es del
        // contenido viejo: se libera; lo compartido apuntaba a los arrays viejos
        if (!(u->datosComp & W3DMD_POS))   delete[] u->vertex;
        if (!(u->datosComp & W3DMD_NOR))   delete[] u->normals;
        if (!(u->datosComp & W3DMD_UV))    delete[] u->uv;
        if (!(u->datosComp & W3DMD_COL))   delete[] u->vertexColor;
        if (!(u->datosComp & W3DMD_FACES)) delete[] u->faces;
        u->vertex = 0; u->normals = 0; u->uv = 0; u->vertexColor = 0; u->faces = 0;
        AplicarAMalla(u, r, false);
        if (posada || u->flipbook || u->TieneArm2D()) RePosar(u, posada);
    }
    SoltarExtrasSiSobran(r);
    delete[] vVie; delete[] nVie; delete[] uVie; delete[] cVie; delete[] fVie;
    if (r->rec) r->rec->bytes = r->Bytes();
    return true;
}

void W3dMallaRecursoCambiarMaterial(MallaRecurso* r, int k, Material* mat) {
    if (!r || k < 0) return;
    if ((size_t)k >= r->materiales.size()) {
        if ((size_t)k >= r->partes.size()) return;
        r->materiales.resize(r->partes.size(), (Material*)0);
    }
    r->materiales[(size_t)k] = mat;
    for (size_t i = 0; i < r->usuarios.size(); i++) {
        Mesh* u = r->usuarios[i];
        if (u && (size_t)k < u->materialsGroup.size()) u->materialsGroup[(size_t)k].material = mat;
    }
}

// ============================================================================
//  VBO COMPARTIDOS
// ============================================================================
bool W3dMallaRecursoAsegurarVBO(MallaRecurso* r) {
    if (!r || !gfx::VBOSoportado() || !r->vertex || r->vertexSize <= 0) return false;
    if (r->vboPos && r->vboVer == r->version) return true;
    const int nv = r->vertexSize;
    if (!r->vboPos) r->vboPos = gfx::GenBuffer();
    gfx::ArrayBufferData(r->vboPos, r->vertex, nv * 3 * (int)sizeof(GLfloat));
    if (r->normals)     { if (!r->vboNor) r->vboNor = gfx::GenBuffer(); gfx::ArrayBufferData(r->vboNor, r->normals, nv * 3 * (int)sizeof(GLbyte)); }
    if (r->vertexColor) { if (!r->vboCol) r->vboCol = gfx::GenBuffer(); gfx::ArrayBufferData(r->vboCol, r->vertexColor, nv * 4 * (int)sizeof(GLubyte)); }
    if (r->uv)          { if (!r->vboUV)  r->vboUV  = gfx::GenBuffer(); gfx::ArrayBufferData(r->vboUV,  r->uv, nv * 2 * (int)sizeof(GLfloat)); }
    if (r->faces && r->facesSize > 0) {
        if (!r->vboIdx) r->vboIdx = gfx::GenBuffer();
        gfx::IndexBufferData(r->vboIdx, r->faces, r->facesSize * (int)sizeof(MeshIndex));
    }
    r->vboVer = r->version;
    return true;
}

// ============================================================================
//  MALLAS EDITADAS
// ============================================================================
void W3dMallaMarcarEditada(Mesh* m) {
    if (!m || !m->malla) return;
    for (size_t i = 0; i < gEditadas.size(); i++) if (gEditadas[i] == m) return;
    gEditadas.push_back(m);
}
void W3dMallasTomarEditadas(std::vector<Mesh*>& out) {
    out.clear();
    out.swap(gEditadas);
}

// ============================================================================
//  MEDICION
// ============================================================================
int W3dMallasRecursosCargados() {
    int n = 0;
    for (size_t i = 0; i < gRegistro.size(); i++) if (gRegistro[i]->rec) n++;
    return n;
}
int W3dMallasVBOCompartidos() {
    int n = 0;
    for (size_t i = 0; i < gRegistro.size(); i++) if (gRegistro[i]->vboPos) n++;
    return n;
}
long W3dMallasVBOBytes() {
    long b = 0;
    for (size_t i = 0; i < gRegistro.size(); i++) {
        const MallaRecurso* r = gRegistro[i];
        if (!r->vboPos) continue;
        const long nv = (long)r->vertexSize;
        b += nv * 3L * (long)sizeof(GLfloat);
        if (r->vboNor) b += nv * 3L;
        if (r->vboCol) b += nv * 4L;
        if (r->vboUV)  b += nv * 2L * (long)sizeof(GLfloat);
        if (r->vboIdx) b += (long)r->facesSize * (long)sizeof(MeshIndex);
    }
    return b;
}
