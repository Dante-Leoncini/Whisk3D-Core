// ============================================================================
//  W3dConfig.cpp — implementacion de la configuracion persistente. Ver W3dConfig.h.
//
//  Una sola tabla clave=valor en memoria y DOS backends de almacenamiento:
//    · WEB: localStorage (una entrada con todo el texto). Sobrevive a recargar
//      la pagina y es lo unico persistente que garantiza el navegador sin pedir
//      permisos. Si el usuario navega en privado y falla, se degrada en silencio.
//    · RESTO: un archivo de texto (fopen/fwrite): sirve igual en desktop, en
//      Symbian y en consolas. Sin memoria dinamica ni STL: C++03 puro.
// ============================================================================
#include "W3dConfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
    #include <emscripten.h>
// localStorage: guardar / leer una sola entrada de texto. Si el navegador lo
// bloquea (modo privado, cookies off) devuelve 0 y la app sigue con los defaults.
EM_JS(void, w3dCfgJS_Set, (const char* k, const char* v), {
    try { localStorage.setItem(UTF8ToString(k), UTF8ToString(v)); } catch (e) {}
});
EM_JS(char*, w3dCfgJS_Get, (const char* k), {
    var s = null;
    try { s = localStorage.getItem(UTF8ToString(k)); } catch (e) {}
    if (s === null) return 0;
    var n = lengthBytesUTF8(s) + 1, p = _malloc(n);
    stringToUTF8(s, p, n);
    return p;
});
#endif

#include "io/w3dFilesystem.h"   // GetUserDataDir(): carpeta ESCRIBIBLE (en Android/desktop) donde va el config

