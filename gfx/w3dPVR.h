#pragma once
// ============================================================================
//  Whisk3DCore — DIBUJO DIRECTO A LA PVR DE LA DREAMCAST (solo W3D_DREAMCAST)
//
//  GLdc (el OpenGL de la Dreamcast que usa el backend fijo) es comodo pero caro: procesa cada vertice dos veces con
//  codigo generico (al dibujar transforma; al cerrar el cuadro recorta, divide por w y lo manda), unos 400 ciclos
//  del SH-4 por vertice. Un juego que necesita sus 30 cuadros por segundo dibuja por aca: la matriz de pantalla
//  (viewport * proyeccion * vista * modelo) va al registro XMTRX del SH-4 y cada vertice sale con UNA instruccion
//  (FTRV), su division por w y 8 palabras a la PVR por las store queues (el "direct rendering" de KallistiOS).
//
//  Lo que hay que saber de la PVR (y que este modulo deja a la vista):
//    - LISTAS: lo opaco, lo recortado por alfa ("punch through") y lo translucido van en tres listas que se mandan
//      una despues de la otra, cada una UNA vez por cuadro: EmpezarLista(Opaca) ... EmpezarLista(Recorte) ...
//      EmpezarLista(Translucida). Dentro de la translucida manda el orden de envio (sin autosort).
//    - PROFUNDIDAD: es 1/w (mas grande = mas cerca); no hay plano lejano.
//    - PLANO CERCANO: la PVR no recorta lo que queda detras de la camara. Tiras(..., recortar = true) recorta cada
//      triangulo que lo cruza (mas lento: solo para lo que puede cruzarlo; lo que esta entero adelante va rapido).
//    - TEXTURAS: en orden "twiddled", de 8 a 1024 por lado, de 16 bits (565, 1555, 4444) o de 4 bits con una paleta
//      de 16 colores (64 bancos; los colores en ARGB8888). Ojo: con filtro bilineal, las de paleta de 32 bits
//      rinden la mitad en la PVR.
//    - NIEBLA: por tabla (128 entradas segun 1/w); NieblaConstante deja todo lo que la tenga prendida oscurecido
//      igual (el "StgTint" de un juego: el escenario se oscurece sin tocar sus vertices).
//
//  No convive con GLdc en el mismo cuadro: un programa usa uno o el otro (Iniciar arranca la PVR).
// ============================================================================
#ifdef W3D_DREAMCAST
#include <stdint.h>

namespace w3dPVR {

    // --- el cuadro y sus listas ---
    enum Lista { ListaOpaca = 0, ListaRecorte, ListaTranslucida };
    // vid_set_mode(640x480) + pvr_init con las tres listas, sin autosort, paletas en ARGB8888. vertices = bytes del
    // buffer de vertices de la PVR (por defecto 1 MB: dos, uno por cuadro en curso)
    bool Iniciar(int bytesVertices = 1024 * 1024);
    void EmpezarCuadro();          // espera a que la PVR acepte otro cuadro
    void EmpezarLista(Lista l);    // (cierra la anterior)
    void TerminarCuadro();         // cierra la lista abierta y el cuadro (la PVR lo dibuja mientras se arma el proximo)
    void ColorDeFondo(float r, float g, float b);

    // --- texturas ---
    // PANTALLA: una copia de la imagen de la pantalla (RGB565 sin twiddle, filas de 640; ver CapturarPantalla).
    // PAL8: 8 bits por texel (twiddled) con su paleta aparte, que puede cambiar cada cuadro (ver ReservarPaleta8)
    enum Formato { RGB565 = 0, ARGB1555, ARGB4444, PAL4, PANTALLA, PAL8 };
    struct Textura {
        void* vram;
        uint16_t ancho, alto;
        uint8_t formato, banco;      // banco: el de su paleta (PAL4)
        uint8_t repetir, filtro;
    };
    // 'datos': 16 bits ya twiddled; PAL4: 16 colores RGBA (64 bytes) y los texeles de 4 bits twiddled (el par
    // (2k, 2k+1) en el byte k: el par en el nibble bajo). false si no hay VRAM o bancos de paleta
    // PAL8: 'datos' son solo los indices; su paleta es la del banco que se le ponga en 'banco' (ReservarPaleta8)
    bool SubirTextura(Textura& t, int ancho, int alto, int formato, const void* datos, bool repetir, bool filtro);
    void SoltarTextura(Textura& t);
    // vuelve a copiar los texeles de una ya subida (mismo tamano y formato): una animacion por cuadros que tiene uno
    // solo en la VRAM. Conviene hacerlo con AnterioresDibujados(): si no, se puede ver a medias en el cuadro anterior
    void ActualizarTextura(Textura& t, const void* datos);
    // LAS PALETAS DE 8 BITS: la paleta de la PVR tiene 1024 colores, los mismos que usan los 64 bancos de 16 de las
    // PAL4; una de 8 bits usa los de su banco de 256 (0..3), pero solo los primeros 'colores': reserva esos bancos de
    // 16 (hay que hacerlo antes de cargar las PAL4) y el resto de su banco de 256 sigue para las PAL4. -1 si no hay
    int  ReservarPaleta8(int colores);
    void SoltarPaleta8(int banco, int colores);
    void PaletaPAL8(int banco, const uint8_t* rgba, int colores);   // RGBA de 8 bits
    // la PVR dibuja un cuadro mientras se arma el siguiente: true si ya termino (y mostro) todos los que se mandaron.
    // Recien ahi se puede cambiar una paleta o una textura que usaba el anterior sin que se vea a medias
    bool AnterioresDibujados();
    int  VRAMLibre();              // bytes
    int  BancosLibres();
    // copia lo que se esta viendo a una textura (la reserva la primera vez; SoltarTextura la libera): la ultima imagen
    // de un corte, para fundirla sobre la nueva. Sus UV: (0, 0) a (UPantalla(), VPantalla()). Lee la VRAM con el SH-4:
    // unos milisegundos, una vez por corte
    bool CapturarPantalla(Textura& t);
    inline float UPantalla() { return 640.0f / 1024.0f; }
    inline float VPantalla() { return 480.0f / 512.0f; }

