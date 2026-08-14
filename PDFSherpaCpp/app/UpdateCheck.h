// The launch-time update check and the download/handoff that follows it.
//
// Split from Updater.h deliberately: everything there is pure logic with no
// wxWidgets and no network, so it can be unit-tested.  This file is the part
// that talks to GitHub and to the user, and it is the part that cannot be
// meaningfully tested without a real published release.
//
// Network note: the only host contacted is api.github.com, for this app's own
// public releases.  No document, path, or setting is ever transmitted.

#ifndef PDFSHERPA_APP_UPDATE_CHECK_H
#define PDFSHERPA_APP_UPDATE_CHECK_H

#include <filesystem>
#include <string>

#include <wx/progdlg.h>
#include <wx/webrequest.h>
#include <wx/wx.h>

#include "Updater.h"

namespace pdfsherpa {

class Config;

class UpdateCheck : public wxEvtHandler {
public:
    UpdateCheck(wxWindow* parent, Config* config);

    // Ask GitHub whether there is a newer release.
    //
    // `manual` distinguishes the two callers, exactly as app.py does: the
    // automatic check at launch stays silent on failure and on "you are up to
    // date", because neither is worth interrupting someone who just wanted to
    // read a PDF.  A check the user asked for reports both.
    void start(bool manual);

private:
    void on_release_state(wxWebRequestEvent& event);
    void on_download_state(wxWebRequestEvent& event);
    void handle_release_json(const std::string& body);
    void offer_update(const ReleaseInfo& info);
    void begin_download(const ReleaseInfo& info, const std::string& url,
                        bool portable);
    void install_and_exit(const std::filesystem::path& downloaded);

    wxWindow* parent_ = nullptr;
    Config* config_ = nullptr;
    bool manual_ = false;
    bool portable_ = false;
    std::string pending_version_;
    wxWebRequest request_;
    wxProgressDialog* progress_ = nullptr;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_UPDATE_CHECK_H
