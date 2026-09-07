import struct, sys
def raster(width, height, bpp, bpl, copies=1, cs=3, pixels=None):
    h = bytearray(1796)
    struct.pack_into(">I", h, 340, copies)        # NumCopies
    struct.pack_into(">I", h, 372, width)         # cupsWidth
    struct.pack_into(">I", h, 376, height)        # cupsHeight
    struct.pack_into(">I", h, 384, 1 if bpp == 1 else 8)  # cupsBitsPerColor
    struct.pack_into(">I", h, 388, bpp)           # cupsBitsPerPixel
    struct.pack_into(">I", h, 392, bpl)           # cupsBytesPerLine
    struct.pack_into(">I", h, 400, cs)            # cupsColorSpace
    struct.pack_into(">I", h, 420, 1)             # cupsNumColors
    struct.pack_into(">II", h, 276, 203, 203)
    body = pixels if pixels is not None else bytes([0xAA]) * (bpl * min(height, 4096))
    return b"RaS3" + bytes(h) + body
cases = {
  "A_width_max_8bpp": raster(0xFFFFFFFF, 10, 8, 100),
  "B_width_wrap_1bpp": raster(0xFFFFFFF9, 10, 1, 100),
  "C_bpl_lt_rowbytes": raster(800, 10, 1, 10),
  "D_width_2000": raster(2000, 10, 1, 250),
  "E_height_0": raster(800, 0, 1, 100),
  "F_width_0": raster(0, 10, 1, 100),
  "G_copies_max": raster(800, 10, 1, 100, copies=0xFFFFFFFF),
  "H_8bpp_bpl_lt_cols": raster(800, 10, 8, 100),
  "I_width_1992": raster(1992, 10, 1, 249),
  "J_normal": raster(800, 10, 1, 100),
}
for name, data in cases.items():
    open(f"build/case_{name}.ras", "wb").write(data)
print(" ".join(cases))
