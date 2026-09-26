# gatelog.ps1 - dump the FIRST 64 write gates and FIRST 64 steps since reset:
# time, track, ID/data fields decoded, whether the track was loaded (visible)
# when the gate opened, first sector; and each step's direction and cylinder.
$env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
Set-Location (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')) 'firmware')
$s = @{}
if (-not (Test-Path build\flashed.bin) -or (Get-FileHash build\tdsfloppy.bin).Hash -ne (Get-FileHash build\flashed.bin).Hash) { "FIRMWARE MISMATCH: build\tdsfloppy.bin is not the image last flashed - run make flash-swd first (symbol addresses would be wrong)."; exit 3 }
arm-none-eabi-nm build\tdsfloppy.elf | ForEach-Object { $p = $_ -split '\s+'; if ($p.Count -eq 3) { $s[$p[2]] = "0x" + $p[0] } }
"init`necho `"n=[read_memory $($s.flpy_dbg_wr_gates) 32 1]`"`necho `"a=[read_memory $($s.flpy_dbg_gate_a) 32 64]`"`necho `"b=[read_memory $($s.flpy_dbg_gate_b) 32 64]`"`necho `"sn=[read_memory $($s.flpy_dbg_step_count) 32 1]`"`necho `"st=[read_memory $($s.flpy_dbg_steplog) 32 64]`"`nexit" |
  Set-Content build\gatelog.cfg -Encoding ASCII
$out = openocd -f interface/stlink.cfg -f target/artery/at32f4x.cfg -f build/gatelog.cfg 2>&1
$v = @{}; foreach ($l in $out) { if ("$l" -match '^(n|a|b|sn|st)=(.*)$') { $v[$matches[1]] = @($matches[2].Trim() -split '\s+' | ForEach-Object { [Convert]::ToUInt32($_, 16) }) } }
"write gates: $($v.n[0]) (first {0} shown)" -f [Math]::Min($v.n[0], 64)
for ($i = 0; $i -lt [Math]::Min($v.n[0], 64); $i++) {
  $a = $v.a[$i]; $b = $v.b[$i]
  "  gate #{0,-3} t={1,5} ms  cyl {2,2} head {3}  ids {4,2} data {5,2}  visible {6}  first {7}" -f $i, ($a -shr 16), (($a -shr 8) -band 0xFF), ($a -band 0xFF), ($b -shr 24), (($b -shr 16) -band 0xFF), (($b -shr 8) -band 1), $(if (($b -band 0xFF) -eq 255) { '-' } else { $b -band 0xFF })
}
"steps: $($v.sn[0]) (first {0} shown)" -f [Math]::Min($v.sn[0], 64)
$line = ""
for ($i = 0; $i -lt [Math]::Min($v.sn[0], 64); $i++) {
  $w = $v.st[$i]; $line += "{0}{1}@{2} " -f $(if ((($w -shr 8) -band 1) -eq 1) { '+' } else { '-' }), ($w -band 0xFF), ($w -shr 16)
}
$line
