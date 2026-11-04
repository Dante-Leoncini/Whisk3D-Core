// ============================================================================
//  EL INTERPRETE DE @vertice EN LA CPU (objects/ShaderCPU.h). Tres partes: las PALABRAS (el texto en tokens, con los
//  renglones del archivo), el ARBOL (un compilador de descenso recursivo que resuelve los nombres a lugares: en cada
//  vertice no se busca nada por nombre) y la EVALUACION (recorre el arbol con valores de 1 a 4 numeros).
// ============================================================================
#include "ShaderCPU.h"
#include <vector>
#include <map>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

namespace {

// ---------------------------------------------------------------------------
//  LOS VALORES: float, int y bool son un numero; vecN son N. (int y bool se guardan como float)
// ---------------------------------------------------------------------------
struct V { float c[4]; int n; };
static V Escalar(float f) { V v; v.c[0] = f; v.c[1] = v.c[2] = v.c[3] = 0.0f; v.n = 1; return v; }
static V Vacio(int n) { V v; v.c[0] = v.c[1] = v.c[2] = v.c[3] = 0.0f; v.n = n; return v; }
static float Comp(const V& v, int k) { return v.n == 1 ? v.c[0] : (k < v.n ? v.c[k] : 0.0f); }

// ---------------------------------------------------------------------------
//  LAS PALABRAS
// ---------------------------------------------------------------------------
enum { T_ID, T_NUM, T_OP, T_FIN };
struct Tok { int t; std::string s; float num; int linea; };

static bool EsLetra(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool EsDigito(char c) { return c >= '0' && c <= '9'; }

static bool Palabras(const std::string& src, std::vector<Tok>& out, std::string& err) {
    int linea = 1;
    size_t i = 0;
    const size_t n = src.size();
    bool inicioRenglon = true;
    while (i < n) {
        const char c = src[i];
        if (c == '\n') { linea++; i++; inicioRenglon = true; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '#' && inicioRenglon) {
            // "#line N": en GLSL ES el renglon SIGUIENTE es el N + 1 (ver W3dShaderPartir)
            size_t f = src.find('\n', i); if (f == std::string::npos) f = n;
            const std::string d = src.substr(i, f - i);
            if (d.compare(0, 5, "#line") == 0) { linea = atoi(d.c_str() + 5); i = f; continue; }
            char b[160]; snprintf(b, sizeof(b), "renglon %d: '%s' no se puede usar en la CPU", linea, d.c_str());
            err = b; return false;
        }
        inicioRenglon = false;
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) { if (src[i] == '\n') linea++; i++; }
            i += 2; continue;
        }
        Tok t; t.linea = linea; t.num = 0.0f;
        if (EsLetra(c)) {
            size_t j = i; while (j < n && (EsLetra(src[j]) || EsDigito(src[j]))) j++;
            t.t = T_ID; t.s = src.substr(i, j - i); i = j; out.push_back(t); continue;
        }
        if (EsDigito(c) || (c == '.' && i + 1 < n && EsDigito(src[i + 1]))) {
            const char* p = src.c_str() + i; char* fin = 0;
            t.num = (float)strtod(p, &fin);
            size_t j = i + (size_t)(fin - p);
            if (j < n && (src[j] == 'f' || src[j] == 'F')) j++;
            t.t = T_NUM; i = j; out.push_back(t); continue;
        }
        static const char* kOps2[] = { "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "&&", "||", "++", "--", 0 };
        bool hecho = false;
        for (int k = 0; kOps2[k] && !hecho; k++)
            if (src.compare(i, 2, kOps2[k]) == 0) { t.t = T_OP; t.s = kOps2[k]; i += 2; out.push_back(t); hecho = true; }
        if (hecho) continue;
        if (strchr("+-*/%=<>!?:(){}[],;.", c)) { t.t = T_OP; t.s = std::string(1, c); i++; out.push_back(t); continue; }
        char b[96]; snprintf(b, sizeof(b), "renglon %d: no se entiende '%c'", linea, c);
        err = b; return false;
    }
    Tok f; f.t = T_FIN; f.linea = linea; f.num = 0.0f; out.push_back(f);
    return true;
}

// ---------------------------------------------------------------------------
//  EL ARBOL
// ---------------------------------------------------------------------------
enum {
    // expresiones
    E_NUM, E_VAR, E_BIN, E_UN, E_ASIGN, E_TERN, E_LLAMADA, E_FUNC, E_CONSTR, E_SWIZ, E_INDICE, E_INCDEC,
    // sentencias
    S_BLOQUE, S_EXPR, S_DECL, S_IF, S_FOR, S_RET, S_BREAK, S_CONT, S_NADA
};
// operadores (E_BIN / E_UN / E_ASIGN)
enum { O_MAS, O_MENOS, O_POR, O_DIV, O_MOD, O_IG, O_DIST, O_MEN, O_MAY, O_MENIG, O_MAYIG, O_Y, O_O, O_NO, O_NADA };

struct Nodo {
    int t, op;
    int a, b, c, d;          // hijos (-1 = ninguno)
    float num;
    int var; bool global;    // E_VAR / S_DECL: el lugar
    int tam;                 // S_DECL / E_CONSTR: cuantos numeros
    int sw[4]; int nsw;      // E_SWIZ: los componentes
    int fn;                  // E_LLAMADA: la funcion del usuario; E_FUNC: la de GLSL
    std::vector<int> args;   // E_LLAMADA / E_FUNC / E_CONSTR / S_BLOQUE
    Nodo() : t(S_NADA), op(O_NADA), a(-1), b(-1), c(-1), d(-1), num(0), var(-1), global(false), tam(1), nsw(0), fn(-1) {}
};

struct Funcion {
    std::string nombre;
    int tamRet;                      // 0 = void
    std::vector<int> paramTam, paramModo, paramSlot;   // modo: 0 in, 1 out, 2 inout
    std::vector<int> slotTam;        // los lugares de la funcion (parametros y variables)
    int cuerpo;
};

// las funciones de GLSL que se entienden (el numero de argumentos)
struct Builtin { const char* nombre; int minArgs, maxArgs; };
static const Builtin kBuiltins[] = {
    { "sin", 1, 1 }, { "cos", 1, 1 }, { "tan", 1, 1 }, { "asin", 1, 1 }, { "acos", 1, 1 }, { "atan", 1, 2 },
    { "radians", 1, 1 }, { "degrees", 1, 1 }, { "pow", 2, 2 }, { "exp", 1, 1 }, { "log", 1, 1 }, { "exp2", 1, 1 },
    { "log2", 1, 1 }, { "sqrt", 1, 1 }, { "inversesqrt", 1, 1 }, { "abs", 1, 1 }, { "sign", 1, 1 }, { "floor", 1, 1 },
    { "ceil", 1, 1 }, { "fract", 1, 1 }, { "mod", 2, 2 }, { "min", 2, 2 }, { "max", 2, 2 }, { "clamp", 3, 3 },
    { "mix", 3, 3 }, { "step", 2, 2 }, { "smoothstep", 3, 3 }, { "length", 1, 1 }, { "distance", 2, 2 },
    { "dot", 2, 2 }, { "cross", 2, 2 }, { "normalize", 1, 1 }, { "reflect", 2, 2 }, { "refract", 3, 3 },
    { 0, 0, 0 }
};
static int BuscarBuiltin(const std::string& n) {
    for (int i = 0; kBuiltins[i].nombre; i++) if (n == kBuiltins[i].nombre) return i;
    return -1;
}
// el tamanio de un tipo (0 = void, -1 = no es un tipo)
static int TamTipo(const std::string& s) {
    if (s == "void") return 0;
    if (s == "float" || s == "int" || s == "bool") return 1;
    if (s == "vec2" || s == "ivec2" || s == "bvec2") return 2;
    if (s == "vec3" || s == "ivec3" || s == "bvec3") return 3;
    if (s == "vec4" || s == "ivec4" || s == "bvec4") return 4;
    return -1;
}
static bool EsPrecision(const std::string& s) { return s == "lowp" || s == "mediump" || s == "highp"; }

// las entradas de afuera: los primeros lugares GLOBALES
static const char* kEntradas[] = { "w3d_tiempo", "w3d_ior", "w3d_transmision", "w3d_rugosidad", "w3d_metalico", 0 };
static const int kNumEntradas = 5;

} // namespace

