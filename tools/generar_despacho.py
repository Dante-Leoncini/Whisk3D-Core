# ============================================================================
#  generar_despacho.py - arma la capa de DESPACHO de los backends graficos (varios backends en UN ejecutable,
#  el que se usa se elige al ARRANCAR: ver gfx/w3dBackend.h).
#
#  Lee las funciones de w3dEngine (gfx/w3dGraphics.h y gfx/w3dTexture.h) que implementan TODOS los backends y genera:
#    gfx/w3dBackendTabla.h    la tabla de punteros (una entrada por funcion)
#    gfx/w3dBackendLlenar.inc lo que cada backend incluye al final: llena la tabla con SUS funciones
#    gfx/w3dDespacho.inc      w3dEngine::Foo(...) -> el backend activo (lo incluye gfx/w3dGraphicsDespacho.cpp)
#  y marca esas declaraciones en los headers con W3D_BACKEND_ABRIR / CERRAR: en el .cpp de un backend se declaran en
#  SU namespace (w3dEngine::gl::Foo). Si viera tambien w3dEngine::Foo, la busqueda por argumento (ADL: un parametro de
#  un tipo de w3dEngine) encontraria las dos y la llamada seria ambigua. Los tipos y los globales quedan en w3dEngine.
#  Correr de nuevo cuando se agregue una funcion a los backends (desde libs/Whisk3DCore):
#    python tools/generar_despacho.py
# ============================================================================
import re, os
raiz = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def leer(p): return open(os.path.join(raiz, p), encoding='utf-8', errors='replace').read()
def escribir(p, s):
    q = os.path.join(raiz, p)
    if os.path.exists(q) and open(q, encoding='utf-8').read() == s: return
    open(q, 'w', encoding='utf-8', newline='\n').write(s)
MARCAS = ('W3D_BACKEND_ABRIR', 'W3D_BACKEND_CERRAR')
def sinMarcas(s): return '\n'.join(l for l in s.split('\n') if l.strip() not in MARCAS)
# (cada header: hasta donde llega lo suyo de w3dEngine)
HEADERS = [('gfx/w3dGraphics.h', 'extern bool w3dRenderWireframe'), ('gfx/w3dTexture.h', None)]
BACKENDS = ['gfx/w3dGraphics.cpp', 'gles2/w3dGraphicsGLES2.cpp']

def rango(s, marcaFin):
    i = s.index('namespace w3dEngine {')
    j = s.index(marcaFin) if marcaFin else len(s)
    return i, j
def declaraciones(s, marcaFin):
    i, j = rango(s, marcaFin)
    c = re.sub(r'//[^\n]*', '', s[i:j]); c = re.sub(r'/\*.*?\*/', '', c, flags=re.S)
    c = re.sub(r'(?m)^\s*#.*$', '', c)   # (las directivas del preprocesador no son parte de nada)
    out = []
    for st in c.split(';'):
        t = ' '.join(st.split())
        if '}' in t: t = t[t.rfind('}') + 1:].strip()
        if '(' not in t or t.startswith(('extern', 'typedef', 'enum', 'inline', 'struct', 'namespace', 'class', 'static')): continue
        m = re.match(r'^([\w:<>\*& ]+?)\s*\b(\w+)\s*\((.*)\)\s*$', t)
        if m: out.append((m.group(1).strip(), m.group(2), m.group(3).strip()))
    return out
def definidas(p):
    s = re.sub(r'//[^\n]*', '', leer(p))
    return set(m.group(1) for m in re.finditer(r'(?m)^[ \t]*(?:static\s+)?[\w:<>\*&]+[\w:<>\*& \t]*?\b(\w+)\s*\(([^;{]*)\)\s*(?:const\s*)?\{', s))

defs = [definidas(b) for b in BACKENDS]
tabla = []; vistos = set(); todas = set()
for hp_, fin_ in HEADERS:
    for d in declaraciones(sinMarcas(leer(hp_)), fin_):
        todas.add(d[1])
        if all(d[1] in x for x in defs) and d[1] not in vistos: vistos.add(d[1]); tabla.append(d)
nombres = set(d[1] for d in tabla)

