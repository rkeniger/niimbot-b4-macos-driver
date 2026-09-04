/*
 * rastertoniimbot - CUPS raster filter for NIIMBOT B4 label printers.
 *
 * Reads CUPS raster (1-bit K, 203 dpi) on stdin and writes Niimbot packet
 * protocol to stdout for the CUPS usb backend. Printer replies arrive on the
 * back-channel (fd 3) via cupsBackChannelRead().
 *
 * Usage (from CUPS): rastertoniimbot job user title copies options [file]
 *
 * Protocol details: docs/protocol-b4.md
 */

#define _PPD_DEPRECATED /* PPD API is deprecated but still what CUPS filters use */
#include <cups/cups.h>
#include <cups/sidechannel.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- protocol constants -------------------------------------------- */
enum {
  CMD_CONNECT        = 0xC1, RSP_CONNECT        = 0xC2,
  CMD_SET_DENSITY    = 0x21, RSP_SET_DENSITY    = 0x31,
  CMD_SET_LABEL_TYPE = 0x23, RSP_SET_LABEL_TYPE = 0x33,
  CMD_PRINT_START    = 0x01, RSP_PRINT_START    = 0x02,
  CMD_SET_PAGE_SIZE  = 0x13, RSP_SET_PAGE_SIZE  = 0x14,
  CMD_BITMAP_ROW     = 0x85,
  CMD_EMPTY_ROW      = 0x84,
  CMD_CHECK_LINE     = 0x86, RSP_CHECK_LINE     = 0xD3,
  CMD_PAGE_END       = 0xE3, RSP_PAGE_END       = 0xE4,
  CMD_PRINT_STATUS   = 0xA3, RSP_PRINT_STATUS   = 0xB3,
  CMD_PRINT_END      = 0xF3, RSP_PRINT_END      = 0xF4,
  CMD_CANCEL_PRINT   = 0xDA, RSP_CANCEL_PRINT   = 0xD0,
  RSP_PAGE_INDEX     = 0xE0,
  RSP_ERROR          = 0xDB,
  RSP_NOT_SUPPORTED  = 0x00,
};

#define CHECK_LINE_INTERVAL 200
#define MAX_REPEAT 255

static const char *error_names[] = {
  NULL, "cover open", "out of labels", "low battery", "battery exception",
  "cancelled by user", "data error", "printhead too hot", "paper out exception",
  "printer busy", "no printhead", "temperature low", "printhead loose",
  "no ribbon", "wrong ribbon", "used ribbon", "wrong paper", "set paper failed",
  "set print mode failed", "set density failed", "write RFID failed",
  "set margin failed", "communication exception", "disconnected",
};
#define N_ERROR_NAMES (int)(sizeof(error_names) / sizeof(error_names[0]))

/* ---- state ---------------------------------------------------------- */
static volatile sig_atomic_t cancelled = 0;
static int ack_enabled = 1;      /* wait for replies on the back-channel */
static int ack_available = -1;   /* -1 unknown, 0 no back-channel, 1 yes */
static int verbose = 0;
static unsigned char rxbuf[8192];
static size_t rxlen = 0;
static int printer_error = 0;    /* last 0xDB code */
static unsigned page_index = 0;  /* from 0xE0, if the firmware sends it */

static void on_term(int sig) { (void)sig; cancelled = 1; }

/* Monotonic seconds; used for every deadline so a flood of packets from a
 * misbehaving device cannot stall the clock. */
static double now_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_s(double s)
{
  if (s <= 0) return;
  if (s > 600) s = 600;
  struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !cancelled) { }
}

/* ---- packet I/O ----------------------------------------------------- */
static void send_packet(unsigned char cmd, const unsigned char *data, size_t len)
{
  if (len > 255) { /* protocol length byte; never reachable after header validation */
    fprintf(stderr, "ERROR: Internal error: packet 0x%02x too long (%zu bytes)\n", cmd, len);
    exit(1);
  }
  unsigned char hdr[4] = { 0x55, 0x55, cmd, (unsigned char)len };
  unsigned char cs = cmd ^ (unsigned char)len;
  unsigned char tail[3];
  for (size_t i = 0; i < len; i++) cs ^= data[i];
  tail[0] = cs; tail[1] = 0xAA; tail[2] = 0xAA;
  if (fwrite(hdr, 1, 4, stdout) != 4 || (len && fwrite(data, 1, len, stdout) != len)
      || fwrite(tail, 1, 3, stdout) != 3) {
    fprintf(stderr, "ERROR: Unable to write to printer: %s\n", strerror(errno));
    exit(1);
  }
}

