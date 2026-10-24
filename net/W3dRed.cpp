// ============================================================================
//  W3dRed.cpp — ver W3dRed.h.
//
//  EL PROTOCOLO (UDP, todo en un socket por programa; cada paquete empieza con "W3R1" y su tipo):
//    BUSCAR      el que busca -> a todos (broadcast al 47023, a cada red local y a 127.0.0.1): "hay partidas
//                de este juego?"
//    PARTIDA     el host -> al que pregunto: su sesion, su nombre, el de su dispositivo, cuantos hay / caben
//    UNIRSE      el que entra -> al host (con su ficha: un numero al azar que identifica el pedido)
//    BIENVENIDO  el host -> al que entra: su id; RECHAZO si esta llena o cerrada
//    JUGADORES   el host -> a todos: la lista (cada vez que cambia y cada segundo)
//    DATOS       un mensaje (de, para, confiable, numero de orden): el host reparte los que no son para el
//    ACK         "llegaron todos los confiables hasta el n": el que los mando deja de reenviarlos
//    PING / PONG cada segundo sin nada que mandar; a los 8 s sin noticias el otro se fue
//    ADIOS       me voy
//  Los confiables se reenvian cada 0,25 s hasta su ACK y se entregan en orden (los que llegan adelantados
//  esperan a los que faltan).
//
//  DREAMCAST: los sockets de KallistiOS (el adaptador de banda ancha; el juego arranca la red: eth_init + net_init).
//  Sin Lua (-DW3D_RED_SIN_LUA): el juego arma y lee los valores con W3dRedValor.h, el mismo formato.
// ============================================================================
#include "W3dRed.h"
#include "W3dRedValor.h"
#include "base/w3dlog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <deque>

#if defined(_WIN32)
    #define W3D_RED_WINSOCK 1
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    typedef SOCKET SocketRed;
    #define SOCKET_NULO INVALID_SOCKET
#elif defined(__EMSCRIPTEN__) || defined(W3D_3DS)
    // (el navegador no tiene UDP; la 3DS, todavia no)
    #define W3D_RED_NINGUNA 1
    typedef int SocketRed;
    #define SOCKET_NULO (-1)
#else
    #define W3D_RED_POSIX 1
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <time.h>
    #include <sys/time.h>
    #if defined(W3D_DREAMCAST)
        #include <kos/net.h>      // net_default_dev: la IP del adaptador
        #include <arch/timer.h>   // timer_us_gettime64
    #endif
    #ifndef INADDR_LOOPBACK
        #define INADDR_LOOPBACK 0x7F000001u   // (KallistiOS no lo trae)
    #endif
    // (Symbian: los sockets de Open C, sin la lista de interfaces; Android, desde la API 24; la Dreamcast tampoco)
    #if !defined(W3D_SYMBIAN) && !defined(W3D_DREAMCAST) && \
        (!defined(__ANDROID__) || (defined(__ANDROID_API__) && __ANDROID_API__ >= 24))
        #include <ifaddrs.h>
        #include <net/if.h>
        #define W3D_RED_IFADDRS 1
    #endif
    typedef int SocketRed;
    #define SOCKET_NULO (-1)
#endif

#if !defined(W3D_RED_SIN_LUA)
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#endif

namespace w3dRed {

namespace {

// ----------------------------------------------------------------------------------------------- constantes
enum Tipo { T_BUSCAR = 1, T_PARTIDA, T_UNIRSE, T_BIENVENIDO, T_RECHAZO, T_JUGADORES, T_DATOS, T_ACK, T_PING, T_PONG,
            T_ADIOS };
const double REENVIO = 0.25;        // un confiable sin ACK se vuelve a mandar
const double PING_CADA = 1.0;       // sin nada que mandar, un ping
const double SE_FUE = 8.0;          // sin noticias: se fue
const double BUSCAR_CADA = 1.0;     // mientras busca, vuelve a preguntar
const double PARTIDA_VIEJA = 4.0;   // una partida que no responde sale de la lista
const double UNIRSE_CADA = 0.5;     // el pedido de unirse se repite hasta la respuesta
const double UNIRSE_MAX = 6.0;      // y se rinde
const int PAQUETE_MAX = 60000;
const unsigned char PARA_TODOS = 255;

// --------------------------------------------------------------------------------------------- el reloj
double Ahora() {
#if defined(W3D_RED_WINSOCK)
    static LARGE_INTEGER f = { 0 };
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#elif defined(W3D_DREAMCAST)
    return (double)timer_us_gettime64() * 1e-6;
#elif defined(W3D_RED_POSIX) && !defined(W3D_SYMBIAN)
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#elif defined(W3D_RED_POSIX)
    struct timeval t;
    gettimeofday(&t, 0);
    return (double)t.tv_sec + (double)t.tv_usec * 1e-6;
#else
    return 0.0;
#endif
}

// ------------------------------------------------------------------------------------- escribir y leer bytes
struct Escritor {
    std::string b;
    void u8(unsigned v) { b += (char)(v & 0xFF); }
    void u32(unsigned v) { u8(v); u8(v >> 8); u8(v >> 16); u8(v >> 24); }
    void str(const std::string& s) {
        const size_t n = s.size() < 255 ? s.size() : 255;
        u8((unsigned)n);
        b.append(s, 0, n);
    }
    void bytes(const void* d, int n) { b.append((const char*)d, (size_t)n); }
};
struct Lector {
    const unsigned char* p;
    const unsigned char* fin;
    bool error;
    Lector(const void* d, int n) : p((const unsigned char*)d), fin((const unsigned char*)d + n), error(false) {}
    unsigned u8() { if (p >= fin) { error = true; return 0; } return *p++; }
    unsigned u32() { unsigned a = u8(), b = u8(), c = u8(), d = u8(); return a | (b << 8) | (c << 16) | (d << 24); }
    std::string str() {
        const unsigned n = u8();
        if (p + n > fin) { error = true; return std::string(); }
        std::string s((const char*)p, n);
        p += n;
        return s;
    }
    int quedan() const { return (int)(fin - p); }
};

// ------------------------------------------------------------------------------------------- direcciones
struct Direccion {
    unsigned ip;      // en el orden de la red
    unsigned short puerto;   // en el orden de la red
    Direccion() : ip(0), puerto(0) {}
    bool operator==(const Direccion& o) const { return ip == o.ip && puerto == o.puerto; }
    bool operator<(const Direccion& o) const { return ip < o.ip || (ip == o.ip && puerto < o.puerto); }
};

std::string Texto(const Direccion& d) {
    const unsigned char* b = (const unsigned char*)&d.ip;
    char s[32];
    sprintf(s, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (unsigned)ntohs(d.puerto));
    return s;
}

// "a.b.c.d", "a.b.c.d:puerto" o un nombre de equipo ("casa.local:47023")
bool Interpretar(const std::string& texto, Direccion& d) {
#if defined(W3D_RED_NINGUNA)
    (void)texto; (void)d;
    return false;
#else
    std::string host = texto;
    unsigned puerto = PUERTO;
    const size_t dos = texto.rfind(':');
    if (dos != std::string::npos) {
        host = texto.substr(0, dos);
        puerto = (unsigned)atoi(texto.c_str() + dos + 1);
        if (!puerto || puerto > 65535) return false;
    }
    if (host.empty()) return false;
    d.puerto = htons((unsigned short)puerto);
    unsigned long ip = inet_addr(host.c_str());
    if (ip != INADDR_NONE || host == "255.255.255.255") {
        d.ip = (unsigned)ip;
        return true;
    }
    struct addrinfo pista, *r = 0;
    memset(&pista, 0, sizeof(pista));
    pista.ai_family = AF_INET;
    pista.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), 0, &pista, &r) != 0 || !r) return false;
    d.ip = (unsigned)((struct sockaddr_in*)r->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(r);
    return true;
#endif
}