struct W3dVertCPU {
    std::vector<Nodo> nodos;
    std::vector<Funcion> funcs;
    std::vector<V> globales;     // con su valor inicial (las constantes ya evaluadas)
    int vertice;                 // la funcion Vertice
};

namespace {

// ---------------------------------------------------------------------------
//  EL COMPILADOR
// ---------------------------------------------------------------------------
struct Compilador {
    std::vector<Tok> tk; size_t p;
    W3dVertCPU* prog;
    std::string err;
    std::map<std::string, int> globalesNombre;
    std::vector<int> globalesTam;
    std::vector<std::map<std::string, int> > ambitos;   // los locales de la funcion que se esta compilando
    Funcion* fActual;

    const Tok& Ver(int k = 0) const { size_t i = p + (size_t)k; return tk[i < tk.size() ? i : tk.size() - 1]; }
    bool EsOp(const char* s, int k = 0) const { return Ver(k).t == T_OP && Ver(k).s == s; }
    bool EsId(const char* s, int k = 0) const { return Ver(k).t == T_ID && Ver(k).s == s; }
    bool Error(const std::string& m) {
        if (err.empty()) { char b[48]; snprintf(b, sizeof(b), "renglon %d: ", Ver().linea); err = std::string(b) + m; }
        return false;
    }
    bool Esperar(const char* s) {
        if (!EsOp(s)) return Error(std::string("falta '") + s + "'" + (Ver().t == T_FIN ? "" : " antes de '" + Ver().s + "'"));
        p++; return true;
    }
    int Nuevo(int t) { Nodo n; n.t = t; prog->nodos.push_back(n); return (int)prog->nodos.size() - 1; }
    Nodo& N(int i) { return prog->nodos[(size_t)i]; }
    void SaltarPrecision() { while (Ver().t == T_ID && EsPrecision(Ver().s)) p++; }

    // ---- el nombre de una variable -> su lugar ----
    bool Resolver(const std::string& nombre, int& var, bool& global) {
        for (int k = (int)ambitos.size() - 1; k >= 0; k--) {
            std::map<std::string, int>::iterator it = ambitos[(size_t)k].find(nombre);
            if (it != ambitos[(size_t)k].end()) { var = it->second; global = false; return true; }
        }
        std::map<std::string, int>::iterator g = globalesNombre.find(nombre);
        if (g != globalesNombre.end()) { var = g->second; global = true; return true; }
        return false;
    }
    int NuevoLocal(const std::string& nombre, int tam) {
        fActual->slotTam.push_back(tam);
        const int s = (int)fActual->slotTam.size() - 1;
        ambitos.back()[nombre] = s;
        return s;
    }
    int BuscarFuncion(const std::string& n) {
        for (size_t i = 0; i < prog->funcs.size(); i++) if (prog->funcs[i].nombre == n) return (int)i;
        return -1;
    }

