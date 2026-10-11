cmake_minimum_required(VERSION 3.24)

project(Vyra VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    # Static CRT is not used: nvCOMP and Qt ship against the dynamic MSVC runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

option(VYRA_WITH_CUDA "Build the nvCOMP GPU backend" ON)
option(VYRA_BUILD_GUI "Build the Qt Widgets desktop app" ON)

include(FetchContent)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# CPU codecs (static). They provide the CPU fallback backend, CRC32 and the
# streaming decoders for archives produced by other tools.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(ZLIB_COMPAT OFF CACHE BOOL "" FORCE)
set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
set(WITH_GZFILEOP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlibng
    GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
    GIT_TAG 2.2.4
    GIT_SHALLOW TRUE)

set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_Declare(lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.10.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake)

# The LZMA SDK (public domain) from Igor Pavlov's official 7-Zip mirror:
# only the LZMA/LZMA2 decoders and branch filters are compiled, so Vyra can
# open ordinary .7z archives (whose headers are LZMA compressed).
FetchContent_Declare(lzmasdk
    GIT_REPOSITORY https://github.com/ip7z/7zip.git
    GIT_TAG 24.09
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-configure)

FetchContent_MakeAvailable(zlibng lz4 zstd lzmasdk)

add_library(vyra_lzma STATIC
    ${lzmasdk_SOURCE_DIR}/C/LzmaDec.c
    ${lzmasdk_SOURCE_DIR}/C/Lzma2Dec.c
    ${lzmasdk_SOURCE_DIR}/C/Bra.c
    ${lzmasdk_SOURCE_DIR}/C/Bra86.c
    ${lzmasdk_SOURCE_DIR}/C/CpuArch.c)
target_include_directories(vyra_lzma PUBLIC ${lzmasdk_SOURCE_DIR}/C)
if(MSVC)
    target_compile_options(vyra_lzma PRIVATE /W1)
endif()

# ---------------------------------------------------------------------------
# nvCOMP GPU backend (optional)
# ---------------------------------------------------------------------------
if(VYRA_WITH_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(NOT CMAKE_CUDA_COMPILER)
        message(WARNING "No CUDA compiler found; building the CPU backend only.")
        set(VYRA_WITH_CUDA OFF)
    endif()
endif()
if(VYRA_WITH_CUDA)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        set(CMAKE_CUDA_ARCHITECTURES 75-real 86-real 89-real 120)
    endif()
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    find_package(CUDAToolkit REQUIRED)
    set(NVCOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.deps/nvidia/libnvcomp" CACHE PATH "nvCOMP installation root")
    # Only the GPU component is used. The Windows wheel's CMake package also
    # references a CPU import library that it does not ship.
    find_path(NVCOMP_INCLUDE_DIR nvcomp/deflate.h HINTS "${NVCOMP_ROOT}/include" REQUIRED)
    find_library(NVCOMP_LIBRARY NAMES nvcomp HINTS "${NVCOMP_ROOT}/lib" "${NVCOMP_ROOT}/lib64" REQUIRED)
    if(WIN32)
        find_file(NVCOMP_RUNTIME nvcomp64_5.dll HINTS "${NVCOMP_ROOT}/bin" REQUIRED)
    else()
        set(NVCOMP_RUNTIME "${NVCOMP_LIBRARY}")
    endif()
    add_library(vyra_nvcomp UNKNOWN IMPORTED)
    set_target_properties(vyra_nvcomp PROPERTIES
        IMPORTED_LOCATION "${NVCOMP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NVCOMP_INCLUDE_DIR}")
endif()

# ---------------------------------------------------------------------------
# Core library: archive formats, pipeline and codec backends. No Qt.
# ---------------------------------------------------------------------------
add_library(vyra_core STATIC
    src/core/archive.cpp
    src/core/archive.h
    src/core/backend.cpp
    src/core/backend.h
    src/core/bytes.h
    src/core/common.h
    src/core/cpubackend.cpp
    src/core/crc32.cpp
    src/core/crc32.h
    src/core/deflatestream.cpp
    src/core/deflatestream.h
    src/core/fileio.cpp
    src/core/fileio.h
    src/core/framing.cpp
    src/core/framing.h
    src/core/sevenz.cpp
    src/core/sevenz.h
    src/core/streamdecoder.cpp
    src/core/streamdecoder.h
    src/core/threadpool.cpp
    src/core/threadpool.h
    src/core/zip.cpp
    src/core/zip.h)
target_include_directories(vyra_core PUBLIC src)
target_link_libraries(vyra_core PRIVATE zlib lz4_static libzstd_static vyra_lzma PUBLIC Threads::Threads)
target_include_directories(vyra_core PRIVATE ${zstd_SOURCE_DIR}/lib ${lz4_SOURCE_DIR}/lib)
target_compile_definitions(vyra_core PUBLIC VYRA_VERSION="${PROJECT_VERSION}")
if(VYRA_WITH_CUDA)
    target_sources(vyra_core PRIVATE src/core/gpubackend.cu)
    target_compile_definitions(vyra_core PUBLIC VYRA_HAVE_CUDA=1)
    target_link_libraries(vyra_core PRIVATE CUDA::cudart_static vyra_nvcomp)
    if(MSVC)
        target_compile_options(vyra_core PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8,/Zc:preprocessor,/W3>)
        # Load nvCOMP lazily so the app still starts (CPU mode) when the DLL is absent.
        target_link_options(vyra_core INTERFACE /DELAYLOAD:nvcomp64_5.dll)
        target_link_libraries(vyra_core PUBLIC delayimp)
    endif()
endif()
if(MSVC)
    target_compile_options(vyra_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
    target_compile_definitions(vyra_core PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

function(vyra_copy_runtime target)
    if(VYRA_WITH_CUDA AND WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NVCOMP_RUNTIME}" "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Command-line tool
# ---------------------------------------------------------------------------
add_executable(vyra_cli src/cli/main.cpp)
set_target_properties(vyra_cli PROPERTIES OUTPUT_NAME vyra-cli)
target_link_libraries(vyra_cli PRIVATE vyra_core)
if(MSVC)
    target_compile_options(vyra_cli PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(vyra_cli PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
vyra_copy_runtime(vyra_cli)

# ---------------------------------------------------------------------------
# Desktop app
# ---------------------------------------------------------------------------
if(VYRA_BUILD_GUI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core Widgets)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets Network LinguistTools)
    add_executable(Vyra WIN32
        src/main.cpp
        src/app/actions.cpp
        src/app/actions.h
        src/app/app.cpp
        src/app/app.h
        src/app/archivemodel.cpp
        src/app/archivemodel.h
        src/app/compressdialog.cpp
        src/app/compressdialog.h
        src/app/dropview.cpp
        src/app/dropview.h
        src/app/jobrunner.cpp
        src/app/jobrunner.h
        src/app/mainwindow.cpp
        src/app/mainwindow.h
        src/app/progressdialog.cpp
        src/app/progressdialog.h
        src/app/qtbridge.h
        src/app/singleinstance.cpp
        src/app/singleinstance.h
        src/platform/shellintegration.cpp
        src/platform/shellintegration.h
        resources/resources.qrc)
    if(WIN32)
        target_sources(Vyra PRIVATE resources/windows.rc)
        target_link_libraries(Vyra PRIVATE Advapi32 Shell32)
    endif()
    target_link_libraries(Vyra PRIVATE vyra_core Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
    target_compile_definitions(Vyra PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
    if(MSVC)
        target_compile_options(Vyra PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(Vyra PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    # Korean translation: lupdate scans sources -> app_ko.ts, lrelease compiles
    # app_ko.qm next to the executable, loaded at runtime via QTranslator.
    qt6_add_translations(Vyra
        TS_FILES translations/app_ko.ts
        SOURCES src/app/actions.cpp src/app/app.cpp src/app/archivemodel.cpp src/app/compressdialog.cpp
                src/app/dropview.cpp src/app/mainwindow.cpp src/app/progressdialog.cpp
        QM_FILES_OUTPUT_VARIABLE ko_qm_files)
    add_dependencies(Vyra Vyra_lrelease)
    vyra_copy_runtime(Vyra)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_executable(vyra_tests tests/core_test.cpp)
    target_link_libraries(vyra_tests PRIVATE vyra_core)
    if(MSVC)
        target_compile_options(vyra_tests PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(vyra_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    vyra_copy_runtime(vyra_tests)
    add_executable(vyra_backend_bench tests/backend_bench.cpp)
    target_link_libraries(vyra_backend_bench PRIVATE vyra_core)
    add_test(NAME core COMMAND vyra_tests "${CMAKE_CURRENT_BINARY_DIR}/test-work")
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME interop COMMAND ${Python3_EXECUTABLE} -I
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop_test.py" "$<TARGET_FILE:vyra_cli>"
            "${CMAKE_CURRENT_BINARY_DIR}/interop-work")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS vyra_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(VYRA_BUILD_GUI)
    install(TARGETS Vyra RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${ko_qm_files} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(VYRA_WITH_CUDA AND WIN32)
    install(FILES "${NVCOMP_RUNTIME}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
