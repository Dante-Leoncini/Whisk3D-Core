#ifndef W3D_MALLA_RECURSO_H
#define W3D_MALLA_RECURSO_H
// ============================================================================
//  MallaRecurso — LA MALLA 3D COMO RECURSO COMPARTIDO (el "datablock" de mesh
//  de Blender): una malla con NOMBRE PROPIO, independiente de los objetos que
//  la usan, guardada UNA vez en su entrada .w3db del contenedor y cargada UNA
//  vez en memoria (CPU y GPU) sin importar cuantos objetos la dibujen.
//
//  PARA QUE: mil arboles iguales eran mil copias de la misma geometria (en el
//  archivo, en el heap y en la GPU). Con el recurso son UNA entrada, UN juego
//  de arrays y UN juego de VBOs; cada objeto guarda solo lo suyo (transform,
//  pose de skinning, vertex anims, flipbook).
//
//  EL MODELO (evolucion de MallaDatos, que se queda con los arrays crudos):
//   - REGISTRO del proyecto (proyecto.json):
//       "mallas": [{"nombre": "Arbol", "entrada": "mallas/arbol.w3db", "carpeta": "Vegetacion"}]
//     y el objeto malla dice "malla": "Arbol". Un recurso del registro sin
//     usuarios (HUERFANO) se conserva igual: es del proyecto, no de un objeto.
//   - EN MEMORIA: un MallaRecurso por entrada del registro, vivo toda la sesion
//     (el registro es el dueno). Los que vienen del ARCHIVO no se borran hasta
//     cerrar el proyecto (sin usuarios son huerfanos y se conservan); los creados
//     en la sesion se PURGAN cuando se quedan sin usuarios y sin pasos de undo que
//     los nombren (W3dMallasPurgarSinUso). Sus ARRAYS se cargan recien cuando
//     alguien los pide (el primer objeto que lo usa, una ListaCarga "malla:") por
//     el almacen de recursos (io/W3dRecursos.h, tipo W3DREC_MALLA, id = nombre de
//     entrada): cada objeto vinculado es UNA referencia; con la ultima que se
//     suelta, los arrays y los VBO se liberan (el recurso sigue en el registro).
//   - Mesh (el objeto) APUNTA ADENTRO: sus punteros de render (vertex, normals,
//     uv, vertexColor, faces) son los del recurso, marcados en Mesh::datosComp.
//     Todo el copy-on-write de siempre (Mesh::DesinstanciarDatos) sigue valiendo:
//     una vertex anim, el flipbook o un script que escribe vertices se hacen su
//     copia propia de ESA capa y el resto sigue compartido.
//   - LOS DATOS DE EDICION (caras, capas, marcas, aristas) NO se cargan por
//     objeto: la malla queda con la edicion PENDIENTE y se materializa del recurso
//     (sus bloques de edicion, releidos de la entrada o de los bytes en memoria)
//     recien cuando alguien los pide: Edit Mode, el objeto ACTIVO (sus listas en
//     Properties, el editor UV), weight/vertex paint, un modificador geometrico,
//     exportar, guardar en texto. La del activo se SUELTA cuando deja de serlo.
//     Para MIRARLA sin editarla (wireframe, snap, seleccion por caja) alcanza la
//     topologia COMPARTIDA del recurso (aristas y caras, una copia para todos).
//   - VBO: una malla no skinneada y sin vertex anim dibuja con los VBO del
//     RECURSO (una subida para N objetos); una skinneada comparte color/uv/indices
//     y sube solo su pose. GLES2/WebGL sin VBO: arrays de cliente (los mismos
//     punteros compartidos).
//   - EDITAR la malla de un objeto edita el RECURSO (lo ven todos sus usuarios):
//     el editor serializa la malla editada y la aplica al recurso con
//     W3dMallaRecursoAplicarBytes, que re-apunta a todos los usuarios. "Hacer
//     unica" copia el recurso con otro nombre (Arbol.001) para ese objeto solo.
//
//  EL FORMATO CANONICO del recurso son sus BYTES .w3db: la entrada del contenedor
//  (sin cargar nada en memoria mientras nadie la toque) o, si se edito o se creo
//  en esta sesion, 'bytes'. Guardar un recurso sin cambios = copiar su entrada.
//  Una entrada de TEXTO (.w3dm, "formatoMallas": "texto") se convierte a .w3db una
//  vez al cargarla (W3dMallaTextoABinHook) y esos bytes quedan como cache.
//  El formato en disco (registro, dedup, huerfanas): formato/mallas-registro.md.
//
//  C++03 puro, compila en TODOS los builds (editor, juego compilado, Symbian):
//  no llama a nada de main/. El editor pone lo suyo (materiales por nombre del
//  archivo, publicar ediciones, el guardado) desde afuera.
//  Motor generico: aca no hay nombres de ningun juego.
// ============================================================================
#include "MallaDatos.h"
#include "io/W3dMallaBin.h"   // W3dMallaBinParte/Grupo/Forma: la malla de render sin Mesh
#include <string>
#include <vector>

