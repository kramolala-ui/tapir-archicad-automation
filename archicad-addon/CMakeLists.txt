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
#  nlohmann/json (header-only, C++17)
# =============================================================================
# API для сборки/парсинга JSON-структур, которые затем кодируются в msgpack
# через json::to_msgpack / json::from_msgpack. Формат на проводе остаётся
# msgpack — Python-клиент не меняется. Зато C++-код становится читаемым,
# и уходит C2766.
#
# Почему не msgpack-cxx напрямую: на MSVC wchar_t ≡ unsigned short,
# msgpack-cxx делает дублирующие explicit-специализации
# convert/pack/object<wchar_t> и <unsigned short> → C2766 при сборке.
# У msgpack-cxx НЕТ макроса это отключить (проверено). nlohmann/json
# работает через std::string — проблемы нет.
FetchContent_Declare (nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable (nlohmann_json)

target_link_libraries (AddOn nlohmann_json::nlohmann_json)

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
