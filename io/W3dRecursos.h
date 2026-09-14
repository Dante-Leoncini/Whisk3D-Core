#ifndef W3D_RECURSOS_H
#define W3D_RECURSOS_H
// ============================================================================
//  W3dRecursos — el ALMACEN DE RECURSOS del motor (C++03, sin plataforma).
//
//  QUE ES: todo dato pesado e inmutable (textura, datos de malla, datos de
//  animacion, lista de visibilidad, ...) identificado por su NOMBRE DE ENTRADA
//  del contenedor v4 (la misma identidad que ya usa el cache de texturas por
//  ruta: NO hay GUIDs). Por cada uno vive UN descriptor con tipo, estado,
//  refcounts y pagina. El almacen es la unica contabilidad: quien tiene el
//  dato, cuantos lo comparten, y cuando se libera DE VERDAD (refcount puro,
//  liberacion inmediata al llegar a 0 — SIN LRU, sin GC, sin escaneo por
//  frame: el costo en regimen es CERO).
//
//  DOS CLASES DE REFERENCIA (el modelo de streaming por listas de carga):
//    PERMANENTE : la tiene un dueno concreto (un material, un nodo, el nivel).
//                 Se suelta explicitamente.
//    TRANSITORIA: la puso una fila de ListaCarga al cruzar un nodo del riel.
//                 PurgarTransitorias() las suelta EN BLOQUE (cambio de curva).
//  refTotal cuenta TODAS; refTransitoria solo las transitorias. Un recurso
//  "residente" es EMERGENTE: nunca cae a 0 porque siempre hay una fila que lo
//  referencia — no existe un flag "residente".
//
//  DOS MODOS DE ADQUISICION:
//    BLOQUEANTE : carga ahora mismo (el unico hipo permitido: listas de
//                 visibilidad al cambiar de curva, y el editor).
//    ASYNC      : queda EN_VUELO en una cola; W3dRecursosPump() (UNA vez por
//                 frame) carga de a tajadas. Resolver() devuelve NULL hasta
//                 LISTO: el que llama difiere su spawn 1 frame y reintenta,
//                 sin placeholder.
//
//  CADA TIPO REGISTRA SUS OPS (cargar/liberar): el almacen no sabe abrir un
//  PNG ni parsear un OBJ; sabe CONTAR. TexturaCache es la primera fachada
//  (mismos call sites de siempre); MallaDatos/AnimDatos siguen el mismo camino.
//
//  REGLA DEL MOTOR GENERICO: aca no hay nombres de ningun juego. Los ids son
//  rutas/entradas del contenedor; que signifiquen "el atlas del nivel 3" es
//  problema del proyecto.
// ============================================================================
#include <string>
#include <vector>

// ---- tipos de recurso del motor (un proyecto NO agrega tipos: agrega ids) ----
enum W3dRecTipo {
    W3DREC_TEXTURA   = 0,   // Texture* (fachada TexturaCache)
    W3DREC_MALLA     = 1,   // MallaDatos* (geometria compartida entre Mesh)
    W3DREC_ANIM      = 2,   // AnimDatos*  (cuadros de vertex-anim compartidos)
    W3DREC_LISTA_VIS = 3,   // VisSet* (la lista de visibilidad ES un recurso)
    W3DREC_AUDIO     = 4,
    W3DREC_OTRO      = 5,
    W3DREC_TIPOS     = 6    // tope (dimensiona las tablas internas)
};

// ---- estados (la maquina del descriptor de 44 B del original, con nombres
//      del motor). FIXUP existe para los bundles del juego compilado: dato en
//      memoria que solo necesita re-basear punteros, no parsear. ----
enum W3dRecEstado {
    W3DREC_NO_CARGADO = 0,
    W3DREC_EN_VUELO   = 1,
    W3DREC_FIXUP      = 2,
    W3DREC_LISTO      = 3,
    W3DREC_FALLO      = 4   // fallo recordado: no se reintenta 300 veces/frame
};

enum W3dRecClase { W3DREC_TRANSITORIA = 0, W3DREC_PERMANENTE = 1 };
enum W3dRecModo  { W3DREC_BLOQUEANTE  = 0, W3DREC_ASYNC      = 1 };

