# NIIMBOT B4 macOS printer driver — plan

## Goal

A native macOS printer queue for the NIIMBOT B4 over USB, so any app can
print to it via the standard print dialog (`lp`, Preview, browsers, etc.).

## What we know (verified on this machine, 2026-09-04)

### Hardware / transport
- B4 enumerates as USB composite device, VID `0x3513`, PID `0x0002`,
  serial `B4-I728010400`. Three interfaces:
  - `0` CDC-ACM control + `1` CDC data → macOS creates
    `/dev/cu.usbmodemB4_I7280104001` (115200 baud).
  - `2` USB Printer class 7/1/2 (bidirectional), bulk OUT `0x03`, IN `0x83`.
    CUPS already sees it: `usb://NIIMBOT/B4?serial=B4-I728010400`
    (IEEE-1284 id `MFG:NIIMBOT;MDL: B4;CMD:ESC`).
- **Both** the serial port and the printer-class interface accept the
  Niimbot packet protocol (`55 55 <cmd> <len> <data…> <xor> AA AA`).
  Verified read-only: Connect, PrinterInfo, PrinterStatusData all answer
  identically on both paths.
- Also paired over Bluetooth Classic (`/dev/cu.B4-I728010400`) — out of scope
  for v1 but the same protocol should work there.

### Printer identity (live probe)
| Field | Value |
| --- | --- |
| Model ID | 6656 (`0x1A00`) = "B4" |
| Firmware | `0x0114` (1.20) |
| Hardware | 1.1 |
| Status-data protocol raw | 302 → "V5"-generation per hass-niimbot (≥302) |
| Density | 1–5, default 3 (currently 3) |
| Label type | 1 = gap (currently 1), 2 = black mark, 5 = transparent |
| DPI | 203 (8 dots/mm) |
| Print direction | top (no rotation) |
| Max print width / height | 108 mm / 350 mm (vendor table) |
| Width setting range | 20–104 mm (vendor table) |
| Printhead pixels | **unknown** — 832 is a guess in every open-source table; 864 (108 mm) also plausible. Must be measured. |
| Loaded label | T100×150 → 800 × 1200 dots. RFID tag readable (label type gap). |

### Software landscape
- Windows "driver" (`niimbot-print_win_4.2.2`) is a Flutter app + `niimbot_print_sdk.dll`
  using libusb/WinUSB — no kernel driver, no INF/GPD. Its `devices.json` is
  the source of the vendor table above. TSPL strings in the DLL belong to the
  SNBC-made models (B32/Z401), not the B4.
- `NIIMBOT 2.app` (iOS-on-Mac) ships the same `printerList.json`; no printhead width anywhere.
- `hass-niimbot` (BLE only) has a B4 entry (gen V4, printheadPixels 832 *estimated*)
  and the most complete parser/command set. Its status parser says this
  printer is V5 (9-byte PrintStart / 9-byte SetPageSize). Conflict to resolve by test.
- `niimbluelib` (TS) has the cleanest protocol reference (commands, row
  encoding, print tasks). No B4 print task mapping.
- `niimprint` (Python) has a working **serial** transport; `NiimPrintX` is BLE only.

### macOS CUPS facts
- cupsd 2.3.4 (Apple fork). `cgpdftoraster` produces CUPS raster from any PDF.
- `usb` backend supports back-channel reads (fd 3 in the filter) → the
  filter can read printer ACKs/status.
- CUPS headers available in the Xcode SDK; `clang`/`swift` present.
- `/usr/libexec/cups` is SIP-protected; third-party filters live under
  `/Library/Printers/<Vendor>/` and are referenced by absolute path in the
  PPD (`*cupsFilter2`). No custom backend needed because the stock `usb`
  backend reaches interface 2.
- Filters run in the CUPS sandbox — direct `/dev/cu.*` access from a filter is
  not something to rely on. Talking through the backend is the sanctioned path.

## Architecture (proposed)

```
app → PDF → cgpdftoraster (203 dpi, 1-bit) → rastertoniimbot → usb backend → B4 iface 2
                                                    ↑ back-channel (ACK/status)
```

Deliverables:
1. `rastertoniimbot` — CUPS raster filter in C (libcups: `cupsRasterOpen`,
   `cupsBackChannelRead`, `cupsSideChannel*`). Per page: SetDensity,
   SetLabelType, PrintStart, [PageStart], SetPageSize, rows (0x85 bitmap /
   0x84 empty / optional 0x83 indexed, 0x86 check every ~200 rows), PageEnd,
   poll PrintStatus (0xA3) until copies done, PrintEnd. Handles copies via
   SetPageSize count. Configurable ACK-wait vs paced fire-and-forget fallback.
2. `NIIMBOT B4.ppd` (from a `.drv` via `ppdc`, or hand-written): 203 dpi,
   monochrome, zero hardware margins, custom page sizes 20–108 mm wide × up to
   350 mm high, presets for common Niimbot label sizes (100×150 default),
   options: Density 1–5, LabelType (gap / black mark / transparent),
   ACK mode.
