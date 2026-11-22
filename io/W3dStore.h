#ifndef W3D_STORE_H
#define W3D_STORE_H

#include <string>
#include <vector>

// forward: el lector de zip vive en W3dZip.h y NO se incluye aca a proposito.
// w3dFilesystem.cpp (que compila en los 4 OS, incluido el .mmp de Symbian y los
// 3 juegos de ejemplo) incluye ESTE header para consultar el almacen montado:
// que no arrastre el zip ni sus <map>/<set> es parte del contrato.
class W3dZipReader;

// ============================================================================
//  W3dStore - de donde salen los archivos de un PROYECTO (C++03, sin
//  dependencias de plataforma).
//
//  EL PRINCIPIO: EL CONTENEDOR SE MONTA, NO SE EXTRAE.
//  El .w3d v2 viejo extraia todo a userdata/w3d_abierto/<nombre> y por eso
//  "adentro del zip" nunca fue verdad: el editor trabajaba sobre la copia, con
//  dos fuentes de verdad y un "guardar" que era "re-empaquetar". En vez de eso,
//  el .w3d se ABRE, se indexa (W3dZipReader) y se REGISTRA como origen virtual
//  de w3dFileSystem::ReadFileBytes, igual que el pak de los juegos compilados.
//
//  Consecuencia: texturas (w3dTexture), fuentes TTF (Fuente2D), scripts lua
//  (W3dScript usa luaL_loadbuffer, no luaL_loadfile), escenas .w3dui
//  (UI2DFormato) y GLB/glTF (import_gltf lee por ReadFileBytes) cargan desde
//  ADENTRO del contenedor SIN tocar un solo call site.
//
//  Tres backends con EXACTAMENTE el mismo layout de nombres de entrada
//  ("scenes/menu.w3dui", "textures/pausa.png", "scripts/menu.lua"):
//    StoreZip      -> el .w3d contenedor (el caso normal)
//    StoreFolder  -> el mismo layout como carpetas de verdad (modo
//                       desempaquetado: diffs legibles en git, VS Code)
//    (legacy)        -> archivos sueltos: no necesita almacen, es el camino
//                       de hoy con rutas relativas al .w3d
//
//  NOMBRES DE ENTRADA: siempre relativos, con '/', sin "..", sin backslash
//  (lo valida W3dZipNameSafe). Una ruta ABSOLUTA nunca es una entrada:
//  cae al filesystem real, que es justo lo que tiene que pasar con los assets
//  externos ("ext:" en el JSON).
// ============================================================================
class W3dStore {
public:
    // inline a proposito: asi los builds que solo tienen el HEADER (el runtime
    // de los juegos, que nada mas consulta el puntero montado) no necesitan
    // linkear W3dStore.cpp
    virtual ~W3dStore() {}
    virtual bool Open(const std::string& path) = 0;
    virtual bool Exists(const std::string& entry) const = 0;
    virtual bool Read(const std::string& entry, std::vector<unsigned char>& out) = 0;
    // 'n' bytes desde 'desde' de una entrada (el streaming de mallas: solo sus bloques de render). Por defecto la
    // entrada entera y el recorte; el zip lo hace con un fseek
    virtual bool ReadRange(const std::string& entry, unsigned from, unsigned n, std::vector<unsigned char>& out);
    virtual void List(std::vector<std::string>& out) const = 0;
    virtual unsigned Size(const std::string& entry) const = 0;
    virtual void Close() = 0;
};

