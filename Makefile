# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Laurent Vivier
CC = gcc
PPDC = ppdc
BUILD = build
ENGINE = encoder
PREFIX ?= /usr
CUPSFILTERDIR ?= $(PREFIX)/lib/cups/filter
PPDDIR ?= $(PREFIX)/share/cups/model/Brother
DATADIR ?= $(PREFIX)/share/mfcj5720dw
CPPFLAGS += -I$(ENGINE) -DDATADIR='"$(DATADIR)"'
CFLAGS = -std=gnu11 -O0 -g -Wall -Wextra -Werror \
	-Wno-deprecated-declarations -fPIE
ENGINE_CFLAGS = -mfpmath=387 -std=gnu11 -O0 -g -fPIE \
	-fno-strict-aliasing -fwrapv -fno-builtin \
	-Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
ENGINE_LINK = -pie
ENGINE_SOURCES = $(wildcard $(ENGINE)/*.c)
ENGINE_HEADERS = $(wildcard $(ENGINE)/*.h)
FILTER_HEADERS = driver.h raster.h ink.h calibration.h
ENGINE_OBJECTS = $(patsubst $(ENGINE)/%.c,$(BUILD)/engine/%.o,$(ENGINE_SOURCES))
FILTER_OBJECTS = $(addprefix $(BUILD)/,rastertomfcj5720dw.o raster.o settings.o paper.o ink.o ink_status.o calibration.o calibration_cache.o)
PPD = ppd/mfcj5720dw.ppd
PPD_LANGUAGES = en,fr
PPD_TRANSLATIONS = fr.po
MANPAGES = rastertomfcj5720dw.1 mfcj5720dw_ink.1 commandtomfcj5720dw.1
PROFILE_SOURCES = $(wildcard lut/*.xml)
BCM_PROFILES = $(patsubst %.xml,%.BCM,$(PROFILE_SOURCES))
RPMBUILD ?= rpmbuild
RPMSPEC ?= rpmspec
RPM_SPEC = mfcj5720dw-cups.spec
RPM_TOPDIR = $(abspath $(BUILD)/rpm)
RPM_SRPMDIR = $(RPM_TOPDIR)/SRPMS
DIST_FILES = .gitattributes .gitignore COPYRIGHT LICENSE PROVENANCE.md README.md \
	Makefile .copr/Makefile $(RPM_SPEC) $(PPD_TRANSLATIONS) $(wildcard *.c *.h *.1 *.drv) \
	$(wildcard encoder/*.c encoder/*.h) \
	lut/Makefile lut/.gitignore $(wildcard lut/*.py lut/*.xml lut/cubes/*.xml lut/curves/*.xml)

.PHONY: all profiles ppd check-static check-man check-encoder-interfaces install rpm srpm rpm-sources clean distclean
all: $(BUILD)/rastertomfcj5720dw $(BUILD)/mfcj5720dw_ink $(BUILD)/commandtomfcj5720dw $(PPD) profiles

profiles:
	$(MAKE) -C lut profiles

ppd: $(PPD)

$(PPD): mfcj5720dw.drv $(PPD_TRANSLATIONS) Makefile
	$(PPDC) --lf -l $(PPD_LANGUAGES) -d ppd $<

$(BUILD)/%.o: %.c $(FILTER_HEADERS) $(ENGINE_HEADERS) Makefile
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/engine/%.o: $(ENGINE)/%.c $(ENGINE_HEADERS) Makefile
	mkdir -p $(BUILD)/engine
	$(CC) $(CPPFLAGS) $(ENGINE_CFLAGS) -c $< -o $@

$(BUILD)/rastertomfcj5720dw: $(FILTER_OBJECTS) $(ENGINE_OBJECTS) Makefile
	$(CC) $(LDFLAGS) $(ENGINE_LINK) $(filter %.o,$^) -lcups -ljpeg -lm -o $@

$(BUILD)/mfcj5720dw_ink: $(BUILD)/mfcj5720dw_ink.o $(BUILD)/ink.o $(BUILD)/ink_status.o Makefile
	$(CC) $(LDFLAGS) -no-pie $(filter %.o,$^) -lcups -o $@

$(BUILD)/commandtomfcj5720dw: $(BUILD)/commandtomfcj5720dw.o $(BUILD)/ink.o $(BUILD)/ink_status.o Makefile
	$(CC) $(LDFLAGS) -pie $(filter %.o,$^) -lcups -o $@

# Compile interface checks and inspect build artifacts without running filters.
check-encoder-interfaces:
	@for header in $(wildcard $(ENGINE)/*.h); do \
		$(CC) $(CPPFLAGS) $(ENGINE_CFLAGS) -x c -fsyntax-only \
			-include "$$header" -include "$$header" /dev/null || exit $$?; \
	done

check-static: all check-man check-encoder-interfaces
	cupstestppd -I filters $(PPD)
	file $(BUILD)/rastertomfcj5720dw $(BUILD)/mfcj5720dw_ink $(BUILD)/commandtomfcj5720dw
	readelf -h $(BUILD)/rastertomfcj5720dw
	readelf -d $(BUILD)/rastertomfcj5720dw

check-man:
	groff -ww -Tutf8 -man rastertomfcj5720dw.1 > /dev/null
	groff -ww -Tutf8 -man mfcj5720dw_ink.1 > /dev/null
	groff -ww -Tutf8 -man commandtomfcj5720dw.1 > /dev/null

install: all
	install -Dm755 $(BUILD)/rastertomfcj5720dw $(DESTDIR)$(CUPSFILTERDIR)/rastertomfcj5720dw
	install -Dm755 $(BUILD)/mfcj5720dw_ink $(DESTDIR)$(CUPSFILTERDIR)/mfcj5720dw_ink
	install -Dm755 $(BUILD)/commandtomfcj5720dw $(DESTDIR)$(CUPSFILTERDIR)/commandtomfcj5720dw
	install -Dm644 $(PPD) $(DESTDIR)$(PPDDIR)/$(notdir $(PPD))
	install -d $(DESTDIR)$(DATADIR)/lut $(DESTDIR)$(PREFIX)/share/man/man1
	install -m644 $(BCM_PROFILES) $(DESTDIR)$(DATADIR)/lut/
	install -m644 $(MANPAGES) $(DESTDIR)$(PREFIX)/share/man/man1/

rpm-sources:
	mkdir -p "$(RPM_TOPDIR)/SOURCES" "$(RPM_TOPDIR)/SPECS"
	set -e; \
	package=$$($(RPMSPEC) -q --srpm --qf '%{NAME}-%{VERSION}' $(RPM_SPEC)); \
	tar --sort=name --owner=0 --group=0 --numeric-owner \
		--transform="s,^,$$package/," \
		-czf "$(RPM_TOPDIR)/SOURCES/$$package.tar.gz" $(DIST_FILES)
	install -m644 $(RPM_SPEC) "$(RPM_TOPDIR)/SPECS/$(RPM_SPEC)"

rpm: rpm-sources
	+$(RPMBUILD) -ba --define '_topdir $(RPM_TOPDIR)' \
		--define '_srcrpmdir $(RPM_SRPMDIR)' "$(RPM_TOPDIR)/SPECS/$(RPM_SPEC)"

srpm: rpm-sources
	$(RPMBUILD) -bs --define '_topdir $(RPM_TOPDIR)' \
		--define '_srcrpmdir $(RPM_SRPMDIR)' "$(RPM_TOPDIR)/SPECS/$(RPM_SPEC)"

clean:
	$(RM) -r -- "$(BUILD)" ppd
	$(MAKE) -C lut clean

distclean: clean
	$(RM) -r -- lut/__pycache__