    // ---- EXPRESIONES ----
    int Primaria() {
        const Tok t = Ver();
        if (t.t == T_NUM) { p++; int n = Nuevo(E_NUM); N(n).num = t.num; return n; }
        if (EsOp("(")) { p++; int e = Expr(); if (e < 0 || !Esperar(")")) return -1; return e; }
        if (t.t != T_ID) { Error(t.t == T_FIN ? "falta una expresion" : "no se esperaba '" + t.s + "'"); return -1; }
        p++;
        if (t.s == "true" || t.s == "false") { int n = Nuevo(E_NUM); N(n).num = (t.s == "true") ? 1.0f : 0.0f; return n; }
        if (EsOp("(")) {   // una llamada: un constructor, una funcion de GLSL o una propia
            p++;
            std::vector<int> args;
            if (!EsOp(")")) for (;;) {
                int a = Asignacion(); if (a < 0) return -1;
                args.push_back(a);
                if (EsOp(",")) { p++; continue; }
                break;
            }
            if (!Esperar(")")) return -1;
            const int tam = TamTipo(t.s);
            if (tam > 0) { int n = Nuevo(E_CONSTR); N(n).tam = tam; N(n).args = args; return n; }
            const int bi = BuscarBuiltin(t.s);
            if (bi >= 0) {
                if ((int)args.size() < kBuiltins[bi].minArgs || (int)args.size() > kBuiltins[bi].maxArgs) { Error("'" + t.s + "' con otra cantidad de argumentos"); return -1; }
                int n = Nuevo(E_FUNC); N(n).fn = bi; N(n).args = args; return n;
            }
            const int f = BuscarFuncion(t.s);
            if (f >= 0) {
                if (args.size() != prog->funcs[(size_t)f].paramTam.size()) { Error("'" + t.s + "' con otra cantidad de argumentos"); return -1; }
                int n = Nuevo(E_LLAMADA); N(n).fn = f; N(n).args = args; return n;
            }
            if (t.s == "texture2D" || t.s == "w3d_Textura" || t.s == "w3d_Pantalla") { Error("'" + t.s + "' no se puede usar en @vertice"); return -1; }
            if (t.s.compare(0, 3, "mat") == 0) { Error("las matrices no se pueden usar en @vertice en la CPU"); return -1; }
            Error("no existe la funcion '" + t.s + "'"); return -1;
        }
        int var = -1; bool global = false;
        if (!Resolver(t.s, var, global)) {
            if (t.s.compare(0, 4, "w3d_") == 0) Error("'" + t.s + "' no se puede usar en @vertice");
            else Error("no existe '" + t.s + "'");
            return -1;
        }
        int n = Nuevo(E_VAR); N(n).var = var; N(n).global = global; return n;
    }
    int Postfijo() {
        int e = Primaria(); if (e < 0) return -1;
        for (;;) {
            if (EsOp(".")) {
                p++;
                if (Ver().t != T_ID) { Error("falta el componente despues de '.'"); return -1; }
                const std::string s = Ver().s; p++;
                if (s.empty() || s.size() > 4) { Error("componente '" + s + "' no valido"); return -1; }
                int n = Nuevo(E_SWIZ); N(n).a = e; N(n).nsw = (int)s.size();
                for (size_t k = 0; k < s.size(); k++) {
                    const char* q = strchr("xyzw", s[k]); int idx = q ? (int)(q - "xyzw") : -1;
                    if (idx < 0) { q = strchr("rgba", s[k]); idx = q ? (int)(q - "rgba") : -1; }
                    if (idx < 0) { q = strchr("stpq", s[k]); idx = q ? (int)(q - "stpq") : -1; }
                    if (idx < 0) { Error("componente '" + s + "' no valido"); return -1; }
                    N(n).sw[k] = idx;
                }
                e = n; continue;
            }
            if (EsOp("[")) {
                p++; int i = Expr(); if (i < 0 || !Esperar("]")) return -1;
                int n = Nuevo(E_INDICE); N(n).a = e; N(n).b = i; e = n; continue;
            }
            if (EsOp("++") || EsOp("--")) {
                const bool mas = EsOp("++"); p++;
                int n = Nuevo(E_INCDEC); N(n).a = e; N(n).op = mas ? O_MAS : O_MENOS; N(n).num = 0.0f;   // num 0 = postfijo
                e = n; continue;
            }
            return e;
        }
    }
    int Unario() {
        if (EsOp("-") || EsOp("+") || EsOp("!")) {
            const std::string o = Ver().s; p++;
            int a = Unario(); if (a < 0) return -1;
            if (o == "+") return a;
            int n = Nuevo(E_UN); N(n).a = a; N(n).op = (o == "-") ? O_MENOS : O_NO; return n;
        }
        if (EsOp("++") || EsOp("--")) {
            const bool mas = EsOp("++"); p++;
            int a = Unario(); if (a < 0) return -1;
            int n = Nuevo(E_INCDEC); N(n).a = a; N(n).op = mas ? O_MAS : O_MENOS; N(n).num = 1.0f;   // prefijo
            return n;
        }
        return Postfijo();
    }
    int Binaria(int nivel) {
        // niveles: 0 ||, 1 &&, 2 == !=, 3 < > <= >=, 4 + -, 5 * / %
        if (nivel > 5) return Unario();
        int a = Binaria(nivel + 1); if (a < 0) return -1;
        for (;;) {
            int op = -1;
            const Tok& t = Ver();
            if (t.t == T_OP) {
                const std::string& s = t.s;
                if (nivel == 0 && s == "||") op = O_O;
                else if (nivel == 1 && s == "&&") op = O_Y;
                else if (nivel == 2 && s == "==") op = O_IG;
                else if (nivel == 2 && s == "!=") op = O_DIST;
                else if (nivel == 3 && s == "<") op = O_MEN;
                else if (nivel == 3 && s == ">") op = O_MAY;
                else if (nivel == 3 && s == "<=") op = O_MENIG;
                else if (nivel == 3 && s == ">=") op = O_MAYIG;
                else if (nivel == 4 && s == "+") op = O_MAS;
                else if (nivel == 4 && s == "-") op = O_MENOS;
                else if (nivel == 5 && s == "*") op = O_POR;
                else if (nivel == 5 && s == "/") op = O_DIV;
                else if (nivel == 5 && s == "%") op = O_MOD;
            }
            if (op < 0) return a;
            p++;
            int b = Binaria(nivel + 1); if (b < 0) return -1;
            int n = Nuevo(E_BIN); N(n).a = a; N(n).b = b; N(n).op = op; a = n;
        }
    }
    int Ternaria() {
        int c = Binaria(0); if (c < 0) return -1;
        if (!EsOp("?")) return c;
        p++;
        int a = Asignacion(); if (a < 0 || !Esperar(":")) return -1;
        int b = Asignacion(); if (b < 0) return -1;
        int n = Nuevo(E_TERN); N(n).a = c; N(n).b = a; N(n).c = b; return n;
    }
    bool EsLvalue(int e) {
        const Nodo& n = N(e);
        if (n.t == E_VAR) return true;
        if (n.t == E_SWIZ || n.t == E_INDICE) return EsLvalue(n.a);
        return false;
    }
    int Asignacion() {
        int a = Ternaria(); if (a < 0) return -1;
        int op = -1;
        if (EsOp("=")) op = O_NADA; else if (EsOp("+=")) op = O_MAS; else if (EsOp("-=")) op = O_MENOS;
        else if (EsOp("*=")) op = O_POR; else if (EsOp("/=")) op = O_DIV;
        if (op < 0) return a;
        if (!EsLvalue(a)) { Error("no se puede asignar a eso"); return -1; }
        p++;
        int b = Asignacion(); if (b < 0) return -1;
        int n = Nuevo(E_ASIGN); N(n).a = a; N(n).b = b; N(n).op = op; return n;
    }
    int Expr() {
        int a = Asignacion(); if (a < 0) return -1;
        while (EsOp(",")) {   // (la coma de un for: se evaluan las dos)
            p++; int b = Asignacion(); if (b < 0) return -1;
            int n = Nuevo(S_BLOQUE); N(n).args.push_back(a); N(n).args.push_back(b); a = n;
        }
        return a;
    }

