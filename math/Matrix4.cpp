#include "Matrix4.h"

Matrix4 Matrix4::operator*(const Matrix4& B) const {
    Matrix4 result;

    const float* A = this->m;
    const float* C = B.m;
    float* R = result.m;

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {

            R[col * 4 + row] =
                A[0 * 4 + row] * C[col * 4 + 0] +
                A[1 * 4 + row] * C[col * 4 + 1] +
                A[2 * 4 + row] * C[col * 4 + 2] +
                A[3 * 4 + row] * C[col * 4 + 3];
        }
    }

    return result;
}

// inversa general 4x4 por cofactores (column-major, mismo layout que operator*)
Matrix4 Matrix4::Inverse() const {
    const float* m = this->m;
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    Matrix4 out; out.Identity();
    if (det > -1e-12f && det < 1e-12f) return out; // singular
    float id = 1.0f / det;
    for (int i = 0; i < 16; i++) out.m[i] = inv[i] * id;
    return out;
}

// inversa AFIN (column-major: m[col*4+fila]; m[12..14] = traslacion; fila inferior [0,0,0,1]).
// Vivia como 'static InvertAffine' en el editor (objects/ObjectMode.cpp): el Join y el Apply
// Transform la usaban para pasar de mundo al espacio local de otro nodo. Subio al Core porque
// deshacer el transform del padre es una cuenta del ENGINE, no de la UI, y aca la ve tambien
// el runtime del juego.
bool Matrix4::InvertAffine(const Matrix4& in, Matrix4& out) {
    const float* m = in.m;
    float a=m[0], b=m[4], c=m[8];   // A[fila][col] = m[col*4+fila]
    float d=m[1], e=m[5], f=m[9];
    float g=m[2], h=m[6], i=m[10];
    float det = a*(e*i - f*h) + b*(f*g - d*i) + c*(d*h - e*g);
    if (det > -1e-12f && det < 1e-12f) { out = in; return false; }
    float invDet = 1.0f/det;
    float i00=(e*i-f*h)*invDet, i01=(c*h-b*i)*invDet, i02=(b*f-c*e)*invDet;
    float i10=(f*g-d*i)*invDet, i11=(a*i-c*g)*invDet, i12=(c*d-a*f)*invDet;
    float i20=(d*h-e*g)*invDet, i21=(b*g-a*h)*invDet, i22=(a*e-b*d)*invDet;
    float tx=m[12], ty=m[13], tz=m[14];
    out.m[0]=i00; out.m[1]=i10; out.m[2]=i20; out.m[3]=0;   // out.m[col*4+fila] = Ainv[fila][col]
    out.m[4]=i01; out.m[5]=i11; out.m[6]=i21; out.m[7]=0;
    out.m[8]=i02; out.m[9]=i12; out.m[10]=i22; out.m[11]=0;
    out.m[12]=-(i00*tx + i01*ty + i02*tz);
    out.m[13]=-(i10*tx + i11*ty + i12*tz);
    out.m[14]=-(i20*tx + i21*ty + i22*tz);
    out.m[15]=1;
    return true;
}
