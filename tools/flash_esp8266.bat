@echo off
REM Flash ESP8266 web-chat firmware. Usage: flash_esp8266.bat COM14
setlocal
if "%~1"=="" (
    echo Usage: %~nx0 ^<COM port^>   e.g. %~nx0 COM14
    echo Available ports:
    powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"
    exit /b 1
)
set "CLI=%ARDUINO_CLI%"
if "%CLI%"=="" set "CLI=arduino-cli"
"%CLI%" version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] arduino-cli not found. Install it or set ARDUINO_CLI.
    exit /b 1
)
"%CLI%" upload -p %1 --fqbn esp8266:esp8266:d1_mini "%~dp0..\firmware\esp8266\web_chat"
if errorlevel 1 (
    echo Upload FAILED
    exit /b 1
)
echo.
echo Done. The ESP8266 should now open AP "ESP-NOW-CHAT" (password 12345678).
