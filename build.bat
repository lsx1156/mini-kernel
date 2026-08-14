@echo off
REM ================================================================
REM  Mini Kernel Build / Flash Script for RP2040
REM  (Safe version: always pauses, writes full log, escapes pitfalls)
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
REM 去掉 %~dp0 末尾的反斜杠，否则 -S "%PROJECT_ROOT%" 中 \" 会转义引号
if "%PROJECT_ROOT:~-1%"=="\" set "PROJECT_ROOT=%PROJECT_ROOT:~0,-1%"
set "BUILD_DIR=%PROJECT_ROOT%build"
set "UF2=%BUILD_DIR%\mini_kernel.uf2"
set "TOOLCHAIN_FILE=%PROJECT_ROOT%toolchain-arm-none-eabi.cmake"
set "LOG=%BUILD_DIR%\build.log"

cd /d "%PROJECT_ROOT%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

REM -------- Start log --------
>  "%LOG%" echo ================================================================
>> "%LOG%" echo Mini Kernel Build Run - %date% %time%
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
if not defined DFOUND if exist "I:\INFO_UF2.TXT" set DFOUND=I:
if not defined DFOUND if exist "J:\INFO_UF2.TXT" set DFOUND=J:
if not defined DFOUND if exist "K:\INFO_UF2.TXT" set DFOUND=K:
if not defined DFOUND if exist "L:\INFO_UF2.TXT" set DFOUND=L:
if not defined DFOUND if exist "M:\INFO_UF2.TXT" set DFOUND=M:
if not defined DFOUND if exist "N:\INFO_UF2.TXT" set DFOUND=N:
if not defined DFOUND if exist "O:\INFO_UF2.TXT" set DFOUND=O:
if not defined DFOUND if exist "P:\INFO_UF2.TXT" set DFOUND=P:
if not defined DFOUND if exist "Q:\INFO_UF2.TXT" set DFOUND=Q:
if not defined DFOUND if exist "R:\INFO_UF2.TXT" set DFOUND=R:
if not defined DFOUND if exist "S:\INFO_UF2.TXT" set DFOUND=S:
if not defined DFOUND if exist "T:\INFO_UF2.TXT" set DFOUND=T:
if not defined DFOUND if exist "U:\INFO_UF2.TXT" set DFOUND=U:
if not defined DFOUND if exist "V:\INFO_UF2.TXT" set DFOUND=V:
if not defined DFOUND if exist "W:\INFO_UF2.TXT" set DFOUND=W:
if not defined DFOUND if exist "X:\INFO_UF2.TXT" set DFOUND=X:
if not defined DFOUND if exist "Y:\INFO_UF2.TXT" set DFOUND=Y:
if not defined DFOUND if exist "Z:\INFO_UF2.TXT" set DFOUND=Z:

if not defined DFOUND (
    >> "%LOG%" echo FLASH WARN: No Pico BOOTSEL drive detected.
    >> "%LOG%" echo Fix: unplug - hold BOOTSEL - replug USB - run 'build.bat flash' again.
    goto OK
)

>> "%LOG%" echo FLASH: drive = %DFOUND%
copy /y "%UF2%" "%DFOUND%\" >> "%LOG%" 2>&1
if errorlevel 1 (
    >> "%LOG%" echo FLASH ERROR: copy failed - Pico may have rebooted early.
    goto FAIL
)
>> "%LOG%" echo FLASH OK. Pico will reboot in ~1 second.

goto OK


:FAIL
echo. >> "%LOG%"
echo ################  FAILED  ################ >> "%LOG%"
echo.
echo ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
echo ^>^>  BUILD / FLASH FAILED
echo ^>^>  Full log = %LOG%
echo vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
call :TAIL "%LOG%" 25
echo ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
echo.
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
echo   UF2      : %UF2%
if %DO_FLASH%==1 (
    echo   Flash op : executed ^(check log for drive/result^)
)
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
set "CFG_CMD=cmake.exe -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN_FILE% -DPICO_SDK_PATH=%PICO_SDK_PATH%"
>> "%LOG%" echo %CFG_CMD%
%CFG_CMD% >> "%LOG%" 2>&1
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
