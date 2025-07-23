# Claude Hooks Dispatcher (cchd) - Windows Installation Script
# Copyright (c) 2025 Sam Joyce. MIT License.

# This script installs the cchd binary on Windows by downloading the latest release from GitHub,
# verifying it if possible, installing to LocalAppData\Programs\cchd, and setting up basic configuration.

param(
    [string]$InstallPath = "$env:LOCALAPPDATA\Programs\cchd",
    [string]$Version = "latest"
)

$ErrorActionPreference = "Stop"

# We define color codes for consistent output formatting across platforms.
# These match the Unix color codes used in the bash scripts.
$script:ColorRed = "Red"
$script:ColorGreen = "Green"
$script:ColorYellow = "Yellow"
$script:ColorNone = "White"

# Helper functions for colored output
function Write-ColorOutput {
    param(
        [string]$Message,
        [string]$Color = $script:ColorNone
    )
    Write-Host $Message -ForegroundColor $Color
}

# Constants
$script:InstallDirectory = $InstallPath
$script:ClaudeConfigurationDirectory = "$env:USERPROFILE\.claude"
$script:ReleaseApiUrl = "https://api.github.com/repos/sammyjoyce/cchd/releases/latest"
$script:Platform = "windows"
$script:DownloadFileExtension = ".tar.gz"

# We limit supported architectures to x86_64 and x86 to match common Windows hardware.
# This function asserts the architecture is supported before proceeding.
function Get-Architecture {
    if ([Environment]::Is64BitOperatingSystem) {
        return "x86_64"
    } else {
        return "x86"
    }
}

# We assert prerequisites are met before proceeding.
# Windows 10+ includes tar by default, which we need for extraction.
function Test-Prerequisites {
    # Check for tar (available in Windows 10+)
    if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
        Write-ColorOutput "❌ tar command not found. Windows 10 or later is required." $script:ColorRed
        exit 1
    }
    
    # Check if running as administrator (optional but noted)
    $currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $isAdmin = $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    
    if (-not $isAdmin) {
        Write-ColorOutput "ℹ️  Running without administrator privileges. Installation will be user-local." $script:ColorYellow
    }
}

# We use Invoke-RestMethod to fetch release information from GitHub API.
function Get-ReleaseInformation {
    try {
        $response = Invoke-RestMethod -Uri $script:ReleaseApiUrl -Headers @{"Accept"="application/vnd.github.v3+json"}
        return $response
    } catch {
        Write-ColorOutput "❌ Failed to fetch release information: $_" $script:ColorRed
        exit 1
    }
}

# We find the download URL for our platform and architecture.
# Assert that a URL is found, or display available assets for debugging.
function Get-DownloadUrl {
    param(
        [object]$ReleaseInfo,
        [string]$Architecture
    )
    
    $asset = $ReleaseInfo.assets | Where-Object { 
        $_.name -like "*${Architecture}-${script:Platform}-none${script:DownloadFileExtension}" 
    } | Select-Object -First 1
    
    if (-not $asset) {
        Write-ColorOutput "❌ No release found for ${Architecture}-${script:Platform}" $script:ColorRed
        Write-ColorOutput "Available releases:" $script:ColorNone
        $ReleaseInfo.assets | ForEach-Object { Write-ColorOutput "  $($_.name)" $script:ColorNone }
        exit 1
    }
    
    return $asset.browser_download_url
}

# Extract the version tag from the release information.
# This provides feedback on what version is being installed.
function Get-Version {
    param([object]$ReleaseInfo)
    return $ReleaseInfo.tag_name
}

# We download the binary archive to a temporary location.
function Get-BinaryArchive {
    param(
        [string]$DownloadUrl,
        [string]$TempDirectory
    )
    
    $archivePath = Join-Path $TempDirectory "cchd$script:DownloadFileExtension"
    
    try {
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $archivePath
        return $archivePath
    } catch {
        Write-ColorOutput "❌ Failed to download binary: $_" $script:ColorRed
        exit 1
    }
}

