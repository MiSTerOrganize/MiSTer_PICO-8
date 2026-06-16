# singleton_state_check.ps1 -- formalizes full-audit Section 6 process matrix.
# The FIRST-response check for any runtime symptom after a deploy (jitter /
# hot-swap-fail / duplicate-UI / hang): catches dual-daemon + orphan-binary
# state pollution that masquerades as code regressions, BEFORE blaming code.
#
# Runs the PPID-filtered diagnostic on the MiSTer via WinSCP (per the
# never-direct-ssh-use-winscp rule), parses it, and prints PASS/FAIL.
#
# Usage:  pwsh tools/singleton_state_check.ps1   (default MiSTer 192.168.1.51)

param([string]$Ip = "192.168.1.51", [string]$Pw = "1")

$tmp = [System.IO.Path]::GetTempPath()
$sh  = Join-Path $tmp "ss_diag.sh"
$scr = Join-Path $tmp "ss_winscp.txt"

# diagnostic script: PPID=1 filter avoids the subshell false-positive on Master_Daemon
@'
#!/bin/sh
echo "MiSTer=$(pidof MiSTer 2>/dev/null | wc -w)"
echo "MasterDaemon=$(ps -e -o pid,ppid,args 2>/dev/null | grep -E 'bash.*Master_Daemon\.sh' | grep -v grep | awk '$2==1' | wc -l)"
for bin in OpenBOR_4086 OpenBOR_7533 PICO-8; do echo "$bin=$(pidof "$bin" 2>/dev/null | wc -w)"; done
echo "handler=$(ps -e -o args 2>/dev/null | grep '_handler.sh' | grep -v grep | wc -l)"
'@ | Out-File -FilePath $sh -Encoding ascii

@"
option batch on
option confirm off
open scp://root:$Pw@$Ip/ -hostkey=* -timeout=45
put "$sh" "/tmp/ss_diag.sh"
call sh -c "sed -i 's/\r`$//' /tmp/ss_diag.sh; sh /tmp/ss_diag.sh"
exit
"@ | Out-File -FilePath $scr -Encoding ascii

# wake the radio + run (retry once on the usual local ARP blip)
& arp -d $Ip 2>$null | Out-Null; & ipconfig /flushdns | Out-Null
& ping -n 4 $Ip | Out-Null
$out = & "C:\Program Files (x86)\WinSCP\WinSCP.com" /timeout=45 /script="$scr" 2>&1
if ($LASTEXITCODE -ne 0) { & arp -d $Ip 2>$null | Out-Null; & ping -n 6 $Ip | Out-Null; $out = & "C:\Program Files (x86)\WinSCP\WinSCP.com" /timeout=45 /script="$scr" 2>&1 }

$vals = @{}
foreach ($line in $out) { if ($line -match '^\s*(\w[\w-]*)=(\d+)\s*$') { $vals[$Matches[1]] = [int]$Matches[2] } }
if ($vals.Count -eq 0) { Write-Output "ERROR: no diagnostic output (connection failed?). Raw:"; $out | ForEach-Object { Write-Output "  $_" }; exit 2 }

function Row($name,$obs,$ok,$exp) { "{0,-16} exp {1,-10} obs {2,-4} {3}" -f $name,$exp,$obs,$(if($ok){"OK"}else{"** FAIL **"}) }

$hyb = $vals['OpenBOR_4086'] + $vals['OpenBOR_7533'] + $vals['PICO-8']
$fail = 0
$rows = @(
  @{n='MiSTer Main'; o=$vals['MiSTer'];        ok=($vals['MiSTer'] -eq 1);          e='1'},
  @{n='Master_Daemon';o=$vals['MasterDaemon']; ok=($vals['MasterDaemon'] -eq 1);    e='1 (PPID=1)'},
  @{n='OpenBOR_4086'; o=$vals['OpenBOR_4086']; ok=($vals['OpenBOR_4086'] -le 1);    e='0 or 1'},
  @{n='OpenBOR_7533'; o=$vals['OpenBOR_7533']; ok=($vals['OpenBOR_7533'] -le 1);    e='0 or 1'},
  @{n='PICO-8';       o=$vals['PICO-8'];        ok=($vals['PICO-8'] -le 1);          e='0 or 1'},
  @{n='_handler.sh';  o=$vals['handler'];       ok=($vals['handler'] -le 1);         e='0 or 1'},
  @{n='hybrid total'; o=$hyb;                   ok=($hyb -le 1);                     e='0 (MENU) or 1'}
)
Write-Output "== singleton_state_check ($Ip) =="
foreach ($r in $rows) { Write-Output (Row $r.n $r.o $r.ok $r.e); if (-not $r.ok) { $fail++ } }
Write-Output ""
if ($fail) {
  Write-Output "RESULT: $fail FAIL -- STATE POLLUTION. Fix this BEFORE blaming code."
  if ($vals['MasterDaemon'] -ge 2) { Write-Output "  Master_Daemon>=2 = dual-daemon -> jitter/hot-swap-fail/dup-UI on every PAK. Kill the script-spawned one by PID." }
  if ($vals['MiSTer'] -eq 0)       { Write-Output "  MiSTer=0 = TV frozen. Recover: cd /media/fat && nohup ./MiSTer >/tmp/m.log 2>&1 & disown" }
  if ($hyb -ge 2)                  { Write-Output "  2+ hybrid binaries = orphan racing on DDR3. Kill the stale one by PID." }
  exit 1
}
Write-Output "RESULT: clean -- singleton state OK (no dual-daemon / orphan pollution)."
exit 0
