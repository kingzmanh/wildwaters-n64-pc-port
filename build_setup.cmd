@echo off
rem Builds "WildWaters-PC-Setup.exe" (host\installer\setup.c) from the window build (build.cmd first).
rem   build_setup.cmd   ->  dist\WildWaters-PC-Setup.exe
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with the C++ x64 tools was not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "ROOT=%~dp0"
set "CRT="
for /d %%d in ("%VCToolsRedistDir%x64\Microsoft.VC14*.CRT") do set "CRT=%%d"
if not defined CRT (echo The Visual C++ redistributable folder was not found & exit /b 1)
python -I "%ROOT%tools\make_payload.py" "%ROOT%host\build-win" "%ROOT%host\installer\payload.bin" "%CRT%" || exit /b 1
if not exist "%ROOT%dist" mkdir "%ROOT%dist"
pushd "%ROOT%host\installer"
rc /nologo /fo setup.res setup.rc || (popd & exit /b 1)
cl /nologo /O1 /MT /W3 /I "%ROOT%..\..\Tools\N64 tools\N64ModernRuntime\thirdparty\xxHash" setup.c setup.res /Fe:"%ROOT%dist\WildWaters-PC-Setup.exe" /link /SUBSYSTEM:WINDOWS comctl32.lib shell32.lib ole32.lib uuid.lib shlwapi.lib advapi32.lib user32.lib gdi32.lib || (popd & exit /b 1)
del setup.obj setup.res 2>nul
popd
echo Built "%ROOT%dist\WildWaters-PC-Setup.exe"
