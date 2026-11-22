// ============================================================================
//  W3dTiltSymbian.cpp — el acelerometro de Symbian S60 3rd FP1 (el N95) para W3dTilt.h.
//
//  LA API: la "Sensor API" de Nokia para S60 3rd FP1 (RRSensorApi.dll: CRRSensorApi, MRRSensorDataListener). Su
//  cabecera venia en un plug-in del SDK que ya no se consigue (no esta en los SDK de Symbian^3 ni de S60 3.2, que
//  traen la Sensor Framework nueva, que el N95 no tiene), asi que la DLL se carga con RLibrary al prender y lo que no
//  se sabe se averigua en el telefono (y queda en el log):
//    · los ordinales: FindSensorsL(RArray<TRRSensorInfo>&) es el que llena la lista; NewL(TRRSensorInfo), el otro;
//    · TRRSensorInfo = { TInt categoria, TInt id, TBuf<N> nombre }: su tamano sale del largo maximo del TBuf;
//    · los datos llegan por HandleDataEventL(TRRSensorInfo, TRRSensorEvent) con las dos estructuras por valor: el
//      oyente la declara con argumentos variables y lee las palabras en orden (AAPCS: la primera estructura empieza
//      en r1 y sigue en la pila; despues van los tres TInt de TRRSensorEvent, x y z del telefono).
//  El acelerometro es el sensor 0x10273024. Si el telefono no tiene la DLL, Open() da false y el juego sigue.
//  Los avisos llegan por un objeto activo del hilo principal (el bucle del juego es un CIdle: corren entre cuadros).
// ============================================================================
#ifdef W3D_SYMBIAN
#include "W3dTilt.h"
#include "w3dlog.h"
#include <e32base.h>
#include <e32std.h>
#include <stdarg.h>
#include <string.h>

