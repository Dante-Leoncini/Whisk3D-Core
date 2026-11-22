// ============================================================================
//  Whisk3DCore (engine) — log de diagnostico. Ver w3dlog.h.
// ============================================================================
#include "w3dlog.h"

#if W3D_DEV_LOG

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#ifndef W3D_SYMBIAN
#include <time.h>     // clock()/CLOCKS_PER_SEC (w3dTickMs en no-Symbian; Symbian usa User::NTickCount)
#endif
#include <iostream>   // std::cout/std::cerr (para w3dRedirectCoutToLog)
#include <string>

// nivel: 0 = INFO, 1 = WARN, 2 = ERROR
#ifdef W3D_SYMBIAN
static char gPathLog[128] = "e:\\whisk3d.log";
#else
static char gPathLog[128] = "whisk3d.log";
#endif

static bool gPathPlaced = false;   // la fijo w3dLogPath (las consolas: sino, la carpeta del editor)
void w3dLogPath(const char* path) {
    if (!path || !*path) return;
    strncpy(gPathLog, path, sizeof(gPathLog) - 1);
    gPathLog[sizeof(gPathLog) - 1] = 0;
    gPathPlaced = true;
}

static const char* LevelTag(int level) { return level == 2 ? "ERROR" : (level == 1 ? "WARN" : "INFO"); }

// ---------------------------------------------------------------------------
//  RING BUFFER en memoria: espejo de las ultimas lineas del log para la UI (el
//  viewport Console del editor). Cada backend llama a RingPush ademas de escribir
//  a su sink real. Buffer FIJO (sin STL ni memoria dinamica): sirve igual en
//  Symbian. Cuando se llena, pisa la mas vieja (circular).
// ---------------------------------------------------------------------------
enum { W3D_LOG_RING_MAX = 200, W3D_LOG_RING_LEN = 240 };
static char gRing[W3D_LOG_RING_MAX][W3D_LOG_RING_LEN];
static int  gRingHead  = 0;   // indice donde se escribe la PROXIMA linea
static int  gRingCount = 0;   // cuantas lineas validas hay (<= MAX)

// NIVEL en tiempo de ejecucion (el editor lo cambia desde Properties > Logs): 3 = todo, 2 = avisos y errores,
// 1 = solo errores, 0 = nada. Se mira ANTES de formatear: apagado, un log no cuesta ni el vsnprintf.
int g_w3dLogLevel = 3;
static inline bool LogLevelOk(int level) { return g_w3dLogLevel > 2 - level; }   // INFO=0 WARN=1 ERROR=2

static void RingPush(int level, const char* aMsg) {
    char* dst = gRing[gRingHead];
    int n = 0;
    // formato "[TAG] mensaje", recortado al largo del slot
    dst[n++] = '[';
    const char* tag = LevelTag(level);
    for (int i = 0; tag[i] && n < W3D_LOG_RING_LEN - 3; i++) dst[n++] = tag[i];
    dst[n++] = ']';
    dst[n++] = ' ';
    if (aMsg) { for (int i = 0; aMsg[i] && n < W3D_LOG_RING_LEN - 1; i++) dst[n++] = aMsg[i]; }
    dst[n] = 0;
    gRingHead = (gRingHead + 1) % W3D_LOG_RING_MAX;
    if (gRingCount < W3D_LOG_RING_MAX) gRingCount++;
}

int w3dLogRingCount() { return gRingCount; }
const char* w3dLogRingLine(int i) {
    if (i < 0 || i >= gRingCount) return "";
    // i=0 = la mas vieja: esta en (cabeza - cant) modulo MAX
    int start = (gRingHead - gRingCount + W3D_LOG_RING_MAX * 2) % W3D_LOG_RING_MAX;
    return gRing[(start + i) % W3D_LOG_RING_MAX];
}

#ifdef W3D_SYMBIAN
// ---------------------------------------------------------------------------
//  Backend Symbian: RFile a e:\whisk3d.log (la tarjeta de memoria). El N95 no
//  tiene stdio a un archivo confiable; ademas asi sobrevive a un cuelgue.
// ---------------------------------------------------------------------------
#include <e32std.h>
#include <f32file.h>
#include "fscompat.h" // cerrar RFs sin importar efsrv@390 (Symbian^3)

