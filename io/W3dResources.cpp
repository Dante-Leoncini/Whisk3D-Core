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
#include "W3dResources.h"
#include <map>
#include <set>

// una tabla id->descriptor POR TIPO (los namespaces no se pisan: una textura
// y una malla pueden tener el mismo id sin conflicto)
typedef std::map<std::string, W3dResource*> MapRec;
static MapRec        gById[W3DREC_TYPES];
static W3dResourceOps  gOps[W3DREC_TYPES];

// cola FIFO de EN_VUELO (modo ASYNC): Pump() carga de a uno por frame
static std::vector<W3dResource*> gInFlight;

// estadistica por tipo (la fachada de texturas la expone como texinfo)
static int  gLoads[W3DREC_TYPES];
static int  gReuses[W3DREC_TYPES];
static long gLoadedBytes[W3DREC_TYPES];

static bool TypeValid(int type) { return type >= 0 && type < W3DREC_TYPES; }

// los descriptores VIVOS (W3dResourceCurrent): todo descriptor nace y muere por aca
static std::set<const W3dResource*> gCurrent;
static unsigned gNextSeries = 1;
static W3dResource* NewDescriptor() {
    W3dResource* r = new W3dResource();
    r->series = gNextSeries++;
    gCurrent.insert(r);
    return r;
}
static void DeleteDescriptor(W3dResource* r) {
    gCurrent.erase(r);
    delete r;
}
bool W3dResourceCurrent(const W3dResource* r, unsigned series) {
    return r && gCurrent.count(r) && r->series == series;
}

// ---------------------------------------------------------------------------
//  POOL DE PAGINAS (ver .h). gOpenPage = la pagina donde se empaquetan los
//  recursos chicos (co-uso); los grandes toman un run contiguo de paginas
//  libres enteras. Todo se libera por refcount de pagina: recursos==0 -> libre.
// ---------------------------------------------------------------------------
struct PagePool { long used; int resources; PagePool() : used(0), resources(0) {} };
static std::vector<PagePool> gPool;
static long gPageSize     = 0;
static int  gOpenPage     = -1;   // indice de la pagina de paquetes chicos (-1 = ninguna)
static int  gPoolOverflow = 0;
static int  gPoolPeak     = 0;

static bool PoolActive() { return gPageSize > 0 && !gPool.empty(); }

void W3dResourcesPoolConfigure(int nPages, long pageSize) {
    gPool.clear();
    if (nPages > 0 && pageSize > 0) gPool.resize((size_t)nPages);
    gPageSize = (nPages > 0) ? pageSize : 0;
    gOpenPage = -1; gPoolOverflow = 0; gPoolPeak = 0;
}
int  W3dResourcesPoolPages()   { return (int)gPool.size(); }
int  W3dResourcesLivePool() {
    int n = 0;
    for (size_t i = 0; i < gPool.size(); i++) if (gPool[i].resources > 0) n++;
    return n;
}
int  W3dResourcesPoolPeak()      { return gPoolPeak; }
long W3dResourcesPoolBytes() {
    long b = 0;
    for (size_t i = 0; i < gPool.size(); i++) b += gPool[i].used;
    return b;
}
int  W3dResourcesPoolOverflows() { return gPoolOverflow; }

static void PoolUpdatePeak() {
    const int v = W3dResourcesLivePool();
    if (v > gPoolPeak) gPoolPeak = v;
}

// hay lugar para 'bytes' SIN tocar nada? (la bomba pregunta antes de cargar)
static bool PoolFits(long bytes) {
    if (!PoolActive()) return true;           // sin pool no hay limite
    if (bytes <= 0) return true;
    if (bytes <= gPageSize) {
        if (gOpenPage >= 0 && gPool[(size_t)gOpenPage].used + bytes <= gPageSize) return true;
        for (size_t i = 0; i < gPool.size(); i++) if (gPool[i].resources == 0) return true;
        return false;
    }
    const int nec = (int)((bytes + gPageSize - 1) / gPageSize);
    int run = 0;
    for (size_t i = 0; i < gPool.size(); i++) {
        run = (gPool[i].resources == 0) ? run + 1 : 0;
        if (run >= nec) return true;
    }
    return false;
}

