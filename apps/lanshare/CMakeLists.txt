# AutoBleem LAN Share (docs/lan-share-plan.md): the owner's PS1 games shared with the AutoBleem Store on the home
# network - core's ableem_lanserver with a window - and a PS1 disc read from the PC's drive into the library
# (core's DiscReader over WinCdDrive, the CD-ROM IOCTLs). Windows only; like UpdateRoms a GUI-subsystem exe,
# statically linked, one file.

add_executable(lanshare
        src/main.cpp
        src/win_cd_drive.cpp
        src/win_cd_drive.h
)
set_target_properties(lanshare PROPERTIES OUTPUT_NAME LanShare)
target_include_directories(lanshare PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lanshare ab_core ableem_lanserver Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(lanshare comctl32 user32 gdi32 shell32 ws2_32)
target_link_options(lanshare PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
