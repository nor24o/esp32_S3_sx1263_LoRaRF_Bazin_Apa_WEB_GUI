@echo off
chcp 65001 >nul
title SX1302 Universal LoRa Station & Fleet Deck
cd /d "%~dp0"
echo ======================================================================
echo   SX1302 Universal Multi-Node LoRa Gateway Coordinator & Pond Node Sim
echo   Fleet Web SCADA: http://localhost:8080
echo ======================================================================
python run.py
if errorlevel 1 (
    echo.
    echo [!] Process exited with error code %errorlevel%.
    pause
)
