#
# newfs_ext2fs - create an ext2 file system
#
# Converted from the OpenBSD bsd.prog.mk build. Upstream pulled ext2fs_bswap.c
# in through .PATH; that is unnecessary here because <fs/ext2fs/ext2fs.h> and
# ext2_compat.h already resolve the byte order macros for little-endian hosts.
#

include ../Makefile.inc

PROG=	newfs_ext2fs
MAN=	newfs_ext2fs.8

SRCS=	newfs_ext2fs.c mke2fs.c

OBJS=	$(SRCS:.c=.o)

CFLAGS+= -I.
LIBS+=   -lutil

all: debug

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

debug: $(PROG)
release: $(PROG)

# Only the man page. The program itself is installed by the top-level Makefile,
# which puts the one copy inside the bundle and symlinks $(PREFIX) at it;
# installing a second copy here would be the drift that arrangement avoids.
install: $(PROG)
	install -d "$(MANPREFIX)/man8"
	install -m 644 $(MAN) "$(MANPREFIX)/man8/$(MAN)"

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: all debug release install clean
