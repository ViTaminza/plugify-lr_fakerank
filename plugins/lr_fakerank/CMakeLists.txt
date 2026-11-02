# lr_fakerank — Plugify C++ port of the Pisex LR module "[LR] FakeRank"
# (the original LR_FakeRanks module, Metamod plugin lr_fakerank).
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The config is read
# by the plugin's own KV1 reader, and the map gate, the client/controller checks and the schema
# offset cache live in src/plugin.cpp - the engine-free parts of them in src/fakerank_core.*.
#
#   lr_fakerank_core       engine-free core (the fakerank.ini model + KV1 reader, the desired
#                          controller values, the Tab/reveal state machine and the map-gated schema
#                          offset cache; also used by tests/lr_fakerank_tests.cpp)
#   plugify-lr_fakerank    the plugin shared library (entry "bin/lr_fakerank")

set(LR_FAKERANK_VERSION "1.1.0")

# --- core ---------------------------------------------------------------------
add_library(lr_fakerank_core STATIC src/fakerank_core.cpp)
target_include_directories(lr_fakerank_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Engine-free: no plugify/s2sdk dependency (the tests link only this target).
target_compile_options(lr_fakerank_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_fakerank_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated levels_ranks bindings -------------------------------------------
# The __levels_ranks_* globals have to be compiled into every consumer plugin; plugins/levels_ranks
# builds them as the object library `lr_bindings` (it sorts before this directory), this is the
# standalone fallback for an exported repository (which has no sibling CMakeLists.txt).
if(TARGET lr_bindings)
    set(LR_FAKERANK_LR_BINDING_TARGET lr_bindings)
elseif(EXISTS
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    add_library(lr_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(lr_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(lr_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(LR_FAKERANK_LR_BINDING_TARGET lr_bindings)
else()
    message(FATAL_ERROR "lr_fakerank needs the generated levels_ranks consumer bindings (plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp)")
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-lr_fakerank SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:${LR_FAKERANK_LR_BINDING_TARGET}>
)
target_include_directories(plugify-lr_fakerank PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources.
target_include_directories(plugify-lr_fakerank SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-lr_fakerank PRIVATE
        plugify_sdk
        lr_fakerank_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-lr_fakerank PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-lr_fakerank PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="lr_fakerank"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-lr_fakerank PROPERTIES OUTPUT_NAME lr_fakerank)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-lr_fakerank PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/lr_fakerank.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lr_fakerank.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Engine-free tests (the shipped fakerank.ini through the plugin's own KV1 reader, the config
# fallback, the reveal state machine and the map-gated schema offset cache); build/tests so
# ../test.sh runs them. Nothing here links pisexcompat, plugify_sdk or an engine binding.
add_executable(lr_fakerank_tests tests/lr_fakerank_tests.cpp)
target_link_libraries(lr_fakerank_tests PRIVATE lr_fakerank_core -static-libstdc++ -static-libgcc)
target_compile_options(lr_fakerank_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_fakerank_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        LR_FAKERANK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(lr_fakerank_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
