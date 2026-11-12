#include "W3dTexto.h"

#include <cstdio>   // sprintf (C89: sirve en RVCT)
#include <cstring>
#include <cmath>

// ===========================================================================
//  POTENCIAS DE 10 EXACTAS
//  1e0..1e22 son los unicos enteros potencia de 10 REPRESENTABLES EXACTO en
//  double. Escalar por uno de esos (o dividir, para exponentes negativos) da
//  un resultado redondeado UNA sola vez, que es lo que hace falta para que el
//  float32 vuelva exacto. Para exponentes mas grandes se encadena de a 22.
// ===========================================================================
static const double kPot10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

static double W3dPot10Pos(int e) {
    if (e < 0) e = 0;
    double r = 1.0;
    while (e > 22) { r *= 1e22; e -= 22; }
    return r * kPot10[e];
}

// ---------------------------------------------------------------------------
void W3dSaltarBlancos(const char*& p, const char* fin) {
    while (p < fin && (*p == ' ' || *p == '\t')) p++;
}

// ---------------------------------------------------------------------------
bool W3dLeerEntero(const char*& p, const char* fin, int& out) {
    W3dSaltarBlancos(p, fin);
    if (p >= fin) return false;
    const char* ini = p;
    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if (p >= fin || *p < '0' || *p > '9') { p = ini; return false; }
    // acumulacion en double: hasta 2^53 es exacta y no depende de long long
    double acc = 0.0;
    while (p < fin && *p >= '0' && *p <= '9') {
        if (acc < 3e9) acc = acc * 10.0 + (double)(*p - '0');
        p++;
    }
    if (acc > 2000000000.0) acc = 2000000000.0;   // satura: nunca desborda
    int v = (int)acc;
    out = neg ? -v : v;
    return true;
}

// ---------------------------------------------------------------------------
//  FLOAT. Un solo recorrido: mantisa entera acumulada + exponente decimal.
//  Cero allocs, cero llamadas a la libc.
// ---------------------------------------------------------------------------
static bool W3dParsearFloat(const char*& p, const char* fin, float& out) {
    W3dSaltarBlancos(p, fin);
    if (p >= fin) return false;
    const char* ini = p;
    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    double mant = 0.0;
    int    digs = 0;      // digitos significativos ya acumulados
    int    exp10 = 0;     // correccion por los digitos que se descartaron / los decimales
    bool   hayDig = false;

    while (p < fin && *p >= '0' && *p <= '9') {
        hayDig = true;
        if (digs < 18) { mant = mant * 10.0 + (double)(*p - '0'); if (mant > 0.0) digs++; }
        else exp10++;     // digito de mas a la izquierda del limite: solo corre la coma
        p++;
    }
    if (p < fin && *p == '.') {
        p++;
        while (p < fin && *p >= '0' && *p <= '9') {
            hayDig = true;
            if (digs < 18) { mant = mant * 10.0 + (double)(*p - '0'); if (mant > 0.0) digs++; exp10--; }
            p++;
        }
    }
    if (!hayDig) { p = ini; return false; }

    if (p < fin && (*p == 'e' || *p == 'E')) {
        const char* guarda = p;
        p++;
        bool eneg = false;
        if (p < fin && (*p == '+' || *p == '-')) { eneg = (*p == '-'); p++; }
        if (p < fin && *p >= '0' && *p <= '9') {
            int ev = 0;
            while (p < fin && *p >= '0' && *p <= '9') {
                if (ev < 100000) ev = ev * 10 + (*p - '0');
                p++;
            }
            exp10 += eneg ? -ev : ev;
        } else p = guarda;   // "1e" suelto: la 'e' no era exponente
    }

    double r;
    if (mant == 0.0)      r = 0.0;
    else if (exp10 >= 0)  r = mant * W3dPot10Pos(exp10);
    else                  r = mant / W3dPot10Pos(-exp10);

    // acotar antes de bajar a float (evita generar inf desde el archivo)
    if (r >  3.4028235e38) r =  3.4028235e38;
    if (r < -3.4028235e38) r = -3.4028235e38;
    out = neg ? -(float)r : (float)r;
    return true;
}

bool W3dLeerFloat(const char*& p, const char* fin, float& out) {
    return W3dParsearFloat(p, fin, out);
}

float W3dTextoAFloat(const char* s, size_t n) {
    const char* p = s;
    const char* f = s + n;
    float v = 0.0f;
    W3dParsearFloat(p, f, v);
    return v;
}

// ---------------------------------------------------------------------------
bool W3dLeerPalabra(const char*& p, const char* fin, std::string& out) {
    W3dSaltarBlancos(p, fin);
    if (p >= fin) return false;
    const char* ini = p;
    while (p < fin && *p != ' ' && *p != '\t') p++;
    out.assign(ini, (size_t)(p - ini));
    return true;
}

// ---------------------------------------------------------------------------
std::string W3dRestoLinea(const char* p, const char* fin) {
    while (p < fin && (*p == ' ' || *p == '\t')) p++;
    while (fin > p && (*(fin - 1) == ' ' || *(fin - 1) == '\t')) fin--;
    return std::string(p, (size_t)(fin - p));
}

// ---------------------------------------------------------------------------
// UN solo blanco (el separador que escribio el escritor) y de ahi el resto TAL
// CUAL: asi un nombre con espacios en los bordes vuelve EXACTO. Ver el porque en
// W3dTexto.h.
std::string W3dNombreLinea(const char* p, const char* fin) {
    if (p < fin && (*p == ' ' || *p == '\t')) p++;
    if (p >= fin) return std::string();
    return std::string(p, (size_t)(fin - p));
}

// ---------------------------------------------------------------------------
bool W3dLineas::Siguiente(const char*& ini, const char*& finLinea) {
    if (p >= f) return false;
    ini = p;
    const char* q = p;
    while (q < f && *q != '\n') q++;
    finLinea = q;
    if (finLinea > ini && *(finLinea - 1) == '\r') finLinea--;  // CRLF de Windows
    p = (q < f) ? q + 1 : f;
    nro++;
    return true;
}

// ===========================================================================
//  ESCRITURA DE NUMEROS
// ===========================================================================
void W3dEscribirInt(std::string& s, int v) {
    char b[16];
    int n = 0;
    bool neg = false;
    unsigned int u;
    if (v < 0) { neg = true; u = (unsigned int)(-(v + 1)) + 1u; } else u = (unsigned int)v;
    if (u == 0) b[n++] = '0';
    while (u > 0) { b[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; }
    if (neg) s += '-';
    while (n > 0) s += b[--n];
}

void W3dEscribirFloat(std::string& s, float v) {
    // NaN: v != v es el unico test portable sin C99
    if (v != v) { s += '0'; return; }
    if (v >  3.4028235e38f) { s += "3.4e38";  return; }
    if (v < -3.4028235e38f) { s += "-3.4e38"; return; }
    char b[40];
    for (int d = 6; d <= 9; d++) {
        snprintf(b, sizeof(b), "%.*g", d, (double)v);
        if (W3dTextoAFloat(b, strlen(b)) == v) break;   // relee IGUAL con NUESTRO lector
    }
    // el "-0" que emite %g NO se normaliza: los bytes de -0.0f no son los de +0.0f
    // y la clave de sharp/seam compara bytes.
    s += b;
}
