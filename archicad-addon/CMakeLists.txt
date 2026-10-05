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
    URL      https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
    URL_HASH SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable (zstd)

# Имя static-таргета в zstd менялось между версиями.
if (TARGET libzstd_static)
    set (ZSTD_LINK_TARGET libzstd_static)
elseif (TARGET zstd_static)
    set (ZSTD_LINK_TARGET zstd_static)
elseif (TARGET zstd)
    set (ZSTD_LINK_TARGET zstd)
else ()
    message (FATAL_ERROR "zstd: не найден CMake target после FetchContent.")
endif ()

target_link_libraries (AddOn PRIVATE ${ZSTD_LINK_TARGET})
target_include_directories (AddOn SYSTEM PRIVATE ${zstd_SOURCE_DIR}/lib)
