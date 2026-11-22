// ============================================================================
//  W3dStore.cpp - ver W3dStore.h. Dialecto C++03 (Symbian compila esto).
//
//  Los dos backends comparten el MISMO layout de nombres, asi que el editor no
//  sabe (ni tiene que saber) si el proyecto esta en un .w3d o en una carpeta.
// ============================================================================
#include "W3dStore.h"
#include "W3dZip.h"          // el tipo completo del lector (el header solo lo declara)
#include "w3dFilesystem.h"
#include "w3dlog.h"
#include <stdio.h>

// ---------------------------------------------------------------------------
//  StoreZip
// ---------------------------------------------------------------------------
StoreZip::StoreZip() : zip(new W3dZipReader()) {}
StoreZip::~StoreZip() { delete zip; }

bool StoreZip::Open(const std::string& path) {
    if (!zip->Open(path)) {
        w3dLogfE("W3dAlmacen: no pude indexar el contenedor %s", path.c_str());
        return false;
    }
    return true;
}
bool StoreZip::Exists(const std::string& entry) const { return zip->Exists(entry); }
bool W3dStore::ReadRange(const std::string& entry, unsigned from, unsigned n, std::vector<unsigned char>& out) {
    out.clear();
    std::vector<unsigned char> all;
    if (!Read(entry, all) || from > all.size() || all.size() - from < n) return false;
    out.assign(all.begin() + from, all.begin() + from + n);
    return true;
}
bool StoreZip::ReadRange(const std::string& entry, unsigned from, unsigned n, std::vector<unsigned char>& out) {
    return zip->ReadRange(entry, from, n, out);
}
bool StoreZip::Read(const std::string& entry, std::vector<unsigned char>& out) {
    return zip->Read(entry, out);
}
void StoreZip::List(std::vector<std::string>& out) const { zip->List(out); }
unsigned StoreZip::Size(const std::string& entry) const { return zip->Size(entry); }
void StoreZip::Close() { zip->Close(); }

// ---------------------------------------------------------------------------
//  StoreFolder
//
//  El nombre de entrada se valida con la MISMA funcion que el zip: sin eso,
//  pedir "../../.bashrc" o "/etc/passwd" a un almacen de carpeta saldria del
//  proyecto. Adentro del zip es imposible por construccion; aca no.
// ---------------------------------------------------------------------------
bool StoreFolder::Open(const std::string& folder) {
    if (folder.empty() || !w3dFileSystem::IsDir(folder)) {
        w3dLogfE("W3dAlmacen: '%s' no es una carpeta", folder.c_str());
        return false;
    }
    base = folder;
    while (base.size() > 1 && (base[base.size() - 1] == '/' || base[base.size() - 1] == '\\'))
        base.erase(base.size() - 1);
    return true;
}
void StoreFolder::Close() { base.clear(); }

// ruta real de una entrada. Vacia = la entrada no es un nombre usable.
static std::string FolderPath(const std::string& base, const std::string& entry) {
    if (base.empty() || !W3dZipNameSafe(entry)) return std::string();
    return base + "/" + entry;
}

bool StoreFolder::Exists(const std::string& entry) const {
    std::string r = FolderPath(base, entry);
    return !r.empty() && w3dFileSystem::FileExists(r);
}
// OJO: fopen DIRECTO, no ReadFileBytes. ReadFileBytes consulta al almacen
// montado ANTES de ir al disco, asi que si el montado es ESTE mismo la lectura
// se llamaria a si misma. El almacen de carpeta lee siempre del disco de verdad.
static bool FolderRead(const std::string& r, std::vector<unsigned char>* out, unsigned* size) {
    FILE* f = fopen(r.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }   // ftell miente con carpetas/pipes
    if (size) *size = (unsigned)len;
    bool ok = true;
    if (out) {
        out->resize((size_t)len);
        size_t rd = len ? fread(&(*out)[0], 1, (size_t)len, f) : 0;
        ok = (rd == (size_t)len);          // lectura CORTA = fallo
        if (!ok) out->clear();
    }
    fclose(f);
    return ok;
}

bool StoreFolder::Read(const std::string& entry, std::vector<unsigned char>& out) {
    std::string r = FolderPath(base, entry);
    if (r.empty()) return false;
    return FolderRead(r, &out, 0);
}
unsigned StoreFolder::Size(const std::string& entry) const {
    std::string r = FolderPath(base, entry);
    unsigned t = 0;
    if (r.empty() || !FolderRead(r, 0, &t)) return 0;
    return t;
}

// recorrido en ANCHO (nada de recursion: Symbian tiene 30 KB de pila)
void StoreFolder::List(std::vector<std::string>& out) const {
    if (base.empty()) return;
    std::vector<std::string> pending;
    pending.push_back(std::string());          // la raiz
    for (size_t i = 0; i < pending.size(); i++) {
        const std::string rel = pending[i];
        std::string dir = rel.empty() ? base : (base + "/" + rel);
        std::vector<w3dFileSystem::DirEntry> children;
        if (!w3dFileSystem::ListDir(dir, children)) continue;
        for (size_t k = 0; k < children.size(); k++) {
            if (children[k].name == "." || children[k].name == "..") continue;
            std::string sub = rel.empty() ? children[k].name : (rel + "/" + children[k].name);
            if (children[k].isDir) pending.push_back(sub);
            else                out.push_back(sub);
        }
        // tope de cortesia: un layout de proyecto no tiene 100.000 archivos, y
        // un symlink circular no puede colgar el editor
        if (pending.size() > 8192 || out.size() > 65535) break;
    }
}
