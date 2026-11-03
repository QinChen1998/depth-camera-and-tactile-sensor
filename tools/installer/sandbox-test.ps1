﻿$ErrorActionPreference='Stop'
$logs='C:\DctsResults'
$source='C:\DctsInstaller'
$app=Join-Path $env:ProgramFiles 'Depth Camera and Tactile Sensor'
$exe=Join-Path $app 'Depth Camera and Tactile Sensor.exe'
$env:RV_TEST_OFFLINE_ONLY='1'
$env:PATH="$env:SystemRoot\System32;$env:SystemRoot"
$checks=[ordered]@{}
function Check([string]$name,[bool]$ok){
    $checks[$name]=$ok
    if(-not $ok){throw "Check failed: $name"}
}
function Run([string]$program,[string]$arguments,[int]$timeout=150){
    $p=Start-Process -FilePath $program -ArgumentList $arguments -PassThru
    if(-not $p.WaitForExit($timeout*1000)){
        $p.Kill()
        throw "$program timed out"
    }
    return $p.ExitCode
}
try {
    $setup=(Get-ChildItem -LiteralPath $source -Filter 'DCTS-Setup-*-x64.exe').FullName
    Check 'install_exit_0' ((Run $setup "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=$logs\install.log" 300) -eq 0)
    Check 'program_files_install' (Test-Path -LiteralPath $exe)
    Check 'application_gpl_license' (Test-Path -LiteralPath (Join-Path $app 'licenses/DCTS-GPL-3.0.txt'))
    $sourceNotice=Get-Content -LiteralPath (Join-Path $app 'licenses/Corresponding-Source.txt') -Raw
    Check 'matching_source_archive_named' ($sourceNotice.Contains('DCTS-1.1.2-corresponding-source.zip'))
    Check 'no_factory_ssh_trust' ((Get-Item -LiteralPath (Join-Path $app 'rv_control/known_hosts')).Length -eq 0)
    Check 'windows_platform_plugin' (Test-Path (Join-Path $app 'platforms\qwindows.dll'))
    Check 'application_local_vc_runtime' (Test-Path (Join-Path $app 'VCRUNTIME140.dll'))
    Check 'desktop_shortcut' (@(Get-ChildItem "$env:PUBLIC\Desktop" -Filter '*.lnk' | Where-Object {
        (New-Object -ComObject WScript.Shell).CreateShortcut($_.FullName).TargetPath -eq $exe
    }).Count -eq 1)
    $forbidden=@(Get-ChildItem -LiteralPath $app -Recurse -File | Where-Object {
        $_.Extension -match '^\.(onnx|pdb|rknn|py)$' -or $_.Name -match '^(README|AGENTS)(\..*)?$|^PackModelCopy\.exe$|model_key_material'
    })
    Check 'no_plaintext_models_or_development_files' ($forbidden.Count -eq 0)
    $modelRoot=Join-Path $app 'UniDepthRuntimeWide576FP16'
    $manifest=Get-Content (Join-Path $modelRoot 'runtime.json') -Raw | ConvertFrom-Json
    Check 'model_manifest_scheme' ($manifest.model_protection.scheme -eq 'aes256-gcm-memory-v1')
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
    foreach($flag in 'port-ui-smoke-test','tactile-render-test','rv-assist-recovery-test','rv-tuner-test','rv-usb-enrollment-test'){
        $checks[$flag]=((Run $exe "--$flag $logs\$flag.txt") -eq 0)
    }
    $ui=Get-Content -LiteralPath "$logs\port-ui-smoke-test.txt" -Raw
    foreach($name in 'NO_TENSORRT_PREVIEW_MENU','HOST_GPU_WARNING_WITH_BOARD_ADVICE','BOARD_DEPTH_START_CAMERA_STOP','BOARD_CAMERA_RECONNECT_DEPTH_STOP'){
        Check $name ($ui -match "(?m)^$name PASS\s*$")
    }
    $adb=Join-Path $app 'platform-tools\adb.exe'
    $worker=Join-Path $app 'rv_control\RvControlWorker.exe'
    $boardRoot=Join-Path $app 'rv_control'
    $boardManifest=Get-Content -LiteralPath (Join-Path $boardRoot 'implementation.json') -Raw | ConvertFrom-Json
    foreach($name in 'model.dctsm','litesr-fov1.bin','litesr-fov2.bin','litesr_gui.sh','dtof-switch.sh'){
        $relative="litesr_board/$name"
        $hash=(Get-FileHash -LiteralPath (Join-Path $boardRoot $relative)).Hash
        Check ("board_payload_sha256_"+$name) ($hash -eq $boardManifest.payload_files.$relative)
    }
    $boardModel=[IO.File]::ReadAllBytes((Join-Path $boardRoot 'litesr_board/model.dctsm'))
    Check 'board_encrypted_model_header' ([Text.Encoding]::ASCII.GetString($boardModel,0,7) -eq 'DCTSM01')
    Check 'board_import_button' ($ui -match '(?m)^LITESR_IMPORT_ACTION PASS\s*$')
    Check 'native_worker_self_test' ((Run $worker '--self-test' 20) -eq 0)
    Check 'managed_vlc_rtsp_plugin' (Test-Path (Join-Path $app 'vlc\plugins\access\liblive555_plugin.dll'))
    Check 'managed_vlc_h265_plugin' (Test-Path (Join-Path $app 'vlc\plugins\codec\libavcodec_plugin.dll'))
    Check 'adb_loads' ((Run $adb 'version' 15) -eq 0)

    $process=Start-Process -FilePath $exe -WorkingDirectory $env:TEMP -PassThru
    try {
        $end=[DateTime]::UtcNow.AddSeconds(40)
        do {
            Start-Sleep -Milliseconds 250
            $process.Refresh()
        } until ($process.HasExited -or $process.MainWindowHandle -ne 0 -or [DateTime]::UtcNow -gt $end)
        Check 'ordinary_start_main_window' (-not $process.HasExited -and $process.MainWindowHandle -ne 0)
        Check 'ordinary_start_chinese_title' ($process.MainWindowTitle -eq '触想工作台')
        $prefs=Join-Path $env:LOCALAPPDATA 'DepthCameraAndTactileSensor\config\preferences\DepthCameraAndTactileSensor\DepthCameraAndTactileSensor.ini'
        Check 'preferences_in_local_appdata' (Test-Path -LiteralPath $prefs)
        Check 'first_run_zh_cn' ([bool](Select-String -LiteralPath $prefs -SimpleMatch 'language=zh_CN'))
        Check 'config_outside_install_directory' (-not (Test-Path (Join-Path $app 'config')))
        Check 'close_main_window' ($process.CloseMainWindow())
        Check 'ordinary_exit_0' ($process.WaitForExit(30000) -and $process.ExitCode -eq 0)
    } finally {
        if(-not $process.HasExited){$process.Kill()}
    }
    $uninstall=Join-Path $app 'unins000.exe'
    Check 'uninstall_exit_0' ((Run $uninstall "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=$logs\uninstall.log" 300) -eq 0)
    Check 'uninstalled_executable_removed' (-not (Test-Path -LiteralPath $exe))
    Check 'user_preferences_preserved' (Test-Path -LiteralPath $prefs)
    $checks.passed=(@($checks.Values | Where-Object {$_ -eq $false}).Count -eq 0)
} catch {
    $checks.passed=$false
    $checks.error=$_.Exception.Message
} finally {
    $checks | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath "$logs\sandbox-result.json"
}
