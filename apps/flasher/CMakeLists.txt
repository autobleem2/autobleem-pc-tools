# The flasher: the PC USB stick's image onto a stick, from Windows. The job - FlasherJob - is
# autobleem-core's ab_installer (its suite runs there, over a disk in memory); this is the program over it:
# the raw disk (win32_disk.*), a plain Win32 window in the installer's look, and a --quiet line. It shares
# the installer's WinINet downloader (apps/installer/src/win32_platform.*). requireAdministrator in its
# manifest - writing \\.\PhysicalDriveN cannot be done otherwise (the owner's one exception, 2026-09-23).

add_executable(flasher
        src/main.cpp
        src/flasher_window.cpp
        src/flasher_window.h
        src/win32_disk.cpp
        src/win32_disk.h
        ../installer/src/win32_platform.cpp
        ../installer/src/win32_platform.h
)
set_target_properties(flasher PROPERTIES OUTPUT_NAME AutoBleemFlasher)
target_link_libraries(flasher channel_choice ab_installer Threads::Threads ${CMAKE_DL_LIBS})

if (WIN32)
    # flasher_exe.rc: the manifest, the icon, the shared look's fonts (ui_theme) and this program's hero
    target_sources(flasher PRIVATE src/flasher_exe.rc)
    set_source_files_properties(src/flasher_exe.rc PROPERTIES OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/hero-flasher.png;${CMAKE_CURRENT_SOURCE_DIR}/ui/hero-flasher@2x.png;${CMAKE_CURRENT_SOURCE_DIR}/../common/icons/AutoBleemFlasher.ico;${CMAKE_CURRENT_SOURCE_DIR}/../common/resources/RedHatText-Medium.ttf;${CMAKE_CURRENT_SOURCE_DIR}/../common/resources/RedHatText-SemiBold.ttf")
    target_link_libraries(flasher ui_theme comctl32 comdlg32 user32 gdi32 gdiplus ole32 wininet)
    # the fonts' licence (SIL OFL) next to the exe
    add_custom_command(TARGET flasher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/../common/resources/RedHatText-OFL.txt $<TARGET_FILE_DIR:flasher>)
    target_link_options(flasher PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
endif()

# README.txt next to the binary: the release folder is the exe and that file
add_custom_command(TARGET flasher PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/resources $<TARGET_FILE_DIR:flasher>)
