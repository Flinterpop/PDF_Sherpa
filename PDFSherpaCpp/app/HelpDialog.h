// HELP.md, rendered.
//
// The Python app hand-rolls a Markdown renderer onto tk.Text tags
// (_configure_help_tags / _md_inline / _md_table / _render_markdown, about 150
// lines of it).  Here md4c does the parsing and wxHtmlWindow the display, so
// the whole thing is a format conversion rather than a renderer.

#ifndef PDFSHERPA_APP_HELP_DIALOG_H
#define PDFSHERPA_APP_HELP_DIALOG_H

#include <filesystem>
#include <string>

#include <wx/wx.h>

namespace pdfsherpa {

// Where HELP.md sits: beside the exe once installed, and further up when
// running out of the build tree.  Empty if it cannot be found.
std::filesystem::path find_help_document();

// Convert Markdown to a standalone HTML document for wxHtmlWindow.
std::string markdown_to_html(const std::string& markdown);

class HelpDialog : public wxDialog {
public:
    explicit HelpDialog(wxWindow* parent);
};

// The About box, which also carries the licence notice.  This is not
// decoration: the shipped binary statically links MuPDF under AGPL-3.0, and
// stating that plainly in the running application is part of complying.
void show_about_dialog(wxWindow* parent);

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_HELP_DIALOG_H
