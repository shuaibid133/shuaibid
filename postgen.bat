@echo off
rem postgen.bat - run after CubeMX regenerates code to restore manual fixes
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0postgen.ps1"
echo.
pause
