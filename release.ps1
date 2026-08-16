<#
Release PDF Sherpa: bump the version everywhere, build the C++ app, the
installer and the portable zip, commit and push the bump, publish a GitHub
release with both assets, then reinstall locally.

Usage:
  .\release.ps1 1.3.13
  .\release.ps1 1.3.13 -NotesFile notes.md      # release notes from a file
  .\release.ps1 1.3.13 -Notes "- fixed X"       # inline release notes
  .\release.ps1 1.3.13 -SkipInstall             # don't reinstall/relaunch here

Without -Notes/-NotesFile the GitHub notes are auto-generated from commits.

Builds PDFSherpaCpp (C++20 / wxWidgets / MuPDF).  The Python app is deprecated
and is NOT built here; its build path is guarded off deliberately, because it
would produce the same two asset names and downgrade every install.

Requires: cmake, Visual Studio 18 (2026) toolset v145, a built MuPDF at
C:\source\mupdf (see README), Inno Setup 6, gh (authenticated), git.
Windows PowerShell 5.1 compatible.
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [string]$Notes = "",
    [string]$NotesFile = "",
    [switch]$SkipInstall
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
# BOTH lines, and the second is not redundant.  Set-Location moves PowerShell's
# own location; it does NOT touch .NET's working directory, which is whatever
# the shell PROCESS was started in.  Bump() below pairs Test-Path (PowerShell,
# so it finds the file) with [IO.File]::ReadAllText (.NET, so it resolves the
# same relative path somewhere else entirely) -- the failure is a
# DirectoryNotFoundException naming a path that is half this repo and half the
# directory the shell happened to start in.  Running this script from a shell
# opened anywhere but the repo root is enough to hit it.
[Environment]::CurrentDirectory = $PSScriptRoot

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function CheckExit($what) {
    if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit $LASTEXITCODE)" }
}

# --- Preflight ---------------------------------------------------------------
$iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) {
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { $iscc = $cmd.Source } else { Fail "ISCC.exe not found (Inno Setup 6)" }
}
foreach ($tool in "cmake", "git", "gh") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool not on PATH" }
}
if ($NotesFile -and -not (Test-Path $NotesFile)) { Fail "notes file not found: $NotesFile" }

# MuPDF is a pinned sibling tree, not a vcpkg package; without it the configure
# step fails with a long message and it is better to say so up front.
$mupdfLib = "C:\source\mupdf\platform\win32\x64\Release\libmupdf.lib"
if (-not (Test-Path $mupdfLib)) {
    Fail "MuPDF is not built: $mupdfLib is missing.`nSee the build prerequisites in README.md."
}

$dirty = git status --porcelain
if ($dirty) { Fail "working tree not clean -- commit or stash first:`n$dirty" }

# --- Bump versions -----------------------------------------------------------
# Version lockstep: every place the number appears moves together, in one
# commit.  That deliberately INCLUDES the deprecated Python app, so a file
# resurrected for archaeology never reports a version that never shipped.
Write-Host "==> Bumping version to $Version" -ForegroundColor Cyan

function Bump($path, $pattern, $replacement) {
    if (-not (Test-Path $path)) { Fail "not found: $path" }
    $text = [IO.File]::ReadAllText($path)
    if ($text -notmatch $pattern) { Fail "pattern not found in $path : $pattern" }
    [IO.File]::WriteAllText($path, ($text -replace $pattern, $replacement),
                            (New-Object Text.UTF8Encoding($false)))
}

Bump "PDFSherpaCpp\app\Version.h" `
     '#define PDFSHERPA_VERSION_STRING "[^"]+"' "#define PDFSHERPA_VERSION_STRING `"$Version`""
Bump "PDFSherpaCpp\CMakeLists.txt" `
     'project\(PDFSherpaCpp VERSION [0-9]+\.[0-9]+\.[0-9]+' "project(PDFSherpaCpp VERSION $Version"

# The .rc carries the numeric tuples Explorer shows, which cannot reference a
# string macro and so must be rewritten separately.  test_version.cpp asserts
# they stay in step with Version.h.
$tupleVersion = ($Version -replace '\.', ',') + ",0"
Bump "PDFSherpaCpp\app\PDFSherpa.rc" `
     'FILEVERSION     [0-9]+,[0-9]+,[0-9]+,[0-9]+' "FILEVERSION     $tupleVersion"
