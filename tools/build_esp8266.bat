@echo off
REM Compile ESP8266 web-chat firmware.
REM Requires arduino-cli in PATH, or set ARDUINO_CLI to its full path.
setlocal
set "CLI=%ARDUINO_CLI%"
if "%CLI%"=="" set "CLI=arduino-cli"
"%CLI%" version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] arduino-cli not found. Install it or set ARDUINO_CLI.
    exit /b 1
)
"%CLI%" compile --fqbn esp8266:esp8266:d1_mini "%~dp0..\firmware\esp8266\web_chat"
