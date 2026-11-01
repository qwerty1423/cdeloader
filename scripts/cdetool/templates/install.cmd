@echo off
rem @@NAME@@ @@VERSION@@ - installer (proxy mode, no change to CDE.exe).
rem Renames vcruntime140.dll, installs the forwarder + loader.
rem Use install-delta.cmd for the old import-patch mode.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Proxy %*
pause
