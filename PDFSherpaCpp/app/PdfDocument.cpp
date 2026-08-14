#include "PdfDocument.h"

#include <cassert>
#include <cstring>

#include "PathUtf8.h"

// MuPDF's headers are not /W4-clean (C4100 on unused ctx parameters, C4611 on
// the setjmp machinery).  They are included through /external:I in CMake so
// their own warnings are silenced; C4611 still fires here because fz_try
// expands into *this* file, so it is disabled explicitly and locally.
#pragma warning(push)
#pragma warning(disable : 4611)  // _setjmp and C++ object destruction

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfsherpa {

// The opaque holder promised by the header.
struct PdfDocument::Context {
    fz_context* ctx = nullptr;
};

namespace {

fz_rect to_fz(const Rect& rect)
{
    fz_rect out;
    out.x0 = rect.x0;
    out.y0 = rect.y0;
    out.x1 = rect.x1;
    out.y1 = rect.y1;
    return out;
}

Rect from_fz(const fz_rect& rect)
{
    Rect out;
    out.x0 = rect.x0;
    out.y0 = rect.y0;
    out.x1 = rect.x1;
    out.y1 = rect.y1;
    return out;
}

Rect from_quad(const fz_quad& quad)
{
    // fz_rect_from_quad takes the bounding box, which is what PyMuPDF's
    // search_for() and get_text("words") both hand back.
    return from_fz(fz_rect_from_quad(quad));
}

// Append one UTF-8-encoded code point.
void append_utf8(std::string* out, int codepoint)
{
    assert(out != nullptr);
    char buffer[8] = {};
    const int written = fz_runetochar(buffer, codepoint);
    assert(written > 0 && written < static_cast<int>(sizeof(buffer)));
    out->append(buffer, static_cast<std::size_t>(written));
}

bool is_word_break(int codepoint)
{
    // PyMuPDF splits words on whitespace.  MuPDF normalises horizontal
    // whitespace to U+0020 unless FZ_STEXT_PRESERVE_WHITESPACE is set, which
    // it is not here, so this stays a short list rather than a Unicode table.
    return codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
           codepoint == '\r' || codepoint == '\f' || codepoint == 0xA0;
}

}  // namespace

PdfDocument::PdfDocument()
    : context_(new Context())
{
    // No locks installed: one context per PdfDocument, never shared across
    // threads.  See the threading note in the header.
    context_->ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (context_->ctx == nullptr) {
        last_error_ = "could not initialise MuPDF";
        return;
    }
    fz_try(context_->ctx) {
        fz_register_document_handlers(context_->ctx);
    }
    fz_catch(context_->ctx) {
        last_error_ = fz_caught_message(context_->ctx);
        fz_drop_context(context_->ctx);
        context_->ctx = nullptr;
    }
}

PdfDocument::~PdfDocument()
{
    close();
    if (context_ != nullptr) {
        if (context_->ctx != nullptr) {
            fz_drop_context(context_->ctx);
        }
        delete context_;
        context_ = nullptr;
    }
}

bool PdfDocument::open(const std::filesystem::path& path)
{
    close();
    if (context_ == nullptr || context_->ctx == nullptr) {
        last_error_ = "MuPDF is not initialised";
        return false;
    }

    fz_context* ctx = context_->ctx;
    // fz_open_document takes a filename in the system encoding; MuPDF's
    // Windows build expects UTF-8 and widens it internally.
    const std::string utf8 = path_to_utf8(path);
    fz_document* doc = nullptr;

    fz_try(ctx) {
        doc = fz_open_document(ctx, utf8.c_str());
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        return false;
    }

    document_ = doc;
    dirty_ = false;
    last_error_.clear();
    return document_ != nullptr;
}

void PdfDocument::close()
{
    if (document_ != nullptr && context_ != nullptr && context_->ctx != nullptr) {
        fz_drop_document(context_->ctx, static_cast<fz_document*>(document_));
    }
    document_ = nullptr;
    dirty_ = false;
}

bool PdfDocument::is_pdf() const
{
    if (!is_open()) {
        return false;
    }
    return pdf_specifics(context_->ctx,
                         static_cast<fz_document*>(document_)) != nullptr;
}

int PdfDocument::page_count() const
{
    if (!is_open()) {
        return 0;
    }
    fz_context* ctx = context_->ctx;
    int count = 0;
    fz_try(ctx) {
        count = fz_count_pages(ctx, static_cast<fz_document*>(document_));
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        count = 0;
    }
    return count;
}

Rect PdfDocument::page_bounds(int page_index) const
{
    Rect bounds;
    if (!is_open() || page_index < 0) {
        return bounds;
    }
    fz_context* ctx = context_->ctx;
    fz_page* page = nullptr;
    fz_try(ctx) {
        page = fz_load_page(ctx, static_cast<fz_document*>(document_), page_index);
        bounds = from_fz(fz_bound_page(ctx, page));
    }
    fz_always(ctx) {
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        bounds = Rect{};
    }
    return bounds;
}

bool PdfDocument::render_page(int page_index, float zoom, RenderedPage* out)
{
    assert(out != nullptr);
    assert(zoom > 0.0F);
    if (!is_open() || page_index < 0) {
        last_error_ = "no document open";
        return false;
    }

    fz_context* ctx = context_->ctx;
    fz_page* page = nullptr;
    fz_pixmap* pix = nullptr;
    bool ok = false;

    fz_try(ctx) {
        page = fz_load_page(ctx, static_cast<fz_document*>(document_), page_index);
        const fz_matrix matrix = fz_scale(zoom, zoom);
        // alpha = 0, matching get_pixmap(alpha=False): three channels, no mask.
        pix = fz_new_pixmap_from_page(ctx, page, matrix, fz_device_rgb(ctx), 0);

        out->width = pix->w;
        out->height = pix->h;
        const std::size_t row_bytes = static_cast<std::size_t>(pix->w) * 3U;
        out->rgb.resize(row_bytes * static_cast<std::size_t>(pix->h));

        // MuPDF may pad each row; wxImage demands stride == width * 3, so
        // repack when they differ and memcpy wholesale when they do not.
        if (static_cast<std::size_t>(pix->stride) == row_bytes) {
            std::memcpy(out->rgb.data(), pix->samples, out->rgb.size());
        } else {
            for (int y = 0; y < pix->h; ++y) {
                const unsigned char* src =
                    pix->samples + static_cast<std::ptrdiff_t>(y) * pix->stride;
                std::memcpy(out->rgb.data() + static_cast<std::size_t>(y) * row_bytes,
                            src, row_bytes);
            }
        }
        ok = true;
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        ok = false;
    }
    return ok;
}

bool PdfDocument::page_text_blocks(int page_index, std::vector<TextBlock>* out)
{
    assert(out != nullptr);
    out->clear();
    if (!is_open() || page_index < 0) {
        last_error_ = "no document open";
        return false;
    }

    fz_context* ctx = context_->ctx;
    fz_page* page = nullptr;
    fz_stext_page* stext = nullptr;
    bool ok = false;

    fz_try(ctx) {
        page = fz_load_page(ctx, static_cast<fz_document*>(document_), page_index);
        stext = fz_new_stext_page_from_page(ctx, page, nullptr);

        for (fz_stext_block* block = stext->first_block; block != nullptr;
             block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) {
                continue;  // images and structure carry no headings
            }
            TextBlock out_block;
            for (fz_stext_line* line = block->u.t.first_line; line != nullptr;
                 line = line->next) {
                TextLine out_line;
                // Group consecutive characters into spans, breaking whenever
                // the font or size changes -- which is how PyMuPDF builds the
                // spans that TocGen's heading test reads.
                const fz_font* span_font = nullptr;
                float span_size = -1.0F;
                for (fz_stext_char* ch = line->first_char; ch != nullptr;
                     ch = ch->next) {
                    if (ch->font != span_font || ch->size != span_size ||
                        out_line.spans.empty()) {
                        Span span;
                        span.size = ch->size;
                        span.bold = fz_font_is_bold(ctx, ch->font) != 0;
                        const char* name = fz_font_name(ctx, ch->font);
                        span.font = (name != nullptr) ? name : "";
                        out_line.spans.push_back(std::move(span));
                        span_font = ch->font;
                        span_size = ch->size;
                    }
                    append_utf8(&out_line.spans.back().text, ch->c);
                }
                if (!out_line.spans.empty()) {
                    out_block.lines.push_back(std::move(out_line));
                }
            }
            if (!out_block.lines.empty()) {
                out->push_back(std::move(out_block));
            }
        }
        ok = true;
    }
    fz_always(ctx) {
        fz_drop_stext_page(ctx, stext);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        out->clear();
        ok = false;
    }
    return ok;
}

