#
# All-in-one Makefile
#

MAKE=make
OUT=out

# Install locations.
EXT_DIR        := /Library/Extensions
FS_DIR         := /Library/Filesystems
SBIN_DIR       := /usr/local/sbin

# Identifiers / runtime files.
BUNDLE_ID      := com.beako.filesystems.ext2fs
ARM_FLAG       := /var/db/ext2fs.enabled
KSYMS_FILE     := /var/db/ext2fs.ksyms
VERSION        := $(strip $(shell cat VERSION 2>/dev/null || echo 0.0.0))

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
all: clean kextfs tools

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
	$(MAKE) -C vendor all clean
	$(MAKE) -C vendor  ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	$(MAKE) debug -C kext ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	$(MAKE) debug -C fs   ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	rm -rf $(OUT)/ext2fs.kext.dSYM $(OUT)/ext2fs.fs.dSYM
	mv kext/ext2fs.kext kext/ext2fs.kext.dSYM fs/ext2fs.fs fs/ext2fs.fs.dSYM $(OUT)
	mv $(OUT)/ext2fs.kext $(OUT)/ext2fs.kext.x86_64
	mv $(OUT)/ext2fs.fs   $(OUT)/ext2fs.fs.x86_64
	cp -r $(OUT)/ext2fs.kext.arm64e $(OUT)/ext2fs.kext
	lipo -create $(OUT)/ext2fs.kext.arm64e/Contents/MacOS/ext2fs $(OUT)/ext2fs.kext.x86_64/Contents/MacOS/ext2fs -output $(OUT)/ext2fs.kext/Contents/MacOS/ext2fs
	cp -r $(OUT)/ext2fs.fs.arm64 $(OUT)/ext2fs.fs
	lipo -create $(OUT)/ext2fs.fs.arm64/Contents/Resources/mount_ext2fs $(OUT)/ext2fs.fs.x86_64/Contents/Resources/mount_ext2fs -output $(OUT)/ext2fs.fs/Contents/Resources/mount_ext2fs
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

tools:
	@mkdir -p $(OUT)
	$(MAKE) -C tools

# Test programs (not part of the default build).
tests:
	$(MAKE) -C tests

check: tests
	@echo "==> ext2fs feature tests"

distcheck:
	@echo "==> Clean distribution build"

# Back-compat aliases.
debug: kextfs
release: TARGET=release
release: kextfs

# ---------------------------------------------------------------------------
# Install  (run as root, AFTER `make`; copies only, never compiles)
# ---------------------------------------------------------------------------

install: preinstall install-kext install-fs install-tools postinstall

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

# Referenced by the install target since the beginning but never defined, which
# made `make install` die with "No rule to make target 'install-tools'" before
# it copied anything. The tool binaries are not produced yet, so the target
# installs whatever tools/ has left in $(OUT) and stays quiet when that is
# nothing.
install-tools:
	@mkdir -p $(SBIN_DIR)
	@for t in $(OUT)/newfs_ext2fs $(OUT)/fsck_ext2fs; do \
		test -f "$$t" || continue; \
		echo "    install $$t -> $(SBIN_DIR)"; \
		install -o root -g wheel -m 755 "$$t" "$(SBIN_DIR)/"; \
	done

postinstall:
	@echo "ext2fs: installed kext, fs."

# ---------------------------------------------------------------------------
# Clean  (never needs sudo: the build never produces root-owned files)
# ---------------------------------------------------------------------------

clean:
	rm -rf $(OUT)
	$(MAKE) -C kext clean
	$(MAKE) -C fs clean
	$(MAKE) -C lib clean
	$(MAKE) -C tests clean
	$(MAKE) -C tools clean
	$(MAKE) -C vendor clean

.PHONY: all kextfs tools tests check distcheck debug release \
        install preinstall install-kext install-fs install-tools \
        postinstall clean
