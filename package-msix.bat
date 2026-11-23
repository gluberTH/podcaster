@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT_DIR=%~dp0"
set "OUT_MSIX=%ROOT_DIR%Podcaster.msix"
if not "%~1"=="" set "OUT_MSIX=%~f1"

call "%ROOT_DIR%build.bat"
if errorlevel 1 exit /b 1

set "MAKEAPPX="
for /f "delims=" %%F in ('dir /b /on /a:d "C:\Program Files (x86)\Windows Kits\10\bin\10.*" 2^>nul') do (
    if exist "C:\Program Files (x86)\Windows Kits\10\bin\%%F\x64\makeappx.exe" (
        set "MAKEAPPX=C:\Program Files (x86)\Windows Kits\10\bin\%%F\x64\makeappx.exe"
    )
)
if not defined MAKEAPPX (
    for /f "delims=" %%F in ('where makeappx.exe 2^>nul') do (
        set "MAKEAPPX=%%F"
    )
)
if not defined MAKEAPPX (
    echo Error: makeappx.exe not found in Windows 10/11 SDK.
    exit /b 1
)

set "STAGE_DIR=%TEMP%\PodcasterMsixStage-%RANDOM%-%RANDOM%"
mkdir "%STAGE_DIR%\Assets" || exit /b 1

copy /y "%ROOT_DIR%Podcaster.exe" "%STAGE_DIR%\Podcaster.exe" >nul
if errorlevel 1 goto stage_failed

copy /y "%ROOT_DIR%msix\AppxManifest.xml" "%STAGE_DIR%\AppxManifest.xml" >nul
if errorlevel 1 goto stage_failed

for %%A in (
    StoreLogo.png
    Square44x44Logo.png
    Square71x71Logo.png
    Square150x150Logo.png
    Square310x310Logo.png
    Wide310x150Logo.png
) do (
    copy /y "%ROOT_DIR%msix\Assets\%%A" "%STAGE_DIR%\Assets\%%A" >nul
    if errorlevel 1 goto stage_failed
)

echo Using MakeAppx: "%MAKEAPPX%"
"%MAKEAPPX%" pack /d "%STAGE_DIR%" /p "%OUT_MSIX%" /o
set "PACK_RESULT=%ERRORLEVEL%"
goto cleanup

:stage_failed
echo Error: Failed to stage MSIX package files.
set "PACK_RESULT=1"

:cleanup
rmdir /s /q "%STAGE_DIR%" 2>nul
if "%PACK_RESULT%"=="0" (
    echo MSIX package created: "%OUT_MSIX%"
)
exit /b %PACK_RESULT%