// asigna page(s) a un recurso ya LISTO. false = no entro (overflow).
static bool PoolAssign(W3dResource* r) {
    if (!PoolActive()) return true;
    const long bytes = (r->bytes > 0) ? r->bytes : 1;
    if (bytes <= gPageSize) {
        // empaquetar en la pagina abierta si cabe; sino abrir una libre
        if (gOpenPage < 0 || gPool[(size_t)gOpenPage].used + bytes > gPageSize) {
            int free = -1;
            for (size_t i = 0; i < gPool.size(); i++)
                if (gPool[i].resources == 0) { free = (int)i; break; }
            if (free < 0) return false;
            gOpenPage = free;
        }
        PagePool& pg = gPool[(size_t)gOpenPage];
        pg.used += bytes; pg.resources++;
        r->page = (short)gOpenPage; r->pages = 1;
        PoolUpdatePeak();
        return true;
    }
    // grande: run contiguo de paginas libres enteras
    const int nec = (int)((bytes + gPageSize - 1) / gPageSize);
    int run = 0, start = -1;
    for (size_t i = 0; i < gPool.size(); i++) {
        run = (gPool[i].resources == 0) ? run + 1 : 0;
        if (run >= nec) { start = (int)i - nec + 1; break; }
    }
    if (start < 0) return false;
    long rest = bytes;
    for (int i = 0; i < nec; i++) {
        PagePool& pg = gPool[(size_t)(start + i)];
        pg.resources = 1;
        pg.used = (rest > gPageSize) ? gPageSize : rest;
        rest -= pg.used;
    }
    r->page = (short)start; r->pages = (short)nec;
    PoolUpdatePeak();
    return true;
}

static void PoolRelease(W3dResource* r) {
    if (r->page < 0 || gPool.empty()) { r->page = -1; r->pages = 0; return; }
    const long bytes = (r->bytes > 0) ? r->bytes : 1;
    const int n = (r->pages > 0) ? r->pages : 1;
    long rest = bytes;
    for (int i = 0; i < n && (size_t)(r->page + i) < gPool.size(); i++) {
        PagePool& pg = gPool[(size_t)(r->page + i)];
        const long part = (n == 1) ? bytes : ((rest > gPageSize) ? gPageSize : rest);
        rest -= part;
        pg.used -= part; if (pg.used < 0) pg.used = 0;
        if (pg.resources > 0) pg.resources--;
        if (pg.resources == 0) {
            pg.used = 0;
            if (gOpenPage == r->page + i) gOpenPage = -1;
        }
    }
    r->page = -1; r->pages = 0;
}

void W3dResourcesRegisterType(int type, const W3dResourceOps& ops) {
    if (TypeValid(type)) gOps[type] = ops;
}

W3dResource* W3dResourceFind(int type, const std::string& id) {
    if (!TypeValid(type) || id.empty()) return 0;
    MapRec::iterator it = gById[type].find(id);
    return (it == gById[type].end()) ? 0 : it->second;
}

// carga BLOQUEANTE de un descriptor NO_CARGADO/EN_VUELO. Deja LISTO o FALLO.
static void LoadNow(W3dResource* r) {
    if (!gOps[r->type].Load) { r->state = W3DREC_FAILED; return; }
    long b = 0;
    void* data = gOps[r->type].Load(r->id, &b);
    if (!data) { r->state = W3DREC_FAILED; r->data = 0; return; }
    r->data = data; r->bytes = b; r->state = W3DREC_READY;
    gLoads[r->type]++; gLoadedBytes[r->type] += b;
    // pool: el dato streameado vive en paginas. Si no entra, la carga (que era
    // critica/bloqueante) NO falla: queda sin pagina y se cuenta el overflow.
    if (!PoolAssign(r)) gPoolOverflow++;
}

W3dResource* W3dResourceAcquire(int type, const std::string& id,
                               int cls, int mode, const char* who) {
    if (!TypeValid(type) || id.empty()) return 0;
    W3dResource* r = W3dResourceFind(type, id);
    if (r) {
        if (r->state == W3DREC_READY) gReuses[type]++;   // pedido sin carga
        // FALLO recordado: se devuelve el descriptor (Resolver dara NULL) y NO
        // se reintenta la carga ni se cuenta nada. EN_VUELO: la ref se suma ya
        // (la carga esta en camino).
        if (r->state == W3DREC_FAILED) return r;
        // EN_VUELO pedido BLOQUEANTE: el que llama necesita el dato YA (el contrato del modo).
        // Sin esto se devolvia EN_VUELO con la ref sumada y el que esperaba LISTO fallaba
        // (una malla que se vincula mientras una ListaCarga async todavia no la bombeo).
        // Sale de la cola y se carga ahora; si falla, la ref nueva no suma (como una carga nueva).
        if (r->state == W3DREC_IN_FLIGHT && mode == W3DREC_BLOCKING) {
            for (size_t i = 0; i < gInFlight.size(); i++)
                if (gInFlight[i] == r) { gInFlight.erase(gInFlight.begin() + (long)i); break; }
            LoadNow(r);
            if (r->state == W3DREC_FAILED) return r;
        }
    } else {
        r = NewDescriptor();
        r->id = id; r->type = (unsigned char)type;
        gById[type][id] = r;
        if (mode == W3DREC_ASYNC) {
            r->state = W3DREC_IN_FLIGHT;
            gInFlight.push_back(r);
        } else {
            LoadNow(r);
            if (r->state == W3DREC_FAILED) return r;   // fallo: ref no suma
        }
    }
    r->refTotal++;
    if (cls == W3DREC_TRANSIENT) r->transientRef++;
    if (who && *who) r->lastWho = who;
    return r;
}

