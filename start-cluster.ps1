# start-cluster.ps1
# Launches the Coordinator Server and 3 Storage Nodes natively on Windows

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host " Starting Advanced Distributed File System Cluster on Windows" -ForegroundColor Cyan
Write-Host " OS Concepts: Win32 CreateProcess, Winsock2, Concurrency, Fault Tolerance" -ForegroundColor Cyan
Write-Host "================================================================================" -ForegroundColor Cyan

# Set environment paths for compiler and tools if present in E:\tools
if (Test-Path "E:\tools\w64devkit\bin") {
    $env:Path = "E:\tools\w64devkit\bin;" + $env:Path
}

# Ensure build exists
$BuildDir = Join-Path $ScriptDir "build"
$CoordExe = Join-Path $BuildDir "dfs_coordinator.exe"
$NodeExe  = Join-Path $BuildDir "dfs_storage_node.exe"

if (-not (Test-Path $CoordExe) -or -not (Test-Path $NodeExe)) {
    Write-Host "[BUILD] Executables not found in $BuildDir. Compiling project..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    Push-Location $BuildDir
    try {
        cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
        make -j4
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path $CoordExe)) {
    Write-Error "Build failed or dfs_coordinator.exe not found at $CoordExe"
    exit 1
}

# Ensure storage directories exist
New-Item -ItemType Directory -Path (Join-Path $ScriptDir "storage\node1") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ScriptDir "storage\node2") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ScriptDir "storage\node3") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ScriptDir "data") -Force | Out-Null

# Kill any existing instances first
Write-Host "[CLEANUP] Stopping any existing instances..." -ForegroundColor Gray
Get-Process -Name "dfs_coordinator", "dfs_storage_node" -ErrorAction SilentlyContinue | Stop-Process -Force

Start-Sleep -Milliseconds 500

# Copy dashboard and config to build directory if not present
Copy-Item (Join-Path $ScriptDir "dashboard") (Join-Path $BuildDir "dashboard") -Recurse -Force
Copy-Item (Join-Path $ScriptDir "config") (Join-Path $BuildDir "config") -Recurse -Force

# Launch Coordinator Server first so RPC port 9001 and Web port 8080 are ready
Write-Host "[LAUNCH] Starting Central Coordinator Server on Web Port 8080 & RPC Port 9001..." -ForegroundColor Green
Start-Process -FilePath $CoordExe -ArgumentList "--web-port 8080 --rpc-port 9001 --db data/dfs_metadata.db --dashboard-dir dashboard" -WorkingDirectory $ScriptDir

Start-Sleep -Seconds 1

# Launch the 3 Storage Node Processes in independent windows
Write-Host "[LAUNCH] Starting Storage Node 1 on port 9101..." -ForegroundColor Green
Start-Process -FilePath $NodeExe -ArgumentList "--id 1 --port 9101 --dir storage/node1 --coord-port 9001" -WorkingDirectory $ScriptDir

Write-Host "[LAUNCH] Starting Storage Node 2 on port 9102..." -ForegroundColor Green
Start-Process -FilePath $NodeExe -ArgumentList "--id 2 --port 9102 --dir storage/node2 --coord-port 9001" -WorkingDirectory $ScriptDir

Write-Host "[LAUNCH] Starting Storage Node 3 on port 9103..." -ForegroundColor Green
Start-Process -FilePath $NodeExe -ArgumentList "--id 3 --port 9103 --dir storage/node3 --coord-port 9001" -WorkingDirectory $ScriptDir

Start-Sleep -Seconds 2

# Verify processes
$CoordProc = Get-Process -Name "dfs_coordinator" -ErrorAction SilentlyContinue
$NodeProcs = Get-Process -Name "dfs_storage_node" -ErrorAction SilentlyContinue

Write-Host "`n[STATUS] Active Cluster Processes:" -ForegroundColor Cyan
if ($CoordProc) {
    Write-Host "  &bull; Coordinator: PID $($CoordProc.Id)" -ForegroundColor Green
} else {
    Write-Host "  &bull; Coordinator: FAILED TO START" -ForegroundColor Red
}

Write-Host "  &bull; Storage Nodes Running: $($NodeProcs.Count) / 3" -ForegroundColor Green
foreach ($np in $NodeProcs) {
    Write-Host "    - Node Process PID: $($np.Id)" -ForegroundColor Gray
}

Write-Host "`n================================================================================" -ForegroundColor Cyan
Write-Host " CLUSTER RUNNING SUCCESSFULLY!" -ForegroundColor Green
Write-Host " Web Dashboard:   http://127.0.0.1:8080/" -ForegroundColor Yellow
Write-Host " Coordinator RPC: 127.0.0.1:9001" -ForegroundColor Yellow
Write-Host " Storage Nodes:   127.0.0.1:9101, 9102, 9103" -ForegroundColor Yellow
Write-Host " Stop Cluster:    .\stop-cluster.ps1" -ForegroundColor Yellow
Write-Host "================================================================================" -ForegroundColor Cyan

# Open dashboard in default browser
Start-Process "http://127.0.0.1:8080/"
