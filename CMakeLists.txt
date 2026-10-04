cmake_minimum_required(VERSION 3.18)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED DMOD_MODULE_VERSION)
    set(DMOD_MODULE_VERSION "0.1" CACHE STRING "DMOD module version")
endif()

# If a local dmod checkout is provided, use it instead of fetching from git
if(DEFINED DMOD_DIR AND NOT "${DMOD_DIR}" STREQUAL "")
    set(FETCHCONTENT_SOURCE_DIR_DMOD "${DMOD_DIR}" CACHE PATH "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    dmod
    GIT_REPOSITORY https://github.com/choco-technologies/dmod.git
    GIT_TAG        develop
)

set(DMOD_MODE "DMOD_MODULE" CACHE STRING "DMOD build mode")
set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")

# project() before FetchContent_MakeAvailable(dmod): the compiler test runs
# before dmod's tools-cfg.cmake adds its include directories (ARG_MAX on CI)
project(todmvs VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "Pages to dmview views (.dmvs)"
    LANGUAGES C CXX)

FetchContent_MakeAvailable(dmod)

set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory")
set(DMOD_SCRIPTS_DIR ${DMOD_DIR}/scripts CACHE PATH "DMOD scripts directory")
include(${DMOD_DIR}/paths.cmake)
dmod_setup_external_module()

# ======================================================================
#               libtodmvs - document -> .dmvs library
# ======================================================================
# Writes the views of the documents dmvsi converters make of pages (HTML,
# ...), with their fonts and images. Usable on a PC (at build time) and on
# a device.
set(DMOD_MODULE_NAME        libtodmvs)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         8192)
set(DMOD_PRIORITY           0)
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/libtodmvs.dmr)
set(DMOD_MAL_IMPLS          )
set(DMOD_DIF_IMPLS          )

dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/write.c
    src/shadow.c
    src/assets.c
    src/out.c
)

# dmvsi: the documents and their converters
dmod_link_modules(${DMOD_MODULE_NAME}
    dmvsi@>=0.2
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

enable_testing()
add_subdirectory(tests)

# ======================================================================
#               todmvs - command-line tool on top of libtodmvs
# ======================================================================
add_subdirectory(apps/todmvs)