class Material;
struct W3dRecurso;

struct MallaRecurso : public MallaDatos {
    std::string nombre;      // el nombre del recurso (unico en el proyecto)
    std::string entrada;     // su entrada del contenedor ("mallas/arbol.w3db"); "" = nunca se guardo
    std::string carpeta;     // carpeta COSMETICA (outliner); no afecta al codigo
    std::string bytes;       // el .w3db EN MEMORIA (editado o creado en la sesion); "" = leer 'entrada'
    bool        modificado;  // 'bytes' difiere de la entrada: guardar = escribirlos
    // 'bytes' es la CONVERSION de una entrada de TEXTO (.w3dm, proyecto con "formatoMallas":
    // "texto"): el recurso vive en binario, asi que el texto se convierte UNA vez y queda como
    // cache (no es una edicion: 'modificado' sigue en false). Se tira al descargar los arrays.
    bool        bytesDeTexto;
    int         serial;      // identidad en la sesion (id del almacen mientras no tenga entrada;
                             // la del undo de un rename: el registro se purga, el serial no cambia)
    int         refsUndo;    // pasos de undo que lo nombran: mientras haya, no se purga
    // HUERFANO PEDIDO: creado en la sesion a proposito sin usuarios (el "Duplicate" de la biblioteca):
    // se conserva como un huerfano del archivo (la purga de la sesion no lo borra)
    bool        conservar;
    // BORRADO por el usuario (la biblioteca): ya no es del proyecto (no se lista ni se guarda), pero
    // sigue vivo mientras algun paso de undo lo retenga (un objeto borrado que lo usaba). Si un undo le
    // devuelve un usuario a la escena vuelve a ser del proyecto; si no, la purga lo borra cuando nadie
    // mas lo nombra (aunque venga del archivo).
    bool        borrado;
    unsigned    version;     // sube con cada cambio de datos (republicar)
    W3dRecurso* rec;         // su descriptor en el almacen mientras los arrays estan cargados
    // TODAS las mallas vinculadas: las de la escena y las que un paso de undo tiene
    // detachadas (siguen vivas y apuntando adentro: al republicar hay que moverlas igual)
    std::vector<Mesh*> usuarios;

    // lo que la malla de render trae ademas de los 5 arrays (W3dMallaBinLeerRender)
    bool  suave;
    int   primTipo; float primTam, primTam2, primAlto; int primVerts, primVerts2;
    std::vector<W3dMallaBinParte> partes;
    std::vector<Material*> materiales;   // el material de cada parte (NULL = el por defecto)
    // capas UV EXTRA de render (bloque RUVX; ver Mesh::uvExtra). No son un array compartido: cada usuario
    // recibe SU copia al vincularse (son chicas y nunca pasan por el COW de MallaDatos).
    std::vector< std::vector<GLfloat> > uvExtra;
    // LOS EXTRAS (identidad de los puntos, vertex/UV groups, shape keys, reposo 2D): cada usuario
    // tiene SU copia (el skinning, el weight paint y el editor los leen de la malla), asi que el
    // recurso NO se queda con otra: la suelta apenas un usuario LIMPIO la tiene
    // (extrasEnRecurso = false) y, para vincular al siguiente, copia la de un usuario limpio o
    // la relee de los bytes (W3dMallaBinLeerExtras). Mil personajes con el mismo rig = mil
    // copias, como antes de los recursos; nunca mil y una.
    std::vector<int> ctrl;               // render-vert -> representante de su punto
    std::vector<W3dMallaBinGrupo> grupos, uvGrupos;
    std::vector<W3dMallaBinForma> formas;
    std::vector<float> uvRest;
    bool  extrasEnRecurso;
    // la FIRMA de los extras (nombre y tamano de cada grupo, cuantas formas, el largo del
    // reposo 2D): lo que W3dMallaLimpia compara aunque el recurso ya no tenga los datos
    struct FirmaGrupo { std::string nombre; size_t n; };
    std::vector<FirmaGrupo> firmaGrupos, firmaUVGrupos;
    size_t firmaFormas, firmaUVRest;
    int   grupoActivo, uvGrupoActivo;
    float aabbMin[3], aabbMax[3], centro[3], radio;
    bool  aabbOk;
    int   vertsAgrupados;
    bool  statsNoEditable;               // el AABB/centro se hornearon como escenario cerrado
    W3dAjenos sello;                     // sello por dominio + bloques .w3db que no se entienden + REQ

