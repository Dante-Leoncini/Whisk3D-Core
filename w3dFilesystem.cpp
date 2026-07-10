// ============================================================
// w3dFilesystem.cpp  (COMPARTIDO: Windows / Linux / Android / Symbian)
//
//  Navegacion de archivos del File browser. UN solo backend para los 4 OS:
//   - Windows  -> std::filesystem (C++17)
//   - Linux / Android / Symbian -> POSIX (opendir/readdir/stat)
//
//  Win32 en Windows, dirent/stat en todo lo demas. Symbian tiene la capa POSIX por Open C/PIPS
//  (libc.lib + \epoc32\include\stdapis, ya en el MMP). Rutas con '/' siempre.
// ============================================================

// IMPORTANTE (Symbian/Open C): los headers POSIX van ANTES que las cabeceras
// de STLport (<string>/<vector> via w3dFilesystem.h). Si <dirent.h> se parsea
// DESPUES de STLport, algun macro (p.ej. tamano de off_t) corre el offset de
// 'd_name' y readdir devuelve nombres BASURA. En Windows/Linux no afecta, pero lo ordenamos igual para todos.

#if !defined(_WIN32)
    #include <dirent.h>     // opendir / readdir / closedir
    #include <sys/stat.h>   // stat
    #if !defined(W3D_SYMBIAN)
        #include <unistd.h> // readlink (Linux/Android)
        #include <limits.h>
    #endif
#endif

#include "w3dFilesystem.h"

#include <string>
#include <vector>
#include <cstdlib>      // getenv
#include <cstdio>       // fopen (RutaExiste de archivos en Symbian)

#ifdef __ANDROID__
    #include <android/asset_manager.h> // leer recursos del APK (NO SDL)
#endif

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
    #include <filesystem>
    #include <system_error>
    #include <sstream>
#elif !defined(W3D_SYMBIAN)
    #include <sstream>     // Linux/Android: istringstream para XDG_DATA_DIRS
#endif

namespace w3dFileSystem {

    static std::string gExeDir;
    static std::string gResDir;

    // ========================================================
    //  helpers de ruta (solo strings: iguales en los 4 OS)
    // ========================================================

    // normaliza: '\' -> '/', saca barra final (salvo raiz "C:/" o "/")
    static std::string Normaliza(const std::string& in) {
        std::string s = in;
        for (size_t i = 0; i < s.size(); i++) if (s[i] == '\\') s[i] = '/';
        while (s.size() > 1 && s[s.size() - 1] == '/' &&
               !(s.size() == 3 && s[1] == ':')) {
            s.erase(s.size() - 1);
        }
        return s;
    }

    std::string JoinPath(const std::string& dir, const std::string& name) {
        if (dir.empty()) return name;
        std::string d = dir;
        if (d[d.size() - 1] != '/') d += '/';
        return Normaliza(d + name);
    }

    std::string ParentPath(const std::string& path) {
        std::string p = Normaliza(path);
        // ya en una raiz ("C:/" o "/"): sin padre
        if (p == "/" || (p.size() == 3 && p[1] == ':' && p[2] == '/')) return path;
        size_t pos = p.find_last_of('/');
        if (pos == std::string::npos) return path;
        if (pos == 0) return "/";                       // raiz unix
        std::string parent = p.substr(0, pos);
        if (parent.size() == 2 && parent[1] == ':') parent += '/'; // "C:" -> "C:/"
        return parent;
    }

#if !defined(_WIN32)
    // opendir robusto: en Symbian/PIPS la raiz de unidad y algunas subrutas
    // pueden necesitar variantes (sin barra final, o separador nativo '\').
    static DIR* OpenDirRobust(const std::string& path) {
        DIR* dir = opendir(path.c_str());
    #if defined(W3D_SYMBIAN)
        if (!dir && path.size() > 1 && path[path.size() - 1] == '/') {
            std::string p2 = path.substr(0, path.size() - 1);   // "C:"
            dir = opendir(p2.c_str());
        }
        if (!dir) {
            std::string back = path;                            // "C:\..."
            for (size_t i = 0; i < back.size(); i++) if (back[i] == '/') back[i] = '\\';
            dir = opendir(back.c_str());
        }
    #endif
        return dir;
    }
#endif

#if defined(_WIN32)
    // Construye un std::filesystem::path desde una ruta UTF-8 (las nuestras SIEMPRE lo son). Sin esto,
    // path(std::string) la interpreta con la ANSI code page del sistema -> los nombres con cirilico/acentos
    // (ej "арена_3") fallan y std::filesystem LANZA system_error -> abort() al navegar. u8path no lanza.
    static std::filesystem::path WPath(const std::string& p) { return std::filesystem::u8path(p); }
#endif

