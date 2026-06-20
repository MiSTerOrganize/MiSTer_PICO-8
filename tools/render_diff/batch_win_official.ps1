# PICO-8 -x window dumps -> win_off\<id>.txt. LOCAL ONLY. Resumable.
$ErrorActionPreference = "SilentlyContinue"
$exe   = "C:\Users\miste\OneDrive\Desktop\MiSTerOrganize\MiSTerFrontier\#PICO-8_Official\Windows\pico-8_0.2.7a6_windows\pico-8\pico8.exe"
$wrap  = "C:\Users\miste\AppData\Local\Temp\rd_run\win_wrap"
$outd  = "C:\Users\miste\AppData\Local\Temp\rd_run\win_off"
$home2 = "C:\Users\miste\AppData\Local\Temp\p8home"
if (-not (Test-Path $outd)) { New-Item -ItemType Directory -Path $outd | Out-Null }
$tmp = "$env:TEMP\win_one.txt"
$files = Get-ChildItem "$wrap\*.p8" | Sort-Object Name
$n = 0; $tot = $files.Count
foreach ($f in $files) {
  $id = $f.BaseName; $dst = Join-Path $outd "$id.txt"
  if ((Test-Path $dst) -and (Get-Item $dst).Length -gt 0) { continue }
  $p = Start-Process -FilePath $exe -ArgumentList "-x `"$($f.FullName)`" -home `"$home2`"" `
        -RedirectStandardOutput $tmp -RedirectStandardError "$tmp.err" -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(30000)) { $p.Kill() | Out-Null }
  Start-Sleep -Milliseconds 50
  $lines = @(); $lines += (Get-Content $tmp -EA SilentlyContinue); $lines += (Get-Content "$tmp.err" -EA SilentlyContinue)
  $h = $lines | Select-String "^(INFO: )?FBDUMP" | ForEach-Object { ($_ -replace '^INFO: ', '').ToString().Trim() } | Sort-Object -Unique
  Set-Content -Path $dst -Value ($h -join "`n")
  $n++; if ($n % 100 -eq 0) { Write-Host "official-win $n / $tot" }
}
Write-Host "official-win complete: $((Get-ChildItem "$outd\*.txt").Count)"
