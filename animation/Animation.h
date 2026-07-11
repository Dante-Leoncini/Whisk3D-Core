#ifndef ANIMATION_H
#define ANIMATION_H

#ifdef _WIN32
    #define NOMINMAX
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <vector>
// reloj de milisegundos de la plataforma (lo provee el EDITOR; el core no
// depende de ninguna libreria de ventana). Ver w3dGetTicks en main.cpp.
unsigned int w3dGetTicks();
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>   // N95: OpenGL ES 1.1 (GLshort/GLbyte/GLfloat)
#else
    #include <GL/gl.h>     // PC: OpenGL de escritorio
#endif
#include <iostream>

#include <iostream>

#include "objects/Objects.h"

// Clases de shape key
class ShapeKeyVertex { 
public:
    int index;
    GLshort vertexX;
    GLshort vertexY;
    GLshort vertexZ;
    GLbyte normalX;
    GLbyte normalY;
    GLbyte normalZ;
};

class ShapeKey { 
public:
    std::vector<ShapeKeyVertex> Vertex;
};

// Clase de animación
class Animation { 
public:
    std::vector<ShapeKey> Frames;
    int MixSpeed;
};

// Variables globales
extern bool PlayAnimation;   // true = reproduciendo (avanza CurrentFrame en cada tick)
extern int AnimPlayDir;      // direccion del play: +1 adelante, -1 en reversa
extern int AnimFPS;          // fps de REPRODUCCION de las animaciones (editable en la pestania Render; default 30).
                             // La UI puede ir a 60 fps: el frame de animacion se repite y NO se recalcula la pose.
extern int StartFrame;
extern int EndFrame;
extern int CurrentFrame;

// avanza CurrentFrame un paso (si PlayAnimation) haciendo loop entre Start..End. Lo llama el main loop
// cada millisecondsPerFrame. Respeta AnimPlayDir (play normal / reversa).
void AnimTick();

extern unsigned int millisecondsPerFrame;
extern int FrameRate;

extern unsigned int lastAnimTime;
extern unsigned int lastRenderTime;

// Funciones de animación
void CalculateMillisecondsPerFrame(int aFPS);

// Constantes de animación
enum { AnimPosition, AnimRotation, AnimScale };

// Keyframe
class keyFrame { 
public:
    int frame;
    GLfloat valueX;
    GLfloat valueY;
    GLfloat valueZ;
    int Interpolation;
};

// Funciones auxiliares
void Swap(keyFrame& a, keyFrame& b);
int Partition(std::vector<keyFrame>& arr, int low, int high);
void QuickSort(std::vector<keyFrame>& arr, int low, int high);
bool compareKeyFrames(const keyFrame& a, const keyFrame& b);

// Propiedad de animación
class AnimProperty { 
public:
    int Property;
    int firstFrameIndex;
    int lastFrameIndex;
    std::vector<keyFrame> keyframes;

    void SortKeyFrames();
};

// Animación de objeto
class AnimationObject { 
public:
    Object* obj; 
    int FirstKeyFrame;
    int LastKeyFrame;
    std::vector<AnimProperty> Propertys;

    void UpdateFirstLastFrame();
};

// Variables globales de objetos animados
extern std::vector<AnimationObject> AnimationObjects;

// Funciones de búsqueda
int BuscarAnimacionObj();
int BuscarAnimProperty(int indice, int propertySelect);
int BuscarShapeKeyAnimation(Object* obj, bool mostrarError);

// Función para recargar animación
void ReloadAnimation();

#endif