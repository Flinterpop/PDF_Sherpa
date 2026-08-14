#include "PdfListPane.h"

#include <algorithm>
#include <cassert>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // WM_HSCROLL, to undo EnsureVisible's sideways scroll
#include <shellapi.h>  // ShellExecuteW, for Open PDF / Reveal in Explorer

#include "Config.h"
#include "Metadata.h"
#include "PathUtf8.h"

namespace pdfsherpa {
namespace {

namespace fs = std::filesystem;

// PDFs whose row is drawn in blue because they carry the user's bookmarks.
const wxColour kBookmarkedColour(0x1E, 0x5A, 0xA8);
const wxColour kNoMetadataColour(0x80, 0x80, 0x80);

std::string to_lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool is_pdf(const fs::path& path)
{
    std::string ext = path_to_utf8(path.extension());
    return to_lower(std::move(ext)) == ".pdf";
}

// Root-relative, '/'-separated, for display and for the expanded-folder keys.
std::string relative_dir(const fs::path& root, const fs::path& file)
{
    std::error_code ec;
    const fs::path rel = fs::relative(file.parent_path(), root, ec);
    if (ec || rel.empty() || rel == ".") {
        return {};
    }
    std::string text = path_to_utf8(rel);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

}  // namespace

PdfListPane::PdfListPane(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    build_ui();
}

void PdfListPane::build_ui()
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    outer->Add(new wxStaticText(this, wxID_ANY, L"Favorites"), 0,
               wxLEFT | wxTOP, 4);
    favorites_list_ = new wxListBox(this, wxID_ANY, wxDefaultPosition,
                                    wxSize(-1, 90));
    outer->Add(favorites_list_, 0, wxEXPAND | wxALL, 4);

    filter_box_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString);
    // Non-ASCII in a narrow literal is decoded in the ANSI codepage and comes
    // out as mojibake; the ellipsis here must be a wide literal.
    filter_box_->SetHint(L"Filter PDFs…");
    outer->Add(filter_box_, 0, wxEXPAND | wxALL, 4);

    tree_ = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxTR_HAS_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE |
                               wxTR_LINES_AT_ROOT);
    outer->Add(tree_, 1, wxEXPAND | wxALL, 4);

    SetSizer(outer);

    tree_->Bind(wxEVT_TREE_SEL_CHANGED, &PdfListPane::on_selection_changed, this);
    filter_box_->Bind(wxEVT_TEXT, &PdfListPane::on_filter_changed, this);
    tree_->Bind(wxEVT_TREE_ITEM_MENU, &PdfListPane::on_item_menu, this);
    favorites_list_->Bind(wxEVT_CONTEXT_MENU, &PdfListPane::on_favorite_menu, this);

    favorites_list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) {
        const int index = event.GetSelection();
        if (index < 0 || static_cast<std::size_t>(index) >= favorites_.size()) {
            return;
        }
        const fs::path path = favorite_absolute(favorites_[static_cast<std::size_t>(index)]);
        if (!select_pdf(path) && on_selected_) {
            // Not listed under the current folder (an absolute favorite from
            // elsewhere): open it directly rather than doing nothing.
            on_selected_(path);
        }
    });
}

void PdfListPane::set_folder(const fs::path& folder)
{
    folder_ = folder;
    rescan();
}

