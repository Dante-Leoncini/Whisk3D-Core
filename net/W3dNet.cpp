// ============================================================================
//  W3dNet.cpp — ver W3dNet.h.
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
//  Sin Lua (-DW3D_RED_SIN_LUA): el juego arma y lee los valores con W3dNetValue.h, el mismo formato.
// ============================================================================
#include "W3dNet.h"
#include "W3dNetValue.h"
#include "base/w3dlog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <deque>

#if defined(_WIN32)
    #define W3D_NET_WINSOCK 1
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    typedef SOCKET SocketNet;
    #define SOCKET_NULL INVALID_SOCKET
#elif defined(__EMSCRIPTEN__) || defined(W3D_3DS) || defined(W3D_VITA) || defined(W3D_PSP)
    // (el navegador no tiene UDP; la 3DS, la Vita y la PSP, todavia no)
    #define W3D_NET_NONE 1
    #include <netinet/in.h>   // (htonl/ntohs e INADDR_LOOPBACK: las direcciones se arman y se muestran igual)
    #include <arpa/inet.h>
    typedef int SocketNet;
    #define SOCKET_NULL (-1)
#else
    #define W3D_NET_POSIX 1
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
        #define W3D_NET_IFADDRS 1
    #endif
    typedef int SocketNet;
    #define SOCKET_NULL (-1)
#endif

#if !defined(W3D_NET_NO_LUA)
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#endif

namespace w3dNet {

namespace {

// ----------------------------------------------------------------------------------------------- constantes
enum Type { T_FIND = 1, T_SESSION, T_JOIN, T_WELCOME, T_REJECT, T_PLAYERS, T_DATA, T_ACK, T_PING, T_PONG,
            T_BYE };
const double RESEND = 0.25;       // un confiable sin ACK se vuelve a mandar
const double PING_EACH = 1.0;     // sin nada que mandar, un ping
const double GONE = 8.0;          // sin noticias: se fue
const double FIND_EACH = 1.0;     // mientras busca, vuelve a preguntar
const double SESSION_OLD = 4.0;   // una partida que no responde sale de la lista
const double JOIN_EACH = 0.5;     // el pedido de unirse se repite hasta la respuesta
const double JOIN_MAX = 6.0;      // y se rinde
const int PACKAGE_MAX = 60000;
const unsigned char FOR_ALL = 255;

// --------------------------------------------------------------------------------------------- el reloj
double Now() {
#if defined(W3D_NET_WINSOCK)
    static LARGE_INTEGER f = { 0 };
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#elif defined(W3D_DREAMCAST)
    return (double)timer_us_gettime64() * 1e-6;
#elif defined(W3D_NET_POSIX) && !defined(W3D_SYMBIAN)
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#elif defined(W3D_NET_POSIX)
    struct timeval t;
    gettimeofday(&t, 0);
    return (double)t.tv_sec + (double)t.tv_usec * 1e-6;
#else
    return 0.0;
#endif
}

// ------------------------------------------------------------------------------------- escribir y leer bytes
struct Writer {
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
struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    bool error;
    Reader(const void* d, int n) : p((const unsigned char*)d), end((const unsigned char*)d + n), error(false) {}
    unsigned u8() { if (p >= end) { error = true; return 0; } return *p++; }
    unsigned u32() { unsigned a = u8(), b = u8(), c = u8(), d = u8(); return a | (b << 8) | (c << 16) | (d << 24); }
    std::string str() {
        const unsigned n = u8();
        if (p + n > end) { error = true; return std::string(); }
        std::string s((const char*)p, n);
        p += n;
        return s;
    }
    int remain() const { return (int)(end - p); }
};

// ------------------------------------------------------------------------------------------- direcciones
struct Direction {
    unsigned ip;      // en el orden de la red
    unsigned short port;   // en el orden de la red
    Direction() : ip(0), port(0) {}
    bool operator==(const Direction& o) const { return ip == o.ip && port == o.port; }
    bool operator<(const Direction& o) const { return ip < o.ip || (ip == o.ip && port < o.port); }
};

std::string Text(const Direction& d) {
    const unsigned char* b = (const unsigned char*)&d.ip;
    char s[32];
    sprintf(s, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (unsigned)ntohs(d.port));
    return s;
}

// "a.b.c.d", "a.b.c.d:puerto" o un nombre de equipo ("casa.local:47023")
bool Interpret(const std::string& text, Direction& d) {
#if defined(W3D_NET_NONE)
    (void)text; (void)d;
    return false;
#else
    std::string host = text;
    unsigned port = PORT;
    const size_t two = text.rfind(':');
    if (two != std::string::npos) {
        host = text.substr(0, two);
        port = (unsigned)atoi(text.c_str() + two + 1);
        if (!port || port > 65535) return false;
    }
    if (host.empty()) return false;
    d.port = htons((unsigned short)port);
    unsigned long ip = inet_addr(host.c_str());
    if (ip != INADDR_NONE || host == "255.255.255.255") {
        d.ip = (unsigned)ip;
        return true;
    }
    struct addrinfo track, *r = 0;
    memset(&track, 0, sizeof(track));
    track.ai_family = AF_INET;
    track.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), 0, &track, &r) != 0 || !r) return false;
    d.ip = (unsigned)((struct sockaddr_in*)r->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(r);
    return true;
#endif
}

