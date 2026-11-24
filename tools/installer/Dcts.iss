#define AppName "触想工作台"
#define AppExe "触想工作台.exe"

[Setup]
#ifdef Research
AppId={{A5D7FCC3-8B49-4F58-AB05-E8712DEAEC3A}
LicenseFile={#Payload}\DepthPro2Runtime\Apple-Model-LICENSE.txt
DefaultDirName={autopf}\DCTS\{#AppName}
DefaultGroupName=DCTS\{#AppName}
UsePreviousGroup=no
UninstallDisplayName={#AppName}
#else
AppId={{E927DDB5-704F-4F63-AEC3-54D67D2B1177}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
#endif
AppName={#AppName}
AppVersion={#Version}
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19041
PrivilegesRequired=admin
WizardStyle=modern
DisableProgramGroupPage=yes
ShowLanguageDialog=no
OutputDir={#Output}
#ifdef Research
OutputBaseFilename={#AppName}-v{#Version}-x64
#else
OutputBaseFilename={#AppName}-内测版-v{#Version}-x64
#endif
Compression=lzma2/fast
SolidCompression=yes
UninstallDisplayIcon={app}\{#AppExe}
SetupIconFile={#IconPath}
CloseApplications=yes

[Languages]
Name: "zh_CN"; MessagesFile: "{#ChineseMessages}"

#ifdef Research
[LangOptions]
zh_CN.DialogFontName=Microsoft YaHei UI
zh_CN.WelcomeFontName=Microsoft YaHei UI
#endif

[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{group}\卸载{#AppName}"; Filename: "{uninstallexe}"
#ifdef Research
Name: "{commondesktop}\{#AppName}（上位机辅助推理）"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
#else
Name: "{commondesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
#endif

[Run]
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Description: "启动{#AppName}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
#ifdef Research
procedure CurStepChanged(CurStep: TSetupStep);
var
  OldGroup: String;
begin
  if CurStep <> ssPostInstall then
    Exit;
  OldGroup := ExpandConstant('{commonprograms}\触想工作台-科研版');
  DeleteFile(ExpandConstant('{commondesktop}\触想工作台-科研版.lnk'));
  DeleteFile(OldGroup + '\触想工作台-科研版.lnk');
  DeleteFile(OldGroup + '\卸载触想工作台-科研版.lnk');
  RemoveDir(OldGroup);
end;
#else
var
  ReplacingCudaRuntime: Boolean;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Manifest: AnsiString;
begin
  Result := '';
  ReplacingCudaRuntime := False;
  if FileExists(ExpandConstant('{app}\unins000.dat')) then
    if LoadStringFromFile(ExpandConstant('{app}\UniDepthRuntimeWide576FP16\runtime.json'), Manifest) then
      ReplacingCudaRuntime := Pos('"onnxruntime-cuda"', String(Manifest)) > 0;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Names: TArrayOfString;
  I: Integer;
  FileName: String;
begin
  if (CurStep <> ssPostInstall) or not ReplacingCudaRuntime then
    Exit;
  Names := [
    'onnxruntime_providers_cuda.dll', 'cublas64_12.dll', 'cublasLt64_12.dll', 'cufft64_11.dll',
    'cudnn64_9.dll', 'cudnn_adv64_9.dll', 'cudnn_cnn64_9.dll', 'cudnn_engines_precompiled64_9.dll',
    'cudnn_engines_runtime_compiled64_9.dll', 'cudnn_graph64_9.dll', 'cudnn_heuristic64_9.dll',
    'cudnn_ops64_9.dll', 'nvrtc-builtins64_124.dll', 'nvrtc64_120_0.dll', 'zlibwapi.dll'
  ];
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    FileName := ExpandConstant('{app}\UniDepthRuntimeWide576FP16\bin\') + Names[I];
    if FileExists(FileName) and not DeleteFile(FileName) then
      RestartReplace(FileName, '');
  end;
end;
#endif
