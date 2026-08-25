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

check: all tests/test_crc tests/test_zmdm tests/test_zmtx tests/test_zmrx \
	    tests/test_posix_io tests/test_posix_cleanup
	./tests/test_crc
	./tests/test_zmdm
	./tests/test_zmtx
	./tests/test_zmrx
	./tests/test_posix_io
	./tests/test_posix_cleanup
	$(PYTHON) tests/test_zmodem.py

check-install: all
	MAKE='$(MAKE)' sh tests/test_install.sh

check-link: all
	$(PYTHON) tests/test_link.py

check-static:
	$(PYTHON) tests/run_quality.py static

check-sanitize:
	$(PYTHON) tests/run_quality.py sanitize

check-fuzz:
	$(PYTHON) tests/run_quality.py fuzz

coverage:
	$(PYTHON) tests/run_quality.py coverage

quality: check check-install check-static check-sanitize check-fuzz coverage

tests/test_crc: tests/test_crc.c crctab.o crctab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_crc.c crctab.o $(LDFLAGS) $(LDLIBS) $(LIBS) \
	    -o tests/test_crc

tests/test_zmdm: tests/test_zmdm.c zmdm.o crctab.o zmdm.h zmodem.h crctab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_zmdm.c zmdm.o crctab.o $(LDFLAGS) $(LDLIBS) $(LIBS) \
	    -o tests/test_zmdm

tests/test_zmtx: tests/test_zmtx.c zmtx.c version.h zmdm.o zmdm_posix.o \
	    crctab.o zmdm.h zmdm_posix.h zmodem.h crctab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_zmtx.c zmdm.o zmdm_posix.o crctab.o \
	    $(LDFLAGS) $(LDLIBS) $(LIBS) -o tests/test_zmtx

tests/test_zmrx: tests/test_zmrx.c zmrx.c version.h zmdm.o zmdm_posix.o \
	    crctab.o zmdm.h zmdm_posix.h zmodem.h crctab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_zmrx.c zmdm.o zmdm_posix.o crctab.o \
	    $(LDFLAGS) $(LDLIBS) $(LIBS) -o tests/test_zmrx

tests/test_posix_io: tests/test_posix_io.c zmdm_posix.o zmdm_posix.h zmdm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_posix_io.c zmdm_posix.o $(LDFLAGS) $(LDLIBS) $(LIBS) \
	    -o tests/test_posix_io

tests/test_posix_cleanup: tests/test_posix_cleanup.c zmdm_posix.c \
	    zmdm_posix.h zmdm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. tests/test_posix_cleanup.c \
	    $(LDFLAGS) $(LDLIBS) $(LIBS) -o tests/test_posix_cleanup

zmtx:	zmtx.o zmdm.o zmdm_posix.o crctab.o
	$(CC) $(LDFLAGS) zmtx.o zmdm.o zmdm_posix.o crctab.o \
	    $(LDLIBS) $(LIBS) -o zmtx

zmrx:	zmrx.o zmdm.o zmdm_posix.o crctab.o
	$(CC) $(LDFLAGS) zmrx.o zmdm.o zmdm_posix.o crctab.o \
	    $(LDLIBS) $(LIBS) -o zmrx

zmtx.o:	zmtx.c version.h zmodem.h zmdm.h zmdm_posix.h
zmrx.o:	zmrx.c version.h zmodem.h zmdm.h zmdm_posix.h

zmdm.o:		zmdm.c zmodem.h zmdm.h crctab.h
zmdm_posix.o:	zmdm_posix.c zmdm_posix.h zmdm.h
crctab.o:	crctab.c crctab.h crctab_slicing.h

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
	rm -f *.o zmtx zmrx tests/test_crc tests/test_zmdm tests/test_zmtx \
	    tests/test_zmrx tests/test_posix_io tests/test_posix_cleanup

.PHONY: all check check-install check-link check-static check-sanitize \
	check-fuzz coverage quality install install-strip uninstall clean
