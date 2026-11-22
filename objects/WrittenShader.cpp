// ============================================================================
//  SHADERS ESCRITOS de los materiales: leer el archivo, partirlo en secciones y pedirle al backend que lo compile
//  (objects/WrittenShader.h). Lo que se le agrega adelante a cada seccion lo pone el backend (graphics/w3dGraphics.h)
// ============================================================================
#include "WrittenShader.h"
#include "Materials.h"
#include "w3dGraphics.h"
#include "w3dFilesystem.h"
#include "w3dlog.h"
#include "animation/Animation.h"   // CurrentFrame / AnimFPS: w3d_time en el editor
#include "ShaderCPU.h"                // la seccion @vertex en la CPU (backends sin shaders)
#include "CameraBase.h"               // g_renderCamPos / g_renderCamRot: la rotacion de la vista (w3d_viewToWorld)
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

bool g_shaderTimeTimeLine = false;
float W3dShaderClock() {
    if (g_shaderTimeTimeLine) return (float)CurrentFrame / (float)(AnimFPS > 0 ? AnimFPS : 30);
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
static bool IsMark(const std::string& l, const char* mark) {
    size_t a = l.find_first_not_of(" \t\r");
    if (a == std::string::npos) return false;
    const size_t n = strlen(mark);
    if (l.compare(a, n, mark) != 0) return false;
    return l.find_first_not_of(" \t\r", a + n) == std::string::npos;
}
void W3dShaderSplit(const std::string& text, std::string& vertex, std::string& fragment) {
    vertex.clear(); fragment.clear();
    std::string* dst = &fragment;
    bool hasMark = false;
    int row = 1;
    char line[32];
    // (en GLSL ES 1.00 "#line N" hace que el renglon SIGUIENTE sea el N + 1)
    snprintf(line, sizeof(line), "#line 0\n");
    fragment = line;
    for (size_t i = 0; i <= text.size();) {
        size_t f = text.find('\n', i); if (f == std::string::npos) f = text.size();
        const std::string l = text.substr(i, f - i);
        if (IsMark(l, "@vertex") || IsMark(l, "@fragment")) {
            // (la seccion arranca en el renglon siguiente a la marca; lo de antes de la primera marca se descarta)
            if (!hasMark) fragment.clear();
            hasMark = true;
            dst = IsMark(l, "@vertex") ? &vertex : &fragment;
            snprintf(line, sizeof(line), "#line %d\n", row);
            *dst += line;
        } else { *dst += l; *dst += '\n'; }
        row++;
        if (f >= text.size()) break;
        i = f + 1;
    }
}

static bool ReadFile(Material* m, std::string& text) {
    if (m->shaderFile.empty()) { m->shaderError = "No shader file"; return false; }
    bool ok = false;
    text = w3dFileSystem::ReadTextFile(m->shaderFile, &ok);
    if (!ok) { m->shaderError = "Could not read " + m->shaderFile; return false; }
    return true;
}

bool W3dShaderMaterialCompile(Material* m) {
    if (!m) return false;
    W3dShaderMaterialForget(m);
    m->shaderState = -1;
    std::string text;
    if (!ReadFile(m, text)) return false;
    std::string vs, fs;
    W3dShaderSplit(text, vs, fs);
    m->shaderScreen = strstr(fs.c_str(), "w3d_Screen") != 0;
    m->shaderTime = strstr(text.c_str(), "w3d_time") != 0;
    // SIN SHADERS en este backend (GLES 1.1, GL fijo): el @vertex lo corre la CPU; el @fragment no se usa (el
    // material se dibuja como siempre)
    if (!w3dEngine::ShadersWritten()) {
        if (strstr(vs.c_str(), "Vertex")) {
            std::string e;
            W3dVertCPU* p = W3dVertCPUCompile(vs, e);
            if (!p) {
                m->shaderError = "@vertex (CPU): " + e;
                w3dLogfE("[shader] %s (%s): %s", m->name.c_str(), m->shaderFile.c_str(), m->shaderError.c_str());
                return false;
            }
            m->shaderCPU = p;
        }
        m->shaderState = 1; m->shaderError.clear();
        w3dLogf("[shader] %s: %s (@vertex en la CPU: %s)", m->name.c_str(), m->shaderFile.c_str(), m->shaderCPU ? "si" : "no hay");
        return true;
    }
    char log[2048]; log[0] = 0;
    const int id = w3dEngine::WrittenShaderCompile(vs.c_str(), fs.c_str(), log, (int)sizeof(log));
    if (id <= 0) {
        m->shaderError = log[0] ? log : "The shader does not compile";
        // (en un renglon: los saltos del log del driver salen como letras raras en el panel)
        for (size_t k = 0; k < m->shaderError.size(); k++) if (m->shaderError[k] == '\n' || m->shaderError[k] == '\r') m->shaderError[k] = ' ';
        while (!m->shaderError.empty() && m->shaderError[m->shaderError.size() - 1] == ' ') m->shaderError.erase(m->shaderError.size() - 1);
        w3dLogfE("[shader] %s (%s): %s", m->name.c_str(), m->shaderFile.c_str(), m->shaderError.c_str());
        return false;
    }
    m->shaderId = id; m->shaderState = 1; m->shaderError.clear();
    w3dLogf("[shader] %s: %s compilado", m->name.c_str(), m->shaderFile.c_str());
    return true;
}

int W3dShaderMaterial(Material* m) {
    if (!m || !m->shaderOn || m->shaderFile.empty() || !w3dEngine::ShadersWritten()) return 0;
    if (m->shaderState == 0) W3dShaderMaterialCompile(m);   // (una sola vez: si no compila no se reintenta en cada cuadro)
    return m->shaderState == 1 ? m->shaderId : 0;
}

void W3dShaderMaterialForget(Material* m) {
    if (!m) return;
    if (m->shaderId) w3dEngine::WrittenShaderDelete(m->shaderId);
    if (m->shaderCPU) W3dVertCPUDelete((W3dVertCPU*)m->shaderCPU);
    m->shaderId = 0; m->shaderState = 0; m->shaderCPU = NULL;
}

const W3dVertCPU* W3dShaderVertexCPU(Material* m) {
    if (!m || !m->shaderOn || m->shaderFile.empty() || w3dEngine::ShadersWritten()) return NULL;
    if (m->shaderState == 0) W3dShaderMaterialCompile(m);
    return m->shaderState == 1 ? (const W3dVertCPU*)m->shaderCPU : NULL;
}

// los shaders por ARCHIVO (sin material: el paso "Shader" de las rutinas)
namespace { struct ShaderFile { int id, state; std::string error; ShaderFile() : id(0), state(0) {} }; }
static std::map<std::string, ShaderFile> gFiles;
int W3dShaderFile(const std::string& path, std::string* error) {
    if (path.empty() || !w3dEngine::ShadersWritten()) return 0;
    ShaderFile& a = gFiles[path];
    if (a.state == 0) {
        a.state = -1;
        bool ok = false;
        const std::string text = w3dFileSystem::ReadTextFile(path, &ok);
        if (!ok) a.error = "Could not read " + path;
        else {
            std::string vs, fs; W3dShaderSplit(text, vs, fs);
            char log[2048]; log[0] = 0;
            a.id = w3dEngine::WrittenShaderCompile(vs.c_str(), fs.c_str(), log, (int)sizeof(log));
            if (a.id > 0) { a.state = 1; a.error.clear(); }
            else { a.error = log; w3dLogfE("[shader] %s: %s", path.c_str(), log); }
        }
    }
    if (error) *error = a.error;
    return a.state == 1 ? a.id : 0;
}
void W3dShaderFileForget(const std::string& path) {
    std::map<std::string, ShaderFile>::iterator it = gFiles.find(path);
    if (it == gFiles.end()) return;
    if (it->second.id) w3dEngine::WrittenShaderDelete(it->second.id);
    gFiles.erase(it);
}
void W3dShaderParameters(float* p, float ior, float transmission, float roughness, float metallic) {
    for (int k = 0; k < w3dEngine::ShaderWrittenParams; k++) p[k] = 0.0f;
    p[0] = W3dShaderClock(); p[1] = ior; p[2] = transmission; p[3] = roughness; p[4] = metallic;
    // la rotacion de la VISTA al MUNDO (w3d_viewToWorld): la de la camara del render, transpuesta (por columnas)
    CameraBase cam; cam.pos = g_renderCamPos; cam.rot = g_renderCamRot;
    const Matrix4 V = cam.ViewMatrix();
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) p[8 + c * 3 + r] = V.m[r * 4 + c];
}

