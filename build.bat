@echo off
setlocal

:: Lumos build script (MSVC)
:: Usage: build.bat          (release, no logging)
::        build.bat debug    (debug, logging to %APPDATA%\Lumos\lumos-*.log)

if not exist build mkdir build

set DEFS=/D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0A00
if /i "%1"=="debug" (
    echo Building Lumos [DEBUG]...
    set DEFS=%DEFS% /DDEBUG
) else (
    echo Building Lumos [RELEASE]...
)

:: Compile resource
rc /nologo /i src /i res /fo build\lumos.res res\lumos.rc
if errorlevel 1 (
    echo Resource compilation failed.
    exit /b 1
)

:: Compile and link (all intermediates and the exe go to build\)
cl /nologo /O2 /W4 /WX- %DEFS% /I src ^
   src\lumos.c src\monitor.c src\brightmap.c src\ui.c src\ui_draw.c src\ui_popup.c ^
   src\ui_osd.c src\ui_menu.c src\ui_sched.c src\ui_settings.c src\ui_about.c src\ui_hass.c ^
   src\presets.c src\schedule.c src\hotkey.c src\a11y.c src\remote.c src\wmibright.c ^
   src\capture.c src\hass.c src\hassurl.c src\json.c src\ambient.c src\secret.c ^
   build\lumos.res ^
   /Fo"build\\" /Fe:build\lumos.exe ^
   /link /subsystem:windows ^
   dxva2.lib user32.lib gdi32.lib shell32.lib ^
   comctl32.lib advapi32.lib ole32.lib oleaut32.lib wbemuuid.lib ^
   dwmapi.lib wtsapi32.lib oleacc.lib winhttp.lib crypt32.lib uxtheme.lib kernel32.lib

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

:: lumosctl: console program for the command line
cl /nologo /O2 /W4 /WX- %DEFS% /I src src\lumosctl.c src\cliparse.c ^
   /Fo"build\\" /Fe:build\lumosctl.exe /link /subsystem:console user32.lib

if errorlevel 1 (
    echo lumosctl build failed.
    exit /b 1
)

echo.
echo Build successful: build\lumos.exe
del /q build\*.obj 2>nul

endlocal
