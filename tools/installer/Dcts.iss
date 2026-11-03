#define AppName "触想工作台"
#define AppExe "Depth Camera and Tactile Sensor.exe"

[Setup]
AppId={{E927DDB5-704F-4F63-AEC3-54D67D2B1177}
AppName={#AppName}
AppVersion={#Version}
DefaultDirName={autopf}\Depth Camera and Tactile Sensor
DefaultGroupName={#AppName}
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.17763
PrivilegesRequired=admin
WizardStyle=modern
DisableProgramGroupPage=yes
ShowLanguageDialog=no
OutputDir={#Output}
OutputBaseFilename=DCTS-Setup-{#Version}-x64
Compression=lzma2/fast
SolidCompression=yes
UninstallDisplayIcon={app}\{#AppExe}
SetupIconFile={#IconPath}
CloseApplications=yes

[Languages]
Name: "zh_CN"; MessagesFile: "{#ChineseMessages}"

[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{group}\卸载{#AppName}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"

[Run]
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Description: "启动{#AppName}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
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
