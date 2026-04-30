#!/bin/bash
# Package the pre-built airies-upsd + airies-ups binaries into a .deb
# suitable for `apt install ./airies-ups_*.deb` on Debian trixie or any
# derivative shipping the same runtime libs (libssl3t64, libcurl4t64,
# libmicrohttpd12t64, libmodbus5, libsqlite3-0).
#
# Usage:  scripts/build-deb.sh <amd64|arm64> [BINDIR] [OUTDIR]
#         BINDIR defaults to "build", OUTDIR to "dist".
#
# Versioning: <semver>~<YYYYMMDD>.<HHMM>.<short_sha>
#   The tilde sorts BELOW any future real <semver> release per
#   `dpkg --compare-versions`, so snapshots auto-supersede each other
#   and a future tagged release supersedes all snapshots.

set -euo pipefail

ARCH="${1:?usage: $0 <amd64|arm64> [BINDIR] [OUTDIR]}"
BINDIR="${2:-build}"
OUTDIR="${3:-dist}"

case "$ARCH" in
    amd64|arm64) ;;
    *) echo "build-deb: unsupported arch '$ARCH'" >&2; exit 1 ;;
esac

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG_DIR="$REPO_ROOT/packaging/deb"

SEMVER="$(tr -d '[:space:]' < "$REPO_ROOT/release_version")"
TS="$(date -u '+%Y%m%d.%H%M')"
# Prefer the env var so CI builds get a real SHA without needing git
# installed in the package:deb:* image. Fall back to git for local
# builds, then to "unknown" as a last-resort sentinel that still sorts
# correctly through dpkg --compare-versions.
SHA="${CI_COMMIT_SHORT_SHA:-$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
VERSION="${SEMVER}~${TS}.${SHA}"
PKG="airies-ups_${VERSION}_${ARCH}"
STAGE="$REPO_ROOT/$BINDIR/deb-$ARCH"

rm -rf "$STAGE"
# /lib and /etc paths land binaries/units in the wrong place under modern
# Debian's UsrMerge: /lib is a symlink to /usr/lib, and vendor-shipped udev
# rules belong in /usr/lib/udev/rules.d (not /etc/udev/rules.d, which is
# reserved for sysadmin overrides — putting our rule there would also
# require declaring it as a conffile).
install -d \
    "$STAGE/DEBIAN" \
    "$STAGE/usr/bin" \
    "$STAGE/usr/lib/systemd/system" \
    "$STAGE/usr/lib/udev/rules.d" \
    "$STAGE/usr/share/polkit-1/rules.d" \
    "$STAGE/usr/share/doc/airies-ups"

install -m 0755 "$REPO_ROOT/$BINDIR/airies-upsd" "$STAGE/usr/bin/airies-upsd"
install -m 0755 "$REPO_ROOT/$BINDIR/airies-ups"  "$STAGE/usr/bin/airies-ups"
# Strip release binaries — debug symbols would inflate the .deb ~5x and
# Debian policy expects production binaries to be stripped. We use the
# host toolchain's strip; for cross builds the caller is responsible for
# pre-stripping or pointing STRIP at the cross strip.
STRIP="${STRIP:-strip}"
"$STRIP" --strip-unneeded "$STAGE/usr/bin/airies-upsd" "$STAGE/usr/bin/airies-ups"

install -m 0644 "$PKG_DIR/airies-ups.service" \
                "$STAGE/usr/lib/systemd/system/airies-ups.service"
install -m 0644 "$REPO_ROOT/99-airies-ups-ftdi.rules" \
                "$STAGE/usr/lib/udev/rules.d/99-airies-ups-ftdi.rules"
install -m 0644 "$PKG_DIR/polkit/50-airies-ups.rules" \
                "$STAGE/usr/share/polkit-1/rules.d/50-airies-ups.rules"
install -m 0644 "$PKG_DIR/copyright" \
                "$STAGE/usr/share/doc/airies-ups/copyright"

# airies-ups is a Debian-native package (no separate upstream tarball),
# so the changelog file is `changelog.gz`, not `changelog.Debian.gz`.
DATE_RFC="$(date -u '+%a, %d %b %Y %H:%M:%S +0000')"
cat > "$STAGE/usr/share/doc/airies-ups/changelog" <<EOF
airies-ups (${VERSION}) trixie; urgency=medium

  * Snapshot build from commit ${SHA}.

 -- XxI0NICxX <vifair22@gmail.com>  ${DATE_RFC}
EOF
gzip -9n "$STAGE/usr/share/doc/airies-ups/changelog"

install -m 0755 "$PKG_DIR/postinst" "$STAGE/DEBIAN/postinst"
install -m 0755 "$PKG_DIR/prerm"    "$STAGE/DEBIAN/prerm"
install -m 0755 "$PKG_DIR/postrm"   "$STAGE/DEBIAN/postrm"

sed -e "s/@VERSION@/${VERSION}/g" -e "s/@ARCH@/${ARCH}/g" \
    "$PKG_DIR/control.in" > "$STAGE/DEBIAN/control"

mkdir -p "$REPO_ROOT/$OUTDIR"
dpkg-deb --root-owner-group --build "$STAGE" \
         "$REPO_ROOT/$OUTDIR/${PKG}.deb"

echo
echo "Built: $REPO_ROOT/$OUTDIR/${PKG}.deb"
ls -lh "$REPO_ROOT/$OUTDIR/${PKG}.deb"