// ---------------------------------------------------------------------------------------------- el socket
#if !defined(W3D_RED_NINGUNA)
bool gWinsock = false;

SocketRed AbrirSocket(unsigned short puerto, std::string& error) {
#if defined(W3D_RED_WINSOCK)
    if (!gWinsock) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { error = "no arranca Winsock"; return SOCKET_NULO; }
        gWinsock = true;
    }
#endif
    SocketRed s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCKET_NULO) { error = "no se pudo abrir un socket"; return SOCKET_NULO; }
    int si = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&si, sizeof(si));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&si, sizeof(si));
#if defined(W3D_RED_WINSOCK)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(puerto);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        char m[96];
        sprintf(m, "el puerto %u esta ocupado (otra partida en este equipo?)", (unsigned)puerto);
        error = puerto ? m : "no se pudo abrir un puerto";
#if defined(W3D_RED_WINSOCK)
        closesocket(s);
#else
        close(s);
#endif
        return SOCKET_NULO;
    }
    return s;
}

void CerrarSocket(SocketRed& s) {
    if (s == SOCKET_NULO) return;
#if defined(W3D_RED_WINSOCK)
    closesocket(s);
#else
    close(s);
#endif
    s = SOCKET_NULO;
}
#endif

// las direcciones de broadcast: la de cada red local (si se pueden saber), la de "toda la red" y este mismo equipo
// (dos programas en la misma maquina, para probar)
std::vector<Direccion> Difusiones() {
    std::vector<Direccion> v;
#if !defined(W3D_RED_NINGUNA)
    Direccion d;
    d.puerto = htons((unsigned short)PUERTO);
#if defined(W3D_RED_IFADDRS)
    struct ifaddrs* lista = 0;
    if (getifaddrs(&lista) == 0) {
        for (struct ifaddrs* i = lista; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_BROADCAST) || !(i->ifa_flags & IFF_UP) || !i->ifa_broadaddr) continue;
            d.ip = (unsigned)((struct sockaddr_in*)i->ifa_broadaddr)->sin_addr.s_addr;
            v.push_back(d);
        }
        freeifaddrs(lista);
    }
#endif
    d.ip = htonl(INADDR_BROADCAST);
    v.push_back(d);
    d.ip = htonl(INADDR_LOOPBACK);
    v.push_back(d);
#endif
    return v;
}

// --------------------------------------------------------------------------------------- los otros (enlaces)
// un mensaje confiable que espera su ACK
struct Pendiente {
    std::string paquete;
    double enviado;
};

// el enlace con otro programa: el host tiene uno por jugador; el que se une, uno con el host
struct Enlace {
    int id;
    Direccion dir;
    std::string nombre;
    unsigned seqEnviar;                       // el numero del proximo confiable que mando
    std::map<unsigned, Pendiente> pendientes; // los mandados sin ACK
    unsigned seqRecibir;                      // el proximo que espero
    std::map<unsigned, std::string> adelantados;   // los que llegaron antes de tiempo (con su "de")
    double oido, enviado;                     // la ultima noticia de el y lo ultimo que le mande
    Enlace() : id(-1), seqEnviar(1), seqRecibir(1), oido(0), enviado(0) {}
};

// un mensaje para Recibir
struct Mensaje {
    int de;
    std::string datos;
};

// ------------------------------------------------------------------------------------------------ el estado
struct Sesion {
    bool permitida;
    std::string nombreDispositivo;
    Estado estado;
    std::string error;
    SocketRed s;
    std::string juego, nombre;
    unsigned sesion;            // el numero de la partida (al azar: dos partidas no se confunden)
    int maximo;
    bool abierta;
    int miId;
    // buscando
    std::vector<Partida> partidas;
    double ultimaBusqueda;
    // uniendose
    Direccion host;
    unsigned ficha;
    double pedidoDesde, ultimoPedido;
    // los enlaces (por id) y lo recibido
    std::map<int, Enlace> enlaces;
    std::vector<Jugador> jugadores;
    double ultimaLista;
    std::deque<Mensaje> recibidos;
    Sesion() : permitida(true), estado(NADA), s(SOCKET_NULO), sesion(0), maximo(2), abierta(true), miId(-1),
               ultimaBusqueda(0), ficha(0), pedidoDesde(0), ultimoPedido(0), ultimaLista(0) {}
};
Sesion G;

