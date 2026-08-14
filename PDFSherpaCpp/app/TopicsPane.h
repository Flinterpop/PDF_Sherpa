// The middle pane: the user's bookmarks above, the PDF's topics below.
//
// The bookmark list only exists while the PDF has bookmarks; otherwise the
// topics take the full height.  The divider between them is draggable and its
// position persists, which is why this is a splitter rather than two stacked
// panels.

#ifndef PDFSHERPA_APP_TOPICS_PANE_H
#define PDFSHERPA_APP_TOPICS_PANE_H

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <wx/splitter.h>
#include <wx/treectrl.h>
#include <wx/wx.h>

#include "Metadata.h"

namespace pdfsherpa {

class TopicsPane : public wxPanel {
public:
    explicit TopicsPane(wxWindow* parent);

    // Show the topics and bookmarks for a PDF.  Passing an empty path clears
    // the pane.
    void load(const std::filesystem::path& pdf_path);
    void clear();

    // Re-read only the bookmarks, after one was added, renamed or deleted.
    void reload_bookmarks();

    const std::vector<Bookmark>& bookmarks() const { return bookmarks_; }

    // Raised when the user picks a topic or a bookmark; carries a 1-based page.
    void set_page_requested_handler(std::function<void(int)> handler)
    {
        on_page_requested_ = std::move(handler);
    }
    // Raised after bookmarks are changed here, so the frame can persist them
    // and recolour the PDF list row.
    void set_bookmarks_changed_handler(std::function<void()> handler)
    {
        on_bookmarks_changed_ = std::move(handler);
    }

    std::optional<int> sash_position() const;
    void set_sash_position(int pixels);

    // Add a bookmark for `page` (1-based), prompting for its name.
    void add_bookmark(int page);

private:
    void build_ui();
    void rebuild_topics();
    void rebuild_bookmarks();
    void save_bookmarks_now();
    void on_bookmark_menu(wxTreeEvent& event);

    std::filesystem::path pdf_path_;
    std::vector<TopicEntry> topics_;
    std::vector<Bookmark> bookmarks_;
    std::string filter_;
    std::string placeholder_;
    int wanted_sash_ = 0;

    wxSplitterWindow* splitter_ = nullptr;
    wxPanel* bookmarks_panel_ = nullptr;
    wxPanel* topics_panel_ = nullptr;
    wxTreeCtrl* bookmark_tree_ = nullptr;
    wxTreeCtrl* topic_tree_ = nullptr;
    wxTextCtrl* filter_box_ = nullptr;

    std::vector<int> topic_pages_;     // parallel to visible topic rows
    std::vector<int> bookmark_index_;  // parallel to visible bookmark rows

    std::function<void(int)> on_page_requested_;
    std::function<void()> on_bookmarks_changed_;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_TOPICS_PANE_H
