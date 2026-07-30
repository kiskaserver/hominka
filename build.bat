@echo off
REM Збирає Svitix.exe (standalone) через PyInstaller.
cd /d "%~dp0"
chcp 65001 >nul

where py >nul 2>nul && (set PY=py) || (set PY=python)

if not exist ".venv\" ( %PY% -m venv .venv )
call ".venv\Scripts\activate.bat"

echo [build] Ставлю залежності...
python -m pip install --upgrade pip >nul
python -m pip install -r requirements.txt >nul
python -m pip install pyinstaller pillow >nul

echo [build] Генерую іконку...
python make_icon.py

echo [build] Збираю Svitix.exe (кілька хвилин)...
pyinstaller --noconfirm --clean --windowed --name Svitix ^
  --icon svitix.ico --version-file version_info.txt --add-data "svitix.ico;." ^
  chat_overlay.py

echo.
echo [build] Готово. Запускай:  dist\Svitix\Svitix.exe
pause
