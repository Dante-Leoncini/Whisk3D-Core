#include "Materials.h"
#include "W3dNames.h"     // nombres unicos (misma regla que objetos/huesos/grupos)
#include "w3dGraphics.h"  // w3dEngine::Mezcla*: los codigos de mezcla (W3dMaterialIsLight)
#include <math.h>         // cosf / sinf: la matriz de la transformacion UV

Material* FactoryMaterial(Material* m) {
    if (m) m->perPixel = w3dEngine::ShadingPerPixelSupported();
    return m;
}

// Variables globales
std::vector<Material*> Materials;
Material* MaterialDefault = NULL;

// ---- MARCA BASE del registro de materiales (ver el comentario de Materials.h) ----
static size_t gMatBase = 0;
static bool   gMatBaseFixed = false;

void MaterialsSetBase() {
    if (gMatBaseFixed) return;   // una sola vez: la UI se carga en el arranque
    gMatBase = Materials.size();
    gMatBaseFixed = true;
}
int MaterialsBase() { return (int)gMatBase; }

// el CEMENTERIO de los materiales purgados (ver MaterialPurge en el .h)
static std::vector<Material*> gPurgedMat;

bool MaterialPurge(Material* m) {
    if (!m || m == MaterialDefault) return false;
    for (size_t i = gMatBase; i < Materials.size(); i++) {
        if (Materials[i] != m) continue;
        Materials.erase(Materials.begin() + (long)i);
        gPurgedMat.push_back(m);
        return true;
    }
    return false;   // de la UI (debajo de la marca) o ya purgado
}

bool MaterialRevive(Material* m) {
    for (size_t i = 0; i < gPurgedMat.size(); i++) {
        if (gPurgedMat[i] != m) continue;
        gPurgedMat.erase(gPurgedMat.begin() + (long)i);
        // mientras estuvo afuera alguien pudo tomar su nombre: vuelve con uno libre
        m->name = MaterialFreeName(m->name, NULL);
        Materials.push_back(m);
        return true;
    }
    return false;
}

const std::vector<Material*>& MaterialsPurged() { return gPurgedMat; }

int MaterialsFreeScene() {
    int n = 0;
    while (Materials.size() > gMatBase) {
        Material* m = Materials.back();
        Materials.pop_back();
        // MaterialDefault no entra a la lista (ver el ctor), pero se chequea igual:
        // liberarlo dejaria colgado al fallback de TODAS las mallas sin material.
        if (m && m != MaterialDefault) { delete m; n++; }
    }
    // los PURGADOS tambien son del proyecto (el undo que los podia nombrar ya se limpio)
    for (size_t i = 0; i < gPurgedMat.size(); i++)
        if (gPurgedMat[i] && gPurgedMat[i] != MaterialDefault) { delete gPurgedMat[i]; n++; }
    gPurgedMat.clear();
    // los materiales ANIMADOS apuntan a materiales del proyecto (targets): si
    // quedaran, su Update() escribiria sobre memoria liberada en el proyecto nuevo.
    for (size_t i = 0; i < AnimatedMaterials.size(); i++) delete AnimatedMaterials[i];
    AnimatedMaterials.clear();
    return n;
}

