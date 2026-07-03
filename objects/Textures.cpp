#include "Textures.h"
#include "w3dTexture.h" // cargador UNIVERSAL del motor (engine/)

std::vector<Texture*> Textures;

bool LoadTexture(const char* filename, GLuint &textureID) {
    // delega en el cargador UNIVERSAL del motor (engine/w3dTexture, que usa stb)
    unsigned int id = 0;
    if (!w3dEngine::LoadTexture(filename, id)) {
        return false;
    }
    textureID = (GLuint)id;
    return true;
}