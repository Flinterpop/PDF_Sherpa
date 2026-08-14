// The left-hand pane: favorites, a name filter, and every PDF under the
// configured top-level folders, grouped by folder.
//
// Each configured root is a top-level node in the tree, with its own subfolder
// hierarchy nested beneath it.

#ifndef PDFSHERPA_APP_PDF_LIST_PANE_H
#define PDFSHERPA_APP_PDF_LIST_PANE_H

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <wx/splitter.h>
#include <wx/treectrl.h>
#include <wx/wx.h>

#include "Config.h"

namespace pdfsherpa {

// One PDF found under a root.
struct PdfEntry {
    std::filesystem::path path;
    std::size_t root_index = 0;  // which configured root it came from
    std::string relative_dir;    // "" at the root, UTF-8, '/'-separated
    bool has_metadata = false;
    bool has_bookmarks = false;
};

class PdfListPane : public wxPanel {
public:
    explicit PdfListPane(wxWindow* parent);

    // Rescan every root and rebuild the tree.
    void set_roots(std::vector<Root> roots);
    const std::vector<Root>& roots() const { return roots_; }

    void refresh_row(const std::filesystem::path& pdf_path);
    void rescan();

    bool select_pdf(const std::filesystem::path& pdf_path);

    void set_selection_handler(std::function<void(const std::filesystem::path&)> h)
    {
        on_selected_ = std::move(h);
    }

    // Folder rows the user has left open, as "<root name>/<relative path>".
    std::vector<std::string> expanded_folders() const;
    void set_expanded_folders(std::vector<std::string> folders);

    const std::vector<std::string>& favorites() const { return favorites_; }
    void set_favorites(std::vector<std::string> favorites);
    void set_favorites_changed_handler(std::function<void()> h)
    {
        on_favorites_changed_ = std::move(h);
    }
    bool is_favorite(const std::filesystem::path& pdf_path) const;
    void toggle_favorite(const std::filesystem::path& pdf_path);

    // PDFs across all roots that have no topics file yet.
    std::vector<std::filesystem::path> pdfs_without_metadata() const;

    // Where a dropped PDF should be filed: the root holding the current
    // selection, or the first root when nothing is selected.  Empty when no
    // roots are configured at all.
    std::filesystem::path drop_root() const;

    std::optional<int> sash_position() const;
    void set_sash_position(int pixels);

private:
    void build_ui();
    void rebuild_tree();
    void rebuild_favorites();
    void on_selection_changed(wxTreeEvent& event);
    void on_filter_changed(wxCommandEvent& event);
    void on_item_menu(wxTreeEvent& event);
    void on_favorite_menu(wxContextMenuEvent& event);
    // The "..." menu beside the Favorites heading.
    void show_favorites_menu();
    void clear_favorites();
    void export_favorites();
    void import_favorites();
    void open_externally(const std::filesystem::path& path) const;
    void reveal_in_explorer(const std::filesystem::path& path) const;

    // Favorites are stored relative to whichever root contains them, so they
    // survive that root being moved or renamed on disk (the behaviour added in
    // 2b0b44c).  With several roots a bare relative path is ambiguous, so
    // resolution walks the roots in order and takes the first that exists.
    // A PDF outside every root is stored absolute.
    std::string favorite_store_form(const std::filesystem::path& pdf_path) const;
    std::filesystem::path favorite_absolute(const std::string& stored) const;

    std::vector<Root> roots_;
    std::vector<PdfEntry> entries_;
    std::string filter_;
    std::vector<std::string> favorites_;
    std::set<std::string> expanded_;

    wxSplitterWindow* splitter_ = nullptr;
    wxListBox* favorites_list_ = nullptr;
    wxTextCtrl* filter_box_ = nullptr;
    wxTreeCtrl* tree_ = nullptr;
    std::map<wxTreeItemId, std::filesystem::path> pdf_by_item_;
    std::map<wxTreeItemId, std::string> dir_by_item_;   // item -> qualified key
    std::map<wxTreeItemId, std::filesystem::path> folder_path_by_item_;

    std::function<void(const std::filesystem::path&)> on_selected_;
    std::function<void()> on_favorites_changed_;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_PDF_LIST_PANE_H
