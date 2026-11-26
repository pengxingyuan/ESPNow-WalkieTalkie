@echo off
REM Build ESP32-S3 firmware. Run inside an ESP-IDF Command Prompt (idf.py in PATH).
setlocal
where idf.py >nul 2>&1
if errorlevel 1 (
    echo [ERROR] idf.py not found. Open "ESP-IDF 5.5 CMD" / run export.bat first.
    exit /b 1
)
cd /d "%~dp0..\firmware\esp32s3"
echo Building %CD% ...
idf.py build
if errorlevel 1 (
    echo Build failed. If you see a GCC "internal compiler error", retry with:  ninja -j 4
    exit /b 1
)
echo.
echo Build OK: build\2.8inch_ESP32-S3_LVGL.bin
