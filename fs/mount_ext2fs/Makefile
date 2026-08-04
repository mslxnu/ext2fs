#
# mount_ext2fs - mount an ext2 file system
#
# Converted from the OpenBSD bsd.prog.mk build. Upstream pulled getmntopts.c in
# from ../mount through .PATH; that is unnecessary here because the option
# parsing lives in mount_ext2fs.c itself and the mntopts.h it needs comes from
# vendor/libutil, which ../Makefile.inc already puts on the include path.
#
# Unlike its three neighbours this program is not installed into $(PREFIX):
# diskarbitrationd execs it out of the bundle's Contents/Resources, named by
# FSMountExecutable in Info.plist, and nothing invokes it by hand. ../Makefile
# is what copies it there.
#

include ../Makefile.inc

PROG=	mount_ext2fs
MAN=	mount_ext2fs.8

SRCS=	mount_ext2fs.c

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

# Only the man page: the program itself belongs in the bundle, not in sbin.
install: $(PROG)
	install -d "$(MANPREFIX)/man8"
	install -m 644 $(MAN) "$(MANPREFIX)/man8/$(MAN)"

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: all debug release install clean
