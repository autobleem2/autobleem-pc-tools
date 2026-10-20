# The PC tools' unit tests, on autobleem-core's harness (doctest, EnvFixture, TempDir, ab_add_test).
include(${CMAKE_SOURCE_DIR}/autobleem-core/tests/harness.cmake)

ab_add_test(test_updateroms_core apps/test_updateroms_core.cpp LIBS updateroms_core)
ab_add_test(test_lastresort_core apps/test_lastresort_core.cpp LIBS lastresort_core)
ab_add_test(test_lanshare_core apps/test_lanshare_core.cpp LIBS lanshare_core)
ab_add_test(test_channel_choice apps/test_channel_choice.cpp LIBS channel_choice)
ab_add_test(test_installer_args apps/test_installer_args.cpp LIBS installer_args)
ab_add_test(test_retroboot_offer apps/test_retroboot_offer.cpp LIBS retroboot_offer)
# the installer job's suites run in autobleem-core (ab_installer)

# all_tests: every test executable of this folder, however it was added - the CI build step builds this one target,
# so a new test is never left out of the build by a hand-kept list
get_directory_property(_ab_targets BUILDSYSTEM_TARGETS)
set(_ab_test_exes "")
foreach (_t ${_ab_targets})
    get_target_property(_type ${_t} TYPE)
    if (_type STREQUAL "EXECUTABLE")
        list(APPEND _ab_test_exes ${_t})
    endif ()
endforeach ()
add_custom_target(all_tests DEPENDS ${_ab_test_exes})
