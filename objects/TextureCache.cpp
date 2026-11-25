// ============================================================================
//  Whisk3DCore (engine) — CACHE DE TEXTURAS POR RUTA, con refcount
//
//  Reporte del dueno (2026-08): "las texturas las estas cargando multiples
//  veces [...] tenes que guardar el path de una textura y si esa textura ya se
//  cargo, ya se cargo! no la cargues en memoria multiples veces".
//
//  Era literal: TODOS los caminos que veian un `map_Kd` hacian `new Texture()`
//  + LoadTexture sin mirar si esa ruta ya estaba cargada (import_obj.cpp:155 en
//  la cola diferida, import_obj.cpp:858 en los sprites animados, el dialogo de
//  "Load Texture"...). Un escenario partido en N trozos que comparten el mismo
//  atlas lo sube N VECES a la GPU: medido en un nivel de plataformas, 28 trozos
//  con un atlas de 512x512 = 28 MB, y 43 cajas con 2 atlas = 11 MB. Sin cache,
//  el proyecto tiene que esquivarlo con el truco del "ancla" (un solo .mtl
//  declara el map_Kd y los demas comparten el MATERIAL por nombre); con cache
//  por ruta esa muleta sobra. Ver docs/streaming-y-render.md.
//
//  DESDE EL ALMACEN DE RECURSOS ESTO ES UNA FACHADA: la identidad (ruta ->
//  descriptor), el refcount, la memoria de fallos y las estadisticas viven en
//  W3dRecursos (io/W3dResources.h), que es EL MISMO mecanismo que comparten
//  mallas, animaciones y listas de visibilidad. Aca queda lo especifico de
//  texturas: LoadTexture, el vector global `Textures`, el prefijo FIJADO de la
//  UI y la limpieza de los Texture* de los materiales al cerrar escena.
//  Mismos call sites, mismo A/B `texcache on|off`, mismos numeros.
//
//  POR QUE HAY REFCOUNT Y NO SOLO UN MAPA: sin contar referencias no se puede
//  LIBERAR. Y liberar es la mitad del pedido ("que libere de verdad al
//  descargar la escena"): RestartScene() no tocaba `Textures` ni una vez, o
//  sea que abrir un proyecto tras otro fugaba TODAS las texturas del anterior
//  (GPU incluida) para el resto de la vida del proceso.
// ============================================================================

#include "Textures.h"
#include "Materials.h"       // limpiar los Texture* de los materiales al liberar
#include "w3dTexture.h"      // TextureSize / DeleteTexture del motor
#include "w3dGraphics.h"     // PixelatedGlobal (decide si hay piramide de mipmaps)
#include "io/W3dResources.h" // el almacen: identidad + refcount + fallos + stats
#include <set>
#include <string>

// cuantas texturas del vector global son "de la UI" (font/origen/cursor3d/
// relationshipLine/lamp). Medio editor las indexa por POSICION -- Textures[0],
// Textures[3], el "Textures[5 + id - 2]" del desplegable de Properties -- asi
// que el cache NUNCA borra ni reordena por debajo de esta marca.
static size_t gBase = 0;
static bool   gBaseFixed = false;

// A/B: apagarlo reproduce el motor de ANTES (una copia por pedido). Ver Textures.h.
static bool gCacheOn = true;
void TexturesActiveCache(bool on) { gCacheOn = on; }
bool TexturesCacheIsActive()    { return gCacheOn; }

void TexturesSetBase() {
    if (gBaseFixed) return;      // una sola vez: la UI se carga en el arranque
    gBase = Textures.size();
    gBaseFixed = true;
}
int TexturesBase() { return (int)gBase; }

// la estadistica es la del almacen, tipo TEXTURA (texinfo la expone)
void TexturesStatsReset()   { W3dResourcesStatsReset(W3DREC_TEXTURE); }
int  TexturesReuses()       { return W3dResourcesReuses(W3DREC_TEXTURE); }
int  TexturesLoads()       { return W3dResourcesLoads(W3DREC_TEXTURE); }
long TexturesUploadedBytes() { return W3dResourcesBytesLoaded(W3DREC_TEXTURE); }

