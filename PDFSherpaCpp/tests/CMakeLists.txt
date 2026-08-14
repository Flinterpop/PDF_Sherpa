find_package(Catch2 3 CONFIG REQUIRED)

add_executable(sherpa_tests
    test_metadata.cpp
    test_config.cpp
    test_favorites_file.cpp
    test_pdf_document.cpp
    test_tocgen.cpp
    test_source_literals.cpp
    test_updater.cpp
    test_version.cpp
)

target_link_libraries(sherpa_tests PRIVATE sherpa_core Catch2::Catch2WithMain)

# fixture.pdf is a committed, synthetic two-page document (generated once by
# make_fixture.py, which is kept beside it for regeneration).  It is committed
# rather than built because:
#   - it makes the test build hermetic, with no Python or PyMuPDF needed to
#     run ctest;
#   - it is 2 KB and never changes;
#   - it contains nothing controlled.  The real corpus this app is used
#     against lives in C:\ICD and is export-controlled; no test may depend on
#     it, and this repo is public.
target_compile_definitions(sherpa_tests PRIVATE
    SHERPA_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixture.pdf"
    # test_source_literals scans the app sources as text, so it needs to find
    # them regardless of where the build tree is.
    SHERPA_APP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../app"
    # So test_version can assert Version.h and project(VERSION) agree.
    SHERPA_CMAKE_VERSION="${PROJECT_VERSION}")

include(Catch)
catch_discover_tests(sherpa_tests)
