/*
 * niimbot-ipp-print - ippeveprinter(1) print command for the NIIMBOT B4.
 *
 * ippeveprinter runs this once per job with the spool file as argv[1]
 * (image/pwg-raster from IPP Everywhere clients, image/urf from AirPrint
 * clients; both are read with libcups' raster API). The command talks the
 * Niimbot packet protocol (niimbot.c) directly to the printer's serial port:
 * the USB CDC port (/dev/cu.usbmodemB4*) or the Bluetooth Classic port
 * (/dev/cu.B4-*). Progress and errors go to stderr in the CUPS/ippeveprinter
 * message format (ERROR:, STATE:, ATTR:, INFO:, DEBUG:).
 *
 * Usage:
 *   niimbot-ipp-print spool-file      (from ippeveprinter -c)
 *   niimbot-ipp-print --probe         open the printer, print its identity, exit
 *
 * Environment (set in the LaunchAgent plist, or by ippeveprinter per job):
 *   NIIMBOT_DEVICE      auto (default) | usb | bluetooth | /dev/cu.xxx | file:/path (dry run)
 *   NIIMBOT_DENSITY     1..5 used for print-quality=normal (default 3)
 *   NIIMBOT_LABEL_TYPE  1 gap | 2 black mark | 5 transparent, when the job has no media-type
 *   NIIMBOT_ACK         off = do not wait for printer acknowledgements
 *   NIIMBOT_DEBUG       1 = hex-dump printer replies
 *   IPP_COPIES, IPP_PRINT_QUALITY, IPP_MEDIA_COL, IPP_MEDIA_TYPE, CONTENT_TYPE (from ippeveprinter)
 */

#include "niimbot.h"
#include <cups/raster.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define USB_GLOB       "/dev/cu.usbmodemB4*"
#define BT_GLOB        "/dev/cu.B4-*"
#define OUTBUF_SIZE    16384
#define MAX_WIDTH_PX   880          /* 110 mm at 203 dpi: vendor max 108 mm plus rounding */

/* ---- transport: a serial port (or a file for dry runs) --------------- */
static int dev_fd = -1;
static int dev_is_file = 0;
static unsigned char outbuf[OUTBUF_SIZE];
static size_t outlen = 0;

static int write_all(int fd, const unsigned char *buf, size_t len)
{
  while (len > 0) {
    ssize_t n = write(fd, buf, len);
    if (n < 0) { if (errno == EINTR) continue; return -1; }
    buf += n; len -= (size_t)n;
  }
  return 0;
}

static int dev_flush(void *ctx)
{
  (void)ctx;
  if (outlen == 0) return 0;
  if (write_all(dev_fd, outbuf, outlen) != 0) return -1;
  outlen = 0;
  return 0;
}

static int dev_write(void *ctx, const unsigned char *buf, size_t len)
{
  if (outlen + len > sizeof(outbuf) && dev_flush(ctx) != 0) return -1;
  if (len > sizeof(outbuf)) return write_all(dev_fd, buf, len);
  memcpy(outbuf + outlen, buf, len);
  outlen += len;
  return 0;
}

static ssize_t dev_read(void *ctx, unsigned char *buf, size_t len, double timeout)
{
  (void)ctx;
  if (dev_is_file) return -1;
  fd_set rfds;
  struct timeval tv;
  FD_ZERO(&rfds);
  FD_SET(dev_fd, &rfds);
  tv.tv_sec = (time_t)timeout;
  tv.tv_usec = (suseconds_t)((timeout - (double)tv.tv_sec) * 1e6);
  int rc = select(dev_fd + 1, &rfds, NULL, NULL, &tv);
  if (rc <= 0) return 0;                         /* timeout or EINTR */
  ssize_t n = read(dev_fd, buf, len);
  return n < 0 ? 0 : n;                          /* EAGAIN/EIO: treat as silence; writes will fail */
}

static int open_serial(const char *path)
{
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return -1;
  ioctl(fd, TIOCEXCL);                            /* keep other jobs/tools off the port */
  fcntl(fd, F_SETFL, 0);                          /* blocking from here on */
  struct termios t;
  if (tcgetattr(fd, &t) == 0) {
    cfmakeraw(&t);
    cfsetspeed(&t, B115200);                      /* ignored by USB CDC and RFCOMM, harmless */
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | PARENB | CSTOPB);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
  }
  tcflush(fd, TCIOFLUSH);
  return fd;
}

