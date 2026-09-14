// ============================================================================
//  Whisk3DCore (engine) — ALMACEN DE RECURSOS con refcount puro (ver .h)
//
//  El modelo es el del streaming clasico de consola: un descriptor por
//  recurso, dos clases de referencia (permanente/transitoria), liberacion
//  INMEDIATA al llegar a 0 y una bomba asincronica que avanza UNA carga por
//  frame. Nada se escanea por frame: en regimen el costo es cero.
//
//  C++03, sin dependencias de plataforma: compila en los 4 OS. La carga real
//  la hacen los ops registrados por tipo (TexturaCache registra el suyo).
// ============================================================================
#include "W3dRecursos.h"
#include <map>

// una tabla id->descriptor POR TIPO (los namespaces no se pisan: una textura
// y una malla pueden tener el mismo id sin conflicto)
typedef std::map<std::string, W3dRecurso*> MapaRec;
static MapaRec        gPorId[W3DREC_TIPOS];
static W3dRecursoOps  gOps[W3DREC_TIPOS];

// cola FIFO de EN_VUELO (modo ASYNC): Pump() carga de a uno por frame
static std::vector<W3dRecurso*> gEnVuelo;

// estadistica por tipo (la fachada de texturas la expone como texinfo)
static int  gCargas[W3DREC_TIPOS];
static int  gReusos[W3DREC_TIPOS];
static long gBytesCargados[W3DREC_TIPOS];

static bool TipoValido(int tipo) { return tipo >= 0 && tipo < W3DREC_TIPOS; }

// ---------------------------------------------------------------------------
//  POOL DE PAGINAS (ver .h). gPagAbierta = la pagina donde se empaquetan los
//  recursos chicos (co-uso); los grandes toman un run contiguo de paginas
//  libres enteras. Todo se libera por refcount de pagina: recursos==0 -> libre.
// ---------------------------------------------------------------------------
struct PaginaPool { long usados; int recursos; PaginaPool() : usados(0), recursos(0) {} };
static std::vector<PaginaPool> gPool;
static long gTamPagina    = 0;
static int  gPagAbierta   = -1;   // indice de la pagina de paquetes chicos (-1 = ninguna)
static int  gPoolOverflow = 0;
static int  gPoolPico     = 0;

static bool PoolActivo() { return gTamPagina > 0 && !gPool.empty(); }

void W3dRecursosPoolConfigurar(int nPaginas, long tamPagina) {
    gPool.clear();
    if (nPaginas > 0 && tamPagina > 0) gPool.resize((size_t)nPaginas);
    gTamPagina = (nPaginas > 0) ? tamPagina : 0;
    gPagAbierta = -1; gPoolOverflow = 0; gPoolPico = 0;
}
int  W3dRecursosPoolPaginas()   { return (int)gPool.size(); }
int  W3dRecursosPoolVivas() {
    int n = 0;
    for (size_t i = 0; i < gPool.size(); i++) if (gPool[i].recursos > 0) n++;
    return n;
}
int  W3dRecursosPoolPico()      { return gPoolPico; }
long W3dRecursosPoolBytes() {
    long b = 0;
    for (size_t i = 0; i < gPool.size(); i++) b += gPool[i].usados;
    return b;
}
int  W3dRecursosPoolOverflows() { return gPoolOverflow; }

static void PoolActualizarPico() {
    const int v = W3dRecursosPoolVivas();
    if (v > gPoolPico) gPoolPico = v;
}

// hay lugar para 'bytes' SIN tocar nada? (la bomba pregunta antes de cargar)
static bool PoolCabe(long bytes) {
    if (!PoolActivo()) return true;           // sin pool no hay limite
    if (bytes <= 0) return true;
    if (bytes <= gTamPagina) {
        if (gPagAbierta >= 0 && gPool[(size_t)gPagAbierta].usados + bytes <= gTamPagina) return true;
        for (size_t i = 0; i < gPool.size(); i++) if (gPool[i].recursos == 0) return true;
        return false;
    }
    const int nec = (int)((bytes + gTamPagina - 1) / gTamPagina);
    int corrida = 0;
    for (size_t i = 0; i < gPool.size(); i++) {
        corrida = (gPool[i].recursos == 0) ? corrida + 1 : 0;
        if (corrida >= nec) return true;
    }
    return false;
}