static void flush_out(void)
{
  if (fflush(stdout) != 0) {
    fprintf(stderr, "ERROR: Unable to write to printer: %s\n", strerror(errno));
    exit(1);
  }
}

static void note_packet(unsigned char cmd, const unsigned char *data, size_t len)
{
  if (cmd == RSP_ERROR && len >= 1) {
    printer_error = data[0];
  } else if (cmd == RSP_PAGE_INDEX && len >= 2) {
    page_index = (data[0] << 8) | data[1];
  }
}

/*
 * Pull bytes from the back-channel into rxbuf and return the first complete
 * packet (cmd, data, len) found, or 0 if none within `timeout` seconds.
 */
static int read_packet(unsigned char *cmd, unsigned char *data, size_t *len, double timeout)
{
  for (;;) {
    /* scan buffer for a complete frame */
    size_t i = 0;
    while (i + 1 < rxlen && !(rxbuf[i] == 0x55 && rxbuf[i + 1] == 0x55)) i++;
    if (i > 0) { memmove(rxbuf, rxbuf + i, rxlen - i); rxlen -= i; }
    if (rxlen >= 4) {
      size_t n = rxbuf[3], total = n + 7;
      if (rxlen >= total) {
        unsigned char cs = rxbuf[2] ^ rxbuf[3];
        for (size_t k = 0; k < n; k++) cs ^= rxbuf[4 + k];
        if (cs == rxbuf[4 + n] && rxbuf[5 + n] == 0xAA && rxbuf[6 + n] == 0xAA) {
          *cmd = rxbuf[2];
          *len = n;
          memcpy(data, rxbuf + 4, n);
          memmove(rxbuf, rxbuf + total, rxlen - total);
          rxlen -= total;
          if (verbose) {
            fprintf(stderr, "DEBUG: << %02x len=%zu", *cmd, n);
            for (size_t k = 0; k < n && k < 16; k++) fprintf(stderr, " %02x", data[k]);
            fputc('\n', stderr);
          }
          note_packet(*cmd, data, n);
          return 1;
        }
        /* bad frame: resync */
        memmove(rxbuf, rxbuf + 2, rxlen - 2);
        rxlen -= 2;
        continue;
      }
    }
    if (timeout <= 0) return 0;
    ssize_t got = cupsBackChannelRead((char *)rxbuf + rxlen, sizeof(rxbuf) - rxlen, timeout);
    if (got <= 0) return 0;
    rxlen += (size_t)got;
    timeout = 0.05; /* keep draining what arrived */
  }
}

static void drain(void)
{
  unsigned char cmd, data[256]; size_t len;
  while (read_packet(&cmd, data, &len, 0)) { /* note_packet() already ran */ }
}

/*
 * Send a request and wait for `resp`. Returns 1 on success (data/len filled),
 * 0 on timeout. Fatal on printer error / unsupported.
 */
static int transceive(unsigned char cmd, const unsigned char *req, size_t reqlen,
                      unsigned char resp, unsigned char *data, size_t *len, double timeout)
{
  send_packet(cmd, req, reqlen);
  flush_out();
  if (!ack_enabled || ack_available == 0) return 0;

  double deadline = now_s() + timeout;
  unsigned char c; unsigned char d[256]; size_t n;
  for (;;) {
    double remaining = deadline - now_s();
    if (remaining <= 0 || cancelled) return 0;
    double slice = remaining < 0.25 ? remaining : 0.25;
    if (read_packet(&c, d, &n, slice)) {
      if (c == resp) { if (data) memcpy(data, d, n); if (len) *len = n; ack_available = 1; return 1; }
      if (c == RSP_ERROR || c == RSP_NOT_SUPPORTED) return 0; /* caller checks printer_error */
      /* unsolicited; keep waiting, deadline is wall-clock so a flood cannot stall us */
    }
  }
}

static void fail(const char *msg) __attribute__((noreturn));
static void fail(const char *msg)
{
  if (printer_error) {
    const char *name = (printer_error > 0 && printer_error < N_ERROR_NAMES && error_names[printer_error])
                       ? error_names[printer_error] : "unknown";
    if (printer_error == 1) fputs("STATE: +cover-open-error\n", stderr);
    if (printer_error == 2 || printer_error == 8) fputs("STATE: +media-empty-error\n", stderr);
    fprintf(stderr, "ERROR: NIIMBOT %s (code %d) - %s\n", name, printer_error, msg);
  } else {
    fprintf(stderr, "ERROR: %s\n", msg);
  }
  /* best effort: get the printer out of the job */
  send_packet(CMD_CANCEL_PRINT, (const unsigned char *)"\x01", 1);
  send_packet(CMD_PRINT_END, (const unsigned char *)"\x01", 1);
  flush_out();
  exit(1);
}