# Attempt signature verification if minisign is available.
# This is optional but improves safety by verifying integrity when possible.
function Test-SignatureIfPossible {
    param(
        [object]$ReleaseInfo,
        [string]$ArchivePath,
        [string]$TempDirectory
    )
    
    if (-not (Get-Command minisign -ErrorAction SilentlyContinue)) {
        Write-ColorOutput "⚠️  Skipping signature verification (minisign not available)" $script:ColorYellow
        Write-ColorOutput "To enable verification: install minisign from https://jedisct1.github.io/minisign/" $script:ColorNone
        return
    }
    
    # Find signature and public key URLs
    $minisigAsset = $ReleaseInfo.assets | Where-Object { $_.name -eq "$(Split-Path -Leaf $ArchivePath).minisig" } | Select-Object -First 1
    $pubkeyAsset = $ReleaseInfo.assets | Where-Object { $_.name -like "*minisign.pub" } | Select-Object -First 1
    
    if (-not $minisigAsset -or -not $pubkeyAsset) {
        Write-ColorOutput "⚠️  No signature available for verification" $script:ColorYellow
        return
    }
    
    Write-ColorOutput "Downloading signature..." $script:ColorNone
    $sigPath = "$ArchivePath.minisig"
    Invoke-WebRequest -Uri $minisigAsset.browser_download_url -OutFile $sigPath
    
    Write-ColorOutput "Downloading public key..." $script:ColorNone
    $pubkeyPath = Join-Path $TempDirectory "minisign.pub"
    Invoke-WebRequest -Uri $pubkeyAsset.browser_download_url -OutFile $pubkeyPath
    
    Write-ColorOutput "Verifying signature..." $script:ColorNone
    $result = & minisign -V -p $pubkeyPath -m $ArchivePath 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-ColorOutput "✓ Signature verified" $script:ColorGreen
    } else {
        Write-ColorOutput "❌ Signature verification failed!" $script:ColorRed
        exit 1
    }
}

# Extract the binary from the archive.
# We assert the extraction succeeds and limit to the temp directory.
function Expand-Binary {
    param(
        [string]$ArchivePath,
        [string]$TempDirectory
    )
    
    Write-ColorOutput "Extracting cchd..." $script:ColorNone
    & tar -xzf $ArchivePath -C $TempDirectory
    if ($LASTEXITCODE -ne 0) {
        Write-ColorOutput "❌ Failed to extract archive" $script:ColorRed
        exit 1
    }
}

# Install the binary to the installation directory.
function Install-Binary {
    param([string]$TempDirectory)
    
    Write-ColorOutput "" $script:ColorNone
    Write-ColorOutput "📦 Installing cchd to $script:InstallDirectory..." $script:ColorNone
    
    # Create installation directory if it doesn't exist
    if (-not (Test-Path $script:InstallDirectory)) {
        New-Item -ItemType Directory -Force -Path $script:InstallDirectory | Out-Null
    }
    
    $binaryPath = Join-Path $TempDirectory "cchd.exe"
    $destinationPath = Join-Path $script:InstallDirectory "cchd.exe"
    
    Copy-Item -Path $binaryPath -Destination $destinationPath -Force
}

# Add cchd to PATH if not already present.
# We modify the user PATH to avoid requiring administrator privileges.
function Add-ToPath {
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($userPath -notlike "*$script:InstallDirectory*") {
        Write-ColorOutput "Adding cchd to user PATH..." $script:ColorNone
        [Environment]::SetEnvironmentVariable("Path", "$userPath;$script:InstallDirectory", "User")
        $env:Path = "$env:Path;$script:InstallDirectory"
        Write-ColorOutput "✓ Added to PATH. You may need to restart your terminal." $script:ColorGreen
    } else {
        Write-ColorOutput "ℹ️  cchd is already in PATH" $script:ColorYellow
    }
}

# Verify the installation by checking if cchd is in PATH and running --version.
# This asserts the binary works post-install.
function Test-Installation {
    $cchd = Join-Path $script:InstallDirectory "cchd.exe"
    
    try {
        $output = & $cchd --version 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-ColorOutput "✓ cchd installed successfully" $script:ColorGreen
            Write-ColorOutput $output $script:ColorNone
        } else {
            Write-ColorOutput "⚠️  cchd installed but not working properly" $script:ColorYellow
        }
    } catch {
        Write-ColorOutput "⚠️  cchd installed but not in PATH" $script:ColorYellow
        Write-ColorOutput "Add $script:InstallDirectory to your PATH if needed" $script:ColorNone
    }
}