unsigned Azar32() {
    static bool sembrado = false;
    if (!sembrado) {
        srand((unsigned)(Ahora() * 1000.0) ^ (unsigned)(size_t)&G);
        sembrado = true;
    }
    return ((unsigned)rand() << 16) ^ (unsigned)rand() ^ ((unsigned)rand() << 8);
}

void Cabecera(Escritor& e, Tipo t) {
    e.b.reserve(64);
    e.bytes("W3R1", 4);
    e.u8((unsigned)t);
}

void Mandar(const Direccion& a, const std::string& paquete) {
#if !defined(W3D_RED_NINGUNA)
    if (G.s == SOCKET_NULO) return;
    struct sockaddr_in d;
    memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
    d.sin_addr.s_addr = a.ip;
    d.sin_port = a.puerto;
    sendto(G.s, paquete.data(), (int)paquete.size(), 0, (struct sockaddr*)&d, sizeof(d));
#else
    (void)a; (void)paquete;
#endif
}

void MandarA(Enlace& en, const std::string& paquete) {
    Mandar(en.dir, paquete);
    en.enviado = Ahora();
}

void Fallar(const std::string& motivo) {
    G.estado = FALLO;
    G.error = motivo;
    w3dLogfW("red: %s", motivo.c_str());
}

// la lista de jugadores (del host: el y sus enlaces; del que se une: la que manda el host)
void ArmarListaHost() {
    G.jugadores.clear();
    Jugador yo;
    yo.id = 0; yo.nombre = NombreDispositivo(); yo.yo = true;
    G.jugadores.push_back(yo);
    for (std::map<int, Enlace>::iterator it = G.enlaces.begin(); it != G.enlaces.end(); ++it) {
        Jugador j;
        j.id = it->first; j.nombre = it->second.nombre; j.yo = false;
        G.jugadores.push_back(j);
    }
}

void MandarLista() {
    Escritor e;
    Cabecera(e, T_JUGADORES);
    e.u32(G.sesion);
    e.u8((unsigned)G.jugadores.size());
    for (size_t i = 0; i < G.jugadores.size(); i++) {
        e.u8((unsigned)G.jugadores[i].id);
        e.str(G.jugadores[i].nombre);
    }
    for (std::map<int, Enlace>::iterator it = G.enlaces.begin(); it != G.enlaces.end(); ++it) MandarA(it->second, e.b);
    G.ultimaLista = Ahora();
}

// un DATOS a un enlace (confiable: con su numero, queda pendiente hasta el ACK)
void MandarDatos(Enlace& en, int de, int para, const std::string& datos, bool confiable) {
    Escritor e;
    Cabecera(e, T_DATOS);
    e.u32(G.sesion);
    e.u8((unsigned)de);
    e.u8(para < 0 ? PARA_TODOS : (unsigned)para);
    e.u8(confiable ? 1 : 0);
    const unsigned seq = confiable ? en.seqEnviar++ : 0;
    e.u32(seq);
    e.b += datos;
    MandarA(en, e.b);
    if (confiable) {
        Pendiente p;
        p.paquete = e.b;
        p.enviado = Ahora();
        en.pendientes[seq] = p;
    }
}

void Entregar(int de, const std::string& datos) {
    Mensaje m;
    m.de = de;
    m.datos = datos;
    G.recibidos.push_back(m);
}

// un DATOS que llego por un enlace: si es para mi, a la cola; el host reparte lo que es para otros
void LlegoDatos(int de, int para, const std::string& datos, bool confiable) {
    if (G.miId == 0) {
        for (std::map<int, Enlace>::iterator it = G.enlaces.begin(); it != G.enlaces.end(); ++it) {
            if (it->first == de) continue;
            if (para == PARA_TODOS || para == it->first) MandarDatos(it->second, de, para == PARA_TODOS ? -1 : para,
                                                                   datos, confiable);
        }
        if (para == PARA_TODOS || para == 0) Entregar(de, datos);
    } else {
        Entregar(de, datos);
    }
}

void MandarAck(Enlace& en) {
    Escritor e;
    Cabecera(e, T_ACK);
    e.u32(G.sesion);
    e.u8((unsigned)G.miId);
    e.u32(en.seqRecibir - 1);
    MandarA(en, e.b);
}

Enlace* EnlaceDe(const Direccion& d) {
    for (std::map<int, Enlace>::iterator it = G.enlaces.begin(); it != G.enlaces.end(); ++it)
        if (it->second.dir == d) return &it->second;
    return 0;
}

void Quitar(int id, const char* motivo) {
    std::map<int, Enlace>::iterator it = G.enlaces.find(id);
    if (it == G.enlaces.end()) return;
    w3dLogf("red: se fue %s (%s)", it->second.nombre.c_str(), motivo);
    G.enlaces.erase(it);
    if (G.miId == 0) {
        ArmarListaHost();
        MandarLista();
    }
}

