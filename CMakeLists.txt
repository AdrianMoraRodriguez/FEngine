cmake_minimum_required(VERSION 3.24)

project(FEngine
    VERSION 0.1.0
    DESCRIPTION "A 3D game engine with a scene editor"
    LANGUAGES CXX)

# Estándar del lenguaje

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Salida de binarios
# Todos los ejecutables al mismo sitio, para que las rutas relativas a
# assets funcionen igual en Debug y en Release.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# Opciones del compilador

if(MSVC)
    add_compile_options(
        /W4              # nivel de aviso alto
        /permissive-     # conformidad estricta con el estándar
        /utf-8           # fuentes y ejecución en UTF-8
        /MP              # compilación en paralelo
    )
endif()

# Subproyectos

add_subdirectory(engine)
add_subdirectory(editor)
