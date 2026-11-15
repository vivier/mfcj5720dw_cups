# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Laurent Vivier
PYTHON ?= python3
DESCRIPTIONS = $(wildcard *.xml)
TABLE_SOURCES = $(wildcard cubes/*.xml curves/*.xml)
PROFILES = $(patsubst %.xml,%.BCM,$(DESCRIPTIONS))

.PHONY: all profiles clean distclean
all: profiles

profiles: $(PROFILES)

%.BCM: %.xml $(TABLE_SOURCES) cubes curves bcm_from_text.py bcm_text.py bcm_xml.py Makefile
	$(PYTHON) bcm_from_text.py $< $@

clean:
	$(RM) $(PROFILES)

distclean: clean
	$(RM) -r __pycache__