Bump "PDFSherpaCpp\app\PDFSherpa.rc" `
     'PRODUCTVERSION  [0-9]+,[0-9]+,[0-9]+,[0-9]+' "PRODUCTVERSION  $tupleVersion"
Bump "PDFSherpaCpp\installer-cpp.iss" `
     '#define AppVersion "[^"]+"' "#define AppVersion `"$Version`""
# Deprecated, still bumped -- see the note above.
Bump "app.py"        'APP_VERSION = "[^"]+"'      "APP_VERSION = `"$Version`""
Bump "installer.iss" '#define AppVersion "[^"]+"' "#define AppVersion `"$Version`""

# --- Build -------------------------------------------------------------------
# A running instance holds a lock on the exe and the link fails with LNK1104,
# which reads as "my change did nothing" rather than "close the app".
$running = Get-Process PDFSherpa -ErrorAction SilentlyContinue
if ($running) {
    Fail "PDF Sherpa is running (pid $($running.Id -join ', ')). Close it and re-run: it holds a lock on the exe and the link would fail with LNK1104."
}

Write-Host "==> Configuring (CMake)" -ForegroundColor Cyan
cmake --preset windows-static -S PDFSherpaCpp
CheckExit "cmake configure"

Write-Host "==> Building (Release)" -ForegroundColor Cyan
cmake --build PDFSherpaCpp\build --config Release
CheckExit "cmake build"

Write-Host "==> Running tests" -ForegroundColor Cyan
ctest --test-dir PDFSherpaCpp\build -C Release --output-on-failure
CheckExit "ctest"

$exe = "PDFSherpaCpp\build\app\Release\PDFSherpa.exe"
if (-not (Test-Path $exe)) { Fail "expected build output missing: $exe" }

# The shipped exe must need no VC++ redistributable.  Checked rather than
# assumed, because a stray /MD dependency only shows up on a machine that has
# never had Visual Studio installed -- i.e. a user's.
$dumpbin = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC" `
    -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'HostX64\\x64' } | Select-Object -First 1
if ($dumpbin) {
    $deps = & $dumpbin.FullName /nologo /dependents $exe
    $bad = $deps | Select-String -Pattern 'VCRUNTIME|MSVCP\d|api-ms-win-crt'
    if ($bad) { Fail "exe depends on the VC++ runtime:`n$bad" }
    Write-Host "    static CRT verified" -ForegroundColor DarkGray
} else {
    Write-Host "    dumpbin not found -- skipping static-CRT check" -ForegroundColor Yellow
}

Write-Host "==> Building installer (ISCC)" -ForegroundColor Cyan
& $iscc "PDFSherpaCpp\installer-cpp.iss"
CheckExit "ISCC"

Write-Host "==> Building portable zip" -ForegroundColor Cyan
# HELP.md rides along: the Help window looks for it beside the exe, and a
# portable copy without it answers F1 with "Help not found".
Compress-Archive -Force -Path $exe, "HELP.md" `
                 -DestinationPath "installer\PDFSherpa-Portable.zip"

# Both asset names are load-bearing: the in-app updater matches them exactly.
foreach ($asset in "installer\PDFSherpa-Setup.exe", "installer\PDFSherpa-Portable.zip") {
    if (-not (Test-Path $asset)) { Fail "expected artifact missing: $asset" }
}

# --- Commit + push -----------------------------------------------------------
git add PDFSherpaCpp\app\Version.h PDFSherpaCpp\app\PDFSherpa.rc `
        PDFSherpaCpp\CMakeLists.txt `
        PDFSherpaCpp\installer-cpp.iss app.py installer.iss
$staged = git diff --cached --name-only
if ($staged) {
    git commit -m "Bump version to $Version"
    CheckExit "git commit"
} else {
    Write-Host "==> Versions already at $Version, nothing to commit" -ForegroundColor Yellow
}

Write-Host "==> Syncing with origin (README is sometimes edited on the web)" -ForegroundColor Cyan
git pull --rebase origin main
CheckExit "git pull --rebase"
git push origin main
CheckExit "git push"

# --- Publish release ---------------------------------------------------------
Write-Host "==> Publishing GitHub release v$Version" -ForegroundColor Cyan
$ghArgs = @("release", "create", "v$Version",
            "installer\PDFSherpa-Setup.exe", "installer\PDFSherpa-Portable.zip",
            "--title", "v$Version")
if ($NotesFile)  { $ghArgs += @("--notes-file", $NotesFile) }
elseif ($Notes)  { $ghArgs += @("--notes", $Notes) }
else             { $ghArgs += "--generate-notes" }
& gh @ghArgs
CheckExit "gh release create"

# --- Local reinstall ---------------------------------------------------------
if (-not $SkipInstall) {
    Write-Host "==> Reinstalling locally and relaunching" -ForegroundColor Cyan
    # The installer now defaults to per-machine and asks, so a silent re-run
    # needs an explicit scope or it may land somewhere other than the existing
    # install.  /CURRENTUSER matches the historical per-user location.
    Start-Process (Join-Path $PSScriptRoot "installer\PDFSherpa-Setup.exe") `
        -ArgumentList "/VERYSILENT", "/NORESTART", "/SUPPRESSMSGBOXES", "/CURRENTUSER" -Wait
    $installed = "$env:LOCALAPPDATA\Programs\PDF Sherpa\PDFSherpa.exe"
    if (Test-Path $installed) { Start-Process $installed }
}

Write-Host "==> Done: https://github.com/Flinterpop/PDF_Sherpa/releases/tag/v$Version" -ForegroundColor Green
