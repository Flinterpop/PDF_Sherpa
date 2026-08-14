# CLAUDE.md

*Last updated: 13 Aug 2026*

Guidance for Claude Code working in this repository.

## What this repo is

A topic-indexed PDF browser and annotator for Windows. Two implementations live here, but they are **not** peers:

- **`PDFSherpaCpp/`** — C++20 on wxWidgets and MuPDF. **This is the reference implementation and the only thing that ships** (installer + portable zip). When something is ambiguous, the C++ app defines the behaviour.
- **`app.py` + `tocgen.py`** — Python/Tkinter. **Deprecated.** Kept in-tree as a historical reference and parity record, but no longer the oracle, and not built, packaged, or shipped. Do not treat a difference from the Python app as a bug in the C++ app any more — if a change is wanted, it is made in `PDFSherpaCpp/` and the Python side is left alone. Touch `app.py`/`tocgen.py` only for an explicit request about the legacy app.

**The repo is public.** Everything here is world-readable the moment it is pushed. The app's own source is releasable; the *documents* it is used on (`C:\ICD`) are export-controlled and must never appear here, in a test fixture, or in a commit message.

## Licence: AGPL, and why it is not MIT

The shipped executable statically links **MuPDF**, which is AGPL-3.0-or-later, so the combined work is conveyed under the AGPL and `LICENSE` matches. This was corrected during the C++ port; earlier releases carried an MIT notice while already bundling MuPDF through PyMuPDF, which was wrong.

Practical consequences:

- Section 13 (the network clause) is inert — this is a desktop app with no network service. The real obligation is that corresponding source stays published, which it is.
- Do not reintroduce an MIT badge or notice anywhere.
- If a permissive licence is ever wanted back, the engine has to change (PDFium is BSD-3), not the label.

## The deprecated Python build is guarded off, not merely unused

`PDFSherpa.spec` exits unless `PDFSHERPA_BUILD_DEPRECATED_PYTHON` is set, and `installer.iss` refuses without `/DAllowDeprecatedPythonBuild`.

This matters because both produce **`PDFSherpa-Setup.exe`** and **`PDFSherpa-Portable.zip`** — the exact two asset names every installed copy polls for. Regenerating one by accident and publishing it would silently "update" every install back onto the dead app. The overrides exist for local archaeology (bisecting an old bug, reproducing a past release); **never publish what they produce.**

Note this is the *opposite* rule from the version lockstep below: a deprecated file keeps getting its version bumped but must not be built.

## Version lockstep

One version number, bumped across every one of these in a single commit, always greater than the highest existing tag:

`PDFSherpaCpp/app/Version.h` · `PDFSherpaCpp/CMakeLists.txt` (`project(... VERSION ...)`) · `PDFSherpaCpp/installer-cpp.iss` · `app.py` (`APP_VERSION`) · `installer.iss`

`release.ps1 <version>` does all of it. The lockstep includes the deprecated files on purpose, so a resurrected file never reports a version that never shipped.

## Building

MuPDF is **not** vendored and **not** a vcpkg package. It lives beside the repo at `C:\source\mupdf` and is consumed by absolute path — the same convention TacPlot uses for WireCodecs — because it is 190 MB of third-party source and this repo is public. `PDFSherpaCpp/cmake/MuPdf.cmake` pins **1.28.2** and warns at configure time if the tree disagrees.

Full prerequisites and the msbuild line are in [README.md](README.md). Two things about that build are easy to get wrong:

- MuPDF's projects pin PlatformToolset `v142`; VS 18 (2026) provides **`v145`**. Retarget on the command line, never by editing the pinned tree.
- `third_party/mupdf-static-runtime.props` forces `/MT` and switches off OCR, barcodes, the non-PDF document handlers, and the CJK/SIL fallback fonts. Without it the exe is ~40 MB instead of ~22 MB and mixes two C runtimes. The feature defines are visible in MuPDF's *public* headers, so `MuPdf.cmake` passes the identical list to app code — if the two drift, the app and the library disagree about which functions exist.

Everything else comes from vcpkg in **classic** mode. Do not add a `vcpkg.json`: it switches the toolchain to manifest mode and rebuilds wxWidgets from source for no gain.

## Traps that have already cost time here

- **`wxTreeCtrl::SetItemData` takes ownership and deletes the pointer.** Never cast a row index into it. Index 0 becomes `nullptr` and deletes harmlessly, so a one-row list works and a two-row list crashes with an access violation, presenting as a startup crash nowhere near the tree code. Use a real `wxTreeItemData` subclass.
- **`last_pages` in `config.json` uses JSON key insertion order as an LRU.** `Config` is written against `nlohmann::ordered_json` for exactly this reason; plain `nlohmann::json` sorts keys and would silently evict the wrong entries with no error.
- **Never drop a config key you do not understand.** Both apps read the same `%APPDATA%\PDFGuide\config.json` (the folder keeps the app's pre-rename name — do not "fix" it), so every write is a read-modify-write.
- **Non-ASCII in a narrow string literal renders as mojibake.** `tests/test_source_literals.cpp` fails the build on it; write `L"…"` or `wxString::FromUTF8(...)`. It has already caught two real occurrences.
- **All MuPDF calls stay in `PdfDocument.cpp`.** `fz_try`/`fz_catch` are `setjmp` macros: they trip C4611 (an error under `/W4 /WX`) in whatever translation unit expands them, and `longjmp` does not run destructors. The CMake dependency is `PRIVATE` to enforce this at build time.
- **A running instance locks the exe** and the link fails with `LNK1104`, which reads as "my change did nothing". `release.ps1` checks and asks you to close it rather than killing it.

## Testing

`ctest --test-dir PDFSherpaCpp/build -C Release`. The suite is headless — `sherpa_core` deliberately excludes the GUI so tests neither link nor initialise wxWidgets.

The `tests/fixture*.pdf` files are **synthetic**, generated by `tests/make_fixture.py`, and committed so the build is hermetic. They exist so no test ever depends on a controlled document. `tests/tocgen_oracle.py` regenerates the golden values in `test_tocgen.cpp` by running the Python `tocgen.py` — pin `pymupdf==1.28.2` when doing so, since that is the release wrapping the MuPDF the C++ side links, and comparing across engine versions proves nothing.
