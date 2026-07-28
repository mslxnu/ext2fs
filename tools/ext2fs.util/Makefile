#
# ext2fs.util - diskarbitrationd probe helper
#
# Installed into the ext2fs.fs bundle rather than into sbin: the bundle's
# FSMediaTypes entries reference it as ../../ext2fs.util, relative to each
# personality directory.
#

include ../Makefile.inc

PROG=	ext2fs.util

SRCS=	ext2fs.util.c

OBJS=	$(SRCS:.c=.o)

all: debug

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

debug: $(PROG)
release: $(PROG)

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: all debug release clean
