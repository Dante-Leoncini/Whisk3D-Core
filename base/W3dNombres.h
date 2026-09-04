// ============================================================================
//  W3dNombres.h — LA regla de NOMBRES UNICOS de Whisk3D (una sola, compartida).
//
//  PEDIDO DEL DUENO: "no tienen que poder haber objetos con el mismo nombre.
//  vertex group. uv. materiales. nada con el mismo nombre. ponele el .001 si
//  queres repetirlo. y si esta el xxx.001 pones el 002. y asi hasta que el
//  nombre no se repita".
//
//  Antes esta regla estaba escrita SIETE veces (Object::SetName, BoneNombreUnico,
//  UniqueNombre, Bone2DNombreUnico, Arm2DNombreUnico, CrearVertexGroup,
//  CrearUVGroup) con CINCO convenciones de sufijo incompatibles. Aca vive una
//  sola: el resto delega.
//
//  HEADER-ONLY a proposito: un .cpp nuevo del Core hay que agregarlo A MANO a
//  las 10 listas de fuentes (linux/android/web de los 3 juegos de ejemplo +
//  CompilarJuego.cpp en DOS lugares + Whisk3D.mmp). Todo inline, C++03 puro
//  (compila con RVCT de Symbian): nada de snprintf/std::snprintf ni C++11.
//
//  ===== LA REGLA, EXACTA =====
//  1) NORMALIZAR: se recortan espacios/tabs/CR/LF de los dos extremos. Si queda
//     vacio se usa 'porDefecto'. El nombre vacio NO es un nombre valido: dos
//     vacios chocarian igual que dos "Cubo".
//  2) Si el nombre normalizado esta LIBRE se devuelve TAL CUAL, aunque termine
//     en .NNN ("Cubo.007" libre queda "Cubo.007").
//  3) Si esta OCUPADO se separa el sufijo y se INCREMENTA ese numero; NUNCA se
//     encadena otro sufijo ("Cubo.001" -> "Cubo.002", jamas "Cubo.001.001").
//     Hay DOS lecturas del sufijo y cada una tiene su trabajo:
//       * CANONICA (W3dNombreSepararSufijo): la que GENERA esta unidad. Es
//         sufijo un '.' (que no sea el primer caracter) seguido de digitos D con
//           (a) D de EXACTAMENTE 3 digitos             -> ".001" ".999"
//           (b) D de MAS de 3 digitos sin cero inicial -> ".1000" ".12345"
//         Asi Formatear(Separar(x)) == x SIEMPRE. Se usa para decidir si un
//         nombre del scope OCUPA un candidato nuestro: ".1", ".01" y ".0001" no
//         son candidatos posibles, asi que no cuentan.
//       * DE BUSQUEDA (W3dNombreRaizDeBusqueda): para elegir la RAIZ acepta
//         CUALQUIER corrida de digitos detras del punto (".1", ".01", ".0000",
//         ".99999999999"). Sin esto "Cubo.0000" ocupado daba "Cubo.0000.001" —
//         DOS sufijos encadenados, justo lo que la regla promete que no pasa.
//         Ahora "Cubo.0000" ocupado da "Cubo.001" y "Cubo.1" da "Cubo.002".
//     UNICA excepcion (y es inevitable): un nombre que es SOLO sufijo, ".001",
//     no tiene raiz que rerootear (quedaria vacia) y va a ".001.001". A partir
//     de ahi ya es canonico y sigue por ".001.002".
//  4) Se busca el primer numero libre desde num+1. Formato: <1000 con tres
//     digitos y ceros a la izquierda (001..999), >=1000 los digitos que hagan
//     falta (1000, 1001...). NO hay tope.
//  5) La funcion NUNCA devuelve un nombre ocupado. Si el callback 'existe'
//     estuviera roto y no encontrara hueco en un millon de intentos, cae a un
//     contador estatico monotono. (Ese era justo el bug de Bone2DNombreUnico,
//     Arm2DNombreUnico y NombreMaterialLibre al llegar a 999: devolvian un
//     nombre YA ocupado.)
//  6) COMPARACION: EXACTA byte a byte, SENSIBLE A MAYUSCULAS, UTF-8 tal cual.
//     "Cubo" y "cubo" son DOS nombres distintos y ambos son legales a la vez.
//     Es lo que ya hacia todo el editor. OJO para el
//     .w3d-zip que viene: un zip extraido en Windows/macOS SI pisa "Cubo.glb"
//     con "cubo.glb" -> la ruta de la entrada se tiene que uniquificar aparte,
//     con su propio callback sobre las rutas ya emitidas.
// ============================================================================
#ifndef W3D_NOMBRES_H
#define W3D_NOMBRES_H

