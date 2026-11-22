#ifndef W3D_ANIMSET_H
#define W3D_ANIMSET_H

// ============================================================================
//  ANIMSETS: LOS CLIPS DE ESQUELETO COMO RECURSO COMPARTIDO (.w3da, version 1)
//
//  Antes los clips de cada armature viajaban INLINE en project.json: un keyframe
//  = una fila de texto de 8 numeros, y cada armature repetia los clips que
//  compartia con los demas (el mismo "caminar" en diez enemigos = diez copias en
//  el archivo Y diez copias en memoria). Abrir era parsear un JSON gigante.
//
//  Ahora un ANIMSET es una BIBLIOTECA de clips (los de un tipo de esqueleto) en
//  una entrada binaria del contenedor, animations/<slug>.w3da. El armature guarda
//  solo a que animset pertenece y QUE clips usa de el, por indice:
//      "anims": "Enemigo", "clips": [0, 3, 4, 7]
//  y el proyecto registra los animsets en la raiz:
//      "animsets": [{ "nombre": "Enemigo", "entrada": "animations/enemigo.w3da", "carpeta": "" }]
//  El formato inline viejo ("anims": [ {...clips...} ]) se sigue leyendo y al
//  guardar se migra solo.
//
//  EN MEMORIA: el animset es un recurso del almacen (W3DREC_ANIMSET, id = su
//  entrada) con los clips UNA sola vez. Los armatures que lo usan apuntan a esos
//  clips (Armature::animations son punteros ADENTRO del recurso, igual que los
//  arrays de render de una malla compartida). Los clips son datos INMUTABLES; el
//  estado de reproduccion (clip activo, cabezal, capas, transicion) es de cada
//  armature, como siempre. Las curvas HORNEADAS (frames regulares, una
//  interpolacion y un tipo de handle para todas, handles en cero) quedan
//  COMPACTAS: ~4 B por keyframe (W3dKeys, Animation.h; formato/w3da.md, "En
//  memoria"). Jugar no las expande; el escritor las escribe sin expandirlas.
//
//  LOS HUESOS VAN POR NOMBRE: el animset trae su tabla de huesos (HUES) y cada
//  pista del clip dice su hueso por indice EN ESA TABLA. Al asignarlo a un armature
//  se resuelve cada nombre a su indice ahi (el k-esimo hueso con ese nombre, por si
//  hay repetidos). Si el esqueleto calza tal cual (el caso comun: todos los
//  enemigos del mismo rig) el armature usa los clips del recurso directamente; si
//  no, se arma UNA copia reindexada por cada mapeo distinto (una "variante"), que
//  comparten todos los armatures con ese mismo mapeo.
//
//  EDITAR ES COPIAR (copy-on-write): antes de tocar un clip, el armature pide sus
//  clips PROPIOS (W3dOwnArmatureAnims): se copian (o, si es el unico usuario,
//  se le pasan sin copiar) y el recurso sigue intacto para los demas. Al guardar
//  se deduplica POR CLIP: lo que no cambio vuelve a caer en el mismo clip de la
//  biblioteca.
//
//  FORMATO (little-endian EXPLICITO, sin padding basura: los mismos bytes en cada
//  guardado; el mismo esquema de bloques que el .w3db):
//      cabecera (20 B): "W3DA" | u32 version(=1) | u32 flags(0) | u32 nBlocks | u32 reservado(0)
//      bloque:          fourcc | u32 domain(0) | u32 cantidad | u32 bytes | payload (relleno a 4 con ceros)
//  - REQ  (va primero): los fourcc que un lector DEBE entender (mismo contrato que
//    el .w3db: si nombra uno desconocido, el animset NO carga y el guardado se frena).
//  - HUES: la tabla de huesos. cantidad = nBones; por hueso un string.
//  - CLIP: un bloque por clip, en el orden de la biblioteca. cantidad = nPistas.
//    nombre, fps, inicio, fin; por pista: hueso (indice en HUES, -1 = ninguno) y sus
//    curvas; por curva: propiedad, componente, nKeys y los keyframes COMPACTOS: los
//    frames (o solo el primero y el paso si son regulares), los valores f32, la
//    interpolacion y el tipo de handle (o uno solo si son todos iguales) y los
//    handles f32 x4 (o nada si son todos cero). Todo exacto: los floats van con sus
//    bits (un -0.0 sigue siendo -0.0).
//    Si el clip sabe el TAMANO del esqueleto que lo grabo (SkeletalAnimation::heightRest, el
//    retarget: lo anotan el editor al crearlo y los importadores FBX/glTF), va al final del bloque
//    como un f32 mas; si no (0: un clip de un archivo viejo) no se escribe nada y el bloque queda
//    byte a byte como antes. (Por eso dos clips con las mismas curvas de esqueletos de distinto
//    tamano no se deduplican: cada uno conserva su origen.)
//  - JCLP: un bloque por CLIP DE JERARQUIA (ver abajo), despues de los CLIP. cantidad = nPistas.
//    nombre, fps, inicio, fin, tamano de origen (f32), retarget por defecto (u32); por pista: su RUTA
//    (string) y sus curvas (el mismo formato de curva que CLIP). Si el animset tiene alguno, JCLP va
//    en REQ (un lector que no lo entiende no lo puede usar ni re-escribir).
//  - Un bloque desconocido que no esta en REQ se PRESERVA y se reescribe al guardar
//    mientras el animset no se edite.
//  Los strings van como u32 largo + bytes (sin terminador, sin relleno).
//  Spec completa: Whisk3D/formato/w3da.md; los fourcc estan en formato/bloques.tsv
//  (filas "w3da:").
//
//  C++03 puro. Entra en la build del JUEGO COMPILADO (Linux/Android/web/Symbian):
//  no puede llamar a nada de main/ (el editor).
// ============================================================================

