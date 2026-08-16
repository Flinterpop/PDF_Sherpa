#include "ProgressJob.h"

#include <atomic>
#include <cassert>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

#include <wx/progdlg.h>

namespace pdfsherpa {
namespace {

// Shared between the worker and the UI thread for the life of one job.
//
// Only `cancel` crosses the boundary; the dialog and the completion handler
// are touched on the UI thread alone, and the worklist and the two callbacks
// are captured by the worker alone.  Keeping the split this narrow is what
// makes the whole thing reviewable.
struct JobState {
    wxProgressDialog* dialog = nullptr;
    std::function<void(ProgressJobResult)> on_done;
    std::atomic<bool> cancel{false};
};

// Tear the dialog down and hand the result to the caller.  UI thread only.
void finish_job(const std::shared_ptr<JobState>& state, ProgressJobResult result)
{
    assert(state != nullptr);
    if (state->dialog != nullptr) {
        state->dialog->Destroy();
        state->dialog = nullptr;
    }
    if (state->on_done) {
        state->on_done(std::move(result));
    }
}

}  // namespace

void run_progress_job(wxWindow* parent, const wxString& title,
                      std::size_t count,
                      std::function<std::string(std::size_t)> label,
                      std::function<std::string(std::size_t)> step,
                      std::function<void(ProgressJobResult)> on_done)
{
    assert(parent != nullptr);
    assert(on_done != nullptr);
    assert(count == 0 || (label != nullptr && step != nullptr));
    // wxProgressDialog counts in int; every caller's worklist is a directory
    // scan's output, which PdfListPane already bounds well below this.
    assert(count <= static_cast<std::size_t>(std::numeric_limits<int>::max()));

    if (count == 0) {
        // No worklist is not an error, and callers should not have to write
        // the empty case themselves.  Still deferred, so the caller sees the
        // same "later, on the UI thread" ordering it would get for real work.
        wxTheApp->CallAfter([done = std::move(on_done)]() {
            done(ProgressJobResult{});
        });
        return;
    }

    auto state = std::make_shared<JobState>();
    state->on_done = std::move(on_done);
    state->dialog = new wxProgressDialog(
        title, wxString::FromUTF8(label(0)), static_cast<int>(count), parent,
        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);

    std::thread worker([state, count, label = std::move(label),
                        step = std::move(step)]() {
        ProgressJobResult result;
        // An uncaught exception on a worker is a silent std::terminate with
        // exit code 0xC0000409 and no dialog, so the whole body is guarded --
        // the same reason ViewerPane::start_search guards its own.
        try {
            for (std::size_t index = 0; index < count; ++index) {
                if (state->cancel.load()) {
                    result.cancelled = true;
                    break;
                }

                // Announce the item before working on it, so the dialog names
                // what it is busy with rather than what it just finished.
                wxTheApp->CallAfter([state, index, text = label(index)]() {
                    if (state->dialog == nullptr) {
                        return;
                    }
                    if (!state->dialog->Update(static_cast<int>(index),
                                               wxString::FromUTF8(text))) {
                        state->cancel.store(true);
                    }
                });

                const std::string error = step(index);
                ++result.completed;
                if (error.empty()) {
                    ++result.succeeded;
                } else {
                    result.errors.push_back(error);
                }
            }
        } catch (...) {
            // Dying quietly beats terminating the process; the counts already
            // accumulated still describe what did happen.
        }

        wxTheApp->CallAfter([state, result = std::move(result)]() mutable {
            finish_job(state, std::move(result));
        });
    });
    worker.detach();
}

}  // namespace pdfsherpa