    // --- el estado de lo que sigue: un encabezado de la PVR ---
    enum Mezcla { Opaca = 0, Alfa, Suma, Multiplicar };
    enum FuncionZ { ZMayorIgual = 0, ZMayor, ZSiempre };
    struct Estado {
        const Textura* tex;   // NULL = sin textura (color de vertice)
        uint8_t mezcla;       // Mezcla (Opaca en la lista opaca / de recorte)
        uint8_t escribeZ;
        uint8_t funcionZ;     // FuncionZ (1/w: mas grande = mas cerca)
        uint8_t caras;        // 1 descartar las de atras (giro antihorario adelante, como GL); 2 las de adelante
        uint8_t niebla;       // la tabla de niebla (NieblaConstante)
        uint8_t suma;         // a cada vertice se le suma ColorSumado (despues de la textura)
        Estado() : tex(0), mezcla(Opaca), escribeZ(1), funcionZ(ZMayorIgual), caras(0), niebla(0), suma(0) {}
    };
    void Encabezado(const Estado& e);
    // el color que se suma a los vertices que siguen (con Estado::suma; 0 = nada): un destello sin tocar la textura
    void ColorSumado(uint32_t argb);

    // --- la matriz de pantalla: viewport * proyeccion * vista * modelo (4 x 4 por columnas, como GL); la pantalla
    //     va en pixeles con la y para abajo. PantallaDesde arma viewport * proyeccion para Matriz() ---
    void PantallaDesde(const float* proyeccion16, float* salida16);
    void Matriz(const float* m16);   // la carga en XMTRX
    // la matriz por k: la pantalla no cambia (x/w, y/w) y la profundidad (1/w) se divide por k. Apenas menos de 1
    // acerca sin mover nada (las calcomanias); FONDO manda detras de todo lo demas (lo que en GL se dibuja primero sin
    // escribir la profundidad, pero que aca va en la translucida, despues de lo opaco). El plano cercano no se escala
    enum { FONDO = 1 << 14 };
    void MatrizEscalada(const float* m16, float k);
    float WCercano();                 // el w minimo (el plano cercano: 0.05 por defecto)
    void SetWCercano(float w);

    // --- dibujar ---
    // tiras con indices: CORTE termina una y arranca la siguiente (a la PVR le cuesta solo el encabezado de la nueva;
    // unirlas con degenerados le costaba 2 o 3 vertices). pos: 3 floats; uv: 2 floats (NULL = 0,0); argb: un color
    // por vertice (NULL = 'fijo'). Cada indice se transforma con la matriz cargada. 'recortar': alguna puede cruzar
    // el plano cercano (recorta triangulo por triangulo; si no, rapido y sin preguntar)
    enum { CORTE = 0xFFFF };
    void Tiras(const float* pos, const float* uv, const uint32_t* argb, uint32_t fijo, const uint16_t* idx, int n,
               bool recortar);
    // vertices YA transformados (los de un personaje: se transforman una vez y se dibujan en dos pasadas): x, y de
    // pantalla y z = 1/w, o w <= 0 (detras del plano cercano: sus triangulos no se dibujan)
    struct Vertice { float x, y, z, w; };
    // ('adelante': el que llama ya sabe que ninguno quedo detras; si no, se fija aca)
    void TirasTransformadas(const Vertice* v, const float* uv, uint32_t argb, const uint16_t* idx, int n,
                            bool adelante = false);
    // un cuadro 2D en pixeles (x0, y0)-(x1, y1) con sus UV y color, delante de todo
    void Cuadro2D(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t argb,
                  float z = 1e4f);
    // las cuatro esquinas (x, y) en orden 0 1 / 2 3 (para girarlo)
    void Cuadro2DEsquinas(const float* xy8, const float* uv8, uint32_t argb, float z = 1e4f);

    // --- niebla ---
    void NieblaConstante(float alfa, float r = 0, float g = 0, float b = 0);

    // --- lo que se mando en el cuadro (para el perfil) ---
    struct Cuenta { int vertices, recortados, encabezados; };
    Cuenta CuentaCuadro();
    // lo que ocupo el buffer de vertices de la PVR el ultimo cuadro y el que mas (bytes): si pasa el de Iniciar, la
    // PVR pierde poligonos
    void BufferVertices(int& ultimo, int& maximo);

    // 1/w para w > 0 con FSRRA (1/raiz de w*w, un ciclo del SH-4) en vez de FDIV (unos 10): la division por w de cada
    // vertice. Error ~1/2^21: nada en la pantalla
    inline float Inversa(float w) {
        float r = w * w;
        __asm__("fsrra %0" : "+f"(r));
        return r;
    }

    inline uint32_t ARGB(float r, float g, float b, float a) {
        int R = (int)(r * 255.0f + 0.5f), G = (int)(g * 255.0f + 0.5f), B = (int)(b * 255.0f + 0.5f),
            A = (int)(a * 255.0f + 0.5f);
        R = R < 0 ? 0 : (R > 255 ? 255 : R); G = G < 0 ? 0 : (G > 255 ? 255 : G);
        B = B < 0 ? 0 : (B > 255 ? 255 : B); A = A < 0 ? 0 : (A > 255 ? 255 : A);
        return ((uint32_t)A << 24) | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
    }
}

#endif // W3D_DREAMCAST