void W3dResourceRetain(W3dResource* r, int cls) {
    if (!r) return;
    r->refTotal++;
    if (cls == W3DREC_TRANSIENT) r->transientRef++;
}

// liberar de verdad: op Liberar + borrar el descriptor. Si el op dice false
// (dato FIJADO, p.ej. texturas de la UI), el descriptor queda vivo con ref 0.
static void FreeResource(W3dResource* r) {
    if (r->state == W3DREC_READY && r->data) {
        if (gOps[r->type].Free && !gOps[r->type].Free(r->data))
            return;                         // fijado: vive con ref 0
        PoolRelease(r);                     // la(s) page(s) quedan libres al llegar a 0
    }
    if (r->state == W3DREC_IN_FLIGHT) {    // estaba en la cola: sacarlo
        for (size_t i = 0; i < gInFlight.size(); i++)
            if (gInFlight[i] == r) { gInFlight.erase(gInFlight.begin() + (long)i); break; }
    }
    MapRec::iterator it = gById[r->type].find(r->id);
    if (it != gById[r->type].end() && it->second == r) gById[r->type].erase(it);
    DeleteDescriptor(r);
}

void W3dResourceRelease(W3dResource* r, int cls) {
    if (!r) return;
    if (cls == W3DREC_TRANSIENT && r->transientRef > 0) r->transientRef--;
    if (r->refTotal > 0) r->refTotal--;
    if (r->refTotal > 0) return;
    FreeResource(r);
}

void* W3dResourceResolve(W3dResource* r) {
    if (!r || r->state != W3DREC_READY) return 0;
    return r->data;
}

int W3dResourcesPurgeTransient() {
    int freed = 0;
    for (int t = 0; t < W3DREC_TYPES; t++) {
        MapRec::iterator it = gById[t].begin();
        while (it != gById[t].end()) {
            W3dResource* r = it->second;
            ++it;                          // antes de un posible erase
            if (r->transientRef <= 0) continue;
            // SIN truncar: refTotal es int (una malla compartida por mas de 32767 objetos). Un
            // cast a short daba negativo y liberaba el recurso con todos sus usuarios vivos.
            r->refTotal = r->refTotal - r->transientRef;
            r->transientRef = 0;
            if (r->refTotal <= 0) { r->refTotal = 0; FreeResource(r); freed++; }
        }
    }
    return freed;
}

void W3dResourcesPump() {
    // UNA carga por llamada (que es una por frame): el hipo maximo es un
    // recurso, nunca la cola entera.
    if (gInFlight.empty()) return;
    W3dResource* r = gInFlight.front();
    gInFlight.erase(gInFlight.begin());
    if (r->state != W3DREC_IN_FLIGHT) return;
    // pool lleno y el tipo sabe su tamano: reintentar mas adelante (la senal
    // medible del pool chico es el faltante sostenido, no un crash)
    if (PoolActive() && gOps[r->type].Size) {
        const long t = gOps[r->type].Size(r->id);
        if (t > 0 && !PoolFits(t)) { gInFlight.push_back(r); return; }
    }
    LoadNow(r);
}

void W3dResourcesForgetType(int type) {
    if (!TypeValid(type)) return;
    // los payloads ya los destruyo el cliente (cierre de escena): aca solo
    // muere la contabilidad, sin tocar ops.
    for (MapRec::iterator it = gById[type].begin(); it != gById[type].end(); ++it) {
        // sacarlo de la cola en vuelo si estaba
        for (size_t i = 0; i < gInFlight.size(); i++)
            if (gInFlight[i] == it->second) { gInFlight.erase(gInFlight.begin() + (long)i); break; }
        PoolRelease(it->second);   // sus paginas quedan libres
        DeleteDescriptor(it->second);
    }
    gById[type].clear();
}

