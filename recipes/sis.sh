#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: LGPL-2.1-only
#
# sis.sh -- after the component, in the toolchain container: the package as
#   $PHOREPOC_BUILD/sis/phevmware-tools-x32.sis   phevmwsvc.exe, its menu entry and icon, the licences, NOTICE and the source offer
# with recipes/pack.py (the pdk's tools do the registration resource and the icon: rcomp and mifconv are in the toolchain image).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
B=${PHOREPOC_BUILD:-/build}
export PHOREPOC_PDK=${PHOREPOC_PDK:?PHOREPOC_PDK is not set}
V=${PHOREPOC_PACKAGE_VERSION:-0.1.0}
# Link stubs: a name the service calls that no provider exports is linked to a stub that panics the caller. The release fails on
# any name recipes/linkstubs.allow does not list ("name  # reason" lines).
STUBS=$B/x32img/phevmwsvc.exe.stubs
if [ -s "$STUBS" ]; then
    BAD=$(grep -v '^$' "$STUBS" | while read -r n; do grep -qE "^$n([[:space:]]|$)" "$HERE/linkstubs.allow" 2>/dev/null || echo "$n"; done)
    [ -z "$BAD" ] || { echo "sis.sh: $STUBS has link stubs not in recipes/linkstubs.allow:"; echo "$BAD"; exit 1; }
fi
mkdir -p "$B/sis"
python3 "$HERE/pack.py" --service "$B/x32img/phevmwsvc.exe" --version "$V" --out "$B/sis/phevmware-tools-x32.sis"
ls -l "$B/sis"
