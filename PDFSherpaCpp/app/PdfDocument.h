// The one place PDF Sherpa talks to MuPDF.
//
// Nothing outside PdfDocument.cpp includes a mupdf header, for two reasons:
//
//   1. fz_try/fz_catch are macros over setjmp/longjmp.  They trip MSVC's
//      C4611 ("interaction between '_setjmp' and C++ object destruction is
//      non-portable") in whatever translation unit expands them, which under
//      the /W4 /WX this app builds at is a hard error.  Confining them to one
//      file confines the pragma that silences it to one file too.
//   2. longjmp does not run destructors.  Any C++ object with a non-trivial
//      destructor living across an fz_try boundary leaks on error.  Keeping
//      MuPDF behind this wall means the rest of the app is ordinary RAII C++
//      that cannot make that mistake.
//
// Everything here returns a plain value type or a bool + last_error(); no
// MuPDF type, and no MuPDF error, escapes.
//
// THREADING: a PdfDocument owns its own fz_context and is NOT safe to share
// between threads.  The content-search worker therefore constructs its *own*
// PdfDocument over the same file rather than borrowing the viewer's.  That
// costs one extra file open and sidesteps fz_clone_context and MuPDF's
// lock-installation contract entirely -- reopening is microseconds against a
// scan that reads every page, and no shared mutable state can race.

#ifndef PDFSHERPA_APP_PDF_DOCUMENT_H
#define PDFSHERPA_APP_PDF_DOCUMENT_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pdfsherpa {

// A rectangle in PDF points (origin top-left, y growing downward), matching
// fitz.Rect so page coordinates mean the same thing in both apps.
struct Rect {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;

    float width() const { return x1 - x0; }
    float height() const { return y1 - y0; }
    bool contains(float x, float y) const
    {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
    bool intersects(const Rect& other) const
    {
        return x0 < other.x1 && other.x0 < x1 && y0 < other.y1 && other.y0 < y1;
    }
};

// One whitespace-delimited word and its box, the unit drag-selection works in.
// Mirrors what PyMuPDF's page.get_text("words") hands back.
struct Word {
    Rect rect;
    std::string text;  // UTF-8
};

// A run of characters sharing a font and size -- PyMuPDF's "span".  TocGen's
// heading detection reads exactly these three fields.
struct Span {
    std::string text;  // UTF-8
    std::string font;  // font name, e.g. "Arial-BoldMT"
    float size = 0.0F;
    bool bold = false;
};

struct TextLine {
    std::vector<Span> spans;
};

struct TextBlock {
    std::vector<TextLine> lines;
};

// One flattened outline entry, as doc.get_toc() yields (level, title, page).
struct OutlineItem {
    int level = 1;      // 1-based depth
    std::string title;  // UTF-8
    int page = 1;       // 1-based; 0 when the destination is not a page
};

// A rendered page as tightly-packed 24-bit RGB, ready for wxImage, which
// requires stride == width * 3 and will not accept MuPDF's padded pixmap.
struct RenderedPage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;  // width * height * 3
};

class PdfDocument {
public:
    PdfDocument();
    ~PdfDocument();

    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;
    PdfDocument(PdfDocument&&) = delete;
    PdfDocument& operator=(PdfDocument&&) = delete;

    // Open a document, closing any already-open one.  False on failure, with
    // the reason in last_error().
    bool open(const std::filesystem::path& path);
    void close();

    bool is_open() const { return document_ != nullptr; }
    // Whether the open file is a PDF.  MuPDF opens other formats too, but only
    // a PDF can carry annotations or be saved incrementally.
    bool is_pdf() const;

    int page_count() const;
    // Page size in points; a zero-size result means the index was invalid.
    Rect page_bounds(int page_index) const;

    // Render at `zoom` (1.0 == 72 dpi), as get_pixmap(fitz.Matrix(z, z)) does.
    bool render_page(int page_index, float zoom, RenderedPage* out);

    // Structured text for TocGen.  Images and other non-text blocks are
    // dropped, matching what the Python code iterates over.
    bool page_text_blocks(int page_index, std::vector<TextBlock>* out);

    // Case-insensitive search within one page, as page.search_for() does.
    bool search_page(int page_index, const std::string& needle,
                     std::vector<Rect>* out);

    // Word boxes for drag-selection.
    bool page_words(int page_index, std::vector<Word>* out);

    // The document outline, flattened depth-first.
    std::vector<OutlineItem> outline() const;

    // -- Highlight annotations ------------------------------------------------
    // Add one highlight covering `rects`.  Marks the document dirty.
    bool add_highlight(int page_index, const std::vector<Rect>& rects);

    // Delete the topmost highlight containing the point, if any.  Returns true
    // only when one was actually removed.
    bool remove_highlight_at(int page_index, float x, float y);

    // True once a highlight has been added or removed and not yet saved.
    bool dirty() const { return dirty_; }

    // Whether the document carries a digital signature, which saving would
    // invalidate -- doc.get_sigflags() > 0 in the Python app.
    bool is_signed() const;

    // Append-only update written back into the original file (saveIncr()).
    bool save_incremental();
    // Full write to a new file (doc.save(path)), used for the "(ann)" copy.
    bool save_as(const std::filesystem::path& path);

    const std::string& last_error() const { return last_error_; }

private:
    // Opaque MuPDF types, so this header stays free of mupdf includes.
    struct Context;
    Context* context_ = nullptr;
    void* document_ = nullptr;  // fz_document*
    bool dirty_ = false;
    mutable std::string last_error_;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_PDF_DOCUMENT_H