    // VBO COMPARTIDOS (una subida para todos los usuarios): 0 = sin crear
    void*        flujos;                 // los flujos de indices derivados por parte (MallaFlujos.h; NULL = sin armar)
    int          refsRutina;             // pasos de Rutina que la dibujan (la mantienen cargada aunque no la use un objeto)
    unsigned int vboPos, vboNor, vboCol, vboUV, vboIdx;
    unsigned     vboVer;                 // 'version' con la que se subieron (0 = nunca)

    // LA TOPOLOGIA COMPARTIDA para MIRAR a los usuarios con la edicion PENDIENTE (wireframe,
    // snap a aristas y caras, seleccion por caja, estadisticas): las aristas y las caras del
    // recurso, leidas UNA vez de sus bloques de edicion cuando alguien las pide (nunca por
    // objeto: mil arboles en wireframe = una lista de aristas). Se tiran al descargar los
    // arrays y al republicar. Ver W3dMallaAristasVista / W3dMallaCarasVista / W3dMallaBordesVista.
    std::vector<int>      aristas;       // pares de render-verts (lo que seria Mesh::edges)
    std::vector<GLfloat>  bordes;        // esas aristas EN REPOSO como lineas (Mesh::bordesBuf)
    std::vector<MeshFace> caras;         // lo que seria Mesh::faces3d
    int                   topoEstado;    // 0 = sin leer, 1 = aristas, 2 = aristas y caras, -1 = no se pudo
    // LA LIBRERIA de la que es ("" = del proyecto). Una malla de una libreria externa (otro .w3d de solo
    // lectura, ver W3dMallaRecursoNuevoLib) NO esta en el registro del proyecto: no se lista, no se guarda ni se
    // edita. Su nombre lleva el prefijo ("Personajes/Cuerpo") y su entrada la ruta de la libreria
    // ("lib:Personajes/mallas/cuerpo.w3db"), asi que dos librerias no chocan y N proxies la comparten.
    std::string           libreria;

    MallaRecurso();
    ~MallaRecurso();
    bool Cargada() const { return rec != 0; }
    // la clave en el almacen de recursos: la entrada, o "*<serial>" mientras no tenga
    std::string IdAlmacen() const;
private:
    MallaRecurso(const MallaRecurso&);             // sin copia (C++03: privadas y sin definir)
    MallaRecurso& operator=(const MallaRecurso&);
};

// ---------------------------------------------------------------------------
//  EL REGISTRO del proyecto (orden = el del archivo; los nuevos al final)
// ---------------------------------------------------------------------------
const std::vector<MallaRecurso*>& W3dMallasRegistro();
MallaRecurso* W3dMallaRecursoPorNombre(const std::string& nombre);
MallaRecurso* W3dMallaRecursoPorEntrada(const std::string& entrada);
// indice en el registro (-1 = no esta)
int           W3dMallaRecursoIndice(const MallaRecurso* r);
// por su serial de la sesion (NULL = ya no esta): la identidad ESTABLE de los pasos de undo
// (un rename de la malla o de su carpeta), porque el registro se purga y los indices se corren
MallaRecurso* W3dMallaRecursoPorSerial(int serial);
// nombre libre en el registro para 'base' ("Arbol" ocupado -> "Arbol.001"); 'excepto' puede conservarlo
std::string   W3dMallaNombreLibre(const std::string& base, const MallaRecurso* excepto = 0);
// la CARPETA cosmetica escrita por el usuario, normalizada: separador '/' (tambien acepta '\\'),
// sin espacios en los bordes de cada tramo y sin tramos vacios ("  Vegetacion // Arboles/ " ->
// "Vegetacion/Arboles"). "" = sin carpeta (la raiz de la vista de mallas).
std::string   W3dMallaCarpetaNormalizar(const std::string& carpeta);
// crea un recurso VACIO en el registro con un nombre libre derivado de 'nombre'. Sus datos
// llegan por 'entrada' (proyecto que se abre) o por W3dMallaRecursoPonerBytes.
MallaRecurso* W3dMallaRecursoNuevo(const std::string& nombre, const std::string& entrada,
                                   const std::string& carpeta);