// ---- backend ZIP: el .w3d contenedor, leido por indice (no se extrae nada) ----
class StoreZip : public W3dStore {
public:
    StoreZip();
    virtual ~StoreZip();
    virtual bool Open(const std::string& path);
    virtual bool Exists(const std::string& entry) const;
    virtual bool Read(const std::string& entry, std::vector<unsigned char>& out);
    virtual bool ReadRange(const std::string& entry, unsigned from, unsigned n, std::vector<unsigned char>& out);
    virtual void List(std::vector<std::string>& out) const;
    virtual unsigned Size(const std::string& entry) const;
    virtual void Close();
    // acceso al lector para el guardado incremental (W3dZipWriter::CopyFrom)
    W3dZipReader& Reader() { return *zip; }
private:
    StoreZip(const StoreZip&);
    StoreZip& operator=(const StoreZip&);
    W3dZipReader* zip;   // por puntero: el header no conoce el tipo (ver arriba)
};

// ---- backend CARPETA: el MISMO layout, en carpetas reales ----
class StoreFolder : public W3dStore {
public:
    virtual bool Open(const std::string& folder);   // la raiz del layout
    virtual bool Exists(const std::string& entry) const;
    virtual bool Read(const std::string& entry, std::vector<unsigned char>& out);
    virtual void List(std::vector<std::string>& out) const;
    virtual unsigned Size(const std::string& entry) const;
    virtual void Close();
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
//  W3dStore.cpp, a proposito: asi los builds que compilan el filesystem del
//  Core pero NO el almacen (el .mmp de Symbian, los 3 juegos de ejemplo)
//  siguen linkeando sin arrastrar el zip.
// ---------------------------------------------------------------------------
void W3dStoreMount(W3dStore* a);
void W3dStoreUnmount();
W3dStore* W3dMountedStore();

// ---------------------------------------------------------------------------
//  VARIOS CONTENEDORES MONTADOS A LA VEZ: EL REGISTRO POR NOMBRE (las librerias)
//
//  El proyecto abierto es el almacen SIN nombre de arriba: sus entradas van peladas
//  ("textures/x.png"). Cada LIBRERIA externa (otro .w3d, de solo lectura, del que el
//  proyecto usa prefabs, escenas o recursos) se monta ADEMAS con su nombre, y sus
//  entradas se nombran con PREFIJO: "lib:<nombre>/textures/x.png". ReadFileBytes y
//  FileExists enrutan ese prefijo al almacen de esa libreria: dos librerias (y el
//  proyecto) no chocan aunque tengan entradas con el mismo nombre, y todo lo que se
//  carga tarde (texturas de la cola diferida, scripts, .w3db, .w3da) sigue
//  encontrando lo suyo sin tocar sus call sites.
//
//  SIN un almacen con ese nombre (el JUEGO COMPILADO: las entradas de cada libreria
//  viajan extraidas en "libs/<nombre>/<entrada>", al lado del project.json o en
//  el pak) la ruta se traduce a W3dLibPathPacked y se busca como cualquier
//  otro archivo del juego (pak, APK, carpeta del binario).
//
//  Igual que el montado: registrar NO transfiere la propiedad (el que registra
//  destruye, y quita el registro antes). Implementado en w3dFilesystem.cpp (mismo
//  motivo que las tres de arriba).
// ---------------------------------------------------------------------------
void W3dStoreRegister(const std::string& name, W3dStore* a);   // a NULL = lo quita
W3dStore* W3dStoreByName(const std::string& name);          // NULL = no hay
// "lib:<nombre>/<entrada>" -> su nombre y su entrada. false = no es una ruta de libreria
bool W3dSplitLibPath(const std::string& path, std::string* name, std::string* entry);
// donde vive una entrada de libreria en el JUEGO COMPILADO: "libs/<nombre>/<entrada>"
std::string W3dLibPathPacked(const std::string& name, const std::string& entry);
// se pidio "lib:<nombre>/..." y no hay almacen con ese nombre: el editor lo MONTA en ese momento (una libreria
// vinculada que todavia nadie uso: la textura de un material del proyecto que nombra una de ella). true = quedo
// registrado. NULL (el juego compilado, los builds sin librerias) = se va derecho a la copia empaquetada
extern bool (*W3dStoreRequestHook)(const std::string& name);

#endif // W3D_STORE_H
