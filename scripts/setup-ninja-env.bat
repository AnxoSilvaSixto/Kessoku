@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found at "%VSWHERE%"
    exit /b 1
)

set "VS_INSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"

if not defined VS_INSTALL (
    echo ERROR: Could not find a Visual Studio installation with the C++ toolset
    exit /b 1
)

echo Found VS: %VS_INSTALL%

call "%VS_INSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed
    exit /b 1
)

cmake --preset compile-commands
if errorlevel 1 exit /b 1

cmake --build --preset compile-commands
if errorlevel 1 exit /b 1

echo Compile commands written to build\ninja\compile_commands.json