    // ---- SENTENCIAS ----
    bool EsInicioTipo() {
        size_t k = 0;
        if (EsId("const", (int)k)) k++;
        while (Ver((int)k).t == T_ID && EsPrecision(Ver((int)k).s)) k++;
        return Ver((int)k).t == T_ID && TamTipo(Ver((int)k).s) > 0 && Ver((int)k + 1).t == T_ID;
    }
    int Declaracion() {
        if (EsId("const")) p++;
        SaltarPrecision();
        const int tam = TamTipo(Ver().s); p++;
        int bloque = Nuevo(S_BLOQUE);
        for (;;) {
            if (Ver().t != T_ID) { Error("falta el nombre de la variable"); return -1; }
            const std::string nombre = Ver().s; p++;
            if (EsOp("[")) { Error("los arreglos no se pueden usar en @vertice en la CPU"); return -1; }
            int init = -1;
            if (EsOp("=")) { p++; init = Asignacion(); if (init < 0) return -1; }
            int d = Nuevo(S_DECL); N(d).tam = tam; N(d).a = init;
            N(d).var = NuevoLocal(nombre, tam);   // (despues del valor: "float x = x;" lee la de afuera)
            N(bloque).args.push_back(d);
            if (EsOp(",")) { p++; continue; }
            break;
        }
        return bloque;
    }
    int Sentencia() {
        if (EsOp("{")) return Bloque();
        if (EsOp(";")) { p++; return Nuevo(S_NADA); }
        if (EsId("if")) {
            p++; if (!Esperar("(")) return -1;
            int c = Expr(); if (c < 0 || !Esperar(")")) return -1;
            int a = Sentencia(); if (a < 0) return -1;
            int b = -1;
            if (EsId("else")) { p++; b = Sentencia(); if (b < 0) return -1; }
            int n = Nuevo(S_IF); N(n).a = c; N(n).b = a; N(n).c = b; return n;
        }
        if (EsId("for")) {
            p++; if (!Esperar("(")) return -1;
            ambitos.push_back(std::map<std::string, int>());
            int ini = -1, cond = -1, paso = -1;
            if (EsInicioTipo()) { ini = Declaracion(); if (ini < 0) return -1; }
            else if (!EsOp(";")) { int e = Expr(); if (e < 0) return -1; ini = Nuevo(S_EXPR); N(ini).a = e; }
            if (!Esperar(";")) return -1;
            if (!EsOp(";")) { cond = Expr(); if (cond < 0) return -1; }
            if (!Esperar(";")) return -1;
            if (!EsOp(")")) { paso = Expr(); if (paso < 0) return -1; }
            if (!Esperar(")")) return -1;
            int cuerpo = Sentencia(); if (cuerpo < 0) return -1;
            ambitos.pop_back();
            int n = Nuevo(S_FOR); N(n).a = ini; N(n).b = cond; N(n).c = paso; N(n).d = cuerpo; return n;
        }
        if (EsId("while") || EsId("do")) { Error("'" + Ver().s + "' no se puede usar en @vertice en la CPU (usa for)"); return -1; }
        if (EsId("return")) {
            p++;
            int e = -1;
            if (!EsOp(";")) { e = Expr(); if (e < 0) return -1; }
            if (!Esperar(";")) return -1;
            int n = Nuevo(S_RET); N(n).a = e; return n;
        }
        if (EsId("break")) { p++; if (!Esperar(";")) return -1; return Nuevo(S_BREAK); }
        if (EsId("continue")) { p++; if (!Esperar(";")) return -1; return Nuevo(S_CONT); }
        if (EsId("discard")) { Error("'discard' no se puede usar en @vertice"); return -1; }
        if (EsInicioTipo()) { int d = Declaracion(); if (d < 0 || !Esperar(";")) return -1; return d; }
        int e = Expr(); if (e < 0 || !Esperar(";")) return -1;
        int n = Nuevo(S_EXPR); N(n).a = e; return n;
    }
    int Bloque() {
        if (!Esperar("{")) return -1;
        ambitos.push_back(std::map<std::string, int>());
        int b = Nuevo(S_BLOQUE);
        while (!EsOp("}")) {
            if (Ver().t == T_FIN) { Error("falta '}'"); return -1; }
            int s = Sentencia(); if (s < 0) return -1;
            N(b).args.push_back(s);
        }
        p++;
        ambitos.pop_back();
        return b;
    }

