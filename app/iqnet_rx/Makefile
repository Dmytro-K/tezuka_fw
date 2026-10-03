# iqnet_rx - host-side test receiver for the iqnet UDP IQ stream.
# Build on the host (Linux x86_64):  make
PROTO_DIR ?= ../../package/iqnet-kmod/src

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 -Wall -Wextra -I$(PROTO_DIR)
LDFLAGS ?=
PREFIX  ?= /usr/local

TARGET = iqnet_rx

all: $(TARGET)

$(TARGET): $(TARGET).c $(PROTO_DIR)/iqnet_proto.h
	$(CC) $(CFLAGS) $(TARGET).c $(LDFLAGS) -o $@

install: $(TARGET)
	install -d -m 0755 $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	$(RM) $(TARGET)

.PHONY: all install clean
