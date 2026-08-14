// "Folders…": add, rename, reorder and remove the top-level folders the PDF
// list shows.  Capped at kMaxRoots.
//
// Modelled on MDBossCpp/app/FoldersDialog, which solves the same problem for
// Markdown folders. The dialog edits a copy and hands it back on OK, so a
// cancelled edit cannot half-apply.

#ifndef PDFSHERPA_APP_FOLDERS_DIALOG_H
#define PDFSHERPA_APP_FOLDERS_DIALOG_H

#include <vector>

#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/wx.h>

#include "Config.h"

namespace pdfsherpa {

class FoldersDialog : public wxDialog {
public:
    FoldersDialog(wxWindow* parent, std::vector<Root> roots);

    const std::vector<Root>& roots() const { return roots_; }

private:
    void reload();
    void on_add();
    void on_remove();
    void on_rename();
    void move_selected(int delta);

    wxListBox* list_ = nullptr;
    std::vector<Root> roots_;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_FOLDERS_DIALOG_H
