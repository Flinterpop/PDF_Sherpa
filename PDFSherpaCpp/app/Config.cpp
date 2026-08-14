#include "Config.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <sstream>

// Only for CharLowerW, which is the one thing the CRT cannot do correctly
// here; see lowercase() below.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include "PathUtf8.h"

namespace pdfsherpa {
namespace {

namespace fs = std::filesystem;

// ordered_json, NOT json.  This is load-bearing, not a style choice.
//
// app.py's _save_last_page uses the *insertion order* of the last_pages object
// as an LRU: it pops a key before re-adding it so the map stays oldest-first,
// then prunes from the front once it passes MAX_REMEMBERED_PAGES.  Python
// dicts preserve insertion order and JSON round-trips it.
//
// nlohmann::json is backed by std::map and would silently re-sort every key
// alphabetically on the first write.  The file would still parse, the app
// would still run, and the pruning would quietly start evicting whichever PDF
// sorts first by path instead of the least recently read one.  Nothing would
// look broken; users would just lose reading positions at random.
using json = nlohmann::ordered_json;

// getenv() is deprecated under /W4 /WX on MSVC, and the _s variant hands back
// an allocation the caller owns.
std::string environment(const char* name)
{
    assert(name != nullptr);
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
}

// Python's _config_path(): APPDATA, falling back to the home directory.
fs::path user_data_base()
{
    const std::string appdata = environment("APPDATA");
    if (!appdata.empty()) {
        return path_from_utf8(appdata);
    }
    const std::string profile = environment("USERPROFILE");
    return profile.empty() ? fs::path(".") : path_from_utf8(profile);
}

// Unicode-aware lowering, matching Python's str.lower() closely enough that
// the two apps agree on a key.  std::tolower/_wcslwr are locale-dependent and
// handle only ASCII under the default "C" locale, which would put a path with
// an accented folder name under a different key in each app -- and the symptom
// would be a silently forgotten reading position, not an error.
std::wstring lowercase(std::wstring text)
{
    if (!text.empty()) {
        // CharLowerBuffW works in place and returns the count processed.
        const DWORD length = static_cast<DWORD>(text.size());
        const DWORD done = ::CharLowerBuffW(text.data(), length);
        assert(done == length);
        (void)done;
    }
    return text;
}

// Read the whole config file as JSON, or a null json on any failure.
json read_document()
{
    std::ifstream stream(Config::path(), std::ios::binary);
    if (!stream) {
        return json{};
    }
    // Buffer first so a BOM can be stripped: a JSON parser rejects one
    // outright, and the whole config would silently fall back to defaults --
    // losing the user's folder, favorites and reading positions -- if anything
    // ever wrote the file with one.
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    std::string text = buffer.str();
    static constexpr std::string_view kBom = "\xEF\xBB\xBF";
    if (text.size() >= kBom.size() &&
        std::string_view(text).substr(0, kBom.size()) == kBom) {
        text.erase(0, kBom.size());
    }

    json document = json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return json{};
    }
    return document;
}

// Merge `patch`'s keys into the file on disk and write it back.  This is
// Python's update_config(): keys this app has never heard of are read, kept,
// and written out untouched.
bool merge_write(const json& patch)
{
    assert(patch.is_object());
    json document = read_document();
    if (!document.is_object()) {
        document = json::object();
    }
    for (const auto& entry : patch.items()) {
        document[entry.key()] = entry.value();
    }

    const fs::path path = Config::path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Ignore ec: the directory may already exist, and the open below is the
    // real test of whether the write can proceed.

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    // Python writes json.dump(settings, fh) with no indent, so keep it
    // compact; a file the two apps take turns rewriting should not churn.
    stream << document.dump();
    stream.flush();
    return stream.good();
}

std::string string_at(const json& document, const char* key)
{
    if (document.contains(key) && document[key].is_string()) {
        return document[key].get<std::string>();
    }
    return {};
}

bool bool_at(const json& document, const char* key, bool fallback)
{
    // bool(cfg.get(k, True)) in Python accepts anything truthy; the only
    // values these keys ever hold are real booleans, so anything else is
    // corruption and the default is the safer answer.
    if (document.contains(key) && document[key].is_boolean()) {
        return document[key].get<bool>();
    }
    return fallback;
}

std::vector<std::string> string_array(const json& document, const char* key,
                                      std::size_t limit)
{
    std::vector<std::string> out;
    if (!document.contains(key) || !document[key].is_array()) {
        return out;
    }
    for (const json& item : document[key]) {
        if (out.size() >= limit) {
            break;
        }
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

// The comparison key for a FOLDER path.
//
// page_key() alone is not enough here: it preserves a trailing separator, so
// "C:\ICD\ASTERIX" and "C:\ICD\ASTERIX\" key differently even though they are
// the same folder.  Both spellings occur in the wild -- Explorer and the
// registry's InstallLocation both hand back the trailing form -- and a folder
// that silently stopped being flat depending on how its path was spelled would
// be a maddening bug to chase.
std::string folder_key(const std::string& path)
{
    std::string key = page_key(path_from_utf8(path));
    while (key.size() > 1 && (key.back() == '\\' || key.back() == '/')) {
        key.pop_back();
    }
    return key;
}

json to_array(const std::vector<std::string>& values)
{
    json array = json::array();
    for (const std::string& value : values) {
        array.push_back(value);
    }
    return array;
}

}  // namespace

std::string page_key(const fs::path& pdf_path)
{
    std::error_code ec;
    fs::path absolute = fs::absolute(pdf_path, ec);
    if (ec) {
        absolute = pdf_path;  // best effort; a relative key still round-trips
    }
    absolute = absolute.lexically_normal();
    absolute.make_preferred();  // forward slashes to backslashes, as normcase does
    return path_to_utf8(fs::path(lowercase(absolute.wstring())));
}

fs::path Config::path()
{
    // "PDFGuide", not "PDFSherpa": the app was renamed, the folder was not,
    // and changing it now would strand every existing profile.
    return user_data_base() / "PDFGuide" / "config.json";
}

void Config::load()
{
    const json document = read_document();
    if (!document.is_object()) {
        return;
    }

    folder_ = string_at(document, "folder");

    roots_.clear();
    if (document.contains("roots") && document["roots"].is_array()) {
        for (const json& entry : document["roots"]) {
            if (roots_.size() >= kMaxRoots) {
                break;
            }
            if (!entry.is_object() || !entry.contains("path") ||
                !entry["path"].is_string()) {
                continue;
            }
            Root root;
            root.path = entry["path"].get<std::string>();
            if (root.path.empty()) {
                continue;
            }
            root.name = (entry.contains("name") && entry["name"].is_string())
                            ? entry["name"].get<std::string>()
                            : std::string();
            if (root.name.empty()) {
                // Fall back to the folder's own name, which is what the user
                // would have called it anyway.
                root.name = path_to_utf8(path_from_utf8(root.path).filename());
                if (root.name.empty()) {
                    root.name = root.path;
                }
            }
            roots_.push_back(std::move(root));
        }
    }
    // A profile written before multi-root support carries only "folder".
    // Promote it rather than starting the user with nothing.
    if (roots_.empty() && !folder_.empty()) {
        Root root;
        root.path = folder_;
        root.name = path_to_utf8(path_from_utf8(folder_).filename());
        if (root.name.empty()) {
            root.name = folder_;
        }
        roots_.push_back(std::move(root));
    }
    last_pdf_ = string_at(document, "last_pdf");
    geometry_ = string_at(document, "geometry");
    skip_version_ = string_at(document, "skip_version");

    const std::string pref = string_at(document, "fit_pref");
    fit_pref_ = (pref == "width" || pref == "page") ? pref : "width";

    bm_sash_.reset();
    if (document.contains("bm_sash") && document["bm_sash"].is_number_integer()) {
        const int sash = document["bm_sash"].get<int>();
        if (sash > 0) {
            bm_sash_ = sash;
        }
    }

    fav_sash_.reset();
    if (document.contains("fav_sash") && document["fav_sash"].is_number_integer()) {
        const int sash = document["fav_sash"].get<int>();
        if (sash > 0) {
            fav_sash_ = sash;
        }
    }

    check_updates_ = bool_at(document, "check_updates", true);
    show_pdf_list_ = bool_at(document, "show_pdf_list", true);
    show_topics_ = bool_at(document, "show_topics", true);

    // No cap on expanded_folders: the Python app writes every open folder and
    // capping here would silently collapse the tree on the next launch.
    expanded_folders_ = string_array(document, "expanded_folders",
                                     static_cast<std::size_t>(-1));
    favorites_ = string_array(document, "favorites", kMaxFavorites);

    // Any folder can be flattened, not just a top-level one, so the cap is
    // well above the five roots -- it exists only to stop a corrupted file
    // growing without bound.
    flat_folders_ = string_array(document, "flat_folders", 4096);

    last_pages_.clear();
    if (document.contains("last_pages") && document["last_pages"].is_object()) {
        for (const auto& entry : document["last_pages"].items()) {
            if (entry.value().is_number_integer()) {
                last_pages_.emplace_back(entry.key(), entry.value().get<int>());
            }
        }
    }
}

std::optional<int> Config::last_page(const fs::path& pdf_path) const
{
    const std::string key = page_key(pdf_path);
    const auto it = std::find_if(last_pages_.begin(), last_pages_.end(),
                                 [&key](const std::pair<std::string, int>& e) {
                                     return e.first == key;
                                 });
    if (it == last_pages_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Config::save_roots(std::vector<Root> roots)
{
    if (roots.size() > kMaxRoots) {
        roots.resize(kMaxRoots);
    }
    roots_ = std::move(roots);

    json array = json::array();
    for (const Root& root : roots_) {
        array.push_back(json{{"name", root.name}, {"path", root.path}});
    }

    json patch = json{{"roots", array}};
    // Keep "folder" pointing at the first root.  The deprecated Python app
    // knows nothing about "roots" and opens "folder"; leaving it stale would
    // send it to a folder the user may have removed.
    folder_ = roots_.empty() ? std::string() : roots_.front().path;
    patch["folder"] = folder_;

    return merge_write(patch);
}

bool Config::save_last_pdf(const std::string& pdf_path)
{
    last_pdf_ = pdf_path;
    return merge_write(json{{"last_pdf", pdf_path}});
}

bool Config::save_fit_pref(const std::string& mode)
{
    assert(mode == "width" || mode == "page");
    fit_pref_ = mode;
    return merge_write(json{{"fit_pref", mode}});
}

bool Config::save_skip_version(const std::string& version)
{
    skip_version_ = version;
    return merge_write(json{{"skip_version", version}});
}

bool Config::is_flat_folder(const std::string& path) const
{
    const std::string key = folder_key(path);
    for (const std::string& flat : flat_folders_) {
        if (folder_key(flat) == key) {
            return true;
        }
    }
    return false;
}

bool Config::set_flat_folder(const std::string& path, bool flat)
{
    assert(!path.empty() && "a flat folder needs a path");
    const std::string key = folder_key(path);

    for (auto it = flat_folders_.begin(); it != flat_folders_.end(); ++it) {
        if (folder_key(*it) == key) {
            if (flat) {
                return true;  // already flat; nothing to write
            }
            flat_folders_.erase(it);
            return merge_write(json{{"flat_folders", to_array(flat_folders_)}});
        }
    }
    if (!flat) {
        return true;  // already a tree; nothing to write
    }
    flat_folders_.push_back(path);
    return merge_write(json{{"flat_folders", to_array(flat_folders_)}});
}

bool Config::save_expanded_folders(std::vector<std::string> folders)
{
    // Python writes sorted(self._expanded); keeping that makes the two apps'
    // output byte-identical instead of merely equivalent.
    std::sort(folders.begin(), folders.end());
    expanded_folders_ = std::move(folders);
    return merge_write(json{{"expanded_folders", to_array(expanded_folders_)}});
}

bool Config::save_favorites(std::vector<std::string> favorites)
{
    if (favorites.size() > kMaxFavorites) {
        favorites.resize(kMaxFavorites);
    }
    favorites_ = std::move(favorites);
    return merge_write(json{{"favorites", to_array(favorites_)}});
}

bool Config::save_pane_visibility(bool show_pdf_list, bool show_topics)
{
    show_pdf_list_ = show_pdf_list;
    show_topics_ = show_topics;
    return merge_write(json{{"show_pdf_list", show_pdf_list},
                            {"show_topics", show_topics}});
}

bool Config::save_window_state(const std::string& geometry,
                               std::optional<int> bm_sash,
                               std::optional<int> fav_sash)
{
    geometry_ = geometry;
    json patch = json{{"geometry", geometry}};
    // _on_close only writes bm_sash when it has one, so an unset sash keeps
    // whatever the previous run stored rather than erasing it.  Same for the
    // favorites divider, which does not exist while the pane is unsplit.
    if (bm_sash.has_value()) {
        bm_sash_ = bm_sash;
        patch["bm_sash"] = *bm_sash;
    }
    if (fav_sash.has_value()) {
        fav_sash_ = fav_sash;
        patch["fav_sash"] = *fav_sash;
    }
    return merge_write(patch);
}

bool Config::save_last_page(const fs::path& pdf_path, int page_index)
{
    assert(page_index >= 0);
    const std::string key = page_key(pdf_path);

    // Erase then append, so the freshest entry is always last and the prune
    // below drops the least recently read.  This is the whole reason the file
    // is written through ordered_json.
    const auto stale = std::remove_if(
        last_pages_.begin(), last_pages_.end(),
        [&key](const std::pair<std::string, int>& e) { return e.first == key; });
    last_pages_.erase(stale, last_pages_.end());
    last_pages_.emplace_back(key, page_index);

    if (last_pages_.size() > kMaxRememberedPages) {
        const std::size_t excess = last_pages_.size() - kMaxRememberedPages;
        last_pages_.erase(last_pages_.begin(),
                          last_pages_.begin() + static_cast<std::ptrdiff_t>(excess));
    }

    json pages = json::object();
    for (const auto& entry : last_pages_) {
        pages[entry.first] = entry.second;
    }
    return merge_write(json{{"last_pages", pages}});
}

}  // namespace pdfsherpa
