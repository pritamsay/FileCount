@echo off
setlocal
set "source=%~dp0"
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%p in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vs=%%p"
if not defined vs exit /b 1
call "%vs%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
set "build=%TEMP%\FileCount-Tests-%RANDOM%-%RANDOM%"
mkdir "%build%" || exit /b 1
pushd "%build%" || exit /b 1
cl /nologo /std:c++17 /W4 /O2 /EHsc /MT /DUNICODE /D_UNICODE "%source%count_worker_test.cpp" /link shell32.lib ole32.lib /OUT:count_worker_test.exe
if errorlevel 1 exit /b 1
count_worker_test.exe
set "result=%errorlevel%"
popd
exit /b %result%
