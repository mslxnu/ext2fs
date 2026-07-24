#
# All-in-one Makefile
#

MAKE=make

all: debug

debug:
	$(MAKE) -C fsck_ext2fs
	$(MAKE) -C mount_ext2fs
	$(MAKE) -C newfs_ext2fs

release: TARGET=release
release: debug

clean:
	$(MAKE) -C fsck_ext2fs clean
	$(MAKE) -C mount_ext2fs clean
	$(MAKE) -C newfs_ext2fs clean

.PHONY: all debug release clean
