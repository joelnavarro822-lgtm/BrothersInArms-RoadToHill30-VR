@echo off
rem BiA-VR uninstaller: removes the mod files from the game's System folder (game files are untouched).
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$game = 'Brothers in Arms Road to Hill 30';" ^
  "$roots = @();" ^
  "try { $sp = (Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath; if ($sp) { $roots += $sp } } catch {}" ^
  "foreach ($r in @($roots)) { $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'; if (Test-Path $vdf) { foreach ($m in (Select-String -Path $vdf -Pattern '\"path\"\s+\"(.+)\"')) { $roots += $m.Matches[0].Groups[1].Value -replace '\\\\','\' } } }" ^
  "$target = $null;" ^
  "foreach ($r in $roots | Select-Object -Unique) { $s = Join-Path $r ('steamapps\common\' + $game + '\System'); if (Test-Path (Join-Path $s 'bia.exe')) { $target = $s; break } }" ^
  "if (-not $target) { $g = Read-Host 'Paste the game folder (the one that contains the System folder)'; $target = Join-Path $g.Trim('\"') 'System' }" ^
  "foreach ($f in 'd3d9.dll','openvr_api.dll','BiAVR.ini','BiAVR.log') { $p = Join-Path $target $f; if (Test-Path $p) { Remove-Item $p -Force; Write-Host ('Removed ' + $f) } }" ^
  "Write-Host 'BiA-VR removed. The game runs normally again.'"
echo.
pause