// ---------------------------------------------------------------------------------------------- el socket
#if !defined(W3D_NET_NONE)
bool gWinsock = false;

SocketNet OpenSocket(unsigned short port, std::string& error) {
#if defined(W3D_NET_WINSOCK)
    if (!gWinsock) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { error = "no arranca Winsock"; return SOCKET_NULL; }
        gWinsock = true;
    }
#endif
    SocketNet s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCKET_NULL) { error = "no se pudo abrir un socket"; return SOCKET_NULL; }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
#if defined(W3D_NET_WINSOCK)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        char m[96];
        sprintf(m, "el puerto %u esta ocupado (otra partida en este equipo?)", (unsigned)port);
        error = port ? m : "no se pudo abrir un puerto";
#if defined(W3D_NET_WINSOCK)
        closesocket(s);
#else
        close(s);
#endif
        return SOCKET_NULL;
    }
    return s;
}

void CloseSocket(SocketNet& s) {
    if (s == SOCKET_NULL) return;
#if defined(W3D_NET_WINSOCK)
    closesocket(s);
#else
    close(s);
#endif
    s = SOCKET_NULL;
}
#endif

// las direcciones de broadcast: la de cada red local (si se pueden saber), la de "toda la red" y este mismo equipo
// (dos programas en la misma maquina, para probar)
std::vector<Direction> Broadcasts() {
    std::vector<Direction> v;
#if !defined(W3D_NET_NONE)
    Direction d;
    d.port = htons((unsigned short)PORT);
#if defined(W3D_NET_IFADDRS)
    struct ifaddrs* list = 0;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_BROADCAST) || !(i->ifa_flags & IFF_UP) || !i->ifa_broadaddr) continue;
            d.ip = (unsigned)((struct sockaddr_in*)i->ifa_broadaddr)->sin_addr.s_addr;
            v.push_back(d);
        }
        freeifaddrs(list);
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
struct Pending {
    std::string package;
    double sent;
};

// el enlace con otro programa: el host tiene uno por jugador; el que se une, uno con el host
struct Link {
    int id;
    Direction dir;
    std::string name;
    unsigned seqSend;                    // el numero del proximo confiable que mando
    std::map<unsigned, Pending> pending; // los mandados sin ACK
    unsigned seqReceive;                 // el proximo que espero
    std::map<unsigned, std::string> advanced;   // los que llegaron antes de tiempo (con su "de")
    double heard, sent;                     // la ultima noticia de el y lo ultimo que le mande
    Link() : id(-1), seqSend(1), seqReceive(1), heard(0), sent(0) {}
};

// un mensaje para Recibir
struct Message {
    int sender;
    std::string data;
};

// ------------------------------------------------------------------------------------------------ el estado
struct Session {
    bool allowed;
    std::string deviceName;
    State state;
    std::string error;
    SocketNet s;
    std::string game, name;
    unsigned session;            // el numero de la partida (al azar: dos partidas no se confunden)
    int max;
    bool open;
    int miId;
    // buscando
    std::vector<Match> sessions;
    double lastSearch;
    // uniendose
    Direction host;
    unsigned token;
    double requestFrom, lastRequest;
    // los enlaces (por id) y lo recibido
    std::map<int, Link> links;
    std::vector<Player> players;
    double lastList;
    std::deque<Message> received;
    Session() : allowed(true), state(NOTHING), s(SOCKET_NULL), session(0), max(2), open(true), miId(-1),
               lastSearch(0), token(0), requestFrom(0), lastRequest(0), lastList(0) {}
};
Session G;

unsigned Random32() {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)(Now() * 1000.0) ^ (unsigned)(size_t)&G);
        seeded = true;
    }
    return ((unsigned)rand() << 16) ^ (unsigned)rand() ^ ((unsigned)rand() << 8);
}

void Header(Writer& e, Type t) {
    e.b.reserve(64);
    e.bytes("W3R1", 4);
    e.u8((unsigned)t);
}

void SendTo(const Direction& a, const std::string& package) {
#if !defined(W3D_NET_NONE)
    if (G.s == SOCKET_NULL) return;
    struct sockaddr_in d;
    memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
    d.sin_addr.s_addr = a.ip;
    d.sin_port = a.port;
    sendto(G.s, package.data(), (int)package.size(), 0, (struct sockaddr*)&d, sizeof(d));
#else
    (void)a; (void)package;
#endif
}

void SendTo(Link& in, const std::string& package) {
    SendTo(in.dir, package);
    in.sent = Now();
}

void Fail(const std::string& reason) {
    G.state = FAILED;
    G.error = reason;
    w3dLogfW("red: %s", reason.c_str());
}

// la lista de jugadores (del host: el y sus enlaces; del que se une: la que manda el host)
void BuildListHost() {
    G.players.clear();
    Player yo;
    yo.id = 0; yo.name = DeviceName(); yo.yo = true;
    G.players.push_back(yo);
    for (std::map<int, Link>::iterator it = G.links.begin(); it != G.links.end(); ++it) {
        Player j;
        j.id = it->first; j.name = it->second.name; j.yo = false;
        G.players.push_back(j);
    }
}