// ---------------------------------------------------------------------------------------------- lo que llega
void Procesar(const Direccion& de, const unsigned char* p, int n) {
    if (n < 5 || memcmp(p, "W3R1", 4) != 0) return;
    Lector l(p + 5, n - 5);
    const Tipo t = (Tipo)p[4];
    const double ahora = Ahora();
    switch (t) {
    case T_BUSCAR: {
        const std::string juego = l.str();
        if (l.error || G.estado != ESPERANDO || !G.abierta || juego != G.juego) return;
        if ((int)G.enlaces.size() + 1 >= G.maximo) return;
        Escritor e;
        Cabecera(e, T_PARTIDA);
        e.str(G.juego);
        e.u32(G.sesion);
        e.str(G.nombre);
        e.str(NombreDispositivo());
        e.u8((unsigned)G.enlaces.size() + 1);
        e.u8((unsigned)G.maximo);
        Mandar(de, e.b);
        return;
    }
    case T_PARTIDA: {
        if (G.estado != BUSCANDO) return;
        Partida q;
        q.juego = l.str();
        q.sesion = l.u32();
        q.nombre = l.str();
        q.dispositivo = l.str();
        q.jugadores = (int)l.u8();
        q.maximo = (int)l.u8();
        if (l.error || q.juego != G.juego) return;
        q.direccion = Texto(de);
        q.visto = ahora;
        // (la misma partida responde por varios caminos: este equipo, la red local; se queda con una, la de la
        // red si la hay)
        for (size_t i = 0; i < G.partidas.size(); i++) {
            if (G.partidas[i].sesion != q.sesion) continue;
            const bool local = de.ip == htonl(INADDR_LOOPBACK);
            if (local) q.direccion = G.partidas[i].direccion;
            G.partidas[i] = q;
            return;
        }
        G.partidas.push_back(q);
        return;
    }
    case T_UNIRSE: {
        const std::string juego = l.str();
        const unsigned sesion = l.u32(), ficha = l.u32();
        const std::string nombre = l.str();
        if (l.error || G.estado != ESPERANDO || juego != G.juego || sesion != G.sesion) return;
        Enlace* ya = EnlaceDe(de);
        int id = ya ? ya->id : -1;
        if (!ya) {
            if (!G.abierta || (int)G.enlaces.size() + 1 >= G.maximo) {
                Escritor e;
                Cabecera(e, T_RECHAZO);
                e.u32(ficha);
                e.str(G.abierta ? "la partida esta llena" : "la partida ya empezo");
                Mandar(de, e.b);
                return;
            }
            for (id = 1; G.enlaces.count(id); id++) {}
            Enlace en;
            en.id = id;
            en.dir = de;
            en.nombre = nombre.empty() ? "?" : nombre;
            en.oido = ahora;
            G.enlaces[id] = en;
            w3dLogf("red: entro %s (%s) como jugador %d", en.nombre.c_str(), Texto(de).c_str(), id);
        }
        Escritor e;
        Cabecera(e, T_BIENVENIDO);
        e.u32(G.sesion);
        e.u32(ficha);
        e.u8((unsigned)id);
        Mandar(de, e.b);
        ArmarListaHost();
        MandarLista();
        return;
    }
    case T_BIENVENIDO: {
        const unsigned sesion = l.u32(), ficha = l.u32();
        const int id = (int)l.u8();
        if (l.error || G.estado != CONECTANDO || ficha != G.ficha || !(de == G.host)) return;
        G.sesion = sesion;
        G.miId = id;
        G.estado = CONECTADO;
        Enlace en;
        en.id = 0;
        en.dir = de;
        en.oido = ahora;
        G.enlaces.clear();
        G.enlaces[0] = en;
        w3dLogf("red: adentro de la partida de %s como jugador %d", Texto(de).c_str(), id);
        return;
    }
    case T_RECHAZO: {
        const unsigned ficha = l.u32();
        const std::string motivo = l.str();
        if (l.error || G.estado != CONECTANDO || ficha != G.ficha) return;
        Fallar(motivo);
        return;
    }
    default: break;
    }
    // lo demas es de una sesion: de un enlace conocido
    const unsigned sesion = l.u32();
    if (l.error || sesion != G.sesion || (G.estado != ESPERANDO && G.estado != CONECTADO)) return;
    Enlace* en = EnlaceDe(de);
    if (!en) return;
    en->oido = ahora;
    switch (t) {
    case T_JUGADORES: {
        if (G.miId == 0) return;
        const int cuantos = (int)l.u8();
        std::vector<Jugador> v;
        for (int i = 0; i < cuantos && !l.error; i++) {
            Jugador j;
            j.id = (int)l.u8();
            j.nombre = l.str();
            j.yo = j.id == G.miId;
            v.push_back(j);
        }
        if (!l.error) G.jugadores = v;
        if (!G.jugadores.empty()) en->nombre = G.jugadores[0].nombre;
        return;
    }
    case T_DATOS: {
        const int origen = (int)l.u8();
        const int para = (int)l.u8();
        const bool confiable = l.u8() != 0;
        const unsigned seq = l.u32();
        if (l.error) return;
        // (el host sabe quien manda: el que se une no puede hacerse pasar por otro)
        const int deQuien = G.miId == 0 ? en->id : origen;
        std::string datos((const char*)l.p, (size_t)l.quedan());
        if (!confiable) { LlegoDatos(deQuien, para, datos, false); return; }
        if (seq >= en->seqRecibir && !en->adelantados.count(seq)) {
            Escritor guardado;
            guardado.u8((unsigned)deQuien);
            guardado.u8((unsigned)para);
            guardado.b += datos;
            en->adelantados[seq] = guardado.b;
        }
        // los que ya estan en orden
        std::map<unsigned, std::string>::iterator it;
        while ((it = en->adelantados.find(en->seqRecibir)) != en->adelantados.end()) {
            const std::string& g = it->second;
            const int d = (unsigned char)g[0], pa = (unsigned char)g[1];
            const std::string cuerpo = g.substr(2);
            en->adelantados.erase(it);
            en->seqRecibir++;
            LlegoDatos(d, pa, cuerpo, true);
            en = EnlaceDe(de);
            if (!en) return;
        }
        MandarAck(*en);
        return;
    }
    case T_ACK: {
        l.u8();
        const unsigned hasta = l.u32();
        if (l.error) return;
        while (!en->pendientes.empty() && en->pendientes.begin()->first <= hasta)
            en->pendientes.erase(en->pendientes.begin());
        return;
    }
    case T_PING: {
        l.u8();
        const unsigned marca = l.u32();
        Escritor e;
        Cabecera(e, T_PONG);
        e.u32(G.sesion);
        e.u8((unsigned)G.miId);
        e.u32(marca);
        MandarA(*en, e.b);
        return;
    }
    case T_PONG:
        return;
    case T_ADIOS:
        if (G.miId == 0) Quitar(en->id, "se despidio");
        else Fallar("el host cerro la partida");
        return;
    default:
        return;
    }
}

