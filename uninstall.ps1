# Stasha uninstaller — Windows

$ErrorActionPreference = 'Stop'

$InstallDir = if ($env:STASHA_INSTALL_DIR) { $env:STASHA_INSTALL_DIR } `
              else { Join-Path $env:LOCALAPPDATA "stasha" }
$BinDir     = Join-Path $InstallDir "bin"

if (-not (Test-Path $InstallDir)) {
    Write-Host "Stasha does not appear to be installed at $InstallDir"
    exit 0
}

Write-Host "Removing $InstallDir..."
Remove-Item -Recurse -Force $InstallDir

# Strip from user PATH
$CurrentPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($CurrentPath -like "*$BinDir*") {
    $NewPath = ($CurrentPath.Split(';') | Where-Object { $_ -ne $BinDir }) -join ';'
    [Environment]::SetEnvironmentVariable("PATH", $NewPath, "User")
    Write-Host "  removed $BinDir from user PATH"
}

Write-Host ""
Write-Host "Stasha has been uninstalled."
Write-Host "Open a new terminal window for the PATH change to take effect."
