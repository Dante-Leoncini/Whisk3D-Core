// ============================================================================
//  W3dMandosSymbian.cpp — los joysticks de Symbian para W3dMandos.h, por el driver Bluetooth bthid-s60
//  (github.com/Dante-Leoncini/bthid-s60): DualShock 3, DualShock 4 y los genericos que hablan como ellos, hasta 4.
//
//  bthidhost.dll se carga en runtime (RLibrary) y sus funciones se buscan por ORDINAL (eabi/bthidhostu.def): si el
//  telefono no tiene el driver, la app arranca igual y no hay mandos; y el ejecutable no importa nada que un telefono
//  sin el driver (o el N95 de fabrica) no tenga. Los tipos de abajo son copia de los de inc/bthidmanager.h (el
//  formato que viaja al servidor): si el driver los cambia, cambiarlos aca.
// ============================================================================
#ifdef W3D_SYMBIAN
#include <e32std.h>
#include "W3dMandos.h"
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
    TInt    iIrCount;
    TInt    iIrX[4];
    TInt    iIrY[4];
    TInt    iIrCorner[4];
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
class RSesionBtHid : public RSessionBase {};
typedef TInt (*TFnConectar)(RSesionBtHid*);
typedef TInt (*TFnEstado)(RSesionBtHid*, TInt, TBtHidPadStatePckg&);
typedef TInt (*TFnVibrar)(RSesionBtHid*, TInt, TBool);
typedef TInt (*TFnWii)(RSesionBtHid*, TInt, TBtHidWiiStatePckg&);
const TInt KOrdConnect = 2, KOrdPadState = 9, KOrdSetPadRumble = 10, KOrdWiiState = 14;

RLibrary     gLib;
RSesionBtHid gSesion;
bool         gBuscado = false, gListo = false;
TFnEstado    gFnEstado = 0;
TFnVibrar    gFnVibrar = 0;
TFnWii       gFnWii = 0;   // (0 con un driver sin Wiimote: anterior a la 0.10)
bool        gVibrando[w3dEngine::MandosMax] = { false, false, false, false };

bool Driver() {
    if (gBuscado) return gListo;
    gBuscado = true;
    _LIT(KDll, "bthidhost.dll");
    if (gLib.Load(KDll) != KErrNone) {
        w3dLog("[mandos] sin bthidhost.dll (el driver Bluetooth bthid-s60 no esta instalado): sin joysticks");
        return false;
    }
    TFnConectar conectar = (TFnConectar)gLib.Lookup(KOrdConnect);
    gFnEstado = (TFnEstado)gLib.Lookup(KOrdPadState);
    gFnVibrar = (TFnVibrar)gLib.Lookup(KOrdSetPadRumble);
    gFnWii = (TFnWii)gLib.Lookup(KOrdWiiState);
    if (!conectar || !gFnEstado || !gFnVibrar) {
        w3dLog("[mandos] bthidhost.dll es de una version sin joysticks: sin joysticks");
        gLib.Close();
        return false;
    }
    const TInt err = conectar(&gSesion);
    if (err != KErrNone) {
        w3dLogf("[mandos] no se pudo hablar con el servidor de bthid (error %d): sin joysticks", err);
        gLib.Close();
        return false;
    }
    w3dLog("[mandos] driver bthid-s60: joysticks de PS3/PS4 por Bluetooth (hasta 4)");
    gListo = true;
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

bool MandoLeer(int i, MandoEstado& e) {
    if (i < 0 || i >= MandosMax || !Driver()) return false;
    TBtHidPadStatePckg pk;
    if (gFnEstado(&gSesion, i, pk) != KErrNone) return false;
    const TBtHidPadState& s = pk();
    if (s.iKind == EBtHidPadNone || s.iState != EBtHidConnected) return false;
    e.tipo = (s.iKind == EBtHidPadPs3) ? MandoPS3 : (s.iKind == EBtHidPadPs4) ? MandoPS4
           : (s.iKind == EBtHidPadWii) ? MandoWii : MandoGenerico;
    e.ejes[MandoLX] = Stick(s.iAxis[0]); e.ejes[MandoLY] = Stick(s.iAxis[1]);
    e.ejes[MandoRX] = Stick(s.iAxis[2]); e.ejes[MandoRY] = Stick(s.iAxis[3]);
    // los gatillos: la presion (analogica); si el mando no la manda, el boton entero
    const TUint32 b = s.iButtons;
    e.ejes[MandoLT] = s.iPressure[EBtHidPadL2] ? s.iPressure[EBtHidPadL2] / 255.0f : ((b >> EBtHidPadL2) & 1) ? 1.0f : 0.0f;
    e.ejes[MandoRT] = s.iPressure[EBtHidPadR2] ? s.iPressure[EBtHidPadR2] / 255.0f : ((b >> EBtHidPadR2) & 1) ? 1.0f : 0.0f;
    // PlayStation -> el orden de SDL (disposicion de Xbox: A abajo, B derecha, X izquierda, Y arriba)
    static const int kDe[MandoBotones] = {
        EBtHidPadCross, EBtHidPadCircle, EBtHidPadSquare, EBtHidPadTriangle, EBtHidPadSelect, EBtHidPadPS,
        EBtHidPadStart, EBtHidPadL3, EBtHidPadR3, EBtHidPadL1, EBtHidPadR1,
        EBtHidPadUp, EBtHidPadDown, EBtHidPadLeft, EBtHidPadRight };
    for (int k = 0; k < MandoBotones; k++) e.botones[k] = ((b >> kDe[k]) & 1) != 0;
    return true;
}

void MandoVibrar(int i, bool si) {
    if (i < 0 || i >= MandosMax || !Driver() || gVibrando[i] == si) return;
    gVibrando[i] = si;
    gFnVibrar(&gSesion, i, si ? ETrue : EFalse);
}

bool MandoAcelerometro(int i, float g[3]) {
    if (i < 0 || i >= MandosMax || !Driver() || !gFnWii) return false;
    TBtHidWiiStatePckg pk;
    if (gFnWii(&gSesion, i, pk) != KErrNone) return false;
    const TBtHidWiiState& s = pk();
    if (!s.iConnected) return false;
    for (int k = 0; k < 3; k++) g[k] = s.iAccelG[k];
    return true;
}

void MandosCerrar() {
    if (!gListo) return;
    for (int i = 0; i < MandosMax; i++) if (gVibrando[i]) MandoVibrar(i, false);
    gSesion.Close();
    gLib.Close();
    gListo = false; gBuscado = false;
}

} // namespace w3dEngine
#endif