    // existe la ruta? (portable)
    static bool RutaExiste(const std::string& p) {
    #if defined(_WIN32)
        std::error_code ec; return std::filesystem::exists(WPath(p), ec);
    #elif defined(W3D_SYMBIAN)
        // stat es poco confiable en el N95; un dir abre con opendir y un archivo
        // existe si fopen lo abre. (RutaExiste se usa para drives -> son dirs.)
        DIR* d = OpenDirRobust(p);
        if (d) { closedir(d); return true; }
        FILE* f = fopen(p.c_str(), "rb");
        if (f) { fclose(f); return true; }
        return false;
    #else
        struct stat st; return stat(p.c_str(), &st) == 0;
    #endif
    }
    static bool EsCarpeta(const std::string& p) {
    #if defined(_WIN32)
        std::error_code ec; return std::filesystem::is_directory(WPath(p), ec);
    #elif defined(W3D_SYMBIAN)
        // En el N95 NO confiar en stat/st_mode: el struct stat del SDK tiene
        // layout dudoso (st_mode sale basura -> S_IFDIR fallaba y TODAS las
        // carpetas quedaban isDir=false). opendir es lo confiable: si abre,
        // es carpeta; si no, es archivo.
        DIR* d = OpenDirRobust(p);
        if (d) { closedir(d); return true; }
        return false;
    #else
        struct stat st;
        if (stat(p.c_str(), &st) == 0) return (st.st_mode & S_IFDIR) != 0;
        DIR* d = opendir(p.c_str());
        if (d) { closedir(d); return true; }
        return false;
    #endif
    }

    // ========================================================
    //  Directorio del ejecutable + carpeta res (solo escritorio lo usa)
    // ========================================================

