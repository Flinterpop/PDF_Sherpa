; Inno Setup script for PDF Guide
; Build with:  iscc installer.iss   (produces installer\PDFGuide-Setup.exe)
; Requires the app to be built first:  python -m PyInstaller PDFGuide.spec

#define AppName "PDF Guide"
#define AppVersion "1.0.0"
#define AppExe "PDFGuide.exe"

[Setup]
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=RabidFox
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=installer
OutputBaseFilename=PDFGuide-Setup
Compression=lzma2
SolidCompression=yes
; Per-user install so no admin rights are needed.
PrivilegesRequired=lowest
WizardStyle=modern

[Files]
Source: "dist\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

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
