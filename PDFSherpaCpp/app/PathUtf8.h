// Lossless conversion between std::filesystem::path and UTF-8 std::string.
//
// Everything in this app treats std::string as UTF-8: it is what config.json
// holds, what the .toc and bookmark sidecars hold, and what wxString::FromUTF8
// expects.  std::filesystem does NOT.  path::string() converts to the current
// ANSI code page and *throws* if a character has no mapping there, and building
// a path from a narrow string interprets it as ANSI rather than UTF-8.
//
// That is not theoretical.  The same mistake in MDBoss took the whole app down
// from a worker thread when one document folder held a filename with a
// non-ANSI character ("No mapping for the Unicode character exists in the
// target multi-byte code page").  PDF Sherpa points at arbitrary user folders
// of PDFs, so it is at least as exposed.
//
// Use these two at every boundary; never call path::string() on a path that
// came from, or is going to, the rest of the app.

#ifndef PDFSHERPA_APP_PATH_UTF8_H
#define PDFSHERPA_APP_PATH_UTF8_H

#include <filesystem>
#include <string>
#include <string_view>

namespace pdfsherpa {

std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(std::string_view utf8);

}  // namespace pdfsherpa

#endif  // PDFSHERPA_APP_PATH_UTF8_H