// cierra el proyecto: borra el registro entero (lo llama ReiniciarEscena con el arbol YA
// destruido). Un recurso que todavia tuviera usuarios se deja vivo con un aviso (sus
// punteros no pueden quedar colgando).
void          W3dMallasRegistroLimpiar();
// ---- LAS MALLAS DE LAS LIBRERIAS EXTERNAS (aparte del registro del proyecto) ----
// un recurso de la libreria 'libreria' ('nombre' ya con su prefijo, 'entrada' ya con "lib:<libreria>/"). Lo
// encuentran W3dMallaRecursoPorNombre / PorEntrada (despues de los del proyecto), pero W3dMallasRegistro no lo
// lista: no es del proyecto. Si ya habia uno con ese nombre, devuelve ese.
MallaRecurso* W3dMallaRecursoNuevoLib(const std::string& nombre, const std::string& entrada,
                                      const std::string& libreria);
// las mallas de las librerias ("" = de todas): se borran las que nadie usa (las usadas quedan vivas con un aviso,
// como en W3dMallasRegistroLimpiar). W3dMallasRegistroLimpiar tambien las borra (cierre del proyecto).
void          W3dMallasLibLimpiar(const std::string& libreria);
// cuantas mallas de librerias hay registradas (la prueba)
int           W3dMallasLibCantidad();
// las mallas de las librerias (aparte del registro del proyecto; ver W3dMallaRecursoNuevoLib)
const std::vector<MallaRecurso*>& W3dMallasLib();
// PURGA los recursos que ya no sirven: creados en la sesion (sin entrada en el archivo), sin
// usuarios y sin ningun paso de undo que los nombre (un "New Copy" deshecho cuyo redo se
// descarto, un Shift+D borrado). Los del ARCHIVO no se purgan: son huerfanos del proyecto y se
// conservan. La llama el tick del editor y el guardado; nunca en medio de un vinculo.
// Devuelve cuantos borro.
int           W3dMallasPurgarSinUso();
// BORRA UN HUERFANO del registro aunque venga del ARCHIVO (el "Purge orphans" / "Delete" del
// outliner por recursos): el proximo guardado ya no lo escribe (su entrada se descarta como toda
// malla no referenciada). Solo si nadie lo usa: sin usuarios (tampoco detachados por un borrado
// deshacible) y sin pasos de undo que lo nombren. false = no se puede (queda como estaba).
bool          W3dMallaRecursoBorrar(MallaRecurso* r);
// los pasos de undo que guardan un MallaRecurso* lo RETIENEN (no se purga) hasta soltarlo
void          W3dMallaRecursoUndoRetener(MallaRecurso* r);
void          W3dMallaRecursoUndoSoltar(MallaRecurso* r);

// ---------------------------------------------------------------------------
//  DATOS: los bytes canonicos y la carga de los arrays
// ---------------------------------------------------------------------------
// el .w3db del recurso: los bytes en memoria, o la entrada leida del contenedor. false = no hay.
bool W3dMallaRecursoLeerBytes(const MallaRecurso* r, std::string& out);
// reemplaza los datos del recurso por estos bytes (el editor, al crear/copiar/editar): quedan
// en memoria y 'modificado'. NO toca a los usuarios: para eso W3dMallaRecursoAplicarBytes.
void W3dMallaRecursoPonerBytes(MallaRecurso* r, const std::string& bytes);
// el material de cada parte al CARGAR un recurso por primera vez: lo pone el lector del
// proyecto (los nombres de PARTMAT son los del bloque "materiales" del archivo). NULL =
// BuscarMaterialPorNombre (el nombre vivo).
extern Material* (*W3dMallaMaterialPorNombreHook)(const std::string& nombre);
// una entrada del registro guardada en TEXTO (.w3dm) se convierte a .w3db para cargarla: el
// lector de texto y la derivacion (index buffer, bordes, capas) son del lector del proyecto, que
// pone este gancho (import_w3d.cpp). 'nombre' va en INFO. NULL / false = no se puede leer.
extern bool (*W3dMallaTextoABinHook)(const unsigned char* datos, size_t n, const std::string& nombre,
                                     std::string& out);

