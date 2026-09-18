# One command to put the current code on the Tilt-a-tron.
#
#   .\update.ps1            build, then install over Wi-Fi if the watch answers,
#                           otherwise over USB if it's plugged in (debug mode)
#   .\update.ps1 -NoBuild   install the last build
#   .\update.ps1 -Usb       force USB        .\update.ps1 -Wifi   force Wi-Fi
#   .\update.ps1 -Log       print the watch's recent log (over Wi-Fi)
#   .\update.ps1 -Status    show what the watch is running
#   .\update.ps1 -Crash     the last crash, with its backtrace turned into source lines
#   .\update.ps1 -Bin releases\pre-canvas.bin   install a saved build (roll back)
#
# Every build has a unique hash; the script compares the watch's hash with the
# file it sent, so "OK" really means the new build is running.
param(
    [string]$Ip = "",
    [switch]$NoBuild,
    [switch]$Usb,
    [switch]$Wifi,
    [switch]$Log,
    [switch]$Status,
    [switch]$Crash,
    [string]$Bin = ""
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$ipCache = Join-Path $PSScriptRoot "build\.watch-ip"

function Get-WatchStatus($target) {
    try { return (Invoke-WebRequest -UseBasicParsing "http://$target/status" -TimeoutSec 5).Content | ConvertFrom-Json }
    catch { return $null }
}

# Find the watch on the network: explicit -Ip, then its .local name, then the last address that worked.
function Find-Watch {
    $candidates = @()
    if ($Ip) { $candidates += $Ip }
    try {
        $candidates += ([System.Net.Dns]::GetHostAddresses("tilt-a-tron.local") |
            Where-Object { $_.AddressFamily -eq 'InterNetwork' } | ForEach-Object { $_.IPAddressToString })
    } catch {}
    if (Test-Path $ipCache) { $candidates += (Get-Content $ipCache -ErrorAction SilentlyContinue) }
    foreach ($c in ($candidates | Where-Object { $_ } | Select-Object -Unique)) {
        $s = Get-WatchStatus $c
        if ($s) {
            New-Item -ItemType Directory -Force (Split-Path $ipCache) | Out-Null
            Set-Content -Path $ipCache -Value $c
            return @{ Ip = $c; Status = $s }
        }
    }
    return $null
}

function Find-UsbPort {
    $dev = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match 'VID_303A' } | Select-Object -First 1
    if ($dev -and $dev.FriendlyName -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

# The build's fingerprint: first 8 bytes of the ELF SHA-256 stored in the image header.
function Get-BinSha($bin) {
    $fs = [IO.File]::OpenRead($bin)
    try {
        $buf = New-Object byte[] 8
        $fs.Position = 0x20 + 144
        [void]$fs.Read($buf, 0, 8)
    } finally { $fs.Close() }
    return -join ($buf | ForEach-Object { $_.ToString("x2") })
}

if ($Log -or $Status -or $Crash) {
    $w = Find-Watch
    if (-not $w) { throw "The watch isn't answering on Wi-Fi (is WI-FI ON in Settings, and is it awake?)." }
    if ($Status) { $w.Status | Format-List; return }
    if ($Crash) {
        $text = "$($w.Status.crash)"
        if (-not $text) { Write-Host "No crash recorded since the last boot."; return }
        Write-Host $text
        $addrs = [regex]::Matches($text, '\b4[0-9a-f]{7}\b') | ForEach-Object { "0x" + $_.Value }
        $elf = Join-Path $PSScriptRoot "build\tiltatron.elf"
        $a2l = Get-ChildItem "C:\Espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin\xtensa-esp32s3-elf-addr2line.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($addrs -and $a2l -and (Test-Path $elf)) {
            if ($w.Status.sha -ne (Get-BinSha (Join-Path $PSScriptRoot "build\tiltatron.bin"))) {
                Write-Host "(note: build/ is a different build than the watch runs; lines may be off)" -ForegroundColor Yellow
            }
            & $a2l.FullName -pfiaC -e $elf $addrs
        }
        return
    }
    (Invoke-WebRequest -UseBasicParsing "http://$($w.Ip)/log" -TimeoutSec 10).Content
    return
}

if (-not $NoBuild -and -not $Bin) {
    Write-Host "Building ..."
    $ErrorActionPreference = "Continue"   # the compiler writes to stderr; that's not a script error
    $out = powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\idf.ps1" build 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($code -ne 0) {
        Write-Host "Build failed:" -ForegroundColor Red
        $out | Where-Object { $_ -match "(error|undefined reference)" -and $_ -notmatch "-Werror" } |
            Select-Object -First 15 | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        exit 1
    }
}
$bin = if ($Bin) { $Bin } else { Join-Path $PSScriptRoot "build\tiltatron.bin" }
if (-not (Test-Path $bin)) { throw "No build found at $bin" }
$want = Get-BinSha $bin
$sizeKB = [math]::Round((Get-Item $bin).Length / 1024)

