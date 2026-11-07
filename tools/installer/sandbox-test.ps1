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
function Check([string]$name,[bool]$ok){
    $checks[$name]=$ok
    if(-not $ok){throw "Check failed: $name"}
}
function Run([string]$program,[string]$arguments,[int]$timeout=150){
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
    Check 'inference_worker_installed' (Test-Path -LiteralPath (Join-Path $app 'DctsInferenceWorker.exe') -PathType Leaf)
    Check 'windows_platform_plugin' (Test-Path (Join-Path $app 'platforms\qwindows.dll'))
    Check 'application_local_vc_runtime' (Test-Path (Join-Path $app 'VCRUNTIME140.dll'))
    Check 'desktop_shortcut' (@(Get-ChildItem "$env:PUBLIC\Desktop" -Filter '*.lnk' | Where-Object {
        (New-Object -ComObject WScript.Shell).CreateShortcut($_.FullName).TargetPath -eq $exe
    }).Count -eq 1)
    $forbidden=@(Get-ChildItem -LiteralPath $app -Recurse -File | Where-Object {
        $_.Extension -match '^\.(onnx|pdb|rknn|py)$' -or $_.Name -match '^(README|AGENTS)(\..*)?$|^PackModelCopy\.exe$|model_key_material'
    })
    Check 'no_plaintext_models_or_development_files' ($forbidden.Count -eq 0)
    $modelRoot=Join-Path $app 'UniDepthRuntimeRgb672VgaFP16RTX'
    $manifest=Get-Content (Join-Path $modelRoot 'runtime.json') -Raw | ConvertFrom-Json
    Check 'model_manifest_scheme' ($manifest.model_protection.scheme -eq 'aes256-gcm-memory-v1')
    Check 'vga_model_identity' ($manifest.model_protection.plaintext_sha256 -eq '50d22cd33e126832793d905788a1efadeb4c9298de72cbfa634d49fdcdbc2dc1')
    Check 'tensorrt_rtx_backend' ($manifest.backend -eq 'onnxruntime-tensorrt-rtx' -and $manifest.ort_version -eq '1.31.1')
    Check 'no_legacy_cuda_runtime_or_preview' (@(Get-ChildItem -LiteralPath $app -Recurse -File | Where-Object {
        $_.Name -match '^(onnxruntime_providers_cuda|cudnn.*|cublas.*|cufft.*|RvAssistTensorRtRtx)\.(dll|exe)$'
    }).Count -eq 0)
    Check 'vlc_cache_present' (Test-Path (Join-Path $app 'vlc\plugins\plugins.dat'))
    Check 'vlc_unused_modules_removed' (-not (Test-Path (Join-Path $app 'vlc\plugins\gui')) -and
        -not (Test-Path (Join-Path $app 'vlc\plugins\visualization')))
    foreach($item in $manifest.files.PSObject.Properties){
        $actual=(Get-FileHash -LiteralPath (Join-Path $modelRoot $item.Name) -Algorithm SHA256).Hash
        if($actual -ne $item.Value){throw "Installed model asset failed checksum: $($item.Name)"}
    }
    Check 'model_assets_sha256' $true
    $boardRoot=Join-Path $app 'rv_control'
    $boardManifest=Get-Content -LiteralPath (Join-Path $boardRoot 'implementation.json') -Raw | ConvertFrom-Json
    $boardCipher=Join-Path $boardRoot 'litesr_board/rect-model.dctsm'
    Check 'board384_ciphertext_sha256' ((Get-FileHash -LiteralPath $boardCipher -Algorithm SHA256).Hash -eq $boardManifest.payload_files.'litesr_board/rect-model.dctsm')
    $boardBytes=[IO.File]::ReadAllBytes($boardCipher)
    Check 'board384_encrypted_envelope' ([Text.Encoding]::ASCII.GetString($boardBytes,0,8) -eq "DCTSM01`0")
    $tinyCipher=Join-Path $boardRoot 'litesr_board/tiny-model.dctsm'
    foreach($name in 'tiny-model.dctsm','tiny-engine.bin','tiny-camera.bin','tiny-camera-session.sh'){
        Check "tiny_$name" ((Get-FileHash -LiteralPath (Join-Path $boardRoot "litesr_board/$name") -Algorithm SHA256).Hash -eq $boardManifest.payload_files."litesr_board/$name")
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
    foreach($name in 'EXACT_IMAGING_SCHEMES','OTHER_IMAGING_SCHEMES_ABSENT','CHINESE_TWO_SCHEMES','ENGLISH_TWO_SCHEMES',
        'HOST_GPU_WARNING_WITH_BOARD_ADVICE','BOARD_DEPTH_START_CAMERA_STOP','BOARD_CAMERA_RECONNECT_DEPTH_STOP'){
        Check $name ($ui -match "(?m)^$name PASS\s*$")
    }
    $adb=Join-Path $app 'platform-tools\adb.exe'
    $worker=Join-Path $app 'rv_control\RvControlWorker.exe'
    Check 'native_worker_self_test' ((Run $worker '--self-test' 20) -eq 0)
    Check 'managed_vlc_rtsp_plugin' (Test-Path (Join-Path $app 'vlc\plugins\access\liblive555_plugin.dll'))
    Check 'managed_vlc_h265_plugin' (Test-Path (Join-Path $app 'vlc\plugins\codec\libavcodec_plugin.dll'))
    Check 'adb_loads' ((Run $adb 'version' 15) -eq 0)
    }

    $process=Start-Process -FilePath $exe -WorkingDirectory $env:TEMP -PassThru
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
        Check 'first_run_board384' ([bool](Select-String -LiteralPath $prefs -SimpleMatch 'imagingScheme=board_litesr_near_384x256_host_contour_hd'))
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
    Check 'uninstalled_inference_worker_removed' (-not (Test-Path -LiteralPath (Join-Path $app 'DctsInferenceWorker.exe')))
    Check 'uninstalled_model_removed' (-not (Test-Path -LiteralPath (Join-Path $modelRoot 'model.dctsm')))
    Check 'uninstalled_board_model_removed' (-not (Test-Path -LiteralPath $boardCipher))
    Check 'uninstalled_tiny_model_removed' (-not (Test-Path -LiteralPath $tinyCipher))
    Check 'desktop_shortcut_removed' (-not (Test-Path -LiteralPath (Join-Path $env:PUBLIC 'Desktop/触想工作台.lnk')))
    Check 'uninstall_registration_removed' (-not (Test-Path -LiteralPath 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{E927DDB5-704F-4F63-AEC3-54D67D2B1177}_is1'))
    Check 'user_preferences_preserved' (Test-Path -LiteralPath $prefs)
    $checks.passed=(@($checks.Values | Where-Object {$_ -eq $false}).Count -eq 0)
} catch {
    $checks.passed=$false
    $checks.error=$_.Exception.Message
} finally {
    $checks | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath "$logs\sandbox-result.json"
}
