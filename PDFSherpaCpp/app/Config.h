// Per-user settings, shared byte-for-byte with the Python app.
//
// Both apps read and write the same %APPDATA%\PDFGuide\config.json, so a user
// who ran the Python app keeps their profile when they update onto this one.
// Two rules follow from that, and neither is optional:
//
//   1. *Never drop a key this app does not understand.*  Every write is a
//      read-modify-write of the file as it is on disk right now, exactly like
//      Python's update_config().
//
//   2. *Key order in `last_pages` is data, not formatting.*  See below.
//
// The folder is "PDFGuide", the app's name before it was renamed to PDF
// Sherpa.  It stays that way so settings saved before the rename keep working.
// Do not "fix" it.
//
// Ported from app.py's _config_path / load_config / save_config /
// update_config / _page_key / _save_last_page.

#ifndef PDFSHERPA_APP_CONFIG_H
#define PDFSHERPA_APP_CONFIG_H

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pdfsherpa {

// app.py's MAX_REMEMBERED_PAGES and MAX_FAVORITES.  Public so the favorites
// import path caps against the same number rather than a second copy of it.
inline constexpr std::size_t kMaxRememberedPages = 200;
inline constexpr std::size_t kMaxFavorites = 10;

// Top-level folders the PDF list shows, capped at the same five MDBoss allows.
// A cap at all exists because every root is rescanned on Refresh and walked on
// startup; five is generous for the use case and keeps that bounded.
inline constexpr std::size_t kMaxRoots = 5;

// One top-level folder, as stored: {"name": ..., "path": ...}.  The name is
// what the tree shows, so a root can be called "ICDs" rather than displaying
// a long absolute path.
struct Root {
    std::string name;
    std::string path;
};

// The stable per-PDF key for the last-page map: absolute, backslash-separated
// and lowercased, matching os.path.normcase(os.path.abspath(p)).  Two spellings
// of one file must land on one key or the reading position is lost whenever the
// user reaches the file by a different route.
std::string page_key(const std::filesystem::path& pdf_path);

class Config {
public:
    // %APPDATA%\PDFGuide\config.json, falling back to the user profile when
    // APPDATA is unset.
    static std::filesystem::path path();

    // Read from disk.  A missing or malformed file yields defaults rather than
    // an error: settings are a convenience and must never be load-bearing.
    void load();

    // The configured top-level folders, in display order.
    //
    // Back-compat: a profile written before multi-root support has no "roots"
    // key, only the single "folder".  load() promotes that to a one-element
    // list, and save_roots() keeps "folder" pointing at the first root, so a
    // profile stays readable by the deprecated Python app.
    const std::vector<Root>& roots() const { return roots_; }
    bool save_roots(std::vector<Root> roots);

    const std::string& folder() const { return folder_; }
    const std::string& last_pdf() const { return last_pdf_; }
    const std::string& fit_pref() const { return fit_pref_; }
    const std::string& geometry() const { return geometry_; }
    const std::string& skip_version() const { return skip_version_; }
    std::optional<int> bm_sash() const { return bm_sash_; }
    // Divider between the favorites list and the PDF tree.  A port-only key:
    // the Python app has no such divider, and it preserves keys it does not
    // know, so writing it is safe for a shared profile.
    std::optional<int> fav_sash() const { return fav_sash_; }
    bool check_updates() const { return check_updates_; }
    bool show_pdf_list() const { return show_pdf_list_; }
    bool show_topics() const { return show_topics_; }
    const std::vector<std::string>& expanded_folders() const { return expanded_folders_; }
    const std::vector<std::string>& favorites() const { return favorites_; }

    // The remembered reading position for a PDF, or nullopt.  The value is a
    // 0-based page index, as the Python app stores it.
    std::optional<int> last_page(const std::filesystem::path& pdf_path) const;

    // Each of these is one Python update_config({...}) call: it merges just
    // its own keys into the file on disk and writes it back, so a setting
    // changed by the other app between load() and now survives.  All return
    // false if the file could not be written; a failed settings write is
    // reported, never fatal.
    bool save_last_pdf(const std::string& pdf_path);
    bool save_fit_pref(const std::string& mode);
    bool save_skip_version(const std::string& version);
    bool save_expanded_folders(std::vector<std::string> folders);
    bool save_favorites(std::vector<std::string> favorites);
    bool save_pane_visibility(bool show_pdf_list, bool show_topics);
    // Written together on close, as _on_close does, so one write closes the
    // window rather than two racing ones.
    bool save_window_state(const std::string& geometry, std::optional<int> bm_sash,
                           std::optional<int> fav_sash);

    // Record the reading position for a PDF, pruning the least recently viewed
    // entries past kMaxRememberedPages.
    bool save_last_page(const std::filesystem::path& pdf_path, int page_index);

private:
    std::vector<Root> roots_;
    std::string folder_;
    std::string last_pdf_;
    std::string fit_pref_ = "width";
    std::string geometry_;
    std::string skip_version_;
    std::optional<int> bm_sash_;
    std::optional<int> fav_sash_;
    bool check_updates_ = true;
    bool show_pdf_list_ = true;
    bool show_topics_ = true;
    std::vector<std::string> expanded_folders_;
    std::vector<std::string> favorites_;
    // Insertion-ordered oldest-first, mirroring the JSON object on disk.
    std::vector<std::pair<std::string, int>> last_pages_;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_CONFIG_H