#include <string>
#include <vector>
#include <cstddef>
#include "animation/Animation.h"    // AnimProperty: las curvas de un clip de jerarquia
#include "animation/W3dLayerAnim.h" // las CAPAS de clips de jerarquia de una raiz (el Mix)

class Armature;
class SkeletalAnimation;
class Object;
struct W3dResource;

// ---------------------------------------------------------------------------
//  CLIPS DE JERARQUIA ("un objeto raiz anima a sus hijos"). Un clip de jerarquia lo TIENE un
//  objeto raiz y anima a esa raiz y a sus DESCENDIENTES por RUTA RELATIVA de nombres:
//      "."  la raiz misma          "Chasis"  un hijo          "Chasis/PuertaIzq"  un nieto
//  con pistas de transformacion y de propiedades animables (fov de camara, colores de luz,
//  visible, formas...): las mismas curvas que una animacion de escena. Sirve para jerarquias de
//  objetos SIN armature (un auto con sus puertas, una grua, un ascensor) y para camaras y luces.
//  Un clip de un solo objeto es el caso de la ruta ".".
//
//  COMPARTIDO: viven en un ANIMSET (el mismo recurso .w3da que los clips de esqueleto, bloques
//  JCLP) y el objeto raiz nombra a su BIBLIOTECA (Object::clipsHier, "clipsJerarquia" en el
//  .w3d). Cualquier objeto cuya jerarquia de NOMBRES coincida lo puede reproducir (los nodos que
//  no estan en el destino se ignoran): N objetos iguales reproduciendo el mismo clip = UNA copia
//  en memoria. Editar un clip (desde cualquiera de sus usuarios) lo cambia para TODOS.
//
//  LAS RUTAS toleran los sufijos de nombre unico: duplicar una jerarquia en la misma escena
//  renombra sus hijos ("Chasis.001"), asi que cada tramo se busca primero por el nombre exacto y
//  despues por su nombre BASE (sin el ".NNN"). Si hay varios hermanos con la misma base el tramo
//  lleva su orden entre ellos: "Rueda#1" (el segundo "Rueda..." en el orden de los hijos). Lo
//  arma W3dHierPath.
//
//  LA RAIZ ES RELATIVA: las curvas de transformacion de la pista "." se aplican como DELTA desde
//  su valor en el primer frame del clip, sumado a donde estaba la raiz al arrancarlo (N puertas en
//  distintos lugares del nivel abren igual cada una en su lugar). Las pistas de los descendientes
//  son locales a su padre, asi que van tal cual.
//
//  RAICES ANIDADAS (un nodo del clip que suena en una raiz es la raiz de otro que suena): se posan los
//  ancestros primero y la pista "." de la anidada se suma a lo que le hizo el ancestro. Ver "RAICES
//  ANIDADAS" en Animation.cpp.
// ---------------------------------------------------------------------------
struct W3dHierTrack {
    std::string path;                    // "." = la raiz del clip; si no, "Hijo/Nieto" (ver W3dHierPath)
    std::vector<AnimProperty> props;     // sus curvas (propiedad, componente, keyframes)
};
// modo de RETARGET de un clip de jerarquia (mismos valores que el de los armatures, SkeletalAnimation.h):
//   0 completo (el clip tal cual) / 1 rotaciones (los descendientes solo rotan: sus traslaciones y
//   escalas son las del destino; el movimiento de la raiz se escala por tamanoDestino / tamano)
class W3dClipHier {
public:
    std::string name;
    int fps, start, end;
    float size;                          // TAMANO de la jerarquia con la que se grabo (W3dHierSize); 0 = no se sabe
    int retarget;                        // el modo por defecto de este clip (la instancia lo puede cambiar)
    std::vector<W3dHierTrack> tracks;
    // (solo en memoria) 'version' sube con cada cambio de sus datos (las vistas de edicion del editor saben si
    // quedaron viejas); 'serie' identifica al clip para siempre (una direccion reciclada no se confunde con el)
    unsigned version, series;
    W3dClipHier();
    // la duracion en segundos (a su fps)
    float Duration() const;
};

