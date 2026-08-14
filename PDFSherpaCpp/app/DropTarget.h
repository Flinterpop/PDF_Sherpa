// Accept PDFs dropped from Explorer anywhere on the window.
//
// The Python app does this the hard way: it registers the toplevel with
// DragAcceptFiles and subclasses its window procedure to catch WM_DROPFILES,
// with the added hazard that the hook runs reentrantly inside Tcl's own event
// dispatch and must not call into Tk -- so it queues paths and a 200 ms poller
// drains them.  wxFileDropTarget replaces all of it, and the callback already
// runs on the UI thread.

#ifndef PDFSHERPA_APP_DROP_TARGET_H
#define PDFSHERPA_APP_DROP_TARGET_H

#include <filesystem>
#include <functional>
#include <vector>

#include <wx/dnd.h>

namespace pdfsherpa {

class PdfDropTarget : public wxFileDropTarget {
public:
    using Handler = std::function<void(const std::vector<std::filesystem::path>&)>;

    explicit PdfDropTarget(Handler handler) : handler_(std::move(handler)) {}

    bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString& filenames) override;

private:
    Handler handler_;
};

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_DROP_TARGET_H
