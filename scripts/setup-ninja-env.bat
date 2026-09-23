@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWERE%" (
    echo ERROR: vswhere.exe not found
    exit /b 1
)

set "VS_INSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"

if not defined VS_INSTALL (
    echo ERROR: Could not find VS installation
    exit /b 1
)

echo Found VS: %VS_INSTALL%

call "%VS_INSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64

cmake --preset compile-commands
if !errorlevel!==0 (
    cmake --build --preset compile-commands
)