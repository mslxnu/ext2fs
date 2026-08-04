#
# fsck_ext2fs - ext2 file system consistency checker
#
# Converted from the OpenBSD bsd.prog.mk build. Upstream reached fsutil.c
# through .PATH from a shared fsck directory whose other contents - the
# standalone fsck(8) driver - this tree has no use for, macOS having its own,
# so fsutil.c simply lives here now. Upstream also pulled in ext2fs_bswap.c;
# that is unnecessary because <fs/ext2fs/ext2fs.h> and ext2_compat.h already
# resolve the byte order macros for little-endian hosts.
#
# ../compat is the one thing still reached across directories: bsdcompat.c is
# generic libc gap-filling rather than anything to do with checking a file
# system, so it is not buried in here.
#

include ../Makefile.inc

PROG=	fsck_ext2fs
MAN=	fsck_ext2fs.8

SRCS=	dir.c inode.c main.c pass1.c pass1b.c pass2.c pass3.c pass4.c \
	pass5.c setup.c utilities.c fsutil.c \
	../compat/bsdcompat.c

OBJS=	$(SRCS:.c=.o)

CFLAGS+= -I. -I../compat
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
