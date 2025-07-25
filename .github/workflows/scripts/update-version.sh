#!/bin/bash
# Script to update version in all relevant files

set -e

if [ $# -ne 1 ]; then
    echo "Usage: $0 <version>"
    echo "Example: $0 0.2.0"
    exit 1
fi

VERSION=$1

# Remove 'v' prefix if present (GoReleaser passes version with 'v')
VERSION=${VERSION#v}

# Validate version format
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: Version must be in format X.Y.Z"
    exit 1
fi

# Update version in build.zig.zon
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    sed -i '' "s/\.version = \"[0-9]*\.[0-9]*\.[0-9]*\"/.version = \"$VERSION\"/" build.zig.zon
    
    # Update version in build.zig
    sed -i '' "s/const version_str = \"[0-9]*\.[0-9]*\.[0-9]*\"/const version_str = \"$VERSION\"/" build.zig
    
    # Update version in opencli.json
    sed -i '' "s/\"version\": \"[0-9]*\.[0-9]*\.[0-9]*\"/\"version\": \"$VERSION\"/" opencli.json
else
    # Linux
    sed -i "s/\.version = \"[0-9]*\.[0-9]*\.[0-9]*\"/.version = \"$VERSION\"/" build.zig.zon
    
    # Update version in build.zig
    sed -i "s/const version_str = \"[0-9]*\.[0-9]*\.[0-9]*\"/const version_str = \"$VERSION\"/" build.zig
    
    # Update version in opencli.json
    sed -i "s/\"version\": \"[0-9]*\.[0-9]*\.[0-9]*\"/\"version\": \"$VERSION\"/" opencli.json
fi

echo "Updated version to $VERSION in:"
echo "  - build.zig.zon"
echo "  - build.zig"
echo "  - opencli.json"