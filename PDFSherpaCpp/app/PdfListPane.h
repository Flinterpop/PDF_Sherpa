// The left-hand pane: favorites, a name filter, and every PDF under the
// configured top-level folders, grouped by folder.
//
// Each configured root is a top-level node in the tree, with its own subfolder
// hierarchy nested beneath it.
//
// THE SCAN RUNS ON A WORKER THREAD, and anything added here that touches the
// filesystem in bulk must too.  The walk is not just a directory listing: for
// every PDF it stats for a topics file and reads and JSON-parses the sidecar
// bookmarks file, so a root on a network share froze the window for as long as
// that took -- including in the MainFrame constructor, before the first paint.
//
// The shape is the one ViewerPane::start_search already uses:
//
//   - a worker thread, never the UI thread;
//   - a generation counter bumped on each start, so a superseded result is
//     discarded instead of applied out of order.  It is std::atomic because
//     the WORKER polls it mid-walk to abandon a scan the user has already
//     replaced -- a plain unsigned would be a data race;
//   - `alive_`, a shared flag the destructor clears, so a completion lambda
//     can tell the pane is gone;
//   - the results applied by CallAfter on the UI thread, and nowhere else.
//
// Because the scan is now asynchronous, nothing may assume the tree is
// populated when rescan() returns -- see select_pdf_when_ready().

#ifndef PDFSHERPA_APP_PDF_LIST_PANE_H
#define PDFSHERPA_APP_PDF_LIST_PANE_H

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
    ~PdfListPane() override;

    // Start a rescan of every root.  Returns immediately; the tree keeps
    // showing the previous results until the new ones land.
    void set_roots(std::vector<Root> roots);
    const std::vector<Root>& roots() const { return roots_; }

    void refresh_row(const std::filesystem::path& pdf_path);
    void rescan();

    // True from the moment a scan starts until its results have been applied.
    bool scanning() const { return scanning_; }

    // Called on the UI thread when a scan starts (true) and when its results
    // have been applied (false), so the frame can say so in the status bar.
    void set_scan_state_handler(std::function<void(bool)> h)
    {
        on_scan_state_ = std::move(h);
    }

    bool select_pdf(const std::filesystem::path& pdf_path);

    // Select `pdf_path` now if its row exists, otherwise once the scan in
    // flight has landed.  This is what callers want after starting a rescan:
    // select_pdf() alone would silently do nothing, because the rows it looks
    // through are not built yet.  One shot -- a path the next scan does not
    // find is forgotten rather than retried forever.
    void select_pdf_when_ready(const std::filesystem::path& pdf_path);

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

    // Whether a folder is shown as a flat list of every PDF beneath it, and
    // the toggle for it.  Both are supplied by the frame, which owns Config;
    // the pane deliberately does not reach into settings itself.
    void set_flat_hooks(std::function<bool(const std::filesystem::path&)> query,
                        std::function<void(const std::filesystem::path&)> toggle)
    {
        is_flat_folder_ = std::move(query);
        on_toggle_flat_ = std::move(toggle);
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
    // Bump the generation and join the worker, so no in-flight result can be
    // applied afterwards.  Joining rather than detaching is safe because the
    // walk polls the generation on every entry.
    void stop_scan();
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

    std::thread scan_worker_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<unsigned>> scan_generation_;
    bool scanning_ = false;  // UI thread only
    std::filesystem::path pending_selection_;

    wxSplitterWindow* splitter_ = nullptr;
    wxListBox* favorites_list_ = nullptr;
    wxTextCtrl* filter_box_ = nullptr;
    wxTreeCtrl* tree_ = nullptr;
    std::map<wxTreeItemId, std::filesystem::path> pdf_by_item_;
    std::map<wxTreeItemId, std::string> dir_by_item_;   // item -> qualified key
    std::map<wxTreeItemId, std::filesystem::path> folder_path_by_item_;

    std::function<void(const std::filesystem::path&)> on_selected_;
    std::function<void()> on_favorites_changed_;
    std::function<void(bool)> on_scan_state_;
    std::function<bool(const std::filesystem::path&)> is_flat_folder_;
    std::function<void(const std::filesystem::path&)> on_toggle_flat_;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_PDF_LIST_PANE_H