void SendList() {
    Writer e;
    Header(e, T_PLAYERS);
    e.u32(G.session);
    e.u8((unsigned)G.players.size());
    for (size_t i = 0; i < G.players.size(); i++) {
        e.u8((unsigned)G.players[i].id);
        e.str(G.players[i].name);
    }
    for (std::map<int, Link>::iterator it = G.links.begin(); it != G.links.end(); ++it) SendTo(it->second, e.b);
    G.lastList = Now();
}

// un DATOS a un enlace (confiable: con su numero, queda pendiente hasta el ACK)
void SendData(Link& in, int sender, int recipient, const std::string& data, bool reliable) {
    Writer e;
    Header(e, T_DATA);
    e.u32(G.session);
    e.u8((unsigned)sender);
    e.u8(recipient < 0 ? FOR_ALL : (unsigned)recipient);
    e.u8(reliable ? 1 : 0);
    const unsigned seq = reliable ? in.seqSend++ : 0;
    e.u32(seq);
    e.b += data;
    SendTo(in, e.b);
    if (reliable) {
        Pending p;
        p.package = e.b;
        p.sent    = Now();
        in.pending[seq] = p;
    }
}

void Deliver(int sender, const std::string& data) {
    Message m;
    m.sender = sender;
    m.data = data;
    G.received.push_back(m);
}

// un DATOS que llego por un enlace: si es para mi, a la cola; el host reparte lo que es para otros
void ReachedData(int sender, int recipient, const std::string& data, bool reliable) {
    if (G.miId == 0) {
        for (std::map<int, Link>::iterator it = G.links.begin(); it != G.links.end(); ++it) {
            if (it->first == sender) continue;
            if (recipient == FOR_ALL || recipient == it->first) SendData(it->second, sender, recipient == FOR_ALL ? -1 : recipient,
                                                                   data, reliable);
        }
        if (recipient == FOR_ALL || recipient == 0) Deliver(sender, data);
    } else {
        Deliver(sender, data);
    }
}

void SendAck(Link& in) {
    Writer e;
    Header(e, T_ACK);
    e.u32(G.session);
    e.u8((unsigned)G.miId);
    e.u32(in.seqReceive - 1);
    SendTo(in, e.b);
}

Link* LinkOf(const Direction& d) {
    for (std::map<int, Link>::iterator it = G.links.begin(); it != G.links.end(); ++it)
        if (it->second.dir == d) return &it->second;
    return 0;
}

void Remove(int id, const char* reason) {
    std::map<int, Link>::iterator it = G.links.find(id);
    if (it == G.links.end()) return;
    w3dLogf("red: se fue %s (%s)", it->second.name.c_str(), reason);
    G.links.erase(it);
    if (G.miId == 0) {
        BuildListHost();
        SendList();
    }
}

