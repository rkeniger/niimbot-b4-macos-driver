#!/usr/bin/env python3
"""NIIMBOT B4 diagnostics / protocol prototype.

Talks the Niimbot packet protocol over the B4's USB CDC serial port (default)
or its USB printer-class interface (--transport usb, needs pyusb).

    niimbot.py info
    niimbot.py status
    niimbot.py rfid
    niimbot.py print image.png [--density 3] [--label-type 1] [--copies 1]
                               [--seq v5|v4] [--head 832] [--dry-run] [-v]

Packet format: 55 55 <cmd> <len> <data...> <xor of cmd,len,data> AA AA
"""
from __future__ import annotations

import argparse
import glob
import struct
import sys
import time
from dataclasses import dataclass

# ---------------------------------------------------------------- commands
CMD = {
    "CONNECT": 0xC1,
    "PRINTER_INFO": 0x40,
    "STATUS_DATA": 0xA5,
    "RFID_INFO": 0x1A,
    "HEARTBEAT": 0xDC,
    "PAPER_INFO": 0x59,
    "SET_DENSITY": 0x21,
    "SET_LABEL_TYPE": 0x23,
    "PRINT_START": 0x01,
    "PAGE_START": 0x03,
    "SET_PAGE_SIZE": 0x13,
    "BITMAP_ROW": 0x85,
    "BITMAP_ROW_INDEXED": 0x83,
    "EMPTY_ROW": 0x84,
    "CHECK_LINE": 0x86,
    "PAGE_END": 0xE3,
    "PRINT_STATUS": 0xA3,
    "PRINT_END": 0xF3,
    "CANCEL_PRINT": 0xDA,
}
RESP_PAGE_INDEX = 0xE0
RESP_ERROR = 0xDB
RESP_CHECK_LINE = 0xD3

INFO_TYPES = {
    "density": 1, "speed": 2, "label_type": 3, "language": 6,
    "auto_shutdown": 7, "model_id": 8, "sw_version": 9, "battery": 10,
    "serial": 11, "hw_version": 12, "bt_address": 13, "print_mode": 14,
    "area": 15,
}

ERROR_NAMES = {
    1: "cover open", 2: "lack paper", 3: "low battery", 4: "battery exception",
    5: "user cancel", 6: "data error", 7: "too hot", 8: "paper out exception",
    9: "printer busy", 10: "no printer head", 11: "temperature low",
    12: "printer head loose", 13: "no ribbon", 14: "wrong ribbon",
    15: "used ribbon", 16: "wrong paper", 17: "set paper fail",
    18: "set print mode fail", 19: "set print density fail",
    20: "write rfid fail", 21: "set margin fail", 22: "communication exception",
    23: "disconnect", 24: "canvas parameter error", 25: "rotation parameter exception",
    26: "json parameter exception", 27: "b3s abnormal paper output",
    28: "e-check paper", 29: "rfid tag not written", 30: "set density fail",
    31: "set label type fail", 32: "set print speed fail",
}


class PrinterError(Exception):
    pass


class Timeout(PrinterError):
    pass


# ---------------------------------------------------------------- packets
@dataclass
class Packet:
    cmd: int
    data: bytes

    def to_bytes(self) -> bytes:
        body = bytes([self.cmd, len(self.data)]) + self.data
        cs = 0
        for b in body:
            cs ^= b
        return b"\x55\x55" + body + bytes([cs]) + b"\xaa\xaa"

    @staticmethod
    def parse_stream(buf: bytearray) -> list["Packet"]:
        """Consume complete packets from buf (in place)."""
        out = []
        while True:
            i = buf.find(b"\x55\x55")
            if i < 0:
                buf.clear()
                break
            if i > 0:
                del buf[:i]
            if len(buf) < 4:
                break
            n = buf[3]
            total = n + 7
            if len(buf) < total:
                break
            cmd = buf[2]
            data = bytes(buf[4:4 + n])
            cs = buf[4 + n]
            calc = cmd ^ n
            for b in data:
                calc ^= b
            if calc != cs or buf[5 + n:7 + n] != b"\xaa\xaa":
                # bad frame: skip the header and resync
                del buf[:2]
                continue
            del buf[:total]
            out.append(Packet(cmd, data))
        return out


