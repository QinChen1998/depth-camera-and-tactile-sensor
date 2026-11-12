param([string]$ApplicationDirectory='', [string]$OutputDirectory='')
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'

# Read-only host diagnostics. Do not restart ADB, change USB drivers/network
# settings, or send shell commands to the board. Listing devices may start ADB.
function Invoke-AdbRead([string]$Program,[string]$Arguments) {
    $process=New-Object System.Diagnostics.Process
    $process.StartInfo.FileName=$Program
    $process.StartInfo.Arguments=$Arguments
    $process.StartInfo.UseShellExecute=$false
    $process.StartInfo.CreateNoWindow=$true
    $process.StartInfo.RedirectStandardOutput=$true
    $process.StartInfo.RedirectStandardError=$true
    try {
        [void]$process.Start()
        $stdout=$process.StandardOutput.ReadToEndAsync()
        $stderr=$process.StandardError.ReadToEndAsync()
        $finished=$process.WaitForExit(15000)
        if(-not $finished){$process.Kill();[void]$process.WaitForExit(3000)}
        $output=if($stdout.Wait(3000)){$stdout.Result}else{'Output read timed out.'}
        $errors=if($stderr.Wait(3000)){$stderr.Result}else{'Error read timed out.'}
        return [ordered]@{arguments=$Arguments;exit_code=if($finished){$process.ExitCode}else{-1};timed_out=(-not $finished);stdout=$output;stderr=$errors}
    } catch { return [ordered]@{arguments=$Arguments;error=$_.Exception.Message} }
    finally {$process.Dispose()}
}

if(-not $OutputDirectory){$OutputDirectory=[Environment]::GetFolderPath('Desktop')}
if(-not $OutputDirectory){$OutputDirectory=$PSScriptRoot}
[void][IO.Directory]::CreateDirectory($OutputDirectory)
$report=[ordered]@{schema='dcts-usb-host-diagnostics-v1';time_utc=[DateTime]::UtcNow.ToString('o');read_only=$true}
$os=Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
$report.os=[ordered]@{caption=$os.Caption;version=$os.Version;build=$os.BuildNumber;architecture=$os.OSArchitecture}

$candidates=New-Object 'System.Collections.Generic.List[string]'
if($ApplicationDirectory){$candidates.Add((Join-Path $ApplicationDirectory 'platform-tools/adb.exe'))}
$appName=([string][char]0x89e6)+[char]0x60f3+[char]0x5de5+[char]0x4f5c+[char]0x53f0
$uninstall='Software\Microsoft\Windows\CurrentVersion\Uninstall\{E927DDB5-704F-4F63-AEC3-54D67D2B1177}_is1'
foreach($hive in 'HKLM:','HKCU:'){
    $installed=Get-ItemProperty -LiteralPath ($hive+'\'+$uninstall) -ErrorAction SilentlyContinue
    if($installed.InstallLocation){$candidates.Add((Join-Path $installed.InstallLocation 'platform-tools/adb.exe'))}
}
if($env:ProgramFiles){$candidates.Add((Join-Path $env:ProgramFiles ($appName+'\platform-tools\adb.exe')))}
$candidates.Add((Join-Path $PSScriptRoot '../platform-tools/adb.exe'))
$fromPath=Get-Command adb.exe -ErrorAction SilentlyContinue
if($fromPath){$candidates.Add($fromPath.Source)}
$adb=$candidates | Where-Object {Test-Path -LiteralPath $_ -PathType Leaf} | Select-Object -First 1
$report.adb_found=[bool]$adb
if($adb){
    $report.adb_sha256=(Get-FileHash -LiteralPath $adb -Algorithm SHA256).Hash.ToLowerInvariant()
    $report.adb_version=Invoke-AdbRead $adb 'version'
    $report.adb_devices=Invoke-AdbRead $adb 'devices -l'
}
try {
    $report.usb_devices=@(Get-CimInstance Win32_PnPEntity | Where-Object {
        $_.PNPDeviceID -like 'USB\VID_2207*'
    } | Select-Object Name,PNPClass,Status,ConfigManagerErrorCode,Service,PNPDeviceID,HardwareID,CompatibleID)
    $report.usb_drivers=@(Get-CimInstance Win32_PnPSignedDriver | Where-Object {
        $_.DeviceID -like 'USB\VID_2207*'
    } | Select-Object DeviceName,DeviceID,DriverProviderName,DriverVersion,InfName,IsSigned)
} catch {$report.usb_query_error=$_.Exception.Message}

$path=Join-Path $OutputDirectory ('rv1103-usb-check-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss')+'.txt')
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
Write-Host ('ADB found: '+$report.adb_found)
if($adb){Write-Host $report.adb_devices.stdout;Write-Host $report.adb_devices.stderr}
Write-Host ('Rockchip USB entries: '+@($report.usb_devices).Count)
foreach($device in $report.usb_devices){Write-Host ($device.Name+' | status='+$device.Status+' | problem='+$device.ConfigManagerErrorCode+' | service='+$device.Service)}
Write-Host ('Report saved: '+$path)
Write-Host 'No driver, network setting or board file was changed.'