void PdfListPane::rescan()
{
    entries_.clear();

    std::error_code ec;
    if (folder_.empty() || !fs::is_directory(folder_, ec)) {
        rebuild_tree();
        return;
    }

    // operator++ on a recursive_directory_iterator THROWS on an unreadable
    // entry even when the iterator was built with the error_code overload, so
    // the walk is explicit.  A range-for here dies on the first locked folder.
    fs::recursive_directory_iterator it(
        folder_, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;

    // A bound the walk cannot exceed, so a pathological tree (or a symlink
    // loop the iterator does not catch) cannot hang the UI indefinitely.
    constexpr std::size_t kMaxEntries = 200000;
    std::size_t visited = 0;

    while (!ec && it != end && visited < kMaxEntries) {
        ++visited;
        const fs::directory_entry entry = *it;
        if (entry.is_regular_file(ec) && is_pdf(entry.path())) {
            PdfEntry pdf;
            pdf.path = entry.path();
            pdf.relative_dir = relative_dir(folder_, entry.path());
            pdf.has_metadata = find_metadata_path(entry.path()).has_value();
            pdf.has_bookmarks = !load_bookmarks(entry.path()).empty();
            entries_.push_back(std::move(pdf));
        }
        it.increment(ec);
    }

    std::sort(entries_.begin(), entries_.end(),
              [](const PdfEntry& a, const PdfEntry& b) {
                  if (a.relative_dir != b.relative_dir) {
                      return a.relative_dir < b.relative_dir;
                  }
                  return a.path.filename() < b.path.filename();
              });

    rebuild_tree();
    rebuild_favorites();
}

void PdfListPane::rebuild_tree()
{
    assert(tree_ != nullptr);
    tree_->Freeze();
    tree_->DeleteAllItems();
    pdf_by_item_.clear();
    dir_by_item_.clear();

    const wxTreeItemId root = tree_->AddRoot("root");
    std::map<std::string, wxTreeItemId> folder_items;
    const std::string needle = to_lower(filter_);

    for (const PdfEntry& entry : entries_) {
        const std::string name = path_to_utf8(entry.path.filename());
        if (!needle.empty() && to_lower(name).find(needle) == std::string::npos) {
            continue;  // folders with no surviving child simply never appear
        }

        wxTreeItemId parent = root;
        if (!entry.relative_dir.empty()) {
            const auto found = folder_items.find(entry.relative_dir);
            if (found != folder_items.end()) {
                parent = found->second;
            } else {
                parent = tree_->AppendItem(
                    root, wxString::FromUTF8(entry.relative_dir));
                tree_->SetItemBold(parent, true);
                folder_items.emplace(entry.relative_dir, parent);
                dir_by_item_.emplace(parent, entry.relative_dir);
            }
        }

        wxString label = wxString::FromUTF8(name);
        if (!entry.has_metadata) {
            label += L"  (no metadata)";
        }
        const wxTreeItemId item = tree_->AppendItem(parent, label);
        pdf_by_item_.emplace(item, entry.path);

        if (entry.has_bookmarks) {
            tree_->SetItemTextColour(item, kBookmarkedColour);
        } else if (!entry.has_metadata) {
            tree_->SetItemTextColour(item, kNoMetadataColour);
        }
    }

    // Restore the folders the user had open, or open everything while a
    // filter is active so matches are visible without hunting.
    for (const auto& pair : dir_by_item_) {
        if (!needle.empty() || expanded_.count(pair.second) != 0) {
            tree_->Expand(pair.first);
        }
    }

    tree_->Thaw();
}

void PdfListPane::rebuild_favorites()
{
    assert(favorites_list_ != nullptr);
    favorites_list_->Freeze();
    favorites_list_->Clear();
    for (const std::string& stored : favorites_) {
        favorites_list_->Append(wxString::FromUTF8(stored));
    }
    favorites_list_->Thaw();
}

void PdfListPane::refresh_row(const fs::path& pdf_path)
{
    for (PdfEntry& entry : entries_) {
        if (entry.path == pdf_path) {
            entry.has_metadata = find_metadata_path(pdf_path).has_value();
            entry.has_bookmarks = !load_bookmarks(pdf_path).empty();
            break;
        }
    }
    rebuild_tree();
}

bool PdfListPane::select_pdf(const fs::path& pdf_path)
{
    const std::string want = page_key(pdf_path);
    for (const auto& pair : pdf_by_item_) {
        if (page_key(pair.second) == want) {
            tree_->SelectItem(pair.first);
            tree_->EnsureVisible(pair.first);
            // EnsureVisible scrolls horizontally to bring the whole label into
            // view, which on a deep folder with long filenames pushes the
            // start of every *other* row off the left edge.  The filename
            // matters more than its tail, so scroll back to the left margin.
            ::SendMessageW(tree_->GetHandle(), WM_HSCROLL, SB_LEFT, 0);
            return true;
        }
    }
    return false;
}

void PdfListPane::on_selection_changed(wxTreeEvent& event)
{
    const auto found = pdf_by_item_.find(event.GetItem());
    if (found != pdf_by_item_.end() && on_selected_) {
        on_selected_(found->second);
    }
}

void PdfListPane::on_filter_changed(wxCommandEvent&)
{
    filter_ = filter_box_->GetValue().utf8_string();
    rebuild_tree();
}

std::vector<std::string> PdfListPane::expanded_folders() const
{
    std::vector<std::string> open;
    for (const auto& pair : dir_by_item_) {
        if (tree_->IsExpanded(pair.first)) {
            open.push_back(pair.second);
        }
    }
    std::sort(open.begin(), open.end());
    return open;
}

void PdfListPane::set_expanded_folders(std::vector<std::string> folders)
{
    expanded_ = std::set<std::string>(folders.begin(), folders.end());
    rebuild_tree();
}

std::string PdfListPane::favorite_store_form(const fs::path& pdf_path) const
{
    std::error_code ec;
    const fs::path rel = fs::relative(pdf_path, folder_, ec);
    // A path that climbs out of the root is not "under" it; keep those
    // absolute so they still resolve after the root changes.
    if (ec || rel.empty() || path_to_utf8(rel).rfind("..", 0) == 0) {
        return path_to_utf8(pdf_path);
    }
    std::string text = path_to_utf8(rel);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

fs::path PdfListPane::favorite_absolute(const std::string& stored) const
{
    const fs::path candidate = path_from_utf8(stored);
    if (candidate.is_absolute()) {
        return candidate;
    }
    return folder_ / candidate;
}

bool PdfListPane::is_favorite(const fs::path& pdf_path) const
{
    const std::string key = page_key(pdf_path);
    return std::any_of(favorites_.begin(), favorites_.end(),
                       [this, &key](const std::string& stored) {
                           return page_key(favorite_absolute(stored)) == key;
                       });
}

void PdfListPane::set_favorites(std::vector<std::string> favorites)
{
    favorites_ = std::move(favorites);
    rebuild_favorites();
}

void PdfListPane::toggle_favorite(const fs::path& pdf_path)
{
    const std::string key = page_key(pdf_path);
    const auto stale = std::remove_if(
        favorites_.begin(), favorites_.end(),
        [this, &key](const std::string& stored) {
            return page_key(favorite_absolute(stored)) == key;
        });

    if (stale != favorites_.end()) {
        favorites_.erase(stale, favorites_.end());
    } else {
        // Newest first, oldest dropped past the cap.
        favorites_.insert(favorites_.begin(), favorite_store_form(pdf_path));
        if (favorites_.size() > kMaxFavorites) {
            favorites_.resize(kMaxFavorites);
        }
    }

    rebuild_favorites();
    if (on_favorites_changed_) {
        on_favorites_changed_();
    }
}

void PdfListPane::open_externally(const fs::path& path) const
{
    // ShellExecute with no verb runs the file's default association, which is
    // what "Open PDF" means -- the user's own reader, not this one.
    ::ShellExecuteW(nullptr, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void PdfListPane::reveal_in_explorer(const fs::path& path) const
{
    // /select, needs the path quoted: without quotes a filename containing a
    // comma or space opens the wrong folder, or none.
    const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                    SW_SHOWNORMAL);
}

void PdfListPane::on_item_menu(wxTreeEvent& event)
{
    const wxTreeItemId item = event.GetItem();
    if (!item.IsOk()) {
        return;
    }

    wxMenu menu;
    const int kOpen = wxWindow::NewControlId();
    const int kReveal = wxWindow::NewControlId();
    const int kFavorite = wxWindow::NewControlId();

    const auto pdf = pdf_by_item_.find(item);
    if (pdf != pdf_by_item_.end()) {
        const fs::path path = pdf->second;
        menu.Append(kOpen, L"Open PDF");
        menu.Append(kReveal, L"Reveal in Explorer");
        menu.Append(kFavorite, is_favorite(path) ? L"Remove from favorites"
                                                 : L"Add to favorites");
        menu.Bind(wxEVT_MENU, [this, path](wxCommandEvent&) {
            open_externally(path);
        }, kOpen);
        menu.Bind(wxEVT_MENU, [this, path](wxCommandEvent&) {
            reveal_in_explorer(path);
        }, kReveal);
        menu.Bind(wxEVT_MENU, [this, path](wxCommandEvent&) {
            toggle_favorite(path);
        }, kFavorite);
        PopupMenu(&menu);
        return;
    }

    const auto dir = dir_by_item_.find(item);
    if (dir != dir_by_item_.end()) {
        const fs::path path = folder_ / path_from_utf8(dir->second);
        menu.Append(kReveal, L"Open folder in Explorer");
        menu.Bind(wxEVT_MENU, [this, path](wxCommandEvent&) {
            ::ShellExecuteW(nullptr, nullptr, path.c_str(), nullptr, nullptr,
                            SW_SHOWNORMAL);
        }, kReveal);
        PopupMenu(&menu);
    }
}

void PdfListPane::on_favorite_menu(wxContextMenuEvent&)
{
    const int index = favorites_list_->GetSelection();
    if (index < 0 || static_cast<std::size_t>(index) >= favorites_.size()) {
        return;
    }
    const fs::path path =
        favorite_absolute(favorites_[static_cast<std::size_t>(index)]);

    wxMenu menu;
    const int kOpen = wxWindow::NewControlId();
    const int kReveal = wxWindow::NewControlId();
    const int kRemove = wxWindow::NewControlId();
    menu.Append(kOpen, L"Open");
    menu.Append(kReveal, L"Reveal in Explorer");
    menu.Append(kRemove, L"Remove from favorites");

    menu.Bind(wxEVT_MENU, [this, path](wxCommandEvent&) {
        if (!select_pdf(path) && on_selected_) {
            on_selected_(path);
        }
    }, kOpen);
    menu.Bind(wxEVT_MENU, [this, path](wxCommandEvent&) {
        reveal_in_explorer(path);
    }, kReveal);
    menu.Bind(wxEVT_MENU, [this, path](wxCommandEvent&) {
        toggle_favorite(path);
    }, kRemove);

    PopupMenu(&menu);
}

std::vector<fs::path> PdfListPane::pdfs_without_metadata() const
{
    std::vector<fs::path> out;
    for (const PdfEntry& entry : entries_) {
        if (!entry.has_metadata) {
            out.push_back(entry.path);
        }
    }
    return out;
}

}  // namespace pdfsherpa
