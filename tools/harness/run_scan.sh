#!/bin/sh
# No-arg launcher: hardcodes the library root so the path never crosses the
# git-bash / PowerShell -> wsl.exe boundary (both mangle inline $vars or long
# paths). Invoke as:  wsl.exe -d docker-desktop sh /tmp/z8/run_scan.sh
exec sh /tmp/z8/scan_library.sh \
  "/mnt/host/c/Users/miste/OneDrive/Desktop/MiSTerOrganize/MiSTerFrontier/PICO-8_Carts/Carts" \
  /tmp/z8/advance_input.txt
