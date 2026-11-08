#include "W3dVideoTex.h"
#include "../io/w3dCompress.h"
#include "../gfx/w3dTexture.h"
#include "../base/w3dlog.h"
#include <stdlib.h>
#include <string.h>

namespace w3dEngine {

namespace {
unsigned int LeerU16(const unsigned char* p) { return (unsigned int)p[0] | ((unsigned int)p[1] << 8); }
unsigned int LeerU32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
float LeerF32(const unsigned char* p) {
    const unsigned int u = LeerU32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
bool Pot(int n) { return n > 0 && (n & (n - 1)) == 0; }
}

W3dVideoTex::W3dVideoTex()
    : archivo(0), ancho(0), alto(0), clave(1), cuadros(0), fps(15.0f), tabla(0), px(0), tmp(0), leido(0), leidoMax(0),
      decod(-1), actual(-1), visible(0), nDecod(0), nSubidos(0), bytesLeidos(0) {
    ids[0] = ids[1] = 0;
}

W3dVideoTex::~W3dVideoTex() { Cerrar(); }

void W3dVideoTex::Cerrar() {
    if (archivo) fclose(archivo);
    archivo = 0;
    for (int i = 0; i < 2; i++) {
        if (ids[i]) DeleteTexture(ids[i]);
        ids[i] = 0;
    }
    free(tabla);
    free(px);
    free(tmp);
    free(leido);
    tabla = 0;
    px = tmp = 0;
    leido = 0;
    leidoMax = 0;
    ancho = alto = cuadros = 0;
    clave = 1;
    decod = actual = -1;
    visible = 0;
    nDecod = nSubidos = 0;
    bytesLeidos = 0;
}

bool W3dVideoTex::Abrir(const char* ruta) {
    Cerrar();
    if (!ruta) return false;
    FILE* f = fopen(ruta, "rb");
    if (!f) return false;
    unsigned char cab[32];
    if (fread(cab, 1, 32, f) != 32 || memcmp(cab, "W3DV", 4) != 0 || LeerU16(cab + 4) != 1 || LeerU16(cab + 6) != 1) {
        w3dLogf("W3dVideoTex: %s no es un video RGB565 (W3DV 1)", ruta);
        fclose(f);
        return false;
    }
    const int w = (int)LeerU16(cab + 8), h = (int)LeerU16(cab + 10);
    const int n = (int)LeerU32(cab + 20);
    if (!Pot(w) || !Pot(h) || w > 512 || h > 512 || n <= 0 || n > 1000000) {
        w3dLogf("W3dVideoTex: %s: %dx%d con %d cuadros (los lados van en potencias de dos)", ruta, w, h, n);
        fclose(f);
        return false;
    }
    unsigned char* t = (unsigned char*)malloc((size_t)(n + 1) * 4);
    tabla = (unsigned int*)malloc((size_t)(n + 1) * sizeof(unsigned int));
    px = (unsigned short*)malloc((size_t)w * h * 2);
    tmp = (unsigned short*)malloc((size_t)w * h * 2);
    if (!t || !tabla || !px || !tmp || fread(t, 4, (size_t)(n + 1), f) != (size_t)(n + 1)) {
        free(t);
        archivo = f;
        Cerrar();
        w3dLogf("W3dVideoTex: %s cortado o sin memoria", ruta);
        return false;
    }
    for (int k = 0; k <= n; k++) tabla[k] = LeerU32(t + k * 4);
    free(t);
    archivo = f;
    ancho = w;
    alto = h;
    clave = (int)LeerU16(cab + 12);
    if (clave < 1) clave = 1;
    fps = LeerF32(cab + 16);
    if (!(fps > 0.0f)) fps = 15.0f;
    cuadros = n;
    // las dos texturas con el primer cuadro (la que se escribe despues tambien: si se ve antes de tiempo, no es basura)
    memset(px, 0, (size_t)w * h * 2);
    Decodificar(0);
    for (int i = 0; i < 2; i++) ids[i] = Reservar565(0, w, h, px);
    visible = 0;
    actual = 0;
    if (!ids[0] || !ids[1]) {
        w3dLogf("W3dVideoTex: %s: no se pudieron crear las texturas", ruta);
        Cerrar();
        return false;
    }
    return true;
}

bool W3dVideoTex::Decodificar(int k) {
    if (!archivo || k < 0 || k >= cuadros) return false;
    const unsigned int desde = tabla[k], hasta = tabla[k + 1];
    if (hasta < desde + 4) return false;
    const int largo = (int)(hasta - desde);
    if (largo > leidoMax) {
        unsigned char* nuevo = (unsigned char*)realloc(leido, (size_t)largo);
        if (!nuevo) return false;
        leido = nuevo;
        leidoMax = largo;
    }
    if (fseek(archivo, (long)desde, SEEK_SET) != 0 || fread(leido, 1, (size_t)largo, archivo) != (size_t)largo) return false;
    bytesLeidos += largo;
    const int tipo = leido[0];
    const int bytes = ancho * alto * 2;
    if (tipo == 2) {
        // (igual al anterior: si el anterior es el que esta, no hay nada que hacer)
        if (decod != k - 1) return false;
    } else if (tipo == 0) {
        if (!Inflate(leido + 4, largo - 4, (unsigned char*)px, bytes)) return false;
    } else if (tipo == 1) {
        if (decod != k - 1) return false;
        if (!Inflate(leido + 4, largo - 4, (unsigned char*)tmp, bytes)) return false;
        // (de a 32 bits: las filas son pares de pixeles)
        unsigned int* a = (unsigned int*)px;
        const unsigned int* b = (const unsigned int*)tmp;
        for (int i = bytes / 4; i > 0; i--) *a++ ^= *b++;
    } else {
        return false;
    }
    decod = k;
    nDecod++;
    return true;
}

unsigned int W3dVideoTex::IrCuadro(int k) {
    if (!archivo) return 0;
    if (k < 0) k = 0;
    if (k >= cuadros) k = cuadros - 1;
    if (k == actual) return ids[visible];
    // de donde se arranca: el que esta si viene antes y no hay un entero mas cerca, si no el entero de k
    const int entero = (k / clave) * clave;
    int desde;
    if (decod >= 0 && decod <= k && decod >= entero) desde = decod + 1;
    else desde = entero;
    for (int i = desde; i <= k; i++) {
        if (!Decodificar(i)) {
            // (algo fallo: se rearma desde el entero; si ese tampoco, queda lo que se ve)
            decod = -1;
            if (i == entero || !Decodificar(entero)) return ids[visible];
            for (int j = entero + 1; j <= k; j++)
                if (!Decodificar(j)) return ids[visible];
            break;
        }
    }
    if (decod != k) return ids[visible];
    const int otra = 1 - visible;
    if (Actualizar565(ids[otra], ancho, alto, px)) {
        visible = otra;
        nSubidos++;
    }
    actual = k;
    return ids[visible];
}

unsigned int W3dVideoTex::Ir(float t) {
    if (!archivo) return 0;
    const int k = t <= 0.0f ? 0 : (int)(t * fps + 0.0001f);
    return IrCuadro(k);
}

}