void Leer() {
#if !defined(W3D_RED_NINGUNA)
    if (G.s == SOCKET_NULO) return;
    static char buf[65536];
    for (int vueltas = 0; vueltas < 512; vueltas++) {
        struct sockaddr_in a;
#if defined(W3D_RED_WINSOCK)
        int la = sizeof(a);
#else
        socklen_t la = sizeof(a);
#endif
        const int n = (int)recvfrom(G.s, buf, sizeof(buf), 0, (struct sockaddr*)&a, &la);
        if (n <= 0) break;
        Direccion d;
        d.ip = (unsigned)a.sin_addr.s_addr;
        d.puerto = a.sin_port;
        Procesar(d, (const unsigned char*)buf, n);
        if (G.s == SOCKET_NULO) break;
    }
#endif
}

void MandarBuscar() {
    Escritor e;
    Cabecera(e, T_BUSCAR);
    e.str(G.juego);
    const std::vector<Direccion> v = Difusiones();
    for (size_t i = 0; i < v.size(); i++) Mandar(v[i], e.b);
    G.ultimaBusqueda = Ahora();
}

void MandarUnirse() {
    Escritor e;
    Cabecera(e, T_UNIRSE);
    e.str(G.juego);
    e.u32(G.sesion);
    e.u32(G.ficha);
    e.str(NombreDispositivo());
    Mandar(G.host, e.b);
    G.ultimoPedido = Ahora();
}

bool Abrir(unsigned short puerto) {
#if defined(W3D_RED_NINGUNA)
    (void)puerto;
    Fallar("esta plataforma todavia no tiene red");
    return false;
#else
    if (!G.permitida) { Fallar("este proyecto no tiene permiso para usar la red"); return false; }
    std::string error;
    G.s = AbrirSocket(puerto, error);
    if (G.s == SOCKET_NULO) { Fallar(error); return false; }
    return true;
#endif
}

} // namespace

// =========================================================================================== la interfaz
void SetPermitida(bool si) {
    G.permitida = si;
    if (!si && G.estado != NADA) Cerrar();
}
bool Permitida() { return G.permitida; }

bool Soportada() {
#if defined(W3D_RED_NINGUNA)
    return false;
#else
    return true;
#endif
}

bool Disponible() { return Soportada() && G.permitida; }

void SetNombreDispositivo(const std::string& nombre) { G.nombreDispositivo = nombre; }

std::string NombreDispositivo() {
    return G.nombreDispositivo.empty() ? NombrePorDefecto() : G.nombreDispositivo;
}

std::string NombrePorDefecto() {
    static std::string defecto;
    if (!defecto.empty()) return defecto;
#if !defined(W3D_RED_NINGUNA)
#if defined(W3D_RED_WINSOCK)
    if (!gWinsock) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) == 0) gWinsock = true;
    }
#endif
#if defined(W3D_DREAMCAST)
    defecto = "Dreamcast";   // (la consola no tiene nombre)
#else
    char h[128];
    if (gethostname(h, sizeof(h)) == 0) {
        h[sizeof(h) - 1] = 0;
        // (sin el dominio: "casa.local" -> "casa")
        char* punto = strchr(h, '.');
        if (punto && punto != h) *punto = 0;
        defecto = h;
    }
#endif
#endif
    if (defecto.empty() || defecto == "localhost") {
        char s[32];
        sprintf(s, "Whisk3D-%04u", Azar32() % 10000);
        defecto = s;
    }
    return defecto;
}

std::string DireccionLocal() {
#if defined(W3D_RED_IFADDRS)
    std::string r;
    struct ifaddrs* lista = 0;
    if (getifaddrs(&lista) != 0) return r;
    for (struct ifaddrs* i = lista; i && r.empty(); i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (!(i->ifa_flags & IFF_UP)) continue;
        Direccion d;
        d.ip = (unsigned)((struct sockaddr_in*)i->ifa_addr)->sin_addr.s_addr;
        r = Texto(d);
        r = r.substr(0, r.find(':'));
    }
    freeifaddrs(lista);
    return r;
#elif defined(W3D_DREAMCAST)
    // (la que le dio el DHCP al adaptador)
    const netif_t* n = net_default_dev;
    if (!n || !n->ip_addr[0]) return std::string();
    char s[20];
    sprintf(s, "%d.%d.%d.%d", n->ip_addr[0], n->ip_addr[1], n->ip_addr[2], n->ip_addr[3]);
    return s;
#elif !defined(W3D_RED_NINGUNA)
    // (sin la lista de interfaces: la que usaria para salir; conectar un socket UDP no manda nada)
    std::string error;
    SocketRed s = AbrirSocket(0, error);
    if (s == SOCKET_NULO) return std::string();
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    a.sin_port = htons(53);
    std::string r;
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) == 0) {
#if defined(W3D_RED_WINSOCK)
        int la = sizeof(a);
#else
        socklen_t la = sizeof(a);
#endif
        if (getsockname(s, (struct sockaddr*)&a, &la) == 0) {
            Direccion d;
            d.ip = (unsigned)a.sin_addr.s_addr;
            r = Texto(d);
            r = r.substr(0, r.find(':'));
        }
    }
    CerrarSocket(s);
    return r;
#else
    return std::string();
#endif
}

bool Hostear(const std::string& juego, const std::string& nombre, int maximo) {
    Cerrar();
    if (!Abrir((unsigned short)PUERTO)) return false;
    G.juego = juego;
    G.nombre = nombre.empty() ? NombreDispositivo() : nombre;
    G.maximo = maximo < 2 ? 2 : (maximo > MAX_JUGADORES ? MAX_JUGADORES : maximo);
    G.sesion = Azar32() | 1;
    G.abierta = true;
    G.miId = 0;
    G.estado = ESPERANDO;
    ArmarListaHost();
    w3dLogf("red: hosteando \"%s\" (%s, hasta %d jugadores) en el puerto %d", G.nombre.c_str(), juego.c_str(),
            G.maximo, (int)PUERTO);
    return true;
}

void SetAbierta(bool si) { G.abierta = si; }

bool Buscar(const std::string& juego) {
    if (G.estado != BUSCANDO) {
        Cerrar();
        if (!Abrir(0)) return false;
        G.estado = BUSCANDO;
    }
    G.juego = juego;
    G.partidas.clear();
    MandarBuscar();
    return true;
}