enum { W3DA_VERSION = 1 };

// ---------------------------------------------------------------------------
//  FORMATO
// ---------------------------------------------------------------------------
// true si los bytes arrancan con la firma "W3DA"
bool W3dAnimSetBinIs(const unsigned char* data, size_t n);
// TODOS los fourcc que esta version entiende, en el orden en que se escriben;
// 'requeridos' (opcional) recibe los que el escritor pone en REQ
void W3dAnimSetBlocksKnown(std::vector<std::string>& known,
                                std::vector<std::string>* required = 0);

// un bloque que esta version no entiende (y no esta en REQ): se preserva tal cual
struct W3dAnimSetForeignBlock {
    std::string id;          // fourcc
    unsigned domain, count;
    std::string data;       // el payload, sin el relleno
    W3dAnimSetForeignBlock() : domain(0), count(0) {}
};

// el CONTENIDO de un .w3da en memoria. Es DUENO de los clips (el destructor los libera).
struct W3dAnimSetData {
    std::vector<std::string> bones;               // tabla HUES: el espacio de indices del animset
    std::vector<SkeletalAnimation*> clips;        // tracks[].bone = indice en 'huesos' (o -1)
    std::vector<W3dClipHier*> hierarchies;        // los CLIPS DE JERARQUIA (bloques JCLP)
    std::vector<W3dAnimSetForeignBlock> foreign;
    W3dAnimSetData() {}
    ~W3dAnimSetData();
private:
    W3dAnimSetData(const W3dAnimSetData&);
    W3dAnimSetData& operator=(const W3dAnimSetData&);
};

// ESCRITURA en dos pasos, porque el guardado deduplica POR CLIP:
//  1) W3dAnimSetClipBlock: el bloque CLIP ENTERO (cabecera + payload + relleno) de un
//     clip, con sus huesos traducidos al espacio del animset: mapBones[i] = indice en
//     la tabla del hueso i del armature (-1 = no esta). Una pista cuyo hueso no se puede
//     traducir sale con -1 (no mueve nada, igual que en memoria). DETERMINISTA: el mismo
//     clip con el mismo mapa da SIEMPRE los mismos bytes (sirven de clave de dedup).
//  2) W3dAnimSetBuild: cabecera + REQ + HUES + los bloques CLIP (en ese orden) + ajenos.
void W3dAnimSetClipBlock(const SkeletalAnimation& c, const std::vector<int>& mapBones,
                          int nBones, std::string& out);
void W3dAnimSetBuild(const std::vector<std::string>& bones,
                     const std::vector<const std::string*>& blocksClip,
                     const std::vector<W3dAnimSetForeignBlock>* foreign, std::string& out,
                     const std::vector<const std::string*>* blocksHier = 0);
