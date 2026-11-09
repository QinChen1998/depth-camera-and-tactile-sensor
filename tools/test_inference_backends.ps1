param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Output,
    [switch]$Ui,
    [switch]$Protected
)
$ErrorActionPreference='Stop'
$program=(Resolve-Path -LiteralPath $Executable).Path
$out=[IO.Path]::GetFullPath($Output)
if(Test-Path -LiteralPath $out){throw 'Use a new inference validation output directory.'}
New-Item -ItemType Directory -Path $out,(Join-Path $out 'tmp') | Out-Null
$env:RV_TEST_OFFLINE_ONLY='1'
$env:TEMP=Join-Path $out 'tmp';$env:TMP=$env:TEMP
$env:PATH=(Split-Path $program)+';'+$env:SystemRoot+'\System32'
Remove-Item Env:RV_INFERENCE_BACKEND -ErrorAction SilentlyContinue
function Run([string[]]$Arguments,[int]$Timeout=900){
    $quoted=@($Arguments | ForEach-Object {'"'+$_+'"'})
    $process=Start-Process -FilePath $program -ArgumentList $quoted -WindowStyle Hidden -PassThru
    if(-not $process.WaitForExit($Timeout*1000)){$process.Kill();throw 'Native inference validation timed out.'}
    if($process.ExitCode -ne 0){throw "Native inference validation failed ($($process.ExitCode)): $Arguments"}
}
$report=Join-Path $out 'devices.json'
Run @('--inference-discovery-test',$report)
$devices=Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
if(-not $devices.pass -or $devices.checks -le 0){throw 'Discovery policy validation failed.'}
$results=@()
foreach($choice in $devices.available){
    $env:RV_INFERENCE_BACKEND=$choice.id
    $name=$choice.id -replace '[^A-Za-z0-9_-]','_'
    $path=Join-Path $out "$name-host-path.json"
    Run @('--rv-assist-path-test',$path) 300
    $result=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if(-not $result.pass -or $result.depth_bytes -ne 1228800){throw "Full host path failed: $($choice.id)"}
    if($Protected -and ($choice.probe.loading.plaintext_file_used -ne $false -or -not $choice.probe.loading.plaintext_sha256_verified)){
        throw "Protected loading contract failed: $($choice.id)"
    }
    $results+=@{device=$choice.id;fps=$choice.probe.fps;host_path=$result;loading=$choice.probe.loading}
    Write-Output ("PASS {0}: {1:N2} model FPS, full host path" -f $choice.id,$choice.probe.fps)
}
Remove-Item Env:RV_INFERENCE_BACKEND
if($Ui){Run @('--inference-ui-test',(Join-Path $out 'ui.json'))}
@{passed=$true;hardware_capture_started=$false;protected=$Protected.IsPresent;devices=$results;ui=$Ui.IsPresent} |
    ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding UTF8
