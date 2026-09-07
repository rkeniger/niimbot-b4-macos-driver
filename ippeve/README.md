# Vendored ippeveprinter (CUPS 2.3.6)

`ippeveprinter.c`, `config.h` and `cups/*.h` are copied unmodified from
Apple's CUPS 2.3.6 release (https://github.com/apple/cups, tag v2.3.6,
Apache License 2.0 with the GPL exception, see LICENSE and NOTICE).

Why: the `/usr/bin/ippeveprinter` shipped with macOS (CUPS 2.3.4) rejects
Create-Job requests that use chunked transfer encoding with
"Unexpected document data following request". iOS AirPrint sends exactly
that, so printing from an iPhone fails; the 2.3.6 source does not have the
bug. The tool is built against the system libcups (the private structures it
shares with libcups are identical between 2.3.3 and 2.3.6) and installed as
`/Library/Printers/NIIMBOT/ippeveprinter`.

`config.h` is the output of `./configure` on macOS; only the `HAVE_*` and
`CUPS_*` constants matter to this one tool.
