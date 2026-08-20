#!/usr/bin/env bash
#
# monitor-serial.sh — tail the board's debug UART over the ST-Link virtual COM
# port. Auto-detects the port (don't hardcode COM7 — it can shift), 115200 baud
# per firmware_config.h's DEBUG_UART_BAUDRATE.
#
# Usage:
#   ./monitor-serial.sh              # stream until Ctrl-C
#   ./monitor-serial.sh 60           # stream for 60 seconds then exit

set -euo pipefail

DURATION="${1:-0}"   # 0 = run until Ctrl-C

powershell.exe -NoProfile -Command "
\$portInfo = Get-WmiObject Win32_PnPEntity | Where-Object { \$_.Name -match 'STLink.*Virtual COM Port \((COM[0-9]+)\)' }
if (-not \$portInfo) {
    Write-Error 'No ST-Link Virtual COM Port found. Is the board plugged in via USB?'
    exit 1
}
\$portName = [regex]::Match(\$portInfo.Name, 'COM[0-9]+').Value
Write-Host \"==> Monitoring \$portName @115200\" -ForegroundColor Cyan

\$port = New-Object System.IO.Ports.SerialPort \$portName,115200,None,8,One
\$port.ReadTimeout = 500
\$duration = $DURATION
try {
    \$port.Open()
    \$sw = [System.Diagnostics.Stopwatch]::StartNew()
    while (\$duration -eq 0 -or \$sw.Elapsed.TotalSeconds -lt \$duration) {
        try { Write-Output \$port.ReadLine() } catch [System.TimeoutException] { }
    }
} catch {
    Write-Error \$_.Exception.Message
    exit 1
} finally {
    if (\$port.IsOpen) { \$port.Close() }
}
"