const std::vector<Partida>& Partidas() { return G.partidas; }

bool Unirse(const std::string& direccion) {
    Direccion d;
    if (!Interpretar(direccion, d)) {
        Cerrar();
        Fallar("no entiendo la direccion \"" + direccion + "\"");
        return false;
    }
    // (si venia de la lista, la sesion es la de esa partida; si es una direccion escrita, la que conteste)
    unsigned sesion = 0;
    for (size_t i = 0; i < G.partidas.size(); i++)
        if (G.partidas[i].direccion == Texto(d)) sesion = G.partidas[i].sesion;
    const std::string juego = G.juego;
    if (G.estado != BUSCANDO) {
        Cerrar();
        if (!Abrir(0)) return false;
    }
    G.juego = juego;
    G.host = d;
    G.sesion = sesion;
    G.ficha = Azar32();
    G.estado = CONECTANDO;
    G.pedidoDesde = Ahora();
    MandarUnirse();
    return true;
}

void Cerrar() {
#if !defined(W3D_RED_NINGUNA)
    if (G.s != SOCKET_NULO && (G.estado == ESPERANDO || G.estado == CONECTADO)) {
        Escritor e;
        Cabecera(e, T_ADIOS);
        e.u32(G.sesion);
        e.u8((unsigned)G.miId);
        for (std::map<int, Enlace>::iterator it = G.enlaces.begin(); it != G.enlaces.end(); ++it) MandarA(it->second, e.b);
    }
    CerrarSocket(G.s);
#endif
    G.estado = NADA;
    G.error.clear();
    G.enlaces.clear();
    G.jugadores.clear();
    G.partidas.clear();
    G.recibidos.clear();
    G.miId = -1;
    G.sesion = 0;
}

Estado EstadoActual() { return G.estado; }
const std::string& Error() { return G.error; }
int MiId() { return (G.estado == ESPERANDO || G.estado == CONECTADO) ? G.miId : -1; }
const std::vector<Jugador>& Jugadores() { return G.jugadores; }

bool Enviar(int para, const void* datos, int bytes, bool confiable) {
    if ((G.estado != ESPERANDO && G.estado != CONECTADO) || bytes < 0 || bytes > PAQUETE_MAX) return false;
    const std::string d((const char*)datos, (size_t)bytes);
    if (G.miId == 0) {
        for (std::map<int, Enlace>::iterator it = G.enlaces.begin(); it != G.enlaces.end(); ++it)
            if (para < 0 || para == it->first) MandarDatos(it->second, 0, para, d, confiable);
    } else {
        std::map<int, Enlace>::iterator it = G.enlaces.find(0);
        if (it == G.enlaces.end()) return false;
        MandarDatos(it->second, G.miId, para, d, confiable);
    }
    return true;
}

bool Recibir(int& de, std::string& datos) {
    if (G.recibidos.empty()) Leer();
    if (G.recibidos.empty()) return false;
    de = G.recibidos.front().de;
    datos.swap(G.recibidos.front().datos);
    G.recibidos.pop_front();
    return true;
}

double Silencio(int id) {
    if (G.estado != ESPERANDO && G.estado != CONECTADO) return 0;
    if (id >= 0 && id == G.miId) return 0;
    const double ahora = Ahora();
    double peor = 0;
    for (std::map<int, Enlace>::iterator it = G.enlaces.begin(); it != G.enlaces.end(); ++it) {
        // (el host mira el enlace de ese jugador; el que se une tiene uno solo, el del host, y todo pasa por ahi)
        if (G.miId == 0 && id >= 0 && it->first != id) continue;
        if (ahora - it->second.oido > peor) peor = ahora - it->second.oido;
    }
    return peor;
}

double LimiteSilencio() { return SE_FUE; }

void Actualizar() {
    if (G.estado == NADA || G.estado == FALLO) return;
    Leer();
    const double ahora = Ahora();
    if (G.estado == BUSCANDO) {
        if (ahora - G.ultimaBusqueda > BUSCAR_CADA) MandarBuscar();
        for (size_t i = G.partidas.size(); i-- > 0;)
            if (ahora - G.partidas[i].visto > PARTIDA_VIEJA) G.partidas.erase(G.partidas.begin() + (long)i);
        return;
    }
    if (G.estado == CONECTANDO) {
        if (ahora - G.pedidoDesde > UNIRSE_MAX) Fallar("el host no responde");
        else if (ahora - G.ultimoPedido > UNIRSE_CADA) MandarUnirse();
        return;
    }
    // reenviar lo confiable sin ACK, pings y los que se fueron
    std::vector<int> idos;
    for (std::map<int, Enlace>::iterator it = G.enlaces.begin(); it != G.enlaces.end(); ++it) {
        Enlace& en = it->second;
        if (ahora - en.oido > SE_FUE) { idos.push_back(it->first); continue; }
        for (std::map<unsigned, Pendiente>::iterator p = en.pendientes.begin(); p != en.pendientes.end(); ++p) {
            if (ahora - p->second.enviado < REENVIO) continue;
            Mandar(en.dir, p->second.paquete);
            p->second.enviado = ahora;
            en.enviado = ahora;
        }
        if (ahora - en.enviado > PING_CADA) {
            Escritor e;
            Cabecera(e, T_PING);
            e.u32(G.sesion);
            e.u8((unsigned)G.miId);
            e.u32((unsigned)(ahora * 1000.0));
            MandarA(en, e.b);
        }
    }
    for (size_t i = 0; i < idos.size(); i++) {
        if (G.miId == 0) Quitar(idos[i], "no responde");
        else { Fallar("se perdio la conexion con el host"); return; }
    }
    if (G.miId == 0 && ahora - G.ultimaLista > 1.0) MandarLista();
}

