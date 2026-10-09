#pragma once
// ============================================================================
//  Whisk3DCore (engine) — log de diagnostico (modo DEV), comun a los 4 OS.
//
//  Toda app deberia poder prender/apagar un log: es una facilidad BASICA, asi
//  que el SINK (abrir el archivo, el timestamp, el on/off) vive en el core. Lo
//  que NO es del core es COMO cada plataforma arma lo que loguea (p.ej. Symbian
//  formatea sus descriptors y termina llamando a w3dLog) — eso queda en el shell.
//
//  Backend por plataforma (ver w3dlog.cpp): Symbian escribe a e:\whisk3d.log via
//  RFile; PC/escritorio a whisk3d.log via stdio. Cada linea se abre/escribe/
//  cierra con flush: si la app muere, el log queda completo hasta el final.
//
//  Para builds de release: -DW3D_DEV_LOG=0 desde el build (las llamadas quedan vacias).
// ============================================================================
#ifndef W3D_DEV_LOG
#define W3D_DEV_LOG 1
#endif

#if W3D_DEV_LOG
// NIVEL en tiempo de ejecucion: 3 = todo (default), 2 = avisos y errores, 1 = solo errores, 0 = nada
extern int g_w3dLogNivel;
void w3dLogReset();                   // trunca/abre el log (al iniciar la app)
// Tres NIVELES. En desktop/Symbian van todas al archivo con su tag ([INFO]/[WARN]/[ERROR]);
// en WebGL cada una llama a la funcion del browser: console.log / console.warn / console.error.
void w3dLog(const char* aMsg);        // INFO
void w3dLogW(const char* aMsg);       // WARNING
void w3dLogE(const char* aMsg);       // ERROR
void w3dLogf(const char* aFmt, ...);  // INFO, printf-style
void w3dLogfW(const char* aFmt, ...); // WARNING
void w3dLogfE(const char* aFmt, ...); // ERROR

// ---- RING BUFFER en memoria (para la UI: viewport Console del editor) ------
// Espejo liviano de las ultimas lineas logueadas (con su tag [INFO]/[WARN]/[ERROR]
// adelante). NO reemplaza al sink de archivo/consola: se llena en paralelo. Asi
// el editor puede mostrar el log sin abrir el .log ni tocar stdout. C++03 puro.
int          w3dLogRingCount();       // cuantas lineas hay guardadas (<= N)
const char*  w3dLogRingLinea(int i);  // linea i (0 = la mas VIEJA); "" si i fuera de rango

// redirige std::cout/std::cerr AL LOG (no a la consola/terminal). Llamar UNA vez al arrancar (PC main() y Symbian
// ConstructL). Cualquier cout/cerr olvidado en el codigo cae al log en vez de abrir/spamear una terminal.
void w3dRedirigirCoutAlLog();

// reloj monotono en MILISEGUNDOS para medir fases (p.ej. cuanto tarda cargar un .w3d en el N95).
// Symbian: tick del nanokernel; resto: clock(). Suficiente para medir fases de segundos.
unsigned long w3dTickMs();
#else
inline void w3dLogReset() {}
inline void w3dLog(const char*) {}
inline void w3dLogW(const char*) {}
inline void w3dLogE(const char*) {}
inline void w3dLogf(const char*, ...) {}
inline void w3dLogfW(const char*, ...) {}
inline void w3dLogfE(const char*, ...) {}
inline int         w3dLogRingCount() { return 0; }
inline const char* w3dLogRingLinea(int) { return ""; }
inline void        w3dRedirigirCoutAlLog() {}
inline unsigned long w3dTickMs() { return 0; }
#endif
