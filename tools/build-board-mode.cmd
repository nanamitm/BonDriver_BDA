@echo off
setlocal
pushd "%~dp0.."
if not exist work mkdir work
call "%~dp0vcvars.cmd"
if errorlevel 1 exit /b 1
cl.exe /nologo /EHsc /std:c++17 /MT /W3 /DUNICODE /D_UNICODE tools\tbs6812-board-mode.cpp /Fe:work\tbs6812-board-mode.exe /Fo:work\tbs6812-board-mode.obj
set buildExit=%errorlevel%
popd
exit /b %buildExit%
