// ============================================================================
//  w3dpack — herramienta del AUTOR: empaqueta assets en un .w3dpack CIFRADO.
//
//  Uso:
//    w3dpack salida.w3dpack --key <64 hex>  nombre1=archivo1 [nombre2=archivo2 ...]
//    w3dpack salida.w3dpack --genkey          nombre1=archivo1 [ ...]   (genera + imprime la clave)
//
//  Ejemplo:
//    w3dpack assets.w3dpack --genkey  fondo.mp4=fondo.mp4  frente.webm=frente.webm
//    -> imprime la clave (32 bytes hex); pegala en tu app (embebida = deterrence,
//       o servila desde tu servidor por sesion = proteccion fuerte). Ver THREAT_MODEL.md.
//
//  Compila con el mismo cifrado del Core (io/W3dPack.cpp), asi el formato no se desincroniza.
// ============================================================================
#include "W3dPack.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>

using namespace w3dEngine;

static bool leerArchivo(const char* path, std::vector<unsigned char>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t rd = n ? fread(&out[0], 1, (size_t)n, f) : 0;
    fclose(f);
    return rd == (size_t)n;
}

static bool hexAKey(const char* hex, unsigned char key[32]) {
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        int hi, lo; char c;
        c = hex[i*2];   hi = (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1;
        c = hex[i*2+1]; lo = (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1;
        if (hi < 0 || lo < 0) return false;
        key[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "uso: w3dpack salida.w3dpack --key <64hex> | --genkey  nombre=archivo [...]\n");
        return 1;
    }
    const char* outPath = argv[1];
    unsigned char key[32];
    int firstAsset;

    if (strcmp(argv[2], "--genkey") == 0) {
        std::random_device rd; // fuente del SO (no determinista)
        for (int i = 0; i < 32; i++) key[i] = (unsigned char)(rd() & 0xff);
        printf("clave generada (guardala, la necesita tu app):\n  ");
        for (int i = 0; i < 32; i++) printf("%02x", key[i]);
        printf("\n");
        firstAsset = 3;
    } else if (strcmp(argv[2], "--key") == 0 && argc >= 5) {
        if (!hexAKey(argv[3], key)) { fprintf(stderr, "clave invalida (esperaba 64 hex)\n"); return 1; }
        firstAsset = 4;
    } else {
        fprintf(stderr, "falta --key <64hex> o --genkey\n");
        return 1;
    }

    // parsear pares nombre=archivo
    std::vector<std::string> names;
    std::vector<std::vector<unsigned char> > blobs;
    for (int i = firstAsset; i < argc; i++) {
        const char* eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "esperaba nombre=archivo: %s\n", argv[i]); return 1; }
        std::string name(argv[i], eq - argv[i]);
        std::vector<unsigned char> data;
        if (!leerArchivo(eq + 1, data)) { fprintf(stderr, "no pude leer %s\n", eq + 1); return 1; }
        names.push_back(name);
        blobs.push_back(data);
    }
    if (names.empty()) { fprintf(stderr, "no diste assets\n"); return 1; }

    // armar los arrays para W3dPackBuild
    int n = (int)names.size();
    std::vector<const char*> cnames(n);
    std::vector<const unsigned char*> cdatas(n);
    std::vector<size_t> clens(n);
    for (int i = 0; i < n; i++) {
        cnames[i] = names[i].c_str();
        cdatas[i] = blobs[i].empty() ? (const unsigned char*)"" : &blobs[i][0];
        clens[i]  = blobs[i].size();
    }

    if (!W3dPackBuild(outPath, &cnames[0], &cdatas[0], &clens[0], n, key)) {
        fprintf(stderr, "no pude escribir %s\n", outPath);
        return 1;
    }
    printf("ok: %s  (%d assets, cifrado ChaCha20)\n", outPath, n);
    return 0;
}
