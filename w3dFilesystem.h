#pragma once

#include <string>
#include <vector>

namespace w3dFileSystem {
    // Inicializa rutas globales
    void Init();

    // rutas cacheadas
    const std::string& GetExeDir();
    const std::string& GetResDir();

    // ------------------------------------------------------------------
    //  Lectura / escritura de archivos UNIFICADA (una sola API para todas
    //  las plataformas; el editor y el Core la usan en vez de fopen/ifstream
    //  sueltos). En Android, si la ruta es RELATIVA lee del APK (AAssetManager,
    //  NO SDL); si es ABSOLUTA o en otra plataforma, abre el archivo real.
    // ------------------------------------------------------------------

    // Android: la capa de plataforma pasa UNA vez el AAssetManager del NDK para
    // poder leer del APK. No-op en el resto de plataformas. void* = no exponer tipos Android.
    void SetAssetManager(void* assetManager);

    // Lee un archivo entero a memoria. Devuelve false si no se pudo abrir/leer.
    bool ReadFileBytes(const std::string& path, std::vector<unsigned char>& out);
    // Lee un archivo de texto entero. 'ok' (opcional) queda en false si fallo.
    std::string ReadTextFile(const std::string& path, bool* ok = 0);
    // Escribe/crea un archivo de texto (trunca). false si no se pudo.
    bool WriteTextFile(const std::string& path, const std::string& data);

    // Carpeta ESCRIBIBLE de datos del usuario (config/bookmarks). En Android el
    // GetResDir es el APK (solo lectura); la plataforma setea aca una ruta escribible
    // (SDL_GetPrefPath). Si no se setea, cae en GetResDir (PC portable).
    void SetUserDataDir(const std::string& dir);
    const std::string& GetUserDataDir();

    // salida por defecto (render/export) que setea la plataforma. En Android API>=30 (scoped storage) es el dir
    // externo PROPIO de la app (SDL_AndroidGetExternalStoragePath), unico escribible por fopen sin permisos.
    void SetDefaultOutputDir(const std::string& dir);

    // ------------------------------------------------------------------
    //  Navegacion de archivos (la usa el File browser compartido). El
    //  backend es por plataforma: PC/escritorio = std::filesystem; Symbian
    //  (cuando se porte) = RDir. Las rutas usan '/' como separador.
    // ------------------------------------------------------------------
    struct DirEntry {
        std::string name;
        bool isDir;
    };

    // lista 'path' en 'out' (carpetas primero, alfabetico). false si no se pudo.
    bool ListDir(const std::string& path, std::vector<DirEntry>& out);

    // carpeta del usuario (HOME / USERPROFILE) como punto de partida
    std::string GetHomeDir();

    // carpeta de SALIDA por defecto (donde caen render/export si el usuario no elige otra).
    // Android: /storage/emulated/0/Download (Descargas). Resto: GetHomeDir.
    std::string GetDefaultOutputDir();

    // true si existe un ARCHIVO en 'path' (para preguntar antes de sobrescribir render/export).
    bool FileExists(const std::string& path);

    // accesos rapidos del panel lateral (Home, Escritorio, Documentos, unidades)
    //esto lo voy a quitar y mover a Whisk3D editor... es que en realidad es algo del editor 3D. no hace falta un
    //bookmak en un juego por ejemplo
    struct Bookmark {
        std::string name;
        std::string path;
        bool user;   // true = lo agrego el usuario (con "+") -> se PERSISTE en bookmarks.txt
        Bookmark() : user(false) {}
    };

    void GetBookmarks(std::vector<Bookmark>& out); // auto (Home/drives) + los del usuario
    // guarda los bookmarks con user==true en disco (GetResDir()/bookmarks.txt). Lo llaman el
    // "+" y el "-" del explorador para que queden persistidos entre sesiones.
    void SaveUserBookmarks(const std::vector<Bookmark>& all);

    // helpers de ruta (no tocan disco)
    std::string ParentPath(const std::string& path); // sube un nivel
    std::string JoinPath(const std::string& dir, const std::string& name);
}
