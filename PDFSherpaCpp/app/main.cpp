#include <filesystem>

#include <wx/cmdline.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/wx.h>

#include "Config.h"
#include "MainFrame.h"
#include "PathUtf8.h"
#include "Version.h"

namespace fs = std::filesystem;

namespace {

// A "pdfs" folder beside the executable, matching _default_folder().
fs::path default_folder()
{
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    return pdfsherpa::path_from_utf8(exe.GetPath().utf8_string()) / "pdfs";
}

}  // namespace

class PdfSherpaApp : public wxApp {
public:
    bool OnInit() override;
    void OnInitCmdLine(wxCmdLineParser& parser) override;
    bool OnCmdLineParsed(wxCmdLineParser& parser) override;

private:
    wxString folder_argument_;
};

void PdfSherpaApp::OnInitCmdLine(wxCmdLineParser& parser)
{
    wxApp::OnInitCmdLine(parser);
    // Without declaring this, wx's default parser REJECTS any positional
    // argument, OnInit() returns false, and the app exits with no window and
    // no message.  `PDFSherpa.exe C:\docs` is exactly the documented usage, so
    // the failure would be silent and total.
    parser.AddParam("folder", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL);
}

bool PdfSherpaApp::OnCmdLineParsed(wxCmdLineParser& parser)
{
    if (parser.GetParamCount() > 0) {
        folder_argument_ = parser.GetParam(0);
    }
    return wxApp::OnCmdLineParsed(parser);
}

bool PdfSherpaApp::OnInit()
{
    if (!wxApp::OnInit()) {
        return false;
    }

    wxInitAllImageHandlers();

    // Folder precedence, as main() in app.py resolves it: the command line,
    // then the last folder chosen (only if it still exists, since it may be on
    // a drive that is no longer plugged in), then ./pdfs beside the exe.
    pdfsherpa::Config config;
    config.load();

    fs::path folder;
    if (!folder_argument_.empty()) {
        folder = pdfsherpa::path_from_utf8(folder_argument_.utf8_string());
    } else {
        const fs::path saved = pdfsherpa::path_from_utf8(config.folder());
        std::error_code ec;
        folder = (!config.folder().empty() && fs::is_directory(saved, ec))
                     ? saved
                     : default_folder();
    }

    auto* frame = new pdfsherpa::MainFrame(folder, std::move(config));
    frame->Show(true);
    return true;
}

wxIMPLEMENT_APP(PdfSherpaApp);