// ===================================================
// Implementación de Material
// ===================================================
Material::Material(const std::string& newName, bool MaterialDefaultFlag, bool HasVertexColor)
    : textureOn(true), filtered(true), transparent(false), vertexColor(false), lighting(true), repeat(true),
      uv8bit(false), culling(true), depth_test(true), background(false),
      depth_write(true), depth_bias(0.0f), bias_meters(0.0f), order_pass(0), mix(0), alphaTest(0.0f),
      rtRoughness(0.5f), rtMetallic(0.0f),
      chrome(false), reflectMode(0), reflectionProbe(false),
      normalMap(false), normalTexture(NULL), textureB(NULL), mixB(0.0f), toon(0), outline(0.0f),
      perPixel(false), lightIndirect(false), lines(false), thicknessLine(1.0f),
      interpolation(0), texture(NULL), shininess(12.0f) { // interpolacion 0 = lineal (suave), 1 = closest (pixel); igual en TODOS los sistemas
    // defaults que eran inicializadores de clase (C++11)
    diffuse[0] = diffuse[1] = diffuse[2] = diffuse[3] = 1.0f;
    specular[0] = specular[1] = specular[2] = 0.3f; specular[3] = 1.0f;
    emission[0] = emission[1] = emission[2] = 0.0f; emission[3] = 1.0f;
    ambient[0] = ambient[1] = ambient[2] = 0.3f; ambient[3] = 1.0f; // gris (como el preview)
    for (int k = 0; k < W3D_MAPS; k++) { mapTex[k] = NULL; mapWithTexture[k] = false; uvMap[k] = 0; }
    ior = 1.0f;   // (1 = sin refraccion: lo transparente deja ver lo de atras derecho; 1.33 agua, 1.5 vidrio)
    transmission = 0.0f;
    ssr = false;   // (los reflejos en pantalla: apagados)
    // la fuerza de la emision: 1 (un material de siempre se ve igual)
    emissionStrength = 1.0f;
    { static unsigned sSerial = 0; serial = ++sSerial; }   // (no se recicla: ver Material::serial)
    posedByCurves = false;
    shaderOn = false; shaderId = 0; shaderState = 0; shaderScreen = false; shaderTime = false; shaderCPU = NULL;
    if (!MaterialDefaultFlag){
        // los materiales del proyecto son GLOBALES y unicos: el ctor es el unico
        // punto de alta (se auto-registra), asi que uniquificar aca cubre el
        // "New Material" del panel, los importadores y la carga de los .glb.
        // El material POR DEFECTO no entra a la lista -> no participa.
        name = MaterialFreeName(newName, NULL);
        Materials.push_back(this);
    } else {
        name = newName;
    }
    vertexColor = HasVertexColor;
    // interpolacion ya quedo en 'lineal' (0) por la lista de inicializacion, igual en todos los
    // sistemas. (Antes Symbian la ponia en 1 = closest y PC en 0 = lineal: quedaban distintas.)
}

Material::~Material() {}

static const char* const kMapKeys[W3D_MAPS] = { "base", "normal", "emission", "roughness", "metallic", "alpha" };
const char* W3dMatMapKey(int m) { return (m >= 0 && m < W3D_MAPS) ? kMapKeys[m] : ""; }
int W3dMatMapByKey(const std::string& c) {
    for (int k = 0; k < W3D_MAPS; k++) if (c == kMapKeys[k]) return k;
    return -1;
}
Texture** Material::MapSlot(int m) {
    if (m == W3D_MAP_BASE) return &texture;
    if (m == W3D_MAP_NORMAL) return &normalTexture;
    if (m > W3D_MAP_NORMAL && m < W3D_MAPS) return &mapTex[m];
    return NULL;
}
Texture* Material::MapTexture(int m) const {
    if (m == W3D_MAP_BASE) return texture;
    if (m == W3D_MAP_NORMAL) return normalTexture;
    if (m > W3D_MAP_NORMAL && m < W3D_MAPS) return mapTex[m];
    return NULL;
}

const char* W3dLayerMixName(int m) {
    switch (m) {
        case W3D_LAYER_MULTIPLY:   return "Multiply";
        case W3D_LAYER_ADD:        return "Add";
        case W3D_LAYER_MULTIPLY2X: return "Multiply 2x";
        case W3D_LAYER_SUBTRACT:   return "Subtract";
        case W3D_LAYER_SCREEN:     return "Screen";
        default:                  return "Mix";
    }
}