    static std::string DetectExeDir() {
    #if defined(_WIN32)
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string path(buffer);
        size_t pos = path.find_last_of("\\/");
        if (pos != std::string::npos) path = path.substr(0, pos);
        return Normaliza(path);
    #elif defined(W3D_SYMBIAN)
        return std::string(); // Symbian arma el res a mano (no usa esto)
    #else
        char result[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
        std::string path;
        if (count != -1) {
            path = std::string(result, count);
            size_t pos = path.find_last_of('/');
            if (pos != std::string::npos) path = path.substr(0, pos);
        }
        return path;
    #endif
    }

    static std::string DetectResDir() {
    #if defined(__ANDROID__)
        return "res";
    #elif defined(W3D_SYMBIAN)
        return std::string(); // Symbian no usa GetResDir (skinDir va a mano)
    #else
        std::string exeDir = gExeDir;
        if (!exeDir.empty()) {
            std::string portable = exeDir + "/res";   // ./res al lado del exe
            if (RutaExiste(portable)) return portable;
        }
        #if !defined(_WIN32)
            // instalacion del sistema (Linux)
            const char* xdg = std::getenv("XDG_DATA_DIRS");
            std::string dirs = xdg ? xdg : "/usr/local/share:/usr/share";
            std::istringstream stream(dirs);
            std::string dir;
            while (std::getline(stream, dir, ':')) {
                std::string path = dir + "/Whisk3d/res";
                if (RutaExiste(path)) return path;
            }
            if (RutaExiste("/usr/share/Whisk3d/res")) return "/usr/share/Whisk3d/res";
        #endif
        return exeDir + "/res";
    #endif
    }

    void Init() {
        gExeDir = DetectExeDir();
        gResDir = DetectResDir();
    }
    const std::string& GetExeDir() { return gExeDir; }
    const std::string& GetResDir() { return gResDir; }

    // ========================================================
    //  Lectura / escritura UNIFICADA de archivos + AAssetManager (Android)
    // ========================================================
    #ifdef __ANDROID__
    static AAssetManager* gAssetMgr = 0;
    #endif
    void SetAssetManager(void* am) {
    #ifdef __ANDROID__
        gAssetMgr = (AAssetManager*)am;
    #else
        (void)am;
    #endif
    }

    static std::string gUserDataDir; // ruta escribible (config/bookmarks). La setea la plataforma.
    void SetUserDataDir(const std::string& dir) { gUserDataDir = dir; }
    const std::string& GetUserDataDir() { return gUserDataDir.empty() ? GetResDir() : gUserDataDir; }

    // salida por defecto (render/export) seteada por la PLATAFORMA. En Android API>=30 (scoped storage) NO se puede
    // escribir a Descargas por fopen -> la capa SDL (main.cpp) setea aca el dir EXTERNO PROPIO de la app
    // (SDL_AndroidGetExternalStoragePath), que no requiere permiso y anda en todo Android. El Core es SDL-free.
    static std::string gAppOutputDir;
    void SetDefaultOutputDir(const std::string& dir) { gAppOutputDir = dir; }

    bool ReadFileBytes(const std::string& path, std::vector<unsigned char>& out) {
        out.clear();
    #ifdef __ANDROID__
        // ruta RELATIVA (ej. "res/..") = asset del APK; ABSOLUTA ("/storage/..") = archivo real
        if (!path.empty() && path[0] != '/' && gAssetMgr) {
            AAsset* a = AAssetManager_open(gAssetMgr, path.c_str(), AASSET_MODE_BUFFER);
            if (!a) return false;
            off_t len = AAsset_getLength(a);
            const void* buf = AAsset_getBuffer(a);
            if (buf && len > 0) out.assign((const unsigned char*)buf, (const unsigned char*)buf + len);
            AAsset_close(a);
            return buf != 0;
        }
    #endif
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 0) {
            out.resize((size_t)len);
            size_t rd = fread(&out[0], 1, (size_t)len, f);
            out.resize(rd);
        }
        fclose(f);
        return true;
    }

    std::string ReadTextFile(const std::string& path, bool* ok) {
        std::vector<unsigned char> b;
        bool r = ReadFileBytes(path, b);
        if (ok) *ok = r;
        return r ? std::string(b.begin(), b.end()) : std::string();
    }

