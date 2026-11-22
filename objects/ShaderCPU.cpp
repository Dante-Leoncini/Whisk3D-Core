// ============================================================================
//  EL INTERPRETE DE @vertex EN LA CPU (objects/ShaderCPU.h). Tres partes: las PALABRAS (el texto en tokens, con los
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
static V Scale(float f) { V v; v.c[0] = f; v.c[1] = v.c[2] = v.c[3] = 0.0f; v.n = 1; return v; }
static V IsEmpty(int n) { V v; v.c[0] = v.c[1] = v.c[2] = v.c[3] = 0.0f; v.n = n; return v; }
static float Comp(const V& v, int k) { return v.n == 1 ? v.c[0] : (k < v.n ? v.c[k] : 0.0f); }

// ---------------------------------------------------------------------------
//  LAS PALABRAS
// ---------------------------------------------------------------------------
enum { T_ID, T_NUM, T_OP, T_END };
struct Tok { int t; std::string s; float num; int line; };

static bool IsLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

static bool Words(const std::string& src, std::vector<Tok>& out, std::string& err) {
    int line = 1;
    size_t i = 0;
    const size_t n = src.size();
    bool startRow = true;
    while (i < n) {
        const char c = src[i];
        if (c == '\n') { line++; i++; startRow = true; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '#' && startRow) {
            // "#line N": en GLSL ES el renglon SIGUIENTE es el N + 1 (ver W3dShaderSplit)
            size_t f = src.find('\n', i); if (f == std::string::npos) f = n;
            const std::string d = src.substr(i, f - i);
            if (d.compare(0, 5, "#line") == 0) { line = atoi(d.c_str() + 5); i = f; continue; }
            char b[160]; snprintf(b, sizeof(b), "renglon %d: '%s' no se puede usar en la CPU", line, d.c_str());
            err = b; return false;
        }
        startRow = false;
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) { if (src[i] == '\n') line++; i++; }
            i += 2; continue;
        }
        Tok t; t.line = line; t.num = 0.0f;
        if (IsLetter(c)) {
            size_t j = i; while (j < n && (IsLetter(src[j]) || IsDigit(src[j]))) j++;
            t.t = T_ID; t.s = src.substr(i, j - i); i = j; out.push_back(t); continue;
        }
        if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(src[i + 1]))) {
            const char* p = src.c_str() + i; char* end = 0;
            t.num = (float)strtod(p, &end);
            size_t j = i + (size_t)(end - p);
            if (j < n && (src[j] == 'f' || src[j] == 'F')) j++;
            t.t = T_NUM; i = j; out.push_back(t); continue;
        }
        static const char* kOps2[] = { "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "&&", "||", "++", "--", 0 };
        bool done = false;
        for (int k = 0; kOps2[k] && !done; k++)
            if (src.compare(i, 2, kOps2[k]) == 0) { t.t = T_OP; t.s = kOps2[k]; i += 2; out.push_back(t); done = true; }
        if (done) continue;
        if (strchr("+-*/%=<>!?:(){}[],;.", c)) { t.t = T_OP; t.s = std::string(1, c); i++; out.push_back(t); continue; }
        char b[96]; snprintf(b, sizeof(b), "renglon %d: no se entiende '%c'", line, c);
        err = b; return false;
    }
    Tok f; f.t = T_END; f.line = line; f.num = 0.0f; out.push_back(f);
    return true;
}

// ---------------------------------------------------------------------------
//  EL ARBOL
// ---------------------------------------------------------------------------
enum {
    // expresiones
    E_NUM, E_VAR, E_BIN, E_UN, E_ASIGN, E_TERN, E_CALL, E_FUNC, E_CONSTR, E_SWIZ, E_INDEX, E_INCDEC,
    // sentencias
    S_BLOCK, S_EXPR, S_DECL, S_IF, S_FOR, S_RET, S_BREAK, S_CONT, S_NOTHING
};
// operadores (E_BIN / E_UN / E_ASIGN)
enum { O_MORE, O_MINUS, O_BY, O_DIV, O_MOD, O_IG, O_DIST, O_MEN, O_MAY, O_MENIG, O_MAYIG, O_Y, O_O, O_NO, O_NOTHING };

struct Node {
    int t, op;
    int a, b, c, d;          // hijos (-1 = ninguno)
    float num;
    int var; bool global;    // E_VAR / S_DECL: el lugar
    int size;                // S_DECL / E_CONSTR: cuantos numeros
    int sw[4]; int nsw;      // E_SWIZ: los componentes
    int fn;                  // E_CALL: la funcion del usuario; E_FUNC: la de GLSL
    std::vector<int> args;   // E_CALL / E_FUNC / E_CONSTR / S_BLOCK
    Node() : t(S_NOTHING), op(O_NOTHING), a(-1), b(-1), c(-1), d(-1), num(0), var(-1), global(false), size(1), nsw(0), fn(-1) {}
};

struct Function {
    std::string name;
    int retSize;                      // 0 = void
    std::vector<int> paramSize, paramMode, paramSlot;   // modo: 0 in, 1 out, 2 inout
    std::vector<int> slotSize;        // los lugares de la funcion (parametros y variables)
    int body;
};

