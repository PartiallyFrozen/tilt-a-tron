# Copies the current build into firmware/ so people can install Tilt-a-tron
# without ESP-IDF (see install.ps1 / install.sh). Run after a build:
#
#   .\tools\make_release.ps1
param()
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$build = Join-Path $root "build"
$out = Join-Path $root "firmware"
$args = Get-Content (Join-Path $build "flasher_args.json") | ConvertFrom-Json
if (-not (Test-Path (Join-Path $build "tiltatron.bin"))) { throw "No build in $build - run .\update.ps1 or .\idf.ps1 build first." }
New-Item -ItemType Directory -Force $out | Out-Null

$files = @()
foreach ($p in $args.flash_files.PSObject.Properties) {
    $src = Join-Path $build $p.Value
    $name = Split-Path $p.Value -Leaf
    Copy-Item $src (Join-Path $out $name) -Force
    $files += [ordered]@{ offset = $p.Name; file = $name }
}
$files = $files | Sort-Object { [Convert]::ToInt32($_.offset, 16) }

# Build fingerprint: first 8 bytes of the ELF SHA-256 in the app image header.
$fs = [IO.File]::OpenRead((Join-Path $build "tiltatron.bin"))
try { $fs.Seek(0x20 + 144, 'Begin') | Out-Null; $buf = New-Object byte[] 8; [void]$fs.Read($buf, 0, 8) } finally { $fs.Close() }
$sha = ($buf | ForEach-Object { $_.ToString("x2") }) -join ""
$version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'PROJECT_VER\s+"([^"]+)"').Matches[0].Groups[1].Value

$manifest = [ordered]@{
    name = "Tilt-a-tron"
    version = $version
    sha = $sha
    chip = "esp32s3"
    flash = [ordered]@{ mode = $args.flash_settings.flash_mode; freq = $args.flash_settings.flash_freq; size = $args.flash_settings.flash_size }
    files = $files
}
# Written without a byte-order mark, which Set-Content -Encoding utf8 would add and which
# trips up anything that reads JSON strictly.
[IO.File]::WriteAllText((Join-Path $out "manifest.json"), ($manifest | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
# The browser installer (site/) gets its own copy plus an ESP Web Tools manifest.
$site = Join-Path $root "site"
$siteFw = Join-Path $site "firmware"
New-Item -ItemType Directory -Force $siteFw | Out-Null
$parts = @()
foreach ($f in $files) {
    Copy-Item (Join-Path $out $f.file) (Join-Path $siteFw $f.file) -Force
    $parts += [ordered]@{ path = "firmware/$($f.file)"; offset = [Convert]::ToInt32($f.offset, 16) }
}
$web = [ordered]@{
    name = "Tilt-a-tron"
    version = $version
    build = $sha
    new_install_prompt_erase = $true
    builds = @([ordered]@{ chipFamily = "ESP32-S3"; parts = $parts })
}
$json = $web | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $site "manifest.json"), $json, (New-Object Text.UTF8Encoding($false)))

Write-Host "firmware/ updated: $version build $sha"
Get-ChildItem $out | ForEach-Object { Write-Host ("  {0,-22} {1,9} bytes" -f $_.Name, $_.Length) }
