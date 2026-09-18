# Install Tilt-a-tron on a Waveshare ESP32-S3-Touch-AMOLED-1.75C over USB.
# No ESP-IDF needed: this flashes the prebuilt firmware in firmware/ with esptool.
#
#   .\install.ps1            install (keeps settings, Wi-Fi and themes)
#   .\install.ps1 -Erase     wipe the whole flash first (fresh start)
#   .\install.ps1 -Port COM5 use a specific port
#
# Needs Python 3 (https://python.org, tick "Add python to PATH"); esptool is
# installed automatically the first time.
param(
    [string]$Port = "",
    [switch]$Erase
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$fw = Join-Path $here "firmware"
$manifest = Get-Content (Join-Path $fw "manifest.json") | ConvertFrom-Json

function Find-Python {
    foreach ($c in @("py -3", "python", "python3")) {
        try {
            $v = & cmd /c "$c -c `"import sys; print(sys.version_info[0])`" 2>nul"
            if ("$v".Trim() -eq "3") { return $c }
        } catch {}
    }
    return $null
}

function Find-Port {
    $dev = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match 'VID_303A' } | Select-Object -First 1
    if ($dev -and $dev.FriendlyName -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

Write-Host "Tilt-a-tron installer - firmware $($manifest.version) (build $($manifest.sha))"
$py = Find-Python
if (-not $py) {
    Write-Host "Python 3 is needed. Install it from https://www.python.org/downloads/ (tick 'Add python.exe to PATH'), then run this again." -ForegroundColor Yellow
    exit 1
}
& cmd /c "$py -m esptool version >nul 2>nul"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installing esptool (one time) ..."
    & cmd /c "$py -m pip install --user --quiet esptool"
    if ($LASTEXITCODE -ne 0) { throw "pip could not install esptool" }
}

# esptool 5 spells its commands and options with dashes; 4 used underscores.
$ver = (& cmd /c "$py -m esptool version 2>nul" | Select-Object -First 1) -replace '[^0-9.]', ''
$dash = ([int]($ver.Split('.')[0]) -ge 5)
function Opt($name) { if ($dash) { $name -replace '_', '-' } else { $name } }

# The user's own tools (e.g. a Pal engine polling COM ports) can grab the port; ask them to pause.
try {
    [void](Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:8790/api/usb/pause?seconds=180" -TimeoutSec 1)
    Start-Sleep -Seconds 2   # let it drop the port
} catch {}

if (-not $Port) { $Port = Find-Port }
if (-not $Port) {
    Write-Host ""
    Write-Host "Put the watch in install mode:" -ForegroundColor Cyan
    Write-Host "  1. Unplug it."
    Write-Host "  2. Hold the small BOOT button next to the USB port."
    Write-Host "  3. Plug the USB cable into this PC, keep holding for 2 seconds, then let go."
    Write-Host "Waiting for it ..."
    $deadline = (Get-Date).AddSeconds(120)
    while (-not $Port -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $Port = Find-Port
    }
    if (-not $Port) { throw "No watch found. Check the cable (it must carry data, not just power) and try again." }
}
Write-Host "Found the watch on $Port"

$files = @()
foreach ($f in $manifest.files) { $files += $f.offset; $files += (Join-Path $fw $f.file) }
$common = @("--chip", $manifest.chip, "--port", $Port, "--baud", "921600", "--before", (Opt "default_reset"), "--after", (Opt "hard_reset"))

if ($Erase) {
    Write-Host "Erasing flash ..."
    & cmd /c "$py -m esptool $($common -join ' ') $(Opt 'erase_flash')"
    if ($LASTEXITCODE -ne 0) { throw "erase failed" }
}
Write-Host "Writing firmware ..."
$flash = @((Opt "write_flash"), (Opt "--flash_mode"), $manifest.flash.mode, (Opt "--flash_freq"), $manifest.flash.freq, (Opt "--flash_size"), $manifest.flash.size) + $files
& cmd /c "$py -m esptool $($common -join ' ') $($flash -join ' ')"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Flashing failed. If the watch was running, put it in install mode (hold BOOT while plugging in) and run this again." -ForegroundColor Red
    exit 1
}
Write-Host ""
Write-Host "Done! The watch is restarting into Tilt-a-tron." -ForegroundColor Green
Write-Host "Tips: swipe the home screen to pick a game, tap to play, BOOT goes home, double-click PWR to sleep."
Write-Host "      Settings > WI-FI lets you update over Wi-Fi next time (.\update.ps1)."