// EL ARCHIVO QUEDA ABIERTO y las lineas se JUNTAN en memoria: antes cada linea conectaba con el servidor de archivos,
// abria el log de la tarjeta, buscaba el final, escribia, forzaba a disco y cerraba -- decenas de ms por linea en el
// N95, y el [PERF] del modo juego escribe varias cada pocos segundos: eran los MICROCUELGUES. Ahora:
//   INFO       se junta y baja al archivo cuando hay ~3 KB (o con el proximo WARN / ERROR);
//   WARN       baja YA (lo de antes incluido);
//   ERROR      baja YA y se fuerza a disco (Flush): si lo que sigue es un cuelgue, queda escrito.
// w3dLogFlush() baja lo que haya (al salir de la app, antes de leer el log desde la PC).
static RFs   gLogFs;
static RFile gLogFile;
static bool  gLogOpen = false;
static TBuf8<4096> gLogBuf;
static bool LogOpen(bool aTruncate) {
    if (gLogOpen && !aTruncate) return true;
    if (gLogOpen) { gLogFile.Close(); FsCloseCompat(gLogFs); gLogOpen = false; }
    if (gLogFs.Connect() != KErrNone) return false;   // sin tarjeta E: u otro problema: no hay log, pero no molesta
    TBuf<128> path;
    path.Copy(TPtrC8((const TUint8*)gPathLog));
    TInt err;
    if (aTruncate) err = gLogFile.Replace(gLogFs, path, EFileWrite | EFileShareAny);
    else {
        err = gLogFile.Open(gLogFs, path, EFileWrite | EFileShareAny);
        if (err == KErrNotFound) err = gLogFile.Create(gLogFs, path, EFileWrite | EFileShareAny);
    }
    if (err != KErrNone) { FsCloseCompat(gLogFs); return false; }
    TInt pos = 0;
    gLogFile.Seek(ESeekEnd, pos);
    gLogOpen = true;
    return true;
}
static void LogFlush(bool aADisk) {
    if (!gLogOpen || gLogBuf.Length() == 0) return;
    gLogFile.Write(gLogBuf);
    gLogBuf.Zero();
    if (aADisk) gLogFile.Flush();
}
void w3dLogFlush() { LogFlush(true); }
static void WriteSymbian(int level, const char* aMsg, bool aTruncate) {
    RingPush(level, aMsg); // espejo en memoria para la UI
    if (aTruncate) gLogBuf.Zero();
    if (!LogOpen(aTruncate)) return;
    TBuf8<280> line;
    line.AppendNum((TUint)User::NTickCount()); // timestamp en ms
    line.Append(_L8(" ["));
    line.Append(TPtrC8((const TUint8*)LevelTag(level))); // [INFO]/[WARN]/[ERROR]
    line.Append(_L8("] "));
    TPtrC8 m((const TUint8*)(aMsg ? aMsg : ""));
    line.Append(m.Left(200));
    line.Append(_L8("\r\n"));
    if (gLogBuf.Length() + line.Length() > gLogBuf.MaxLength()) LogFlush(false);
    gLogBuf.Append(line);
    if (level >= 1 || aTruncate || gLogBuf.Length() > 3072) LogFlush(level >= 2 || aTruncate);
}
void w3dLogReset() { WriteSymbian(0, "=== Whisk3D DEV LOG: inicio de sesion ===", true); }
void w3dLog(const char* aMsg)  { if (!LogLevelOk(0)) return; WriteSymbian(0, aMsg, false); }
void w3dLogW(const char* aMsg) { if (!LogLevelOk(1)) return; WriteSymbian(1, aMsg, false); }
void w3dLogE(const char* aMsg) { if (!LogLevelOk(2)) return; WriteSymbian(2, aMsg, false); }

#else
void w3dLogFlush() {}
#endif
#if defined(W3D_SYMBIAN)
#elif defined(__ANDROID__)
// ---------------------------------------------------------------------------
//  Backend Android: a logcat (via __android_log_print). En Android no hay un
//  cwd escribible garantizado (fopen("whisk3d.log") suele fallar); logcat es
//  el canal natural. Se ve con: adb logcat -s Whisk3D
// ---------------------------------------------------------------------------
#include <android/log.h>

static void WriteAndroid(int level, const char* aMsg) {
    RingPush(level, aMsg); // espejo en memoria para la UI
    int pri = (level == 2) ? ANDROID_LOG_ERROR : (level == 1 ? ANDROID_LOG_WARN : ANDROID_LOG_INFO);
    __android_log_write(pri, "Whisk3D", aMsg ? aMsg : "");
}
void w3dLogReset() { WriteAndroid(0, "=== Whisk3D DEV LOG ==="); }
void w3dLog(const char* aMsg)  { if (!LogLevelOk(0)) return; WriteAndroid(0, aMsg); }
void w3dLogW(const char* aMsg) { if (!LogLevelOk(1)) return; WriteAndroid(1, aMsg); }
void w3dLogE(const char* aMsg) { if (!LogLevelOk(2)) return; WriteAndroid(2, aMsg); }

