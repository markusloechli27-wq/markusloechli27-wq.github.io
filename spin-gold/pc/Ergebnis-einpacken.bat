@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$z = Join-Path ([Environment]::GetFolderPath('Desktop')) ('spin-ergebnis-pc-' + (Get-Date -Format 'yyyyMMdd-HHmm') + '.zip'); Copy-Item ergebnis-pc.jsonl $env:TEMP\spin-ergebnis-pc.jsonl -Force; Compress-Archive -Path $env:TEMP\spin-ergebnis-pc.jsonl -DestinationPath $z -Force; Write-Host ('Fertig: ' + $z)"
pause
