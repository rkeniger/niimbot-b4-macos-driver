# IPP Everywhere / AirPrint front end (2026-09-07)

Apple has deprecated PPD-based CUPS drivers; the supported path is a printer
that speaks IPP. The B4 does not, so this repo now ships a small local IPP
server that does it on the printer's behalf:

```
any app (Mac, iPhone, iPad) --IPP/AirPrint--> ippeveprinter (LaunchAgent, port 8631)
                                                  |  -c niimbot-ipp-print  (spool file = PWG or Apple raster)
                                                  v
                                       Niimbot packets over /dev/cu.usbmodemB4* (USB CDC)
                                                  or /dev/cu.B4-* (Bluetooth Classic)
```

- `ippeveprinter` is Apple's IPP Everywhere server. We build our own copy from
  the CUPS 2.3.6 source (`ippeve/`, see its README) because the 2.3.4 binary
  in `/usr/bin` rejects chunked Create-Job requests, which is what iOS
  AirPrint sends. It advertises the printer with DNS-SD (`_ipp._tcp`, subtypes
  `_print,_universal`), validates jobs, spools the document and runs our
  command once per job with the job attributes in `IPP_*` environment variables.
- `niimbot-ipp-print` (filter/niimbot-ipp-print.c) reads the spool file with
  libcups' raster API (PWG raster and Apple raster/URF both work) and drives
  the printer with the same protocol core as the CUPS filter
  (filter/niimbot.c). It opens the serial port itself: ippeveprinter's own
  device URIs (`file://`, `socket://`) have no back-channel, and the ACK /
  status polling needs one.
