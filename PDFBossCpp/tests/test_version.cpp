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

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Version.h"

TEST_CASE("Version.h and the CMake project version agree", "[version]")
{
    // PDFBOSS_CMAKE_VERSION comes from ${PROJECT_VERSION}.
    CHECK(std::string(pdfboss::kAppVersion) == std::string(PDFBOSS_CMAKE_VERSION));
}

TEST_CASE("PDFBoss.rc version tuples match Version.h", "[version]")
{
    // The .rc carries FILEVERSION / PRODUCTVERSION as comma-separated numeric
    // tuples, which cannot reference the string macro and so are rewritten
    // separately by release.ps1.  That is exactly the kind of second copy that
    // drifts, and the symptom is an installed exe whose Explorer properties
    // disagree with its own About box.
    const std::filesystem::path rc =
        std::filesystem::path(PDFBOSS_APP_DIR) / "PDFBoss.rc";
    REQUIRE(std::filesystem::exists(rc));

    std::ifstream stream(rc, std::ios::binary);
    REQUIRE(stream);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();

    // "2.0.1" -> "2,0,1,0"
    std::string expected = pdfboss::kAppVersion;
    for (char& c : expected) {
        if (c == '.') {
            c = ',';
        }
    }
    expected += ",0";

    for (const char* key : {"FILEVERSION", "PRODUCTVERSION"}) {
        const std::regex pattern(std::string(key) +
                                 R"(\s+([0-9]+,[0-9]+,[0-9]+,[0-9]+))");
        std::smatch match;
        INFO("looking for " << key << " in PDFBoss.rc");
        REQUIRE(std::regex_search(text, match, pattern));
        CHECK(match[1].str() == expected);
    }
}

TEST_CASE("the version is a plain three-part number", "[version]")
{
    // The updater parses this with parse_version() and compares it against the
    // release tag; anything it cannot parse would make every launch either
    // silently skip the check or offer an update forever.
    const std::string version = pdfboss::kAppVersion;
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
