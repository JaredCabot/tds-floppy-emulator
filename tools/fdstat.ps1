# fdstat.ps1 - read the floppy layer's debug state from the running emulator over SWD.
# Usage: powershell -File tools\fdstat.ps1 [-Steps N]
# Prints counters, the track-build trace and the last N STEP events.
param([int]$Steps = 24)
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
$fw = Join-Path $PSScriptRoot '..\firmware' | Resolve-Path
Set-Location $fw

$a = @{}
if (-not (Test-Path build\flashed.bin) -or (Get-FileHash build\tdsfloppy.bin).Hash -ne (Get-FileHash build\flashed.bin).Hash) { "FIRMWARE MISMATCH: build\tdsfloppy.bin is not the image last flashed - run make flash-swd first (symbol addresses would be wrong)."; exit 3 }
arm-none-eabi-nm build\tdsfloppy.elf | Select-String ' (flpy_dbg_\w+|s_cyl|s_head|s_selected)(\.lto_priv\.\d+)?$' |   # (LTO renames statics)
  ForEach-Object { $p = $_.Line -split ' '; $a[($p[2] -replace '\.lto_priv\.\d+$','')] = '0x' + $p[0] }

$cfg = @"
init
set fp [open "build/fdstat.txt" w]
foreach {n a w} {ms $($a.flpy_dbg_ms) 32 index $($a.flpy_dbg_index_count) 32 steps $($a.flpy_dbg_step_count) 32 sels $($a.flpy_dbg_sel_count) 32 load_us $($a.flpy_dbg_load_us) 32 store_us $($a.flpy_dbg_store_us) 32 wr_gates $($a.flpy_dbg_wr_gates) 32 wr_sectors $($a.flpy_dbg_wr_sectors) 32 wr_badcrc $($a.flpy_dbg_wr_badcrc) 32 wr_lost $($a.flpy_dbg_wr_lost) 32 vfail $($a.flpy_dbg_store_verify_fail) 32 ldirty $($a.flpy_dbg_load_while_dirty) 32 trkchg $($a.flpy_dbg_wr_track_changed) 32 cyl $($a.s_cyl) 8 head $($a.s_head) 8 sel $($a.s_selected) 8 trace_n $($a.flpy_dbg_trace_n) 32} {
  puts `$fp "`$n [read_memory `$a `$w 1]"
}
puts `$fp "trace [read_memory $($a.flpy_dbg_trace) 32 64]"
puts `$fp "steplog [read_memory $($a.flpy_dbg_steplog) 32 64]"
close `$fp
exit
"@
$cfg | Out-File -Encoding ascii build\fdstat.cfg
openocd -f interface/stlink.cfg -f target/artery/at32f4x.cfg -f build/fdstat.cfg 2>$null

function num($x) { [Convert]::ToUInt32(($x -replace '^0x',''), 16) }
$v = @{}
Get-Content build\fdstat.txt | ForEach-Object { $k, $r = $_ -split ' ', 2; $v[$k] = $r }
'uptime {0} ms | index {1} | steps {2} | selects {3} | now cyl {4} head {5} sel {6}' -f `
  (num $v.ms), (num $v.index), (num $v.steps), (num $v.sels), (num $v.cyl), (num $v.head), (num $v.sel)
'track load {0} us, store {1} us | writes: gates {2}, sectors {3}, bad CRC {4}, lost {5}' -f `
  (num $v.load_us), (num $v.store_us), (num $v.wr_gates), (num $v.wr_sectors), (num $v.wr_badcrc), (num $v.wr_lost)
'self-checks (should be 0): store verify failures {0}, loads while dirty {1}, track changed mid-write {2}' -f (num $v.vfail), (num $v.ldirty), (num $v.trkchg)

function ring($name, $count, $show, $fmt) {
  $t = $v[$name] -split ' ' | ForEach-Object { num $_ }
  $start = [Math]::Max([Math]::Max(0, $count - 64), $count - $show)
  for ($i = $start; $i -lt $count; $i++) { & $fmt $i $t[$i % 64] }
}
$tn = num $v.trace_n
"track loads/stores: $tn (last 12)"
ring 'trace' $tn 12 { param($i, $w) '  #{0,-4} t={1,5} ms  {4} cyl {2,2} head {3}' -f $i, ($w -shr 16), ($w -band 0xFF), (($w -shr 8) -band 1), $(if (($w -shr 15) -band 1) {'STORE'} else {'load '}) }
$sn = num $v.steps
"steps: $sn (last $Steps)  [t = ms mod 65536]"
ring 'steplog' $sn $Steps { param($i, $w) '  #{0,-4} t={1,5} ms  {2}  -> cyl {3,2}' -f $i, ($w -shr 16), $(if (($w -shr 8) -band 1) {'IN '} else {'OUT'}), ($w -band 0xFF) }
