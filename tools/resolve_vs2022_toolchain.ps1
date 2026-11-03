# Resolve paths only. Never install components, run vcvars or change PATH.
function Resolve-Vs2022Toolchain {
    param(
        [string]$MSBuild='',
        [string]$DumpBin='',
        [string]$VsWhere='',
        [switch]$RequireMSBuild,
        [switch]$RequireDumpBin,
        [switch]$RequireRedist
    )
    $vsRoot=''
    if($MSBuild){
        if(-not (Test-Path -LiteralPath $MSBuild -PathType Leaf)){throw "MSBuild missing: $MSBuild"}
        $MSBuild=(Resolve-Path -LiteralPath $MSBuild).Path
        if($MSBuild -notmatch '(?i)[\\/]MSBuild[\\/]Current[\\/]Bin(?:[\\/]amd64)?[\\/]MSBuild\.exe$'){
            throw "Expected a VS 2022 MSBuild/Current/Bin[/amd64]/MSBuild.exe path: $MSBuild"
        }
        $vsRoot=$MSBuild -replace '(?i)[\\/]MSBuild[\\/]Current[\\/]Bin(?:[\\/]amd64)?[\\/]MSBuild\.exe$',''
    }
    if($DumpBin){
        if(-not (Test-Path -LiteralPath $DumpBin -PathType Leaf)){throw "dumpbin missing: $DumpBin"}
        $DumpBin=(Resolve-Path -LiteralPath $DumpBin).Path
        if(-not $vsRoot -and $DumpBin -match '(?i)[\\/]VC[\\/]Tools[\\/]MSVC[\\/]14\.[^\\/]+[\\/]bin[\\/]Hostx64[\\/]x64[\\/]dumpbin\.exe$'){
            $vsRoot=$DumpBin -replace '(?i)[\\/]VC[\\/]Tools[\\/]MSVC[\\/]14\.[^\\/]+[\\/]bin[\\/]Hostx64[\\/]x64[\\/]dumpbin\.exe$',''
        }
    }
    # A standalone explicit dumpbin needs no Visual Studio discovery unless
    # deployment also requires that installation's MSBuild or VC143 redist.
    if(-not $vsRoot -and ($RequireMSBuild -or $RequireRedist -or ($RequireDumpBin -and -not $DumpBin))){
        if(-not $VsWhere){
            $vsInstallerRoot=[Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
            if(-not $vsInstallerRoot){$vsInstallerRoot=[Environment]::GetEnvironmentVariable('ProgramFiles')}
            if($vsInstallerRoot){$VsWhere=Join-Path $vsInstallerRoot 'Microsoft Visual Studio/Installer/vswhere.exe'}
        }
        if(-not $VsWhere -or -not (Test-Path -LiteralPath $VsWhere -PathType Leaf)){throw 'VS 2022 vswhere is required; supply -MSBuild or an explicit -DumpBin for SDK preparation.'}
        $vsRoots=@(& $VsWhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
        if($LASTEXITCODE -or $vsRoots.Count -ne 1 -or -not (Test-Path -LiteralPath $vsRoots[0] -PathType Container)){throw 'VS 2022 with v143 x64 C++ tools was not found.'}
        $vsRoot=(Resolve-Path -LiteralPath $vsRoots[0]).Path
    }
    if($RequireMSBuild -and -not $MSBuild){
        $MSBuild=Join-Path $vsRoot 'MSBuild/Current/Bin/MSBuild.exe'
        if(-not (Test-Path -LiteralPath $MSBuild -PathType Leaf)){throw "MSBuild missing: $MSBuild"}
    }
    if($RequireDumpBin -and -not $DumpBin){
        $vsCompilerRoot=Join-Path $vsRoot 'VC/Tools/MSVC'
        if(Test-Path -LiteralPath $vsCompilerRoot -PathType Container){
            foreach($vsCompiler in (Get-ChildItem -LiteralPath $vsCompilerRoot -Directory | Where-Object {$_.Name -match '^14\.\d+\.\d+$'} | Sort-Object {[version]$_.Name} -Descending)){
                $vsCandidate=Join-Path $vsCompiler.FullName 'bin/Hostx64/x64/dumpbin.exe'
                if(Test-Path -LiteralPath $vsCandidate -PathType Leaf){$DumpBin=$vsCandidate;break}
            }
        }
        if(-not $DumpBin){throw "VS 2022 v143 x64 dumpbin is required: $vsRoot"}
    }
    $vsRedistDirectories=@()
    if($RequireRedist){
        $vsRedistRoot=Join-Path $vsRoot 'VC/Redist/MSVC'
        if(Test-Path -LiteralPath $vsRedistRoot -PathType Container){
            foreach($vsRedist in (Get-ChildItem -LiteralPath $vsRedistRoot -Directory | Where-Object {$_.Name -match '^14\.\d+\.\d+$'} | Sort-Object {[version]$_.Name} -Descending)){
                $vsCandidate=Join-Path $vsRedist.FullName 'x64/Microsoft.VC143.CRT'
                if(Test-Path -LiteralPath $vsCandidate -PathType Container){$vsRedistDirectories+=$vsCandidate}
            }
        }
        if(-not $vsRedistDirectories.Count){throw "VS 2022 x64 VC143 CRT redistributable directory is required: $vsRoot"}
    }
    [pscustomobject]@{VisualStudioRoot=$vsRoot;MSBuild=$MSBuild;DumpBin=$DumpBin;RedistDirectories=$vsRedistDirectories}
}

# Evaluate the same early Qt defaults used by the VS projects. An explicit
# argument is never replaced with another installation when it is invalid.
function Resolve-Qt611Root {
    param([string]$QtRoot='', [string]$MSBuild='')
    if($QtRoot){return [IO.Path]::GetFullPath($QtRoot)}
    $qtBuildTool=(Resolve-Vs2022Toolchain -MSBuild $MSBuild -RequireMSBuild).MSBuild
    $qtDefaults=Join-Path $PSScriptRoot 'QtDevelopment.props'
    $qtEvaluation=@(& $qtBuildTool $qtDefaults -nologo -getProperty:QtInstall)
    if($LASTEXITCODE){throw "Qt installation evaluation failed: $qtDefaults"}
    $qtResolved=($qtEvaluation -join [Environment]::NewLine).Trim()
    if(-not $qtResolved){throw 'Qt 6.11.1 MSVC x64 was not found. Set QtInstall in QtDevelopment.local.props or supply -QtRoot.'}
    return [IO.Path]::GetFullPath($qtResolved)
}

# Protect shared worker intermediates and deployed closures across IDE and
# command-line builds. Named mutexes leave no stale files after process exit.
function Enter-NativeBuildLock {
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][ValidateSet('all','handpose','dex','rv')][string]$Component,
        [Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration,
        [ValidateRange(1,1800)][int]$TimeoutSeconds=300
    )
    $buildIdentity=([IO.Path]::GetFullPath($ProjectRoot).TrimEnd([char[]]'\/')+'|'+$Component+'|'+$Configuration).ToLowerInvariant()
    $buildHasher=[Security.Cryptography.SHA256]::Create()
    try {$buildKey=[BitConverter]::ToString($buildHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($buildIdentity))).Replace('-','')}
    finally {$buildHasher.Dispose()}
    $buildMutex=[Threading.Mutex]::new($false,('Local\DexterousHandBuild_'+$buildKey))
    try {
        $buildWait=[Diagnostics.Stopwatch]::StartNew()
        try {$buildOwned=$buildMutex.WaitOne(0)} catch [Threading.AbandonedMutexException] {$buildOwned=$true}
        if(-not $buildOwned){Write-Host "Waiting for the existing $Component $Configuration native build to finish..."}
        while(-not $buildOwned -and $buildWait.Elapsed.TotalSeconds -lt $TimeoutSeconds){
            try {$buildOwned=$buildMutex.WaitOne(1000)} catch [Threading.AbandonedMutexException] {$buildOwned=$true}
        }
        if(-not $buildOwned){throw "Timed out waiting for the $Component $Configuration native build. Finish the other build and retry."}
        return $buildMutex
    } catch {$buildMutex.Dispose();throw}
}

function Exit-NativeBuildLock {
    param([Parameter(Mandatory)][Threading.Mutex]$Mutex)
    try {$Mutex.ReleaseMutex()} finally {$Mutex.Dispose()}
}
