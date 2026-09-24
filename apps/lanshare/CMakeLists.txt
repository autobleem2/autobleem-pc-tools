# AutoBleem LAN Share (docs/lan-share-plan.md): the owner's PS1 games shared with the AutoBleem Store on the home
# network - core's LanServer with a window - and a PS1 disc read from the PC's drive into a library (core's
# DiscReader over WinCdDrive, the CD-ROM IOCTLs). lanshare_core (the settings) builds everywhere, for the tests;
# the program is Windows only, like UpdateRoms a GUI-subsystem exe, statically linked, one file.

add_library(lanshare_core STATIC
        src/core/lanshare_settings.cpp
        src/core/lanshare_settings.h
)
target_include_directories(lanshare_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/code)
target_link_libraries(lanshare_core ab_core ableem_lanserver)

if (WIN32)
    add_executable(lanshare
            src/main.cpp
            src/win_cd_drive.cpp
            src/win_cd_drive.h
            src/win32_window.cpp
            src/win32_window.h
            src/lanshare.rc
    )
    set_target_properties(lanshare PROPERTIES OUTPUT_NAME LanShare)
    target_link_libraries(lanshare lanshare_core Threads::Threads ${CMAKE_DL_LIBS})
    target_link_libraries(lanshare comctl32 comdlg32 user32 gdi32 shell32 ole32 advapi32 iphlpapi ws2_32)
    target_link_options(lanshare PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
endif()