// ---------------------------------------------------------------------------
//  LA CARGA DE VERDAD (compartida por el op del almacen y el camino A/B off):
//  new Texture + LoadTexture + tamano real + mipmaps + alta en el vector
//  global. Devuelve NULL si la imagen no carga.
// ---------------------------------------------------------------------------
static Texture* LoadForReal(const std::string& path, long* bytes) {
    Texture* t = new Texture(path);
    if (!LoadTexture(path.c_str(), t->iID)) { delete t; return NULL; }
    // tamano real: lo publica el cargador del motor (w3dTexture.cpp lo anota en
    // g_texSizes por id). Es lo unico con lo que se puede CONTAR la memoria.
    int w = 0, h = 0;
    if (w3dEngine::TextureSize((unsigned int)t->iID, w, h)) { t->width = w; t->height = h; }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(W3D_SYMBIAN)
    // el LoadTexture de escritorio arma la piramide de mipmaps... salvo con el
    // PIXELADO GLOBAL prendido, que la saltea (y ahi la textura ocupa ~1/4 menos)
    t->withMipmaps = !w3dEngine::PixelatedGlobal();
#endif
    Textures.push_back(t);
    if (bytes) {
        long b = (long)t->width * (long)t->height * 4L;   // RGBA8, como sube el motor
        if (t->withMipmaps) b += b / 3;
        *bytes = b;
    }
    return t;
}

// baja del vector global + libera GL. No mira refs (el que llama ya decidio).
// Devuelve false si la textura esta FIJADA (es de la UI): esa no se toca jamas.
static bool FreeForReal(Texture* t) {
    if (!t) return false;
    for (size_t i = 0; i < gBase && i < Textures.size(); i++)
        if (Textures[i] == t) return false;

    if (t->iID) w3dEngine::DeleteTexture((unsigned int)t->iID);
    for (size_t i = gBase; i < Textures.size(); i++)
        if (Textures[i] == t) { Textures.erase(Textures.begin() + (long)i); g_w3dTexturesGen++; break; }
    delete t;
    return true;
}

// ---- los ops que este tipo registra en el almacen ----
static void* OpLoad(const std::string& id, long* bytes) {
    return LoadForReal(id, bytes);
}
static bool OpFree(void* data) {
    return FreeForReal((Texture*)data);
}
static void EnsureOps() {
    static bool ready = false;
    if (ready) return;
    W3dResourceOps ops; ops.Load = OpLoad; ops.Free = OpFree;
    W3dResourcesRegisterType(W3DREC_TEXTURE, ops);
    ready = true;
}

// LA CLAVE del cache es la ruta NORMALIZADA: separador '/', y los "dir/../"
// colapsados. Sin esto, la MISMA textura pedida desde .mtl de carpetas
// distintas ("escenario/../textures/atlas.png" vs "models/crash/../../
// textures/atlas.png") eran DOS claves -> DOS copias del atlas en GPU (2x4MB
// en el N95, y el editor UV la listaba dos veces). Colapsar tambien hace que
// la ruta exista como entrada del contenedor v4 (el VFS no resuelve "..").
static std::string TexKey(std::string r) {
    for (size_t i = 0; i < r.size(); i++)
        if (r[i] == '\\') r[i] = '/';
    size_t p;
    while ((p = r.find("/../")) != std::string::npos) {
        size_t s = r.rfind('/', p ? p - 1 : 0);
        if (s == std::string::npos) break;   // "../x" sin dir previo: se queda
        r.erase(s, p + 3 - s);
    }
    return r;
}

Texture* TextureFind(const std::string& rawPath) {
    if (rawPath.empty()) return NULL;
    std::string path = TexKey(rawPath);
    W3dResource* r = W3dResourceFind(W3DREC_TEXTURE, path);
    return (r && r->state == W3DREC_READY) ? (Texture*)r->data : NULL;
}

