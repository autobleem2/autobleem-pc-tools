# The PC tools' unit tests, on autobleem-core's harness (doctest, EnvFixture, TempDir, ab_add_test).
include(${CMAKE_SOURCE_DIR}/autobleem-core/tests/harness.cmake)

ab_add_test(test_updateroms_core apps/test_updateroms_core.cpp LIBS updateroms_core)
ab_add_test(test_lastresort_core apps/test_lastresort_core.cpp LIBS lastresort_core)
# the installer job's suites run in autobleem-core (ab_installer)
