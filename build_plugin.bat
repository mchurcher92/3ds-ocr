@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_plugin.ps1" %*
