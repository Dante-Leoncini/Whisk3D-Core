#include "Materials.h"
#include "W3dNombres.h"   // nombres unicos (misma regla que objetos/huesos/grupos)
#include "w3dGraphics.h"  // w3dEngine::Mezcla*: los codigos de mezcla (W3dMaterialEsLuz)

// Variables globales
std::vector<Material*> Materials;
Material* MaterialDefecto = NULL;

// ---- MARCA BASE del registro de materiales (ver el comentario de Materials.h) ----
static size_t gMatBase = 0;
static bool   gMatBaseFijada = false;

void MaterialesFijarBase() {
    if (gMatBaseFijada) return;   // una sola vez: la UI se carga en el arranque
    gMatBase = Materials.size();
    gMatBaseFijada = true;
}
int MaterialesBase() { return (int)gMatBase; }

int MaterialesLiberarEscena() {
    int n = 0;
    while (Materials.size() > gMatBase) {
        Material* m = Materials.back();
        Materials.pop_back();
        // MaterialDefecto no entra a la lista (ver el ctor), pero se chequea igual:
        // liberarlo dejaria colgado al fallback de TODAS las mallas sin material.
        if (m && m != MaterialDefecto) { delete m; n++; }
    }
    // los materiales ANIMADOS apuntan a materiales del proyecto (targets): si
    // quedaran, su Update() escribiria sobre memoria liberada en el proyecto nuevo.
    for (size_t i = 0; i < AnimatedMaterials.size(); i++) delete AnimatedMaterials[i];
    AnimatedMaterials.clear();
    return n;
}

// ===================================================
// Implementación de Material
// ===================================================
Material::Material(const std::string& nombre, bool MaterialDefectoFlag, bool TieneVertexColor)
    : textureOn(true), filtrado(true), transparent(false), vertexColor(false), lighting(true), repeat(true),
      uv8bit(false), culling(true), depth_test(true), fondo(false),
      depth_write(true), depth_bias(0.0f), orden_pasada(0), mezcla(0),
      chrome(false), reflectMode(0),
      normalMap(false), normalTexture(NULL),
      lineas(false), grosorLinea(1.0f),
      interpolacion(0), texture(NULL), shininess(12.0f) { // interpolacion 0 = lineal (suave), 1 = closest (pixel); igual en TODOS los sistemas
    // defaults que eran inicializadores de clase (C++11)
    diffuse[0] = diffuse[1] = diffuse[2] = diffuse[3] = 1.0f;
    specular[0] = specular[1] = specular[2] = 0.3f; specular[3] = 1.0f;
    emission[0] = emission[1] = emission[2] = 0.0f; emission[3] = 1.0f;
    ambient[0] = ambient[1] = ambient[2] = 0.3f; ambient[3] = 1.0f; // gris (como el preview)
    if (!MaterialDefectoFlag){
        // los materiales del proyecto son GLOBALES y unicos: el ctor es el unico
        // punto de alta (se auto-registra), asi que uniquificar aca cubre el
        // "New Material" del panel, los importadores y la carga de los .glb.
        // El material POR DEFECTO no entra a la lista -> no participa.
        name = MaterialNombreLibre(nombre, NULL);
        Materials.push_back(this);
    } else {
        name = nombre;
    }
    vertexColor = TieneVertexColor;
    // interpolacion ya quedo en 'lineal' (0) por la lista de inicializacion, igual en todos los
    // sistemas. (Antes Symbian la ponia en 1 = closest y PC en 0 = lineal: quedaban distintas.)
}

Material::~Material() {}

// ===================================================
// Funciones auxiliares
// ===================================================
// ===== NOMBRES UNICOS de material (espacio GLOBAL de proyecto) =====
// 'excepto' = el material que puede conservar su nombre (rename); NULL = ninguno.
static bool MatNombreExiste(const std::string& n, void* ctx) {
    const Material* excepto = (const Material*)ctx;
    for (size_t i = 0; i < Materials.size(); ++i)
        if (Materials[i] != excepto && Materials[i]->name == n) return true;
    return false;
}
std::string MaterialNombreLibre(const std::string& base, const Material* excepto) {
    return W3dNombreUnico(base, "Material", MatNombreExiste, (void*)excepto);
}
void Material::SetName(const std::string& nombre) {
    name = MaterialNombreLibre(nombre, this);
}

