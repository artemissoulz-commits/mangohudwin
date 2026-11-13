#define RepoRoot AddBackslash(SourcePath) + "..\.."
#define PackageRoot RepoRoot + "\source\package"
#define OutputDirectory RepoRoot + "\installer-output"
#define AssetDirectory AddBackslash(SourcePath) + "assets"

[Setup]
AppId={{A867D12A-4C79-4F7A-8B49-4B7E889C03B4}
AppName=MangoHud para Windows (experimental)
AppVersion=0.1.0
AppVerName=MangoHud para Windows (experimental) 0.1.0
AppPublisher=MangoHud Windows Port contributors
AppPublisherURL=https://github.com/artemissoulz-commits/mangohudwin
AppSupportURL=https://github.com/artemissoulz-commits/mangohudwin/issues
DefaultDirName={localappdata}\Programs\MangoHud
DefaultGroupName=MangoHud para Windows
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0.19041
OutputDir={#OutputDirectory}
OutputBaseFilename=MangoHud-Windows-Setup-x64
SetupLogging=yes
SetupIconFile={#AssetDirectory}\MangoHud.ico
WizardImageFile={#AssetDirectory}\WizardImage.bmp
WizardSmallImageFile={#AssetDirectory}\WizardSmallImage.bmp
LicenseFile={#RepoRoot}\source\LICENSE
UninstallDisplayName=MangoHud para Windows (experimental)
UninstallDisplayIcon={app}\MangoHud.ico
VersionInfoCompany=MangoHud Windows Port
VersionInfoDescription=Instalador experimental x64 de MangoHud para Windows
VersionInfoProductName=MangoHud para Windows
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
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Files]
Source: "{#RepoRoot}\source\LICENSE"; DestDir: "{app}"; DestName: "LICENSE"; Flags: ignoreversion
; The experimental readme from the ZIP is replaced with the installer-specific
; instructions. Preserve any config file the user already edited on upgrades.
Source: "{#PackageRoot}\*"; DestDir: "{app}"; Excludes: "README-EXPERIMENTAL.txt,config\MangoHud.conf"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PackageRoot}\config\MangoHud.conf"; DestDir: "{app}\config"; Flags: onlyifdoesntexist
Source: "README-Windows.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\MangoHud Control"; Filename: "{app}\MangoHudControl.exe"; IconFilename: "{app}\MangoHud.ico"
Name: "{group}\README - Windows Port"; Filename: "{app}\README-Windows.txt"
Name: "{group}\Open install folder"; Filename: "{sys}\explorer.exe"; Parameters: """{app}"""
Name: "{group}\Open configuration folder"; Filename: "{sys}\explorer.exe"; Parameters: """{app}\config"""

[Run]
Filename: "{app}\MangoHudControl.exe"; Description: "Iniciar MangoHud Control"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\cache"
