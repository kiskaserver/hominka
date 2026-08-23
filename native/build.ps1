# Збірка нативної частини (overlay.dll + injector.exe) у Docker.
#
# Навіщо Docker: щоб зібрати під Windows не потрібен був ні Visual Studio, ні
# Windows SDK — той самий mingw-w64, що вже збирає Linux-версію програми.
#
#     .\build.ps1            зібрати обидві розрядності в .\dist
#     .\build.ps1 -Clean     перебудувати образ з нуля
param([switch]$Clean)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $here "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null

if ($Clean) {
    docker rmi hominka-native:latest -f 2>$null | Out-Null
}

Write-Host "Збираю образ hominka-native…" -ForegroundColor Cyan
docker build -t hominka-native:latest $here
if ($LASTEXITCODE -ne 0) { throw "docker build не вдався" }

Write-Host "Компілюю overlay.dll та injector.exe…" -ForegroundColor Cyan
# Монтуємо саму теку native як том і збираємо з неї — так у контейнер завжди
# потрапляє поточний код, а не той, що був на момент складання образу.
docker run --rm -v "${here}:/work" hominka-native:latest /work/dist
if ($LASTEXITCODE -ne 0) { throw "збірка не вдалася" }

Write-Host ""
Write-Host "Готово. Артефакти в $dist:" -ForegroundColor Green
Get-ChildItem $dist | Format-Table Name, Length
