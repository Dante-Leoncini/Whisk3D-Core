// ============================================================================
//  W3dVibrate.cpp — ver W3dVibrate.h.
//
//  WEB: dos caminos, porque no hay uno solo que sirva en todos lados.
//    · navigator.vibrate(ms): Android. Preciso, se elige la duracion.
//    · iOS/Safari: navigator.vibrate NO EXISTE. Lo unico que dispara el motor
//      haptico desde una pagina es activar un <input type="checkbox" switch>
//      (iOS 17.4+): el sistema hace su "tic" al cambiarlo. No se puede elegir
//      intensidad ni duracion, y Apple lo puede cerrar cuando quiera; por eso
//      todo esto vive detras de una API que promete poco.
//  ANDROID: el vibrador del telefono por JNI, con el metodo vibrate(int) de la
//  actividad del juego (lo genera Compilar juego).
//  SYMBIAN (S60 3ra edicion, el N95): el vibrador del telefono con CHWRMVibra
//  (hwrmvibraclient.lib; no pide capacidades). Si el perfil tiene la vibracion
//  apagada, el sistema no vibra y no pasa nada.
//  ESCRITORIO/otros: solo los MANDOS (W3dScriptGamepadsVibrateHook).
// ============================================================================
#include "W3dVibrate.h"
#include "W3dConfig.h"

#ifdef __EMSCRIPTEN__
    #include <emscripten.h>
#endif
#ifdef W3D_SYMBIAN
    #include <hwrmvibra.h>   // CHWRMVibra: el vibrador del telefono
#endif
#ifdef __ANDROID__
    #include <jni.h>
    #include <SDL2/SDL.h>   // SDL_AndroidGetJNIEnv / SDL_AndroidGetActivity
#endif

// (script/W3dScript.cpp: el vibrate() de lua y el motor de los mandos)
extern void (*W3dScriptVibrateHook)(int ms, float force);
extern void (*W3dScriptGamepadsVibrateHook)(int ms, float force);

