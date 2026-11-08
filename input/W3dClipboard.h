#ifndef W3DCLIPBOARD_H
#define W3DCLIPBOARD_H

#include <string>

// ============================================================================
//  Portapapeles del SISTEMA, abstraccion del Whisk3D Core. Backend por plataforma
//  (mismo esquema que W3dAudio): SDL en PC/web, CClipboard en Symbian. Es el MISMO
//  portapapeles del sistema operativo -> se puede copiar/pegar con el navegador, las
//  notas u otras apps. Reutilizable por cualquier programa que use el Core.
// ============================================================================
namespace w3dEngine {
    void        W3dClipboardSet(const std::string& s);  // copiar 's' al portapapeles del sistema
    std::string W3dClipboardGet();                       // pegar desde el portapapeles del sistema ("" si esta vacio)
}

#endif // W3DCLIPBOARD_H