namespace w3dEngine {

struct Entrada { char k[W3D_CFG_KEY]; char v[W3D_CFG_VAL]; };

static Entrada gTab[W3D_CFG_MAX];
static int     gN = 0;
static char    gNombre[32] = "w3dconfig";
static bool    gMudo = false;   // mute global (ver ConfigSetMudo/ConfigMudo)

static void copiar(char* dst, int max, const char* src) {
    int i = 0;
    if (src) { for (; src[i] && i < max - 1; i++) dst[i] = src[i]; }
    dst[i] = 0;
}

static int buscar(const char* clave) {
    for (int i = 0; i < gN; i++) if (strcmp(gTab[i].k, clave) == 0) return i;
    return -1;
}

#ifndef __EMSCRIPTEN__
// Arma en 'out' la ruta del archivo de config. Normalmente ABSOLUTA:
// "<carpeta escribible>/<gNombre>.txt" (GetUserDataDir da esa carpeta en Android/desktop; en
// Android el CWD es "/" y no es escribible, por eso hay que anteponerla). En Symbian
// GetUserDataDir viene VACIO -> se cae a la ruta RELATIVA vieja "<gNombre>.txt" (no rompe ese port).
static void rutaCfg(char* out, int max) {
    const std::string& dir = w3dFileSystem::GetUserDataDir();
    if (dir.empty()) { snprintf(out, (size_t)max, "%s.txt", gNombre); return; }
    const char* d = dir.c_str();
    int n = 0;
    while (d[n] && n < max - 1) { out[n] = d[n]; n++; }             // copia la carpeta (buffer transitorio)
    if (n > 0 && out[n - 1] != '/' && n < max - 1) out[n++] = '/';  // separador si no lo trae
    out[n] = 0;
    snprintf(out + n, (size_t)(max - n), "%s.txt", gNombre);        // agrega "<nombre>.txt"
}
#endif

void ConfigSetNombre(const char* nombre) { copiar(gNombre, (int)sizeof(gNombre), nombre); }
void ConfigClear() { gN = 0; }

// MUTE GLOBAL: el beep() del motor lo consulta antes de sonar (ver W3dScript.cpp).
void ConfigSetMudo(bool mudo) { gMudo = mudo; }
bool ConfigMudo() { return gMudo; }

void ConfigSetStr(const char* clave, const char* valor) {
    if (!clave || !*clave) return;
    int i = buscar(clave);
    if (i < 0) {
        if (gN >= W3D_CFG_MAX) return;          // tabla llena: se ignora (no rompe)
        i = gN++;
        copiar(gTab[i].k, W3D_CFG_KEY, clave);
    }
    copiar(gTab[i].v, W3D_CFG_VAL, valor);
}
void ConfigSetFloat(const char* clave, float valor) {
    char b[32]; snprintf(b, sizeof(b), "%.6g", (double)valor); ConfigSetStr(clave, b);
}
void ConfigSetInt(const char* clave, int valor) {
    char b[24]; snprintf(b, sizeof(b), "%d", valor); ConfigSetStr(clave, b);
}

const char* ConfigGetStr(const char* clave, const char* porDefecto) {
    int i = clave ? buscar(clave) : -1;
    return (i >= 0) ? gTab[i].v : porDefecto;
}
float ConfigGetFloat(const char* clave, float porDefecto) {
    const char* s = ConfigGetStr(clave, 0);
    return s ? (float)atof(s) : porDefecto;
}
int ConfigGetInt(const char* clave, int porDefecto) {
    const char* s = ConfigGetStr(clave, 0);
    return s ? atoi(s) : porDefecto;
}

// --- serializacion: lineas "clave=valor" ------------------------------------
static void desdeTexto(const char* txt) {
    gN = 0;
    if (!txt) return;
    char k[W3D_CFG_KEY], v[W3D_CFG_VAL];
    int ki = 0, vi = 0, enValor = 0;
    for (const char* p = txt; ; p++) {
        char c = *p;
        if (c == '\n' || c == '\r' || c == 0) {
            if (ki > 0) { k[ki] = 0; v[vi] = 0; ConfigSetStr(k, v); }
            ki = vi = 0; enValor = 0;
            if (c == 0) break;
        } else if (!enValor && c == '=') {
            enValor = 1;
        } else if (enValor) {
            if (vi < W3D_CFG_VAL - 1) v[vi++] = c;
        } else {
            if (ki < W3D_CFG_KEY - 1) k[ki++] = c;
        }
    }
}

static int aTexto(char* out, int max) {
    int n = 0;
    for (int i = 0; i < gN; i++) {
        int m = snprintf(out + n, (size_t)(max - n), "%s=%s\n", gTab[i].k, gTab[i].v);
        if (m < 0 || n + m >= max) break;
        n += m;
    }
    out[n < max ? n : max - 1] = 0;
    return n;
}

bool ConfigLoad() {
#ifdef __EMSCRIPTEN__
    char* s = w3dCfgJS_Get(gNombre);
    if (!s) return false;
    desdeTexto(s);
    free(s);
    gMudo = (ConfigGetInt("mute", 0) != 0);   // restaura el mute global desde lo guardado
    return true;
#else
    char ruta[512]; rutaCfg(ruta, sizeof(ruta));
    FILE* f = fopen(ruta, "rb");
    if (!f) return false;
    static char buf[W3D_CFG_MAX * (W3D_CFG_KEY + W3D_CFG_VAL) + 16];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    desdeTexto(buf);
    gMudo = (ConfigGetInt("mute", 0) != 0);   // restaura el mute global desde lo guardado
    return true;
#endif
}

bool ConfigSave() {
    static char buf[W3D_CFG_MAX * (W3D_CFG_KEY + W3D_CFG_VAL) + 16];
    aTexto(buf, (int)sizeof(buf));
#ifdef __EMSCRIPTEN__
    w3dCfgJS_Set(gNombre, buf);
    return true;
#else
    char ruta[512]; rutaCfg(ruta, sizeof(ruta));
    FILE* f = fopen(ruta, "wb");
    if (!f) return false;
    size_t n = strlen(buf);
    size_t wr = fwrite(buf, 1, n, f);
    // fclose es el flush: sin chequearlo se devolvia true con el config truncado (disco lleno)
    bool cerro = (fclose(f) == 0);
    return cerro && wr == n;
#endif
}

} // namespace w3dEngine
