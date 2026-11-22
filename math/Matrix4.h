#ifndef MATRIX4_H
#define MATRIX4_H

#include "Vector3.h"

class Matrix4 {
    public:
        float m[16];

        void Identity() {
            for(int i=0;i<16;i++) m[i] = 0;
            m[0] = m[5] = m[10] = m[15] = 1;
        }

        // Multiplicacion matriz x vector (asumiendo w = 1)
        Vector3 operator*(const Vector3& v) const {
            return Vector3(
                m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12],
                m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13],
                m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]
            );
        }

        // Multiplicacion de matrices
        Matrix4 operator*(const Matrix4& B) const;

        // inversa general 4x4 (cofactores). Devuelve identidad si es singular. La usa el FK del esqueleto
        // para pasar el tail de rest al espacio LOCAL del hueso.
        Matrix4 Inverse() const;

        // inversa de una matriz AFIN (fila inferior [0,0,0,1]): invierte la parte lineal 3x3 por
        // adjugada/determinante y la traslacion como -Ainv*t. Devuelve false si es singular (escala 0),
        // y en ese caso deja out = in (el que llama sigue con una matriz usable en vez de basura).
        // Existe APARTE de Inverse() porque casi toda matriz del engine es afin y esta cuesta ~1/4 de
        // los flops: el N95 no tiene que pagar los cofactores de una 4x4 general para deshacer un T*R*S.
        static bool InvertAffine(const Matrix4& in, Matrix4& out);
};

#endif
