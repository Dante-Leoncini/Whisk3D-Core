#ifndef RENDER_COLORS_H
#define RENDER_COLORS_H

// ============================================================================
//  Colores que el RENDER del core necesita para dibujar SELECCION / overlays de
//  normales / gizmos. El core NO depende de la paleta de la UI (WhiskUI/colores):
//  tiene estos con DEFAULTS y el EDITOR los sincroniza con su tema (ver el sync
//  en main.cpp tras cargar el skin). Asi el engine es reusable: quien lo use
//  tiene colores razonables por defecto y puede pisarlos.
// ============================================================================
enum RenderColorId {
    RC_wireframe = 0, // objeto no seleccionado / wireframe
    RC_selActive,     // seleccionado + activo
    RC_selInactive,   // seleccionado, no activo
    RC_normalFace,    // overlay: normal de cara
    RC_normalVert,    // overlay: normal de vertice
    RC_normalCustom,  // overlay: normal custom
    RC_gizmoDark,     // gizmo apagado (ej: luz no seleccionada)
    RC_COUNT
};

extern float gRenderColors[RC_COUNT][4];

#endif // RENDER_COLORS_H