// las funciones de GLSL que se entienden (el numero de argumentos)
struct Builtin { const char* name; int minArgs, maxArgs; };
static const Builtin kBuiltins[] = {
    { "sin", 1, 1 }, { "cos", 1, 1 }, { "tan", 1, 1 }, { "asin", 1, 1 }, { "acos", 1, 1 }, { "atan", 1, 2 },
    { "radians", 1, 1 }, { "degrees", 1, 1 }, { "pow", 2, 2 }, { "exp", 1, 1 }, { "log", 1, 1 }, { "exp2", 1, 1 },
    { "log2", 1, 1 }, { "sqrt", 1, 1 }, { "inversesqrt", 1, 1 }, { "abs", 1, 1 }, { "sign", 1, 1 }, { "floor", 1, 1 },
    { "ceil", 1, 1 }, { "fract", 1, 1 }, { "mod", 2, 2 }, { "min", 2, 2 }, { "max", 2, 2 }, { "clamp", 3, 3 },
    { "mix", 3, 3 }, { "step", 2, 2 }, { "smoothstep", 3, 3 }, { "length", 1, 1 }, { "distance", 2, 2 },
    { "dot", 2, 2 }, { "cross", 2, 2 }, { "normalize", 1, 1 }, { "reflect", 2, 2 }, { "refract", 3, 3 },
    { 0, 0, 0 }
};
static int FindBuiltin(const std::string& n) {
    for (int i = 0; kBuiltins[i].name; i++) if (n == kBuiltins[i].name) return i;
    return -1;
}
// el tamanio de un tipo (0 = void, -1 = no es un tipo)
static int SizeType(const std::string& s) {
    if (s == "void") return 0;
    if (s == "float" || s == "int" || s == "bool") return 1;
    if (s == "vec2" || s == "ivec2" || s == "bvec2") return 2;
    if (s == "vec3" || s == "ivec3" || s == "bvec3") return 3;
    if (s == "vec4" || s == "ivec4" || s == "bvec4") return 4;
    return -1;
}
static bool IsPrecision(const std::string& s) { return s == "lowp" || s == "mediump" || s == "highp"; }

// las entradas de afuera: los primeros lugares GLOBALES
static const char* kEntries[] = { "w3d_time", "w3d_ior", "w3d_transmission", "w3d_roughness", "w3d_metallic", 0 };
static const int kNumEntries = 5;

} // namespace

struct W3dVertCPU {
    std::vector<Node> nodes;
    std::vector<Function> funcs;
    std::vector<V> global;     // con su valor inicial (las constantes ya evaluadas)
    int vertex;                // la funcion Vertice
};

namespace {

// ---------------------------------------------------------------------------
//  EL COMPILADOR
// ---------------------------------------------------------------------------
struct Compiler {
    std::vector<Tok> tk; size_t p;
    W3dVertCPU* prog;
    std::string err;
    std::map<std::string, int> globalName;
    std::vector<int> globalSize;
    std::vector<std::map<std::string, int> > scopes;   // los locales de la funcion que se esta compilando
    Function* fCurrent;

    const Tok& Show(int k = 0) const { size_t i = p + (size_t)k; return tk[i < tk.size() ? i : tk.size() - 1]; }
    bool IsOp(const char* s, int k = 0) const { return Show(k).t == T_OP && Show(k).s == s; }
    bool IsId(const char* s, int k = 0) const { return Show(k).t == T_ID && Show(k).s == s; }
    bool Error(const std::string& m) {
        if (err.empty()) { char b[48]; snprintf(b, sizeof(b), "renglon %d: ", Show().line); err = std::string(b) + m; }
        return false;
    }
    bool Wait(const char* s) {
        if (!IsOp(s)) return Error(std::string("falta '") + s + "'" + (Show().t == T_END ? "" : " antes de '" + Show().s + "'"));
        p++; return true;
    }
    int New(int t) { Node n; n.t = t; prog->nodes.push_back(n); return (int)prog->nodes.size() - 1; }
    Node& N(int i) { return prog->nodes[(size_t)i]; }
    void SkipPrecision() { while (Show().t == T_ID && IsPrecision(Show().s)) p++; }

    // ---- el nombre de una variable -> su lugar ----
    bool Resolve(const std::string& name, int& var, bool& global) {
        for (int k = (int)scopes.size() - 1; k >= 0; k--) {
            std::map<std::string, int>::iterator it = scopes[(size_t)k].find(name);
            if (it != scopes[(size_t)k].end()) { var = it->second; global = false; return true; }
        }
        std::map<std::string, int>::iterator g = globalName.find(name);
        if (g != globalName.end()) { var = g->second; global = true; return true; }
        return false;
    }
    int NewLocal(const std::string& name, int size) {
        fCurrent->slotSize.push_back(size);
        const int s = (int)fCurrent->slotSize.size() - 1;
        scopes.back()[name] = s;
        return s;
    }
    int FindFunction(const std::string& n) {
        for (size_t i = 0; i < prog->funcs.size(); i++) if (prog->funcs[i].name == n) return (int)i;
        return -1;
    }

