// Guard: no non-ASCII byte may appear inside a NARROW string literal.
//
// A narrow "Filter PDFs…" in a UTF-8 source is handed to wxString as raw
// bytes and decoded in the current ANSI codepage, so it renders as
// "Filter PDFsâ€¦".  /utf-8 does not help: it controls how the compiler reads
// the file, not how wx interprets the bytes.  The fix is always the same --
// write L"Filter PDFs…" or wxString::FromUTF8(...).
//
// This is a test rather than a note because the bug is invisible until
// somebody looks at the running UI, and it re-appears every time a menu label
// gains an ellipsis or an em-dash.  In MDBossCpp it was fixed once and was
// back a few commits later in the toolbar tooltips.
//
// Comments are skipped: prose may legitimately contain non-ASCII, and this
// file's own comments do.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {

struct Offence {
    std::string file;
    int line = 0;
    std::string text;
};

// Scan one line, returning true if it ends inside a block comment.
bool scan_line(const std::string& line, const std::string& file, int lineno,
               bool in_block_comment, std::vector<Offence>* offences)
{
    bool in_block = in_block_comment;
    bool in_string = false;
    bool narrow = false;   // the literal currently open is narrow
    bool escaped = false;
    bool reported = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        const unsigned char byte = static_cast<unsigned char>(c);

        if (in_block) {
            if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                in_block = false;
                ++i;
            }
            continue;
        }

        if (!in_string) {
            if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
                break;  // line comment: prose may hold anything
            }
            if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
                in_block = true;
                ++i;
                continue;
            }
            if (c == '"') {
                in_string = true;
                escaped = false;
                // A literal is wide if an L, u8, u or U prefix precedes it.
                // Only L matters for the wx bug, but treating the others as
                // non-narrow keeps this from firing on u8"..." too.
                narrow = true;
                if (i > 0) {
                    const char prev = line[i - 1];
                    if (prev == 'L' || prev == 'u' || prev == 'U') {
                        narrow = false;
                    }
                    if (prev == '8' && i > 1 && line[i - 2] == 'u') {
                        narrow = false;
                    }
                }
            }
            continue;
        }

        // Inside a string literal.
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = false;
            continue;
        }
        if (narrow && byte > 127 && !reported) {
            offences->push_back(Offence{file, lineno, line});
            reported = true;  // one report per line is enough to locate it
        }
    }

    return in_block;
}

}  // namespace

TEST_CASE("no non-ASCII bytes in narrow string literals", "[source][i18n]")
{
    const fs::path app_dir = fs::path(SHERPA_APP_DIR);
    REQUIRE(fs::is_directory(app_dir));

    std::vector<Offence> offences;
    int files_scanned = 0;

    for (const fs::directory_entry& entry : fs::directory_iterator(app_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h") {
            continue;
        }

        std::ifstream stream(entry.path(), std::ios::binary);
        REQUIRE(stream);
        ++files_scanned;

        std::string line;
        int lineno = 0;
        bool in_block = false;
        while (std::getline(stream, line)) {
            ++lineno;
            in_block = scan_line(line, entry.path().filename().string(), lineno,
                                 in_block, &offences);
        }
    }

    // A mis-set path would make this test pass by scanning nothing, which is
    // the one failure mode a guard like this must not have.
    CHECK(files_scanned >= 8);

    for (const Offence& offence : offences) {
        UNSCOPED_INFO(offence.file << ":" << offence.line << "  " << offence.text);
    }
    CHECK(offences.empty());
}