    // ---- LO DE AFUERA: constantes y funciones ----
    bool Programa() {
        while (Ver().t != T_FIN) {
            if (EsOp(";")) { p++; continue; }
            if (EsId("precision")) { while (!EsOp(";") && Ver().t != T_FIN) p++; p++; continue; }
            if (EsId("uniform") || EsId("attribute") || EsId("varying")) return Error("'" + Ver().s + "' no se declara: lo pone Whisk3D");
            const bool constante = EsId("const");
            if (constante) p++;
            SaltarPrecision();
            if (Ver().t != T_ID || TamTipo(Ver().s) < 0) return Error("se esperaba un tipo (float, vec3...) y vino '" + Ver().s + "'");
            const int tam = TamTipo(Ver().s); p++;
            if (Ver().t != T_ID) return Error("falta un nombre");
            const std::string nombre = Ver().s; p++;
            if (EsOp("(")) {   // una FUNCION
                p++;
                Funcion f; f.nombre = nombre; f.tamRet = tam; f.cuerpo = -1;
                prog->funcs.push_back(f);
                fActual = &prog->funcs.back();
                const int fi = (int)prog->funcs.size() - 1;
                ambitos.clear(); ambitos.push_back(std::map<std::string, int>());
                if (EsId("void") && EsOp(")", 1)) p++;
                if (!EsOp(")")) for (;;) {
                    int modo = 0;
                    if (EsId("in")) { p++; } else if (EsId("out")) { modo = 1; p++; } else if (EsId("inout")) { modo = 2; p++; }
                    if (EsId("const")) p++;
                    SaltarPrecision();
                    const int pt = TamTipo(Ver().s);
                    if (pt <= 0) return Error("tipo de parametro no valido: '" + Ver().s + "'");
                    p++;
                    if (Ver().t != T_ID) return Error("falta el nombre del parametro");
                    const std::string pn = Ver().s; p++;
                    prog->funcs[(size_t)fi].paramTam.push_back(pt);
                    prog->funcs[(size_t)fi].paramModo.push_back(modo);
                    prog->funcs[(size_t)fi].paramSlot.push_back(NuevoLocal(pn, pt));
                    if (EsOp(",")) { p++; continue; }
                    break;
                }
                if (!Esperar(")")) return false;
                if (EsOp(";")) { return Error("no hace falta declarar '" + nombre + "' antes: escribila completa arriba de donde se usa"); }
                const int cuerpo = Bloque(); if (cuerpo < 0) return false;
                prog->funcs[(size_t)fi].cuerpo = cuerpo;
                ambitos.clear();
                fActual = 0;
                continue;
            }
            // una CONSTANTE (o variable) global
            if (!constante) return Error("una variable afuera de una funcion tiene que ser 'const'");
            for (;;) {
                if (!Esperar("=")) return false;
                ambitos.clear();
                int init = Asignacion(); if (init < 0) return false;
                globalesTam.push_back(tam);
                const int slot = (int)prog->globales.size();
                prog->globales.push_back(Vacio(tam));
                globalesNombre[nombre] = slot;
                // (se evalua al terminar: el valor queda en prog->globales)
                int d = Nuevo(S_DECL); N(d).tam = tam; N(d).a = init; N(d).var = slot; N(d).global = true;
                inicGlobales.push_back(d);
                if (EsOp(",")) {
                    p++;
                    if (Ver().t != T_ID) return Error("falta un nombre");
                    // (el mismo tipo para la que sigue)
                    const std::string otro = Ver().s; p++;
                    (void)otro;
                    return Error("una constante por renglon, por favor");
                }
                break;
            }
            if (!Esperar(";")) return false;
        }
        return true;
    }
    std::vector<int> inicGlobales;
};

// ---------------------------------------------------------------------------
//  LA EVALUACION
// ---------------------------------------------------------------------------
enum { X_SIGUE, X_BREAK, X_CONT, X_RET };
struct Ejecutor {
    const W3dVertCPU* prog;
    std::vector<V>* globales;
    // los lugares de las funciones en curso, en UNA pila reservada una vez (una llamada por vertice no pide memoria:
    // en el N95 eso costaba mas que la cuenta). 'marco' = donde empiezan los de la funcion en curso; 'tope' = el primero libre
    std::vector<V>* pila;
    size_t marco, tope;
    V ret;
    int profundidad;
    bool fallo;