    // ---- EXPRESIONES ----
    int Primary() {
        const Tok t = Show();
        if (t.t == T_NUM) { p++; int n = New(E_NUM); N(n).num = t.num; return n; }
        if (IsOp("(")) { p++; int e = Expr(); if (e < 0 || !Wait(")")) return -1; return e; }
        if (t.t != T_ID) { Error(t.t == T_END ? "falta una expresion" : "no se esperaba '" + t.s + "'"); return -1; }
        p++;
        if (t.s == "true" || t.s == "false") { int n = New(E_NUM); N(n).num = (t.s == "true") ? 1.0f : 0.0f; return n; }
        if (IsOp("(")) {   // una llamada: un constructor, una funcion de GLSL o una propia
            p++;
            std::vector<int> args;
            if (!IsOp(")")) for (;;) {
                int a = Assignment(); if (a < 0) return -1;
                args.push_back(a);
                if (IsOp(",")) { p++; continue; }
                break;
            }
            if (!Wait(")")) return -1;
            const int size = SizeType(t.s);
            if (size > 0) { int n = New(E_CONSTR); N(n).size = size; N(n).args = args; return n; }
            const int bi = FindBuiltin(t.s);
            if (bi >= 0) {
                if ((int)args.size() < kBuiltins[bi].minArgs || (int)args.size() > kBuiltins[bi].maxArgs) { Error("'" + t.s + "' con otra cantidad de argumentos"); return -1; }
                int n = New(E_FUNC); N(n).fn = bi; N(n).args = args; return n;
            }
            const int f = FindFunction(t.s);
            if (f >= 0) {
                if (args.size() != prog->funcs[(size_t)f].paramSize.size()) { Error("'" + t.s + "' con otra cantidad de argumentos"); return -1; }
                int n = New(E_CALL); N(n).fn = f; N(n).args = args; return n;
            }
            if (t.s == "texture2D" || t.s == "w3d_Texture" || t.s == "w3d_Screen") { Error("'" + t.s + "' no se puede usar en @vertex"); return -1; }
            if (t.s.compare(0, 3, "mat") == 0) { Error("las matrices no se pueden usar en @vertex en la CPU"); return -1; }
            Error("no existe la funcion '" + t.s + "'"); return -1;
        }
        int var = -1; bool global = false;
        if (!Resolve(t.s, var, global)) {
            if (t.s.compare(0, 4, "w3d_") == 0) Error("'" + t.s + "' no se puede usar en @vertex");
            else Error("no existe '" + t.s + "'");
            return -1;
        }
        int n = New(E_VAR); N(n).var = var; N(n).global = global; return n;
    }
    int Postfijo() {
        int e = Primary(); if (e < 0) return -1;
        for (;;) {
            if (IsOp(".")) {
                p++;
                if (Show().t != T_ID) { Error("falta el componente despues de '.'"); return -1; }
                const std::string s = Show().s; p++;
                if (s.empty() || s.size() > 4) { Error("componente '" + s + "' no valido"); return -1; }
                int n = New(E_SWIZ); N(n).a = e; N(n).nsw = (int)s.size();
                for (size_t k = 0; k < s.size(); k++) {
                    const char* q = strchr("xyzw", s[k]); int idx = q ? (int)(q - "xyzw") : -1;
                    if (idx < 0) { q = strchr("rgba", s[k]); idx = q ? (int)(q - "rgba") : -1; }
                    if (idx < 0) { q = strchr("stpq", s[k]); idx = q ? (int)(q - "stpq") : -1; }
                    if (idx < 0) { Error("componente '" + s + "' no valido"); return -1; }
                    N(n).sw[k] = idx;
                }
                e = n; continue;
            }
            if (IsOp("[")) {
                p++; int i = Expr(); if (i < 0 || !Wait("]")) return -1;
                int n = New(E_INDEX); N(n).a = e; N(n).b = i; e = n; continue;
            }
            if (IsOp("++") || IsOp("--")) {
                const bool more = IsOp("++"); p++;
                int n = New(E_INCDEC); N(n).a = e; N(n).op = more ? O_MORE : O_MINUS; N(n).num = 0.0f;   // num 0 = postfijo
                e = n; continue;
            }
            return e;
        }
    }
    int Unary() {
        if (IsOp("-") || IsOp("+") || IsOp("!")) {
            const std::string o = Show().s; p++;
            int a = Unary(); if (a < 0) return -1;
            if (o == "+") return a;
            int n = New(E_UN); N(n).a = a; N(n).op = (o == "-") ? O_MINUS : O_NO; return n;
        }
        if (IsOp("++") || IsOp("--")) {
            const bool more = IsOp("++"); p++;
            int a = Unary(); if (a < 0) return -1;
            int n = New(E_INCDEC); N(n).a = a; N(n).op = more ? O_MORE : O_MINUS; N(n).num = 1.0f;   // prefijo
            return n;
        }
        return Postfijo();
    }
    int Binary(int level) {
        // niveles: 0 ||, 1 &&, 2 == !=, 3 < > <= >=, 4 + -, 5 * / %
        if (level > 5) return Unary();
        int a = Binary(level + 1); if (a < 0) return -1;
        for (;;) {
            int op = -1;
            const Tok& t = Show();
            if (t.t == T_OP) {
                const std::string& s = t.s;
                if (level == 0 && s == "||") op = O_O;
                else if (level == 1 && s == "&&") op = O_Y;
                else if (level == 2 && s == "==") op = O_IG;
                else if (level == 2 && s == "!=") op = O_DIST;
                else if (level == 3 && s == "<") op = O_MEN;
                else if (level == 3 && s == ">") op = O_MAY;
                else if (level == 3 && s == "<=") op = O_MENIG;
                else if (level == 3 && s == ">=") op = O_MAYIG;
                else if (level == 4 && s == "+") op = O_MORE;
                else if (level == 4 && s == "-") op = O_MINUS;
                else if (level == 5 && s == "*") op = O_BY;
                else if (level == 5 && s == "/") op = O_DIV;
                else if (level == 5 && s == "%") op = O_MOD;
            }
            if (op < 0) return a;
            p++;
            int b = Binary(level + 1); if (b < 0) return -1;
            int n = New(E_BIN); N(n).a = a; N(n).b = b; N(n).op = op; a = n;
        }
    }
    int Ternary() {
        int c = Binary(0); if (c < 0) return -1;
        if (!IsOp("?")) return c;
        p++;
        int a = Assignment(); if (a < 0 || !Wait(":")) return -1;
        int b = Assignment(); if (b < 0) return -1;
        int n = New(E_TERN); N(n).a = c; N(n).b = a; N(n).c = b; return n;
    }
    bool IsLvalue(int e) {
        const Node& n = N(e);
        if (n.t == E_VAR) return true;
        if (n.t == E_SWIZ || n.t == E_INDEX) return IsLvalue(n.a);
        return false;
    }
    int Assignment() {
        int a = Ternary(); if (a < 0) return -1;
        int op = -1;
        if (IsOp("=")) op = O_NOTHING; else if (IsOp("+=")) op = O_MORE; else if (IsOp("-=")) op = O_MINUS;
        else if (IsOp("*=")) op = O_BY; else if (IsOp("/=")) op = O_DIV;
        if (op < 0) return a;
        if (!IsLvalue(a)) { Error("no se puede asignar a eso"); return -1; }
        p++;
        int b = Assignment(); if (b < 0) return -1;
        int n = New(E_ASIGN); N(n).a = a; N(n).b = b; N(n).op = op; return n;
    }
    int Expr() {
        int a = Assignment(); if (a < 0) return -1;
        while (IsOp(",")) {   // (la coma de un for: se evaluan las dos)
            p++; int b = Assignment(); if (b < 0) return -1;
            int n = New(S_BLOCK); N(n).args.push_back(a); N(n).args.push_back(b); a = n;
        }
        return a;
    }

