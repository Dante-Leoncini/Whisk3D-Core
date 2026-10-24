// ============================================================================
//  W3dVibrar.cpp — ver W3dVibrar.h.
//
//  WEB: dos caminos, porque no hay uno solo que sirva en todos lados.
//    · navigator.vibrate(ms): Android. Preciso, se elige la duracion.
//    · iOS/Safari: navigator.vibrate NO EXISTE. Lo unico que dispara el motor
//      haptico desde una pagina es activar un <input type="checkbox" switch>
//      (iOS 17.4+): el sistema hace su "tic" al cambiarlo. No se puede elegir
//      intensidad ni duracion, y Apple lo puede cerrar cuando quiera; por eso
//      todo esto vive detras de una API que promete poco.
//  ANDROID: el vibrador del telefono por JNI, con el metodo vibrar(int) de la
//  actividad del juego (lo genera Compilar juego).
//  SYMBIAN/otros: el hueco esta listo (ver el #else) para engancharlo con la
//  API de vibracion del sistema; los MANDOS vibran por W3dScriptMandosVibrarHook.
// ============================================================================
#include "W3dVibrar.h"
#include "W3dConfig.h"

#ifdef __EMSCRIPTEN__
    #include <emscripten.h>
#endif
#ifdef __ANDROID__
    #include <jni.h>
    #include <SDL2/SDL.h>   // SDL_AndroidGetJNIEnv / SDL_AndroidGetActivity
#endif

// (script/W3dScript.cpp: el vibrar() de lua y el motor de los mandos)
extern void (*W3dScriptVibrarHook)(int ms, float fuerza);
extern void (*W3dScriptMandosVibrarHook)(int ms, float fuerza);

