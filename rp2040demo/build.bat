@echo off
REM ================================================================
REM  RP2040 Demo Build / Flash Script
REM
REM  Usage:
REM    build.bat              Build incrementally
REM    build.bat clean        Delete build/ then reconfigure + build
REM    build.bat flash        Build + auto-flash (Pico in BOOTSEL)
REM    build.bat just-flash   Skip compile, only flash (BOOTSEL)
REM    build.bat /nowait      Do not pause at the end (scripted use)
REM ================================================================
setlocal

set "ARG1=%1"
set "ARG2=%2"

set "DO_CLEAN=0"
set "DO_FLASH=0"
set "SKIP_BUILD=0"
set "NO_WAIT=0"

if /i "%ARG1%"=="clean"        set DO_CLEAN=1
if /i "%ARG1%"=="flash"        set DO_FLASH=1
if /i "%ARG1%"=="just-flash" ( set DO_FLASH=1 & set SKIP_BUILD=1 )
if /i "%ARG1%"=="/nowait"      set NO_WAIT=1
if /i "%ARG2%"=="/nowait"      set NO_WAIT=1

REM -------- Fixed paths --------
set "PICO_SDK_PATH=E:\ppCD\pico-sdk"
set "TOOLCHAIN_BIN=C:\Users\master\.platformio\packages\toolchain-gccarmnoneeabi\bin"
set "CMAKE_BIN=C:\Program Files\CMake\bin"
set "NINJA_BIN=C:\Users\master\AppData\Local\Programs\Python\Python312\Scripts"
set "PROJECT_ROOT=%~dp0"
if "%PROJECT_ROOT:~-1%"=="\" set "PROJECT_ROOT=%PROJECT_ROOT:~0,-1%"
REM NOTE: "%PROJECT_ROOT%\build" has an explicit backslash so that the
REM output lands inside the source tree as "rp2040demo\build", not a
REM confusing sibling directory called "rp2040demobuild".
set "BUILD_DIR=%PROJECT_ROOT%\build"
set "UF2=%BUILD_DIR%\rp2040demo.uf2"
set "MINIK_DIR=%BUILD_DIR%\minik-build"
set "UF2_LED=%MINIK_DIR%\minimal_led_test.uf2"
set "UF2_USB=%MINIK_DIR%\usb_print_test.uf2"
set "TOOLCHAIN_FILE=%PROJECT_ROOT%\..\mini-kernel\toolchain-arm-none-eabi.cmake"
set "LOG=%BUILD_DIR%\build.log"

cd /d "%PROJECT_ROOT%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

REM -------- Start log --------
>  "%LOG%" echo ================================================================
>> "%LOG%" echo RP2040 Demo Build Run - %date% %time%
>> "%LOG%" echo Args = [%ARG1%] [%ARG2%]
>> "%LOG%" echo ================================================================

REM -------- Sanity checks --------
set ERR=0
if not exist "%PICO_SDK_PATH%\pico_sdk_init.cmake" (
    >> "%LOG%" echo ERROR: PICO_SDK_PATH invalid = %PICO_SDK_PATH%
    set ERR=1
)
if not exist "%TOOLCHAIN_BIN%\arm-none-eabi-gcc.exe" (
    >> "%LOG%" echo ERROR: toolchain missing at %TOOLCHAIN_BIN%
    set ERR=1
)
if not exist "%CMAKE_BIN%\cmake.exe" (
    >> "%LOG%" echo ERROR: cmake missing at %CMAKE_BIN%
    set ERR=1
)
if not exist "%NINJA_BIN%\ninja.exe" (
    >> "%LOG%" echo ERROR: ninja missing at %NINJA_BIN%
    set ERR=1
)
if %ERR% NEQ 0 goto FAIL

PATH %TOOLCHAIN_BIN%;%CMAKE_BIN%;%NINJA_BIN%;%PATH%

REM -------- Clean --------
if %DO_CLEAN% EQU 1 (
    >> "%LOG%" echo [CLEAN] Removing %BUILD_DIR%
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
)

REM -------- Configure --------
if %SKIP_BUILD% EQU 1 goto after_build
if exist "%BUILD_DIR%\build.ninja" goto skip_config
call :RUN_CMAKE_CONFIG
if errorlevel 1 goto FAIL
:skip_config
call :RUN_CMAKE_BUILD
if errorlevel 1 goto FAIL
:after_build

