// The favorites interchange file.
//
// This logic is headless on purpose: the dialogs around it cannot be tested,
// but everything that can actually be got wrong -- the format, the tolerant
// reader, the merge order and the cap -- can be, and is.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "Config.h"          // kMaxFavorites
#include "FavoritesFile.h"

using namespace pdfsherpa;

TEST_CASE("favorites round-trip through the file format", "[favorites]")
{
    const std::vector<std::string> favorites = {
        "manuals/Bravo Handbook.pdf",
        "D:\\elsewhere\\Loose Document.pdf",
    };

    const std::string text = favorites_to_json(favorites);
    // Readable and hand-editable: indented, and ending in a newline.
    CHECK(text.find("\n  \"favorites\"") != std::string::npos);
    CHECK(text.back() == '\n');

    const FavoritesFile parsed = parse_favorites_json(text);
    REQUIRE(parsed.ok);
    REQUIRE(parsed.had_list);
    CHECK(parsed.favorites == favorites);
}

TEST_CASE("a bare top-level array is accepted", "[favorites]")
{
    // The shape a user is most likely to hand-write.
    const FavoritesFile parsed =
        parse_favorites_json(R"(["a.pdf", "sub/b.pdf"])");
    REQUIRE(parsed.ok);
    CHECK(parsed.had_list);
    REQUIRE(parsed.favorites.size() == 2);
    CHECK(parsed.favorites[1] == "sub/b.pdf");
}

TEST_CASE("junk entries are skipped, not fatal", "[favorites]")
{
    // A hand-edited file should import what it can rather than refusing.
    const FavoritesFile parsed = parse_favorites_json(
        R"({"favorites": ["good.pdf", 42, null, "   ", "also-good.pdf"]})");
    REQUIRE(parsed.ok);
    REQUIRE(parsed.favorites.size() == 2);
    CHECK(parsed.favorites[0] == "good.pdf");
    CHECK(parsed.favorites[1] == "also-good.pdf");
}

TEST_CASE("malformed JSON is an error", "[favorites]")
{
    const FavoritesFile parsed = parse_favorites_json("{ not json");
    CHECK_FALSE(parsed.ok);
    CHECK_FALSE(parsed.error.empty());
}

TEST_CASE("a valid file that is not a favorites file is distinguishable",
          "[favorites]")
{
    // Parsed fine, but carries no list -- a different message from "the list
    // is empty", because it is a different mistake.
    const FavoritesFile parsed = parse_favorites_json(R"({"roots": []})");
    CHECK(parsed.ok);
    CHECK_FALSE(parsed.had_list);
    CHECK(parsed.favorites.empty());

    const FavoritesFile empty = parse_favorites_json(R"({"favorites": []})");
    CHECK(empty.ok);
    CHECK(empty.had_list);
    CHECK(empty.favorites.empty());
}

TEST_CASE("merging keeps the existing order and appends the new", "[favorites]")
{
    const std::vector<std::string> existing = {"a.pdf", "b.pdf"};
    const std::vector<std::string> incoming = {"c.pdf", "d.pdf"};

    const std::vector<std::string> merged =
        merge_favorites(existing, incoming, kMaxFavorites);
    REQUIRE(merged.size() == 4);
    CHECK(merged[0] == "a.pdf");
    CHECK(merged[1] == "b.pdf");
    CHECK(merged[2] == "c.pdf");
    CHECK(merged[3] == "d.pdf");
}

TEST_CASE("merging never promotes an entry the user already had", "[favorites]")
{
    // Importing a file containing b.pdf must not move it to the front; that
    // would silently reshuffle a hand-curated list.
    const std::vector<std::string> merged =
        merge_favorites({"a.pdf", "b.pdf"}, {"b.pdf", "c.pdf"}, kMaxFavorites);
    REQUIRE(merged.size() == 3);
    CHECK(merged[0] == "a.pdf");
    CHECK(merged[1] == "b.pdf");
    CHECK(merged[2] == "c.pdf");
}

TEST_CASE("duplicates differing only in case or separator are one entry",
          "[favorites]")
{
    // These are Windows paths: the same file, spelled three ways.
    const std::vector<std::string> merged = merge_favorites(
        {"Manuals/Guide.pdf"},
        {"manuals/guide.pdf", "Manuals\\Guide.pdf", "other.pdf"},
        kMaxFavorites);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0] == "Manuals/Guide.pdf");  // the spelling already stored wins
    CHECK(merged[1] == "other.pdf");
}

TEST_CASE("merging respects the cap", "[favorites]")
{
    std::vector<std::string> existing;
    for (std::size_t i = 0; i < kMaxFavorites; ++i) {
        existing.push_back("have" + std::to_string(i) + ".pdf");
    }
    const std::vector<std::string> merged =
        merge_favorites(existing, {"new.pdf"}, kMaxFavorites);

    CHECK(merged.size() == kMaxFavorites);
    // A full list drops the import rather than evicting something the user
    // chose to keep.
    CHECK(std::find(merged.begin(), merged.end(), "new.pdf") == merged.end());
}

TEST_CASE("replace is expressible as a merge into nothing", "[favorites]")
{
    // How the Replace branch of the import dialog is implemented.
    const std::vector<std::string> merged =
        merge_favorites({}, {"x.pdf", "y.pdf", "x.pdf"}, kMaxFavorites);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0] == "x.pdf");
    CHECK(merged[1] == "y.pdf");
}
