param([ValidateSet('Debug','Release')][string]$Configuration='Release',[string]$QtRoot='',[string]$MSBuild='', [string]$EvidenceDirectory='', [switch]$TransportTests, [string]$OutputDirectory='', [string]$DeployDirectory='', [switch]$CheckInputsOnly)
$ErrorActionPreference='Stop'
$rvProject=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'check_build_inputs.ps1')
Assert-RvBuildInputs -ProjectRoot $rvProject
if($CheckInputsOnly){Write-Output 'RV worker SDK and matching board build inputs are present.';return}
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'resolve_vs2022_toolchain.ps1')
$rvTools=Resolve-Vs2022Toolchain -MSBuild $MSBuild -RequireMSBuild -RequireDumpBin -RequireRedist
$QtRoot=Resolve-Qt611Root -QtRoot $QtRoot -MSBuild $rvTools.MSBuild
$rvSdk=Join-Path $rvProject 'x64/rv_control_sdk/install'
$rvOut=Join-Path $rvProject "x64/rv_control/$Configuration"
if($OutputDirectory){$rvOut=[IO.Path]::GetFullPath($OutputDirectory)}
if($TransportTests){$rvOut=Join-Path $rvProject 'x64/rv_control/transport_tests'}
if(!$EvidenceDirectory){$EvidenceDirectory=Join-Path $rvProject ('x64/validation/rv_build_'+(Get-Date -Format 'yyyyMMdd_HHmmss_fff'))}
$rvBuildOwner=Enter-NativeBuildLock -ProjectRoot $rvProject -Component rv -Configuration $Configuration
try {
New-Item -ItemType Directory -Force -Path $rvOut,$EvidenceDirectory | Out-Null
& $rvTools.MSBuild (Join-Path $rvProject 'RvControlWorker.vcxproj') /t:Rebuild "/p:Configuration=$Configuration" /p:Platform=x64 "/p:QtInstall=$QtRoot" "/p:RvTransportTests=$($TransportTests.IsPresent.ToString().ToLower())" "/p:OutDir=$rvOut/" "/p:IntDir=$rvOut/obj/" /m:2 /nologo /v:minimal *> (Join-Path $EvidenceDirectory 'rebuild.log')
if($LASTEXITCODE){Get-Content (Join-Path $EvidenceDirectory 'rebuild.log') -Tail 30;throw 'RV native worker rebuild failed'}
$rvRuntime=Join-Path $rvOut 'runtime'
if($DeployDirectory){$rvRuntime=[IO.Path]::GetFullPath($DeployDirectory)}
New-Item -ItemType Directory -Force -Path $rvRuntime | Out-Null
$rvExecutable=Join-Path $rvOut 'RvControlWorker.exe'
$rvSearch=@($rvOut,(Join-Path $rvSdk 'bin'),(Join-Path $QtRoot 'bin'))+@($rvTools.RedistDirectories)
$rvQueue=[Collections.Generic.Queue[string]]::new();$rvQueue.Enqueue($rvExecutable)
$rvDlls=@{};$rvVisited=@{}
while($rvQueue.Count){
 $rvImage=$rvQueue.Dequeue();$rvImageName=[IO.Path]::GetFileName($rvImage)
 if($rvVisited.ContainsKey($rvImageName)){continue};$rvVisited[$rvImageName]=$true
 foreach($rvLine in @(& $rvTools.DumpBin /nologo /dependents $rvImage)){
  if("$rvLine" -notmatch '^\s+([A-Za-z0-9_.-]+\.dll)\s*$'){continue};$rvName=$Matches[1]
  if($rvName -match '(?i)python|boost_python'){throw 'Python DLL in native SSH closure'}
  if($rvName -match '(?i)^(api-ms-win-|ext-ms-win-)'){continue}
  if($rvImageName -eq 'Qt6Core.dll' -and $rvName -eq 'icuuc.dll'){continue}
  if($rvDlls.ContainsKey($rvName)){continue}
  $rvResolved=$null
  foreach($rvDir in $rvSearch){$rvCandidate=Join-Path $rvDir $rvName;if(Test-Path -LiteralPath $rvCandidate -PathType Leaf){$rvResolved=$rvCandidate;break}}
  if(!$rvResolved){if($rvName -notmatch '(?i)^(msvcp|vcruntime|concrt)' -and (Test-Path -LiteralPath (Join-Path "$env:SystemRoot/System32" $rvName))){continue};throw "Unresolved RV dependency: $rvName"}
  $rvDlls[$rvName]=$rvResolved;$rvQueue.Enqueue($rvResolved)
 }
 if($LASTEXITCODE){throw 'RV PE dependency inspection failed'}
}
Copy-Item -LiteralPath $rvExecutable -Destination $rvRuntime -Force
$rvHashes=[ordered]@{}
foreach($rvName in ($rvDlls.Keys | Sort-Object)){Copy-Item -LiteralPath $rvDlls[$rvName] -Destination (Join-Path $rvRuntime $rvName) -Force;$rvHashes[$rvName]=(Get-FileHash (Join-Path $rvRuntime $rvName) -Algorithm SHA256).Hash.ToLower()}
New-Item -ItemType Directory -Force -Path (Join-Path $rvRuntime 'licenses') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'known_hosts') -Destination $rvRuntime -Force
Copy-Item -LiteralPath (Join-Path $rvSdk 'LICENSE-libssh2') -Destination (Join-Path $rvRuntime 'licenses/libssh2.txt') -Force
$rvOpenSslLicense=Join-Path $rvSdk 'LICENSE-OpenSSL'
if(!(Test-Path -LiteralPath $rvOpenSslLicense)){throw 'Pinned OpenSSL license is missing from the native SDK'}
Copy-Item -LiteralPath $rvOpenSslLicense -Destination (Join-Path $rvRuntime 'licenses/OpenSSL.txt') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'upstream.lock.json') -Destination (Join-Path $rvRuntime 'licenses/upstream.lock.json') -Force
$rvPayload=[ordered]@{}
New-Item -ItemType Directory -Force -Path (Join-Path $rvRuntime 'dtof_tuner') | Out-Null
foreach($rvName in @('session.sh','tmf8829_tuner','bounded_log.awk','LICENSE-MIT.TXT','provenance.json')) {
 Copy-Item -LiteralPath (Join-Path $rvProject "tools/dtof_tuner/payload/$rvName") -Destination (Join-Path $rvRuntime "dtof_tuner/$rvName") -Force
 $rvPayload["dtof_tuner/$rvName"]=(Get-FileHash (Join-Path $rvRuntime "dtof_tuner/$rvName") -Algorithm SHA256).Hash.ToLower()
}
Copy-Item -LiteralPath (Join-Path $rvProject 'tools/dtof_tuner/coordinator.sh') -Destination (Join-Path $rvRuntime 'dtof_tuner/coordinator.sh') -Force
$rvPayload['dtof_tuner/coordinator.sh']=(Get-FileHash (Join-Path $rvRuntime 'dtof_tuner/coordinator.sh') -Algorithm SHA256).Hash.ToLower()
$resolvedRuntime=(Get-Item -LiteralPath $rvRuntime).FullName
foreach($rvRetired in @('litesr_board','rv_capture/litesr')){
 $retiredCapture=Join-Path $resolvedRuntime $rvRetired
 if(Test-Path -LiteralPath $retiredCapture){
  $retiredItem=Get-Item -LiteralPath $retiredCapture -Force
  $retiredParent=Get-Item -LiteralPath (Split-Path -Parent $retiredCapture)
  if(!$retiredItem.PSIsContainer -or !$retiredItem.FullName.StartsWith($resolvedRuntime+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
     (($retiredItem.Attributes -bor $retiredParent.Attributes) -band [IO.FileAttributes]::ReparsePoint)){throw 'Invalid retired capture target'}
  Remove-Item -LiteralPath $retiredItem.FullName -Recurse -Force
 }
}
New-Item -ItemType Directory -Force -Path (Join-Path $rvRuntime 'rv_capture') | Out-Null
foreach($rvRetired in @('rv_capture/native-fov1.bin','rv_capture/native-fov2.bin','rv_capture/model-switch.sh','rv1103_bootstrap/native_hd_gui')) {
 $rvRetiredPath=[IO.Path]::GetFullPath((Join-Path $rvRuntime $rvRetired))
 if(!$rvRetiredPath.StartsWith([IO.Path]::GetFullPath($rvRuntime)+[IO.Path]::DirectorySeparatorChar)){throw 'Invalid retired payload path'}
 if(Test-Path -LiteralPath $rvRetiredPath){Remove-Item -LiteralPath $rvRetiredPath -Force}
}
foreach($rvName in @('native_gui.sh','dtof-host.sh','native-fov6.bin')) {
 $rvRelative="rv_capture/$rvName"
 $rvSource=if($rvName.EndsWith('.bin')){"tools/rv_capture/native/payload/$rvName"}else{"tools/rv_capture/payload/$rvName"}
 Copy-Item -LiteralPath (Join-Path $rvProject $rvSource) -Destination (Join-Path $rvRuntime $rvRelative) -Force
 $rvPayload[$rvRelative]=(Get-FileHash (Join-Path $rvRuntime $rvRelative) -Algorithm SHA256).Hash.ToLower()
}
$rvBootstrap=Join-Path $rvRuntime 'rv1103_bootstrap'
New-Item -ItemType Directory -Force -Path $rvBootstrap | Out-Null
$oldNpuLibrary=Join-Path $rvBootstrap 'librknnmrt.so'
if(Test-Path -LiteralPath $oldNpuLibrary){Remove-Item -LiteralPath $oldNpuLibrary -Force}
foreach($rvName in @('install.sh','ensure-spi.sh','camera-takeover.sh','S99zdepth','M48_geometry.bin','bounded_log.awk',
 'camera_rtsp_config.awk','depth_status','no_route_monitor.so','tof_driver',
 'depth-control.sh','candidate-control.sh','session.sh','service.sh')) {
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot "bootstrap/$rvName") -Destination $rvBootstrap -Force
 $rvPayload["rv1103_bootstrap/$rvName"]=(Get-FileHash -LiteralPath (Join-Path $rvBootstrap $rvName) -Algorithm SHA256).Hash.ToLower()
}
$rvCalibration=Join-Path $rvProject 'assets/camera/calibration_20261109'
Copy-Item -LiteralPath (Join-Path $rvCalibration 'M48_geometry.bin') -Destination $rvBootstrap -Force
$rvPayload['rv1103_bootstrap/M48_geometry.bin']=(Get-FileHash (Join-Path $rvBootstrap 'M48_geometry.bin')).Hash.ToLower()
New-Item -ItemType Directory -Force -Path (Join-Path $rvBootstrap 'calibration') | Out-Null
foreach($rvName in @('calibration.json','intrinsics.json','extrinsics.json','projection.bin','M48_geometry.bin','manifest.json','host_contract.json')) {
 Copy-Item -LiteralPath (Join-Path $rvCalibration $rvName) -Destination (Join-Path $rvBootstrap "calibration/$rvName") -Force
 $rvPayload["rv1103_bootstrap/calibration/$rvName"]=(Get-FileHash (Join-Path $rvBootstrap "calibration/$rvName")).Hash.ToLower()
}
foreach($rvName in @('known_hosts','licenses/libssh2.txt','licenses/OpenSSL.txt','licenses/upstream.lock.json')){$rvPayload[$rvName]=(Get-FileHash (Join-Path $rvRuntime $rvName) -Algorithm SHA256).Hash.ToLower()}
$rvBoardFiles=[ordered]@{}
foreach($rvName in $rvPayload.Keys){if($rvName -match '^(rv_capture|rv1103_bootstrap|dtof_tuner)/'){$rvBoardFiles[$rvName]=$rvPayload[$rvName]}}
[ordered]@{schema=1;calibration_release='RV1103_SC3336_TMF8829_20261109';processing_fov=6;
 supported_capture_modes=@(14,24,25);sensor_modes=@(5,11,12);models_required=$false;
 native_hole_fill_protocol='native48-hole-fill-v1';native_hole_fill_modes=@(24,25);
 replacement_policy='complete_hash_checked_transaction_with_rollback';files=$rvBoardFiles} |
 ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $rvRuntime 'board-import.json') -Encoding UTF8
$rvPayload['board-import.json']=(Get-FileHash (Join-Path $rvRuntime 'board-import.json')).Hash.ToLower()
[ordered]@{implementation='rv-control-cpp-v1';executable='RvControlWorker.exe';executable_sha256=(Get-FileHash $rvExecutable -Algorithm SHA256).Hash.ToLower();dependencies=$rvHashes;payload_files=$rvPayload;host_key_algorithm='ssh-ed25519';crypto_backend='OpenSSL';libssh2='1.11.1';transport_test_fixture=$TransportTests.IsPresent;python_required=$false;camera_opened=$false;board_connected=$false} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $rvRuntime 'implementation.json') -Encoding UTF8
Write-Output "Native RV $Configuration built/deployed: $rvRuntime"
} finally {Exit-NativeBuildLock -Mutex $rvBuildOwner}