// ---------------------------------------------------------------------------------------------- lo que llega
void Process(const Direction& from, const unsigned char* p, int n) {
    if (n < 5 || memcmp(p, "W3R1", 4) != 0) return;
    Reader l(p + 5, n - 5);
    const Type t = (Type)p[4];
    const double now = Now();
    switch (t) {
    case T_FIND: {
        const std::string game = l.str();
        if (l.error || G.state != WAITING || !G.open || game != G.game) return;
        if ((int)G.links.size() + 1 >= G.max) return;
        Writer e;
        Header(e, T_SESSION);
        e.str(G.game);
        e.u32(G.session);
        e.str(G.name);
        e.str(DeviceName());
        e.u8((unsigned)G.links.size() + 1);
        e.u8((unsigned)G.max);
        SendTo(from, e.b);
        return;
    }
    case T_SESSION: {
        if (G.state != FINDING) return;
        Match q;
        q.game = l.str();
        q.session = l.u32();
        q.name    = l.str();
        q.device = l.str();
        q.players = (int)l.u8();
        q.max = (int)l.u8();
        if (l.error || q.game != G.game) return;
        q.direction = Text(from);
        q.seen = now;
        // (la misma partida responde por varios caminos: este equipo, la red local; se queda con una, la de la
        // red si la hay)
        for (size_t i = 0; i < G.sessions.size(); i++) {
            if (G.sessions[i].session != q.session) continue;
            const bool local = from.ip == htonl(INADDR_LOOPBACK);
            if (local) q.direction = G.sessions[i].direction;
            G.sessions[i] = q;
            return;
        }
        G.sessions.push_back(q);
        return;
    }
    case T_JOIN: {
        const std::string game = l.str();
        const unsigned session = l.u32(), token = l.u32();
        const std::string name = l.str();
        if (l.error || G.state != WAITING || game != G.game || session != G.session) return;
        Link* already = LinkOf(from);
        int id = already ? already->id : -1;
        if (!already) {
            if (!G.open || (int)G.links.size() + 1 >= G.max) {
                Writer e;
                Header(e, T_REJECT);
                e.u32(token);
                e.str(G.open ? "la partida esta llena" : "la partida ya empezo");
                SendTo(from, e.b);
                return;
            }
            for (id = 1; G.links.count(id); id++) {}
            Link in;
            in.id = id;
            in.dir = from;
            in.name = name.empty() ? "?" : name;
            in.heard = now;
            G.links[id] = in;
            w3dLogf("red: entro %s (%s) como jugador %d", in.name.c_str(), Text(from).c_str(), id);
        }
        Writer e;
        Header(e, T_WELCOME);
        e.u32(G.session);
        e.u32(token);
        e.u8((unsigned)id);
        SendTo(from, e.b);
        BuildListHost();
        SendList();
        return;
    }
    case T_WELCOME: {
        const unsigned session = l.u32(), token = l.u32();
        const int id = (int)l.u8();
        if (l.error || G.state != CONNECTING || token != G.token || !(from == G.host)) return;
        G.session = session;
        G.miId = id;
        G.state = CONNECTED;
        Link in;
        in.id = 0;
        in.dir = from;
        in.heard = now;
        G.links.clear();
        G.links[0] = in;
        w3dLogf("red: adentro de la partida de %s como jugador %d", Text(from).c_str(), id);
        return;
    }
    case T_REJECT: {
        const unsigned token = l.u32();
        const std::string reason = l.str();
        if (l.error || G.state != CONNECTING || token != G.token) return;
        Fail(reason);
        return;
    }
    default: break;
    }
    // lo demas es de una sesion: de un enlace conocido
    const unsigned session = l.u32();
    if (l.error || session != G.session || (G.state != WAITING && G.state != CONNECTED)) return;
    Link* in = LinkOf(from);
    if (!in) return;
    in->heard = now;
    switch (t) {
    case T_PLAYERS: {
        if (G.miId == 0) return;
        const int count = (int)l.u8();
        std::vector<Player> v;
        for (int i = 0; i < count && !l.error; i++) {
            Player j;
            j.id = (int)l.u8();
            j.name = l.str();
            j.yo = j.id == G.miId;
            v.push_back(j);
        }
        if (!l.error) G.players = v;
        if (!G.players.empty()) in->name = G.players[0].name;
        return;
    }
    case T_DATA: {
        const int origin = (int)l.u8();
        const int recipient = (int)l.u8();
        const bool reliable = l.u8() != 0;
        const unsigned seq = l.u32();
        if (l.error) return;
        // (el host sabe quien manda: el que se une no puede hacerse pasar por otro)
        const int sender = G.miId == 0 ? in->id : origin;
        std::string data((const char*)l.p, (size_t)l.remain());
        if (!reliable) { ReachedData(sender, recipient, data, false); return; }
        if (seq >= in->seqReceive && !in->advanced.count(seq)) {
            Writer saved;
            saved.u8((unsigned)sender);
            saved.u8((unsigned)recipient);
            saved.b += data;
            in->advanced[seq] = saved.b;
        }
        // los que ya estan en orden
        std::map<unsigned, std::string>::iterator it;
        while ((it = in->advanced.find(in->seqReceive)) != in->advanced.end()) {
            const std::string& g = it->second;
            const int d = (unsigned char)g[0], pa = (unsigned char)g[1];
            const std::string body = g.substr(2);
            in->advanced.erase(it);
            in->seqReceive++;
            ReachedData(d, pa, body, true);
            in = LinkOf(from);
            if (!in) return;
        }
        SendAck(*in);
        return;
    }
    case T_ACK: {
        l.u8();
        const unsigned until = l.u32();
        if (l.error) return;
        while (!in->pending.empty() && in->pending.begin()->first <= until)
            in->pending.erase(in->pending.begin());
        return;
    }
    case T_PING: {
        l.u8();
        const unsigned mark = l.u32();
        Writer e;
        Header(e, T_PONG);
        e.u32(G.session);
        e.u8((unsigned)G.miId);
        e.u32(mark);
        SendTo(*in, e.b);
        return;
    }
    case T_PONG:
        return;
    case T_BYE:
        if (G.miId == 0) Remove(in->id, "se despidio");
        else Fail("el host cerro la partida");
        return;
    default:
        return;
    }
}

void Read() {
#if !defined(W3D_NET_NONE)
    if (G.s == SOCKET_NULL) return;
    static char buf[65536];
    for (int turns = 0; turns < 512; turns++) {
        struct sockaddr_in a;
#if defined(W3D_NET_WINSOCK)
        int la = sizeof(a);
#else
        socklen_t la = sizeof(a);
#endif
        const int n = (int)recvfrom(G.s, buf, sizeof(buf), 0, (struct sockaddr*)&a, &la);
        if (n <= 0) break;
        Direction d;
        d.ip = (unsigned)a.sin_addr.s_addr;
        d.port = a.sin_port;
        Process(d, (const unsigned char*)buf, n);
        if (G.s == SOCKET_NULL) break;
    }
#endif
}

void SendFind() {
    Writer e;
    Header(e, T_FIND);
    e.str(G.game);
    const std::vector<Direction> v = Broadcasts();
    for (size_t i = 0; i < v.size(); i++) SendTo(v[i], e.b);
    G.lastSearch = Now();
}

void SendJoin() {
    Writer e;
    Header(e, T_JOIN);
    e.str(G.game);
    e.u32(G.session);
    e.u32(G.token);
    e.str(DeviceName());
    SendTo(G.host, e.b);
    G.lastRequest = Now();
}

