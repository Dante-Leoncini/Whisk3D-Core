#include "Quaternion.h"
#include "Matrix4.h"

Quaternion Quaternion::Slerp(
    const Quaternion& a,
    const Quaternion& b,
    float t
) {
    // Clamp
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;

    // Producto punto
    float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;

    Quaternion end = b;

    // Si el dot es negativo, invertimos uno para tomar
    // el camino corto (MUY IMPORTANTE)
    if (dot < 0.0f) {
        dot = -dot;
        end.x = -end.x;
        end.y = -end.y;
        end.z = -end.z;
        end.w = -end.w;
    }

    // Si estan muy cerca, usar LERP normal (mas barato)
    const float DOT_THRESHOLD = 0.9995f;
    if (dot > DOT_THRESHOLD) {
        Quaternion result;
        result.x = a.x + t * (end.x - a.x);
        result.y = a.y + t * (end.y - a.y);
        result.z = a.z + t * (end.z - a.z);
        result.w = a.w + t * (end.w - a.w);
        return result.Normalized();
    }

    // SLERP real
    float theta0 = acosf(dot);
    float theta  = theta0 * t;

    float sinTheta  = sinf(theta);
    float sinTheta0 = sinf(theta0);

    float s0 = cosf(theta) - dot * sinTheta / sinTheta0;
    float s1 = sinTheta / sinTheta0;

    Quaternion result;
    result.x = (a.x * s0) + (end.x * s1);
    result.y = (a.y * s0) + (end.y * s1);
    result.z = (a.z * s0) + (end.z * s1);
    result.w = (a.w * s0) + (end.w * s1);

    return result;
}

Matrix4 Quaternion::ToMatrix() const {
    // una sola fuente de verdad: la version inline del header llena el float[16]
    Matrix4 m;
    m.Identity();
    ToMatrix(m.m);
    return m;
}

// Metodo de ayuda dentro de la clase Quaternion o utilidades:
Quaternion Quaternion::FromDirection(const Vector3& direction, const Vector3& worldUp) {
    
    // 1. Vector de Direccion (que apunta HASTA DONDE SE MUEVE el personaje)
    Vector3 moveDirection = direction.Normalized();

    // 2. CORRECCION CLAVE: El eje Z local del personaje debe ser opuesto al movimiento.
    Vector3 forward = -moveDirection; // Eje Z local del personaje
    
    // 3. Eje Right (X local)
    // El orden Cross(WorldUp, Forward) asegura que el Roll sea cero.
    // Usamos el 'forward' ya invertido.
    Vector3 right = Vector3::Cross(worldUp, forward).Normalized();
    
    // 4. Eje Up (Y local)
    // Se calcula para ortogonalidad perfecta.
    Vector3 up = Vector3::Cross(forward, right).Normalized();
    
    // 5. Construir Matriz de Orientacion (Positivo [Right | Up | Forward])
    Matrix4 M;
    M.Identity();

    // Columna X = Right
    M.m[0] = right.x; M.m[1] = right.y; M.m[2] = right.z;
    
    // Columna Y = Up
    M.m[4] = up.x; M.m[5] = up.y; M.m[6] = up.z;
    
    // Columna Z = Forward (es el vector -direction)
    M.m[8] = forward.x; M.m[9] = forward.y; M.m[10] = forward.z;

    return Quaternion::FromMatrix(M);
}

Quaternion Quaternion::FromMatrix(const Matrix4& m) {
    Quaternion q;
    float trace = m.m[0] + m.m[5] + m.m[10];

    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        // La rama 'trace' esta bien
        q.x = (m.m[6] - m.m[9]) / s;
        q.y = (m.m[8] - m.m[2]) / s;
        q.z = (m.m[1] - m.m[4]) / s;
    }
    // Rama 1: m.m[0] > m.m[5] y m.m[0] > m.m[10] (Maxima rotacion alrededor de X)
    else if (m.m[0] > m.m[5] && m.m[0] > m.m[10]) {
        float s = sqrtf(1.0f + m.m[0] - m.m[5] - m.m[10]) * 2.0f;
        // CORRECCION: Invertir signo de W para estabilidad
        q.w = (m.m[6] - m.m[9]) / s; // m12 - m21
        q.x = 0.25f * s;
        q.y = (m.m[1]  + m.m[4]) / s;
        q.z = (m.m[2]  + m.m[8]) / s;
    }
    // Rama 2: m.m[5] > m.m[10] (Maxima rotacion alrededor de Y)
    else if (m.m[5] > m.m[10]) {
        float s = sqrtf(1.0f + m.m[5] - m.m[0] - m.m[10]) * 2.0f;
        // CORRECCION: Invertir signo de W para estabilidad
        q.w = (m.m[8] - m.m[2]) / s; // m20 - m02
        q.x = (m.m[1]  + m.m[4]) / s;
        q.y = 0.25f * s;
        q.z = (m.m[6]  + m.m[9]) / s;
    }
    // Rama 3: (Maxima rotacion alrededor de Z)
    else {
        float s = sqrtf(1.0f + m.m[10] - m.m[0] - m.m[5]) * 2.0f;
        q.w = (m.m[1] - m.m[4]) / s; // m10 - m01 (era m[4]-m[1] = signo INVERTIDO -> quaternion mal cerca de Z=180,
                                     // rompia la rotacion de huesos como la clavicula al exportar/reimportar glTF)
        q.x = (m.m[2]  + m.m[8]) / s;
        q.y = (m.m[6]  + m.m[9]) / s;
        q.z = 0.25f * s;
    }

    return q.Normalized();
}