    // ---- SENTENCIAS ----
    bool IsStartType() {
        size_t k = 0;
        if (IsId("const", (int)k)) k++;
        while (Show((int)k).t == T_ID && IsPrecision(Show((int)k).s)) k++;
        return Show((int)k).t == T_ID && SizeType(Show((int)k).s) > 0 && Show((int)k + 1).t == T_ID;
    }
    int Declaration() {
        if (IsId("const")) p++;
        SkipPrecision();
        const int size = SizeType(Show().s); p++;
        int block = New(S_BLOCK);
        for (;;) {
            if (Show().t != T_ID) { Error("falta el nombre de la variable"); return -1; }
            const std::string name = Show().s; p++;
            if (IsOp("[")) { Error("los arreglos no se pueden usar en @vertex en la CPU"); return -1; }
            int init = -1;
            if (IsOp("=")) { p++; init = Assignment(); if (init < 0) return -1; }
            int d = New(S_DECL); N(d).size = size; N(d).a = init;
            N(d).var = NewLocal(name, size);   // (despues del valor: "float x = x;" lee la de afuera)
            N(block).args.push_back(d);
            if (IsOp(",")) { p++; continue; }
            break;
        }
        return block;
    }
    int Statement() {
        if (IsOp("{")) return Block();
        if (IsOp(";")) { p++; return New(S_NOTHING); }
        if (IsId("if")) {
            p++; if (!Wait("(")) return -1;
            int c = Expr(); if (c < 0 || !Wait(")")) return -1;
            int a = Statement(); if (a < 0) return -1;
            int b = -1;
            if (IsId("else")) { p++; b = Statement(); if (b < 0) return -1; }
            int n = New(S_IF); N(n).a = c; N(n).b = a; N(n).c = b; return n;
        }
        if (IsId("for")) {
            p++; if (!Wait("(")) return -1;
            scopes.push_back(std::map<std::string, int>());
            int start = -1, cond = -1, step = -1;
            if (IsStartType()) { start = Declaration(); if (start < 0) return -1; }
            else if (!IsOp(";")) { int e = Expr(); if (e < 0) return -1; start = New(S_EXPR); N(start).a = e; }
            if (!Wait(";")) return -1;
            if (!IsOp(";")) { cond = Expr(); if (cond < 0) return -1; }
            if (!Wait(";")) return -1;
            if (!IsOp(")")) { step = Expr(); if (step < 0) return -1; }
            if (!Wait(")")) return -1;
            int body = Statement(); if (body < 0) return -1;
            scopes.pop_back();
            int n = New(S_FOR); N(n).a = start; N(n).b = cond; N(n).c = step; N(n).d = body; return n;
        }
        if (IsId("while") || IsId("do")) { Error("'" + Show().s + "' no se puede usar en @vertex en la CPU (usa for)"); return -1; }
        if (IsId("return")) {
            p++;
            int e = -1;
            if (!IsOp(";")) { e = Expr(); if (e < 0) return -1; }
            if (!Wait(";")) return -1;
            int n = New(S_RET); N(n).a = e; return n;
        }
        if (IsId("break")) { p++; if (!Wait(";")) return -1; return New(S_BREAK); }
        if (IsId("continue")) { p++; if (!Wait(";")) return -1; return New(S_CONT); }
        if (IsId("discard")) { Error("'discard' no se puede usar en @vertex"); return -1; }
        if (IsStartType()) { int d = Declaration(); if (d < 0 || !Wait(";")) return -1; return d; }
        int e = Expr(); if (e < 0 || !Wait(";")) return -1;
        int n = New(S_EXPR); N(n).a = e; return n;
    }
    int Block() {
        if (!Wait("{")) return -1;
        scopes.push_back(std::map<std::string, int>());
        int b = New(S_BLOCK);
        while (!IsOp("}")) {
            if (Show().t == T_END) { Error("falta '}'"); return -1; }
            int s = Statement(); if (s < 0) return -1;
            N(b).args.push_back(s);
        }
        p++;
        scopes.pop_back();
        return b;
    }

