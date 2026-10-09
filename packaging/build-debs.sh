#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the Debian packages dc30-dkms and dc30-capture.
#
#   packaging/build-debs.sh <version> [<output dir>]
#
# <version> must match PACKAGE_VERSION in src/dkms.conf and the project
# version in app/CMakeLists.txt. Needs cmake, the build dependencies of
# dc30-capture, dpkg-dev (dpkg-shlibdeps) and nfpm. Used by the release
# workflow; runs the same way locally.
set -euo pipefail

VERSION=${1:?usage: $0 <version> [<output dir>]}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${2:-$ROOT/dist}")
WORK=$ROOT/build-pkg

die() { echo "build-debs: $*" >&2; exit 1; }

# ---- versions must agree ----
dkms_ver=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$ROOT/src/dkms.conf")
app_ver=$(sed -n 's/^project(dc30-capture VERSION \([^ ]*\).*/\1/p' "$ROOT/app/CMakeLists.txt")
[ "$dkms_ver" = "$VERSION" ] || die "src/dkms.conf has $dkms_ver, not $VERSION"
[ "$app_ver" = "$VERSION" ] || die "app/CMakeLists.txt has $app_ver, not $VERSION"
for f in dc30_core.c dc30_vpx3220.c; do
	mod_ver=$(sed -n 's/^MODULE_VERSION("\(.*\)");/\1/p' "$ROOT/src/$f")
	[ "$mod_ver" = "$VERSION" ] || die "src/$f has MODULE_VERSION $mod_ver, not $VERSION"
done

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
STAGE=$WORK/stage

# ---- dc30-capture and dc30-metadump ----
cmake -S "$ROOT/app" -B "$WORK/app" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$WORK/app" -j"$(nproc)"
ctest --test-dir "$WORK/app" --output-on-failure
DESTDIR=$STAGE cmake --install "$WORK/app"
strip "$STAGE/usr/bin/dc30-capture"

${CC:-cc} -O2 -Wall -Wextra -o "$STAGE/usr/bin/dc30-metadump" \
	"$ROOT/tools/dc30-metadump.c" -lm
strip "$STAGE/usr/bin/dc30-metadump"

# Shared library dependencies, as dpkg-shlibdeps finds them.
mkdir -p "$WORK/shlibs/debian"
touch "$WORK/shlibs/debian/control"
deps=$(cd "$WORK/shlibs" && dpkg-shlibdeps -O \
	-e"$STAGE/usr/bin/dc30-capture" -e"$STAGE/usr/bin/dc30-metadump" \
	2>/dev/null | sed -n 's/^shlibs:Depends=//p')
[ -n "$deps" ] || die "dpkg-shlibdeps found no dependencies"
echo "dc30-capture depends: $deps"

# ---- driver sources for DKMS ----
SRC=$WORK/dkms-src
mkdir -p "$SRC"
for f in "$ROOT"/src/*.c "$ROOT"/src/*.h; do
	case $f in *.mod.c) continue ;; esac	# left over from a local build
	cp "$f" "$SRC/"
done
cp "$ROOT/src/Makefile" "$ROOT/src/dkms.conf" "$SRC/"
chmod 0755 "$SRC"
chmod 0644 "$SRC"/*

SCRIPTS=$WORK/scripts
mkdir -p "$SCRIPTS"
for s in dkms-postinst.sh dkms-prerm.sh; do
	sed "s/@VERSION@/$VERSION/g" "$ROOT/packaging/$s" > "$SCRIPTS/$s"
	chmod 0755 "$SCRIPTS/$s"
done

# ---- nfpm configs ----
sed -e "s|@VERSION@|$VERSION|g" -e "s|@SRC@|$SRC|g" -e "s|@SCRIPTS@|$SCRIPTS|g" \
	"$ROOT/packaging/nfpm-dkms.yaml" > "$WORK/nfpm-dkms.yaml"

dep_lines=$(echo "$deps" | tr ',' '\n' | sed 's/^ *//; s/^/  - /')
awk -v deps="$dep_lines" '{ print } /^depends:$/ { print deps }' \
	"$ROOT/packaging/nfpm-capture.yaml" |
	sed -e "s|@VERSION@|$VERSION|g" -e "s|@STAGE@|$STAGE|g" \
	> "$WORK/nfpm-capture.yaml"

nfpm package -f "$WORK/nfpm-dkms.yaml" -p deb -t "$OUT/"
nfpm package -f "$WORK/nfpm-capture.yaml" -p deb -t "$OUT/"

rm -rf "$WORK"
ls -l "$OUT"/*.deb
