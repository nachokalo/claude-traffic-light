@echo off
chcp 65001 >nul
title Claude Traffic Light - uninstall hooks
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-hooks.ps1" -Remove
echo.
pause
