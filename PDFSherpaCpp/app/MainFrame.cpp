#include "MainFrame.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include <wx/display.h>
#include <wx/tglbtn.h>

#include "DropTarget.h"
#include "HelpDialog.h"
#include "Metadata.h"
#include "PathUtf8.h"
#include "PdfListPane.h"
#include "TocGen.h"
#include "TopicsPane.h"
#include "UpdateCheck.h"
#include "Version.h"
#include "ViewerPane.h"

namespace pdfsherpa {
namespace {

namespace fs = std::filesystem;

constexpr int kDefaultWidth = 1100;
constexpr int kDefaultHeight = 720;

// A distinct id for the update timer.
//
// wxTimer given an owner posts its events to THAT owner, not to itself, so
// timer.Bind(wxEVT_TIMER, ...) never fires.  The working shape is a distinct
// id plus a Bind on the owner, which is what this constant exists for.
constexpr int kUpdateTimerId = 1001;

// How long after the window appears to check for updates.  app.py uses 2 s,
// for the same reason: the check must not compete with the first render.
constexpr int kUpdateDelayMs = 2000;

}  // namespace

MainFrame::MainFrame(const fs::path& folder, Config config)
    : wxFrame(nullptr, wxID_ANY,
              wxString::Format("%s - V%s", kAppName, kAppVersion),
              wxDefaultPosition, wxSize(kDefaultWidth, kDefaultHeight)),
      config_(std::move(config)),
      folder_(folder)
{
    build_ui();
    bind_shortcuts();
    restore_geometry();

    pdf_list_->set_favorites(config_.favorites());
    pdf_list_->set_folder(folder_);
    pdf_list_->set_expanded_folders(config_.expanded_folders());

    if (config_.bm_sash().has_value()) {
        topics_->set_sash_position(*config_.bm_sash());
    }

    apply_pane_visibility(false);

    // Reopen the PDF the user was last reading, at the page they left off on.
    const fs::path last = path_from_utf8(config_.last_pdf());
    std::error_code ec;
    if (!config_.last_pdf().empty() && fs::is_regular_file(last, ec)) {
        if (!pdf_list_->select_pdf(last)) {
            open_pdf(last);  // still listed nowhere, but it does exist
        }
    }

    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::on_close, this);

    // Check for a newer release once the window has settled.
    updates_ = new UpdateCheck(this, &config_);
    update_timer_.SetOwner(this, kUpdateTimerId);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { updates_->start(false); },
         kUpdateTimerId);
    update_timer_.StartOnce(kUpdateDelayMs);
}

