#include "DropTarget.h"

#include "PathUtf8.h"

namespace pdfsherpa {

bool PdfDropTarget::OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames)
{
    if (!handler_) {
        return false;
    }
    std::vector<std::filesystem::path> paths;
    paths.reserve(filenames.GetCount());
    for (const wxString& name : filenames) {
        paths.push_back(path_from_utf8(name.utf8_string()));
    }
    handler_(paths);
    return true;
}

}  // namespace pdfsherpa
