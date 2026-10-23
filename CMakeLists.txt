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

# Loaders and their third-party decoders, kept free of Windows APIs so tests run anywhere.
find_package(Threads REQUIRED)
add_library(htv_image STATIC src/image.cpp)
target_include_directories(htv_image PUBLIC src third_party)
target_link_libraries(htv_image PUBLIC Threads::Threads)
if(MSVC)
    # Third-party headers are compiled inside image.cpp; keep their warnings out of the log.
    set_source_files_properties(src/image.cpp PROPERTIES COMPILE_OPTIONS "/W1")
endif()

if(WIN32)
    add_executable(hyper-texture-viewer WIN32 src/main.cpp)
    target_compile_definitions(hyper-texture-viewer PRIVATE UNICODE _UNICODE)
    target_link_libraries(hyper-texture-viewer PRIVATE htv_image d3d11 dxgi shlwapi)
    if(MINGW)
        target_link_options(hyper-texture-viewer PRIVATE -static -s)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(htv_tests tests/test_image.cpp)
    target_link_libraries(htv_tests PRIVATE htv_image)
    if(MINGW)
        target_link_options(htv_tests PRIVATE -static)
    endif()
    add_test(NAME image_loaders COMMAND htv_tests)
endif()