namespace w3dEngine {

static bool gActive = true;
static bool gReady  = false;

// (la fuerza en un vibrador que solo prende y apaga: el pulso dura menos)
static int MsWithForce(int ms, float f) {
    if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
    const int m = (int)(ms * (0.35f + 0.65f * f) + 0.5f);
    return m < 1 ? 1 : m;
}

#ifdef __EMSCRIPTEN__

// 1 = el navegador tiene navigator.vibrate (Android). 0 = hay que usar el truco (iOS).
EM_JS(int, w3dVibJS_Native, (), {
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

EM_JS(void, w3dVibJS_Vibrate, (int ms), {
    try {
        if (navigator && typeof navigator.vibrate === 'function') { navigator.vibrate(ms); return; }
        // iOS: el "tic" sale de ALTERNAR el interruptor. Se alterna y listo; el valor da igual.
        var i = Module.__w3dVibSw;
        if (i) { i.checked = !i.checked; i.dispatchEvent(new Event('change', { bubbles: false })); }
    } catch (e) {}
});

EM_JS(int, w3dVibJS_Available, (), {
    if (navigator && typeof navigator.vibrate === 'function') return 1;
    return Module.__w3dVibSw ? 1 : 0;      // el truco: al menos se puede intentar
});

void VibrateInit() {
    if (gReady) return;
    gReady = true;
    ConfigLoad();
    gActive = ConfigGetInt("vibracion", 1) != 0;
    w3dVibJS_Init();
}

void Vibrate(VibForce f) {
    if (!gReady) VibrateInit();   // PRIMERO cargar la config: sino la primera vibracion ignora el ajuste guardado
    if (!gActive) return;
    int ms = (f == VibHit) ? 32 : (f == VibTouch ? 18 : 10);
    w3dVibJS_Vibrate(ms);
}

bool VibrateAvailable() { if (!gReady) VibrateInit(); return w3dVibJS_Available() != 0; }

void VibrateMs(int ms, float force) {
    if (!gReady) VibrateInit();
    if (!gActive || ms <= 0) return;
    w3dVibJS_Vibrate(MsWithForce(ms, force));
    if (W3dScriptGamepadsVibrateHook) W3dScriptGamepadsVibrateHook(ms, force);
}

#elif defined(__ANDROID__)   // ---------------- Android: el vibrador del telefono ----------------

// el metodo vibrate(int) de la actividad (JuegoActivity, la genera Compilar juego; Whisk3DActivity, el editor). Si una
// actividad no lo tiene, la primera vez que falta no se pregunta mas
static bool gNoMethod = false;
static void VibrateAndroid(int ms) {
    if (gNoMethod || ms <= 0) return;
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (!env || !act) return;
    jclass cl = env->GetObjectClass(act);
    jmethodID mid = cl ? env->GetMethodID(cl, "vibrate", "(I)V") : 0;
    if (mid) env->CallVoidMethod(act, mid, (jint)ms);
    else gNoMethod = true;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (cl) env->DeleteLocalRef(cl);
    env->DeleteLocalRef(act);
}

void VibrateInit() {
    if (gReady) return;
    gReady = true;
    ConfigLoad();
    gActive = ConfigGetInt("vibracion", 1) != 0;
}

void Vibrate(VibForce f) {
    if (!gReady) VibrateInit();
    if (!gActive) return;
    VibrateAndroid((f == VibHit) ? 32 : (f == VibTouch ? 18 : 10));
}

bool VibrateAvailable() { if (!gReady) VibrateInit(); return !gNoMethod; }

void VibrateMs(int ms, float force) {
    if (!gReady) VibrateInit();
    if (!gActive || ms <= 0) return;
    VibrateAndroid(MsWithForce(ms, force));
    if (W3dScriptGamepadsVibrateHook) W3dScriptGamepadsVibrateHook(ms, force);   // (un mando conectado al telefono)
}

#elif defined(W3D_SYMBIAN)   // ---------------- Symbian: el vibrador del telefono ----------------
static CHWRMVibra* gVibrates = 0;
static bool gVibratesFailed = false;   // (no se pudo abrir: no se vuelve a intentar)
static void VibrateSymbian(int ms, float force) {
    if (gVibratesFailed || ms <= 0) return;
    if (!gVibrates) {
        TRAPD(err, gVibrates = CHWRMVibra::NewL());
        if (err != KErrNone || !gVibrates) { gVibrates = 0; gVibratesFailed = true; return; }
    }
    // la intensidad va de -100 a 100 (0 = la del perfil); un poco de fuerza siempre, que se sienta
    if (force < 0.0f) force = 0.0f; if (force > 1.0f) force = 1.0f;
    const TInt intensity = (TInt)(30.0f + 70.0f * force);
    TRAPD(err2, gVibrates->StartVibraL((TInt)ms, intensity));
    (void)err2;   // (perfil sin vibracion: KErrAccessDenied, se ignora)
}
void VibrateInit() {
    if (gReady) return;
    gReady = true;
    ConfigLoad();
    gActive = ConfigGetInt("vibracion", 1) != 0;
}
void Vibrate(VibForce f) {
    if (!gReady) VibrateInit();
    if (!gActive) return;
    VibrateSymbian((f == VibHit) ? 32 : (f == VibTouch ? 18 : 10), f == VibHit ? 1.0f : 0.6f);
}
bool VibrateAvailable() { if (!gReady) VibrateInit(); return !gVibratesFailed; }
void VibrateMs(int ms, float force) {
    if (!gReady) VibrateInit();
    if (!gActive || ms <= 0) return;
    VibrateSymbian(ms, force);
    if (W3dScriptGamepadsVibrateHook) W3dScriptGamepadsVibrateHook(ms, force);   // (un mando conectado al telefono)
}

#else   // ---------------- escritorio / consolas ----------------

void VibrateInit() {
    if (gReady) return;
    gReady = true;
    ConfigLoad();
    gActive = ConfigGetInt("vibracion", 1) != 0;
}

// En escritorio vibra el mando (el rumble); sin mando no hace nada y nadie se rompe.
void Vibrate(VibForce /*f*/) { if (!gReady) VibrateInit(); }

bool VibrateAvailable() { return W3dScriptGamepadsVibrateHook != 0; }

void VibrateMs(int ms, float force) {
    if (!gReady) VibrateInit();
    if (!gActive || ms <= 0) return;
    if (W3dScriptGamepadsVibrateHook) W3dScriptGamepadsVibrateHook(ms, force);
}

#endif

void VibrateSetActive(bool active) {
    if (!gReady) VibrateInit();
    gActive = active;
    ConfigSetInt("vibracion", active ? 1 : 0);
    ConfigSave();
}

bool VibrateActive() { if (!gReady) VibrateInit(); return gActive; }

// vibrate() de lua: donde se compila este .cpp manda el (respeta la opcion)
namespace {
struct InstallVibrate { InstallVibrate() { W3dScriptVibrateHook = w3dEngine::VibrateMs; } };
static InstallVibrate gInstallVibrate;
}

} // namespace w3dEngine
