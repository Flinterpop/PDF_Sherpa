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
#define AppVersion "1.3.12"
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

; Ask per-user vs per-machine and default to per-machine, per the workspace
; convention.  {autopf} follows whichever the user picks.
;
; This is a deliberate change from the deprecated Python installer, which was
; PrivilegesRequired=lowest.  It has a consequence the updater must honour:
; the silent re-run has to pass /ALLUSERS or /CURRENTUSER matching where the
; running exe actually lives, or it takes the per-machine default and plants a
; SECOND copy beside the per-user one.  See install_scope_flag in
; PDFSherpaCpp\app\Updater.cpp, which is tested for exactly that.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

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
