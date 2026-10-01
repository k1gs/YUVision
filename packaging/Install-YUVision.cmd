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
if exist "%LOGDIR%\YUVision-install.previous.log" del /q "%LOGDIR%\YUVision-install.previous.log"
if exist "%LOGDIR%\YUVision-install.log" move /y "%LOGDIR%\YUVision-install.log" "%LOGDIR%\YUVision-install.previous.log" >nul

echo Installing YUVision...
echo Installer log: %LOGDIR%\YUVision-install.log
start /wait "" msiexec.exe /i "%MSI%" /norestart /L*V! "%LOGDIR%\YUVision-install.log"
set "RESULT=%ERRORLEVEL%"

if "%RESULT%"=="0" (
  echo YUVision was installed successfully.
) else if "%RESULT%"=="3010" (
  echo YUVision was installed successfully. Windows requests a restart.
) else (
  echo Installation failed with Windows Installer code %RESULT%.
  echo Send the log above when reporting the problem.
  pause
)
exit /b %RESULT%