// ---------------------------------------------------------------------------
//  LOS USUARIOS
// ---------------------------------------------------------------------------
// VINCULA la malla al recurso: carga sus arrays si hace falta, suelta lo que la malla tenia
// (propio o compartido) y apunta sus capas de render ADENTRO del recurso. Copia lo chico por
// instancia (mesh parts con su material, grupos, formas, reposo 2D, AABB, sello). La edicion
// queda PENDIENTE (se materializa del recurso al pedirla) salvo con 'conservarEdicion': la
// malla YA tiene su edicion y coincide con el recurso (acaba de publicarlo o de guardarse).
// false = el recurso no se pudo cargar: la malla NO se toca.
bool W3dMallaVincular(Mesh* m, MallaRecurso* r, bool conservarEdicion = false);
// MUDA el vinculo de la malla a otro recurso SIN tocar sus datos: lo que compartia pasa a
// ser propio (copia) y la malla sigue con su edicion y su jaula (la malla en Edit Mode que un
// guardado separo a otro recurso). false = el recurso no se pudo cargar (no cambia nada).
bool W3dMallaMudar(Mesh* m, MallaRecurso* r);
// suelta el recurso (lo llama Mesh::SoltarDatosCompartidos, o sea ~Mesh): las capas
// compartidas quedan NULL, sale de la lista de usuarios y se suelta la referencia.
void W3dMallaDesvincular(Mesh* m);
// cuantas veces una malla quedo SIN recurso en la sesion (contador monotono): el editor, que quiere
// que TODA malla tenga el suyo, solo recorre la escena cuando esto o W3dObjetosNacidos cambian
unsigned W3dMallasDesvinculadas();
// la edicion materializada de una malla LIMPIA (sus capas siguen siendo las del recurso)
// vuelve a quedar pendiente: libera caras, capas y aristas que se pueden releer. No-op si la
// malla no es de un recurso o si algo suyo pudo cambiar.
void W3dMallaSoltarEdicion(Mesh* m);
// usuarios que estan en la ESCENA (colgados de 'raiz'; si es NULL, de TODAS las raices cargadas:
// la activa y las escenas/prefabs inactivos, ver W3dRaicesVivas)
int  W3dMallaRecursoUsuariosEnEscena(const MallaRecurso* r, const Object* raiz = 0);
// la malla sigue siendo la del recurso? (todas las capas que el recurso tiene siguen
// compartidas y no hay edicion propia que pueda haberse tocado): guardar = sus bytes
bool W3dMallaLimpia(const Mesh* m);
// lo que la malla copia POR INSTANCIA del recurso sigue igual? (mesh parts con su material,
// vertex/UV groups, cuantas formas, el suavizado): la parte de W3dMallaLimpia que no mira la
// edicion ni los arrays. false = la malla tiene algo propio sin publicar (un grupo nuevo, un
// rename de mesh part) o no es de un recurso.
bool W3dMallaExtrasComoRecurso(const Mesh* m);
// los datos del recurso CAMBIARON (el editor publico una edicion): se reemplazan por 'bytes'
// (en memoria, modificado) y se re-apunta a TODOS los usuarios salvo 'excepto' (el que
// publico: se queda con lo suyo, que es lo mismo). 'mats' = el material de cada parte (el
// de la malla que publico; NULL = por nombre). false = los bytes no son un .w3db legible.
bool W3dMallaRecursoAplicarBytes(MallaRecurso* r, const std::string& bytes, Mesh* excepto,
                                 const std::vector<Material*>* mats);
// idem con VARIOS exceptuados (el que publica y la malla en Edit Mode: su jaula y sus capas son
// suyas y re-apuntarla las tiraria). Cada exceptuado se queda con una copia PROPIA de lo que ve.
bool W3dMallaRecursoAplicarBytes(MallaRecurso* r, const std::string& bytes, const std::vector<Mesh*>& exceptos,
                                 const std::vector<Material*>* mats);
