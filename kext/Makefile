include ../Makefile.inc

SDKROOT?=       $(shell xcrun --sdk macosx --show-sdk-path)
KERNEL_FW=      $(SDKROOT)/System/Library/Frameworks/Kernel.framework
KERNEL_HEADERS= $(KERNEL_FW)/Headers
KERNEL_LIBDIR=  $(SDKROOT)/usr/lib

BUNDLEID?=           $(BUNDLEDOMAIN).$(KEXTNAME)
KEXTBUNDLE?=         $(KEXTNAME).kext
KEXTMACHO?=          $(KEXTNAME).out

ARCHFLAGS?=          -arch arm64e
TARGET_TRIPLE?=      arm64e-apple-macos12.0

PREFIX?=             /Library/Extensions
DEVELOPER_DIR?=      /Applications/Xcode.app/Contents/Developer
MACOSX_VERSION_MIN?= 12.0

CC=       $(shell xcrun -find -sdk $(SDKROOT) cc)
CODESIGN= $(shell xcrun -find -sdk $(SDKROOT) codesign)

# Sources are listed explicitly rather than globbed. The ext2_*.c files in this
# directory are still the verbatim NextBSD import and do not compile against the
# XNU KPI yet; each one moves from PENDING_CSRCS to CSRCS as it is ported, so the
# tree builds green at every step and the remaining work is visible here.
CSRCS:=   ext2fs.c \
          ext2_alloc.c \
          ext2_balloc.c \
          ext2_bmap.c \
          ext2_extents.c \
          ext2_ihash.c \
          ext2_inode.c \
          ext2_inode_cnv.c \
          ext2_lookup.c \
          ext2_readwrite.c \
          ext2_subr.c \
          ext2_vfsops.c \
          ext2_vnops.c

# Nothing left pending: every imported source is ported and built.
PENDING_CSRCS:=

OBJS:=    $(CSRCS:.c=.o)

CFLAGS+= -DKEXTNAME_S=\"$(KEXTNAME)\" \
         -DKEXTVERSION_S=\"$(KEXTVERSION)\" \
         -DKEXTBUILD_S=\"$(KEXTBUILD)\" \
         -DBUNDLEID_S=\"$(BUNDLEID)\" \
         -DBUNDLEID=$(BUNDLEID)

TIME_STAMP:= $(shell date +'%Y/%m/%d\ %H:%M:%S%z')
CFLAGS+=     -D__TS__=\"$(TIME_STAMP)\"

# ext2fs builds against the public Kernel.framework KPI only.
#
# The sibling procfs kext compiles with a vendored XNU tree and the full set of
# kernel-private defines, because it walks private structures - struct proc and
# its file descriptor table - whose layout is not published. A file system needs
# none of that: vnodes, buffers, mounts and locks are all public KPI. This tree
# carried a copy of those private headers for a while; it no longer does, and
# nothing here should put them back. Relying on them would mean compiling
# against struct layouts not guaranteed to match the running kernel.
#
# Note that the public headers are not by themselves a guarantee: they declare
# some symbols the kernel does not export to kexts, which link cleanly and then
# fail at load. fls() is one - see ext2_fls() in <fs/ext2fs/ext2_apple.h>.
#
# _KERNEL is the BSD spelling of "this translation unit is kernel code". XNU
# itself uses KERNEL and never tests _KERNEL (the single occurrence, in
# sys/queue.h, sits inside an inert "#if NOTFB31"), but the imported ext2fs
# headers guard their in-kernel halves with it - struct ext2mount and struct
# inode among them - so without it those types stay incomplete.
#
# Note that BSD_KERNEL_PRIVATE in particular must stay off: it makes the SDK's
# sys/ioctl.h pull in sys/ioctl_compat.h, which includes a sys/ttychars.h that
# the public SDK does not ship.
CFLAGS+= -DKERNEL \
         -D_KERNEL \
         -D_DARWIN_C_SOURCE

# Diagnostic switch, off by default. Build with
#
#	make UNMOUNT_MINIMAL=1
#
# and ext2_unmount() returns straight after flushing the vnodes, releasing
# nothing and freeing nothing. The mount leaks and the volume cannot be
# remounted without reloading the kext; it exists to bisect a machine freeze on
# umount that produces no panic, by taking the entire teardown out of the
# picture in one build. Never ship this.
ifdef UNMOUNT_MINIMAL
CFLAGS+= -DEXT2_UNMOUNT_MINIMAL
endif

ifeq ($(findstring arm64,$(TARGET_TRIPLE)),arm64)
ARCH_SYS_INCLUDES := -isystem $(KERNEL_HEADERS)/arm64 -isystem $(KERNEL_HEADERS)/arm
else
ARCH_SYS_INCLUDES :=
endif

CFLAGS+= -target $(TARGET_TRIPLE) \
         -isysroot $(SDKROOT) \
         $(ARCHFLAGS) \
         -x c \
         -std=gnu99 \
         -nostdinc \
         -fno-builtin \
         -fno-common \
         -mkernel \
         -fno-stack-check \
         -fno-stack-protector \
         -I../lib/libkext/include \
         -I../include/bsdcompat \
         -I../include \
         -isystem $(KERNEL_HEADERS) \
         $(ARCH_SYS_INCLUDES)

