@echo off
setlocal
cd /d "%~dp0"

echo.
echo ============================================================
echo  BRIO DUAL SPOUT V17.1 - INSTALADOR AUTOMATICO
echo ============================================================
echo.
echo Se solicitara permiso de administrador una sola vez.
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "Start-Process powershell.exe -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File ""%~dp0INSTALL.ps1""'"

exit /b