- macOS creates a normal queue from it with `lpadmin -m everywhere` (the PPD
  is generated from the printer's attributes), or via System Settings →
  Printers → Add Printer, where it shows up as a Bonjour "NIIMBOT B4".
- iPhones/iPads on the same network see it as an AirPrint printer (URF).

## Files

| File | Purpose |
| --- | --- |
| `filter/niimbot.[ch]` | protocol core: packets, page sequence, ACK/status handling, raster validation |
| `filter/niimbot-ipp-print.c` | ippeveprinter print command + `--probe` |
| `ipp/niimbot-b4.conf` | printer attributes for `ippeveprinter -a` (media, formats, quality, copies) |
| `ippeve/` | vendored CUPS 2.3.6 `ippeveprinter.c` + headers, built as `ippeveprinter` |
| `ipp/local.niimbot.b4-ipp.plist.in` | LaunchAgent template; Makefile substitutes paths/port |

## Install

```bash
make
sudo make install-ipp      # copies to /Library/Printers/NIIMBOT, loads the LaunchAgent,
                           # creates queue NIIMBOT_B4_IPP via lpadmin -m everywhere
sudo make uninstall-ipp
```

Variables: `IPP_PORT` (8631), `IPP_QUEUE` (NIIMBOT_B4_IPP), `IPP_NAME`
("NIIMBOT B4"), `IPP_HOST` (empty = listen on all interfaces and advertise on
the LAN; `localhost` = loopback only, no AirPrint from phones, nothing exposed).

The agent lives in `/Library/LaunchAgents/local.niimbot.b4-ipp.plist`, runs as
the logged-in user, logs to `/tmp/niimbot-ipp.log`, and serves a status/web
page at `http://localhost:8631/` (the "Media" form there sets the loaded label
size that AirPrint clients default to).

## How print options map

| Client option | Source | Printer setting |
| --- | --- | --- |
| Media size | raster dimensions | `SetPageSize` rows/cols; the printer centres the image on the head |
| Media type Labels / LabelsBlackMark / Transparency | `IPP_MEDIA_COL` → `media-type` | label type 1 / 2 / 5 (`NIIMBOT_LABEL_TYPE` when absent) |
| Quality Draft / Normal / High | `IPP_PRINT_QUALITY` | density 2 / `NIIMBOT_DENSITY` (3) / 5 |
| Copies | `IPP_COPIES`, else raster `NumCopies` | printer-side copies (`SetPageSize`) |

macOS's generated PPD says `cupsManualCopies: true`, so cupsd renders N copies
as N pages and sends `copies=1`; each page then prints once (verified with
`lp -n 2`: two pages, one copy each). AirPrint clients may instead send
`copies=N` with one page; both paths give N labels.

Environment knobs (set in the plist's `EnvironmentVariables`):
`NIIMBOT_DEVICE` (`auto` | `usb` | `bluetooth` | `/dev/cu.xxx` |
`file:/path` dry run), `NIIMBOT_DENSITY`, `NIIMBOT_LABEL_TYPE`,
`NIIMBOT_ACK=off`, `NIIMBOT_DEBUG=1`.

## Raster formats

The attribute file advertises `image/pwg-raster` (`black_1`, `sgray_8`) and
`image/urf` (`W8`). macOS prefers Apple raster when both are offered, so Mac
jobs arrive as 8-bit grey rendered by `cgpdftoraster`; the command thresholds
at 128 (crisp text and barcodes, no dithering of photos). PWG `black_1` jobs
(Linux clients, or if URF is removed from the conf) arrive already dithered.

## Verified (2026-09-07, no labels used)

- `filter/niimbot.c` refactor: `rastertoniimbot` output is byte-identical
  (stdout and stderr) to the pre-refactor binary on all 12 test rasters in
  `build/` (crafted header cases A–J, ruler, README test page).
- `ippeveprinter -a ipp/niimbot-b4.conf` loads; `ipptool get-printer-attributes`
  shows all 12 sizes, the 20–108 × 20–350 mm custom range, both formats.
- `lpadmin -m everywhere` against it produced a PPD with every size as
  `NNNxMMMmm.Borderless`, `Custom.WIDTHxHEIGHT`, MediaType and quality menus.
- Jobs from `lp` arrived as URF W8 799×1198 px for 100×150 mm and produced the
  expected packet stream (dry run to a file); `-o MediaType=LabelsBlackMark
  -o print-quality=5` → label type 2, density 5.
- `niimbot-ipp-print --probe` over `/dev/cu.usbmodemB4_*` answered: model
  6656 (B4), firmware 1.20, serial, battery, density, label type.
- DNS-SD record (loopback-only instance): `rp=ipp/print ty=NIIMBOT B4
  pdl=image/pwg-raster,image/urf URF=CP99,IS1,PQ3-4-5,RS203,V1.4,W8`.

- 2026-09-07 15:11: `sudo make install-ipp` and a real 100x150 ruler label
  through `NIIMBOT_B4_IPP` over USB: printed correctly (the 49 s job time
  was the printer running out of labels mid-job).

- AirPrint from an iPhone, first attempt: the printer appeared but the only
  paper size was "Photo Small" (iOS offers `media-col-ready` sizes only; we
  reported just 100x150) and every Create-Job failed with "Unexpected
  document data following request". Reproduced with `ipptool` using
  `TRANSFER chunked`: `/usr/bin/ippeveprinter` (2.3.4) fails, a build of the
  2.3.6 source against the same libcups passes. Fixed by vendoring 2.3.6
  (`ippeve/`) and by reporting every preset as ready media.

Not yet verified: a print from an iPhone with the vendored server, Bluetooth (the same serial code
opens `/dev/cu.B4-*`; RFCOMM connect delay and any Bluetooth privacy prompt
for a LaunchAgent are unknowns).

## Troubleshooting

```bash
tail -f /tmp/niimbot-ipp.log                                   # server + command messages
/Library/Printers/NIIMBOT/niimbot-ipp-print --probe            # is the printer reachable?
ipptool -tv ipp://localhost:8631/ipp/print get-printer-attributes.test
lpstat -p NIIMBOT_B4_IPP; lpstat -W completed -o NIIMBOT_B4_IPP
launchctl print gui/$(id -u)/local.niimbot.b4-ipp | head       # agent state
```

Printer errors (cover open, out of labels, ...) abort the job with an
`ERROR: NIIMBOT ...` message in the log and in the job's `job-state-message`;
`cover-open` / `media-needed` are set on `printer-state-reasons` until the
next job starts. (`media-empty` is deliberately not used: ippeveprinter holds
all later jobs while it is set and only a web-form change clears it.)

## Notes

- `ippeveprinter -f` cannot be combined with `-a` (it switches to legacy mode);
  the `-a` default format list is already `image/pwg-raster,image/urf`.
- Media-type names are shown as the keywords (`LabelsBlackMark`); the 2.3.6
  server supports `-S file.strings`, but macOS's PPD generator was not seen
  to use them, so none is shipped.
- The old PPD queue (`NIIMBOT_B4`, usb backend on the printer-class interface)
  can stay installed; the IPP path uses the CDC serial interface. Do not print
  to both at once.
