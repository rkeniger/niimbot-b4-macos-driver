# Security audit of rastertoniimbot (2026-09-04)

## Threat model

The filter runs as the unprivileged `_lp` user inside the CUPS filter sandbox,
so a compromise yields no more than that user's rights and the ability to talk
to the printer. Untrusted inputs are:

1. **The CUPS raster stream on stdin.** Normally produced by `cgpdftoraster`,
   but any local user who can print can submit a hand-made
   `application/vnd.cups-raster` file and cupsd will run only this filter on
   it. libcups' `cupsRasterReadHeader2` does not cross-check header fields
   (verified: inconsistent `cupsWidth` / `cupsBytesPerLine` pass through).
2. **Printer replies on the back-channel (fd 3).** A malicious or faulty USB
   device controls every byte.
3. **Job options (`argv[5]`)** and the `PPD` environment variable, set by cupsd
   from user-supplied job attributes.

## Method

- Manual review of `filter/rastertoniimbot.c`.
- `clang --analyze` (clean before and after).
- AddressSanitizer + UndefinedBehaviorSanitizer build run against crafted
  raster headers (cases A–J, generator now in `tools/craft_rasters.py`) and a flooding back-channel
  (`build/flood.py`).

## Findings and fixes

| # | Severity | Issue | Fix |
| --- | --- | --- | --- |
| 1 | High | `cupsWidth` near `UINT_MAX` wrapped in `(cols + 7) / 8` to a zero-byte row buffer; the 8-bit path then wrote `row[x >> 3]` for `x < cols` → **heap buffer overflow write** (ASan, line 413). The 1-bit path masked `row[rowbytes - 1]` → pointer underflow (UBSan, line 417). | Compute `rowbytes` in `size_t`; reject `cols == 0`, `rowbytes > 249`. |
| 2 | High | `cupsBytesPerLine` smaller than the row implied by `cupsWidth` → **heap over-read** of the line buffer in both the 1-bit `memcpy` (line 405) and the 8-bit loop. | Require `bpl >= rowbytes` (1-bit) / `bpl >= cols` (8-bit); reject `bpl == 0` or > 64 KiB. |
| 3 | Medium | `send_packet` truncated `len` to a byte. A row of 250 data bytes made a 256-byte payload whose length byte wrapped to 0, silently corrupting the protocol stream. | Cap `rowbytes` at 249 and make `send_packet` abort on `len > 255`. |
| 4 | Medium | Huge `NumCopies` produced a multi-hour (`useconds_t`-overflowing) sleep in the no-ACK completion wait → job hang. | Reject `copies > 999`; replace `usleep` with a bounded `nanosleep` helper. |
| 5 | Medium | Deadlines were decremented per read slice, so a device streaming unsolicited packets kept `transceive` and the completion wait alive forever → **filter hang / queue DoS** (reproduced with the flood test). | All deadlines now use `CLOCK_MONOTONIC`. Flood test completes in ~14 s. |
| 6 | Low | Zero-size pages (`cupsWidth`/`cupsHeight` = 0) were sent to the printer as a bogus page. | Rejected as "Empty page". |
| 7 | Low | Unsupported color spaces / bit-depth combinations were only partly checked. | Explicit whitelist: 1-bit or 8-bit, K/W/SW gray. |

## Reviewed and judged safe

- Back-channel parser: length byte bounds every copy (`n ≤ 255` into 256-byte
  buffers), the receive buffer is bounded at 8 KiB and resyncs on bad frames,
  and error codes index `error_names` only after a range check.
- Row packets: static 262-byte buffer vs. `6 + rowbytes ≤ 255`.
- Options: values go through `atoi` and are clamped or whitelisted; no
  user-controlled format strings.
- Signals: handler only sets a `sig_atomic_t` flag.
- Python tools run as the invoking user against a serial port; they are
  development aids, not part of the installed driver.
- `Makefile install`: the device URI from `lpinfo` is passed quoted to
  `lpadmin`; no shell interpolation of device-supplied text.

## Residual risks

- A device can still make a job slow (each control step waits up to its
  timeout), but never unbounded.
- The filter trusts `cupsRasterReadPixels` to honour `cupsBytesPerLine`.
- No fuzzing of the raster *pixel* stream was done; the pixel path only
  copies/thresholds a validated number of bytes.

## Addendum: IPP Everywhere command (2026-09-07)

`niimbot-ipp-print` shares `filter/niimbot.c` with the filter, so the raster
header validation above applies unchanged (the refactor was checked
byte-for-byte against the crafted cases A–J). Differences in the threat model:

- **Network exposure.** `ippeveprinter` listens on all interfaces by default
  and advertises via DNS-SD, so anyone on the LAN can submit jobs (AirPrint's
  design). Spool files are untrusted: PWG/Apple raster from arbitrary clients
  goes through the same header checks; a page wider than 880 px is refused
  before any printer traffic. `make install-ipp IPP_HOST=localhost` restricts
  the server to loopback. ippeveprinter itself is Apple-maintained code
  running as the logged-in user, not root.
- **Job attributes** arrive as environment variables. Only `IPP_COPIES`
  (`atoi`, bounded by the core to 1–999), `IPP_PRINT_QUALITY` and the
  `media-type` keyword scanned out of `IPP_MEDIA_COL` (bounded copy into a
  64-byte buffer, `[A-Za-z0-9_.-]` only) are used.
- **Serial port.** Opened with `TIOCEXCL`; device replies are parsed by the
  same bounded frame parser. The device path comes from `glob(3)` of fixed
  patterns or the operator-set `NIIMBOT_DEVICE`.
- `clang --analyze` clean on all three C files.