// el bloque JCLP ENTERO de un clip de jerarquia (determinista, como W3dAnimSetClipBlock)
void W3dAnimSetClipHierBlock(const W3dClipHier& c, std::string& out);
// lee un .w3da. false = no es un .w3da, es de una version mas nueva, pide (REQ) un
// bloque que esta version no conoce, o esta corrupto; 'motivo' dice cual. En los
// cuatro casos 'out' queda VACIO.
bool W3dAnimSetRead(const unsigned char* data, size_t n, W3dAnimSetData& out, std::string* reason);
// DIAGNOSTICO: una linea por bloque ("CLIP 0 12 pistas 4400 B"). false = no es un .w3da valido.
bool W3dAnimSetList(const unsigned char* data, size_t n, std::vector<std::string>& lines);

// ---------------------------------------------------------------------------
//  EL RECURSO (W3DREC_ANIMSET)
// ---------------------------------------------------------------------------
// clips reindexados para los armatures cuyo esqueleto NO calza con la tabla del animset
struct W3dAnimSetVariant {
    std::vector<int> map;                         // indice de la tabla -> hueso del armature (-1 = no lo tiene)
    std::vector<SkeletalAnimation*> clips;        // paralelo a datos.clips; NULL = todavia no se pidio
    W3dAnimSetVariant() {}
    ~W3dAnimSetVariant();
private:
    W3dAnimSetVariant(const W3dAnimSetVariant&);
    W3dAnimSetVariant& operator=(const W3dAnimSetVariant&);
};
// el DATO del recurso: una biblioteca de clips inmutables compartida por armatures
struct W3dAnimSet {
    std::string name;                           // su nombre en el registro del proyecto
    W3dAnimSetData data;                        // la tabla de huesos + los clips canonicos
    std::vector<W3dAnimSetVariant*> variants;
    // HUELLA de los bytes de los que se leyo: el guardado sabe si la entrada que escribe
    // sigue siendo la misma que tiene este recurso en memoria (ver SaveAnimSets.cpp)
    unsigned footprint1, footprint2;
    size_t   fileSize;
    // cuantas de las referencias del recurso son del HISTORIAL DE UNDO del editor (un paso que guarda un clip
    // sacado de esta biblioteca, o la biblioteca que un objeto tenia antes): no son usuarios. Las mediciones
    // de "cuantos la usan" las descuentan (el harness: jerinfo refs).
    int      refsUndo;
    W3dAnimSet() : footprint1(0), footprint2(0), fileSize(0), refsUndo(0) {}
    ~W3dAnimSet();
private:
    W3dAnimSet(const W3dAnimSet&);
    W3dAnimSet& operator=(const W3dAnimSet&);
};
// registra las ops del tipo en el almacen (idempotente; lo llaman las funciones de abajo)
void W3dAnimSetsEnsureOps();
// las dos huellas de un bloque de bytes (las mismas que guarda W3dAnimSet)
void W3dAnimSetFootprint(const unsigned char* d, size_t n, unsigned& h1, unsigned& h2);

// ---------------------------------------------------------------------------
//  EL REGISTRO DEL PROYECTO ("animsets" de project.json)
// ---------------------------------------------------------------------------
struct W3dAnimSetRow {
    std::string name, entry, folder;   // carpeta: cosmetica (el outliner por recursos)
};
void W3dAnimSetsRegistryClear();
void W3dAnimSetsRegistryAdd(const std::string& name, const std::string& entry,
                                const std::string& folder);
