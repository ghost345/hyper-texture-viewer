cmake_minimum_required(VERSION 3.20)
project(HyperTextureViewer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C runtime: the exe runs without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Flags for this project's own targets only: third-party warnings stay out of the log, and libavif's
# /source-charset:utf-8 clashes with a global /utf-8.
if(MSVC)
    add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    set(htv_options /W4 /utf-8 /permissive-)
else()
    set(htv_options -Wall -Wextra)
endif()
# Sanitizers for this project's code (including the single-header decoders in image.cpp), e.g. "address,undefined".
# The fetched libraries stay uninstrumented: their own sanitizer findings are theirs to fix.
set(HTV_SANITIZE "" CACHE STRING "Comma-separated -fsanitize= list for the loaders and tests")
if(HTV_SANITIZE)
    list(APPEND htv_options -fsanitize=${HTV_SANITIZE} -fno-sanitize-recover=all)
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

option(HTV_WITH_JXL "Build the JPEG XL decoder (libjxl)" ON)
option(HTV_WITH_KTX "Build the KTX/KTX2 decoder (libktx with Basis Universal)" ON)
option(HTV_WITH_AVIF "Build the AVIF decoder (libavif with dav1d; needs meson and nasm)" ON)

# Third-party test suites stay out of the build: a normal variable shadows the cache entry while they configure.
set(BUILD_TESTING OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(HTV_WITH_JXL)
    set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_JPEGLI OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_DOXYGEN OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_JNI OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_TRANSCODE_JPEG OFF CACHE BOOL "" FORCE)
    set(JPEGXL_ENABLE_SKCMS OFF CACHE BOOL "" FORCE)  # lcms2 instead; both are bundled
    set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL "" FORCE)
    # JPEGXL_STATIC would inject "-Wl,-Bdynamic" into MinGW links; BUILD_SHARED_LIBS OFF is enough.
    set(JPEGXL_STATIC OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libjxl
        GIT_REPOSITORY https://github.com/libjxl/libjxl.git
        GIT_TAG v0.11.1
        GIT_SHALLOW TRUE
        GIT_SUBMODULES third_party/brotli third_party/highway third_party/lcms)
    FetchContent_MakeAvailable(libjxl)
endif()

