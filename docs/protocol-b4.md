# NIIMBOT B4 protocol notes (measured on fw 1.20 / hw 1.1, 2026-09-04)

Everything below was verified against a real B4 (model id 6656, serial
I728010400) using `tools/niimbot.py` over the USB CDC serial port. Logs of the
test prints are in `docs/test*.log`.

## Transport

- USB VID `0x3513` PID `0x0002`. Interfaces: 0/1 CDC-ACM (serial, 115200 8N1,
  `/dev/cu.usbmodemB4_*` on macOS), 2 USB printer class 7/1/2 (bulk OUT 0x03,
  IN 0x83). Both carry the same packet protocol with no extra framing.
- IEEE-1284 device id on the printer interface:
  `SERN:B4-I728010400;MFG:NIIMBOT;CMD:ESC;MDL: B4;`
- No inter-packet pacing needed on serial: 1200 rows stream in ~2.4 s.

## Packet framing

`55 55 <cmd> <len> <data…> <xor> AA AA` where `xor` is the XOR of cmd, len and
every data byte. Responses use `cmd+1`-ish codes (see table). Unsolicited
packets: `0xDB` error (data[0] = error code), `0xE0` page index (not observed
on this firmware).

## Command sequence that works (the "V5" family)

| Step | Packet | Reply |
| --- | --- | --- |
| Connect | `C1 01 01` | `C2 01 03` |
| SetDensity n (1–5) | `21 01 n` | `31 01 01` |
| SetLabelType t (1 gap, 2 black mark, 5 transparent) | `23 01 t` | `33 01 01` |
| PrintStart, 9 bytes: total_pages u16, 0,0,0,0, page_color, speed, flag | `01 09 00 NN 00 00 00 00 00 00 00` | `02 01 01` |
| PrintStatus one-way (niimbluelib quirk; harmless) | `A3 01 01` | `B3 08 …` |
| SetPageSize, 9 bytes: rows u16, cols u16, copies u16, 0 u16, 0 | `13 09 RR RR CC CC NN NN 00 00 00` | `14 02 01 00` |
| rows… | see below | none |
| PrinterCheckLine every 200 rows: line u16, 1 | `86 03 LL LL 01` | `D3 03 LL LL 01` (sent twice) |
| PageEnd | `E3 01 01` | `E4 01 01` |
| poll PrintStatus until done | `A3 01 01` | `B3 08 PP PP pr fd …` |
| PrintEnd | `F3 01 01` | `F4 01 01` |
| CancelPrint (abort) | `DA 01 01` | `D0 01 01` |

No `PageStart` (0x03) is needed in this sequence. `total_pages` in PrintStart
and `copies` in SetPageSize are both set to the copy count.

### Rows

- `0x85` PrintBitmapRow: `row u16, counters[3], repeat u8, bits[cols/8]`.
  Bits MSB-first, 1 = black. For an 832 px head the three "counters" are the
  total-form `00 lo hi` (total black pixels); the split-count form used by
  small heads overflows a byte at this width. Zeros also work.
- `0x84` PrintEmptyRow: `row u16, repeat u8` (repeat ≤ 255).
- Identical consecutive rows are merged with `repeat` (≤ 255).

### PrintStatus (0xB3) payload, 8 bytes

`page u16, print_progress u8, feed_progress u8, ?? u16, ?? u8, ?? u8`.
Observed: `print_progress` ramps 0→100 while imaging, then `feed_progress`
ramps 0→100 while the label feeds to the gap; `page` increments after each copy
completes (1 for a single copy, 2 for two). Before a job the previous job's
100/100 is retained, so wait for progress to drop below 100 before treating
100/100 as completion. Sending PrintEnd at print=100/feed<100 still let the
label feed out, but the driver waits for feed=100 and page ≥ copies.

## Geometry

- 203 dpi in both axes (8 px/mm), confirmed with a printed ruler.
- **The printer centres the row data on the printhead.** Sending rows exactly
  as wide as the label (100 mm → 800 px) prints edge to edge with nothing
  clipped. Sending 832 px on a 100 mm label clips ~2 mm on each side.
  Therefore the driver sends `cols = label width in px`, no padding to the head.
- Printhead width is ≥ 832 px (104 mm, the vendor app's max width setting);
  the exact number is not needed by the driver. Vendor max print width is 108 mm.
- Print direction "top": image rows go in as printed; no rotation.
- Max label length (vendor): 350 mm = 2800 rows.

## Info queries (0x40 + type → reply 0x40|type)

| type | meaning | B4 reply |
| --- | --- | --- |
| 8 | model id u16 | `1A 00` = 6656 |
| 9 | software version | `01 14` = 1.20 |
| 12 | hardware version | `01 01` |
| 11 | serial (ASCII) | `I728010400` |
| 1 / 3 | density / label type | 3 / 1 |
| 10 | battery % | 100 (mains powered, always 100) |
| 15 | "area" | `00 1A` (meaning unknown) |

`0xA5 01` PrinterStatusData → `0xB5`: `30302ee000c80000000f000302`; bytes
11–12 = `03 02` → "302", which hass-niimbot maps to protocol generation 5.

`0x1A 01` RFID → `0x1B`: uuid[8], barcode (len-prefixed), serial
(len-prefixed), total u16, used u16, type u8, capacity u16.

## Settings persistence

`SetDensity` / `SetLabelType` are stored in the printer and survive PrintEnd,
CancelPrint, a new Connect, and a USB reconnect (verified). Every job therefore
sets both explicitly; a job printed from another app with different options
changes what the printer reports afterwards.
