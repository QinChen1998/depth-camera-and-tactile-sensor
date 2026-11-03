@echo off
setlocal
rem PowerShell 7 may export a module path that Windows PowerShell 5 cannot load.
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules;%ProgramFiles%\WindowsPowerShell\Modules"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0rv_control\build.ps1" -Configuration "%~1" -QtRoot "%~2" -MSBuild "%~3"
exit /b %errorlevel%