// ---- el descriptor. En el juego compilado esto es lo unico que crece con el
//      catalogo (objetivo < 20 KB para cientos de recursos). ----
struct W3dRecurso {
    std::string   id;         // nombre de entrada del contenedor v4 (o ruta)
    unsigned char tipo;       // W3dRecTipo
    unsigned char estado;     // W3dRecEstado
    short refTotal;           // TODAS las referencias vivas
    short refTransitoria;     // solo las de ListaCarga (subconjunto de refTotal)
    short pagina;             // primera pagina del pool que lo aloja (-1 = sin pagina)
    short paginas;            // cuantas paginas ocupa (0 = sin pagina / empaquetado en 1)
    long  bytes;              // medida del dato (lo reporta el op de carga)
    void* dato;               // el payload (Texture*, MallaDatos*, ...)
    std::string ultimoQuien;  // ultimo que lo adquirio (volcado de fugas)

    W3dRecurso() : tipo(0), estado(W3DREC_NO_CARGADO),
                   refTotal(0), refTransitoria(0), pagina(-1), paginas(0),
                   bytes(0), dato(0) {}
};

// ---- ops por tipo: el almacen cuenta, el tipo sabe cargar/liberar ----
struct W3dRecursoOps {
    // carga bloqueante del dato. NULL = fallo (queda FALLO recordado).
    // 'bytes' (opcional) = medida para meminfo.
    void* (*Cargar)(const std::string& id, long* bytes);
    // libera de verdad. false = el dato esta FIJADO (p.ej. texturas de la UI):
    // el descriptor queda vivo con ref 0, igual que hoy.
    bool  (*Liberar)(void* dato);
    // OPCIONAL: tamano del dato SIN cargarlo (streaming: la bomba reserva la
    // pagina ANTES de cargar; sin esto la carga async se paga primero y la
    // pagina se asigna despues, con overflow contado si no entra).
    long  (*Tam)(const std::string& id);
    W3dRecursoOps() : Cargar(0), Liberar(0), Tam(0) {}
};

void W3dRecursosRegistrarTipo(int tipo, const W3dRecursoOps& ops);

// ---- el API del almacen (los nombres del diseno) ----
// la que ya este registrada con ese id, sin cargar ni contar (NULL si no hay)
W3dRecurso* W3dRecursoBuscar(int tipo, const std::string& id);
// +1 ref. Carga si hace falta (segun modo). Devuelve el descriptor SIEMPRE que
// el id sea valido (aunque este EN_VUELO o FALLO): el dato se pide con
// Resolver. 'quien' es para el volcado de fugas, puede ser "".
W3dRecurso* W3dRecursoAdquirir(int tipo, const std::string& id,
                               int clase, int modo, const char* quien);
// +1 ref sobre un descriptor que ya se tiene (copiar un material, clonar)
void        W3dRecursoRetener(W3dRecurso* r, int clase);
// -1 ref. Al llegar refTotal a 0 libera DE VERDAD (op Liberar + descriptor).
void        W3dRecursoSoltar(W3dRecurso* r, int clase);
// el dato, o NULL si no esta LISTO (EN_VUELO/FALLO -> reintentar otro frame)
void*       W3dRecursoResolver(W3dRecurso* r);
// suelta EN BLOQUE todas las refs transitorias (cambio de curva). Devuelve
// cuantos recursos se liberaron de verdad.
int         W3dRecursosPurgarTransitorias();
// la bomba de carga asincronica: UNA llamada por frame, carga de a uno.
void        W3dRecursosPump();
// borra los descriptores de un tipo SIN llamar a Liberar: es el camino del
// cierre de escena, donde el cliente ya destruyo los payloads por su cuenta
// (TexturasLiberarEscena barre el vector global ANTES de esto).
void        W3dRecursosOlvidarTipo(int tipo);

