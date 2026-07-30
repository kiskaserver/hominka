@echo off
REM Запуск оверлея чату. Перший запуск сам поставить залежності.
cd /d "%~dp0"

where py >nul 2>nul && (set PY=py) || (set PY=python)

if not exist ".venv\" (
  echo [chat-overlay] Створюю venv і ставлю PySide6 (одноразово, ~хвилина)...
  %PY% -m venv .venv
  call ".venv\Scripts\activate.bat"
  python -m pip install --upgrade pip >nul
  python -m pip install -r requirements.txt
) else (
  call ".venv\Scripts\activate.bat"
)

REM pythonw = без чорного вікна консолі
start "" ".venv\Scripts\pythonw.exe" chat_overlay.py %*
