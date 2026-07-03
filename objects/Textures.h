#ifndef TEXTURES_H
#define TEXTURES_H

#ifdef _WIN32
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <string>
#include <vector>

// Dialecto C++03 compartido (PLAN-UNIFICACION.md). La CLASE Texture es
// comun y el vector global tambien. Los cargadores: SDL/stb en PC/Android,
// ICL sincronico en Symbian (w3dtexload.cpp).
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>
#else
    #include <iostream>
    #include <GL/gl.h>
    #ifndef __ANDROID__
        #include <GL/glu.h>
    #endif
#endif

class Texture {
    public:
        GLuint iID;
        std::string path;

        Texture(const std::string& p = "") : iID(0), path(p) {}
};

// vector global COMPARTIDO (PC lo define en Textures.cpp; Symbian en
// platform/symbian/src/w3dtexload.cpp)
extern std::vector<Texture*> Textures;

#ifdef W3D_SYMBIAN
// carga sincronica via ICL (w3dtexload.cpp), misma firma que PC
bool LoadTexture(const char* filename, GLuint &textureID);
#else

// carga una textura desde archivo (delega en w3dEngine::LoadTexture, que usa stb)
bool LoadTexture(const char* filename, GLuint &textureID);
#endif // !W3D_SYMBIAN

#endif
