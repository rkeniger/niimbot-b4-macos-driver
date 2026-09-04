#!/usr/bin/env python3
"""Run rastertoniimbot against the B4's serial port, emulating the CUPS
backend: filter stdout -> serial, serial -> filter fd 3 (back-channel).

    run_filter_serial.py build/rastertoniimbot build/test.ras [--ppd build/niimbot-b4.ppd]
                         [--options "niimbotDensity=4 niimbotDebug=1"] [--copies 1] [--port /dev/cu...]

Lets us test the real filter end to end without installing it (no sudo).
"""
import argparse
import glob
import os
import selectors
import subprocess
import sys
import threading

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("filter")
    ap.add_argument("raster")
    ap.add_argument("--ppd", default="build/niimbot-b4.ppd")
    ap.add_argument("--options", default="")
    ap.add_argument("--copies", default="1")
    ap.add_argument("--port")
    ap.add_argument("--log", help="write raw bytes sent to the printer here")
    a = ap.parse_args()

    port = a.port or (glob.glob("/dev/cu.usbmodem*B4*") or glob.glob("/dev/cu.usbmodem*"))[0]
    ser = serial.Serial(port, 115200, timeout=0.05)

    # back-channel pipe: we write printer bytes into bc_w; filter reads fd 3 = bc_r
    bc_r, bc_w = os.pipe()
    # In the child: dup the pipe's read end onto fd 3 (preexec_fn runs before
    # close_fds), and list both bc_r and 3 in pass_fds so neither gets closed.
    env = dict(os.environ, PPD=a.ppd)
    proc = subprocess.Popen(
        [a.filter, "1", os.environ.get("USER", "user"), "test", a.copies, a.options, a.raster],
        stdout=subprocess.PIPE, env=env, pass_fds=(bc_r, 3),
        preexec_fn=lambda: os.dup2(bc_r, 3),
    )
    os.close(bc_r)
    logf = open(a.log, "wb") if a.log else None
    sent = 0
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            data = ser.read(4096)
            if data:
                try:
                    os.write(bc_w, data)
                except OSError:
                    break

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    while True:
        chunk = proc.stdout.read1(65536)
        if not chunk:
            break
        ser.write(chunk)
        sent += len(chunk)
        if logf:
            logf.write(chunk)
    rc = proc.wait()
    stop.set()
    ser.flush()
    print(f"filter exit {rc}, {sent} bytes sent to {port}", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