TIPOS = set('int unsigned float double bool char short long void const signed size_t'.split())
def params(ps):
    if ps in ('', 'void'): return [], []
    out, nom = [], []
    nivel = 0; cur = ''; partes = []
    for ch in ps:
        if ch in '(<': nivel += 1
        if ch in ')>': nivel -= 1
        if ch == ',' and nivel == 0: partes.append(cur); cur = ''
        else: cur += ch
    partes.append(cur)
    for k, p in enumerate(partes):
        p = p.split('=')[0].strip()
        m = re.match(r'^(.*?[\s\*&])(\w+)(\s*\[[^\]]*\])?$', p)
        if m and m.group(2) not in TIPOS and m.group(1).strip(): out.append(p); nom.append(m.group(2))
        else: n = 'a%d' % k; out.append(p + ' ' + n); nom.append(n)
    return out, nom

lt = ['// GENERADO por tools/generar_despacho.py: no editar a mano.', '#ifndef W3D_BACKEND_TABLA_H', '#define W3D_BACKEND_TABLA_H',
      '#include "w3dGraphics.h"', '#include "w3dTexture.h"', 'namespace w3dEngine {', 'struct W3dBackendTabla {']
ll = ['// GENERADO por tools/generar_despacho.py: no editar a mano. Lo incluye cada backend al FINAL (con W3D_BACKEND_NS).',
      'void W3D_BACKEND_LLENAR(W3dBackendTabla& t) {']
ld = ['// GENERADO por tools/generar_despacho.py: no editar a mano. Lo incluye gfx/w3dGraphicsDespacho.cpp.']
for ret, nom, ps in tabla:
    tp, ns = params(ps)
    lt.append('    %s (*%s)(%s);' % (ret, nom, ', '.join(tp)))
    ll.append('    t.%s = &w3dEngine::W3D_BACKEND_NS::%s;' % (nom, nom))
    ld.append('%s %s(%s) { %sW3D_TABLA()->%s(%s); }' % (ret, nom, ', '.join(tp), '' if ret == 'void' else 'return ', nom, ', '.join(ns)))
lt += ['};', '}', '#endif']; ll += ['}']
escribir('gfx/w3dBackendTabla.h', '\n'.join(lt) + '\n')
escribir('gfx/w3dBackendLlenar.inc', '\n'.join(ll) + '\n')
escribir('gfx/w3dDespacho.inc', '\n'.join(ld) + '\n')

# ---- las MARCAS en los headers (alrededor de cada tanda de declaraciones de la tabla; y del struct StatScope, que
#      es inline y las llama) ----
for hp_, fin_ in HEADERS:
    s = sinMarcas(leer(hp_))
    i, j = rango(s, fin_)
    ini = s[:i].count('\n'); fin = s[:j].count('\n')
    hl = s.split('\n'); out = []; dentro = False; bloque = False
    for k, l in enumerate(hl):
        m = re.match(r'^\s*[\w:<>\*& ]+?\b(\w+)\s*\([^;{]*\)\s*;', l) if ini < k < fin else None
        es = bool(m and m.group(1) in nombres and not l.strip().startswith(('extern', 'inline', 'typedef', '//', 'static')))
        if ini < k < fin and re.match(r'^\s*struct StatScope\b', l): bloque = True
        if bloque:
            es = True
            if l.strip() == '};': bloque = False
        if es and not dentro: out.append('W3D_BACKEND_ABRIR'); dentro = True
        if not es and dentro: out.append('W3D_BACKEND_CERRAR'); dentro = False
        out.append(l)
    if dentro: out.append('W3D_BACKEND_CERRAR')
    s2 = '\n'.join(out)
    if '#include "w3dBackend.h"' not in s2:
        s2 = s2.replace('namespace w3dEngine {', '#include "w3dBackend.h"   // (W3D_BACKEND_ABRIR / CERRAR: varios backends en un ejecutable)\nnamespace w3dEngine {', 1)
    escribir(hp_, s2)
solo = sorted(n for n in (defs[1] - defs[0]) if n in todas)
print('funciones en la tabla: %d (las que tiene un solo backend: %s)' % (len(tabla), solo))
