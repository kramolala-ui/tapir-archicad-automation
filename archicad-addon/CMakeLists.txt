cmake_minimum_required (VERSION 3.17)

include (Tools/CMakeCommon.cmake)

set_property (GLOBAL PROPERTY USE_FOLDERS ON)

set (CMAKE_CONFIGURATION_TYPES Debug;Release;RelWithDebInfo)
set (AC_API_DEVKIT_DIR ${AC_API_DEVKIT_DIR} CACHE PATH "API DevKit directory.")
set (AC_VERSION ${API_AC_VERSION} CACHE STRING "Archicad version.")

SetGlobalCompilerDefinitions (${AC_VERSION})

if (WIN32)
	set (NamePostFix Win)
else ()
	set (NamePostFix Mac)
endif ()
set (AddOnName "TapirAddOn_AC${AC_VERSION}_${NamePostFix}")

project (${AddOnName})

set (AddOnSourcesFolder Sources)
set (AddOnResourcesFolder Sources)
GenerateAddOnProject (AddOn ${AC_VERSION} ${AC_API_DEVKIT_DIR} ${AddOnName} ${AddOnSourcesFolder} ${AddOnResourcesFolder} INT)


# =============================================================================
#  Third-party libraries (vendored via FetchContent)
# =============================================================================
#
# zstd — сжатие для bulk-транспорта (см. BulkCommands.cpp и
# AI_PRINCIPLES.md, раздел 4). Вендоринг через FetchContent, а не
# vcpkg/brew, чтобы аддон собирался «из коробки».

include (FetchContent)

set (ZSTD_BUILD_PROGRAMS    OFF CACHE BOOL "" FORCE)
set (ZSTD_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set (ZSTD_BUILD_CONTRIB     OFF CACHE BOOL "" FORCE)
set (ZSTD_BUILD_STATIC      ON  CACHE BOOL "" FORCE)
set (ZSTD_BUILD_SHARED      OFF CACHE BOOL "" FORCE)
set (ZSTD_LEGACY_SUPPORT    OFF CACHE BOOL "" FORCE)

FetchContent_Declare (
    zstd
    URL          https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
    URL_HASH     SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
    # В корне репозитория zstd лежит «SDK»-CMakeLists, который НЕ
    # объявляет библиотечные таргеты. Рабочий проект — в build/cmake,
    # и именно его надо подключать как subdirectory.
    SOURCE_SUBDIR build/cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable (zstd)

# =============================================================================
#  msgpack-cxx (header-only, C++17)
# =============================================================================
# Для шага 3 bulk-транспорта: msgpack-cxx — header-only, линковать нечего,
# только include-путь. FetchContent со SOURCE_SUBDIR в корне, потому что
# у msgpack-cxx CMakeLists.txt в корне репозитория (не как у zstd).

FetchContent_Declare (
    msgpack_cxx
    URL          https://github.com/msgpack/msgpack-c/releases/download/cpp-6.1.1/msgpack-cxx-6.1.1.tar.gz
    URL_HASH     SHA256=7165f3aa966e8e4a9889e0e7a2174f92b8f1cf3dc1be0fd2c2c2a5c9bbdd41df
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable (msgpack_cxx)

if (TARGET msgpack-cxx)
    target_link_libraries (AddOn msgpack-cxx)
elseif (TARGET msgpackc-cxx)
    target_link_libraries (AddOn msgpackc-cxx)
else ()
    # Fallback: только include-путь (msgpack-cxx — header-only)
    target_include_directories (AddOn SYSTEM PRIVATE
        ${msgpack_cxx_SOURCE_DIR}/include)
endif ()

# Имя static-таргета в zstd менялось между версиями. Пробуем
# несколько вариантов; если ни один не найден — печатаем список
# всех доступных таргетов для диагностики.
if (TARGET libzstd_static)
    set (ZSTD_LINK_TARGET libzstd_static)
elseif (TARGET zstd_static)
    set (ZSTD_LINK_TARGET zstd_static)
elseif (TARGET libzstd)
    set (ZSTD_LINK_TARGET libzstd)
elseif (TARGET zstd)
    set (ZSTD_LINK_TARGET zstd)
else ()
    message (STATUS "zstd: доступные CMake-таргеты после FetchContent:")
    get_property (_zstd_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach (_t IN LISTS _zstd_targets)
        message (STATUS "  target: ${_t}")
    endforeach ()
    message (FATAL_ERROR "zstd: не найден CMake target после FetchContent.")
endif ()

# ВАЖНО: Graphisoft CMakeCommon.cmake использует plain signature
# (target_link_libraries без PRIVATE/PUBLIC). CMake запрещает смешивать
# plain и keyworded для одного target'а — поэтому и здесь без ключевых
# слов. Include-пути добавляем отдельной директивой (она к линковке
# не относится и ключевые слова поддерживает).
target_link_libraries (AddOn ${ZSTD_LINK_TARGET})
target_include_directories (AddOn SYSTEM PRIVATE
    ${zstd_SOURCE_DIR}/lib
    ${zstd_SOURCE_DIR}/lib/common
    ${zstd_SOURCE_DIR}/lib/dict
    ${zstd_SOURCE_DIR}/lib/compress
    ${zstd_SOURCE_DIR}/lib/decompress
)
