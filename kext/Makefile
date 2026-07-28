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
          ext2_extents.c \
          ext2_inode_cnv.c \
          ext2_subr.c

PENDING_CSRCS:= ext2_alloc.c \
                ext2_balloc.c \
                ext2_bmap.c \
                ext2_inode.c \
                ext2_lookup.c \
                ext2_vfsops.c \
                ext2_vnops.c

OBJS:=    $(CSRCS:.c=.o)

CFLAGS+= -DKEXTNAME_S=\"$(KEXTNAME)\" \
         -DKEXTVERSION_S=\"$(KEXTVERSION)\" \
         -DKEXTBUILD_S=\"$(KEXTBUILD)\" \
         -DBUNDLEID_S=\"$(BUNDLEID)\" \
         -DBUNDLEID=$(BUNDLEID)

TIME_STAMP:= $(shell date +'%Y/%m/%d\ %H:%M:%S%z')
CFLAGS+=     -D__TS__=\"$(TIME_STAMP)\"

# Compile the vendored XNU headers in their kernel-private configuration.
# These must be defined on the command line (not just in procfs.h) so they
# are visible to every header from the very first include in each translation
# unit; otherwise headers like <arm/types.h> are processed before the macros
# are seen and omit definitions such as user32_t/user64_t.
# _KERNEL is the BSD spelling of "this translation unit is kernel code". XNU
# itself uses KERNEL and never tests _KERNEL (the single occurrence, in
# sys/queue.h, sits inside an inert "#if NOTFB31"), but the imported ext2fs
# headers guard their in-kernel halves with it - struct ext2mount and struct
# inode among them - so without it those types stay incomplete.
CFLAGS+= -DKERNEL \
         -D_KERNEL \
         -DPRIVATE \
         -DKERNEL_PRIVATE \
         -DXNU_KERNEL_PRIVATE \
         -DBSD_KERNEL_PRIVATE \
         -DMACH_KERNEL_PRIVATE \
         -D__APPLE_API_PRIVATE \
         -D_DARWIN_C_SOURCE \
         -DMACH_BSD \
         -DXNU_TARGET_OS_OSX \
         -DXNU_PLATFORM_MacOSX

# Match the struct layout of the RELEASE macOS kernel. Several structs the kext
# dereferences (notably struct proc, whose p_fd lives after these fields) have
# members guarded by CONFIG_PERSONAS and CONFIG_AUDIT, both of which are enabled
# in the macOS configuration (config/MASTER.arm64.MacOSX: BSD_BASE has
# config_personas, SECURITY_BASE has config_audit). Without these the kext's
# struct proc is short by those fields, p_fd lands at the wrong offset, and
# &p->p_fd.fd_lock points at garbage -> "Invalid/destroyed mutex" panic.
# CONFIG_PROC_RESOURCE_LIMITS is intentionally NOT defined: it is DEV/DEBUG-only
# and off in the release kernel, so leaving it off matches.
#
# CONFIG_DTRACE is ON in the shipping macOS kernel (confirmed at runtime via
# /dev/dtrace, /dev/dtracehelper and the kern.dtrace.* sysctls), so struct proc
# includes the p_dtrace_* block. The fields after it - p_argslen, p_argc,
# user_stack (read by the cmdline node) - land at the wrong offset without it.
# It sits after p_fd/p_uthlist, so enabling it does not disturb fd/ or threads/.
CFLAGS+= -DCONFIG_PERSONAS \
         -DCONFIG_AUDIT \
         -DCONFIG_DTRACE

ifeq ($(findstring arm64,$(TARGET_TRIPLE)),arm64)
XNU_INCLUDES := -I../include \
                -I../include/xnu/bsd \
                -I../include/xnu/libkern \
                -I../include/xnu/osfmk \
                -I../include/xnu/pexpert
ARCH_SYS_INCLUDES := -isystem $(KERNEL_HEADERS)/arm64 -isystem $(KERNEL_HEADERS)/arm
CFLAGS += -DAPPLEFIRESTORM
else
XNU_INCLUDES := -I../include \
                -I../include/xnu/bsd \
                -I../include/xnu/libkern \
                -I../include/xnu/osfmk \
                -I../include/xnu/pexpert
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
         -I../lib/libbsdmalloc/include \
         -I../lib/libsbuf/include \
         -I../lib/libkext/include \
         -I../include/bsdcompat \
         $(XNU_INCLUDES) \
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

# CXXFLAGS := $(filter-out -x c -std=gnu99 $(XNU_INCLUDES),$(CFLAGS)) \
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
       -L../lib/libkext \
       -L../lib/libsbuf \
       -L../lib/libbsdmalloc

LIBS+= -lkmod -lkext -lsbuf -lbsdmalloc

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
	$(CODESIGN) --force --timestamp=none --sign $(SIGNCERT) $@
	/usr/libexec/PlistBuddy -c 'Add :CFBundleSignature string ????' $@/Contents/Info.plist
endif
ifdef MACOSX_VERSION_MIN
	/usr/libexec/PlistBuddy -c 'Add :LSMinimumSystemVersion string "$(MACOSX_VERSION_MIN)"' $@/Contents/Info.plist
endif
	/usr/libexec/PlistBuddy -c 'Print OSBundleLibraries' $@/Contents/Info.plist &> /dev/null || \
		/usr/libexec/PlistBuddy -c 'Add :OSBundleLibraries:com.apple.kpi.bsd string "8.0b1"' $@/Contents/Info.plist
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
