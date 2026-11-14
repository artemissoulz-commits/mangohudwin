#define RepoRoot AddBackslash(SourcePath) + "..\.."
#define PackageRoot RepoRoot + "\source\package"
#define OutputDirectory RepoRoot + "\installer-output"
#define AssetDirectory AddBackslash(SourcePath) + "assets"

[Setup]
AppId={{A867D12A-4C79-4F7A-8B49-4B7E889C03B4}
AppName=YORHUD - Windows Performance Overlay
AppVersion=0.2.0
AppVerName=YORHUD - Windows Performance Overlay 0.2.0
AppPublisher=YORHUD Windows Port
AppPublisherURL=https://github.com/artemissoulz-commits/mangohudwin
AppSupportURL=https://github.com/artemissoulz-commits/mangohudwin/issues
DefaultDirName={localappdata}\Programs\YorHud
DefaultGroupName=YORHUD
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0.19041
OutputDir={#OutputDirectory}
OutputBaseFilename=YorHud-Setup-x64
SetupLogging=yes
SetupIconFile={#AssetDirectory}\YorHud.ico
LicenseFile={#RepoRoot}\source\LICENSE
UninstallDisplayName=YORHUD - Windows Performance Overlay
UninstallDisplayIcon={app}\YorHud.ico
VersionInfoCompany=YORHUD Windows Port
VersionInfoDescription=YORHUD Windows Performance Overlay installer x64
VersionInfoProductName=YORHUD
VersionInfoProductVersion=0.2.0
VersionInfoVersion=0.2.0.0
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern light
WizardImageFile={#AssetDirectory}\YorHud-Wizard.bmp
WizardSmallImageFile={#AssetDirectory}\YorHud-WizardSmall.bmp
CloseApplications=no
RestartApplications=no
Uninstallable=yes
AllowNoIcons=yes

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Files]
Source: "{#RepoRoot}\source\LICENSE"; DestDir: "{app}"; DestName: "LICENSE"; Flags: ignoreversion uninsrestartdelete
; The experimental readme from the ZIP is replaced with the installer-specific
; instructions. Preserve any config file the user already edited on upgrades.
Source: "{#PackageRoot}\*"; DestDir: "{app}"; Excludes: "README-EXPERIMENTAL.txt,config\MangoHud.conf"; Flags: ignoreversion recursesubdirs createallsubdirs uninsrestartdelete
Source: "{#PackageRoot}\config\MangoHud.conf"; DestDir: "{app}\config"; Flags: onlyifdoesntexist uninsrestartdelete
Source: "assets\YorHud.ico"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "README-Windows.txt"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete

[Icons]
Name: "{group}\YORHUD"; Filename: "{app}\YorHud.exe"; IconFilename: "{app}\YorHud.ico"
Name: "{group}\README - Windows Port"; Filename: "{app}\README-Windows.txt"
Name: "{group}\Open install folder"; Filename: "{sys}\explorer.exe"; Parameters: """{app}"""
Name: "{group}\Open configuration folder"; Filename: "{sys}\explorer.exe"; Parameters: """{app}\config"""

[Run]
Filename: "{app}\YorHud.exe"; Description: "Iniciar YORHUD"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; Remove generated user state that is not tracked as an installed file.
Type: filesandordirs; Name: "{app}\cache"
Type: filesandordirs; Name: "{app}\config"
Type: files; Name: "{app}\skin.ini"

[Code]
function InitializeUninstall(): Boolean;
var
  ControlExe: string;
  ResultCode: Integer;
begin
  Result := True;
  ControlExe := ExpandConstant('{app}\YorHud.exe');

  if not FileExists(ControlExe) then
    Exit;

  { Ask Control to reset the overlay signals and exit before files are removed.
    The helper waits for the process; failure safely aborts uninstallation. }
  if not Exec(ControlExe, '/shutdown', ExpandConstant('{app}'), SW_HIDE,
      ewWaitUntilTerminated, ResultCode) then
  begin
    MsgBox('No se pudo cerrar YORHUD. Cierra el programa e inténtalo de nuevo.',
      mbError, MB_OK);
    Result := False;
    Exit;
  end;

  if ResultCode <> 0 then
  begin
    MsgBox('YORHUD no se ha cerrado correctamente (código ' +
      IntToStr(ResultCode) + '). No se desinstalará para evitar dejar el HUD activo. Cierra el programa e inténtalo de nuevo.',
      mbError, MB_OK);
    Result := False;
  end;
end;
