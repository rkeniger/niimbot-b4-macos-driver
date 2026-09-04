# NIIMBOT B4 macOS driver
#
#   make            build filter + PPD into build/
#   sudo make install     install into /Library/Printers/NIIMBOT and create the queue
#   sudo make uninstall   remove queue and files
#   make test-raster      render a test PDF to CUPS raster and run the filter offline

PREFIX      ?= /Library/Printers/NIIMBOT
QUEUE       ?= NIIMBOT_B4
CC          ?= clang
CFLAGS      ?= -O2 -Wall -Wextra -Wno-deprecated-declarations
LDLIBS       = -lcups
BUILD        = build
FILTER       = $(BUILD)/rastertoniimbot
PPD          = $(BUILD)/niimbot-b4.ppd

all: $(FILTER) $(PPD)

$(BUILD):
	mkdir -p $(BUILD)

$(FILTER): filter/rastertoniimbot.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(PPD): ppd/niimbot-b4.drv | $(BUILD)
	ppdc -d $(BUILD) $<
	-cupstestppd -W all $(PPD) | grep -vE 'Adobe standard name|PCFileName|REF:'

install: all
	install -d -m 755 $(PREFIX)
	install -m 755 $(FILTER) $(PREFIX)/rastertoniimbot
	install -m 644 $(PPD) $(PREFIX)/niimbot-b4.ppd
	@uri=$$(lpinfo -v | awk '/usb:\/\/NIIMBOT\/B4/ {print $$2; exit}'); \
	if [ -z "$$uri" ]; then echo "NIIMBOT B4 not found on USB; plug it in and run: lpadmin -p $(QUEUE) -E -v 'usb://NIIMBOT/B4?serial=...' -P $(PREFIX)/niimbot-b4.ppd"; exit 1; fi; \
	echo "creating queue $(QUEUE) -> $$uri"; \
	lpadmin -p $(QUEUE) -E -v "$$uri" -P $(PREFIX)/niimbot-b4.ppd -D "NIIMBOT B4" -o printer-is-shared=false && \
	cupsenable $(QUEUE) && cupsaccept $(QUEUE) && lpstat -p $(QUEUE)

uninstall:
	-lpadmin -x $(QUEUE)
	rm -rf $(PREFIX)

# Offline check: PDF -> CUPS raster -> filter -> hexdump of the first packets.
test-raster: all $(BUILD)/test.pdf
	PPD=$(PPD) cupsfilter -p $(PPD) -m application/vnd.cups-raster -o niimbotDensity=3 $(BUILD)/test.pdf > $(BUILD)/test.ras
	PPD=$(PPD) $(FILTER) 1 rob test 1 "" $(BUILD)/test.ras 3>/dev/null | head -c 64 | xxd

$(BUILD)/test.pdf: | $(BUILD)
	cupsfilter -m application/pdf README.md > $@

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall test-raster clean
