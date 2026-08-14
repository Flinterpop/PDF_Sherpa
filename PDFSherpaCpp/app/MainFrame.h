// The application window: toolbar, and the three resizable panes
// (PDFs | Topics | Viewer).

#ifndef PDFSHERPA_APP_MAIN_FRAME_H
#define PDFSHERPA_APP_MAIN_FRAME_H

#include <filesystem>
#include <vector>

#include <wx/splitter.h>
#include <wx/tglbtn.h>
#include <wx/wx.h>

#include "Config.h"

namespace pdfsherpa {

class PdfListPane;
class TopicsPane;
class UpdateCheck;
class ViewerPane;

class MainFrame : public wxFrame {
public:
    // `override_root` is a folder given on the command line.  When set it is
    // the only root for this session and is NOT persisted, matching how the
    // Python app lets a command-line folder override the saved one.
    MainFrame(const std::filesystem::path& override_root, Config config);

private:
    void build_ui();
    void bind_shortcuts();
    void open_pdf(const std::filesystem::path& pdf_path);
    void manage_folders();
    void update_status();
    void refresh_folder();
    // Copy dropped PDFs into <folder>/inbox and build any missing .toc files.
    void on_files_dropped(const std::vector<std::filesystem::path>& paths);
    void apply_pane_visibility(bool persist);
    void on_close(wxCloseEvent& event);

    // The Tk geometry string ("WxH+X+Y") the Python app stores, so a profile
    // written by either app restores in the other.
    void restore_geometry();
    std::string current_geometry() const;

    Config config_;
    // True when a command-line folder is in force, which suppresses persisting
    // root changes for the session.
    bool roots_overridden_ = false;
    std::filesystem::path current_pdf_;

    wxSplitterWindow* outer_split_ = nullptr;
    wxSplitterWindow* inner_split_ = nullptr;
    PdfListPane* pdf_list_ = nullptr;
    TopicsPane* topics_ = nullptr;
    ViewerPane* viewer_ = nullptr;

    wxToggleButton* show_pdfs_button_ = nullptr;
    wxToggleButton* show_topics_button_ = nullptr;
    // Owned, but not a unique_ptr: wxEvtHandler-derived objects are kept
    // alive for the frame's lifetime and destroyed with it.
    UpdateCheck* updates_ = nullptr;
    wxTimer update_timer_;

    int outer_sash_ = 300;
    int inner_sash_ = 280;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_MAIN_FRAME_H