Vector3 Quaternion::ToEulerYXZ() const {
    Vector3 euler;

    // Pitch (X)
    float sinp = 2.0f * (w * x - y * z);
    if (fabs(sinp) >= 1.0f)
        euler.x = (sinp < 0.0f) ? -(float)(M_PI / 2.0) : (float)(M_PI / 2.0);   // sin copysignf (C99): RVCT de Symbian no la trae
    else
        euler.x = asinf(sinp);

    // Yaw (Y)
    float siny = 2.0f * (w * y + x * z);
    float cosy = 1.0f - 2.0f * (x * x + y * y);
    euler.y = atan2f(siny, cosy);

    // Roll (Z)
    float sinr = 2.0f * (w * z + x * y);
    float cosr = 1.0f - 2.0f * (x * x + z * z);
    euler.z = atan2f(sinr, cosr);

    // RAD -> DEG
    const float RAD2DEG = 57.2957795f;
    euler.x *= RAD2DEG;
    euler.y *= RAD2DEG;
    euler.z *= RAD2DEG;

    return euler;
}

Quaternion Quaternion::FromEulerYXZ(float pitchDeg, float yawDeg, float rollDeg){
    float xRad = pitchDeg * (M_PI / 180.0f);
    float yRad = yawDeg  * (M_PI / 180.0f);
    float zRad = rollDeg * (M_PI / 180.0f);

    float cx = cosf(xRad * 0.5f);
    float sx = sinf(xRad * 0.5f);

    float cy = cosf(yRad * 0.5f);
    float sy = sinf(yRad * 0.5f);

    float cz = cosf(zRad * 0.5f);
    float sz = sinf(zRad * 0.5f);

    Quaternion q;
    q.w = cx*cy*cz + sx*sy*sz;
    q.x = sx*cy*cz - cx*sy*sz;
    q.y = cx*sy*cz + sx*cy*sz;
    q.z = cx*cy*sz - sx*sy*cz;

    return q;
}

// Euler XYZ: rotar alrededor de X, luego Y, luego Z. q = Qx*Qy*Qz.
Quaternion Quaternion::FromEulerXYZ(float xDeg, float yDeg, float zDeg){
    Quaternion qx = FromAxisAngle(1, 0, 0, xDeg);
    Quaternion qy = FromAxisAngle(0, 1, 0, yDeg);
    Quaternion qz = FromAxisAngle(0, 0, 1, zDeg);
    return qx * qy * qz;
}

// inverso exacto de FromEulerXYZ (extrae los grados X,Y,Z de R = Rx*Ry*Rz)
Vector3 Quaternion::ToEulerXYZ() const {
    Vector3 e;
    float sy = 2.0f * (x*z + w*y);          // R02 = sin(y)
    if (sy > 1.0f) sy = 1.0f;
    if (sy < -1.0f) sy = -1.0f;
    e.y = asinf(sy);
    if (fabsf(sy) < 0.99999f) {
        e.x = atan2f(2.0f*(w*x - y*z), 1.0f - 2.0f*(x*x + y*y));
        e.z = atan2f(2.0f*(w*z - x*y), 1.0f - 2.0f*(y*y + z*z));
    } else {
        // gimbal lock (y = +-90): X y Z quedan acoplados -> fijamos Z=0
        e.x = atan2f(2.0f*(x*y + w*z), 1.0f - 2.0f*(x*x + z*z));
        e.z = 0.0f;
    }
    const float RAD2DEG = 57.2957795f;
    e.x *= RAD2DEG; e.y *= RAD2DEG; e.z *= RAD2DEG;
    return e;
}

// angulo (grados) + eje normalizado
void Quaternion::ToAxisAngle(float& angleDeg, Vector3& axis) const {
    float ww = w;
    if (ww > 1.0f) ww = 1.0f;
    if (ww < -1.0f) ww = -1.0f;
    float ang = 2.0f * acosf(ww);           // radianes
    float s = sqrtf(1.0f - ww*ww);
    if (s < 1e-5f) {                        // angulo ~0: eje arbitrario
        axis.x = 0.0f; axis.y = 0.0f; axis.z = 1.0f;
    } else {
        axis.x = x / s; axis.y = y / s; axis.z = z / s;
    }
    angleDeg = ang * 57.2957795f;
}
