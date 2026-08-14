// Sidecar parsing.  The .toc format is hand-edited by users, so the parser's
// tolerances and its error message are both part of the contract.

#include <filesystem>
#include <fstream>
#include <string>

#include <process.h>  // _getpid, so the scratch dir is unique per ctest process

#include <catch2/catch_test_macros.hpp>

#include "Metadata.h"

namespace fs = std::filesystem;
using namespace pdfsherpa;

namespace {

// A scratch directory that cleans itself up, so a failing test cannot leave
// files behind that make the next run pass for the wrong reason.
class TempDir {
public:
    TempDir()
        : path_(fs::temp_directory_path() /
                ("sherpa_meta_" + std::to_string(_getpid()) + "_" +
                 std::to_string(counter_++)))
    {
        std::error_code ec;
        fs::create_directories(path_, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

    fs::path write(const std::string& name, const std::string& content) const
    {
        const fs::path file = path_ / name;
        std::ofstream stream(file, std::ios::binary);
        stream << content;
        return file;
    }

private:
    fs::path path_;
    static inline int counter_ = 0;
};

}  // namespace

TEST_CASE("toc lines take the page from the trailing number", "[metadata]")
{
    const TopicLoadResult result = parse_toc_text(
        "# a comment\n"
        "\n"
        "Introduction: 1\n"
        "Chapter 1 - Setup: 5\n"
        "Advanced Topics: 42\n");

    REQUIRE(result.ok);
    REQUIRE(result.entries.size() == 3);
    CHECK(result.entries[0].topic == "Introduction");
    CHECK(result.entries[0].page == 1);
    // The dash must stay in the topic; splitting on it would mangle the most
    // common heading style in the corpus.
    CHECK(result.entries[1].topic == "Chapter 1 - Setup");
    CHECK(result.entries[1].page == 5);
    CHECK(result.entries[2].page == 42);
}

TEST_CASE("a topic may contain colons", "[metadata]")
{
    // rpartition(':'), not partition(':') -- the LAST colon separates.
    const TopicLoadResult result =
        parse_toc_text("Annex B: Message Formats: 118\n");
    REQUIRE(result.ok);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].topic == "Annex B: Message Formats");
    CHECK(result.entries[0].page == 118);
}

TEST_CASE("a line with no colon splits on the last space", "[metadata]")
{
    const TopicLoadResult result = parse_toc_text("Introduction 7\n");
    REQUIRE(result.ok);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].topic == "Introduction");
    CHECK(result.entries[0].page == 7);
}

TEST_CASE("comments, blanks and CRLF are tolerated", "[metadata]")
{
    const TopicLoadResult result = parse_toc_text(
        "# Topics for manual.pdf\r\n"
        "\r\n"
        "   \r\n"
        "Intro: 1\r\n");
    REQUIRE(result.ok);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].topic == "Intro");
    CHECK(result.entries[0].page == 1);
}

TEST_CASE("a malformed toc line reports its line number", "[metadata]")
{
    const TopicLoadResult result = parse_toc_text(
        "Good: 1\n"
        "no page here\n");
    CHECK_FALSE(result.ok);
    // The user hand-edits this file; the message has to say where.
    CHECK(result.error.find("Line 2") != std::string::npos);
}

TEST_CASE("a line that is only a page number is an error", "[metadata]")
{
    const TopicLoadResult result = parse_toc_text("42\n");
    CHECK_FALSE(result.ok);
}

TEST_CASE("json topics parse", "[metadata]")
{
    const TopicLoadResult result = parse_json_metadata(
        R"([{"topic": " Intro ", "page": 1}, {"topic": "Body", "page": 9}])");
    REQUIRE(result.ok);
    REQUIRE(result.entries.size() == 2);
    CHECK(result.entries[0].topic == "Intro");  // trimmed
    CHECK(result.entries[1].page == 9);
}

TEST_CASE("malformed json is an error, not an empty list", "[metadata]")
{
    // Distinguishing these matters: the UI says "error reading X" for one and
    // "no topics in X" for the other.
    CHECK_FALSE(parse_json_metadata("{not json").ok);
    CHECK_FALSE(parse_json_metadata(R"([{"topic": "x"}])").ok);
    CHECK(parse_json_metadata("[]").ok);
    CHECK(parse_json_metadata("[]").entries.empty());
}

