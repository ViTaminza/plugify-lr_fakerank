# lr_fakerank — Plugify C++ port of the Pisex LR module "[LR] FakeRank"
# (ref/src/cs2-lvl_ranks_modules/LR_FakeRanks, production plugin lr_fakerank).
#
#   lr_fakerank_core       engine-free core (the fakerank.ini model, the desired controller values,
#                          the Tab/reveal state machine; also used by tests/fakerank_tests.cpp)
#   plugify-lr_fakerank    the plugin shared library (entry "bin/lr_fakerank")

set(LR_FAKERANK_VERSION "1.0.0")

# --- core ---------------------------------------------------------------------
add_library(lr_fakerank_core STATIC src/fakerank_core.cpp)
target_include_directories(lr_fakerank_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# pisexcompat brings the KeyValues parser (and, PUBLIC-linked, the s2sdk/hudmenu globals).
target_link_libraries(lr_fakerank_core PUBLIC pisexcompat)
target_compile_options(lr_fakerank_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_fakerank_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated levels_ranks bindings -------------------------------------------
# The __levels_ranks_* globals have to be compiled into every consumer plugin; plugins/levels_ranks
# builds them as the object library `lr_bindings` (it sorts before this directory), this is the
# fallback for a build of this directory alone.
if(NOT TARGET lr_bindings AND EXISTS
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    add_library(lr_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(lr_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(lr_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-lr_fakerank SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:lr_bindings>
)
target_include_directories(plugify-lr_fakerank PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources.
target_include_directories(plugify-lr_fakerank SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-lr_fakerank PRIVATE
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
# Engine-free tests (the production fakerank.ini and the reveal state machine); build/tests so
# ../test.sh runs them.
add_executable(lr_fakerank_tests tests/fakerank_tests.cpp)
target_link_libraries(lr_fakerank_tests PRIVATE lr_fakerank_core -static-libstdc++ -static-libgcc)
target_compile_options(lr_fakerank_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_fakerank_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        LR_FAKERANK_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/configs/levels_ranks")
set_target_properties(lr_fakerank_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