    V* Lugar(const Nodo& n) { return n.global ? &(*globales)[(size_t)n.var] : &(*pila)[marco + (size_t)n.var]; }
    V Ajustar(const V& v, int tam) {   // al tamanio del lugar (un escalar se repite)
        V r = Vacio(tam);
        for (int k = 0; k < tam; k++) r.c[k] = (v.n == 1) ? v.c[0] : (k < v.n ? v.c[k] : 0.0f);
        return r;
    }
    void Guardar(int e, const V& v) {
        const Nodo& n = prog->nodos[(size_t)e];
        if (n.t == E_VAR) { V* l = Lugar(n); *l = Ajustar(v, l->n > 0 ? l->n : v.n); return; }
        if (n.t == E_SWIZ) {
            V base = Eval(n.a);
            for (int k = 0; k < n.nsw; k++) base.c[n.sw[k]] = (v.n == 1) ? v.c[0] : (k < v.n ? v.c[k] : 0.0f);
            Guardar(n.a, base); return;
        }
        if (n.t == E_INDICE) {
            V base = Eval(n.a);
            const int i = (int)Eval(n.b).c[0];
            if (i >= 0 && i < 4) base.c[i] = v.c[0];
            Guardar(n.a, base);
        }
    }
    static V Bin(int op, const V& a, const V& b) {
        const int n = a.n > b.n ? a.n : b.n;
        V r = Vacio(n);
        switch (op) {
            case O_MAS:   for (int k = 0; k < n; k++) r.c[k] = Comp(a, k) + Comp(b, k); return r;
            case O_MENOS: for (int k = 0; k < n; k++) r.c[k] = Comp(a, k) - Comp(b, k); return r;
            case O_POR:   for (int k = 0; k < n; k++) r.c[k] = Comp(a, k) * Comp(b, k); return r;
            case O_DIV:   for (int k = 0; k < n; k++) { const float d = Comp(b, k); r.c[k] = d != 0.0f ? Comp(a, k) / d : 0.0f; } return r;
            case O_MOD:   for (int k = 0; k < n; k++) { const float d = Comp(b, k); r.c[k] = d != 0.0f ? Comp(a, k) - d * floorf(Comp(a, k) / d) : 0.0f; } return r;
            case O_IG: case O_DIST: {
                bool ig = true; for (int k = 0; k < n; k++) if (Comp(a, k) != Comp(b, k)) ig = false;
                return Escalar((op == O_IG) == ig ? 1.0f : 0.0f);
            }
            case O_MEN:   return Escalar(a.c[0] < b.c[0] ? 1.0f : 0.0f);
            case O_MAY:   return Escalar(a.c[0] > b.c[0] ? 1.0f : 0.0f);
            case O_MENIG: return Escalar(a.c[0] <= b.c[0] ? 1.0f : 0.0f);
            case O_MAYIG: return Escalar(a.c[0] >= b.c[0] ? 1.0f : 0.0f);
        }
        return r;
    }
    static float Largo(const V& v) { float s = 0; for (int k = 0; k < v.n; k++) s += v.c[k] * v.c[k]; return sqrtf(s); }
    static float Punto(const V& a, const V& b) { const int n = a.n < b.n ? a.n : b.n; float s = 0; for (int k = 0; k < n; k++) s += a.c[k] * b.c[k]; return s; }
    V Func(int fn, const std::vector<int>& args) {
        V a = Eval(args[0]);
        V b = args.size() > 1 ? Eval(args[1]) : Vacio(1);
        V c = args.size() > 2 ? Eval(args[2]) : Vacio(1);
        const std::string nombre = kBuiltins[fn].nombre;
        const int n = (a.n > b.n ? a.n : b.n) > c.n ? (a.n > b.n ? a.n : b.n) : c.n;
        V r = Vacio(a.n);
        // las de UN argumento, componente a componente
        #define W3D_UNO(nom, expr) if (nombre == nom) { for (int k = 0; k < a.n; k++) { const float x = a.c[k]; r.c[k] = (expr); } return r; }
        W3D_UNO("sin", sinf(x)) W3D_UNO("cos", cosf(x)) W3D_UNO("tan", tanf(x))
        W3D_UNO("asin", asinf(x < -1 ? -1 : (x > 1 ? 1 : x))) W3D_UNO("acos", acosf(x < -1 ? -1 : (x > 1 ? 1 : x)))
        W3D_UNO("radians", x * 0.017453292f) W3D_UNO("degrees", x * 57.29578f)
        W3D_UNO("exp", expf(x)) W3D_UNO("log", x > 0 ? logf(x) : 0.0f) W3D_UNO("exp2", powf(2.0f, x))
        W3D_UNO("log2", x > 0 ? logf(x) * 1.4426950f : 0.0f) W3D_UNO("sqrt", x > 0 ? sqrtf(x) : 0.0f)
        W3D_UNO("inversesqrt", x > 0 ? 1.0f / sqrtf(x) : 0.0f) W3D_UNO("abs", fabsf(x))
        W3D_UNO("sign", x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f)) W3D_UNO("floor", floorf(x)) W3D_UNO("ceil", ceilf(x))
        W3D_UNO("fract", x - floorf(x))
        #undef W3D_UNO
        if (nombre == "atan") {
            if (args.size() == 1) { for (int k = 0; k < a.n; k++) r.c[k] = atanf(a.c[k]); return r; }
            r = Vacio(n); for (int k = 0; k < n; k++) r.c[k] = atan2f(Comp(a, k), Comp(b, k)); return r;
        }
        if (nombre == "pow") { r = Vacio(n); for (int k = 0; k < n; k++) r.c[k] = powf(Comp(a, k), Comp(b, k)); return r; }
        if (nombre == "mod") return Bin(O_MOD, a, b);
        if (nombre == "min") { r = Vacio(n); for (int k = 0; k < n; k++) { const float x = Comp(a, k), y = Comp(b, k); r.c[k] = x < y ? x : y; } return r; }
        if (nombre == "max") { r = Vacio(n); for (int k = 0; k < n; k++) { const float x = Comp(a, k), y = Comp(b, k); r.c[k] = x > y ? x : y; } return r; }
        if (nombre == "clamp") { r = Vacio(n); for (int k = 0; k < n; k++) { float x = Comp(a, k); const float lo = Comp(b, k), hi = Comp(c, k); if (x < lo) x = lo; if (x > hi) x = hi; r.c[k] = x; } return r; }
        if (nombre == "mix") { r = Vacio(n); for (int k = 0; k < n; k++) { const float t = Comp(c, k); r.c[k] = Comp(a, k) * (1.0f - t) + Comp(b, k) * t; } return r; }
        if (nombre == "step") { r = Vacio(n); for (int k = 0; k < n; k++) r.c[k] = Comp(b, k) < Comp(a, k) ? 0.0f : 1.0f; return r; }
        if (nombre == "smoothstep") {
            r = Vacio(n);
            for (int k = 0; k < n; k++) {
                const float e0 = Comp(a, k), e1 = Comp(b, k);
                float t = e1 != e0 ? (Comp(c, k) - e0) / (e1 - e0) : 0.0f; if (t < 0) t = 0; if (t > 1) t = 1;
                r.c[k] = t * t * (3.0f - 2.0f * t);
            }
            return r;
        }
        if (nombre == "length") return Escalar(Largo(a));
        if (nombre == "distance") return Escalar(Largo(Bin(O_MENOS, a, b)));
        if (nombre == "dot") return Escalar(Punto(a, b));
        if (nombre == "cross") {
            r = Vacio(3);
            r.c[0] = a.c[1] * b.c[2] - a.c[2] * b.c[1]; r.c[1] = a.c[2] * b.c[0] - a.c[0] * b.c[2]; r.c[2] = a.c[0] * b.c[1] - a.c[1] * b.c[0];
            return r;
        }
        if (nombre == "normalize") { const float l = Largo(a); for (int k = 0; k < a.n; k++) r.c[k] = l > 0 ? a.c[k] / l : 0.0f; return r; }
        if (nombre == "reflect") {   // I - 2 dot(N, I) N
            const float d = 2.0f * Punto(b, a);
            for (int k = 0; k < a.n; k++) r.c[k] = a.c[k] - d * Comp(b, k); return r;
        }
        if (nombre == "refract") {
            const float eta = c.c[0], d = Punto(b, a), kk = 1.0f - eta * eta * (1.0f - d * d);
            if (kk < 0.0f) return Vacio(a.n);
            for (int k = 0; k < a.n; k++) r.c[k] = eta * a.c[k] - (eta * d + sqrtf(kk)) * Comp(b, k); return r;
        }
        return r;
    }
    V Eval(int e) {
        const Nodo& n = prog->nodos[(size_t)e];
        switch (n.t) {
            case E_NUM: return Escalar(n.num);
            case E_VAR: return *Lugar(n);
            case E_UN: {
                V a = Eval(n.a);
                if (n.op == O_NO) return Escalar(a.c[0] != 0.0f ? 0.0f : 1.0f);
                for (int k = 0; k < a.n; k++) a.c[k] = -a.c[k];
                return a;
            }
            case E_BIN: {
                if (n.op == O_Y) { if (Eval(n.a).c[0] == 0.0f) return Escalar(0.0f); return Escalar(Eval(n.b).c[0] != 0.0f ? 1.0f : 0.0f); }
                if (n.op == O_O) { if (Eval(n.a).c[0] != 0.0f) return Escalar(1.0f); return Escalar(Eval(n.b).c[0] != 0.0f ? 1.0f : 0.0f); }
                return Bin(n.op, Eval(n.a), Eval(n.b));
            }
            case E_TERN: return Eval(n.a).c[0] != 0.0f ? Eval(n.b) : Eval(n.c);
            case E_ASIGN: {
                V v = Eval(n.b);
                if (n.op != O_NADA) v = Bin(n.op, Eval(n.a), v);
                Guardar(n.a, v);
                return Eval(n.a);
            }
            case E_INCDEC: {
                const V antes = Eval(n.a);
                V despues = Bin(n.op, antes, Escalar(1.0f));
                Guardar(n.a, despues);
                return n.num != 0.0f ? despues : antes;
            }
            case E_SWIZ: {
                const V a = Eval(n.a);
                V r = Vacio(n.nsw);
                for (int k = 0; k < n.nsw; k++) r.c[k] = (a.n == 1) ? a.c[0] : a.c[n.sw[k]];
                return r;
            }
            case E_INDICE: {
                const V a = Eval(n.a);
                const int i = (int)Eval(n.b).c[0];
                return Escalar(i >= 0 && i < a.n ? a.c[i] : 0.0f);
            }
            case E_CONSTR: {
                V r = Vacio(n.tam);
                if (n.args.size() == 1) { const V a = Eval(n.args[0]); if (a.n == 1) { for (int k = 0; k < n.tam; k++) r.c[k] = a.c[0]; return r; } }
                int k = 0;
                for (size_t i = 0; i < n.args.size() && k < n.tam; i++) {
                    const V a = Eval(n.args[i]);
                    for (int j = 0; j < a.n && k < n.tam; j++) r.c[k++] = a.c[j];
                }
                return r;
            }
            case E_FUNC: return Func(n.fn, n.args);
            case E_LLAMADA: return Llamar(n);
            case S_BLOQUE: {   // (la coma de un for)
                V r = Vacio(1);
                for (size_t i = 0; i < n.args.size(); i++) r = Eval(n.args[i]);
                return r;
            }
        }
        return Vacio(1);
    }
    V Llamar(const Nodo& n) {
        const Funcion& f = prog->funcs[(size_t)n.fn];
        if (profundidad > 16) { fallo = true; return Vacio(f.tamRet > 0 ? f.tamRet : 1); }
        const size_t base = tope, nn = f.slotTam.size();
        if (pila->size() < base + nn) pila->resize((base + nn) * 2);
        tope = base + nn;   // (antes de los argumentos: una llamada adentro de un argumento usa lo de mas arriba)
        for (size_t i = 0; i < nn; i++) (*pila)[base + i] = Vacio(f.slotTam[i]);
        for (size_t i = 0; i < f.paramSlot.size(); i++)
            if (f.paramModo[i] != 1) { const V a = Ajustar(Eval(n.args[i]), f.paramTam[i]); (*pila)[base + (size_t)f.paramSlot[i]] = a; }
        const size_t antes = marco;
        marco = base;
        profundidad++;
        ret = Vacio(f.tamRet > 0 ? f.tamRet : 1);
        Exec(f.cuerpo);
        profundidad--;
        const V r = ret;
        marco = antes;
        for (size_t i = 0; i < f.paramSlot.size(); i++)
            if (f.paramModo[i] != 0) { const V v = (*pila)[base + (size_t)f.paramSlot[i]]; Guardar(n.args[i], v); }
        tope = base;
        return r;
    }
    int Exec(int s) {
        if (s < 0) return X_SIGUE;
        const Nodo& n = prog->nodos[(size_t)s];
        switch (n.t) {
            case S_NADA: return X_SIGUE;
            case S_EXPR: Eval(n.a); return X_SIGUE;
            case S_DECL: {
                V* l = n.global ? &(*globales)[(size_t)n.var] : &(*pila)[marco + (size_t)n.var];
                *l = n.a >= 0 ? Ajustar(Eval(n.a), n.tam) : Vacio(n.tam);
                return X_SIGUE;
            }
            case S_BLOQUE:
                for (size_t i = 0; i < n.args.size(); i++) { const int r = Exec(n.args[i]); if (r != X_SIGUE) return r; }
                return X_SIGUE;
            case S_IF: return Eval(n.a).c[0] != 0.0f ? Exec(n.b) : Exec(n.c);
            case S_FOR: {
                Exec(n.a);
                for (int vuelta = 0; vuelta < 4096; vuelta++) {   // (tope: un for que no termina no cuelga el editor)
                    if (n.b >= 0 && Eval(n.b).c[0] == 0.0f) break;
                    const int r = Exec(n.d);
                    if (r == X_BREAK) break;
                    if (r == X_RET) return r;
                    if (n.c >= 0) Eval(n.c);
                }
                return X_SIGUE;
            }
            case S_RET: if (n.a >= 0) ret = Eval(n.a); return X_RET;
            case S_BREAK: return X_BREAK;
            case S_CONT: return X_CONT;
        }
        Eval(s);
        return X_SIGUE;
    }
};

} // namespace