#elif defined(W3D_DREAMCAST)
// ---------------------------------------------------------------------------
//  Backend Dreamcast: a la salida estandar, que KallistiOS manda por el puerto serie (dbgio). Se ve con dc-tool en la
//  consola real y en Flycast con la consola serie prendida. La consola no tiene un disco escribible para un archivo.
// ---------------------------------------------------------------------------
static void WriteDC(int level, const char* aMsg) {
    RingPush(level, aMsg); // espejo en memoria para la UI
    printf("[%s] %s\n", LevelTag(level), aMsg ? aMsg : "");
    fflush(stdout);
}
void w3dLogReset() { WriteDC(0, "=== Whisk3D DEV LOG ==="); }
void w3dLog(const char* aMsg)  { if (!LogLevelOk(0)) return; WriteDC(0, aMsg); }
void w3dLogW(const char* aMsg) { if (!LogLevelOk(1)) return; WriteDC(1, aMsg); }
void w3dLogE(const char* aMsg) { if (!LogLevelOk(2)) return; WriteDC(2, aMsg); }

#elif defined(W3D_3DS)
// ---------------------------------------------------------------------------
//  Backend 3DS: svcOutputDebugString, que sale en el log del emulador (Azahar/Citra) y en un depurador por la red con
//  la consola real, y a la tarjeta SD (sdmc:/3ds/Whisk3D/whisk3d.log, se pisa en cada sesion; un juego, en la suya con
//  w3dLogPath): en la consola de verdad es lo que queda para ver que paso. Cada linea se escribe en el momento (si la
//  consola se cuelga, ya esta en la SD).
//  La pantalla no se usa: las dos son del editor.
// ---------------------------------------------------------------------------
#include <3ds.h>
#include <sys/stat.h>
static void Write3ds(int level, const char* aMsg) {
    RingPush(level, aMsg); // espejo en memoria para la UI
    char buf[300];
    const int n = snprintf(buf, sizeof(buf), "[%s] %s", LevelTag(level), aMsg ? aMsg : "");
    svcOutputDebugString(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    static FILE* f = NULL;
    static bool tested = false;
    if (!tested) {
        tested = true;
        if (gPathPlaced) f = fopen(gPathLog, "w");
        else {
            mkdir("sdmc:/3ds", 0777);
            mkdir("sdmc:/3ds/Whisk3D", 0777);
            f = fopen("sdmc:/3ds/Whisk3D/whisk3d.log", "w");
        }
    }
    if (f) { fputs(buf, f); fputc('\n', f); fflush(f); }
}
void w3dLogReset() { Write3ds(0, "=== Whisk3D DEV LOG ==="); }
void w3dLog(const char* aMsg)  { if (!LogLevelOk(0)) return; Write3ds(0, aMsg); }
void w3dLogW(const char* aMsg) { if (!LogLevelOk(1)) return; Write3ds(1, aMsg); }
void w3dLogE(const char* aMsg) { if (!LogLevelOk(2)) return; Write3ds(2, aMsg); }

#elif defined(W3D_VITA)
// ---------------------------------------------------------------------------
//  Backend PS Vita: sceClibPrintf, que sale en el log del emulador (Vita3K) y por la consola de depuracion con la Vita
//  de verdad, y a un archivo (ux0:data/Whisk3D/whisk3d.log, se pisa en cada sesion; un juego, en la suya con
//  w3dLogPath), como en la 3DS. Cada linea se
//  escribe en el momento (si la consola se cuelga, ya esta en el archivo).
// ---------------------------------------------------------------------------
#include <psp2/kernel/clib.h>
#include <sys/stat.h>
static void WriteVita(int level, const char* aMsg) {
    RingPush(level, aMsg); // espejo en memoria para la UI
    sceClibPrintf("[%s] %s\n", LevelTag(level), aMsg ? aMsg : "");
    static FILE* f = NULL;
    static bool tested = false;
    if (!tested) {
        tested = true;
        if (gPathPlaced) f = fopen(gPathLog, "w");
        else {
            mkdir("ux0:data/Whisk3D", 0777);
            f = fopen("ux0:data/Whisk3D/whisk3d.log", "w");
        }
    }
    if (f) { fprintf(f, "[%s] %s\n", LevelTag(level), aMsg ? aMsg : ""); fflush(f); }
}
void w3dLogReset() { WriteVita(0, "=== Whisk3D DEV LOG ==="); }
void w3dLog(const char* aMsg)  { if (!LogLevelOk(0)) return; WriteVita(0, aMsg); }
void w3dLogW(const char* aMsg) { if (!LogLevelOk(1)) return; WriteVita(1, aMsg); }
void w3dLogE(const char* aMsg) { if (!LogLevelOk(2)) return; WriteVita(2, aMsg); }

#elif defined(W3D_PSP)
// ---------------------------------------------------------------------------
//  Backend PSP: la salida estandar, que sale en el log del emulador (PPSSPP) y por psplink con la consola de verdad, y
//  un archivo al lado del EBOOT.PBP (whisk3d.log en la carpeta actual: ms0:/PSP/GAME/Whisk3D, se pisa en cada sesion),
//  como en la 3DS y la Vita. Cada linea se escribe en el momento (si la consola se cuelga, ya esta en el archivo).
// ---------------------------------------------------------------------------
static void WritePSP(int level, const char* aMsg) {
    RingPush(level, aMsg); // espejo en memoria para la UI
    printf("[%s] %s\n", LevelTag(level), aMsg ? aMsg : "");
    static FILE* f = NULL;
    static bool tested = false;
    if (!tested) { tested = true; f = fopen(gPathLog, "w"); }   // (whisk3d.log, o el de w3dLogPath)
    if (f) { fprintf(f, "[%s] %s\n", LevelTag(level), aMsg ? aMsg : ""); fflush(f); }
}
void w3dLogReset() { WritePSP(0, "=== Whisk3D DEV LOG ==="); }
void w3dLog(const char* aMsg)  { if (!LogLevelOk(0)) return; WritePSP(0, aMsg); }
void w3dLogW(const char* aMsg) { if (!LogLevelOk(1)) return; WritePSP(1, aMsg); }
void w3dLogE(const char* aMsg) { if (!LogLevelOk(2)) return; WritePSP(2, aMsg); }

#elif defined(__EMSCRIPTEN__)
// ---------------------------------------------------------------------------
//  Backend WebGL / Emscripten: a la CONSOLA del browser, RESPETANDO el nivel:
//  INFO -> console.log, WARN -> console.warn, ERROR -> console.error. Asi un log
//  bueno NO sale rojo como error. Sin timestamp: la consola ya pone la hora.
// ---------------------------------------------------------------------------
#include <emscripten/console.h>

static void WriteWeb(int level, const char* aMsg) {
    RingPush(level, aMsg); // espejo en memoria para la UI
    char buf[280];
    snprintf(buf, sizeof(buf), "[Whisk3D] %s", aMsg ? aMsg : "");
    if      (level == 2) emscripten_console_error(buf);
    else if (level == 1) emscripten_console_warn(buf);
    else                 emscripten_console_log(buf);
}
void w3dLogReset() { WriteWeb(0, "=== Whisk3D DEV LOG ==="); }
void w3dLog(const char* aMsg)  { if (!LogLevelOk(0)) return; WriteWeb(0, aMsg); }
void w3dLogW(const char* aMsg) { if (!LogLevelOk(1)) return; WriteWeb(1, aMsg); }
void w3dLogE(const char* aMsg) { if (!LogLevelOk(2)) return; WriteWeb(2, aMsg); }

#else
// ---------------------------------------------------------------------------
//  Backend escritorio (PC): whisk3d.log via stdio (append + flush). Android NO
//  cae aca (tiene su backend logcat arriba). El nivel va como tag
//  [INFO]/[WARN]/[ERROR] en la linea.
// ---------------------------------------------------------------------------
#include <time.h>

static void WriteStdio(int level, const char* aMsg, bool aTruncate) {
    RingPush(level, aMsg); // espejo en memoria para la UI
    FILE* f = fopen(gPathLog, aTruncate ? "w" : "a");
    if (!f) return;
    unsigned long ms = (unsigned long)(clock() * 1000.0 / CLOCKS_PER_SEC);
    fprintf(f, "%lu [%s] %s\r\n", ms, LevelTag(level), aMsg ? aMsg : "");
    fflush(f);
    fclose(f);
}
void w3dLogReset() { WriteStdio(0, "=== Whisk3D DEV LOG: inicio de sesion ===", true); }
void w3dLog(const char* aMsg)  { if (!LogLevelOk(0)) return; WriteStdio(0, aMsg, false); }
void w3dLogW(const char* aMsg) { if (!LogLevelOk(1)) return; WriteStdio(1, aMsg, false); }
void w3dLogE(const char* aMsg) { if (!LogLevelOk(2)) return; WriteStdio(2, aMsg, false); }

#endif

// versiones printf-style, comunes a todos los backends (el sink ya es por-nivel)
static void w3dLogfImpl(void (*sink)(const char*), const char* aFmt, va_list ap) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), aFmt ? aFmt : "", ap);
    sink(buf);
}
void w3dLogf(const char* aFmt, ...)  { if (!LogLevelOk(0)) return; va_list ap; va_start(ap, aFmt); w3dLogfImpl(w3dLog,  aFmt, ap); va_end(ap); }
void w3dLogfW(const char* aFmt, ...) { if (!LogLevelOk(1)) return; va_list ap; va_start(ap, aFmt); w3dLogfImpl(w3dLogW, aFmt, ap); va_end(ap); }
void w3dLogfE(const char* aFmt, ...) { if (!LogLevelOk(2)) return; va_list ap; va_start(ap, aFmt); w3dLogfImpl(w3dLogE, aFmt, ap); va_end(ap); }

