@echo off
chcp 65001 >nul
setlocal
if exist "%~dp0x64\Release\Qt6Multimedia.dll" if exist "%~dp0x64\Release\platforms\qwindows.dll" goto local_runtime
set "DCTS_QT_ROOT="
for %%Q in ("E:\Qt\6.11.1\msvc2022_64" "D:\Qt\6.11.1\msvc2022_64") do if not defined DCTS_QT_ROOT if exist "%%~Q\bin\Qt6Multimedia.dll" if exist "%%~Q\plugins\platforms\qwindows.dll" set "DCTS_QT_ROOT=%%~Q"
if not defined DCTS_QT_ROOT (
    echo Qt 6.11.1 MSVC x64 runtime not found. 1>&2
    exit /b 1
)
set "PATH=%DCTS_QT_ROOT%\bin;%SystemRoot%\System32;%SystemRoot%"
set "QT_PLUGIN_PATH=%DCTS_QT_ROOT%\plugins"
set "QML_IMPORT_PATH=%DCTS_QT_ROOT%\qml"
set "QML2_IMPORT_PATH=%DCTS_QT_ROOT%\qml"
goto launch
:local_runtime
set "PATH=%~dp0x64\Release;%SystemRoot%\System32;%SystemRoot%"
set "QT_PLUGIN_PATH=%~dp0x64\Release"
set "QML_IMPORT_PATH=%~dp0x64\Release\qml"
set "QML2_IMPORT_PATH=%~dp0x64\Release\qml"
:launch
cd /d "%~dp0"
rem The default Release build selects the workspace TensorRT-RTX runtime.
start "" "%~dp0x64\Release\触想工作台.exe" %*
endlocal