Texture* TextureTake(const std::string& rawPath) {
    if (rawPath.empty()) return NULL;
    std::string path = TexKey(rawPath);
    EnsureOps();
    if (!gCacheOn) {
        // el camino de ANTES: una copia nueva en GPU por cada pedido, aunque la
        // ruta se repita. El mapa recuerda la ULTIMA copia (pisando la
        // anterior), los fallos se recuerdan igual, y las cargas CUENTAN igual:
        // el A/B mide las dos mitades con los mismos numeros.
        long b = 0;
        Texture* t = LoadForReal(path, &b);
        if (!t) { W3dResourceRegisterFailed(W3DREC_TEXTURE, path); return NULL; }
        t->refs = 1;
        W3dResourceRegisterExternal(W3DREC_TEXTURE, path, t, b);
        W3dResourcesCountExternalLoad(W3DREC_TEXTURE, b);
        return t;
    }
    W3dResource* r = W3dResourceAcquire(W3DREC_TEXTURE, path,
                                       W3DREC_PERMANENT, W3DREC_BLOCKING, "");
    Texture* t = (Texture*)W3dResourceResolve(r);
    if (!t) return NULL;                 // fallo (recordado: no se reintenta)
    t->refs = r->refTotal;               // espejo para texinfo/Properties
    return t;
}

// ACTUALIZAR: vuelve a leer la imagen de SU ruta y la sube de nuevo en la MISMA Texture (los materiales que la usan la
// ven cambiada sin tocarlos). false = no se pudo leer o decodificar: queda la de antes
bool TextureReread(Texture* t) {
    if (!t || t->path.empty()) return false;
    unsigned int id = 0;
    if (!LoadTexture(t->path.c_str(), id) || !id) return false;
    if (t->iID) w3dEngine::DeleteTexture((unsigned int)t->iID);
    t->iID = id;
    int w = 0, h = 0;
    if (w3dEngine::TextureSize(id, w, h)) { t->width = w; t->height = h; }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(W3D_SYMBIAN)
    t->withMipmaps = !w3dEngine::PixelatedGlobal();
#endif
    g_w3dTexturesGen++;   // (lo que cachea por textura -vistas previas, el trazado de rayos- la vuelve a leer)
    return true;
}

// registra una textura YA SUBIDA (creada en memoria por el editor: "Nueva textura") bajo 'rawPath',
// como si se hubiera cargado de ahi. Entra al vector global y al registro con 1 ref.
Texture* TextureRegister(const std::string& rawPath, Texture* t) {
    if (!t || rawPath.empty()) return NULL;
    std::string path = TexKey(rawPath);
    EnsureOps();
    t->path = path; t->refs = 1;
    bool is = false;
    for (size_t i = 0; i < Textures.size(); i++) if (Textures[i] == t) is = true;
    if (!is) Textures.push_back(t);
    const long b = (long)t->width * (long)t->height * 4L;
    W3dResourceRegisterExternal(W3DREC_TEXTURE, path, t, b);
    return t;
}
// la textura cambia de ruta (interna <-> externa): misma ficha del registro, otra clave; los materiales
// que apuntan al Texture* no se enteran (y el guardado escribe la ruta nueva)
void TextureRename(Texture* t, const std::string& newRaw) {
    if (!t || newRaw.empty()) return;
    std::string newValue = TexKey(newRaw);
    if (newValue == t->path) return;
    // MOVER (no solo renombrar): la clave nueva pudo quedar tomada por un descriptor SIN dato (el
    // FALLO recordado de una vista previa que la pidio antes de que existiera). Con Renombrar eso
    // caia al RegisterExternal de abajo y la clave VIEJA seguia apuntando a esta textura.
    if (!W3dResourceMove(W3DREC_TEXTURE, t->path, newValue)) {
        // no estaba registrada por su ruta (copia con refcount local): se registra con la nueva
        const long b = (long)t->width * (long)t->height * 4L;
        W3dResourceRegisterExternal(W3DREC_TEXTURE, newValue, t, b);
    }
    t->path = newValue;
}
static bool InGraveyard(const Texture* t);   // (las purgadas en caliente: mas abajo)
void TextureRetain(Texture* t) {
    if (!t || InGraveyard(t)) return;   // (la del cementerio no tiene cuenta: ver TextureRelease)
    W3dResource* r = W3dResourceFind(W3DREC_TEXTURE, t->path);
    if (r && r->data == t) { W3dResourceRetain(r, W3DREC_PERMANENT); t->refs = r->refTotal; }
    else t->refs++;                      // copia sin descriptor: refcount local
}