/* Control step: ACK required when the back-channel works; otherwise pace. */
static void control(unsigned char cmd, const unsigned char *req, size_t reqlen,
                    unsigned char resp, const char *what, double timeout)
{
  unsigned char data[256]; size_t len = 0;
  if (transceive(cmd, req, reqlen, resp, data, &len, timeout)) {
    if (len >= 1 && data[0] == 0 && cmd != CMD_PRINT_STATUS)
      fail(what);
    return;
  }
  if (printer_error) fail(what);
  if (ack_enabled && ack_available == 1) {
    char buf[128]; snprintf(buf, sizeof(buf), "%s: no reply from printer", what);
    fail(buf);
  }
  if (ack_enabled && ack_available == -1) {
    fputs("WARNING: No back-channel data from printer; continuing without acknowledgements\n", stderr);
    ack_available = 0;
  }
  usleep(50000);
}

/* ---- rows ----------------------------------------------------------- */
static int row_is_empty(const unsigned char *row, size_t n)
{
  for (size_t i = 0; i < n; i++) if (row[i]) return 0;
  return 1;
}

static unsigned popcount_row(const unsigned char *row, size_t n)
{
  unsigned c = 0;
  for (size_t i = 0; i < n; i++) c += (unsigned)__builtin_popcount(row[i]);
  return c;
}

static void send_row(unsigned y, const unsigned char *row, size_t nbytes, unsigned repeat)
{
  static unsigned char pkt[6 + 256];
  if (row_is_empty(row, nbytes)) {
    unsigned char d[3] = { (unsigned char)(y >> 8), (unsigned char)y, (unsigned char)repeat };
    send_packet(CMD_EMPTY_ROW, d, 3);
  } else {
    unsigned total = popcount_row(row, nbytes);
    pkt[0] = (unsigned char)(y >> 8); pkt[1] = (unsigned char)y;
    pkt[2] = 0; pkt[3] = (unsigned char)(total & 0xFF); pkt[4] = (unsigned char)(total >> 8);
    pkt[5] = (unsigned char)repeat;
    memcpy(pkt + 6, row, nbytes);
    send_packet(CMD_BITMAP_ROW, pkt, 6 + nbytes);
  }
}

static void check_line(unsigned line)
{
  unsigned char d[3] = { (unsigned char)(line >> 8), (unsigned char)line, 1 };
  unsigned char data[256]; size_t len;
  transceive(CMD_CHECK_LINE, d, 3, RSP_CHECK_LINE, data, &len, 1.0);
  if (printer_error) fail("printer reported an error while receiving image data");
}

/* ---- wait for completion ------------------------------------------- */
static void wait_print_complete(unsigned copies, double timeout)
{
  unsigned char st[256]; size_t len;
  int in_page = 0; unsigned completed = 0;
  double deadline = now_s() + timeout;

  if (!ack_enabled || ack_available != 1) {
    /* no feedback: wait a fixed fraction of the budget */
    sleep_s(timeout / 4);
    return;
  }
  while (now_s() < deadline && !cancelled) {
    drain();
    if (printer_error) fail("printer reported an error while printing");
    if (page_index >= copies) return;
    if (!transceive(CMD_PRINT_STATUS, (const unsigned char *)"\x01", 1, RSP_PRINT_STATUS, st, &len, 3.0)) {
      if (printer_error) fail("printer reported an error while printing");
      continue;
    }
    if (len >= 4) {
      unsigned page = (st[0] << 8) | st[1];
      unsigned pp = st[2], fp = st[3];
      if (pp < 100 || fp < 100) in_page = 1;
      else if (in_page) { in_page = 0; completed++; }
      if (page >= copies && pp >= 100 && fp >= 100) return;
      if (completed >= copies) return;
    }
    sleep_s(0.25);
  }
  if (cancelled) fail("job cancelled");
  fputs("WARNING: Print completion not confirmed by printer\n", stderr);
}