// ===================================================
// Funciones auxiliares
// ===================================================
// ===== NOMBRES UNICOS de material (espacio GLOBAL de proyecto) =====
// 'excepto' = el material que puede conservar su nombre (rename); NULL = ninguno.
static bool MatNameExists(const std::string& n, void* ctx) {
    const Material* except = (const Material*)ctx;
    for (size_t i = 0; i < Materials.size(); ++i)
        if (Materials[i] != except && Materials[i]->name == n) return true;
    return false;
}
std::string MaterialFreeName(const std::string& base, const Material* except) {
    return W3dUniqueName(base, "Material", MatNameExists, (void*)except);
}
void Material::SetName(const std::string& newName) {
    name = MaterialFreeName(newName, this);
}

Material* FindMaterialByName(const std::string& name) {
    for (size_t i = 0; i < Materials.size(); ++i){
        if (Materials[i]->name == name) return Materials[i];
    }
    return NULL;
}

// ===== MATERIAL "LUZ": MODULA EL FRAMEBUFFER, NO LO TAPA (ver Materials.h) =====
// Se pregunta por la MEZCLA, no por el nombre del material: cualquier modo que
// componga con lo que YA HAY (sumar, restar, multiplicar, screen) describe luz.
// Quedan afuera a proposito el alfa comun (0 = default, y MixAlpha) y el alfa
// premultiplicado: esos dos REEMPLAZAN el fondo segun el alfa, o sea que si el
// contenido es opaco tapan de verdad y su z sirve (el recorte de una hoja, una
// reja, un cartel).
bool W3dMaterialIsLight(const Material* m) {
    if (!m || !m->transparent) return false;
    switch (m->mix) {
        case w3dEngine::MixAdd:       // ONE, ONE                  (aclara)
        case w3dEngine::MixAddAlpha:  // SRC_ALPHA, ONE            (aclara)
        case w3dEngine::MixMultiply:  // DST_COLOR, ZERO           (oscurece)
        case w3dEngine::MixScreen:    // ONE, ONE_MINUS_SRC_COLOR  (aclara)
        case w3dEngine::MixSubtract:  // resta                     (oscurece)
            return true;
        default:
            return false;
    }
}

// ===== LA RECETA DE LA CALCOMANIA (ver Materials.h) =====
bool W3dMaterialIsDecal(const Material* m) {
    return m && m->transparent && !m->depth_write && m->depth_bias < 0.0f && m->order_pass == 1;
}

void W3dMaterialApplyDecal(Material* m, bool on, float units) {
    if (!m) return;
    if (on) {
        if (units == 0.0f) units = -4.0f;
        m->transparent = true;    // el blob es negro con alpha: necesita blend
        m->depth_write = false;   // LA clave: no escribe z -> no pelea con el piso
        m->depth_bias  = units;
        m->order_pass  = 1;       // opacos -> DECALS -> transparentes
        m->lighting    = false;   // la sombra no se ilumina
    } else {
        // se deshace SOLO la receta. La transparencia se deja como esta: puede ser un
        // alpha comun que el usuario quiere conservar, y adivinarlo seria peor.
        m->depth_write = true;
        m->depth_bias  = 0.0f;
        m->order_pass  = 0;
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
        // la RANURA base del material es duena de su referencia (Textures.h: TextureSetIn), tambien la del cuadro
        // que muestra: retiene el nuevo y suelta el anterior. Asi un undo o un desplegable que la cambian, o el
        // streaming que la duerme, sueltan algo suyo y nunca el cuadro del material animado
        for (size_t t = 0; t < targets.size(); t++) {
            if (targets[t])
                TextureSetIn(targets[t]->texture, frameTextures[frameIndex]);
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

bool HasAnimationActive() {
    // tambien cuentan las animaciones UV "tira de atlas" (Mesh.cpp): el loop
    // event-driven tiene que seguir redibujando mientras haya UVs moviendose.
    extern bool HasUVAnims();
    extern bool W3dShaderHasAnimated();   // (un shader escrito que usa w3d_time se mueve solo)
    return !AnimatedMaterials.empty() || HasUVAnims() || W3dShaderHasAnimated();
}