W3dVertCPU* W3dVertCPUCompilar(const std::string& texto, std::string& error) {
    error.clear();
    W3dVertCPU* prog = new W3dVertCPU();
    prog->vertice = -1;
    Compilador c;
    c.p = 0; c.prog = prog; c.fActual = 0;
    if (!Palabras(texto, c.tk, c.err)) { error = c.err; delete prog; return 0; }
    for (int i = 0; kEntradas[i]; i++) { c.globalesNombre[kEntradas[i]] = i; prog->globales.push_back(Vacio(1)); c.globalesTam.push_back(1); }
    if (!c.Programa()) { error = c.err; delete prog; return 0; }
    prog->vertice = c.BuscarFuncion("Vertice");
    if (prog->vertice < 0) { error = "falta la funcion void Vertice(inout vec3 pos, inout vec3 normal)"; delete prog; return 0; }
    const Funcion& f = prog->funcs[(size_t)prog->vertice];
    if (f.paramTam.size() != 2 || f.paramTam[0] != 3 || f.paramTam[1] != 3) {
        error = "Vertice tiene que ser: void Vertice(inout vec3 pos, inout vec3 normal)"; delete prog; return 0;
    }
    // las constantes de afuera, en orden
    std::vector<V> pila;
    Ejecutor x; x.prog = prog; x.globales = &prog->globales; x.pila = &pila; x.marco = 0; x.tope = 0; x.profundidad = 0; x.fallo = false;
    for (size_t i = 0; i < c.inicGlobales.size(); i++) x.Exec(c.inicGlobales[i]);
    return prog;
}

