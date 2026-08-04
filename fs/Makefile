#
# ext2fs.fs - the file system bundle
#
# This directory owns the bundle and everything that goes inside it. The four
# helper programs are built from subdirectories here rather than from ../tools,
# because Contents/Resources is where they have to end up: diskarbitrationd
# resolves FSProbeExecutable, FSMountExecutable, FSFormatExecutable and
# FSRepairExecutable from Info.plist against that directory. ../tools is for
# optional third-party tools that are not part of the bundle.
#
# The subdirectories share ./Makefile.inc for their compiler flags. Note that
# the file this Makefile includes below is ../Makefile.inc, the top-level one -
# a different file, reached from a different directory.
#

include ../Makefile.inc

SDKROOT?=       $(shell xcrun --sdk macosx --show-sdk-path)

BUNDLEID?=           $(BUNDLEDOMAIN).$(FSNAME)
FSBUNDLE?=           $(FSNAME).fs
PREFIX?=             /Library/Filesystems
DEVELOPER_DIR?=      /Applications/Xcode.app/Contents/Developer
MACOSX_VERSION_MIN?= 12.0

CC=       $(shell xcrun -find -sdk $(SDKROOT) cc)
CODESIGN= $(shell xcrun -find -sdk $(SDKROOT) codesign)

# Everything that lands in Contents/Resources. mount_ext2fs is named separately
# because it is the one the dSYM is generated from and the one Info.plist
# points at as FSMountExecutable.
MOUNTDIR=	mount_ext2fs
TOOLDIRS=	$(MOUNTDIR) newfs_ext2fs fsck_ext2fs ext2fs.util

# Those of TOOLDIRS that ship a man page. ext2fs.util has none: it is a probe
# helper that only diskarbitrationd ever runs, and Apple documents no
# equivalent for its own filesystems either.
MANDIRS=	$(MOUNTDIR) newfs_ext2fs fsck_ext2fs

RESOURCES=	$(MOUNTDIR)/mount_$(FSNAME) \
		newfs_ext2fs/newfs_ext2fs \
		fsck_ext2fs/fsck_ext2fs \
		ext2fs.util/ext2fs.util

# debug/release is chosen by the caller and handed to every subdirectory, so
# the bundle is never a mix of the two.
TARGET?=	debug

all: debug

Info.plist~: Info.plist.in ../Makefile.inc
	sed \
		-e 's/__FSNAME__/$(FSNAME)/g' \
		-e 's/__FSMACHO__/$(FSNAME)/g' \
		-e 's/__FSMOUNTEXE__/$(FSMOUNTEXE)/g' \
		-e 's/__FSLABEL__/$(FSLABEL)/g' \
		-e 's/__FSVERSION__/$(FSVERSION)/g' \
		-e 's/__FSBUILD__/$(FSBUILD)/g' \
		-e 's/__BUNDLEID__/$(BUNDLEID)/g' \
		-e 's/__OSBUILD__/$(shell /usr/bin/sw_vers -buildVersion)/g' \
		-e 's/__CLANGVER__/$(shell $(CC) -v 2>&1 | grep version)/g' \
	$< > $@

# ARCHFLAGS and TARGET_TRIPLE reach the subdirectories on their own: the
# top-level Makefile passes them as command-line overrides, and GNU make
# propagates those to sub-makes.
tools:
	@for d in $(TOOLDIRS); do $(MAKE) -C $$d $(TARGET) || exit 1; done

$(FSBUNDLE): Info.plist~ tools
	mkdir -p $@/Contents/Resources
	cp Info.plist~ $@/Contents/Info.plist
	@for f in $(RESOURCES); do \
		echo "    stage $$f"; \
		cp "$$f" $@/Contents/Resources/ || exit 1; \
	done
ifdef COMPATIBLE_VERSION
	/usr/libexec/PlistBuddy -c 'Add :OSBundleCompatibleVersion string "$(COMPATIBLE_VERSION)"' $@/Contents/Info.plist
endif
ifdef COPYRIGHT
	/usr/libexec/PlistBuddy -c 'Add :NSHumanReadableCopyright string "$(COPYRIGHT)"' $@/Contents/Info.plist
endif
ifdef MACOSX_VERSION_MIN
	/usr/libexec/PlistBuddy -c 'Add :LSMinimumSystemVersion string "$(MACOSX_VERSION_MIN)"' $@/Contents/Info.plist
endif
	/usr/libexec/PlistBuddy -c 'Print OSBundleLibraries' $@/Contents/Info.plist &> /dev/null || \
		/usr/libexec/PlistBuddy -c 'Add :OSBundleLibraries:com.apple.kpi.bsd string "8.0b1"' $@/Contents/Info.plist
	dsymutil $(ARCHFLAGS) -o $(FSBUNDLE).dSYM $@/Contents/Resources/mount_$(FSNAME)
	@# Sign last: the signature has to cover the staged Resources, not just
	@# the Info.plist, so this cannot move above the copies above.
ifdef SIGNCERT
	$(CODESIGN) --force --timestamp=none --sign $(SIGNCERT) $@
endif
	touch $@

debug:   TARGET=debug
debug:   $(FSBUNDLE)

release: TARGET=release
release: $(FSBUNDLE)

# Man pages only - see the subdirectory Makefiles. The bundle itself is
# installed by the top-level Makefile.
install-man:
	@for d in $(MANDIRS); do $(MAKE) -C $$d install || exit 1; done

install: $(FSBUNDLE) uninstall
	test -d "$(PREFIX)"
	sudo cp -r $(FSBUNDLE) "$(PREFIX)/$(FSBUNDLE)"
	sudo chmod -R 755 "$(PREFIX)/$(FSBUNDLE)"
	sudo chown -R root:wheel "$(PREFIX)/$(FSBUNDLE)"

uninstall:
	test -d "$(PREFIX)"
	test -e "$(PREFIX)/$(FSBUNDLE)" && \
	sudo rm -rf "$(PREFIX)/$(FSBUNDLE)" || true

clean:
	rm -rf $(FSBUNDLE) $(FSBUNDLE).dSYM Info.plist~
	@for d in $(TOOLDIRS); do $(MAKE) -C $$d clean || exit 1; done

.PHONY: all tools debug release install install-man uninstall clean
