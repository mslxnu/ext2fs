#
# All-in-one Makefile
#

MAKE=make
OUT=out

# Install locations.
EXT_DIR        := /Library/Extensions
FS_DIR         := /Library/Filesystems
SBIN_DIR       := /usr/local/sbin
LAUNCHD_DIR    := /Library/LaunchDaemons
LAUNCHD_PLIST  := com.beako.ext2fs.plist

# Identifiers / runtime files.
BUNDLE_ID      := com.beako.filesystems.ext2fs
ARM_FLAG       := /var/db/ext2fs.enabled
KSYMS_FILE     := /var/db/ext2fs.ksyms
VERSION        := $(strip $(shell cat VERSION 2>/dev/null || echo 0.0.0))

# Installer artefacts.
PKG_ID         := com.beako.filesystems.ext2fs.pkg
PKG_COMP       := $(OUT)/ext2fs-component.pkg
PKG_OUT        := $(OUT)/ext2fs-$(VERSION).pkg
DMG_OUT        := $(OUT)/ext2fs-$(VERSION).dmg

# Detect native arch if ARCH not specified
NATIVE_ARCH := $(shell uname -m)
ifeq ($(NATIVE_ARCH),arm64)
    DEFAULT_ARCH := arm64e
else
    DEFAULT_ARCH := x86_64
endif
ARCH ?= $(DEFAULT_ARCH)
# Accept arm64 as alias for arm64e (kexts require arm64e ABI)
ifeq ($(ARCH),arm64)
    override ARCH := arm64e
endif

# Per-arch settings
ifeq ($(ARCH),arm64e)
    KEXT_ARCHFLAGS    := -arch arm64e
    KEXT_TRIPLE       := arm64e-apple-macos12.0
    FS_ARCHFLAGS      := -arch arm64
    FS_TRIPLE         := arm64-apple-macos12.0
    LIB_ARCHFLAGS     := -arch arm64e
    LIB_TRIPLE        := arm64e-apple-macos12.0
else ifeq ($(ARCH),x86_64)
    KEXT_ARCHFLAGS    := -arch x86_64
    KEXT_TRIPLE       := x86_64-apple-macos10.15
    FS_ARCHFLAGS      := -arch x86_64
    FS_TRIPLE         := x86_64-apple-macos10.15
    LIB_ARCHFLAGS     := -arch x86_64
    LIB_TRIPLE        := x86_64-apple-macos10.15
else ifeq ($(ARCH),universal)
    KEXT_ARCHFLAGS    := -arch arm64e
    KEXT_TRIPLE       := arm64e-apple-macos12.0
    FS_ARCHFLAGS      := -arch arm64
    FS_TRIPLE         := arm64-apple-macos12.0
    LIB_ARCHFLAGS     := -arch arm64e
    LIB_TRIPLE        := arm64e-apple-macos12.0
else
    $(error Unknown ARCH=$(ARCH). Use arm64e, x86_64, or universal)
endif

KEXT_FLAGS := ARCHFLAGS="$(KEXT_ARCHFLAGS)" TARGET_TRIPLE="$(KEXT_TRIPLE)"
FS_FLAGS   := ARCHFLAGS="$(FS_ARCHFLAGS)"   TARGET_TRIPLE="$(FS_TRIPLE)"
LIB_FLAGS  := ARCHFLAGS="$(LIB_ARCHFLAGS)"  TARGET_TRIPLE="$(LIB_TRIPLE)"

# The build wipes and repopulates $(OUT) in a fixed order; never parallelise it.
.NOTPARALLEL:

# ---------------------------------------------------------------------------
# Build  ->  $(OUT)
# ---------------------------------------------------------------------------

# Default: everything needed to install (kext, fs, tools, plists, GUI). Not tests.
all: clean kextfs tools pkg dmg

ifeq ($(ARCH),universal)

