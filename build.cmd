@echo off
rem Builds WildWaters PC with MSVC + Ninja.
rem   build.cmd            window build (RT64)        -> host\build-win\WildWatersPC.exe
rem   build.cmd headless   test build (no window)     -> host\build-headless\WildWatersPC.exe
rem RecompiledFuncs\ must exist (recomp.cmd). Builds use half the CPU threads.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with the C++ x64 tools was not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set /a JOBS=%NUMBER_OF_PROCESSORS%/2
if %JOBS% LSS 1 set JOBS=1
set "ROOT=%~dp0"
if /i "%1"=="headless" (
  cmake -S "%ROOT%host" -B "%ROOT%host\build-headless" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWW_RT64=OFF || exit /b 1
  cmake --build "%ROOT%host\build-headless" -j %JOBS% || exit /b 1
) else (
  cmake -S "%ROOT%host" -B "%ROOT%host\build-win" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
  cmake --build "%ROOT%host\build-win" -j %JOBS% || exit /b 1
)
