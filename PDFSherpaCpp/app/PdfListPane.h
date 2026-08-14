// The left-hand pane: favorites, a name filter, and every PDF under the
// current folder grouped by subfolder.

#ifndef PDFSHERPA_APP_PDF_LIST_PANE_H
#define PDFSHERPA_APP_PDF_LIST_PANE_H

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

namespace pdfsherpa {

// One PDF found under the root folder.
struct PdfEntry {
    std::filesystem::path path;
    std::string relative_dir;  // "" for the root, UTF-8, '/'-separated
    bool has_metadata = false;
    bool has_bookmarks = false;
};

class PdfListPane : public wxPanel {
public:
    explicit PdfListPane(wxWindow* parent);

    // Rescan `folder` and rebuild the tree.
    void set_folder(const std::filesystem::path& folder);
    const std::filesystem::path& folder() const { return folder_; }

    // Re-read the bookmark/metadata state of one PDF and recolour its row,
    // without a full rescan.
    void refresh_row(const std::filesystem::path& pdf_path);
    void rescan();

    // Select a PDF if it is listed; returns false when it is not.
    bool select_pdf(const std::filesystem::path& pdf_path);

    void set_selection_handler(std::function<void(const std::filesystem::path&)> h)
    {
        on_selected_ = std::move(h);
    }

    // Folder rows the user has left open, as root-relative paths.
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

    // PDFs under the current folder that have no topics file yet.
    std::vector<std::filesystem::path> pdfs_without_metadata() const;

    // Where the user dragged the favorites/PDF-list divider, in pixels.
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
    // Open in the system PDF viewer / show in Explorer with the file selected.
    void open_externally(const std::filesystem::path& path) const;
    void reveal_in_explorer(const std::filesystem::path& path) const;

    // Favorites are stored root-relative when the PDF is under the current
    // folder, so they survive the folder being moved or renamed; PDFs outside
    // it stay absolute.  Matches _fav_store_form / _fav_abs_path.
    std::string favorite_store_form(const std::filesystem::path& pdf_path) const;
    std::filesystem::path favorite_absolute(const std::string& stored) const;

    std::filesystem::path folder_;
    std::vector<PdfEntry> entries_;
    std::string filter_;
    std::vector<std::string> favorites_;
    std::set<std::string> expanded_;

    wxSplitterWindow* splitter_ = nullptr;
    wxListBox* favorites_list_ = nullptr;
    wxTextCtrl* filter_box_ = nullptr;
    wxTreeCtrl* tree_ = nullptr;
    std::map<wxTreeItemId, std::filesystem::path> pdf_by_item_;
    std::map<wxTreeItemId, std::string> dir_by_item_;

    std::function<void(const std::filesystem::path&)> on_selected_;
    std::function<void()> on_favorites_changed_;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_PDF_LIST_PANE_H
