@echo off
rem Opens the Alias Isolation NEXT installer menu.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-AliasIsolationNEXT.ps1" %*
pause
