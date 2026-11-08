// ============================================================================
//  W3dTactil.cpp — ver W3dTactil.h. Cada dedo -> los eventos que SDL sintetiza en Android, en el mismo orden:
//  el primer dedo mueve el mouse (si cambio de lugar) y lo aprieta, y despues avisa como dedo; al subir, suelta
//  el mouse y despues avisa. El mouse queda en el dedo que lo agarro hasta que ese dedo sube.
// ============================================================================
#include "W3dTactil.h"

namespace w3dEngine {

namespace {
    enum { kMaxDedos = 10 };
    struct DedoTactil { bool abajo; float x, y; };   // x/y normalizados, como los guarda SDL
    DedoTactil gDedos[kMaxDedos];
    int  gDedoMouse = -1;            // el dedo que maneja el mouse (el primero que bajo); -1 = ninguno
    int  gMouseX = 0, gMouseY = 0;   // donde quedo el mouse (SDL arranca en 0,0)
    int  gAncho = 1, gAlto = 1;
    bool gEnEvento = false;
    TactilReceptor gReceptor = 0;

    void Entregar(TactilEvento& e) {
        if (!gReceptor) return;
        const bool antes = gEnEvento;
        gEnEvento = true;
        gReceptor(e);
        gEnEvento = antes;
    }

    TactilEvento Nuevo(TactilTipo tipo) {
        TactilEvento e;
        e.tipo = tipo; e.dedo = 0; e.x = e.y = e.xrel = e.yrel = 0; e.apretado = false;
        e.nx = e.ny = e.ndx = e.ndy = 0.0f;
        return e;
    }

    // SDL descarta el movimiento del mouse que no cambia la posicion
    void MouseA(int x, int y, bool apretado) {
        if (x < 0) x = 0;
        if (x > gAncho - 1) x = gAncho - 1;
        if (y < 0) y = 0;
        if (y > gAlto - 1) y = gAlto - 1;
        const int xrel = x - gMouseX, yrel = y - gMouseY;
        if (xrel == 0 && yrel == 0) return;
        gMouseX = x; gMouseY = y;
        TactilEvento e = Nuevo(TactilMouseMovio);
        e.x = x; e.y = y; e.xrel = xrel; e.yrel = yrel; e.apretado = apretado;
        Entregar(e);
    }

    void BotonMouse(bool apretado) {
        TactilEvento e = Nuevo(TactilMouseBoton);
        e.x = gMouseX; e.y = gMouseY; e.apretado = apretado;
        Entregar(e);
    }

    void AvisarDedo(TactilTipo tipo, int id, float nx, float ny, float ndx, float ndy) {
        TactilEvento e = Nuevo(tipo);
        e.dedo = id; e.nx = nx; e.ny = ny; e.ndx = ndx; e.ndy = ndy;
        Entregar(e);
    }

    void Arriba(int id) {
        DedoTactil& d = gDedos[id];
        if (!d.abajo) return;   // (SDL: "this finger is already up")
        if (gDedoMouse == id) { gDedoMouse = -1; BotonMouse(false); }
        d.abajo = false;
        AvisarDedo(TactilDedoSubio, id, d.x, d.y, 0.0f, 0.0f);   // (SDL manda las coordenadas que ya tenia)
    }
}

void TactilSetReceptor(TactilReceptor r) { gReceptor = r; }

void TactilDedo(int id, int fase, int x, int y, int ancho, int alto) {
    if (id < 0 || id >= kMaxDedos) return;
    if (ancho > 0) gAncho = ancho;
    if (alto > 0)  gAlto = alto;
    const float nx = (float)x / (float)gAncho, ny = (float)y / (float)gAlto;
    DedoTactil& d = gDedos[id];
    if (fase == DedoArriba) { Arriba(id); return; }
    if (fase == DedoMovio && !d.abajo) fase = DedoAbajo;   // (SDL: un movimiento de un dedo que no bajo es un down)
    if (fase == DedoAbajo) {
        if (d.abajo) Arriba(id);   // se perdio el up anterior: SDL lo manda antes del nuevo down
        if (gDedoMouse < 0) {
            gDedoMouse = id;
            MouseA(x, y, false);
            BotonMouse(true);
        }
        d.abajo = true; d.x = nx; d.y = ny;
        AvisarDedo(TactilDedoBajo, id, nx, ny, 0.0f, 0.0f);
        return;
    }
    // se movio
    if (gDedoMouse == id) MouseA(x, y, true);
    const float ndx = nx - d.x, ndy = ny - d.y;
    if (ndx == 0.0f && ndy == 0.0f) return;   // SDL descarta el que no cambia nada
    d.x = nx; d.y = ny;
    AvisarDedo(TactilDedoSeMovio, id, nx, ny, ndx, ndy);
}

void TactilSoltarTodo() {
    // primero los que no tienen el mouse: el ultimo en subir lo suelta, como al levantar la mano
    for (int i = 0; i < kMaxDedos; i++) if (i != gDedoMouse) Arriba(i);
    if (gDedoMouse >= 0) Arriba(gDedoMouse);
}

int TactilDedos() {
    int n = 0;
    for (int i = 0; i < kMaxDedos; i++) if (gDedos[i].abajo) n++;
    return n;
}

bool TactilEnEvento() { return gEnEvento; }

} // namespace w3dEngine
