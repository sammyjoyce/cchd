#!/usr/bin/env bash
set -euo pipefail

APP=cchd
REPO=sammyjoyce/cchd

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
ORANGE='\033[38;2;255;140;0m'
NC='\033[0m' # No Color

requested_version=${VERSION:-}

# Detect OS and architecture
os=$(uname -s | tr '[:upper:]' '[:lower:]')
if [[ "$os" == "darwin" ]]; then
    os="macos"
fi

arch=$(uname -m)
if [[ "$arch" == "arm64" ]]; then
    arch="aarch64"
elif [[ "$arch" == "x86_64" ]]; then
    arch="x86_64"
fi

# Construct filename based on the release naming convention
filename="$APP-$arch-apple-darwin.tar.gz"

# Validate platform support
case "$filename" in
    *"-apple-darwin"*)
        [[ "$arch" == "x86_64" || "$arch" == "aarch64" ]] || {
            echo -e "${RED}Unsupported architecture: $arch${NC}"
            exit 1
        }
    ;;
    *)
        echo -e "${RED}This installer is for macOS only${NC}"
        exit 1
    ;;
esac

INSTALL_DIR=$HOME/.cchd/bin
mkdir -p "$INSTALL_DIR"

# Determine download URL
if [ -z "$requested_version" ]; then
    url="https://github.com/$REPO/releases/latest/download/$filename"
    specific_version=$(curl -s https://api.github.com/repos/$REPO/releases/latest | grep '"tag_name"' | cut -d '"' -f 4 | sed 's/^v//')
    
    if [[ $? -ne 0 || -z "$specific_version" ]]; then
        echo -e "${RED}Failed to fetch version information${NC}"
        exit 1
    fi
else
    url="https://github.com/$REPO/releases/download/v${requested_version}/$filename"
    specific_version=$requested_version
fi

print_message() {
    local level=$1
    local message=$2
    local color=""
    
    case $level in
        info) color="${GREEN}" ;;
        warning) color="${YELLOW}" ;;
        error) color="${RED}" ;;
    esac
    
    echo -e "${color}${message}${NC}"
}