static bool gKeepOrphan = false;
void TexturesSetKeepOrphans(bool on) { gKeepOrphan = on; }
bool TexturesKeepOrphans() { return gKeepOrphan; }
// la de una LIBRERIA externa ("lib:<nombre>/...") no es de la biblioteca del proyecto: se vuelve a pedir de la libreria
// cuando haga falta, asi que sin usuarios se libera tambien en el editor
static bool KeepsOrphan(const Texture* t) { return gKeepOrphan && t->path.compare(0, 4, "lib:") != 0; }

// keep = false: la ultima referencia la libera aunque el editor guarde huerfanas (TextureReleaseAsleep)
static void Release(Texture* t, bool keep) {
    if (!t) return;
    // una PURGADA en caliente es del CEMENTERIO (sus referencias se cerraron al purgarla y el objeto lo libera el
    // cierre del proyecto): soltarla aca -un paso de undo que la tenia y se cae del historial- la borraba dos veces
    if (InGraveyard(t)) return;
    W3dResource* r = W3dResourceFind(W3DREC_TEXTURE, t->path);
    if (r && r->data == t) {
        // espejo ANTES de soltar: si el refcount llega a 0 el almacen libera
        // de verdad (y `t` deja de existir; si esta FIJADA queda con refs 0).
        t->refs = (r->refTotal > 0) ? (r->refTotal - 1) : 0;
        if (keep && KeepsOrphan(t) && r->refTotal <= 1) {   // la ultima: queda cargada, huerfana (ver el .h)
            r->refTotal = 0; r->transientRef = 0;
            return;
        }
        W3dResourceRelease(r, W3DREC_PERMANENT);
        return;
    }
    // sin descriptor (una copia vieja del camino A/B off): refcount local
    if (t->refs > 0) t->refs--;
    if (t->refs > 0 || (keep && KeepsOrphan(t))) return;
    FreeForReal(t);
}
void TextureRelease(Texture* t)       { Release(t, true); }
void TextureReleaseAsleep(Texture* t) { Release(t, false); }

// la RANURA de un material es duena de una referencia (ver Textures.h): la nueva se retiene ANTES de soltar la vieja
// (si es la misma no se toca: soltarla primero la podia liberar en el medio)
void TextureSetIn(Texture*& slot, Texture* t) {
    if (slot == t) return;
    Texture* prev = slot;
    TextureRetain(t);
    slot = t;
    TextureRelease(prev);
}

W3dResource* TextureRequest(const std::string& rawPath, int mode) {
    if (rawPath.empty()) return NULL;
    EnsureOps();
    W3dResource* r = W3dResourceAcquire(W3DREC_TEXTURE, TexKey(rawPath), W3DREC_PERMANENT, mode, "streaming");
    if (!r || r->state == W3DREC_FAILED) return NULL;   // (un fallo no suma referencia)
    if (r->state == W3DREC_READY && r->data) ((Texture*)r->data)->refs = r->refTotal;   // espejo para texinfo
    return r;
}

// ---------------------------------------------------------------------------
//  PURGAR EN CALIENTE / REVIVIR (ver Textures.h): el CEMENTERIO de las purgadas
// ---------------------------------------------------------------------------
static std::vector<Texture*> gPurged;

const std::vector<Texture*>& TexturesPurged() { return gPurged; }
static bool InGraveyard(const Texture* t) {
    for (size_t i = 0; i < gPurged.size(); i++) if (gPurged[i] == t) return true;
    return false;
}