#include <string>
#include <vector>

// "existe 'n' en este espacio de nombres?" — 'ctx' es el dueno del scope
// (Mesh*, Armature*, Object* raiz de escena, ...). NUNCA se le pasa NULL a 'n'.
typedef bool (*W3dNombreExisteFn)(const std::string& n, void* ctx);

// ---------------------------------------------------------------------------
//  helpers puros (testeables sin escena: los usa el comando 'nombreunico')
// ---------------------------------------------------------------------------

// recorta blancos de los DOS extremos; si queda vacio devuelve 'porDefecto'
// (o "Objeto" si el caller no dijo nada).
inline std::string W3dNombreNormalizar(const std::string& n, const char* porDefecto) {
    size_t a = 0, b = n.size();
    while (a < b) {
        char c = n[a];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') a++; else break;
    }
    while (b > a) {
        char c = n[b - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') b--; else break;
    }
    if (b > a) return n.substr(a, b - a);
    return std::string(porDefecto && porDefecto[0] ? porDefecto : "Objeto");
}

// motor comun de las dos lecturas del sufijo (ver la regla 3 del encabezado).
// 'canonico' = exigir el formato que genera W3dNombreConSufijo. Devuelve true si
// separo algo; con false deja raiz = n, num = 0.
inline bool W3dNombreSepararImpl(const std::string& n, std::string& raiz, int& num, bool canonico) {
    raiz = n; num = 0;
    size_t p = n.find_last_of('.');
    // p == 0 -> ".001" pelado: su raiz seria vacia, no es sufijo de nada
    if (p == std::string::npos || p == 0 || p + 1 >= n.size()) return false;
    size_t d = n.size() - (p + 1);            // cantidad de digitos
    for (size_t i = p + 1; i < n.size(); i++)
        if (n[i] < '0' || n[i] > '9') return false; // no son todos digitos: no es sufijo
    // OVERFLOW: 'v' es un long y en RVCT/Symbian long mide 32 bits, asi que el
    // tope se chequea ANTES de multiplicar (antes se chequeaba despues y el
    // desbordamiento CON SIGNO ya habia ocurrido: UB en C++03). Con 9 digitos o
    // menos el maximo es 999.999.999 y no hay forma de desbordar; 10 o mas es un
    // numero absurdo y se trata como parte del nombre.
    if (d > 9) return false;
    // (a) exactamente 3 digitos, o (b) mas de 3 SIN cero inicial (round-trip exacto)
    if (canonico && !(d == 3 || (d > 3 && n[p + 1] != '0'))) return false;
    long v = 0;
    for (size_t i = p + 1; i < n.size(); i++) v = v * 10 + (n[i] - '0');
    raiz = n.substr(0, p);                    // p >= 1: nunca queda vacia
    num  = (int)v;
    return true;
}

// "Cubo.007" -> raiz "Cubo", num 7. Sin sufijo CANONICO -> raiz = n, num = 0.
inline void W3dNombreSepararSufijo(const std::string& n, std::string& raiz, int& num) {
    W3dNombreSepararImpl(n, raiz, num, true);
}

// RAIZ para BUSCAR hueco: acepta cualquier corrida de digitos detras del punto,
// asi ningun nombre queda con dos sufijos encadenados ("Cubo.0000" -> raiz
// "Cubo"). Ver la regla 3. El numero es el que se leyo (0 si es absurdo).
inline void W3dNombreRaizDeBusqueda(const std::string& n, std::string& raiz, int& num) {
    if (W3dNombreSepararImpl(n, raiz, num, false)) return;
    raiz = n; num = 0;
    // el unico caso que queda con digitos es el absurdo de 10+ cifras: igual se
    // rerootea (con numero 0) para no encadenar.
    size_t p = n.find_last_of('.');
    if (p == std::string::npos || p == 0 || p + 1 >= n.size()) return;
    for (size_t i = p + 1; i < n.size(); i++)
        if (n[i] < '0' || n[i] > '9') return;
    raiz = n.substr(0, p);
}

// (raiz, 7) -> "raiz.007"; (raiz, 1000) -> "raiz.1000". Sin sprintf (RVCT/STLport).
inline std::string W3dNombreConSufijo(const std::string& raiz, int n) {
    if (n < 0) n = 0;
    char buf[24];
    int len = 0;
    if (n == 0) { buf[len++] = '0'; }
    else { int t = n; char tmp[24]; int k = 0;
           while (t > 0) { tmp[k++] = (char)('0' + (t % 10)); t /= 10; }
           while (k > 0) buf[len++] = tmp[--k]; }
    std::string dig(buf, (size_t)len);
    while (dig.size() < 3) dig = std::string("0") + dig;   // 001..999; >=1000 queda tal cual
    return raiz + "." + dig;
}

// ---------------------------------------------------------------------------
//  INDICE — la busqueda RAPIDA (UNA sola pasada por el espacio de nombres).
//
//  PORQUE EXISTE: preguntar "existe?" candidato por candidato cuesta una pasada
//  COMPLETA por el scope POR CANDIDATO. Con N homonimos eso es O(N) candidatos x
//  O(N) nodos = O(N^2) por nombre y O(N^3) para armar la escena. Medido con
//  "add vertex" xN en el editor: N=800 0.60s, N=1600 2.90s, N=3000 18.98s.
//  El indice recorre el scope UNA vez, se queda solo con los numeros que ocupan
//  la MISMA raiz y elige el hueco por PIGEONHOLE: O(N) por nombre.
//
//  El resultado es EXACTAMENTE el mismo que el de la busqueda vieja: el primer
//  numero libre desde num+1. Solo un nombre CANONICO (el que genera
//  W3dNombreConSufijo) puede ocupar un candidato, asi que alcanza con mirar esos.
//
//  Uso:
//      W3dNombreIndice ix; ix.Empezar(base, "Objeto");
//      for (cada nombre 'n' del scope, salteando el propio) ix.Ver(n);
//      std::string libre = ix.Resolver();
// ---------------------------------------------------------------------------
struct W3dNombreIndice {
    std::string      pedido;    // el nombre normalizado que se pidio
    std::string      raiz;      // su raiz de busqueda (pedido sin el .NNN)
    int              desde;     // primer numero candidato (num del sufijo + 1)
    bool             choca;     // 'pedido' ya esta tomado
    std::vector<int> tomados;   // numeros >= desde que ya ocupan esa raiz

    W3dNombreIndice() : desde(1), choca(false) {}

    void Empezar(const std::string& base, const char* porDefecto) {
        pedido = W3dNombreNormalizar(base, porDefecto);
        int num = 0;
        W3dNombreRaizDeBusqueda(pedido, raiz, num);
        desde = num + 1;
        choca = false;
        tomados.clear();
    }

    // un nombre YA TOMADO del scope. Barato: el 99% sale por el largo o el punto.
    void Ver(const std::string& n) {
        if (n == pedido) choca = true;
        if (n.size() <= raiz.size() + 1) return;       // no entra "raiz" + "." + digito
        if (n[raiz.size()] != '.') return;
        if (n.compare(0, raiz.size(), raiz) != 0) return;
        std::string r; int v = 0;
        W3dNombreSepararSufijo(n, r, v);               // CANONICO: solo lo que podriamos generar
        if (v >= desde && r.size() == raiz.size() && r == raiz) tomados.push_back(v);
    }

    std::string Resolver() {
        if (!choca) return pedido;                     // libre: tal cual (aunque termine en .NNN)
        // PIGEONHOLE: vimos M numeros tomados >= desde, asi que entre 'desde' y
        // 'desde+M' hay por lo menos UN hueco. Marcador de M+1 casilleros: O(M),
        // sin ordenar y sin buscar de a uno.
        const size_t M = tomados.size();
        std::vector<unsigned char> marca(M + 1, (unsigned char)0);
        for (size_t i = 0; i < M; i++) {
            long off = (long)tomados[i] - (long)desde;
            if (off >= 0 && off <= (long)M) marca[(size_t)off] = 1;
        }
        for (size_t k = 0; k <= M; k++)
            if (!marca[k]) return W3dNombreConSufijo(raiz, desde + (int)k);
        return W3dNombreConSufijo(raiz, desde + (int)M + 1);   // inalcanzable (pigeonhole)
    }
};

// ---------------------------------------------------------------------------
//  LA funcion. Devuelve SIEMPRE un nombre libre en el espacio que describe 'existe'.
//
//  Es la version LENTA (una pasada por el scope POR CANDIDATO): queda para los
//  scopes chiquitos que solo saben responder "existe?" (materiales, huesos,
//  clips). Los scopes que pueden ENUMERAR sus nombres (los objetos de la escena,
//  las listas) usan W3dNombreIndice y no pagan el cuadrado.
// ---------------------------------------------------------------------------
inline std::string W3dNombreUnico(const std::string& base, const char* porDefecto,
                                  W3dNombreExisteFn existe, void* ctx) {
    std::string n = W3dNombreNormalizar(base, porDefecto);
    if (!existe) return n;                 // sin espacio de nombres que consultar
    if (!existe(n, ctx)) return n;         // libre: se respeta tal cual (aunque termine en .NNN)

    std::string raiz; int num = 0;
    W3dNombreRaizDeBusqueda(n, raiz, num); // "Cubo.001" -> ("Cubo", 1); "Cubo" -> ("Cubo", 0)

    for (int i = num + 1; i < num + 1000001; i++) {
        std::string cand = W3dNombreConSufijo(raiz, i);
        if (!existe(cand, ctx)) return cand;
    }
    // red de seguridad: 'existe' esta roto (siempre true). Igual NO devolvemos
    // un nombre ocupado: contador monotono de proceso.
    static int gEmergencia = 0;
    return W3dNombreConSufijo(raiz, 1000001 + (gEmergencia++));
}

// ---------------------------------------------------------------------------
//  variante por LISTA: para los scopes que ya juntan punteros a sus nombres.
//  'excluir' = el nombre PROPIO (renombrar al mismo valor NO debe dar .001).
//  NULL = no se excluye ninguno.
// ---------------------------------------------------------------------------
struct W3dNombreLista {
    const std::vector<std::string*>* v;
    const std::string* excluir;
};
inline bool W3dNombreEnListaExiste(const std::string& n, void* ctx) {
    W3dNombreLista* L = (W3dNombreLista*)ctx;
    for (size_t i = 0; i < L->v->size(); i++) {
        std::string* p = (*L->v)[i];
        if (!p || p == L->excluir) continue;
        if (*p == n) return true;
    }
    return false;
}
// (usa el INDICE: una sola pasada por la lista, no una por candidato)
inline std::string W3dNombreUnicoEnLista(const std::string& base, const char* porDefecto,
                                         const std::vector<std::string*>& tomados,
                                         const std::string* excluir) {
    W3dNombreIndice ix; ix.Empezar(base, porDefecto);
    for (size_t i = 0; i < tomados.size(); i++) {
        std::string* p = tomados[i];
        if (!p || p == excluir) continue;
        ix.Ver(*p);
    }
    return ix.Resolver();
}

// misma idea pero sobre una lista de VALORES (cuando el caller no tiene punteros
// estables; ej: los huesos, que viven por valor dentro del armature).
struct W3dNombreListaVal {
    const std::vector<std::string>* v;
    int excluir;   // indice que puede conservar su nombre (-1 = ninguno)
};
inline bool W3dNombreEnListaValExiste(const std::string& n, void* ctx) {
    W3dNombreListaVal* L = (W3dNombreListaVal*)ctx;
    for (size_t i = 0; i < L->v->size(); i++)
        if ((int)i != L->excluir && (*L->v)[i] == n) return true;
    return false;
}
inline std::string W3dNombreUnicoEnValores(const std::string& base, const char* porDefecto,
                                           const std::vector<std::string>& tomados, int excluir) {
    W3dNombreIndice ix; ix.Empezar(base, porDefecto);
    for (size_t i = 0; i < tomados.size(); i++)
        if ((int)i != excluir) ix.Ver(tomados[i]);
    return ix.Resolver();
}

#endif // W3D_NOMBRES_H
