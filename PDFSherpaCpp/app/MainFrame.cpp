#include "MainFrame.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include <wx/display.h>
#include <wx/tglbtn.h>
#include <wx/wrapsizer.h>

#include "DropTarget.h"
#include "FoldersDialog.h"
#include "HelpDialog.h"
#include "Metadata.h"
#include "PathUtf8.h"
#include "PdfListPane.h"
#include "ProgressJob.h"
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

// One dropped PDF, after the user has answered for it and before any of the
// work has been done.  `copy` is false when the file the user dropped IS the
// one already in the inbox, which still wants indexing but must not be copied
// over itself.
struct DropItem {
    fs::path src;
    fs::path dest;
    bool copy = true;
};

}  // namespace

MainFrame::MainFrame(const fs::path& override_root, Config config)
    : wxFrame(nullptr, wxID_ANY,
              wxString::Format("%s - V%s", kAppName, kAppVersion),
              wxDefaultPosition, wxSize(kDefaultWidth, kDefaultHeight)),
      config_(std::move(config)),
      roots_overridden_(!override_root.empty())
{
    build_ui();
    bind_shortcuts();
    restore_geometry();

    std::vector<Root> roots = config_.roots();
    if (roots_overridden_) {
        Root root;
        root.path = path_to_utf8(override_root);
        root.name = path_to_utf8(override_root.filename());
        if (root.name.empty()) {
            root.name = root.path;
        }
        roots = {root};
    }

    pdf_list_->set_favorites(config_.favorites());
    // Before set_roots, not after: the scan is asynchronous now, and the set is
    // re-applied by every rebuild, so it has to be in place when the results
    // land rather than pushed at a tree that has no rows yet.
    pdf_list_->set_expanded_folders(config_.expanded_folders());
    // The scan owns the status bar from here on.  A standalone update_status()
    // after set_roots() would overwrite the "Scanning" message the scan has
    // just put there, and the scan finishing is the only moment at which the
    // folder summary is actually true.
    pdf_list_->set_scan_state_handler([this](bool scanning) {
        if (scanning) {
            SetStatusText(L"Scanning folders…");
        } else if (!pending_status_.empty()) {
            SetStatusText(pending_status_);
            pending_status_.clear();
        } else if (!current_pdf_.empty()) {
            // open_pdf() owns the status bar once a document is showing; a
            // scan finishing behind it must not quietly replace the path.
            SetStatusText(wxString::FromUTF8(path_to_utf8(current_pdf_)));
        } else {
            update_status();
        }
    });
    pdf_list_->set_roots(std::move(roots));

    if (config_.bm_sash().has_value()) {
        topics_->set_sash_position(*config_.bm_sash());
    }
    if (config_.fav_sash().has_value()) {
        pdf_list_->set_sash_position(*config_.fav_sash());
    }

    apply_pane_visibility(false);

    // Reopen the PDF the user was last reading, at the page they left off on.
    //
    // The scan started above has not landed, so there is no row to select yet
    // and select_pdf() would silently do nothing.  Open the document straight
    // away -- waiting for a scan of a network folder to finish before showing
    // the page someone was already reading is the wrong trade -- and let its
    // row light up when the tree fills in.  open_pdf() ignores the reopen the
    // selection event then asks for.
    const fs::path last = path_from_utf8(config_.last_pdf());
    std::error_code ec;
    if (!config_.last_pdf().empty() && fs::is_regular_file(last, ec)) {
        open_pdf(last);
        pdf_list_->select_pdf_when_ready(last);
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
    // A wrap sizer for the same reason as the viewer's nav bar: a horizontal
    // box silently clips whatever does not fit off the right edge, and at a
    // narrow window width that hid the Help button completely -- the feature
    // was unreachable rather than merely cramped.  wxREMOVE_LEADING_SPACES
    // only, so the last button on a row is not stretched to fill it.
    auto* bar = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
    auto* choose = new wxButton(root, wxID_ANY, L"Folders…");
    choose->SetToolTip(L"Add, rename, reorder or remove the top-level folders");
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
    // No stretch spacer: in a wrap sizer it would consume the row and force
    // everything after it onto the next line even when there is room.
    bar->AddSpacer(12);
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

    // The title bar and Alt-Tab icon.  PDFSherpa.rc gives the EXE its icon, so
    // Explorer and the shortcut looked right and this was easy to miss -- but
    // a wxFrame does not inherit it, and without this the window wore the
    // generic default.  Load it as a RESOURCE, not by reading the exe as an
    // image file: the file form needs an ICO image handler registered and
    // without one pops "No image handler for type 3 defined" at every launch.
    // "#1" is the ordinal the .rc assigns it.
    wxIcon icon;
    if (icon.LoadFile("#1", wxBITMAP_TYPE_ICO_RESOURCE)) {
        SetIcon(icon);
    }

    CreateStatusBar();
    update_status();

    // -- wiring --
    pdf_list_->set_selection_handler(
        [this](const fs::path& path) { open_pdf(path); });
    pdf_list_->set_favorites_changed_handler([this]() {
        config_.save_favorites(pdf_list_->favorites());
    });
    pdf_list_->set_flat_hooks(
        [this](const fs::path& folder) {
            return config_.is_flat_folder(path_to_utf8(folder));
        },
        [this](const fs::path& folder) {
            const std::string key = path_to_utf8(folder);
            config_.set_flat_folder(key, !config_.is_flat_folder(key));
            // Rebuild so the change is visible immediately; the expansion
            // state is reapplied because flattening removes folder rows the
            // tree was remembering.
            pdf_list_->rescan();
            pdf_list_->set_expanded_folders(config_.expanded_folders());
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
    viewer_->set_fit_changed_handler([this](FitMode mode) {
        config_.save_fit_pref(mode == FitMode::kPage ? "page" : "width");
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

    choose->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { manage_folders(); });
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_folder(); });
    bookmark->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (viewer_->has_document()) {
            topics_->add_bookmark(viewer_->current_page() + 1);
        }
    });
    help->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // The manual check always answers, including "up to date", and still
        // offers a version the user previously skipped -- which is exactly
        // what HELP.md promises this button does.
        HelpDialog dialog(this, [this]() { updates_->start(true); });
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
        // The manual check always answers, including "up to date", and still
        // offers a version the user previously skipped -- which is exactly
        // what HELP.md promises this button does.
        HelpDialog dialog(this, [this]() { updates_->start(true); });
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
    // Already showing: do nothing.  Selecting a row fires the selection event,
    // which lands here, and the deferred selection that follows an
    // asynchronous scan therefore asks for the document that is already open.
    // Reloading it would throw away the page the user is on and re-prompt
    // about annotations they have already answered for.
    if (pdf_path == current_pdf_ && viewer_->has_document()) {
        return;
    }

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

void MainFrame::manage_folders()
{
    FoldersDialog dialog(this, pdf_list_->roots());
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    std::vector<Root> roots = dialog.roots();
    // Before set_roots, which starts the scan: the expansion set is re-applied
    // by every rebuild, so it has to be in place for the one that follows.
    pdf_list_->set_expanded_folders(config_.expanded_folders());
    pdf_list_->set_roots(roots);

    // A command-line folder is a session override; persisting a change made
    // on top of it would quietly replace the user's saved folders.
    if (!roots_overridden_) {
        config_.save_roots(std::move(roots));
    }
    // No update_status() here: set_roots put "Scanning" in the status bar and
    // the scan-state handler reports the new folders when it finishes.  Doing
    // it now would only overwrite that with a summary of a scan still running.
}

void MainFrame::update_status()
{
    const std::vector<Root>& roots = pdf_list_->roots();
    if (roots.empty()) {
        SetStatusText(L"No folders configured — use Folders… to add one");
    } else if (roots.size() == 1) {
        SetStatusText(wxString::FromUTF8(roots.front().path));
    } else {
        SetStatusText(wxString::Format("%zu folders", roots.size()));
    }
}

void MainFrame::refresh_folder()
{
    // A scan already in flight has not produced its entry list yet, so
    // pdfs_without_metadata() would answer from the previous one and offer to
    // rebuild topics for files that may no longer be there.  Let it finish.
    if (pdf_list_->scanning()) {
        return;
    }

    const std::vector<fs::path> missing = pdf_list_->pdfs_without_metadata();
    if (missing.empty()) {
        pdf_list_->set_expanded_folders(config_.expanded_folders());
        pdf_list_->rescan();
        return;
    }

    const wxString question = wxString::Format(
        "%zu PDF(s) here have no topics file.\n\nBuild topic lists for "
        "them now?", missing.size());
    if (wxMessageBox(question, L"Refresh", wxYES_NO | wxICON_QUESTION,
                     this) != wxYES) {
        pdf_list_->set_expanded_folders(config_.expanded_folders());
        pdf_list_->rescan();
        return;
    }

    // write_toc() reads text from every page of every one of these, which is
    // where the window used to freeze for minutes behind a busy cursor.  The
    // shared_ptr is what lets the worklist outlive this function: the job runs
    // on a worker and the callbacks below are called long after we return.
    auto work = std::make_shared<std::vector<fs::path>>(missing);
    run_progress_job(
        this, L"Building topic lists", work->size(),
        [work](std::size_t index) {
            return path_to_utf8((*work)[index].filename());
        },
        [work](std::size_t index) -> std::string {
            // Worker thread: write_toc opens its own PdfDocument, and so its
            // own fz_context, which is the only way MuPDF may be used here.
            const TocResult result = write_toc((*work)[index]);
            if (result.ok) {
                return {};
            }
            return path_to_utf8((*work)[index].filename()) + ": " +
                   result.error;
        },
        [this](ProgressJobResult result) {
            pending_status_ = wxString::Format(
                result.cancelled ? "Built %zu topic list(s) before cancelling"
                                 : "Built %zu topic list(s)",
                result.succeeded);
            report_job_errors(L"Refresh", result.errors);
            pdf_list_->set_expanded_folders(config_.expanded_folders());
            pdf_list_->rescan();
        });
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

    // Dropped PDFs go to the inbox of the root holding the current
    // selection, falling back to the first root -- see PdfListPane::drop_root.
    const fs::path drop_base = pdf_list_->drop_root();
    if (drop_base.empty()) {
        wxMessageBox(L"Add a folder first, with the Folders… button.",
                     L"Drop PDFs", wxOK | wxICON_INFORMATION, this);
        return;
    }
    const fs::path inbox = drop_base / "inbox";
    fs::create_directories(inbox, ec);
    if (ec) {
        wxMessageBox(wxString::FromUTF8("Could not create " +
                                        path_to_utf8(inbox) + ":\n\n" +
                                        ec.message()),
                     L"Drop PDFs", wxOK | wxICON_ERROR, this);
        return;
    }

    // Pass one, on the UI thread: every question the user has to answer, and
    // no work beyond the stats needed to ask them.  The prompts have to be
    // here, and they have to be finished before the copying starts -- a worker
    // cannot put up a dialog, and interleaving the two would mean the window
    // freezing between questions, which is the bug being fixed.
    auto work = std::make_shared<std::vector<DropItem>>();
    for (const fs::path& src : pdfs) {
        DropItem item;
        item.src = src;
        item.dest = inbox / src.filename();

        if (fs::exists(item.dest, ec)) {
            if (fs::equivalent(src, item.dest, ec) && !ec) {
                item.copy = false;  // already in the inbox; index it in place
                work->push_back(std::move(item));
                continue;
            }
            const std::string name = path_to_utf8(src.filename());
            if (wxMessageBox(wxString::FromUTF8(name +
                                 " is already in the inbox.\nReplace it?"),
                             L"Replace PDF?", wxYES_NO | wxICON_QUESTION,
                             this) != wxYES) {
                continue;
            }
        }
        ec.clear();
        work->push_back(std::move(item));
    }
    if (work->empty()) {
        return;  // every one of them was declined
    }

    // Pass two, on a worker: the copy and the topic build, both of which are
    // slow enough on a large document to stall the message loop.
    const fs::path last_added = work->back().dest;
    const std::size_t wanted = work->size();
    run_progress_job(
        this, L"Adding PDFs", wanted,
        [work](std::size_t index) {
            return path_to_utf8((*work)[index].src.filename());
        },
        [work](std::size_t index) -> std::string {
            const DropItem& item = (*work)[index];
            const std::string name = path_to_utf8(item.src.filename());
            std::error_code copy_ec;
            if (item.copy) {
                fs::copy_file(item.src, item.dest,
                              fs::copy_options::overwrite_existing, copy_ec);
                if (copy_ec) {
                    return name + ": " + copy_ec.message();
                }
            }
            // Keep an existing (possibly hand-edited) topics file; only
            // generate one when the PDF has no metadata yet.
            if (find_metadata_path(item.dest).has_value()) {
                return {};
            }
            const TocResult result = write_toc(item.dest);
            if (result.ok) {
                return {};
            }
            return name + ": added, but no topics built (" + result.error + ")";
        },
        [this, last_added, wanted](ProgressJobResult result) {
            // `succeeded`, not `completed`: a file that copied but whose topic
            // build failed is counted as a problem and named in the dialog, so
            // the number here must not quietly claim it went through.
            pending_status_ = wxString::Format(
                "Added %zu of %zu PDF(s) to the inbox", result.succeeded,
                wanted);
            report_job_errors(L"Drop PDFs", result.errors);

            // Show the inbox open so the new files are actually visible.
            std::vector<std::string> expanded = config_.expanded_folders();
            if (std::find(expanded.begin(), expanded.end(), "inbox") ==
                expanded.end()) {
                expanded.push_back("inbox");
                config_.save_expanded_folders(expanded);
            }
            pdf_list_->set_expanded_folders(config_.expanded_folders());
            pdf_list_->rescan();
            // Not select_pdf(): the rescan just started has no rows yet.
            pdf_list_->select_pdf_when_ready(last_added);
        });
}

void MainFrame::report_job_errors(const wxString& title,
                                  const std::vector<std::string>& errors)
{
    if (errors.empty()) {
        return;
    }
    constexpr std::size_t kMaxShown = 10;
    std::string detail;
    for (std::size_t i = 0; i < errors.size() && i < kMaxShown; ++i) {
        detail += errors[i] + "\n";
    }
    if (errors.size() > kMaxShown) {
        detail += "... and " + std::to_string(errors.size() - kMaxShown) +
                  " more\n";
    }
    wxMessageBox(wxString::FromUTF8("Problems:\n" + detail), title,
                 wxOK | wxICON_WARNING, this);
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
    config_.save_window_state(current_geometry(), topics_->sash_position(),
                              pdf_list_->sash_position());
    config_.save_expanded_folders(pdf_list_->expanded_folders());
    event.Skip();
}

}  // namespace pdfsherpa