bool TexturePurge(Texture* t) {
    if (!t) return false;
    for (size_t i = 0; i < gBase && i < Textures.size(); i++)
        if (Textures[i] == t) return false;                  // de la UI: jamas
    bool was = false;
    for (size_t i = gBase; i < Textures.size(); i++)
        if (Textures[i] == t) { Textures.erase(Textures.begin() + (long)i); g_w3dTexturesGen++; was = true; break; }
    if (!was) return false;
    // la GPU se libera YA (es lo que pesa); el objeto queda para los punteros viejos
    if (t->iID) w3dEngine::DeleteTexture((unsigned int)t->iID);
    t->iID = 0;
    // el almacen la olvida SIN liberarla (Olvidar no llama al op Liberar): nadie la vuelve a
    // obtener por su ruta, y un TextureTake posterior de esa ruta la carga como nueva
    W3dResource* r = W3dResourceFind(W3DREC_TEXTURE, TexKey(t->path));
    if (r && r->data == t) {
        r->refTotal = 0; r->transientRef = 0;              // las refs eran de esta copia
        const std::string key = r->id;                     // (copia: Olvidar borra el descriptor)
        W3dResourceForget(W3DREC_TEXTURE, key);
    }
    t->refs = 0;
    gPurged.push_back(t);
    return true;
}

bool TextureRevive(Texture* t) {
    size_t k = 0;
    while (k < gPurged.size() && gPurged[k] != t) k++;
    if (!t || k == gPurged.size()) return false;
    if (TextureFind(t->path)) return false;                // la ruta ya tiene OTRA viva
    GLuint id = 0;
    if (!LoadTexture(t->path.c_str(), id)) return false;     // la imagen ya no esta: sigue vacia
    t->iID = id;
    int w = 0, h = 0;
    if (w3dEngine::TextureSize((unsigned int)t->iID, w, h)) { t->width = w; t->height = h; }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(W3D_SYMBIAN)
    t->withMipmaps = !w3dEngine::PixelatedGlobal();
#endif
    gPurged.erase(gPurged.begin() + (long)k);
    Textures.push_back(t);
    EnsureOps();
    long b = (long)t->width * (long)t->height * 4L;
    if (t->withMipmaps) b += b / 3;
    W3dResourceRegisterExternal(W3DREC_TEXTURE, TexKey(t->path), t, b);
    t->refs = 1;
    return true;
}

