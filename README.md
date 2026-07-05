# Whisk3D Core

<p align="center">
    <img src="logo_outlined.svg" width="400" alt="Whisk3D logo">
</p>

## Motor multiplataforma 2D y 3D estilo retro

Whisk3D Core es un motor 3D multiplataforma. Es una biblioteca escrita en C++ para crear juegos y aplicaciones 2D/3D livianas que funcionan tanto en sistemas modernos como retro. Proporciona una API gráfica unificada que abstrae las diferencias entre las distintas APIs de renderizado y sistemas operativos, permitiendo desarrollar una vez y ejecutar en Windows, Linux, Android, Symbian y navegadores web (pueden sumarse mas sistemas en el futuro).

Su diseño prioriza la simplicidad, el rendimiento y la portabilidad, inspirándose en motores clásicos como RenderWare y siendo ideal para proyectos con estética retro o de bajos requisitos.

**NOTA:** El proyecto aun no esta listo para usar en produccion. se siguen haciendo cambios y reescrituras!. cualquier duda, pueden consultar en el grupo de [Telegram](https://t.me/Whisk3D)
Actualmente sigo limpiando el Core para:
* Simplificar el codigo... tiene mucha logica aun del Whisk3D Editor (modo edicion, modo objeto, modes de render, Mesh Editor, Modificadores. todo eso NO va en el Core)
* Pulir las animaciones
* Agregar un sistema de esqueletos
* Agregar algun sistema estilo BSP para mapas u otro sistema para mundos abiertos
* Algun sistema de fisicas... aunque eso capaz podria ser algo opcional al igual que los bsp
* Soportar otros sistemas y backend graficos retros.
* Agregar ejemplos y documentacion (los ejemplos se iran subiendo al repositorio [Whisk3D-Examples](https://github.com/Dante-Leoncini/Whisk3D-Examples))

## Capacidades actuales

Lo que el Core ya hace hoy:

* **Sistemas soportados:** Windows, Linux, Android, Symbian y navegadores web (WebGL). El mismo codigo base corre en todos; cada plataforma solo aporta su arranque de ventana/contexto.
* **Backends graficos:** un backend de *pipeline fijo* (OpenGL de escritorio y OpenGL ES 1.1 para Android/Symbian) y un backend de *shaders* (OpenGL ES 2.0 / WebGL). Se elige en tiempo de compilacion y ambos implementan la misma API de estado, asi que el codigo de alto nivel no cambia. (Vulkan y DirectX quedan para mas adelante.)
* **API grafica unificada (sin OpenGL a la vista):** toda la app dibuja a traves de `w3dEngine` (estados de render, matrices, arrays de vertices, materiales, niebla, blending, scissor, lineas y puntos...) sin llamar nunca a OpenGL directo ni depender de sus headers.
* **Carga de texturas:** `w3dEngine::LoadTexture(path, ...)` abre una imagen del disco y la sube como textura (decodifica con stb en PC/Android e ICL en Symbian; el upload es comun). Tambien `DecodeImage` para obtener los pixeles RGBA crudos (sprites, etc.), con filtrado lineal o nearest.
* **Sistema de luces:** clase `Light` con luces direccional, puntual y spot (cono), atenuacion configurable y componentes ambient/diffuse/specular. Hasta 8 luces por escena.
* **Camara:** clase `CameraBase` reutilizable (posicion, orientacion por cuaternion, FOV, planos near/far) que entrega su matriz de vista y de proyeccion perspectiva al render.
* **Sistema de log:** log de diagnostico comun a las plataformas, con tres niveles (info / warning / error). En escritorio y Symbian escribe a un archivo con su etiqueta; en WebGL cada nivel va a `console.log` / `console.warn` / `console.error`. Estilo printf y se puede apagar por completo en builds de release.
* **Matematica:** vectores, matrices 4x4 y cuaterniones propios (C puro, compilan igual en PC y en sistemas retro).
* **Escena y geometria:** mallas con vertices/normales/UV/color por vertice, materiales, grafo de objetos con transformaciones y jerarquia (padre/hijo), y animaciones.

Whisk3D Core incluye los componentes fundamentales de un motor 3D, entre ellos:

* Abstracción del pipeline de renderizado.
* Representación de mallas y geometría.
* Materiales y texturas.
* Cámaras y administración de vistas.
* Sistema de iluminación.
* Objetos de escena y transformaciones.
* Buffers de vértices e índices.
* Estados básicos de renderizado.
* Utilidades matemáticas (vectores, matrices y cuaterniones).

El proyecto está diseñado para facilitar la implementación de nuevos backends gráficos (como OpenGL, OpenGL ES, WebGl, Vulkan, Direct3D, Metal o renderizadores por software) sin necesidad de modificar el código de alto nivel del motor.

## Sus principales objetivos son:

* Compatibilidad multiplataforma.
* Independencia de la API gráfica.
* Arquitectura limpia y modular.
* Ligero y fácil de portar.
* Compatible con computadoras de escritorio, dispositivos móviles, sistemas embebidos y plataformas retro.

Whisk3D Core constituye la base del motor y del editor Whisk3D, pero también puede utilizarse como un framework de renderizado independiente para otros proyectos.

El código esta bajo licencia MIT. asi que podes usuarlo en tus propios proyectos ya sean libres o comerciales. podes hacer un fork. modificarlo etc.
