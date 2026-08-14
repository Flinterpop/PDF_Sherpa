; Inno Setup script for PDF Sherpa (C++ build).
;
; Build with:  iscc PDFSherpaCpp\installer-cpp.iss
; Requires the app to be built first:
;   cmake --preset windows-static && cmake --build build --config Release
;
; Produces installer\PDFSherpa-Setup.exe.  Releases also ship
; installer\PDFSherpa-Portable.zip (just the exe plus HELP.md, zipped);
; release.ps1 does the whole cycle -- see the README's build section.
;
; NOTE the asset name.  PDFSherpa-Setup.exe is exactly what every installed
; copy polls for, so this installer inherits the name from the deprecated
; Python build rather than picking a new one: publishing it is what migrates
; existing users onto the C++ app.  The old installer.iss at the repo root now
; refuses to run without an explicit override, so the two can never both
; produce this file by accident.

#define AppName "PDF Sherpa"
#define AppVersion "2.1.0"
#define AppExe "PDFSherpa.exe"
#define BuildDir "build\app\Release"

[Setup]
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=RabidFox
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=..\installer
OutputBaseFilename=PDFSherpa-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

; The AGPL text is shown during install.  The shipped exe statically links
; MuPDF, so the binary is conveyed under AGPL-3.0 and the user is entitled to
; see the terms before installing.
LicenseFile=..\LICENSE

; Per-user, no elevation -- deliberately NOT the workspace default of
; per-machine-with-a-dialog, and this must not be "corrected" in this release.
;
; The v2.0.0 release is a MIGRATION release: it is applied by the updater
; built into the deprecated Python app, which every existing install is still
; running.  That updater invokes the installer as
;
;     "<setup>" /VERYSILENT /NORESTART /SUPPRESSMSGBOXES
;
; with no install-scope flag, because install_scope_flag did not exist when it
; shipped.  Against PrivilegesRequired=admin that silent run either fails to
; elevate, or elevates and installs to {commonpf}\PDF Sherpa while the
; existing copy stays in {userpf}\PDF Sherpa -- a second copy, with the old
; one still polling for updates.  Either way the migration breaks, for every
; existing user, silently.
;
; {autopf} under PrivilegesRequired=lowest resolves to {userpf}
; (%LOCALAPPDATA%\Programs), which is exactly where the Python installer put
; it, so the update lands on top of the existing install as intended.
;
; WHEN TO CHANGE THIS: once a release has shipped that everyone is running the
; C++ updater from, this can become PrivilegesRequired=admin plus
; PrivilegesRequiredOverridesAllowed=dialog.  That updater passes
; /ALLUSERS or /CURRENTUSER matching where the running exe actually lives --
; see install_scope_flag in PDFSherpaCpp\app\Updater.cpp, which exists for
; this and is tested for it.  Do not make that change in the same release
; that migrates people onto it.
PrivilegesRequired=lowest

[Files]
Source: "{#BuildDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
; HELP.md is loaded at runtime by the Help window, which looks for it beside
; the executable first.  Omitting it turns F1 into "Help not found".
Source: "..\HELP.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; \
    Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; \
    GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; \
    Flags: nowait postinstall skipifsilent
