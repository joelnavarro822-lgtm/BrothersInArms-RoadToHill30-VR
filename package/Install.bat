@echo off
rem BiA-VR installer: finds Brothers in Arms: Road to Hill 30 and copies the mod into its System folder.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop';" ^
  "$src = Join-Path '%~dp0' 'System';" ^
  "$game = 'Brothers in Arms Road to Hill 30';" ^
  "$roots = @();" ^
  "try { $sp = (Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath; if ($sp) { $roots += $sp } } catch {}" ^
  "foreach ($r in @($roots)) { $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'; if (Test-Path $vdf) { foreach ($m in (Select-String -Path $vdf -Pattern '\"path\"\s+\"(.+)\"')) { $roots += $m.Matches[0].Groups[1].Value -replace '\\\\','\' } } }" ^
  "$target = $null;" ^
  "foreach ($r in $roots | Select-Object -Unique) { $s = Join-Path $r ('steamapps\common\' + $game + '\System'); if (Test-Path (Join-Path $s 'bia.exe')) { $target = $s; break } }" ^
  "if (-not $target) { Write-Host 'Could not find the game automatically.'; $g = Read-Host 'Paste the game folder (the one that contains the System folder)'; $target = Join-Path $g.Trim('\"') 'System'; if (-not (Test-Path (Join-Path $target 'bia.exe'))) { throw ('bia.exe not found in ' + $target) } }" ^
  "Copy-Item (Join-Path $src 'd3d9.dll'), (Join-Path $src 'openvr_api.dll') $target -Force;" ^
  "Write-Host ''; Write-Host ('Installed BiA-VR into: ' + $target); Write-Host 'Start SteamVR, then launch the game from Steam.'"
if errorlevel 1 echo. & echo Install failed - you can also drag the System folder into the game folder by hand.
echo.
pause
