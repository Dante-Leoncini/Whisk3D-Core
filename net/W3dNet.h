// ============================================================================
//  W3dNet.h — jugar en red: LAN, wifi o internet (UDP).
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
//  EL PERMISO: un juego no usa la red si la aplicacion no lo deja (SetAllowed). El editor le pregunta al
//  usuario al abrir un proyecto que usa la red y recuerda la respuesta por archivo; un juego compilado e
//  instalado ya tiene el permiso (el de la plataforma).
//
//  Uso (cada cuadro Actualizar: lo llama W3dScriptEndFrame; Cerrar al terminar la partida):
//      w3dNet::Host("mi_juego", "La partida de Dante", 2);      // o
//      w3dNet::Find("mi_juego"); ... w3dNet::Matches() ... w3dNet::Join(p.direction);
//      w3dNet::Send(-1, datos, n, true); while (w3dNet::Receive(sender, data)) ...
//
//  C++03 (Symbian). Plataformas: POSIX (Linux, mac, Android, Symbian por Open C) y Windows. El resto compila sin
//  red (Supported() = false) hasta que tenga la suya (web: WebRTC; Dreamcast: el adaptador de banda ancha o el modem).
// ============================================================================
#ifndef W3D_NET_H
#define W3D_NET_H

#include <string>
#include <vector>

namespace w3dNet {

enum { PORT = 47023, MAX_PLAYERS = 8 };

enum State {
    NOTHING = 0,     // sin sesion
    FINDING,         // preguntando por partidas en la red local
    WAITING,         // hosteando (con o sin jugadores)
    CONNECTING,      // pidio unirse y espera la respuesta del host
    CONNECTED,       // adentro de la partida de otro
    FAILED           // algo salio mal: Error() dice que (Cerrar vuelve a NADA)
};

// una partida que respondio a Buscar
struct Match {
    std::string game, name, device;
    std::string direction;   // "ip:puerto" (lo que pide Unirse)
    int players, max;
    unsigned int session;
    double seen;            // (cuando respondio por ultima vez)
};

struct Player {
    int id;                // 0 = el host
    std::string name;      // el nombre de su dispositivo
    bool yo;
};

// --- el permiso, la plataforma y el nombre ---
void SetAllowed(bool yes);
bool Allowed();
bool Supported();                       // esta plataforma tiene red
bool Available();                       // soportada y permitida
void SetDeviceName(const std::string& name);   // "" = el por defecto
std::string DeviceName();
std::string DefaultName();             // el del equipo (hostname) o "Whisk3D-NNNN"
std::string DirectionLocal();          // la IP de este equipo en la red local ("" si no se sabe)

// --- la sesion ---
bool Host(const std::string& game, const std::string& name, int max);
void SetOpen(bool yes);               // el host deja de aparecer y de aceptar jugadores (la partida empezo)
bool Find(const std::string& game);   // (otra vez = actualizar: vacia la lista y vuelve a preguntar)
const std::vector<Match>& Matches();
bool Join(const std::string& direction);
void Close();
State CurrentState();
const std::string& Error();
int MiId();                              // 0 el host, 1.. los demas; -1 sin sesion
const std::vector<Player>& Players();
// hace cuantos segundos no llega nada de ese jugador (-1: del que hace mas; el que se une solo oye al host, asi que
// para el es el silencio del host). Con mas de SilenceLimit() se lo da por ido. Un juego lo usa para esperar al otro
// (pausar y avisar) en vez de seguir como si nada mientras la red se traba
double Silence(int id);
double SilenceLimit();

// --- los mensajes: para = el id de un jugador o -1 (todos los demas) ---
bool Send(int recipient, const void* data, int bytes, bool reliable);
bool Receive(int& sender, std::string& data);

// una vez por cuadro: lee lo que llego, reenvia lo confiable que falta, se fija quien se fue
void Update();

// los binds de Lua (redHostear, redBuscar, redPartidas, redUnirse, redEnviar, redRecibir...; ver W3dNet.cpp)
void RegisterBinds(void* L);

} // namespace w3dNet

#endif // W3D_NET_H
