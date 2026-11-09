@echo off
setlocal enabledelayedexpansion
title Wellspring PTP - Driver Install / Uninstall

rem %1 = touchpad driver base name (no extension), optional.
rem %2 = optional mode: "install" or "uninstall". When omitted a menu is shown.
rem This script and the driver files (.sys/.inf/.cat/.cer) are all extracted
rem by Setup (or unzipped) into the same folder, so %~dp0 (this script's own
rem folder) is where they live. The haptic actuator driver
rem (AmtHapticDeviceUsbKm.*) is looked up next to this script first, then in
rem a sibling "HapticDriver" folder (the layout of the release archive).
set "DRVNAME=%~1"
if "%DRVNAME%"=="" set "DRVNAME=AmtPtpDeviceUsbKm"
set "MODE=%~2"
set "HAPNAME=AmtHapticDeviceUsbKm"

set "HERE=%~dp0"
set "INF=%HERE%%DRVNAME%.inf"
set "CER=%HERE%%DRVNAME%.cer"

set "HAPINF=%HERE%%HAPNAME%.inf"
if not exist "%HAPINF%" set "HAPINF=%HERE%..\HapticDriver\%HAPNAME%.inf"

rem Must run elevated: bcdedit, pnputil and Import-Certificate into
rem Cert:\LocalMachine\* all require Administrator privileges.
rem If not elevated yet, relaunch this same script with a UAC prompt,
rem preserving the arguments and working folder, then exit this
rem non-elevated instance.
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator privileges...
    set "ELEVATE_VBS=%TEMP%\wsp_elevate_%RANDOM%.vbs"
    > "!ELEVATE_VBS!" echo On Error Resume Next
    >> "!ELEVATE_VBS!" echo Set UAC = CreateObject("Shell.Application")
    >> "!ELEVATE_VBS!" echo UAC.ShellExecute "%~f0", "%DRVNAME% %MODE%", "%HERE%", "runas", 1
    cscript //nologo "!ELEVATE_VBS!"
    del "!ELEVATE_VBS!" >nul 2>&1
    goto END
)

if /i "%MODE%"=="install"   goto ASK_INSTALL
if /i "%MODE%"=="uninstall" goto ASK_UNINSTALL

:MENU
cls
echo =====================================================
echo   Wellspring PTP - Driver Install / Uninstall
echo =====================================================
echo.
echo   [I] Install  - touchpad driver + haptic actuator driver
echo   [U] Uninstall - remove both drivers
echo   [N] Cancel
echo.
choice /c IUN /n /m "Select an option [I/U/N]: "
if errorlevel 3 goto SKIP
if errorlevel 2 goto ASK_UNINSTALL
if errorlevel 1 goto ASK_INSTALL
goto MENU

rem ======================================================================
rem  INSTALL
rem ======================================================================
:ASK_INSTALL
cls
echo =====================================================
echo   Wellspring PTP - Driver Installation
echo =====================================================
echo.
echo This will:
echo   1. Enable Windows Test Mode (testsigning) so the drivers'
echo      test certificate is trusted.
echo   2. Import that certificate into Trusted Root and Trusted
echo      Publishers.
echo   3. Install the %DRVNAME% touchpad driver (pnputil).
echo   4. Install the %HAPNAME% haptic actuator driver (pnputil),
echo      if its files are present.
echo   5. Reboot this computer - REQUIRED for Test Mode to take
echo      effect and for the drivers to load.
echo.
echo Save any open work before continuing.
echo.

choice /c YN /n /m "Install the drivers and reboot now? [Y]es / [N]o: "

if errorlevel 2 goto SKIP
if errorlevel 1 goto INSTALL

goto ASK_INSTALL

:INSTALL
echo.
echo [1/5] Enabling Test Mode (bcdedit /set testsigning on) ...
bcdedit /set testsigning on
if errorlevel 1 (
    echo.
    echo [ERROR] Could not enable Test Mode. If Secure Boot is on in the
    echo         BIOS/UEFI, it must be disabled first. Driver installation
    echo         was NOT completed.
    echo.
    pause
    goto END
)

if not exist "%CER%" (
    echo.
    echo [ERROR] Certificate not found: %CER%
    echo.
    pause
    goto END
)

echo.
echo [2/5] Importing certificate into Trusted Root ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath '%CER%' -CertStoreLocation Cert:\LocalMachine\Root | Out-Null"
if errorlevel 1 (
    echo.
    echo [ERROR] Failed to import the certificate into Trusted Root.
    echo.
    pause
    goto END
)

