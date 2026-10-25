@echo off
rem Starts Ape Escape 2 with the disc files in the "game" folder next to this file.
rem Log messages show in this window; keep it open while playing.
cd /d "%~dp0"
if not exist "game\SYSTEM.CNF" (
  echo No game files found in "%~dp0game".
  echo Run: python tools\setup_game.py your.iso --game-dir "%~dp0game"
  pause
  exit /b 1
)
ae2.exe "game" %*
if errorlevel 1 pause