bool W3dResourceForget(int type, const std::string& id) {
    if (!TypeValid(type)) return false;
    MapRec::iterator it = gById[type].find(id);
    if (it == gById[type].end() || it->second->refTotal > 0) return false;
    for (size_t i = 0; i < gInFlight.size(); i++)
        if (gInFlight[i] == it->second) { gInFlight.erase(gInFlight.begin() + (long)i); break; }
    PoolRelease(it->second);
    DeleteDescriptor(it->second);
    gById[type].erase(it);
    return true;
}

// ---------------------------------------------------------------------------
//  MEDICION
// ---------------------------------------------------------------------------
int W3dLiveResources(int type) {
    if (!TypeValid(type)) return 0;
    int n = 0;
    for (MapRec::iterator it = gById[type].begin(); it != gById[type].end(); ++it)
        if (it->second->state == W3DREC_READY) n++;
    return n;
}
long W3dResourcesBytes(int type) {
    if (!TypeValid(type)) return 0;
    long b = 0;
    for (MapRec::iterator it = gById[type].begin(); it != gById[type].end(); ++it)
        if (it->second->state == W3DREC_READY) b += it->second->bytes;
    return b;
}
int  W3dResourcesLoads(int type)        { return TypeValid(type) ? gLoads[type] : 0; }
int  W3dResourcesReuses(int type)        { return TypeValid(type) ? gReuses[type] : 0; }
long W3dResourcesBytesLoaded(int type) { return TypeValid(type) ? gLoadedBytes[type] : 0; }

void W3dResourcesStatsReset(int type) {
    for (int t = 0; t < W3DREC_TYPES; t++) {
        if (type >= 0 && t != type) continue;
        gLoads[t] = 0; gReuses[t] = 0; gLoadedBytes[t] = 0;
    }
}
void W3dResourcesCountExternalLoad(int type, long bytes) {
    if (!TypeValid(type)) return;
    gLoads[type]++; gLoadedBytes[type] += bytes;
}
W3dResource* W3dResourceRegisterFailed(int type, const std::string& id) {
    if (!TypeValid(type) || id.empty()) return 0;
    W3dResource* r = W3dResourceFind(type, id);
    if (!r) {
        r = NewDescriptor();
        r->id = id; r->type = (unsigned char)type;
        gById[type][id] = r;
    }
    if (r->state == W3DREC_NOT_LOADED) r->state = W3DREC_FAILED;
    return r;
}
// cambia el ID de un recurso ya registrado (una textura que pasa de interna a externa o al reves):
// la misma ficha, otra clave. false si no existe el viejo o ya existe el nuevo.
bool W3dResourceRename(int type, const std::string& old, const std::string& newValue) {
    if (!TypeValid(type) || old.empty() || newValue.empty() || old == newValue) return false;
    MapRec& m = gById[type];
    MapRec::iterator it = m.find(old);
    if (it == m.end() || m.find(newValue) != m.end()) return false;
    W3dResource* rr = it->second;
    m.erase(it);
    rr->id = newValue;
    m[newValue] = rr;
    return true;
}
bool W3dResourceMove(int type, const std::string& old, const std::string& newValue) {
    if (!TypeValid(type) || old.empty() || newValue.empty()) return false;
    if (old == newValue) return W3dResourceFind(type, old) != 0;
    MapRec& m = gById[type];
    MapRec::iterator it = m.find(old);
    if (it == m.end()) return false;
    MapRec::iterator ot = m.find(newValue);
    if (ot != m.end()) {
        W3dResource* other = ot->second;
        // la clave la tiene un dato VIVO de otro: no se puede pisar (seria el mismo id para dos)
        if (other->state == W3DREC_READY && other->data) return false;
        // un descriptor SIN dato (EN_VUELO de una ListaCarga, un FALLO recordado): pedia ESE id,
        // que ahora es el que se muda -> sus referencias pasan al que llega y el viejo se borra
        W3dResource* rr = it->second;
        rr->refTotal += other->refTotal;
        rr->transientRef += other->transientRef;
        for (size_t i = 0; i < gInFlight.size(); i++)
            if (gInFlight[i] == other) { gInFlight.erase(gInFlight.begin() + (long)i); break; }
        PoolRelease(other);
        m.erase(ot);   // (borrar OTRA clave del map no invalida 'it')
        DeleteDescriptor(other);
    }
    W3dResource* rr = it->second;
    m.erase(it);
    rr->id = newValue;
    m[newValue] = rr;
    return true;
}
W3dResource* W3dResourceRegisterExternal(int type, const std::string& id,
                                       void* data, long bytes) {
    if (!TypeValid(type) || id.empty() || !data) return 0;
    W3dResource* r = W3dResourceFind(type, id);
    if (!r) {
        r = NewDescriptor();
        r->id = id; r->type = (unsigned char)type;
        gById[type][id] = r;
    }
    // pisa la copia anterior SIN liberarla: la vieja sigue viva por su cuenta
    // (refcount local del cliente), exactamente como el mapa de antes.
    r->data = data; r->bytes = bytes;
    r->state = W3DREC_READY;
    r->refTotal = 1; r->transientRef = 0;
    return r;
}
void W3dResourcesList(int type, std::vector<W3dResource*>& out) {
    out.clear();
    if (!TypeValid(type)) return;
    for (MapRec::iterator it = gById[type].begin(); it != gById[type].end(); ++it)
        out.push_back(it->second);
}