REM -------- Artifact check --------
if not exist "%UF2%" (
    >> "%LOG%" echo ERROR: UF2 missing - %UF2%
    goto FAIL
)
for %%F in ("%UF2%") do >> "%LOG%" echo BUILD OK - UF2 %%~zF bytes @ %%~fF

REM -------- Also build + publish diagnostic firmwares to build root --------
if exist "%UF2_LED%" (
    copy /y "%UF2_LED%" "%BUILD_DIR%\diagnostic_minimal_led_test.uf2" >> "%LOG%" 2>&1
    >> "%LOG%" echo [PUBLISH] diagnostic_minimal_led_test.uf2 (pure SDK LED blink, HW verify)
) else (
    >> "%LOG%" echo [WARN] diagnostic_minimal_led_test.uf2 not built (build.ninja target out of date? run with 'clean')
)
if exist "%UF2_USB%" (
    copy /y "%UF2_USB%" "%BUILD_DIR%\diagnostic_usb_print_test.uf2" >> "%LOG%" 2>&1
    >> "%LOG%" echo [PUBLISH] diagnostic_usb_print_test.uf2 (pure SDK USB CDC print, USB verify)
) else (
    >> "%LOG%" echo [WARN] diagnostic_usb_print_test.uf2 not built (build.ninja target out of date? run with 'clean')
)

REM -------- Flash step --------
if %DO_FLASH% NEQ 1 goto OK

>> "%LOG%" echo.
>> "%LOG%" echo [FLASH] Scanning drives for Pico BOOTSEL marker INFO_UF2.TXT
set DFOUND=
if exist "C:\INFO_UF2.TXT" set DFOUND=C:
if not defined DFOUND if exist "D:\INFO_UF2.TXT" set DFOUND=D:
if not defined DFOUND if exist "E:\INFO_UF2.TXT" set DFOUND=E:
if not defined DFOUND if exist "F:\INFO_UF2.TXT" set DFOUND=F:
if not defined DFOUND if exist "G:\INFO_UF2.TXT" set DFOUND=G:
if not defined DFOUND if exist "H:\INFO_UF2.TXT" set DFOUND=H:
if not defined DFOUND (
    >> "%LOG%" echo ERROR: Pico BOOTSEL drive not found. Plug Pico in BOOTSEL mode then retry.
    goto FAIL
)
>> "%LOG%" echo [FLASH] Found Pico BOOTSEL at %DFOUND%
copy /y "%UF2%" "%DFOUND%\" >> "%LOG%" 2>&1
if errorlevel 1 (
    >> "%LOG%" echo ERROR: Failed to copy UF2 to %DFOUND%
    goto FAIL
)
>> "%LOG%" echo [FLASH] Copy completed, Pico will reboot automatically.

:OK
>> "%LOG%" echo.
>> "%LOG%" echo ===== BUILD SUCCESS =====
echo ===== BUILD SUCCESS =====
if %NO_WAIT% EQU 0 pause
endlocal
exit /b 0

:FAIL
echo.
echo ===== BUILD FAILED =====
echo Last 40 lines of %LOG%:
powershell -NoProfile -Command "Get-Content '%LOG%' -Tail 40"
if %NO_WAIT% EQU 0 pause
endlocal
exit /b 1

REM -------- Subroutine: Run CMake Configure --------
:RUN_CMAKE_CONFIG
>> "%LOG%" echo [CMAKE CONFIG] Configuring RP2040 Demo...
cd /d "%BUILD_DIR%"
"%CMAKE_BIN%\cmake.exe" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DPICO_SDK_PATH="%PICO_SDK_PATH%" ^
    -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN_FILE%" ^
    "%PROJECT_ROOT%" >> "%LOG%" 2>&1
exit /b %errorlevel%

REM -------- Subroutine: Run CMake Build --------
:RUN_CMAKE_BUILD
>> "%LOG%" echo [CMAKE BUILD] Building RP2040 Demo + diagnostic firmwares...
cd /d "%BUILD_DIR%"
"%NINJA_BIN%\ninja.exe" rp2040demo minimal_led_test usb_print_test >> "%LOG%" 2>&1
exit /b %errorlevel%
