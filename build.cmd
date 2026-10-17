@echo off
setlocal
set "REFERENCE_DIAG_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%REFERENCE_DIAG_VSWHERE%" (
  echo Microsoft Visual Studio C++ Build Tools and vswhere are required.
  exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%REFERENCE_DIAG_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "REFERENCE_DIAG_VSROOT=%%I"
if not defined REFERENCE_DIAG_VSROOT exit /b 1
call "%REFERENCE_DIAG_VSROOT%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0"
if not exist build mkdir build
cl.exe /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /Fo"build\reference-test.obj" reference-test.cpp /Fe"reference-test.exe" /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT d3d11.lib dxgi.lib dcomp.lib user32.lib gdi32.lib
set "REFERENCE_DIAG_RESULT=%errorlevel%"
popd
exit /b %REFERENCE_DIAG_RESULT%
