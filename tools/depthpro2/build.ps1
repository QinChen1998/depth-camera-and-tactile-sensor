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
    [string]$OutputDirectory=''
)

$ErrorActionPreference='Stop'
$projectRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $projectRoot 'tools/resolve_vs2022_toolchain.ps1')
$MSBuild=(Resolve-Vs2022Toolchain -MSBuild $MSBuild -RequireMSBuild).MSBuild
$QtRoot=Resolve-Qt611Root -QtRoot $QtRoot -MSBuild $MSBuild
$engineName='depthpro2-s-480x640-fp16.engine'
if(!$OutputDirectory){$OutputDirectory=Join-Path $projectRoot 'DepthPro2Runtime'}

$buildArgs=@((Join-Path $PSScriptRoot 'DepthPro2Worker.vcxproj'),'/t:Build','/p:Configuration=Release','/p:Platform=x64',
    "/p:DepthPro2QtRoot=$QtRoot","/p:DepthPro2TrtSdk=$TensorRtSdk","/p:DepthPro2CudaRoot=$CudaRoot",
    "/p:DepthPro2CudaCrtInclude=$CudaCrtInclude",'/nologo','/v:minimal')
& $MSBuild @buildArgs
if($LASTEXITCODE){throw "Depth Pro 2-$Variant worker build failed"}

$inputs=[ordered]@{
    'DctsDepthPro2Worker.exe'=(Join-Path $projectRoot 'x64/depthpro2_worker/DctsDepthPro2Worker.exe')
    $engineName=$EnginePath
    'Apple-Model-LICENSE.txt'=$AppleLicense
    'Qt6Core.dll'=(Join-Path $QtRoot 'bin/Qt6Core.dll')
    'cudart64_12.dll'=(Join-Path $CudaRoot 'bin/cudart64_12.dll')
    'nvinfer_10.dll'=(Join-Path $TensorRtLibraries 'nvinfer_10.dll')
    'nvinfer_plugin_10.dll'=(Join-Path $TensorRtLibraries 'nvinfer_plugin_10.dll')
}
$files=[ordered]@{}
foreach($name in $inputs.Keys){
    $files[$name]=(Get-FileHash -LiteralPath $inputs[$name] -Algorithm SHA256).Hash.ToLowerInvariant()
}
$engineHash=$files[$engineName]
$profile=Get-Content -LiteralPath (Join-Path $projectRoot 'sensors/RvCameraView.h') -Raw
if($profile -notmatch 'DepthPro2EngineSha="([a-f0-9]{64})"' -or $engineHash -ne $Matches[1]){
    throw "The engine does not match the selected local Depth Pro 2-$Variant profile"
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
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'runtime.json'),($manifest | ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
Write-Output "Depth Pro 2-$Variant research runtime: $OutputDirectory"
