#
# All-in-one Makefile
#
# All three are submodules carried over from the procfs project. Only libkext
# is actually used: kext/ext2fs.c calls libkext_vma_uuid() and
# libkext_massert(). libsbuf and libbsdmalloc contribute no code to the linked
# kext - nothing in kext/ references sbuf_* or the bsdmalloc allocator - so the
# -lsbuf and -lbsdmalloc in kext/Makefile are inert. They are still built and
# linked rather than dropped, because removing them means deinitialising the
# submodules, which is a separate decision.
#

MAKE=make

all: debug

debug:
	$(MAKE) -C libbsdmalloc
	$(MAKE) -C libsbuf
	$(MAKE) -C libkext

release: TARGET=release
release: debug

clean:
	$(MAKE) -C libbsdmalloc clean
	$(MAKE) -C libsbuf clean
	$(MAKE) -C libkext clean

.PHONY: all debug release clean