const std::vector<W3dAnimSetRow>& W3dAnimSetsRegistry();
void W3dAnimSetsRegistrySet(const std::vector<W3dAnimSetRow>& rows);
// la entrada del animset 'nombre' ("" si no esta registrado)
std::string W3dAnimSetsEntryOf(const std::string& name);
// cerrar el proyecto: vacia el registro y olvida los descriptores que quedaron (los
// FALLO recordados no pueden bloquear una entrada homonima del proyecto siguiente)
void W3dAnimSetsCloseProject();
// ---- LOS ANIMSETS DE LAS LIBRERIAS EXTERNAS (aparte del registro del proyecto) ----
// 'nombre' ya con el prefijo de su libreria ("Personajes/Caminar") y 'entrada' con el de su ruta
// ("lib:Personajes/animations/caminar.w3da"). Los encuentra W3dAnimSetsEntryOf (despues de los del proyecto)
// pero W3dAnimSetsRegistry no los lista: no son del proyecto (no se guardan ni se renombran). El recurso del
// almacen se comparte igual (N proxies del mismo personaje = UN animset en memoria). El cierre del proyecto los
// olvida (W3dAnimSetsCloseProject).
void W3dAnimSetsLibAdd(const std::string& name, const std::string& entry);
// los de la libreria 'libreria' ("" = los de todas)
void W3dAnimSetsLibClear(const std::string& lib);
// 'nombre' es un animset de una libreria (no del proyecto)?
bool W3dAnimSetIsFromLib(const std::string& name);
// LIBERAR LA CLAVE de un descriptor ANIMSET (su entrada pasa a ser de otro dato: el cierre del
// proyecto, una entrada recien escrita por el guardado). Sin referencias y sin dato (un FALLO
// recordado) se BORRA del almacen (true); con usuarios se corre a "#<motivo><n>:<entrada>",
// una clave que no es una entrada, sin acumular los prefijos de un desenganche anterior.
bool W3dAnimSetUnhook(W3dResource* r, const char* reason, int n);
// la ENTRADA de un id de descriptor: el id sin el prefijo "#...:" de un desenganche (o de un
// animset EN MEMORIA, que todavia no tiene entrada: ahi da su nombre)
std::string W3dAnimSetIdEntry(const std::string& id);

// ---------------------------------------------------------------------------
//  ARMATURES
// ---------------------------------------------------------------------------
// asigna al armature los clips 'indices' (NULL = todos, en su orden) del animset
// registrado como 'nombre'. Los clips quedan COMPARTIDOS (a->animsRec). Reemplaza
// los que tuviera. false + motivo = no se pudo (el armature queda SIN clips).
bool W3dArmatureAssignAnimSet(Armature* a, const std::string& name,
                               const std::vector<int>* indices, std::string* reason);
