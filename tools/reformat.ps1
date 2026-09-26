# reformat.ps1 - reset the emulator's disk image to the initial (test) image.
# Asks the running firmware (dbg_flash_req = 2) to re-format the SPI-flash buffer
# and restart; it comes back with DISK CHANGE raised, so the TDS re-reads it.
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
# one bench tool at a time (see benchlock.py)
$lock = Join-Path $root 'captures\bench.lock'
if (Test-Path $lock) {
  $h = (Get-Content $lock -Raw).Trim().Split(' ', 2)
  if ([int]$h[0] -ne $PID -and (Get-Process -Id ([int]$h[0]) -ErrorAction SilentlyContinue)) { "BENCH BUSY: '$($h[1])' (pid $($h[0])) is using the scope/emulator."; exit 2 }
}
New-Item -ItemType Directory -Force (Split-Path $lock) | Out-Null
Set-Content $lock "$PID reformat.ps1" -NoNewline
Set-Location (Join-Path $root 'firmware')
if (-not (Test-Path build\flashed.bin) -or (Get-FileHash build\tdsfloppy.bin).Hash -ne (Get-FileHash build\flashed.bin).Hash) { "FIRMWARE MISMATCH: build\tdsfloppy.bin is not the image last flashed - run make flash-swd first (symbol addresses would be wrong)."; exit 3 }
$req = (arm-none-eabi-nm build\tdsfloppy.elf | Select-String ' dbg_flash_req$').Line.Split(' ')[0]
$unl = (arm-none-eabi-nm build\tdsfloppy.elf | Select-String ' dbg_unlock$').Line.Split(' ')[0]
openocd -f interface/stlink.cfg -f target/artery/at32f4x.cfg -c "init; mww 0x$unl 0x5AFE1234; mww 0x$req 2; exit" 2>$null
Start-Sleep 4
"re-format requested; firmware restarted"
Remove-Item $lock -ErrorAction SilentlyContinue
