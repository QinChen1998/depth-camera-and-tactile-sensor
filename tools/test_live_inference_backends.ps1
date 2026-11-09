param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$ConfigSource,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][switch]$AllowLiveDevice,
    [string[]]$Backends=@('tensorrt','directml:0','openvino:CPU','openvino:INT8','cpu')
)
$ErrorActionPreference='Stop'
if(-not $AllowLiveDevice){throw 'Explicit real-device authorization is required.'}
$program=(Resolve-Path -LiteralPath $Executable).Path
$config=(Resolve-Path -LiteralPath $ConfigSource).Path
$out=[IO.Path]::GetFullPath($Output)
if(Test-Path -LiteralPath $out){throw 'Use a new live validation directory.'}
$registry=Get-Content -LiteralPath (Join-Path $config 'devices.json') -Raw | ConvertFrom-Json
$profiles=@($registry.devices | Where-Object {$_.id -eq $registry.active})
if($profiles.Count -ne 1 -or -not $profiles[0].serial){throw 'Select one registered device before live testing.'}
New-Item -ItemType Directory -Path $out,(Join-Path $out 'tmp') | Out-Null
Get-ChildItem Env: | Where-Object {$_.Name -match '^(PYTHON|CONDA|RV_|DCTS_|QT_|QML)'} |
    ForEach-Object {Remove-Item -LiteralPath ('Env:'+ $_.Name)}
$env:PATH=(Split-Path $program)+';'+$env:SystemRoot+'\System32'
$env:TEMP=Join-Path $out 'tmp';$env:TMP=$env:TEMP
$env:DCTS_LIVE_ENABLE='1';$env:DCTS_LIVE_SERIAL=$profiles[0].serial
$env:DCTS_LIVE_CONFIG_SOURCE=$config
$results=@()
foreach($backend in $Backends){
    $env:RV_INFERENCE_BACKEND=$backend
    $name=$backend -replace '[^A-Za-z0-9_-]','_'
    $report=Join-Path $out "$name.json"
    Write-Output "RUN real-device host depth: $backend"
    $process=Start-Process -FilePath $program -ArgumentList @('--host-assist-live-validation',('"'+$report+'"')) `
        -WorkingDirectory (Split-Path $program) -WindowStyle Normal -PassThru `
        -RedirectStandardOutput (Join-Path $out "$name.stdout.log") -RedirectStandardError (Join-Path $out "$name.stderr.log")
    if(-not $process.WaitForExit(600000)){
        $null=$process.CloseMainWindow()
        if(-not $process.WaitForExit(60000)){$process.Kill()}
        throw "Live test timed out: $backend. Inspect the board owner before continuing."
    }
    if(-not (Test-Path -LiteralPath $report)){throw "Live report missing: $backend (exit $($process.ExitCode))"}
    $r=Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    $ok=$process.ExitCode -eq 0 -and $r.passed -and $r.normal_stop -and $r.saved_sensor_files_unchanged -and $r.late_results_after_stop -eq 0
    $results+=@{device=$backend;passed=$ok;exit_code=$process.ExitCode;depth_fps=@($r.runs.depth_fps);normal_stop=$r.normal_stop;saved_sensor_files_unchanged=$r.saved_sensor_files_unchanged}
    @{passed=(@($results|Where-Object {-not $_.passed}).Count -eq 0);completed_devices=$results.Count;requested_devices=$Backends.Count;results=$results} |
        ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding UTF8
    if(-not $ok){throw "Real-device validation failed: $backend; see its local report."}
    Write-Output ("PASS {0}: depth FPS {1}, clean stop and saved settings unchanged" -f $backend,((@($r.runs.depth_fps)|ForEach-Object {'{0:N2}' -f $_}) -join ', '))
}