check_version() {
    if command -v cchd >/dev/null 2>&1; then
        cchd_path=$(which cchd)
        
        # Get installed version
        installed_version=$(cchd --version 2>/dev/null | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+' | head -1 || echo "unknown")
        
        if [[ "$installed_version" == "$specific_version" ]]; then
            print_message info "Version ${YELLOW}$specific_version${GREEN} already installed"
            exit 0
        elif [[ "$installed_version" != "unknown" ]]; then
            print_message info "Installed version: ${YELLOW}$installed_version${GREEN}, upgrading to ${YELLOW}$specific_version"
        fi
    fi
}

verify_signature() {
    local archive_path=$1
    local temp_dir=$2
    local download_url=$3
    
    if ! command -v minisign >/dev/null 2>&1; then
        print_message warning "Skipping signature verification (minisign not installed)"
        print_message info "To enable verification: ${YELLOW}brew install minisign"
        return 0
    fi
    
    # Download signature and public key
    local sig_url="${download_url}.minisig"
    local pubkey_url="https://github.com/$REPO/releases/latest/download/minisign.pub"
    
    print_message info "Downloading signature..."
    if curl -sL -o "${archive_path}.minisig" "$sig_url" 2>/dev/null; then
        if curl -sL -o "${temp_dir}/minisign.pub" "$pubkey_url" 2>/dev/null; then
            print_message info "Verifying signature..."
            if minisign -V -p "${temp_dir}/minisign.pub" -m "$archive_path" >/dev/null 2>&1; then
                print_message info "✓ Signature verified"
            else
                print_message warning "Signature verification failed"
            fi
        else
            print_message warning "Could not download public key"
        fi
    else
        print_message warning "No signature available for this release"
    fi
}

download_and_install() {
    print_message info "Downloading ${ORANGE}$APP ${GREEN}version ${YELLOW}$specific_version${GREEN}..."
    
    # Create temporary directory
    temp_dir=$(mktemp -d)
    trap 'rm -rf "$temp_dir"' EXIT
    
    cd "$temp_dir"
    
    # Download with progress bar
    curl -# -L -o "$filename" "$url"
    
    # Verify signature if possible
    verify_signature "$filename" "$temp_dir" "$url"
    
    # Extract and install
    tar -xzf "$filename"
    mv cchd "$INSTALL_DIR/"
    chmod +x "$INSTALL_DIR/cchd"
    
    cd - >/dev/null
}

setup_claude_config() {
    local claude_dir="$HOME/.claude"
    local config_file="$claude_dir/settings.json"
    
    if [[ ! -d "$claude_dir" ]]; then
        print_message info "Creating Claude configuration directory..."
        mkdir -p "$claude_dir"
    fi
    
    if [[ ! -f "$config_file" ]]; then
        print_message info "Creating example hook configuration..."
        cat > "$config_file" << 'EOF'
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
        print_message info "Created $config_file"
    else
        print_message info "Claude configuration already exists at $config_file"
    fi
}

add_to_path() {
    local config_file=$1
    local command=$2
    
    if grep -Fxq "$command" "$config_file" 2>/dev/null; then
        print_message info "Command already exists in $config_file"
    elif [[ -w $config_file ]]; then
        echo -e "\n# cchd - Claude Hooks Dispatcher" >> "$config_file"
        echo "$command" >> "$config_file"
        print_message info "Added ${ORANGE}$APP ${GREEN}to \$PATH in $config_file"
    else
        print_message warning "Manually add the directory to $config_file:"
        print_message info "  $command"
    fi
}

# Main execution
print_message info "Installing Claude Hooks Dispatcher (cchd) for macOS"
echo

check_version
download_and_install
setup_claude_config

# Configure PATH
XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-$HOME/.config}

current_shell=$(basename "$SHELL")
case $current_shell in
    fish)
        config_files="$HOME/.config/fish/config.fish"
    ;;
    zsh)
        config_files="$HOME/.zshrc $HOME/.zshenv $XDG_CONFIG_HOME/zsh/.zshrc $XDG_CONFIG_HOME/zsh/.zshenv"
    ;;
    bash)
        config_files="$HOME/.bashrc $HOME/.bash_profile $HOME/.profile $XDG_CONFIG_HOME/bash/.bashrc $XDG_CONFIG_HOME/bash/.bash_profile"
    ;;
    *)
        config_files="$HOME/.bashrc $HOME/.bash_profile"
    ;;
esac

config_file=""
for file in $config_files; do
    if [[ -f $file ]]; then
        config_file=$file
        break
    fi
done

if [[ -z $config_file ]]; then
    print_message error "No shell config file found. Checked: $config_files"
    exit 1
fi

if [[ ":$PATH:" != *":$INSTALL_DIR:"* ]]; then
    case $current_shell in
        fish)
            add_to_path "$config_file" "fish_add_path $INSTALL_DIR"
        ;;
        *)
            add_to_path "$config_file" "export PATH=$INSTALL_DIR:\$PATH"
        ;;
    esac
fi

# GitHub Actions support
if [ -n "${GITHUB_ACTIONS-}" ] && [ "${GITHUB_ACTIONS}" == "true" ]; then
    echo "$INSTALL_DIR" >> $GITHUB_PATH
    print_message info "Added $INSTALL_DIR to \$GITHUB_PATH"
fi

echo
print_message info "✨ Installation complete!"
echo
print_message info "Next steps:"
print_message info "1. Reload your shell: ${YELLOW}source $config_file"
print_message info "2. Verify installation: ${YELLOW}cchd --version"
print_message info "3. Start your hook server (see examples at https://github.com/$REPO/tree/main/examples)"
print_message info "4. Claude Desktop will automatically trigger hooks"
echo
print_message info "For more information: https://github.com/$REPO"