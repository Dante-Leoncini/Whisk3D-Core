// ============================================================================
//  Whisk3DCore (engine) — log de diagnostico. Ver w3dlog.h.
// ============================================================================
#include "w3dlog.h"

#if W3D_DEV_LOG

#include <stdio.h>
#include <stdarg.h>
#ifndef W3D_SYMBIAN
#include <time.h>     // clock()/CLOCKS_PER_SEC (w3dTickMs en no-Symbian; Symbian usa User::NTickCount)
#endif
#include <iostream>   // std::cout/std::cerr (para w3dRedirigirCoutAlLog)
#include <string>

// nivel: 0 = INFO, 1 = WARN, 2 = ERROR
static const char* NivelTag(int nivel) { return nivel == 2 ? "ERROR" : (nivel == 1 ? "WARN" : "INFO"); }

// ---------------------------------------------------------------------------
//  RING BUFFER en memoria: espejo de las ultimas lineas del log para la UI (el
//  viewport Console del editor). Cada backend llama a RingPush ademas de escribir
//  a su sink real. Buffer FIJO (sin STL ni memoria dinamica): sirve igual en
//  Symbian. Cuando se llena, pisa la mas vieja (circular).
// ---------------------------------------------------------------------------
enum { W3D_LOG_RING_MAX = 200, W3D_LOG_RING_LEN = 240 };
static char gRing[W3D_LOG_RING_MAX][W3D_LOG_RING_LEN];
static int  gRingCabeza = 0;   // indice donde se escribe la PROXIMA linea
static int  gRingCant   = 0;   // cuantas lineas validas hay (<= MAX)

static void RingPush(int nivel, const char* aMsg) {
    char* dst = gRing[gRingCabeza];
    int n = 0;
    // formato "[TAG] mensaje", recortado al largo del slot
    dst[n++] = '[';
    const char* tag = NivelTag(nivel);
    for (int i = 0; tag[i] && n < W3D_LOG_RING_LEN - 3; i++) dst[n++] = tag[i];
    dst[n++] = ']';
    dst[n++] = ' ';
    if (aMsg) { for (int i = 0; aMsg[i] && n < W3D_LOG_RING_LEN - 1; i++) dst[n++] = aMsg[i]; }
    dst[n] = 0;
    gRingCabeza = (gRingCabeza + 1) % W3D_LOG_RING_MAX;
    if (gRingCant < W3D_LOG_RING_MAX) gRingCant++;
}

int w3dLogRingCount() { return gRingCant; }
const char* w3dLogRingLinea(int i) {
    if (i < 0 || i >= gRingCant) return "";
    // i=0 = la mas vieja: esta en (cabeza - cant) modulo MAX
    int inicio = (gRingCabeza - gRingCant + W3D_LOG_RING_MAX * 2) % W3D_LOG_RING_MAX;
    return gRing[(inicio + i) % W3D_LOG_RING_MAX];
}

#ifdef W3D_SYMBIAN
// ---------------------------------------------------------------------------
//  Backend Symbian: RFile a e:\whisk3d.log (la tarjeta de memoria). El N95 no
//  tiene stdio a un archivo confiable; ademas asi sobrevive a un cuelgue.
// ---------------------------------------------------------------------------
#include <e32std.h>
#include <f32file.h>
#include "fscompat.h" // cerrar RFs sin importar efsrv@390 (Symbian^3)

