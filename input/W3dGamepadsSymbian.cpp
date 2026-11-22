// ============================================================================
//  W3dGamepadsSymbian.cpp — los joysticks de Symbian para W3dGamepads.h, por el driver Bluetooth bthid-s60
//  (github.com/Dante-Leoncini/bthid-s60): DualShock 3, DualShock 4 y los genericos que hablan como ellos, hasta 4.
//
//  bthidhost.dll se carga en runtime (RLibrary) y sus funciones se buscan por ORDINAL (eabi/bthidhostu.def): si el
//  telefono no tiene el driver, la app arranca igual y no hay mandos; y el ejecutable no importa nada que un telefono
//  sin el driver (o el N95 de fabrica) no tenga. Los tipos de abajo son copia de los de inc/bthidmanager.h (el
//  formato que viaja al servidor): si el driver los cambia, cambiarlos aca.
// ============================================================================
#ifdef W3D_SYMBIAN
#include <e32std.h>
#include "W3dGamepads.h"
#include "w3dlog.h"

namespace {

// ---- copia de inc/bthidmanager.h (bthid-s60) ----
enum { EBtHidPadNone = 0, EBtHidPadUnknown = 1, EBtHidPadPs3 = 3, EBtHidPadPs4 = 4, EBtHidPadWii = 5 };
enum { EBtHidConnected = 2 };
enum TBtHidPadButton {
    EBtHidPadUp, EBtHidPadRight, EBtHidPadDown, EBtHidPadLeft,
    EBtHidPadCross, EBtHidPadCircle, EBtHidPadSquare, EBtHidPadTriangle,
    EBtHidPadL1, EBtHidPadR1, EBtHidPadL2, EBtHidPadR2,
    EBtHidPadSelect, EBtHidPadStart, EBtHidPadL3, EBtHidPadR3, EBtHidPadPS,
    EBtHidPadButtons
};
struct TBtHidPadState {
    TInt    iKind;
    TInt    iState;
    TUint32 iButtons;                    // bit (1 << TBtHidPadButton)
    TUint8  iAxis[4];                    // LX LY RX RY, 0-255 (128 al centro)
    TUint8  iPressure[EBtHidPadButtons]; // 0-255 (el DS4 solo en L2/R2)
    TInt    iRumble;
    TInt    iReports;
    TUint8  iAddr[6];
};
typedef TPckgBuf<TBtHidPadState> TBtHidPadStatePckg;
struct TBtHidWiiState {
    TInt    iConnected;
    TUint32 iButtons;
    TInt    iAccel[3];
    TReal32 iAccelG[3];     // en g, con la calibracion del control
    TInt    iMotionPlus;
    TInt    iGyro[3];
    TReal32 iGyroDps[3];
    TInt    iGoCount;
    TInt    iGoX[4];
    TInt    iGoY[4];
    TInt    iGoCorner[4];
    TInt    iAim;
    TReal32 iAimX, iAimY;
    TInt    iShots;
    TReal32 iShotX, iShotY;
    TInt    iShotAim;
    TInt    iBattery;
    TInt    iReports;
};
typedef TPckgBuf<TBtHidWiiState> TBtHidWiiStatePckg;

// RBtHidManager es una RSessionBase sin datos propios: alcanza con una sesion y llamar a sus funciones exportadas
// pasandole la sesion como 'this' (en ARM EABI el this va primero, como cualquier argumento)
class RSessionBtHid : public RSessionBase {};
typedef TInt (*TFnConnect)(RSessionBtHid*);
typedef TInt (*TFnState)(RSessionBtHid*, TInt, TBtHidPadStatePckg&);
typedef TInt (*TFnVibrate)(RSessionBtHid*, TInt, TBool);
typedef TInt (*TFnWii)(RSessionBtHid*, TInt, TBtHidWiiStatePckg&);
const TInt KOrdConnect = 2, KOrdPadState = 9, KOrdSetPadRumble = 10, KOrdWiiState = 14;

RLibrary     gLib;
RSessionBtHid gSession;
bool         gFound = false, gReady = false;
TFnState    gFnState     = 0;
TFnVibrate    gFnVibrate = 0;
TFnWii       gFnWii = 0;   // (0 con un driver sin Wiimote: anterior a la 0.10)
bool        gVibrating[w3dEngine::GamepadsMax] = { false, false, false, false };

bool Driver() {
    if (gFound) return gReady;
    gFound = true;
    _LIT(KDll, "bthidhost.dll");
    if (gLib.Load(KDll) != KErrNone) {
        w3dLog("[mandos] sin bthidhost.dll (el driver Bluetooth bthid-s60 no esta instalado): sin joysticks");
        return false;
    }
    TFnConnect connect = (TFnConnect)gLib.Lookup(KOrdConnect);
    gFnState   = (TFnState)gLib.Lookup(KOrdPadState);
    gFnVibrate = (TFnVibrate)gLib.Lookup(KOrdSetPadRumble);
    gFnWii = (TFnWii)gLib.Lookup(KOrdWiiState);
    if (!connect || !gFnState || !gFnVibrate) {
        w3dLog("[mandos] bthidhost.dll es de una version sin joysticks: sin joysticks");
        gLib.Close();
        return false;
    }
    const TInt err = connect(&gSession);
    if (err != KErrNone) {
        w3dLogf("[mandos] no se pudo hablar con el servidor de bthid (error %d): sin joysticks", err);
        gLib.Close();
        return false;
    }
    w3dLog("[mandos] driver bthid-s60: joysticks de PS3/PS4 por Bluetooth (hasta 4)");
    gReady = true;
    return true;
}

float Stick(TUint8 v) {
    float f = ((float)v - 128.0f) / 127.0f;
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return f;
}

} // namespace

