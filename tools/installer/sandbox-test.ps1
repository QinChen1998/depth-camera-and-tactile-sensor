param(
    [ValidateSet('Normal','Customer')][string]$Edition='Normal',
    [switch]$SmokeOnly,
    [string]$InputDirectory='C:\DctsInstaller',
    [string]$ResultsDirectory='C:\DctsResults',
    [string]$NativeInstallDirectory=''
)
$ErrorActionPreference='Stop'
$logs=(Resolve-Path -LiteralPath $ResultsDirectory).Path
$source=(Resolve-Path -LiteralPath $InputDirectory).Path
$native=[bool]$NativeInstallDirectory
$customer=$Edition -eq 'Customer'
$label=if($customer){'客户版'}else{'内测版'}
$appName=if($customer){'触想工作台-客户版'}else{'触想工作台'}
$profileName=if($customer){'DepthCameraAndTactileSensorCustomer'}else{'DepthCameraAndTactileSensor'}
$appId=if($customer){'{782875EB-5F59-4117-AD83-F4BE96E724A3}'}else{'{E927DDB5-704F-4F63-AEC3-54D67D2B1177}'}
$windowTitles=@("触想工作台（$label）","触想工作台（$label） - 触想工作台")
$app=if($native){[IO.Path]::GetFullPath($NativeInstallDirectory)}else{Join-Path $env:ProgramFiles $appName}
$exe=Join-Path $app '触想工作台.exe'
$shortcut=Join-Path $env:PUBLIC "Desktop/$appName.lnk"
$uninstallKeys=@('HKLM','HKCU' | ForEach-Object {"$($_):\Software\Microsoft\Windows\CurrentVersion\Uninstall\$($appId)_is1"})
$installAttempted=$false
$env:RV_TEST_OFFLINE_ONLY='1'
$env:PATH="$env:SystemRoot\System32;$env:SystemRoot"
$checks=[ordered]@{}
$checks.edition=$Edition
$checks.environment=if($native){'Windows host'}else{'Windows Sandbox'}
$checks.scope=if($native){'isolated-install-offline-tests-uninstall'}elseif($SmokeOnly){'install-start-close-uninstall'}else{'full-offline-and-lifecycle'}
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
function InstallerArguments([string]$logName){
    return '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="{0}"' -f (Join-Path $logs "$logName.log")
}
try {
    $setups=@(Get-ChildItem -LiteralPath $source -Filter "触想工作台-$label-v*-x64.exe" -File)
    Check 'single_setup' ($setups.Count -eq 1)
    $setup=$setups[0].FullName
    $expectedHash=(Get-Content -LiteralPath "$setup.sha256" -First 1).Split(' ')[0]
    $checks.setup_sha256=(Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
    Check 'setup_checksum_matches' ($expectedHash -ceq $checks.setup_sha256)
    Check 'isolated_clean_install' (-not (Test-Path -LiteralPath $app) -and -not (Test-Path -LiteralPath $shortcut) -and
        @($uninstallKeys | Where-Object {Test-Path -LiteralPath $_}).Count -eq 0)
    $installArgs=(InstallerArguments 'install') + (' /DIR="{0}"' -f $app)
    $installAttempted=$true
    Check 'install_exit_0' ((Run $setup $installArgs 300) -eq 0)
    Check 'desktop_installed' (Test-Path -LiteralPath $exe)
    $expected=Get-Content -LiteralPath (Join-Path $source 'desktop-imaging.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Check 'compiled_edition_report' ((Run $exe ('--edition-info "{0}"' -f (Join-Path $logs 'edition.json')) 30) -eq 0)
    $info=Get-Content -LiteralPath "$logs\edition.json" -Raw -Encoding UTF8 | ConvertFrom-Json
    Check 'compiled_edition_matches_installer' ($info.edition -ceq $Edition -and $expected.edition -ceq $Edition -and
        ($info.imaging_schemes -join ',') -ceq ($expected.imaging_schemes -join ',') -and
        $info.default_imaging_scheme -ceq $expected.default_imaging_scheme -and $info.measurement_capture -eq $expected.measurement_capture)
    Check 'installed_desktop_sha256' ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -eq $expected.files.'触想工作台.exe')
    Check 'windows_platform_plugin' (Test-Path (Join-Path $app 'platforms\qwindows.dll'))
    Check 'application_local_vc_runtime' (Test-Path (Join-Path $app 'VCRUNTIME140.dll'))
    Check 'desktop_shortcut' ((Test-Path -LiteralPath $shortcut) -and
        (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut).TargetPath -eq $exe)
    $forbidden=@(Get-ChildItem -LiteralPath $app -Recurse -File | Where-Object {
        $_.Extension -match '^\.(onnx|ovm|engine|safetensors|dctsm|pdb|rknn|py)$' -or
            $_.Name -match '^(README|AGENTS)(\..*)?$|^(PackModelCopy|DctsDepthPro2Worker)\.exe$|^(DctsDepthPro2Runtime|nvinfer.*|cudart64_.*)\.dll$|model_key_material'
    })
    Check 'no_plaintext_models_or_development_files' ($forbidden.Count -eq 0)
    Check 'no_bundled_inference_models' (@(Get-ChildItem -LiteralPath $app -Directory | Where-Object {
        $_.Name -match '^(UniDepthRuntime.*|DepthPro2Runtime)$'
    }).Count -eq 0)
    Check 'sdk_3_1_0_abi_3' ($info.sdk_version -ceq '3.1.0' -and $info.sdk_abi -eq 3)
    Check 'installed_sdk_runtime' (Test-Path (Join-Path $app 'CxwlDepthSdk.dll'))
    Check 'installed_sdk_decoder' (Test-Path (Join-Path $app 'cxwl-depth-sdk/avformat-61.dll'))
    $retired=@(Get-ChildItem -LiteralPath $app -Recurse -Directory | Where-Object {$_.Name -in 'rv_control','platform-tools','vlc'})
    Check 'usb_direct_without_legacy_runtime' ($retired.Count -eq 0 -and
        -not (Get-ChildItem -LiteralPath $app -Recurse -File | Where-Object {$_.Name -in 'RvControlWorker.exe','adb.exe','libssh2.dll'}))
    if(-not $SmokeOnly){
        foreach($flag in 'port-ui-smoke-test','tactile-serial-test','tactile-visualization-test','tactile-3d-test',
            'tactile-render-test','rv-assist-recovery-test','rv-tuner-test','rv-usb-enrollment-test'){
            $exitCode=Run $exe ('--{0} "{1}"' -f $flag,(Join-Path $logs "$flag.txt")) 240
            $report=Get-Content -LiteralPath "$logs\$flag.txt" -Raw
            $assertions=@($report -split "`r?`n" | Where-Object {$_ -notmatch '^RESULT ' -and $_ -match '(?:^PASS\b|\bPASS$)'})
            Check $flag ($exitCode -eq 0 -and $assertions.Count -gt 0 -and $report -notmatch '\bFAIL\b')
            $checks["${flag}_assertions"]=$assertions.Count
        }
        $ui=Get-Content -LiteralPath "$logs\port-ui-smoke-test.txt" -Raw
        $uiChecks=@('EXACT_IMAGING_SCHEMES','OTHER_IMAGING_SCHEMES_ABSENT','FIRST_RUN_USES_AVAILABLE_DEFAULT')
        if($customer){
            $uiChecks+=@('CUSTOMER_HOST_RUNTIME_DISABLED','CUSTOMER_HOST_DIAGNOSTICS_ABSENT','CUSTOMER_SAVED_SCHEME_FALLBACK_host_assisted_depth')
        }else{
            $uiChecks+=@('UNAVAILABLE_HOST_DEFAULTS_TO_ORDINARY_BOARD','UNAVAILABLE_HOST_MENU_IS_DISABLED_WITH_REASON')
        }
        foreach($name in $uiChecks){
            Check $name ($ui -match "(?m)^$name PASS\s*$")
        }
    }

    if(-not $native){
        # This is the foreground GUI under test inside the disposable guest.
        # Hidden is appropriate for the helpers above, but prevents checking the
        # ordinary visible window and its normal interactive close action.
        $process=Start-Process -FilePath $exe -WorkingDirectory $env:TEMP -WindowStyle Normal -PassThru
        try {
            $end=[DateTime]::UtcNow.AddSeconds(40)
            do {
                Start-Sleep -Milliseconds 250
                $process.Refresh()
            } until ($process.HasExited -or ($process.MainWindowHandle -ne 0 -and $process.MainWindowTitle -in $windowTitles) -or [DateTime]::UtcNow -gt $end)
            $checks.ordinary_window_title=$process.MainWindowTitle
            Check 'ordinary_start_main_window' (-not $process.HasExited -and $process.MainWindowHandle -ne 0)
            Check 'ordinary_start_chinese_title' ($process.MainWindowTitle -in $windowTitles)
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
            $prefs=Join-Path $env:LOCALAPPDATA "$profileName\config\preferences\DepthCameraAndTactileSensor\DepthCameraAndTactileSensor.ini"
            Check 'preferences_in_local_appdata' (Test-Path -LiteralPath $prefs)
            Check 'first_run_zh_cn' ([bool](Select-String -LiteralPath $prefs -SimpleMatch 'language=zh_CN'))
            $schemePattern='^imagingScheme='+[Regex]::Escape($expected.default_imaging_scheme)+'$'
            Check 'first_run_imaging_preference' ([bool](Select-String -LiteralPath $prefs -Pattern $schemePattern))
            Check 'config_outside_install_directory' (-not (Test-Path (Join-Path $app 'config')))
            Check 'close_main_window' ($process.CloseMainWindow())
            Check 'ordinary_exit_0' ($process.WaitForExit(30000) -and $process.ExitCode -eq 0)
        } finally {
            if(-not $process.HasExited){$process.Kill()}
        }
        if(-not $SmokeOnly){
            Check 'reinstall_exit_0' ((Run $setup (InstallerArguments 'reinstall') 300) -eq 0)
            Check 'reinstall_preserves_preferences' (Test-Path -LiteralPath $prefs)
        }
    }
    $uninstall=Join-Path $app 'unins000.exe'
    Check 'uninstall_exit_0' ((Run $uninstall (InstallerArguments 'uninstall') 300) -eq 0)
    Check 'uninstalled_executable_removed' (-not (Test-Path -LiteralPath $exe))
    Check 'desktop_shortcut_removed' (-not (Test-Path -LiteralPath $shortcut))
    Check 'uninstall_registration_removed' (@($uninstallKeys | Where-Object {Test-Path -LiteralPath $_}).Count -eq 0)
    if(-not $native){Check 'user_preferences_preserved' (Test-Path -LiteralPath $prefs)}
    $checks.passed=(@($checks.Values | Where-Object {$_ -eq $false}).Count -eq 0)
} catch {
    $checks.passed=$false
    $checks.error=$_.Exception.Message
} finally {
    # Even a failed assertion must leave the disposable VM uninstalled and
    # preserve evidence. Never turn a failed run into a pass after cleanup.
    $cleanupUninstaller=Join-Path $app 'unins000.exe'
    if($installAttempted -and $checks.passed -ne $true -and (Test-Path -LiteralPath $cleanupUninstaller)){
        try {
            $checks.failure_cleanup_uninstall_exit=Run $cleanupUninstaller (InstallerArguments 'failure-cleanup') 300
            $checks.failure_cleanup_removed_executable=-not (Test-Path -LiteralPath $exe)
        } catch {$checks.failure_cleanup_error=$_.Exception.Message}
    }
    $resultName=if($native){'windows-result.json'}else{'sandbox-result.json'}
    $checks | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $logs $resultName)
}
