// The version-lockstep guard.
//
// One version number has to move across five files in a single commit:
// app/Version.h, CMakeLists.txt, installer-cpp.iss, and the two deprecated
// Python-side files.  release.ps1 bumps all of them, but a hand-edit of any
// one is exactly the sort of half-applied change that ships a binary whose
// About box, installer and update check disagree about what it is.
//
// This pins the two the build can see.  The .iss and the Python files are
// covered by release.ps1 failing loudly when a pattern is not found.

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Version.h"

TEST_CASE("Version.h and the CMake project version agree", "[version]")
{
    // SHERPA_CMAKE_VERSION comes from ${PROJECT_VERSION}.
    CHECK(std::string(pdfsherpa::kAppVersion) == std::string(SHERPA_CMAKE_VERSION));
}

TEST_CASE("the version is a plain three-part number", "[version]")
{
    // The updater parses this with parse_version() and compares it against the
    // release tag; anything it cannot parse would make every launch either
    // silently skip the check or offer an update forever.
    const std::string version = pdfsherpa::kAppVersion;
    REQUIRE_FALSE(version.empty());

    int dots = 0;
    for (const char c : version) {
        if (c == '.') {
            ++dots;
        } else {
            CHECK(c >= '0');
            CHECK(c <= '9');
        }
    }
    CHECK(dots == 2);
}
