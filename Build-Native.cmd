@echo off
setlocal
set "source=%~dp0."
set "client=%~dp0..\hailstone-client"
set "imgui=%~dp0..\production\dependencies\imgui"
set "build=%~dp0build"
if defined HAILSTONE_SDK_CLIENT set "client=%HAILSTONE_SDK_CLIENT%"
if defined HAILSTONE_IMGUI_SOURCE set "imgui=%HAILSTONE_IMGUI_SOURCE%"

if not exist "%client%\sdk\hailstone_plugin.h" (
    echo Hailstone public SDK not found under "%client%".
    exit /b 1
)
if not exist "%imgui%\imgui.h" (
    echo Dear ImGui source not found at "%imgui%".
    exit /b 1
)

set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" exit /b 1
for /f "usebackq delims=" %%I in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "studio=%%I"
if not defined studio exit /b 1
call "%studio%\VC\Auxiliary\Build\vcvars64.bat" > nul
if errorlevel 1 exit /b %errorlevel%

if not exist "%build%" mkdir "%build%"
pushd "%build%"
if /i "%~1"=="test-existing" goto test
cl /nologo /c /MP /O2 /MD /EHsc /W4 /std:c++20 /permissive- /utf-8 /DNDEBUG ^
    /DIMGUI_DISABLE_OBSOLETE_FUNCTIONS /I"%client%\sdk" /I"%imgui%" ^
    "%source%\health_column.cpp" "%source%\retained_draw.cpp" "%imgui%\imgui.cpp" "%imgui%\imgui_draw.cpp" ^
    "%imgui%\imgui_tables.cpp" "%imgui%\imgui_widgets.cpp"
if errorlevel 1 exit /b %errorlevel%
link /nologo /DLL /MACHINE:X64 /INCREMENTAL:NO /OUT:ui-pack-native.dll ^
    health_column.obj retained_draw.obj imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj
if errorlevel 1 exit /b %errorlevel%
if /i not "%~1"=="test" goto done

:test
cl /nologo /O2 /MD /EHsc /W4 /std:c++20 /permissive- /utf-8 /DNDEBUG ^
    /DIMGUI_DISABLE_OBSOLETE_FUNCTIONS /I"%client%\sdk" /I"%imgui%" ^
    "%source%\retained_draw_test.cpp" imgui.obj imgui_draw.obj ^
    imgui_tables.obj imgui_widgets.obj /Fe:retained_draw_test.exe
if errorlevel 1 exit /b %errorlevel%
retained_draw_test.exe
if errorlevel 1 exit /b %errorlevel%

:done
popd
exit /b 0
