// ============================================================================
//  w3dconsola.cpp — ver w3dconsola.h. Una seccion por consola.
// ============================================================================
#include "w3dconsola.h"
#ifdef W3D_CONSOLE
#include "w3dGraphics.h"
#include "w3dlog.h"
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

// las consolas no lanzan procesos: popen/pclose fallan siempre (como en el editor de cada una)
extern "C" FILE* popen(const char*, const char*) { errno = ENOSYS; return NULL; }
extern "C" int pclose(FILE*) { errno = ENOSYS; return -1; }

// ============================================================================
//  PSP: el GE (graphics/gu/w3dGraphicsGu.cpp), sin OpenGL
// ============================================================================
#if defined(W3D_PSP)
#include <pspmoduleinfo.h>
#include "graphics/gu/w3dGu.h"

// SDL2 de la PSP trae el contexto de OpenGL de PSPGL; el juego no lo usa (dibuja el GE) y PSPGL no se enlaza: las
// funciones de EGL que nombra SDL fallan siempre
extern "C" {
int eglGetError(void) { return 0x3001; }
void* eglGetDisplay(void*) { return NULL; }
unsigned eglInitialize(void*, int*, int*) { return 0; }
unsigned eglTerminate(void*) { return 0; }
unsigned eglChooseConfig(void*, const int*, void*, int, int*) { return 0; }
unsigned eglGetConfigAttrib(void*, void*, int, int*) { return 0; }
void* eglCreateContext(void*, void*, void*, const int*) { return NULL; }
unsigned eglDestroyContext(void*, void*) { return 0; }
void* eglCreateWindowSurface(void*, void*, void*, const int*) { return NULL; }
unsigned eglMakeCurrent(void*, void*, void*, void*) { return 0; }
unsigned eglSwapBuffers(void*, void*) { return 0; }
unsigned eglSwapInterval(void*, int) { return 0; }
void* eglGetProcAddress(const char*) { return NULL; }
// (la informacion del modulo la pone SDL2main) la pila del hilo principal y lo que el heap le deja al sistema para
// los hilos de SDL (el sonido)
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
PSP_HEAP_THRESHOLD_SIZE_KB(1536);
}

// (la carpeta actual ya es la del EBOOT.PBP, donde estan los assets; el log, whisk3d.log, va ahi mismo)
void W3dConsoleBeforeSDL(const char*) {}
SDL_Window* W3dConsoleWindow(const char* title) {
    SDL_Window* w = SDL_CreateWindow(title, 0, 0, 480, 272, SDL_WINDOW_FULLSCREEN);
    if (w && !w3dEngine::GuStart()) { w3dLogfE("[psp] el GE no inicio"); SDL_DestroyWindow(w); return NULL; }
    return w;
}
void W3dConsoleSize(int* w, int* h) { *w = 480; *h = 272; }
std::string W3dConsoleData(const char*) {
    char cwd[512];
    return getcwd(cwd, sizeof(cwd)) ? std::string(cwd) : std::string("ms0:/PSP/GAME");
}
void W3dConsoleFrameBegin() { w3dEngine::GuFrameBegin(); }
void W3dConsoleFrameFinish(SDL_Window*) { w3dEngine::GuFrameFinish(); }
void W3dConsoleEnd(SDL_Window*) {}

// ============================================================================
//  PS VITA: vitaGL (OpenGL con el pipeline fijo emulado), con el contexto de SDL
// ============================================================================
#elif defined(W3D_VITA)
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>