bool Open(unsigned short port) {
#if defined(W3D_NET_NONE)
    (void)port;
    Fail("esta plataforma todavia no tiene red");
    return false;
#else
    if (!G.allowed) { Fail("este proyecto no tiene permiso para usar la red"); return false; }
    std::string error;
    G.s = OpenSocket(port, error);
    if (G.s == SOCKET_NULL) { Fail(error); return false; }
    return true;
#endif
}

} // namespace

// =========================================================================================== la interfaz
void SetAllowed(bool yes) {
    G.allowed = yes;
    if (!yes && G.state != NOTHING) Close();
}
bool Allowed() { return G.allowed; }

bool Supported() {
#if defined(W3D_NET_NONE)
    return false;
#else
    return true;
#endif
}

bool Available() { return Supported() && G.allowed; }

void SetDeviceName(const std::string& name) { G.deviceName = name; }

std::string DeviceName() {
    return G.deviceName.empty() ? DefaultName() : G.deviceName;
}

std::string DefaultName() {
    static std::string defaultValue;
    if (!defaultValue.empty()) return defaultValue;
#if !defined(W3D_NET_NONE)
#if defined(W3D_NET_WINSOCK)
    if (!gWinsock) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) == 0) gWinsock = true;
    }
#endif
#if defined(W3D_DREAMCAST)
    defaultValue = "Dreamcast";   // (la consola no tiene nombre)
#else
    char h[128];
    if (gethostname(h, sizeof(h)) == 0) {
        h[sizeof(h) - 1] = 0;
        // (sin el dominio: "casa.local" -> "casa")
        char* point = strchr(h, '.');
        if (point && point != h) *point = 0;
        defaultValue = h;
    }
#endif
#endif
    if (defaultValue.empty() || defaultValue == "localhost") {
        char s[32];
        sprintf(s, "Whisk3D-%04u", Random32() % 10000);
        defaultValue = s;
    }
    return defaultValue;
}

std::string DirectionLocal() {
#if defined(W3D_NET_IFADDRS)
    std::string r;
    struct ifaddrs* list = 0;
    if (getifaddrs(&list) != 0) return r;
    for (struct ifaddrs* i = list; i && r.empty(); i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (!(i->ifa_flags & IFF_UP)) continue;
        Direction d;
        d.ip = (unsigned)((struct sockaddr_in*)i->ifa_addr)->sin_addr.s_addr;
        r = Text(d);
        r = r.substr(0, r.find(':'));
    }
    freeifaddrs(list);
    return r;
#elif defined(W3D_DREAMCAST)
    // (la que le dio el DHCP al adaptador)
    const netif_t* n = net_default_dev;
    if (!n || !n->ip_addr[0]) return std::string();
    char s[20];
    sprintf(s, "%d.%d.%d.%d", n->ip_addr[0], n->ip_addr[1], n->ip_addr[2], n->ip_addr[3]);
    return s;
#elif !defined(W3D_NET_NONE)
    // (sin la lista de interfaces: la que usaria para salir; conectar un socket UDP no manda nada)
    std::string error;
    SocketNet s = OpenSocket(0, error);
    if (s == SOCKET_NULL) return std::string();
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    a.sin_port = htons(53);
    std::string r;
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) == 0) {
#if defined(W3D_NET_WINSOCK)
        int la = sizeof(a);
#else
        socklen_t la = sizeof(a);
#endif
        if (getsockname(s, (struct sockaddr*)&a, &la) == 0) {
            Direction d;
            d.ip = (unsigned)a.sin_addr.s_addr;
            r = Text(d);
            r = r.substr(0, r.find(':'));
        }
    }
    CloseSocket(s);
    return r;
#else
    return std::string();
#endif
}

bool Host(const std::string& game, const std::string& name, int max) {
    Close();
    if (!Open((unsigned short)PORT)) return false;
    G.game = game;
    G.name    = name.empty() ? DeviceName() : name;
    G.max     = max < 2 ? 2 : (max > MAX_PLAYERS ? MAX_PLAYERS : max);
    G.session = Random32() | 1;
    G.open = true;
    G.miId = 0;
    G.state = WAITING;
    BuildListHost();
    w3dLogf("red: hosteando \"%s\" (%s, hasta %d jugadores) en el puerto %d", G.name.c_str(), game.c_str(),
            G.max, (int)PORT);
    return true;
}

void SetOpen(bool yes) { G.open = yes; }

bool Find(const std::string& game) {
    if (G.state != FINDING) {
        Close();
        if (!Open(0)) return false;
        G.state = FINDING;
    }
    G.game = game;
    G.sessions.clear();
    SendFind();
    return true;
}

const std::vector<Match>& Matches() { return G.sessions; }

