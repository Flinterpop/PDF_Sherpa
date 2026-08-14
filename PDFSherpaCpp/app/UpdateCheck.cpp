#include "UpdateCheck.h"

#include <cassert>

#include <wx/filename.h>
#include <wx/progdlg.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <nlohmann/json.hpp>

#include "Config.h"
#include "PathUtf8.h"
#include "Version.h"

namespace pdfsherpa {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path executable_path()
{
    return path_from_utf8(
        wxStandardPaths::Get().GetExecutablePath().utf8_string());
}

}  // namespace

UpdateCheck::UpdateCheck(wxWindow* parent, Config* config)
    : parent_(parent), config_(config)
{
    assert(parent != nullptr);
    assert(config != nullptr);
}

void UpdateCheck::start(bool manual)
{
    manual_ = manual;

    // "check_updates": false in config.json opts out of the automatic check
    // entirely, but never blocks one the user asked for.
    if (!manual && !config_->check_updates()) {
        return;
    }

    request_ = wxWebSession::GetDefault().CreateRequest(this, kReleaseApiUrl);
    if (!request_.IsOk()) {
        if (manual_) {
            wxMessageBox(L"Could not start the update check.", L"Check for updates",
                         wxOK | wxICON_ERROR, parent_);
        }
        return;
    }
    // GitHub rejects requests with no User-Agent.
    request_.SetHeader("User-Agent", "PDFSherpa");
    request_.SetHeader("Accept", "application/vnd.github+json");

    Bind(wxEVT_WEBREQUEST_STATE, &UpdateCheck::on_release_state, this);
    request_.Start();
}

void UpdateCheck::on_release_state(wxWebRequestEvent& event)
{
    switch (event.GetState()) {
        case wxWebRequest::State_Completed:
            handle_release_json(event.GetResponse().AsString().utf8_string());
            break;

        case wxWebRequest::State_Failed:
        case wxWebRequest::State_Unauthorized:
            // Silent on the automatic check: someone opening a PDF on a
            // machine with no network does not need a dialog about it.
            if (manual_) {
                wxMessageBox(wxString::FromUTF8("Could not check for updates:\n\n" +
                                                event.GetErrorDescription().utf8_string()),
                             L"Check for updates", wxOK | wxICON_WARNING, parent_);
            }
            break;

        default:
            break;  // Active / Idle: nothing to do yet
    }
}

void UpdateCheck::handle_release_json(const std::string& body)
{
    const json data = json::parse(body, nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
        if (manual_) {
            wxMessageBox(L"The release information could not be read.",
                         L"Check for updates", wxOK | wxICON_WARNING, parent_);
        }
        return;
    }

    ReleaseInfo info;
    const std::string tag =
        data.contains("tag_name") && data["tag_name"].is_string()
            ? data["tag_name"].get<std::string>()
            : "";
    const auto parsed = parse_version(tag);
    if (!parsed.has_value()) {
        if (manual_) {
            wxMessageBox(wxString::FromUTF8("Unrecognized release tag: " + tag),
                         L"Check for updates", wxOK | wxICON_WARNING, parent_);
        }
        return;
    }
    info.version = *parsed;
    info.version_str = tag;
    while (!info.version_str.empty() &&
           (info.version_str.front() == 'v' || info.version_str.front() == 'V')) {
        info.version_str.erase(0, 1);
    }
    info.html_url = data.contains("html_url") && data["html_url"].is_string()
                        ? data["html_url"].get<std::string>()
                        : kReleasesPageUrl;

    std::string fallback_exe;
    if (data.contains("assets") && data["assets"].is_array()) {
        for (const json& asset : data["assets"]) {
            if (!asset.is_object() || !asset.contains("name") ||
                !asset.contains("browser_download_url")) {
                continue;
            }
            const std::string name = asset["name"].get<std::string>();
            const std::string url = asset["browser_download_url"].get<std::string>();
            if (name == kSetupAssetName) {
                info.setup_url = url;
            } else if (name == kPortableAssetName) {
                info.portable_url = url;
            } else if (fallback_exe.empty() && name.size() > 4 &&
                       name.compare(name.size() - 4, 4, ".exe") == 0) {
                fallback_exe = url;
            }
        }
    }
    if (info.setup_url.empty()) {
        info.setup_url = fallback_exe;
    }

    const auto current = parse_version(kAppVersion);
    assert(current.has_value() && "the app's own version must parse");
    if (!is_newer(info.version, *current)) {
        if (manual_) {
            wxMessageBox(wxString::FromUTF8(std::string(kAppName) + " " +
                                            kAppVersion + " is up to date."),
                         L"Check for updates", wxOK | wxICON_INFORMATION, parent_);
        }
        return;
    }

    // A version the user explicitly skipped stays skipped, but only for the
    // automatic check.
    if (!manual_ && config_->skip_version() == info.version_str) {
        return;
    }

    offer_update(info);
}

