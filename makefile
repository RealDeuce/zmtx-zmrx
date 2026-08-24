CFLAGS := $(CFLAGS) -std=c99 -D_POSIX_C_SOURCE=200112L -D_FILE_OFFSET_BITS=64

all:	zmtx zmrx

check: all
	python3 tests/test_zmodem.py

zmtx:	zmtx.o zmdm.o crctab.o
	$(CC) $(CFLAGS) $(OFLAG) zmtx.o zmdm.o crctab.o -o zmtx

zmrx:	zmrx.o zmdm.o crctab.o
	$(CC) $(CFLAGS) $(OFLAG) zmrx.o zmdm.o crctab.o -o zmrx

zmtx.o:		zmtx.c version.h zmodem.h zmdm.h opts.h
zmrx.o:		zmrx.c version.h zmodem.h zmdm.h opts.h

zmdm.o:		zmdm.c zmodem.h zmdm.h crctab.h
crctab.o:	crctab.c crctab.h

clean:
	rm *.o
	rm zmtx zmrx

.PHONY: all check clean
