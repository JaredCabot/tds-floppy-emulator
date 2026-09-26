# stick.ps1 - put a PC file onto / get a file from the USB stick in the emulator, over SWD
# (the firmware's dbg_flash_req 3/4 hook; the floppy keeps running).
#   powershell -File tools\stick.ps1 put C:\path\file.bin NAME.EXT
#   powershell -File tools\stick.ps1 get NAME.EXT C:\path\out.bin
# Used for unattended tests, e.g. placing UPDATE.UPD. ~0.1 s per 512-byte chunk.
param([Parameter(Mandatory)][ValidateSet('put','get')][string]$Op,
      [Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B)
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
if ($Op -eq 'put') { $A = (Resolve-Path $A).Path } else { $B = [IO.Path]::GetFullPath($B) }
# one bench tool at a time (see benchlock.py)
$lock = Join-Path $root 'captures\bench.lock'
if (Test-Path $lock) {
  $h = (Get-Content $lock -Raw).Trim().Split(' ', 2)
  if ([int]$h[0] -ne $PID -and (Get-Process -Id ([int]$h[0]) -ErrorAction SilentlyContinue)) { "BENCH BUSY: '$($h[1])' (pid $($h[0])) is using the scope/emulator."; exit 2 }
}
New-Item -ItemType Directory -Force (Split-Path $lock) | Out-Null
Set-Content $lock "$PID stick.ps1 $Op" -NoNewline
Set-Location (Join-Path $root 'firmware')
if (-not (Test-Path build\flashed.bin) -or (Get-FileHash build\tdsfloppy.bin).Hash -ne (Get-FileHash build\flashed.bin).Hash) { Remove-Item $lock; "FIRMWARE MISMATCH: build\tdsfloppy.bin is not the image last flashed - run make flash-swd first."; exit 3 }
$s = @{}
arm-none-eabi-nm build\tdsfloppy.elf | ForEach-Object { $p = $_ -split '\s+'; if ($p.Count -eq 3) { $s[$p[2]] = "0x" + $p[0] } }
$tmp = Join-Path (Get-Location) 'build\stick'
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $tmp | Out-Null
$name = if ($Op -eq 'put') { $B } else { $A }
$nameBytes = ([Text.Encoding]::ASCII.GetBytes($name) + 0) -join ' '
$tcl = @("init", "write_memory $($s.dbg_name) 8 {$nameBytes}",
  "proc xfer {req off len} {",
  "  mww $($s.dbg_unlock) 0x5AFE1234; mww $($s.dbg_flash_addr) `$off; mww $($s.dbg_flash_len) `$len; mww $($s.dbg_flash_req) `$req",
  "  set t 0; while {[read_memory $($s.dbg_flash_req) 32 1] != 0} { sleep 5; incr t; if {`$t > 2000} { return -99 } }",
  "  set v [read_memory $($s.dbg_flash_len) 32 1]; if {`$v > 0x7FFFFFFF} { set v [expr {`$v - 0x100000000}] }; return `$v }")
if ($Op -eq 'put') {
  $data = [IO.File]::ReadAllBytes($A); $n = 0
  for ($off = 0; $off -lt $data.Length; $off += 512) {
    $len = [Math]::Min(512, $data.Length - $off)
    $f = Join-Path $tmp "c$n.bin"; [IO.File]::WriteAllBytes($f, $data[$off..($off + $len - 1)])
    $tcl += "load_image {$($f.Replace('\','/'))} $($s.dbg_flash_buf) bin"
    $tcl += "set r [xfer 3 $off $len]; if {`$r != $len} { echo `"FAIL chunk $n at $off`: `$r`"; exit }"
    $n++
  }
  $tcl += "echo `"PUT OK $($data.Length) bytes in $n chunks`""
} else {
  $tcl += "set off 0; set i 0"
  $tcl += "while {1} { set r [xfer 4 `$off 512]; if {`$r < 0} { echo `"FAIL at `$off`: `$r`"; exit }; if {`$r == 0} break; dump_image `"$($tmp.Replace('\','/'))/g`$i.bin`" $($s.dbg_flash_buf) `$r; incr off `$r; incr i; if {`$r < 512} break }"
  $tcl += "echo `"GET OK `$off bytes in `$i chunks`""
}
$tcl += "exit"
Set-Content build\stick.cfg -Value ($tcl -join "`n") -Encoding ASCII
$out = openocd -f interface/stlink.cfg -f target/artery/at32f4x.cfg -f build/stick.cfg 2>&1 | Where-Object { "$_" -match '^(PUT|GET) OK|^FAIL' }
if ($Op -eq 'get' -and "$out" -match '^GET OK') {
  $parts = Get-ChildItem $tmp -Filter 'g*.bin' | Sort-Object { [int]($_.BaseName.Substring(1)) }
  $all = New-Object System.Collections.Generic.List[byte]; foreach ($p in $parts) { $all.AddRange([IO.File]::ReadAllBytes($p.FullName)) }
  [IO.File]::WriteAllBytes($B, $all.ToArray())
}
Remove-Item $lock -ErrorAction SilentlyContinue
$out
