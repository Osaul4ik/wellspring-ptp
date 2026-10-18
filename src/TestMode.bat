@echo off
setlocal

chcp 65001 >nul
title Windows Test Mode Manager

:: ------------------------------------------------------------
:: Check for Administrator privileges
:: ------------------------------------------------------------
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator privileges...

    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs"

    exit /b 0
)

:: ------------------------------------------------------------
:: Detect current Test Mode status
::
:: Note: bcdedit's Yes/No value text can be localized on
:: non-English Windows builds, so we don't compare against it.
:: We simply check whether "testsigning" exists in the output.
:: ------------------------------------------------------------
set "STATE=DISABLED"

bcdedit /enum | findstr /i "testsigning" >nul
if not errorlevel 1 set "STATE=ENABLED"

:MENU
cls

echo =====================================
echo        Windows Test Mode Manager
echo =====================================
echo.
echo Current status: %STATE%
echo.
echo [1] Enable Test Mode
echo [2] Disable Test Mode
echo [0] Exit
echo.

choice /c 120 /n /m "Select an option: "

if errorlevel 3 exit /b
if errorlevel 2 goto DISABLE
if errorlevel 1 goto ENABLE

:ENABLE
cls

echo Enabling Test Mode...
echo.

bcdedit /set testsigning on

if errorlevel 1 (
    echo.
    echo Failed to enable Test Mode.
    echo.
    echo Possible reasons:
    echo  - Secure Boot is enabled.
    echo  - This script is not running as Administrator.
    echo.
    pause
    exit /b 1
)

echo.
echo Test Mode has been ENABLED successfully.
echo.
echo Please restart your computer for the changes to take effect.
echo.

pause
exit /b 0

:DISABLE
cls

echo Disabling Test Mode...
echo.

rem "bcdedit /set testsigning off" keeps a "testsigning No" element in the
rem BCD store, and the existence-based status check above would then keep
rem reporting ENABLED. Deleting the value removes the element entirely.
bcdedit /deletevalue testsigning

if errorlevel 1 (
    rem Element not found means Test Mode is already off - not a failure.
    rem If it is still present, the delete really failed.
    bcdedit /enum | findstr /i "testsigning" >nul
    if not errorlevel 1 (
        echo.
        echo Failed to disable Test Mode.
        echo.
        echo Possible reasons:
        echo  - Secure Boot is enabled.
        echo  - This script is not running as Administrator.
        echo.
        pause
        exit /b 1
    )
)

echo.
echo Test Mode has been DISABLED successfully.
echo.
echo Please restart your computer for the changes to take effect.
echo.

pause
exit /b 0