void MainFrame::build_ui()
{
    auto* root = new wxPanel(this, wxID_ANY);
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // -- toolbar --
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* choose = new wxButton(root, wxID_ANY, L"Choose folder…");
    auto* refresh = new wxButton(root, wxID_ANY, L"Refresh");
    show_pdfs_button_ = new wxToggleButton(root, wxID_ANY, L"PDFs");
    show_topics_button_ = new wxToggleButton(root, wxID_ANY, L"Topics");
    // The Python app labels this button with a bookmark emoji, which Tk gets
    // for free from the system emoji fallback.  wx's button font has no such
    // fallback here and renders a substitution box, so this one says what it
    // does instead.
    auto* bookmark = new wxButton(root, wxID_ANY, L"Bookmark");
    bookmark->SetToolTip(L"Bookmark this page (Ctrl+B)");
    auto* help = new wxButton(root, wxID_ANY, L"Help");
    help->SetToolTip(L"Show the help document (F1)");

    bar->Add(choose, 0, wxALL, 3);
    bar->Add(refresh, 0, wxALL, 3);
    bar->AddSpacer(12);
    bar->Add(show_pdfs_button_, 0, wxALL, 3);
    bar->Add(show_topics_button_, 0, wxALL, 3);
    bar->AddStretchSpacer();
    bar->Add(bookmark, 0, wxALL, 3);
    bar->Add(help, 0, wxALL, 3);

    // -- panes --
    outer_split_ = new wxSplitterWindow(root, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize,
                                        wxSP_LIVE_UPDATE | wxSP_3DSASH);
    outer_split_->SetMinimumPaneSize(120);

    pdf_list_ = new PdfListPane(outer_split_);

    inner_split_ = new wxSplitterWindow(outer_split_, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize,
                                        wxSP_LIVE_UPDATE | wxSP_3DSASH);
    inner_split_->SetMinimumPaneSize(120);

    topics_ = new TopicsPane(inner_split_);
    const FitMode initial_fit =
        (config_.fit_pref() == "page") ? FitMode::kPage : FitMode::kWidth;
    viewer_ = new ViewerPane(inner_split_, initial_fit);

    inner_split_->SplitVertically(topics_, viewer_, inner_sash_);
    outer_split_->SplitVertically(pdf_list_, inner_split_, outer_sash_);

    outer->Add(bar, 0, wxEXPAND);
    outer->Add(outer_split_, 1, wxEXPAND);
    root->SetSizer(outer);

    auto* frame_sizer = new wxBoxSizer(wxVERTICAL);
    frame_sizer->Add(root, 1, wxEXPAND);
    SetSizer(frame_sizer);

    CreateStatusBar();
    SetStatusText(wxString::FromUTF8(path_to_utf8(folder_)));

    // -- wiring --
    pdf_list_->set_selection_handler(
        [this](const fs::path& path) { open_pdf(path); });
    pdf_list_->set_favorites_changed_handler([this]() {
        config_.save_favorites(pdf_list_->favorites());
    });

    topics_->set_page_requested_handler([this](int page_1based) {
        viewer_->goto_page(page_1based - 1);
    });
    topics_->set_bookmarks_changed_handler([this]() {
        if (!current_pdf_.empty()) {
            pdf_list_->refresh_row(current_pdf_);
        }
    });

    viewer_->set_page_changed_handler([this](int page_index) {
        if (!current_pdf_.empty()) {
            config_.save_last_page(current_pdf_, page_index);
        }
    });
    viewer_->set_bookmark_requested_handler(
        [this](int page_1based) { topics_->add_bookmark(page_1based); });
    viewer_->set_documents_changed_handler([this]() {
        // An "(ann)" copy was just written; it is a new file in the folder.
        pdf_list_->rescan();
    });

    // One drop target on the frame covers the whole window: a drop over any
    // child bubbles up to the nearest ancestor that accepts files.
    SetDropTarget(new PdfDropTarget([this](const std::vector<fs::path>& paths) {
        on_files_dropped(paths);
    }));

    choose->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose_folder(); });
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_folder(); });
    bookmark->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (viewer_->has_document()) {
            topics_->add_bookmark(viewer_->current_page() + 1);
        }
    });
    help->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        HelpDialog dialog(this);
        dialog.ShowModal();
    });

    show_pdfs_button_->Bind(wxEVT_TOGGLEBUTTON,
                            [this](wxCommandEvent&) { apply_pane_visibility(true); });
    show_topics_button_->Bind(wxEVT_TOGGLEBUTTON,
                              [this](wxCommandEvent&) { apply_pane_visibility(true); });
}

void MainFrame::bind_shortcuts()
{
    const int kBookmark = wxWindow::NewControlId();
    const int kNextPage = wxWindow::NewControlId();
    const int kPrevPage = wxWindow::NewControlId();
    const int kRefresh = wxWindow::NewControlId();
    const int kFind = wxWindow::NewControlId();
    const int kFindNext = wxWindow::NewControlId();
    const int kFindPrev = wxWindow::NewControlId();
    const int kHelp = wxWindow::NewControlId();

    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (viewer_->has_document()) {
            topics_->add_bookmark(viewer_->current_page() + 1);
        }
    }, kBookmark);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { viewer_->next_page(); }, kNextPage);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { viewer_->prev_page(); }, kPrevPage);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { refresh_folder(); }, kRefresh);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { viewer_->focus_search(); }, kFind);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { viewer_->next_match(); }, kFindNext);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { viewer_->prev_match(); }, kFindPrev);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        HelpDialog dialog(this);
        dialog.ShowModal();
    }, kHelp);

    const wxAcceleratorEntry entries[] = {
        wxAcceleratorEntry(wxACCEL_CTRL, 'B', kBookmark),
        wxAcceleratorEntry(wxACCEL_NORMAL, WXK_PAGEDOWN, kNextPage),
        wxAcceleratorEntry(wxACCEL_NORMAL, WXK_PAGEUP, kPrevPage),
        wxAcceleratorEntry(wxACCEL_NORMAL, WXK_F5, kRefresh),
        wxAcceleratorEntry(wxACCEL_CTRL, 'F', kFind),
        wxAcceleratorEntry(wxACCEL_NORMAL, WXK_F3, kFindNext),
        wxAcceleratorEntry(wxACCEL_SHIFT, WXK_F3, kFindPrev),
        wxAcceleratorEntry(wxACCEL_NORMAL, WXK_F1, kHelp),
    };
    SetAcceleratorTable(
        wxAcceleratorTable(static_cast<int>(std::size(entries)), entries));
}