static void EscribirSymbian(int nivel, const char* aMsg, bool aTruncar) {
    RingPush(nivel, aMsg); // espejo en memoria para la UI
    RFs fs;
    if (fs.Connect() != KErrNone) {
        return; // sin tarjeta E: u otro problema: no hay log, pero no molesta
    }
    RFile f;
    _LIT(KPath, "e:\\whisk3d.log");
    TInt err;
    if (aTruncar) {
        err = f.Replace(fs, KPath, EFileWrite | EFileShareAny);
    } else {
        err = f.Open(fs, KPath, EFileWrite | EFileShareAny);
        if (err == KErrNotFound) {
            err = f.Create(fs, KPath, EFileWrite | EFileShareAny);
        }
    }
    if (err == KErrNone) {
        TInt pos = 0;
        f.Seek(ESeekEnd, pos);
        TBuf8<280> line;
        line.AppendNum((TUint)User::NTickCount()); // timestamp en ms
        line.Append(_L8(" ["));
        line.Append(TPtrC8((const TUint8*)NivelTag(nivel))); // [INFO]/[WARN]/[ERROR]
        line.Append(_L8("] "));
        TPtrC8 m((const TUint8*)(aMsg ? aMsg : ""));
        line.Append(m.Left(200));
        line.Append(_L8("\r\n"));
        f.Write(line);
        f.Flush(); // a disco YA, por si lo proximo es el cuelgue
        f.Close();
    }
    FsCloseCompat(fs);
}
void w3dLogReset() { EscribirSymbian(0, "=== Whisk3D DEV LOG: inicio de sesion ===", true); }
void w3dLog(const char* aMsg)  { EscribirSymbian(0, aMsg, false); }
void w3dLogW(const char* aMsg) { EscribirSymbian(1, aMsg, false); }
void w3dLogE(const char* aMsg) { EscribirSymbian(2, aMsg, false); }

#elif defined(__ANDROID__)
// ---------------------------------------------------------------------------
//  Backend Android: a logcat (via __android_log_print). En Android no hay un
//  cwd escribible garantizado (fopen("whisk3d.log") suele fallar); logcat es
//  el canal natural. Se ve con: adb logcat -s Whisk3D
// ---------------------------------------------------------------------------
#include <android/log.h>

static void EscribirAndroid(int nivel, const char* aMsg) {
    RingPush(nivel, aMsg); // espejo en memoria para la UI
    int pri = (nivel == 2) ? ANDROID_LOG_ERROR : (nivel == 1 ? ANDROID_LOG_WARN : ANDROID_LOG_INFO);
    __android_log_write(pri, "Whisk3D", aMsg ? aMsg : "");
}
void w3dLogReset() { EscribirAndroid(0, "=== Whisk3D DEV LOG ==="); }
void w3dLog(const char* aMsg)  { EscribirAndroid(0, aMsg); }
void w3dLogW(const char* aMsg) { EscribirAndroid(1, aMsg); }
void w3dLogE(const char* aMsg) { EscribirAndroid(2, aMsg); }

#elif defined(__EMSCRIPTEN__)
// ---------------------------------------------------------------------------
//  Backend WebGL / Emscripten: a la CONSOLA del browser, RESPETANDO el nivel:
//  INFO -> console.log, WARN -> console.warn, ERROR -> console.error. Asi un log
//  bueno NO sale rojo como error. Sin timestamp: la consola ya pone la hora.
// ---------------------------------------------------------------------------
#include <emscripten/console.h>

static void EscribirWeb(int nivel, const char* aMsg) {
    RingPush(nivel, aMsg); // espejo en memoria para la UI
    char buf[280];
    snprintf(buf, sizeof(buf), "[Whisk3D] %s", aMsg ? aMsg : "");
    if      (nivel == 2) emscripten_console_error(buf);
    else if (nivel == 1) emscripten_console_warn(buf);
    else                 emscripten_console_log(buf);
}
void w3dLogReset() { EscribirWeb(0, "=== Whisk3D DEV LOG ==="); }
void w3dLog(const char* aMsg)  { EscribirWeb(0, aMsg); }
void w3dLogW(const char* aMsg) { EscribirWeb(1, aMsg); }
void w3dLogE(const char* aMsg) { EscribirWeb(2, aMsg); }

#else
// ---------------------------------------------------------------------------
//  Backend escritorio (PC): whisk3d.log via stdio (append + flush). Android NO
//  cae aca (tiene su backend logcat arriba). El nivel va como tag
//  [INFO]/[WARN]/[ERROR] en la linea.
// ---------------------------------------------------------------------------
#include <time.h>

