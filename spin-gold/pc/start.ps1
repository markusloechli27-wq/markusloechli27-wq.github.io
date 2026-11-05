# Spin & Gold PC-Rechner einrichten und starten. In PowerShell einfuegen:
#   irm https://markusloechli27-wq.github.io/spin-gold/pc/start.ps1 | iex
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
$d = Join-Path $env:USERPROFILE 'SpinRechner'; New-Item -ItemType Directory -Force -Path $d | Out-Null
$t = [DateTimeOffset]::Now.ToUnixTimeSeconds()
foreach ($f in 'spin-pc.ps1', 'Rechnen-starten.bat', 'Ergebnis-einpacken.bat', 'LIESMICH.txt') {
  Invoke-WebRequest -UseBasicParsing -Uri "https://markusloechli27-wq.github.io/spin-gold/pc/$f`?t=$t" -OutFile (Join-Path $d $f) }
Write-Host "Eingerichtet in $d. Der Rechner startet jetzt in einem eigenen Fenster."
Start-Process explorer $d
Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $d 'spin-pc.ps1')`"" -WorkingDirectory $d
