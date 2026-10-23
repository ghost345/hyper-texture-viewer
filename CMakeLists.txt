cmake_minimum_required(VERSION 3.20)
project(HyperTextureViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C runtime: the exe runs without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive-)
    add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
else()
    add_compile_options(-Wall -Wextra)
endif()

# libwebp: only the libraries, none of its command-line tools.
include(FetchContent)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libwebp
    GIT_REPOSITORY https://github.com/webmproject/libwebp.git
    GIT_TAG v1.5.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libwebp)

# Loaders and their third-party decoders, kept free of Windows APIs so tests run anywhere.
find_package(Threads REQUIRED)
add_library(htv_image STATIC src/image.cpp)
target_include_directories(htv_image PUBLIC src third_party)
target_link_libraries(htv_image PUBLIC Threads::Threads PRIVATE webpdemux webp)
if(MSVC)
    # Third-party headers are compiled inside image.cpp; keep their warnings out of the log.
    set_source_files_properties(src/image.cpp PROPERTIES COMPILE_OPTIONS "/W1")
endif()

if(WIN32)
    add_executable(hyper-texture-viewer WIN32 src/main.cpp src/wic_loader.cpp)
    target_compile_definitions(hyper-texture-viewer PRIVATE UNICODE _UNICODE)
    target_link_libraries(hyper-texture-viewer PRIVATE htv_image d3d11 dxgi shlwapi windowscodecs ole32)
    if(MINGW)
        target_link_options(hyper-texture-viewer PRIVATE -static -s)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(htv_tests tests/test_image.cpp)
    # The tests encode WebP samples, so they link libwebp's encoder too.
    target_link_libraries(htv_tests PRIVATE htv_image webp)
    if(MINGW)
        target_link_options(htv_tests PRIVATE -static)
    endif()
    add_test(NAME image_loaders COMMAND htv_tests)
endif()
