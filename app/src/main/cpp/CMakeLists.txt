cmake_minimum_required(VERSION 3.22.1)
project(hifiengine LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(oboe REQUIRED CONFIG)

# dr_libs (dominio público): se descargan una vez. Puedes versionarlas en third_party/ y quitar el bloque.
set(DR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dr_libs)
foreach(h dr_flac.h dr_wav.h)
    if(NOT EXISTS ${DR_DIR}/${h})
        message(STATUS "Descargando ${h}")
        file(DOWNLOAD https://raw.githubusercontent.com/mackron/dr_libs/master/${h}
             ${DR_DIR}/${h} TLS_VERIFY ON STATUS dl_status)
    endif()
endforeach()

add_library(hifiengine SHARED
    jni_bridge.cpp
    AudioEngine.cpp
    PcmSource.cpp)

target_include_directories(hifiengine PRIVATE ${DR_DIR})
target_compile_options(hifiengine PRIVATE -O3 -Wall -Wno-unused-function)
target_link_libraries(hifiengine oboe::oboe log)
# Android 15+: páginas de 16 KB
target_link_options(hifiengine PRIVATE -Wl,-z,max-page-size=16384)
