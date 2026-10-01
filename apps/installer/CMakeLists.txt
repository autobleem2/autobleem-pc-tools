# The installer: AutoBleem onto a PlayStation Classic stick from a PC - the stick's file system from the
# package, the rest from the download repository. The job - InstallerJob, WindowsInstallJob - is
# autobleem-core's ab_installer (moved there 2026-09-23 so the console's own updater in the launcher runs the
# same code; its suites run there); installer is the program over it: a plain Win32 window on Windows (no
# SDL, no AutoBleem theme - one self-contained exe, like UpdateRoms), the --quiet console path elsewhere.
# Built for the dev hosts only: it is what runs on the PC the stick is plugged into.

add_executable(installer
        src/main.cpp
        src/win32_platform.cpp
        src/win32_platform.h
        src/win32_window.cpp
        src/win32_window.h
)
set_target_properties(installer PROPERTIES OUTPUT_NAME AutoBleemInstaller)
target_link_libraries(installer channel_choice ab_installer Threads::Threads ${CMAKE_DL_LIBS})

if (WIN32)
    # a GUI-subsystem exe (no console window when double-clicked; --quiet attaches to the parent's), the
    # manifest and the picture as resources, and everything static: nothing of MSYS2 is needed next to it
    # installer_exe.rc: the manifest, the icon, the shared look's fonts (ui_theme) and this program's hero
    target_sources(installer PRIVATE src/installer_exe.rc)
    set_source_files_properties(src/installer_exe.rc PROPERTIES OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/hero-installer.png;${CMAKE_CURRENT_SOURCE_DIR}/ui/hero-installer@2x.png;${CMAKE_CURRENT_SOURCE_DIR}/../common/icons/AutoBleemInstaller.ico;${CMAKE_CURRENT_SOURCE_DIR}/../common/resources/RedHatText-Medium.ttf;${CMAKE_CURRENT_SOURCE_DIR}/../common/resources/RedHatText-SemiBold.ttf")
    target_link_libraries(installer ui_theme comctl32 user32 gdi32 gdiplus ole32 wininet)
    # the fonts' licence (SIL OFL) next to the exe, and so in the release bundle (make_installer_bundle.sh)
    add_custom_command(TARGET installer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/../common/resources/RedHatText-OFL.txt $<TARGET_FILE_DIR:installer>)
    target_link_options(installer PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
endif()

# README.txt next to the binary: the folder for a release is the exe, that file and the package
add_custom_command(TARGET installer PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/resources $<TARGET_FILE_DIR:installer>)

# AutoBleemWinSetup: the Windows product's data tree (windows_install_job) behind the same kind of window -
# what the NSIS installer runs once the program is in place, and the Start Menu's "AutoBleem Setup"
add_executable(winsetup
        src/main_winsetup.cpp
        src/win32_platform.cpp
        src/win32_platform.h
        src/win32_setup_window.cpp
        src/win32_setup_window.h
)
set_target_properties(winsetup PROPERTIES OUTPUT_NAME AutoBleemWinSetup)
target_link_libraries(winsetup ab_installer Threads::Threads ${CMAKE_DL_LIBS})
if (WIN32)
    # winsetup_exe.rc: the manifest, the icon, the shared look's fonts (ui_theme) and this program's hero
    target_sources(winsetup PRIVATE src/winsetup_exe.rc)
    set_source_files_properties(src/winsetup_exe.rc PROPERTIES OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/hero-winsetup.png;${CMAKE_CURRENT_SOURCE_DIR}/ui/hero-winsetup@2x.png;${CMAKE_CURRENT_SOURCE_DIR}/../common/icons/AutoBleemWinSetup.ico;${CMAKE_CURRENT_SOURCE_DIR}/../common/resources/RedHatText-Medium.ttf;${CMAKE_CURRENT_SOURCE_DIR}/../common/resources/RedHatText-SemiBold.ttf")
    target_link_libraries(winsetup ui_theme comctl32 user32 gdi32 gdiplus ole32 shell32 wininet)
    # the fonts' licence (SIL OFL) next to the exe
    add_custom_command(TARGET winsetup POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/../common/resources/RedHatText-OFL.txt $<TARGET_FILE_DIR:winsetup>)
    target_link_options(winsetup PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
endif()
