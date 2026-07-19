# gen_goldens.ps1 -- regenerate goldens.txt from the LOCAL reference PICO-8
# binary (never committed/shipped/CI'd -- dev machine only; we ship zepto8).
# Uses .NET Process stream redirection: printh goes to the GUI app's stderr,
# which PowerShell-native 2>&1 wraps in ErrorRecords and cmd redirection
# loses entirely.
#
#   powershell -ExecutionPolicy Bypass -File gen_goldens.ps1 -P8 <path\to\pico8.exe>
#
# Writes per-cart blocks into goldens.txt (LF, C-locale-stable content --
# run_conformance.sh re-sorts both sides at compare time, so stored order is
# cosmetic). Review the diff before committing: changed values must be
# explained by an intentional cart/generator change.
param(
  [Parameter(Mandatory=$true)][string]$P8,
  [string]$Home8 = "$env:TEMP\p8conf_home"
)
$conf = $PSScriptRoot
Set-Location $conf
New-Item -ItemType Directory -Force $Home8 | Out-Null

$blocks = @()
foreach ($c in (Get-ChildItem *.p8 | Sort-Object Name)) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $P8
  $psi.Arguments = "-x `"$($c.FullName)`" -home `"$Home8`""
  $psi.RedirectStandardError = $true
  $psi.RedirectStandardOutput = $true
  $psi.UseShellExecute = $false
  $p = [System.Diagnostics.Process]::Start($psi)
  $so = $p.StandardOutput.ReadToEnd()
  $se = $p.StandardError.ReadToEnd()
  $p.WaitForExit()
  $lines = (($so + "`n" + $se) -split "`r?`n") |
           Where-Object { $_ -match "^INFO: CONF(HASH|VAL)" } |
           ForEach-Object { $_ -replace "^INFO:\s*","" } | Sort-Object -Unique
  Write-Host "$($c.BaseName) : $(($lines | Measure-Object).Count) lines"
  $blocks += "[$($c.BaseName)]"
  $blocks += $lines
  $blocks += ""
}
# Preserve the existing header (comment lines up to the first [block]).
$header = @()
foreach ($l in (Get-Content goldens.txt)) {
  if ($l -match "^\[") { break }
  $header += $l
}
$out = ($header + $blocks) -join "`n"
[System.IO.File]::WriteAllText("$conf\goldens.txt", $out + "`n")
Write-Host "goldens.txt regenerated -- review `git diff` before committing."
