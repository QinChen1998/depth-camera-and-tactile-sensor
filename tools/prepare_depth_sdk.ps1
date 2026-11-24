param(
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [string]$Archive = $env:DCTS_DEPTH_SDK_ZIP,
    [string]$DeployDirectory = ''
)

$ErrorActionPreference = 'Stop'
$package = 'CXWLDepthSDK-2.2.2-win64'
$archiveHash = '306f30cefdcf0f289574d7a1361fa7bc041fd8d8ebba0148c5570dd6eed5ccab'
$manifestHash = 'dae235c981fa63bc33b80c07dc8912dc945534ac2ea0af46dc97cb1265ac53ec'
$manifestPath = Join-Path $SdkRoot 'sdk-manifest.json'

if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    if (!$Archive) { $Archive = "F:/Work/CXWL Depth Camera SDK/release/$package.zip" }
    if (!(Test-Path -LiteralPath $Archive -PathType Leaf)) {
        throw "Set DCTS_DEPTH_SDK_ZIP to the Windows $package.zip release package."
    }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $archiveHash) {
        throw "The Windows depth SDK ZIP does not match the pinned $package release."
    }
    if ((Split-Path $SdkRoot -Leaf) -ne $package) {
        throw "An unpacked SDK root must contain sdk-manifest.json. An automatic extraction root must end in $package."
    }
    $SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
    $parent = Split-Path $SdkRoot -Parent
    $temporary = Join-Path $parent ('.extract-' + [guid]::NewGuid().ToString('N'))
    try {
        Expand-Archive -LiteralPath $Archive -DestinationPath $temporary
        try {
            [IO.Directory]::Move((Join-Path $temporary $package), $SdkRoot)
        } catch [IO.IOException] {
            if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw }
        }
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            $staged = Get-Item -LiteralPath $temporary -Force
            if ($staged.FullName -ne $temporary -or ($staged.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Unexpected SDK extraction directory.'
            }
            Remove-Item -LiteralPath $staged.FullName -Recurse -Force
        }
    }
}
if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ne $manifestHash) {
    throw 'The unpacked Windows depth SDK manifest does not match version 2.2.2.'
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$appFiles = @('CxwlDepthSdk.dll', 'avcodec-61.dll', 'avformat-61.dll', 'avutil-59.dll', 'swresample-5.dll', 'swscale-8.dll')
$sdkFiles = @('SDK/include/DepthSdk.h', 'SDK/include/DepthSdkVersion.h', 'SDK/lib/CxwlDepthSdk.lib')
$sdkFiles += $appFiles | ForEach-Object { "SDK/bin/$_" }
if ($DeployDirectory) {
    $sdkFiles += $manifest.sha256.PSObject.Properties.Name | Where-Object {
        $_ -match '^(SDK/bin/(rv_control/|platform-tools/|runtime-paths\.json$)|Licenses/|LICENSE$)'
    }
}

foreach ($relative in $sdkFiles) {
    $source = Join-Path $SdkRoot $relative
    if (!(Test-Path -LiteralPath $source -PathType Leaf) -or
        (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $manifest.sha256.$relative) {
        throw "SDK input is missing or has changed: $relative"
    }
}

if ($DeployDirectory) {
    foreach ($relative in $sdkFiles) {
        $target = if ($appFiles -contains (Split-Path $relative -Leaf)) {
            Join-Path $DeployDirectory (Split-Path $relative -Leaf)
        } elseif ($relative.StartsWith('SDK/bin/')) {
            Join-Path $DeployDirectory ('cxwl-depth-sdk/' + $relative.Substring('SDK/bin/'.Length))
        } elseif ($relative.StartsWith('Licenses/') -or $relative -eq 'LICENSE') {
            Join-Path $DeployDirectory ('cxwl-depth-sdk/' + $relative)
        } else { continue }
        New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
        Copy-Item -LiteralPath (Join-Path $SdkRoot $relative) -Destination $target -Force
    }
}