3. `install.sh` / `Makefile`: builds filter, copies filter + PPD into
   `/Library/Printers/NIIMBOT/`, runs `lpadmin -p NIIMBOT_B4 -E -v usb://… -P …`.
   `uninstall.sh` reverses it.
4. `tools/niimbot_proto.py` — the Python protocol prototype (already have the
   probe) used to answer the hardware questions before writing C. Kept in repo
   as a diagnostics tool (`info`, `status`, `print <png>`).

## Status (2026-09-04)

- Phase 0 scaffold: done.
- Phase 1 prototype + measurements: done (4 labels). See `docs/protocol-b4.md`.
- Phase 2 filter: done, verified over serial harness and through the installed
  queue with the stock usb backend (back-channel ACKs confirmed).
- Phase 3 PPD + queue: installed as `NIIMBOT_B4`; 100x150 preset, density
  (3 vs 5 visibly different), copies=2 verified via `lp`; Preview print dialog
  shows sizes/options and prints correctly. 11 labels used in total.
- Phase 4 packaging: `make install` / `make uninstall`; `.pkg` not started.

## Phases

### 0 — Scaffold
- `git init`, README, MIT license, `.gitignore`, `tools/`, `filter/`, `ppd/`.

### 1 — Protocol prototype + first test prints (Python over serial)
Goal: remove every hardware unknown before writing the filter.
- Extend the probe into `tools/niimbot_proto.py` with a `print` command
  (PNG → 1-bit rows → packets), selectable V4/V5 sequences and ACK handling.
- **Test print #1 — geometry ruler**: full-width pattern (try 864 px, then
  832) with mm tick marks and a column-number ladder on a T100×150 label.
  User reports: which columns printed, left/right offset, vertical scale.
  → Establishes printhead pixels, left margin/centering, that 800-wide rows
  print correctly, and V4 vs V5 sequence.
- **Test print #2 — copies + density**: 2 copies at density 3 and 5.
  → Establishes copies semantics and PrintStatus polling/end conditions.
- Record findings in `docs/protocol-b4.md`.

### 2 — Filter
- Write `filter/rastertoniimbot.c`; build with `clang -lcups`.
- Unit-test offline with `cupsfilter -m application/vnd.cups-raster` → raster
  file → filter → hexdump, compare against the Python prototype's bytes.
- Then live: run the filter by hand with the serial port stand-in (dev only),
  then via CUPS.

### 3 — PPD + queue
- Write `ppd/niimbot-b4.drv`, compile with `ppdc`, validate with `cupstestppd`.
- `lpadmin` the queue; `lp -d NIIMBOT_B4 test.pdf`; print from Preview.
- User verifies: size/scale correct, no clipping, copies work, density option works.

### 4 — Packaging + docs
- `install.sh` / `uninstall.sh`, README with troubleshooting
  (`/var/log/cups/error_log`, `LogLevel debug`).
- Optional later: `.pkg` installer, code signing.

### Later / out of scope for v1
- Bluetooth transport, B4 Pro (300 dpi, id 6657), a generic model table for
  other Niimbot printers, RFID/label-remaining reporting, calibration button.

## Decisions (grill-me, 2026-09-04)

| Topic | Decision |
| --- | --- |
| Driver shape | CUPS PPD + raster filter for v1. Protocol code kept as a standalone module so an `ippeveprinter`-based IPP Everywhere wrapper can reuse it later (Apple's recommended driverless path; also gives AirPrint from iPhones). |
| Transport | Printer-class USB interface via the stock CUPS `usb` backend; ACKs read on back-channel fd 3. No custom backend, no direct tty access from the filter. |
| Scope | B4 only, USB only. B4 Pro, model table, Bluetooth are follow-ups. |
| Filter language | C with libcups. |
| Label type | PPD option `LabelType`, default Gap (1); Black mark (2), Transparent (5). |
| Copies | Printer-side via SetPageSize copies count; PPD declares hardware copies. |
| ACK strategy | Request/response for control packets; rows streamed with PrinterCheckLine (0x86) every ~200 rows; fall back to paced fire-and-forget if the back-channel is silent. |
| Error policy | Fail the job: CancelPrint/PrintEnd, emit `STATE:`/`ERROR:` messages, exit non-zero so the queue stops with a readable reason. |
| Page sizes | Presets (100x150 default, 100x100, 100x180, 75x100, 50x30, 40x30, …) + CustomPageSize within 20–108 mm × 20–350 mm. Labels always portrait. |
| Halftone | Request 1-bit K raster; `cgpdftoraster` dithers. |
| Density | PPD option 1–5, default 3. |
| V4 vs V5 sequence | Try the V5 (9-byte) sequence first since the printer reports protocol raw 302; decided by test print #1. |
| Test budget | Up to ~10 T100×150 labels; each print confirmed with the user. |
| Repo | `git init` here, MIT, publish to GitHub later; upstream printhead width to hass-niimbot / niimbluelib. |
| Install | `make install` / `make uninstall` with sudo; signed `.pkg` is later polish. |
| Python prototype | Kept as `tools/niimbot.py` diagnostics CLI (info / status / rfid / print-png over serial). |
