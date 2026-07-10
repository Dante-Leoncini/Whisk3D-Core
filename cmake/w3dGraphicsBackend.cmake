# ============================================================================
#  Backend grafico + dependencias nativas de Whisk3DCore, por plataforma.
#  El CMakeLists raiz lo incluye y llama:  w3d_configure_graphics_backend(<target>)
#
#  NOTA: este archivo se agrego en el PR de soporte macOS ("Delegate graphics
#  backend configuration to Core"), pero el commit del submodulo Core (e22ed865)
#  nunca se pusheo al remoto de Whisk3Dcore -> RECONSTRUIDO aca a partir de la
#  config que antes vivia inline en el CMakeLists raiz (misma logica Win/Linux/
#  Android) + la rama Apple. Si soykhaler pushea el commit real del Core, este
#  archivo se reemplaza por ese.
# ============================================================================

function(w3d_configure_graphics_backend TARGET)
    if(WIN32)
        # OpenGL de escritorio (Windows)
        target_compile_definitions(${TARGET} PRIVATE W3D_OPENGL)
        target_link_libraries(${TARGET} PRIVATE opengl32 glu32)
        target_compile_definitions(${TARGET} PRIVATE SDL_MAIN_HANDLED)

    elseif(APPLE)
        # OpenGL de escritorio (macOS). GL/GLU vienen en el framework OpenGL (deprecado pero funcional).
        find_package(OpenGL REQUIRED)
        target_compile_definitions(${TARGET} PRIVATE W3D_OPENGL)
        target_link_libraries(${TARGET} PRIVATE OpenGL::GL)
        if(TARGET OpenGL::GLU)
            target_link_libraries(${TARGET} PRIVATE OpenGL::GLU)
        endif()

    elseif(ANDROID)
        # GLES 1.x (el build real de Android usa Android.mk/ndk-build; esto es por si se compila via CMake)
        target_compile_definitions(${TARGET} PRIVATE W3D_GLES1)

    else()
        # Linux / otros UNIX: OpenGL de escritorio
        find_package(OpenGL REQUIRED)
        target_link_libraries(${TARGET} PRIVATE
            pthread
            dl
            OpenGL::GL
            GLU
        )
        target_compile_definitions(${TARGET} PRIVATE W3D_OPENGL)
    endif()
endfunction()
