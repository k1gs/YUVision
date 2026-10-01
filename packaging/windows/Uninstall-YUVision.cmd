@echo off
setlocal

set "ROOT=%~dp0"
set "MSI="
for %%F in ("%ROOT%YUVision-*.msi") do if exist "%%~fF" if not defined MSI set "MSI=%%~fF"

if not defined MSI (
  echo YUVision MSI was not found next to this script.
  pause
  exit /b 2
)

set "LOGDIR=%ROOT%logs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
if exist "%LOGDIR%\YUVision-uninstall.previous.log" del /q "%LOGDIR%\YUVision-uninstall.previous.log"
if exist "%LOGDIR%\YUVision-uninstall.log" move /y "%LOGDIR%\YUVision-uninstall.log" "%LOGDIR%\YUVision-uninstall.previous.log" >nul

echo Uninstalling YUVision...
echo Uninstaller log: %LOGDIR%\YUVision-uninstall.log
start /wait "" msiexec.exe /x "%MSI%" /norestart /L*V! "%LOGDIR%\YUVision-uninstall.log"
set "RESULT=%ERRORLEVEL%"

if "%RESULT%"=="0" (
  echo YUVision was uninstalled successfully.
) else if "%RESULT%"=="3010" (
  echo YUVision was uninstalled successfully. Windows requests a restart.
) else (
  echo Uninstallation failed with Windows Installer code %RESULT%.
  echo Send the log above when reporting the problem.
  pause
)
exit /b %RESULT%
