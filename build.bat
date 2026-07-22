@echo off
setlocal

:: Lumos build script (MSVC)
:: Usage: build.bat          (release, no logging)
::        build.bat debug    (debug, logging to lumos.log)

set DEFS=/D_UNICODE /DUNICODE
if /i "%1"=="debug" (
    echo Building Lumos [DEBUG]...
    set DEFS=%DEFS% /DDEBUG
) else (
    echo Building Lumos [RELEASE]...
)

:: Compile resource
rc /nologo lumos.rc
if errorlevel 1 (
    echo Resource compilation failed.
    exit /b 1
)

:: Compile and link
cl /nologo /O2 /W4 /WX- %DEFS% ^
   lumos.c monitor.c ui.c presets.c schedule.c wmibright.c ^
   lumos.res ^
   /Fe:lumos.exe ^
   /link /subsystem:windows ^
   dxva2.lib user32.lib gdi32.lib shell32.lib ^
   comctl32.lib advapi32.lib ole32.lib oleaut32.lib wbemuuid.lib ^
   dwmapi.lib wtsapi32.lib kernel32.lib

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo.
echo Build successful: lumos.exe
del /q *.obj 2>nul

endlocal