namespace w3dEngine {
namespace tilt_platform {

namespace {
const TInt KAccelerometer = 0x10273024;
// TRRSensorInfo con lugar de sobra: la DLL copia la suya (mas chica) y NewL lee solo lo suyo
struct TInfo {
    TInt iCategory;
    TInt iId;
    TUint32 iRest[62];
};
// CRRSensorApi: despues de lo de CBase, sus virtuales
class CSensorRR : public CBase {
public:
    virtual void AddDataListener(TAny* aListener) = 0;
    virtual void RemoveDataListener() = 0;
};
typedef void (*TFindL)(RArray<TInfo>& aList);
typedef CSensorRR* (*TNewL)(TInfo aInfo);

RLibrary gDll;
TBool gOpenDll = EFalse;
CSensorRR* gSensor = 0;
int gWordsInfo = 0;        // el tamano de TRRSensorInfo en palabras
float gLast[3];
unsigned gSamples = 0;
int gNotices = 0;              // (las primeras muestras van al log enteras)
char gInfo[64] = "sin sensor";

// el oyente (MRRSensorDataListener): una sola virtual
class TListener {
public:
    virtual void HandleDataEventL(TInt aWord0, ...);
};

void TListener::HandleDataEventL(TInt aWord0, ...) {
    TInt w[80];
    const int n = gWordsInfo + 3;
    w[0] = aWord0;
    va_list ap;
    va_start(ap, aWord0);
    for (int k = 1; k < n && k < 80; k++) w[k] = va_arg(ap, TInt);
    va_end(ap);
    if (w[1] != KAccelerometer) {
        // (por las dudas, por referencia: la primera palabra seria un puntero a TRRSensorInfo)
        if (gNotices < 3) w3dLogf("inclinacion: aviso de otro sensor (0x%08x 0x%08x)", (unsigned)w[0], (unsigned)w[1]);
        gNotices++;
        return;
    }
    const TInt* e = w + gWordsInfo;
    gLast[0] = (float)e[0];
    gLast[1] = (float)e[1];
    gLast[2] = (float)e[2];
    gSamples++;
    if (gNotices < 4) {
        w3dLogf("inclinacion: muestra %u: %d %d %d", gSamples, (int)e[0], (int)e[1], (int)e[2]);
        gNotices++;
    }
}
TListener gListener;

// el tamano de TRRSensorInfo: 8 bytes y el TBuf (largo y tipo, largo maximo, los caracteres)
int words_of(const TInfo& i) {
    const TUint32 length = i.iRest[0] & 0x0FFFFFFF, type = i.iRest[0] >> 28, max = i.iRest[1];
    if (type != 3 || max == 0 || max > 200 || length > max) return 0;   // (3 = EBuf)
    const TUint8* c   = (const TUint8*)&i.iRest[2];
    const bool from16 = length < 2 || (c[1] == 0 && c[3] == 0);
    const int bytes   = 16 + (int)((from16 ? 2 * max : max) + 3 & ~3u);
    return bytes / 4;
}

void name_of(const TInfo& i, char* s, int size) {
    const TUint32 length = i.iRest[0] & 0x0FFFFFFF;
    const TUint8* c   = (const TUint8*)&i.iRest[2];
    const bool from16 = length < 2 || (c[1] == 0 && c[3] == 0);
    int k = 0;
    for (TUint32 j = 0; j < length && k < size - 1 && j < 60; j++) s[k++] = (char)(from16 ? c[2 * j] : c[j]);
    s[k] = 0;
}
}

bool Open() {
    if (gSensor) return true;
    if (!gOpenDll) {
        const TInt r = gDll.Load(_L("RRSensorApi.dll"));
        if (r != KErrNone) {
            w3dLogf("inclinacion: no hay RRSensorApi.dll (%d)", (int)r);
            strcpy(gInfo, "sin sensor");
            return false;
        }
        gOpenDll = ETrue;
    }
    int exported = 0;
    while (exported < 16 && gDll.Lookup(exported + 1)) exported++;
    // FindSensorsL: el que llena la lista (por ordinal: probando, empezando por el 1)
    RArray<TInfo> list(4);
    int ordFind = 0;
    for (int o = 1; o <= exported && !ordFind; o++) {
        list.Reset();
        TFindL find = (TFindL)gDll.Lookup(o);
        TRAPD(r, find(list));
        if (r == KErrNone && list.Count() > 0) ordFind = o;
        else w3dLogf("inclinacion: el ordinal %d no lista sensores (%d)", o, (int)r);
    }
    if (!ordFind) {
        list.Close();
        w3dLogf("inclinacion: RRSensorApi.dll con %d funciones y ninguna lista sensores", exported);
        strcpy(gInfo, "sin sensor");
        return false;
    }
    int picked = -1;
    for (int k = 0; k < list.Count(); k++) {
        char name[64];
        name_of(list[k], name, sizeof(name));
        w3dLogf("inclinacion: sensor %d: categoria %d, id 0x%08x, '%s' (TRRSensorInfo de %d bytes)", k,
                (int)list[k].iCategory, (unsigned)list[k].iId, name, words_of(list[k]) * 4);
        if (picked < 0 && list[k].iId == KAccelerometer) picked = k;
    }
    if (picked < 0) {
        list.Close();
        w3dLog("inclinacion: ningun sensor es el acelerometro");
        strcpy(gInfo, "sin acelerometro");
        return false;
    }
    TInfo info = list[picked];
    list.Close();
    gWordsInfo = words_of(info);
    if (gWordsInfo <= 2) gWordsInfo = 2 + 2 + 15;   // (si no se entiende el TBuf: TBuf<30>, lo mas comun)
    // NewL: otro ordinal
    for (int o = 1; o <= exported && !gSensor; o++) {
        if (o == ordFind) continue;
        TNewL newValue = (TNewL)gDll.Lookup(o);
        CSensorRR* s = 0;
        TRAPD(r, s = newValue(info));
        if (r == KErrNone && s) gSensor = s;
        else w3dLogf("inclinacion: el ordinal %d no crea el sensor (%d)", o, (int)r);
    }
    if (!gSensor) {
        strcpy(gInfo, "sin sensor");
        return false;
    }
    gSamples = 0;
    gNotices = 0;
    gSensor->AddDataListener(&gListener);
    w3dLogf("inclinacion: acelerometro prendido (RRSensorApi, FindSensorsL = %d, %d funciones)", ordFind, exported);
    strcpy(gInfo, "RRSensorApi");
    return true;
}

void Close() {
    if (gSensor) {
        gSensor->RemoveDataListener();
        delete gSensor;
        gSensor = 0;
        w3dLog("inclinacion: acelerometro apagado");
    }
    // (la DLL queda cargada: prenderlo otra vez no la vuelve a buscar)
}

bool Raw(float v[3], unsigned& n) {
    n = gSamples;
    if (!gSensor || !gSamples) return false;
    v[0] = gLast[0];
    v[1] = gLast[1];
    v[2] = gLast[2];
    return true;
}

const char* Info() { return gInfo; }

} // namespace tilt_platform
} // namespace w3dEngine
#endif // W3D_SYMBIAN
