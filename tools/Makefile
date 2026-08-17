#
# Optional third-party tools.
#
# Nothing here is part of the file system itself. The four programs that are -
# mount_ext2fs, newfs_ext2fs, fsck_ext2fs and ext2fs.util - live under ../fs,
# next to the bundle they are installed into, along with the shared fsck/ and
# util/ sources that fsck_ext2fs builds against.
#
# What is left are upstream projects that have not been ported to macOS. They
# are named in UNPORTED rather than silently forgotten, and SUBDIRS is empty, so
# every target here is a successful no-op until one of them is ported.
#

MAKE=make

SUBDIRS=

UNPORTED=	e2undel extfsmgr

all: debug

debug:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d debug || exit 1; done

release:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d release || exit 1; done

install:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d install || exit 1; done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean || exit 1; done

.PHONY: all debug release install clean
