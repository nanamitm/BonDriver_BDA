@echo off
rem Sets up the x64 compiler of the installed Visual Studio, preferring the 14.44 tools.
set "vsRoot="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vsRoot=%%i"
if not defined vsRoot exit /b 1
call "%vsRoot%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul 2>nul
if errorlevel 1 call "%vsRoot%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b %errorlevel%
