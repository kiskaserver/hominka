# Збірка нативної частини (overlay.dll + injector.exe) у Docker.
#
# Навіщо Docker: щоб зібрати під Windows не потрібен був ні Visual Studio, ні
# Windows SDK — той самий mingw-w64, що вже збирає Linux-версію програми.
#
# Джерела компілюються ПІД ЧАС docker build (див. Dockerfile), а не через
# bind-mount: монтування теки з Windows віддає файли з затримкою кешу, і збірка
# тихо брала вчорашній код. Готові бінарі витягуємо з образу через docker cp —
# теж без монтування, тож ніякого stale.
#
#     .\build.ps1            зібрати обидві розрядності в .\dist
#     .\build.ps1 -Clean     перебудувати образ з нуля
param([switch]$Clean)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $here "dist"

if ($Clean) { docker rmi hominka-native:latest -f 2>$null | Out-Null }

Write-Host "Збираю образ і компілюю overlay.dll + injector.exe…" -ForegroundColor Cyan
# COPY-шар зникає з кешу рівно тоді, коли джерело змінилося, тож тут завжди
# свіжий код. --no-cache не потрібен.
docker build -t hominka-native:latest $here
if ($LASTEXITCODE -ne 0) { throw "docker build (він же збірка) не вдався" }

Write-Host "Витягую бінарі з образу…" -ForegroundColor Cyan
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path $dist | Out-Null

# docker cp — надійне копіювання з контейнера, без гонок bind-mount.
$cid = docker create hominka-native:latest
try {
    docker cp "${cid}:/out/." $dist
    if ($LASTEXITCODE -ne 0) { throw "docker cp не вдався" }
} finally {
    docker rm $cid | Out-Null
}

Write-Host ""
Write-Host "Готово. Артефакти в $dist:" -ForegroundColor Green
Get-ChildItem $dist | Format-Table Name, Length
