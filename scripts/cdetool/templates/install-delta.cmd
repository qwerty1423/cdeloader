@echo off
rem @@NAME@@ @@VERSION@@ - legacy installer (patches CDE.exe's import table).
rem Prefer install.cmd (proxy mode). The delta is kept for fallback and for
rem users already on a delta install.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
pause
