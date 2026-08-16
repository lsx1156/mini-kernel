@echo off
REM ================================================================
REM  Mini Kernel - Pure Static Library Build Script
REM
REM  Usage:
REM    build.bat              Build incrementally
REM    build.bat clean        Delete build/ then reconfigure + build
REM    build.bat /nowait      Do not pause at the end (scripted use)
REM
REM  NOTE: This is a pure kernel STATIC LIBRARY (no firmware/UF2).
REM  To build the RP2040 firmware, use rp2040demo/build.bat.
REM ================================================================
setlocal

set "ARG1=%1"
set "ARG2=%2"

set "DO_CLEAN=0"
set "SKIP_BUILD=0"
set "NO_WAIT=0"

if /i "%ARG1%"=="clean"        set DO_CLEAN=1
if /i "%ARG1%"=="/nowait"      set NO_WAIT=1
if /i "%ARG2%"=="/nowait"      set NO_WAIT=1

REM -------- Fixed paths --------
set "PICO_SDK_PATH=E:\ppCD\pico-sdk"
set "TOOLCHAIN_BIN=C:\Users\master\.platformio\packages\toolchain-gccarmnoneeabi\bin"
set "CMAKE_BIN=C:\Program Files\CMake\bin"
set "NINJA_BIN=C:\Users\master\AppData\Local\Programs\Python\Python312\Scripts"
set "PROJECT_ROOT=%~dp0"
if "%PROJECT_ROOT:~-1%"=="\" set "PROJECT_ROOT=%PROJECT_ROOT:~0,-1%"
set "BUILD_DIR=%PROJECT_ROOT%\build"
set "KERNEL_A=%BUILD_DIR%\libkernel_core.a"
set "SHELL_A=%BUILD_DIR%\libshell_module.a"
set "TOOLCHAIN_FILE=%PROJECT_ROOT%\toolchain-arm-none-eabi.cmake"
set "LOG=%BUILD_DIR%\build.log"

cd /d "%PROJECT_ROOT%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

REM -------- Start log --------
>  "%LOG%" echo ================================================================
>> "%LOG%" echo Mini Kernel Pure Library Build Run - %date% %time%
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

REM -------- Artifact check (static library outputs) --------
if not exist "%KERNEL_A%" (
    >> "%LOG%" echo ERROR: kernel_core library missing - %KERNEL_A%
    goto FAIL
)
for %%F in ("%KERNEL_A%") do >> "%LOG%" echo BUILD OK - kernel_core.a %%~zF bytes @ %%~fF
if exist "%SHELL_A%" (
    for %%F in ("%SHELL_A%") do >> "%LOG%" echo BUILD OK - shell_module.a %%~zF bytes @ %%~fF
) else (
    >> "%LOG%" echo BUILD OK - shell_module disabled (OS_CFG_SHELL=0)
)

goto OK

:FAIL
echo. >> "%LOG%"
echo ################  FAILED  ################ >> "%LOG%"
echo.
echo ^>^>  BUILD FAILED
echo ^>^>  Full log = %LOG%
call :TAIL "%LOG%" 25
if %NO_WAIT%==0 (
    echo Press any key to close...
    pause >nul
)
exit /b 1

:OK
echo.
echo ================================================================
echo  SUCCESS
echo   Log file : %LOG%
echo   Library  : %KERNEL_A%
if exist "%SHELL_A%" echo   Library  : %SHELL_A%
echo   (pure static kernel library; build firmware via rp2040demo)
echo ================================================================
echo.
if %NO_WAIT%==0 (
    echo Press any key to close...
    pause >nul
)
endlocal
exit /b 0


REM ================== subroutines ==================

:RUN_CMAKE_CONFIG
>> "%LOG%" echo [CONFIG] cmake configure
>> "%LOG%" echo cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN_FILE%" -DPICO_SDK_PATH="%PICO_SDK_PATH%"
cmake.exe -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN_FILE%" -DPICO_SDK_PATH="%PICO_SDK_PATH%" >> "%LOG%" 2>&1
exit /b %errorlevel%

:RUN_CMAKE_BUILD
>> "%LOG%" echo [BUILD] cmake --build
cmake --build "%BUILD_DIR%" --parallel >> "%LOG%" 2>&1
exit /b %errorlevel%

:TAIL file lineCount
REM Simple file tail using for /f (no dependency on external tail.exe)
setlocal enabledelayedexpansion
set F=%~1
set N=%~2
set CNT=0
for /f "usebackq tokens=*" %%L in ("%F%") do (
    set /a CNT+=1
    set "LINE_!CNT!=%%L"
)
set /a FROM=CNT-N+1
if %FROM% lss 1 set FROM=1
for /l %%I in (%FROM%,1,%CNT%) do echo !LINE_%%I!
endlocal
goto :eof