# tryread.ps1 - apply a live register tweak over SWD, make the scope access fd0:,
# then report what the floppy layer saw. Usage:
#   powershell -File tools\tryread.ps1 -Poke "mww 0x40000420 0x10"
# Success indicator: the trace shows head 1 (the root directory, after the boot
# sector at cyl 0 head 0 was decoded) and DIR? lists TEST.BIN.
# Always re-asserts DSKCHG (GPIOB CLR bit7) so the scope treats it as a new disk
# and genuinely re-reads instead of answering from its cache.
param([string]$Poke = "")
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location (Join-Path $root 'firmware')
if (-not (Test-Path build\flashed.bin) -or (Get-FileHash build\tdsfloppy.bin).Hash -ne (Get-FileHash build\flashed.bin).Hash) { "FIRMWARE MISMATCH: build\tdsfloppy.bin is not the image last flashed - run make flash-swd first (symbol addresses would be wrong)."; exit 3 }
$dsk = (arm-none-eabi-nm build\tdsfloppy.elf | Select-String ' st_dskchg$').Line.Split(' ')[0]
$cmd = "init; mwb 0x$dsk 1"          # logical DISK CHANGE = true (shown on next select)
if ($Poke) { $cmd += "; $Poke" }
openocd -f interface/stlink.cfg -f target/artery/at32f4x.cfg -c "$cmd; exit" 2>$null
"applied: st_dskchg=1; $Poke"
Set-Location $root
python tools\tds.py 'FILESystem:CWD \"hd0:/\"' 'FILESystem:CWD \"fd0:/\"' 'FILESystem:DIR?'
Start-Sleep 6
powershell -ExecutionPolicy Bypass -File tools\fdstat.ps1 2>$null
