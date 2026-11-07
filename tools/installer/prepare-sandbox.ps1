param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$RunName='',
    [switch]$SmokeOnly
)
$ErrorActionPreference='Stop'
$work=(Resolve-Path -LiteralPath $BuildDirectory).Path
$suffix=if($RunName){"-$RunName"}else{''}
$inputFolder=Join-Path $work "sandbox-input$suffix"
$results=Join-Path $work "sandbox-results$suffix"
if((Test-Path -LiteralPath $inputFolder) -or (Test-Path -LiteralPath $results)){
    throw 'Sandbox evidence directories already exist. Preserve them before generating a new run.'
}
$installer=@(Get-ChildItem -LiteralPath (Join-Path $work 'delivery') -Filter '触想工作台-内测版-v*-x64.exe' -File)
if($installer.Count -ne 1){throw 'Expected a single setup EXE.'}
New-Item -ItemType Directory -Path $inputFolder,$results | Out-Null
Copy-Item -LiteralPath $installer[0].FullName -Destination $inputFolder
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'sandbox-test.ps1') -Destination $inputFolder
$inputFolder=[Security.SecurityElement]::Escape($inputFolder)
$results=[Security.SecurityElement]::Escape($results)
$testArguments=if($SmokeOnly){' -SmokeOnly'}else{''}
@"
<Configuration>
  <vGPU>Disable</vGPU>
  <Networking>Disable</Networking>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <MappedFolders>
    <MappedFolder><HostFolder>$inputFolder</HostFolder><SandboxFolder>C:\DctsInstaller</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$results</HostFolder><SandboxFolder>C:\DctsResults</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\DctsInstaller\sandbox-test.ps1$testArguments</Command></LogonCommand>
</Configuration>
"@ | Set-Content -LiteralPath (Join-Path $work "DctsCleanInstall$suffix.wsb") -Encoding utf8
