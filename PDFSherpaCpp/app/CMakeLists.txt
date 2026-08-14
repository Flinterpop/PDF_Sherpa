# sherpa_core -- everything that does not touch the GUI.
#
# The split is deliberate: document handling, sidecar files, settings and topic
# generation are all testable headlessly, and keeping them out of the wxWidgets
# target means the test suite neither links nor initialises a GUI toolkit.

add_library(sherpa_core STATIC
    Config.cpp
    Config.h
    Metadata.cpp
    Metadata.h
    PathUtf8.cpp
    PathUtf8.h
    PdfDocument.cpp
    PdfDocument.h
    TocGen.cpp
    TocGen.h
    # The updater's logic lives here, not in the GUI target, so the version
    # comparison, the install-scope decision and the handoff batch text can be
    # unit-tested without starting wxWidgets.  Nothing in it includes wx.
    Updater.cpp
    Updater.h
    Version.h
)

target_include_directories(sherpa_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(sherpa_core
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        # PRIVATE: only PdfDocument.cpp may include a MuPDF header, and making
        # the dependency private is what enforces that at build time rather
        # than by convention.  See the comment at the top of PdfDocument.h.
        mupdf
)

# -- The application ---------------------------------------------------------

find_package(wxWidgets CONFIG REQUIRED COMPONENTS core base html net)
find_package(md4c CONFIG REQUIRED)

add_executable(PDFSherpa WIN32
    main.cpp
    DropTarget.cpp
    DropTarget.h
    FoldersDialog.cpp
    FoldersDialog.h
    HelpDialog.cpp
    HelpDialog.h
    MainFrame.cpp
    MainFrame.h
    PdfListPane.cpp
    PdfListPane.h
    TopicsPane.cpp
    TopicsPane.h
    UpdateCheck.cpp
    UpdateCheck.h
    ViewerPane.cpp
    ViewerPane.h
    # Listed as a source so MSBuild embeds it.  Without the manifest the
    # process is DPI-virtualised: a blurry page, and every coordinate the
    # viewer computes disagreeing with what the OS reports.
    PDFSherpa.manifest
)

target_link_libraries(PDFSherpa PRIVATE
    sherpa_core
    wx::core
    wx::base
    wx::html
    wx::net
    md4c::md4c-html)

set_target_properties(PDFSherpa PROPERTIES OUTPUT_NAME "PDFSherpa")
