#!/usr/bin/env bash
# Release script for cchd
set -euo pipefail

# Get build targets from environment or arguments
TARGETS="${BUILD_TARGETS:-$1}"
PLATFORM_NAME="${PLATFORM_NAME:-$(uname -s | tr '[:upper:]' '[:lower:]')}"

# Get the current tag
TAG=$(git describe --exact-match --tags HEAD 2>/dev/null || echo "dev")
VERSION=${TAG#v}  # Remove 'v' prefix

echo "Building release for version $VERSION on $PLATFORM_NAME..."
echo "Targets: $TARGETS"

# Create dist directory
mkdir -p dist

# Build each target
for target in $TARGETS; do
    echo "Building $target..."
    
    # Always build native (no cross-compilation with system libraries)
    echo "Building native..."
    zig build -Doptimize=ReleaseSafe
    
    # Determine output name based on target
    case $target in
        x86_64-linux-gnu)
            cp ./zig-out/bin/cchd dist/cchd-linux-amd64
            ;;
        aarch64-linux-gnu)
            cp ./zig-out/bin/cchd dist/cchd-linux-arm64
            ;;
        x86_64-macos)
            cp ./zig-out/bin/cchd dist/cchd-darwin-amd64
            ;;
        aarch64-macos)
            cp ./zig-out/bin/cchd dist/cchd-darwin-arm64
            ;;
        *)
            echo "Unknown target: $target"
            exit 1
            ;;
    esac
done

echo "Build complete!"