@echo off
rem Builds the bridge with MSVC (Visual Studio 2022/2026 "Desktop development with C++").
rem   out\vft-papertracker-bridge.exe     console version (status window, --list/--dump/--stop)
rem   out\vft-papertracker-bridge-bg.exe  windowless version used by the logon task
setlocal
set "HERE=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VCINSTALLDIR (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VCINSTALLDIR (
  if not defined VSDIR ( echo Visual Studio C++ tools not found & exit /b 1 )
  call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
set "SRC=%HERE%src\main.cpp %HERE%src\vft_camera.cpp %HERE%src\vft_xu.cpp %HERE%src\pt_udp.cpp %HERE%src\jpeg_wic.cpp %HERE%src\log.cpp"
set "CFLAGS=/nologo /std:c++17 /O2 /EHsc /W4 /permissive- /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
set "LIBS=mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib oleaut32.lib windowscodecs.lib ws2_32.lib user32.lib"
if not exist "%HERE%out\obj-con" mkdir "%HERE%out\obj-con"
if not exist "%HERE%out\obj-bg" mkdir "%HERE%out\obj-bg"

cl %CFLAGS% /Fo"%HERE%out\obj-con\\" %SRC% /Fe:"%HERE%out\vft-papertracker-bridge.exe" /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
cl %CFLAGS% /DBRIDGE_BACKGROUND /Fo"%HERE%out\obj-bg\\" %SRC% /Fe:"%HERE%out\vft-papertracker-bridge-bg.exe" /link /SUBSYSTEM:WINDOWS %LIBS% || exit /b 1
echo Built: %HERE%out\vft-papertracker-bridge.exe and vft-papertracker-bridge-bg.exe
