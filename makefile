CFLAGS += -std=c99
CPPFLAGS += -D_POSIX_C_SOURCE=200112L -D_FILE_OFFSET_BITS=64

prefix ?= /usr/local
PREFIX ?= $(prefix)
exec_prefix ?= $(PREFIX)
bindir ?= $(exec_prefix)/bin
mandir ?= $(PREFIX)/share/man
BINDIR ?= $(bindir)
MANDIR ?= $(mandir)

INSTALL ?= install
BSD_INSTALL_PROGRAM ?= $(INSTALL) -m 0555
BSD_INSTALL_MAN ?= $(INSTALL) -m 0444
INSTALL_PROGRAM ?= $(BSD_INSTALL_PROGRAM)
INSTALL_DATA ?= $(BSD_INSTALL_MAN)
MKDIR_P ?= mkdir -p
MKDIR ?= $(MKDIR_P)
PYTHON ?= python3

all:	zmtx zmrx

check: all tests/test_crc
	./tests/test_crc
	$(PYTHON) tests/test_zmodem.py

check-install: all
	MAKE='$(MAKE)' sh tests/test_install.sh

tests/test_crc: tests/test_crc.c crctab.o crctab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_crc.c crctab.o $(LDFLAGS) $(LDLIBS) $(LIBS) \
	    -o tests/test_crc

zmtx:	zmtx.o zmdm.o crctab.o
	$(CC) $(LDFLAGS) zmtx.o zmdm.o crctab.o $(LDLIBS) $(LIBS) -o zmtx

zmrx:	zmrx.o zmdm.o crctab.o
	$(CC) $(LDFLAGS) zmrx.o zmdm.o crctab.o $(LDLIBS) $(LIBS) -o zmrx

zmtx.o:	zmtx.c version.h zmodem.h zmdm.h opts.h
zmrx.o:	zmrx.c version.h zmodem.h zmdm.h opts.h

zmdm.o:		zmdm.c zmodem.h zmdm.h crctab.h
crctab.o:	crctab.c crctab.h

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

install: all
	$(MKDIR) "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)/man1"
	$(INSTALL_PROGRAM) zmtx zmrx "$(DESTDIR)$(BINDIR)"
	$(INSTALL_DATA) zmtx.1 zmrx.1 "$(DESTDIR)$(MANDIR)/man1"

install-strip:
	$(MAKE) INSTALL_PROGRAM='$(INSTALL_PROGRAM) -s' install

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/zmtx" "$(DESTDIR)$(BINDIR)/zmrx" \
	    "$(DESTDIR)$(MANDIR)/man1/zmtx.1" \
	    "$(DESTDIR)$(MANDIR)/man1/zmrx.1"

clean:
	rm -f *.o zmtx zmrx tests/test_crc

.PHONY: all check check-install install install-strip uninstall clean
