/*
 * rastertoniimbot - CUPS raster filter for NIIMBOT B4 label printers.
 *
 * Reads CUPS raster (1-bit K, 203 dpi) on stdin and writes Niimbot packet
 * protocol to stdout for the CUPS usb backend. Printer replies arrive on the
 * back-channel (fd 3) via cupsBackChannelRead(). The protocol itself lives in
 * niimbot.c, shared with the IPP Everywhere command niimbot-ipp-print.
 *
 * Usage (from CUPS): rastertoniimbot job user title copies options [file]
 *
 * Protocol details: docs/protocol-b4.md
 */

#define _PPD_DEPRECATED /* PPD API is deprecated but still what CUPS filters use */
#include "niimbot.h"
#include <cups/cups.h>
#include <cups/sidechannel.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- transport: stdout to the backend, fd 3 back from it ------------ */
static int cups_write(void *ctx, const unsigned char *buf, size_t len)
{
  (void)ctx;
  return fwrite(buf, 1, len, stdout) == len ? 0 : -1;
}

static int cups_flush(void *ctx)
{
  (void)ctx;
  return fflush(stdout);
}

static ssize_t cups_read(void *ctx, unsigned char *buf, size_t len, double timeout)
{
  (void)ctx;
  return cupsBackChannelRead((char *)buf, len, timeout);
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
  niimbot_job_t job = { 3, 1, 1, 0, 0, NULL };
  niimbot_io_t io = { NULL, cups_write, cups_flush, cups_read };
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

  /* options: PPD defaults, then job options */
  num_options = cupsParseOptions(argv[5], 0, &options);
  ppd = ppdOpenFile(getenv("PPD"));
  if (ppd) {
    ppdMarkDefaults(ppd);
    cupsMarkOptions(ppd, num_options, options);
    ppd_choice_t *ch;
    if ((ch = ppdFindMarkedChoice(ppd, "niimbotDensity")) != NULL) job.density = atoi(ch->choice);
    if ((ch = ppdFindMarkedChoice(ppd, "niimbotLabelType")) != NULL) job.label_type = atoi(ch->choice);
    if ((ch = ppdFindMarkedChoice(ppd, "niimbotAck")) != NULL) job.ack = strcmp(ch->choice, "Off") != 0;
  }
  const char *v;
  if ((v = cupsGetOption("niimbotDensity", num_options, options)) != NULL) job.density = atoi(v);
  if ((v = cupsGetOption("niimbotLabelType", num_options, options)) != NULL) job.label_type = atoi(v);
  if ((v = cupsGetOption("niimbotAck", num_options, options)) != NULL) job.ack = strcmp(v, "Off") != 0;
  if (cupsGetOption("niimbotDebug", num_options, options) != NULL) job.verbose = 1;

  niimbot_init(&io, &job);

  ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras) { fputs("ERROR: Unable to read raster data\n", stderr); return 1; }

  niimbot_connect();

  while (cupsRasterReadHeader2(ras, &hdr)) {
    if (niimbot_cancelled) niimbot_fail("job cancelled");
    page++;
    unsigned copies = hdr.NumCopies > 0 ? hdr.NumCopies : 1;
    fprintf(stderr, "PAGE: %u %u\n", page, copies);
    niimbot_print_page(ras, &hdr, page, copies);
  }

  cupsRasterClose(ras);
  if (fd) close(fd);
  if (ppd) ppdClose(ppd);
  if (page == 0) { fputs("ERROR: No pages found in raster data\n", stderr); return 1; }
  fputs("INFO: Ready to print.\n", stderr);
  return 0;
}
