@echo off
setlocal
set "PODCASTER_BUILD_TARGET=%~dp0Podcaster.exe"
if not "%~1"=="" set "PODCASTER_BUILD_TARGET=%~f1"

if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    if errorlevel 1 exit /b 1
)

set "PODCASTER_BUILD_TEMP=%TEMP%\PodcasterBuild-%RANDOM%-%RANDOM%"
mkdir "%PODCASTER_BUILD_TEMP%" || exit /b 1
pushd "%~dp0src" || exit /b 1

rc.exe /nologo /fo "%PODCASTER_BUILD_TEMP%\resource.res" resource.rc
if errorlevel 1 goto build_failed

cl.exe /nologo /utf-8 /O2 /EHsc /std:c++20 /W3 /DUNICODE /D_UNICODE ^
  /Fo:"%PODCASTER_BUILD_TEMP%\main.obj" ^
  /Fe:"%PODCASTER_BUILD_TARGET%" ^
  main.cpp "%PODCASTER_BUILD_TEMP%\resource.res" ^
  user32.lib gdi32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib d2d1.lib dwrite.lib winmm.lib avrt.lib propsys.lib ^
  /link /SUBSYSTEM:WINDOWS
if errorlevel 1 goto build_failed

echo Build succeeded: "%PODCASTER_BUILD_TARGET%"
set "PODCASTER_BUILD_RESULT=0"
goto build_cleanup

:build_failed
set "PODCASTER_BUILD_RESULT=1"

:build_cleanup
popd
del /q "%PODCASTER_BUILD_TEMP%\main.obj" "%PODCASTER_BUILD_TEMP%\resource.res" 2>nul
rmdir "%PODCASTER_BUILD_TEMP%"
exit /b %PODCASTER_BUILD_RESULT%
