#!/bin/bash
# Builds the plugin from this repository, the way CI does.
#
#   tools/build.sh [output-dir]          (default: dist/)
#
# Linux only (the Docker image rust:1-bookworm has everything). Needs git, curl,
# tar, python3 and gcc. Steps:
#   1. clones xenon-forge (the open Xbox 360 toolchain) at the pinned commit
#      into .xenon-forge/, or uses XENON_FORGE_DIR;
#   2. runs the desk test of the stick and trigger shaping (hidforge/tests);
#   3. ports the untouched hiddriver360 v0.5 source in hiddriver/ with
#      hidforge/port_v05.py and builds it twice with buck2: the normal build
#      (HIDFORGE_DIAG=0) and the diagnostic build (HIDFORGE_DIAG=1, writes
#      hidforge.log on the USB drive and blinks the ring of light);
#   4. audits both ELFs with tools/audit-elf.sh and fails on any finding;
#   5. writes hidforge.xex, hidforge-diag.xex and SHA1SUMS to the output dir.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "${1:-$REPO/dist}" && cd "${1:-$REPO/dist}" && pwd)
XF=${XENON_FORGE_DIR:-$REPO/.xenon-forge}
XF_URL=https://github.com/landaire/xenon-forge.git
XF_COMMIT=f369053666c0d56176c8fdefd7d87f3dcc168c63   # the commit every console-tested build used

# Init in place rather than clone: CI restores the cached .xenon-forge/.tools
# (DotSlash and Buck2) before this runs, so the directory may already exist.
if [ ! -d "$XF/.git" ]; then
	mkdir -p "$XF"
	git -C "$XF" init --quiet
	git -C "$XF" remote add origin "$XF_URL"
fi
if [ "$(git -C "$XF" rev-parse -q --verify HEAD || true)" != "$XF_COMMIT" ]; then
	git -C "$XF" fetch --quiet --depth 1 origin "$XF_COMMIT"
	git -C "$XF" checkout --quiet --force FETCH_HEAD
fi

echo "== desk test"
gcc -std=c99 -Wall -Wextra -Werror -O2 -o "$OUT/.sticks_test" "$REPO/hidforge/tests/sticks_test.c"
"$OUT/.sticks_test"
rm -f "$OUT/.sticks_test"

TARGET=$XF/src/examples/hidforge
for DIAG in 0 1; do
	echo "== build HIDFORGE_DIAG=$DIAG"
	rm -rf "$TARGET"
	python3 "$REPO/hidforge/port_v05.py" "$REPO/hiddriver" "$REPO/hidforge/shim" "$TARGET"
	sed -i "s/-DHIDFORGE_DIAG=[01]/-DHIDFORGE_DIAG=$DIAG/" "$TARGET/BUCK"
	grep -q -- "-DHIDFORGE_DIAG=$DIAG" "$TARGET/BUCK"
	line=$(cd "$XF" && bin/buck2 build -m release -m ppc64-ilp32 //src/examples/hidforge:hidforge --show-output 2>&1 |
		tee /dev/stderr | grep -E '^xbox360//src/examples/hidforge:hidforge ' | tail -1)
	xex=$XF/${line#* }
	[ -f "$xex" ] || { echo "build: no hidforge.xex in the buck2 output"; exit 1; }
	elf=$(dirname "$xex")/image
	echo "== audit HIDFORGE_DIAG=$DIAG"
	XENON_FORGE_DIR=$XF "$REPO/tools/audit-elf.sh" "$elf"
	name=hidforge.xex
	[ $DIAG -eq 1 ] && name=hidforge-diag.xex
	cp -L "$xex" "$OUT/$name"
done

(cd "$OUT" && sha1sum hidforge.xex hidforge-diag.xex > SHA1SUMS && cat SHA1SUMS)
