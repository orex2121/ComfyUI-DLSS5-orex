$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host "[probe] ERROR: $Message" -ForegroundColor Red
    exit 1
}

# Same MSVC/SDK discovery as build_bridge.ps1, trimmed to the probe.
$pf86 = [Environment]::GetFolderPath('ProgramFilesX86')
$pf = [Environment]::GetFolderPath('ProgramFiles')
$programRoots = @($pf86, $pf) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path $_) } | Select-Object -Unique

$clCandidates = @()
foreach ($programRoot in $programRoots) {
    $vsRoot = Join-Path $programRoot 'Microsoft Visual Studio'
    if (-not (Test-Path $vsRoot)) { continue }
    $clCandidates += Get-ChildItem -Path $vsRoot -Filter cl.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\VC\\Tools\\MSVC\\[^\\]+\\bin\\Hostx64\\x64\\cl\.exe$' } |
        Select-Object -ExpandProperty FullName
}
$clCandidates = $clCandidates | Select-Object -Unique
if (-not $clCandidates) { Fail 'MSVC x64 compiler cl.exe not found.' }

$cl = $clCandidates | Sort-Object {
    if ($_ -match '\\MSVC\\([^\\]+)\\bin\\Hostx64') { try { [version]$Matches[1] } catch { [version]'0.0' } } else { [version]'0.0' }
} -Descending | Select-Object -First 1

$msvcVersionDir = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $cl)))
$msvcInclude = Join-Path $msvcVersionDir 'include'
$msvcLib = Join-Path $msvcVersionDir 'lib\x64'

$sdkRoot = $null
foreach ($programRoot in $programRoots) {
    $candidateSdk = Join-Path $programRoot 'Windows Kits\10'
    if (Test-Path (Join-Path $candidateSdk 'Include')) { $sdkRoot = $candidateSdk; break }
}
if (-not $sdkRoot) { Fail 'Windows Kits\10 not found.' }

$sdkVersions = Get-ChildItem -Path (Join-Path $sdkRoot 'Include') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } |
    Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending
if (-not $sdkVersions) { Fail 'No usable Windows SDK.' }

$sdkVersion = $sdkVersions[0].Name
$incBase = Join-Path (Join-Path $sdkRoot 'Include') $sdkVersion
$libBase = Join-Path (Join-Path $sdkRoot 'Lib') $sdkVersion

$includeDirs = @($msvcInclude, "$incBase\ucrt", "$incBase\shared", "$incBase\um") | Where-Object { Test-Path $_ }
$libDirs = @($msvcLib, "$libBase\ucrt\x64", "$libBase\um\x64") | Where-Object { Test-Path $_ }

$env:PATH = (Split-Path -Parent $cl) + ";$env:PATH"
$env:INCLUDE = ($includeDirs -join ';')
$env:LIB = ($libDirs -join ';')

$src = Join-Path $PSScriptRoot 'probe_fence_shared.cpp'
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\probe_fence_shared.exe'

$args = @('/nologo', '/std:c++17', '/EHsc', '/MT', '/O2', $src, '/link', "/OUT:$out", 'd3d12.lib', 'dxgi.lib')
foreach ($inc in $includeDirs) { $args += ('/I' + $inc) }

Write-Host "[probe] compiler: $cl"
Write-Host "[probe] SDK:      $sdkVersion"
& $cl @args
if ($LASTEXITCODE -ne 0) { Fail "compile failed with cl.exe exit code $LASTEXITCODE." }
Get-ChildItem -Path (Split-Path -Parent $PSScriptRoot) -Filter '*.obj' -File -ErrorAction SilentlyContinue | Remove-Item -Force
Write-Host "[probe] built: $out" -ForegroundColor Green