static const char *glob_first(const char *pattern, char *buf, size_t n)
{
  glob_t g;
  const char *result = NULL;
  if (glob(pattern, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
    strlcpy(buf, g.gl_pathv[0], n);
    result = buf;
  }
  globfree(&g);
  return result;
}

/* Resolve NIIMBOT_DEVICE to a path. Returns NULL if nothing matches. */
static const char *find_device(const char *spec, char *buf, size_t n)
{
  if (!spec || !*spec || !strcmp(spec, "auto"))
    return glob_first(USB_GLOB, buf, n) ? buf : (glob_first(BT_GLOB, buf, n) ? buf : NULL);
  if (!strcmp(spec, "usb")) return glob_first(USB_GLOB, buf, n);
  if (!strcmp(spec, "bluetooth") || !strcmp(spec, "bt")) return glob_first(BT_GLOB, buf, n);
  strlcpy(buf, spec, n);
  return buf;
}

static int open_device(niimbot_job_t *job, char *path, size_t n)
{
  const char *spec = getenv("NIIMBOT_DEVICE");
  if (spec && !strncmp(spec, "file:", 5)) {
    strlcpy(path, spec + 5, n);
    dev_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dev_is_file = 1;
    job->dry_run = 1;
    return dev_fd;
  }
  if (!find_device(spec, path, n)) {
    fprintf(stderr, "ERROR: NIIMBOT B4 not found: no %s (USB) or %s (Bluetooth) port"
                    " - is the printer on and connected?\n", USB_GLOB, BT_GLOB);
    return -1;
  }
  dev_fd = open_serial(path);
  if (dev_fd < 0)
    fprintf(stderr, "ERROR: Unable to open %s: %s\n", path, strerror(errno));
  return dev_fd;
}

/* ---- job options from the environment -------------------------------- */
static int env_int(const char *name, int dflt)
{
  const char *v = getenv(name);
  return (v && *v) ? atoi(v) : dflt;
}

/* Find "media-type=<keyword>" inside an ippAttributeString() collection dump. */
static const char *media_type_from_col(const char *col, char *buf, size_t n)
{
  const char *p = col ? strstr(col, "media-type=") : NULL;
  if (!p) return NULL;
  p += 11;
  if (*p == '"' || *p == '\'') p++;
  size_t i = 0;
  while (*p && i + 1 < n && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.')) buf[i++] = *p++;
  buf[i] = '\0';
  return i ? buf : NULL;
}

static int label_type_for_media(const char *type, int dflt)
{
  if (!type) return dflt;
  if (!strcmp(type, "labels")) return 1;
  if (!strcmp(type, "labels-black-mark") || !strcmp(type, "continuous")) return 2;
  if (!strcmp(type, "transparency")) return 5;
  return dflt;
}

static int density_for_quality(const char *q, int normal)
{
  if (!q || !*q) return normal;
  if (!strcmp(q, "draft") || !strcmp(q, "3")) return 2;
  if (!strcmp(q, "high") || !strcmp(q, "5")) return 5;
  return normal;
}

/* ---- --probe --------------------------------------------------------- */
static int probe(void)
{
  char path[256];
  niimbot_job_t job = { 3, 1, 1, getenv("NIIMBOT_DEBUG") != NULL, 0, NULL };
  niimbot_io_t io = { NULL, dev_write, dev_flush, dev_read };
  if (open_device(&job, path, sizeof(path)) < 0) return 1;
  niimbot_init(&io, &job);
  printf("device: %s\n", path);
  unsigned char d[256]; size_t n = 0;
  if (!niimbot_transceive(0xC1, (const unsigned char *)"\x01", 1, 0xC2, d, &n, 2.0)) {
    fputs("ERROR: printer did not answer Connect\n", stderr);
    return 1;
  }
  static const struct { unsigned char type; const char *label; int kind; } q[] = {
    { 8,  "model",      2 }, { 9, "firmware", 3 }, { 12, "hardware", 3 },
    { 11, "serial",     1 }, { 10, "battery",  0 }, { 1, "density", 0 }, { 3, "label type", 0 },
  };
  for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
    unsigned char t = q[i].type;
    n = 0;
    if (!niimbot_transceive(0x40, &t, 1, (unsigned char)(0x40 | t), d, &n, 2.0)) {
      printf("%-11s (no reply)\n", q[i].label);
      continue;
    }
    if (q[i].kind == 1) printf("%-11s %.*s\n", q[i].label, (int)n, d);
    else if (q[i].kind == 2 && n >= 2) {
      unsigned id = (unsigned)((d[0] << 8) | d[1]);
      printf("%-11s %u%s\n", q[i].label, id, id == 6656 ? " (B4)" : "");
    } else if (q[i].kind == 3 && n >= 2) printf("%-11s %u.%u\n", q[i].label, d[0], d[1]);
    else if (n >= 1) printf("%-11s %u\n", q[i].label, d[n - 1]);
  }
  return 0;
}

/* ---- main ------------------------------------------------------------ */
int main(int argc, char *argv[])
{
  setbuf(stderr, NULL);
  if (argc == 2 && !strcmp(argv[1], "--probe")) return probe();
  if (argc != 2) {
    fputs("Usage: niimbot-ipp-print spool-file | --probe\n", stderr);
    return 1;
  }

  const char *content_type = getenv("CONTENT_TYPE");
  if (content_type && strcmp(content_type, "image/pwg-raster") && strcmp(content_type, "image/urf")
      && strcmp(content_type, "application/vnd.cups-raster") && strcmp(content_type, "application/octet-stream")) {
    fprintf(stderr, "ERROR: Unsupported document format %s (need image/pwg-raster or image/urf)\n", content_type);
    return 1;
  }

  niimbot_job_t job = { 3, 1, 1, 0, 0, "media-needed-error" };
  niimbot_io_t io = { NULL, dev_write, dev_flush, dev_read };
  char mtbuf[64], path[256];

  int normal_density = env_int("NIIMBOT_DENSITY", 3);
  job.density = density_for_quality(getenv("IPP_PRINT_QUALITY"), normal_density);
  const char *media_type = getenv("IPP_MEDIA_TYPE");
  if (!media_type) media_type = media_type_from_col(getenv("IPP_MEDIA_COL"), mtbuf, sizeof(mtbuf));
  job.label_type = label_type_for_media(media_type, env_int("NIIMBOT_LABEL_TYPE", 1));
  const char *ack = getenv("NIIMBOT_ACK");
  job.ack = !(ack && (!strcasecmp(ack, "off") || !strcmp(ack, "0")));
  job.verbose = getenv("NIIMBOT_DEBUG") != NULL;

  /* copies: the IPP job attribute wins; the raster header is the fallback */
  int ipp_copies = env_int("IPP_COPIES", 0);

  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) { fprintf(stderr, "ERROR: Unable to open %s: %s\n", argv[1], strerror(errno)); return 1; }
  cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras) { fputs("ERROR: Unable to read raster data (not PWG or Apple raster?)\n", stderr); return 1; }

  if (open_device(&job, path, sizeof(path)) < 0) return 1;
  niimbot_init(&io, &job);
  fprintf(stderr, "DEBUG: %s, device %s, density %d, label type %d (media-type %s), copies attr %d\n",
          content_type ? content_type : "unknown type", path, job.density, job.label_type,
          media_type ? media_type : "none", ipp_copies);

  /* a new job: previous printer conditions no longer apply until proven otherwise */
  fputs("STATE: -cover-open,media-needed\n", stderr);
  niimbot_connect();

  cups_page_header2_t hdr;
  unsigned page = 0;
  while (cupsRasterReadHeader2(ras, &hdr)) {
    if (niimbot_cancelled) niimbot_fail("job cancelled");
    page++;
    unsigned copies = ipp_copies > 0 ? (unsigned)ipp_copies : (hdr.NumCopies > 0 ? hdr.NumCopies : 1);
    /* IPP clients choose sizes freely (and cupsd falls back to Letter for an
     * unknown PageSize); anything wider than the 108 mm head would print as
     * a clipped mess on a long feed, so refuse it here. */
    if (hdr.cupsWidth > MAX_WIDTH_PX)
      niimbot_fail("Page wider than the printer supports (max 108 mm)");
    if (ipp_copies > 0 && hdr.NumCopies > 1)
      fprintf(stderr, "DEBUG: raster header asks for %u copies, using IPP copies=%d\n", hdr.NumCopies, ipp_copies);
    niimbot_print_page(ras, &hdr, page, copies);
    fprintf(stderr, "ATTR: job-impressions-completed=%u\n", page);
  }

  cupsRasterClose(ras);
  close(fd);
  dev_flush(NULL);
  close(dev_fd);
  if (page == 0) { fputs("ERROR: No pages found in raster data\n", stderr); return 1; }
  fputs("INFO: Ready to print.\n", stderr);
  return 0;
}
