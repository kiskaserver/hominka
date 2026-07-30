@echo off
REM Збирає ChatOverlay.exe (standalone) через PyInstaller.
cd /d "%~dp0"
chcp 65001 >nul

where py >nul 2>nul && (set PY=py) || (set PY=python)

if not exist ".venv\" ( %PY% -m venv .venv )
call ".venv\Scripts\activate.bat"

echo [build] Ставлю залежності...
python -m pip install --upgrade pip >nul
python -m pip install -r requirements.txt >nul
python -m pip install pyinstaller >nul

echo [build] Збираю ChatOverlay.exe (кілька хвилин)...
pyinstaller --noconfirm --clean --windowed --name ChatOverlay chat_overlay.py

echo.
echo [build] Готово. Запускай:  dist\ChatOverlay\ChatOverlay.exe
pause
