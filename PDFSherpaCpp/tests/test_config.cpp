// Settings compatibility with the Python app.
//
// Every test here redirects APPDATA to a scratch directory first.  Config
// resolves its path from the environment on each call, so this keeps the suite
// from reading -- or worse, rewriting -- the developer's real
// %APPDATA%\PDFGuide\config.json.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <process.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Config.h"

namespace fs = std::filesystem;
using pdfsherpa::Config;
using pdfsherpa::Root;

namespace {

// Redirects APPDATA for as long as it is alive, then puts back whatever was
// there so one test cannot leak into the next.
class ScopedAppData {
public:
    ScopedAppData()
        : dir_(fs::temp_directory_path() /
               ("sherpa_cfg_" + std::to_string(_getpid()) + "_" +
                std::to_string(counter_++)))
    {
        std::error_code ec;
        fs::create_directories(dir_, ec);

        char* previous = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&previous, &size, "APPDATA") == 0 && previous != nullptr) {
            previous_ = previous;
            had_previous_ = true;
            std::free(previous);
        }
        _putenv_s("APPDATA", dir_.string().c_str());
    }

    ~ScopedAppData()
    {
        if (had_previous_) {
            _putenv_s("APPDATA", previous_.c_str());
        } else {
            _putenv_s("APPDATA", "");
        }
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedAppData(const ScopedAppData&) = delete;
    ScopedAppData& operator=(const ScopedAppData&) = delete;

    // Write config.json directly, standing in for the Python app.
    void write_raw(const std::string& text) const
    {
        const fs::path path = Config::path();
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream stream(path, std::ios::binary);
        stream << text;
    }

    std::string read_raw() const
    {
        std::ifstream stream(Config::path(), std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }

private:
    fs::path dir_;
    std::string previous_;
    bool had_previous_ = false;
    static inline int counter_ = 0;
};

}  // namespace

TEST_CASE("config lives under the pre-rename PDFGuide folder", "[config]")
{
    const ScopedAppData appdata;
    const fs::path path = Config::path();
    // Renaming this folder would strand every profile written by the Python
    // app, which is the whole point of keeping the old name.
    CHECK(path.parent_path().filename() == "PDFGuide");
    CHECK(path.filename() == "config.json");
}

TEST_CASE("defaults apply when there is no file", "[config]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();

    CHECK(config.fit_pref() == "width");
    CHECK(config.check_updates());
    CHECK(config.show_pdf_list());
    CHECK(config.show_topics());
    CHECK(config.folder().empty());
    CHECK_FALSE(config.bm_sash().has_value());
}

TEST_CASE("a malformed file falls back to defaults instead of failing", "[config]")
{
    const ScopedAppData appdata;
    appdata.write_raw("{ this is not json");

    Config config;
    config.load();
    CHECK(config.fit_pref() == "width");
    CHECK(config.check_updates());
}

TEST_CASE("keys this app does not understand survive a write", "[config]")
{
    const ScopedAppData appdata;
    appdata.write_raw(
        R"({"folder": "C:/docs", "some_future_key": {"nested": [1, 2, 3]},
            "another": "kept"})");

    Config config;
    config.load();
    REQUIRE(config.save_fit_pref("page"));

    const nlohmann::json after = nlohmann::json::parse(appdata.read_raw());
    // This is the rule that lets a user run either app against one profile.
    REQUIRE(after.contains("some_future_key"));
    CHECK(after["some_future_key"]["nested"][2] == 3);
    CHECK(after["another"] == "kept");
    CHECK(after["folder"] == "C:/docs");
    CHECK(after["fit_pref"] == "page");
}

TEST_CASE("last_pages keeps insertion order as its LRU", "[config][lru]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();

    // Names chosen so alphabetical order is the REVERSE of insertion order.
    // A std::map-backed json would silently re-sort these and the prune below
    // would evict the wrong entry -- with no error, just users losing their
    // reading positions.
    REQUIRE(config.save_last_page("C:/docs/zebra.pdf", 10));
    REQUIRE(config.save_last_page("C:/docs/mango.pdf", 20));
    REQUIRE(config.save_last_page("C:/docs/apple.pdf", 30));

    const nlohmann::ordered_json after =
        nlohmann::ordered_json::parse(appdata.read_raw());
    REQUIRE(after.contains("last_pages"));

    std::vector<std::string> order;
    for (const auto& entry : after["last_pages"].items()) {
        order.push_back(entry.key());
    }
    REQUIRE(order.size() == 3);
    CHECK(order[0].find("zebra") != std::string::npos);
    CHECK(order[1].find("mango") != std::string::npos);
    CHECK(order[2].find("apple") != std::string::npos);
}