echo [2/5] Importing certificate into Trusted Publishers ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath '%CER%' -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null"
if errorlevel 1 (
    echo.
    echo [ERROR] Failed to import the certificate into Trusted Publishers.
    echo.
    pause
    goto END
)

if not exist "%INF%" (
    echo.
    echo [ERROR] Driver .inf not found: %INF%
    echo.
    pause
    goto END
)

echo.
echo Removing previous driver versions ...
call :REMOVEPKG "%DRVNAME%"
call :REMOVEPKG "%HAPNAME%"

echo.
echo [3/5] Installing touchpad driver (pnputil) ...
pnputil /add-driver "%INF%" /install
set "RC=!ERRORLEVEL!"
rem 3010 = ERROR_SUCCESS_REBOOT_REQUIRED - installed fine, reboot needed.
if not "!RC!"=="0" if not "!RC!"=="3010" (
    echo.
    echo [ERROR] pnputil reported an error installing the touchpad driver ^(code !RC!^).
    echo.
    pause
    goto END
)

echo.
if exist "%HAPINF%" (
    echo [4/5] Installing haptic actuator driver (pnputil) ...
    pnputil /add-driver "%HAPINF%" /install
    set "RC=!ERRORLEVEL!"
    if not "!RC!"=="0" if not "!RC!"=="3010" (
        echo.
        echo [WARNING] pnputil reported an error installing the haptic driver ^(code !RC!^).
        echo           The touchpad driver is installed, but haptic clicks
        echo           will not work.
        echo.
    )
) else (
    echo [4/5] Haptic driver files not found - skipping the haptic driver.
)

pnputil /scan-devices >nul 2>&1

echo.
echo [5/5] Rebooting in 30 seconds to finish driver installation.
echo       Run "shutdown /a" in another window to cancel the reboot.
shutdown /r /t 30 /c "Wellspring PTP: reboot required to finish driver installation."
goto END

rem ======================================================================
rem  UNINSTALL
rem ======================================================================
:ASK_UNINSTALL
cls
echo =====================================================
echo   Wellspring PTP - Driver Removal
echo =====================================================
echo.
echo This will remove from the Windows Driver Store (and from any
echo device bound to them):
echo   - %HAPNAME% (haptic actuator driver)
echo   - %DRVNAME% (touchpad driver)
echo.
echo Afterwards you can choose to turn Windows Test Mode off again.
echo.

choice /c YN /n /m "Remove the drivers now? [Y]es / [N]o: "

if errorlevel 2 goto SKIP
if errorlevel 1 goto UNINSTALL

goto ASK_UNINSTALL

:UNINSTALL
echo.
echo [1/3] Removing haptic actuator driver ...
call :REMOVEPKG "%HAPNAME%"

echo [2/3] Removing touchpad driver ...
call :REMOVEPKG "%DRVNAME%"

echo.
choice /c YN /n /m "[3/3] Disable Windows Test Mode (testsigning)? Say No if another test-signed driver needs it [Y/N]: "
if errorlevel 2 goto UNINSTALL_SCAN
bcdedit /deletevalue testsigning >nul 2>&1
echo Test Mode will be turned off after the next reboot.

:UNINSTALL_SCAN
rem Without a rescan the touchpad would have no bound driver until the next
rem boot - this lets Windows re-bind it to its default HID driver right away.
pnputil /scan-devices >nul 2>&1

echo.
echo ==========================================
echo Drivers removed. A restart is recommended.
echo ==========================================
echo.
pause
goto END

:SKIP
echo.
echo Nothing was changed. You can rerun this script later
echo if you change your mind.
echo.
pause >nul
goto END

rem ----------------------------------------------------------------------
rem  :REMOVEPKG <inf base name>
rem  Removes every published Driver Store package whose original INF is
rem  <name>.inf, whatever oemNN.inf number Windows gave it. Uses
rem  Get-WindowsDriver instead of parsing localized pnputil text output.
rem  Without -All it only lists third-party (oemNN.inf) packages.
rem ----------------------------------------------------------------------
:REMOVEPKG
set "FOUND="
for /f "usebackq delims=" %%D in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*%~1.inf' } | Select-Object -ExpandProperty Driver"`) do (
    set "FOUND=1"
    echo   Removing %%D ^(%~1^) ...
    pnputil /delete-driver %%D /uninstall /force >nul 2>&1
)
if not defined FOUND echo   No installed package found for %~1.
exit /b 0

:END
endlocal
exit /b 0
