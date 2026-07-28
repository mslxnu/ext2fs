#
# All-in-one Makefile
#
# SUBDIRS lists what currently builds. The remaining directories are present in
# the tree but not ported to macOS yet, so they are named in UNPORTED rather
# than silently forgotten.
#
# tools/mount_ext2fs is deliberately absent: mount_ext2fs is built by ../fs as
# part of the ext2fs.fs bundle, which is where the mount helper has to live for
# the bundle's FSMountExecutable to find it.
#

MAKE=make

SUBDIRS=	newfs_ext2fs \
		fsck_ext2fs \
		ext2fs.util

UNPORTED=	e2undel extfsmgr e2fsprogs

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
