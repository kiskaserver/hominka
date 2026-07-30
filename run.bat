@echo off
cd /d "%~dp0"
chcp 65001 >nul

where py >nul 2>nul && (set PY=py) || (set PY=python)

if not exist ".venv\" (
    echo [chat-overlay] Creating venv and installing PySide6...
    %PY% -m venv .venv
    call ".venv\Scripts\activate.bat"
    python -m pip install --upgrade pip >nul
    python -m pip install -r requirements.txt
) else (
    call ".venv\Scripts\activate.bat"
)

start "" ".venv\Scripts\pythonw.exe" chat_overlay.py %*