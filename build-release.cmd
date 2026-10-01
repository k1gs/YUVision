@echo off
setlocal
cd /d "%~dp0"

echo Building YUVision Release executable and MSI installer...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\build-msi.ps1"
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
  echo.
  echo Build failed with exit code %RESULT%.
  pause
  exit /b %RESULT%
)

echo.
echo Build completed successfully.
echo Output: %~dp0out\msi
pause
exit /b 0
