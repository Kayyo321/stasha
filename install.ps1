# Stasha installer — Windows
# Usage: irm https://raw.githubusercontent.com/Kayyo321/stasha/main/install.ps1 | iex

$ErrorActionPreference = 'Stop'

$Repo        = "Kayyo321/stasha"
$InstallDir  = if ($env:STASHA_INSTALL_DIR) { $env:STASHA_INSTALL_DIR } `
               else { Join-Path $env:LOCALAPPDATA "stasha" }

# ── detect architecture ────────────────────────────────────────
$Arch = $env:PROCESSOR_ARCHITECTURE
switch ($Arch) {
    "AMD64" { $ArchName = "x86_64" }
    "ARM64" {
        Write-Error "Windows ARM64 builds are not yet available. Please check https://github.com/$Repo/releases for updates."
        exit 1
    }
    default {
        Write-Error "Unsupported architecture: $Arch"
        exit 1
    }
}

$Archive = "stasha-windows-$ArchName.zip"

# ── find latest release ────────────────────────────────────────
Write-Host "Fetching latest Stasha release..."

try {
    $Release = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest"
    $Tag     = $Release.tag_name
} catch {
    Write-Error "Failed to fetch release info from GitHub: $_"
    exit 1
}

if (-not $Tag) {
    Write-Error "Could not determine latest release tag."
    exit 1
}

$Url = "https://github.com/$Repo/releases/download/$Tag/$Archive"
Write-Host "Installing Stasha $Tag (windows/$ArchName)..."

# ── download and extract ───────────────────────────────────────
$TmpDir  = Join-Path $env:TEMP "stasha-install-$(Get-Random)"
$ZipPath = Join-Path $TmpDir $Archive

New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

try {
    Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing
} catch {
    Write-Error "Download failed: $_"
    Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
    exit 1
}

if (Test-Path $InstallDir) {
    Remove-Item -Recurse -Force $InstallDir
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Expand-Archive -Path $ZipPath -DestinationPath $TmpDir\extracted -Force

# The archive contains bin/ and lib/ at its root — move them into InstallDir
$Extracted = Get-ChildItem "$TmpDir\extracted" | Select-Object -First 1
if ($Extracted -and (Test-Path $Extracted.FullName)) {
    Copy-Item -Recurse -Force "$($Extracted.FullName)\*" $InstallDir
} else {
    Copy-Item -Recurse -Force "$TmpDir\extracted\*" $InstallDir
}

Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue

# ── update PATH ────────────────────────────────────────────────
$BinDir     = Join-Path $InstallDir "bin"
$CurrentPath = [Environment]::GetEnvironmentVariable("PATH", "User")

if ($CurrentPath -notlike "*$BinDir*") {
    $NewPath = "$BinDir;$CurrentPath"
    [Environment]::SetEnvironmentVariable("PATH", $NewPath, "User")
    Write-Host "  added $BinDir to user PATH"
} else {
    Write-Host "  $BinDir already in PATH"
}

# ── done ───────────────────────────────────────────────────────
Write-Host ""
Write-Host "Stasha $Tag installed to $InstallDir"
Write-Host ""
Write-Host "Open a new terminal window for the PATH change to take effect."
Write-Host ""
Write-Host "Verify installation:"
Write-Host "  stasha --version"
