param(
    [Parameter(Mandatory=$true)][string]$TensorRtSdk,
    [Parameter(Mandatory=$true)][string]$TensorRtLibraries,
    [Parameter(Mandatory=$true)][string]$CudaRoot,
    [Parameter(Mandatory=$true)][string]$CudaCrtInclude,
    [Parameter(Mandatory=$true)][string]$EnginePath,
    [Parameter(Mandatory=$true)][string]$AppleLicense,
    [ValidateSet('S')][string]$Variant='S',
    [string]$QtRoot='',
    [string]$MSBuild='',
    [string]$OutputDirectory='',
    [switch]$ProtectForResearch,
    [string]$TensorRtLicense='',
    [string]$CudaLicense=''
)

$ErrorActionPreference='Stop'
$projectRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $projectRoot 'tools/resolve_vs2022_toolchain.ps1')
$MSBuild=(Resolve-Vs2022Toolchain -MSBuild $MSBuild -RequireMSBuild).MSBuild
$QtRoot=Resolve-Qt611Root -QtRoot $QtRoot -MSBuild $MSBuild
$engineName='depthpro2-s-480x640-fp16.engine'
$engineHash=(Get-FileHash -LiteralPath $EnginePath -Algorithm SHA256).Hash.ToLowerInvariant()
$profile=Get-Content -LiteralPath (Join-Path $projectRoot 'sensors/RvCameraView.h') -Raw
if($profile -notmatch 'DepthPro2EngineSha="([a-f0-9]{64})"' -or $engineHash -ne $Matches[1]){
    throw "The engine does not match the selected local Depth Pro 2-$Variant profile"
}
if($ProtectForResearch -and !$OutputDirectory){throw 'Select a fresh research runtime directory under x64/validation.'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $projectRoot 'DepthPro2Runtime'}

$buildArgs=@((Join-Path $PSScriptRoot 'DepthPro2Worker.vcxproj'),'/t:Build','/p:Configuration=Release','/p:Platform=x64',
    "/p:DepthPro2QtRoot=$QtRoot","/p:DepthPro2TrtSdk=$TensorRtSdk","/p:DepthPro2CudaRoot=$CudaRoot",
    "/p:DepthPro2CudaCrtInclude=$CudaCrtInclude",'/nologo','/v:minimal')
$inputs=[ordered]@{
    'DctsDepthPro2Worker.exe'=(Join-Path $projectRoot 'x64/depthpro2_worker/DctsDepthPro2Worker.exe')
    $engineName=$EnginePath
    'Apple-Model-LICENSE.txt'=$AppleLicense
    'Qt6Core.dll'=(Join-Path $QtRoot 'bin/Qt6Core.dll')
    'cudart64_12.dll'=(Join-Path $CudaRoot 'bin/cudart64_12.dll')
    'nvinfer_10.dll'=(Join-Path $TensorRtLibraries 'nvinfer_10.dll')
    'nvinfer_plugin_10.dll'=(Join-Path $TensorRtLibraries 'nvinfer_plugin_10.dll')
}

if($ProtectForResearch){
    $OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
    $validationRoot=Join-Path $projectRoot 'x64\validation'
    if(!$OutputDirectory.StartsWith($validationRoot+'\',[StringComparison]::OrdinalIgnoreCase) -or
       (Test-Path -LiteralPath $OutputDirectory)){
        throw 'Select a fresh research runtime directory under x64/validation.'
    }
    if(!$TensorRtLicense -or !$CudaLicense){throw 'Provide the license files accompanying the TensorRT and CUDA libraries.'}
    $inputs['NVIDIA-TensorRT-LICENSE.txt']=(Resolve-Path -LiteralPath $TensorRtLicense).Path
    $inputs['NVIDIA-CUDA-LICENSE.txt']=(Resolve-Path -LiteralPath $CudaLicense).Path

    $privateBuild=Join-Path (Split-Path $OutputDirectory -Parent) 'private_build'
    if(Test-Path -LiteralPath $privateBuild){throw 'Use a fresh build directory for each protected runtime.'}
    New-Item -ItemType Directory -Path $privateBuild | Out-Null
    $protectionProject=Join-Path $projectRoot 'tools/model_protection/ModelProtection.vcxproj'
    $protectionArgs=@($protectionProject,'/t:Build','/p:Configuration=Release','/p:Platform=x64',
        "/p:DctsPrivateBuildDir=$privateBuild",'/nologo','/v:minimal')
    & $MSBuild @protectionArgs '/p:PackModelCopy=true'
    if($LASTEXITCODE){throw 'Model packer build failed'}

    $protectionBin=Join-Path $privateBuild 'bin/Release'
    $copiedEngine=Join-Path $privateBuild $engineName
    $encryptedName='depthpro2-s-480x640-fp16.dctsm'
    $encryptedEngine=Join-Path $privateBuild $encryptedName
    $keyHeader=Join-Path $privateBuild 'model_key_material.h'
    try {
        Copy-Item -LiteralPath $EnginePath -Destination $copiedEngine
        if((Get-FileHash -LiteralPath $copiedEngine -Algorithm SHA256).Hash -ine $engineHash){throw 'Engine copy fingerprint mismatch'}
        & (Join-Path $protectionBin 'PackModelCopy.exe') $copiedEngine $encryptedEngine $keyHeader
        if($LASTEXITCODE){throw 'Engine encryption failed'}
    } finally {
        if(Test-Path -LiteralPath $copiedEngine -PathType Leaf){Remove-Item -LiteralPath $copiedEngine}
    }
    & $MSBuild @protectionArgs "/p:DctsTensorRtSdk=$TensorRtSdk" "/p:DepthPro2CudaRoot=$CudaRoot" "/p:DepthPro2CudaCrtInclude=$CudaCrtInclude"
    if($LASTEXITCODE){throw 'Protected TensorRT loader build failed'}

    $workerOutput=Join-Path $privateBuild 'worker'
    $buildArgs+=@("/p:DepthPro2ProtectionDir=$protectionBin", "/p:OutDir=$workerOutput/", "/p:IntDir=$privateBuild/obj_worker/")
    $inputs['DctsDepthPro2Worker.exe']=Join-Path $workerOutput 'DctsDepthPro2Worker.exe'
    $inputs.Remove($engineName)
    $inputs[$encryptedName]=$encryptedEngine
    $inputs['DctsDepthPro2Runtime.dll']=Join-Path $protectionBin 'DctsDepthPro2Runtime.dll'
    $inputs['Research-Model-NOTICE.txt']=Join-Path $PSScriptRoot 'Research-Model-NOTICE.txt'
}

& $MSBuild @buildArgs
if($LASTEXITCODE){throw "Depth Pro 2-$Variant worker build failed"}

$files=[ordered]@{}
foreach($name in $inputs.Keys){
    $files[$name]=(Get-FileHash -LiteralPath $inputs[$name] -Algorithm SHA256).Hash.ToLowerInvariant()
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
foreach($name in $inputs.Keys){
    Copy-Item -LiteralPath $inputs[$name] -Destination (Join-Path $OutputDirectory $name)
}
$manifest=[ordered]@{
    schema=1
    backend='native-tensorrt-fp16'
    tensorrt_version='10.16.1.11'
    engine_sha256=$engineHash
    files=$files
}
if($ProtectForResearch){$manifest.model_protection='aes-256-gcm'}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'runtime.json'),($manifest | ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
Write-Output "Depth Pro 2-$Variant research runtime: $OutputDirectory"
