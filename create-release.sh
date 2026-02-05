#!/bin/bash
#
# Copyright (c) 2025 Martin Munk Hansen
#
# Script to create a new release
#
# Usage:
#   ./create-release.sh              # Interactive mode (default)
#   ./create-release.sh 1.2.3        # Interactive with version specified
#   ./create-release.sh -y 1.2.3     # Non-interactive mode

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Parse command line arguments
NON_INTERACTIVE=false
NEW_VERSION=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -y|--yes|--non-interactive)
            NON_INTERACTIVE=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [-y|--non-interactive] [version]"
            echo ""
            echo "Options:"
            echo "  -y, --yes, --non-interactive    Skip all prompts"
            echo "  -h, --help                      Show this help"
            echo ""
            echo "Examples:"
            echo "  $0                              Interactive mode"
            echo "  $0 1.2.3                        Interactive with version"
            echo "  $0 -y 1.2.3                     Non-interactive"
            exit 0
            ;;
        *)
            NEW_VERSION="$1"
            shift
            ;;
    esac
done

# Get current version from changelog
CURRENT_VERSION=$(dpkg-parsechangelog -S Version)
PACKAGE_NAME=$(dpkg-parsechangelog -S Source)
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)

echo -e "${BLUE}================================${NC}"
echo -e "${BLUE}Create Release for ${PACKAGE_NAME}${NC}"
echo -e "${BLUE}================================${NC}"
echo ""
echo -e "Current version: ${GREEN}${CURRENT_VERSION}${NC}"
echo -e "Branch: ${CURRENT_BRANCH}"
echo ""

# Check for uncommitted changes
if ! git diff-index --quiet HEAD -- 2>/dev/null; then
    echo -e "${YELLOW}WARNING: You have uncommitted changes:${NC}"
    git status --short
    echo ""
    if [ "$NON_INTERACTIVE" = false ]; then
        read -p "Continue anyway? (y/N) " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            echo "Aborted."
            exit 1
        fi
        echo ""
    fi
fi

# Prompt for version if not provided
if [ -z "$NEW_VERSION" ]; then
    while true; do
        read -p "Enter new version (X.Y.Z format): " NEW_VERSION

        # Validate version format
        if [[ ! $NEW_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
            echo -e "${RED}Error: Version must be in format X.Y.Z (e.g., 1.2.3)${NC}"
            continue
        fi

        # Check if version is newer
        if dpkg --compare-versions "$NEW_VERSION" le "$CURRENT_VERSION"; then
            echo -e "${RED}Error: New version ($NEW_VERSION) must be greater than current ($CURRENT_VERSION)${NC}"
            continue
        fi

        break
    done
    echo ""
else
    # Validate provided version
    if [[ ! $NEW_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo -e "${RED}Error: Version must be in format X.Y.Z (e.g., 1.2.3)${NC}"
        exit 1
    fi

    if dpkg --compare-versions "$NEW_VERSION" le "$CURRENT_VERSION"; then
        echo -e "${RED}Error: New version ($NEW_VERSION) must be greater than current ($CURRENT_VERSION)${NC}"
        exit 1
    fi
fi

# Show what will happen
TAG_NAME="${PACKAGE_NAME}/release/${NEW_VERSION}"
echo -e "${YELLOW}Release Summary:${NC}"
echo -e "  Package:    ${PACKAGE_NAME}"
echo -e "  Version:    ${GREEN}${CURRENT_VERSION}${NC} → ${GREEN}${NEW_VERSION}${NC}"
echo -e "  Tag:        ${TAG_NAME}"
echo -e "  Branch:     ${CURRENT_BRANCH}"
echo ""

# Generate changelog from git history
echo -e "${GREEN}Generating changelog from git history...${NC}"
if [ "$NON_INTERACTIVE" = true ]; then
    gbp dch --new-version="$NEW_VERSION" --release --commit --spawn-editor=never
else
    echo -e "${YELLOW}Your editor will open to review and edit the changelog.${NC}"
    echo -e "${YELLOW}Save and close the editor when done.${NC}"
    echo ""
    read -p "Press Enter to open editor..."
    gbp dch --new-version="$NEW_VERSION" --release --commit --spawn-editor=always
fi

echo -e "${GREEN}✓${NC} Changelog updated"
echo ""

# Create git tag
echo -e "${GREEN}Creating git tag: ${TAG_NAME}${NC}"
git tag -a "$TAG_NAME" -m "Release ${PACKAGE_NAME} ${NEW_VERSION}"
echo -e "${GREEN}✓${NC} Tag created"
echo ""

# Build the package (optional)
if [ "$NON_INTERACTIVE" = false ]; then
    read -p "Build package locally to verify? (Y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Nn]$ ]]; then
        echo -e "${GREEN}Building package...${NC}"
        SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
        "$SCRIPT_DIR/buildpackage.sh"

        echo ""
        echo -e "${GREEN}Build complete!${NC}"
        ls -lh build-deb/*.deb 2>/dev/null || echo "No .deb files found"
        echo ""
    fi
fi

# Final confirmation before push
echo -e "${YELLOW}Ready to push release:${NC}"
echo -e "  • Changelog commit"
echo -e "  • Tag: ${TAG_NAME}"
echo -e "  • This will trigger Gitea Actions to build and publish"
echo ""

if [ "$NON_INTERACTIVE" = false ]; then
    read -p "Push to remote? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo ""
        echo -e "${YELLOW}Release prepared locally but not pushed.${NC}"
        echo "To push later, run:"
        echo "  git push origin ${CURRENT_BRANCH}"
        echo "  git push origin ${TAG_NAME}"
        exit 0
    fi
fi

# Push to Gitea
echo ""
echo -e "${GREEN}Pushing to Gitea...${NC}"
git push origin "$CURRENT_BRANCH"
git push origin "$TAG_NAME"

echo ""
echo -e "${GREEN}================================${NC}"
echo -e "${GREEN}Release ${NEW_VERSION} created!${NC}"
echo -e "${GREEN}================================${NC}"
echo ""
echo "Gitea Actions will now:"
echo "  • Build packages for AMD64 and ARM64"
echo "  • Publish to Gitea Debian repository"
echo ""
echo "Monitor the build at:"
REPO_URL=$(git config --get remote.origin.url | sed 's/\.git$//' | sed 's|^git@\(.*\):|http://\1/|')
echo -e "  ${BLUE}${REPO_URL}/actions${NC}"
echo ""