static void EscribirStdio(int nivel, const char* aMsg, bool aTruncar) {
    RingPush(nivel, aMsg); // espejo en memoria para la UI
    FILE* f = fopen("whisk3d.log", aTruncar ? "w" : "a");
    if (!f) return;
    unsigned long ms = (unsigned long)(clock() * 1000.0 / CLOCKS_PER_SEC);
    fprintf(f, "%lu [%s] %s\r\n", ms, NivelTag(nivel), aMsg ? aMsg : "");
    fflush(f);
    fclose(f);
}
void w3dLogReset() { EscribirStdio(0, "=== Whisk3D DEV LOG: inicio de sesion ===", true); }
void w3dLog(const char* aMsg)  { EscribirStdio(0, aMsg, false); }
void w3dLogW(const char* aMsg) { EscribirStdio(1, aMsg, false); }
void w3dLogE(const char* aMsg) { EscribirStdio(2, aMsg, false); }

#endif

// versiones printf-style, comunes a todos los backends (el sink ya es por-nivel)
static void w3dLogfImpl(void (*sink)(const char*), const char* aFmt, va_list ap) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), aFmt ? aFmt : "", ap);
    sink(buf);
}
void w3dLogf(const char* aFmt, ...)  { va_list ap; va_start(ap, aFmt); w3dLogfImpl(w3dLog,  aFmt, ap); va_end(ap); }
void w3dLogfW(const char* aFmt, ...) { va_list ap; va_start(ap, aFmt); w3dLogfImpl(w3dLogW, aFmt, ap); va_end(ap); }
void w3dLogfE(const char* aFmt, ...) { va_list ap; va_start(ap, aFmt); w3dLogfImpl(w3dLogE, aFmt, ap); va_end(ap); }

// ---- redirigir std::cout/std::cerr AL LOG (no la consola/terminal) ---------------------------------------
// LogCoutBuf junta caracteres hasta el '\n' y emite UNA entrada de log (w3dLogf/w3dLogfW). Se instala al arrancar
// (main() en PC, ConstructL en Symbian). Cualquier cout/cerr olvidado en el codigo cae al log en vez de spamear
// una terminal. Los buffers son static: viven toda la app.
namespace {
    class LogCoutBuf : public std::streambuf {
        std::string linea; bool esErr;
    public:
        explicit LogCoutBuf(bool err) : esErr(err) {}
    protected:
        virtual int overflow(int c) {
            if (c == '\n') {
                if (!linea.empty()) { if (esErr) w3dLogfW("%s", linea.c_str()); else w3dLogf("%s", linea.c_str()); linea.clear(); }
            } else if (c != EOF && c != '\r') {
                linea += (char)c;
            }
            return c;
        }
    };
}
// Se puede llamar muchas veces (AbrirW3D la llama en CADA apertura, por el caso Symbian de un arranque
// que no la instalo), pero solo toma el stream si sigue en su buffer ORIGINAL (la consola). Si
// alguien le puso OTRO a proposito (el harness que captura lo que se loguea durante una apertura, una app
// que embebe el motor) se respeta: antes cada apertura se lo robaba y esa captura quedaba siempre vacia.
void w3dRedirigirCoutAlLog() {
    static LogCoutBuf sOut(false);
    static LogCoutBuf sErr(true);
    static std::streambuf* sOrigOut = 0;   // los buffers de la consola, vistos en la PRIMERA llamada
    static std::streambuf* sOrigErr = 0;
    static bool primera = true;
    if (primera) {
        primera = false;
        sOrigOut = std::cout.rdbuf();
        sOrigErr = std::cerr.rdbuf();
        std::cout.rdbuf(&sOut);
        std::cerr.rdbuf(&sErr);
        return;
    }
    if (std::cout.rdbuf() == sOrigOut) std::cout.rdbuf(&sOut);
    if (std::cerr.rdbuf() == sOrigErr) std::cerr.rdbuf(&sErr);
}

// ---- reloj de ms para instrumentar (ver w3dlog.h) --------------------------------------------------------
// En Symbian e32std.h ya entro por el backend de arriba (User::NTickCount, ~ms). En el resto, clock() con
// <time.h>. No hace falta que sea preciso: se usa para atribuir SEGUNDOS de carga a cada fase.
unsigned long w3dTickMs() {
#ifdef W3D_SYMBIAN
    return (unsigned long)User::NTickCount();
#else
    return (unsigned long)((double)clock() * 1000.0 / (double)CLOCKS_PER_SEC);
#endif
}

#endif // W3D_DEV_LOG
