// Engine tests: does the MuPDF wrapper give back what app.py's PyMuPDF calls
// gave back?  These run against a synthetic two-page fixture, so they assert
// exact values rather than "not empty".
//
// fixture.pdf, from make_fixture.py:
//   page 1: "Introduction to the Sherpa"  18pt bold (Helvetica-Bold)
//           "the quick brown fox jumps over the lazy dog"   11pt
//           "the second body line, also at eleven point"    11pt
//   page 2: "Chapter 2 - the second page"  14pt
//           "more body text on the second page"             11pt
//   outline: Introduction -> page 1, Chapter 2 -> page 2

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "PdfDocument.h"

namespace fs = std::filesystem;
using pdfsherpa::PdfDocument;

namespace {

fs::path fixture()
{
    return fs::path(SHERPA_TEST_FIXTURE);
}

// Flatten every span on a page into one string, so a test can ask "is this
// text here" without caring how MuPDF split the runs.
std::string all_text(PdfDocument& doc, int page)
{
    std::vector<pdfsherpa::TextBlock> blocks;
    REQUIRE(doc.page_text_blocks(page, &blocks));
    std::string out;
    for (const auto& block : blocks) {
        for (const auto& line : block.lines) {
            for (const auto& span : line.spans) {
                out += span.text;
            }
            out += "\n";
        }
    }
    return out;
}

}  // namespace

TEST_CASE("fixture opens and reports its shape", "[pdf]")
{
    PdfDocument doc;
    REQUIRE(doc.open(fixture()));
    CHECK(doc.is_open());
    CHECK(doc.is_pdf());
    CHECK(doc.page_count() == 2);
    CHECK_FALSE(doc.is_signed());

    const pdfsherpa::Rect bounds = doc.page_bounds(0);
    // A4 in points.
    CHECK_THAT(bounds.width(), Catch::Matchers::WithinAbs(595.0, 0.5));
    CHECK_THAT(bounds.height(), Catch::Matchers::WithinAbs(842.0, 0.5));
}

TEST_CASE("opening a missing file fails without throwing", "[pdf]")
{
    PdfDocument doc;
    CHECK_FALSE(doc.open(fixture().parent_path() / "no-such-file.pdf"));
    CHECK_FALSE(doc.is_open());
    CHECK_FALSE(doc.last_error().empty());
    // The object must stay usable: the app reuses one PdfDocument across
    // selections, and a bad file must not poison the next good one.
    CHECK(doc.open(fixture()));
}

TEST_CASE("render produces tightly packed RGB at the requested zoom", "[pdf]")
{
    PdfDocument doc;
    REQUIRE(doc.open(fixture()));

    pdfsherpa::RenderedPage page;
    REQUIRE(doc.render_page(0, 1.0F, &page));
    CHECK(page.width == 595);
    CHECK(page.height == 842);
    // wxImage requires stride == width * 3; anything else displays as a
    // sheared page, which is the bug this assert exists to prevent.
    CHECK(page.rgb.size() ==
          static_cast<std::size_t>(page.width) *
              static_cast<std::size_t>(page.height) * 3U);

    pdfsherpa::RenderedPage zoomed;
    REQUIRE(doc.render_page(0, 2.0F, &zoomed));
    CHECK(zoomed.width == 1190);
    CHECK(zoomed.height == 1684);
}

TEST_CASE("structured text carries the size and bold flags TocGen needs", "[pdf]")
{
    PdfDocument doc;
    REQUIRE(doc.open(fixture()));

    std::vector<pdfsherpa::TextBlock> blocks;
    REQUIRE(doc.page_text_blocks(0, &blocks));
    REQUIRE_FALSE(blocks.empty());

    // The heading is the only 18pt run, and it must come back bold -- this
    // pair is exactly what _extract_headings tests against the body size.
    bool found_heading = false;
    bool found_body = false;
    for (const auto& block : blocks) {
        for (const auto& line : block.lines) {
            for (const auto& span : line.spans) {
                if (span.size > 17.0F && span.size < 19.0F) {
                    found_heading = true;
                    CHECK(span.bold);
                    CHECK_FALSE(span.font.empty());
                }
                if (span.size > 10.5F && span.size < 11.5F) {
                    found_body = true;
                    CHECK_FALSE(span.bold);
                }
            }
        }
    }
    CHECK(found_heading);
    CHECK(found_body);

    CHECK(all_text(doc, 0).find("Introduction to the Sherpa") != std::string::npos);
    CHECK(all_text(doc, 1).find("Chapter 2") != std::string::npos);
}