bool Join(const std::string& direction) {
    Direction d;
    if (!Interpret(direction, d)) {
        Close();
        Fail("no entiendo la direccion \"" + direction + "\"");
        return false;
    }
    // (si venia de la lista, la sesion es la de esa partida; si es una direccion escrita, la que conteste)
    unsigned session = 0;
    for (size_t i = 0; i < G.sessions.size(); i++)
        if (G.sessions[i].direction == Text(d)) session = G.sessions[i].session;
    const std::string game = G.game;
    if (G.state != FINDING) {
        Close();
        if (!Open(0)) return false;
    }
    G.game = game;
    G.host = d;
    G.session = session;
    G.token = Random32();
    G.state = CONNECTING;
    G.requestFrom = Now();
    SendJoin();
    return true;
}

void Close() {
#if !defined(W3D_NET_NONE)
    if (G.s != SOCKET_NULL && (G.state == WAITING || G.state == CONNECTED)) {
        Writer e;
        Header(e, T_BYE);
        e.u32(G.session);
        e.u8((unsigned)G.miId);
        for (std::map<int, Link>::iterator it = G.links.begin(); it != G.links.end(); ++it) SendTo(it->second, e.b);
    }
    CloseSocket(G.s);
#endif
    G.state = NOTHING;
    G.error.clear();
    G.links.clear();
    G.players.clear();
    G.sessions.clear();
    G.received.clear();
    G.miId = -1;
    G.session = 0;
}

State CurrentState() { return G.state; }
const std::string& Error() { return G.error; }
int MiId() { return (G.state == WAITING || G.state == CONNECTED) ? G.miId : -1; }
const std::vector<Player>& Players() { return G.players; }

bool Send(int recipient, const void* data, int bytes, bool reliable) {
    if ((G.state != WAITING && G.state != CONNECTED) || bytes < 0 || bytes > PACKAGE_MAX) return false;
    const std::string d((const char*)data, (size_t)bytes);
    if (G.miId == 0) {
        for (std::map<int, Link>::iterator it = G.links.begin(); it != G.links.end(); ++it)
            if (recipient < 0 || recipient == it->first) SendData(it->second, 0, recipient, d, reliable);
    } else {
        std::map<int, Link>::iterator it = G.links.find(0);
        if (it == G.links.end()) return false;
        SendData(it->second, G.miId, recipient, d, reliable);
    }
    return true;
}

bool Receive(int& sender, std::string& data) {
    if (G.received.empty()) Read();
    if (G.received.empty()) return false;
    sender = G.received.front().sender;
    data.swap(G.received.front().data);
    G.received.pop_front();
    return true;
}

double Silence(int id) {
    if (G.state != WAITING && G.state != CONNECTED) return 0;
    if (id >= 0 && id == G.miId) return 0;
    const double now = Now();
    double worst = 0;
    for (std::map<int, Link>::iterator it = G.links.begin(); it != G.links.end(); ++it) {
        // (el host mira el enlace de ese jugador; el que se une tiene uno solo, el del host, y todo pasa por ahi)
        if (G.miId == 0 && id >= 0 && it->first != id) continue;
        if (now - it->second.heard > worst) worst = now - it->second.heard;
    }
    return worst;
}

double SilenceLimit() { return GONE; }

void Update() {
    if (G.state == NOTHING || G.state == FAILED) return;
    Read();
    const double now = Now();
    if (G.state == FINDING) {
        if (now - G.lastSearch > FIND_EACH) SendFind();
        for (size_t i = G.sessions.size(); i-- > 0;)
            if (now - G.sessions[i].seen > SESSION_OLD) G.sessions.erase(G.sessions.begin() + (long)i);
        return;
    }
    if (G.state == CONNECTING) {
        if (now - G.requestFrom > JOIN_MAX) Fail("el host no responde");
        else if (now - G.lastRequest > JOIN_EACH) SendJoin();
        return;
    }
    // reenviar lo confiable sin ACK, pings y los que se fueron
    std::vector<int> gone;
    for (std::map<int, Link>::iterator it = G.links.begin(); it != G.links.end(); ++it) {
        Link& in = it->second;
        if (now - in.heard > GONE) { gone.push_back(it->first); continue; }
        for (std::map<unsigned, Pending>::iterator p = in.pending.begin(); p != in.pending.end(); ++p) {
            if (now - p->second.sent < RESEND) continue;
            SendTo(in.dir, p->second.package);
            p->second.sent = now;
            in.sent = now;
        }
        if (now - in.sent > PING_EACH) {
            Writer e;
            Header(e, T_PING);
            e.u32(G.session);
            e.u8((unsigned)G.miId);
            e.u32((unsigned)(now * 1000.0));
            SendTo(in, e.b);
        }
    }
    for (size_t i = 0; i < gone.size(); i++) {
        if (G.miId == 0) Remove(gone[i], "no responde");
        else { Fail("se perdio la conexion con el host"); return; }
    }
    if (G.miId == 0 && now - G.lastList > 1.0) SendList();
}