void MainFrame::open_pdf(const fs::path& pdf_path)
{
    // Unsaved highlights belong to the document that is about to go away, so
    // offer to keep them before it does.  A cancel abandons the switch.
    if (!viewer_->maybe_save_annotations()) {
        return;
    }
    current_pdf_ = pdf_path;

    const int page = config_.last_page(pdf_path).value_or(0);
    if (!viewer_->load(pdf_path, page)) {
        SetStatusText(wxString::FromUTF8("Could not open " +
                                         path_to_utf8(pdf_path.filename())));
        return;
    }

    topics_->load(pdf_path);
    config_.save_last_pdf(path_to_utf8(pdf_path));
    SetStatusText(wxString::FromUTF8(path_to_utf8(pdf_path)));
}

void MainFrame::choose_folder()
{
    wxDirDialog dialog(this, L"Choose a folder of PDFs",
                       wxString::FromUTF8(path_to_utf8(folder_)),
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }
    folder_ = path_from_utf8(dialog.GetPath().utf8_string());
    config_.save_folder(path_to_utf8(folder_));
    pdf_list_->set_folder(folder_);
    SetStatusText(wxString::FromUTF8(path_to_utf8(folder_)));
}

void MainFrame::refresh_folder()
{
    const std::vector<fs::path> missing = pdf_list_->pdfs_without_metadata();
    if (!missing.empty()) {
        const wxString question = wxString::Format(
            "%zu PDF(s) here have no topics file.\n\nBuild topic lists for "
            "them now?", missing.size());
        if (wxMessageBox(question, L"Refresh", wxYES_NO | wxICON_QUESTION,
                         this) == wxYES) {
            wxBusyCursor busy;
            int built = 0;
            for (const fs::path& pdf : missing) {
                if (write_toc(pdf).ok) {
                    ++built;
                }
            }
            SetStatusText(wxString::Format("Built %d topic list(s)", built));
        }
    }
    pdf_list_->rescan();
    pdf_list_->set_expanded_folders(config_.expanded_folders());
}