TEST_CASE("re-reading a PDF moves it to the freshest position", "[config][lru]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();

    REQUIRE(config.save_last_page("C:/docs/a.pdf", 1));
    REQUIRE(config.save_last_page("C:/docs/b.pdf", 2));
    REQUIRE(config.save_last_page("C:/docs/a.pdf", 7));

    const nlohmann::ordered_json after =
        nlohmann::ordered_json::parse(appdata.read_raw());
    std::vector<std::string> order;
    for (const auto& entry : after["last_pages"].items()) {
        order.push_back(entry.key());
    }
    REQUIRE(order.size() == 2);
    CHECK(order[0].find("b.pdf") != std::string::npos);
    CHECK(order[1].find("a.pdf") != std::string::npos);

    Config reloaded;
    reloaded.load();
    const auto page = reloaded.last_page("C:/docs/a.pdf");
    REQUIRE(page.has_value());
    CHECK(*page == 7);
}

TEST_CASE("last_pages prunes the least recently read past the cap", "[config][lru]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();

    // One more than the cap; the first one written must be the one evicted.
    for (std::size_t i = 0; i < pdfsherpa::kMaxRememberedPages + 1; ++i) {
        REQUIRE(config.save_last_page(
            "C:/docs/file" + std::to_string(i) + ".pdf", static_cast<int>(i)));
    }

    const nlohmann::ordered_json after =
        nlohmann::ordered_json::parse(appdata.read_raw());
    CHECK(after["last_pages"].size() == pdfsherpa::kMaxRememberedPages);

    Config reloaded;
    reloaded.load();
    CHECK_FALSE(reloaded.last_page("C:/docs/file0.pdf").has_value());
    CHECK(reloaded.last_page("C:/docs/file1.pdf").has_value());
    CHECK(reloaded.last_page(
                  "C:/docs/file" +
                  std::to_string(pdfsherpa::kMaxRememberedPages) + ".pdf")
              .has_value());
}

TEST_CASE("page keys normalise case and separators", "[config]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();

    REQUIRE(config.save_last_page("C:/Docs/Manual.PDF", 42));

    // The same file reached by a different spelling must find the same
    // position, or the reading position silently resets depending on how the
    // user got to the file.
    Config reloaded;
    reloaded.load();
    const auto page = reloaded.last_page("C:\\docs\\manual.pdf");
    REQUIRE(page.has_value());
    CHECK(*page == 42);
}

TEST_CASE("favorites cap at the same limit as the Python app", "[config]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();

    std::vector<std::string> many;
    for (std::size_t i = 0; i < pdfsherpa::kMaxFavorites + 5; ++i) {
        many.push_back("fav" + std::to_string(i) + ".pdf");
    }
    REQUIRE(config.save_favorites(many));
    CHECK(config.favorites().size() == pdfsherpa::kMaxFavorites);

    Config reloaded;
    reloaded.load();
    CHECK(reloaded.favorites().size() == pdfsherpa::kMaxFavorites);
    CHECK(reloaded.favorites().front() == "fav0.pdf");
}

TEST_CASE("expanded folders are written sorted, as Python writes them", "[config]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();
    REQUIRE(config.save_expanded_folders({"zulu", "alpha", "mike"}));

    const nlohmann::json after = nlohmann::json::parse(appdata.read_raw());
    REQUIRE(after["expanded_folders"].size() == 3);
    CHECK(after["expanded_folders"][0] == "alpha");
    CHECK(after["expanded_folders"][1] == "mike");
    CHECK(after["expanded_folders"][2] == "zulu");
}

TEST_CASE("window state writes each sash only when it has one", "[config]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();

    REQUIRE(config.save_window_state("1200x800+10+10", std::nullopt, std::nullopt));
    nlohmann::json after = nlohmann::json::parse(appdata.read_raw());
    CHECK(after["geometry"] == "1200x800+10+10");
    // Never written, so a sash saved by a previous run is not erased.
    CHECK_FALSE(after.contains("bm_sash"));
    CHECK_FALSE(after.contains("fav_sash"));

    REQUIRE(config.save_window_state("1000x700+0+0", 250, 130));
    after = nlohmann::json::parse(appdata.read_raw());
    CHECK(after["bm_sash"] == 250);
    CHECK(after["fav_sash"] == 130);

    REQUIRE(config.save_window_state("900x600+5+5", std::nullopt, std::nullopt));
    after = nlohmann::json::parse(appdata.read_raw());
    CHECK(after["bm_sash"] == 250);   // preserved, not cleared
    CHECK(after["fav_sash"] == 130);  // likewise
}

TEST_CASE("the favorites divider round-trips", "[config]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();
    CHECK_FALSE(config.fav_sash().has_value());

    REQUIRE(config.save_window_state("800x600+0+0", std::nullopt, 175));

    Config reloaded;
    reloaded.load();
    REQUIRE(reloaded.fav_sash().has_value());
    CHECK(*reloaded.fav_sash() == 175);
}

