#include "Materials.h"
#include <iostream>

// Variables globales
std::vector<Material*> Materials;
Material* MaterialDefecto = NULL;

// ===================================================
// Implementación de Material
// ===================================================
Material::Material(const std::string& nombre, bool MaterialDefectoFlag, bool TieneVertexColor)
    : textureOn(true), filtrado(true), transparent(false), vertexColor(false), lighting(true), repeat(true),
      uv8bit(false), culling(true), depth_test(true), chrome(false), reflectMode(0),
      normalMap(false), normalTexture(NULL),
      interpolacion(0), texture(NULL), shininess(12.0f) { // interpolacion 0 = lineal (suave), 1 = closest (pixel); igual en TODOS los sistemas
    // defaults que eran inicializadores de clase (C++11)
    diffuse[0] = diffuse[1] = diffuse[2] = diffuse[3] = 1.0f;
    specular[0] = specular[1] = specular[2] = 0.3f; specular[3] = 1.0f;
    emission[0] = emission[1] = emission[2] = 0.0f; emission[3] = 1.0f;
    ambient[0] = ambient[1] = ambient[2] = 0.3f; ambient[3] = 1.0f; // gris (como el preview)
    name = nombre;
    if (!MaterialDefectoFlag){
        Materials.push_back(this);
    }
    vertexColor = TieneVertexColor;
    // interpolacion ya quedo en 'lineal' (0) por la lista de inicializacion, igual en todos los
    // sistemas. (Antes Symbian la ponia en 1 = closest y PC en 0 = lineal: quedaban distintas.)
}

Material::~Material() {}

// ===================================================
// Funciones auxiliares
// ===================================================
Material* BuscarMaterialPorNombre(const std::string& name) {
    for (size_t i = 0; i < Materials.size(); ++i){
        if (Materials[i]->name == name) return Materials[i];
    }
    return NULL;
}

// ===================================================
// Implementación de Material animado
// ===================================================
void AnimatedMaterial::Update() {
    tickCounter++;

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

void UpdateAnimatedMaterials() {
    for (size_t a = 0; a < AnimatedMaterials.size(); a++) {
        if (AnimatedMaterials[a])
            AnimatedMaterials[a]->Update();
    }
}

bool HayAnimacionActiva() {
    return !AnimatedMaterials.empty();
}