// The favorites interchange file: clear, export and import.
//
// Deliberately headless and free of wxWidgets, so the parsing, serialising and
// merging can be unit-tested without starting a GUI.  The dialogs live in
// PdfListPane; everything that can be got wrong lives here.
//
// Format -- {"favorites": ["<entry>", ...]} with a two-space indent:
//
//     {
//       "favorites": [
//         "manuals/Bravo Handbook.pdf",
//         "D:\\elsewhere\\Loose Document.pdf"
//       ]
//     }
//
// Entries are written in the app's *stored* form: relative to whichever
// top-level folder contains them, or absolute for a PDF under none.  That is
// what makes the file portable, which is the point of exporting one -- another
// machine will have its folders somewhere else, and a relative entry resolves
// against whatever the local folders happen to be.  Absolute entries are kept
// verbatim, since nothing better can be inferred about them.
//
// The reader is deliberately tolerant: a plain top-level array is accepted as
// well as the object form, and non-string elements are skipped rather than
// failing the whole import.  A file a user hand-edited should import what it
// can.

#ifndef PDFSHERPA_APP_FAVORITES_FILE_H
#define PDFSHERPA_APP_FAVORITES_FILE_H

#include <cstddef>
#include <string>
#include <vector>

namespace pdfsherpa {

std::string favorites_to_json(const std::vector<std::string>& favorites);

struct FavoritesFile {
    bool ok = false;
    // True when the document carried a favorites list at all.  Distinct from
    // an empty one: "this is not a favorites file" and "this file lists no
    // favorites" deserve different messages.
    bool had_list = false;
    std::vector<std::string> favorites;
    std::string error;  // set only when ok is false
};

FavoritesFile parse_favorites_json(const std::string& text);

// Merge `incoming` into `existing`, newest-first, de-duplicated, capped.
//
// `existing` keeps its order and its precedence: importing the same entry
// twice must not promote it, or a merge would silently reshuffle the list.
// New entries are appended in the order they appear in the file.
std::vector<std::string> merge_favorites(const std::vector<std::string>& existing,
                                         const std::vector<std::string>& incoming,
                                         std::size_t cap);

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_FAVORITES_FILE_H
