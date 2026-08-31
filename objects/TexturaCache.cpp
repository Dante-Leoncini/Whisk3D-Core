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
//  W3dRecursos (io/W3dRecursos.h), que es EL MISMO mecanismo que comparten
//  mallas, animaciones y listas de visibilidad. Aca queda lo especifico de
//  texturas: LoadTexture, el vector global `Textures`, el prefijo FIJADO de la
//  UI y la limpieza de los Texture* de los materiales al cerrar escena.
//  Mismos call sites, mismo A/B `texcache on|off`, mismos numeros.
//
//  POR QUE HAY REFCOUNT Y NO SOLO UN MAPA: sin contar referencias no se puede
//  LIBERAR. Y liberar es la mitad del pedido ("que libere de verdad al
//  descargar la escena"): ReiniciarEscena() no tocaba `Textures` ni una vez, o
//  sea que abrir un proyecto tras otro fugaba TODAS las texturas del anterior
//  (GPU incluida) para el resto de la vida del proceso.
// ============================================================================

#include "Textures.h"
#include "Materials.h"      // limpiar los Texture* de los materiales al liberar
#include "w3dTexture.h"     // TextureSize / DeleteTexture del motor
#include "w3dGraphics.h"    // PixeladoGlobal (decide si hay piramide de mipmaps)
#include "io/W3dRecursos.h" // el almacen: identidad + refcount + fallos + stats
#include <set>
#include <string>

// cuantas texturas del vector global son "de la UI" (font/origen/cursor3d/
// relationshipLine/lamp). Medio editor las indexa por POSICION -- Textures[0],
// Textures[3], el "Textures[5 + id - 2]" del desplegable de Properties -- asi
// que el cache NUNCA borra ni reordena por debajo de esta marca.
static size_t gBase = 0;
static bool   gBaseFijada = false;

// A/B: apagarlo reproduce el motor de ANTES (una copia por pedido). Ver Textures.h.
static bool gCacheOn = true;
void TexturasCacheActivo(bool on) { gCacheOn = on; }
bool TexturasCacheEstaActivo()    { return gCacheOn; }

void TexturasFijarBase() {
    if (gBaseFijada) return;      // una sola vez: la UI se carga en el arranque
    gBase = Textures.size();
    gBaseFijada = true;
}
int TexturasBase() { return (int)gBase; }

// la estadistica es la del almacen, tipo TEXTURA (texinfo la expone)
void TexturasStatsReset()   { W3dRecursosStatsReset(W3DREC_TEXTURA); }
int  TexturasReusos()       { return W3dRecursosReusos(W3DREC_TEXTURA); }
int  TexturasCargas()       { return W3dRecursosCargas(W3DREC_TEXTURA); }
long TexturasBytesSubidos() { return W3dRecursosBytesCargados(W3DREC_TEXTURA); }

// ---------------------------------------------------------------------------
//  LA CARGA DE VERDAD (compartida por el op del almacen y el camino A/B off):
//  new Texture + LoadTexture + tamano real + mipmaps + alta en el vector
//  global. Devuelve NULL si la imagen no carga.
// ---------------------------------------------------------------------------
static Texture* CargarDeVerdad(const std::string& path, long* bytes) {
    Texture* t = new Texture(path);
    if (!LoadTexture(path.c_str(), t->iID)) { delete t; return NULL; }
    // tamano real: lo publica el cargador del motor (w3dTexture.cpp lo anota en
    // g_texSizes por id). Es lo unico con lo que se puede CONTAR la memoria.
    int w = 0, h = 0;
    if (w3dEngine::TextureSize((unsigned int)t->iID, w, h)) { t->ancho = w; t->alto = h; }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(W3D_SYMBIAN)
    // el LoadTexture de escritorio arma la piramide de mipmaps... salvo con el
    // PIXELADO GLOBAL prendido, que la saltea (y ahi la textura ocupa ~1/4 menos)
    t->conMipmaps = !w3dEngine::PixeladoGlobal();
#endif
    Textures.push_back(t);
    if (bytes) {
        long b = (long)t->ancho * (long)t->alto * 4L;   // RGBA8, como sube el motor
        if (t->conMipmaps) b += b / 3;
        *bytes = b;
    }
    return t;
}

