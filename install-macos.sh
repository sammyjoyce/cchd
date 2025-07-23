#!/bin/bash
# Claude Hooks Dispatcher (cchd) - macOS Installation Script
# Copyright (c) 2025 Sam Joyce. MIT License.

# This script installs the cchd binary on macOS by downloading the latest release from GitHub,
# verifying it if possible, installing to /usr/local/bin, and setting up basic configuration.

set -euo pipefail

readonly COLOR_RED='\033[0;31m'
readonly COLOR_GREEN='\033[0;32m'
readonly COLOR_YELLOW='\033[1;33m'
readonly COLOR_NONE='\033[0m'

readonly INSTALL_DIRECTORY="/usr/local/bin"
readonly CLAUDE_CONFIGURATION_DIRECTORY="$HOME/.claude"
readonly RELEASE_API_URL="https://api.github.com/repos/sammyjoyce/cchd/releases/latest"
readonly PLATFORM="macos"
readonly DOWNLOAD_FILE_EXTENSION=".tar.gz"

# We limit supported architectures to x86_64 and arm64 to match common macOS hardware.
# This function asserts the architecture is supported before proceeding.
detect_architecture() {
    local machine_arch
    machine_arch=$(uname -m)

    if [ "$machine_arch" = "x86_64" ]; then
        echo "x86_64"
        return 0
    fi

    if [ "$machine_arch" = "arm64" ]; then
        echo "aarch64"
        return 0
    fi

    echo -e "${COLOR_RED}❌ Unsupported architecture: $machine_arch${COLOR_NONE}" >&2
    exit 1
}

# We assert Homebrew is installed first since it's used to install curl if needed.
# This ensures the script fails early if the environment isn't suitable.
check_prerequisites() {
    if ! command -v brew > /dev/null 2>&1; then
        echo -e "${COLOR_RED}❌ Homebrew not found${COLOR_NONE}"
        echo "Please install Homebrew first: https://brew.sh"
        exit 1
    fi

    if ! command -v curl > /dev/null 2>&1; then
        echo -e "${COLOR_YELLOW}⚠️  Installing curl...${COLOR_NONE}"
        brew install curl
    fi
}

# We use curl with --fail to assert successful HTTP response and limit output to silent mode.
fetch_release_information() {
    curl --silent --fail "$RELEASE_API_URL"
}

# We use grep to find the exact matching URL pattern for our platform to avoid incorrect downloads.
# Assert that a URL is found, or display available assets for debugging.
extract_download_url() {
    local release_information="$1"
    local platform_arch="$2"

    local download_url
    download_url=$(echo "$release_information" | grep -o "https://[^\"]*cchd-${platform_arch}-${PLATFORM}-none${DOWNLOAD_FILE_EXTENSION}" | head -n 1)

    if [ -z "$download_url" ]; then
        echo -e "${COLOR_RED}❌ No release found for ${platform_arch}-${PLATFORM}${COLOR_NONE}" >&2
        echo "Available releases:" >&2
        echo "$release_information" | grep -o "cchd-[^\"]*${DOWNLOAD_FILE_EXTENSION}" | sort -u >&2
        exit 1
    fi

    echo "$download_url"
}

# Extract the version tag from the release information.
# This provides feedback on what version is being installed.
extract_version() {
    local release_information="$1"
    echo "$release_information" | grep -o '"tag_name": "[^"]*"' | cut -d '"' -f 4
}

# We use curl with -L to follow redirects and assert success.
download_binary_archive() {
    local download_url="$1"
    local temp_directory="$2"

    local archive_path="${temp_directory}/cchd${DOWNLOAD_FILE_EXTENSION}"
    curl --location --fail "$download_url" --output "$archive_path"
    echo "$archive_path"
}

# Attempt signature verification if minisign is available.
# This is optional but improves safety by verifying integrity when possible.
# We download the signature and public key only if needed.
verify_signature_if_possible() {
    local release_information="$1"
    local archive_path="$2"
    local temp_directory="$3"

    if ! command -v minisign > /dev/null 2>&1; then
        echo -e "${COLOR_YELLOW}⚠️  Skipping signature verification (minisign not available)${COLOR_NONE}"
        echo "To enable verification: brew install minisign"
        return 0
    fi

    local minisig_url="${archive_path}.minisig"
    minisig_url=$(echo "$archive_path" | sed 's|$|.minisig|')  # Avoid variable issues
    local pubkey_url
    pubkey_url=$(echo "$release_information" | grep -o "https://[^\"]*minisign\.pub" | head -n 1)

    if [ -z "$minisig_url" ] || [ -z "$pubkey_url" ]; then
        echo -e "${COLOR_YELLOW}⚠️  No signature available for verification${COLOR_NONE}"
        return 0
    fi

    echo "Downloading signature..."
    curl --location --fail "$minisig_url" --output "${archive_path}.minisig"

    echo "Downloading public key..."
    curl --location --fail "$pubkey_url" --output "${temp_directory}/minisign.pub"

    echo "Verifying signature..."
    if minisign -V -p "${temp_directory}/minisign.pub" -m "$archive_path"; then
        echo -e "${COLOR_GREEN}✓${COLOR_NONE} Signature verified"
    else
        echo -e "${COLOR_RED}❌ Signature verification failed!${COLOR_NONE}" >&2
        exit 1
    fi
}