TEST_CASE("toc wins over json when both exist", "[metadata]")
{
    const TempDir dir;
    dir.write("manual.pdf", "%PDF-1.4\n");
    dir.write("manual.toc", "From toc: 1\n");
    dir.write("manual.json", R"([{"topic": "From json", "page": 1}])");

    const auto found = find_metadata_path(dir.path() / "manual.pdf");
    REQUIRE(found.has_value());
    CHECK(found->extension() == ".toc");
}

TEST_CASE("a PDF with no sidecar has no metadata path", "[metadata]")
{
    const TempDir dir;
    dir.write("lonely.pdf", "%PDF-1.4\n");
    CHECK_FALSE(find_metadata_path(dir.path() / "lonely.pdf").has_value());
}

TEST_CASE("the bookmarks sidecar keeps clear of the topics lookup", "[metadata]")
{
    const TempDir dir;
    const fs::path pdf = dir.path() / "manual.pdf";
    // manual.bookmarks.json must NOT be mistaken for manual.json.
    CHECK(bookmarks_path(pdf).filename() == "manual.bookmarks.json");

    dir.write("manual.pdf", "%PDF-1.4\n");
    dir.write("manual.bookmarks.json", R"([{"page": 3, "title": "Here"}])");
    CHECK_FALSE(find_metadata_path(pdf).has_value());
}

TEST_CASE("bookmarks round-trip and sort by page", "[metadata]")
{
    const TempDir dir;
    const fs::path pdf = dir.path() / "manual.pdf";
    dir.write("manual.pdf", "%PDF-1.4\n");

    const std::vector<Bookmark> written = {
        Bookmark{9, "Ninth"}, Bookmark{2, "Second"}, Bookmark{5, "Fifth"}};
    REQUIRE(save_bookmarks(pdf, written));

    const std::vector<Bookmark> read = load_bookmarks(pdf);
    REQUIRE(read.size() == 3);
    CHECK(read[0].page == 2);
    CHECK(read[1].page == 5);
    CHECK(read[2].page == 9);
    CHECK(read[0].title == "Second");
}

TEST_CASE("saving an empty list deletes the sidecar", "[metadata]")
{
    const TempDir dir;
    const fs::path pdf = dir.path() / "manual.pdf";
    dir.write("manual.pdf", "%PDF-1.4\n");

    REQUIRE(save_bookmarks(pdf, {Bookmark{1, "One"}}));
    REQUIRE(fs::exists(bookmarks_path(pdf)));

    // No file means no bookmarks; an empty [] left behind would make the PDF
    // list show it as bookmarked forever.
    REQUIRE(save_bookmarks(pdf, {}));
    CHECK_FALSE(fs::exists(bookmarks_path(pdf)));
    CHECK(load_bookmarks(pdf).empty());
}

TEST_CASE("bookmarks with a bad page or blank title are dropped", "[metadata]")
{
    const TempDir dir;
    const fs::path pdf = dir.path() / "manual.pdf";
    dir.write("manual.pdf", "%PDF-1.4\n");
    dir.write("manual.bookmarks.json",
              R"([{"page": 0, "title": "Page zero"},
                  {"page": 3, "title": "   "},
                  {"page": 4, "title": "Keeper"}])");

    const std::vector<Bookmark> read = load_bookmarks(pdf);
    REQUIRE(read.size() == 1);
    CHECK(read[0].title == "Keeper");
    CHECK(read[0].page == 4);
}

TEST_CASE("a corrupt sidecar warns instead of throwing", "[metadata]")
{
    const TempDir dir;
    const fs::path pdf = dir.path() / "manual.pdf";
    dir.write("manual.pdf", "%PDF-1.4\n");
    dir.write("manual.bookmarks.json", "{ this is not json");

    std::string warning;
    const std::vector<Bookmark> read = load_bookmarks(pdf, &warning);
    CHECK(read.empty());
    CHECK_FALSE(warning.empty());
}

TEST_CASE("a missing sidecar is silent, not a warning", "[metadata]")
{
    const TempDir dir;
    const fs::path pdf = dir.path() / "manual.pdf";
    dir.write("manual.pdf", "%PDF-1.4\n");

    std::string warning = "untouched";
    const std::vector<Bookmark> read = load_bookmarks(pdf, &warning);
    CHECK(read.empty());
    // Having no bookmarks is the normal case and must not nag the user.
    CHECK(warning == "untouched");
}