    // ---- LO DE AFUERA: constantes y funciones ----
    bool Program() {
        while (Show().t != T_END) {
            if (IsOp(";")) { p++; continue; }
            if (IsId("precision")) { while (!IsOp(";") && Show().t != T_END) p++; p++; continue; }
            if (IsId("uniform") || IsId("attribute") || IsId("varying")) return Error("'" + Show().s + "' no se declara: lo pone Whisk3D");
            const bool constant = IsId("const");
            if (constant) p++;
            SkipPrecision();
            if (Show().t != T_ID || SizeType(Show().s) < 0) return Error("se esperaba un tipo (float, vec3...) y vino '" + Show().s + "'");
            const int size = SizeType(Show().s); p++;
            if (Show().t != T_ID) return Error("falta un nombre");
            const std::string name = Show().s; p++;
            if (IsOp("(")) {   // una FUNCION
                p++;
                Function f; f.name = name; f.retSize = size; f.body = -1;
                prog->funcs.push_back(f);
                fCurrent = &prog->funcs.back();
                const int fi = (int)prog->funcs.size() - 1;
                scopes.clear(); scopes.push_back(std::map<std::string, int>());
                if (IsId("void") && IsOp(")", 1)) p++;
                if (!IsOp(")")) for (;;) {
                    int mode = 0;
                    if (IsId("in")) { p++; } else if (IsId("out")) { mode = 1; p++; } else if (IsId("inout")) { mode = 2; p++; }
                    if (IsId("const")) p++;
                    SkipPrecision();
                    const int pt = SizeType(Show().s);
                    if (pt <= 0) return Error("tipo de parametro no valido: '" + Show().s + "'");
                    p++;
                    if (Show().t != T_ID) return Error("falta el nombre del parametro");
                    const std::string pn = Show().s; p++;
                    prog->funcs[(size_t)fi].paramSize.push_back(pt);
                    prog->funcs[(size_t)fi].paramMode.push_back(mode);
                    prog->funcs[(size_t)fi].paramSlot.push_back(NewLocal(pn, pt));
                    if (IsOp(",")) { p++; continue; }
                    break;
                }
                if (!Wait(")")) return false;
                if (IsOp(";")) { return Error("no hace falta declarar '" + name + "' antes: escribila completa arriba de donde se usa"); }
                const int body = Block(); if (body < 0) return false;
                prog->funcs[(size_t)fi].body = body;
                scopes.clear();
                fCurrent = 0;
                continue;
            }
            // una CONSTANTE (o variable) global
            if (!constant) return Error("una variable afuera de una funcion tiene que ser 'const'");
            for (;;) {
                if (!Wait("=")) return false;
                scopes.clear();
                int init = Assignment(); if (init < 0) return false;
                globalSize.push_back(size);
                const int slot = (int)prog->global.size();
                prog->global.push_back(IsEmpty(size));
                globalName[name] = slot;
                // (se evalua al terminar: el valor queda en prog->global)
                int d = New(S_DECL); N(d).size = size; N(d).a = init; N(d).var = slot; N(d).global = true;
                globalInic.push_back(d);
                if (IsOp(",")) {
                    p++;
                    if (Show().t != T_ID) return Error("falta un nombre");
                    // (el mismo tipo para la que sigue)
                    const std::string other = Show().s; p++;
                    (void)other;
                    return Error("una constante por renglon, por favor");
                }
                break;
            }
            if (!Wait(";")) return false;
        }
        return true;
    }
    std::vector<int> globalInic;
};

// ---------------------------------------------------------------------------
//  LA EVALUACION
// ---------------------------------------------------------------------------
enum { X_STILL, X_BREAK, X_CONT, X_RET };
struct Executor {
    const W3dVertCPU* prog;
    std::vector<V>* global;
    // los lugares de las funciones en curso, en UNA pila reservada una vez (una llamada por vertice no pide memoria:
    // en el N95 eso costaba mas que la cuenta). 'marco' = donde empiezan los de la funcion en curso; 'tope' = el primero libre
    std::vector<V>* stack;
    size_t frame, limit;
    V ret;
    int depth;
    bool failed;