// COPY-ON-WRITE: deja al armature con clips PROPIOS (los puede editar sin tocar a
// nadie). No-op si ya lo eran. Lo llama todo lo que modifica un clip.
void W3dOwnArmatureAnims(Armature* a);
// suelta los clips compartidos SIN liberarlos (el destructor del armature)
void W3dArmatureAnimsRelease(Armature* a);
// DUPLICAR UN ARMATURE (Shift+D / Alt+D del editor). Los dos reemplazan lo que 'dst' tuviera y le
// pasan el animSetName de 'src' (la biblioteca a la que lo manda el guardado: la copia vuelve a
// caer en el mismo animset y deduplica).
//  - Copiar (Shift+D, copia REAL): clips PROPIOS, independientes de los de src.
//  - Vincular (Alt+D, duplicado VINCULADO): los MISMOS clips en memoria. Si src ya cuelga de un
//    animset, dst toma una referencia mas. Si src tiene clips PROPIOS, primero pasan a ser un
//    animset EN MEMORIA (todavia sin entrada: el guardado le da una), como la malla suelta que
//    pasa a tener su recurso; src no cambia de clips (el animset se queda con esos mismos
//    objetos, sin copiar) y editar despues es copiar, como siempre. false = no se pudo compartir
//    (clips repetidos o con nombres que la reparacion del editor tocaria): dst queda con una
//    copia propia, igual que con Copiar.
void W3dArmatureAnimsCopy(Armature* dst, const Armature* src);
bool W3dArmatureAnimsLink(Armature* dst, Armature* src);
// BORRA el clip i de la lista del armature (DeleteActiveAnimation). Sacar un clip de la lista
// NO escribe en ningun clip: si son COMPARTIDOS se suelta solo el puntero (el clip sigue en el
// animset para los demas) y el armature queda colgado con los que le quedan, SIN copiar nada
// (igual que reordenar); si le quedan 0, suelta el recurso. El UNICO usuario (salvo por una
// variante) pasa a clips propios (se le pasan sin copiar) y el borrado se libera de verdad.
// Con clips propios, se libera. false = indice fuera de rango.
bool W3dArmatureAnimsRemove(Armature* a, int i);
// true si los clips del armature son del recurso (no propios)
bool W3dArmatureAnimsShared(const Armature* a);
// QUIEN COPIA: solo el que ESCRIBE, justo antes de escribir. MIRAR no copia: ver el clip en el
// timeline o en el editor de curvas, elegir keyframes, la tarjeta Keyframe, posar en Pose Mode
// o reproducir dejan al armature colgado del animset (sin memoria extra). Los que escriben:
//   Core:  CreateAnimation, DuplicateActiveAnimation, InsertKeyframeSkeleton,
//          AutoKeyBone/AutoKeySkeletonPrep/Fin (SkeletalAnimation.cpp) y los setters de
//          rango/fps del clip activo (AnimSetStart/End/Fps, Animation.cpp: solo si el valor cambia);
//   main:  el resolver de curvas del dope/editor de curvas (Timeline.cpp: DopeResolveProp con
//          editar=true; el dibujo, el hit-test y la tarjeta Keyframe lo piden con editar=false),
//          los dos renames de clip del panel (Properties.cpp), el borrado de huesos (BoneEdit.cpp)
//          y el undo que restaura curvas/pistas/nombres (Undo.cpp: KFResolve al aplicar,
//          BonesUndo, el destino ClipArm de un rename).
//  REORDENAR y BORRAR un clip (MoveActiveAnimation, ClipSwapInArm de Undo.cpp y
//  DeleteActiveAnimation -> W3dArmatureAnimsRemove) tampoco copian: solo tocan la lista de
//  punteros, que es del armature (el guardado ya escribe cualquier subconjunto, en cualquier orden).
//  El harness verifica ademas, antes de cada comando, que NINGUN clip de un animset cargado haya
//  cambiado en memoria (test/W3dTestsAnims.cpp, "sellos"): un camino nuevo que escriba sin
//  copiar rompe la suite en vez de pasar callado.
// el animset del que cuelga el armature (NULL = clips propios)
W3dAnimSet* W3dArmatureAnimSet(const Armature* a);
W3dResource* W3dArmatureAnimSetResource(const Armature* a);
// true si el armature sigue colgado de su animset Y su esqueleto todavia calza con la tabla
// como cuando se asigno (mismo mapeo por nombre). Un hueso renombrado despues (p.ej. arrastrado
// por el rename de su vertex group, que no toca los clips) rompe el mapeo: ese armature ya no se
// puede guardar en el animset por nombre y el guardado lo trata como de clips propios.
bool W3dArmatureAnimSetFits(const Armature* a);
// indice en el animset de cada clip del armature (solo armatures con clips compartidos;
// false si alguno no se encuentra, que no deberia pasar)
bool W3dArmatureAnimsIndices(const Armature* a, std::vector<int>& out);
// mapa hueso del armature -> indice de la tabla 'huesos' (por nombre y ocurrencia;
// -1 = el hueso no esta en la tabla). Con el mismo esqueleto es la identidad.
void W3dAnimSetMapFromArmature(const std::vector<std::string>& bones, const Armature* a,
                                 std::vector<int>& out);
// los nombres de los huesos del armature, en orden
void W3dArmatureBoneNames(const Armature* a, std::vector<std::string>& out);

