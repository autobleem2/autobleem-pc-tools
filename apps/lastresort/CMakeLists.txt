# LastResortRecovery: a PlayStation Classic that no longer starts, brought back from Windows - its own
# LBOOT.EPB written back over USB with Android's fastboot client (platform-tools, shipped in the zip next to
# the exe - tools/fetch_platform_tools.sh). lastresort_core is the job (portable, tested from tests/apps);
# lastresort the program: a Win32 window in the installer's look, the WinUSB driver for the console's
# fastboot mode (signed on the spot with libwdi's pki.c, third_party/libwdi, LGPL-3.0), and --quiet.

add_library(lastresort_core STATIC
        src/core/fastboot.cpp
        src/core/fastboot.h
        src/core/lboot_image.cpp
        src/core/lboot_image.h
        src/core/recovery_job.cpp
        src/core/recovery_job.h
)
target_include_directories(lastresort_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lastresort_core ab_installer)

add_executable(lastresort
        src/main.cpp
        src/recovery_window.cpp
        src/recovery_window.h
        src/win32_console.cpp
        src/win32_console.h
        ../installer/src/win32_platform.cpp
        ../installer/src/win32_platform.h
)
set_target_properties(lastresort PROPERTIES OUTPUT_NAME LastResortRecovery)
target_link_libraries(lastresort lastresort_core Threads::Threads ${CMAKE_DL_LIBS})

if (WIN32)
    # libwdi's catalog making and signing, and the little it needs from the rest of libwdi - vendored C,
    # built as it is (warnings off, like the other vendored code)
    add_library(lastresort_pki STATIC
            third_party/libwdi/pki.c
            third_party/libwdi/pki_glue.c
    )
    target_include_directories(lastresort_pki PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwdi)
    target_compile_options(lastresort_pki PRIVATE -w)
    target_compile_definitions(lastresort_pki PRIVATE _CRT_SECURE_NO_WARNINGS)

    target_sources(lastresort PRIVATE src/lastresort.rc)
    target_link_libraries(lastresort lastresort_pki comctl32 comdlg32 user32 gdi32 gdiplus ole32 shell32
            setupapi newdev cfgmgr32 wininet)
    target_link_options(lastresort PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
endif()

# README.txt next to the binary; platform-tools/ joins it at packaging time
add_custom_command(TARGET lastresort PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/resources $<TARGET_FILE_DIR:lastresort>)
