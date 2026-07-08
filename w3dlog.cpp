// ============================================================================
//  Whisk3DCore (engine) — log de diagnostico. Ver w3dlog.h.
// ============================================================================
#include "w3dlog.h"

#if W3D_DEV_LOG

#include <stdio.h>
#include <stdarg.h>

// nivel: 0 = INFO, 1 = WARN, 2 = ERROR
static const char* NivelTag(int nivel) { return nivel == 2 ? "ERROR" : (nivel == 1 ? "WARN" : "INFO"); }

#ifdef W3D_SYMBIAN
// ---------------------------------------------------------------------------
//  Backend Symbian: RFile a e:\whisk3d.log (la tarjeta de memoria). El N95 no
//  tiene stdio a un archivo confiable; ademas asi sobrevive a un cuelgue.
// ---------------------------------------------------------------------------
#include <e32std.h>
#include <f32file.h>
#include "fscompat.h" // cerrar RFs sin importar efsrv@390 (Symbian^3)

static void EscribirSymbian(int nivel, const char* aMsg, bool aTruncar) {
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
//  Backend escritorio (PC/Android): whisk3d.log via stdio (append + flush).
//  El nivel va como tag [INFO]/[WARN]/[ERROR] en la linea.
// ---------------------------------------------------------------------------
#include <time.h>

static void EscribirStdio(int nivel, const char* aMsg, bool aTruncar) {
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

#endif // W3D_DEV_LOG
