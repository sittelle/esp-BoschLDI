param(
    [string]$Environment = "esp32s3",
    [string]$Target = "app",
    [switch]$Clean,
    [switch]$Configure
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root
$tempRoot = Join-Path $root ".pio\tmp"
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$env:TEMP = $tempRoot
$env:TMP = $tempRoot
$idfPath = Join-Path $env:USERPROFILE ".platformio\packages\framework-espidf"
if (Test-Path $idfPath) {
    $env:IDF_PATH = $idfPath
}
$toolEsptool = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy"
if (Test-Path $toolEsptool) {
    if ([string]::IsNullOrWhiteSpace($env:PYTHONPATH)) {
        $env:PYTHONPATH = $toolEsptool
    } elseif (!$env:PYTHONPATH.Split([System.IO.Path]::PathSeparator).Contains($toolEsptool)) {
        $env:PYTHONPATH = "$toolEsptool$([System.IO.Path]::PathSeparator)$env:PYTHONPATH"
    }
}

$buildDir = Join-Path $root ".pio\build\$Environment"
if ($Clean -and (Test-Path $buildDir)) {
    $resolvedBuildDir = Resolve-Path $buildDir
    $allowedPrefix = Join-Path $root ".pio\build"
    if (!$resolvedBuildDir.Path.StartsWith($allowedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove unexpected build directory: $resolvedBuildDir"
    }
    Remove-Item -LiteralPath $resolvedBuildDir -Recurse -Force
}

$buildNinja = Join-Path $buildDir "build.ninja"
if ($Clean -or $Configure -or !(Test-Path $buildNinja)) {
    Write-Host "Configuring PlatformIO/ESP-IDF project..."
    pio run -e $Environment
    if ($LASTEXITCODE -ne 0) {
        if (!(Test-Path $buildNinja)) {
            exit $LASTEXITCODE
        }
        Write-Host "PlatformIO configuration/build returned $LASTEXITCODE; generated Ninja project exists, continuing with Ninja."
    }
}

$ninja = Join-Path $env:USERPROFILE ".platformio\packages\tool-ninja\ninja.exe"
if (!(Test-Path $ninja)) {
    throw "Ninja not found at $ninja"
}

$lock = Join-Path $buildDir ".ninja_lock"
for ($attempt = 1; $attempt -le 3; $attempt++) {
    Remove-Item -LiteralPath $lock -Force -ErrorAction SilentlyContinue
    if ([string]::IsNullOrWhiteSpace($Target) -or $Target -eq "all") {
        & $ninja -C $buildDir -j 1
    } else {
        & $ninja -C $buildDir -j 1 $Target
    }
    if ($LASTEXITCODE -eq 0) {
        exit 0
    }

    Write-Host "Ninja attempt $attempt failed with $LASTEXITCODE."
    Remove-Item -LiteralPath $lock -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

exit $LASTEXITCODE
