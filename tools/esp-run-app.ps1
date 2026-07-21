param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"

$toolEsptool = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy"
if (Test-Path $toolEsptool) {
    if ([string]::IsNullOrWhiteSpace($env:PYTHONPATH)) {
        $env:PYTHONPATH = $toolEsptool
    } elseif (!$env:PYTHONPATH.Split([System.IO.Path]::PathSeparator).Contains($toolEsptool)) {
        $env:PYTHONPATH = "$toolEsptool$([System.IO.Path]::PathSeparator)$env:PYTHONPATH"
    }
}
$pioPenvSitePackages = Join-Path $env:USERPROFILE ".platformio\penv\Lib\site-packages"
if (Test-Path $pioPenvSitePackages) {
    if ([string]::IsNullOrWhiteSpace($env:PYTHONPATH)) {
        $env:PYTHONPATH = $pioPenvSitePackages
    } elseif (!$env:PYTHONPATH.Split([System.IO.Path]::PathSeparator).Contains($pioPenvSitePackages)) {
        $env:PYTHONPATH = "$pioPenvSitePackages$([System.IO.Path]::PathSeparator)$env:PYTHONPATH"
    }
}

$pioPython = Join-Path $env:USERPROFILE ".platformio\penv\.espidf-5.5.4\Scripts\python.exe"
if (!(Test-Path $pioPython)) {
    $pioPython = "python"
}

& $pioPython -m esptool --chip esp32s3 --port $Port --baud $Baud run
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Started app from flash on $Port."
