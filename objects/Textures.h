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
        // tamano REAL de la imagen (0 = todavia no se sabe). Lo llena el cache al
        // cargar, preguntandole a w3dEngine::TextureSize: es lo unico con lo que se
        // puede CONTAR la memoria de GPU que ocupa el proyecto.
        int  ancho, alto;
        // MEDIDA de la memoria: cuantos materiales/capas la estan usando. Es el
        // refcount del cache por ruta (ver abajo). 0 = nadie la reclamo todavia
        // (las 5 de la UI viven con 0 y ademas estan FIJADAS: no se liberan nunca).
        int  refs;
        bool conMipmaps;   // subida con piramide de mipmaps -> VRAM x ~4/3

        Texture(const std::string& p = "")
            : iID(0), path(p), ancho(0), alto(0), refs(0), conMipmaps(false) {}
};

// vector global COMPARTIDO (PC lo define en Textures.cpp; Symbian en
// platform/symbian/src/w3dtexload.cpp)
extern std::vector<Texture*> Textures;

// ============================================================================
//  CACHE DE TEXTURAS POR RUTA  (TexturaCache.cpp, se compila en los 4 OS)
//
//  EL BUG QUE ARREGLA: cada camino que veia un `map_Kd` hacia `new Texture()` +
//  LoadTexture, sin mirar si esa MISMA ruta ya estaba cargada. Con el proyecto
//  medido (28 trozos de escenario con el mismo atlas de 1 MB, 43 cajas con el
//  mismo atlas...) eso son decenas de copias del mismo PNG en la GPU. El
//  proyecto lo esquivaba con el truco del "ancla" (un solo .mtl declara el
//  map_Kd y los otros 27 comparten el MATERIAL por nombre), que es una muleta.
//
//  Ahora hay UN mapa ruta -> Texture* con REFCOUNT:
//    - TexturaTomar(ruta)   la carga UNA vez; la segunda llamada devuelve la
//                           misma y suma 1 al refcount.
//    - TexturaSoltar(tex)   resta 1; al llegar a 0 libera de verdad (glDeleteTextures
//                           + delete del objeto + baja del vector global).
//    - TexturasLiberarEscena() lo que hace ReiniciarEscena al cerrar un proyecto:
//                           libera TODAS las del proyecto (menos las FIJADAS de la
//                           UI) y deja en NULL los punteros de los materiales, asi
//                           no queda ni un puntero colgado.
//
//  LAS 5 PRIMERAS SON DE LA UI (font/origen/cursor3d/relationshipLine/lamp) y se
//  indexan por POSICION en medio codigo del editor (Textures[0], Textures[3],
//  "Textures[5 + id - 2]" del desplegable de Properties). TexturasFijarBase() se
//  llama justo despues de cargarlas y congela ese prefijo: el cache nunca borra
//  ni reordena por debajo de esa marca.
// ============================================================================

// +1 ref. Si la ruta ya estaba cargada devuelve LA MISMA (sin tocar el disco ni
// la GPU); si no, la carga y la registra. NULL si la ruta esta vacia o falla la
// carga (los fallos se recuerdan para no reintentar 300 veces por frame).
Texture* TexturaTomar(const std::string& path);
// la que ya este cargada con esa ruta, SIN cargar ni contar (NULL si no hay)
Texture* TexturaBuscar(const std::string& path);
// registra una textura YA SUBIDA (creada en memoria) bajo 'path' (vector global + registro, 1 ref)
Texture* TexturaRegistrar(const std::string& path, Texture* t);
// cambia la ruta de una textura registrada (interna <-> externa) sin tocar a quien la usa
void     TexturaRenombrar(Texture* t, const std::string& nuevo);
// +1 ref sobre un puntero que ya se tiene (copiar un material, por ejemplo)
void     TexturaRetener(Texture* t);
// -1 ref. Al llegar a 0 la libera de verdad (GL + objeto + vector global).
void     TexturaSoltar(Texture* t);

// congela las texturas YA cargadas como "de la UI": no se liberan ni se reordenan
void TexturasFijarBase();
int  TexturasBase();               // cuantas quedaron fijadas
// cierre de proyecto: libera todo lo NO fijado y limpia los Texture* de los
// materiales (texture / normalTexture / capas / frames animados). Devuelve
// cuantas libero.
int  TexturasLiberarEscena();

// ---- A/B DEL CACHE (comando 'texcache off|on' del harness) ----
// Con el cache APAGADO, TexturaTomar vuelve a hacer lo que hacia el motor antes
// de este arreglo: una copia nueva en la GPU por cada pedido, aunque la ruta sea
// la misma. Existe para poder MEDIR el antes y el despues con el MISMO binario y
// la misma escena, en una sola corrida (ver tools/pruebas/prueba_texcache.w3s).
// Nunca se apaga solo: default = prendido.
void TexturasCacheActivo(bool on);
bool TexturasCacheEstaActivo();

// ---- MEDICION (comando 'texinfo' del harness) ----
int  TexturasVivas();          // objetos Texture registrados
long TexturasBytes();          // bytes de GPU estimados de las vivas (con mipmaps si los tienen)
long TexturasBytesAhorrados(); // los que NO se subieron gracias al cache: sum((refs-1)*bytes)
int  TexturasReusos();         // veces que TexturaTomar devolvio una ya cargada
int  TexturasCargas();         // veces que TexturaTomar cargo de disco de verdad
long TexturasBytesSubidos();   // bytes ACUMULADOS subidos a la GPU (el pico que se pago)
void TexturasStatsReset();     // pone reusos/cargas/bytes subidos en 0 (para medir un import puntual)

#ifdef W3D_SYMBIAN
// carga sincronica via ICL (w3dtexload.cpp), misma firma que PC
bool LoadTexture(const char* filename, GLuint &textureID);
#else

// carga una textura desde archivo (delega en w3dEngine::LoadTexture, que usa stb)
bool LoadTexture(const char* filename, GLuint &textureID);
#endif // !W3D_SYMBIAN

#endif
