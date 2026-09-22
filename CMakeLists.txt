# autobleem-pc-tools: the Windows console-stick installer (AutoBleemInstaller), the Windows-product setup
# (AutoBleemWinSetup) and UpdateRoms. Win32 programs (no SDL of their own) that link ab_core from the
# autobleem-core submodule. Built on Windows (MSYS2) or cross with mingw.
cmake_minimum_required(VERSION 3.12)
project(autobleem-pc-tools)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(AB_PLATFORM_DEV)
set(AB_BUILD_TESTS OFF CACHE BOOL "" FORCE)   # the tools' tests stay with the core repo for now
add_subdirectory(autobleem-core)

add_subdirectory(apps/updateroms)
add_subdirectory(apps/installer)
