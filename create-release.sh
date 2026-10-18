#!/bin/bash
#
# Cut a release of ONE of the two Debian source packages in this repo:
#
#   alsa-control-avr   (root debian/)    C++ ALSA plugins, avr-cec-relay, avr-power
#   avr-controld       (python/debian/)  Python broker daemon (venv package)
#
# They version independently. Each has its own tag namespace, and CI
# (.forgejo/workflows/publish.yml) builds and publishes only the package named
# in the tag:
#
#   martinmunk-alsa-control-avr/release/X.Y.Z
#   martinmunk-avr-controld/release/X.Y.Z
#
# Usage:
#   ./create-release.sh <package> <version>          # interactive (default)
#   ./create-release.sh -y <package> <version>       # non-interactive
#   ./create-release.sh -n <package> <version>       # dry run: print, change nothing
#
# The changelog entry is generated from the commits touching that package's
# paths since its previous release tag, then opened in $EDITOR for review
# (unless -y). Identity comes from git (user.name / user.email).

set -e

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

NON_INTERACTIVE=false
DRY_RUN=false
PKG=""
NEW_VERSION=""

usage() {
    sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -y|--yes|--non-interactive) NON_INTERACTIVE=true; shift ;;
        -n|--dry-run) DRY_RUN=true; shift ;;
        -h|--help) usage 0 ;;
        -*) echo -e "${RED}Unknown option: $1${NC}"; usage 1 ;;
        *) if [ -z "$PKG" ]; then PKG="$1"; elif [ -z "$NEW_VERSION" ]; then NEW_VERSION="$1";
           else echo -e "${RED}Too many arguments${NC}"; usage 1; fi; shift ;;
    esac
done

case "$PKG" in
    alsa-control-avr|martinmunk-alsa-control-avr)
        PKG_DIR="."; PATHS="src include CMakeLists.txt debian systemd examples tests buildpackage.sh" ;;
    avr-controld|martinmunk-avr-controld)
        PKG_DIR="python"; PATHS="python" ;;
    "") echo -e "${RED}Missing <package> (alsa-control-avr | avr-controld)${NC}"; usage 1 ;;
    *)  echo -e "${RED}Unknown package: $PKG${NC}"; usage 1 ;;
esac

cd "$(dirname "${BASH_SOURCE[0]}")"
CHANGELOG="$PKG_DIR/debian/changelog"
SOURCE=$(dpkg-parsechangelog -l "$CHANGELOG" -S Source)
CURRENT_VERSION=$(dpkg-parsechangelog -l "$CHANGELOG" -S Version)
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
TAG_PREFIX="$SOURCE/release/"
# The previous release of this package: newest (by tag date) of its own namespace
# and the legacy shared release/X.Y.Z tags used between 0.2.x and 0.5.1. (The
# repo started with per-package tags, so the namespace alone would pick 0.1.1.)
LAST_TAG=$(git for-each-ref --sort=-creatordate --format='%(refname:short)' \
           "refs/tags/${TAG_PREFIX}" "refs/tags/release/" | head -1)

export DEBFULLNAME=$(git config user.name)
export DEBEMAIL=$(git config user.email)
if [ -z "$DEBEMAIL" ]; then
    echo -e "${RED}git user.email is not set; refusing to write a changelog entry${NC}"; exit 1
fi

echo -e "${BLUE}================================${NC}"
echo -e "${BLUE}Create release: ${SOURCE}${NC}"
echo -e "${BLUE}================================${NC}"
echo -e "Current version: ${GREEN}${CURRENT_VERSION}${NC}   last tag: ${LAST_TAG:-none}"
echo -e "Branch:          ${CURRENT_BRANCH}"
echo -e "Identity:        ${DEBFULLNAME} <${DEBEMAIL}>"
echo ""