TEST_CASE("a folder-only profile is promoted to a single root", "[config][roots]")
{
    const ScopedAppData appdata;
    // Exactly what a profile written before multi-root support looks like.
    appdata.write_raw(R"({"folder": "C:\\ICD"})");

    Config config;
    config.load();
    REQUIRE(config.roots().size() == 1);
    CHECK(config.roots()[0].path == "C:\\ICD");
    // Named after the folder itself, since the old format carried no name.
    CHECK(config.roots()[0].name == "ICD");
}

TEST_CASE("roots round-trip and keep folder pointing at the first",
          "[config][roots]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();

    REQUIRE(config.save_roots({Root{"ICDs", "C:\\ICD"},
                               Root{"Manuals", "D:\\docs\\manuals"}}));

    const nlohmann::json after = nlohmann::json::parse(appdata.read_raw());
    REQUIRE(after["roots"].size() == 2);
    CHECK(after["roots"][0]["name"] == "ICDs");
    CHECK(after["roots"][1]["path"] == "D:\\docs\\manuals");
    // The deprecated Python app knows only "folder"; leaving it stale would
    // send it somewhere the user may have removed.
    CHECK(after["folder"] == "C:\\ICD");

    Config reloaded;
    reloaded.load();
    REQUIRE(reloaded.roots().size() == 2);
    CHECK(reloaded.roots()[1].name == "Manuals");
    CHECK(reloaded.folder() == "C:\\ICD");
}

TEST_CASE("roots are capped", "[config][roots]")
{
    const ScopedAppData appdata;
    Config config;
    config.load();

    std::vector<Root> many;
    for (std::size_t i = 0; i < pdfsherpa::kMaxRoots + 3; ++i) {
        many.push_back(Root{"root" + std::to_string(i), "C:\\r" + std::to_string(i)});
    }
    REQUIRE(config.save_roots(many));
    CHECK(config.roots().size() == pdfsherpa::kMaxRoots);

    Config reloaded;
    reloaded.load();
    CHECK(reloaded.roots().size() == pdfsherpa::kMaxRoots);
    CHECK(reloaded.roots().front().name == "root0");
}

TEST_CASE("roots win over folder when both are present", "[config][roots]")
{
    const ScopedAppData appdata;
    // A profile the C++ app has written and the Python app has since opened:
    // both keys exist, and "roots" is the authoritative one.
    appdata.write_raw(R"({"folder": "C:\\ICD",
                          "roots": [{"name": "A", "path": "C:\\a"},
                                    {"name": "B", "path": "C:\\b"}]})");

    Config config;
    config.load();
    REQUIRE(config.roots().size() == 2);
    CHECK(config.roots()[0].path == "C:\\a");
}

TEST_CASE("a root entry with no path is skipped", "[config][roots]")
{
    const ScopedAppData appdata;
    appdata.write_raw(R"({"roots": [{"name": "broken"},
                                    {"name": "fine", "path": "C:\\ok"}]})");

    Config config;
    config.load();
    REQUIRE(config.roots().size() == 1);
    CHECK(config.roots()[0].path == "C:\\ok");
}

TEST_CASE("a Python-written profile loads whole", "[config]")
{
    const ScopedAppData appdata;
    // Shaped exactly as app.py writes it.
    appdata.write_raw(R"({
        "folder": "C:\\ICD",
        "fit_pref": "page",
        "bm_sash": 180,
        "expanded_folders": ["sub/one", "sub/two"],
        "favorites": ["a.pdf", "b.pdf"],
        "check_updates": false,
        "geometry": "1427x1209+100+50",
        "last_pdf": "C:\\ICD\\manual.pdf",
        "last_pages": {"c:\\icd\\manual.pdf": 17},
        "show_pdf_list": false,
        "show_topics": true,
        "skip_version": "1.3.11"
    })");

    Config config;
    config.load();
    CHECK(config.folder() == "C:\\ICD");
    CHECK(config.fit_pref() == "page");
    REQUIRE(config.bm_sash().has_value());
    CHECK(*config.bm_sash() == 180);
    CHECK(config.expanded_folders().size() == 2);
    CHECK(config.favorites().size() == 2);
    CHECK_FALSE(config.check_updates());
    CHECK(config.geometry() == "1427x1209+100+50");
    CHECK(config.last_pdf() == "C:\\ICD\\manual.pdf");
    CHECK_FALSE(config.show_pdf_list());
    CHECK(config.show_topics());
    CHECK(config.skip_version() == "1.3.11");

    const auto page = config.last_page("C:\\ICD\\manual.pdf");
    REQUIRE(page.has_value());
    CHECK(*page == 17);
}
