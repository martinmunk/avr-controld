#!/bin/bash
#
# Copyright (c) 2025 Martin Munk Hansen
#
# Script to create a new release
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Get current version from changelog
CURRENT_VERSION=$(dpkg-parsechangelog -S Version)
PACKAGE_NAME=$(dpkg-parsechangelog -S Source)

echo -e "${GREEN}Current version: ${CURRENT_VERSION}${NC}"
echo

# Get version from command line argument
NEW_VERSION="$1"

if [ -z "$NEW_VERSION" ]; then
    echo -e "${RED}Error: Version number is required${NC}"
    echo "Usage: $0 <version>"
    echo "Example: $0 1.2.3"
    exit 1
fi

# Validate version format
if ! [[ "$NEW_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo -e "${RED}Error: Version must be in format X.Y.Z (e.g., 2.1.2)${NC}"
    exit 1
fi

# Get current branch name
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)

# Generate changelog from git history
echo
echo -e "${GREEN}Generating changelog from git history...${NC}"
gbp dch --new-version="$NEW_VERSION" --release --commit --spawn-editor=never

# Create git tag
TAG_NAME="${PACKAGE_NAME}/release/${NEW_VERSION}"
echo
echo -e "${GREEN}Creating git tag: ${TAG_NAME}${NC}"
git tag -a "$TAG_NAME" -m "Release ${PACKAGE_NAME} ${NEW_VERSION}"

# Build the package
echo
echo -e "${GREEN}Building package...${NC}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$SCRIPT_DIR/buildpackage.sh"

# Show build results
echo
echo -e "${GREEN}Build complete!${NC}"
ls -lh build-deb/*.deb

echo
echo -e "${GREEN}=== Release Summary ===${NC}"
echo -e "Package: ${PACKAGE_NAME}"
echo -e "Version: ${NEW_VERSION}"
echo -e "Tag: ${TAG_NAME}"
echo -e "Branch: ${CURRENT_BRANCH}"
echo

# Push to Gitea
echo -e "${GREEN}Pushing to Gitea...${NC}"
git push origin "$CURRENT_BRANCH"
git push origin "$TAG_NAME"
echo -e "${GREEN}Pushed successfully!${NC}"
echo -e "Gitea Actions will automatically build and publish to package registry"
