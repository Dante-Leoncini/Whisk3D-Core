// ============================================================================
//  W3dAlmacen.cpp - ver W3dAlmacen.h. Dialecto C++03 (Symbian compila esto).
//
//  Los dos backends comparten el MISMO layout de nombres, asi que el editor no
//  sabe (ni tiene que saber) si el proyecto esta en un .w3d o en una carpeta.
// ============================================================================
#include "W3dAlmacen.h"
#include "W3dZip.h"          // el tipo completo del lector (el header solo lo declara)
#include "w3dFilesystem.h"
#include "w3dlog.h"
#include <stdio.h>

// ---------------------------------------------------------------------------
//  AlmacenZip
// ---------------------------------------------------------------------------
AlmacenZip::AlmacenZip() : zip(new W3dZipLector()) {}
AlmacenZip::~AlmacenZip() { delete zip; }

bool AlmacenZip::Abrir(const std::string& ruta) {
    if (!zip->Abrir(ruta)) {
        w3dLogfE("W3dAlmacen: no pude indexar el contenedor %s", ruta.c_str());
        return false;
    }
    return true;
}
bool AlmacenZip::Existe(const std::string& entrada) const { return zip->Existe(entrada); }
bool W3dAlmacen::LeerRango(const std::string& entrada, unsigned desde, unsigned n, std::vector<unsigned char>& out) {
    out.clear();
    std::vector<unsigned char> todo;
    if (!Leer(entrada, todo) || desde > todo.size() || todo.size() - desde < n) return false;
    out.assign(todo.begin() + desde, todo.begin() + desde + n);
    return true;
}
bool AlmacenZip::LeerRango(const std::string& entrada, unsigned desde, unsigned n, std::vector<unsigned char>& out) {
    return zip->LeerRango(entrada, desde, n, out);
}
bool AlmacenZip::Leer(const std::string& entrada, std::vector<unsigned char>& out) {
    return zip->Leer(entrada, out);
}
void AlmacenZip::Listar(std::vector<std::string>& out) const { zip->Listar(out); }
unsigned AlmacenZip::Tam(const std::string& entrada) const { return zip->Tam(entrada); }
void AlmacenZip::Cerrar() { zip->Cerrar(); }

// ---------------------------------------------------------------------------
//  AlmacenCarpeta
//
//  El nombre de entrada se valida con la MISMA funcion que el zip: sin eso,
//  pedir "../../.bashrc" o "/etc/passwd" a un almacen de carpeta saldria del
//  proyecto. Adentro del zip es imposible por construccion; aca no.
// ---------------------------------------------------------------------------
bool AlmacenCarpeta::Abrir(const std::string& carpeta) {
    if (carpeta.empty() || !w3dFileSystem::IsDir(carpeta)) {
        w3dLogfE("W3dAlmacen: '%s' no es una carpeta", carpeta.c_str());
        return false;
    }
    base = carpeta;
    while (base.size() > 1 && (base[base.size() - 1] == '/' || base[base.size() - 1] == '\\'))
        base.erase(base.size() - 1);
    return true;
}
void AlmacenCarpeta::Cerrar() { base.clear(); }

// ruta real de una entrada. Vacia = la entrada no es un nombre usable.
static std::string CarpetaRuta(const std::string& base, const std::string& entrada) {
    if (base.empty() || !W3dZipNombreSeguro(entrada)) return std::string();
    return base + "/" + entrada;
}

bool AlmacenCarpeta::Existe(const std::string& entrada) const {
    std::string r = CarpetaRuta(base, entrada);
    return !r.empty() && w3dFileSystem::FileExists(r);
}
// OJO: fopen DIRECTO, no ReadFileBytes. ReadFileBytes consulta al almacen
// montado ANTES de ir al disco, asi que si el montado es ESTE mismo la lectura
// se llamaria a si misma. El almacen de carpeta lee siempre del disco de verdad.
static bool CarpetaLeer(const std::string& r, std::vector<unsigned char>* out, unsigned* tam) {
    FILE* f = fopen(r.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }   // ftell miente con carpetas/pipes
    if (tam) *tam = (unsigned)len;
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

bool AlmacenCarpeta::Leer(const std::string& entrada, std::vector<unsigned char>& out) {
    std::string r = CarpetaRuta(base, entrada);
    if (r.empty()) return false;
    return CarpetaLeer(r, &out, 0);
}
unsigned AlmacenCarpeta::Tam(const std::string& entrada) const {
    std::string r = CarpetaRuta(base, entrada);
    unsigned t = 0;
    if (r.empty() || !CarpetaLeer(r, 0, &t)) return 0;
    return t;
}

// recorrido en ANCHO (nada de recursion: Symbian tiene 30 KB de pila)
void AlmacenCarpeta::Listar(std::vector<std::string>& out) const {
    if (base.empty()) return;
    std::vector<std::string> pendientes;
    pendientes.push_back(std::string());          // la raiz
    for (size_t i = 0; i < pendientes.size(); i++) {
        const std::string rel = pendientes[i];
        std::string dir = rel.empty() ? base : (base + "/" + rel);
        std::vector<w3dFileSystem::DirEntry> hijos;
        if (!w3dFileSystem::ListDir(dir, hijos)) continue;
        for (size_t k = 0; k < hijos.size(); k++) {
            if (hijos[k].name == "." || hijos[k].name == "..") continue;
            std::string sub = rel.empty() ? hijos[k].name : (rel + "/" + hijos[k].name);
            if (hijos[k].isDir) pendientes.push_back(sub);
            else                out.push_back(sub);
        }
        // tope de cortesia: un layout de proyecto no tiene 100.000 archivos, y
        // un symlink circular no puede colgar el editor
        if (pendientes.size() > 8192 || out.size() > 65535) break;
    }
}
