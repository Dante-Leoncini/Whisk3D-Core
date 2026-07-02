# Whisk3D Core
Whisk3D Core es la capa de abstracción gráfica multiplataforma que impulsa todo el ecosistema de Whisk3D.

Escrito en C++, proporciona una API unificada que permite ejecutar la misma base de código sobre diferentes APIs gráficas y sistemas operativos. Su objetivo es ofrecer una arquitectura ligera, portable y modular, inspirada en la filosofía de RenderWare, separando el motor de la API gráfica subyacente.

Whisk3D Core incluye los componentes fundamentales de un motor 3D, entre ellos:

Abstracción del pipeline de renderizado.
Representación de mallas y geometría.
Materiales y texturas.
Cámaras y administración de vistas.
Sistema de iluminación.
Objetos de escena y transformaciones.
Buffers de vértices e índices.
Estados básicos de renderizado.
Utilidades matemáticas (vectores, matrices y cuaterniones).

El proyecto está diseñado para facilitar la implementación de nuevos backends gráficos (como OpenGL, OpenGL ES, Vulkan, Direct3D, Metal o renderizadores por software) sin necesidad de modificar el código de alto nivel del motor.

Sus principales objetivos son:

Compatibilidad multiplataforma.
Independencia de la API gráfica.
Arquitectura limpia y modular.
Ligero y fácil de portar.
Compatible con computadoras de escritorio, dispositivos móviles, sistemas embebidos y plataformas retro.

Whisk3D Core constituye la base del motor y del editor Whisk3D, pero también puede utilizarse como un framework de renderizado independiente para otros proyectos.