bool PdfDocument::search_page(int page_index, const std::string& needle,
                              std::vector<Rect>* out)
{
    assert(out != nullptr);
    out->clear();
    if (!is_open() || page_index < 0 || needle.empty()) {
        return false;
    }

    fz_context* ctx = context_->ctx;
    fz_page* page = nullptr;
    bool ok = false;

    // fz_search_page fills a caller-supplied buffer and returns how many hits
    // it found.  A page with more matches than this is pathological for a
    // reader, and the cap keeps the loop bounded.
    constexpr int kMaxHitsPerPage = 512;
    static_assert(kMaxHitsPerPage > 0, "hit buffer must hold something");

    fz_try(ctx) {
        page = fz_load_page(ctx, static_cast<fz_document*>(document_), page_index);
        std::vector<fz_quad> quads(static_cast<std::size_t>(kMaxHitsPerPage));
        const int found = fz_search_page(ctx, page, needle.c_str(), nullptr,
                                         quads.data(), kMaxHitsPerPage);
        for (int i = 0; i < found && i < kMaxHitsPerPage; ++i) {
            out->push_back(from_quad(quads[static_cast<std::size_t>(i)]));
        }
        ok = true;
    }
    fz_always(ctx) {
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        out->clear();
        ok = false;
    }
    return ok;
}