CFLAGS+= -Werror -Wall -Wextra \
         -Wno-unused-parameter \
         -Wno-unused-variable \
         -Wno-unused-value \
         -Wno-vla \
         -Wno-gnu-folding-constant \
         -Wno-visibility \
         -Wno-unused-function \
         -Wno-deprecated-declarations

# CXXFLAGS := $(filter-out -x c -std=gnu99,$(CFLAGS)) \
#            -x c++ -std=gnu++17 -fno-rtti -fno-exceptions -fapple-kext

LDFLAGS+= -target $(TARGET_TRIPLE) \
          -isysroot $(SDKROOT) \
          $(ARCHFLAGS) \
          -nostdlib \
          -Xlinker -kext \
          $(foreach o,$(OBJS),-Xlinker -object_path_lto -Xlinker $(o)) \
          -Xlinker -export_dynamic

# vendor/libutil and vendor/diskdev_cmds/disklib are userland libraries built
# against libSystem; they belong to the tools, not to the kext, and linking them
# here would drag in unresolved userland symbols.
LIBS+= -L$(KERNEL_LIBDIR) \
       -L../lib/libkext

LIBS+= -lkmod -lkext

KLFLAGS+= -xml -c -undef-symbols

all: debug

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp $(HDRS)
	$(CC) $(CXXFLAGS) -c -o $@ $<

$(KEXTMACHO): $(OBJS)
	$(CC) $(LDFLAGS) $(LIBS) -o $@ $^
	otool -h $@
	otool -l $@ | grep uuid

Info.plist~: Info.plist.in
	sed \
		-e 's/__KEXTNAME__/$(KEXTNAME)/g' \
		-e 's/__KEXTMACHO__/$(KEXTNAME)/g' \
		-e 's/__KEXTVERSION__/$(KEXTVERSION)/g' \
		-e 's/__KEXTBUILD__/$(KEXTBUILD)/g' \
		-e 's/__BUNDLEID__/$(BUNDLEID)/g' \
		-e 's/__OSBUILD__/$(shell /usr/bin/sw_vers -buildVersion)/g' \
		-e 's/__CLANGVER__/$(shell $(CC) -v 2>&1 | grep version)/g' \
	$^ > $@

$(KEXTBUNDLE): $(KEXTMACHO) Info.plist~
	mkdir -p $@/Contents/MacOS
	mv $< $@/Contents/MacOS/$(KEXTNAME)
	cp Info.plist~ $@/Contents/Info.plist
ifdef COMPATIBLE_VERSION
	/usr/libexec/PlistBuddy -c 'Add :OSBundleCompatibleVersion string "$(COMPATIBLE_VERSION)"' $@/Contents/Info.plist
endif
ifdef COPYRIGHT
	/usr/libexec/PlistBuddy -c 'Add :NSHumanReadableCopyright string "$(COPYRIGHT)"' $@/Contents/Info.plist
endif
ifdef SIGNCERT
	/usr/libexec/PlistBuddy -c 'Add :CFBundleSignature string ????' $@/Contents/Info.plist
endif
ifdef MACOSX_VERSION_MIN
	/usr/libexec/PlistBuddy -c 'Add :LSMinimumSystemVersion string "$(MACOSX_VERSION_MIN)"' $@/Contents/Info.plist
endif
	/usr/libexec/PlistBuddy -c 'Print OSBundleLibraries' $@/Contents/Info.plist &> /dev/null || \
		/usr/libexec/PlistBuddy -c 'Add :OSBundleLibraries:com.apple.kpi.bsd string "8.0b1"' $@/Contents/Info.plist
	@# Sign last. The signature covers Info.plist, so every PlistBuddy edit
	@# above has to have happened already - signing before them left a kext
	@# that codesign(1) rejected with "plist or signature have been modified".
ifdef SIGNCERT
	$(CODESIGN) --force --timestamp=none --sign $(SIGNCERT) $@
endif
	touch $@
	dsymutil $(ARCHFLAGS) -o $(KEXTNAME).kext.dSYM $@/Contents/MacOS/$(KEXTNAME)

debug: CFLAGS += -O0 -g
debug: $(KEXTBUNDLE)

release: CFLAGS += -O2
release: $(KEXTBUNDLE)

load: $(KEXTBUNDLE)
	sudo chown -R root:wheel $<
	sudo sync
	sudo kextutil $<
	sudo chown -R '$(USER):$(shell id -gn)' $<
	sudo dmesg | grep $(KEXTNAME) | tail -1

stat:
	kextstat | grep $(KEXTNAME)

unload:
	sudo kextunload $(KEXTBUNDLE)
	sudo dmesg | grep $(KEXTNAME) | tail -2

install: $(KEXTBUNDLE) uninstall
	test -d "$(PREFIX)"
	sudo cp -r $< "$(PREFIX)/$<"
	sudo chmod -R 755 "$(PREFIX)/$<"
	sudo chown -R root:wheel "$(PREFIX)/$<"

uninstall:
	test -d "$(PREFIX)"
	test -e "$(PREFIX)/$(KEXTBUNDLE)" && \
	sudo rm -rf "$(PREFIX)/$(KEXTBUNDLE)" || true

clean:
	rm -rf $(KEXTBUNDLE) $(KEXTBUNDLE).dSYM Info.plist~ $(OBJS) $(KEXTMACHO)

.PHONY: all debug release load stat unload install uninstall clean
