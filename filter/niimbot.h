/*
 * niimbot.h - NIIMBOT B4 packet protocol core shared by the CUPS raster
 * filter (rastertoniimbot) and the IPP Everywhere print command
 * (niimbot-ipp-print).
 *
 * The core knows nothing about the transport: the front-end supplies write /
 * flush / read callbacks. Fatal errors (printer error, protocol timeout with a
 * live back-channel, bad raster) send CancelPrint + PrintEnd and exit(1) after
 * printing a CUPS-style "ERROR:" line, which both cupsd and ippeveprinter
 * understand.
 *
 * Protocol details: docs/protocol-b4.md
 */

#ifndef NIIMBOT_H
#define NIIMBOT_H

#include <cups/raster.h>
#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct niimbot_io_s {
  void    *ctx;
  /* Write exactly len bytes (may buffer). Return 0 on success, -1 on error with errno set. */
  int      (*write)(void *ctx, const unsigned char *buf, size_t len);
  /* Push buffered output to the device. Return 0 on success, -1 on error. May be NULL. */
  int      (*flush)(void *ctx);
  /* Read up to len bytes, waiting at most timeout seconds.
   * Return bytes read, 0 on timeout, <0 if there is no back-channel at all. */
  ssize_t  (*read)(void *ctx, unsigned char *buf, size_t len, double timeout);
} niimbot_io_t;

typedef struct niimbot_job_s {
  int density;     /* 1..5, clamped */
  int label_type;  /* 1 gap, 2 black mark, 5 transparent; anything else -> 1 */
  int ack;         /* wait for printer acknowledgements on the back-channel */
  int verbose;     /* DEBUG: hex dumps of received packets */
  int dry_run;     /* never wait or sleep: output only (testing) */
  const char *media_out_reason; /* STATE keyword for out-of-labels; NULL = media-empty-error */
} niimbot_job_t;

/* Set by the SIGTERM handler installed in niimbot_init(); checked between packets. */
extern volatile sig_atomic_t niimbot_cancelled;

/* Initialise the core, install signal handlers. Must be called first. */
void niimbot_init(const niimbot_io_t *io, const niimbot_job_t *job);

/* Connect handshake. Decides whether the back-channel is usable. Never fatal. */
void niimbot_connect(void);

/*
 * Print one page from the raster stream. `copies` is passed to the printer
 * (SetPageSize / PrintStart), not emulated. Fatal on any error.
 * Emits "DEBUG:"/"INFO:" progress lines on stderr.
 */
void niimbot_print_page(cups_raster_t *ras, const cups_page_header2_t *hdr, unsigned page, unsigned copies);

/*
 * Send a request and wait up to `timeout` seconds for a reply with command
 * byte `resp`. Returns 1 with data/len filled on success, 0 on timeout,
 * unsupported command or printer error (see niimbot_last_error()).
 */
int niimbot_transceive(unsigned char cmd, const unsigned char *req, size_t reqlen,
                       unsigned char resp, unsigned char *data, size_t *len, double timeout);

/* Last 0xDB error code received from the printer (0 = none) and its name. */
int niimbot_last_error(void);
const char *niimbot_error_name(int code);

/* Print "ERROR: ..." (with printer error decoration), abort the job on the printer, exit(1). */
void niimbot_fail(const char *msg) __attribute__((noreturn));

#endif /* NIIMBOT_H */
