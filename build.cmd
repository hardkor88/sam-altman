@echo off
rem Удобная обёртка для сборки: запускает build.ps1 с обходом политики выполнения.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
pause
