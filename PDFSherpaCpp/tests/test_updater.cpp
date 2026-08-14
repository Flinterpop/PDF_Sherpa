// Updater logic.
//
// These are pinned by tests because the update path is the one thing that
// cannot be safely tried out: getting it wrong republishes an asset that every
// existing install polls for, and the failure lands on users rather than here.
// MDBoss learned each of these against a real release, with every unit test
// passing while the update was broken.

#include <filesystem>
#include <fstream>
#include <string>

#include <process.h>

#include <catch2/catch_test_macros.hpp>

#include "Updater.h"

namespace fs = std::filesystem;
using namespace pdfsherpa;

TEST_CASE("version tags parse the way the Python app parses them", "[updater]")
{
    REQUIRE(parse_version("1.3.12").has_value());
    CHECK(*parse_version("1.3.12") == std::vector<int>{1, 3, 12});
    CHECK(*parse_version("v1.3.12") == std::vector<int>{1, 3, 12});
    CHECK(*parse_version("V2.0") == std::vector<int>{2, 0});

    // A pre-release tag is not a version; the check must ignore it rather
    // than offering a beta to everyone.
    CHECK_FALSE(parse_version("1.3.12-beta").has_value());
    CHECK_FALSE(parse_version("nightly").has_value());
    CHECK_FALSE(parse_version("").has_value());
    CHECK_FALSE(parse_version("v").has_value());
    CHECK_FALSE(parse_version("1..2").has_value());
}

TEST_CASE("newer-version comparison handles differing lengths", "[updater]")
{
    CHECK(is_newer({1, 3, 13}, {1, 3, 12}));
    CHECK(is_newer({1, 4}, {1, 3, 99}));      // 1.4 > 1.3.99
    CHECK(is_newer({2, 0, 0}, {1, 99, 99}));
    CHECK_FALSE(is_newer({1, 3, 12}, {1, 3, 12}));
    CHECK_FALSE(is_newer({1, 3}, {1, 3, 0}));  // 1.3 == 1.3.0, not newer
    CHECK_FALSE(is_newer({1, 3, 11}, {1, 3, 12}));
    // The case that matters most: the running version must never think a
    // release equal to itself is an update, or every launch nags.
    CHECK_FALSE(is_newer({1, 3, 12}, {1, 3, 12}));
}

TEST_CASE("install scope follows where the exe actually lives", "[updater]")
{
    // A per-machine install must re-run the installer with /ALLUSERS.  Without
    // it the silent re-run takes the per-machine default and plants a SECOND
    // copy beside the per-user one.
    CHECK(install_scope_flag("C:\\Program Files\\PDF Sherpa\\PDFSherpa.exe") ==
          "/ALLUSERS");
    CHECK(install_scope_flag(
              "C:\\Program Files (x86)\\PDF Sherpa\\PDFSherpa.exe") ==
          "/ALLUSERS");
    CHECK(install_scope_flag(
              "C:\\Users\\someone\\AppData\\Local\\Programs\\PDF Sherpa\\"
              "PDFSherpa.exe") == "/CURRENTUSER");
    CHECK(install_scope_flag("D:\\portable\\PDFSherpa.exe") == "/CURRENTUSER");
}

TEST_CASE("portable detection keys on the uninstaller beside the exe",
          "[updater]")
{
    const fs::path dir = fs::temp_directory_path() /
                         ("sherpa_upd_" + std::to_string(_getpid()));
    std::error_code ec;
    fs::create_directories(dir, ec);

    const fs::path exe = dir / "PDFSherpa.exe";
    std::ofstream(exe, std::ios::binary) << "stub";

    // No uninstaller: a loose copy, so updates swap the exe in place.
    CHECK(running_portable(exe));

    std::ofstream(dir / "unins000.exe", std::ios::binary) << "stub";
    // Inno Setup's uninstaller is present, so this is an install and updates
    // must go through the installer instead.
    CHECK_FALSE(running_portable(exe));

    fs::remove_all(dir, ec);
}

TEST_CASE("the installer batch waits, installs with the right scope, relaunches",
          "[updater]")
{
    const std::string batch = installer_batch(
        "C:\\Temp\\PDFSherpa-Setup.exe",
        "C:\\Program Files\\PDF Sherpa\\PDFSherpa.exe", 4242);

    // Waits on the PID, not the image name: a second build of the same app
    // sitting open would otherwise burn the whole timeout.
    CHECK(batch.find("PID eq 4242") != std::string::npos);
    // Absolute System32 paths.  Git for Windows puts a GNU find on PATH that
    // fails on these arguments, and that failure reads as "already exited",
    // so the install races the still-running app.
    CHECK(batch.find("%SystemRoot%\\System32\\find.exe") != std::string::npos);
    CHECK(batch.find("%SystemRoot%\\System32\\tasklist.exe") != std::string::npos);
    // ping, not timeout: with no console, timeout exits immediately with
    // "Input redirection is not supported" and the wait collapses.
    CHECK(batch.find("PING.EXE") != std::string::npos);
    CHECK(batch.find("timeout") == std::string::npos);

    CHECK(batch.find("/VERYSILENT") != std::string::npos);
    CHECK(batch.find("/ALLUSERS") != std::string::npos);
    CHECK(batch.find("start \"\" \"C:\\Program Files\\PDF Sherpa\\PDFSherpa.exe\"") !=
          std::string::npos);
}

TEST_CASE("the portable batch refuses to copy a zip without the exe",
          "[updater]")
{
    const std::string batch =
        portable_batch("C:\\Temp\\PDFSherpa-Portable.zip", "C:\\Temp\\stage",
                       "D:\\portable\\PDFSherpa.exe", 99);

    // Both layouts the Python app accepts: the exe at the zip root, or one
    // folder down.
    CHECK(batch.find("\\PDFSherpa.exe\" set \"_src=") != std::string::npos);
    CHECK(batch.find("\\PDFSherpa\\PDFSherpa.exe\" set \"_src=") !=
          std::string::npos);

    // The no-brick guarantee: with no exe found, _src stays empty and the
    // copy is skipped, but the relaunch still runs.  A failed update must be
    // a no-op, never a wiped install.
    CHECK(batch.find("if \"%_src%\"==\"\" goto psrelaunch") != std::string::npos);
    CHECK(batch.find(":psrelaunch") != std::string::npos);
    CHECK(batch.find("robocopy.exe") != std::string::npos);
    CHECK(batch.find("start \"\" \"D:\\portable\\PDFSherpa.exe\"") !=
          std::string::npos);
}

TEST_CASE("asset names match what existing installs poll for", "[updater]")
{
    // Changing either of these strands every installed copy: the updater looks
    // for exactly these names on the release, and nothing else is offered.
    CHECK(std::string(kSetupAssetName) == "PDFSherpa-Setup.exe");
    CHECK(std::string(kPortableAssetName) == "PDFSherpa-Portable.zip");
}
