// ============================================================================
//  W3dInclinacion.cpp — ver W3dInclinacion.h. Lo comun: prender y apagar (con la config), pasar la muestra del
//  sensor a g y a los ejes de la pantalla. Lo de cada plataforma va en inclinacion_plataforma (Symbian:
//  W3dInclinacionSymbian.cpp; las demas, todavia sin sensor: el #ifndef del final).
// ============================================================================
#include "W3dInclinacion.h"
#include "W3dConfig.h"
#include <math.h>

namespace w3dEngine {

namespace {
bool gPrendida = false;
bool gDisponible = true;      // (hasta que se pruebe)
bool gApaisada = false;
// la gravedad en las unidades del sensor (se aprende de las muestras: el N95 no dice cuanto es 1 g)
float gG = 0.0f;
// el signo del eje de arriba: con que lado del telefono para arriba se ve derecha la imagen
float gArriba = 1.0f;
float gSuave[3] = { 0.0f, 0.0f, 0.0f };
unsigned gVistas = 0;
}

bool InclinacionElegida() {
    return ConfigGetInt("incl.activa", 1) != 0;
}

void InclinacionPrender(bool si) {
    if (si && !gPrendida) {
        gPrendida = inclinacion_plataforma::Abrir();
        gDisponible = gPrendida;
        gG = 0.0f;
        gVistas = 0;
    } else if (!si && gPrendida) {
        inclinacion_plataforma::Cerrar();
        gPrendida = false;
    }
    if (gDisponible) {
        ConfigSetInt("incl.activa", si ? 1 : 0);
        ConfigSave();
    }
}

bool InclinacionPrendida() { return gPrendida; }
bool InclinacionDisponible() { return gDisponible; }
void InclinacionSetApaisada(bool si) { gApaisada = si; }
const char* InclinacionInfo() { return gPrendida ? inclinacion_plataforma::Info() : (gDisponible ? "apagada" : "sin sensor"); }

bool InclinacionLeer(float& x, float& y, float& z) {
    if (!gPrendida) return false;
    float v[3];
    unsigned n = 0;
    if (!inclinacion_plataforma::Cruda(v, n) || n == 0) return false;
    if (n != gVistas) {
        gVistas = n;
        // 1 g: el largo de la muestra, promediado despacio (las sacudidas se van en el promedio)
        const float largo = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (largo > 0.0f) gG = gG <= 0.0f ? largo : gG + (largo - gG) * 0.02f;
        for (int k = 0; k < 3; k++) gSuave[k] = gG > 0.0f ? v[k] / gG : 0.0f;
    }
    if (gG <= 0.0f) return false;
    // arriba de la pantalla: el eje largo del telefono (vertical) o el corto (apaisada), con el signo que marca el peso
    // (con un margen: inclinado de costado casi 90 grados no se da vuelta)
    const float a = gApaisada ? gSuave[0] : gSuave[1];
    if (a > 0.25f) gArriba = 1.0f;
    else if (a < -0.25f) gArriba = -1.0f;
    // U = arriba, Z = la normal de la pantalla, R = U x Z (la derecha). x del volante = -(a . R)
    float U[3] = { 0.0f, 0.0f, 0.0f }, R[3];
    if (gApaisada) U[0] = gArriba; else U[1] = gArriba;
    R[0] = U[1]; R[1] = -U[0]; R[2] = 0.0f;   // (U x (0, 0, 1))
    x = -(gSuave[0] * R[0] + gSuave[1] * R[1]);
    y = gSuave[0] * U[0] + gSuave[1] * U[1];
    z = gSuave[2] * gArriba;
    return true;
}

#ifndef W3D_SYMBIAN
// (sin sensor en esta plataforma, por ahora)
namespace inclinacion_plataforma {
bool Abrir() { return false; }
void Cerrar() {}
bool Cruda(float*, unsigned& n) { n = 0; return false; }
const char* Info() { return "sin sensor"; }
}
#endif

} // namespace w3dEngine
