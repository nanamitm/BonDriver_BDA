@echo off
setlocal
pushd "%~dp0.."
if not exist work mkdir work
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /MT /W4 /WX /IBdaSpecials\TBS6812 tests\tbs6812-test.cpp BdaSpecials\TBS6812\Cxd2857.cpp /Fe:work\tbs6812-test.exe /Fo:work\
if errorlevel 1 exit /b 1
work\tbs6812-test.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /MT /W4 /WX /IBdaSpecials\TBS6812 tests\tbs6812-routing-test.cpp /Fe:work\tbs6812-routing-test.exe /Fo:work\tbs6812-routing-test.obj
if errorlevel 1 exit /b 1
work\tbs6812-routing-test.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /MT /W4 /WX /IBdaSpecials\TBS6812 tests\tbs6812-board-mode-test.cpp /Fe:work\tbs6812-board-mode-test.exe /Fo:work\tbs6812-board-mode-test.obj
if errorlevel 1 exit /b 1
work\tbs6812-board-mode-test.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /MT /W4 /WX tests\tbs6812-identity-test.cpp /Fe:work\tbs6812-identity-test.exe /Fo:work\tbs6812-identity-test.obj
if errorlevel 1 exit /b 1
work\tbs6812-identity-test.exe "%CD%\x64\Release_Static\TBS6812.dll"
set testExit=%errorlevel%
popd
exit /b %testExit%