void MainFrame::on_files_dropped(const std::vector<fs::path>& paths)
{
    std::vector<fs::path> pdfs;
    std::error_code ec;
    for (const fs::path& path : paths) {
        std::string ext = path_to_utf8(path.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".pdf" && fs::is_regular_file(path, ec)) {
            pdfs.push_back(path);
        }
    }
    if (pdfs.empty()) {
        wxMessageBox(L"Drop one or more .pdf files to add them to the inbox.",
                     L"Drop PDFs", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const fs::path inbox = folder_ / "inbox";
    fs::create_directories(inbox, ec);
    if (ec) {
        wxMessageBox(wxString::FromUTF8("Could not create " +
                                        path_to_utf8(inbox) + ":\n\n" +
                                        ec.message()),
                     L"Drop PDFs", wxOK | wxICON_ERROR, this);
        return;
    }

    wxBusyCursor busy;
    std::vector<fs::path> added;
    std::vector<std::string> failed;

    for (const fs::path& src : pdfs) {
        const fs::path dest = inbox / src.filename();
        const std::string name = path_to_utf8(src.filename());

        if (fs::exists(dest, ec)) {
            if (fs::equivalent(src, dest, ec) && !ec) {
                added.push_back(dest);  // already in the inbox
                continue;
            }
            if (wxMessageBox(wxString::FromUTF8(name +
                                 " is already in the inbox.\nReplace it?"),
                             L"Replace PDF?", wxYES_NO | wxICON_QUESTION,
                             this) != wxYES) {
                continue;
            }
        }

        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            failed.push_back(name + ": " + ec.message());
            ec.clear();
            continue;
        }
        added.push_back(dest);

        // Keep an existing (possibly hand-edited) topics file; only generate
        // one when the PDF has no metadata yet.
        if (!find_metadata_path(dest).has_value()) {
            const TocResult result = write_toc(dest);
            if (!result.ok) {
                failed.push_back(name + ": added, but no topics built (" +
                                 result.error + ")");
            }
        }
    }

    if (!added.empty()) {
        // Show the inbox open so the new files are actually visible.
        std::vector<std::string> expanded = config_.expanded_folders();
        if (std::find(expanded.begin(), expanded.end(), "inbox") ==
            expanded.end()) {
            expanded.push_back("inbox");
            config_.save_expanded_folders(expanded);
        }
        pdf_list_->rescan();
        pdf_list_->set_expanded_folders(config_.expanded_folders());
        pdf_list_->select_pdf(added.back());
    }

    if (!failed.empty()) {
        std::string detail;
        for (std::size_t i = 0; i < failed.size() && i < 10; ++i) {
            detail += failed[i] + "\n";
        }
        wxMessageBox(
            wxString::FromUTF8("Added " + std::to_string(added.size()) +
                               " PDF(s) to the inbox.\n\nProblems:\n" + detail),
            L"Drop PDFs", wxOK | wxICON_WARNING, this);
    }
}

void MainFrame::apply_pane_visibility(bool persist)
{
    if (!persist) {
        // Initial call: adopt the saved state rather than reading the buttons,
        // which have not been set yet.
        show_pdfs_button_->SetValue(config_.show_pdf_list());
        show_topics_button_->SetValue(config_.show_topics());
    }

    const bool show_pdfs = show_pdfs_button_->GetValue();
    const bool show_topics = show_topics_button_->GetValue();

    // Remember the sash before collapsing, so restoring puts it back where the
    // user had it rather than at a default.
    if (outer_split_->IsSplit()) {
        outer_sash_ = outer_split_->GetSashPosition();
    }
    if (inner_split_->IsSplit()) {
        inner_sash_ = inner_split_->GetSashPosition();
    }

    if (show_topics && !inner_split_->IsSplit()) {
        topics_->Show();
        inner_split_->SplitVertically(topics_, viewer_, inner_sash_);
    } else if (!show_topics && inner_split_->IsSplit()) {
        inner_split_->Unsplit(topics_);
    }

    if (show_pdfs && !outer_split_->IsSplit()) {
        pdf_list_->Show();
        outer_split_->SplitVertically(pdf_list_, inner_split_, outer_sash_);
    } else if (!show_pdfs && outer_split_->IsSplit()) {
        outer_split_->Unsplit(pdf_list_);
    }

    if (persist) {
        config_.save_pane_visibility(show_pdfs, show_topics);
    }
}

void MainFrame::restore_geometry()
{
    const std::string geometry = config_.geometry();
    if (geometry.empty()) {
        return;
    }
    // Tk's format: WIDTHxHEIGHT+X+Y, where the offsets may be negative on a
    // multi-monitor desktop.
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    // sscanf_s rather than sscanf: the latter is deprecated under /W4 /WX.
    // The _s form needs no extra size arguments here because every conversion
    // is an integer, not a buffer.
    const int fields =
        sscanf_s(geometry.c_str(), "%dx%d+%d+%d", &width, &height, &x, &y);
    if (fields < 2 || width <= 0 || height <= 0) {
        return;
    }
    SetSize(width, height);
    if (fields == 4) {
        // Only honour a position that lands on a display the user still has.
        const wxRect wanted(x, y, width, height);
        if (wxDisplay::GetFromRect(wanted) != wxNOT_FOUND) {
            SetPosition(wxPoint(x, y));
        }
    }
}

std::string MainFrame::current_geometry() const
{
    const wxSize size = GetSize();
    const wxPoint position = GetPosition();
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%dx%d+%d+%d", size.x, size.y,
                  position.x, position.y);
    return buffer;
}

void MainFrame::on_close(wxCloseEvent& event)
{
    if (!viewer_->maybe_save_annotations() && event.CanVeto()) {
        event.Veto();
        return;
    }
    config_.save_window_state(current_geometry(), topics_->sash_position());
    config_.save_expanded_folders(pdf_list_->expanded_folders());
    event.Skip();
}

}  // namespace pdfsherpa
