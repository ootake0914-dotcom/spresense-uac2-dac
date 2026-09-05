@echo off
setlocal
cd /d "%~dp0\.."

echo ========================================================
echo  Spresense UAC2 Shared Mode Fix (OEMFormat + DeviceFormat)
echo ========================================================
echo.

python tools\fix_shared_mode.py --apply
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Failed to apply format. Please make sure this batch file
    echo         is executed with Administrator privileges (Right-click -^> Run as administrator).
    echo.
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo [1/2] Restarting Windows Audio Endpoint Builder (AudioEndpointBuilder)...
powershell -NoProfile -Command "Restart-Service -Name AudioEndpointBuilder -Force"

echo.
echo [2/2] Restarting Windows Audio Service (Audiosrv)...
powershell -NoProfile -Command "Restart-Service -Name Audiosrv -Force"

echo.
echo ========================================================
echo  Successfully applied 192kHz/24bit Shared Mode Fix!
echo ========================================================
echo.
pause
