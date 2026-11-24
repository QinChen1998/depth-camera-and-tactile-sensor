param([switch]$SmokeOnly)
$ErrorActionPreference='Stop'
$logs='C:\DctsResults'
$source='C:\DctsInstaller'
$app=Join-Path $env:ProgramFiles '触想工作台'
$exe=Join-Path $app '触想工作台.exe'
$env:RV_TEST_OFFLINE_ONLY='1'
$env:PATH="$env:SystemRoot\System32;$env:SystemRoot"
$checks=[ordered]@{}
$checks.scope=if($SmokeOnly){'install-start-close-uninstall'}else{'full-offline-and-lifecycle'}
function Progress([string]$message){
    ([DateTime]::UtcNow.ToString('o')+' '+$message) | Add-Content -LiteralPath "$logs\progress.txt" -Encoding UTF8
}
function Check([string]$name,[bool]$ok){
    $checks[$name]=$ok
    Progress "$name=$ok"
    if(-not $ok){throw "Check failed: $name"}
}
function Run([string]$program,[string]$arguments,[int]$timeout=150){
    Progress ("Running "+(Split-Path -Leaf $program)+" "+$arguments)
    $p=Start-Process -FilePath $program -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if(-not $p.WaitForExit($timeout*1000)){
        $p.Kill()
        throw "$program timed out"
    }
    return $p.ExitCode
}
try {
    $setup=(Get-ChildItem -LiteralPath $source -Filter '触想工作台-内测版-v*-x64.exe').FullName
    Check 'install_exit_0' ((Run $setup "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=$logs\install.log" 300) -eq 0)
    Check 'program_files_install' (Test-Path -LiteralPath $exe)
    Check 'windows_platform_plugin' (Test-Path (Join-Path $app 'platforms\qwindows.dll'))
    Check 'application_local_vc_runtime' (Test-Path (Join-Path $app 'VCRUNTIME140.dll'))
    Check 'desktop_shortcut' (@(Get-ChildItem "$env:PUBLIC\Desktop" -Filter '*.lnk' | Where-Object {
        (New-Object -ComObject WScript.Shell).CreateShortcut($_.FullName).TargetPath -eq $exe
    }).Count -eq 1)
    $forbidden=@(Get-ChildItem -LiteralPath $app -Recurse -File | Where-Object {
        $_.Extension -match '^\.(onnx|ovm|engine|safetensors|dctsm|pdb|rknn|py)$' -or $_.Name -match '^(README|AGENTS)(\..*)?$|^PackModelCopy\.exe$|model_key_material'
    })
    Check 'no_plaintext_models_or_development_files' ($forbidden.Count -eq 0)
    Check 'no_bundled_inference_models' (@(Get-ChildItem -LiteralPath $app -Directory | Where-Object {
        $_.Name -match '^(UniDepthRuntime.*|DepthPro2Runtime)$'
    }).Count -eq 0)
    Check 'installed_sdk_runtime' (Test-Path (Join-Path $app 'CxwlDepthSdk.dll'))
    Check 'installed_sdk_decoder' (Test-Path (Join-Path $app 'cxwl-depth-sdk/avformat-61.dll'))
    Check 'vlc_cache_present' (Test-Path (Join-Path $app 'vlc\plugins\plugins.dat'))
    Check 'vlc_unused_modules_removed' (-not (Test-Path (Join-Path $app 'vlc\plugins\gui')) -and
        -not (Test-Path (Join-Path $app 'vlc\plugins\visualization')))
    $boardRoot=Join-Path $app 'rv_control'
    $boardManifest=Get-Content -LiteralPath (Join-Path $boardRoot 'implementation.json') -Raw | ConvertFrom-Json
    Check 'capture_has_no_board_model' (-not (Test-Path -LiteralPath (Join-Path $boardRoot 'litesr_board')))
    foreach($name in @('native_gui.sh','dtof-host.sh','native-fov6.bin')){
        $relative="rv_capture/$name"
        Check "capture_asset_$name" ((Get-FileHash -LiteralPath (Join-Path $boardRoot $relative) -Algorithm SHA256).Hash -eq $boardManifest.payload_files.$relative)
    }
    if(-not $SmokeOnly){
    foreach($flag in 'port-ui-smoke-test','tactile-serial-test','tactile-visualization-test','tactile-3d-test',
        'tactile-render-test','rv-assist-recovery-test','rv-tuner-test','rv-usb-enrollment-test'){
        $exitCode=Run $exe "--$flag $logs\$flag.txt" 240
        $report=Get-Content -LiteralPath "$logs\$flag.txt" -Raw
        $assertions=@($report -split "`r?`n" | Where-Object {$_ -notmatch '^RESULT ' -and $_ -match '(?:^PASS\b|\bPASS$)'})
        Check $flag ($exitCode -eq 0 -and $assertions.Count -gt 0 -and $report -notmatch '\bFAIL\b')
        $checks["${flag}_assertions"]=$assertions.Count
    }
    $ui=Get-Content -LiteralPath "$logs\port-ui-smoke-test.txt" -Raw
    foreach($name in 'EXACT_IMAGING_SCHEMES','OTHER_IMAGING_SCHEMES_ABSENT','INFERENCE_DEVICE_IN_SETTINGS'){
        Check $name ($ui -match "(?m)^$name PASS\s*$")
    }
    $adb=Join-Path $app 'platform-tools\adb.exe'
    $worker=Join-Path $app 'rv_control\RvControlWorker.exe'
    Check 'native_worker_self_test' ((Run $worker '--self-test' 20) -eq 0)
    Check 'managed_vlc_rtsp_plugin' (Test-Path (Join-Path $app 'vlc\plugins\access\liblive555_plugin.dll'))
    Check 'managed_vlc_h265_plugin' (Test-Path (Join-Path $app 'vlc\plugins\codec\libavcodec_plugin.dll'))
    Check 'adb_loads' ((Run $adb 'version' 15) -eq 0)
    }

    # This is the foreground GUI under test inside the disposable guest.
    # Hidden is appropriate for the helpers above, but prevents checking the
    # ordinary visible window and its normal interactive close action.
    $process=Start-Process -FilePath $exe -WorkingDirectory $env:TEMP -WindowStyle Normal -PassThru
    try {
        $end=[DateTime]::UtcNow.AddSeconds(40)
        do {
            Start-Sleep -Milliseconds 250
            $process.Refresh()
        } until ($process.HasExited -or ($process.MainWindowHandle -ne 0 -and $process.MainWindowTitle -in @('触想工作台（内测版）','触想工作台（内测版） - 触想工作台')) -or [DateTime]::UtcNow -gt $end)
        $checks.ordinary_window_title=$process.MainWindowTitle
        Check 'ordinary_start_main_window' (-not $process.HasExited -and $process.MainWindowHandle -ne 0)
        Check 'ordinary_start_chinese_title' ($process.MainWindowTitle -in @('触想工作台（内测版）','触想工作台（内测版） - 触想工作台'))
        Start-Sleep -Seconds 2
        $process.Refresh()
        Check 'ordinary_window_responding' (-not $process.HasExited -and $process.Responding)
        Add-Type -AssemblyName System.Windows.Forms,System.Drawing
        $bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bitmap=New-Object Drawing.Bitmap($bounds.Width,$bounds.Height)
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($bounds.Location,[Drawing.Point]::Empty,$bounds.Size)
            $bitmap.Save((Join-Path $logs 'startup.png'),[Drawing.Imaging.ImageFormat]::Png)
        } finally {$graphics.Dispose();$bitmap.Dispose()}
        $prefs=Join-Path $env:LOCALAPPDATA 'DepthCameraAndTactileSensor\config\preferences\DepthCameraAndTactileSensor\DepthCameraAndTactileSensor.ini'
        Check 'preferences_in_local_appdata' (Test-Path -LiteralPath $prefs)
        Check 'first_run_zh_cn' ([bool](Select-String -LiteralPath $prefs -SimpleMatch 'language=zh_CN'))
        Check 'first_run_host_assist' ([bool](Select-String -LiteralPath $prefs -SimpleMatch 'imagingScheme=host_unidepth_rgb672_vga_fp16_tof48_g'))
        Check 'config_outside_install_directory' (-not (Test-Path (Join-Path $app 'config')))
        Check 'close_main_window' ($process.CloseMainWindow())
        Check 'ordinary_exit_0' ($process.WaitForExit(30000) -and $process.ExitCode -eq 0)
    } finally {
        if(-not $process.HasExited){$process.Kill()}
    }
    if(-not $SmokeOnly){
        Check 'reinstall_exit_0' ((Run $setup "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=$logs\reinstall.log" 300) -eq 0)
        Check 'reinstall_preserves_preferences' (Test-Path -LiteralPath $prefs)
    }
    $uninstall=Join-Path $app 'unins000.exe'
    Check 'uninstall_exit_0' ((Run $uninstall "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=$logs\uninstall.log" 300) -eq 0)
    Check 'uninstalled_executable_removed' (-not (Test-Path -LiteralPath $exe))
    Check 'desktop_shortcut_removed' (-not (Test-Path -LiteralPath (Join-Path $env:PUBLIC 'Desktop/触想工作台.lnk')))
    Check 'uninstall_registration_removed' (-not (Test-Path -LiteralPath 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{E927DDB5-704F-4F63-AEC3-54D67D2B1177}_is1'))
    Check 'user_preferences_preserved' (Test-Path -LiteralPath $prefs)
    $checks.passed=(@($checks.Values | Where-Object {$_ -eq $false}).Count -eq 0)
} catch {
    $checks.passed=$false
    $checks.error=$_.Exception.Message
} finally {
    # Even a failed assertion must leave the disposable VM uninstalled and
    # preserve evidence. Never turn a failed run into a pass after cleanup.
    $cleanupUninstaller=Join-Path $app 'unins000.exe'
    if($checks.passed -ne $true -and (Test-Path -LiteralPath $cleanupUninstaller)){
        try {
            $checks.failure_cleanup_uninstall_exit=Run $cleanupUninstaller "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=$logs\failure-cleanup.log" 300
            $checks.failure_cleanup_removed_executable=-not (Test-Path -LiteralPath $exe)
        } catch {$checks.failure_cleanup_error=$_.Exception.Message}
    }
    $checks | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath "$logs\sandbox-result.json"
}
