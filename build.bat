@echo off
rem Builds RHDLAA.asi (x86) and RHDLAA64.exe (x64) with Visual Studio 2019/2022 or Build Tools into bin\.
rem Needs the NVIDIA DLSS SDK (github.com/NVIDIA/DLSS): include\ and lib\Windows_x86_64\x64\nvsdk_ngx_s.lib in
rem third_party\ngx\include and third_party\ngx\lib, and lib\Windows_x86_64\rel\nvngx_dlss.dll in third_party\ngx\lib.
setlocal
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "delims=" %%i in ('vswhere -latest -products * -property installationPath') do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio / Build Tools not found & exit /b 1)
cd /d "%~dp0"
if not exist bin\RHDLAA mkdir bin\RHDLAA
if not exist obj\x86 mkdir obj\x86
if not exist obj\x64 mkdir obj\x64

setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
fxc /nologo /T vs_5_0 /E VS /O3 /Vn g_MotionVectorsVS /Fh obj\MotionVectorsVS.h src\MotionVectors.hlsl || exit /b 1
fxc /nologo /T ps_5_0 /E PS /O3 /Vn g_MotionVectorsPS /Fh obj\MotionVectorsPS.h src\MotionVectors.hlsl || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /DNDEBUG /W4 /LD /Iobj /Foobj\x86\ src\RHDLAA.cpp /Fe:bin\RHDLAA.asi /link /IMPLIB:obj\x86\RHDLAA.lib user32.lib d3d11.lib dxgi.lib || exit /b 1
endlocal

setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /DNDEBUG /W3 /Ithird_party\ngx\include /Foobj\x64\ src\RHDLAA64.cpp /Fe:bin\RHDLAA\RHDLAA64.exe /link /SUBSYSTEM:WINDOWS third_party\ngx\lib\nvsdk_ngx_s.lib d3d11.lib dxgi.lib user32.lib advapi32.lib ole32.lib shell32.lib version.lib || exit /b 1
endlocal

copy /y third_party\ngx\lib\nvngx_dlss.dll bin\RHDLAA\ >nul
copy /y RHDLAA.ini bin\ >nul
del /q bin\*.exp bin\RHDLAA\*.exp bin\RHDLAA\*.lib 2>nul
echo built bin\RHDLAA.asi and bin\RHDLAA\RHDLAA64.exe
