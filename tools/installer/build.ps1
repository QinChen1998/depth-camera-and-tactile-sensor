param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [ValidateSet('Normal','Customer')][string]$Edition='Normal',
    [string]$QtRoot='',
    [string]$MSBuild='',
    [string]$Version='',
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
$Edition=if($Edition -eq 'Customer'){'Customer'}else{'Normal'}
$payload=Join-Path $work 'payload'
$exe='触想工作台.exe'
$product=Get-Content -LiteralPath (Join-Path $project 'DCTS_APP.json') -Raw | ConvertFrom-Json -AsHashtable
if(!$Version){$Version=$product.version}
if($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$'){throw 'Invalid installer version.'}
$profile=if($Edition -eq 'Customer'){$product.customer}else{$product}
$imagingSchemes=$profile.imaging_schemes
if(!$imagingSchemes -or $profile.measurement_capture -isnot [bool]){throw 'Invalid desktop edition settings.'}
if($Edition -eq 'Customer' -and @($imagingSchemes | Where-Object {$_ -cnotlike 'board_native_*'}).Count){
    throw 'The customer edition requires native board imaging schemes.'
}
$editionLabel=if($Edition -eq 'Customer'){'客户版'}else{'内测版'}
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

function Get-SourceSnapshot {
    if(!(Test-Path -LiteralPath (Join-Path $project '.git'))){return @{commit=$null; fingerprint=$null; dirty=$null}}
    $commit=& git -C $project rev-parse --verify HEAD
    if($LASTEXITCODE){throw 'Cannot identify the source commit.'}
    $tracked=@(& git -C $project -c core.quotepath=false diff --name-only --no-renames HEAD --)
    if($LASTEXITCODE){throw 'Cannot identify modified source files.'}
    $untracked=@(& git -C $project -c core.quotepath=false ls-files --others --exclude-standard)
    if($LASTEXITCODE){throw 'Cannot identify new source files.'}
    $paths=@(($tracked+$untracked) | Sort-Object -CaseSensitive -Unique)
    $files=foreach($relative in $paths){
        $path=Join-Path $project $relative
        $hash=if(Test-Path -LiteralPath $path -PathType Leaf){Digest $path}else{'deleted'}
        "$relative $hash"
    }
    $bytes=[Text.Encoding]::UTF8.GetBytes((@($commit)+@($files)) -join "`n")
    $fingerprint=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    return @{commit=$commit; fingerprint=$fingerprint; dirty=($paths.Count -gt 0)}
}

function Assert-EditionInfo($Info){
    if($Info.edition -cne $Edition -or ($Info.imaging_schemes -join ',') -cne ($imagingSchemes -join ',') -or
        $Info.version -cne $product.version -or $Info.sdk_version -cne '3.1.0' -or $Info.sdk_abi -ne 3 -or
        $Info.default_imaging_scheme -cne $imagingSchemes[0] -or $Info.measurement_capture -isnot [bool] -or
        $Info.measurement_capture -cne $profile.measurement_capture){
        throw 'The desktop edition or its compiled features do not match the requested installer.'
    }
}

function Get-DesktopFiles {
    $sdk=Join-Path $payload 'cxwl-depth-sdk'
    $manifest=Join-Path $sdk 'sdk-manifest.json'
    if(!(Test-Path -LiteralPath $manifest -PathType Leaf) -or
        (Digest $manifest) -cne 'a88713c5063b1083d9d9ce88dd525b2f8c5f66a69960f5a7e9708a5d2feca6ae'){
        throw 'The staged SDK must match the published Windows SDK 3.1.0 manifest.'
    }
    $hashes=(Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json -AsHashtable).sha256
    $files=@($exe,'cxwl-depth-sdk/sdk-manifest.json')
    foreach($entry in $hashes.GetEnumerator()){
        $path=$entry.Key
        $installed = switch -Regex ($path) {
            '^SDK/bin/CxwlDepthSdk\.dll$' { 'CxwlDepthSdk.dll'; break }
            '^SDK/bin/(avcodec-61|avformat-61|avutil-59|swresample-5|swscale-8)\.dll$' {
                'cxwl-depth-sdk/'+(Split-Path $path -Leaf); break
            }
            '^SDK/bin/(networkinformation|tls)/' { $path.Substring('SDK/bin/'.Length); break }
            '^(LICENSE$|Licenses/)' { 'cxwl-depth-sdk/'+$path; break }
        }
        if(!$installed){continue}
        $file=Join-Path $payload $installed
        if(!(Test-Path -LiteralPath $file -PathType Leaf) -or (Digest $file) -cne $entry.Value){
            throw "The staged runtime does not match the Windows SDK 3.1.0 file: $installed"
        }
        $files+=$installed
    }
    return $files
}

function Assert-DesktopFiles($Files){
    $expected=@(Get-DesktopFiles)
    if(!$Files -or $Files.Count -ne $expected.Count){throw 'The staged desktop file list changed.'}
    foreach($name in $expected){
        $file=Join-Path $payload $name
        if(!(Test-Path -LiteralPath $file -PathType Leaf) -or (Digest $file) -cne $Files[$name]){
            throw "Staged desktop fingerprint mismatch: $name"
        }
    }
}

if($UseStagedPayload){
    if(-not (Test-Path -LiteralPath $desktopReceipt -PathType Leaf)){
        throw 'Stage the desktop from current source before reusing this payload.'
    }
    $receipt=Get-Content -LiteralPath $desktopReceipt -Raw | ConvertFrom-Json -AsHashtable
    Assert-EditionInfo $receipt
    $source=Get-SourceSnapshot
    if(!$receipt.source -or $receipt.source.commit -cne $source.commit -or
        $receipt.source.fingerprint -cne $source.fingerprint){
        throw 'The staged desktop was built from different sources. Stage it again from current source.'
    }
    Assert-DesktopFiles $receipt.files
}

if(-not $UseStagedPayload){
    if(Test-Path -LiteralPath $payload){throw 'The payload already exists. Choose a fresh build directory.'}
    $toolchain=Resolve-Vs2022Toolchain -MSBuild $MSBuild -RequireMSBuild -RequireRedist
    $qt=Resolve-Qt611Root -QtRoot $QtRoot -MSBuild $toolchain.MSBuild
    $desktop=Join-Path $work 'desktop'
    New-Item -ItemType Directory -Force -Path $desktop | Out-Null

    Invoke-Native (Join-Path $qt 'bin/lrelease.exe') @(
        (Join-Path $project 'translations/hand_zh_CN.ts'),'-qm',(Join-Path $project 'translations/hand_zh_CN.qm'))
    $source=Get-SourceSnapshot
    Invoke-Native $toolchain.MSBuild @(
        (Join-Path $project 'Depth Camera and Tactile Sensor.sln'),'/t:Build','/p:Configuration=Release','/p:Platform=x64',
        "/p:QtInstall=$qt","/p:DctsEdition=$Edition",'/p:DctsInstallerBuild=true',
        "/p:OutDir=$desktop/","/p:IntDir=$work/obj/",
        '/m:2','/nologo','/v:minimal',"/flp:logfile=$work/build.log;verbosity=normal")

    New-Item -ItemType Directory -Path $payload | Out-Null
    Copy-Item -LiteralPath (Join-Path $desktop $exe) -Destination $payload
    Invoke-Native (Join-Path $qt 'bin/windeployqt.exe') @(
        '--release','--no-translations','--no-compiler-runtime',(Join-Path $payload $exe))
    Copy-Relative $desktop $payload 'CxwlDepthSdk.dll'
    Copy-Item -LiteralPath (Join-Path $desktop 'cxwl-depth-sdk') -Destination $payload -Recurse
    foreach($pluginDirectory in 'networkinformation','tls'){
        foreach($plugin in Get-ChildItem -LiteralPath (Join-Path $desktop $pluginDirectory) -File){
            Copy-Relative $desktop $payload "$pluginDirectory/$($plugin.Name)"
        }
    }
    Get-ChildItem -LiteralPath $toolchain.RedistDirectories[0] -Filter '*.dll' | Copy-Item -Destination $payload
    foreach($name in 'avcodec-61.dll','avformat-61.dll','avutil-59.dll','swresample-5.dll','swscale-8.dll'){
        if(-not (Test-Path -LiteralPath (Join-Path $payload $name) -PathType Leaf)){throw "Missing Qt FFmpeg library $name"}
    }
    $editionReport=Join-Path $work 'compiled-edition.json'
    $infoProcess=Start-Process -FilePath (Join-Path $payload $exe) -WorkingDirectory $payload -WindowStyle Hidden -Wait -PassThru `
        -ArgumentList ('--edition-info "{0}"' -f $editionReport)
    if($infoProcess.ExitCode -ne 0){throw "Cannot read the compiled desktop edition ($($infoProcess.ExitCode))."}
    $compiled=Get-Content -LiteralPath $editionReport -Raw | ConvertFrom-Json -AsHashtable
    Assert-EditionInfo $compiled

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

# The research model is prepared locally with tools/depthpro2/build.ps1.
$legacyInference = Get-ChildItem -LiteralPath $payload -Directory | Where-Object { $_.Name -match '^(UniDepthRuntime.*|DepthPro2Runtime)$' }
if ($legacyInference) { throw 'Build the installer from a fresh payload without local inference assets.' }
$unexpected=Get-ChildItem -LiteralPath $payload -File -Recurse | Where-Object {
    $_.Name -match '^(README|AGENTS)(\..*)?$|^(PackModelCopy|DctsInferenceWorker|DctsDepthPro2Worker|RvControlWorker|adb)\.exe$|^libssh2\.dll$|model_key_material' -or
    $_.Name -match '^(onnxruntime_providers_cuda|cudnn.*|cublas.*|cufft.*|nvrtc.*|zlibwapi|RvAssistTensorRtRtx)\.(dll|exe)$' -or
    $_.Name -match '^(DctsDepthPro2Runtime|nvinfer.*|cudart64_.*)\.dll$' -or
    $_.Extension -match '^\.(onnx|ovm|rknn|pt|pth|engine|safetensors|dctsm|pdb|ilk|lib|exp|vcxproj|py)$'
}
$retired=@(Get-ChildItem -LiteralPath $payload -Directory -Recurse | Where-Object { $_.Name -in 'rv_control','platform-tools','vlc' })
if($retired){throw 'An obsolete SSH, ADB or RTSP runtime was staged.'}
if($unexpected){throw "Unexpected payload content: $($unexpected.FullName -join ', ')"}
if(-not $UseStagedPayload){
    if((Get-SourceSnapshot).fingerprint -cne $source.fingerprint){throw 'The application sources changed during staging.'}
    $desktopHashes=@{}
    foreach($name in (Get-DesktopFiles)){$desktopHashes[$name]=Digest (Join-Path $payload $name)}
    $compiled.source=$source
    $compiled.files=$desktopHashes
    Write-Json $desktopReceipt $compiled
}
if($StageOnly){return}
if(-not $Iscc -or -not $ChineseMessages){throw 'Provide -Iscc and -ChineseMessages for installer compilation.'}
$out=Join-Path $work 'delivery'
New-Item -ItemType Directory -Force $out | Out-Null
$icon=(Resolve-Path -LiteralPath (Join-Path $project 'assets/cx.ico')).Path
Invoke-Native (Resolve-Path -LiteralPath $Iscc).Path @(
    "/DVersion=$Version","/DEdition=$Edition","/DOutput=$out","/DPayload=$payload","/DIconPath=$icon",
    "/DChineseMessages=$((Resolve-Path -LiteralPath $ChineseMessages).Path)",(Join-Path $PSScriptRoot 'Dcts.iss'))
$installer=Join-Path $out "触想工作台-$editionLabel-v$Version-x64.exe"
"$(Digest $installer) *$(Split-Path $installer -Leaf)" | Set-Content -LiteralPath "$installer.sha256" -Encoding utf8NoBOM
Get-Item -LiteralPath $installer
