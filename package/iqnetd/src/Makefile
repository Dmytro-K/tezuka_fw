# iqnetd - control daemon for iqnet zero-copy UDP IQ streaming.
# Buildroot passes CC/CFLAGS/LDFLAGS via TARGET_CONFIGURE_OPTS and the
# iqnet header directory (package/iqnet-kmod/src) via IQNET_INC; the default
# below only works when building in place from package/iqnetd/src.

IQNET_INC ?= ../../iqnet-kmod/src

CC ?= gcc
CFLAGS ?= -O2
# override: Buildroot passes CFLAGS on the make command line, which would
# otherwise discard these (and the iqnet header path with them).
override CFLAGS += -Wall -Wextra -I$(IQNET_INC)

PREFIX ?= /usr
DESTDIR ?=

all: iqnetd

iqnetd: iqnetd.c $(IQNET_INC)/iqnet_uapi.h $(IQNET_INC)/iqnet_proto.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ iqnetd.c

install: iqnetd
	install -D -m 0755 iqnetd $(DESTDIR)$(PREFIX)/sbin/iqnetd

clean:
	rm -f iqnetd

.PHONY: all install clean