# kext + fs as fat (arm64e + x86_64) binaries.
kextfs:
	rm -rf $(OUT)
	mkdir $(OUT)
	$(MAKE) -C lib  ARCHFLAGS="-arch arm64e" TARGET_TRIPLE="arm64e-apple-macos12.0"
	$(MAKE) debug -C kext ARCHFLAGS="-arch arm64e" TARGET_TRIPLE="arm64e-apple-macos12.0"
	$(MAKE) debug -C fs   ARCHFLAGS="-arch arm64"  TARGET_TRIPLE="arm64-apple-macos12.0"
	mv kext/ext2fs.kext kext/ext2fs.kext.dSYM fs/ext2fs.fs fs/ext2fs.fs.dSYM $(OUT)
	mv $(OUT)/ext2fs.kext $(OUT)/ext2fs.kext.arm64e
	mv $(OUT)/ext2fs.fs   $(OUT)/ext2fs.fs.arm64
	$(MAKE) -C kext clean
	$(MAKE) -C fs clean
	$(MAKE) -C lib all clean
	$(MAKE) -C lib  ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	$(MAKE) debug -C kext ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	$(MAKE) debug -C fs   ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	rm -rf $(OUT)/ext2fs.kext.dSYM $(OUT)/ext2fs.fs.dSYM
	mv kext/ext2fs.kext kext/ext2fs.kext.dSYM fs/ext2fs.fs fs/ext2fs.fs.dSYM $(OUT)
	mv $(OUT)/ext2fs.kext $(OUT)/ext2fs.kext.x86_64
	mv $(OUT)/ext2fs.fs   $(OUT)/ext2fs.fs.x86_64
	cp -r $(OUT)/ext2fs.kext.arm64e $(OUT)/ext2fs.kext
	lipo -create $(OUT)/ext2fs.kext.arm64e/Contents/MacOS/ext2fs $(OUT)/ext2fs.kext.x86_64/Contents/MacOS/ext2fs -output $(OUT)/ext2fs.kext/Contents/MacOS/ext2fs
	cp -r $(OUT)/ext2fs.fs.arm64 $(OUT)/ext2fs.fs
	@# Every program in Contents/Resources has to be fattened, not just the
	@# mount helper: newfs, fsck and the probe helper are separate binaries.
	@for t in mount_ext2fs newfs_ext2fs fsck_ext2fs ext2fs.util; do \
		echo "    lipo $$t"; \
		lipo -create $(OUT)/ext2fs.fs.arm64/Contents/Resources/$$t \
		             $(OUT)/ext2fs.fs.x86_64/Contents/Resources/$$t \
		     -output $(OUT)/ext2fs.fs/Contents/Resources/$$t || exit 1; \
	done
	codesign --force --timestamp=none --sign - $(OUT)/ext2fs.kext
	codesign --force --timestamp=none --sign - $(OUT)/ext2fs.fs
	rm -rf $(OUT)/ext2fs.kext.arm64e $(OUT)/ext2fs.kext.x86_64
	rm -rf $(OUT)/ext2fs.fs.arm64 $(OUT)/ext2fs.fs.x86_64

else

# kext + fs for a single arch.
kextfs:
	rm -rf $(OUT)
	mkdir $(OUT)
	$(MAKE) -C lib $(LIB_FLAGS)
	$(MAKE) debug -C kext $(KEXT_FLAGS)
	mv kext/ext2fs.kext kext/ext2fs.kext.dSYM $(OUT)
	$(MAKE) debug -C fs $(FS_FLAGS)
	mv fs/ext2fs.fs fs/ext2fs.fs.dSYM $(OUT)

endif

# Optional third-party tools. The four programs the
# file system itself needs are built by fs/ and staged into the bundle there,
# because Contents/Resources is where diskarbitrationd resolves
# FSProbeExecutable / FSMountExecutable / FSFormatExecutable /
# FSRepairExecutable. Nothing in tools/ is ported yet, so this is a no-op.
tools:
	$(MAKE) -C tools

# ---------------------------------------------------------------------------
# Distribution  ->  installer package and disk image
# ---------------------------------------------------------------------------

