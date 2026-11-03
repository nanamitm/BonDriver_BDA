@echo off
setlocal
pushd "%~dp0.."
call "%~dp0vcvars.cmd"
if errorlevel 1 exit /b 1
set "stage=work\host-test"
set "objects=work\host-test-obj"
if exist "%stage%" rmdir /s /q "%stage%"
if not exist "%objects%" mkdir "%objects%"
mkdir "%stage%\BonDriverHost"
set "flags=/nologo /EHsc /std:c++17 /MT /W4 /WX /DUNICODE /D_UNICODE /IBonDriver_BDA /IBonDriverHost /Fo:%objects%\"
cl %flags% /LD BonDriverHost\BonDriverProxy.cpp ole32.lib /Fe:"%stage%\BonDriver_Mock.dll"
if errorlevel 1 exit /b 1
copy /y "%stage%\BonDriver_Mock.dll" "%stage%\BonDriver_NoInner.dll" >nul
cl %flags% BonDriverHost\BonDriverHost.cpp shell32.lib user32.lib /Fe:"%stage%\BonDriverHost\BonDriverHost.exe" /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1
cl %flags% /LD tests\bon-driver-host-mock.cpp /Fe:"%stage%\BonDriverHost\BonDriver_Mock.dll"
if errorlevel 1 exit /b 1
cl %flags% tests\bon-driver-host-test.cpp /Fe:work\bon-driver-host-test.exe
if errorlevel 1 exit /b 1
work\bon-driver-host-test.exe "%CD%\%stage%"
set testExit=%errorlevel%
popd
exit /b %testExit%