void W3dVertCPUBorrar(W3dVertCPU* p) { delete p; }

bool W3dVertCPUAplicar(const W3dVertCPU* p, const W3dVertCPUEntradas& e, const float* pos, const signed char* nor, int n,
                       float* posOut, signed char* norOut) {
    if (!p || p->vertice < 0 || n <= 0) return false;
    std::vector<V> globales = p->globales;
    globales[0] = Escalar(e.tiempo); globales[1] = Escalar(e.ior); globales[2] = Escalar(e.transmision);
    globales[3] = Escalar(e.rugosidad); globales[4] = Escalar(e.metalico);
    const Funcion& f = p->funcs[(size_t)p->vertice];
    const size_t nn = f.slotTam.size();
    std::vector<V> pila(nn + 64);
    Ejecutor x; x.prog = p; x.globales = &globales; x.pila = &pila; x.profundidad = 0; x.fallo = false;
    const int sPos = f.paramSlot[0], sNor = f.paramSlot[1];
    for (int i = 0; i < n; i++) {
        for (size_t k = 0; k < nn; k++) pila[k] = Vacio(f.slotTam[k]);
        V& vp = pila[(size_t)sPos]; vp.c[0] = pos[i * 3]; vp.c[1] = pos[i * 3 + 1]; vp.c[2] = pos[i * 3 + 2];
        V& vn = pila[(size_t)sNor];
        if (nor) { vn.c[0] = nor[i * 3] / 127.0f; vn.c[1] = nor[i * 3 + 1] / 127.0f; vn.c[2] = nor[i * 3 + 2] / 127.0f; }
        else { vn.c[0] = 0; vn.c[1] = 1; vn.c[2] = 0; }
        x.marco = 0; x.tope = nn;
        x.Exec(f.cuerpo);
        if (x.fallo) return false;
        const V& rp = pila[(size_t)sPos];
        posOut[i * 3] = rp.c[0]; posOut[i * 3 + 1] = rp.c[1]; posOut[i * 3 + 2] = rp.c[2];
        if (nor && norOut) {
            const V& rn = pila[(size_t)sNor];
            for (int k = 0; k < 3; k++) { float q = rn.c[k] * 127.0f; if (q > 127.0f) q = 127.0f; if (q < -127.0f) q = -127.0f; norOut[i * 3 + k] = (signed char)(q < 0 ? q - 0.5f : q + 0.5f); }
        }
    }
    return true;
}