namespace w3dEngine {

bool GamepadRead(int i, GamepadState& e) {
    if (i < 0 || i >= GamepadsMax || !Driver()) return false;
    TBtHidPadStatePckg pk;
    if (gFnState(&gSession, i, pk) != KErrNone) return false;
    const TBtHidPadState& s = pk();
    if (s.iKind == EBtHidPadNone || s.iState != EBtHidConnected) return false;
    e.type = (s.iKind == EBtHidPadPs3) ? GamepadPS3 : (s.iKind == EBtHidPadPs4) ? GamepadPS4
           : (s.iKind == EBtHidPadWii) ? GamepadWii : GenericGamepad;
    e.axes[GamepadLX] = Stick(s.iAxis[0]); e.axes[GamepadLY] = Stick(s.iAxis[1]);
    e.axes[GamepadRX] = Stick(s.iAxis[2]); e.axes[GamepadRY] = Stick(s.iAxis[3]);
    // los gatillos: la presion (analogica); si el mando no la manda, el boton entero
    const TUint32 b   = s.iButtons;
    e.axes[GamepadLT] = s.iPressure[EBtHidPadL2] ? s.iPressure[EBtHidPadL2] / 255.0f : ((b >> EBtHidPadL2) & 1) ? 1.0f : 0.0f;
    e.axes[GamepadRT] = s.iPressure[EBtHidPadR2] ? s.iPressure[EBtHidPadR2] / 255.0f : ((b >> EBtHidPadR2) & 1) ? 1.0f : 0.0f;
    // PlayStation -> el orden de SDL (disposicion de Xbox: A abajo, B derecha, X izquierda, Y arriba)
    static const int kOf[GamepadButtons] = {
        EBtHidPadCross, EBtHidPadCircle, EBtHidPadSquare, EBtHidPadTriangle, EBtHidPadSelect, EBtHidPadPS,
        EBtHidPadStart, EBtHidPadL3, EBtHidPadR3, EBtHidPadL1, EBtHidPadR1,
        EBtHidPadUp, EBtHidPadDown, EBtHidPadLeft, EBtHidPadRight };
    for (int k = 0; k < GamepadButtons; k++) e.buttons[k] = ((b >> kOf[k]) & 1) != 0;
    return true;
}

void GamepadVibrate(int i, bool yes) {
    if (i < 0 || i >= GamepadsMax || !Driver() || gVibrating[i] == yes) return;
    gVibrating[i] = yes;
    gFnVibrate(&gSession, i, yes ? ETrue : EFalse);
}

bool GamepadAccelerometer(int i, float g[3]) {
    if (i < 0 || i >= GamepadsMax || !Driver() || !gFnWii) return false;
    TBtHidWiiStatePckg pk;
    if (gFnWii(&gSession, i, pk) != KErrNone) return false;
    const TBtHidWiiState& s = pk();
    if (!s.iConnected) return false;
    for (int k = 0; k < 3; k++) g[k] = s.iAccelG[k];
    return true;
}

void GamepadsClose() {
    if (!gReady) return;
    for (int i = 0; i < GamepadsMax; i++) if (gVibrating[i]) GamepadVibrate(i, false);
    gSession.Close();
    gLib.Close();
    gReady = false; gFound = false;
}

} // namespace w3dEngine
#endif