# Installer package. The payload is staged into a root that mirrors the
# destination layout, then pkgbuild wraps it and productbuild adds the panes.
#
# The /usr/local/sbin symlinks are not part of the payload: they are made by
# the postinstall script, which keeps one description of where they point
# rather than two that can disagree.
pkg: kextfs tools
	@echo "==> Staging installer payload"
	rm -rf $(OUT)/pkgroot $(OUT)/pkgres
	install -d $(OUT)/pkgroot/Library/Extensions $(OUT)/pkgroot/Library/Filesystems $(OUT)/pkgroot/Library/LaunchDaemons
	cp -R $(OUT)/ext2fs.kext $(OUT)/pkgroot/Library/Extensions/
	cp -R $(OUT)/ext2fs.fs   $(OUT)/pkgroot/Library/Filesystems/
	cp tools/$(LAUNCHD_PLIST) $(OUT)/pkgroot/Library/LaunchDaemons/
	@# codesign and pkgbuild reject Finder-info and similar xattrs.
	xattr -cr $(OUT)/pkgroot
	@echo "==> Building component package"
	pkgbuild --root $(OUT)/pkgroot --identifier $(PKG_ID) --version $(VERSION) \
	         --scripts installer/scripts --ownership recommended \
	         --component-plist installer/ext2fs-component.plist \
	         --install-location / $(PKG_COMP)
	@echo "==> Building product archive"
	mkdir -p $(OUT)/pkgres
	cp installer/resources/welcome.html installer/resources/conclusion.html $(OUT)/pkgres/
	@# The <license> pane reads this by name out of the resources directory.
	cp LICENSE $(OUT)/pkgres/LICENSE
	sed -e 's/__KEXTVERSION__/$(VERSION)/g' installer/distribution.xml.in > $(OUT)/distribution.xml
	productbuild --distribution $(OUT)/distribution.xml --package-path $(OUT) \
	             --resources $(OUT)/pkgres $(PKG_OUT)
	rm -rf $(PKG_COMP) $(OUT)/distribution.xml $(OUT)/pkgroot $(OUT)/pkgres
	@echo "==> Built $(PKG_OUT)"

# Disk image wrapping the installer package, a README and the uninstaller.
dmg: pkg
	@echo "==> Building disk image"
	rm -f $(DMG_OUT)
	rm -rf $(OUT)/dmg
	mkdir -p $(OUT)/dmg
	cp $(PKG_OUT) $(OUT)/dmg/
	cp installer/resources/DMG-README.txt $(OUT)/dmg/README.txt
	@# The .command is only a front-end; uninstall.sh must travel with it.
	cp installer/uninstall.command "$(OUT)/dmg/Uninstall ext2fs.command"
	cp installer/uninstall.sh      $(OUT)/dmg/uninstall.sh
	chmod +x "$(OUT)/dmg/Uninstall ext2fs.command" $(OUT)/dmg/uninstall.sh
	hdiutil create -volname "ext2fs $(VERSION)" -srcfolder $(OUT)/dmg \
	               -ov -format UDZO $(DMG_OUT)
	rm -rf $(OUT)/dmg
	@echo "==> Built $(DMG_OUT)"

# Test programs (not part of the default build).
tests:
	$(MAKE) -C tests

check: tests
	@echo "==> ext2fs feature tests"

distcheck: dmg
	@echo "==> Verifying distribution artefacts"
	@pkgutil --check-signature "$(PKG_OUT)" >/dev/null 2>&1 || true
	@pkgutil --payload-files "$(PKG_OUT)" >/dev/null 2>&1 || \
		{ echo "FAIL: $(PKG_OUT) has no readable payload"; exit 1; }
	@hdiutil imageinfo "$(DMG_OUT)" >/dev/null 2>&1 || \
		{ echo "FAIL: $(DMG_OUT) is not a valid disk image"; exit 1; }
	@echo "    $(PKG_OUT)"
	@echo "    $(DMG_OUT)"

# Back-compat aliases.
debug: kextfs
release: TARGET=release
release: kextfs

# ---------------------------------------------------------------------------
# Install  (run as root, AFTER `make`; copies only, never compiles)
# ---------------------------------------------------------------------------

install: preinstall install-kext install-fs install-tools install-launchd install-man postinstall

# Tear down any previously installed/loaded build first. macOS caches third-party
# kexts in the Auxiliary Kernel Collection, so a stale staged copy otherwise
# shadows the freshly installed build and the new version is never detected.
preinstall:
	@echo "==> Removing any previously installed ext2fs (unmount, unload, clear staging)"
	-@mount | awk '/\(ext2fs[ ,]/ { print $$3 }' | while read -r mp; do \
		echo "    umount $$mp"; umount "$$mp" 2>/dev/null || true; done
	-@kmutil unload -b $(BUNDLE_ID) 2>/dev/null || true
	-@kmutil clear-staging 2>/dev/null || true

