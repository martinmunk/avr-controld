#!/bin/bash
set -e

# Get current version from debian/changelog
CURRENT_VERSION=$(dpkg-parsechangelog -S Version)

# Bump patch version
IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT_VERSION"
NEW_VERSION="$MAJOR.$MINOR.$((PATCH + 1))"

echo "Current version: $CURRENT_VERSION"
echo "New version: $NEW_VERSION"

# Get commit message from last commit
LAST_COMMIT_MSG=$(git log -1 --pretty=%B)

# Update debian/changelog
DEBFULLNAME="Martin Munk Hansen" DEBEMAIL="mhansen@roku.com" \
  dch -v "$NEW_VERSION" -D stable "$LAST_COMMIT_MSG"

# Commit the changelog update
git add debian/changelog
git commit -m "Bump version to $NEW_VERSION"

# Create release tag
git tag "release/$NEW_VERSION"

# Push commit and tag
git push origin main
git push origin "release/$NEW_VERSION"

echo ""
echo "========================================="
echo "✅ Released version $NEW_VERSION"
echo "   Tag: release/$NEW_VERSION"
echo "========================================="
