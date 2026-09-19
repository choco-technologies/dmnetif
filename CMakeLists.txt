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

FetchContent_MakeAvailable(dmod)

project(dmnetif VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "dmnetif DMOD library module"
    LANGUAGES C CXX)

set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory")
set(DMOD_SCRIPTS_DIR ${DMOD_DIR}/scripts CACHE PATH "DMOD scripts directory")
include(${DMOD_DIR}/paths.cmake)
dmod_setup_external_module()

# Name of the module
set(DMOD_MODULE_NAME        dmnetif)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmnetif.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

# List of DIF interfaces implemented by this module (leave empty if none)
set(DMOD_DIF_IMPLS          )

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    src/dmnetif.c
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# dmroute: a separately published module, fetched via dmod_link_modules()
# (which downloads its headers via dmf-get at configure time).
#   dmroute_addr_t: the type dmnetif's IP/netmask/broadcast fields use
#   dmnetif_set_ip_address() also calls dmroute_add()/_remove() directly
#   to keep an interface's connected route in sync - see
#   update_connected_route() in src/dmnetif.c
# dmlist: backs the interface registry
# dmdrvi: DMDRVI_IOCTL_NET_* commands/types used to talk to a registered
#         interface's underlying device file
# libsystemd: every interface is reported as a "netif" class device, so a
#         device rule can start one service instance per interface (see
#         notify_iface_added() in src/dmnetif.c). A real dependency, not an
#         optional integration: dmnetif is simply not started without it.
# dmini:  dmdrvi.h includes dmini.h (dmini_context_t appears in dmdrvi_create's
#         signature) - needed transitively even though dmnetif never calls it
# dmosi:  mutex guarding the registry
dmod_link_modules(${DMOD_MODULE_NAME}
    libsystemd
    dmroute
    dmlist
    dmdrvi
    dmini
    dmosi
)

add_subdirectory(tests)
