param(
    [ValidateSet('Release','Debug')][string]$Configuration='Release',
    [ValidateSet('Normal','Customer')][string]$Edition='Normal',
    [string]$QtRoot='',
    [string]$MSBuild=''
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'resolve_vs2022_toolchain.ps1')
$MSBuild=(Resolve-Vs2022Toolchain -MSBuild $MSBuild -RequireMSBuild).MSBuild
$QtRoot=Resolve-Qt611Root -QtRoot $QtRoot -MSBuild $MSBuild
$editionDirectory=if($Edition -eq 'Customer'){'x64/Customer'}else{'x64'}
$out=Join-Path $projectRoot "$editionDirectory/$Configuration"
New-Item -ItemType Directory -Force $out | Out-Null
& (Join-Path $QtRoot 'bin/lrelease.exe') (Join-Path $projectRoot 'translations/hand_zh_CN.ts') -qm (Join-Path $projectRoot 'translations/hand_zh_CN.qm')
if($LASTEXITCODE){throw 'lrelease failed'}
& $MSBuild (Join-Path $projectRoot 'Depth Camera and Tactile Sensor.sln') /t:Rebuild "/p:Configuration=$Configuration" /p:Platform=x64 "/p:DctsEdition=$Edition" "/p:QtInstall=$QtRoot" /m:2 /nologo /v:minimal *> (Join-Path $out 'build.log')
if($LASTEXITCODE){Get-Content (Join-Path $out 'build.log') -Tail 45;throw 'Desktop rebuild failed'}
& (Join-Path $QtRoot 'bin/windeployqt.exe') --release --no-translations --compiler-runtime (Join-Path $out '触想工作台.exe') *> (Join-Path $out 'deploy.log')
if($LASTEXITCODE){Get-Content (Join-Path $out 'deploy.log') -Tail 25;throw 'Qt runtime deployment failed'}
Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Name -match '^Qt6(Quick|Qml|LabsQml).*\.dll$' } |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName }
$oldImports=Join-Path $out 'qml'
if (Test-Path -LiteralPath $oldImports) {
    $directory=Get-Item -LiteralPath $oldImports -Force
    if ($directory.FullName -ne [IO.Path]::GetFullPath($oldImports) -or
        ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unexpected old QML deployment path' }
    Remove-Item -LiteralPath $directory.FullName -Recurse -Force
}
Write-Output "$Configuration rebuilt: $out"
