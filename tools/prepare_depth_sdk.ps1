param(
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [string]$SourceDirectory = $env:DCTS_DEPTH_SDK_DIR,
    [string]$DeployDirectory = ''
)

$ErrorActionPreference = 'Stop'
$package = 'Cxwl-Depth-Camera-SDK-v3.1.0-windows-x64'
$manifestHash = 'a88713c5063b1083d9d9ce88dd525b2f8c5f66a69960f5a7e9708a5d2feca6ae'
$SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
$manifestPath = Join-Path $SdkRoot 'sdk-manifest.json'
$prepareLocalCopy = !(Test-Path -LiteralPath $manifestPath -PathType Leaf)
$inputRoot = $SdkRoot
if ($prepareLocalCopy) {
    if (!$SourceDirectory) { throw "Set DCTS_DEPTH_SDK_DIR to the unpacked Windows $package package." }
    $inputRoot = [IO.Path]::GetFullPath($SourceDirectory)
    if (!(Test-Path -LiteralPath (Join-Path $inputRoot 'sdk-manifest.json') -PathType Leaf)) {
        throw "Set DCTS_DEPTH_SDK_DIR to the unpacked Windows $package package."
    }
    if ((Split-Path $SdkRoot -Leaf) -ne $package) {
        throw "A local SDK preparation root must end in $package."
    }
}
$inputManifest = Join-Path $inputRoot 'sdk-manifest.json'
if ((Get-FileHash -LiteralPath $inputManifest -Algorithm SHA256).Hash -ne $manifestHash) {
    throw "The unpacked Windows depth SDK manifest does not match $package."
}

$manifest = Get-Content -LiteralPath $inputManifest -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.sdk_version -ne '3.1.0' -or $manifest.abi_version -ne 3 -or $manifest.platform -ne 'windows-x64') {
    throw 'The SDK package is not Windows x64 3.1.0 / ABI v3.'
}
$ffmpegFiles = @('avcodec-61.dll', 'avformat-61.dll', 'avutil-59.dll', 'swresample-5.dll', 'swscale-8.dll')
$appFiles = @('CxwlDepthSdk.dll') + $ffmpegFiles
$sdkFiles = @('SDK/include/DepthSdk.h', 'SDK/include/DepthSdkVersion.h', 'SDK/lib/CxwlDepthSdk.lib')
$sdkFiles += $appFiles | ForEach-Object { "SDK/bin/$_" }
if ($prepareLocalCopy -or $DeployDirectory) {
    $sdkFiles += $manifest.sha256.PSObject.Properties.Name | Where-Object {
        $_ -match '^(SDK/bin/(networkinformation/|tls/)|Licenses/|LICENSE$)'
    }
}

foreach ($relative in $sdkFiles) {
    $source = Join-Path $inputRoot $relative
    if (!(Test-Path -LiteralPath $source -PathType Leaf) -or
        (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $manifest.sha256.$relative) {
        throw "SDK input is missing or has changed: $relative"
    }
}

if ($prepareLocalCopy) {
    $parent = Split-Path $SdkRoot -Parent
    $temporary = Join-Path $parent ('.stage-' + [guid]::NewGuid().ToString('N'))
    try {
        foreach ($relative in @('sdk-manifest.json') + $sdkFiles) {
            $target = Join-Path $temporary $relative
            New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
            Copy-Item -LiteralPath (Join-Path $inputRoot $relative) -Destination $target
        }
        try {
            [IO.Directory]::Move($temporary, $SdkRoot)
        } catch [IO.IOException] {
            if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
                (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ne $manifestHash) { throw }
        }
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            $staged = Get-Item -LiteralPath $temporary -Force
            if ($staged.FullName -ne $temporary -or
                !$staged.FullName.StartsWith($parent + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
                ($staged.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Unexpected SDK staging directory.'
            }
            Remove-Item -LiteralPath $staged.FullName -Recurse -Force
        }
    }
}

if ($DeployDirectory) {
    New-Item -ItemType Directory -Force -Path $DeployDirectory | Out-Null
    $sdkRuntime = Join-Path $DeployDirectory 'cxwl-depth-sdk'
    New-Item -ItemType Directory -Force -Path $sdkRuntime | Out-Null
    Copy-Item -LiteralPath $manifestPath -Destination $sdkRuntime -Force
    foreach ($relative in $sdkFiles) {
        $source = Join-Path $SdkRoot $relative
        $name = Split-Path $relative -Leaf
        if ($appFiles -contains $name) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $DeployDirectory $name) -Force
            if ($ffmpegFiles -notcontains $name) { continue }
        }
        $target = if ($relative -match '^SDK/bin/(networkinformation|tls)/') {
            Join-Path $DeployDirectory $relative.Substring('SDK/bin/'.Length)
        } elseif ($relative.StartsWith('SDK/bin/')) {
            # The SDK resolves FFmpeg relative to the directory passed to the API.
            Join-Path $sdkRuntime $relative.Substring('SDK/bin/'.Length)
        } elseif ($relative.StartsWith('Licenses/') -or $relative -eq 'LICENSE') {
            Join-Path $sdkRuntime $relative
        } else { continue }
        New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
}