bool PdfDocument::page_words(int page_index, std::vector<Word>* out)
{
    assert(out != nullptr);
    out->clear();
    if (!is_open() || page_index < 0) {
        return false;
    }

    fz_context* ctx = context_->ctx;
    fz_page* page = nullptr;
    fz_stext_page* stext = nullptr;
    bool ok = false;

    fz_try(ctx) {
        page = fz_load_page(ctx, static_cast<fz_document*>(document_), page_index);
        stext = fz_new_stext_page_from_page(ctx, page, nullptr);

        for (fz_stext_block* block = stext->first_block; block != nullptr;
             block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) {
                continue;
            }
            for (fz_stext_line* line = block->u.t.first_line; line != nullptr;
                 line = line->next) {
                Word word;
                bool building = false;
                for (fz_stext_char* ch = line->first_char; ch != nullptr;
                     ch = ch->next) {
                    if (is_word_break(ch->c)) {
                        if (building) {
                            out->push_back(std::move(word));
                            word = Word{};
                            building = false;
                        }
                        continue;
                    }
                    const Rect box = from_quad(ch->quad);
                    if (!building) {
                        word.rect = box;
                        building = true;
                    } else {
                        word.rect.x0 = (word.rect.x0 < box.x0) ? word.rect.x0 : box.x0;
                        word.rect.y0 = (word.rect.y0 < box.y0) ? word.rect.y0 : box.y0;
                        word.rect.x1 = (word.rect.x1 > box.x1) ? word.rect.x1 : box.x1;
                        word.rect.y1 = (word.rect.y1 > box.y1) ? word.rect.y1 : box.y1;
                    }
                    append_utf8(&word.text, ch->c);
                }
                // A line ends a word even without trailing whitespace.
                if (building) {
                    out->push_back(std::move(word));
                }
            }
        }
        ok = true;
    }
    fz_always(ctx) {
        fz_drop_stext_page(ctx, stext);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        out->clear();
        ok = false;
    }
    return ok;
}

std::vector<OutlineItem> PdfDocument::outline() const
{
    std::vector<OutlineItem> items;
    if (!is_open()) {
        return items;
    }

    fz_context* ctx = context_->ctx;
    fz_document* doc = static_cast<fz_document*>(document_);
    fz_outline* root = nullptr;

    fz_try(ctx) {
        root = fz_load_outline(ctx, doc);

        // Depth-first with an explicit stack: the outline is user-supplied
        // data of unbounded depth, and recursion over it is exactly the
        // stack-overflow-on-a-malicious-file shape to avoid.
        struct Frame {
            fz_outline* node;
            int level;
        };
        std::vector<Frame> stack;
        if (root != nullptr) {
            stack.push_back(Frame{root, 1});
        }

        // Bound the walk: a corrupt outline can be cyclic, and this is the
        // only thing standing between that and an infinite loop.
        constexpr std::size_t kMaxOutlineItems = 100000;
        while (!stack.empty() && items.size() < kMaxOutlineItems) {
            const Frame frame = stack.back();
            stack.pop_back();
            if (frame.node == nullptr) {
                continue;
            }
            // Queue the sibling first so the child is popped next, which
            // yields document order.
            if (frame.node->next != nullptr) {
                stack.push_back(Frame{frame.node->next, frame.level});
            }
            if (frame.node->down != nullptr) {
                stack.push_back(Frame{frame.node->down, frame.level + 1});
            }

            OutlineItem item;
            item.level = frame.level;
            item.title = (frame.node->title != nullptr) ? frame.node->title : "";
            const int page = fz_page_number_from_location(ctx, doc, frame.node->page);
            // get_toc() reports 1-based pages; a destination that is not a
            // page resolves to -1, which the caller drops.
            item.page = (page >= 0) ? page + 1 : 0;
            items.push_back(std::move(item));
        }
    }
    fz_always(ctx) {
        fz_drop_outline(ctx, root);
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        items.clear();
    }
    return items;
}