void W3dShadersForgetAll() {
    for (size_t i = 0; i < Materials.size(); i++) if (Materials[i]) { Materials[i]->shaderId = 0; Materials[i]->shaderState = 0; }
    gFiles.clear();   // (el contexto se perdio: los programas ya no existen)
}

bool W3dShaderHasAnimated() {
    if (g_shaderTimeTimeLine) return false;   // (en el editor lo mueve el Play de la linea de tiempo)
    for (size_t i = 0; i < Materials.size(); i++) {
        const Material* m = Materials[i];
        if (m && m->shaderOn && m->shaderState == 1 && m->shaderTime) return true;
    }
    return false;
}

const char* W3dShaderTemplate() {
    return
        "// Shader de Whisk3D. Se escriben solo las funciones: lo demas lo pone el motor.\n"
        "// Anda igual en GLES 2.0 (Android, web) y en Vulkan. Despues de editar: \"Compilar\" en el material.\n"
        "// Lo que hay para usar (ver el manual: editor/shaders):\n"
        "//   w3d_uv, w3d_Normal(), w3d_ToLight(), w3d_ToCamera(), w3d_Light(n)\n"
        "//   w3d_color (el color del material), w3d_Texture(uv), w3d_colorVertice\n"
        "//   w3d_Screen(p) / w3d_ScreenPoint(): lo que ya esta dibujado detras\n"
        "//   w3d_time (segundos), w3d_ior, w3d_transmission, w3d_roughness, w3d_metallic\n"
        "\n"
        "@vertex\n"
        "// opcional: mover los vertices (en el espacio del objeto)\n"
        "void Vertex(inout vec3 pos, inout vec3 normal) {\n"
        "}\n"
        "\n"
        "@fragment\n"
        "vec4 Fragment() {\n"
        "    vec4 base = w3d_color * w3d_Texture(w3d_uv);\n"
        "    vec3 luz = w3d_Light(w3d_Normal());\n"
        "    return vec4(base.rgb * luz, base.a);\n"
        "}\n";
}