/* ---- main ----------------------------------------------------------- */
int main(int argc, char *argv[])
{
  int fd = 0;
  cups_raster_t *ras;
  cups_page_header2_t hdr;
  ppd_file_t *ppd;
  cups_option_t *options = NULL;
  int num_options;
  int density = 3, label_type = 1;
  unsigned page = 0;

  setbuf(stderr, NULL);
  if (argc < 6 || argc > 7) {
    fprintf(stderr, "Usage: %s job-id user title copies options [file]\n", argv[0]);
    return 1;
  }
  if (argc == 7) {
    fd = open(argv[6], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "ERROR: Unable to open raster file %s: %s\n", argv[6], strerror(errno)); return 1; }
  }
  signal(SIGTERM, on_term);
  signal(SIGPIPE, SIG_IGN);

  /* options: PPD defaults, then job options */
  num_options = cupsParseOptions(argv[5], 0, &options);
  ppd = ppdOpenFile(getenv("PPD"));
  if (ppd) {
    ppdMarkDefaults(ppd);
    cupsMarkOptions(ppd, num_options, options);
    ppd_choice_t *ch;
    if ((ch = ppdFindMarkedChoice(ppd, "niimbotDensity")) != NULL) density = atoi(ch->choice);
    if ((ch = ppdFindMarkedChoice(ppd, "niimbotLabelType")) != NULL) label_type = atoi(ch->choice);
    if ((ch = ppdFindMarkedChoice(ppd, "niimbotAck")) != NULL) ack_enabled = strcmp(ch->choice, "Off") != 0;
  }
  const char *v;
  if ((v = cupsGetOption("niimbotDensity", num_options, options)) != NULL) density = atoi(v);
  if ((v = cupsGetOption("niimbotLabelType", num_options, options)) != NULL) label_type = atoi(v);
  if ((v = cupsGetOption("niimbotAck", num_options, options)) != NULL) ack_enabled = strcmp(v, "Off") != 0;
  if (cupsGetOption("niimbotDebug", num_options, options) != NULL) verbose = 1;
  if (density < 1) density = 1;
  if (density > 5) density = 5;
  if (label_type != 1 && label_type != 2 && label_type != 5) label_type = 1;

  ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras) { fputs("ERROR: Unable to read raster data\n", stderr); return 1; }

  /* say hello */
  {
    unsigned char d[256]; size_t n = 0;
    if (transceive(CMD_CONNECT, (const unsigned char *)"\x01", 1, RSP_CONNECT, d, &n, 2.0))
      fprintf(stderr, "DEBUG: NIIMBOT connect result %d\n", n ? d[0] : -1);
    else if (ack_enabled) {
      fputs("WARNING: No back-channel data from printer; continuing without acknowledgements\n", stderr);
      ack_available = 0;
    }
  }

  while (cupsRasterReadHeader2(ras, &hdr)) {
    if (cancelled) fail("job cancelled");
    page++;
    unsigned copies = hdr.NumCopies > 0 ? hdr.NumCopies : 1;
    unsigned rows = hdr.cupsHeight;
    unsigned cols = hdr.cupsWidth;
    size_t bpl = hdr.cupsBytesPerLine;
    size_t rowbytes = ((size_t)cols + 7) / 8;              /* size_t: no 32-bit wrap */
    unsigned cols_sent = (unsigned)(rowbytes * 8); /* row data is byte-granular; padding bits are white */

    fprintf(stderr, "PAGE: %u %u\n", page, copies);
    fprintf(stderr, "DEBUG: page %u: %ux%u px, %u bpp, cs %d, bpl %zu, copies %u, density %d, label type %d\n",
            page, cols, rows, hdr.cupsBitsPerPixel, hdr.cupsColorSpace, bpl, copies, density, label_type);

    /*
     * The raster header is untrusted: any local user can hand cupsd a
     * hand-made application/vnd.cups-raster file, and libcups does not
     * cross-check these fields. Reject anything inconsistent before it is
     * used to size buffers or protocol fields.
     */
    if (hdr.cupsBitsPerPixel != 1 && hdr.cupsBitsPerPixel != 8)
      fail("Unsupported raster format (need 1-bit or 8-bit grayscale)");
    if (hdr.cupsBitsPerPixel == 1 && hdr.cupsBitsPerColor != 1)
      fail("Unsupported raster format (1-bit pixel must be 1-bit color)");
    if (hdr.cupsBitsPerPixel == 8 && hdr.cupsBitsPerColor != 8)
      fail("Unsupported raster format (8-bit pixel must be 8-bit color)");
    if (hdr.cupsColorSpace != CUPS_CSPACE_K && hdr.cupsColorSpace != CUPS_CSPACE_W
        && hdr.cupsColorSpace != CUPS_CSPACE_SW)
      fail("Unsupported raster color space (need K or W grayscale)");
    if (cols == 0 || rows == 0) fail("Empty page");
    if (rowbytes > 249) fail("Page wider than the printer supports");   /* 6-byte row header + data <= 255 */
    if (rows > 65535) fail("Page longer than the printer supports");    /* protocol u16 */
    if (bpl == 0 || bpl > (64u * 1024u)) fail("Invalid raster bytes-per-line");
    if (hdr.cupsBitsPerPixel == 1 && bpl < rowbytes) fail("Invalid raster bytes-per-line for width");
    if (hdr.cupsBitsPerPixel == 8 && bpl < cols) fail("Invalid raster bytes-per-line for width");
    if (copies > 999) fail("Too many copies (max 999)");

    unsigned char *line = malloc(bpl);
    unsigned char *row = malloc(rowbytes);
    unsigned char *prev = malloc(rowbytes);
    if (!line || !row || !prev) fail("Out of memory");

    /* job/page setup */
    unsigned char d[16];
    d[0] = (unsigned char)density;
    control(CMD_SET_DENSITY, d, 1, RSP_SET_DENSITY, "Unable to set density", 2.0);
    d[0] = (unsigned char)label_type;
    control(CMD_SET_LABEL_TYPE, d, 1, RSP_SET_LABEL_TYPE, "Unable to set label type", 2.0);
    memset(d, 0, 9); d[0] = (unsigned char)(copies >> 8); d[1] = (unsigned char)copies;
    control(CMD_PRINT_START, d, 9, RSP_PRINT_START, "Unable to start print", 3.0);
    /* one-way PrintStatus after PrintStart (vendor quirk) */
    send_packet(CMD_PRINT_STATUS, (const unsigned char *)"\x01", 1);
    flush_out();
    usleep(50000);
    drain();
    memset(d, 0, 9);
    d[0] = (unsigned char)(rows >> 8); d[1] = (unsigned char)rows;
    d[2] = (unsigned char)(cols_sent >> 8); d[3] = (unsigned char)cols_sent;
    d[4] = (unsigned char)(copies >> 8); d[5] = (unsigned char)copies;
    control(CMD_SET_PAGE_SIZE, d, 9, RSP_SET_PAGE_SIZE, "Unable to set page size", 3.0);

    /* stream rows with run-length merging */
    unsigned y = 0, run_start = 0, run = 0;
    int have_prev = 0;
    for (y = 0; y < rows; y++) {
      if (cupsRasterReadPixels(ras, line, (unsigned)bpl) < 1) fail("Short raster data");
      if (hdr.cupsBitsPerPixel == 1) {
        /* CUPS K colorspace: 1 = black, MSB first: exactly what the printer wants */
        memcpy(row, line, rowbytes);
        if (hdr.cupsColorSpace != CUPS_CSPACE_K) for (size_t i = 0; i < rowbytes; i++) row[i] = ~row[i];
      } else {
        /* 8-bit: threshold; K space 255 = black, W space 0 = black */
        memset(row, 0, rowbytes);
        for (unsigned x = 0; x < cols; x++) {
          unsigned char v8 = line[x];
          int black = (hdr.cupsColorSpace == CUPS_CSPACE_K) ? (v8 >= 128) : (v8 < 128);
          if (black) row[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
        }
      }
      /* mask padding bits beyond cols */
      if (cols & 7) row[rowbytes - 1] &= (unsigned char)(0xFF << (8 - (cols & 7)));

      int boundary = (y % CHECK_LINE_INTERVAL) == 0 && y > 0;
      if (have_prev && run < MAX_REPEAT && !boundary && memcmp(row, prev, rowbytes) == 0) {
        run++;
      } else {
        if (have_prev) send_row(run_start, prev, rowbytes, run);
        if (boundary) { flush_out(); check_line(y - 1); }
        memcpy(prev, row, rowbytes);
        run_start = y; run = 1; have_prev = 1;
      }
      if (cancelled) fail("job cancelled");
    }
    if (have_prev) send_row(run_start, prev, rowbytes, run);
    flush_out();

    control(CMD_PAGE_END, (const unsigned char *)"\x01", 1, RSP_PAGE_END, "Unable to end page", 5.0);
    usleep(300000);
    page_index = 0;
    wait_print_complete(copies, 30.0 + 15.0 * copies);
    control(CMD_PRINT_END, (const unsigned char *)"\x01", 1, RSP_PRINT_END, "Unable to end print", 5.0);
    fprintf(stderr, "INFO: Printed page %u (%u copies)\n", page, copies);

    free(line); free(row); free(prev);
  }

  cupsRasterClose(ras);
  if (fd) close(fd);
  if (ppd) ppdClose(ppd);
  if (page == 0) { fputs("ERROR: No pages found in raster data\n", stderr); return 1; }
  fputs("INFO: Ready to print.\n", stderr);
  return 0;
}
