# -*- mode: python ; coding: utf-8 -*-


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
