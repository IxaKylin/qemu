#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the real-board ROCK 5B ZVisor on the remote PM server and copy the
# artifacts used by the QEMU rock-5b Type-C USB CDC boot.
#
# The remote server is the same host as ~/remote-pm.sh
# (zevorn@devstation1.sh1.jcsmt.com) and the workspace is
# ~/work/project/processone-poc.  The remote build is the documented
# "just build" (= scripts/zephyr-build-container.sh host-sd) target; this
# script does not modify any remote source or configuration.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
REMOTE="${REMOTE_PM_TARGET:-zevorn@devstation1.sh1.jcsmt.com}"
WORKSPACE="${REMOTE_PM_WORKSPACE:-work/project/processone-poc}"
OUT="${OUT_DIR:-$REPO_ROOT/builds/rock5b-usb/zvisor}"
LOG_DIR="$REPO_ROOT/builds/rock5b-usb/logs"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="$LOG_DIR/remote-build-$STAMP.log"

mkdir -p "$OUT" "$LOG_DIR"

echo "remote build: $REMOTE:$WORKSPACE -> $OUT"
{
    echo "# remote-build $STAMP"
    ssh "$REMOTE" "set -e
        cd \$HOME/$WORKSPACE
        echo '=== git provenance ==='
        git -C hypervisor/src branch --show-current
        git -C hypervisor/src rev-parse HEAD
        git -C hypervisor/src status --short
        git -C zephyr/src rev-parse HEAD
        git -C zephyr/src status --short
        echo '=== build ==='
        just build"
} 2>&1 | tee "$LOG"

echo "copying artifacts"
ssh "$REMOTE" "cd \$HOME/$WORKSPACE/build/rock5b-plus-dhcp && tar czf - \
    zvisor.img zvisor.dtb zephyr/zephyr.elf zephyr/zvisor/zvisor_system.json \
    build-manifest.json zephyr/.config build.log build_info.yml" \
    | tar xzf - -C "$OUT"

mv "$OUT/zephyr/zephyr.elf" "$OUT/zephyr.elf"
mv "$OUT/zephyr/zvisor/zvisor_system.json" "$OUT/zvisor_system.json"
rm -rf "$OUT/zephyr/zvisor"
cp "$LOG" "$OUT/remote-build-console.log"

echo "verifying artifact hashes against build-manifest.json"
python3 - "$OUT" <<'PYEOF'
import hashlib
import json
import os
import sys

out = sys.argv[1]
manifest = json.load(open(os.path.join(out, "build-manifest.json")))
artifacts = manifest.get("artifacts", {})
failed = False
for name, info in artifacts.items():
    path = os.path.join(out, os.path.basename(name))
    if not os.path.isfile(path):
        path = os.path.join(out, name)
    if not os.path.isfile(path):
        print("missing artifact:", name)
        failed = True
        continue
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    ok = digest == info["sha256"] and os.path.getsize(path) == info["size"]
    print(("OK   " if ok else "FAIL ") + name +
          " sha256=" + digest + " size=" + str(os.path.getsize(path)))
    failed = failed or not ok
sys.exit(1 if failed else 0)
PYEOF

echo "artifacts:"
ls -l "$OUT"
echo "sha256:"
shasum -a 256 "$OUT/zvisor.img" "$OUT/zvisor.dtb" 2>/dev/null || \
    sha256sum "$OUT/zvisor.img" "$OUT/zvisor.dtb"
echo "next: scripts/rock5b-usb/make-zvisor-spi.py"
