param(
    [string]$Environment = "esp32s3",
    [string]$Port = "",
    [switch]$Clean,
    [switch]$Configure,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Port)) {
    throw "Pass the serial port explicitly, for example: .\tools\pio-upload.ps1 -Port COM7"
}

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root
$tempRoot = Join-Path $root ".pio\tmp"
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$env:TEMP = $tempRoot
$env:TMP = $tempRoot
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot "pio-build.ps1") -Environment $Environment -Target "app" -Clean:$Clean -Configure:$Configure
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

$buildDir = Join-Path $root ".pio\build\$Environment"
$bootloader = Join-Path $buildDir "bootloader\bootloader.bin"
$partitionTable = Join-Path $buildDir "partition_table\partition-table.bin"
$app = Join-Path $buildDir "bosch_ldi_accessory.bin"
foreach ($artifact in @($bootloader, $partitionTable, $app)) {
    if (!(Test-Path $artifact)) {
        throw "Missing flash artifact: $artifact. Run .\tools\pio-build.ps1 -Configure first."
    }
}

$pioPython = Join-Path $env:USERPROFILE ".platformio\penv\.espidf-5.5.4\Scripts\python.exe"
if (!(Test-Path $pioPython)) {
    $pioPython = "python"
}

& $pioPython -m esptool --chip esp32s3 --port $Port --baud 921600 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB `
    0x0 ".pio\build\$Environment\bootloader\bootloader.bin" `
    0x8000 ".pio\build\$Environment\partition_table\partition-table.bin" `
    0x10000 ".pio\build\$Environment\bosch_ldi_accessory.bin"
exit $LASTEXITCODE
