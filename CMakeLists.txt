# autobleem-pc-tools: the Windows console-stick installer (AutoBleemInstaller), the Windows-product setup
# (AutoBleemWinSetup) and UpdateRoms. Win32 programs (no SDL of their own) that link ab_core from the
# autobleem-core submodule. Cross-compiled UCRT with llvm-mingw in the autobleem-build image (see build.yml).
cmake_minimum_required(VERSION 3.12)
project(autobleem-pc-tools)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

# AB_TARGET: dev (the default) or win. A cache variable, so the core submodule reads the same value; the
# macro is defined here too because a directory-scoped add_compile_definitions in the submodule does not
# reach these sibling app directories.
set(AB_TARGET "dev" CACHE STRING "dev | win")
if (AB_TARGET STREQUAL "")
    set(AB_TARGET "dev" CACHE STRING "dev | win" FORCE)
endif()
if (NOT AB_TARGET STREQUAL "dev" AND NOT AB_TARGET STREQUAL "win")
    message(FATAL_ERROR "AB_TARGET must be dev or win for the PC tools (got '${AB_TARGET}')")
endif()
string(TOUPPER "${AB_TARGET}" _ab_target_upper)
add_compile_definitions(AB_PLATFORM_${_ab_target_upper})

set(AB_BUILD_TESTS OFF CACHE BOOL "" FORCE)   # the core's own suites run in the core repo
set(AB_CORE_UI OFF CACHE BOOL "" FORCE)       # SDL-free: the Win32 tools need only ab_core, and llvm-mingw
                                              # has no UCRT SDL2 - so core builds ableem_engine + ab_core only
add_subdirectory(autobleem-core)

add_subdirectory(apps/updateroms)
add_subdirectory(apps/installer)

# the tools' own suites (tests/), on the core's harness - run by the CI's native test job
option(AB_TOOL_TESTS "Build the PC tools' unit tests" ON)
if (AB_TOOL_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
