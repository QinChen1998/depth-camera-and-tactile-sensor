@echo off
setlocal
rem Use Windows PowerShell modules even when the build starts from PowerShell 7.
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules;%ProgramFiles%\WindowsPowerShell\Modules"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0prepare_depth_sdk.ps1" %*
exit /b %errorlevel%