// =================================================================================================== Lua
#if defined(W3D_RED_SIN_LUA)
void RegistrarBinds(void*) {}
#else
namespace {

// LOS VALORES DE LUA EN BYTES (redEnviar / redRecibir): nil, booleanos, numeros (enteros de 1, 2 o 4 bytes; los
// demas en float de 4: posiciones, tiempos), textos y tablas (cualquier clave). Una tabla que se repite o se
// contiene a si misma no viaja (va nil). Los objetos de la escena tampoco (son punteros de este programa). Los tipos
// son los de W3dRedValor.h (el mismo formato, para un juego en C++)

void EscribirValor(lua_State* L, int i, Escritor& e, int prof) {
    const int t = lua_type(L, i);
    if (t == LUA_TBOOLEAN) { e.u8(lua_toboolean(L, i) ? V_VERDADERO : V_FALSO); return; }
    if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, i)) {
            const lua_Integer v = lua_tointeger(L, i);
            if (v >= -128 && v <= 127) { e.u8(V_I8); e.u8((unsigned)(v & 0xFF)); return; }
            if (v >= -32768 && v <= 32767) { e.u8(V_I16); e.u8((unsigned)(v & 0xFF)); e.u8((unsigned)((v >> 8) & 0xFF)); return; }
            if (v >= -2147483647 - 1 && v <= 2147483647) { e.u8(V_I32); e.u32((unsigned)v); return; }
            e.u8(V_F64);
            const double d = (double)v;
            e.bytes(&d, 8);
            return;
        }
        const float f = (float)lua_tonumber(L, i);
        unsigned u;
        memcpy(&u, &f, 4);
        e.u8(V_F32);
        e.u32(u);
        return;
    }
    if (t == LUA_TSTRING) {
        size_t n = 0;
        const char* s = lua_tolstring(L, i, &n);
        e.u8(V_TEXTO);
        e.u32((unsigned)n);
        e.bytes(s, (int)n);
        return;
    }
    if (t == LUA_TTABLE && prof < 24) {
        if (i < 0) i = lua_gettop(L) + i + 1;
        e.u8(V_TABLA);
        const size_t lugar = e.b.size();
        e.u32(0);
        unsigned cuantos = 0;
        lua_pushnil(L);
        while (lua_next(L, i) != 0) {
            const int tk = lua_type(L, -2), tv = lua_type(L, -1);
            const bool viaja = (tk == LUA_TNUMBER || tk == LUA_TSTRING || tk == LUA_TBOOLEAN) &&
                               (tv == LUA_TNUMBER || tv == LUA_TSTRING || tv == LUA_TBOOLEAN || tv == LUA_TTABLE);
            if (viaja) {
                EscribirValor(L, lua_gettop(L) - 1, e, prof + 1);
                EscribirValor(L, lua_gettop(L), e, prof + 1);
                cuantos++;
            }
            lua_pop(L, 1);
        }
        e.b[lugar] = (char)(cuantos & 0xFF);
        e.b[lugar + 1] = (char)((cuantos >> 8) & 0xFF);
        e.b[lugar + 2] = (char)((cuantos >> 16) & 0xFF);
        e.b[lugar + 3] = (char)((cuantos >> 24) & 0xFF);
        return;
    }
    e.u8(V_NIL);
}

// deja el valor en la pila (false si los bytes estan mal)
bool LeerValor(lua_State* L, Lector& l, int prof) {
    if (prof > 24 || !lua_checkstack(L, 4)) return false;
    const unsigned t = l.u8();
    switch (t) {
    case V_NIL: lua_pushnil(L); break;
    case V_FALSO: lua_pushboolean(L, 0); break;
    case V_VERDADERO: lua_pushboolean(L, 1); break;
    case V_I8: lua_pushinteger(L, (lua_Integer)(signed char)l.u8()); break;
    case V_I16: { const unsigned a = l.u8(), b = l.u8(); lua_pushinteger(L, (lua_Integer)(short)(a | (b << 8))); break; }
    case V_I32: lua_pushinteger(L, (lua_Integer)(int)l.u32()); break;
    case V_F32: { const unsigned u = l.u32(); float f; memcpy(&f, &u, 4); lua_pushnumber(L, (lua_Number)f); break; }
    case V_F64: {
        if (l.quedan() < 8) return false;
        double d;
        memcpy(&d, l.p, 8);
        l.p += 8;
        lua_pushnumber(L, (lua_Number)d);
        break;
    }
    case V_TEXTO: {
        const unsigned n = l.u32();
        if (l.error || (int)n > l.quedan()) return false;
        lua_pushlstring(L, (const char*)l.p, n);
        l.p += n;
        break;
    }
    case V_TABLA: {
        const unsigned n = l.u32();
        if (l.error || (int)n > l.quedan()) return false;
        lua_createtable(L, 0, (int)n);
        for (unsigned k = 0; k < n; k++) {
            if (!LeerValor(L, l, prof + 1) || !LeerValor(L, l, prof + 1)) return false;
            if (lua_isnil(L, -2)) { lua_pop(L, 2); continue; }
            lua_rawset(L, -3);
        }
        break;
    }
    default: return false;
    }
    return !l.error;
}

const char* NombreEstado(Estado e) {
    switch (e) {
    case BUSCANDO: return "buscando";
    case ESPERANDO: return "esperando";
    case CONECTANDO: return "conectando";
    case CONECTADO: return "conectado";
    case FALLO: return "error";
    default: return "nada";
    }
}