// asigna pagina(s) a un recurso ya LISTO. false = no entro (overflow).
static bool PoolAsignar(W3dRecurso* r) {
    if (!PoolActivo()) return true;
    const long bytes = (r->bytes > 0) ? r->bytes : 1;
    if (bytes <= gTamPagina) {
        // empaquetar en la pagina abierta si cabe; sino abrir una libre
        if (gPagAbierta < 0 || gPool[(size_t)gPagAbierta].usados + bytes > gTamPagina) {
            int libre = -1;
            for (size_t i = 0; i < gPool.size(); i++)
                if (gPool[i].recursos == 0) { libre = (int)i; break; }
            if (libre < 0) return false;
            gPagAbierta = libre;
        }
        PaginaPool& pg = gPool[(size_t)gPagAbierta];
        pg.usados += bytes; pg.recursos++;
        r->pagina = (short)gPagAbierta; r->paginas = 1;
        PoolActualizarPico();
        return true;
    }
    // grande: run contiguo de paginas libres enteras
    const int nec = (int)((bytes + gTamPagina - 1) / gTamPagina);
    int corrida = 0, ini = -1;
    for (size_t i = 0; i < gPool.size(); i++) {
        corrida = (gPool[i].recursos == 0) ? corrida + 1 : 0;
        if (corrida >= nec) { ini = (int)i - nec + 1; break; }
    }
    if (ini < 0) return false;
    long resto = bytes;
    for (int i = 0; i < nec; i++) {
        PaginaPool& pg = gPool[(size_t)(ini + i)];
        pg.recursos = 1;
        pg.usados = (resto > gTamPagina) ? gTamPagina : resto;
        resto -= pg.usados;
    }
    r->pagina = (short)ini; r->paginas = (short)nec;
    PoolActualizarPico();
    return true;
}

static void PoolSoltar(W3dRecurso* r) {
    if (r->pagina < 0 || gPool.empty()) { r->pagina = -1; r->paginas = 0; return; }
    const long bytes = (r->bytes > 0) ? r->bytes : 1;
    const int n = (r->paginas > 0) ? r->paginas : 1;
    long resto = bytes;
    for (int i = 0; i < n && (size_t)(r->pagina + i) < gPool.size(); i++) {
        PaginaPool& pg = gPool[(size_t)(r->pagina + i)];
        const long parte = (n == 1) ? bytes : ((resto > gTamPagina) ? gTamPagina : resto);
        resto -= parte;
        pg.usados -= parte; if (pg.usados < 0) pg.usados = 0;
        if (pg.recursos > 0) pg.recursos--;
        if (pg.recursos == 0) {
            pg.usados = 0;
            if (gPagAbierta == r->pagina + i) gPagAbierta = -1;
        }
    }
    r->pagina = -1; r->paginas = 0;
}

void W3dRecursosRegistrarTipo(int tipo, const W3dRecursoOps& ops) {
    if (TipoValido(tipo)) gOps[tipo] = ops;
}

W3dRecurso* W3dRecursoBuscar(int tipo, const std::string& id) {
    if (!TipoValido(tipo) || id.empty()) return 0;
    MapaRec::iterator it = gPorId[tipo].find(id);
    return (it == gPorId[tipo].end()) ? 0 : it->second;
}

// carga BLOQUEANTE de un descriptor NO_CARGADO/EN_VUELO. Deja LISTO o FALLO.
static void CargarAhora(W3dRecurso* r) {
    if (!gOps[r->tipo].Cargar) { r->estado = W3DREC_FALLO; return; }
    long b = 0;
    void* dato = gOps[r->tipo].Cargar(r->id, &b);
    if (!dato) { r->estado = W3DREC_FALLO; r->dato = 0; return; }
    r->dato = dato; r->bytes = b; r->estado = W3DREC_LISTO;
    gCargas[r->tipo]++; gBytesCargados[r->tipo] += b;
    // pool: el dato streameado vive en paginas. Si no entra, la carga (que era
    // critica/bloqueante) NO falla: queda sin pagina y se cuenta el overflow.
    if (!PoolAsignar(r)) gPoolOverflow++;
}