// =================================================================================================== Lua
#if defined(W3D_NET_NO_LUA)
void RegisterBinds(void*) {}
#else
namespace {

// LOS VALORES DE LUA EN BYTES (netSend / netReceive): nil, booleanos, numeros (enteros de 1, 2 o 4 bytes; los
// demas en float de 4: posiciones, tiempos), textos y tablas (cualquier clave). Una tabla que se repite o se
// contiene a si misma no viaja (va nil). Los objetos de la escena tampoco (son punteros de este programa). Los tipos
// son los de W3dNetValue.h (el mismo formato, para un juego en C++)

void WriteValue(lua_State* L, int i, Writer& e, int prof) {
    const int t = lua_type(L, i);
    if (t == LUA_TBOOLEAN) { e.u8(lua_toboolean(L, i) ? V_TRUE : V_FALSE); return; }
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
        e.u8(V_TEXT);
        e.u32((unsigned)n);
        e.bytes(s, (int)n);
        return;
    }
    if (t == LUA_TTABLE && prof < 24) {
        if (i < 0) i = lua_gettop(L) + i + 1;
        e.u8(V_TABLE);
        const size_t place = e.b.size();
        e.u32(0);
        unsigned count = 0;
        lua_pushnil(L);
        while (lua_next(L, i) != 0) {
            const int tk = lua_type(L, -2), tv = lua_type(L, -1);
            const bool travels = (tk == LUA_TNUMBER || tk == LUA_TSTRING || tk == LUA_TBOOLEAN) &&
                               (tv == LUA_TNUMBER || tv == LUA_TSTRING || tv == LUA_TBOOLEAN || tv == LUA_TTABLE);
            if (travels) {
                WriteValue(L, lua_gettop(L) - 1, e, prof + 1);
                WriteValue(L, lua_gettop(L), e, prof + 1);
                count++;
            }
            lua_pop(L, 1);
        }
        e.b[place] = (char)(count & 0xFF);
        e.b[place + 1] = (char)((count >> 8) & 0xFF);
        e.b[place + 2] = (char)((count >> 16) & 0xFF);
        e.b[place + 3] = (char)((count >> 24) & 0xFF);
        return;
    }
    e.u8(V_NIL);
}

// deja el valor en la pila (false si los bytes estan mal)
bool ReadValue(lua_State* L, Reader& l, int prof) {
    if (prof > 24 || !lua_checkstack(L, 4)) return false;
    const unsigned t = l.u8();
    switch (t) {
    case V_NIL: lua_pushnil(L); break;
    case V_FALSE: lua_pushboolean(L, 0); break;
    case V_TRUE: lua_pushboolean(L, 1); break;
    case V_I8: lua_pushinteger(L, (lua_Integer)(signed char)l.u8()); break;
    case V_I16: { const unsigned a = l.u8(), b = l.u8(); lua_pushinteger(L, (lua_Integer)(short)(a | (b << 8))); break; }
    case V_I32: lua_pushinteger(L, (lua_Integer)(int)l.u32()); break;
    case V_F32: { const unsigned u = l.u32(); float f; memcpy(&f, &u, 4); lua_pushnumber(L, (lua_Number)f); break; }
    case V_F64: {
        if (l.remain() < 8) return false;
        double d;
        memcpy(&d, l.p, 8);
        l.p += 8;
        lua_pushnumber(L, (lua_Number)d);
        break;
    }
    case V_TEXT: {
        const unsigned n = l.u32();
        if (l.error || (int)n > l.remain()) return false;
        lua_pushlstring(L, (const char*)l.p, n);
        l.p += n;
        break;
    }
    case V_TABLE: {
        const unsigned n = l.u32();
        if (l.error || (int)n > l.remain()) return false;
        lua_createtable(L, 0, (int)n);
        for (unsigned k = 0; k < n; k++) {
            if (!ReadValue(L, l, prof + 1) || !ReadValue(L, l, prof + 1)) return false;
            if (lua_isnil(L, -2)) { lua_pop(L, 2); continue; }
            lua_rawset(L, -3);
        }
        break;
    }
    default: return false;
    }
    return !l.error;
}

const char* StateName(State e) {
    switch (e) {
    case FINDING: return "searching";
    case WAITING: return "waiting";
    case CONNECTING: return "connecting";
    case CONNECTED: return "connected";
    case FAILED: return "error";
    default: return "none";
    }
}

