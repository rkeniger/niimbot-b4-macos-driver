# NIIMBOT B4 macOS printer driver

A CUPS printer driver (PPD + raster filter) for the NIIMBOT B4 thermal label
printer over USB, so any macOS app can print labels through the normal print
dialog.

Status: **work in progress**. See [PLAN.md](PLAN.md) and
[docs/protocol-b4.md](docs/protocol-b4.md).

## Layout

- `filter/` — `rastertoniimbot`, the CUPS raster filter (C, libcups).
- `ppd/` — PPD source (`.drv`) compiled with `ppdc`.
- `tools/niimbot.py` — protocol diagnostics CLI (info / status / rfid / print PNG)
  over the USB serial port. Needs `pillow` and `pyserial` (see `venv`).
- `docs/` — protocol notes and test-print logs.

## Quick protocol check

```bash
python3 -m venv venv && ./venv/bin/pip install pillow pyserial
./venv/bin/python tools/niimbot.py info
./venv/bin/python tools/niimbot.py print label.png --density 3 --copies 1
```

License: MIT.