    bool WriteTextFile(const std::string& path, const std::string& data) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        return true;
    }

    // ========================================================
    //  orden: carpetas primero, alfabetico case-insensitive
    // ========================================================
    static bool Menor(const DirEntry& a, const DirEntry& b) {
        const std::string& x = a.name; const std::string& y = b.name;
        size_t n = (x.size() < y.size()) ? x.size() : y.size();
        for (size_t i = 0; i < n; i++) {
            char cx = x[i], cy = y[i];
            if (cx >= 'A' && cx <= 'Z') cx = (char)(cx + 32);
            if (cy >= 'A' && cy <= 'Z') cy = (char)(cy + 32);
            if (cx != cy) return cx < cy;
        }
        return x.size() < y.size();
    }
    static void Ordenar(std::vector<DirEntry>& v) {
        for (size_t i = 1; i < v.size(); i++) {      // insertion sort (listas chicas)
            DirEntry key = v[i];
            size_t j = i;
            while (j > 0 && Menor(key, v[j - 1])) { v[j] = v[j - 1]; j--; }
            v[j] = key;
        }
    }

    // ========================================================
    //  ListDir
    // ========================================================
    bool ListDir(const std::string& path, std::vector<DirEntry>& out) {
        out.clear();
        if (path.empty()) return false;

        std::vector<DirEntry> dirs, files;

    #if defined(_WIN32)
        std::error_code ec;
        if (!std::filesystem::is_directory(WPath(path), ec)) return false;
        std::filesystem::directory_iterator it(
            WPath(path), std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) return false;
        for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            // u8string() (UTF-8) en vez de generic_string(): este ULTIMO lanza system_error si el nombre
            // tiene caracteres fuera de la ANSI code page (cirilico/acentos) -> era el abort() al navegar.
            auto _u8 = it->path().filename().u8string(); // std::string (C++17) o std::u8string (C++20)
            std::string name(reinterpret_cast<const char*>(_u8.data()), _u8.size());
            if (name.empty() || name[0] == '.') continue;
            std::error_code ec2;
            DirEntry de; de.name = name; de.isDir = it->is_directory(ec2);
            (de.isDir ? dirs : files).push_back(de);
        }
    #else
        DIR* dir = OpenDirRobust(path);
        if (!dir) return false;
        struct dirent* e;
        while ((e = readdir(dir)) != NULL) {
    #if defined(W3D_SYMBIAN)
            // El <dirent.h> del SDK del N95 declara MAL d_name (apunta al
            // offset 8, que es un char* interno -> daba basura tipo "?bq").
            // El nombre real, inline y null-terminado, esta en el OFFSET 20
            // del struct. Verificado con volcado hex en el telefono:
            //   ino@0  namelen@4  ptr@8  ..  256@16  NOMBRE@20
            std::string name((const char*)e + 20);
    #else
            std::string name = e->d_name;
    #endif
            if (name.empty() || name[0] == '.') continue; // ocultos y . / ..
            DirEntry de; de.name = name; de.isDir = EsCarpeta(JoinPath(path, name));
            (de.isDir ? dirs : files).push_back(de);
        }
        closedir(dir);
    #endif

        Ordenar(dirs);
        Ordenar(files);
        out.reserve(dirs.size() + files.size());
        for (size_t i = 0; i < dirs.size(); i++)  out.push_back(dirs[i]);
        for (size_t i = 0; i < files.size(); i++) out.push_back(files[i]);
        return true;
    }

    // ========================================================
    //  GetHomeDir
    // ========================================================
    std::string GetHomeDir() {
    #if defined(_WIN32)
        const char* up = std::getenv("USERPROFILE");
        if (up && *up) return Normaliza(up);
        return "C:/";
    #elif defined(W3D_SYMBIAN)
        if (EsCarpeta("E:/")) return "E:/"; // tarjeta de memoria del N95
        return "C:/";
    #elif defined(__ANDROID__)
        // almacenamiento COMPARTIDO del usuario (Descargas, DCIM, etc.), NO el
        // sandbox de la app. El permiso de lectura lo pide la app; sin permiso
        // el listado sale vacio.
        if (EsCarpeta("/storage/emulated/0")) return "/storage/emulated/0";
        if (EsCarpeta("/sdcard")) return "/sdcard";
        return "/";
    #else
        const char* h = std::getenv("HOME");
        return (h && *h) ? Normaliza(h) : std::string("/");
    #endif
    }

    // carpeta de salida por defecto: en Android, Descargas (compartida, visible por USB);
    // en el resto, la carpeta del usuario. Symbian usa sus rutas fijas E:/whisk3d/* en el editor.
    std::string GetDefaultOutputDir() {
        // si la plataforma seteo una salida (Android API>=30: dir externo propio de la app), esa gana: es la unica
        // escribible por fopen bajo scoped storage. Sin esto, en Android 11+ el render/export fallaba en silencio.
        if (!gAppOutputDir.empty()) return gAppOutputDir;
    #if defined(__ANDROID__)
        if (EsCarpeta("/storage/emulated/0/Download")) return "/storage/emulated/0/Download";
        if (EsCarpeta("/sdcard/Download")) return "/sdcard/Download";
        return GetHomeDir();
    #else
        return GetHomeDir();
    #endif
    }

    // existe un ARCHIVO en 'path'? (fopen: file-specific, no matchea carpetas). Se usa para
    // preguntar antes de sobrescribir (render/export).
    bool FileExists(const std::string& path) {
        if (path.empty()) return false;
        FILE* f = fopen(path.c_str(), "rb");
        if (f) { fclose(f); return true; }
        return false;
    }

    // ========================================================
    //  GetBookmarks
    // ========================================================
    //esto NO tendria que estar en el CORE de whisk3D. se va a quitar! si sos programador.. no lo uses
    void GetBookmarks(std::vector<Bookmark>& out) {
        out.clear();
        Bookmark home; home.name = "Home"; home.path = GetHomeDir();
        out.push_back(home);

    #if defined(_WIN32)
        // Windows: todas las unidades disponibles (C:, D:, E: ...)
        for (char d = 'C'; d <= 'Z'; d++) {
            std::string raiz = std::string(1, d) + ":/";
            if (RutaExiste(raiz)) {
                Bookmark b; b.name = std::string(1, d) + ":"; b.path = raiz;
                out.push_back(b);
            }
        }
    #elif defined(W3D_SYMBIAN)
        // Symbian (N95): SOLO las unidades de usuario: C (telefono) y E
        // (tarjeta). D=RAM, Z=ROM y demas drives de sistema NO se muestran.
        // tengo que ver si en el n8 que tiene como 3 memorias esto afecta
        const char syms[] = { 'C', 'E', 0 };
        for (int i = 0; syms[i]; i++) {
            std::string raiz = std::string(1, syms[i]) + ":/";
            if (RutaExiste(raiz)) {
                Bookmark b; b.name = std::string(1, syms[i]) + ":"; b.path = raiz;
                out.push_back(b);
            }
        }
    #else
        // Linux / Android: carpetas tipicas del usuario + la raiz
        std::string h = GetHomeDir();
        const char* nombres[]   = { "Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos" };
        const char* etiquetas[] = { "Escritorio", "Documentos", "Descargas", "Imagenes", "Musica", "Videos" };
        for (int i = 0; i < 6; i++) {
            std::string p = JoinPath(h, nombres[i]);
            if (EsCarpeta(p)) { Bookmark b; b.name = etiquetas[i]; b.path = p; out.push_back(b); }
        }
        Bookmark br; br.name = "/"; br.path = "/"; out.push_back(br);
    #endif

        // === bookmarks del USUARIO (los del "+"), persistidos en GetResDir()/bookmarks.txt === (esto se va a quitar)
        {
            std::string bmFile = GetUserDataDir() + "/bookmarks.txt";
            FILE* f = fopen(bmFile.c_str(), "r");
            if (f) {
                char line[1024];
                while (fgets(line, sizeof(line), f)) {
                    std::string s(line);
                    while (!s.empty() && (s[s.size()-1] == '\n' || s[s.size()-1] == '\r')) s.erase(s.size()-1);
                    size_t t = s.find('\t');
                    if (s.empty() || t == std::string::npos) continue;
                    Bookmark b; b.name = s.substr(0, t); b.path = s.substr(t + 1); b.user = true;
                    out.push_back(b);
                }
                fclose(f);
            }
        }
    }

    // guarda en disco SOLO los bookmarks con user==true (Home/drives/carpetas se regeneran). (esto se va a quitar)
    void SaveUserBookmarks(const std::vector<Bookmark>& all) {
        std::string bmFile = GetUserDataDir() + "/bookmarks.txt";
        FILE* f = fopen(bmFile.c_str(), "w");
        if (!f) return;
        for (size_t i = 0; i < all.size(); i++) {
            if (!all[i].user) continue;
            fprintf(f, "%s\t%s\n", all[i].name.c_str(), all[i].path.c_str());
        }
        fclose(f);
    }
}
