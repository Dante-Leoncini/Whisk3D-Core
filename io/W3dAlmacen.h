#ifndef W3DALMACEN_H
#define W3DALMACEN_H

#include <string>
#include <vector>

// forward: el lector de zip vive en W3dZip.h y NO se incluye aca a proposito.
// w3dFilesystem.cpp (que compila en los 4 OS, incluido el .mmp de Symbian y los
// 3 juegos de ejemplo) incluye ESTE header para consultar el almacen montado:
// que no arrastre el zip ni sus <map>/<set> es parte del contrato.
class W3dZipLector;

// ============================================================================
//  W3dAlmacen - de donde salen los archivos de un PROYECTO (C++03, sin
//  dependencias de plataforma).
//
//  EL PRINCIPIO: EL CONTENEDOR SE MONTA, NO SE EXTRAE.
//  El .w3d v2 viejo extraia todo a userdata/w3d_abierto/<nombre> y por eso
//  "adentro del zip" nunca fue verdad: el editor trabajaba sobre la copia, con
//  dos fuentes de verdad y un "guardar" que era "re-empaquetar". En vez de eso,
//  el .w3d se ABRE, se indexa (W3dZipLector) y se REGISTRA como origen virtual
//  de w3dFileSystem::ReadFileBytes, igual que el pak de los juegos compilados.
//
//  Consecuencia: texturas (w3dTexture), fuentes TTF (Fuente2D), scripts lua
//  (W3dScript usa luaL_loadbuffer, no luaL_loadfile), escenas .w3dui
//  (UI2DFormato) y GLB/glTF (import_gltf lee por ReadFileBytes) cargan desde
//  ADENTRO del contenedor SIN tocar un solo call site.
//
//  Tres backends con EXACTAMENTE el mismo layout de nombres de entrada
//  ("escenas/menu.w3dui", "texturas/pausa.png", "scripts/menu.lua"):
//    AlmacenZip      -> el .w3d contenedor (el caso normal)
//    AlmacenCarpeta  -> el mismo layout como carpetas de verdad (modo
//                       desempaquetado: diffs legibles en git, VS Code)
//    (legacy)        -> archivos sueltos: no necesita almacen, es el camino
//                       de hoy con rutas relativas al .w3d
//
//  NOMBRES DE ENTRADA: siempre relativos, con '/', sin "..", sin backslash
//  (lo valida W3dZipNombreSeguro). Una ruta ABSOLUTA nunca es una entrada:
//  cae al filesystem real, que es justo lo que tiene que pasar con los assets
//  externos ("ext:" en el JSON).
// ============================================================================
class W3dAlmacen {
public:
    // inline a proposito: asi los builds que solo tienen el HEADER (el runtime
    // de los juegos, que nada mas consulta el puntero montado) no necesitan
    // linkear W3dAlmacen.cpp
    virtual ~W3dAlmacen() {}
    virtual bool Abrir(const std::string& ruta) = 0;
    virtual bool Existe(const std::string& entrada) const = 0;
    virtual bool Leer(const std::string& entrada, std::vector<unsigned char>& out) = 0;
    // 'n' bytes desde 'desde' de una entrada (el streaming de mallas: solo sus bloques de render). Por defecto la
    // entrada entera y el recorte; el zip lo hace con un fseek
    virtual bool LeerRango(const std::string& entrada, unsigned desde, unsigned n, std::vector<unsigned char>& out);
    virtual void Listar(std::vector<std::string>& out) const = 0;
    virtual unsigned Tam(const std::string& entrada) const = 0;
    virtual void Cerrar() = 0;
};

// ---- backend ZIP: el .w3d contenedor, leido por indice (no se extrae nada) ----
class AlmacenZip : public W3dAlmacen {
public:
    AlmacenZip();
    virtual ~AlmacenZip();
    virtual bool Abrir(const std::string& ruta);
    virtual bool Existe(const std::string& entrada) const;
    virtual bool Leer(const std::string& entrada, std::vector<unsigned char>& out);
    virtual bool LeerRango(const std::string& entrada, unsigned desde, unsigned n, std::vector<unsigned char>& out);
    virtual void Listar(std::vector<std::string>& out) const;
    virtual unsigned Tam(const std::string& entrada) const;
    virtual void Cerrar();
    // acceso al lector para el guardado incremental (W3dZipWriter::CopiarDe)
    W3dZipLector& Lector() { return *zip; }
private:
    AlmacenZip(const AlmacenZip&);
    AlmacenZip& operator=(const AlmacenZip&);
    W3dZipLector* zip;   // por puntero: el header no conoce el tipo (ver arriba)
};