// ---- redirigir std::cout/std::cerr AL LOG (no la consola/terminal) ---------------------------------------
// LogCoutBuf junta caracteres hasta el '\n' y emite UNA entrada de log (w3dLogf/w3dLogfW). Se instala al arrancar
// (main() en PC, ConstructL en Symbian). Cualquier cout/cerr olvidado en el codigo cae al log en vez de spamear
// una terminal. Los buffers son static: viven toda la app.
namespace {
    class LogCoutBuf : public std::streambuf {
        std::string line; bool isErr;
    public:
        explicit LogCoutBuf(bool err) : isErr(err) {}
    protected:
        virtual int overflow(int c) {
            if (c == '\n') {
                if (!line.empty()) { if (isErr) w3dLogfW("%s", line.c_str()); else w3dLogf("%s", line.c_str()); line.clear(); }
            } else if (c != EOF && c != '\r') {
                line += (char)c;
            }
            return c;
        }
    };
}
// Se puede llamar muchas veces (OpenW3D la llama en CADA apertura, por el caso Symbian de un arranque
// que no la instalo), pero solo toma el stream si sigue en su buffer ORIGINAL (la consola). Si
// alguien le puso OTRO a proposito (el harness que captura lo que se loguea durante una apertura, una app
// que embebe el motor) se respeta: antes cada apertura se lo robaba y esa captura quedaba siempre vacia.
void w3dRedirectCoutToLog() {
    static LogCoutBuf sOut(false);
    static LogCoutBuf sErr(true);
    static std::streambuf* sOrigOut = 0;   // los buffers de la consola, vistos en la PRIMERA llamada
    static std::streambuf* sOrigErr = 0;
    static bool first = true;
    if (first) {
        first = false;
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

// ---- memoria (ver w3dlog.h) -------------------------------------------------------------------------------
#if defined(W3D_SYMBIAN)
#include <hal.h>      // HAL::Get(EMemoryRAMFree): la RAM libre del telefono (hal.lib)
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>   // mallinfo2: el heap EN USO (no lo que el allocator retiene)
#define W3D_LOG_MALLINFO2 1
#endif
void w3dMemoryKB(long& usedHeap, long& heapSize, long& freeRam) {
    usedHeap = heapSize = freeRam = -1;
#if defined(W3D_SYMBIAN)
    TInt total = 0;
    User::AllocSize(total);                          // lo pedido y no devuelto en el heap de la app
    usedHeap = (long)(total / 1024);
    heapSize = (long)(User::Heap().Size() / 1024);    // lo que el chunk del heap ya tomo del telefono
    TInt free = 0;
    if (HAL::Get(HALData::EMemoryRAMFree, free) == KErrNone) freeRam = (long)(free / 1024);
#elif defined(W3D_LOG_MALLINFO2)
    const struct mallinfo2 mi = mallinfo2();
    usedHeap = (long)((mi.uordblks + mi.hblkhd) / 1024u);
    heapSize = (long)((mi.arena + mi.hblkhd) / 1024u);
#endif
}
void w3dLogMemory(const char* label) {
    if (!LogLevelOk(0)) return;
    long u, t, l;
    w3dMemoryKB(u, t, l);
    if (u < 0) return;   // (la plataforma no lo mide: no hay nada que decir)
    if (l >= 0) w3dLogf("[mem] %s: heap %ld KB en uso (%ld KB tomados), RAM libre %ld KB", label ? label : "", u, t, l);
    else        w3dLogf("[mem] %s: heap %ld KB en uso (%ld KB tomados)", label ? label : "", u, t);
}

#endif // W3D_DEV_LOG
