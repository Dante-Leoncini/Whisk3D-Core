#pragma once
// ============================================================================
//  Whisk3DCore (engine) — cargador de texturas UNIVERSAL
//
//  Abrir archivos y cargar imagenes es lo basico de cualquier juego o programa, sin
//  importar el sistema operativo. por eso se incluye en el engine
//
//  Una textura se carga en dos pasos:
//    1) DECODE  archivo -> pixeles RGBA   (depende del sistema: stb en
//                                          PC/Android, ICL en Symbian)
//    2) UPLOAD  pixeles -> textura GL      (comun a todos)
//
//  Esta cabecera NO expone tipos de GL a proposito (el id es un unsigned int)
// ============================================================================

namespace w3dEngine {

    // Carga 'path' del disco y la sube como textura. Devuelve true y deja en
    // 'outId' el id de textura (!=0) si funciono. 'outW'/'outH' (opcionales)
    // reciben el tamano en pixeles. El decode lo resuelve cada plataforma; el
    // upload es comun (UploadRGBA).
    bool LoadTexture(const char* path, unsigned int& outId,
                     int* outW = 0, int* outH = 0);

    // .w3dt: TEXTURA LISTA PARA LA GPU (16 bits + mipmaps hechos; formato en w3dTexture.cpp). LoadTexture y
    // DecodeImage la reconocen por la extension en todas las plataformas: cargarla es leer y subir, sin decodificar
    bool EsW3dt(const char* path);
    bool LoadTextureW3dt(const char* path, unsigned int& outId, int* outW = 0, int* outH = 0);
    // su nivel 0 (o el primero que entra en 'maxLado', si > 0) a RGBA 8888, en un buffer de FreeImage
    bool DecodeW3dt(const char* path, int maxLado, unsigned char** outRGBA, int* outW, int* outH);

    // Sube pixeles RGBA 8888 ya decodificados como textura 2D y devuelve su id
    // (0 si falla). 'filtrado' = LINEAR si true, NEAREST si false. Formato
    // interno GL_RGBA: valido tanto en GL de escritorio como en GLES 1.1.
    // 'conMips' = false la excluye del mipmapping global (la UI 2D lo evita a
    // proposito: los mips promediaban los slice9 chicos).
    // 'forzarPOT' remuestrea a potencia-de-dos ANTES de subir (solo hace falta
    // en Symbian: GLES 1.1 del N95 rechaza las texturas NPOT y el billboard sale
    // sin textura). El camino de material ya lo hace (w3dtexload ReescalarPOT); el
    // de particulas/UI-2D (Textura2D) se lo saltaba. No-op en el resto de OSes.
    // 'permitir16': si la imagen ENTRA en 16 bits sin perdida visible (cada canal a menos de 2/255 de su valor
    // cuantizado: las texturas de la ROM, la UI de pocos colores), sube 565 (opaca) o 5551 (alfa de un bit): la MITAD
    // de memoria. Solo para las que nunca se reescriben (UpdateRGBA manda RGBA de 8 bits y GLES exige el mismo
    // formato): los materiales y las imagenes de la UI, no la pintura ni el video
    unsigned int UploadRGBA(const unsigned char* rgba, int w, int h,
                            bool filtrado = true, bool conMips = true,
                            bool forzarPOT = false, bool permitir16 = false);
    // TEXTURAS DE 16 BITS del PROYECTO ("texturas16" en el .w3d): false (defecto) = solo las que entran sin perdida
    // visible; true = SIEMPRE 565 / 5551 (opacas y de alfa de un bit; hasta 4/255 de error por canal: la mitad de
    // memoria). Las de alfa con niveles quedan en 8 bits. Gobierna las cargas nuevas
    bool Texturas16Siempre();
    void SetTexturas16Siempre(bool on);
    // ...y lo mismo SOLO EN EL N95 ("texturas16Movil" en el .w3d): el proyecto se ve igual en PC y en el telefono sus
    // texturas ocupan la mitad (junto con texturasMitadDesde, la cuarta parte). Solo en Symbian; en el resto se ignora
    bool Texturas16Movil();
    void SetTexturas16Movil(bool on);
    // TEXTURAS A LA MITAD EN EL N95 ("texturasMitadDesde" en el .w3d; 0 = no, el defecto): cada textura del PROYECTO
    // (las de los materiales y las imagenes de la UI; no las del editor ni las fuentes) con un lado MAYOR a ese se sube
    // a la mitad de resolucion. Un proyecto pensado para PC no entra en la memoria del telefono (Daytona: ~35 MB de
    // texturas, el N95 se cae a los ~20) y en 320x240 la mitad casi no se nota. Las UV no cambian (van de 0 a 1) y lo
    // que se informa como tamano de la imagen es el original. Solo en Symbian; en el resto se ignora. Gobierna las
    // cargas nuevas
    int TexturasMitadDesde();
    void SetTexturasMitadDesde(int lado);
    // la imagen RGBA a la MITAD (cada 2x2 promediado): un buffer nuevo que se libera con FreeImage; NULL sin memoria
    unsigned char* ReducirMitadRGBA(const unsigned char* rgba, int w, int h, int& ow, int& oh);
    // la MEMORIA de las texturas subidas (bytes, con su piramide de mips) y cuantas hay / cuantas en 16 bits
    // bytes32: lo que serian todas en RGBA8; bytesPot: lo que ocupan en el N95 (remuestreadas a potencia de dos)
    struct MemoriaTexturas { long bytes, bytes32, bytesPot; int cantidad, de16; };
    MemoriaTexturas TexturasMemoria();
    // la de UNA textura subida (false si ese id no se subio por aca)
    bool TexturaMemoriaDe(unsigned int id, long& bytes, long& bytesPot, bool& de16);

