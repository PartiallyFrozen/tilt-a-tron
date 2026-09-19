# Builds and runs the host tests with the MSVC build tools, for machines without gcc.
# Same tests as run.sh, which is what CI uses.
#
#   .\tests\run.ps1
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot

$cl = Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe" `
    -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $cl) { Write-Host "No MSVC build tools found; use tests/run.sh with gcc instead." ; exit 1 }

$msvc = Split-Path (Split-Path (Split-Path (Split-Path $cl.FullName)))
$sdkInc = (Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Include\*" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkLib = (Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Lib\*" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$env:INCLUDE = "$msvc\include;$sdkInc\ucrt;$sdkInc\shared;$sdkInc\um"
$env:LIB = "$msvc\lib\x64;$sdkLib\ucrt\x64;$sdkLib\um\x64"

New-Item -ItemType Directory -Force out | Out-Null
$inc = @("/I.", "/Istubs", "/I..\components\link\include", "/I..\components\storage\include",
         "/I..\components\net\include", "/I..\components\engine\include",
         "/I..\components\tat_api\include")

$fail = 0
Write-Host "building..."
& $cl.FullName /nologo /W3 /wd4100 /wd4996 @inc /Fo:out\ /Fe:out\test_link.exe test_link.c 2>&1 |
    Where-Object { $_ -notmatch '^test_link\.c$' } | Write-Host
if ($LASTEXITCODE -ne 0) { exit 1 }
& $cl.FullName /nologo /W3 /wd4100 /wd4996 /EHsc @inc /Fo:out\ /Fe:out\test_store.exe test_store.cpp 2>&1 |
    Where-Object { $_ -notmatch '^test_store\.cpp$' } | Write-Host
if ($LASTEXITCODE -ne 0) { exit 1 }

Write-Host ""
& .\out\test_link.exe;  if ($LASTEXITCODE -ne 0) { $fail = 1 }
& .\out\test_store.exe; if ($LASTEXITCODE -ne 0) { $fail = 1 }
Write-Host ""
python test_protocol.py; if ($LASTEXITCODE -ne 0) { $fail = 1 }

Write-Host ""
if ($fail -eq 0) { Write-Host "all tests passed" -ForegroundColor Green } else { Write-Host "TESTS FAILED" -ForegroundColor Red }
exit $fail
