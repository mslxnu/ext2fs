#
# Kernel-side static libraries linked into the kext.
#
# libkext is the only one left. It is a submodule carried over from the procfs
# project, and it is genuinely used: kext/ext2fs.c calls libkext_vma_uuid() and
# libkext_massert().
#
# libsbuf went because nothing here ever referenced sbuf_*. libbsdmalloc was
# load-bearing despite appearances - it shadowed <sys/malloc.h> to add the
# FreeBSD malloc(size, type, flags) / free(addr, type) spelling, and supplied
# those two functions from a static library. include/bsdcompat/sys/malloc.h
# now does the same job with inline wrappers over _MALLOC()/_FREE(), which are
# exported BSD KPI, so no library is needed.
#

MAKE=make

SUBDIRS=	libkext

all: debug

debug:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d || exit 1; done

release: TARGET=release
release: debug

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean || exit 1; done

.PHONY: all debug release clean