# ---------------------------------------------------------------- Wi-Fi
$watch = $null
if (-not $Usb) { $watch = Find-Watch }

if ($watch) {
    if ($watch.Status.sha -eq $want) {
        Write-Host "Already up to date (build $want)." -ForegroundColor Green
        return
    }
    # The update restarts the watch. If its drive is open here, Windows may not have
    # written everything yet, and the restart corrupts the file table: eject first.
    $drives = Get-Volume -ErrorAction SilentlyContinue | Where-Object {
        $_.DriveLetter -and (Test-Path "$($_.DriveLetter):\Theme") -and (Test-Path "$($_.DriveLetter):\Guide") }
    foreach ($d in $drives) {
        Write-Host "Ejecting the Tilt-a-tron drive ($($d.DriveLetter):) so nothing is lost ..."
        try { (New-Object -ComObject Shell.Application).NameSpace(17).ParseName("$($d.DriveLetter):").InvokeVerb("Eject") } catch {}
        Start-Sleep -Seconds 4   # the watch reloads its theme when the drive comes back
    }
    Write-Host "Sending $sizeKB KB to $($watch.Ip) over Wi-Fi ..."
    $sent = $false
    foreach ($attempt in 1..3) {
        try {
            $sw = [Diagnostics.Stopwatch]::StartNew()
            [void](Invoke-WebRequest -UseBasicParsing -Method Post -Uri "http://$($watch.Ip)/update" -InFile $bin `
                -ContentType "application/octet-stream" -TimeoutSec 180)
            Write-Host ("  sent in {0:N1} s" -f $sw.Elapsed.TotalSeconds)
            $sent = $true
            break
        } catch {
            Write-Host "  attempt $attempt failed: $($_.Exception.Message)"
            Start-Sleep -Seconds 3
        }
    }
    if ($sent) {
        Write-Host "Waiting for the watch to restart ..."
        Start-Sleep -Seconds 7
        $after = $null
        foreach ($i in 1..20) { $after = Get-WatchStatus $watch.Ip; if ($after) { break }; Start-Sleep -Seconds 2 }
        if ($after -and $after.sha -eq $want) {
            Write-Host "OK: the watch is running build $want." -ForegroundColor Green
            if ($after.safe) { Write-Host "  (but it's in SAFE MODE: $($after.crash))" -ForegroundColor Yellow }
            return
        }
        if ($after) {
            Write-Host "The watch restarted but is running build $($after.sha), not $want." -ForegroundColor Red
            if ($after.crash) { Write-Host "  It reported: $($after.crash)" -ForegroundColor Red }
        } else {
            Write-Host "The watch didn't come back on Wi-Fi." -ForegroundColor Red
        }
    }
    if ($Wifi) { exit 1 }
    Write-Host "Trying USB instead ..."
} elseif ($Wifi) {
    throw "The watch isn't answering on Wi-Fi (is WI-FI ON in Settings, and is it awake?)."
}

# ---------------------------------------------------------------- USB
$port = Find-UsbPort
if (-not $port) {
    Write-Host ""
    Write-Host "Can't reach the watch. Any one of these will do:" -ForegroundColor Yellow
    Write-Host "  - Wi-Fi: wake it, and make sure Settings > WI-FI is ON."
    Write-Host "  - USB:   Settings > DEBUG MODE ON, plug it into this PC, and restart the watch."
    Write-Host "  - Always works: unplug it, hold BOOT, plug it back in, release after 2 s."
    exit 1
}
# The Pal engine polls Espressif serial ports and would grab this one mid-flash.
try { [void](Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:8790/api/usb/pause?seconds=600" -TimeoutSec 2) } catch {}
Write-Host "Flashing over USB ($port) ..."
$ErrorActionPreference = "Continue"
$out = powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\idf.ps1" -p $port flash 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($code -ne 0) {
    Write-Host "USB flash failed:" -ForegroundColor Red
    $out | Where-Object { $_ -match "fatal|busy|denied|No such" } | Select-Object -First 6 |
        ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
Write-Host "OK: flashed build $want over USB." -ForegroundColor Green