Material* BuscarMaterialPorNombre(const std::string& name) {
    for (size_t i = 0; i < Materials.size(); ++i){
        if (Materials[i]->name == name) return Materials[i];
    }
    return NULL;
}

// ===== MATERIAL "LUZ": MODULA EL FRAMEBUFFER, NO LO TAPA (ver Materials.h) =====
// Se pregunta por la MEZCLA, no por el nombre del material: cualquier modo que
// componga con lo que YA HAY (sumar, restar, multiplicar, screen) describe luz.
// Quedan afuera a proposito el alfa comun (0 = default, y MezclaAlpha) y el alfa
// premultiplicado: esos dos REEMPLAZAN el fondo segun el alfa, o sea que si el
// contenido es opaco tapan de verdad y su z sirve (el recorte de una hoja, una
// reja, un cartel).
bool W3dMaterialEsLuz(const Material* m) {
    if (!m || !m->transparent) return false;
    switch (m->mezcla) {
        case w3dEngine::MezclaAdd:       // ONE, ONE                  (aclara)
        case w3dEngine::MezclaAddAlpha:  // SRC_ALPHA, ONE            (aclara)
        case w3dEngine::MezclaMultiply:  // DST_COLOR, ZERO           (oscurece)
        case w3dEngine::MezclaScreen:    // ONE, ONE_MINUS_SRC_COLOR  (aclara)
        case w3dEngine::MezclaSubtract:  // resta                     (oscurece)
            return true;
        default:
            return false;
    }
}

// ===== LA RECETA DE LA CALCOMANIA (ver Materials.h) =====
bool W3dMaterialEsDecal(const Material* m) {
    return m && m->transparent && !m->depth_write && m->depth_bias < 0.0f && m->orden_pasada == 1;
}

void W3dMaterialAplicarDecal(Material* m, bool on, float unidades) {
    if (!m) return;
    if (on) {
        if (unidades == 0.0f) unidades = -4.0f;
        m->transparent  = true;    // el blob es negro con alpha: necesita blend
        m->depth_write  = false;   // LA clave: no escribe z -> no pelea con el piso
        m->depth_bias   = unidades;
        m->orden_pasada = 1;       // opacos -> DECALS -> transparentes
        m->lighting     = false;   // la sombra no se ilumina
    } else {
        // se deshace SOLO la receta. La transparencia se deja como esta: puede ser un
        // alpha comun que el usuario quiere conservar, y adivinarlo seria peor.
        m->depth_write  = true;
        m->depth_bias   = 0.0f;
        m->orden_pasada = 0;
    }
}

// ===================================================
// Implementación de Material animado
// ===================================================
void AnimatedMaterial::Update(float dtSeg) {
    // un material animado mal cargado (listas vacias o de largos distintos) no puede tirar
    // el motor: sin este guard habia division por cero e indexado fuera de rango.
    if (frameTextures.empty() || frameDurations.size() != frameTextures.size()) return;
    // avanza por TIEMPO REAL: a 30 o a 120 fps la textura animada va a la MISMA
    // velocidad. Las duraciones del contenido se calibraron con el loop viejo
    // (~120 updates/seg): ESE es el tick de referencia.
    tickCounter += dtSeg * 120.0f;

    if (tickCounter >= frameDurations[frameIndex]) {
        tickCounter = 0;
        frameIndex = (frameIndex + 1) % frameTextures.size();
        for (size_t t = 0; t < targets.size(); t++) {
            if (targets[t])
                targets[t]->texture = frameTextures[frameIndex];
        }
    }
}

std::vector<AnimatedMaterial*> AnimatedMaterials;

void UpdateAnimatedMaterials(float dtSeg) {
    for (size_t a = 0; a < AnimatedMaterials.size(); a++) {
        if (AnimatedMaterials[a])
            AnimatedMaterials[a]->Update(dtSeg);
    }
}

bool HayAnimacionActiva() {
    // tambien cuentan las animaciones UV "tira de atlas" (Mesh.cpp): el loop
    // event-driven tiene que seguir redibujando mientras haya UVs moviendose.
    extern bool HayUVAnims();
    return !AnimatedMaterials.empty() || HayUVAnims();
}