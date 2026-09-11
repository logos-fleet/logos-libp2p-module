cmake_minimum_required(VERSION 3.14)
project(Libp2pModulePlugin LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# WHERE TinyCBOR COMES FROM. The nim-ffi header (lib/libp2p.h) includes
# <tinycbor/cbor.h> and its CBOR marshalling calls into TinyCBOR. Natively that
# is the `tinycbor` nix.packages.runtime dependency. On a CROSS-BUILT Bare
# module it cannot be: nixpkgs' tinycbor does not build for iOS at all (its own
# iOS stdenv, which logos-nix deliberately does not use), and on Android a
# separate shared library beside the module would be an unbundled soname. So
# nix/mobile-cbind.nix compiles nim-ffi's VENDORED TinyCBOR for the target and
# adds it to the same archive as cbind — one library, already linked, and
# naming it again here would only ask the linker for a second one that is not
# there.
set(LIBP2P_MODULE_LINK_LIBRARIES tinycbor)
if(LOGOS_MODULE_BARE AND CMAKE_CROSSCOMPILING)
    set(LIBP2P_MODULE_LINK_LIBRARIES "")
endif()

logos_module(
    NAME libp2p_module
    SOURCES
        src/plugin.h
        src/utils.h
        src/utils.cpp
        src/topic_queues.h
        src/topic_queues.cpp
        src/stream_queues.h
        src/stream_queues.cpp
        src/plugin.cpp
        src/callbacks.cpp
        src/kademlia.cpp
        src/stream.cpp
        src/protocol_bridge.cpp
        src/gossipsub.cpp
        src/service_discovery.cpp
        src/custom_handlers.cpp
        src/peerstore.cpp
        src/metrics.cpp
    EXTERNAL_LIBS
        libp2p
    LINK_LIBRARIES
        ${LIBP2P_MODULE_LINK_LIBRARIES}
)

# A Bare build defines no plugin target and no Qt: logos_module() routes to
# logos_bare_module() and RETURNS, so everything below names things that do not
# exist. Measured as "Cannot specify link libraries for target
# libp2p_module_module_plugin which is not built by this project" on the
# aarch64-android leg — the module could not produce a Bare artifact at all.
if(NOT LOGOS_MODULE_BARE)
    target_link_libraries(libp2p_module_module_plugin PUBLIC nlohmann_json::nlohmann_json)

    add_subdirectory(tutorial)
endif()
