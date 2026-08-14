// Check GitHub for a newer release, download it, and hand off to an installer
// or an in-place exe swap.
//
// Ported from app.py's _start_update_check / _fetch_latest_release /
// _prompt_update / _begin_download / _launch_installer_and_exit /
// _spawn_handoff_batch / _swap_portable_and_exit, with the batch-file and
// install-scope logic taken from MDBossCpp/app/Updater.cpp rather than
// re-derived -- see the comments there and in the notes below, all of which
// record bugs that only showed up against a real published release.

#ifndef PDFSHERPA_APP_UPDATER_H
#define PDFSHERPA_APP_UPDATER_H

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pdfsherpa {

// The two asset names published on every release.  Old installs poll for
// exactly these, which is why the C++ app inherits them rather than picking
// new ones: republishing either name is what migrates existing users.
inline constexpr const char* kSetupAssetName = "PDFSherpa-Setup.exe";
inline constexpr const char* kPortableAssetName = "PDFSherpa-Portable.zip";
inline constexpr const char* kReleaseApiUrl =
    "https://api.github.com/repos/Flinterpop/PDF_Sherpa/releases/latest";
inline constexpr const char* kReleasesPageUrl =
    "https://github.com/Flinterpop/PDF_Sherpa/releases/latest";

struct ReleaseInfo {
    std::vector<int> version;   // parsed tag, e.g. {1, 3, 12}
    std::string version_str;    // the tag with any leading v stripped
    std::string setup_url;      // installer asset, or a fallback .exe
    std::string portable_url;   // portable zip asset, empty if absent
    std::string html_url;       // the release page, for "What's new"
};

// 'v1.3.3' / '1.3.3' -> {1,3,3}; nullopt for anything else (a beta tag, say).
std::optional<std::vector<int>> parse_version(const std::string& text);

// True when `candidate` is strictly newer than `current`.
bool is_newer(const std::vector<int>& candidate, const std::vector<int>& current);

// True when `app_exe` is a loose portable copy rather than an installed one.
//
// The test is the absence of Inno Setup's uninstaller beside the exe.  When in
// doubt it answers "portable", because an in-place exe swap is less invasive
// than silently running an installer.
//
// The exe path is a parameter rather than looked up internally so this file
// needs no wxWidgets, which is what lets the whole updater live in the
// headless core and be unit-tested without starting a GUI.
bool running_portable(const std::filesystem::path& app_exe);

// "/ALLUSERS" or "/CURRENTUSER", matching where the running exe actually
// lives.  A silent installer re-run without this takes the per-machine default
// and plants a SECOND copy alongside the per-user one.
std::string install_scope_flag(const std::filesystem::path& app_exe);

// The batch that waits for this process to exit, then installs and relaunches.
std::string installer_batch(const std::filesystem::path& setup_path,
                            const std::filesystem::path& app_exe,
                            unsigned long pid);

// The batch that waits, unpacks the portable zip, and copies it over the
// install only if it really contains PDFSherpa.exe.
std::string portable_batch(const std::filesystem::path& zip_path,
                           const std::filesystem::path& staging_dir,
                           const std::filesystem::path& app_exe,
                           unsigned long pid);

// Write `batch` to a .bat and start it detached, so it outlives this process.
bool spawn_handoff_batch(const std::string& batch,
                         const std::filesystem::path& batch_path);

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_UPDATER_H