// redDisponible() -> bool, motivo ("sin permiso" / "sin red")
int LDisponible(lua_State* L) {
    lua_pushboolean(L, Disponible() ? 1 : 0);
    if (Disponible()) return 1;
    lua_pushstring(L, !Soportada() ? "sin red" : "sin permiso");
    return 2;
}
// redDispositivo() -> el nombre de este dispositivo; redDireccion() -> su IP en la red local
int LDispositivo(lua_State* L) { lua_pushstring(L, NombreDispositivo().c_str()); return 1; }
int LDireccion(lua_State* L) { lua_pushstring(L, DireccionLocal().c_str()); return 1; }
// redHostear(nombre [, maximo [, juego]]) -> bool [, error]
int LHostear(lua_State* L) {
    const bool ok = Hostear(luaL_optstring(L, 3, "w3d"), luaL_optstring(L, 1, ""), (int)luaL_optinteger(L, 2, 2));
    lua_pushboolean(L, ok ? 1 : 0);
    if (ok) return 1;
    lua_pushstring(L, Error().c_str());
    return 2;
}
// redAbrir(bool): el host aparece y acepta jugadores (true) o no (la partida empezo)
int LAbrir(lua_State* L) { SetAbierta(lua_toboolean(L, 1) != 0); return 0; }
// redBuscar([juego]) -> bool [, error]: empieza a buscar o actualiza la lista
int LBuscar(lua_State* L) {
    const bool ok = Buscar(luaL_optstring(L, 1, "w3d"));
    lua_pushboolean(L, ok ? 1 : 0);
    if (ok) return 1;
    lua_pushstring(L, Error().c_str());
    return 2;
}
// redPartidas() -> { {nombre, dispositivo, jugadores, maximo, direccion}, ... }
int LPartidas(lua_State* L) {
    Actualizar();
    const std::vector<Partida>& v = Partidas();
    lua_createtable(L, (int)v.size(), 0);
    for (size_t i = 0; i < v.size(); i++) {
        lua_createtable(L, 0, 5);
        lua_pushstring(L, v[i].nombre.c_str()); lua_setfield(L, -2, "nombre");
        lua_pushstring(L, v[i].dispositivo.c_str()); lua_setfield(L, -2, "dispositivo");
        lua_pushinteger(L, v[i].jugadores); lua_setfield(L, -2, "jugadores");
        lua_pushinteger(L, v[i].maximo); lua_setfield(L, -2, "maximo");
        lua_pushstring(L, v[i].direccion.c_str()); lua_setfield(L, -2, "direccion");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
// redUnirse(direccion) -> bool [, error]
int LUnirse(lua_State* L) {
    const bool ok = Unirse(luaL_checkstring(L, 1));
    lua_pushboolean(L, ok ? 1 : 0);
    if (ok) return 1;
    lua_pushstring(L, Error().c_str());
    return 2;
}
// redEstado() -> "nada" | "buscando" | "esperando" | "conectando" | "conectado" | "error", el error
int LEstado(lua_State* L) {
    lua_pushstring(L, NombreEstado(EstadoActual()));
    lua_pushstring(L, Error().c_str());
    return 2;
}
// redYo() -> mi id (0 el host) o -1
int LYo(lua_State* L) { lua_pushinteger(L, MiId()); return 1; }
// redJugadores() -> { {id, nombre, yo}, ... }
int LJugadores(lua_State* L) {
    const std::vector<Jugador>& v = Jugadores();
    lua_createtable(L, (int)v.size(), 0);
    for (size_t i = 0; i < v.size(); i++) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, v[i].id); lua_setfield(L, -2, "id");
        lua_pushstring(L, v[i].nombre.c_str()); lua_setfield(L, -2, "nombre");
        lua_pushboolean(L, v[i].yo ? 1 : 0); lua_setfield(L, -2, "yo");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
// redSilencio([id]) -> segundos sin noticias de ese jugador (sin id: del que hace mas), el limite (despues, se fue)
int LSilencio(lua_State* L) {
    Actualizar();
    lua_pushnumber(L, Silencio(lua_isnoneornil(L, 1) ? -1 : (int)luaL_checkinteger(L, 1)));
    lua_pushnumber(L, LimiteSilencio());
    return 2;
}
// redEnviar(valor [, para [, confiable]]) -> bool. para: el id de un jugador (nil = todos); confiable por defecto
int LEnviar(lua_State* L) {
    Escritor e;
    EscribirValor(L, 1, e, 0);
    const int para = lua_isnoneornil(L, 2) ? -1 : (int)luaL_checkinteger(L, 2);
    const bool confiable = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    lua_pushboolean(L, Enviar(para, e.b.data(), (int)e.b.size(), confiable) ? 1 : 0);
    return 1;
}
// redRecibir() -> { {de = id, datos = valor}, ... } (todo lo que llego, en orden)
int LRecibir(lua_State* L) {
    lua_newtable(L);
    int de, n = 0;
    std::string d;
    while (Recibir(de, d)) {
        const int base = lua_gettop(L);
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, de);
        lua_setfield(L, -2, "de");
        Lector l(d.data(), (int)d.size());
        if (!LeerValor(L, l, 0)) { lua_settop(L, base); continue; }   // (bytes rotos: se descarta)
        lua_setfield(L, -2, "datos");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
// redCerrar()
int LCerrar(lua_State* L) { (void)L; Cerrar(); return 0; }

} // namespace

void RegistrarBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LDisponible);  lua_setglobal(L, "redDisponible");
    lua_pushcfunction(L, LDispositivo); lua_setglobal(L, "redDispositivo");
    lua_pushcfunction(L, LDireccion);   lua_setglobal(L, "redDireccion");
    lua_pushcfunction(L, LHostear);     lua_setglobal(L, "redHostear");
    lua_pushcfunction(L, LAbrir);       lua_setglobal(L, "redAbrir");
    lua_pushcfunction(L, LBuscar);      lua_setglobal(L, "redBuscar");
    lua_pushcfunction(L, LPartidas);    lua_setglobal(L, "redPartidas");
    lua_pushcfunction(L, LUnirse);      lua_setglobal(L, "redUnirse");
    lua_pushcfunction(L, LEstado);      lua_setglobal(L, "redEstado");
    lua_pushcfunction(L, LYo);          lua_setglobal(L, "redYo");
    lua_pushcfunction(L, LJugadores);   lua_setglobal(L, "redJugadores");
    lua_pushcfunction(L, LSilencio);    lua_setglobal(L, "redSilencio");
    lua_pushcfunction(L, LEnviar);      lua_setglobal(L, "redEnviar");
    lua_pushcfunction(L, LRecibir);     lua_setglobal(L, "redRecibir");
    lua_pushcfunction(L, LCerrar);      lua_setglobal(L, "redCerrar");
}
#endif // W3D_RED_SIN_LUA

} // namespace w3dRed
