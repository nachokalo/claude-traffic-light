@echo off
chcp 65001 >nul
title Claude Traffic Light - install hooks
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-hooks.ps1"
echo.
pause
