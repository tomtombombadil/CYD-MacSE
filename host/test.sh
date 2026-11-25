#!/bin/sh
# PC test: builds the host runner, runs the stand-in ROM, checks the Mac
# screen reached the canvas and the UI pages draw. Linux / macOS / CI.
set -e
cd "$(dirname "$0")"
make -s
D=$(mktemp -d)
python3 ../tools/make_test_rom.py "$D/MacSE.ROM"
head -c 819200 /dev/zero > "$D/blank.dsk"
./macse_host "$D" wait:1000 shot:"$D/run.pbm" tap:72,30 wait:200 shot:"$D/disks.pbm" | tee "$D/out.txt"
python3 - "$D" <<'PY'
import sys
d = sys.argv[1]
def load(n):
    b = open(d + "/" + n, "rb").read()
    return b[b.index(b"\n", b.index(b"\n") + 1) + 1:]
run = load("run.pbm")
# Mac screen row 100, bytes 18..81 of the canvas row = 0xAAAA5555 stripes
row = run[100 * 100 + 18: 100 * 100 + 82]
assert row == bytes.fromhex("AAAA5555") * 16, row.hex()
out = open(d + "/out.txt").read()
ticks = int(out.split("ticks ")[1].split(",")[0])
assert 65 <= ticks <= 95, out          # 60.15 ticks a second over the 1.3 s run
assert "rom 1" in out, out
disks = load("disks.pbm")
assert disks != run                     # the Disks page drew
print("host test OK:", ticks, "ticks in 1.3 s")
PY
rm -rf "$D"