# Set up the Claude configuration directory and example settings file.
# We create only if not exists to avoid overwriting user configurations.
function Set-Configuration {
    Write-ColorOutput "" $script:ColorNone
    Write-ColorOutput "📁 Setting up Claude configuration..." $script:ColorNone
    
    if (-not (Test-Path $script:ClaudeConfigurationDirectory)) {
        New-Item -ItemType Directory -Force -Path $script:ClaudeConfigurationDirectory | Out-Null
    }
    
    $hookConfigPath = Join-Path $script:ClaudeConfigurationDirectory "settings.json"
    if (-not (Test-Path $hookConfigPath)) {
        Write-ColorOutput "Creating example hook configuration..." $script:ColorNone
        $defaultSettings = @'
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "cchd --server http://localhost:8080/hook"
          }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "cchd --server http://localhost:8080/hook"
          }
        ]
      }
    ],
    "UserPromptSubmit": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "cchd --server http://localhost:8080/hook"
          }
        ]
      }
    ]
  }
}
'@
        $defaultSettings | Out-File -FilePath $hookConfigPath -Encoding UTF8
        Write-ColorOutput "✓ Created $hookConfigPath" $script:ColorGreen
    } else {
        Write-ColorOutput "ℹ️  Hook configuration already exists at $hookConfigPath" $script:ColorYellow
    }
}

# Test the installed cchd with sample input.
# This checks basic functionality, expecting possible failure if no server is running.
function Test-Cchd {
    Write-ColorOutput "" $script:ColorNone
    Write-ColorOutput "🧪 Testing cchd..." $script:ColorNone
    
    $testInput = '{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"dir"}}'
    $cchd = Join-Path $script:InstallDirectory "cchd.exe"
    
    try {
        $result = $testInput | & $cchd 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-ColorOutput "✓ Test completed successfully" $script:ColorGreen
        } else {
            Write-ColorOutput "ℹ️  Test failed (this is expected if no server is running)" $script:ColorYellow
        }
    } catch {
        Write-ColorOutput "ℹ️  Test failed (this is expected if no server is running)" $script:ColorYellow
    }
}

# Main installation flow
Write-ColorOutput "🚀 Installing Claude Hooks Dispatcher (cchd) for Windows" $script:ColorNone
Write-ColorOutput "==================================================" $script:ColorNone
Write-ColorOutput "" $script:ColorNone

Test-Prerequisites

$platformArch = Get-Architecture
Write-ColorOutput "Detected architecture: ${platformArch}-${script:Platform}" $script:ColorNone

# Create temp directory
$tempDirectory = New-TemporaryFile | ForEach-Object { Remove-Item $_; New-Item -ItemType Directory -Path $_ }

try {
    Write-ColorOutput "" $script:ColorNone
    Write-ColorOutput "📥 Downloading cchd..." $script:ColorNone
    Write-ColorOutput "Fetching latest release information..." $script:ColorNone
    
    $releaseInfo = Get-ReleaseInformation
    $downloadUrl = Get-DownloadUrl -ReleaseInfo $releaseInfo -Architecture $platformArch
    $version = Get-Version -ReleaseInfo $releaseInfo
    
    Write-ColorOutput "Latest version: $version" $script:ColorNone
    Write-ColorOutput "Downloading from: $downloadUrl" $script:ColorNone
    
    $archivePath = Get-BinaryArchive -DownloadUrl $downloadUrl -TempDirectory $tempDirectory.FullName
    
    Test-SignatureIfPossible -ReleaseInfo $releaseInfo -ArchivePath $archivePath -TempDirectory $tempDirectory.FullName
    
    Expand-Binary -ArchivePath $archivePath -TempDirectory $tempDirectory.FullName
    
    Install-Binary -TempDirectory $tempDirectory.FullName
    
    Add-ToPath
    
    Test-Installation
    
    Set-Configuration
    
    Test-Cchd
    
    Write-ColorOutput "" $script:ColorNone
    Write-ColorOutput "✨ Installation complete!" $script:ColorNone
    Write-ColorOutput "" $script:ColorNone
    Write-ColorOutput "Next steps:" $script:ColorNone
    Write-ColorOutput "1. Start your hook server (see examples at https://github.com/sammyjoyce/cchd/tree/main/examples)" $script:ColorNone
    Write-ColorOutput "2. Configure hooks in ${script:ClaudeConfigurationDirectory}\settings.json" $script:ColorNone
    Write-ColorOutput "3. Use Claude Desktop - hooks will be automatically triggered" $script:ColorNone
    Write-ColorOutput "" $script:ColorNone
    Write-ColorOutput "For more information: https://github.com/sammyjoyce/cchd" $script:ColorNone
    
} finally {
    # Clean up temp directory
    if (Test-Path $tempDirectory.FullName) {
        Remove-Item -Path $tempDirectory.FullName -Recurse -Force
    }
}