if(HTV_WITH_KTX)
    set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_LOADTEST_APPS "" CACHE STRING "" FORCE)
    set(KTX_FEATURE_ETC_UNPACK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(ktx
        GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
        GIT_TAG v4.4.2
        GIT_SHALLOW TRUE
        GIT_SUBMODULES "")
    FetchContent_MakeAvailable(ktx)
    # Basis Universal is only transcoded to BC formats here; drop the mobile targets from the transcoder.
    target_compile_definitions(ktx_read PRIVATE BASISD_SUPPORT_PVRTC1=0 BASISD_SUPPORT_PVRTC2=0 BASISD_SUPPORT_ATC=0
        BASISD_SUPPORT_ASTC=0 BASISD_SUPPORT_ETC2_EAC_A8=0 BASISD_SUPPORT_ETC2_EAC_RG11=0)
endif()

if(HTV_WITH_AVIF)
    # dav1d builds only with meson (and nasm for its assembly), so it is an external project feeding libavif.
    find_program(HTV_MESON meson)
    find_program(HTV_NINJA NAMES ninja ninja-build)
    if(NOT HTV_MESON OR NOT HTV_NINJA)
        message(FATAL_ERROR "AVIF support needs meson and ninja (pip install meson ninja), or -DHTV_WITH_AVIF=OFF")
    endif()
    FetchContent_Declare(dav1d
        GIT_REPOSITORY https://github.com/videolan/dav1d.git
        GIT_TAG 1.5.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(dav1d)  # no CMakeLists.txt: this only fetches the source

    set(dav1d_install "${CMAKE_BINARY_DIR}/dav1d-install")
    set(dav1d_meson_args --buildtype=release --default-library=static --prefix=${dav1d_install} --libdir=lib
        -Denable_asm=true -Denable_tools=false -Denable_examples=false -Denable_tests=false)
    set(dav1d_env)
    if(MSVC)
        # Meson finds Visual Studio itself; /MT matches this project's static runtime.
        list(APPEND dav1d_meson_args --vsenv -Db_vscrt=static_from_buildtype)
    elseif(CMAKE_CROSSCOMPILING)
        set(dav1d_cross "${CMAKE_BINARY_DIR}/dav1d-cross.ini")
        file(WRITE ${dav1d_cross} "[binaries]\nc = '${CMAKE_C_COMPILER}'\nar = '${CMAKE_AR}'\n"
            "windres = '${CMAKE_RC_COMPILER}'\n[host_machine]\nsystem = 'windows'\ncpu_family = 'x86_64'\n"
            "cpu = 'x86_64'\nendian = 'little'\n")
        list(APPEND dav1d_meson_args --cross-file=${dav1d_cross})
    else()
        set(dav1d_env CC=${CMAKE_C_COMPILER})
    endif()
    include(ExternalProject)
    ExternalProject_Add(dav1d_build
        SOURCE_DIR ${dav1d_SOURCE_DIR}
        BINARY_DIR ${CMAKE_BINARY_DIR}/dav1d-build
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${dav1d_env} ${HTV_MESON} setup ${dav1d_meson_args} <BINARY_DIR> <SOURCE_DIR>
        BUILD_COMMAND ${HTV_NINJA} -C <BINARY_DIR>
        INSTALL_COMMAND ${HTV_NINJA} -C <BINARY_DIR> install
        BUILD_BYPRODUCTS ${dav1d_install}/lib/libdav1d.a
        LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON)
    file(MAKE_DIRECTORY ${dav1d_install}/include)
    add_library(dav1d::dav1d STATIC IMPORTED GLOBAL)
    set_target_properties(dav1d::dav1d PROPERTIES IMPORTED_LOCATION ${dav1d_install}/lib/libdav1d.a)
    target_include_directories(dav1d::dav1d INTERFACE ${dav1d_install}/include)
    add_dependencies(dav1d::dav1d dav1d_build)

    set(AVIF_CODEC_DAV1D SYSTEM CACHE STRING "" FORCE)  # the dav1d::dav1d target above
    set(AVIF_LIBYUV OFF CACHE STRING "" FORCE)
    set(AVIF_LIBSHARPYUV OFF CACHE STRING "" FORCE)
    set(AVIF_LIBXML2 OFF CACHE STRING "" FORCE)
    set(AVIF_ZLIBPNG OFF CACHE STRING "" FORCE)
    set(AVIF_JPEG OFF CACHE STRING "" FORCE)
    set(AVIF_BUILD_APPS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libavif
        GIT_REPOSITORY https://github.com/AOMediaCodec/libavif.git
        GIT_TAG v1.4.2
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(libavif)
endif()

unset(BUILD_TESTING)

# Loaders and their third-party decoders, kept free of Windows APIs so tests run anywhere.
find_package(Threads REQUIRED)
add_library(htv_image STATIC src/image.cpp src/psd.cpp src/apng.cpp src/metadata.cpp src/stats.cpp)
target_compile_options(htv_image PRIVATE ${htv_options})
target_include_directories(htv_image PUBLIC src third_party)
target_sources(htv_image PRIVATE src/codecs_modern.cpp)
target_link_libraries(htv_image PUBLIC Threads::Threads PRIVATE webpdemux webp)
if(HTV_WITH_JXL)
    target_compile_definitions(htv_image PRIVATE HTV_HAVE_JXL)
    target_link_libraries(htv_image PRIVATE jxl jxl_threads)
endif()
if(HTV_WITH_KTX)
    target_compile_definitions(htv_image PRIVATE HTV_HAVE_KTX)
    target_link_libraries(htv_image PRIVATE ktx_read)  # the read-only library, without the encoders
endif()
if(HTV_WITH_AVIF)
    target_compile_definitions(htv_image PRIVATE HTV_HAVE_AVIF)
    target_link_libraries(htv_image PRIVATE avif)
endif()
if(MSVC)
    # Third-party headers are compiled inside image.cpp; keep their warnings out of the log.
    set_source_files_properties(src/image.cpp PROPERTIES COMPILE_OPTIONS "/W1")
endif()

if(WIN32)
    # Dear ImGui with its Win32 and Direct3D 11 backends. The backend's D3DCompile calls are renamed so they go
    # through the viewer's lazily loaded d3dcompiler_47.dll instead of adding it to the import table.
    add_library(htv_imgui STATIC
        third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_win32.cpp
        third_party/imgui/backends/imgui_impl_dx11.cpp)
    target_include_directories(htv_imgui PUBLIC third_party/imgui)
    target_compile_definitions(htv_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS
        PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD D3DCompile=htvD3DCompile UNICODE _UNICODE)
    target_link_libraries(htv_imgui PUBLIC d3d11 dwmapi imm32)

    # Interface fonts (Inter, JetBrains Mono) are linked in as resources.
    configure_file(src/resources.rc.in ${CMAKE_BINARY_DIR}/resources.rc @ONLY)
    add_executable(hyper-texture-viewer WIN32 src/main.cpp src/render.cpp src/files.cpp src/ui.cpp src/ui_widgets.cpp
        src/ui_bars.cpp src/ui_views.cpp src/ui_panels.cpp src/ui_settings.cpp src/ui_compare.cpp src/ui_frames.cpp src/keys.cpp src/icons.cpp src/wic_loader.cpp ${CMAKE_BINARY_DIR}/resources.rc)
    target_compile_options(hyper-texture-viewer PRIVATE ${htv_options})
    target_compile_definitions(hyper-texture-viewer PRIVATE UNICODE _UNICODE HTV_VERSION="${PROJECT_VERSION}")
    target_link_libraries(hyper-texture-viewer PRIVATE htv_image htv_imgui d3d11 dxgi dwmapi shlwapi windowscodecs ole32
        comdlg32)
    if(MINGW)
        target_link_options(hyper-texture-viewer PRIVATE -static -s)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(htv_tests tests/test_image.cpp)
    target_compile_options(htv_tests PRIVATE ${htv_options})
    if(HTV_SANITIZE)
        target_link_options(htv_tests PRIVATE -fsanitize=${HTV_SANITIZE})
    endif()
    # The tests encode WebP samples, so they link libwebp's encoder too.
    target_link_libraries(htv_tests PRIVATE htv_image webp)
    if(HTV_WITH_JXL)
        target_compile_definitions(htv_tests PRIVATE HTV_HAVE_JXL)
        target_link_libraries(htv_tests PRIVATE jxl)
    endif()
    if(HTV_WITH_KTX)
        target_compile_definitions(htv_tests PRIVATE HTV_HAVE_KTX)
        target_link_libraries(htv_tests PRIVATE ktx)
    endif()
    if(HTV_WITH_AVIF)
        # libavif's own test images; the tests have no AV1 encoder to make samples with.
        target_compile_definitions(htv_tests PRIVATE HTV_HAVE_AVIF HTV_AVIF_TEST_DATA="${libavif_SOURCE_DIR}/tests/data")
    endif()
    if(MINGW)
        target_link_options(htv_tests PRIVATE -static)
    endif()
    add_test(NAME image_loaders COMMAND htv_tests)
endif()