# ---------------------------------------------------------------- transports
class SerialTransport:
    def __init__(self, port: str | None = None):
        import serial  # pyserial

        if port is None:
            cands = glob.glob("/dev/cu.usbmodem*B4*") or glob.glob("/dev/cu.usbmodem*")
            if not cands:
                raise PrinterError("no /dev/cu.usbmodem* serial port found")
            port = cands[0]
        self.port = port
        self.ser = serial.Serial(port, 115200, timeout=0)

    def write(self, b: bytes) -> None:
        self.ser.write(b)
        self.ser.flush()

    def read(self) -> bytes:
        return self.ser.read(4096)

    def close(self) -> None:
        self.ser.close()

    def __str__(self):
        return f"serial {self.port}"


class UsbTransport:
    """USB printer-class interface (bulk endpoints) via pyusb."""

    def __init__(self, vid=0x3513, pid=0x0002):
        import usb.core
        import usb.util

        self.usb = usb
        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev is None:
            raise PrinterError("USB device %04x:%04x not found" % (vid, pid))
        cfg = dev.get_active_configuration()
        intf = usb.util.find_descriptor(cfg, bInterfaceClass=7)
        usb.util.claim_interface(dev, intf.bInterfaceNumber)
        self.dev, self.intf = dev, intf
        self.ep_out = usb.util.find_descriptor(
            intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.ep_in = usb.util.find_descriptor(
            intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)

    def write(self, b: bytes) -> None:
        self.ep_out.write(b, timeout=2000)

    def read(self) -> bytes:
        try:
            return bytes(self.ep_in.read(self.ep_in.wMaxPacketSize * 8, timeout=20))
        except self.usb.core.USBTimeoutError:
            return b""

    def close(self) -> None:
        self.usb.util.release_interface(self.dev, self.intf.bInterfaceNumber)

    def __str__(self):
        return "usb printer-class interface"


# ---------------------------------------------------------------- client
class Client:
    def __init__(self, transport, verbose=False, dry_run=False):
        self.t = transport
        self.verbose = verbose
        self.dry_run = dry_run
        self.buf = bytearray()
        self.unsolicited: list[Packet] = []
        self.last_page_index = 0

    def log(self, prefix, b: bytes):
        if self.verbose:
            print(f"{prefix} {b.hex(' ')}", file=sys.stderr)

    def send(self, cmd: int, data: bytes = b"") -> None:
        raw = Packet(cmd, data).to_bytes()
        self.log(">>", raw)
        if not self.dry_run:
            self.t.write(raw)

    def poll(self) -> list[Packet]:
        if self.dry_run:
            return []
        chunk = self.t.read()
        if chunk:
            self.buf.extend(chunk)
        pkts = Packet.parse_stream(self.buf)
        for p in pkts:
            self.log("<<", p.to_bytes())
        return pkts

    def _note(self, p: Packet) -> None:
        if p.cmd == RESP_PAGE_INDEX and len(p.data) >= 2:
            self.last_page_index = struct.unpack(">H", p.data[:2])[0]
        elif p.cmd == RESP_ERROR and p.data:
            code = p.data[0]
            raise PrinterError(f"printer error {code}: {ERROR_NAMES.get(code, '?')}")

    def transceive(self, cmd: int, data: bytes = b"", resp: int | None = None,
                   timeout: float = 2.0) -> Packet:
        """Send and wait for a response. Default expected response is cmd+1."""
        if resp is None:
            resp = (cmd + 1) & 0xFF
        self.send(cmd, data)
        if self.dry_run:
            return Packet(resp, b"\x01")
        t0 = time.time()
        while time.time() - t0 < timeout:
            for p in self.poll():
                if p.cmd == resp:
                    return p
                if p.cmd == RESP_ERROR:
                    self._note(p)
                if p.cmd == 0x00:  # In_NotSupported
                    raise PrinterError(f"command 0x{cmd:02x} not supported")
                self._note(p)
                self.unsolicited.append(p)
            time.sleep(0.005)
        raise Timeout(f"no 0x{resp:02x} response to 0x{cmd:02x} within {timeout}s")

    # ---- queries
    def connect(self) -> int:
        return self.transceive(CMD["CONNECT"], b"\x01", 0xC2).data[0]

    def info(self, kind: str):
        p = self.transceive(CMD["PRINTER_INFO"], bytes([INFO_TYPES[kind]]),
                            0x40 | INFO_TYPES[kind])
        d = p.data
        if kind in ("serial", "bt_address"):
            return d.decode(errors="replace") if kind == "serial" else d.hex(":")
        if kind in ("model_id", "area"):
            return struct.unpack(">H", d[:2])[0]
        if kind in ("sw_version", "hw_version"):
            return f"{d[0]}.{d[1]}"
        return int.from_bytes(d, "big")

    def status_data(self) -> dict:
        d = self.transceive(CMD["STATUS_DATA"], b"\x01", 0xB5).data
        raw = d[11] * 100 + d[12] if len(d) > 12 else 0
        return {"raw": d.hex(), "support_color": d[10] if len(d) > 10 else None,
                "protocol_raw": raw}

    def rfid(self) -> dict | None:
        d = self.transceive(CMD["RFID_INFO"], b"\x01", 0x1B).data
        if len(d) <= 1:
            return None
        i = 8
        uuid = d[:8].hex()
        bl = d[i]; i += 1
        barcode = d[i:i + bl].decode(errors="replace"); i += bl
        sl = d[i]; i += 1
        serial = d[i:i + sl].decode(errors="replace"); i += sl
        total, used, typ = struct.unpack(">HHB", d[i:i + 5]); i += 5
        cap = struct.unpack(">H", d[i:i + 2])[0] if len(d) >= i + 2 else None
        return {"uuid": uuid, "barcode": barcode, "serial": serial, "total": total,
                "used": used, "type": typ, "capacity": cap, "raw": d.hex()}

    def heartbeat(self, kind=4, wait=True):
        if not wait:
            self.send(CMD["HEARTBEAT"], bytes([kind]))
            return None
        return self.transceive(CMD["HEARTBEAT"], bytes([kind]), 0xD9 if kind == 4 else None).data.hex()

    def print_status(self) -> dict:
        d = self.transceive(CMD["PRINT_STATUS"], b"\x01", 0xB3, timeout=3.0).data
        page, p1, p2 = struct.unpack(">HBB", d[:4])
        return {"page": page, "print_progress": p1, "feed_progress": p2, "raw": d.hex()}

    # ---- print control
    def set_density(self, n: int) -> bool:
        return bool(self.transceive(CMD["SET_DENSITY"], bytes([n]), 0x31).data[0])

    def set_label_type(self, n: int) -> bool:
        return bool(self.transceive(CMD["SET_LABEL_TYPE"], bytes([n]), 0x33).data[0])

    def print_start(self, seq: str, total_pages: int) -> bool:
        if seq == "v4":
            data = struct.pack(">HBBBBB", total_pages, 0, 0, 0, 0, 0)
        else:  # v5: + speed, flag
            data = struct.pack(">HBBBBBBB", total_pages, 0, 0, 0, 0, 0, 0, 0)
        return bool(self.transceive(CMD["PRINT_START"], data, 0x02).data[0])

    def page_start(self) -> bool:
        return bool(self.transceive(CMD["PAGE_START"], b"\x01", 0x04).data[0])

    def set_page_size(self, seq: str, rows: int, cols: int, copies: int) -> bool:
        if seq == "v4":
            data = struct.pack(">HHH", rows, cols, copies)
        else:
            data = struct.pack(">HHHHB", rows, cols, copies, 0, 0)
        return bool(self.transceive(CMD["SET_PAGE_SIZE"], data, 0x14).data[0])

    def page_end(self) -> bool:
        return bool(self.transceive(CMD["PAGE_END"], b"\x01", 0xE4, timeout=5.0).data[0])

    def print_end(self) -> bool:
        return bool(self.transceive(CMD["PRINT_END"], b"\x01", 0xF4, timeout=5.0).data[0])

    def cancel_print(self) -> bool:
        return bool(self.transceive(CMD["CANCEL_PRINT"], b"\x01", 0xD0).data[0])

    def check_line(self, line: int) -> None:
        try:
            self.transceive(CMD["CHECK_LINE"], struct.pack(">HB", line, 1), RESP_CHECK_LINE, timeout=1.0)
        except Timeout:
            if self.verbose:
                print(f"check_line({line}): no reply (ignored)", file=sys.stderr)

    # ---- rows
    @staticmethod
    def row_counters(row: bytes, head_pixels: int) -> bytes:
        chunk = head_pixels // 8 // 3
        # Split counts (three u8 chunk totals) only fit heads <= 765 px; wider
        # heads (B4: 832) use the "total" form: 0x00, lo, hi.
        if 0 < chunk and chunk * 8 <= 255 and len(row) <= chunk * 3:
            return bytes((
                bin(int.from_bytes(row[0:chunk], "big")).count("1"),
                bin(int.from_bytes(row[chunk:chunk * 2], "big")).count("1"),
                bin(int.from_bytes(row[chunk * 2:chunk * 3], "big")).count("1"),
            ))
        total = bin(int.from_bytes(row, "big")).count("1")
        return bytes((0, total & 0xFF, (total >> 8) & 0xFF))

    def send_rows(self, rows: list[bytes], head_pixels: int, check_every: int = 200,
                  pace: float = 0.0) -> None:
        """rows: list of packed 1-bit rows (1 = black), MSB first."""
        y = 0
        n = len(rows)
        while y < n:
            row = rows[y]
            # run-length: identical consecutive rows
            rep = 1
            while y + rep < n and rows[y + rep] == row and rep < 255 \
                    and (y + rep) % check_every != 0:
                rep += 1
            if not any(row):
                self.send(CMD["EMPTY_ROW"], struct.pack(">HB", y, rep))
            else:
                hdr = struct.pack(">H3sB", y, self.row_counters(row, head_pixels), rep)
                self.send(CMD["BITMAP_ROW"], hdr + row)
            for p in self.poll():
                self._note(p)
            if pace:
                time.sleep(pace)
            y += rep
            if check_every and y % check_every == 0 and y < n:
                self.check_line(y - 1)

    def wait_print_complete(self, copies: int, timeout: float = 60.0, poll: float = 0.25) -> dict:
        """Poll PrintStatus until every copy has printed AND fed.

        Observed on B4 fw 1.20: page counter stays 0 for a single copy;
        print_progress ramps 0..100, then feed_progress ramps 0..100.
        We count a copy as done on each print=100 & feed=100 after having seen
        progress below 100 (a fresh page), and also accept page >= copies or an
        unsolicited 0xE0 page index if the firmware sends one.
        """
        t0 = time.time()
        in_page = False
        completed = 0
        st = {}
        while time.time() - t0 < timeout:
            for p in self.poll():
                self._note(p)
            if self.last_page_index >= copies:
                return {"page": self.last_page_index, "done": "page_index"}
            st = self.print_status()
            if self.verbose:
                print(f"status {st}", file=sys.stderr)
            pp, fp = st["print_progress"], st["feed_progress"]
            if pp < 100 or fp < 100:
                in_page = True
            elif in_page:  # transition to fully done
                in_page = False
                completed += 1
            if st["page"] >= copies and pp >= 100 and fp >= 100:
                return st
            if completed >= copies:
                st["completed"] = completed
                return st
            time.sleep(poll)
        raise Timeout(f"print not confirmed complete after {timeout}s (last {st})")

    def print_image(self, rows: list[bytes], cols: int, *, seq="v5", density=3,
                    label_type=1, copies=1, head_pixels=832, check_every=200,
                    pace=0.0) -> dict:
        n_rows = len(rows)
        print(f"print: {cols}x{n_rows} px, seq={seq}, density={density}, "
              f"label_type={label_type}, copies={copies}, head={head_pixels}", file=sys.stderr)
        self.set_density(density)
        self.set_label_type(label_type)
        self.print_start(seq, copies)
        if seq == "v4":
            self.page_start()
        else:
            # V5 quirk (niimbluelib): a one-way PrintStatus right after PrintStart
            self.send(CMD["PRINT_STATUS"], b"\x01")
            time.sleep(0.05)
            self.poll()
        self.set_page_size(seq, n_rows, cols, copies)
        t0 = time.time()
        self.send_rows(rows, head_pixels, check_every=check_every, pace=pace)
        print(f"rows sent in {time.time() - t0:.1f}s", file=sys.stderr)
        self.page_end()
        time.sleep(0.5)
        st = self.wait_print_complete(copies)
        self.print_end()
        return st


# ---------------------------------------------------------------- image helpers
def image_to_rows(path: str, width: int | None = None, threshold: int = 128):
    """Load an image, convert to 1-bit rows (1 = black). Returns (rows, cols)."""
    from PIL import Image

    img = Image.open(path).convert("L")
    if width and img.width != width:
        canvas = Image.new("L", (width, img.height), 255)
        canvas.paste(img, (0, 0))
        img = canvas
    cols = (img.width + 7) // 8 * 8
    if cols != img.width:
        canvas = Image.new("L", (cols, img.height), 255)
        canvas.paste(img, (0, 0))
        img = canvas
    bw = img.point(lambda v: 0 if v < threshold else 255, "1")  # 0 = black
    rows = []
    raw = bw.tobytes()  # 1 bpp, MSB first, padded to byte per row; 1 = white
    stride = cols // 8
    for y in range(img.height):
        line = raw[y * stride:(y + 1) * stride]
        rows.append(bytes(b ^ 0xFF for b in line))  # invert: 1 = black
    return rows, cols


# ---------------------------------------------------------------- CLI
def open_client(args) -> Client:
    t = UsbTransport() if args.transport == "usb" else SerialTransport(args.port)
    c = Client(t, verbose=args.verbose, dry_run=args.dry_run)
    if not args.dry_run:
        c.connect()
    return c


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--transport", choices=["serial", "usb"], default="serial")
    ap.add_argument("--port", help="serial port (default: auto /dev/cu.usbmodem*B4*)")
    ap.add_argument("-v", "--verbose", action="store_true", help="hex-dump packets")
    ap.add_argument("--dry-run", action="store_true", help="don't touch the printer")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    sub.add_parser("status")
    sub.add_parser("rfid")
    p = sub.add_parser("print")
    p.add_argument("image")
    p.add_argument("--density", type=int, default=3)
    p.add_argument("--label-type", type=int, default=1, help="1 gap, 2 black mark, 5 transparent")
    p.add_argument("--copies", type=int, default=1)
    p.add_argument("--seq", choices=["v4", "v5"], default="v5")
    p.add_argument("--head", type=int, default=832, help="printhead pixels (for row counters)")
    p.add_argument("--width", type=int, help="pad/crop image to this width in px")
    p.add_argument("--check-every", type=int, default=200, help="0 disables CheckLine")
    p.add_argument("--pace", type=float, default=0.0, help="sleep between row packets (s)")
    args = ap.parse_args(argv)

    if args.cmd == "print":
        rows, cols = image_to_rows(args.image, args.width)
    c = open_client(args)
    try:
        if args.cmd == "info":
            for k in ("model_id", "sw_version", "hw_version", "serial", "density",
                      "label_type", "battery", "area", "print_mode", "auto_shutdown"):
                try:
                    print(f"{k:14s} {c.info(k)}")
                except PrinterError as e:
                    print(f"{k:14s} ({e})")
            print(f"{'status_data':14s} {c.status_data()}")
        elif args.cmd == "status":
            print("status_data", c.status_data())
            print("heartbeat  ", c.heartbeat())
            print("print_stat ", c.print_status())
        elif args.cmd == "rfid":
            print(c.rfid())
        elif args.cmd == "print":
            st = c.print_image(rows, cols, seq=args.seq, density=args.density,
                               label_type=args.label_type, copies=args.copies,
                               head_pixels=args.head, check_every=args.check_every,
                               pace=args.pace)
            print("done:", st)
    except PrinterError as e:
        print(f"error: {e}", file=sys.stderr)
        if args.cmd == "print" and not args.dry_run:
            try:
                c.cancel_print()
                c.print_end()
            except PrinterError:
                pass
        return 1
    finally:
        c.t.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