// vitaGL arma los shaders del pipeline fijo con el compilador de Sony (libshacccg.suprx). No viene con la consola ni
// con VitaSDK: sin el, el primer cuadro cuelga la consola. Se avisa y se cierra
// vitaGL carga el compilador de shaders de UN solo lugar: ur0:data/external/libshacccg.suprx (donde lo deja ShaRKBR33D
// hoy). Las versiones viejas de ShaRKBR33D lo dejaban en ur0:data/libshacccg.suprx: si esta ahi (o en el sistema, en las
// consolas de desarrollo), se copia al lugar nuevo una sola vez. false = no esta en ningun lado o no se pudo copiar
// ('failedCopy' = estaba en un lugar viejo)
static bool CopyFileVita(const char* of, const char* a) {
    const SceUID in = sceIoOpen(of, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    const SceUID out = sceIoOpen(a, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out < 0) { sceIoClose(in); return false; }
    static char buf[64 * 1024];
    bool ok = true;
    for (;;) {
        const int n = sceIoRead(in, buf, sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (sceIoWrite(out, buf, n) != n) { ok = false; break; }
    }
    sceIoClose(in);
    sceIoClose(out);
    if (!ok) sceIoRemove(a);   // (un modulo a medias no se deja: vitaGL lo cargaria igual)
    return ok;
}
static bool CompilerShadersVita(bool* failedCopy) {
    static const char* const kNew = "ur0:data/external/libshacccg.suprx";
    static const char* const kOld[] = { "ur0:data/libshacccg.suprx", "vs0:sys/external/libshacccg.suprx" };
    if (failedCopy) *failedCopy = false;
    SceIoStat st;
    if (sceIoGetstat(kNew, &st) >= 0) return true;
    for (int i = 0; i < 2; i++) {
        if (sceIoGetstat(kOld[i], &st) < 0) continue;
        sceIoMkdir("ur0:data/external", 0777);
        if (CopyFileVita(kOld[i], kNew)) {
            w3dLogf("[vita] el compilador de shaders estaba en %s: copiado a %s", kOld[i], kNew);
            return true;
        }
        w3dLogfE("[vita] el compilador de shaders esta en %s y no se pudo copiar a %s", kOld[i], kNew);
        if (failedCopy) *failedCopy = true;
        return false;
    }
    return false;
}

void W3dConsoleBeforeSDL(const char* game) {
    chdir("app0:");   // (los assets van adentro del .vpk)
    const std::string d = std::string("ux0:data/") + game;
    mkdir("ux0:data", 0777);
    mkdir(d.c_str(), 0777);
    w3dLogPath((d + "/whisk3d.log").c_str());
}
SDL_Window* W3dConsoleWindow(const char* title) {
    bool failedCopy = false;
    if (!CompilerShadersVita(&failedCopy)) {
        const char* msg = failedCopy ? "libshacccg.suprx esta en ur0:data/, pero vitaGL lo busca en ur0:data/external/ y no se pudo copiar ahi.\n"
                          "Copialo con VitaShell a ur0:data/external/libshacccg.suprx."
                                     : "Falta el compilador de shaders de la Vita (ur0:data/external/libshacccg.suprx).\n"
                                       "Se instala una vez con la aplicacion ShaRKBR33D.";
        w3dLogfE("[vita] %s", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
        return NULL;
    }
    SDL_Window* w = SDL_CreateWindow(title, 0, 0, 960, 544, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (!w) return NULL;
    if (!SDL_GL_CreateContext(w)) { w3dLogfE("[vita] sin contexto GL: %s", SDL_GetError()); SDL_DestroyWindow(w); return NULL; }
    SDL_GL_SetSwapInterval(1);
    return w;
}
void W3dConsoleSize(int* w, int* h) { *w = 960; *h = 544; }
std::string W3dConsoleData(const char* game) {
    mkdir("ux0:data", 0777);
    const std::string d = std::string("ux0:data/") + game;
    mkdir(d.c_str(), 0777);
    return d;
}
void W3dConsoleFrameBegin() {}
void W3dConsoleFrameFinish(SDL_Window* v) { SDL_GL_SwapWindow(v); }
void W3dConsoleEnd(SDL_Window*) {}

// ============================================================================
//  3DS: citro3d (graphics/citro3d), sin OpenGL. El juego en la pantalla de arriba
// ============================================================================
#elif defined(W3D_3DS)
#include <3ds.h>
#include "graphics/citro3d/w3dC3D.h"

// (SDL instala manejadores de senales si newlib declara sigaction; la 3DS no tiene senales)
struct sigaction;
extern "C" int sigaction(int, const struct sigaction*, struct sigaction*) { errno = ENOSYS; return -1; }

void W3dConsoleBeforeSDL(const char* game) {
    chdir("romfs:/");   // (SDL2main ya monto el romfs del .3dsx: ahi van los assets)
    const std::string d = std::string("sdmc:/3ds/") + game;
    mkdir("sdmc:/3ds", 0777);
    mkdir(d.c_str(), 0777);
    w3dLogPath((d + "/whisk3d.log").c_str());
}
SDL_Window* W3dConsoleWindow(const char* title) {
    // la ventana de SDL es la pantalla de abajo (la tactil: SDL entrega ahi los toques); citro3d dibuja en las dos
    SDL_Window* w = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED_DISPLAY(1), SDL_WINDOWPOS_UNDEFINED_DISPLAY(1),
                                     320, 240, SDL_WINDOW_FULLSCREEN);
    if (w && !w3dEngine::C3DInit()) { w3dLogfE("[3ds] citro3d no inicio"); SDL_DestroyWindow(w); return NULL; }
    return w;
}
void W3dConsoleSize(int* w, int* h) { *w = 400; *h = 240; }
std::string W3dConsoleData(const char* game) {
    mkdir("sdmc:/3ds", 0777);
    const std::string d = std::string("sdmc:/3ds/") + game;
    mkdir(d.c_str(), 0777);
    return d;
}
void W3dConsoleFrameBegin() {
    w3dEngine::C3DFrameBegin();
    w3dEngine::C3DScreen(w3dEngine::ScreenDown);   // (la de abajo: solo se limpia)
    w3dEngine::C3DScreen(w3dEngine::ScreenUp);     // el juego, en la grande
}
void W3dConsoleFrameFinish(SDL_Window*) { w3dEngine::C3DFrameFinish(); }
void W3dConsoleEnd(SDL_Window*) { w3dEngine::C3DEnd(); }   // (antes de que SDL apague las pantallas)

// ============================================================================
//  DREAMCAST: GLdc (OpenGL 1.x de la PVR), con el contexto de SDL
// ============================================================================
#elif defined(W3D_DREAMCAST)
#include <kos/fs.h>

void W3dConsoleBeforeSDL(const char*) {   // (el log sale por el puerto serie: no hay disco escribible)
    // el driver de video de SDL engancha GLdc solo en este modo; sin el hint abre un framebuffer de software
    SDL_SetHint(SDL_HINT_DC_VIDEO_MODE, "SDL_DC_OPENGL_VIDEO");
    fs_chdir("/rd");   // (el romdisk que va adentro del ejecutable: ahi van los assets)
}
SDL_Window* W3dConsoleWindow(const char* title) {
    SDL_Window* w = SDL_CreateWindow(title, 0, 0, 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (!w) return NULL;
    if (!SDL_GL_CreateContext(w)) { w3dLogfE("[dreamcast] sin contexto GL: %s", SDL_GetError()); SDL_DestroyWindow(w); return NULL; }
    return w;
}
void W3dConsoleSize(int* w, int* h) { *w = 640; *h = 480; }
std::string W3dConsoleData(const char*) { return "/ram"; }   // (lo unico escribible: el disco en RAM)
void W3dConsoleFrameBegin() {}
void W3dConsoleFrameFinish(SDL_Window* v) { SDL_GL_SwapWindow(v); }
void W3dConsoleEnd(SDL_Window*) {}

#else
#error "W3D_CONSOLE sin consola: definir W3D_PSP, W3D_VITA, W3D_3DS o W3D_DREAMCAST"
#endif
#endif // W3D_CONSOLE
