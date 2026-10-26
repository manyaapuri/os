# run-tests.ps1
# Builds and runs the comprehensive OS automated test suite

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host " Running Automated Comprehensive OS Test Suite (dfs_tests.exe)" -ForegroundColor Cyan
Write-Host "================================================================================" -ForegroundColor Cyan

# Check for GCC or tools in PATH, prioritizing w64devkit if present
if (Test-Path "E:\tools\w64devkit\bin") {
    $env:Path = "E:\tools\w64devkit\bin;" + $env:Path
}

$BuildDir = Join-Path $ScriptDir "build"
$TestExe  = Join-Path $BuildDir "dfs_tests.exe"

# Clean any existing stale test database to ensure repeatable, deterministic test runs
$TestDb = Join-Path $ScriptDir "data\test_metadata.db*"
if (Test-Path $TestDb) {
    Remove-Item -Force $TestDb -ErrorAction SilentlyContinue
}

if (-not (Test-Path $TestExe)) {
    Write-Host "[BUILD] dfs_tests.exe not found. Building..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    Push-Location $BuildDir
    try {
        cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
        make dfs_tests -j4
    } finally {
        Pop-Location
    }
}

Push-Location $ScriptDir
try {
    & $TestExe
} finally {
    Pop-Location
}
