# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Laurent Vivier

source_dir := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
outdir ?= $(source_dir)/build/copr

.PHONY: srpm
srpm:
	$(MAKE) -C "$(source_dir)" srpm RPM_SRPMDIR="$$(realpath -m -- "$(outdir)")"