W3dRecurso* W3dRecursoAdquirir(int tipo, const std::string& id,
                               int clase, int modo, const char* quien) {
    if (!TipoValido(tipo) || id.empty()) return 0;
    W3dRecurso* r = W3dRecursoBuscar(tipo, id);
    if (r) {
        if (r->estado == W3DREC_LISTO) gReusos[tipo]++;   // pedido sin carga
        // FALLO recordado: se devuelve el descriptor (Resolver dara NULL) y NO
        // se reintenta la carga ni se cuenta nada. EN_VUELO: la ref se suma ya
        // (la carga esta en camino).
        if (r->estado == W3DREC_FALLO) return r;
    } else {
        r = new W3dRecurso();
        r->id = id; r->tipo = (unsigned char)tipo;
        gPorId[tipo][id] = r;
        if (modo == W3DREC_ASYNC) {
            r->estado = W3DREC_EN_VUELO;
            gEnVuelo.push_back(r);
        } else {
            CargarAhora(r);
            if (r->estado == W3DREC_FALLO) return r;   // fallo: ref no suma
        }
    }
    r->refTotal++;
    if (clase == W3DREC_TRANSITORIA) r->refTransitoria++;
    if (quien && *quien) r->ultimoQuien = quien;
    return r;
}

void W3dRecursoRetener(W3dRecurso* r, int clase) {
    if (!r) return;
    r->refTotal++;
    if (clase == W3DREC_TRANSITORIA) r->refTransitoria++;
}

// liberar de verdad: op Liberar + borrar el descriptor. Si el op dice false
// (dato FIJADO, p.ej. texturas de la UI), el descriptor queda vivo con ref 0.
static void LiberarRecurso(W3dRecurso* r) {
    if (r->estado == W3DREC_LISTO && r->dato) {
        if (gOps[r->tipo].Liberar && !gOps[r->tipo].Liberar(r->dato))
            return;                        // fijado: vive con ref 0
        PoolSoltar(r);                     // la(s) pagina(s) quedan libres al llegar a 0
    }
    if (r->estado == W3DREC_EN_VUELO) {    // estaba en la cola: sacarlo
        for (size_t i = 0; i < gEnVuelo.size(); i++)
            if (gEnVuelo[i] == r) { gEnVuelo.erase(gEnVuelo.begin() + (long)i); break; }
    }
    MapaRec::iterator it = gPorId[r->tipo].find(r->id);
    if (it != gPorId[r->tipo].end() && it->second == r) gPorId[r->tipo].erase(it);
    delete r;
}

void W3dRecursoSoltar(W3dRecurso* r, int clase) {
    if (!r) return;
    if (clase == W3DREC_TRANSITORIA && r->refTransitoria > 0) r->refTransitoria--;
    if (r->refTotal > 0) r->refTotal--;
    if (r->refTotal > 0) return;
    LiberarRecurso(r);
}

void* W3dRecursoResolver(W3dRecurso* r) {
    if (!r || r->estado != W3DREC_LISTO) return 0;
    return r->dato;
}

int W3dRecursosPurgarTransitorias() {
    int liberados = 0;
    for (int t = 0; t < W3DREC_TIPOS; t++) {
        MapaRec::iterator it = gPorId[t].begin();
        while (it != gPorId[t].end()) {
            W3dRecurso* r = it->second;
            ++it;                          // antes de un posible erase
            if (r->refTransitoria <= 0) continue;
            r->refTotal = (short)(r->refTotal - r->refTransitoria);
            r->refTransitoria = 0;
            if (r->refTotal <= 0) { r->refTotal = 0; LiberarRecurso(r); liberados++; }
        }
    }
    return liberados;
}

void W3dRecursosPump() {
    // UNA carga por llamada (que es una por frame): el hipo maximo es un
    // recurso, nunca la cola entera.
    if (gEnVuelo.empty()) return;
    W3dRecurso* r = gEnVuelo.front();
    gEnVuelo.erase(gEnVuelo.begin());
    if (r->estado != W3DREC_EN_VUELO) return;
    // pool lleno y el tipo sabe su tamano: reintentar mas adelante (la senal
    // medible del pool chico es el faltante sostenido, no un crash)
    if (PoolActivo() && gOps[r->tipo].Tam) {
        const long t = gOps[r->tipo].Tam(r->id);
        if (t > 0 && !PoolCabe(t)) { gEnVuelo.push_back(r); return; }
    }
    CargarAhora(r);
}