bool PdfDocument::add_highlight(int page_index, const std::vector<Rect>& rects)
{
    if (!is_open() || rects.empty() || page_index < 0) {
        last_error_ = "nothing to highlight";
        return false;
    }

    fz_context* ctx = context_->ctx;
    pdf_document* pdf = pdf_specifics(ctx, static_cast<fz_document*>(document_));
    if (pdf == nullptr) {
        last_error_ = "highlights can only be added to a PDF";
        return false;
    }

    pdf_page* page = nullptr;
    bool ok = false;

    fz_try(ctx) {
        page = pdf_load_page(ctx, pdf, page_index);
        pdf_annot* annot = pdf_create_annot(ctx, page, PDF_ANNOT_HIGHLIGHT);

        std::vector<fz_quad> quads;
        quads.reserve(rects.size());
        for (const Rect& rect : rects) {
            quads.push_back(fz_quad_from_rect(to_fz(rect)));
        }
        pdf_set_annot_quad_points(ctx, annot, static_cast<int>(quads.size()),
                                  quads.data());
        // Without this the annotation has no appearance stream, and other
        // readers show nothing at all.  Writing highlights that Acrobat can
        // see is the entire point of the feature.
        pdf_update_annot(ctx, annot);
        ok = true;
    }
    fz_always(ctx) {
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        ok = false;
    }

    if (ok) {
        dirty_ = true;
    }
    return ok;
}

bool PdfDocument::remove_highlight_at(int page_index, float x, float y)
{
    if (!is_open() || page_index < 0) {
        return false;
    }

    fz_context* ctx = context_->ctx;
    pdf_document* pdf = pdf_specifics(ctx, static_cast<fz_document*>(document_));
    if (pdf == nullptr) {
        return false;
    }

    pdf_page* page = nullptr;
    bool removed = false;

    fz_try(ctx) {
        page = pdf_load_page(ctx, pdf, page_index);
        for (pdf_annot* annot = pdf_first_annot(ctx, page); annot != nullptr;
             annot = pdf_next_annot(ctx, annot)) {
            if (pdf_annot_type(ctx, annot) != PDF_ANNOT_HIGHLIGHT) {
                continue;
            }
            const Rect bounds = from_fz(pdf_bound_annot(ctx, annot));
            if (bounds.contains(x, y)) {
                pdf_delete_annot(ctx, page, annot);
                removed = true;
                break;  // the iterator is invalid past a delete
            }
        }
    }
    fz_always(ctx) {
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        removed = false;
    }

    if (removed) {
        dirty_ = true;
    }
    return removed;
}

bool PdfDocument::is_signed() const
{
    if (!is_open()) {
        return false;
    }
    fz_context* ctx = context_->ctx;
    pdf_document* pdf = pdf_specifics(ctx, static_cast<fz_document*>(document_));
    if (pdf == nullptr) {
        return false;
    }

    int flags = 0;
    fz_try(ctx) {
        // doc.get_sigflags() reads AcroForm/SigFlags; absent means unsigned.
        pdf_obj* trailer = pdf_trailer(ctx, pdf);
        pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
        pdf_obj* acroform = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        pdf_obj* sigflags = pdf_dict_get(ctx, acroform, PDF_NAME(SigFlags));
        flags = pdf_to_int(ctx, sigflags);
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        flags = 0;
    }
    return flags > 0;
}

bool PdfDocument::save_incremental()
{
    if (!is_open()) {
        last_error_ = "no document open";
        return false;
    }

    fz_context* ctx = context_->ctx;
    pdf_document* pdf = pdf_specifics(ctx, static_cast<fz_document*>(document_));
    if (pdf == nullptr) {
        last_error_ = "only a PDF can be saved";
        return false;
    }

    bool ok = false;
    fz_try(ctx) {
        pdf_write_options options = pdf_default_write_options;
        options.do_incremental = 1;
        pdf_save_document(ctx, pdf, nullptr, &options);
        ok = true;
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        ok = false;
    }

    if (ok) {
        dirty_ = false;
    }
    return ok;
}

bool PdfDocument::save_as(const std::filesystem::path& path)
{
    if (!is_open()) {
        last_error_ = "no document open";
        return false;
    }

    fz_context* ctx = context_->ctx;
    pdf_document* pdf = pdf_specifics(ctx, static_cast<fz_document*>(document_));
    if (pdf == nullptr) {
        last_error_ = "only a PDF can be saved";
        return false;
    }

    const std::string utf8 = path_to_utf8(path);
    bool ok = false;
    fz_try(ctx) {
        pdf_write_options options = pdf_default_write_options;
        pdf_save_document(ctx, pdf, utf8.c_str(), &options);
        ok = true;
    }
    fz_catch(ctx) {
        last_error_ = fz_caught_message(ctx);
        ok = false;
    }

    if (ok) {
        dirty_ = false;
    }
    return ok;
}

}  // namespace pdfsherpa

#pragma warning(pop)
