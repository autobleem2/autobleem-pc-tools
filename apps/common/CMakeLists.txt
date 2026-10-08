# ui_theme: the PC installers' shared dark look (graphite window, owner-drawn buttons and checkboxes, cyan
# progress bars, Red Hat Text, the hero) - design: autobleem-design themes/ab2.0.0/design/pcinstall. Windows only;
# each program that uses it embeds the two fonts and its own hero in its .rc under the ids of ui_theme_ids.h
# (the fonts: resources/RedHatText-*.ttf, SIL OFL - the licence text resources/RedHatText-OFL.txt goes along
# with every exe that embeds them).

if (WIN32)
    add_library(ui_theme STATIC ui_theme.cpp ui_theme.h ui_theme_ids.h)
    target_include_directories(ui_theme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ui_theme PUBLIC comctl32 user32 gdi32 gdiplus ole32)
endif()

# channel_choice: the channels the Installer and the Flasher list, from the site's channels.json (any platform -
# the tests in tests/apps run it with a fake downloader)
add_library(channel_choice STATIC channel_choice.cpp channel_choice.h)
target_include_directories(channel_choice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(channel_choice ab_installer)

# install_args: the Installer's command line into InstallOptions (any platform - tested in tests/apps)
add_library(install_args STATIC install_args.cpp install_args.h)
target_include_directories(install_args PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(install_args ab_installer)
