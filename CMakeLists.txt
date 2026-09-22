# autobleem-pc-tools: the Windows console-stick installer (AutoBleemInstaller), the Windows-product setup
# (AutoBleemWinSetup) and UpdateRoms. Win32 programs (no SDL of their own) that link ab_core from the
# autobleem-core submodule. Cross-compiled UCRT with llvm-mingw in the autobleem-build image (see build.yml).
cmake_minimum_required(VERSION 3.12)
project(autobleem-pc-tools)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
add_compile_definitions(AB_PLATFORM_DEV)

set(AB_BUILD_TESTS OFF CACHE BOOL "" FORCE)   # the tools' tests stay with the core repo for now
set(AB_CORE_UI OFF CACHE BOOL "" FORCE)       # SDL-free: the Win32 tools need only ab_core, and llvm-mingw
                                              # has no UCRT SDL2 - so core builds ableem_engine + ab_core only
add_subdirectory(autobleem-core)

add_subdirectory(apps/updateroms)
add_subdirectory(apps/installer)
