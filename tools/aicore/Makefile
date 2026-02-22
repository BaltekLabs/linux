# SPDX-License-Identifier: GPL-2.0
#
# tools/aicore/Makefile - Linux AI Core Daemon
#
# Build:    make -C tools/aicore
# Install:  make -C tools/aicore install
#
# Dependencies: libcurl, pthreads
# Optional:     libssl (for TLS via curl)

CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -Wpedantic -std=c11 \
	   -I../../include \
	   -D_GNU_SOURCE \
	   -Wno-unused-parameter
LDFLAGS := -lcurl -lpthread

PREFIX  ?= /usr
BINDIR  := $(PREFIX)/sbin
CONFDIR := /etc/aicore
UNITDIR := /lib/systemd/system

TARGET  := aicore
SRCS    := aicore.c config.c tools.c llm_api.c
OBJS    := $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(CONFDIR)
	test -f $(DESTDIR)$(CONFDIR)/config.json || \
		install -m 640 config.json.example $(DESTDIR)$(CONFDIR)/config.json
	install -d $(DESTDIR)$(UNITDIR)
	install -m 644 aicore.service $(DESTDIR)$(UNITDIR)/

install-rootfs: $(TARGET)
	install -d ../../rootfs/usr/sbin
	install -m 755 $(TARGET) ../../rootfs/usr/sbin/$(TARGET)
	install -d ../../rootfs/etc/aicore
	test -f ../../rootfs/etc/aicore/config.json || \
		install -m 640 config.json.example ../../rootfs/etc/aicore/config.json

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all install install-rootfs clean
