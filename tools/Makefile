# SPDX-License-Identifier: GPL-2.0-only
#
# CoFDP user-space tools (see README.md).
#
#   make                          build all tools
#   make fp-remap pidirfs-mount   build only the tools that need no xNVMe
#   make install [PREFIX=/usr/local] [DESTDIR=...]
#   make clean
#
# fdp-reset and fdp-ruh-status need xNVMe (tested with 0.7.4), found with
# pkg-config, or set XNVME_CFLAGS and XNVME_LIBS.

CFLAGS ?= -O2 -g -Wall -Wextra
PKG_CONFIG ?= pkg-config
XNVME_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags xnvme 2>/dev/null)
XNVME_LIBS ?= $(shell $(PKG_CONFIG) --libs xnvme 2>/dev/null || echo -lxnvme)

PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin

TOOLS := fp-remap pidirfs-mount fdp-reset fdp-ruh-status

all: $(TOOLS)

fp-remap pidirfs-mount: %: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

fdp-reset fdp-ruh-status: %: %.c fdp-xnvme.c fdp-xnvme.h
	$(CC) $(CPPFLAGS) $(XNVME_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
		$< fdp-xnvme.c $(XNVME_LIBS) $(LDLIBS)

install: $(TOOLS)
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 $(TOOLS) $(DESTDIR)$(SBINDIR)

clean:
	$(RM) $(TOOLS)

.PHONY: all install clean
