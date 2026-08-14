#include "TopicsPane.h"

#include <algorithm>
#include <cassert>

#include <wx/textdlg.h>

#include "PathUtf8.h"

namespace pdfsherpa {
namespace {

namespace fs = std::filesystem;

constexpr int kDefaultSash = 150;

std::string to_lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

// The row index behind a tree item.
//
// This MUST be a real heap object, not an integer smuggled through the
// pointer.  wxTreeCtrl takes ownership of whatever SetItemData is given and
// deletes it when the item goes away, so casting an index to wxTreeItemData*
// makes DeleteAllItems call delete on address 1, 2, 3...  Index 0 casts to
// nullptr and deletes harmlessly, which is exactly the sort of bug that looks
// fine on a one-row list and crashes on a two-row one.
class RowData : public wxTreeItemData {
public:
    explicit RowData(std::size_t row) : row_(row) {}
    std::size_t row() const { return row_; }

private:
    std::size_t row_;
};

// The row index for an item, or nullopt when it carries none (a placeholder
// row such as "(no matches)").
std::optional<std::size_t> row_of(wxTreeCtrl* tree, const wxTreeItemId& item)
{
    if (tree == nullptr || !item.IsOk()) {
        return std::nullopt;
    }
    const auto* data = dynamic_cast<RowData*>(tree->GetItemData(item));
    if (data == nullptr) {
        return std::nullopt;
    }
    return data->row();
}

}  // namespace

TopicsPane::TopicsPane(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    build_ui();
}

void TopicsPane::build_ui()
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize,
                                     wxSP_LIVE_UPDATE | wxSP_3DSASH);
    splitter_->SetMinimumPaneSize(60);

    bookmarks_panel_ = new wxPanel(splitter_, wxID_ANY);
    auto* bm_sizer = new wxBoxSizer(wxVERTICAL);
    bm_sizer->Add(new wxStaticText(bookmarks_panel_, wxID_ANY, L"Bookmarks"), 0,
                  wxLEFT | wxTOP, 4);
    bookmark_tree_ = new wxTreeCtrl(bookmarks_panel_, wxID_ANY, wxDefaultPosition,
                                    wxDefaultSize,
                                    wxTR_HIDE_ROOT | wxTR_SINGLE |
                                        wxTR_NO_LINES | wxTR_FULL_ROW_HIGHLIGHT);
    bm_sizer->Add(bookmark_tree_, 1, wxEXPAND | wxALL, 4);
    bookmarks_panel_->SetSizer(bm_sizer);

    topics_panel_ = new wxPanel(splitter_, wxID_ANY);
    auto* topic_sizer = new wxBoxSizer(wxVERTICAL);
    topic_sizer->Add(new wxStaticText(topics_panel_, wxID_ANY, L"Topics"), 0,
                     wxLEFT | wxTOP, 4);
    filter_box_ = new wxTextCtrl(topics_panel_, wxID_ANY, wxEmptyString);
    filter_box_->SetHint(L"Filter topics…");
    topic_sizer->Add(filter_box_, 0, wxEXPAND | wxALL, 4);
    topic_tree_ = new wxTreeCtrl(topics_panel_, wxID_ANY, wxDefaultPosition,
                                 wxDefaultSize,
                                 wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES |
                                     wxTR_FULL_ROW_HIGHLIGHT);
    topic_sizer->Add(topic_tree_, 1, wxEXPAND | wxALL, 4);
    topics_panel_->SetSizer(topic_sizer);

    // Starts unsplit: with no PDF loaded there are no bookmarks to show.
    splitter_->Initialize(topics_panel_);
    bookmarks_panel_->Hide();

    outer->Add(splitter_, 1, wxEXPAND);
    SetSizer(outer);

    topic_tree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent& event) {
        if (!on_page_requested_) {
            return;
        }
        const auto row = row_of(topic_tree_, event.GetItem());
        if (row.has_value() && *row < topic_pages_.size()) {
            on_page_requested_(topic_pages_[*row]);
        }
    });

    bookmark_tree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent& event) {
        if (!on_page_requested_) {
            return;
        }
        const auto row = row_of(bookmark_tree_, event.GetItem());
        if (row.has_value() && *row < bookmarks_.size()) {
            on_page_requested_(bookmarks_[*row].page);
        }
    });

    bookmark_tree_->Bind(wxEVT_TREE_ITEM_MENU, &TopicsPane::on_bookmark_menu, this);

    filter_box_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        filter_ = filter_box_->GetValue().utf8_string();
        rebuild_topics();
    });
}