namespace w3dEngine {

static bool gActiva = true;
static bool gListo  = false;

// (la fuerza en un vibrador que solo prende y apaga: el pulso dura menos)
static int MsConFuerza(int ms, float f) {
    if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
    const int m = (int)(ms * (0.35f + 0.65f * f) + 0.5f);
    return m < 1 ? 1 : m;
}

#ifdef __EMSCRIPTEN__

// 1 = el navegador tiene navigator.vibrate (Android). 0 = hay que usar el truco (iOS).
EM_JS(int, w3dVibJS_Nativa, (), {
    return (navigator && typeof navigator.vibrate === 'function') ? 1 : 0;
});

// Prepara el interruptor escondido que usa el truco de iOS. Tiene que estar EN el documento
// y no puede estar display:none (si no, iOS no lo considera interactuable).
EM_JS(void, w3dVibJS_Init, (), {
    if (Module.__w3dVibSw) return;
    try {
        var l = document.createElement('label');
        l.style.cssText = 'position:fixed;left:-100px;top:-100px;width:1px;height:1px;' +
                          'opacity:0.001;pointer-events:none;';
        var i = document.createElement('input');
        i.type = 'checkbox';
        i.setAttribute('switch', '');        // iOS 17.4+: lo dibuja como interruptor -> haptica
        l.appendChild(i);
        document.body.appendChild(l);
        Module.__w3dVibSw = i;
    } catch (e) {}
});

EM_JS(void, w3dVibJS_Vibrar, (int ms), {
    try {
        if (navigator && typeof navigator.vibrate === 'function') { navigator.vibrate(ms); return; }
        // iOS: el "tic" sale de ALTERNAR el interruptor. Se alterna y listo; el valor da igual.
        var i = Module.__w3dVibSw;
        if (i) { i.checked = !i.checked; i.dispatchEvent(new Event('change', { bubbles: false })); }
    } catch (e) {}
});

EM_JS(int, w3dVibJS_Disponible, (), {
    if (navigator && typeof navigator.vibrate === 'function') return 1;
    return Module.__w3dVibSw ? 1 : 0;      // el truco: al menos se puede intentar
});

void VibrarInit() {
    if (gListo) return;
    gListo = true;
    ConfigLoad();
    gActiva = ConfigGetInt("vibracion", 1) != 0;
    w3dVibJS_Init();
}

void Vibrar(VibFuerza f) {
    if (!gListo) VibrarInit();   // PRIMERO cargar la config: sino la primera vibracion ignora el ajuste guardado
    if (!gActiva) return;
    int ms = (f == VibGolpe) ? 32 : (f == VibToque ? 18 : 10);
    w3dVibJS_Vibrar(ms);
}

bool VibrarDisponible() { if (!gListo) VibrarInit(); return w3dVibJS_Disponible() != 0; }

void VibrarMs(int ms, float fuerza) {
    if (!gListo) VibrarInit();
    if (!gActiva || ms <= 0) return;
    w3dVibJS_Vibrar(MsConFuerza(ms, fuerza));
    if (W3dScriptMandosVibrarHook) W3dScriptMandosVibrarHook(ms, fuerza);
}

#elif defined(__ANDROID__)   // ---------------- Android: el vibrador del telefono ----------------

// el metodo vibrar(int) de la actividad (JuegoActivity, la genera Compilar juego). El editor de Android no lo tiene:
// la primera vez que falta no se pregunta mas
static bool gSinMetodo = false;
static void VibrarAndroid(int ms) {
    if (gSinMetodo || ms <= 0) return;
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (!env || !act) return;
    jclass cl = env->GetObjectClass(act);
    jmethodID mid = cl ? env->GetMethodID(cl, "vibrar", "(I)V") : 0;
    if (mid) env->CallVoidMethod(act, mid, (jint)ms);
    else gSinMetodo = true;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (cl) env->DeleteLocalRef(cl);
    env->DeleteLocalRef(act);
}

void VibrarInit() {
    if (gListo) return;
    gListo = true;
    ConfigLoad();
    gActiva = ConfigGetInt("vibracion", 1) != 0;
}

void Vibrar(VibFuerza f) {
    if (!gListo) VibrarInit();
    if (!gActiva) return;
    VibrarAndroid((f == VibGolpe) ? 32 : (f == VibToque ? 18 : 10));
}

bool VibrarDisponible() { if (!gListo) VibrarInit(); return !gSinMetodo; }

void VibrarMs(int ms, float fuerza) {
    if (!gListo) VibrarInit();
    if (!gActiva || ms <= 0) return;
    VibrarAndroid(MsConFuerza(ms, fuerza));
    if (W3dScriptMandosVibrarHook) W3dScriptMandosVibrarHook(ms, fuerza);   // (un mando conectado al telefono)
}

#else   // ---------------- escritorio / Symbian / consolas ----------------

void VibrarInit() {
    if (gListo) return;
    gListo = true;
    ConfigLoad();
    gActiva = ConfigGetInt("vibracion", 1) != 0;
}

// Hueco a completar por plataforma: en Symbian con la API de vibracion del telefono, en
// escritorio con el rumble del mando. Mientras tanto no hace nada y nadie se rompe.
void Vibrar(VibFuerza /*f*/) { if (!gListo) VibrarInit(); }

bool VibrarDisponible() { return W3dScriptMandosVibrarHook != 0; }

void VibrarMs(int ms, float fuerza) {
    if (!gListo) VibrarInit();
    if (!gActiva || ms <= 0) return;
    if (W3dScriptMandosVibrarHook) W3dScriptMandosVibrarHook(ms, fuerza);
}

#endif

void VibrarSetActiva(bool activa) {
    if (!gListo) VibrarInit();
    gActiva = activa;
    ConfigSetInt("vibracion", activa ? 1 : 0);
    ConfigSave();
}

bool VibrarActiva() { if (!gListo) VibrarInit(); return gActiva; }

// vibrar() de lua: donde se compila este .cpp manda el (respeta la opcion)
namespace {
struct InstalarVibrar { InstalarVibrar() { W3dScriptVibrarHook = w3dEngine::VibrarMs; } };
static InstalarVibrar gInstalarVibrar;
}

} // namespace w3dEngine