// ---------------------------------------------------------------------------
//  LISTAS DE CARGA (ver .h). El registro vive aca; el parseo del sidecar
//  `.cargas.json` es del editor (Curve.cpp) o de quien traiga las filas.
// ---------------------------------------------------------------------------
struct LoadsCurve {
    std::string name;
    std::vector<W3dLoadsRow> forward, back;
};
static std::vector<LoadsCurve*> gListsLoad;   // handle = indice (NULL = liberado)

int W3dLoadsRegister(const std::string& name,
                       const std::vector<W3dLoadsRow>& forward,
                       const std::vector<W3dLoadsRow>& back) {
    LoadsCurve* c = new LoadsCurve();
    c->name = name; c->forward = forward; c->back = back;
    gListsLoad.push_back(c);
    return (int)gListsLoad.size() - 1;
}
void W3dLoadsForgetAll() {
    for (size_t i = 0; i < gListsLoad.size(); i++) delete gListsLoad[i];
    gListsLoad.clear();
}
static LoadsCurve* LoadsOf(int handle) {
    if (handle < 0 || handle >= (int)gListsLoad.size()) return 0;
    return gListsLoad[(size_t)handle];
}
int W3dLoadsRows(int handle, bool forward) {
    LoadsCurve* c = LoadsOf(handle);
    if (!c) return -1;
    return (int)(forward ? c->forward.size() : c->back.size());
}

static void LoadsAcquireRow(const W3dLoadsRow& row, int mode) {
    for (size_t i = 0; i < row.size(); i++)
        W3dResourceAcquire(row[i].type, row[i].id,
                           W3DREC_TRANSIENT, mode, "listacarga");
}
static void LoadsReleaseRow(const W3dLoadsRow& row) {
    for (size_t i = 0; i < row.size(); i++) {
        W3dResource* r = W3dResourceFind(row[i].type, row[i].id);
        if (r && r->transientRef > 0) W3dResourceRelease(r, W3DREC_TRANSIENT);
    }
}

bool W3dLoadsEnter(int handle, bool forward) {
    LoadsCurve* c = LoadsOf(handle);
    if (!c) return false;
    // cambio de curva: TODO lo transitorio se va en bloque, y la fila de la
    // punta de entrada se adquiere BLOQUEANTE (el unico hipo permitido)
    W3dResourcesPurgeTransient();
    const std::vector<W3dLoadsRow>& L = forward ? c->forward : c->back;
    if (L.empty()) return true;
    LoadsAcquireRow(forward ? L.front() : L.back(), W3DREC_BLOCKING);
    return true;
}

bool W3dLoadsCrossNode(int handle, int node, bool forward) {
    LoadsCurve* c = LoadsOf(handle);
    if (!c) return false;
    const std::vector<W3dLoadsRow>& mine = forward ? c->forward : c->back;
    const std::vector<W3dLoadsRow>& op   = forward ? c->back : c->forward;
    if (node >= 0 && node < (int)mine.size())
        LoadsAcquireRow(mine[(size_t)node], W3DREC_ASYNC);
    const int loose = forward ? node - 1 : node + 1;
    if (loose >= 0 && loose < (int)op.size())
        LoadsReleaseRow(op[(size_t)loose]);
    return true;
}
