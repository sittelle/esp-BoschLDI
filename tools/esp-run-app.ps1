param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"

$script = @"
import serial
import sys
import time

port = sys.argv[1]
baud = int(sys.argv[2])

ser = serial.Serial(port, baud, timeout=0.1)
try:
    # On this ESP32-S3 DevKit, DTR=False can hold BOOT low through the
    # auto-reset circuit. Release BOOT, pulse EN/RTS, then close the port.
    ser.dtr = True
    ser.rts = False
    time.sleep(0.2)
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    time.sleep(0.2)
finally:
    ser.close()
"@

python -c $script $Port $Baud
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Released BOOT and pulsed reset on $Port."
