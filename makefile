REDUCED_MEMORY ?= 0
ZMODEM_PLATFORM ?= posix
PLATFORM_CFLAGS_posix = -std=c99
PLATFORM_CFLAGS = $(PLATFORM_CFLAGS_$(ZMODEM_PLATFORM))
CFLAGS += $(PLATFORM_CFLAGS)
PLATFORM_PRELUDE = $(ZMODEM_PLATFORM)/plat.h
PLATFORM_HEADER = $(ZMODEM_PLATFORM)/zmodem_plat.h
PLATFORM_SOURCE = $(ZMODEM_PLATFORM)/zmodem_plat.c
CPPFLAGS += -I$(ZMODEM_PLATFORM) -I. -DREDUCED_MEMORY=$(REDUCED_MEMORY)

ZCC ?= zcc
CPM_DRIVER ?= cpm/rdrpun.c
CPM_STREAMING ?= 0
CPM_CPPFLAGS ?=
CPM_CFLAGS ?=
CPM_COMMON_SOURCES = zmdm.c crctab.c cpm/zmodem_plat.c $(CPM_DRIVER)
CPM_HEADERS = version.h zmodem.h zmdm.h crctab.h cpm/plat.h \
	cpm/inttypes.h cpm/zmodem_plat.h cpm/zmodem_cpm_driver.h

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
TNYLPO ?= tnylpo

all:	zmtx zmrx

cpm: zmtx.com zmrx.com

zmtx.com: zmtx.c $(CPM_COMMON_SOURCES) $(CPM_HEADERS)
	$(ZCC) +cpm -Icpm -I. -DREDUCED_MEMORY=1 \
	    -DZMODEM_CPM_STREAMING=$(CPM_STREAMING) $(CPM_CPPFLAGS) \
	    $(CPM_CFLAGS) zmtx.c $(CPM_COMMON_SOURCES) -o $@

zmrx.com: zmrx.c $(CPM_COMMON_SOURCES) $(CPM_HEADERS)
	$(ZCC) +cpm -Icpm -I. -DREDUCED_MEMORY=1 \
	    -DZMODEM_CPM_STREAMING=$(CPM_STREAMING) $(CPM_CPPFLAGS) \
	    $(CPM_CFLAGS) zmrx.c $(CPM_COMMON_SOURCES) -o $@

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

check-cpm: all cpm
	TNYLPO='$(TNYLPO)' $(PYTHON) tests/test_cpm.py

check-static:
	$(PYTHON) tests/run_quality.py static

check-sanitize:
	$(PYTHON) tests/run_quality.py sanitize

check-fuzz:
	$(PYTHON) tests/run_quality.py fuzz

check-reduced:
	$(PYTHON) tests/run_quality.py reduced

coverage:
	$(PYTHON) tests/run_quality.py coverage

quality: check check-install check-static check-sanitize check-fuzz \
	check-reduced coverage

tests/test_crc: tests/test_crc.c crctab.o crctab.h $(PLATFORM_PRELUDE)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_crc.c crctab.o $(LDFLAGS) $(LDLIBS) $(LIBS) \
	    -o tests/test_crc

tests/test_zmdm: tests/test_zmdm.c zmdm.o crctab.o zmdm.h zmodem.h crctab.h \
	    $(PLATFORM_PRELUDE)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_zmdm.c zmdm.o crctab.o $(LDFLAGS) $(LDLIBS) $(LIBS) \
	    -o tests/test_zmdm

tests/test_zmtx: tests/test_zmtx.c zmtx.c version.h zmdm.o plat.o \
	    crctab.o zmdm.h $(PLATFORM_HEADER) $(PLATFORM_PRELUDE) zmodem.h \
	    crctab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_zmtx.c zmdm.o plat.o crctab.o \
	    $(LDFLAGS) $(LDLIBS) $(LIBS) -o tests/test_zmtx

tests/test_zmrx: tests/test_zmrx.c zmrx.c version.h zmdm.o plat.o \
	    crctab.o zmdm.h $(PLATFORM_HEADER) $(PLATFORM_PRELUDE) zmodem.h \
	    crctab.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_zmrx.c zmdm.o plat.o crctab.o \
	    $(LDFLAGS) $(LDLIBS) $(LIBS) -o tests/test_zmrx

tests/test_posix_io: tests/test_posix_io.c plat.o $(PLATFORM_HEADER) \
	    $(PLATFORM_PRELUDE) zmdm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. \
	    tests/test_posix_io.c plat.o $(LDFLAGS) $(LDLIBS) $(LIBS) \
	    -o tests/test_posix_io

tests/test_posix_cleanup: tests/test_posix_cleanup.c $(PLATFORM_SOURCE) \
	    $(PLATFORM_HEADER) $(PLATFORM_PRELUDE) zmdm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. tests/test_posix_cleanup.c \
	    $(LDFLAGS) $(LDLIBS) $(LIBS) -o tests/test_posix_cleanup

zmtx:	zmtx.o zmdm.o plat.o crctab.o
	$(CC) $(LDFLAGS) zmtx.o zmdm.o plat.o crctab.o \
	    $(LDLIBS) $(LIBS) -o zmtx

zmrx:	zmrx.o zmdm.o plat.o crctab.o
	$(CC) $(LDFLAGS) zmrx.o zmdm.o plat.o crctab.o \
	    $(LDLIBS) $(LIBS) -o zmrx

zmtx.o:	zmtx.c version.h zmodem.h zmdm.h $(PLATFORM_HEADER) \
	    $(PLATFORM_PRELUDE)
zmrx.o:	zmrx.c version.h zmodem.h zmdm.h $(PLATFORM_HEADER) \
	    $(PLATFORM_PRELUDE)

zmdm.o:		zmdm.c zmodem.h zmdm.h crctab.h $(PLATFORM_PRELUDE)
plat.o:	$(PLATFORM_SOURCE) $(PLATFORM_HEADER) $(PLATFORM_PRELUDE) zmdm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $(PLATFORM_SOURCE) -o $@
crctab.o:	crctab.c crctab.h crctab_slicing.h $(PLATFORM_PRELUDE)

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
	rm -f *.o zmtx zmrx zmtx.com zmrx.com ZMTX.COM ZMRX.COM \
	    tests/test_crc tests/test_zmdm tests/test_zmtx \
	    tests/test_zmrx tests/test_posix_io tests/test_posix_cleanup

.PHONY: all check check-install check-link check-static check-sanitize \
	check-fuzz check-reduced coverage quality install install-strip uninstall \
	clean cpm check-cpm zmtx.com zmrx.com