// baja del vector global + libera GL. No mira refs (el que llama ya decidio).
// Devuelve false si la textura esta FIJADA (es de la UI): esa no se toca jamas.
static bool LiberarDeVerdad(Texture* t) {
    if (!t) return false;
    for (size_t i = 0; i < gBase && i < Textures.size(); i++)
        if (Textures[i] == t) return false;

    if (t->iID) w3dEngine::DeleteTexture((unsigned int)t->iID);
    for (size_t i = gBase; i < Textures.size(); i++)
        if (Textures[i] == t) { Textures.erase(Textures.begin() + (long)i); break; }
    delete t;
    return true;
}

// ---- los ops que este tipo registra en el almacen ----
static void* OpCargar(const std::string& id, long* bytes) {
    return CargarDeVerdad(id, bytes);
}
static bool OpLiberar(void* dato) {
    return LiberarDeVerdad((Texture*)dato);
}
static void AsegurarOps() {
    static bool listo = false;
    if (listo) return;
    W3dRecursoOps ops; ops.Cargar = OpCargar; ops.Liberar = OpLiberar;
    W3dRecursosRegistrarTipo(W3DREC_TEXTURA, ops);
    listo = true;
}

// LA CLAVE del cache es la ruta NORMALIZADA: separador '/', y los "dir/../"
// colapsados. Sin esto, la MISMA textura pedida desde .mtl de carpetas
// distintas ("escenario/../texturas/atlas.png" vs "modelos/crash/../../
// texturas/atlas.png") eran DOS claves -> DOS copias del atlas en GPU (2x4MB
// en el N95, y el editor UV la listaba dos veces). Colapsar tambien hace que
// la ruta exista como entrada del contenedor v4 (el VFS no resuelve "..").
static std::string TexClave(std::string r) {
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

Texture* TexturaBuscar(const std::string& pathCrudo) {
    if (pathCrudo.empty()) return NULL;
    std::string path = TexClave(pathCrudo);
    W3dRecurso* r = W3dRecursoBuscar(W3DREC_TEXTURA, path);
    return (r && r->estado == W3DREC_LISTO) ? (Texture*)r->dato : NULL;
}

Texture* TexturaTomar(const std::string& pathCrudo) {
    if (pathCrudo.empty()) return NULL;
    std::string path = TexClave(pathCrudo);
    AsegurarOps();
    if (!gCacheOn) {
        // el camino de ANTES: una copia nueva en GPU por cada pedido, aunque la
        // ruta se repita. El mapa recuerda la ULTIMA copia (pisando la
        // anterior), los fallos se recuerdan igual, y las cargas CUENTAN igual:
        // el A/B mide las dos mitades con los mismos numeros.
        long b = 0;
        Texture* t = CargarDeVerdad(path, &b);
        if (!t) { W3dRecursoRegistrarFallo(W3DREC_TEXTURA, path); return NULL; }
        t->refs = 1;
        W3dRecursoRegistrarExterno(W3DREC_TEXTURA, path, t, b);
        W3dRecursosContarCargaExterna(W3DREC_TEXTURA, b);
        return t;
    }
    W3dRecurso* r = W3dRecursoAdquirir(W3DREC_TEXTURA, path,
                                       W3DREC_PERMANENTE, W3DREC_BLOQUEANTE, "");
    Texture* t = (Texture*)W3dRecursoResolver(r);
    if (!t) return NULL;                 // fallo (recordado: no se reintenta)
    t->refs = r->refTotal;               // espejo para texinfo/Properties
    return t;
}

void TexturaRetener(Texture* t) {
    if (!t) return;
    W3dRecurso* r = W3dRecursoBuscar(W3DREC_TEXTURA, t->path);
    if (r && r->dato == t) { W3dRecursoRetener(r, W3DREC_PERMANENTE); t->refs = r->refTotal; }
    else t->refs++;                      // copia sin descriptor: refcount local
}

void TexturaSoltar(Texture* t) {
    if (!t) return;
    W3dRecurso* r = W3dRecursoBuscar(W3DREC_TEXTURA, t->path);
    if (r && r->dato == t) {
        // espejo ANTES de soltar: si el refcount llega a 0 el almacen libera
        // de verdad (y `t` deja de existir; si esta FIJADA queda con refs 0).
        t->refs = (r->refTotal > 0) ? (r->refTotal - 1) : 0;
        W3dRecursoSoltar(r, W3DREC_PERMANENTE);
        return;
    }
    // sin descriptor (una copia vieja del camino A/B off): refcount local
    if (t->refs > 0) t->refs--;
    if (t->refs > 0) return;
    LiberarDeVerdad(t);
}

// ---------------------------------------------------------------------------
//  CIERRE DE PROYECTO
//  Lo llama ReiniciarEscena() DESPUES de destruir el arbol. Libera todo lo que
//  no sea de la UI y deja en NULL los Texture* de TODOS los materiales vivos:
//  `Materials` no se poda al cerrar un proyecto, asi que sin este barrido
//  quedarian punteros colgados y el primer render del proyecto siguiente
//  dereferenciaria memoria liberada.
// ---------------------------------------------------------------------------
int TexturasLiberarEscena() {
    // las estadisticas son POR ESCENA: cerrar un proyecto las pone en cero, asi
    // "cargas/reusos/bytes subidos" siempre hablan de la escena que esta abierta
    // (sin esto se acumulaban entre proyectos y no se podia asertar nada).
    TexturasStatsReset();

    // 1) juntar las que se van (todo lo que esta por encima de la marca de la UI)
    std::set<Texture*> seVan;
    for (size_t i = gBase; i < Textures.size(); i++)
        if (Textures[i]) seVan.insert(Textures[i]);
    if (seVan.empty()) {
        // igual muere la contabilidad (incluidos los FALLOS recordados)
        W3dRecursosOlvidarTipo(W3DREC_TEXTURA);
        return 0;
    }

    // 2) soltar los punteros de los materiales ANTES de borrar nada
    for (size_t i = 0; i < Materials.size(); i++) {
        Material* m = Materials[i];
        if (!m) continue;
        if (m->texture       && seVan.count(m->texture))       m->texture = NULL;
        if (m->normalTexture && seVan.count(m->normalTexture)) m->normalTexture = NULL;
        for (size_t c = 0; c < m->capas.size(); c++)
            if (m->capas[c].tex && seVan.count(m->capas[c].tex)) m->capas[c].tex = NULL;
    }
    // los frames de material animado apuntan a las mismas texturas
    for (size_t i = 0; i < AnimatedMaterials.size(); i++) {
        AnimatedMaterial* am = AnimatedMaterials[i];
        if (!am) continue;
        for (size_t f = 0; f < am->frameTextures.size(); f++)
            if (am->frameTextures[f] && seVan.count(am->frameTextures[f])) am->frameTextures[f] = NULL;
    }

    // 3) liberar de verdad (GL + objeto). Se recorre de atras para adelante
    //    porque LiberarDeVerdad hace erase() sobre el mismo vector. Se libera
    //    DIRECTO (no via almacen): el cierre de escena ignora refcounts a
    //    proposito, y despues la contabilidad muere en bloque con OlvidarTipo
    //    (que NO vuelve a tocar los payloads: ya no existen).
    int n = 0;
    while (Textures.size() > gBase) {
        Texture* t = Textures.back();
        if (!LiberarDeVerdad(t)) break;   // fijada: no deberia pasar arriba de gBase
        n++;
    }
    W3dRecursosOlvidarTipo(W3DREC_TEXTURA);
    return n;
}

// ---------------------------------------------------------------------------
//  MEDICION
// ---------------------------------------------------------------------------
static long BytesDe(const Texture* t) {
    if (!t || t->ancho <= 0 || t->alto <= 0) return 0;
    long b = (long)t->ancho * (long)t->alto * 4L;   // RGBA8, que es como sube el motor
    if (t->conMipmaps) b += b / 3;                  // la piramide suma ~1/3
    return b;
}

int TexturasVivas() { return (int)Textures.size(); }

long TexturasBytes() {
    long b = 0;
    for (size_t i = 0; i < Textures.size(); i++) b += BytesDe(Textures[i]);
    return b;
}

// lo que NO se subio gracias al cache: cada ref extra habria sido, sin cache,
// una copia entera de la misma imagen en la GPU.
long TexturasBytesAhorrados() {
    long b = 0;
    for (size_t i = 0; i < Textures.size(); i++) {
        const Texture* t = Textures[i];
        if (!t || t->refs <= 1) continue;
        b += BytesDe(t) * (long)(t->refs - 1);
    }
    return b;
}