// ---- backend CARPETA: el MISMO layout, en carpetas reales ----
class AlmacenCarpeta : public W3dAlmacen {
public:
    virtual bool Abrir(const std::string& carpeta);   // la raiz del layout
    virtual bool Existe(const std::string& entrada) const;
    virtual bool Leer(const std::string& entrada, std::vector<unsigned char>& out);
    virtual void Listar(std::vector<std::string>& out) const;
    virtual unsigned Tam(const std::string& entrada) const;
    virtual void Cerrar();
    const std::string& Base() const { return base; }
private:
    std::string base;
};

// ---------------------------------------------------------------------------
//  MONTAJE: el almacen que consulta w3dFileSystem::ReadFileBytes
//
//  El editor monta el proyecto abierto y NADA MAS del editor sabe que backend
//  es. Montar NO transfiere la propiedad: el que monta destruye el almacen (y
//  DEBE desmontarlo antes).
//
//  OJO - la IMPLEMENTACION de estas tres vive en w3dFilesystem.cpp, no en
//  W3dAlmacen.cpp, a proposito: asi los builds que compilan el filesystem del
//  Core pero NO el almacen (el .mmp de Symbian, los 3 juegos de ejemplo)
//  siguen linkeando sin arrastrar el zip.
// ---------------------------------------------------------------------------
void W3dAlmacenMontar(W3dAlmacen* a);
void W3dAlmacenDesmontar();
W3dAlmacen* W3dAlmacenMontado();

// ---------------------------------------------------------------------------
//  VARIOS CONTENEDORES MONTADOS A LA VEZ: EL REGISTRO POR NOMBRE (las librerias)
//
//  El proyecto abierto es el almacen SIN nombre de arriba: sus entradas van peladas
//  ("texturas/x.png"). Cada LIBRERIA externa (otro .w3d, de solo lectura, del que el
//  proyecto usa prefabs, escenas o recursos) se monta ADEMAS con su nombre, y sus
//  entradas se nombran con PREFIJO: "lib:<nombre>/texturas/x.png". ReadFileBytes y
//  FileExists enrutan ese prefijo al almacen de esa libreria: dos librerias (y el
//  proyecto) no chocan aunque tengan entradas con el mismo nombre, y todo lo que se
//  carga tarde (texturas de la cola diferida, scripts, .w3db, .w3da) sigue
//  encontrando lo suyo sin tocar sus call sites.
//
//  SIN un almacen con ese nombre (el JUEGO COMPILADO: las entradas de cada libreria
//  viajan extraidas en "librerias/<nombre>/<entrada>", al lado del proyecto.json o en
//  el pak) la ruta se traduce a W3dRutaLibreriaEmpaquetada y se busca como cualquier
//  otro archivo del juego (pak, APK, carpeta del binario).
//
//  Igual que el montado: registrar NO transfiere la propiedad (el que registra
//  destruye, y quita el registro antes). Implementado en w3dFilesystem.cpp (mismo
//  motivo que las tres de arriba).
// ---------------------------------------------------------------------------
void W3dAlmacenRegistrar(const std::string& nombre, W3dAlmacen* a);   // a NULL = lo quita
W3dAlmacen* W3dAlmacenPorNombre(const std::string& nombre);          // NULL = no hay
// "lib:<nombre>/<entrada>" -> su nombre y su entrada. false = no es una ruta de libreria
bool W3dRutaDeLibreria(const std::string& ruta, std::string* nombre, std::string* entrada);
// donde vive una entrada de libreria en el JUEGO COMPILADO: "librerias/<nombre>/<entrada>"
std::string W3dRutaLibreriaEmpaquetada(const std::string& nombre, const std::string& entrada);
// se pidio "lib:<nombre>/..." y no hay almacen con ese nombre: el editor lo MONTA en ese momento (una libreria
// vinculada que todavia nadie uso: la textura de un material del proyecto que nombra una de ella). true = quedo
// registrado. NULL (el juego compilado, los builds sin librerias) = se va derecho a la copia empaquetada
extern bool (*W3dAlmacenPedirHook)(const std::string& nombre);

#endif // W3DALMACEN_H
