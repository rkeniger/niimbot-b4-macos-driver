# NIIMBOT B4 macOS driver
#
#   make                  build filter, PPD, IPP command and LaunchAgent plist into build/
#   sudo make install     legacy CUPS driver: install into /Library/Printers/NIIMBOT, create queue NIIMBOT_B4
#   sudo make uninstall   remove that queue and the filter/PPD
#   sudo make install-ipp IPP Everywhere / AirPrint: install command + attributes, start the
#                         ippeveprinter LaunchAgent, create queue NIIMBOT_B4_IPP
#   sudo make uninstall-ipp
#   make test-raster      render a test PDF to CUPS raster and run the filter offline
#   make test-ipp         run the IPP command offline against a PWG raster (dry run)
#
# Variables: PREFIX, QUEUE, IPP_QUEUE, IPP_PORT, IPP_NAME, IPP_HOST (set to
# "localhost" to keep the IPP printer off the LAN: no AirPrint from phones).

PREFIX      ?= /Library/Printers/NIIMBOT
QUEUE       ?= NIIMBOT_B4
IPP_QUEUE   ?= NIIMBOT_B4_IPP
IPP_PORT    ?= 8631
IPP_NAME    ?= NIIMBOT B4
IPP_HOST    ?=
AGENT        = local.niimbot.b4-ipp
AGENT_DIR    = /Library/LaunchAgents
# launchd user session to load the agent into (works under sudo too)
GUI_UID      = $(if $(SUDO_UID),$(SUDO_UID),$(shell id -u))
CC          ?= clang
CFLAGS      ?= -O2 -Wall -Wextra -Wno-deprecated-declarations
LDLIBS       = -lcups
BUILD        = build
FILTER       = $(BUILD)/rastertoniimbot
PPD          = $(BUILD)/niimbot-b4.ppd
IPPCMD       = $(BUILD)/niimbot-ipp-print
IPPEVE       = $(BUILD)/ippeveprinter
PLIST        = $(BUILD)/$(AGENT).plist
CORE         = filter/niimbot.c filter/niimbot.h
HOSTARGS     = $(if $(IPP_HOST),<string>-n</string><string>$(IPP_HOST)</string>,)

all: $(FILTER) $(PPD) $(IPPCMD) $(IPPEVE) $(PLIST)

$(BUILD):
	mkdir -p $(BUILD)

$(FILTER): filter/rastertoniimbot.c $(CORE) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ filter/rastertoniimbot.c filter/niimbot.c $(LDLIBS)

$(IPPCMD): filter/niimbot-ipp-print.c $(CORE) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ filter/niimbot-ipp-print.c filter/niimbot.c $(LDLIBS)

# Apple's CUPS 2.3.6 ippeveprinter (see ippeve/README.md): the 2.3.4 one in
# /usr/bin rejects chunked Create-Job requests, which breaks AirPrint from iOS.
$(IPPEVE): ippeve/ippeveprinter.c ippeve/config.h | $(BUILD)
	$(CC) -O2 -Wno-deprecated-declarations -Iippeve -o $@ ippeve/ippeveprinter.c $(LDLIBS) -lz -lpam

$(PLIST): ipp/$(AGENT).plist.in Makefile | $(BUILD)
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@PORT@|$(IPP_PORT)|g' -e 's|@NAME@|$(IPP_NAME)|g' \
	    -e 's|@HOSTARGS@|$(HOSTARGS)|g' $< > $@
	plutil -lint $@

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
	rm -f $(PREFIX)/rastertoniimbot $(PREFIX)/niimbot-b4.ppd
	-rmdir $(PREFIX) 2>/dev/null

# ---- IPP Everywhere / AirPrint ---------------------------------------
install-ipp: $(IPPCMD) $(IPPEVE) $(PLIST)
	install -d -m 755 $(PREFIX)
	install -m 755 $(IPPCMD) $(PREFIX)/niimbot-ipp-print
	install -m 755 $(IPPEVE) $(PREFIX)/ippeveprinter
	install -m 644 ipp/niimbot-b4.conf $(PREFIX)/niimbot-b4.conf
	install -m 644 $(PLIST) $(AGENT_DIR)/$(AGENT).plist
	-launchctl bootout gui/$(GUI_UID)/$(AGENT) 2>/dev/null
	launchctl bootstrap gui/$(GUI_UID) $(AGENT_DIR)/$(AGENT).plist
	@echo "waiting for ippeveprinter on port $(IPP_PORT)"; \
	for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do \
	  ipptool -t ipp://localhost:$(IPP_PORT)/ipp/print get-printer-attributes.test >/dev/null 2>&1 && break; sleep 0.5; done
	lpadmin -p $(IPP_QUEUE) -E -v ipp://localhost:$(IPP_PORT)/ipp/print -m everywhere \
	        -D "$(IPP_NAME)" -o printer-is-shared=false
	cupsenable $(IPP_QUEUE) && cupsaccept $(IPP_QUEUE) && lpstat -p $(IPP_QUEUE)
	@echo "Log: /tmp/niimbot-ipp.log   Web: http://localhost:$(IPP_PORT)/"

uninstall-ipp:
	-lpadmin -x $(IPP_QUEUE)
	-launchctl bootout gui/$(GUI_UID)/$(AGENT) 2>/dev/null
	rm -f $(AGENT_DIR)/$(AGENT).plist
	rm -f $(PREFIX)/niimbot-ipp-print $(PREFIX)/ippeveprinter $(PREFIX)/niimbot-b4.conf
	-rmdir $(PREFIX) 2>/dev/null

# Offline check: PDF -> CUPS raster -> filter -> hexdump of the first packets.
test-raster: all $(BUILD)/test.pdf
	PPD=$(PPD) cupsfilter -p $(PPD) -m application/vnd.cups-raster -o niimbotDensity=3 $(BUILD)/test.pdf > $(BUILD)/test.ras
	PPD=$(PPD) $(FILTER) 1 rob test 1 "" $(BUILD)/test.ras 3>/dev/null | head -c 64 | xxd

# Offline check of the IPP command: PDF -> PWG raster -> command (dry run to a file).
test-ipp: $(IPPCMD) $(PPD) $(BUILD)/test.pdf
	PPD=$(PPD) cupsfilter -p $(PPD) -m image/pwg-raster $(BUILD)/test.pdf > $(BUILD)/test.pwg
	CONTENT_TYPE=image/pwg-raster NIIMBOT_DEVICE=file:$(BUILD)/test-ipp.bin IPP_COPIES=1 \
	  $(IPPCMD) $(BUILD)/test.pwg && head -c 64 $(BUILD)/test-ipp.bin | xxd

$(BUILD)/test.pdf: | $(BUILD)
	cupsfilter -m application/pdf README.md > $@

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall install-ipp uninstall-ipp test-raster test-ipp clean
