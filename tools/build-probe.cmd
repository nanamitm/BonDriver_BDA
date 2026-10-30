@echo off
setlocal
set "probeOutput=%~1"
if not defined probeOutput set "probeOutput=work\bon-driver-probe.exe"
pushd "%~dp0.."
if not exist work mkdir work
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
cl.exe /nologo /EHsc /std:c++17 /MT /DUNICODE /D_UNICODE /IBonDriver_BDA tools\bon-driver-probe.cpp /Fe:"%probeOutput%" /Fo:work\bon-driver-probe.obj
set probeBuildExit=%errorlevel%
popd
exit /b %probeBuildExit%