    // actualiza un RECTANGULO (x,y,w,h) de una textura ya subida con w*h pixeles RGBA (pintura en vivo)
    // regenerarMips=false: no rehace la piramide (en GLES2 eso es la textura ENTERA cada vez); para la pintura en vivo,
    // que la rehace una sola vez al soltar con RegenerarMips. px1 = un pixel cualquiera de la textura (en GLES1/PC la
    // piramide se rehace re-subiendo ese pixel con GL_GENERATE_MIPMAP).
    bool UpdateRGBA(unsigned int id, int x, int y, int w, int h, const unsigned char* rgbaRect, bool regenerarMips = true);
    void RegenerarMips(unsigned int id, int x, int y, const unsigned char* px1);
    // dimensiones con que se subio una textura (para el aspect ratio). false si no se conoce.
    bool TextureSize(unsigned int id, int& w, int& h);

    // Decodifica una imagen del disco a pixeles RGBA 8888 en el heap, sin
    // subirla a GL. Util cuando se necesitan los PIXELES (no solo el id), p.ej.
    // para armar un sprite. El que llama libera con FreeImage. Devuelve true y
    // deja el buffer en *outRGBA (+ tamano en outW/outH) si funciono. El decode
    // es por plataforma (stb en PC/Android, ICL en Symbian).
    bool DecodeImage(const char* path, unsigned char** outRGBA,
                     int* outW, int* outH);

    // Libera un buffer devuelto por DecodeImage.
    void FreeImage(unsigned char* rgba);

    // Borra una textura GL (glDeleteTextures). La usan las MINIATURAS del file browser para liberar la RAM/VRAM
    // al cambiar de carpeta o cerrar (clave en el N95 con poca memoria). id 0 = no-op.
    void DeleteTexture(unsigned int id);

    // MINIATURA: decodifica una imagen y la reduce (box filter) a <= maxSize px por lado, MANTENIENDO el aspecto.
    // El buffer FULL se decodifica y se libera ADENTRO (transitorio); solo queda la mini en *outRGBA (liberar con
    // FreeImage). Pensado para previews livianas: nunca deja en memoria la imagen de 5MP entera. No en Symbian (ICL).
    bool DecodeThumbnail(const char* path, int maxSize, unsigned char** outRGBA, int* outW, int* outH);

    // Codifica pixeles RGBA 8888 a un buffer PNG RGB en el heap (new[]; liberar con delete[] o
    // FreeImage). Entra RGBA pero el PNG se guarda como RGB: el alpha se descarta (los renders son
    // solidos; un alpha del framebuffer haria "huecos" en materiales con transparencia como pelo/hojas).
    // 'outLen' recibe el tamano en bytes. 'flipY' invierte verticalmente (glReadPixels es bottom-left,
    // el PNG top-left). Portable (deflate stored, sin comprimir): lo usan SavePNG (PC/Web con stdio) y
    // el sink de Symbian (RFile). Devuelve 0 si falla.
    unsigned char* EncodePNG(const unsigned char* rgba, int w, int h, bool flipY, int* outLen);
    // idem pero el PNG sale RGBA (conserva el alpha): las texturas generadas con canal alpha
    unsigned char* EncodePNGRGBA(const unsigned char* rgba, int w, int h, bool flipY, int* outLen);

    // Guarda pixeles RGBA 8888 como PNG RGB en 'path' (= EncodePNG + escribir a disco; el alpha se
    // descarta). Devuelve true si escribio. En PC/Web usa stdio; en Symbian usa RFile (w3dtexload.cpp).
    bool SavePNG(const char* path, const unsigned char* rgba, int w, int h, bool flipY = true);
}