void W3dRecursosOlvidarTipo(int tipo) {
    if (!TipoValido(tipo)) return;
    // los payloads ya los destruyo el cliente (cierre de escena): aca solo
    // muere la contabilidad, sin tocar ops.
    for (MapaRec::iterator it = gPorId[tipo].begin(); it != gPorId[tipo].end(); ++it) {
        // sacarlo de la cola en vuelo si estaba
        for (size_t i = 0; i < gEnVuelo.size(); i++)
            if (gEnVuelo[i] == it->second) { gEnVuelo.erase(gEnVuelo.begin() + (long)i); break; }
        PoolSoltar(it->second);   // sus paginas quedan libres
        delete it->second;
    }
    gPorId[tipo].clear();
}

// ---------------------------------------------------------------------------
//  MEDICION
// ---------------------------------------------------------------------------
int W3dRecursosVivos(int tipo) {
    if (!TipoValido(tipo)) return 0;
    int n = 0;
    for (MapaRec::iterator it = gPorId[tipo].begin(); it != gPorId[tipo].end(); ++it)
        if (it->second->estado == W3DREC_LISTO) n++;
    return n;
}
long W3dRecursosBytes(int tipo) {
    if (!TipoValido(tipo)) return 0;
    long b = 0;
    for (MapaRec::iterator it = gPorId[tipo].begin(); it != gPorId[tipo].end(); ++it)
        if (it->second->estado == W3DREC_LISTO) b += it->second->bytes;
    return b;
}
int  W3dRecursosCargas(int tipo)        { return TipoValido(tipo) ? gCargas[tipo] : 0; }
int  W3dRecursosReusos(int tipo)        { return TipoValido(tipo) ? gReusos[tipo] : 0; }
long W3dRecursosBytesCargados(int tipo) { return TipoValido(tipo) ? gBytesCargados[tipo] : 0; }

void W3dRecursosStatsReset(int tipo) {
    for (int t = 0; t < W3DREC_TIPOS; t++) {
        if (tipo >= 0 && t != tipo) continue;
        gCargas[t] = 0; gReusos[t] = 0; gBytesCargados[t] = 0;
    }
}
void W3dRecursosContarCargaExterna(int tipo, long bytes) {
    if (!TipoValido(tipo)) return;
    gCargas[tipo]++; gBytesCargados[tipo] += bytes;
}
W3dRecurso* W3dRecursoRegistrarFallo(int tipo, const std::string& id) {
    if (!TipoValido(tipo) || id.empty()) return 0;
    W3dRecurso* r = W3dRecursoBuscar(tipo, id);
    if (!r) {
        r = new W3dRecurso();
        r->id = id; r->tipo = (unsigned char)tipo;
        gPorId[tipo][id] = r;
    }
    if (r->estado == W3DREC_NO_CARGADO) r->estado = W3DREC_FALLO;
    return r;
}
// cambia el ID de un recurso ya registrado (una textura que pasa de interna a externa o al reves):
// la misma ficha, otra clave. false si no existe el viejo o ya existe el nuevo.
bool W3dRecursoRenombrar(int tipo, const std::string& viejo, const std::string& nuevo) {
    if (!TipoValido(tipo) || viejo.empty() || nuevo.empty() || viejo == nuevo) return false;
    MapaRec& m = gPorId[tipo];
    MapaRec::iterator it = m.find(viejo);
    if (it == m.end() || m.find(nuevo) != m.end()) return false;
    W3dRecurso* rr = it->second;
    m.erase(it);
    rr->id = nuevo;
    m[nuevo] = rr;
    return true;
}
W3dRecurso* W3dRecursoRegistrarExterno(int tipo, const std::string& id,
                                       void* dato, long bytes) {
    if (!TipoValido(tipo) || id.empty() || !dato) return 0;
    W3dRecurso* r = W3dRecursoBuscar(tipo, id);
    if (!r) {
        r = new W3dRecurso();
        r->id = id; r->tipo = (unsigned char)tipo;
        gPorId[tipo][id] = r;
    }
    // pisa la copia anterior SIN liberarla: la vieja sigue viva por su cuenta
    // (refcount local del cliente), exactamente como el mapa de antes.
    r->dato = dato; r->bytes = bytes;
    r->estado = W3DREC_LISTO;
    r->refTotal = 1; r->refTransitoria = 0;
    return r;
}
void W3dRecursosListar(int tipo, std::vector<W3dRecurso*>& out) {
    out.clear();
    if (!TipoValido(tipo)) return;
    for (MapaRec::iterator it = gPorId[tipo].begin(); it != gPorId[tipo].end(); ++it)
        out.push_back(it->second);
}

