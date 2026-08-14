# -*- mode: python ; coding: utf-8 -*-
#
# DEPRECATED -- this builds the retired Python/Tkinter app.
#
# PDFSherpaCpp/ is the shipping implementation.  This spec is kept only so an
# old build can be reproduced for archaeology (bisecting a historic bug,
# checking what a past release actually did).
#
# It refuses to run without an explicit opt-in because of what it produces:
# dist\PDFSherpa.exe, which release.ps1 packages as PDFSherpa-Setup.exe and
# PDFSherpa-Portable.zip -- the exact two asset names every installed copy
# polls for.  Regenerating one by accident and publishing it would silently
# "update" every install back onto the dead app.
#
# NEVER publish what this produces.

import os
import sys

if not os.environ.get("PDFSHERPA_BUILD_DEPRECATED_PYTHON"):
    raise SystemExit(
        "PDFSherpa.spec builds the DEPRECATED Python app and is guarded off.\n"
        "The shipping app is PDFSherpaCpp/ -- see README.md.\n"
        "\n"
        "For local archaeology only, set PDFSHERPA_BUILD_DEPRECATED_PYTHON=1.\n"
        "Never publish the result: it claims the same asset names as the\n"
        "C++ release and would downgrade every existing install."
    )

print("WARNING: building the DEPRECATED Python app. Do not publish this.",
      file=sys.stderr)


a = Analysis(
    ['app.py'],
    pathex=[],
    binaries=[],
    datas=[('sherpaicon.ico', '.'), ('HELP.md', '.')],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        # None of these are used at runtime, they arrive transitively and cost
        # ~35 MB uncompressed.  All three are guarded optional imports in the
        # packages that pull them, so dropping them cannot break an import:
        #   numpy    <- PIL/Image.py's optional fromarray path (app.py only
        #               ever calls Image.frombytes), and it drags a 20 MB
        #               OpenBLAS DLL with it
        #   fontTools <- pymupdf.Document.subset_fonts (try/except ImportError),
        #               which this app never calls
        #   lxml     <- fontTools.misc.etree, so it leaves with fontTools
        'numpy',
        'fontTools',
        'lxml',
        # 7.9 MB decoder for a format the app never opens; PIL skips plugins
        # that fail to import.
        'PIL.AvifImagePlugin',
    ],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='PDFSherpa',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['sherpaicon.ico'],
)
