param(
    [string]$Port = "",
    [string]$Environment = "esp32s3",
    [int]$Baud = 460800,
    [switch]$Full,
    [switch]$Clean,
    [switch]$Configure,
    [switch]$SkipBuild,
    [switch]$NoVerify,
    [string]$VerifyUrl = "http://boschldi.local/config",
    [int]$VerifyTimeoutSec = 45
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path $PSScriptRoot
Set-Location $root

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message"
}

function Show-PortsAndExit {
    Write-Host "Pass the ESP32 serial port explicitly, for example:"
    Write-Host "  .\deploy.ps1 -Port COMx"
    Write-Host ""
    Write-Host "Detected serial devices:"
    pio device list
    exit 2
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    Show-PortsAndExit
}

$buildScript = Join-Path $root "tools\pio-build.ps1"
$uploadScript = Join-Path $root "tools\pio-upload.ps1"
if (!(Test-Path $buildScript)) {
    throw "Missing build helper: $buildScript"
}
if (!(Test-Path $uploadScript)) {
    throw "Missing upload helper: $uploadScript"
}

$deployMode = if ($Full) { "full flash: bootloader, partition table, app" } else { "app-only flash: preserves NVS Wi-Fi, Hue pairing, and configuration" }
Write-Step "Deploying Bosch LDI firmware ($deployMode)"
Write-Host "Environment: $Environment"
Write-Host "Port:        $Port"
Write-Host "Baud:        $Baud"

if (!$SkipBuild) {
    Write-Step "Building firmware"
    & $buildScript -Environment $Environment -Target "app" -Clean:$Clean -Configure:$Configure
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} else {
    Write-Step "Skipping build; using existing firmware artifact"
}

Write-Step "Flashing firmware"
& $uploadScript `
    -Port $Port `
    -Environment $Environment `
    -Baud $Baud `
    -SkipBuild:$SkipBuild `
    -AppOnly:(!$Full) `
    -Clean:$Clean `
    -Configure:$Configure
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if ($NoVerify) {
    Write-Step "Deployment complete"
    exit 0
}

Write-Step "Waiting for device web UI"
$deadline = (Get-Date).AddSeconds($VerifyTimeoutSec)
$lastError = $null
while ((Get-Date) -lt $deadline) {
    try {
        $response = Invoke-WebRequest -UseBasicParsing -Uri $VerifyUrl -TimeoutSec 5
        if ($response.StatusCode -ge 200 -and $response.StatusCode -lt 500) {
            Write-Host "Verified: $VerifyUrl returned HTTP $($response.StatusCode)"
            Write-Step "Deployment complete"
            exit 0
        }
    } catch {
        $lastError = $_.Exception.Message
    }
    Start-Sleep -Seconds 2
}

Write-Warning "Flash succeeded, but verification did not reach $VerifyUrl within $VerifyTimeoutSec seconds."
if ($lastError) {
    Write-Warning "Last verification error: $lastError"
}
Write-Host "If mDNS is unavailable, retry with the IP address, for example:"
Write-Host "  .\deploy.ps1 -Port $Port -SkipBuild -VerifyUrl http://192.168.x.y/config"
exit 0
