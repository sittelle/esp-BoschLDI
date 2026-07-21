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
$env:UV_CACHE_DIR = Join-Path $tempRoot "uv-cache"
$env:PLATFORMIO_OFFLINE = "1"
$pythonScripts = Join-Path $env:USERPROFILE "scoop\apps\python\current\Scripts"
if (Test-Path $pythonScripts) {
    $pathParts = $env:PATH.Split([System.IO.Path]::PathSeparator)
    if (!$pathParts.Contains($pythonScripts)) {
        $env:PATH = "$pythonScripts$([System.IO.Path]::PathSeparator)$env:PATH"
    }
}
$pioPenv = Join-Path $env:USERPROFILE ".platformio\penv"
$pioPenvPython = Join-Path $pioPenv "Scripts\python.exe"
$pioPenvConfig = Join-Path $pioPenv "pyvenv.cfg"
if ((Test-Path $pioPenvPython) -and !(Test-Path $pioPenvConfig)) {
    $hostPython = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (!$hostPython) {
        $hostPython = Join-Path $env:USERPROFILE "scoop\apps\python\current\python.exe"
    }
    $hostPythonHome = Split-Path -Parent $hostPython
    $hostVersion = & $hostPython -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}')"
    $cfg = "home = $hostPythonHome`r`ninclude-system-site-packages = false`r`nversion = $hostVersion`r`nexecutable = $hostPython`r`ncommand = $hostPython -m venv $pioPenv`r`n"
    Set-Content -LiteralPath $pioPenvConfig -Value $cfg -NoNewline
    Write-Host "Restored missing PlatformIO penv pyvenv.cfg."
}
$pioPenvScripts = Join-Path $pioPenv "Scripts"
$pioPenvUv = Join-Path $pioPenvScripts "uv.exe"
if ((Test-Path $pioPenvPython) -and !(Test-Path $pioPenvUv)) {
    $platformioRoot = Join-Path $env:USERPROFILE ".platformio"
    $candidateUv = Get-ChildItem -LiteralPath $platformioRoot -Filter "uv.exe" -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notlike "$pioPenvScripts*" } |
        Select-Object -First 1
    if ($candidateUv) {
        try {
            Copy-Item -LiteralPath $candidateUv.FullName -Destination $pioPenvUv -Force
            Write-Host "Restored missing PlatformIO penv uv.exe from $($candidateUv.FullName)."
        } catch {
            Write-Warning "Could not restore PlatformIO penv uv.exe from $($candidateUv.FullName): $($_.Exception.Message)"
        }
    }
}
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
$pioPenvSitePackages = Join-Path $pioPenv "Lib\site-packages"
if (Test-Path $pioPenvSitePackages) {
    if ([string]::IsNullOrWhiteSpace($env:PYTHONPATH)) {
        $env:PYTHONPATH = $pioPenvSitePackages
    } elseif (!$env:PYTHONPATH.Split([System.IO.Path]::PathSeparator).Contains($pioPenvSitePackages)) {
        $env:PYTHONPATH = "$pioPenvSitePackages$([System.IO.Path]::PathSeparator)$env:PYTHONPATH"
    }
}

function Ensure-PenvModuleFromUvCache {
    param(
        [string]$ModuleName,
        [string]$DistInfoPrefix
    )

    if (!(Test-Path $pioPenvPython) -or !(Test-Path $pioPenvSitePackages)) {
        return
    }

    & $pioPenvPython -c "import $ModuleName" 2>$null
    if ($LASTEXITCODE -eq 0) {
        return
    }

    $platformioRoot = Join-Path $env:USERPROFILE ".platformio"
    $archiveRoot = Join-Path $platformioRoot ".cache\uv\archive-v0"
    if (!(Test-Path $archiveRoot)) {
        Write-Warning "PlatformIO penv is missing Python module '$ModuleName', and uv archive cache was not found."
        return
    }

    $abiTag = & $pioPenvPython -c "import sys; print(f'cp{sys.version_info.major}{sys.version_info.minor}')"
    $candidates = Get-ChildItem -LiteralPath $archiveRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object {
            (Test-Path (Join-Path $_.FullName $ModuleName)) -and
            (Get-ChildItem -LiteralPath $_.FullName -Directory -Filter "$DistInfoPrefix*.dist-info" -ErrorAction SilentlyContinue)
        }

    $selected = $null
    foreach ($candidate in $candidates) {
        $modulePath = Join-Path $candidate.FullName $ModuleName
        $nativeFiles = Get-ChildItem -LiteralPath $modulePath -Recurse -File -Include "*.pyd", "*.so" -ErrorAction SilentlyContinue
        if (!$nativeFiles -or ($nativeFiles | Where-Object { $_.Name -like "*$abiTag*" })) {
            $selected = $candidate
            break
        }
    }

    if (!$selected) {
        Write-Warning "PlatformIO penv is missing Python module '$ModuleName', and no compatible uv archive package was found."
        return
    }

    Copy-Item -LiteralPath (Join-Path $selected.FullName $ModuleName) -Destination $pioPenvSitePackages -Recurse -Force
    Get-ChildItem -LiteralPath $selected.FullName -Directory -Filter "$DistInfoPrefix*.dist-info" -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $pioPenvSitePackages -Recurse -Force }
    Write-Host "Restored PlatformIO penv Python module '$ModuleName' from uv cache."
}

