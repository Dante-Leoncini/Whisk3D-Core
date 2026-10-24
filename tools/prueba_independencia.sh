#!/usr/bin/env bash
# ============================================================================
#  prueba_independencia.sh — ¿el motor se puede usar SIN el editor?
#
#  Dos preguntas, medidas y no opinadas:
#    1. COMPILA: cada header público, solo, en su propia unidad de traducción,
#       con únicamente los include dirs del Core y en C++03 estricto (el
#       dialecto de Symbian). Si uno necesita un header del editor, se ve acá.
#    2. ENLAZA: un programa mínimo que crea una malla y la usa. Lista los
#       símbolos que el Core le pide a alguien más.
#
#  Correr desde cualquier lado:  libs/Whisk3DCore/tools/prueba_independencia.sh
#  Devuelve 0 si las dos pasan.
# ============================================================================
set -u
CORE="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
INC="-I$CORE -I$CORE/base -I$CORE/gfx -I$CORE/io -I$CORE/thirdparty"

echo "== 1/2  cada header, solo, en C++03 =="
ok=0; mal=0
for h in $(cd "$CORE" && find base gfx io objects math animation audio video net -name '*.h' | sort); do
    printf '#include "%s"\nint main(){return 0;}\n' "$h" > "$TMP/t.cpp"
    if g++ -std=c++03 -fsyntax-only $INC "$TMP/t.cpp" 2> "$TMP/e.txt"; then
        ok=$((ok+1))
    else
        mal=$((mal+1))
        echo "   FALLA  $h"
        head -3 "$TMP/e.txt" | tail -1 | sed 's/^/          /'
    fi
done
echo "   compilan solos: $ok    fallan: $mal"

echo
echo "== 2/2  enlazar un programa que solo usa el motor =="
cat > "$TMP/uso.cpp" <<'EOF'
// Un proyecto cualquiera: incluye la malla, crea una, la usa. Nada del editor.
#include "objects/Mesh.h"
#include "math/Matrix4.h"
int main() {
    Mesh m;
    m.GenerarRender(true);
    Matrix4 t; t.Identity();
    return 0;
}
EOF
# w3dTexture/w3dFilesystem/w3dGraphics usan C++11 a propósito fuera de Symbian (está guardado):
# se compilan aparte para que eso no ensucie la prueba de C++03 de los headers.
g++ -std=c++11 -c -DW3D_STB_IMPL $INC "$CORE/gfx/w3dTexture.cpp"  -o "$TMP/a.o" 2>/dev/null
g++ -std=c++11 -c $INC "$CORE/io/w3dFilesystem.cpp"               -o "$TMP/b.o" 2>/dev/null
g++ -std=c++11 -c $INC "$CORE/gfx/w3dGraphics.cpp"                -o "$TMP/c.o" 2>/dev/null
g++ -std=c++03 -c $INC "$TMP/uso.cpp"                             -o "$TMP/u.o" 2>/dev/null

faltan=$(g++ "$TMP/u.o" "$TMP/a.o" "$TMP/b.o" "$TMP/c.o" \
    "$CORE"/objects/*.cpp "$CORE"/math/*.cpp "$CORE"/animation/*.cpp "$CORE"/base/*.cpp \
    -std=c++03 $INC -lGL -lGLU -o "$TMP/uso" 2>&1 \
    | grep -oE "(undefined reference to|referencia a) .[^'\`]*" \
    | sed -E "s/(undefined reference to|referencia a) .//" | sort -u)

if [ -z "$faltan" ]; then
    echo "   enlaza y corre sin nada del editor"
    "$TMP/uso" && echo "   el programa corrio (exit 0)"
else
    echo "   le pide al editor $(echo "$faltan" | wc -l) simbolo(s):"
    echo "$faltan" | sed 's/^/      /'
fi

echo
[ "$mal" = "0" ] && [ -z "$faltan" ] && { echo "MOTOR INDEPENDIENTE"; exit 0; }
echo "TODAVIA NO ES INDEPENDIENTE (ver arriba)"; exit 1
