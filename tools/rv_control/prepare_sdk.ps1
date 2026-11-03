param([string]$QtRoot='D:/Qt/6.11.1/msvc2022_64',[string]$DependencyRoot='', [string]$OpenSslRoot='', [string]$CMake='D:/Qt/Tools/CMake_64/bin/cmake.exe')
$ErrorActionPreference='Stop'
if(!(Test-Path -LiteralPath $CMake -PathType Leaf)){throw "CMake missing: $CMake"}
function Invoke-RvCMake([string[]]$Arguments,[string]$Log){
    # Windows PowerShell 5.1 surfaces native stderr warnings as ErrorRecords.
    # Keep the full log, but decide native command success by its exit code.
    $previousPreference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & $CMake @Arguments *> $Log
        return $LASTEXITCODE
    } finally {$ErrorActionPreference=$previousPreference}
}
$rvProject=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if(!$DependencyRoot){$DependencyRoot=Join-Path $rvProject 'x64/rv_control_sdk'}
if(!$OpenSslRoot){$OpenSslRoot=Join-Path $rvProject 'x64/dex_native_env/Library'}
$OpenSslRoot=(Resolve-Path -LiteralPath $OpenSslRoot).Path.Replace('\','/')
$rvLock=Get-Content (Join-Path $PSScriptRoot 'upstream.lock.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $DependencyRoot | Out-Null
$rvArchive=Join-Path $DependencyRoot 'libssh2-1.11.1.tar.gz'
if(!(Test-Path -LiteralPath $rvArchive)){Invoke-WebRequest $rvLock.libssh2.url -OutFile $rvArchive}
if((Get-FileHash -LiteralPath $rvArchive -Algorithm SHA256).Hash -ine $rvLock.libssh2.sha256){throw 'Pinned libssh2 source SHA256 mismatch'}
if((Get-Content (Join-Path $OpenSslRoot 'include/openssl/opensslv.h') | Select-String 'OPENSSL_VERSION_TEXT') -notmatch 'OpenSSL 3\.6\.4'){throw 'Use the locked OpenSSL 3.6.4 native SDK'}
$rvSource=Join-Path $DependencyRoot 'libssh2-1.11.1'
if(!(Test-Path -LiteralPath $rvSource)){& tar.exe -xf $rvArchive -C $DependencyRoot;if($LASTEXITCODE){throw 'libssh2 source extraction failed'}}
$rvBuild=Join-Path $DependencyRoot 'build'
$rvInstall=Join-Path $DependencyRoot 'install'
$rvConfigureArguments=@('-S',$rvSource,'-B',$rvBuild,'-G','Visual Studio 17 2022','-A','x64','-DCMAKE_SYSTEM_VERSION=10.0.22621.0','-DBUILD_SHARED_LIBS=ON','-DBUILD_STATIC_LIBS=OFF','-DBUILD_EXAMPLES=OFF','-DBUILD_TESTING=OFF','-DCRYPTO_BACKEND=OpenSSL','-DENABLE_ZLIB_COMPRESSION=OFF',"-DOPENSSL_INCLUDE_DIR=$OpenSslRoot/include","-DLIB_EAY_RELEASE:FILEPATH=$OpenSslRoot/lib/libcrypto.lib","-DLIB_EAY_DEBUG:FILEPATH=$OpenSslRoot/lib/libcrypto.lib","-DCMAKE_INSTALL_PREFIX=$rvInstall")
if(Invoke-RvCMake -Arguments $rvConfigureArguments -Log (Join-Path $DependencyRoot 'configure.log')){Get-Content (Join-Path $DependencyRoot 'configure.log') -Tail 20;throw 'libssh2 configuration failed'}
if(Invoke-RvCMake -Arguments @('--build',$rvBuild,'--config','Release','--target','install','--parallel','4') -Log (Join-Path $DependencyRoot 'build.log')){Get-Content (Join-Path $DependencyRoot 'build.log') -Tail 20;throw 'libssh2 build failed'}
Copy-Item -LiteralPath (Join-Path $OpenSslRoot 'include/openssl') -Destination (Join-Path $rvInstall 'include') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $OpenSslRoot 'lib/libcrypto.lib') -Destination (Join-Path $rvInstall 'lib') -Force
Copy-Item -LiteralPath (Join-Path $OpenSslRoot 'bin/libcrypto-3-x64.dll') -Destination (Join-Path $rvInstall 'bin') -Force
Copy-Item -LiteralPath (Join-Path $rvSource 'COPYING') -Destination (Join-Path $rvInstall 'LICENSE-libssh2') -Force
$rvOpenSslLicense=Join-Path $rvInstall 'LICENSE-OpenSSL'
if(!(Test-Path -LiteralPath $rvOpenSslLicense)){
 $rvLicenseCandidate=@(
  (Join-Path $rvProject 'x64/conda_pkgs/openssl-3.6.4-hf411b9b_0/info/licenses/LICENSE.txt'),
  (Join-Path $rvProject 'x64/dex_native_mamba/pkgs/https/conda.anaconda.org/conda-forge/win-64/openssl-3.6.4-hf411b9b_0/info/licenses/LICENSE.txt')
 ) | Where-Object {Test-Path -LiteralPath $_ -PathType Leaf} | Select-Object -First 1
 if(!$rvLicenseCandidate){throw 'Provide the pinned OpenSSL 3.6.4 LICENSE-OpenSSL in the native SDK.'}
 Copy-Item -LiteralPath $rvLicenseCandidate -Destination $rvOpenSslLicense
}
Write-Output "Pinned native SSH SDK: $rvInstall (no interpreter collected)"
