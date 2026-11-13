param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [string]$QtRoot='',
    [string]$MSBuild='',
    [string]$PlatformTools='',
    [string]$VlcRoot='',
    [string]$TensorRtRuntime='',
    [string]$CompatibleRuntime='',
    [string]$Version='1.1.8',
    [switch]$StageOnly,
    [string]$Iscc='',
    [string]$ChineseMessages='',
    [switch]$UseStagedPayload
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
$exe='触想工作台.exe'
$desktopFiles=@($exe,'DctsDepthSdk.dll','DctsInferenceWorker.exe')
$imagingSchemes=(Get-Content -LiteralPath (Join-Path $project 'DCTS_APP.json') -Raw | ConvertFrom-Json).imaging_schemes
$expectedImagingSchemes='host_unidepth_rgb672_vga_fp16_tof48_b','board_native_tof48','board_native_tof48_high_accuracy'
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

function Assert-BoardCapturePayload([string]$Worker,$WorkerManifest){
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
    $captureRoot=Join-Path $Worker 'rv_capture'
    foreach($file in Get-ChildItem -LiteralPath $captureRoot -File -Recurse){
        $relative=[IO.Path]::GetRelativePath($Worker,$file.FullName).Replace('\','/')
        if($expectedCapture -cnotcontains $relative){throw "Unexpected capture payload: $relative"}
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
    foreach($name in $desktopFiles){
        $file=Join-Path $payload $name
        if(-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Digest $file) -ne $receipt.files[$name]){
            throw "Staged desktop fingerprint mismatch: $name"
        }
    }
    $stagedWorker=Join-Path $payload 'rv_control'
    $stagedWorkerManifest=Get-Content -LiteralPath (Join-Path $stagedWorker 'implementation.json') -Raw | ConvertFrom-Json -AsHashtable
    Assert-BoardCapturePayload $stagedWorker $stagedWorkerManifest
}

if(-not $UseStagedPayload){
    if(-not $TensorRtRuntime){$TensorRtRuntime=Join-Path $project 'UniDepthRuntimeRgb672VgaFP16RTX'}
    if(-not $CompatibleRuntime){$CompatibleRuntime=Join-Path $project 'UniDepthRuntimeCompatible'}
    if(-not $VlcRoot){throw 'Provide the verified official VLC x64 directory.'}
    if(Test-Path -LiteralPath $payload){throw 'The payload already exists. Choose a fresh build directory.'}
    $toolchain=Resolve-Vs2022Toolchain -MSBuild $MSBuild -RequireMSBuild -RequireRedist
    $qt=Resolve-Qt611Root -QtRoot $QtRoot -MSBuild $toolchain.MSBuild
    if(-not $PlatformTools){$PlatformTools=Join-Path $project 'x64/Release/platform-tools'}
    $desktop=Join-Path $work 'desktop'
    $private=Join-Path $work 'private_build'
    New-Item -ItemType Directory -Force -Path $desktop,$private | Out-Null

    Invoke-Native (Join-Path $qt 'bin/lrelease.exe') @(
        (Join-Path $project 'translations/hand_zh_CN.ts'),'-qm',(Join-Path $project 'translations/hand_zh_CN.qm'))
    Invoke-Native $toolchain.MSBuild @(
        (Join-Path $project 'Depth Camera and Tactile Sensor.sln'),'/t:Build','/p:Configuration=Release','/p:Platform=x64',
        "/p:QtInstall=$qt",'/p:DctsInstallerBuild=true','/p:NativeWorkersBuild=true',"/p:OutDir=$desktop/","/p:DctsIntermediateRoot=$work/obj",
        '/m:2','/nologo','/v:minimal',"/flp:logfile=$work/build.log;verbosity=normal")

    New-Item -ItemType Directory -Path $payload | Out-Null
    Copy-Item -LiteralPath (Join-Path $desktop $exe) -Destination $payload
    Copy-Relative $desktop $payload 'DctsDepthSdk.dll'
    Copy-Relative $desktop $payload 'DctsInferenceWorker.exe'
    Invoke-Native (Join-Path $qt 'bin/windeployqt.exe') @(
        '--release','--qmldir',(Join-Path $project 'qml'),'--no-translations','--no-compiler-runtime',
        '--skip-plugin-types','qmltooling','--exclude-plugins','qtvirtualkeyboardplugin',
        (Join-Path $payload $exe),(Join-Path $payload 'DctsDepthSdk.dll'))
    Get-ChildItem -LiteralPath (Join-Path $payload 'qml') -Filter '*.qmltypes' -Recurse -File |
        ForEach-Object {Remove-Item -LiteralPath $_.FullName}
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
    foreach($name in 'adb.exe','AdbWinApi.dll','AdbWinUsbApi.dll','libwinpthread-1.dll','NOTICE.txt','source.properties'){
        Copy-Relative $PlatformTools (Join-Path $payload 'platform-tools') $name
    }

    $runtime=(Resolve-Path -LiteralPath $TensorRtRuntime).Path
    $modelPayload=Join-Path $payload 'UniDepthRuntimeRgb672VgaFP16RTX'
    $manifest=Get-Content -LiteralPath (Join-Path $runtime 'runtime.json') -Raw | ConvertFrom-Json -AsHashtable
    $graph='unidepthv2-small-rgb672-vga.onnx'
    $original=Join-Path $runtime $graph
    $originalHash=Digest $original
    $originalWrite=(Get-Item -LiteralPath $original).LastWriteTimeUtc
    if($manifest.backend -ne 'onnxruntime-tensorrt-rtx' -or $manifest.ort_version -ne '1.31.1' -or
       $originalHash -ne '50d22cd33e126832793d905788a1efadeb4c9298de72cbfa634d49fdcdbc2dc1' -or
       $originalHash -ne $manifest.files[$graph]){throw 'RGB672/VGA runtime or source graph identity mismatch.'}
    $files=@{}
    foreach($name in $manifest.files.Keys){
        if($name -eq $graph){continue}
        Copy-Relative $runtime $modelPayload $name
        $actual=Digest (Join-Path $modelPayload $name)
        if($actual -ne $manifest.files[$name]){throw "Model asset fingerprint mismatch: $name"}
        $files[$name]=$actual
    }
    $manifest.files=$files
    Copy-Relative $runtime $private $graph
    $protectionProject=Join-Path $project 'tools/model_protection/ModelProtection.vcxproj'
    $buildArgs=@($protectionProject,'/t:Build','/p:Configuration=Release','/p:Platform=x64',"/p:DctsPrivateBuildDir=$private",'/nologo','/v:minimal')
    Invoke-Native $toolchain.MSBuild ($buildArgs + '/p:PackModelCopy=true')
    $key=Join-Path $private 'model_key_material.h'
    $packReport=& (Join-Path $private 'bin/Release/PackModelCopy.exe') (Join-Path $private $graph) (Join-Path $modelPayload 'model.dctsm') $key
    if($LASTEXITCODE -ne 0){throw 'Model encryption failed'}
    $verification=$packReport | ConvertFrom-Json
    if(-not $verification.roundtrip_bitwise_identical -or $verification.plaintext_sha256 -ne $originalHash){throw 'Model encryption round-trip mismatch.'}
    $packReport | Set-Content -LiteralPath (Join-Path $work 'model-encryption.json') -Encoding utf8NoBOM
    if((Digest $original) -ne $originalHash -or (Get-Item -LiteralPath $original).LastWriteTimeUtc -ne $originalWrite){throw 'Original model changed.'}
    Invoke-Native $toolchain.MSBuild $buildArgs
    Copy-Relative (Join-Path $private 'bin/Release') (Join-Path $modelPayload 'bin') 'DctsModelRuntime.dll'
    $manifest.files['model.dctsm']=Digest (Join-Path $modelPayload 'model.dctsm')
    $manifest.files['bin/DctsModelRuntime.dll']=Digest (Join-Path $modelPayload 'bin/DctsModelRuntime.dll')
    $manifest.model_protection=@{scheme='aes256-gcm-memory-v1';loader_abi=1;ciphertext='model.dctsm';loader='bin/DctsModelRuntime.dll';plaintext_sha256=$originalHash}
    Write-Json (Join-Path $modelPayload 'runtime.json') $manifest

    # Native fallback engines share the protected original ONNX above. Only
    # the optional decoder INT8 IR is an additional model, encrypted separately.
    $compatSource=(Resolve-Path -LiteralPath $CompatibleRuntime).Path
    $compatPayload=Join-Path $payload 'UniDepthRuntimeCompatible'
    $compat=Get-Content -LiteralPath (Join-Path $compatSource 'runtime.json') -Raw | ConvertFrom-Json -AsHashtable
    if($compat.schema -ne 1 -or $compat.source_graph_sha256 -ne $originalHash -or
       $compat.ort_version -ne '1.24.4' -or $compat.openvino_version -ne '2026.4.1' -or
       $compat.int8.sha256 -ne '7dc50150c210b75e72fac35b45967ef874d85e9a883040223dbd5ebd4e01d63d'){
        throw 'Compatible runtime or INT8 source identity mismatch.'
    }
    $int8Source=Join-Path $compatSource $compat.int8.file
    $int8Write=(Get-Item -LiteralPath $int8Source).LastWriteTimeUtc
    $compatFiles=@{}
    foreach($name in $compat.files.Keys){
        if((Digest (Join-Path $compatSource $name)) -ne $compat.files[$name]){throw "Compatible runtime fingerprint mismatch: $name"}
        if($name -eq $compat.int8.file){continue}
        Copy-Relative $compatSource $compatPayload $name
        $compatFiles[$name]=Digest (Join-Path $compatPayload $name)
    }
    $int8Private=Join-Path $private 'int8'
    New-Item -ItemType Directory -Path $int8Private | Out-Null
    Copy-Relative $compatSource $int8Private $compat.int8.file
    $int8Cipher='decoder-int8.dctsm'
    $int8Report=& (Join-Path $private 'bin/Release/PackModelCopy.exe') (Join-Path $int8Private $compat.int8.file) (Join-Path $compatPayload $int8Cipher) (Join-Path $int8Private 'model_key_material.h')
    if($LASTEXITCODE -ne 0){throw 'INT8 model encryption failed'}
    $int8Verification=$int8Report | ConvertFrom-Json
    if(-not $int8Verification.roundtrip_bitwise_identical -or $int8Verification.plaintext_sha256 -ne $compat.int8.sha256){throw 'INT8 encryption round-trip mismatch.'}
    $int8Report | Set-Content -LiteralPath (Join-Path $work 'int8-model-encryption.json') -Encoding utf8NoBOM
    Invoke-Native $toolchain.MSBuild @($protectionProject,'/t:Build','/p:Configuration=Release','/p:Platform=x64',"/p:DctsPrivateBuildDir=$int8Private",'/nologo','/v:minimal')
    Copy-Relative (Join-Path $int8Private 'bin/Release') (Join-Path $compatPayload 'bin') 'DctsModelRuntime.dll'
    $compatFiles[$int8Cipher]=Digest (Join-Path $compatPayload $int8Cipher)
    $compatFiles['bin/DctsModelRuntime.dll']=Digest (Join-Path $compatPayload 'bin/DctsModelRuntime.dll')
    $compat.files=$compatFiles
    $compat.int8.model_protection=@{scheme='aes256-gcm-memory-v1';loader_abi=1;ciphertext=$int8Cipher;loader='bin/DctsModelRuntime.dll';plaintext_sha256=$compat.int8.sha256}
    $compat.int8.Remove('file')
    Write-Json (Join-Path $compatPayload 'runtime.json') $compat
    if((Digest $int8Source) -ne $compat.int8.sha256 -or (Get-Item -LiteralPath $int8Source).LastWriteTimeUtc -ne $int8Write){throw 'INT8 source model changed.'}
    Write-Json (Join-Path $payload 'runtime-paths.json') @{
        RV_ASSIST_RGB672_VGA_FP16_ROOT='UniDepthRuntimeRgb672VgaFP16RTX'
        RV_ASSIST_COMPAT_ROOT='UniDepthRuntimeCompatible'
        RV_CONTROL_CPP='rv_control/RvControlWorker.exe'
    }

    $notices=Join-Path $payload 'licenses'
    Copy-Relative (Join-Path $project 'third_party/ffmpeg_7_1_3') $notices 'COPYING.LGPLv2.1'
    Copy-Relative $PSScriptRoot $notices 'Third-Party-Notices.txt'
    $qtSource=Join-Path (Split-Path $qt) 'Src'
    Copy-Relative (Join-Path $qtSource 'qtbase/LICENSES') $notices 'LGPL-3.0-only.txt'
    Copy-Relative (Join-Path $qtSource 'qtquick3d/LICENSES') $notices 'GPL-3.0-only.txt'
    $sbom=Join-Path $qt 'sbom'
    foreach($module in 'qtbase','qtdeclarative','qtmultimedia','qtquick3d','qtshadertools','qtserialport','qtsvg','qtimageformats','qtlottie'){
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

$stagedModel=Join-Path $payload 'UniDepthRuntimeRgb672VgaFP16RTX'
$stagedManifest=Get-Content -LiteralPath (Join-Path $stagedModel 'runtime.json') -Raw | ConvertFrom-Json -AsHashtable
if($stagedManifest.backend -ne 'onnxruntime-tensorrt-rtx' -or $stagedManifest.ort_version -ne '1.31.1' -or
   $stagedManifest.model_protection.scheme -ne 'aes256-gcm-memory-v1' -or
   -not (Test-Path -LiteralPath (Join-Path $stagedModel 'model.dctsm') -PathType Leaf)){
    throw 'Expected the encrypted RGB672/VGA TensorRT-RTX runtime payload.'
}
if($stagedManifest.model_protection.plaintext_sha256 -ne '50d22cd33e126832793d905788a1efadeb4c9298de72cbfa634d49fdcdbc2dc1'){
    throw 'Unexpected protected VGA model identity.'
}
foreach($name in $stagedManifest.files.Keys){
    if((Digest (Join-Path $stagedModel $name)) -ne $stagedManifest.files[$name]){throw "Staged model fingerprint mismatch: $name"}
}
$stagedCompatRoot=Join-Path $payload 'UniDepthRuntimeCompatible'
$stagedCompat=Get-Content -LiteralPath (Join-Path $stagedCompatRoot 'runtime.json') -Raw | ConvertFrom-Json -AsHashtable
if($stagedCompat.int8.model_protection.scheme -ne 'aes256-gcm-memory-v1' -or
   $stagedCompat.int8.model_protection.plaintext_sha256 -ne '7dc50150c210b75e72fac35b45967ef874d85e9a883040223dbd5ebd4e01d63d'){
    throw 'Expected protected compatible inference runtime.'
}
foreach($name in $stagedCompat.files.Keys){
    if((Digest (Join-Path $stagedCompatRoot $name)) -ne $stagedCompat.files[$name]){throw "Staged compatible runtime fingerprint mismatch: $name"}
}
$unexpected=Get-ChildItem -LiteralPath $payload -File -Recurse | Where-Object {
    $_.Name -match '^(README|AGENTS)(\..*)?$|^PackModelCopy\.exe$|model_key_material' -or
    $_.Name -match '^(onnxruntime_providers_cuda|cudnn.*|cublas.*|cufft.*|nvrtc.*|zlibwapi|RvAssistTensorRtRtx)\.(dll|exe)$' -or
    $_.Extension -match '^\.(onnx|ovm|rknn|pt|pth|pdb|ilk|lib|exp|vcxproj|py)$'
}
if($unexpected){throw "Unexpected payload content: $($unexpected.FullName -join ', ')"}
if(-not $UseStagedPayload){
    $desktopHashes=@{}
    foreach($name in $desktopFiles){$desktopHashes[$name]=Digest (Join-Path $payload $name)}
    Write-Json $desktopReceipt @{imaging_schemes=$imagingSchemes;files=$desktopHashes}
}
if($StageOnly){return}
if(-not $Iscc -or -not $ChineseMessages){throw 'Provide -Iscc and -ChineseMessages for installer compilation.'}
$out=Join-Path $work 'delivery'
New-Item -ItemType Directory -Force $out | Out-Null
$icon=(Resolve-Path -LiteralPath (Join-Path $project 'assets/cx.ico')).Path
Invoke-Native (Resolve-Path -LiteralPath $Iscc).Path @(
    "/DVersion=$Version","/DOutput=$out","/DPayload=$payload","/DIconPath=$icon",
    "/DChineseMessages=$((Resolve-Path -LiteralPath $ChineseMessages).Path)",(Join-Path $PSScriptRoot 'Dcts.iss'))
$installer=Join-Path $out "触想工作台-内测版-v$Version-x64.exe"
"$(Digest $installer) *$(Split-Path $installer -Leaf)" | Set-Content -LiteralPath "$installer.sha256" -Encoding utf8NoBOM
Get-Item -LiteralPath $installer
