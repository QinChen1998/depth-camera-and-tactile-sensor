param([switch]$SmokeOnly,[switch]$Research)
$ErrorActionPreference='Stop'
$logs='C:\DctsResults'
$source='C:\DctsInstaller'
$product='触想工作台'
$setupPrefix=if($Research){$product}else{"$product-内测版"}
$settingsFolder=if($Research){'DepthCameraAndTactileSensorResearch'}else{'DepthCameraAndTactileSensor'}
$appId=if($Research){'{A5D7FCC3-8B49-4F58-AB05-E8712DEAEC3A}'}else{'{E927DDB5-704F-4F63-AEC3-54D67D2B1177}'}
$uninstallKey="HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1"
$windowTitle=if($Research){$product}else{"$product（内测版）"}
$upgrading=$Research -and (Test-Path -LiteralPath $uninstallKey)
$app=if($upgrading){(Get-ItemProperty -LiteralPath $uninstallKey -Name InstallLocation).InstallLocation}
    elseif($Research){Join-Path $env:ProgramFiles "DCTS\$product"}
    else{Join-Path $env:ProgramFiles $product}
$exe=Join-Path $app '触想工作台.exe'
$programs=Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'
$programGroup=if($Research){Join-Path $programs "DCTS\$product"}else{Join-Path $programs $product}
$programLink="$product.lnk"
$uninstallLink="卸载$programLink"
$desktopLink=if($Research){"$product（上位机辅助推理）.lnk"}else{$programLink}
$desktopPath=Join-Path $env:PUBLIC "Desktop\$desktopLink"
$protectedEngine='depthpro2-s-480x640-fp16.dctsm'
$researchRuntime=Join-Path $app 'DepthPro2Runtime'
$env:RV_TEST_OFFLINE_ONLY='1'
$env:PATH="$env:SystemRoot\System32;$env:SystemRoot"
$checks=[ordered]@{}
$checks.scope=if($SmokeOnly){'install-start-close-uninstall'}else{'full-offline-and-lifecycle'}
$checks.edition=if($Research){'research'}else{'standard'}
$checks.install_mode=if($upgrading){'upgrade'}else{'clean'}
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
function Save-Screenshot([string]$name){
    Add-Type -AssemblyName System.Windows.Forms,System.Drawing
    $bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap=New-Object Drawing.Bitmap($bounds.Width,$bounds.Height)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($bounds.Location,[Drawing.Point]::Empty,$bounds.Size)
        $bitmap.Save((Join-Path $logs $name),[Drawing.Imaging.ImageFormat]::Png)
    } finally {$graphics.Dispose();$bitmap.Dispose()}
}
function Check-LicensePrompt([string]$setupPath){
    Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
    Progress 'Opening the interactive installer to check the license prompt'
    $started=(Get-Date).AddSeconds(-3)
    $launcher=Start-Process -FilePath $setupPath -ArgumentList '/NORESTART' -WindowStyle Normal -PassThru
    $wizard=$null
    try {
        $until=[DateTime]::UtcNow.AddSeconds(45)
        do {
            $visibleWindows=Get-Process | Where-Object { $_.MainWindowHandle -ne 0 }
            $wizard=$visibleWindows | Where-Object { $_.MainWindowTitle -like "*$product*" -and $_.StartTime -ge $started } | Select-Object -First 1
            if($wizard){
                $window=[System.Windows.Automation.AutomationElement]::FromHandle($wizard.MainWindowHandle)
                $controls=$window.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)
                $agree=$controls | Where-Object { $_.Current.Name -like '*我同意此协议*' -and !$_.Current.IsOffscreen } | Select-Object -First 1
                $decline=$controls | Where-Object { $_.Current.Name -like '*我不同意此协议*' -and !$_.Current.IsOffscreen } | Select-Object -First 1
                $next=$controls | Where-Object { $_.Current.Name -like '*下一步*' -and !$_.Current.IsOffscreen } | Select-Object -First 1
                if($agree -and $decline -and $next){break}
            }
            Start-Sleep -Milliseconds 250
        } while([DateTime]::UtcNow -lt $until)
        if($wizard){
            $checks.setup_window_title=$wizard.MainWindowTitle
        }else{
            Progress ('Visible windows: ' + (($visibleWindows | ForEach-Object MainWindowTitle) -join ' / '))
            $launcher.Refresh()
            if($launcher.HasExited){Progress "Interactive installer exit code: $($launcher.ExitCode)"}
        }
        Save-Screenshot 'license-prompt.png'
        Check 'setup_window_name' ($wizard -and $wizard.MainWindowTitle -notlike '*科研版*')
        Check 'setup_license_confirmation_visible' ([bool]($agree -and $decline))
        Check 'setup_license_acceptance_required' ($next -and !$next.Current.IsEnabled)
    } finally {
        # End the interactive probe at the agreement screen before installing silently.
        if($wizard -and !$wizard.HasExited){$wizard.Kill();$null=$wizard.WaitForExit(5000)}
        if(!$launcher.HasExited){$launcher.Kill();$null=$launcher.WaitForExit(5000)}
    }
}
try {
    $setup=(Get-ChildItem -LiteralPath $source -Filter "${setupPrefix}-v*-x64.exe").FullName
    $checks.setup_sha256=(Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
    if($Research){Check-LicensePrompt $setup}
    Check 'install_exit_0' ((Run $setup "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=$logs\install.log" 300) -eq 0)
    Check 'program_files_install' (Test-Path -LiteralPath $exe)
    Check 'uninstall_registration_present' (Test-Path -LiteralPath $uninstallKey)
    Check 'windows_platform_plugin' (Test-Path (Join-Path $app 'platforms\qwindows.dll'))
    Check 'application_local_vc_runtime' (Test-Path (Join-Path $app 'VCRUNTIME140.dll'))
    Check 'desktop_shortcut' ((Test-Path -LiteralPath $desktopPath) -and
        (New-Object -ComObject WScript.Shell).CreateShortcut($desktopPath).TargetPath -eq $exe)
    Check 'start_menu_shortcuts' ((Test-Path -LiteralPath (Join-Path $programGroup $programLink)) -and
        (Test-Path -LiteralPath (Join-Path $programGroup $uninstallLink)))
    $forbidden=@(Get-ChildItem -LiteralPath $app -Recurse -File | Where-Object {
        $_.Extension -match '^\.(onnx|ovm|engine|safetensors|pdb|rknn|py)$' -or $_.Name -match '^(README|AGENTS)(\..*)?$|^PackModelCopy\.exe$|model_key_material' -or
        ($_.Extension -eq '.dctsm' -and (!$Research -or $_.FullName -ne (Join-Path $researchRuntime $protectedEngine)))
    })
    Check 'no_plaintext_models_or_development_files' ($forbidden.Count -eq 0)
    Check 'no_unexpected_inference_runtimes' (@(Get-ChildItem -LiteralPath $app -Directory | Where-Object {
        $_.Name -match '^UniDepthRuntime' -or (!$Research -and $_.Name -eq 'DepthPro2Runtime')
    }).Count -eq 0)
    if($Research){
        Check 'registered_display_name' ((Get-ItemProperty -LiteralPath $uninstallKey -Name DisplayName).DisplayName -ceq $product)
        Check 'old_product_shortcuts_removed' (!(Test-Path -LiteralPath (Join-Path $env:PUBLIC 'Desktop/触想工作台-科研版.lnk')) -and
            !(Test-Path -LiteralPath (Join-Path $programs '触想工作台-科研版/触想工作台-科研版.lnk')) -and
            !(Test-Path -LiteralPath (Join-Path $programs '触想工作台-科研版/卸载触想工作台-科研版.lnk')))
        $researchManifest=Get-Content -LiteralPath (Join-Path $researchRuntime 'runtime.json') -Raw | ConvertFrom-Json
        Check 'installed_runtime_protection' ($researchManifest.model_protection -ceq 'aes-256-gcm')
        foreach($name in $protectedEngine,'DctsDepthPro2Runtime.dll','DctsDepthPro2Worker.exe',
            'Apple-Model-LICENSE.txt','Research-Model-NOTICE.txt','NVIDIA-TensorRT-LICENSE.txt','NVIDIA-CUDA-LICENSE.txt',
            'Qt6Core.dll','cudart64_12.dll','nvinfer_10.dll','nvinfer_plugin_10.dll'){
            $file=Join-Path $researchRuntime $name
            Check "installed_research_$name" ((Test-Path -LiteralPath $file -PathType Leaf) -and
                (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq $researchManifest.files.$name)
        }
    }
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
    $adb=Join-Path $app 'platform-tools\adb.exe'
    $worker=Join-Path $app 'rv_control\RvControlWorker.exe'
    Check 'native_worker_self_test' ((Run $worker '--self-test' 20) -eq 0)
    Check 'managed_vlc_rtsp_plugin' (Test-Path (Join-Path $app 'vlc\plugins\access\liblive555_plugin.dll'))
    Check 'managed_vlc_h265_plugin' (Test-Path (Join-Path $app 'vlc\plugins\codec\libavcodec_plugin.dll'))
    Check 'adb_loads' ((Run $adb 'version' 15) -eq 0)
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
    foreach($name in 'EXACT_IMAGING_SCHEMES','OTHER_IMAGING_SCHEMES_ABSENT','NO_INFERENCE_BACKEND_SELECTOR'){
        Check $name ($ui -match "(?m)^$name PASS\s*$")
    }
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
        } until ($process.HasExited -or ($process.MainWindowHandle -ne 0 -and $process.MainWindowTitle -in @($windowTitle,"$windowTitle - 触想工作台")) -or [DateTime]::UtcNow -gt $end)
        $checks.ordinary_window_title=$process.MainWindowTitle
        Check 'ordinary_start_main_window' (-not $process.HasExited -and $process.MainWindowHandle -ne 0)
        Check 'ordinary_start_chinese_title' ($process.MainWindowTitle -in @($windowTitle,"$windowTitle - 触想工作台"))
        Start-Sleep -Seconds 2
        $process.Refresh()
        Check 'ordinary_window_responding' (-not $process.HasExited -and $process.Responding)
        Save-Screenshot 'startup.png'
        $prefs=Join-Path $env:LOCALAPPDATA "$settingsFolder\config\preferences\DepthCameraAndTactileSensor\DepthCameraAndTactileSensor.ini"
        Check 'preferences_in_local_appdata' (Test-Path -LiteralPath $prefs)
        Check 'first_run_zh_cn' ([bool](Select-String -LiteralPath $prefs -SimpleMatch 'language=zh_CN'))
        Check 'first_run_host_assist_preference' ([bool](Select-String -LiteralPath $prefs -Pattern '^imagingScheme=host_assisted_depth$'))
        Check 'config_outside_install_directory' (-not (Test-Path (Join-Path $app 'config')))
        Check 'close_main_window' ($process.CloseMainWindow())
        Check 'ordinary_exit_0' ($process.WaitForExit(30000) -and $process.ExitCode -eq 0)
    } finally {
        if(-not $process.HasExited){$process.Kill()}
    }
    $preferencesSha256=(Get-FileHash -LiteralPath $prefs -Algorithm SHA256).Hash
    if(-not $SmokeOnly){
        Check 'reinstall_exit_0' ((Run $setup "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=$logs\reinstall.log" 300) -eq 0)
        Check 'reinstall_preserves_preferences' ((Test-Path -LiteralPath $prefs) -and
            ((Get-FileHash -LiteralPath $prefs -Algorithm SHA256).Hash -eq $preferencesSha256))
    }
    $uninstall=Join-Path $app 'unins000.exe'
    Check 'uninstall_exit_0' ((Run $uninstall "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=$logs\uninstall.log" 300) -eq 0)
    # Inno's temporary uninstaller removes its own files after the launcher exits.
    $uninstallDeadline=[DateTime]::UtcNow.AddSeconds(15)
    while((Test-Path -LiteralPath $app) -and [DateTime]::UtcNow -lt $uninstallDeadline){
        Start-Sleep -Milliseconds 250
    }
    Check 'uninstalled_executable_removed' (-not (Test-Path -LiteralPath $exe))
    Check 'installed_payload_removed' (-not (Test-Path -LiteralPath $app) -or
        @(Get-ChildItem -LiteralPath $app -File -Recurse -Force).Count -eq 0)
    Check 'desktop_shortcut_removed' (-not (Test-Path -LiteralPath $desktopPath))
    Check 'start_menu_shortcuts_removed' (-not (Test-Path -LiteralPath (Join-Path $programGroup $programLink)) -and
        -not (Test-Path -LiteralPath (Join-Path $programGroup $uninstallLink)))
    Check 'uninstall_registration_removed' (-not (Test-Path -LiteralPath $uninstallKey))
    Check 'user_preferences_preserved' ((Test-Path -LiteralPath $prefs) -and
        ((Get-FileHash -LiteralPath $prefs -Algorithm SHA256).Hash -eq $preferencesSha256))
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
