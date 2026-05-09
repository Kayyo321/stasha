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

$Url    = "https://github.com/$Repo/releases/download/$Tag/$Archive"
$ShaUrl = "$Url.sha256"
Write-Host "Installing Stasha $Tag (windows/$ArchName)..."

# ── download ───────────────────────────────────────────────────
$TmpDir  = Join-Path $env:TEMP "stasha-install-$(Get-Random)"
$ZipPath = Join-Path $TmpDir $Archive

New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

try {
    Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing
} catch {
    if ($_.Exception.Response.StatusCode.value__ -eq 404) {
        Write-Error "No prebuilt archive found for windows/$ArchName (tag $Tag). Check https://github.com/$Repo/releases"
    } else {
        Write-Error "Download failed: $_"
    }
    Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
    exit 1
}

# ── SHA-256 verify ─────────────────────────────────────────────
try {
    $ShaContent  = (Invoke-WebRequest -Uri $ShaUrl -UseBasicParsing).Content.Trim()
    $ExpectedHash = ($ShaContent -split '\s+')[0].ToUpper()
    $ActualHash   = (Get-FileHash $ZipPath -Algorithm SHA256).Hash.ToUpper()
    if ($ExpectedHash -ne $ActualHash) {
        Write-Error "SHA-256 mismatch — archive may be corrupt or tampered.`n  expected: $ExpectedHash`n  got:      $ActualHash"
        Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
        exit 1
    }
    Write-Host "  SHA-256 verified."
} catch {
    Write-Warning "Could not verify SHA-256 (sidecar unavailable): $_"
}

# ── extract ────────────────────────────────────────────────────
if (Test-Path $InstallDir) {
    Remove-Item -Recurse -Force $InstallDir
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Expand-Archive -Path $ZipPath -DestinationPath "$TmpDir\extracted" -Force

# Archive root is flat (bin/ only) — copy contents directly into InstallDir.
Copy-Item -Recurse -Force "$TmpDir\extracted\*" $InstallDir

Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue

# ── smoke test ─────────────────────────────────────────────────
$StashaExe = Join-Path $InstallDir "bin\stasha.exe"
try {
    $VersionOut = & $StashaExe --version 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Smoke test failed: stasha --version exited with code $LASTEXITCODE"
        exit 1
    }
    Write-Host "  smoke test OK: $VersionOut"
} catch {
    Write-Error "Smoke test failed — binary did not run: $_"
    exit 1
}

# ── update PATH ────────────────────────────────────────────────
$BinDir      = Join-Path $InstallDir "bin"
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
