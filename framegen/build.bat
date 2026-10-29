@echo off
rem Builds RHFrameGen64.exe (x64), the frame generation helper, into bin\RHFrameGen\.
rem Needs NVIDIA Streamline 2.x (github.com/NVIDIA-RTX/Streamline, tested with 2.14.16) in third_party\streamline:
rem include\, lib\x64\sl.interposer.lib, and from bin\x64: sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll
rem sl.pcl.dll nvngx_dlssg.dll. Copy bin\RHFrameGen next to _gtaRenderHook.asi.
setlocal
set "SL=%~dp0third_party\streamline"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "delims=" %%i in ('vswhere -latest -products * -property installationPath') do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio / Build Tools not found & exit /b 1)
cd /d "%~dp0"
if not exist bin\RHFrameGen mkdir bin\RHFrameGen
if not exist obj mkdir obj
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem sl.interposer.lib instead of d3d12.lib/dxgi.lib: device, factory and swapchain come from Streamline
cl /nologo /O2 /MT /EHsc /std:c++17 /DNDEBUG /W4 /I"%SL%\include" /Foobj\ src\RHFrameGen64.cpp /Fe:bin\RHFrameGen\RHFrameGen64.exe /link /SUBSYSTEM:WINDOWS "%SL%\lib\x64\sl.interposer.lib" user32.lib || exit /b 1
for %%f in (sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll) do copy /y "%SL%\bin\x64\%%f" bin\RHFrameGen\ >nul
copy /y RHFrameGen64.ini bin\RHFrameGen\ >nul
echo built bin\RHFrameGen\RHFrameGen64.exe (+ Streamline DLLs)
