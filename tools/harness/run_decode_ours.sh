#!/bin/sh
# No-arg launcher for the ours-side decode scan (hardcoded root so the path
# never crosses the git-bash/PowerShell -> wsl.exe boundary).
# Invoke: wsl.exe -d docker-desktop sh -c 'sh /tmp/z8/run_decode_ours.sh'
exec sh /tmp/z8/scan_decode_ours.sh \
  "/mnt/host/c/Users/miste/OneDrive/Desktop/MiSTerOrganize/MiSTerFrontier/PICO-8_Carts/Carts"