install-kext:
	rm -rf $(EXT_DIR)/ext2fs.kext
	cp -R $(OUT)/ext2fs.kext $(EXT_DIR)/ext2fs.kext
	chown -R root:wheel $(EXT_DIR)/ext2fs.kext
	chmod -R 755 $(EXT_DIR)/ext2fs.kext

install-fs:
	rm -rf $(FS_DIR)/ext2fs.fs
	cp -R $(OUT)/ext2fs.fs $(FS_DIR)/ext2fs.fs
	chown -R root:wheel $(FS_DIR)/ext2fs.fs
	chmod -R 755 $(FS_DIR)/ext2fs.fs

# The .fs bundle holds the only copy of newfs_ext2fs and fsck_ext2fs; what
# goes in $(SBIN_DIR) is a symlink to it, so the two can never drift apart.
# This is how the system's own file systems are arranged - /sbin/fsck_msdos
# and /sbin/newfs_msdos are symlinks into msdos.fs/Contents/Resources.
#
# mount_ext2fs is deliberately not linked: nothing calls it by hand, only
# diskarbitrationd through the bundle.
install-tools:
	@install -d "$(SBIN_DIR)"
	@for t in newfs_ext2fs fsck_ext2fs; do \
		target="$(FS_DIR)/ext2fs.fs/Contents/Resources/$$t"; \
		test -f "$$target" || { echo "    missing $$target"; exit 1; }; \
		echo "    link $(SBIN_DIR)/$$t -> $$target"; \
		/bin/ln -sfh "$$target" "$(SBIN_DIR)/$$t"; \
	done

# LaunchDaemon + loader script that auto-loads the kext at boot. Gated by
# /var/db/ext2fs.enabled so a kernel fault cannot boot-loop the machine.
install-launchd:
	@install -d "$(LAUNCHD_DIR)"
	@install -m 755 tools/ext2fs-load "$(SBIN_DIR)/ext2fs-load"
	@install -m 644 tools/$(LAUNCHD_PLIST) "$(LAUNCHD_DIR)/$(LAUNCHD_PLIST)"
	@chown root:wheel "$(LAUNCHD_DIR)/$(LAUNCHD_PLIST)"
	@echo "    installed LaunchDaemon $(LAUNCHD_DIR)/$(LAUNCHD_PLIST)"
	@echo "    installed loader $(SBIN_DIR)/ext2fs-load"
	@echo "    arm with: sudo touch /var/db/ext2fs.enabled && sudo reboot"

# Man pages go where the system's own do - /usr/local/share/man/man8, mirroring
# Apple's fsck_msdos.8 in /usr/share/man/man8 - and not inside the bundle:
# Apple's .fs bundles carry no man pages, and man(1) would not look there.
install-man:
	$(MAKE) -C fs install-man

postinstall:
	@echo "ext2fs: installed kext, fs."

# Uninstall. installer/uninstall.sh is the single description of what an
# uninstall removes; this target only escalates into it.
uninstall:
	@test "$$(id -u)" -eq 0 || { echo "==> make uninstall must be run as root"; exit 1; }
	@/bin/bash installer/uninstall.sh

# ---------------------------------------------------------------------------
# Clean  (never needs sudo: the build never produces root-owned files)
# ---------------------------------------------------------------------------

# vendor/ is not recursed into: libutil and diskdev_cmds are Apple Xcode
# projects with no GNU makefile, and nothing links them any more. The tree uses
# vendor/libutil only as an include path, for mntopts.h.
clean:
	rm -rf $(OUT)
	$(MAKE) -C kext clean
	$(MAKE) -C fs clean
	$(MAKE) -C lib clean
	$(MAKE) -C tests clean
	$(MAKE) -C tools clean

.PHONY: all kextfs tools tests check distcheck debug release pkg dmg \
        install preinstall install-kext install-fs install-tools install-launchd install-man \
        postinstall uninstall clean