// netAvailable() -> bool, motivo ("sin permiso" / "sin red")
int LAvailable(lua_State* L) {
    lua_pushboolean(L, Available() ? 1 : 0);
    if (Available()) return 1;
    lua_pushstring(L, !Supported() ? "sin red" : "sin permiso");
    return 2;
}
// netDevice() -> el nombre de este dispositivo; netAddress() -> su IP en la red local
int LDevice(lua_State* L) { lua_pushstring(L, DeviceName().c_str()); return 1; }
int LDirection(lua_State* L) { lua_pushstring(L, DirectionLocal().c_str()); return 1; }
// netHost(nombre [, maximo [, juego]]) -> bool [, error]
int LHost(lua_State* L) {
    const bool ok = Host(luaL_optstring(L, 3, "w3d"), luaL_optstring(L, 1, ""), (int)luaL_optinteger(L, 2, 2));
    lua_pushboolean(L, ok ? 1 : 0);
    if (ok) return 1;
    lua_pushstring(L, Error().c_str());
    return 2;
}
// netOpen(bool): el host aparece y acepta jugadores (true) o no (la partida empezo)
int LOpen(lua_State* L) { SetOpen(lua_toboolean(L, 1) != 0); return 0; }
// netFind([juego]) -> bool [, error]: empieza a buscar o actualiza la lista
int LFind(lua_State* L) {
    const bool ok = Find(luaL_optstring(L, 1, "w3d"));
    lua_pushboolean(L, ok ? 1 : 0);
    if (ok) return 1;
    lua_pushstring(L, Error().c_str());
    return 2;
}
// netMatches() -> { {nombre, dispositivo, jugadores, maximo, direccion}, ... }
int LSessions(lua_State* L) {
    Update();
    const std::vector<Match>& v = Matches();
    lua_createtable(L, (int)v.size(), 0);
    for (size_t i = 0; i < v.size(); i++) {
        lua_createtable(L, 0, 5);
        lua_pushstring(L, v[i].name.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, v[i].device.c_str()); lua_setfield(L, -2, "device");
        lua_pushinteger(L, v[i].players); lua_setfield(L, -2, "players");
        lua_pushinteger(L, v[i].max); lua_setfield(L, -2, "max");
        lua_pushstring(L, v[i].direction.c_str()); lua_setfield(L, -2, "direction");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
// netJoin(direccion) -> bool [, error]
int LJoin(lua_State* L) {
    const bool ok = Join(luaL_checkstring(L, 1));
    lua_pushboolean(L, ok ? 1 : 0);
    if (ok) return 1;
    lua_pushstring(L, Error().c_str());
    return 2;
}
// netState() -> "nada" | "buscando" | "esperando" | "conectando" | "conectado" | "error", el error
int LState(lua_State* L) {
    lua_pushstring(L, StateName(CurrentState()));
    lua_pushstring(L, Error().c_str());
    return 2;
}
// netSelf() -> mi id (0 el host) o -1
int LYo(lua_State* L) { lua_pushinteger(L, MiId()); return 1; }
// netPlayers() -> { {id, nombre, yo}, ... }
int LPlayers(lua_State* L) {
    const std::vector<Player>& v = Players();
    lua_createtable(L, (int)v.size(), 0);
    for (size_t i = 0; i < v.size(); i++) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, v[i].id); lua_setfield(L, -2, "id");
        lua_pushstring(L, v[i].name.c_str()); lua_setfield(L, -2, "name");
        lua_pushboolean(L, v[i].yo ? 1 : 0); lua_setfield(L, -2, "yo");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
// netSilence([id]) -> segundos sin noticias de ese jugador (sin id: del que hace mas), el limite (despues, se fue)
int LSilence(lua_State* L) {
    Update();
    lua_pushnumber(L, Silence(lua_isnoneornil(L, 1) ? -1 : (int)luaL_checkinteger(L, 1)));
    lua_pushnumber(L, SilenceLimit());
    return 2;
}
// netSend(valor [, para [, confiable]]) -> bool. para: el id de un jugador (nil = todos); confiable por defecto
int LSend(lua_State* L) {
    Writer e;
    WriteValue(L, 1, e, 0);
    const int recipient = lua_isnoneornil(L, 2) ? -1 : (int)luaL_checkinteger(L, 2);
    const bool reliable = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    lua_pushboolean(L, Send(recipient, e.b.data(), (int)e.b.size(), reliable) ? 1 : 0);
    return 1;
}
// netReceive() -> { {from = id, data = valor}, ... } (todo lo que llego, en orden)
int LReceive(lua_State* L) {
    lua_newtable(L);
    int sender, n = 0;
    std::string d;
    while (Receive(sender, d)) {
        const int base = lua_gettop(L);
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, sender);
        lua_setfield(L, -2, "from");
        Reader l(d.data(), (int)d.size());
        if (!ReadValue(L, l, 0)) { lua_settop(L, base); continue; }   // (bytes rotos: se descarta)
        lua_setfield(L, -2, "data");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
// netClose()
int LClose(lua_State* L) { (void)L; Close(); return 0; }

} // namespace

void RegisterBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    lua_pushcfunction(L, LAvailable);  lua_setglobal(L, "netAvailable");
    lua_pushcfunction(L, LDevice); lua_setglobal(L, "netDevice");
    lua_pushcfunction(L, LDirection);   lua_setglobal(L, "netAddress");
    lua_pushcfunction(L, LHost);     lua_setglobal(L, "netHost");
    lua_pushcfunction(L, LOpen);       lua_setglobal(L, "netOpen");
    lua_pushcfunction(L, LFind);      lua_setglobal(L, "netFind");
    lua_pushcfunction(L, LSessions);    lua_setglobal(L, "netMatches");
    lua_pushcfunction(L, LJoin);      lua_setglobal(L, "netJoin");
    lua_pushcfunction(L, LState);      lua_setglobal(L, "netState");
    lua_pushcfunction(L, LYo);          lua_setglobal(L, "netSelf");
    lua_pushcfunction(L, LPlayers);   lua_setglobal(L, "netPlayers");
    lua_pushcfunction(L, LSilence);    lua_setglobal(L, "netSilence");
    lua_pushcfunction(L, LSend);      lua_setglobal(L, "netSend");
    lua_pushcfunction(L, LReceive);     lua_setglobal(L, "netReceive");
    lua_pushcfunction(L, LClose);      lua_setglobal(L, "netClose");
}
#endif // W3D_NET_NO_LUA

} // namespace w3dNet
