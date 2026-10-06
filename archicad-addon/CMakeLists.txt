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

# Используем GIT_REPOSITORY+GIT_TAG, а не URL+URL_HASH: релизные tarball'ы
# msgpack-cxx пересобираются (SHA256 меняется между прогонами), git-тег
# стабилен. GIT_SHALLOW — тянем только коммит тега, без истории.
#
# SOURCE_SUBDIR=include — критично: у msgpack-cxx корневой CMakeLists.txt
# требует Boost (FIND_PACKAGE(Boost)). Он нам НЕ нужен — библиотека
# header-only, мы берём только include-пути. Указывая SOURCE_SUBDIR
# на папку без CMakeLists.txt, заставляем FetchContent_MakeAvailable
# только скачать исходник, не вызывая add_subdirectory.
FetchContent_Declare (
    msgpack_cxx
    GIT_REPOSITORY https://github.com/msgpack/msgpack-c.git
    GIT_TAG        cpp-6.1.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  include
)
FetchContent_MakeAvailable (msgpack_cxx)

target_include_directories (AddOn SYSTEM PRIVATE
    ${msgpack_cxx_SOURCE_DIR}/include)

# КРИТИЧНО: без MSGPACK_NO_BOOST msgpack/sysdep.hpp идёт в #else-ветку
# и включает <boost/predef/other/endian.h>. С макросом берётся
# вендоренный <msgpack/predef/other/endian.h> из самого репозитория —
# Boost нам не нужен. См. include/msgpack/sysdep.hpp, блок
# "#if defined(MSGPACK_NO_BOOST)".
target_compile_definitions (AddOn PRIVATE MSGPACK_NO_BOOST)

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
