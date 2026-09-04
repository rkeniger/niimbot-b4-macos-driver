# NIIMBOT B4 macOS printer driver

A CUPS printer driver (PPD + raster filter) for the NIIMBOT B4 thermal label
printer over USB, so any macOS app can print labels through the normal print
dialog.

This coding work was done by Claude. I was annoyed that the macOS Niimbot app
didn't have support for their B4 printer, and that there was no macOS driver.
Inspired by [this article](https://schlarp.com/posts/everything-i-own-owned/),
I asked Claude to create me a printer driver for the B4 and it works 🎉.

It's currently USB-only and has no Bluetooth support. I've also only tested it
on 100x150mm labels, but it should work on other sizes. It uses the deprecated
CUPS driver path because it was the easiest but I'll probably add Bluetooth
and update to a newer driver type. It also doesn't have an installer, you have
to add the driver manually using the terminal. But it works.

I'm constantly amazed at what these LLMs can do. If you'd told me three years
ago that I could automatically reverse engineer and build a printer driver,
I'd have thought you were nuts. So great for cases like this where the
manufacturer is lazy.

I have no other Niimbot models so although Claude could probably add support
for them, I have no way to test them. So it's B4 only.

Status: **work in progress**. See [PLAN.md](PLAN.md) and
[docs/protocol-b4.md](docs/protocol-b4.md).

## Layout

- `filter/` — `rastertoniimbot`, the CUPS raster filter (C, libcups).
- `ppd/` — PPD source (`.drv`) compiled with `ppdc`.
- `tools/niimbot.py` — protocol diagnostics CLI (info / status / rfid / print PNG)
  over the USB serial port. Needs `pillow` and `pyserial` (see `venv`).
- `docs/` — protocol notes and test-print logs.

## Install

```bash
make
sudo make install      # copies filter + PPD to /Library/Printers/NIIMBOT, creates queue NIIMBOT_B4
lp -d NIIMBOT_B4 -o PageSize=100x150mm docs/ruler-100x150.pdf
```

Options (print dialog "Printer Settings" pane or `-o`): `niimbotDensity=1..5`,
`niimbotLabelType=1|2|5` (gap / black mark / transparent), `niimbotAck=On|Off`.
`-o niimbotDebug=1` dumps printer replies to `/var/log/cups/error_log`.

`sudo make uninstall` removes the queue and the files.

## Quick protocol check

```bash
python3 -m venv venv && ./venv/bin/pip install pillow pyserial
./venv/bin/python tools/niimbot.py info
./venv/bin/python tools/niimbot.py print label.png --density 3 --copies 1
```

## License

MIT. See [LICENSE](LICENSE).

## Notice / acknowledgements

The Niimbot packet protocol used here was learned from prior open-source
reverse-engineering work and then verified against a real B4 (see
[docs/protocol-b4.md](docs/protocol-b4.md)). No code was copied from these
projects, but this driver would not exist without them:

- [eigger/hass-niimbot](https://github.com/eigger/hass-niimbot) (MIT) — the
  most complete Python implementation: command set, status/RFID parsers, row
  encoding, and the vendor device table analysis.
- [MultiMote/niimbluelib](https://github.com/MultiMote/niimbluelib) (MIT) —
  clean TypeScript protocol reference: packet generator, print task sequences,
  image encoder.
- [AndBondStyle/niimprint](https://github.com/AndBondStyle/niimprint) (MIT,
  originally by kjy00302) — the first USB-serial transport for these printers.

Model capability data (dpi, print width, label types, density range) comes
from NIIMBOT's own published device tables. NIIMBOT is a trademark of its
owner; this project is not affiliated with or endorsed by NIIMBOT.
