// ============================================================================
//  SHADERS ESCRITOS de los materiales: leer el archivo, partirlo en secciones y pedirle al backend que lo compile
//  (objects/ShaderEscrito.h). Lo que se le agrega adelante a cada seccion lo pone el backend (graphics/w3dGraphics.h)
// ============================================================================
#include "ShaderEscrito.h"
#include "Materials.h"
#include "w3dGraphics.h"
#include "w3dFilesystem.h"
#include "w3dlog.h"
#include "animation/Animation.h"   // CurrentFrame / AnimFPS: w3d_tiempo en el editor
#include "ShaderCPU.h"                // la seccion @vertice en la CPU (backends sin shaders)
#include "CameraBase.h"               // g_renderCamPos / g_renderCamRot: la rotacion de la vista (w3d_vistaAMundo)
#include <map>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/time.h>
#endif

bool g_shaderTiempoLineaDeTiempo = false;
float W3dShaderReloj() {
    if (g_shaderTiempoLineaDeTiempo) return (float)CurrentFrame / (float)(AnimFPS > 0 ? AnimFPS : 30);
#ifdef _WIN32
    static DWORD t0 = GetTickCount();
    return (float)(GetTickCount() - t0) * 0.001f;
#else
    struct timeval tv; gettimeofday(&tv, 0);
    static double t0 = (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
    return (float)((double)tv.tv_sec + (double)tv.tv_usec * 1e-6 - t0);
#endif
}

// una linea que es SOLO la marca (con espacios alrededor)
static bool EsMarca(const std::string& l, const char* marca) {
    size_t a = l.find_first_not_of(" \t\r");
    if (a == std::string::npos) return false;
    const size_t n = strlen(marca);
    if (l.compare(a, n, marca) != 0) return false;
    return l.find_first_not_of(" \t\r", a + n) == std::string::npos;
}
void W3dShaderPartir(const std::string& texto, std::string& vertice, std::string& fragmento) {
    vertice.clear(); fragmento.clear();
    std::string* dst = &fragmento;
    bool hayMarca = false;
    int renglon = 1;
    char linea[32];
    // (en GLSL ES 1.00 "#line N" hace que el renglon SIGUIENTE sea el N + 1)
    snprintf(linea, sizeof(linea), "#line 0\n");
    fragmento = linea;
    for (size_t i = 0; i <= texto.size();) {
        size_t f = texto.find('\n', i); if (f == std::string::npos) f = texto.size();
        const std::string l = texto.substr(i, f - i);
        if (EsMarca(l, "@vertice") || EsMarca(l, "@fragmento")) {
            // (la seccion arranca en el renglon siguiente a la marca; lo de antes de la primera marca se descarta)
            if (!hayMarca) fragmento.clear();
            hayMarca = true;
            dst = EsMarca(l, "@vertice") ? &vertice : &fragmento;
            snprintf(linea, sizeof(linea), "#line %d\n", renglon);
            *dst += linea;
        } else { *dst += l; *dst += '\n'; }
        renglon++;
        if (f >= texto.size()) break;
        i = f + 1;
    }
}

static bool LeerArchivo(Material* m, std::string& texto) {
    if (m->shaderArchivo.empty()) { m->shaderError = "No shader file"; return false; }
    bool ok = false;
    texto = w3dFileSystem::ReadTextFile(m->shaderArchivo, &ok);
    if (!ok) { m->shaderError = "Could not read " + m->shaderArchivo; return false; }
    return true;
}

bool W3dShaderMaterialCompilar(Material* m) {
    if (!m) return false;
    W3dShaderMaterialOlvidar(m);
    m->shaderEstado = -1;
    std::string texto;
    if (!LeerArchivo(m, texto)) return false;
    std::string vs, fs;
    W3dShaderPartir(texto, vs, fs);
    m->shaderPantalla = strstr(fs.c_str(), "w3d_Pantalla") != 0;
    m->shaderTiempo = strstr(texto.c_str(), "w3d_tiempo") != 0;
    // SIN SHADERS en este backend (GLES 1.1, GL fijo): el @vertice lo corre la CPU; el @fragmento no se usa (el
    // material se dibuja como siempre)
    if (!w3dEngine::ShadersEscritos()) {
        if (strstr(vs.c_str(), "Vertice")) {
            std::string e;
            W3dVertCPU* p = W3dVertCPUCompilar(vs, e);
            if (!p) {
                m->shaderError = "@vertice (CPU): " + e;
                w3dLogfE("[shader] %s (%s): %s", m->name.c_str(), m->shaderArchivo.c_str(), m->shaderError.c_str());
                return false;
            }
            m->shaderCPU = p;
        }
        m->shaderEstado = 1; m->shaderError.clear();
        w3dLogf("[shader] %s: %s (@vertice en la CPU: %s)", m->name.c_str(), m->shaderArchivo.c_str(), m->shaderCPU ? "si" : "no hay");
        return true;
    }
    char log[2048]; log[0] = 0;
    const int id = w3dEngine::ShaderEscritoCompilar(vs.c_str(), fs.c_str(), log, (int)sizeof(log));
    if (id <= 0) {
        m->shaderError = log[0] ? log : "The shader does not compile";
        // (en un renglon: los saltos del log del driver salen como letras raras en el panel)
        for (size_t k = 0; k < m->shaderError.size(); k++) if (m->shaderError[k] == '\n' || m->shaderError[k] == '\r') m->shaderError[k] = ' ';
        while (!m->shaderError.empty() && m->shaderError[m->shaderError.size() - 1] == ' ') m->shaderError.erase(m->shaderError.size() - 1);
        w3dLogfE("[shader] %s (%s): %s", m->name.c_str(), m->shaderArchivo.c_str(), m->shaderError.c_str());
        return false;
    }
    m->shaderId = id; m->shaderEstado = 1; m->shaderError.clear();
    w3dLogf("[shader] %s: %s compilado", m->name.c_str(), m->shaderArchivo.c_str());
    return true;
}

int W3dShaderMaterial(Material* m) {
    if (!m || !m->shaderOn || m->shaderArchivo.empty() || !w3dEngine::ShadersEscritos()) return 0;
    if (m->shaderEstado == 0) W3dShaderMaterialCompilar(m);   // (una sola vez: si no compila no se reintenta en cada cuadro)
    return m->shaderEstado == 1 ? m->shaderId : 0;
}

void W3dShaderMaterialOlvidar(Material* m) {
    if (!m) return;
    if (m->shaderId) w3dEngine::ShaderEscritoBorrar(m->shaderId);
    if (m->shaderCPU) W3dVertCPUBorrar((W3dVertCPU*)m->shaderCPU);
    m->shaderId = 0; m->shaderEstado = 0; m->shaderCPU = NULL;
}

const W3dVertCPU* W3dShaderVerticeCPU(Material* m) {
    if (!m || !m->shaderOn || m->shaderArchivo.empty() || w3dEngine::ShadersEscritos()) return NULL;
    if (m->shaderEstado == 0) W3dShaderMaterialCompilar(m);
    return m->shaderEstado == 1 ? (const W3dVertCPU*)m->shaderCPU : NULL;
}

// los shaders por ARCHIVO (sin material: el paso "Shader" de las rutinas)
namespace { struct ShaderArchivo { int id, estado; std::string error; ShaderArchivo() : id(0), estado(0) {} }; }
static std::map<std::string, ShaderArchivo> gArchivos;
int W3dShaderArchivo(const std::string& ruta, std::string* error) {
    if (ruta.empty() || !w3dEngine::ShadersEscritos()) return 0;
    ShaderArchivo& a = gArchivos[ruta];
    if (a.estado == 0) {
        a.estado = -1;
        bool ok = false;
        const std::string texto = w3dFileSystem::ReadTextFile(ruta, &ok);
        if (!ok) a.error = "Could not read " + ruta;
        else {
            std::string vs, fs; W3dShaderPartir(texto, vs, fs);
            char log[2048]; log[0] = 0;
            a.id = w3dEngine::ShaderEscritoCompilar(vs.c_str(), fs.c_str(), log, (int)sizeof(log));
            if (a.id > 0) { a.estado = 1; a.error.clear(); }
            else { a.error = log; w3dLogfE("[shader] %s: %s", ruta.c_str(), log); }
        }
    }
    if (error) *error = a.error;
    return a.estado == 1 ? a.id : 0;
}
void W3dShaderArchivoOlvidar(const std::string& ruta) {
    std::map<std::string, ShaderArchivo>::iterator it = gArchivos.find(ruta);
    if (it == gArchivos.end()) return;
    if (it->second.id) w3dEngine::ShaderEscritoBorrar(it->second.id);
    gArchivos.erase(it);
}
void W3dShaderParametros(float* p, float ior, float transmision, float rugosidad, float metalico) {
    for (int k = 0; k < w3dEngine::ShaderEscritoParams; k++) p[k] = 0.0f;
    p[0] = W3dShaderReloj(); p[1] = ior; p[2] = transmision; p[3] = rugosidad; p[4] = metalico;
    // la rotacion de la VISTA al MUNDO (w3d_vistaAMundo): la de la camara del render, transpuesta (por columnas)
    CameraBase cam; cam.pos = g_renderCamPos; cam.rot = g_renderCamRot;
    const Matrix4 V = cam.ViewMatrix();
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) p[8 + c * 3 + r] = V.m[r * 4 + c];
}

