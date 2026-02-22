# SPDX-License-Identifier: GPL-2.0
#
# tools/dte/Makefile - Developer Terminal Environment
#
# Build:    make -C tools/dte
# Install:  make -C tools/dte install
#
# Dependencies: ncurses, pthreads

CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 \
	   -I../../include \
	   -D_GNU_SOURCE \
	   -Wno-unused-parameter
LDFLAGS := -lncurses -lpthread

PREFIX  ?= /usr
BINDIR  := $(PREFIX)/bin

TARGET  := dte
SRCS    := dte.c window.c chat.c
OBJS    := $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

install-rootfs: $(TARGET)
	install -d ../../rootfs/usr/bin
	install -m 755 $(TARGET) ../../rootfs/usr/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all install install-rootfs clean