// ---------------------------------------------------------------------------
//  CIERRE DE PROYECTO
//  Lo llama RestartScene() DESPUES de destruir el arbol. Libera todo lo que
//  no sea de la UI y deja en NULL los Texture* de TODOS los materiales vivos:
//  `Materials` no se poda al cerrar un proyecto, asi que sin este barrido
//  quedarian punteros colgados y el primer render del proyecto siguiente
//  dereferenciaria memoria liberada.
// ---------------------------------------------------------------------------
int TexturesFreeScene() {
    // las estadisticas son POR ESCENA: cerrar un proyecto las pone en cero, asi
    // "cargas/reusos/bytes subidos" siempre hablan de la escena que esta abierta
    // (sin esto se acumulaban entre proyectos y no se podia asertar nada).
    TexturesStatsReset();

    // 1) juntar las que se van (todo lo que esta por encima de la marca de la UI)
    std::set<Texture*> seVan;
    for (size_t i = gBase; i < Textures.size(); i++)
        if (Textures[i]) seVan.insert(Textures[i]);
    // el CEMENTERIO de las purgadas en caliente tambien se va (sus objetos siguen vivos)
    for (size_t i = 0; i < gPurged.size(); i++)
        if (gPurged[i]) seVan.insert(gPurged[i]);
    if (seVan.empty()) {
        // igual muere la contabilidad (incluidos los FALLOS recordados)
        W3dResourcesForgetType(W3DREC_TEXTURE);
        return 0;
    }

    // 2) soltar los punteros de los materiales ANTES de borrar nada
    for (size_t i = 0; i < Materials.size(); i++) {
        Material* m = Materials[i];
        if (!m) continue;
        if (m->texture       && seVan.count(m->texture))       m->texture = NULL;
        if (m->normalTexture && seVan.count(m->normalTexture)) m->normalTexture = NULL;
        if (m->textureB && seVan.count(m->textureB)) m->textureB = NULL;
        for (int k = W3D_MAP_EMISSION; k < W3D_MAPS; k++) if (m->mapTex[k] && seVan.count(m->mapTex[k])) m->mapTex[k] = NULL;
        for (size_t c = 0; c < m->layers.size(); c++)
            if (m->layers[c].tex && seVan.count(m->layers[c].tex)) m->layers[c].tex = NULL;
    }
    // el cementerio de MATERIALES tambien puede apuntarlas (se libera despues, en
    // MaterialsFreeScene: que no quede ni un puntero colgado mientras tanto)
    const std::vector<Material*>& mp = MaterialsPurged();
    for (size_t i = 0; i < mp.size(); i++) {
        Material* m = mp[i];
        if (!m) continue;
        if (m->texture       && seVan.count(m->texture))       m->texture = NULL;
        if (m->normalTexture && seVan.count(m->normalTexture)) m->normalTexture = NULL;
        if (m->textureB && seVan.count(m->textureB)) m->textureB = NULL;
        for (int k = W3D_MAP_EMISSION; k < W3D_MAPS; k++) if (m->mapTex[k] && seVan.count(m->mapTex[k])) m->mapTex[k] = NULL;
        for (size_t c = 0; c < m->layers.size(); c++)
            if (m->layers[c].tex && seVan.count(m->layers[c].tex)) m->layers[c].tex = NULL;
    }
    // los frames de material animado apuntan a las mismas texturas
    for (size_t i = 0; i < AnimatedMaterials.size(); i++) {
        AnimatedMaterial* am = AnimatedMaterials[i];
        if (!am) continue;
        for (size_t f = 0; f < am->frameTextures.size(); f++)
            if (am->frameTextures[f] && seVan.count(am->frameTextures[f])) am->frameTextures[f] = NULL;
    }

    // 3) liberar de verdad (GL + objeto). Se recorre de atras para adelante
    //    porque FreeForReal hace erase() sobre el mismo vector. Se libera
    //    DIRECTO (no via almacen): el cierre de escena ignora refcounts a
    //    proposito, y despues la contabilidad muere en bloque con OlvidarTipo
    //    (que NO vuelve a tocar los payloads: ya no existen).
    int n = 0;
    while (Textures.size() > gBase) {
        Texture* t = Textures.back();
        if (!FreeForReal(t)) break;   // fijada: no deberia pasar arriba de gBase
        n++;
    }
    // las purgadas ya no tienen GPU ni estan en el vector: solo queda el objeto
    for (size_t i = 0; i < gPurged.size(); i++) { delete gPurged[i]; n++; }
    gPurged.clear();
    W3dResourcesForgetType(W3DREC_TEXTURE);
    return n;
}

// ---------------------------------------------------------------------------
//  MEDICION
// ---------------------------------------------------------------------------
static long BytesOf(const Texture* t) {
    if (!t || t->width <= 0 || t->height <= 0) return 0;
    long b = (long)t->width * (long)t->height * 4L;   // RGBA8, que es como sube el motor
    if (t->withMipmaps) b += b / 3;                   // la piramide suma ~1/3
    return b;
}

int LiveTextures() { return (int)Textures.size(); }

long TexturesBytes() {
    long b = 0;
    for (size_t i = 0; i < Textures.size(); i++) b += BytesOf(Textures[i]);
    return b;
}

// lo que NO se subio gracias al cache: cada ref extra habria sido, sin cache,
// una copia entera de la misma imagen en la GPU.
long TexturesBytesSaved() {
    long b = 0;
    for (size_t i = 0; i < Textures.size(); i++) {
        const Texture* t = Textures[i];
        if (!t || t->refs <= 1) continue;
        b += BytesOf(t) * (long)(t->refs - 1);
    }
    return b;
}
