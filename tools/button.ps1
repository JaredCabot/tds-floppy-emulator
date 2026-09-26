# button.ps1 - press an emulator button over SWD and report the result.
#   powershell -File tools\button.ps1 in    (RIGHT: data in, stick -> disk)
#   powershell -File tools\button.ps1 out   (LEFT: data out, disk -> stick)
#   powershell -File tools\button.ps1 status
#   -CancelAt N: cancel the transfer once N bytes are copied, as pressing its own
#    button again would (xfer_dbg_cancel_at; always written, 0 = off)
param([string]$Which = "status", [int]$TimeoutS = 180, [int]$CancelAt = 0)
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location (Join-Path $root 'firmware')
$sym = @{}
if (-not (Test-Path build\flashed.bin) -or (Get-FileHash build\tdsfloppy.bin).Hash -ne (Get-FileHash build\flashed.bin).Hash) { "FIRMWARE MISMATCH: build\tdsfloppy.bin is not the image last flashed - run make flash-swd first (symbol addresses would be wrong)."; exit 3 }
arm-none-eabi-nm build\tdsfloppy.elf | ForEach-Object {
  $p = $_ -split '\s+'
  if ($p.Count -eq 3 -and $p[2] -match '^(dbg_unlock|dbg_button|dbg_xfer_result|dbg_xfer_count|dbg_usb_ready|xfer_dbg_files|xfer_dbg_bytes|xfer_dbg_cancel_at|dbg_fw_version|dbg_fw_build|dbg_fault|buffer_dbg_writebacks)$') { $sym[$p[2]] = "0x" + $p[0] }
}
function Read-Vars {
  $tcl = "init`n" + (($sym.GetEnumerator() | ForEach-Object { "echo `"$($_.Key)=[read_memory $($_.Value) 32 1]`"" }) -join "`n") + "`nexit`n"
  Set-Content -Path build\button_read.cfg -Value $tcl -Encoding ASCII
  $out = openocd -f interface/stlink.cfg -f target/artery/at32f4x.cfg -f build/button_read.cfg 2>&1
  $v = @{}; foreach ($l in $out) { if ("$l" -match '^(\w+)=(0x[0-9a-fA-F]+)') { $v[$matches[1]] = [Convert]::ToUInt32($matches[2], 16) } }
  return $v
}
$names = @('OK', 'NO_STICK', 'USB_ERROR', 'BAD_IMAGE', 'NOTHING', 'VERIFY_FAILED', 'BUSY', 'BAD_FORMAT', 'CANCELLED')
$before = Read-Vars
if ($Which -eq 'status') {
  $v = $before.dbg_fw_version
  "firmware {0}.{1}.{2} build {3:x8} fault={4} writebacks={10} | usb_ready={5} transfers={6} last={7} files={8} bytes={9}" -f ($v -shr 16), (($v -shr 8) -band 255), ($v -band 255), $before.dbg_fw_build, $before.dbg_fault, $before.dbg_usb_ready, $before.dbg_xfer_count, $names[$before.dbg_xfer_result], $before.xfer_dbg_files, $before.xfer_dbg_bytes, $before.buffer_dbg_writebacks
  exit }
# one bench tool at a time (see benchlock.py)
$lock = Join-Path $root 'captures\bench.lock'
if (Test-Path $lock) {
  $h = (Get-Content $lock -Raw).Trim().Split(' ', 2)
  if ([int]$h[0] -ne $PID -and (Get-Process -Id ([int]$h[0]) -ErrorAction SilentlyContinue)) { "BENCH BUSY: '$($h[1])' (pid $($h[0])) is using the scope/emulator."; exit 2 }
}
New-Item -ItemType Directory -Force (Split-Path $lock) | Out-Null
Set-Content $lock "$PID button.ps1 $Which" -NoNewline
$code = if ($Which -eq 'in') { 1 } else { 2 }
openocd -f interface/stlink.cfg -f target/artery/at32f4x.cfg -c "init; mww $($sym.xfer_dbg_cancel_at) $CancelAt; mww $($sym.dbg_unlock) 0x5AFE1234; mww $($sym.dbg_button) $code; exit" 2>$null | Out-Null
$t0 = Get-Date
do { Start-Sleep 2; $v = Read-Vars } while ($v.dbg_xfer_count -eq $before.dbg_xfer_count -and ((Get-Date) - $t0).TotalSeconds -lt $TimeoutS)
if ($v.dbg_xfer_count -eq $before.dbg_xfer_count) { Remove-Item $lock -ErrorAction SilentlyContinue; "no transfer completed within $TimeoutS s"; exit 1 }
"{0}: {1}  files={2} bytes={3}  ({4:N1} s, usb_ready={5})" -f $Which.ToUpper(), $names[$v.dbg_xfer_result], $v.xfer_dbg_files, $v.xfer_dbg_bytes, ((Get-Date) - $t0).TotalSeconds, $v.dbg_usb_ready
Remove-Item $lock -ErrorAction SilentlyContinue