Ensure-PenvModuleFromUvCache -ModuleName "certifi" -DistInfoPrefix "certifi-"
Ensure-PenvModuleFromUvCache -ModuleName "littlefs" -DistInfoPrefix "littlefs_python-"
Ensure-PenvModuleFromUvCache -ModuleName "fatfs" -DistInfoPrefix "fatfs_ng-"
function Repair-Esp32S3Toolchain {
    param([string]$BuildDirectory)

    $unifiedBin = Join-Path $env:USERPROFILE ".platformio\packages\toolchain-xtensa-esp-elf\bin"
    $aliasCompiler = Join-Path $unifiedBin "xtensa-esp32s3-elf-gcc.exe"
    $genericCompiler = Join-Path $unifiedBin "xtensa-esp-elf-gcc.exe"
    if ((Test-Path $aliasCompiler) -and (Test-Path $genericCompiler)) {
        $machine = & $aliasCompiler -dumpmachine 2>$null
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($machine)) {
            Write-Host "Repairing ESP32-S3 toolchain aliases in toolchain-xtensa-esp-elf."
            $tools = @(
                "addr2line", "ar", "as", "c++", "c++filt", "cc", "cpp",
                "elfedit", "g++", "gcc", "gcc-ar", "gcc-nm", "gcc-ranlib",
                "gcov-dump", "gcov-tool", "gcov", "gprof", "ld", "ld.bfd",
                "lto-dump", "nm", "objcopy", "objdump", "ranlib", "readelf",
                "size", "strings", "strip"
            )
            foreach ($tool in $tools) {
                $src = Join-Path $unifiedBin "xtensa-esp-elf-$tool.exe"
                $dst = Join-Path $unifiedBin "xtensa-esp32s3-elf-$tool.exe"
                if ((Test-Path $src) -and (Test-Path $dst)) {
                    Copy-Item -LiteralPath $src -Destination $dst -Force
                }
            }
            $machine = & $aliasCompiler -dumpmachine
        }
        if ($LASTEXITCODE -ne 0 -or $machine.Trim() -notmatch "xtensa") {
            throw "Unexpected ESP32-S3 compiler target '$machine' from $aliasCompiler"
        }
        return
    }

    $s3Compiler = Join-Path $env:USERPROFILE ".platformio\packages\toolchain-xtensa-esp32s3\bin\xtensa-esp32s3-elf-gcc.exe"
    if (!(Test-Path $s3Compiler)) {
        return
    }

    $machine = & $s3Compiler -dumpmachine
    if ($LASTEXITCODE -ne 0) {
        throw "ESP32-S3 compiler exists but failed to run: $s3Compiler"
    }
    if ($machine.Trim() -ne "xtensa-esp32s3-elf") {
        throw "Unexpected ESP32-S3 compiler target '$machine' from $s3Compiler"
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
Repair-Esp32S3Toolchain -BuildDirectory $buildDir
if ($Clean -or $Configure -or !(Test-Path $buildNinja)) {
    Write-Host "Configuring PlatformIO/ESP-IDF project..."
    pio run -e $Environment
    if ($LASTEXITCODE -ne 0) {
        if (!(Test-Path $buildNinja)) {
            exit $LASTEXITCODE
        }
        Write-Host "PlatformIO configuration/build returned $LASTEXITCODE; generated Ninja project exists, continuing with Ninja."
    }
    Repair-Esp32S3Toolchain -BuildDirectory $buildDir
}

$ninja = Join-Path $env:USERPROFILE ".platformio\packages\tool-ninja\ninja.exe"
if (!(Test-Path $ninja)) {
    throw "Ninja not found at $ninja"
}

$lock = Join-Path $buildDir ".ninja_lock"
for ($attempt = 1; $attempt -le 3; $attempt++) {
    Repair-Esp32S3Toolchain -BuildDirectory $buildDir
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