    V* Place(const Node& n) { return n.global ? &(*global)[(size_t)n.var] : &(*stack)[frame + (size_t)n.var]; }
    V Fit(const V& v, int size) {   // al tamanio del lugar (un escalar se repite)
        V r = IsEmpty(size);
        for (int k = 0; k < size; k++) r.c[k] = (v.n == 1) ? v.c[0] : (k < v.n ? v.c[k] : 0.0f);
        return r;
    }
    void Save(int e, const V& v) {
        const Node& n = prog->nodes[(size_t)e];
        if (n.t == E_VAR) { V* l = Place(n); *l = Fit(v, l->n > 0 ? l->n : v.n); return; }
        if (n.t == E_SWIZ) {
            V base = Eval(n.a);
            for (int k = 0; k < n.nsw; k++) base.c[n.sw[k]] = (v.n == 1) ? v.c[0] : (k < v.n ? v.c[k] : 0.0f);
            Save(n.a, base); return;
        }
        if (n.t == E_INDEX) {
            V base = Eval(n.a);
            const int i = (int)Eval(n.b).c[0];
            if (i >= 0 && i < 4) base.c[i] = v.c[0];
            Save(n.a, base);
        }
    }
    static V Bin(int op, const V& a, const V& b) {
        const int n = a.n > b.n ? a.n : b.n;
        V r = IsEmpty(n);
        switch (op) {
            case O_MORE:   for (int k = 0; k < n; k++) r.c[k] = Comp(a, k) + Comp(b, k); return r;
            case O_MINUS: for (int k = 0; k < n; k++) r.c[k] = Comp(a, k) - Comp(b, k); return r;
            case O_BY:   for (int k = 0; k < n; k++) r.c[k] = Comp(a, k) * Comp(b, k); return r;
            case O_DIV:   for (int k = 0; k < n; k++) { const float d = Comp(b, k); r.c[k] = d != 0.0f ? Comp(a, k) / d : 0.0f; } return r;
            case O_MOD:   for (int k = 0; k < n; k++) { const float d = Comp(b, k); r.c[k] = d != 0.0f ? Comp(a, k) - d * floorf(Comp(a, k) / d) : 0.0f; } return r;
            case O_IG: case O_DIST: {
                bool ig = true; for (int k = 0; k < n; k++) if (Comp(a, k) != Comp(b, k)) ig = false;
                return Scale((op == O_IG) == ig ? 1.0f : 0.0f);
            }
            case O_MEN:   return Scale(a.c[0] < b.c[0] ? 1.0f : 0.0f);
            case O_MAY:   return Scale(a.c[0] > b.c[0] ? 1.0f : 0.0f);
            case O_MENIG: return Scale(a.c[0] <= b.c[0] ? 1.0f : 0.0f);
            case O_MAYIG: return Scale(a.c[0] >= b.c[0] ? 1.0f : 0.0f);
        }
        return r;
    }
    static float Length(const V& v) { float s = 0; for (int k = 0; k < v.n; k++) s += v.c[k] * v.c[k]; return sqrtf(s); }
    static float Point(const V& a, const V& b) { const int n = a.n < b.n ? a.n : b.n; float s = 0; for (int k = 0; k < n; k++) s += a.c[k] * b.c[k]; return s; }
    V Func(int fn, const std::vector<int>& args) {
        V a = Eval(args[0]);
        V b = args.size() > 1 ? Eval(args[1]) : IsEmpty(1);
        V c = args.size() > 2 ? Eval(args[2]) : IsEmpty(1);
        const std::string name = kBuiltins[fn].name;
        const int n = (a.n > b.n ? a.n : b.n) > c.n ? (a.n > b.n ? a.n : b.n) : c.n;
        V r = IsEmpty(a.n);
        // las de UN argumento, componente a componente
        #define W3D_ONE(fn_, expr) if (name == fn_) { for (int k = 0; k < a.n; k++) { const float x = a.c[k]; r.c[k] = (expr); } return r; }
        W3D_ONE("sin", sinf(x)) W3D_ONE("cos", cosf(x)) W3D_ONE("tan", tanf(x))
        W3D_ONE("asin", asinf(x < -1 ? -1 : (x > 1 ? 1 : x))) W3D_ONE("acos", acosf(x < -1 ? -1 : (x > 1 ? 1 : x)))
        W3D_ONE("radians", x * 0.017453292f) W3D_ONE("degrees", x * 57.29578f)
        W3D_ONE("exp", expf(x)) W3D_ONE("log", x > 0 ? logf(x) : 0.0f) W3D_ONE("exp2", powf(2.0f, x))
        W3D_ONE("log2", x > 0 ? logf(x) * 1.4426950f : 0.0f) W3D_ONE("sqrt", x > 0 ? sqrtf(x) : 0.0f)
        W3D_ONE("inversesqrt", x > 0 ? 1.0f / sqrtf(x) : 0.0f) W3D_ONE("abs", fabsf(x))
        W3D_ONE("sign", x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f)) W3D_ONE("floor", floorf(x)) W3D_ONE("ceil", ceilf(x))
        W3D_ONE("fract", x - floorf(x))
        #undef W3D_ONE
        if (name == "atan") {
            if (args.size() == 1) { for (int k = 0; k < a.n; k++) r.c[k] = atanf(a.c[k]); return r; }
            r = IsEmpty(n); for (int k = 0; k < n; k++) r.c[k] = atan2f(Comp(a, k), Comp(b, k)); return r;
        }
        if (name == "pow") { r = IsEmpty(n); for (int k = 0; k < n; k++) r.c[k] = powf(Comp(a, k), Comp(b, k)); return r; }
        if (name == "mod") return Bin(O_MOD, a, b);
        if (name == "min") { r = IsEmpty(n); for (int k = 0; k < n; k++) { const float x = Comp(a, k), y = Comp(b, k); r.c[k] = x < y ? x : y; } return r; }
        if (name == "max") { r = IsEmpty(n); for (int k = 0; k < n; k++) { const float x = Comp(a, k), y = Comp(b, k); r.c[k] = x > y ? x : y; } return r; }
        if (name == "clamp") { r = IsEmpty(n); for (int k = 0; k < n; k++) { float x = Comp(a, k); const float lo = Comp(b, k), hi = Comp(c, k); if (x < lo) x = lo; if (x > hi) x = hi; r.c[k] = x; } return r; }
        if (name == "mix") { r = IsEmpty(n); for (int k = 0; k < n; k++) { const float t = Comp(c, k); r.c[k] = Comp(a, k) * (1.0f - t) + Comp(b, k) * t; } return r; }
        if (name == "step") { r = IsEmpty(n); for (int k = 0; k < n; k++) r.c[k] = Comp(b, k) < Comp(a, k) ? 0.0f : 1.0f; return r; }
        if (name == "smoothstep") {
            r = IsEmpty(n);
            for (int k = 0; k < n; k++) {
                const float e0 = Comp(a, k), e1 = Comp(b, k);
                float t = e1 != e0 ? (Comp(c, k) - e0) / (e1 - e0) : 0.0f; if (t < 0) t = 0; if (t > 1) t = 1;
                r.c[k] = t * t * (3.0f - 2.0f * t);
            }
            return r;
        }
        if (name == "length") return Scale(Length(a));
        if (name == "distance") return Scale(Length(Bin(O_MINUS, a, b)));
        if (name == "dot") return Scale(Point(a, b));
        if (name == "cross") {
            r = IsEmpty(3);
            r.c[0] = a.c[1] * b.c[2] - a.c[2] * b.c[1]; r.c[1] = a.c[2] * b.c[0] - a.c[0] * b.c[2]; r.c[2] = a.c[0] * b.c[1] - a.c[1] * b.c[0];
            return r;
        }
        if (name == "normalize") { const float l = Length(a); for (int k = 0; k < a.n; k++) r.c[k] = l > 0 ? a.c[k] / l : 0.0f; return r; }
        if (name == "reflect") {   // I - 2 dot(N, I) N
            const float d = 2.0f * Point(b, a);
            for (int k = 0; k < a.n; k++) r.c[k] = a.c[k] - d * Comp(b, k); return r;
        }
        if (name == "refract") {
            const float eta = c.c[0], d = Point(b, a), kk = 1.0f - eta * eta * (1.0f - d * d);
            if (kk < 0.0f) return IsEmpty(a.n);
            for (int k = 0; k < a.n; k++) r.c[k] = eta * a.c[k] - (eta * d + sqrtf(kk)) * Comp(b, k); return r;
        }
        return r;
    }
    V Eval(int e) {
        const Node& n = prog->nodes[(size_t)e];
        switch (n.t) {
            case E_NUM: return Scale(n.num);
            case E_VAR: return *Place(n);
            case E_UN: {
                V a = Eval(n.a);
                if (n.op == O_NO) return Scale(a.c[0] != 0.0f ? 0.0f : 1.0f);
                for (int k = 0; k < a.n; k++) a.c[k] = -a.c[k];
                return a;
            }
            case E_BIN: {
                if (n.op == O_Y) { if (Eval(n.a).c[0] == 0.0f) return Scale(0.0f); return Scale(Eval(n.b).c[0] != 0.0f ? 1.0f : 0.0f); }
                if (n.op == O_O) { if (Eval(n.a).c[0] != 0.0f) return Scale(1.0f); return Scale(Eval(n.b).c[0] != 0.0f ? 1.0f : 0.0f); }
                return Bin(n.op, Eval(n.a), Eval(n.b));
            }
            case E_TERN: return Eval(n.a).c[0] != 0.0f ? Eval(n.b) : Eval(n.c);
            case E_ASIGN: {
                V v = Eval(n.b);
                if (n.op != O_NOTHING) v = Bin(n.op, Eval(n.a), v);
                Save(n.a, v);
                return Eval(n.a);
            }
            case E_INCDEC: {
                const V prev = Eval(n.a);
                V after = Bin(n.op, prev, Scale(1.0f));
                Save(n.a, after);
                return n.num != 0.0f ? after : prev;
            }
            case E_SWIZ: {
                const V a = Eval(n.a);
                V r = IsEmpty(n.nsw);
                for (int k = 0; k < n.nsw; k++) r.c[k] = (a.n == 1) ? a.c[0] : a.c[n.sw[k]];
                return r;
            }
            case E_INDEX: {
                const V a = Eval(n.a);
                const int i = (int)Eval(n.b).c[0];
                return Scale(i >= 0 && i < a.n ? a.c[i] : 0.0f);
            }
            case E_CONSTR: {
                V r = IsEmpty(n.size);
                if (n.args.size() == 1) { const V a = Eval(n.args[0]); if (a.n == 1) { for (int k = 0; k < n.size; k++) r.c[k] = a.c[0]; return r; } }
                int k = 0;
                for (size_t i = 0; i < n.args.size() && k < n.size; i++) {
                    const V a = Eval(n.args[i]);
                    for (int j = 0; j < a.n && k < n.size; j++) r.c[k++] = a.c[j];
                }
                return r;
            }
            case E_FUNC: return Func(n.fn, n.args);
            case E_CALL: return Call(n);
            case S_BLOCK: {   // (la coma de un for)
                V r = IsEmpty(1);
                for (size_t i = 0; i < n.args.size(); i++) r = Eval(n.args[i]);
                return r;
            }
        }
        return IsEmpty(1);
    }
    V Call(const Node& n) {
        const Function& f = prog->funcs[(size_t)n.fn];
        if (depth > 16) { failed = true; return IsEmpty(f.retSize > 0 ? f.retSize : 1); }
        const size_t base = limit, nn = f.slotSize.size();
        if (stack->size() < base + nn) stack->resize((base + nn) * 2);
        limit = base + nn;   // (antes de los argumentos: una llamada adentro de un argumento usa lo de mas arriba)
        for (size_t i = 0; i < nn; i++) (*stack)[base + i] = IsEmpty(f.slotSize[i]);
        for (size_t i = 0; i < f.paramSlot.size(); i++)
            if (f.paramMode[i] != 1) { const V a = Fit(Eval(n.args[i]), f.paramSize[i]); (*stack)[base + (size_t)f.paramSlot[i]] = a; }
        const size_t prev = frame;
        frame = base;
        depth++;
        ret = IsEmpty(f.retSize > 0 ? f.retSize : 1);
        Exec(f.body);
        depth--;
        const V r = ret;
        frame = prev;
        for (size_t i = 0; i < f.paramSlot.size(); i++)
            if (f.paramMode[i] != 0) { const V v = (*stack)[base + (size_t)f.paramSlot[i]]; Save(n.args[i], v); }
        limit = base;
        return r;
    }
    int Exec(int s) {
        if (s < 0) return X_STILL;
        const Node& n = prog->nodes[(size_t)s];
        switch (n.t) {
            case S_NOTHING: return X_STILL;
            case S_EXPR: Eval(n.a); return X_STILL;
            case S_DECL: {
                V* l = n.global ? &(*global)[(size_t)n.var] : &(*stack)[frame + (size_t)n.var];
                *l = n.a >= 0 ? Fit(Eval(n.a), n.size) : IsEmpty(n.size);
                return X_STILL;
            }
            case S_BLOCK:
                for (size_t i = 0; i < n.args.size(); i++) { const int r = Exec(n.args[i]); if (r != X_STILL) return r; }
                return X_STILL;
            case S_IF: return Eval(n.a).c[0] != 0.0f ? Exec(n.b) : Exec(n.c);
            case S_FOR: {
                Exec(n.a);
                for (int turn = 0; turn < 4096; turn++) {   // (tope: un for que no termina no cuelga el editor)
                    if (n.b >= 0 && Eval(n.b).c[0] == 0.0f) break;
                    const int r = Exec(n.d);
                    if (r == X_BREAK) break;
                    if (r == X_RET) return r;
                    if (n.c >= 0) Eval(n.c);
                }
                return X_STILL;
            }
            case S_RET: if (n.a >= 0) ret = Eval(n.a); return X_RET;
            case S_BREAK: return X_BREAK;
            case S_CONT: return X_CONT;
        }
        Eval(s);
        return X_STILL;
    }
};

} // namespace

