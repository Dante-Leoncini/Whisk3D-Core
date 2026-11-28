#ifndef EMPTY_H
#define EMPTY_H
#include "objects/Objects.h"

// COMO SE DIBUJA un Vacio. Es SOLO la vista: no cambia el objeto, ni su transformacion, ni a sus hijos.
// (Pedido del duenio, 2026-11-09: "se puede cambiar la forma a cruz, cuadrado, circulo o esfera con 3 lineas... esto
// solo es como se ve".) Los numeros viajan en el .w3d ("forma"): no cambiar el orden.
// IMAGEN (al final, 2026-11-27): la REFERENCIA para modelar encima (Add > Reference, como el "Image Empty" de Blender).
// No es una malla (no va a "Mallas 3D" de la biblioteca ni al render): una imagen con su proporcion, acostada en el X-Y
// del objeto (como Blender: Add > Reference la crea girada 90 en X, parada mirando al frente), que se ve en TODOS los
// modos de vista -tambien en alambre, para calcar encima-
enum { EmptyCross = 0, EmptySquare, EmptyCircle, EmptySphere, EmptyCube, EmptyImage, EmptyShapes };   // (los nuevos, al final)

class Texture;

// objeto vacio (eje/pivote), compartido PC/Symbian
class Empty : public Object {
public:
    // nace CIRCULO acostado en el piso: se ve mejor que la cruz (la cruz se perdia contra la grilla)
    int     shape;          // EmptyCross / EmptySquare / EmptyCircle / EmptySphere / EmptyCube / EmptyImage
    float   drawingSize;    // el tamanio del dibujo: 1 = 1 metro de punta a punta (la cruz de siempre). La IMAGEN: su
                            // lado largo
    Vector3 rotDrawing;     // el giro del dibujo, en grados, como Propiedades (X, Y, Z con Z arriba)
    // LA IMAGEN (forma EmptyImage). El Vacio retiene su textura (+1 ref) y la suelta al cambiarla o al borrarse
    Texture* image;         // NULL = sin imagen: se dibuja el marco solo
    float    imageOpacity;  // 0..1 (por debajo de 1 se ve lo de atras y no tapa la profundidad)
    int      imageW, imageH;   // el tamano de la imagen (del encabezado del archivo, que un telefono no estira a potencia
                               // de 2); 0 = el de la textura

    Empty(Object* parent = NULL, Vector3 pos = Vector3(0,0,0))
        : Object(parent, "Empty", pos), shape(EmptyCircle), drawingSize(1.0f), rotDrawing(0, 0, 0),
          image(NULL), imageOpacity(1.0f), imageW(0), imageH(0) {
    }
    ~Empty();
    ObjectType getType() W3D_OVERRIDE { return ObjectType::empty; }
    void RenderObject() W3D_OVERRIDE;
    bool DrawsSomething() W3D_OVERRIDE;   // su dibujo, solo con el overlay "Empty"
    void CopyDrawing(const Empty* o);
    // pone la imagen (retiene la nueva y suelta la anterior). w/h: el tamano de la imagen (0 = el de la textura)
    void SetImage(Texture* t, int w = 0, int h = 0);
    // la imagen si sigue cargada: cerrar el proyecto libera TODAS las texturas (TexturesFreeScene) y la del Vacio puede
    // haberse ido antes que el. NULL = no hay
    Texture* ImageAlive() const;
    // los medios lados del cuadro de la imagen (en metros, con el tamanio del dibujo): ancho y alto
    void ImageHalfSize(float& hw, float& hh) const;
    // las 4 esquinas de la imagen en el espacio LOCAL del objeto (12 floats: abajo-izq, abajo-der, arriba-izq,
    // arriba-der). Las usa el dibujo y el pick (clickear la imagen la elige)
    void ImageCorners(float* out12) const;
};
#endif
