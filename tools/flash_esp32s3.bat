@echo off
REM Flash ESP32-S3 firmware. Usage: flash_esp32s3.bat COM8
setlocal
if "%~1"=="" (
    echo Usage: %~nx0 ^<COM port^>   e.g. %~nx0 COM8
    exit /b 1
)
where idf.py >nul 2>&1
if errorlevel 1 (
    echo [ERROR] idf.py not found. Open an ESP-IDF Command Prompt first.
    exit /b 1
)
cd /d "%~dp0..\firmware\esp32s3"
idf.py -p %1 flash