W3dVertCPU* W3dVertCPUCompile(const std::string& text, std::string& error) {
    error.clear();
    W3dVertCPU* prog = new W3dVertCPU();
    prog->vertex = -1;
    Compiler c;
    c.p = 0; c.prog = prog; c.fCurrent = 0;
    if (!Words(text, c.tk, c.err)) { error = c.err; delete prog; return 0; }
    for (int i = 0; kEntries[i]; i++) { c.globalName[kEntries[i]] = i; prog->global.push_back(IsEmpty(1)); c.globalSize.push_back(1); }
    if (!c.Program()) { error = c.err; delete prog; return 0; }
    prog->vertex = c.FindFunction("Vertex");
    if (prog->vertex < 0) { error = "falta la funcion void Vertex(inout vec3 pos, inout vec3 normal)"; delete prog; return 0; }
    const Function& f = prog->funcs[(size_t)prog->vertex];
    if (f.paramSize.size() != 2 || f.paramSize[0] != 3 || f.paramSize[1] != 3) {
        error = "Vertex tiene que ser: void Vertex(inout vec3 pos, inout vec3 normal)"; delete prog; return 0;
    }
    // las constantes de afuera, en orden
    std::vector<V> stack;
    Executor x; x.prog = prog; x.global = &prog->global; x.stack = &stack; x.frame = 0; x.limit = 0; x.depth = 0; x.failed = false;
    for (size_t i = 0; i < c.globalInic.size(); i++) x.Exec(c.globalInic[i]);
    return prog;
}

