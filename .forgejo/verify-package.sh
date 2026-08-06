#!/bin/bash
# In-job verification for the martinmunk-alsa-control-avr .deb.
# Usage: verify-package.sh <dir-with-deb> <arch>
# Asserts the package actually contains the four ALSA ctl plugins, the
# avr-power tool, and the conf.d snippets, and that the binary is the right
# architecture. Runs in CI because this Forgejo instance does not serve Actions
# job logs after the run, so a wrong artifact must fail the build here.
set -eu

DIR="${1:-build-deb}"
ARCH="${2:-}"

DEB=$(ls "$DIR"/*.deb 2>/dev/null | grep -v dbgsym | head -1 || true)
[ -n "$DEB" ] || { echo "FAIL: no .deb produced in $DIR"; exit 1; }

echo "=== control ==="
dpkg-deb -f "$DEB" Package Version Architecture Depends Recommends
echo "=== contents ==="
dpkg-deb -c "$DEB" | awk '{print $6}' | grep -v '/$'

fail() { echo "FAIL: $1"; exit 1; }

# The four ALSA control plugins must be present (path is multiarch-dependent, so
# match on the filename under an alsa-lib directory).
for f in denon nad opalum cec; do
  dpkg-deb -c "$DEB" | grep -qE "alsa-lib/libasound_module_ctl_${f}\.so$" \
    || fail "missing plugin libasound_module_ctl_${f}.so"
done

dpkg-deb -c "$DEB" | grep -qE 'usr/bin/avr-power$' || fail "avr-power missing"
dpkg-deb -c "$DEB" | grep -qE 'etc/alsa/conf.d/50-denon-avr\.conf$' || fail "conf.d snippets missing"

# Architecture sanity on the compiled tool.
rm -rf /tmp/verify-x && mkdir -p /tmp/verify-x && dpkg-deb -x "$DEB" /tmp/verify-x
echo "=== avr-power ==="
file /tmp/verify-x/usr/bin/avr-power
case "$ARCH" in
  arm64) file /tmp/verify-x/usr/bin/avr-power | grep -q 'ARM aarch64' || fail "avr-power is not aarch64" ;;
  amd64) file /tmp/verify-x/usr/bin/avr-power | grep -q 'x86-64'      || fail "avr-power is not x86-64" ;;
esac

echo "OK: package verified"
