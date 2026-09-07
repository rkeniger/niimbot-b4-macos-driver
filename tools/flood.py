#!/usr/bin/env python3
"""Back-channel flood test for rastertoniimbot (security audit finding 5).

Runs the filter on a raster file with fd 3 connected to a pipe that we feed
an endless stream of valid packets: every reply the filter expects (so it
gets past the control steps with acknowledgements enabled) plus a
PrintStatus that never reaches 100 %, plus junk. A filter with per-slice
deadlines would live forever; one with wall-clock deadlines must give up and
exit within the sum of its timeouts (about 50 s for one copy).

    tools/flood.py [build/rastertoniimbot] [build/case_J_normal.ras] [--limit 90]

Exit status 0 if the filter terminated within --limit seconds.
"""
import argparse
import os
import subprocess
import sys
import threading
import time


def packet(cmd, data):
    body = bytes([cmd, len(data)]) + data
    cs = 0
    for b in body:
        cs ^= b
    return b"\x55\x55" + body + bytes([cs]) + b"\xaa\xaa"


# One cycle: acks for every control step, a check-line ack, a status that is
# forever half way, a junk unsolicited packet, and some line noise.
CYCLE = b"".join([
    packet(0xC2, b"\x03"),                  # Connect
    packet(0x31, b"\x01"),                  # SetDensity
    packet(0x33, b"\x01"),                  # SetLabelType
    packet(0x02, b"\x01"),                  # PrintStart
    packet(0x14, b"\x01\x00"),              # SetPageSize
    packet(0xD3, b"\x00\xc7\x01"),          # CheckLine
    packet(0xE4, b"\x01"),                  # PageEnd
    packet(0xB3, b"\x00\x00\x32\x32\x00\x00\x00\x00"),  # PrintStatus: page 0, 50 %/50 %
    packet(0x0F, b"\x01\x02\x03"),          # unsolicited junk
    b"\x55\x55\x00\xff",                    # truncated / bad frame
])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("filter", nargs="?", default="build/rastertoniimbot")
    ap.add_argument("raster", nargs="?", default="build/case_J_normal.ras")
    ap.add_argument("--limit", type=float, default=90.0, help="seconds the filter may run")
    ap.add_argument("--options", default="", help="CUPS job options, e.g. niimbotDebug=1")
    a = ap.parse_args()

    bc_r, bc_w = os.pipe()
    proc = subprocess.Popen(
        [a.filter, "1", "flood", "flood", "1", a.options, a.raster],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        pass_fds=(bc_r, 3), preexec_fn=lambda: os.dup2(bc_r, 3),
    )
    os.close(bc_r)
    stop = threading.Event()
    sent = [0]

    def flood():
        while not stop.is_set():
            try:
                os.write(bc_w, CYCLE)
                sent[0] += len(CYCLE)
            except (BrokenPipeError, OSError):
                break
            time.sleep(0.002)

    t = threading.Thread(target=flood, daemon=True)
    t0 = time.monotonic()
    t.start()
    try:
        err = proc.communicate(timeout=a.limit)[1]
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        stop.set()
        print(f"FAIL: filter still running after {a.limit:.0f}s ({sent[0]} bytes flooded)", file=sys.stderr)
        return 1
    elapsed = time.monotonic() - t0
    stop.set()
    tail = [l for l in err.decode(errors="replace").splitlines() if not l.startswith("DEBUG: <<")]
    print("\n".join(tail[-6:]), file=sys.stderr)
    print(f"OK: filter exited {rc} after {elapsed:.1f}s, {sent[0] / 1e6:.1f} MB flooded", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