void TopicsPane::load(const fs::path& pdf_path)
{
    pdf_path_ = pdf_path;
    topics_.clear();
    placeholder_.clear();

    if (pdf_path.empty()) {
        clear();
        return;
    }

    bookmarks_ = load_bookmarks(pdf_path);

    const auto metadata = find_metadata_path(pdf_path);
    if (!metadata.has_value()) {
        placeholder_ = "(no metadata file found)";
    } else {
        const TopicLoadResult result = load_metadata(*metadata);
        if (!result.ok) {
            placeholder_ = "(error reading " +
                           path_to_utf8(metadata->filename()) + ")";
            wxMessageBox(wxString::FromUTF8("Could not read " +
                                            path_to_utf8(*metadata) + ":\n\n" +
                                            result.error),
                         L"Metadata error", wxOK | wxICON_ERROR, this);
        } else if (result.entries.empty()) {
            placeholder_ = "(no topics in " +
                           path_to_utf8(metadata->filename()) + ")";
        } else {
            topics_ = result.entries;
        }
    }

    rebuild_topics();
    rebuild_bookmarks();
}

void TopicsPane::clear()
{
    pdf_path_.clear();
    topics_.clear();
    bookmarks_.clear();
    placeholder_.clear();
    rebuild_topics();
    rebuild_bookmarks();
}

void TopicsPane::reload_bookmarks()
{
    if (!pdf_path_.empty()) {
        bookmarks_ = load_bookmarks(pdf_path_);
    }
    rebuild_bookmarks();
}

void TopicsPane::rebuild_topics()
{
    assert(topic_tree_ != nullptr);
    topic_tree_->Freeze();
    topic_tree_->DeleteAllItems();
    topic_pages_.clear();

    const wxTreeItemId root = topic_tree_->AddRoot("root");

    if (!placeholder_.empty()) {
        topic_tree_->AppendItem(root, wxString::FromUTF8(placeholder_));
        topic_tree_->Thaw();
        return;
    }

    const std::string needle = to_lower(filter_);
    int shown = 0;
    for (const TopicEntry& entry : topics_) {
        if (!needle.empty() &&
            to_lower(entry.topic).find(needle) == std::string::npos) {
            continue;
        }
        const wxTreeItemId item =
            topic_tree_->AppendItem(root, wxString::FromUTF8(entry.topic));
        // The row index is stashed in the item rather than the page, because
        // topics and pages both repeat and neither is a usable identity.
        topic_tree_->SetItemData(item, new RowData(topic_pages_.size()));
        topic_pages_.push_back(entry.page);
        ++shown;
    }

    if (shown == 0 && !needle.empty()) {
        topic_tree_->AppendItem(root, L"(no matches)");
    }
    topic_tree_->Thaw();
}

