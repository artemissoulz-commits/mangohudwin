#define RepoRoot AddBackslash(SourcePath) + "..\.."
#define PackageRoot RepoRoot + "\source\package"
#define OutputDirectory RepoRoot + "\installer-output"

[Setup]
AppId={{A867D12A-4C79-4F7A-8B49-4B7E889C03B4}
AppName=MangoHud for Windows (Experimental)
AppVersion=0.1.0
AppVerName=MangoHud for Windows (Experimental) 0.1.0
AppPublisher=MangoHud Windows Port contributors
AppPublisherURL=https://github.com/artemissoulz-commits/mangohudwin
AppSupportURL=https://github.com/artemissoulz-commits/mangohudwin/issues
DefaultDirName={autopf}\MangoHud
DefaultGroupName=MangoHud
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0.19041
OutputDir={#OutputDirectory}
OutputBaseFilename=MangoHud-Windows-Setup-x64
SetupLogging=yes
UninstallDisplayName=MangoHud for Windows (Experimental)
UninstallDisplayIcon={app}\tools\MangoHud.exe
VersionInfoCompany=MangoHud Windows Port
VersionInfoDescription=Experimental x64 installer for MangoHud on Windows
VersionInfoProductName=MangoHud for Windows
VersionInfoProductVersion=0.1.0
VersionInfoVersion=0.1.0.0
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no
Uninstallable=yes
AllowNoIcons=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; The experimental readme from the ZIP is replaced with the installer-specific
; instructions. Preserve any config file the user already edited on upgrades.
Source: "{#PackageRoot}\*"; DestDir: "{app}"; Excludes: "README-EXPERIMENTAL.txt;config\MangoHud.conf"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PackageRoot}\config\MangoHud.conf"; DestDir: "{app}\config"; Flags: onlyifdoesntexist
Source: "README-Windows.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\README - Windows Port"; Filename: "{app}\README-Windows.txt"
Name: "{group}\Open install folder"; Filename: "{sys}\explorer.exe"; Parameters: """{app}"""
Name: "{group}\Open configuration folder"; Filename: "{sys}\explorer.exe"; Parameters: """{app}\config"""

[UninstallDelete]
Type: filesandordirs; Name: "{app}\cache"