// ---- medicion (meminfo / texinfo) ----
int  W3dRecursosVivos(int tipo);                 // descriptores con dato LISTO
long W3dRecursosBytes(int tipo);                 // bytes de los LISTOS
int  W3dRecursosCargas(int tipo);                // cargas de verdad (disco)
int  W3dRecursosReusos(int tipo);                // pedidos resueltos sin cargar
long W3dRecursosBytesCargados(int tipo);         // bytes ACUMULADOS cargados
void W3dRecursosStatsReset(int tipo);            // -1 = todos los tipos
// el camino legacy A/B (cache apagado) carga por afuera pero CUENTA igual:
void W3dRecursosContarCargaExterna(int tipo, long bytes);
// registra un FALLO sin intentar la carga (memoria de fallos del camino legacy)
W3dRecurso* W3dRecursoRegistrarFallo(int tipo, const std::string& id);
// registra un dato cargado POR AFUERA como "la copia viva de este id" (pisa la
// anterior si habia: el mapa recuerda la ULTIMA, igual que el cache de antes).
// Deja refTotal=1. No cuenta stats (eso es ContarCargaExterna).
// cambia la CLAVE de un recurso registrado (misma ficha). false si 'viejo' no existe o 'nuevo' ya esta.
bool W3dRecursoRenombrar(int tipo, const std::string& viejo, const std::string& nuevo);
W3dRecurso* W3dRecursoRegistrarExterno(int tipo, const std::string& id,
                                       void* dato, long bytes);
// volcado: punteros a TODOS los descriptores del tipo (para listar/asertar)
void W3dRecursosListar(int tipo, std::vector<W3dRecurso*>& out);

// ============================================================================
//  POOL DE PAGINAS (streaming): memoria de datos streameados en PAGINAS FIJAS
//  (64-256 KB segun plataforma). NADA de LRU: una pagina se libera cuando su
//  contador de recursos llega a 0. Los recursos chicos se EMPAQUETAN en la
//  pagina abierta (por eso el empaquetado por co-uso importa: la pagina vive
//  hasta que TODOS sus recursos mueren); los grandes toman un run contiguo de
//  paginas enteras. Sin pool configurado (el editor) nada de esto corre.
//
//  Politica de falta de lugar:
//    BLOQUEANTE -> se carga igual SIN pagina y se cuenta el overflow (el hipo
//                  critico no puede fallar; el gate de build es quien prohibe
//                  que esto pase en un juego compilado).
//    ASYNC      -> el recurso se queda EN_VUELO y la bomba reintenta: el
//                  faltante sostenido (Resolver == NULL) es la senal medible
//                  de que el pool quedo chico.
// ============================================================================
void W3dRecursosPoolConfigurar(int nPaginas, long tamPagina);  // 0,0 = sin pool
int  W3dRecursosPoolPaginas();     // total configurado
int  W3dRecursosPoolVivas();       // paginas con recursos adentro
int  W3dRecursosPoolPico();        // maximo de vivas visto desde la config
long W3dRecursosPoolBytes();       // bytes ocupados en el pool
int  W3dRecursosPoolOverflows();   // cargas que no entraron (bloqueantes sin pagina)

// ============================================================================
//  LISTAS DE CARGA (streaming por rieles): el modelo del original. Una curva
//  registra DOS listas de filas (una por direccion); cada fila es el conjunto
//  de recursos que el nodo necesita adquirir al cruzarlo en esa direccion.
//
//  Reglas EXACTAS:
//   * Entrar a la curva por una punta = PurgarTransitorias() EN BLOQUE +
//     adquirir BLOQUEANTE la fila de la punta (fila 0 de "adelante" o la
//     ultima de "atras" = el set completo de entrada, horneado asi).
//   * Cruzar el nodo m en direccion D = adquirir ASYNC la fila m de D y
//     SOLTAR la fila m-1 (D=adelante) / m+1 (D=atras) de la lista OPUESTA.
//     Los nodos se cruzan DE A UNO.
//   * Todas las referencias son TRANSITORIAS: lo residente es EMERGENTE (un
//     recurso que figura en todas las filas nunca cae a 0). No hay flags.
//
//  El dato viene del sidecar `<riel>.cargas.json` (formato/cargas-json.md);
//  el parseo JSON es del EDITOR (Curve.cpp): aca entran filas ya resueltas.
// ============================================================================
struct W3dCargasItem { int tipo; std::string id; };
typedef std::vector<W3dCargasItem> W3dCargasFila;

// registra las dos listas; devuelve un handle (>= 0). nombre = para volcados.
int  W3dCargasRegistrar(const std::string& nombre,
                        const std::vector<W3dCargasFila>& adelante,
                        const std::vector<W3dCargasFila>& atras);
void W3dCargasOlvidarTodas();                       // cierre de escena
int  W3dCargasFilas(int handle, bool adelante);     // filas de una direccion (-1 = handle invalido)
bool W3dCargasEntrar(int handle, bool adelante);    // purga + fila punta bloqueante
bool W3dCargasCruzarNodo(int handle, int nodo, bool adelante);

#endif