void TopicsPane::rebuild_bookmarks()
{
    assert(bookmark_tree_ != nullptr);
    bookmark_tree_->Freeze();
    bookmark_tree_->DeleteAllItems();

    const wxTreeItemId root = bookmark_tree_->AddRoot("root");
    for (std::size_t i = 0; i < bookmarks_.size(); ++i) {
        const wxTreeItemId item = bookmark_tree_->AppendItem(
            root, wxString::Format("%s  (p.%d)",
                                   wxString::FromUTF8(bookmarks_[i].title),
                                   bookmarks_[i].page));
        bookmark_tree_->SetItemData(item, new RowData(i));
    }
    bookmark_tree_->Thaw();

    // Attach the bookmark pane only while there is something in it, so an
    // unbookmarked PDF gives the topics the whole height.
    const bool want_split = !bookmarks_.empty();
    if (want_split && !splitter_->IsSplit()) {
        bookmarks_panel_->Show();
        splitter_->SplitHorizontally(bookmarks_panel_, topics_panel_,
                                     (wanted_sash_ > 0) ? wanted_sash_
                                                        : kDefaultSash);
    } else if (!want_split && splitter_->IsSplit()) {
        wanted_sash_ = splitter_->GetSashPosition();
        splitter_->Unsplit(bookmarks_panel_);
    }
}

std::optional<int> TopicsPane::sash_position() const
{
    if (splitter_ != nullptr && splitter_->IsSplit()) {
        return splitter_->GetSashPosition();
    }
    // Not split right now, but a position the user chose earlier is still
    // worth persisting rather than discarding.
    return (wanted_sash_ > 0) ? std::optional<int>(wanted_sash_) : std::nullopt;
}

void TopicsPane::set_sash_position(int pixels)
{
    wanted_sash_ = pixels;
    if (splitter_ != nullptr && splitter_->IsSplit() && pixels > 0) {
        splitter_->SetSashPosition(pixels);
    }
}

void TopicsPane::add_bookmark(int page)
{
    if (pdf_path_.empty() || page < 1) {
        return;
    }
    const wxString suggested = wxString::Format("Page %d", page);
    wxTextEntryDialog dialog(this, L"Name for this bookmark:", L"Add bookmark",
                             suggested);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }
    const std::string title = dialog.GetValue().utf8_string();
    if (title.find_first_not_of(" \t") == std::string::npos) {
        return;  // a blank name would make an unclickable row
    }

    bookmarks_.push_back(Bookmark{page, title});
    std::stable_sort(bookmarks_.begin(), bookmarks_.end(),
                     [](const Bookmark& a, const Bookmark& b) {
                         return a.page < b.page;
                     });
    save_bookmarks_now();
    rebuild_bookmarks();
}

void TopicsPane::save_bookmarks_now()
{
    if (pdf_path_.empty()) {
        return;
    }
    if (!save_bookmarks(pdf_path_, bookmarks_)) {
        wxMessageBox(L"Could not write the bookmarks file.\n\n"
                     L"Bookmarks will be kept for this session only.",
                     L"Bookmarks", wxOK | wxICON_WARNING, this);
    }
    if (on_bookmarks_changed_) {
        on_bookmarks_changed_();
    }
}

void TopicsPane::on_bookmark_menu(wxTreeEvent& event)
{
    const auto found = row_of(bookmark_tree_, event.GetItem());
    if (!found.has_value() || *found >= bookmarks_.size()) {
        return;
    }
    const std::size_t row = *found;

    wxMenu menu;
    const int kRename = wxWindow::NewControlId();
    const int kDelete = wxWindow::NewControlId();
    menu.Append(kRename, L"Rename…");
    menu.Append(kDelete, L"Delete");

    menu.Bind(wxEVT_MENU, [this, row](wxCommandEvent&) {
        wxTextEntryDialog dialog(this, L"New name:", L"Rename bookmark",
                                 wxString::FromUTF8(bookmarks_[row].title));
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        const std::string title = dialog.GetValue().utf8_string();
        if (title.find_first_not_of(" \t") == std::string::npos) {
            return;
        }
        bookmarks_[row].title = title;
        save_bookmarks_now();
        rebuild_bookmarks();
    }, kRename);

    menu.Bind(wxEVT_MENU, [this, row](wxCommandEvent&) {
        bookmarks_.erase(bookmarks_.begin() + static_cast<std::ptrdiff_t>(row));
        save_bookmarks_now();
        rebuild_bookmarks();
    }, kDelete);

    PopupMenu(&menu);
}

}  // namespace pdfsherpa