# Extract the binary from the archive.
# We assert the extraction succeeds and limit to the temp directory.
extract_binary() {
    local archive_path="$1"
    local temp_directory="$2"

    echo "Extracting cchd..."
    tar -xzf "$archive_path" -C "$temp_directory"
}

# Install the binary to the installation directory.
# We check write permissions to decide if sudo is needed, improving UX on different setups.
install_binary() {
    local temp_directory="$1"

    echo
    echo "📦 Installing cchd to $INSTALL_DIRECTORY..."
    local binary_path="${temp_directory}/cchd"

    if [ -w "$INSTALL_DIRECTORY" ]; then
        cp "$binary_path" "$INSTALL_DIRECTORY/"
    else
        sudo cp "$binary_path" "$INSTALL_DIRECTORY/"
    fi

    chmod +x "${INSTALL_DIRECTORY}/cchd"
}

# Verify the installation by checking if cchd is in PATH and running --version.
# This asserts the binary works post-install.
verify_installation() {
    if command -v cchd > /dev/null 2>&1; then
        echo -e "${COLOR_GREEN}✓${COLOR_NONE} cchd installed successfully"
        cchd --version || true
    else
        echo -e "${COLOR_YELLOW}⚠️  cchd installed but not in PATH${COLOR_NONE}"
        echo "Add $INSTALL_DIRECTORY to your PATH if needed"
    fi
}

# Set up the Claude configuration directory and example settings file.
# We create only if not exists to avoid overwriting user configurations.
setup_configuration() {
    echo
    echo "📁 Setting up Claude configuration..."
    mkdir -p "$CLAUDE_CONFIGURATION_DIRECTORY"

    local hook_config_path="${CLAUDE_CONFIGURATION_DIRECTORY}/settings.json"
    if [ ! -f "$hook_config_path" ]; then
        echo "Creating example hook configuration..."
        cat > "$hook_config_path" << 'EOF'
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
EOF
        echo -e "${COLOR_GREEN}✓${COLOR_NONE} Created $hook_config_path"
    else
        echo -e "${COLOR_YELLOW}ℹ️${COLOR_NONE} Hook configuration already exists at $hook_config_path"
    fi
}

# Test the installed cchd with sample input.
# This checks basic functionality, expecting possible failure if no server is running.
test_cchd() {
    echo
    echo "🧪 Testing cchd..."
    local test_input='{"session_id":"test123","hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"ls"}}'
    echo "$test_input" | cchd 2>&1 || echo -e "${COLOR_YELLOW}ℹ️${COLOR_NONE} Test failed (this is expected if no server is running)"
}

# Main installation flow
# We create a temp directory early and set a trap to clean it on exit for safety.
echo "🚀 Installing Claude Hooks Dispatcher (cchd) for macOS"
echo "=================================================="
echo

check_prerequisites

local platform_arch
platform_arch=$(detect_architecture)
echo "Detected architecture: ${platform_arch}-${PLATFORM}"

local temp_directory
temp_directory=$(mktemp -d)
trap 'rm -rf "$temp_directory"' EXIT

echo
echo "📥 Downloading cchd..."
echo "Fetching latest release information..."

local release_information
release_information=$(fetch_release_information)

local download_url
download_url=$(extract_download_url "$release_information" "$platform_arch")

local version
version=$(extract_version "$release_information")
echo "Latest version: $version"

echo "Downloading from: $download_url"
local archive_path
archive_path=$(download_binary_archive "$download_url" "$temp_directory")

verify_signature_if_possible "$release_information" "$archive_path" "$temp_directory"

extract_binary "$archive_path" "$temp_directory"

install_binary "$temp_directory"

verify_installation

setup_configuration

test_cchd

echo
echo "✨ Installation complete!"
echo
echo "Next steps:"
echo "1. Start your hook server (see examples at https://github.com/sammyjoyce/cchd/tree/main/examples)"
echo "2. Configure hooks in ${CLAUDE_CONFIGURATION_DIRECTORY}/settings.json"
echo "3. Use Claude Desktop - hooks will be automatically triggered"
echo
echo "For more information: https://github.com/sammyjoyce/cchd"
