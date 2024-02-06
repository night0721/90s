CC=gcc

VERSION = 0.1
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

CFLAGS = -std=c99 -O0 -Wall -DVERSION=\"${VERSION}\"

SRC = rush.c color.c constants.h history.c commands.c
OBJ = ${SRC:.c=.o}

.c.o:
	${CC} -c ${CFLAGS} $<

rush: ${OBJ}
	${CC} -o $@ ${OBJ}

clean:
	rm -rf rush

dist:
	mkdir -p rush-${VERSION}
	cp -R LICENSE Makefile README.md rush.1 rush rush-${VERSION}
	tar -cf rush-${VERSION}.tar rush-${VERSION}
	gzip rush-${VERSION}.tar
	rm -rf rush-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f rush ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/rush
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/rush.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/rush\
		${DESTDIR}${MANPREFIX}/man1/rush.1
all: rush

.PHONY: all clean dist install uninstall rush
