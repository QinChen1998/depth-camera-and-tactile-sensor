function Assert-RvBuildInputs {
    param([Parameter(Mandatory)][string]$ProjectRoot)
    $manifestPath=Join-Path $ProjectRoot 'tools/rv_control/build_inputs.json'
    if(-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)){
        throw 'RV build input manifest is missing: tools/rv_control/build_inputs.json. Restore the matching source and build-input package.'
    }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if($manifest.schema -ne 1 -or -not $manifest.files){throw 'Invalid RV build input manifest.'}
    $problems=New-Object 'System.Collections.Generic.List[string]'
    foreach($name in @('include/libssh2.h','include/openssl/crypto.h','lib/libssh2.lib','lib/libcrypto.lib',
                       'bin/libssh2.dll','bin/libcrypto-3-x64.dll','LICENSE-libssh2','LICENSE-OpenSSL')){
        $relative='x64/rv_control_sdk/install/'+$name
        if(-not (Test-Path -LiteralPath (Join-Path $ProjectRoot $relative) -PathType Leaf)){
            $problems.Add('Missing SDK file: '+$relative)
        }
    }
    $entries=@($manifest.files.PSObject.Properties)
    foreach($entry in $entries){
        $name=$entry.Name
        if($name -notmatch '^tools/(rv_capture/native/payload|rv_control/bootstrap)/[a-zA-Z0-9_.-]+$' -or
           $entry.Value -notmatch '^[0-9a-f]{64}$'){throw 'Invalid RV build input path or hash.'}
        $path=Join-Path $ProjectRoot $name
        if(-not (Test-Path -LiteralPath $path -PathType Leaf)){
            $problems.Add('Missing board file: '+$name)
        } elseif((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $entry.Value){
            $problems.Add('Board file version mismatch: '+$name)
        }
    }
    if($problems.Count){
        throw ("RV worker dependencies are incomplete or outdated.`n"+($problems -join "`n")+
            "`nRestore DCTS-$($manifest.version)-RV-build-inputs.zip into the source root for board files. "+
            'For missing SDK files, prepare the pinned libssh2/OpenSSL SDK with tools/rv_control/prepare_sdk.ps1. '+
            'A previous-version dependency package is not sufficient. No compilation or deployment was started.')
    }
}
