// ============================================================================
//  W3dRed.h — jugar en red: LAN, wifi o internet (UDP).
//
//  UNA sesion por programa: el que HOSTEA abre la partida en el puerto 47023 y espera; los demas la BUSCAN en
//  la red local (preguntan a todos: broadcast) o se UNEN con una direccion ("192.168.0.10", "x.x.x.x:puerto",
//  para internet hace falta abrir ese puerto en el router del host). Todo pasa por el host (estrella): un
//  jugador le manda al host y el host lo reparte. El jugador 0 es el host; los que se unen, 1, 2...
//
//  Los mensajes son bytes (los binds de Lua mandan valores de Lua: numeros, textos, tablas). Dos clases:
//    · CONFIABLES: llegan todos y en orden (se reenvian hasta que el otro dice que llegaron). Para lo que no
//      se puede perder: "listo", "empezar", lo que pasa una sola vez.
//    · NO CONFIABLES: el que se pierde, se perdio. Para lo que se manda cada cuadro (el estado, los mandos):
//      el siguiente lo reemplaza.
//
//  EL PERMISO: un juego no usa la red si la aplicacion no lo deja (SetPermitida). El editor le pregunta al
//  usuario al abrir un proyecto que usa la red y recuerda la respuesta por archivo; un juego compilado e
//  instalado ya tiene el permiso (el de la plataforma).
//
//  Uso (cada cuadro Actualizar: lo llama W3dScriptFinFrame; Cerrar al terminar la partida):
//      w3dRed::Hostear("mi_juego", "La partida de Dante", 2);      // o
//      w3dRed::Buscar("mi_juego"); ... w3dRed::Partidas() ... w3dRed::Unirse(p.direccion);
//      w3dRed::Enviar(-1, datos, n, true); while (w3dRed::Recibir(de, datos)) ...
//
//  C++03 (Symbian). Plataformas: POSIX (Linux, mac, Android, Symbian por Open C) y Windows. El resto compila sin
//  red (Soportada() = false) hasta que tenga la suya (web: WebRTC; Dreamcast: el adaptador de banda ancha o el modem).
// ============================================================================
#ifndef W3D_RED_H
#define W3D_RED_H

#include <string>
#include <vector>

namespace w3dRed {

enum { PUERTO = 47023, MAX_JUGADORES = 8 };

enum Estado {
    NADA = 0,       // sin sesion
    BUSCANDO,       // preguntando por partidas en la red local
    ESPERANDO,      // hosteando (con o sin jugadores)
    CONECTANDO,     // pidio unirse y espera la respuesta del host
    CONECTADO,      // adentro de la partida de otro
    FALLO           // algo salio mal: Error() dice que (Cerrar vuelve a NADA)
};

// una partida que respondio a Buscar
struct Partida {
    std::string juego, nombre, dispositivo;
    std::string direccion;   // "ip:puerto" (lo que pide Unirse)
    int jugadores, maximo;
    unsigned int sesion;
    double visto;            // (cuando respondio por ultima vez)
};

struct Jugador {
    int id;                  // 0 = el host
    std::string nombre;      // el nombre de su dispositivo
    bool yo;
};

// --- el permiso, la plataforma y el nombre ---
void SetPermitida(bool si);
bool Permitida();
bool Soportada();                        // esta plataforma tiene red
bool Disponible();                       // soportada y permitida
void SetNombreDispositivo(const std::string& nombre);   // "" = el por defecto
std::string NombreDispositivo();
std::string NombrePorDefecto();          // el del equipo (hostname) o "Whisk3D-NNNN"
std::string DireccionLocal();            // la IP de este equipo en la red local ("" si no se sabe)

// --- la sesion ---
bool Hostear(const std::string& juego, const std::string& nombre, int maximo);
void SetAbierta(bool si);                // el host deja de aparecer y de aceptar jugadores (la partida empezo)
bool Buscar(const std::string& juego);   // (otra vez = actualizar: vacia la lista y vuelve a preguntar)
const std::vector<Partida>& Partidas();
bool Unirse(const std::string& direccion);
void Cerrar();
Estado EstadoActual();
const std::string& Error();
int MiId();                              // 0 el host, 1.. los demas; -1 sin sesion
const std::vector<Jugador>& Jugadores();

// --- los mensajes: para = el id de un jugador o -1 (todos los demas) ---
bool Enviar(int para, const void* datos, int bytes, bool confiable);
bool Recibir(int& de, std::string& datos);

// una vez por cuadro: lee lo que llego, reenvia lo confiable que falta, se fija quien se fue
void Actualizar();

// los binds de Lua (redHostear, redBuscar, redPartidas, redUnirse, redEnviar, redRecibir...; ver W3dRed.cpp)
void RegistrarBinds(void* L);

} // namespace w3dRed

#endif // W3D_RED_H