// ---------------------------------------------------------------------------
//  LISTAS DE CARGA (ver .h). El registro vive aca; el parseo del sidecar
//  `.cargas.json` es del editor (Curve.cpp) o de quien traiga las filas.
// ---------------------------------------------------------------------------
struct CargasCurva {
    std::string nombre;
    std::vector<W3dCargasFila> adelante, atras;
};
static std::vector<CargasCurva*> gListasCarga;   // handle = indice (NULL = liberado)

int W3dCargasRegistrar(const std::string& nombre,
                       const std::vector<W3dCargasFila>& adelante,
                       const std::vector<W3dCargasFila>& atras) {
    CargasCurva* c = new CargasCurva();
    c->nombre = nombre; c->adelante = adelante; c->atras = atras;
    gListasCarga.push_back(c);
    return (int)gListasCarga.size() - 1;
}
void W3dCargasOlvidarTodas() {
    for (size_t i = 0; i < gListasCarga.size(); i++) delete gListasCarga[i];
    gListasCarga.clear();
}
static CargasCurva* CargasDe(int handle) {
    if (handle < 0 || handle >= (int)gListasCarga.size()) return 0;
    return gListasCarga[(size_t)handle];
}
int W3dCargasFilas(int handle, bool adelante) {
    CargasCurva* c = CargasDe(handle);
    if (!c) return -1;
    return (int)(adelante ? c->adelante.size() : c->atras.size());
}

static void CargasAdquirirFila(const W3dCargasFila& fila, int modo) {
    for (size_t i = 0; i < fila.size(); i++)
        W3dRecursoAdquirir(fila[i].tipo, fila[i].id,
                           W3DREC_TRANSITORIA, modo, "listacarga");
}
static void CargasSoltarFila(const W3dCargasFila& fila) {
    for (size_t i = 0; i < fila.size(); i++) {
        W3dRecurso* r = W3dRecursoBuscar(fila[i].tipo, fila[i].id);
        if (r && r->refTransitoria > 0) W3dRecursoSoltar(r, W3DREC_TRANSITORIA);
    }
}

bool W3dCargasEntrar(int handle, bool adelante) {
    CargasCurva* c = CargasDe(handle);
    if (!c) return false;
    // cambio de curva: TODO lo transitorio se va en bloque, y la fila de la
    // punta de entrada se adquiere BLOQUEANTE (el unico hipo permitido)
    W3dRecursosPurgarTransitorias();
    const std::vector<W3dCargasFila>& L = adelante ? c->adelante : c->atras;
    if (L.empty()) return true;
    CargasAdquirirFila(adelante ? L.front() : L.back(), W3DREC_BLOQUEANTE);
    return true;
}

bool W3dCargasCruzarNodo(int handle, int nodo, bool adelante) {
    CargasCurva* c = CargasDe(handle);
    if (!c) return false;
    const std::vector<W3dCargasFila>& mia = adelante ? c->adelante : c->atras;
    const std::vector<W3dCargasFila>& op  = adelante ? c->atras : c->adelante;
    if (nodo >= 0 && nodo < (int)mia.size())
        CargasAdquirirFila(mia[(size_t)nodo], W3DREC_ASYNC);
    const int suelta = adelante ? nodo - 1 : nodo + 1;
    if (suelta >= 0 && suelta < (int)op.size())
        CargasSoltarFila(op[(size_t)suelta]);
    return true;
}
