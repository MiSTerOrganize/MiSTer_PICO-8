# Phase C -- official PICO-8 (-x headless, GROUND TRUTH) over every wrapper.
# LOCAL ONLY: official PICO-8 is a reference oracle, never shipped/committed/in-CI.
# Resumable via off_done.txt. Detects "program too large" -> OVERSIZE (8192-token cap).
# Output format = compare_render.py's parser (## <id> + FBHASH/AUDHASH / OVERSIZE).
$ErrorActionPreference = "SilentlyContinue"
$exe   = "C:\Users\miste\OneDrive\Desktop\MiSTerOrganize\MiSTerFrontier\#PICO-8_Official\Windows\pico-8_0.2.7a6_windows\pico-8\pico8.exe"
$wrap  = "C:\Users\miste\AppData\Local\Temp\rd_run\wrap"
$out   = "C:\Users\miste\AppData\Local\Temp\rd_run\off_results.txt"
$done  = "C:\Users\miste\AppData\Local\Temp\rd_run\off_done.txt"
$home2 = "C:\Users\miste\AppData\Local\Temp\p8home"
if (-not (Test-Path $home2)) { New-Item -ItemType Directory -Path $home2 | Out-Null }
if (-not (Test-Path $done))  { New-Item -ItemType File -Path $done | Out-Null }
if (-not (Test-Path $out))   { New-Item -ItemType File -Path $out  | Out-Null }
$doneset = @{}; foreach ($d in (Get-Content $done)) { $doneset[$d] = 1 }
$files = Get-ChildItem "$wrap\*.p8" | Sort-Object Name
$tmp = "$env:TEMP\off_one.txt"
$n = 0; $tot = $files.Count
foreach ($f in $files) {
  $id = $f.BaseName
  if ($doneset.ContainsKey($id)) { continue }
  $p = Start-Process -FilePath $exe -ArgumentList "-x `"$($f.FullName)`" -home `"$home2`"" `
        -RedirectStandardOutput $tmp -RedirectStandardError "$tmp.err" -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(20000)) { $p.Kill() | Out-Null }
  Start-Sleep -Milliseconds 40
  $lines = @(); $lines += (Get-Content $tmp -ErrorAction SilentlyContinue)
  $lines += (Get-Content "$tmp.err" -ErrorAction SilentlyContinue)
  Add-Content $out "## $id"
  if ($lines -match "program too large|too many tokens|out of memory") { Add-Content $out "OVERSIZE" }
  $hashes = $lines | Select-String "^(INFO: )?(FBHASH|AUDHASH)" |
            ForEach-Object { ($_ -replace '^INFO: ', '').ToString().Trim() } | Sort-Object -Unique
  if ($hashes) { Add-Content $out $hashes }
  Add-Content $done $id
  $n++
  if ($n % 100 -eq 0) { Write-Host "official $n done (of $tot remaining-at-start)" }
}
Write-Host "official phase complete: $((Get-Content $done).Count) carts in done-list"
