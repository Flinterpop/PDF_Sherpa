# Locate the pinned MuPDF build and expose it as the imported target `mupdf`.
#
# MuPDF is NOT vendored into this repo.  The source tree is 190 MB and the repo
# is public, so it lives beside it as a sibling and is consumed by absolute
# path -- the same convention TacPlot and RadioCoverage use for WireCodecs and
# GisShell.  Only the build-config props file is in-tree, under third_party/.
#
# To produce the libraries this expects, from MUPDF_DIR:
#
#   msbuild platform/win32/mupdf.sln -t:libmupdf ^
#     -p:Configuration=Release -p:Platform=x64 -p:PlatformToolset=v145 ^
#     -p:ForceImportBeforeCppTargets=<repo>/PDFSherpaCpp/third_party/mupdf-static-runtime.props
#
# The toolset is v145 for Visual Studio 18 (2026).  MuPDF's own projects pin
# v142 and will not build without that override.

set(MUPDF_DIR "C:/source/mupdf" CACHE PATH
    "Root of the pinned MuPDF source tree (built via platform/win32/mupdf.sln)")

# Pinned so an accidental upgrade is loud rather than a silent behaviour change.
# The renderer and the text extractor both change between MuPDF releases, which
# would show up as unexplained pixel-diff and TocGen golden-corpus failures.
set(MUPDF_EXPECTED_VERSION "1.28.2")

if(NOT EXISTS "${MUPDF_DIR}/include/mupdf/fitz.h")
    message(FATAL_ERROR
        "MuPDF not found at MUPDF_DIR=${MUPDF_DIR}.\n"
        "Expected ${MUPDF_DIR}/include/mupdf/fitz.h.\n"
        "Download mupdf-${MUPDF_EXPECTED_VERSION}-source.tar.gz from\n"
        "  https://mupdf.com/downloads/archive/\n"
        "extract it there, and build it (see the comment at the top of "
        "cmake/MuPdf.cmake).")
endif()

# Verify the pin against MuPDF's own version header rather than trusting the
# folder name.
file(STRINGS "${MUPDF_DIR}/include/mupdf/fitz/version.h" _mupdf_version_line
     REGEX "#define[ \t]+FZ_VERSION[ \t]+\"")
string(REGEX MATCH "\"([^\"]+)\"" _mupdf_version_match "${_mupdf_version_line}")
set(MUPDF_FOUND_VERSION "${CMAKE_MATCH_1}")

if(NOT MUPDF_FOUND_VERSION STREQUAL MUPDF_EXPECTED_VERSION)
    message(WARNING
        "MuPDF at ${MUPDF_DIR} is version ${MUPDF_FOUND_VERSION}, but this "
        "build pins ${MUPDF_EXPECTED_VERSION}.\n"
        "Rendering and text extraction can differ between releases: expect the "
        "pixel-diff and TocGen golden-corpus tests to fail until the pin and "
        "the tree agree.  Update MUPDF_EXPECTED_VERSION deliberately, in the "
        "same commit that regenerates the baselines.")
endif()

set(_mupdf_lib_dir "${MUPDF_DIR}/platform/win32/x64/Release")
foreach(_lib libmupdf libthirdparty libresources)
    if(NOT EXISTS "${_mupdf_lib_dir}/${_lib}.lib")
        message(FATAL_ERROR
            "MuPDF is present but not built: ${_mupdf_lib_dir}/${_lib}.lib is "
            "missing.\nBuild it with the msbuild command in "
            "cmake/MuPdf.cmake.")
    endif()
endforeach()

# These defines are visible in MuPDF's PUBLIC headers -- fitz/config.h gates
# declarations on them -- so every translation unit that includes a MuPDF
# header must see the identical list that built the library.  This is the one
# copy; third_party/mupdf-static-runtime.props carries the same list for the
# MuPDF build itself, and the two must be changed together.
set(MUPDF_FEATURE_DEFINES
    FZ_ENABLE_BARCODE=0
    FZ_ENABLE_OCR_OUTPUT=0
    FZ_ENABLE_DOCX_OUTPUT=0
    FZ_ENABLE_ODT_OUTPUT=0
    FZ_ENABLE_JS=0
    FZ_ENABLE_XPS=0
    FZ_ENABLE_SVG=0
    FZ_ENABLE_CBZ=0
    FZ_ENABLE_IMG=0
    FZ_ENABLE_HTML=0
    FZ_ENABLE_FB2=0
    FZ_ENABLE_MOBI=0
    FZ_ENABLE_EPUB=0
    FZ_ENABLE_OFFICE=0
    FZ_ENABLE_TXT=0
    TOFU_CJK
    TOFU_SIL)

add_library(mupdf INTERFACE)

# SYSTEM, because MuPDF's headers are not /W4-clean (C4100 on unused ctx
# parameters, C4611 around the setjmp machinery) and this project builds
# /W4 /WX.  Without this every consumer drowns in warnings it cannot fix.
target_include_directories(mupdf SYSTEM INTERFACE "${MUPDF_DIR}/include")
target_compile_definitions(mupdf INTERFACE ${MUPDF_FEATURE_DEFINES})
target_link_libraries(mupdf INTERFACE
    "${_mupdf_lib_dir}/libmupdf.lib"
    "${_mupdf_lib_dir}/libthirdparty.lib"
    "${_mupdf_lib_dir}/libresources.lib"
    advapi32
    user32
    gdi32)

message(STATUS "MuPDF ${MUPDF_FOUND_VERSION} at ${MUPDF_DIR}")
