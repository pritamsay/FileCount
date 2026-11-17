@echo off
setlocal
set "source=%~dp0"
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" exit /b 1
for /f "usebackq delims=" %%p in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vs=%%p"
if not defined vs exit /b 1
call "%vs%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
set "build=%TEMP%\ExplorerCounts-Build-%RANDOM%-%RANDOM%"
mkdir "%build%" || exit /b 1
pushd "%build%" || exit /b 1
rc /nologo /fo version.res "%source%version.rc" || exit /b 1
cl /nologo /std:c++17 /W4 /O2 /EHsc /MT /guard:cf /DUNICODE /D_UNICODE "%source%main.cpp" /link version.res /DYNAMICBASE /NXCOMPAT /GUARD:CF /SUBSYSTEM:WINDOWS shell32.lib propsys.lib oleaut32.lib ole32.lib uuid.lib user32.lib gdi32.lib advapi32.lib uiautomationcore.lib /OUT:"%source%..\ExplorerCounts.exe"
set "result=%errorlevel%"
popd
exit /b %result%
