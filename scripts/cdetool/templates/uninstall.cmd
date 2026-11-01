@echo off
rem @@NAME@@ @@VERSION@@ - restore the original CDE.exe.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall %*
pause
