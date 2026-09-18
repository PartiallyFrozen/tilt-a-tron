# Runs idf.py with the ESP-IDF 5.5.5 environment loaded, from the project root.
#   .\idf.ps1 build
#   .\idf.ps1 -p COM4 flash monitor
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1 | Out-Null
Set-Location $PSScriptRoot
idf.py @args
exit $LASTEXITCODE
