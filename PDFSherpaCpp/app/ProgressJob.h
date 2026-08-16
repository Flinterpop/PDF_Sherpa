// Run a bounded list of slow, file-touching steps on a worker thread while a
// cancellable progress dialog keeps the window responsive.
//
// This exists because both callers used to run on the UI thread behind nothing
// but a wxBusyCursor: Refresh's "build the missing topic lists", and the drop
// handler's "copy these in and index them".  write_toc() reads text from EVERY
// page of a PDF, so a handful of large documents froze the window for minutes
// with no progress shown and no way to stop it.
//
// THREADING CONTRACT, and it is not negotiable:
//
//   - `label(index)` and `step(index)` run on the WORKER thread.  Neither may
//     touch a wxWidgets object or a member of a GUI class.  Whatever they want
//     to say comes back as a std::string.  The one exception is `label(0)`,
//     called once on the UI thread to size the dialog: a dialog first shown
//     with a short message does not widen for a longer one later, so it has to
//     be built around a representative label rather than a placeholder.
//   - `on_done(result)` runs on the UI thread, after the dialog is gone.
//
// The dialog is wxPD_APP_MODAL, which is load-bearing rather than cosmetic: it
// is what stops a second job (or a folder change, or a close) being started on
// top of a running one, so `parent` is guaranteed to outlive the job.

#ifndef PDFSHERPA_APP_PROGRESS_JOB_H
#define PDFSHERPA_APP_PROGRESS_JOB_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <wx/wx.h>

namespace pdfsherpa {

struct ProgressJobResult {
    std::size_t completed = 0;        // steps that ran
    std::size_t succeeded = 0;        // steps that reported no error
    std::vector<std::string> errors;  // one entry per failing step
    bool cancelled = false;           // the user pressed Cancel
};

// Starts the job and returns immediately; `on_done` runs later on the UI
// thread.  A `count` of zero completes straight away, so callers need no
// special case for an empty worklist.
//
// `step` returns an empty string on success, or a message describing what went
// wrong -- which is collected into `errors` rather than shown, so one bad file
// does not interrupt the run with a dialog the user has to dismiss N times.
void run_progress_job(wxWindow* parent, const wxString& title,
                      std::size_t count,
                      std::function<std::string(std::size_t)> label,
                      std::function<std::string(std::size_t)> step,
                      std::function<void(ProgressJobResult)> on_done);

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_PROGRESS_JOB_H
