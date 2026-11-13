param(
    [Parameter(Mandatory)][string]$BinaryDirectory,
    [Parameter(Mandatory)][string]$RuntimePayload,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$binaries = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$runtime = (Resolve-Path -LiteralPath $RuntimePayload).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Choose a new SDK output directory.' }

$sdkBinaries = @('DctsDepthSdk.dll', 'DctsInferenceWorker.exe', 'DctsDepthSdkExample.exe')
$runtimeDirectories = @('rv_control', 'vlc', 'platform-tools', 'UniDepthRuntimeRgb672VgaFP16RTX', 'UniDepthRuntimeCompatible')
$runtimeManifests = @('rv_control/implementation.json', 'UniDepthRuntimeRgb672VgaFP16RTX/runtime.json', 'UniDepthRuntimeCompatible/runtime.json')
$qtLibraries = @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'Qt6Multimedia.dll')
$codecLibraries = @('avcodec-61.dll', 'avformat-61.dll', 'avutil-59.dll', 'swresample-5.dll', 'swscale-8.dll')
foreach ($name in $sdkBinaries + @('DctsDepthSdk.lib')) {
    if (-not (Test-Path -LiteralPath (Join-Path $binaries $name) -PathType Leaf)) { throw "Missing Release SDK build output $name" }
}
foreach ($name in $runtimeManifests + $qtLibraries + $codecLibraries + @('vcruntime140.dll', 'msvcp140.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtime $name) -PathType Leaf)) { throw "Missing staged runtime file $name" }
}
foreach ($name in $runtimeDirectories + @('multimedia', 'licenses')) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtime $name) -PathType Container)) { throw "Missing staged runtime directory $name" }
}

$bin = Join-Path $output 'bin'
$include = Join-Path $output 'include'
$lib = Join-Path $output 'lib'
$examples = Join-Path $output 'examples'
New-Item -ItemType Directory -Path $bin, $include, $lib, $examples | Out-Null
foreach ($name in $sdkBinaries) { Copy-Item -LiteralPath (Join-Path $binaries $name) -Destination $bin }
Copy-Item -LiteralPath (Join-Path $binaries 'DctsDepthSdk.lib') -Destination $lib
Copy-Item -LiteralPath (Join-Path $project 'sdk/DepthSdk.h') -Destination $include
Copy-Item -LiteralPath (Join-Path $project 'sdk/DepthSdkExample.c') -Destination $examples
Copy-Item -LiteralPath (Join-Path $project 'docs/DEPTH_SDK_WINDOWS.md') -Destination (Join-Path $output 'README.md')
Copy-Item -LiteralPath (Join-Path $project 'LICENSE') -Destination $output
Copy-Item -LiteralPath (Join-Path $runtime 'licenses') -Destination (Join-Path $output 'licenses') -Recurse
foreach ($name in $qtLibraries + $codecLibraries) { Copy-Item -LiteralPath (Join-Path $runtime $name) -Destination $bin }
foreach ($pattern in 'msvcp140*.dll', 'vcruntime140*.dll', 'concrt140.dll', 'Qt6Concurrent.dll') {
    Get-ChildItem -LiteralPath $runtime -Filter $pattern -File | Copy-Item -Destination $bin
}
foreach ($name in $runtimeDirectories + @('multimedia', 'networkinformation', 'tls')) {
    $source = Join-Path $runtime $name
    if (Test-Path -LiteralPath $source -PathType Container) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $bin $name) -Recurse
    }
}
@{
    RV_CONTROL_CPP = 'rv_control/RvControlWorker.exe'
    RV_ASSIST_RGB672_VGA_FP16_ROOT = 'UniDepthRuntimeRgb672VgaFP16RTX'
    RV_ASSIST_COMPAT_ROOT = 'UniDepthRuntimeCompatible'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $bin 'runtime-paths.json') -Encoding utf8

$hashes = [ordered]@{}
$sdkFiles = @('include/DepthSdk.h', 'lib/DctsDepthSdk.lib', 'bin/runtime-paths.json') +
    (($sdkBinaries + $runtimeManifests) | ForEach-Object { "bin/$_" })
foreach ($name in $sdkFiles) {
    $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $output $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
[ordered]@{ abi_version = 1; platform = 'windows-x64'; sha256 = $hashes } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'sdk-manifest.json') -Encoding utf8
Write-Output $output
