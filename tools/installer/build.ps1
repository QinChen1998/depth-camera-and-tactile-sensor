param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [string]$QtRoot='',
    [string]$MSBuild='',
    [string]$PlatformTools='',
    [string]$VlcRoot='',
    [string]$Version='1.1.8',
    [switch]$StageOnly,
    [string]$Iscc='',
    [string]$ChineseMessages='',
    [switch]$UseStagedPayload,
    [string]$ResearchRuntime=''
)
$ErrorActionPreference='Stop'
$project=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $project 'tools/resolve_vs2022_toolchain.ps1')
$work=[IO.Path]::GetFullPath($BuildDirectory)
$validation=Join-Path $project 'x64\validation'
if(-not $work.StartsWith($validation+'\',[StringComparison]::OrdinalIgnoreCase)){
    throw 'Use an isolated subdirectory of x64/validation.'
}
if($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$'){throw 'Invalid installer version.'}
$payload=Join-Path $work 'payload'
$research=[bool]$ResearchRuntime
$stagedResearchRuntime=Join-Path $payload 'DepthPro2Runtime'
$exe='触想工作台.exe'
$desktopFiles=@($exe)
$imagingSchemes=(Get-Content -LiteralPath (Join-Path $project 'DCTS_APP.json') -Raw | ConvertFrom-Json).imaging_schemes
$expectedImagingSchemes='host_assisted_depth','board_native_tof48_1200k','board_native_tof48_high_accuracy_1200k'
if(($imagingSchemes -join ',') -cne ($expectedImagingSchemes -join ',')){throw 'Unsupported desktop imaging menu.'}
$desktopReceipt=Join-Path $work 'desktop-imaging.json'

function Invoke-Native([string]$Program,[string[]]$Arguments){
    & $Program @Arguments
    if($LASTEXITCODE -ne 0){throw "$Program failed ($LASTEXITCODE)"}
}
function Digest([string]$Path){
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Copy-Relative([string]$Source,[string]$Destination,[string]$Name){
    if($Name -notmatch '^[A-Za-z0-9_. /-]+$' -or ($Name -split '/') -contains '..' -or $Name.StartsWith('/')){
        throw "Unexpected manifest path $Name"
    }
    $target=Join-Path $Destination $Name
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Source $Name) -Destination $target
}
function Write-Json([string]$Path,$Value){
    $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}

function Read-ProtectedResearchRuntime([string]$RuntimePath){
    $manifest=Get-Content -LiteralPath (Join-Path $RuntimePath 'runtime.json') -Raw | ConvertFrom-Json -AsHashtable
    $profile=Get-Content -LiteralPath (Join-Path $project 'sensors/RvCameraView.h') -Raw
    if($profile -notmatch 'DepthPro2EngineSha="([a-f0-9]{64})"' -or $manifest.engine_sha256 -cne $Matches[1] -or
       $manifest.schema -ne 1 -or $manifest.backend -cne 'native-tensorrt-fp16' -or
       $manifest.tensorrt_version -cne '10.16.1.11' -or $manifest.model_protection -cne 'aes-256-gcm'){
        throw 'The research installer requires the matching AES-GCM protected Depth Pro 2 runtime.'
    }
    $files=@('depthpro2-s-480x640-fp16.dctsm','DctsDepthPro2Runtime.dll','DctsDepthPro2Worker.exe',
        'Qt6Core.dll','cudart64_12.dll','nvinfer_10.dll','nvinfer_plugin_10.dll',
        'Apple-Model-LICENSE.txt','NVIDIA-TensorRT-LICENSE.txt','NVIDIA-CUDA-LICENSE.txt','Research-Model-NOTICE.txt')
    if($manifest.files.Count -ne $files.Count -or
       @(Get-ChildItem -LiteralPath $RuntimePath -Force).Count -ne ($files.Count + 1)){
        throw 'Unexpected research runtime contents.'
    }
    foreach($name in $files){
        $file=Join-Path $RuntimePath $name
        if(!(Test-Path -LiteralPath $file -PathType Leaf) -or (Digest $file) -cne $manifest.files[$name]){
            throw "Research runtime fingerprint mismatch: $name"
        }
    }
    $reader=[IO.BinaryReader]::new([IO.File]::OpenRead((Join-Path $RuntimePath $files[0])))
    try {
        $length=$reader.BaseStream.Length
        if($length -le 96 -or [Text.Encoding]::ASCII.GetString($reader.ReadBytes(8)) -cne "DCTSM01`0" -or
           $reader.ReadUInt32() -ne 1 -or $reader.ReadUInt32() -ne 96 -or $reader.ReadUInt64() -ne ($length - 96)){
            throw 'Invalid encrypted engine envelope in the research payload.'
        }
    } finally {$reader.Dispose()}
    return ($files + 'runtime.json')
}

function Assert-BoardCapturePayload([string]$Worker,$WorkerManifest,[switch]$Staged){
    $boardImport=Get-Content -LiteralPath (Join-Path $Worker 'board-import.json') -Raw | ConvertFrom-Json -AsHashtable
    $expectedCapture=@('rv_capture/dtof-host.sh','rv_capture/native_gui.sh','rv_capture/native-fov6.bin')
    $captureFiles=@($WorkerManifest.payload_files.Keys | Where-Object {$_ -like 'rv_capture/*'})
    if($WorkerManifest.transport_test_fixture -or ($boardImport.supported_capture_modes -join ',') -ne '14,24,25' -or
       $boardImport.models_required -ne $false -or $captureFiles.Count -ne $expectedCapture.Count -or
       @($expectedCapture | Where-Object {$captureFiles -cnotcontains $_}).Count){
        throw 'The board capture payload does not match the desktop imaging menu.'
    }
    $boardFiles=@($WorkerManifest.payload_files.Keys | Where-Object {$_ -match '^(rv_capture|rv1103_bootstrap|dtof_tuner)/'})
    if($boardImport.files.Count -ne $boardFiles.Count){throw 'The board import file list is incomplete.'}
    foreach($name in $boardFiles){
        $file=Join-Path $Worker $name
        if($boardImport.files[$name] -cne $WorkerManifest.payload_files[$name] -or
           !(Test-Path -LiteralPath $file -PathType Leaf) -or (Digest $file) -cne $boardImport.files[$name]){
            throw "Board import fingerprint mismatch: $name"
        }
    }
    if($Staged){
        $captureRoot=Join-Path $Worker 'rv_capture'
        foreach($file in Get-ChildItem -LiteralPath $captureRoot -File -Recurse){
            $relative=[IO.Path]::GetRelativePath($Worker,$file.FullName).Replace('\','/')
            if($expectedCapture -cnotcontains $relative){throw "Unexpected capture payload: $relative"}
        }
    }
}

if($UseStagedPayload){
    if(-not (Test-Path -LiteralPath $desktopReceipt -PathType Leaf)){
        throw 'Stage the desktop from current source before reusing this payload.'
    }
    $receipt=Get-Content -LiteralPath $desktopReceipt -Raw | ConvertFrom-Json -AsHashtable
    if(($receipt.imaging_schemes -join ',') -cne ($imagingSchemes -join ',')){
        throw 'The staged desktop has a different imaging menu. Stage it again from current source.'
    }
    if([bool]$receipt.research -ne $research){throw 'The staged desktop has a different installer edition.'}
    if($research -and (Digest (Join-Path $stagedResearchRuntime 'runtime.json')) -cne $receipt.research_manifest_sha256){
        throw 'The staged research runtime manifest changed.'
    }
    foreach($name in $desktopFiles){
        $file=Join-Path $payload $name
        if(-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Digest $file) -ne $receipt.files[$name]){
            throw "Staged desktop fingerprint mismatch: $name"
        }
    }
    $stagedWorker=Join-Path $payload 'rv_control'
    $stagedWorkerManifest=Get-Content -LiteralPath (Join-Path $stagedWorker 'implementation.json') -Raw | ConvertFrom-Json -AsHashtable
    Assert-BoardCapturePayload $stagedWorker $stagedWorkerManifest -Staged
}

if(-not $UseStagedPayload){
    if(-not $VlcRoot){throw 'Provide the verified official VLC x64 directory.'}
    if(Test-Path -LiteralPath $payload){throw 'The payload already exists. Choose a fresh build directory.'}
    $toolchain=Resolve-Vs2022Toolchain -MSBuild $MSBuild -RequireMSBuild -RequireRedist
    $qt=Resolve-Qt611Root -QtRoot $QtRoot -MSBuild $toolchain.MSBuild
    if(-not $PlatformTools){$PlatformTools=Join-Path $project 'x64/Release/platform-tools'}
    if($research){
        $ResearchRuntime=(Resolve-Path -LiteralPath $ResearchRuntime).Path
        $researchFiles=Read-ProtectedResearchRuntime $ResearchRuntime
    }
    $desktop=Join-Path $work 'desktop'
    New-Item -ItemType Directory -Force -Path $desktop | Out-Null

    Invoke-Native (Join-Path $qt 'bin/lrelease.exe') @(
        (Join-Path $project 'translations/hand_zh_CN.ts'),'-qm',(Join-Path $project 'translations/hand_zh_CN.qm'))
    Invoke-Native $toolchain.MSBuild @(
        (Join-Path $project 'Depth Camera and Tactile Sensor.sln'),'/t:Build','/p:Configuration=Release','/p:Platform=x64',
        "/p:QtInstall=$qt",'/p:DctsInstallerBuild=true',"/p:DctsResearchInstaller=$($research.ToString().ToLowerInvariant())",
        '/p:NativeWorkersBuild=true',"/p:OutDir=$desktop/","/p:IntDir=$work/obj/",
        '/m:2','/nologo','/v:minimal',"/flp:logfile=$work/build.log;verbosity=normal")

    New-Item -ItemType Directory -Path $payload | Out-Null
    Copy-Item -LiteralPath (Join-Path $desktop $exe) -Destination $payload
    Invoke-Native (Join-Path $qt 'bin/windeployqt.exe') @(
        '--release','--no-translations','--no-compiler-runtime',(Join-Path $payload $exe))
    Copy-Relative $desktop $payload 'CxwlDepthSdk.dll'
    Copy-Item -LiteralPath (Join-Path $desktop 'cxwl-depth-sdk') -Destination $payload -Recurse
    Get-ChildItem -LiteralPath $toolchain.RedistDirectories[0] -Filter '*.dll' | Copy-Item -Destination $payload
    foreach($name in 'avcodec-61.dll','avformat-61.dll','avutil-59.dll','swresample-5.dll','swscale-8.dll'){
        if(-not (Test-Path -LiteralPath (Join-Path $payload $name) -PathType Leaf)){throw "Missing Qt FFmpeg library $name"}
    }

    $worker=Join-Path $project 'x64/rv_control/Release/runtime'
    $workerPayload=Join-Path $payload 'rv_control'
    $workerManifest=Get-Content -LiteralPath (Join-Path $worker 'implementation.json') -Raw | ConvertFrom-Json -AsHashtable
    $workerFiles=@{'RvControlWorker.exe'=$workerManifest.executable_sha256}
    foreach($table in $workerManifest.dependencies,$workerManifest.payload_files){
        foreach($name in $table.Keys){$workerFiles[$name]=$table[$name]}
    }
    Assert-BoardCapturePayload $worker $workerManifest
    foreach($name in $workerFiles.Keys){
        Copy-Relative $worker $workerPayload $name
        if((Digest (Join-Path $workerPayload $name)) -ne $workerFiles[$name]){throw "Worker fingerprint mismatch: $name"}
    }
    Copy-Relative $worker $workerPayload 'implementation.json'
    Assert-BoardCapturePayload $workerPayload $workerManifest -Staged
    foreach($name in 'adb.exe','AdbWinApi.dll','AdbWinUsbApi.dll','libwinpthread-1.dll','NOTICE.txt','source.properties'){
        Copy-Relative $PlatformTools (Join-Path $payload 'platform-tools') $name
    }

    Write-Json (Join-Path $payload 'runtime-paths.json') @{
        RV_CONTROL_CPP='rv_control/RvControlWorker.exe'
    }
    if($research){
        foreach($name in $researchFiles){Copy-Relative $ResearchRuntime $stagedResearchRuntime $name}
    }

    $notices=Join-Path $payload 'licenses'
    Copy-Relative (Join-Path $project 'third_party/ffmpeg_7_1_3') $notices 'COPYING.LGPLv2.1'
    Copy-Relative $PSScriptRoot $notices 'Third-Party-Notices.txt'
    $qtSource=Join-Path (Split-Path $qt) 'Src'
    Copy-Relative (Join-Path $qtSource 'qtbase/LICENSES') $notices 'LGPL-3.0-only.txt'
    $sbom=Join-Path $qt 'sbom'
    foreach($module in 'qtbase','qtmultimedia','qtserialport','qtsvg','qtimageformats'){
        Copy-Relative $sbom (Join-Path $notices 'qt-sbom') "$module-6.11.1.spdx"
    }
}

$vlcPayload=Join-Path $payload 'vlc'
if($VlcRoot){
    if(Test-Path -LiteralPath $vlcPayload){throw 'VLC already staged. Use the verified payload or build in a fresh directory.'}
    $vlcSource=(Resolve-Path -LiteralPath $VlcRoot).Path
    $vlcVersion=(Get-Item -LiteralPath (Join-Path $vlcSource 'libvlc.dll')).VersionInfo.FileVersion
    if($vlcVersion -notmatch '^3\.0\.\d+$'){throw "Unsupported libVLC version: $vlcVersion"}
    foreach($name in 'libvlc.dll','libvlccore.dll'){
        Copy-Relative $vlcSource $vlcPayload $name
    }
    $plugins=Join-Path $vlcSource 'plugins'
    Get-ChildItem -LiteralPath $plugins -File -Recurse | Where-Object {
        $_.Extension -eq '.dll'
    } | ForEach-Object {
        $relative=[IO.Path]::GetRelativePath($vlcSource,$_.FullName).Replace('\','/')
        if(($relative -split '/')[1] -notin 'gui','visualization'){
            Copy-Relative $vlcSource $vlcPayload $relative
        }
    }
    Invoke-Native (Join-Path $vlcSource 'vlc-cache-gen.exe') @((Join-Path $vlcPayload 'plugins'))
    $vlcNotices=Join-Path $payload 'licenses/VLC'
    foreach($name in 'COPYING.txt','AUTHORS.txt'){
        Copy-Relative $vlcSource $vlcNotices $name
    }
    @(
        "VideoLAN VLC $vlcVersion x64",
        "Corresponding upstream release source: https://download.videolan.org/pub/videolan/vlc/$vlcVersion/vlc-$vlcVersion.tar.xz",
        'Module licensing information and dependencies: https://www.videolan.org/legal.html'
    ) | Set-Content -LiteralPath (Join-Path $vlcNotices 'Source.txt') -Encoding utf8NoBOM
}
foreach($name in 'libvlc.dll','libvlccore.dll','plugins/codec/libavcodec_plugin.dll','plugins/access/liblive555_plugin.dll'){
    if(-not (Test-Path -LiteralPath (Join-Path $vlcPayload $name) -PathType Leaf)){
        throw "Missing VLC x64 managed H.265 runtime ($name). Supply an official VLC 3 Windows distribution with -VlcRoot."
    }
}

foreach($name in $desktopFiles){
    if(-not (Test-Path -LiteralPath (Join-Path $payload $name) -PathType Leaf)){
        throw "Missing application executable in staged payload ($name)."
    }
}

if($research){$null=Read-ProtectedResearchRuntime $stagedResearchRuntime}
$legacyInference = Get-ChildItem -LiteralPath $payload -Directory | Where-Object {
    $_.Name -match '^UniDepthRuntime' -or (!$research -and $_.Name -eq 'DepthPro2Runtime')
}
if($legacyInference){throw 'Unexpected local inference assets in the installer payload.'}
$unexpected=Get-ChildItem -LiteralPath $payload -File -Recurse | Where-Object {
    $_.Name -match '^(README|AGENTS)(\..*)?$|^(PackModelCopy|DctsInferenceWorker)\.exe$|model_key_material' -or
    $_.Name -match '^(onnxruntime_providers_cuda|cudnn.*|cublas.*|cufft.*|nvrtc.*|zlibwapi|RvAssistTensorRtRtx)\.(dll|exe)$' -or
    $_.Extension -match '^\.(onnx|ovm|rknn|pt|pth|engine|safetensors|pdb|ilk|lib|exp|vcxproj|py)$' -or
    (($_.Extension -eq '.dctsm' -or $_.Name -in 'DctsDepthPro2Worker.exe','DctsDepthPro2Runtime.dll') -and
        (!$research -or $_.DirectoryName -ne $stagedResearchRuntime))
}
if($unexpected){throw "Unexpected payload content: $($unexpected.FullName -join ', ')"}
if(-not $UseStagedPayload){
    $desktopHashes=@{}
    foreach($name in $desktopFiles){$desktopHashes[$name]=Digest (Join-Path $payload $name)}
    $desktopSnapshot=@{imaging_schemes=$imagingSchemes;files=$desktopHashes;research=$research}
    if($research){$desktopSnapshot.research_manifest_sha256=Digest (Join-Path $stagedResearchRuntime 'runtime.json')}
    Write-Json $desktopReceipt $desktopSnapshot
}
if($StageOnly){return}
if(-not $Iscc -or -not $ChineseMessages){throw 'Provide -Iscc and -ChineseMessages for installer compilation.'}
$out=Join-Path $work 'delivery'
New-Item -ItemType Directory -Force $out | Out-Null
$icon=(Resolve-Path -LiteralPath (Join-Path $project 'assets/cx.ico')).Path
$defines=@(
    "/DVersion=$Version","/DOutput=$out","/DPayload=$payload","/DIconPath=$icon",
    "/DChineseMessages=$((Resolve-Path -LiteralPath $ChineseMessages).Path)")
if($research){$defines+='/DResearch'}
Invoke-Native (Resolve-Path -LiteralPath $Iscc).Path ($defines + (Join-Path $PSScriptRoot 'Dcts.iss'))
$product=if($research){'触想工作台'}else{'触想工作台-内测版'}
$installer=Join-Path $out "$product-v$Version-x64.exe"
"$(Digest $installer) *$(Split-Path $installer -Leaf)" | Set-Content -LiteralPath "$installer.sha256" -Encoding utf8NoBOM
Get-Item -LiteralPath $installer
