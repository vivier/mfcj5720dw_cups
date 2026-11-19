CC = gcc
BUILD ?= ../build/monitor
PREFIX ?= /usr
LOCALEDIR ?= $(PREFIX)/share/locale
PKG_CONFIG ?= pkg-config
GTK_CFLAGS = $(shell $(PKG_CONFIG) --cflags gtk4)
GTK_LIBS = $(shell $(PKG_CONFIG) --libs gtk4)
SNMP_CFLAGS = $(shell $(PKG_CONFIG) --cflags netsnmp)
SNMP_LIBS = $(shell $(PKG_CONFIG) --libs netsnmp)
CFLAGS = -std=gnu11 -O2 -g -fPIE -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"'
OBJECTS = $(addprefix $(BUILD)/,main.o poll.o status.o ink.o)
APP_ID = io.github.vivier.mfcj5720dw.monitor

.PHONY: all install check clean distclean
all: $(BUILD)/mfcj5720dw-monitor $(BUILD)/fr.mo

$(BUILD)/%.o: %.c status.h poll.h Makefile
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) $(SNMP_CFLAGS) -c $< -o $@

$(BUILD)/ink.o: ../ink.c ../ink.h Makefile
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/mfcj5720dw-monitor: $(OBJECTS)
	$(CC) $(LDFLAGS) -pie $^ $(GTK_LIBS) $(SNMP_LIBS) -lcups -o $@

$(BUILD)/fr.mo: fr.po
	mkdir -p $(BUILD)
	msgfmt --check $< -o $@

install: all
	install -Dm755 $(BUILD)/mfcj5720dw-monitor $(DESTDIR)$(PREFIX)/bin/mfcj5720dw-monitor
	install -Dm644 $(APP_ID).desktop $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop
	install -Dm644 mfcj5720dw-monitor.1 $(DESTDIR)$(PREFIX)/share/man/man1/mfcj5720dw-monitor.1
	install -Dm644 $(BUILD)/fr.mo $(DESTDIR)$(LOCALEDIR)/fr/LC_MESSAGES/mfcj5720dw-monitor.mo

check:
	groff -ww -Tutf8 -man mfcj5720dw-monitor.1 > /dev/null
	desktop-file-validate $(APP_ID).desktop
	msgfmt --check fr.po -o /dev/null

clean:
	$(RM) -r -- $(BUILD)

distclean: clean
