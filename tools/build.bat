@echo off
rem build di WinRegProxy (x64 Debug)
setlocal

rem trova Visual Studio (piu' recente) via vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
set "MSBUILD=%VSPATH%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" goto :no_msbuild

rem packages NuGet legacy: se minhook manca, MSBuild fallisce con errore su minhook.targets
if not exist "%~dp0..\src\packages\minhook.1.3.3\build\native\minhook.targets" (
    echo ATTENZIONE: packages\minhook.1.3.3 non presente (packages.config non ripristinato)
)

"%MSBUILD%" "%~dp0..\src\WinRegProxy.slnx" -nologo -v:m
exit /b %errorlevel%

:no_vswhere
echo vswhere non trovato: %VSWHERE%
exit /b 1

:no_msbuild
echo MSBuild non trovato: %MSBUILD%
exit /b 1