TEST_CASE("search finds every occurrence on a page", "[pdf]")
{
    PdfDocument doc;
    REQUIRE(doc.open(fixture()));

    std::vector<pdfsherpa::Rect> hits;
    REQUIRE(doc.search_page(0, "the", &hits));
    // "the" appears in the heading, twice in the fox line and once in the
    // second body line.
    CHECK(hits.size() == 4);
    for (const pdfsherpa::Rect& hit : hits) {
        CHECK(hit.width() > 0.0F);
        CHECK(hit.height() > 0.0F);
    }

    std::vector<pdfsherpa::Rect> none;
    REQUIRE(doc.search_page(0, "zzzznotpresent", &none));
    CHECK(none.empty());
}

TEST_CASE("search is case-insensitive, as page.search_for is", "[pdf]")
{
    PdfDocument doc;
    REQUIRE(doc.open(fixture()));

    std::vector<pdfsherpa::Rect> lower;
    std::vector<pdfsherpa::Rect> upper;
    REQUIRE(doc.search_page(0, "introduction", &lower));
    REQUIRE(doc.search_page(0, "INTRODUCTION", &upper));
    CHECK(lower.size() == 1);
    CHECK(lower.size() == upper.size());
}

TEST_CASE("words come back split on whitespace with sane boxes", "[pdf]")
{
    PdfDocument doc;
    REQUIRE(doc.open(fixture()));

    std::vector<pdfsherpa::Word> words;
    REQUIRE(doc.page_words(0, &words));
    REQUIRE_FALSE(words.empty());

    const auto has = [&words](const std::string& text) {
        return std::any_of(words.begin(), words.end(),
                           [&text](const pdfsherpa::Word& w) { return w.text == text; });
    };
    CHECK(has("quick"));
    CHECK(has("brown"));
    CHECK(has("Introduction"));

    for (const pdfsherpa::Word& word : words) {
        // No word may contain a space: that is the one invariant drag-select
        // depends on when it intersects the rubber band against word boxes.
        CHECK(word.text.find(' ') == std::string::npos);
        CHECK_FALSE(word.text.empty());
        CHECK(word.rect.width() > 0.0F);
    }
}

TEST_CASE("outline flattens to titles and 1-based pages", "[pdf]")
{
    PdfDocument doc;
    REQUIRE(doc.open(fixture()));

    const std::vector<pdfsherpa::OutlineItem> items = doc.outline();
    REQUIRE(items.size() == 2);
    CHECK(items[0].title == "Introduction");
    CHECK(items[0].page == 1);
    CHECK(items[1].title == "Chapter 2");
    CHECK(items[1].page == 2);
}

TEST_CASE("highlights round-trip through a saved copy", "[pdf][annot]")
{
    const fs::path copy =
        fs::temp_directory_path() / "sherpa_highlight_roundtrip.pdf";
    std::error_code ec;
    fs::remove(copy, ec);

    // Highlight the first word, save to a copy, reopen and delete it again.
    {
        PdfDocument doc;
        REQUIRE(doc.open(fixture()));
        CHECK_FALSE(doc.dirty());

        std::vector<pdfsherpa::Word> words;
        REQUIRE(doc.page_words(0, &words));
        REQUIRE_FALSE(words.empty());

        REQUIRE(doc.add_highlight(0, {words[0].rect}));
        CHECK(doc.dirty());

        REQUIRE(doc.save_as(copy));
        CHECK_FALSE(doc.dirty());
    }

    REQUIRE(fs::exists(copy));

    {
        PdfDocument reopened;
        REQUIRE(reopened.open(copy));

        std::vector<pdfsherpa::Word> words;
        REQUIRE(reopened.page_words(0, &words));
        REQUIRE_FALSE(words.empty());

        // A point inside the highlighted word must find the annotation that
        // survived the save.  This is the assertion that would catch a missing
        // appearance stream turning into a highlight no reader can see.
        const pdfsherpa::Rect& box = words[0].rect;
        const float cx = (box.x0 + box.x1) / 2.0F;
        const float cy = (box.y0 + box.y1) / 2.0F;
        CHECK(reopened.remove_highlight_at(0, cx, cy));
        CHECK(reopened.dirty());

        // Removing twice must not claim a second success.
        CHECK_FALSE(reopened.remove_highlight_at(0, cx, cy));
    }

    fs::remove(copy, ec);
}

TEST_CASE("removing a highlight where there is none reports false", "[pdf][annot]")
{
    PdfDocument doc;
    REQUIRE(doc.open(fixture()));
    CHECK_FALSE(doc.remove_highlight_at(0, 5.0F, 5.0F));
    CHECK_FALSE(doc.dirty());
}

TEST_CASE("out-of-range pages fail rather than crash", "[pdf]")
{
    PdfDocument doc;
    REQUIRE(doc.open(fixture()));

    pdfsherpa::RenderedPage page;
    CHECK_FALSE(doc.render_page(99, 1.0F, &page));
    CHECK_FALSE(doc.render_page(-1, 1.0F, &page));

    std::vector<pdfsherpa::TextBlock> blocks;
    CHECK_FALSE(doc.page_text_blocks(99, &blocks));
}
