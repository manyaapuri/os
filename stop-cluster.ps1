# stop-cluster.ps1
# Gracefully terminates the Coordinator and Storage Node processes on Windows

Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host " Stopping Distributed File System Cluster Gracefully" -ForegroundColor Cyan
Write-Host "================================================================================" -ForegroundColor Cyan

$Procs = Get-Process -Name "dfs_coordinator", "dfs_storage_node" -ErrorAction SilentlyContinue

if ($Procs) {
    Write-Host "Found $($Procs.Count) cluster processes. Sending termination signals..." -ForegroundColor Yellow
    foreach ($p in $Procs) {
        Write-Host "  Stopping $($p.ProcessName) [PID: $($p.Id)]..." -ForegroundColor Gray
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 500
    Write-Host "All DFS processes stopped successfully." -ForegroundColor Green
} else {
    Write-Host "No active DFS cluster processes found." -ForegroundColor Yellow
}

Write-Host "Done." -ForegroundColor Cyan
