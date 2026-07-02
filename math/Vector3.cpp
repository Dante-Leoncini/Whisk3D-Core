#include "Vector3.h"

Vector3 ClosestPointOnSegment(const Vector3& A, const Vector3& B, const Vector3& P, float& tOut){
    Vector3 AB = B - A;
    float abLenSq = AB.LengthSq();
    if (abLenSq == 0.0f) {
        tOut = 0.0f;
        return A;
    }

    tOut = (P - A).Dot(AB) / abLenSq;
    // clamp sin <algorithm>: compila en RVCT (C++03) y PC por igual
    if (tOut < 0.0f) tOut = 0.0f;
    if (tOut > 1.0f) tOut = 1.0f;

    return A + AB * tOut;
}
