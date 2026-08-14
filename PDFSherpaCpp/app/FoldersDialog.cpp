#include "FoldersDialog.h"

#include <algorithm>

#include <wx/dirdlg.h>
#include <wx/textdlg.h>

#include "PathUtf8.h"

namespace pdfsherpa {
namespace {

namespace fs = std::filesystem;

}  // namespace

FoldersDialog::FoldersDialog(wxWindow* parent, std::vector<Root> roots)
    : wxDialog(parent, wxID_ANY, L"Folders", wxDefaultPosition, wxSize(620, 360),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      roots_(std::move(roots))
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    outer->Add(new wxStaticText(this, wxID_ANY,
                                L"Top-level folders shown in the PDF list:"),
               0, wxLEFT | wxRIGHT | wxTOP, 10);

    auto* middle = new wxBoxSizer(wxHORIZONTAL);
    list_ = new wxListBox(this, wxID_ANY);
    middle->Add(list_, 1, wxEXPAND | wxALL, 6);

    auto* buttons = new wxBoxSizer(wxVERTICAL);
    auto* add = new wxButton(this, wxID_ANY, L"Add…");
    auto* rename = new wxButton(this, wxID_ANY, L"Rename…");
    auto* remove = new wxButton(this, wxID_ANY, L"Remove");
    auto* up = new wxButton(this, wxID_ANY, L"Move up");
    auto* down = new wxButton(this, wxID_ANY, L"Move down");
    for (wxButton* button : {add, rename, remove, up, down}) {
        buttons->Add(button, 0, wxEXPAND | wxBOTTOM, 4);
    }
    middle->Add(buttons, 0, wxALL, 6);
    outer->Add(middle, 1, wxEXPAND);

    outer->Add(new wxStaticText(this, wxID_ANY,
                                L"The first folder receives dropped PDFs when "
                                L"nothing is selected."),
               0, wxLEFT | wxRIGHT, 10);

    auto* ok_cancel = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    if (ok_cancel != nullptr) {
        outer->Add(ok_cancel, 0, wxEXPAND | wxALL, 10);
    }
    SetSizer(outer);

    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_add(); });
    rename->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_rename(); });
    remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_remove(); });
    up->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { move_selected(-1); });
    down->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { move_selected(1); });
    list_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { on_rename(); });

    reload();
}

void FoldersDialog::reload()
{
    const int selection = list_->GetSelection();
    list_->Freeze();
    list_->Clear();
    for (const Root& root : roots_) {
        // Name first, path after: the name is what the tree shows, but the
        // path is what disambiguates two roots with similar names.  The dash
        // is a WIDE literal; in a narrow one its bytes reach wxString raw and
        // are decoded in the ANSI codepage as mojibake.
        list_->Append(wxString::FromUTF8(root.name) + L"   —   " +
                      wxString::FromUTF8(root.path));
    }
    list_->Thaw();

    if (!roots_.empty()) {
        const int wanted = std::clamp(selection, 0,
                                      static_cast<int>(roots_.size()) - 1);
        list_->SetSelection(wanted);
    }
}

void FoldersDialog::on_add()
{
    if (roots_.size() >= kMaxRoots) {
        wxMessageBox(wxString::Format("At most %zu folders.", kMaxRoots),
                     L"Folders", wxOK | wxICON_INFORMATION, this);
        return;
    }

    wxDirDialog dialog(this, L"Add a folder of PDFs", wxEmptyString,
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    Root root;
    root.path = dialog.GetPath().utf8_string();

    // Adding the same folder twice would show every document in it twice.
    const auto duplicate = std::find_if(
        roots_.begin(), roots_.end(), [&root](const Root& existing) {
            return page_key(path_from_utf8(existing.path)) ==
                   page_key(path_from_utf8(root.path));
        });
    if (duplicate != roots_.end()) {
        wxMessageBox(L"That folder is already in the list.", L"Folders",
                     wxOK | wxICON_INFORMATION, this);
        return;
    }

    root.name = path_to_utf8(path_from_utf8(root.path).filename());
    if (root.name.empty()) {
        root.name = root.path;  // a drive root has no filename component
    }
    roots_.push_back(std::move(root));
    reload();
    list_->SetSelection(static_cast<int>(roots_.size()) - 1);
}

void FoldersDialog::on_rename()
{
    const int selection = list_->GetSelection();
    if (selection < 0 || static_cast<std::size_t>(selection) >= roots_.size()) {
        return;
    }
    Root& root = roots_[static_cast<std::size_t>(selection)];

    wxTextEntryDialog dialog(this,
                             wxString::FromUTF8("Name for " + root.path + ":"),
                             L"Rename folder", wxString::FromUTF8(root.name));
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }
    const std::string name = dialog.GetValue().utf8_string();
    if (name.find_first_not_of(" \t") == std::string::npos) {
        return;  // a blank name would leave an unlabelled row in the tree
    }
    root.name = name;
    reload();
}

void FoldersDialog::on_remove()
{
    const int selection = list_->GetSelection();
    if (selection < 0 || static_cast<std::size_t>(selection) >= roots_.size()) {
        return;
    }
    // Removing a root only stops listing it; nothing on disk is touched, so
    // this does not warrant a confirmation.
    roots_.erase(roots_.begin() + selection);
    reload();
}

void FoldersDialog::move_selected(int delta)
{
    const int selection = list_->GetSelection();
    if (selection < 0) {
        return;
    }
    const int target = selection + delta;
    if (target < 0 || target >= static_cast<int>(roots_.size())) {
        return;
    }
    std::swap(roots_[static_cast<std::size_t>(selection)],
              roots_[static_cast<std::size_t>(target)]);
    reload();
    list_->SetSelection(target);
}

}  // namespace pdfsherpa