void W3dVertCPUDelete(W3dVertCPU* p) { delete p; }

bool W3dVertCPUApply(const W3dVertCPU* p, const W3dVertCPUEntries& e, const float* pos, const signed char* nor, int n,
                       float* posOut, signed char* norOut) {
    if (!p || p->vertex < 0 || n <= 0) return false;
    std::vector<V> global = p->global;
    global[0] = Scale(e.time); global[1] = Scale(e.ior); global[2] = Scale(e.transmission);
    global[3] = Scale(e.roughness); global[4] = Scale(e.metallic);
    const Function& f = p->funcs[(size_t)p->vertex];
    const size_t nn = f.slotSize.size();
    std::vector<V> stack(nn + 64);
    Executor x; x.prog = p; x.global = &global; x.stack = &stack; x.depth = 0; x.failed = false;
    const int sPos = f.paramSlot[0], sNor = f.paramSlot[1];
    for (int i = 0; i < n; i++) {
        for (size_t k = 0; k < nn; k++) stack[k] = IsEmpty(f.slotSize[k]);
        V& vp = stack[(size_t)sPos]; vp.c[0] = pos[i * 3]; vp.c[1] = pos[i * 3 + 1]; vp.c[2] = pos[i * 3 + 2];
        V& vn = stack[(size_t)sNor];
        if (nor) { vn.c[0] = nor[i * 3] / 127.0f; vn.c[1] = nor[i * 3 + 1] / 127.0f; vn.c[2] = nor[i * 3 + 2] / 127.0f; }
        else { vn.c[0] = 0; vn.c[1] = 1; vn.c[2] = 0; }
        x.frame = 0; x.limit = nn;
        x.Exec(f.body);
        if (x.failed) return false;
        const V& rp = stack[(size_t)sPos];
        posOut[i * 3] = rp.c[0]; posOut[i * 3 + 1] = rp.c[1]; posOut[i * 3 + 2] = rp.c[2];
        if (nor && norOut) {
            const V& rn = stack[(size_t)sNor];
            for (int k = 0; k < 3; k++) { float q = rn.c[k] * 127.0f; if (q > 127.0f) q = 127.0f; if (q < -127.0f) q = -127.0f; norOut[i * 3 + k] = (signed char)(q < 0 ? q - 0.5f : q + 0.5f); }
        }
    }
    return true;
}
