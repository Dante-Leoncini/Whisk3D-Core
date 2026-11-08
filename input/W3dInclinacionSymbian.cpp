// ============================================================================
//  W3dInclinacionSymbian.cpp — el acelerometro de Symbian S60 3rd FP1 (el N95) para W3dInclinacion.h.
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
//  El acelerometro es el sensor 0x10273024. Si el telefono no tiene la DLL, Abrir() da false y el juego sigue.
//  Los avisos llegan por un objeto activo del hilo principal (el bucle del juego es un CIdle: corren entre cuadros).
// ============================================================================
#ifdef W3D_SYMBIAN
#include "W3dInclinacion.h"
#include "w3dlog.h"
#include <e32base.h>
#include <e32std.h>
#include <stdarg.h>
#include <string.h>

namespace w3dEngine {
namespace inclinacion_plataforma {

namespace {
const TInt KAcelerometro = 0x10273024;
// TRRSensorInfo con lugar de sobra: la DLL copia la suya (mas chica) y NewL lee solo lo suyo
struct TInfo {
    TInt iCategoria;
    TInt iId;
    TUint32 iResto[62];
};
// CRRSensorApi: despues de lo de CBase, sus virtuales
class CSensorRR : public CBase {
public:
    virtual void AddDataListener(TAny* aOyente) = 0;
    virtual void RemoveDataListener() = 0;
};
typedef void (*TBuscarL)(RArray<TInfo>& aLista);
typedef CSensorRR* (*TNuevoL)(TInfo aInfo);

RLibrary gDll;
TBool gDllAbierta = EFalse;
CSensorRR* gSensor = 0;
int gPalabrasInfo = 0;        // el tamano de TRRSensorInfo en palabras
float gUltima[3];
unsigned gMuestras = 0;
int gAvisos = 0;              // (las primeras muestras van al log enteras)
char gInfo[64] = "sin sensor";

// el oyente (MRRSensorDataListener): una sola virtual
class TOyente {
public:
    virtual void HandleDataEventL(TInt aPalabra0, ...);
};

void TOyente::HandleDataEventL(TInt aPalabra0, ...) {
    TInt w[80];
    const int n = gPalabrasInfo + 3;
    w[0] = aPalabra0;
    va_list ap;
    va_start(ap, aPalabra0);
    for (int k = 1; k < n && k < 80; k++) w[k] = va_arg(ap, TInt);
    va_end(ap);
    if (w[1] != KAcelerometro) {
        // (por las dudas, por referencia: la primera palabra seria un puntero a TRRSensorInfo)
        if (gAvisos < 3) w3dLogf("inclinacion: aviso de otro sensor (0x%08x 0x%08x)", (unsigned)w[0], (unsigned)w[1]);
        gAvisos++;
        return;
    }
    const TInt* e = w + gPalabrasInfo;
    gUltima[0] = (float)e[0];
    gUltima[1] = (float)e[1];
    gUltima[2] = (float)e[2];
    gMuestras++;
    if (gAvisos < 4) {
        w3dLogf("inclinacion: muestra %u: %d %d %d", gMuestras, (int)e[0], (int)e[1], (int)e[2]);
        gAvisos++;
    }
}
TOyente gOyente;

// el tamano de TRRSensorInfo: 8 bytes y el TBuf (largo y tipo, largo maximo, los caracteres)
int palabras_de(const TInfo& i) {
    const TUint32 largo = i.iResto[0] & 0x0FFFFFFF, tipo = i.iResto[0] >> 28, maximo = i.iResto[1];
    if (tipo != 3 || maximo == 0 || maximo > 200 || largo > maximo) return 0;   // (3 = EBuf)
    const TUint8* c = (const TUint8*)&i.iResto[2];
    const bool de16 = largo < 2 || (c[1] == 0 && c[3] == 0);
    const int bytes = 16 + (int)((de16 ? 2 * maximo : maximo) + 3 & ~3u);
    return bytes / 4;
}

void nombre_de(const TInfo& i, char* s, int tam) {
    const TUint32 largo = i.iResto[0] & 0x0FFFFFFF;
    const TUint8* c = (const TUint8*)&i.iResto[2];
    const bool de16 = largo < 2 || (c[1] == 0 && c[3] == 0);
    int k = 0;
    for (TUint32 j = 0; j < largo && k < tam - 1 && j < 60; j++) s[k++] = (char)(de16 ? c[2 * j] : c[j]);
    s[k] = 0;
}
}

bool Abrir() {
    if (gSensor) return true;
    if (!gDllAbierta) {
        const TInt r = gDll.Load(_L("RRSensorApi.dll"));
        if (r != KErrNone) {
            w3dLogf("inclinacion: no hay RRSensorApi.dll (%d)", (int)r);
            strcpy(gInfo, "sin sensor");
            return false;
        }
        gDllAbierta = ETrue;
    }
    int exportadas = 0;
    while (exportadas < 16 && gDll.Lookup(exportadas + 1)) exportadas++;
    // FindSensorsL: el que llena la lista (por ordinal: probando, empezando por el 1)
    RArray<TInfo> lista(4);
    int ordBuscar = 0;
    for (int o = 1; o <= exportadas && !ordBuscar; o++) {
        lista.Reset();
        TBuscarL buscar = (TBuscarL)gDll.Lookup(o);
        TRAPD(r, buscar(lista));
        if (r == KErrNone && lista.Count() > 0) ordBuscar = o;
        else w3dLogf("inclinacion: el ordinal %d no lista sensores (%d)", o, (int)r);
    }
    if (!ordBuscar) {
        lista.Close();
        w3dLogf("inclinacion: RRSensorApi.dll con %d funciones y ninguna lista sensores", exportadas);
        strcpy(gInfo, "sin sensor");
        return false;
    }
    int elegido = -1;
    for (int k = 0; k < lista.Count(); k++) {
        char nombre[64];
        nombre_de(lista[k], nombre, sizeof(nombre));
        w3dLogf("inclinacion: sensor %d: categoria %d, id 0x%08x, '%s' (TRRSensorInfo de %d bytes)", k,
                (int)lista[k].iCategoria, (unsigned)lista[k].iId, nombre, palabras_de(lista[k]) * 4);
        if (elegido < 0 && lista[k].iId == KAcelerometro) elegido = k;
    }
    if (elegido < 0) {
        lista.Close();
        w3dLog("inclinacion: ningun sensor es el acelerometro");
        strcpy(gInfo, "sin acelerometro");
        return false;
    }
    TInfo info = lista[elegido];
    lista.Close();
    gPalabrasInfo = palabras_de(info);
    if (gPalabrasInfo <= 2) gPalabrasInfo = 2 + 2 + 15;   // (si no se entiende el TBuf: TBuf<30>, lo mas comun)
    // NewL: otro ordinal
    for (int o = 1; o <= exportadas && !gSensor; o++) {
        if (o == ordBuscar) continue;
        TNuevoL nuevo = (TNuevoL)gDll.Lookup(o);
        CSensorRR* s = 0;
        TRAPD(r, s = nuevo(info));
        if (r == KErrNone && s) gSensor = s;
        else w3dLogf("inclinacion: el ordinal %d no crea el sensor (%d)", o, (int)r);
    }
    if (!gSensor) {
        strcpy(gInfo, "sin sensor");
        return false;
    }
    gMuestras = 0;
    gAvisos = 0;
    gSensor->AddDataListener(&gOyente);
    w3dLogf("inclinacion: acelerometro prendido (RRSensorApi, FindSensorsL = %d, %d funciones)", ordBuscar, exportadas);
    strcpy(gInfo, "RRSensorApi");
    return true;
}

void Cerrar() {
    if (gSensor) {
        gSensor->RemoveDataListener();
        delete gSensor;
        gSensor = 0;
        w3dLog("inclinacion: acelerometro apagado");
    }
    // (la DLL queda cargada: prenderlo otra vez no la vuelve a buscar)
}

bool Cruda(float v[3], unsigned& n) {
    n = gMuestras;
    if (!gSensor || !gMuestras) return false;
    v[0] = gUltima[0];
    v[1] = gUltima[1];
    v[2] = gUltima[2];
    return true;
}

const char* Info() { return gInfo; }

} // namespace inclinacion_plataforma
} // namespace w3dEngine
#endif // W3D_SYMBIAN