// ---------------------------------------------------------------------------
//  OBJETOS RAIZ DE CLIPS DE JERARQUIA (Object::clipsHier)
// ---------------------------------------------------------------------------
// lo que un objeto tiene de los clips de jerarquia: su BIBLIOTECA (un animset; su referencia) y como
// los reproduce. NULL en Object::clipsHier = nada (el caso de casi todos los objetos).
struct W3dHierRoot {
    std::string animset;        // la biblioteca que usa (nombre del registro o de una en memoria); "" = ninguna
    W3dResource* rec;           // su recurso (una referencia PERMANENTE); NULL = sin biblioteca
    std::string notLoaded;      // la biblioteca no se pudo leer al abrir: el guardado se frena (no la pisa)
    int retarget;               // -1 = el de cada clip; 0 completo; 1 rotaciones (animRetarget de lua, la tarjeta
                              // Animacion del objeto). Se guarda en su nodo ("retarget")
    // CAPAS (el Mix, como las de un armature): varios clips de jerarquia MEZCLADOS sobre esta raiz, cada uno con
    // su influencia y su modo (W3dLayerAnim.anim = el nombre del clip: de su biblioteca o "Biblioteca/clip").
    // Con al menos una capa la raiz muestra la MEZCLA (animObjeto deja de posarla). Se guardan en su nodo
    // ("layersHier"); jugando cada capa tiene su cabezal (objetoCapa... de lua). Ver W3dHierLayersChanged.
    std::vector<W3dLayerAnim> layers;
    int activeLayer;           // la capa elegida en el Mix del editor (-1 = ninguna)
    W3dHierRoot() : rec(0), retarget(-1), activeLayer(-1) {}
};
// la biblioteca de 'o' (NULL = no tiene, o no esta cargada)
W3dAnimSet* W3dHierAnimSet(const Object* o);
W3dResource* W3dHierResource(const Object* o);
// 'o' pasa a usar la biblioteca 'nombre': una que ya vive en memoria con ese nombre o la del registro
// del proyecto (se lee de su entrada). "" = ninguna. false + motivo = no se pudo (queda sin biblioteca
// y con notLoaded = nombre: el guardado se frena en vez de perderla).
bool W3dHierAssign(Object* o, const std::string& name, std::string* reason);
// la biblioteca 'nombre' con una referencia PERMANENTE que el que la pide suelta (W3dResourceRelease): la que ya
// vive en memoria con ese nombre o la del registro, leida de su entrada. NULL + motivo (texto interno, para el
// log) si no se puede leer. El desplegable del editor la prueba ANTES de tocar nada: si no se puede, el objeto
// sigue con la que tenia (W3dHierAssign lo dejaria sin biblioteca y con el guardado frenado por 'notLoaded').
W3dResource* W3dHierLibraryTake(const std::string& name, const char* who, std::string* reason);
// suelta la biblioteca de 'o' (y borra Object::clipsHier si no queda nada que recordar). La llama ~Object.
void W3dHierRelease(Object* o);
// UNDO (el editor): la biblioteca de 'o' vuelve a ser EXACTAMENTE esa: el recurso 'r' (se le toma una referencia
// propia; NULL = ninguna, o una que no se pudo leer), su nombre y su 'notLoaded'. No carga nada.
void W3dHierRestore(Object* o, W3dResource* r, const std::string& animset, const std::string& notLoaded);
// DUPLICAR (Shift+D / Alt+D): la copia usa la MISMA biblioteca (los clips son datos compartidos, como un material)
void W3dHierLink(Object* dst, const Object* src);
// una biblioteca NUEVA en memoria, vacia, para 'o' (todavia sin entrada: el guardado le da una). El nombre
// se hace unico entre los del registro y los cargados. Reemplaza la que tuviera.
bool W3dHierNewLibrary(Object* o, const std::string& name);
// el nombre libre para una biblioteca (unico entre las del registro y las cargadas)
std::string W3dHierFreeLibraryName(const std::string& base);
// el recurso de la biblioteca 'nombre' si esta CARGADA (NULL = no; no la carga)
W3dResource* W3dHierLoadedLibrary(const std::string& name);
// ---- los clips ----
// el clip 'nombre' de la biblioteca de 'o' (NULL = no esta)
W3dClipHier* W3dHierClip(const Object* o, const std::string& name);
// un clip NUEVO (vacio, 1..250 a 30 fps, con el tamano de la jerarquia de 'o') en la biblioteca de 'o'
// (si no tiene, se le crea una con su nombre). Devuelve el clip (NULL si no se pudo).
W3dClipHier* W3dHierNewClip(Object* o, const std::string& name);
// saca el clip de la biblioteca de 'o' y lo libera (lo dejan de ver TODOS sus usuarios). false = no es de ella
bool W3dHierClipDelete(Object* o, W3dClipHier* c);
// UNDO (el editor): SACA el clip de la biblioteca 'r' SIN liberarlo (el paso de undo se lo queda). Devuelve la
// posicion que tenia (-1 = no estaba). Sus vistas y los reproductores que lo nombran lo dejan de ver (validan
// con W3dHierClipLive), igual que si se hubiera borrado.
int  W3dHierClipRemove(W3dResource* r, W3dClipHier* c);
// ...y lo DEVUELVE en la posicion 'pos' (acotada): con el mismo puntero y la misma serie, sus vistas y los que
// lo nombran lo vuelven a ver. false = 'r' no es una biblioteca cargada (o el clip ya estaba).
bool W3dHierClipReturn(W3dResource* r, W3dClipHier* c, int pos);
// el animset de la biblioteca 'r' si esta LISTA (NULL = no)
W3dAnimSet* W3dHierLibrary(W3dResource* r);
// renombra el clip (unico entre los de su biblioteca). Devuelve el nombre que quedo.
std::string W3dHierClipRename(W3dAnimSet* set, W3dClipHier* c, const std::string& newValue);
// true si 'c' (con esa serie) sigue en la biblioteca 'r' (un puntero guardado se valida con esto)
bool W3dHierClipLive(const W3dResource* r, const W3dClipHier* c, unsigned series);
// el clip de nombre CALIFICADO "Biblioteca/clip" (o suelto: el unico con ese nombre entre las bibliotecas
// cargadas) para quien no lo tiene en la suya; 'rec' recibe su biblioteca (sin tomarle referencia)
W3dClipHier* W3dHierClipFind(const std::string& name, W3dResource** rec);
// ---- las rutas ----
// la RUTA de 'n' relativa a 'raiz' ("." = la raiz; "" = 'n' no cuelga de ella). Ver arriba (sufijos, "#k").
std::string W3dHierPath(const Object* root, const Object* n);
// el nodo de 'raiz' que nombra 'ruta' (NULL = esa jerarquia no lo tiene)
Object* W3dHierNode(Object* root, const std::string& path);
// el TAMANO de la jerarquia de 'raiz' para el retarget: la diagonal de la caja de sus nodos (los origenes
// y las cajas de sus mallas) en el espacio del PADRE de la raiz (incluye su propia escala). Se mide SIN
// rotaciones: el tamano no cambia porque un clip le haya girado un nodo (da lo mismo en reposo o a mitad).
float W3dHierSize(Object* root);
// ---- las capas (el Mix de clips de jerarquia; ver W3dHierRoot::capas) ----
// avisar que las capas de 'o' cambiaron (se agrego, se saco o se vacio alguna): el Mix de objetos
// (W3dMixScenesApply/Tick, animation/Animation.cpp) lleva la lista de las raices con capas. La llaman el
// lector del .w3d, lua (objetoCapa...) y el Mix del editor.
void W3dHierLayersChanged(Object* o);
// las raices de la escena ACTIVA que tienen alguna capa (el Play del editor las guarda y las repone)
void W3dHierLayersRoots(std::vector<Object*>& out);
// el clip de la capa 'c' de la raiz 'o' (NULL = no esta): el de su biblioteca con ese nombre o, si no lo tiene,
// por nombre calificado "Biblioteca/clip" (o suelto, si es el unico) entre las bibliotecas CARGADAS. Una capa
// no carga ni retiene bibliotecas: la ajena tiene que estar en uso por algun objeto del proyecto.
// 'rec' (opcional) recibe su biblioteca, SIN tomarle referencia.
const W3dClipHier* W3dHierLayerClip(const Object* o, const W3dLayerAnim& c, W3dResource** rec);

// ---------------------------------------------------------------------------
//  MEDICION (meminfo / harness)
// ---------------------------------------------------------------------------
struct W3dAnimSetsStats {
    int  live;            // animsets cargados (recursos LISTO)
    int  refs;            // armatures que los usan (suma de refs)
    int  clips;           // clips canonicos en memoria
    int  variants;        // variantes (mapeos distintos)
    int  clipsVariant;    // clips materializados en variantes
    long keys;            // keyframes en memoria (canonicos + variantes)
    long bytes;           // memoria aproximada de todo eso
    int  clipsHier;       // clips de jerarquia en memoria (UNA copia por clip, la usen cuantos la usen)
    long keysHier;        // sus keyframes
    W3dAnimSetsStats() : live(0), refs(0), clips(0), variants(0), clipsVariant(0), keys(0), bytes(0),
                         clipsHier(0), keysHier(0) {}
};
void W3dAnimSetsGetStats(W3dAnimSetsStats& st);
// cuantas veces un armature paso a clips propios (copy-on-write) desde el arranque
int  W3dAnimSetsCopies();
// memoria aproximada de un clip (keyframes + vectores)
long W3dAnimSetClipBytes(const SkeletalAnimation* c, long* keys);

#endif // W3D_ANIMSET_H
