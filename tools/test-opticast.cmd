@echo off
setlocal
pushd "%~dp0.."
if not exist work mkdir work
call "%~dp0vcvars.cmd"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /MT /W4 /WX /DUNICODE /D_UNICODE /IBonDriver_BDA tools\bon-driver-list.cpp /Fe:work\bon-driver-list.exe /Fo:work\bon-driver-list.obj
if errorlevel 1 exit /b 1
pwsh -NoProfile -File tools\test-opticast.ps1
set testExit=%errorlevel%
popd
exit /b %testExit%