// el material de cada parte del recurso (r->materiales) resuelto aunque NO este cargado (un
// huerfano: se leen solo los nombres de su PART y se resuelven una vez por el nombre del archivo).
// 'enArchivo' (opcional) = los nombres de material que dicen sus bytes. false = no se pudo leer.
bool W3dMallaRecursoMateriales(MallaRecurso* r, std::vector<std::string>* enArchivo);
// cambia el material de la parte 'k' en el RECURSO y en TODOS sus usuarios (el material es
// dato de la malla, como en Blender). Los bytes se regeneran al guardar (PART por nombre).
void W3dMallaRecursoCambiarMaterial(MallaRecurso* r, int k, Material* mat);

// ---------------------------------------------------------------------------
//  LA TOPOLOGIA PARA MIRAR (sin materializar la edicion)
//  La de la malla si la tiene en memoria; si su edicion esta PENDIENTE en su recurso, la
//  COMPARTIDA del recurso (leida una vez para todos sus usuarios). NULL = no hay (sin caras,
//  escenario cerrado a edicion, o no se pudo leer). Lo que devuelven es de SOLO LECTURA y vale
//  hasta el proximo cambio de la malla o del recurso (no guardar el puntero).
// ---------------------------------------------------------------------------
const std::vector<int>*      W3dMallaAristasVista(const Mesh* m);
const std::vector<MeshFace>* W3dMallaCarasVista(const Mesh* m);
// las aristas como lineas EN REPOSO (lo que dibuja el wireframe): las de la malla, o las del
// recurso si la malla dibuja sus posiciones (no esta posada ni tiene copia propia)
const std::vector<GLfloat>*  W3dMallaBordesVista(const Mesh* m);

// ---------------------------------------------------------------------------
//  RENDER: los VBO compartidos
// ---------------------------------------------------------------------------
// sube (una vez por version) los arrays del recurso a sus VBO. false = sin VBO en esta plataforma.
bool W3dMallaRecursoAsegurarVBO(MallaRecurso* r);
// (la Rutina) una referencia PERMANENTE sobre el recurso: lo carga si hace falta y lo deja cargado aunque ningun
// objeto lo use. true = cargado. Cada Retener se paga con un Soltar
bool W3dMallaRecursoRetener(MallaRecurso* r);
void W3dMallaRecursoSoltar(MallaRecurso* r);
// el recurso sigue en el registro y no se borro (un puntero guardado por una rutina)
bool W3dMallaRecursoVivo(const MallaRecurso* r);
// sube cada vez que un recurso sale del registro (un puntero vivo sigue vivo mientras no cambie)
unsigned W3dMallasGen();
// sube con todo lo que puede cambiar lo que valida el editor de una RUTINA: sus pasos (Resolver, el panel, setPaso, la
// animacion de pasos) y los datos de una malla (version). Jugando, una rutina se revalida solo si cambio (RutinaEditor)
extern unsigned g_w3dRutinasGen;

// ---------------------------------------------------------------------------
//  EDICION PENDIENTE y MALLAS EDITADAS
// ---------------------------------------------------------------------------
// marca que va en Mesh::edicionPendiente cuando la edicion sale del recurso (no de una entrada suelta)
extern const char* const W3D_EDICION_DEL_RECURSO;
// las dos puertas al render de la malla (RefrescarRender / GenerarRender, en el editor)
// anotan aca las mallas VINCULADAS que cambiaron: el editor las publica al recurso cuando
// salen de edicion. Sin editor nadie las procesa (una entrada por malla, como mucho).
void W3dMallaMarcarEditada(Mesh* m);
// saca y devuelve las anotadas (el que las procesa)
void W3dMallasTomarEditadas(std::vector<Mesh*>& out);

// ---------------------------------------------------------------------------
//  MEDICION (meminfo / mallainfo)
// ---------------------------------------------------------------------------
int  W3dMallasRecursosCargados();        // recursos con los arrays en memoria
int  W3dMallasVBOCompartidos();          // recursos con VBO compartido subido
long W3dMallasVBOBytes();                // bytes de GPU de esos VBO

// el alta de los ops del tipo W3DREC_MALLA en el almacen (Cargar/Liberar) y del gancho con
// el que Mesh suelta una MallaDatos legacy. Idempotente.
void W3dMallasAsegurarOps();

#endif