if ! git diff-index --quiet HEAD -- 2>/dev/null; then
    echo -e "${YELLOW}WARNING: uncommitted changes:${NC}"; git status --short; echo ""
    if [ "$NON_INTERACTIVE" = false ] && [ "$DRY_RUN" = false ]; then
        read -p "Continue anyway? (y/N) " -n 1 -r; echo
        [[ $REPLY =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }
    fi
fi

while true; do
    if [ -z "$NEW_VERSION" ]; then
        [ "$NON_INTERACTIVE" = true ] && { echo -e "${RED}Missing <version>${NC}"; exit 1; }
        read -p "New version (X.Y.Z): " NEW_VERSION
    fi
    if [[ ! $NEW_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo -e "${RED}Version must be X.Y.Z${NC}"; NEW_VERSION=""; [ "$NON_INTERACTIVE" = true ] && exit 1; continue
    fi
    if dpkg --compare-versions "$NEW_VERSION" le "$CURRENT_VERSION"; then
        echo -e "${RED}$NEW_VERSION is not greater than $CURRENT_VERSION${NC}"; NEW_VERSION=""; [ "$NON_INTERACTIVE" = true ] && exit 1; continue
    fi
    break
done
TAG_NAME="${TAG_PREFIX}${NEW_VERSION}"
if git rev-parse -q --verify "refs/tags/$TAG_NAME" >/dev/null; then
    echo -e "${RED}Tag $TAG_NAME already exists${NC}"; exit 1
fi

# Changelog body: one line per commit touching this package since its last tag.
RANGE=""; [ -n "$LAST_TAG" ] && RANGE="${LAST_TAG}..HEAD"
# shellcheck disable=SC2086
mapfile -t ENTRIES < <(git log --no-merges --format='%s' $RANGE -- $PATHS | grep -v -i '^release:\|^Update changelog for' || true)
[ ${#ENTRIES[@]} -eq 0 ] && ENTRIES=("Release $NEW_VERSION")

echo -e "${YELLOW}Release summary:${NC}"
echo -e "  Package:  ${SOURCE}  (${PKG_DIR}/debian)"
echo -e "  Version:  ${GREEN}${CURRENT_VERSION}${NC} -> ${GREEN}${NEW_VERSION}${NC}"
echo -e "  Tag:      ${TAG_NAME}"
echo -e "  Changes since ${LAST_TAG:-the beginning} (${#ENTRIES[@]}):"
for e in "${ENTRIES[@]}"; do echo "    * $e"; done
echo ""
if [ "$DRY_RUN" = true ]; then echo -e "${YELLOW}Dry run: nothing changed.${NC}"; exit 0; fi

echo -e "${GREEN}Writing changelog entry...${NC}"
dch --changelog "$CHANGELOG" --newversion "$NEW_VERSION" --distribution stable --urgency medium \
    --force-distribution "${ENTRIES[0]}"
for e in "${ENTRIES[@]:1}"; do dch --changelog "$CHANGELOG" --append "$e"; done
if [ "$NON_INTERACTIVE" = false ]; then
    echo -e "${YELLOW}Review/edit the entry; save and close to continue.${NC}"
    "${EDITOR:-vi}" "$CHANGELOG"
fi
git add "$CHANGELOG"
git commit -q -m "release: ${SOURCE} ${NEW_VERSION}"
git tag -a "$TAG_NAME" -m "Release ${SOURCE} ${NEW_VERSION}"
echo -e "${GREEN}✓${NC} changelog committed, tag ${TAG_NAME} created"
echo ""

if [ "$NON_INTERACTIVE" = false ]; then
    read -p "Push branch + tag to Forgejo (triggers build + publish)? (y/N) " -n 1 -r; echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${YELLOW}Prepared locally, not pushed. Later:${NC}"
        echo "  git push origin ${CURRENT_BRANCH} && git push origin ${TAG_NAME}"
        exit 0
    fi
fi
git push origin "$CURRENT_BRANCH"
git push origin "$TAG_NAME"
echo ""
echo -e "${GREEN}Release ${SOURCE} ${NEW_VERSION} pushed.${NC} CI builds and publishes it to the Forgejo apt registry."
REPO_URL=$(git config --get remote.origin.url | sed 's/\.git$//' | sed 's|^git@\(.*\):|http://\1/|' | sed 's|^ssh://git@\([^/]*\)/|http://\1:3000/|')
echo -e "  ${BLUE}${REPO_URL}/actions${NC}"
