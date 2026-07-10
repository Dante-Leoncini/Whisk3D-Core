#ifndef ARMATURE_H
#define ARMATURE_H

#include "objects/Objects.h"
#include <vector>
#include <string>

// ============================================================================
//  ARMATURE (esqueleto). Objeto INDEPENDIENTE que un mesh 3D usa como padre para
//  deformarse/animarse. En el CORE guarda lo MINIMO que necesita un esqueleto:
//  los huesos (nombre, jerarquia, y su transform de bind = rest pose). El editor
//  lo dibuja: cada hueso = una linea AZUL por encima de todo (ignora el z-buffer).
//  Los pesos por-vertice viven en la MALLA (vertex groups), no aca.
// ============================================================================

// un hueso: nombre + padre + su punta y cola en espacio LOCAL de la armature (rest pose).
struct W3dBone {
    std::string name;
    int parent;        // indice en bones[] (-1 = raiz)
    Vector3 head;      // origen del hueso (rest, local a la armature)
    Vector3 tail;      // punta del hueso (para dibujarlo como palito)
    W3dBone() : parent(-1) {}
};

class Armature : public Object {
    public:
        std::vector<W3dBone> bones;

        Armature(Object* parent = NULL, Vector3 pos = Vector3(0, 0, 0))
            : Object(parent, "Armature", pos) {}

        ObjectType getType() override { return ObjectType::armature; }
        void RenderObject() override; // huesos como lineas AZULES, siempre encima (sin z-test)
};

#endif // ARMATURE_H