void UpdateCheck::offer_update(const ReleaseInfo& info)
{
    portable_ = running_portable(executable_path());
    const std::string url = portable_ ? info.portable_url : info.setup_url;

    if (url.empty()) {
        // Nothing downloadable for this flavour; point at the release page
        // rather than pretending the update can be applied.
        if (wxMessageBox(
                wxString::FromUTF8(
                    "Version " + info.version_str + " is available, but no "
                    "matching download was published.\n\nOpen the release "
                    "page?"),
                L"Update available", wxYES_NO | wxICON_INFORMATION,
                parent_) == wxYES) {
            wxLaunchDefaultBrowser(wxString::FromUTF8(info.html_url));
        }
        return;
    }

    wxMessageDialog dialog(
        parent_,
        wxString::FromUTF8(
            std::string(kAppName) + " " + info.version_str +
            " is available (you have " + kAppVersion + ").\n\n"
            "Yes    -  download and install it now\n"
            "No     -  skip this version\n"
            "Cancel -  remind me next time"),
        L"Update available", wxYES_NO | wxCANCEL | wxICON_INFORMATION);

    const int answer = dialog.ShowModal();
    if (answer == wxID_NO) {
        config_->save_skip_version(info.version_str);
        return;
    }
    if (answer != wxID_YES) {
        return;  // Cancel: ask again next launch
    }

    begin_download(info, url, portable_);
}

void UpdateCheck::begin_download(const ReleaseInfo& info, const std::string& url,
                                 bool portable)
{
    pending_version_ = info.version_str;
    portable_ = portable;

    request_ = wxWebSession::GetDefault().CreateRequest(this, url);
    if (!request_.IsOk()) {
        wxMessageBox(L"Could not start the download.", L"Update",
                     wxOK | wxICON_ERROR, parent_);
        return;
    }
    request_.SetHeader("User-Agent", "PDFSherpa");
    // Storage_File streams to a temp file rather than holding the whole
    // installer in memory.
    request_.SetStorage(wxWebRequest::Storage_File);

    Unbind(wxEVT_WEBREQUEST_STATE, &UpdateCheck::on_release_state, this);
    Bind(wxEVT_WEBREQUEST_STATE, &UpdateCheck::on_download_state, this);

    // The ellipsis lives in a WIDE literal.  In a narrow one its UTF-8 bytes
    // reach wxString as raw bytes and get decoded in the ANSI codepage, so it
    // renders as mojibake -- the exact bug test_source_literals guards, and
    // which it caught on this very line.
    progress_ = new wxProgressDialog(
        L"Downloading update",
        wxString::Format(L"Downloading %s %s…", wxString(kAppName),
                         wxString::FromUTF8(info.version_str)),
        100, parent_, wxPD_APP_MODAL | wxPD_AUTO_HIDE);

    request_.Start();
}

void UpdateCheck::on_download_state(wxWebRequestEvent& event)
{
    switch (event.GetState()) {
        case wxWebRequest::State_Completed: {
            if (progress_ != nullptr) {
                progress_->Destroy();
                progress_ = nullptr;
            }
            install_and_exit(
                path_from_utf8(event.GetDataFile().utf8_string()));
            break;
        }
        case wxWebRequest::State_Failed:
        case wxWebRequest::State_Unauthorized:
            if (progress_ != nullptr) {
                progress_->Destroy();
                progress_ = nullptr;
            }
            wxMessageBox(wxString::FromUTF8("The download failed:\n\n" +
                                            event.GetErrorDescription().utf8_string()),
                         L"Update", wxOK | wxICON_ERROR, parent_);
            break;

        default:
            break;
    }
}

void UpdateCheck::install_and_exit(const fs::path& downloaded)
{
    const fs::path exe = executable_path();
    const fs::path temp = path_from_utf8(
        wxStandardPaths::Get().GetTempDir().utf8_string());

    // wxWebRequest names its temp file arbitrarily; give it the extension the
    // handoff expects so the installer actually runs as one.
    const fs::path staged =
        temp / (portable_ ? "PDFSherpa-Portable.zip" : "PDFSherpa-Setup.exe");
    std::error_code ec;
    fs::remove(staged, ec);
    fs::rename(downloaded, staged, ec);
    if (ec) {
        fs::copy_file(downloaded, staged,
                      fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
        wxMessageBox(L"Could not stage the downloaded update.", L"Update",
                     wxOK | wxICON_ERROR, parent_);
        return;
    }

    const unsigned long pid = static_cast<unsigned long>(::wxGetProcessId());
    const std::string batch =
        portable_ ? portable_batch(staged, temp / "PDFSherpa-update", exe, pid)
                  : installer_batch(staged, exe, pid);

    const fs::path batch_path = temp / "PDFSherpa-update.bat";
    if (!spawn_handoff_batch(batch, batch_path)) {
        wxMessageBox(L"Could not start the updater.", L"Update",
                     wxOK | wxICON_ERROR, parent_);
        return;
    }

    // The batch waits for this process to exit before touching the exe, so
    // closing is the handoff.
    if (parent_ != nullptr) {
        parent_->Close(true);
    }
}

}  // namespace pdfsherpa