void W3dShadersOlvidarTodos() {
    for (size_t i = 0; i < Materials.size(); i++) if (Materials[i]) { Materials[i]->shaderId = 0; Materials[i]->shaderEstado = 0; }
    gArchivos.clear();   // (el contexto se perdio: los programas ya no existen)
}

bool W3dShaderHayAnimados() {
    if (g_shaderTiempoLineaDeTiempo) return false;   // (en el editor lo mueve el Play de la linea de tiempo)
    for (size_t i = 0; i < Materials.size(); i++) {
        const Material* m = Materials[i];
        if (m && m->shaderOn && m->shaderEstado == 1 && m->shaderTiempo) return true;
    }
    return false;
}

const char* W3dShaderPlantilla() {
    return
        "// Shader de Whisk3D. Se escriben solo las funciones: lo demas lo pone el motor.\n"
        "// Anda igual en GLES 2.0 (Android, web) y en Vulkan. Despues de editar: \"Compilar\" en el material.\n"
        "// Lo que hay para usar (ver el manual: editor/shaders):\n"
        "//   w3d_uv, w3d_Normal(), w3d_HaciaLuz(), w3d_HaciaCamara(), w3d_Luz(n)\n"
        "//   w3d_color (el color del material), w3d_Textura(uv), w3d_colorVertice\n"
        "//   w3d_Pantalla(p) / w3d_PuntoPantalla(): lo que ya esta dibujado detras\n"
        "//   w3d_tiempo (segundos), w3d_ior, w3d_transmision, w3d_rugosidad, w3d_metalico\n"
        "\n"
        "@vertice\n"
        "// opcional: mover los vertices (en el espacio del objeto)\n"
        "void Vertice(inout vec3 pos, inout vec3 normal) {\n"
        "}\n"
        "\n"
        "@fragmento\n"
        "vec4 Fragmento() {\n"
        "    vec4 base = w3d_color * w3d_Textura(w3d_uv);\n"
        "    vec3 luz = w3d_Luz(w3d_Normal());\n"
        "    return vec4(base.rgb * luz, base.a);\n"
        "}\n";
}
