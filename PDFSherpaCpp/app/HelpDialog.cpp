#include "HelpDialog.h"

#include <fstream>
#include <sstream>

#include <wx/filename.h>
#include <wx/html/htmlwin.h>
#include <wx/stdpaths.h>

#include <md4c-html.h>

#include "PathUtf8.h"
#include "Version.h"

namespace pdfsherpa {
namespace {

namespace fs = std::filesystem;

// Where HELP.md sits relative to the executable: beside it once installed,
// and a few levels up when running straight out of the build tree.
const char* const kHelpCandidates[] = {
    "HELP.md",
    "..\\HELP.md",
    "..\\..\\..\\..\\HELP.md",
    "..\\..\\..\\..\\..\\HELP.md",
};

// md4c hands output back in chunks rather than one string.
void collect_html(const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(text, size);
}

}  // namespace

fs::path find_help_document()
{
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    for (const char* candidate : kHelpCandidates) {
        wxFileName probe(exe.GetPath() + "\\" + candidate);
        probe.MakeAbsolute();
        if (probe.FileExists()) {
            return path_from_utf8(probe.GetFullPath().utf8_string());
        }
    }
    return {};
}

std::string markdown_to_html(const std::string& markdown)
{
    std::string body;
    const unsigned parser_flags = MD_DIALECT_GITHUB;
    const int rc = md_html(markdown.data(), static_cast<MD_SIZE>(markdown.size()),
                           collect_html, &body, parser_flags, 0);
    if (rc != 0) {
        return "<html><body><p>Could not render the help document.</p></body></html>";
    }

    // wxHtmlWindow is a small subset of HTML with no CSS engine, so the
    // styling that matters has to be inline-ish and simple.  Anything fancier
    // renders as literal text.
    std::string html =
        "<html><body bgcolor=\"#ffffff\" text=\"#202020\">"
        "<font face=\"Segoe UI, Arial\" size=\"2\">";
    html += body;
    html += "</font></body></html>";
    return html;
}

HelpDialog::HelpDialog(wxWindow* parent, std::function<void()> on_check_updates)
    : wxDialog(parent, wxID_ANY, wxString(kAppName) + L" Help",
               wxDefaultPosition, wxSize(820, 760),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* view = new wxHtmlWindow(this, wxID_ANY);

    const fs::path help = find_help_document();
    if (help.empty()) {
        view->SetPage(
            "<html><body><h3>Help not found</h3>"
            "<p>HELP.md could not be located beside the application.</p>"
            "</body></html>");
    } else {
        std::ifstream stream(help, std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        view->SetPage(wxString::FromUTF8(markdown_to_html(buffer.str())));
    }

    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    footer->Add(new wxStaticText(this, wxID_ANY,
                                 wxString(kAppName) + L" v" + kAppVersion),
                0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    footer->AddStretchSpacer(1);
    if (on_check_updates) {
        auto* check = new wxButton(this, wxID_ANY, L"Check for updates");
        footer->Add(check, 0, wxALL, 6);
        check->Bind(wxEVT_BUTTON,
                    [handler = std::move(on_check_updates)](wxCommandEvent&) {
                        handler();
                    });
    }
    auto* about = new wxButton(this, wxID_ANY, L"About");
    footer->Add(about, 0, wxALL, 6);
    footer->Add(new wxButton(this, wxID_OK, L"Close"), 0, wxALL, 6);

    outer->Add(view, 1, wxEXPAND | wxALL, 6);
    outer->Add(footer, 0, wxEXPAND);
    SetSizer(outer);

    about->Bind(wxEVT_BUTTON,
                [this](wxCommandEvent&) { show_about_dialog(this); });
}

void show_about_dialog(wxWindow* parent)
{
    // The licence text is not boilerplate.  This binary statically links
    // MuPDF, which is AGPL-3.0, so the whole program is conveyed under AGPL
    // and saying so where a user can read it is part of the obligation.
    // wxWidgets is LGPL *with the wxWindows exception*, which is what allows
    // static linking without imposing further terms.
    const wxString text =
        wxString(kAppName) + L" v" + kAppVersion + L"\n\n"
        L"A topic-indexed PDF browser and annotator.\n\n"
        L"Licence: GNU Affero General Public License v3.0 or later.\n"
        L"Source: https://github.com/Flinterpop/PDF_Sherpa\n\n"
        L"This program links the following third-party components:\n"
        L"  • MuPDF (Artifex Software) — AGPL-3.0-or-later\n"
        L"  • wxWidgets — LGPL-2.0-or-later with the wxWindows exception\n"
        L"  • md4c — MIT\n"
        L"  • nlohmann/json — MIT\n";

    wxMessageBox(text, wxString(L"About ") + kAppName, wxOK | wxICON_INFORMATION,
                 parent);
}

}  // namespace pdfsherpa
