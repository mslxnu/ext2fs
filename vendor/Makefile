#
# All-in-one Makefile
#

MAKE=make

all: debug

debug:
	$(MAKE) -C libutil
	$(MAKE) -C diskdev_cmds/disklib

release: TARGET=release
release: debug

clean:
	$(MAKE) -C libutil clean
	$(MAKE) -C diskdev_cmds/disklib clean

.PHONY: all debug release clean
