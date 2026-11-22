// ============================================================================
//  W3dTilt.cpp — ver W3dTilt.h. Lo comun: prender y apagar (con la config), pasar la muestra del
//  sensor a g y a los ejes de la pantalla. Lo de cada plataforma va en tilt_platform (Symbian:
//  W3dTiltSymbian.cpp; las demas, todavia sin sensor: el #ifndef del final).
// ============================================================================
#include "W3dTilt.h"
#include "W3dConfig.h"
#include <math.h>

namespace w3dEngine {

namespace {
bool gOn = false;
bool gAvailable = true;      // (hasta que se pruebe)
bool gLandscape = false;
// la gravedad en las unidades del sensor (se aprende de las muestras: el N95 no dice cuanto es 1 g)
float gG = 0.0f;
// el signo del eje de arriba: con que lado del telefono para arriba se ve derecha la imagen
float gUp = 1.0f;
float gSmooth[3] = { 0.0f, 0.0f, 0.0f };
unsigned gViews = 0;
}

bool PickedTilt() {
    return ConfigGetInt("incl.activa", 1) != 0;
}

void TiltTurnOn(bool yes) {
    if (yes && !gOn) {
        gOn = tilt_platform::Open();
        gAvailable = gOn;
        gG = 0.0f;
        gViews = 0;
    } else if (!yes && gOn) {
        tilt_platform::Close();
        gOn = false;
    }
    if (gAvailable) {
        ConfigSetInt("incl.activa", yes ? 1 : 0);
        ConfigSave();
    }
}

bool OnTilt() { return gOn; }
bool AvailableTilt() { return gAvailable; }
void TiltSetLandscape(bool yes) { gLandscape = yes; }
const char* TiltInfo() { return gOn ? tilt_platform::Info() : (gAvailable ? "off" : "sin sensor"); }

bool TiltRead(float& x, float& y, float& z) {
    if (!gOn) return false;
    float v[3];
    unsigned n = 0;
    if (!tilt_platform::Raw(v, n) || n == 0) return false;
    if (n != gViews) {
        gViews = n;
        // 1 g: el largo de la muestra, promediado despacio (las sacudidas se van en el promedio)
        const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length > 0.0f) gG = gG <= 0.0f ? length : gG + (length - gG) * 0.02f;
        for (int k = 0; k < 3; k++) gSmooth[k] = gG > 0.0f ? v[k] / gG : 0.0f;
    }
    if (gG <= 0.0f) return false;
    // (las muestras de arriba ya dejaron gSmooth al dia)
    // arriba de la pantalla: el eje largo del telefono (vertical) o el corto (apaisada), con el signo que marca el peso
    // (con un margen: inclinado de costado casi 90 grados no se da vuelta)
    const float a = gLandscape ? gSmooth[0] : gSmooth[1];
    if (a > 0.25f) gUp = 1.0f;
    else if (a < -0.25f) gUp = -1.0f;
    // U = arriba, Z = la normal de la pantalla, R = U x Z (la derecha). x del volante = -(a . R)
    float U[3] = { 0.0f, 0.0f, 0.0f }, R[3];
    if (gLandscape) U[0] = gUp; else U[1] = gUp;
    R[0] = U[1]; R[1] = -U[0]; R[2] = 0.0f;   // (U x (0, 0, 1))
    x = -(gSmooth[0] * R[0] + gSmooth[1] * R[1]);
    y = gSmooth[0] * U[0] + gSmooth[1] * U[1];
    z = gSmooth[2] * gUp;
    return true;
}

bool TiltReadPhone(float& x, float& y, float& z) {
    float a, b, c;
    if (!TiltRead(a, b, c)) return false;   // (actualiza gSmooth con la ultima muestra)
    x = gSmooth[0]; y = gSmooth[1]; z = gSmooth[2];
    return true;
}

#ifndef W3D_SYMBIAN
// (sin sensor en esta plataforma, por ahora)
namespace tilt_platform {
bool Open() { return false; }
void Close() {}
bool Raw(float*, unsigned& n) { n = 0; return false; }
const char* Info() { return "sin sensor"; }
}
#endif

} // namespace w3dEngine
