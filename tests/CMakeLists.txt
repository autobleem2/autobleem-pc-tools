# The PC tools' unit tests, on autobleem-core's harness (doctest, EnvFixture, TempDir, ab_add_test).
include(${CMAKE_SOURCE_DIR}/autobleem-core/tests/harness.cmake)

ab_add_test(test_updateroms_core apps/test_updateroms_core.cpp LIBS updateroms_core)
ab_add_test(test_installer_core apps/test_installer_core.cpp LIBS installer_core)
ab_add_test(test_windows_install_job apps/test_windows_install_job.cpp LIBS installer_core)
